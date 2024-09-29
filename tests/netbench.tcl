# Copyright (c) 2020 Ashok P. Nadkarni
# All rights reserved.
# See LICENSE file for details.
# For instructions:
#   tclsh netbench.tcl help
#

proc usage {} {
    puts "Usage:"
    puts "  tclsh netbench.tcl help"
    puts "  tclsh netbench.tcl server ?-port PORT?"
    puts "  tclsh netbench.tcl client ?OPTIONS?"
    puts "  tclsh netbench.tcl batch ?OPTIONS?"
}

proc help {} {
    set help {
        SERVER

        To start the server,
            tclsh netbench.tcl server ?-port PORT?
        where PORT is the port for control connections. Data connections
        are automatically allocated. All other options are passed
        from the client side. To exit the server press Ctrl-C.

        CLIENT

        To start the client:
            tclsh netbench.tcl client ?OPTIONS?
            tclsh netbench.tcl batch  ?OPTIONS?

        The client command runs one test one or more times using the
        configuration specified by the options. The batch command runs
        multiple tests with different configurations either reading from
        standard input or a test script.

        The following options are accepted by both client and batch commands
        on the command line but ignored if present in the test script.
        (Defaults shown in parenthesis.)

        -server ADDR - The server address (127.0.0.1)
        -port PORT   - The server control port (10101)

        The following options related to test configuration
        are accepted on the command line by both client
        and batch commands as well as within test scripts by the batch command.
        
        -provider PROVIDER - The socket implementation providers. A pair from
                       tcl, iocp corresponding to native Tcl
                       sockets or iocp_inet package (default tcl) as the socket
                       provider to use for the client and server
                       If only one is given, it is used for both.
        -payload text|binary - specifies the payload type as text or binary.
                       If unspecified, payload is chosen based on whether
                       socket options (below) specify binary transfer or not.
        -readsize N  - How many bytes / characters to read on each call (4096)
        -writesize N - How many bytes / characters to write on each call (4096)
        -duration N  - Number of seconds to run the test (5 seconds)
        -count N     - Number of writes to do for the test, each of size
                       specified by the -writesize option. Cannot be specified
                       with -duration which is the default.
        -repeat N    - Run each test N number of times.
        -print detail|summary - Print summary of results or details (summary)
        -nbwrites true|false - If true, writes are non-blocking event driven
                       otherwise blocking (false).

        In addition, the following socket options may be specified for all
        providers:
           -buffering, -buffersize, -encoding, -eofchar, -translation
        These control the socket configuration for running the benchmark
        on both server and client and may be specified either on the
        command line or in batch scripts.

        Further, for the iocp provider only, the following options may
        be specified:
           -maxpendingreads, -maxpendingwrites, -sosndbuf, -sorcvbuf

        The batch command accepts an additional option:
        -script FILE - Path of file from which to read test configurations.
                       Defaults to standard input.

        Each line of the test configuration script corresponds to a single test.
        It contains any of the test configuration options above. These are
        combined with, and override if necessary, the options specified on the
        command line to form the test configuration case. Empty lines and lines
        beginning with # are skipped. If the -script option is not specified the
        test configuration lines are read from standard input.

        EXAMPLES

        Default configuration - tests Tcl sockets. Uses localhost
        tclsh netbench.tcl server (on 192.168.1.2)
        tclsh netbench.tcl client (on 192.168.1.2)

        Remote server, IOCP
        tclsh netbench.tcl server -port 12345 (on 192.168.1.2)
        tclsh netbench.tcl client -server 192.168.1.2 -port 12345 -buffering none -provider iocp

        Batch test
        tclsh netbench.tcl batch -script netbench.test (will use localhost)

        Script netbench.test:
            -provider tcl -writesize 1 -buffering full
            -provider iocp -writesize 1000 -duration 2
            -count 1000 -readsize 4000 
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
    # -payload
    # -count
    # -duration
    # -print (detail/summary)
    variable options
    proc reset_options {} {
        variable options
        unset -nocomplain options
        array set options {
            -writesize 4096
            -provider tcl
        }
    }

    # Server info
    #  addr - ip address
    #  port - listening control port
    variable server
    array set server {
        -addr 127.0.0.1
        -port 10101
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
    # Nested dictionary, first level text/binary, second level data and size
    variable payload
    set payload [dict create]
}

proc client::payload {type} {
    variable options
    variable payload

    if {$type ni {text binary}} {
        error "Unknown payload type \"$type\"."
    }

    if {[dict exists $payload $type data] &&
        [dict get $payload $type size] == $options(-writesize)} {
        return [dict get $payload $type data]
    }

    # Need to reconstruct payload of that type
    dict unset payload $type

    set chars "0123456789"
    set nchars [string length $chars]
    set nrepeat [expr {$options(-writesize) / [string length $chars]}]
    set data [string repeat $chars $nrepeat]
    set leftover [expr {$options(-writesize) - ($nrepeat * $nchars)}]
    if {$leftover} {
        append data [string range $chars 0 $leftover-1]
    }

    if {$type eq "binary"} {
        # Shimmer to a binary string. Note we cannot construct separately
        # using string repeat as that shimmers to a string.
        set data [encoding convertto ascii $data]
        if {![regexp {^value is a bytearray.*no string representation$} \
                  [tcl::unsupported::representation $data]]} {
            error "Failed to generate binary payload."
        }
    }
    dict set payload $type data $data
    dict set payload $type size $options(-writesize)
    return $data
}

proc client::nonblocking_write_counted {so max_count payload} {
    variable nonblocking_write_count
    variable nonblocking_write_gate
    if {$nonblocking_write_count >= $max_count} {
        fileevent $so writable {}
        set nonblocking_write_gate done
    } else {
        puts -nonewline $so $payload
        incr nonblocking_write_count
    }
    #puts $nonblocking_write_count
}

proc client::nonblocking_write_timed {so end_usec payload} {
    variable nonblocking_write_count
    variable nonblocking_write_gate
    if {[clock microseconds] > $end_usec} {
        fileevent $so writable {}
        set nonblocking_write_gate done
    } else {
        puts -nonewline $so $payload
        incr nonblocking_write_count
    }
}

proc client::nonblocking_write_timer {} {
    variable nonblocking_write_gate
    set nonblocking_write_gate done
}

proc client::bench_nonblocking {local_provider remote_provider} {
    variable options
    variable sooptions
    variable control
    variable server
    variable nonblocking_write_count
    variable nonblocking_write_gate

    set payload [payload $options(-payload)]

    set socommand [socket_command $local_provider]
    # IMPORTANT:
    # Do NOT do any operations on $payload other than writing it
    # since [string length] etc. will all shimmer it.
    set so [$socommand $server(-addr) [dict get $control(dataports) $remote_provider]]
    fconfigure $so {*}[array get sooptions] -blocking 0

    set nonblocking_write_count 0
    if {[info exists options(-count)]} {
        fileevent $so writable [list [namespace current]::nonblocking_write_counted $so $options(-count) $payload]
    } else {
        fileevent $so writable [list [namespace current]::nonblocking_write_timed \
                                    $so \
                                    [expr {[clock microseconds] + ($options(-duration) * 1000000)}] \
                                    $payload]
    }

    set start [clock microseconds]
    vwait     [namespace current]::nonblocking_write_gate

    set sent [expr {$nonblocking_write_count * $options(-writesize)}]
    set end  [clock microseconds]
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

proc client::bench {local_provider remote_provider} {
    variable options
    variable sooptions
    variable control
    variable server

    set payload [payload $options(-payload)]

    set socommand [socket_command $local_provider]
    # IMPORTANT:
    # Do NOT do any operations on $payload other than writing it
    # since [string length] etc. will all shimmer it.
    set so [$socommand $server(-addr) [dict get $control(dataports) $remote_provider]]
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
            #after 100
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
    variable server

    if {[dict exists $args -server]} {
        set server(-addr) [dict get $args -server]
    }
    if {[dict exists $args -port]} {
        set server(-port) [dict get $args -port]
    }
    # Control channel always uses Tcl sockets
    puts "Connecting on $server(-addr),$server(-port)"
    set so [socket $server(-addr) $server(-port)]
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
        -provider {tcl tcl}
        -writesize 4096
        -readsize 4096
        -nbwrites 0
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

    foreach opt {-buffering -buffersize -encoding -eofchar -translation
        -maxpendingreads -maxpendingwrites -sosndbuf -sorcvbuf} {
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

    lassign $opts(-provider) local_provider remote_provider
    if {$remote_provider eq ""} {
        set remote_provider $local_provider
    }

    if {![dict exists $control(dataports) $remote_provider] ||
        [dict get $control(dataports) $remote_provider] == 0} {
        error "Server does not support $remote_provider."
    }

    if {$options(-nbwrites)} {
        set client_result [bench_nonblocking $local_provider $remote_provider]
    } else {
        set client_result [bench $local_provider $remote_provider]
    }

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

proc client::print {result {level summary}} {
    variable options
    lassign [dict get $result Server] server_status server_result
    set client_result [dict get $result Client]
    if {$server_status eq "OK"} {
        if {[dict get $client_result Sent] != [dict get $server_result Received]} {
            puts stdout "ERROR: Client sent [dict get $client_result Sent] bytes but server received [dict get $server_result Received]."
        }
    }
    if {$level eq "detail"} {
        puts "CONFIG:"
        print_2cols [array get options] "        "
        dict with client_result {
            set duration [expr {$End - $Start}]
            set mbps [format %.2f [expr {double($Sent)/$duration}]]
            puts "CLIENT: $mbps MB/s (Sent $Sent bytes in $duration usecs)"
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
                puts "SERVER: $mbps MB/s (Rcvd $Received bytes in $duration usecs)"
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
            puts "$mbps $Sent [format_duration $duration] [join $options(-provider) ->]"
        }
    }
}

proc client::client {args} {
    variable control

    reset_options

    set repeat 1
    if {[dict exists $args -repeat]} {
        set repeat [dict get $args -repeat]
        if {[incr repeat 0] < 0} {
            error "Invalid repeat count \"$repeat\"."
        }
    }
    if {[dict exists $args -print]} {
        set print_level [dict get $args -print]
    } else {
        set print_level summary
    }
    connect {*}$args
    for {set i 0} {$i < $repeat} {incr i} {
        print [runtest {*}$args] $print_level
    }
    close $control(so)
}

proc client::batch {args} {
    variable control

    if {[dict exists $args -print]} {
        set print_level [dict get $args -print]
    } else {
        set print_level summary
    }

    if {[dict exists $args -repeat]} {
        set nrepeats [dict get $args -repeat]
    } else {
        set nrepeats 1
    }
    
    connect $args
    if {[dict exists $args -script]} {
        set inchan [open [dict get $args -script]]
    } else {
        set inchan stdin
    }

    while {[gets $inchan line] >= 0} {
        reset_options
        set line [string trim $line]
        if {[string length $line] == 0 || [string index $line 0] eq "#"} {
            continue;           # Blank or comment
        }
        for {set i 0} {$i < $nrepeats} {incr i} {
            # Not kosher to mix lists and string but what the heck...
            print [runtest {*}[concat $args $line]] $print_level
        }
    }

    if {$inchan ne "stdin"} {
        close $inchan
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

proc server::server {args} {
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
        puts stderr "Could not load Iocpsock package. Iocpsock will not be available."
    } else {
        set listeners(iocpsock) [socket2 -server [namespace current]::accept_data 0]
        set listening_ports(iocpsock) [lindex [fconfigure $listeners(iocpsock) -sockname] 2]
        puts stdout "Iocpsock socket listening on $listening_ports(iocpsock)."
    }

    vwait forever
}

proc server::accept_control {so addr port} {
    puts "Control connection from $addr/$port."
    fileevent $so readable [list [namespace current]::read_control $so]
    fconfigure $so -buffering line
}

proc server::end_test {controlso dataso} {
    # Send back test end response to client on control socket
    #  controlso - control socket
    #  dataso - data socket
    variable sockets
    variable clients
    set result $sockets($dataso)
    dict set result Socket [fconfigure $dataso]
    close $dataso
    unset sockets($dataso)
    puts $controlso [list OK $result]
    set peer [dict get $result Socket -peername]
    dict unset clients [lindex $peer 0] [lindex $peer 2]
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
                        if {[dict exists $sockets($dataso) End]} {
                            end_test $so $dataso
                        } else {
                            # Have not finished reading data yet. Leave marker
                            # for read handler to respond to client when done.
                            dict set sockets($dataso) Control $so
                        }
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
    #puts "Received $len"
    dict incr sockets($so) Received $len
    if {$len < $options(-readsize)} {
        if {[eof $so]} {
            # Note: Do not close socket since we do not want port reused until
            # client collects statistics
            puts $so [dict get $sockets($so) Received]; flush $so
            fileevent $so readable {}
            dict set sockets($so) End [clock microseconds]
            # If we have to respond to client, do so
            if {[dict exists $sockets($so) Control]} {
                end_test [dict get $sockets($so) Control] $so
            }
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
                client::client {*}[lrange $argv 1 end]
            }
            batch {
                client::batch {*}[lrange $argv 1 end]
            }
            server {
                server::server {*}[lrange $argv 1 end]
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
