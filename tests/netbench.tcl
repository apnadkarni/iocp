# Copyright (c) 2019 Ashok P. Nadkarni
# All rights reserved.
# See LICENSE file for details.

proc usage {} {
    puts "Usage:"
    puts "  tclsh netbench.tcl client ?OPTIONS?"
    puts "  tclsh netbench.tcl server ?-port PORT?"
    puts "  tclsh netbench.tcl help"
}

proc help {} {
    set help {
        Usage:
            tclsh netbench.tcl client ?OPTIONS?
            tclsh netbench.tcl server ?-port PORT?
            tclsh netbench.tcl help

        The options below may be specified when running in client mode.
        When running in server mode, only the -port option is accepted.
        All other options for socket configuration are passed from the
        client side.

        -server ADDR - The server address (127.0.0.1)
        -port PORT   - The server control port (10101)
        -provider PROVIDER - The socket implementation provider. One of
                       tcl, iocp corresponding to native Tcl
                       sockets or iocp_inet package (tcl)
        -payload text|binary - specifies the payload type as text or binary.
                       If unspecified, payload is chosen based on whether
                       socket options (below) specify binary transfer or not.
        -readsize N  - How many bytes / characters to read on each call (4096)
        -writesize N - How many bytes / characters to write on each call (4096)
        -duration N  - Number of seconds to run the test (5 seconds)
        -count N     - Number of writes to do for the test, each of size
                       specified by the -writesize option. Cannot be specified
                       with -duration which is the default.
        -repeat N    - Number of times to run the test
        -print detail|summary - Print summary of results or details (summary)

        In addition, the following socket options may be specified:
        -buffering, -buffersize, -encoding, -eofchar, -translation and for
        provider iocp only, -maxpendingreads and -maxpendingwrites. These
        control the socket configuration for running the benchmark
        on both server and client.

        Example:
            tclsh netbench.tcl server (on 192.168.1.2)
            tclsh netbench.tcl client -server 192.168.1.2 -buffering none
    }
    puts $help
}

proc socket_command {provider} {
    if {$provider eq "tcl"} {
        return socket
    } elseif {$provider eq "iocp"} {
        uplevel #0 package require iocp
        return iocp::inet::socket
    } elseif {$provider eq "iocpsock"} {
        uplevel #0 package require Iocpsock
        return socket2
    } else {
        error "Unknown socket provider $provider."
    }
}

################################################################
# Client implementation

namespace eval client {
    # Program options
    # -provider (tcl/iocp/iocpsock)
    # -writesize
    # -readsize
    # -server
    # -port
    # -payload
    # -count
    # -duration
    # -print (detail/summary)

    variable options
    array set options {
        -writesize 4096
        -provider tcl
    }

    # Socket configuration options
    variable sooptions
    array set sooptions {}

    # Control information
    #  so - control socket
    #  dataports - dictionary mapping provider to data socket
    variable control
    array set control {}

    # Data payload
    #  text - text payload
    #  binary - binary payload
    variable payload
    array set payload {}
}

proc client::payload {type} {
    variable options
    variable payload

    set chars "0123456789"
    set nchars [string length $chars]
    set nrepeat [expr {$options(-writesize) / [string length $chars]}]
    set payload(text) [string repeat $chars $nrepeat]
    set leftover [expr {$options(-writesize) - ($nrepeat * $nchars)}]
    if {$leftover} {
        append payload(text) [string range $chars 0 $leftover-1]
    }

    # Shimmer to a binary string. Note we cannot construct separately
    # using string repeat as that shimmers to a string.
    set payload(binary) [encoding convertto ascii $payload(text)]
    if {![regexp {^value is a bytearray.*no string representation$} \
              [tcl::unsupported::representation $payload(binary)]]} {
        error "Failed to generate binary payload."
    }
    proc payload {type} {
        variable payload
        if {$type eq "binary"} {
            return $payload(binary)
        } elseif {$type eq "text"} {
            return $payload(text)
        } else {
            error "Unknown payload type \"$type\"."
        }
    }
    return [payload $type]
}

proc client::bench {provider} {
    variable options
    variable sooptions
    variable control

    set payload [payload $options(-payload)]

    set socommand [socket_command $provider]
    # IMPORTANT:
    # Do NOT do any operations on $payload other than writing it
    # since [string length] etc. will all shimmer it.
    set so [$socommand $options(-server) [dict get $control(dataports) $provider]]
    fconfigure $so {*}[array get sooptions]

    set start [clock microseconds]
    if {[info exists options(-count)]} {
        set i 0
        while {$i < $options(-count)} {
            # Note: DON'T flush here because that should be controlled
            # via -buffering option
            puts -nonewline $so $payload
            incr i
        }
        set sent [expr {$i * $options(-writesize)}]
    } else {
        set sent 0
        set duration [expr {$options(-duration) * 1000000}]
        while {([clock microseconds]-$start) < $duration} {
            puts -nonewline $so $payload
            set now [clock microseconds]
            incr sent $options(-writesize)
        }
    }
    set end [clock microseconds]
    close $so write

    set configuration [fconfigure $so]
    set server_received [gets $so]
    close $so
    return [list Sent $sent \
                ServerReceived $server_received \
                Start $start \
                End $end \
                Socket $configuration]
}

proc client::connect {args} {
    # Creates a control connection to the server
    # Stores the socket and data ports in the control namespace variable.
    variable control
    variable options
    foreach {opt default} {-server 127.0.0.1 -port 10101} {
        if {[dict exists $args $opt]} {
            set options($opt) [dict get $args $opt]
        } else {
            set options($opt) $default
        }
    }
    # Control channel always uses Tcl sockets
    set so [socket $options(-server) $options(-port)]
    fconfigure $so -buffering line
    puts $so PORTS
    lassign [gets $so] status data_ports
    if {$status ne "OK"} {
        close $so
        error "Server failure: $status $data_ports"
    }
    set control(so) $so
    set control(dataports) $data_ports
}

proc client::runtest {args} {
    variable options
    variable sooptions
    variable control

    array set opts [dict merge {
        -provider tcl
        -writesize 4096
        -print summary
        -readsize 4096
    } $args]

    if {[info exists opts(-translation)]} {
        if {$opts(-translation) eq "binary"} {
            if {[info exists opts(-encoding)] && $opts(-encoding) ne "binary"} {
                error "Option -encoding must be \"binary\" if -translation is \"binary\"."
            }
            if {[info exists opts(-eofchar)] && $opts(-eofchar) ne ""} {
                error "Option -eofchar must be \"\" if -translation is \"binary\"."
            }
        }
    } else {
        # Default to -translation assuming no incompatible options specified
        if {(![info exists opts(-encoding)] || $opts(-encoding) eq "binary") &&
            (![info exists opts(-eofchar)] || $opts(-eofchar) eq "")} {
            set opts(-translation) binary
        }
    }

    foreach opt {-buffering -buffersize -encoding -eofchar -translation -maxpendingreads -maxpendingwrites} {
        if {[info exists opts($opt)]} {
            set sooptions($opt) $opts($opt)
            unset opts($opt)
        }
    }

    if {![string is integer -strict $opts(-readsize)] ||
        $opts(-readsize) < 0 ||
        $opts(-readsize) > 0x7fffffff} {
        error "Invalid -readsize value \"$opts(-readsize)\"."
    }

    if {![string is integer -strict $opts(-writesize)] ||
        $opts(-writesize) < 0 ||
        $opts(-writesize) > 1000000000} {
        error "Invalid -readsize value \"$opts(-readsize)\"."
    }

    array set options [array get opts]
 
    if {[info exists options(-payload)]} {
        if {$options(-payload) ni {text binary}} {
            error "Invalid -payload value \"$options(-payload)\"."
        }
    } else {
        if {[info exists sooptions(-translation)] &&
            $sooptions(-translation) eq "binary"} {
            set options(-payload) binary
        } elseif {[info exists sooptions(-encoding)] &&
                  $sooptions(-encoding) eq "binary"} {
            set options(-payload) binary
        } else {
            set options(-payload) text
        }
    }

    if {[info exists options(-count)]} {
        if {[info exists options(-duration)]} {
            error "Options -count and -duration must not be specified together."
        }
    } else {
        if {![info exists options(-duration)]} {
            set options(-duration) 5
        }
    }

    puts $control(so) [list SOCONFIG [array get sooptions]]
    gets $control(so) line
    if {$line ne "OK"} {
        error "Server failure: $line"
    }

    puts $control(so) [list IOSIZE [list -readsize $options(-readsize) -writesize $opts(-writesize)]]
    gets $control(so) line
    if {$line ne "OK"} {
        error "Server failure: $line"
    }

    if {![dict exists $control(dataports) $opts(-provider)] ||
        [dict get $control(dataports) $opts(-provider)] == 0} {
        error "Server does not support $opts(-provider)."
    }

    set client_result [bench $opts(-provider)]

    set sockname [dict get $client_result Socket -sockname]
    set id [list [lindex $sockname 0] [lindex $sockname 2]]
    puts $control(so) [list FINISH $id]
    set server_result [gets $control(so)]

    return [list Client $client_result Server $server_result]
}

proc client::format_duration {duration} {
    if {$duration < 1000} {
        return [format %.6f [expr {double($duration)/1000000}]]
    } elseif {$duration < 1000000} {
        return [format %.3f [expr {double($duration)/1000000}]]
    } else {
        return [format %.2f [expr {double($duration)/1000000}]]
    }
}

proc client::print_2cols {d {indent {    }}} {
    # Prints dictionary two elements on each line
    set keys [lsort -dictionary [dict keys $d]]
    set k1len 0
    set k2len 0
    set e1len 0
    set e2len 0
    foreach {k1 k2} $keys {
        if {[string length $k1] > $k1len} {
            set k1len [string length $k1]
        }
        set e1 [dict get $d $k1]
        if {$e1 eq ""} {
            # Show empty strings as ""
            set e1 "\"\""
            dict set d $k1 $e1
        }
        if {[string length $e1] > $e1len} {
            set e1len [string length $e1]
        }
        if {$k2 ne ""} {
            set e2 [dict get $d $k2]
            if {$e2 eq ""} {
                # Show empty strings as ""
                set e2 "\"\""
                dict set d $k2 $e2
            }
            if {[string length $k2] > $k2len} {
                set k2len [string length $k2]
            }
        }
    }
    incr k1len;                 # For the ":"
    incr k2len
    foreach {k1 k2} $keys {
        if {$k2 eq ""} {
            puts [format "$indent%-*s %-*s" \
                      $k1len $k1: $e1len [dict get $d $k1]]
        } else {
            puts [format "$indent%-*s %-*s    %-*s %s" \
                      $k1len $k1: $e1len [dict get $d $k1] \
                      $k2len $k2: [dict get $d $k2]]
        }
    }
}

proc client::print {result} {
    variable options
    lassign [dict get $result Server] server_status server_result
    set client_result [dict get $result Client]
    if {$server_status eq "OK"} {
        if {[dict get $client_result Sent] != [dict get $server_result Received]} {
            puts stdout "ERROR: Client sent [dict get $client_result Sent] bytes but server received [dict get $server_result Received]."
        }
    }
    if {$options(-print) eq "detail"} {
        dict with client_result {
            set duration [expr {$End - $Start}]
            set mbps [format %.2f [expr {double($Sent)/$duration}]]
            puts "CLIENT: $mbps Mbps (Sent $Sent bytes in $duration usecs)"
            dict unset Socket -sockname
            dict unset Socket -peername
            dict unset Socket -error
            dict unset Socket -connecting
            dict unset Socket -maxpendingaccepts
            print_2cols $Socket "        "
        }
        if {$server_status eq "OK"} {
            dict with server_result {
                set duration [expr {$End - $Start}]
                set mbps [format %.2f [expr {double($Received)/$duration}]]
                puts "SERVER: $mbps Mbps (Rcvd $Received bytes in $duration usecs)"
                dict unset Socket -sockname
                dict unset Socket -peername
                dict unset Socket -error
                dict unset Socket -connecting
                dict unset Socket -maxpendingaccepts
                print_2cols $Socket "        "
            }
        } else {
            puts "Server error: $server_result"
        }
    } else {
        dict with client_result {
            set duration [expr {$End - $Start}]
            set mbps [format %.2f [expr {double($Sent)/$duration}]]
            #puts "$mbps Mbps (Sent $Sent bytes in $duration usecs)"
            puts "$mbps $Sent [format_duration $duration] $options(-provider)"
        }
    }
    
}
proc client::run {args} {
    variable control

    set repeat 1
    if {[dict exists $args -repeat]} {
        set repeat [dict get $args -repeat]
        if {[incr repeat 0] < 0} {
            error "Invalid repeat count \"$repeat\"."
        }
    }
    set options(-print) summary
    if {[dict exists $args -print]} {
        set options(-print) [dict get $args -print]
    }
    connect $args
    for {set i 0} {$i < $repeat} {incr i} {
        print [runtest {*}$args]
    }
    close $control(so)
}

################################################################
# SERVER

namespace eval server {
    # Server options are as sent from client
    variable options
    array set options {}

    # Listening socket handles indexed by provider
    variable listeners
    array set listeners {}
    
    # Listening socket ports indexed by provider
    variable listening_ports
    array set listening_ports {}

    # Socket options for data sockets - sent by client
    variable soconfig
    array set soconfig {}

    # Array indexed by data socket handles. Contains socket stats
    variable sockets
    array set sockets {}

    # Dictionary containing data socket handles keyed by client address and port.
    variable clients
}

proc server::run {args} {
    variable listeners;           # Listening sockets
    variable listening_ports;     # Corresponding ports
    variable soconfig;            # Socket config options

    set port 10101
    if {[dict exists $args -port]} {
        set port [dict get $args -port]
    }
    set listener [socket -server [namespace current]::accept_control $port]
    puts stdout "Control socket listening on [lindex [fconfigure $listener -sockname] 2]"

    set listeners(tcl) [socket -server [namespace current]::accept_data 0]
    set listening_ports(tcl) [lindex [fconfigure $listeners(tcl) -sockname] 2]
    puts stdout "Tcl socket listening on $listening_ports(tcl)."

    if {[catch {uplevel #0 package require iocp_inet}]} {
        set listeners_ports(iocp) 0
        puts stderr "Could not load iocp package."
    }
    set listeners(iocp) [iocp::inet::socket -server [namespace current]::accept_data 0]
    set listening_ports(iocp) [lindex [fconfigure $listeners(iocp) -sockname] 2]
    puts stdout "iocp socket listening on $listening_ports(iocp)."

    if {[catch {uplevel #0 package require Iocpsock}]} {
        set listeners_ports(iocpsock) 0
        puts stderr "Could not load Iocpsock package."
    }
    set listeners(iocpsock) [socket2 -server [namespace current]::accept_data 0]
    set listening_ports(iocpsock) [lindex [fconfigure $listeners(iocpsock) -sockname] 2]
    puts stdout "Iocpsock socket listening on $listening_ports(iocpsock)."

    vwait forever
}

proc server::accept_control {so addr port} {
    puts "Control connection from $addr/$port."
    fileevent $so readable [list [namespace current]::read_control $so]
    fconfigure $so -buffering line
}

proc server::read_control {so} {
    variable listeners
    variable listening_ports
    variable soconfig
    variable options
    set status [catch {gets $so line} nchars ropts]
    if {$status} {
        close $so
        puts stderr "Error reading from socket: $nchars"
    } elseif {$nchars < 0} {
        if {[eof $so]} {
            close $so
        }
    } else {
        # TBD - enclose in try
        # Received a command
        lassign $line command opts
        try {
            switch -exact -- $command {
                PORTS {
                    puts $so [list OK [array get listening_ports]]
                }
                SOCONFIG {
                    array set soconfig $opts
                    puts $so OK
                }
                IOSIZE {
                    array set options $opts
                    puts $so OK
                }
                FINISH {
                    variable sockets
                    variable clients
                    lassign $opts addr port
                    if {[dict exists $clients $addr $port]} {
                        set dataso [dict get $clients $addr $port]
                        set result $sockets($dataso)
                        dict set result Socket [fconfigure $dataso]
                        close $dataso
                        unset sockets($dataso)
                        puts $so [list OK $result]
                        dict unset clients $addr $port
                    } else {
                        puts $so [list ERROR "Unknown client: $addr/$port"]
                    }
                }
                default {
                    error "Unknown command \"$command\"."
                }
            }
        } trap {} {err} {
            puts stderr "ERROR: $err"
            puts $so "ERROR: $err"
        }
    }
}

proc server::accept_data {so addr port} {
    variable soconfig
    variable sockets
    variable clients

    set opts [array get soconfig]
    lappend opts -blocking 0
    puts stdout "Data connection from $addr/$port. Setting socket to $opts."
    fconfigure $so {*}$opts

    fileevent $so readable [list [namespace current]::read_data $so]
    set sockets($so) [dict create Remote [list $addr $port] Received 0 Sent 0 Start [clock microseconds]]
    dict set clients $addr $port $so
}

proc server::read_data {so} {
    variable sockets
    variable options

    if {[catch {
        read $so $options(-readsize)
    } data]} {
        puts stderr "Read error on socket ([dict get $sockets($so) Remote]): $data"
        # Our command protocol does not like newlines :-()
        # Note: do not close socket until client retrieves status.
        puts stderr "Error: $data"
        dict set sockets($so) Error [string map [list \n " "] $data]
        fileevent $so readable {}
        return
    }
    set len [string length $data]
    dict incr sockets($so) Received $len
    if {$len < $options(-readsize)} {
        if {[eof $so]} {
            # Note: Do not close socket since we do not want port reused until
            # client collects statistics
            puts $so [dict get $sockets($so) Received]; flush $so
            fileevent $so readable {}
            dict set sockets($so) End [clock microseconds]
        } else {
            # No data available
        }
    }
}

if {[string equal -nocase [file normalize [info script]/...] [file normalize $argv0/...]]} {
    if {[llength $argv] == 0} {
        usage
    } else {
        switch -exact -- [lindex $argv 0] {
            client {
                client::run {*}[lrange $argv 1 end]
            }
            server {
                server::run {*}[lrange $argv 1 end]
            }
            help {
                help
            }
            default {
                usage
            }
        }
    }
}
