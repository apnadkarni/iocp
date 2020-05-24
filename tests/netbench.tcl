namespace eval client {
    # Program options
    # -provider (tcl/iocp/iocpsock)
    # -writesize
    # -server
    # -port
    # -payload
    # -count
    # -duration

    variable options
    array set options {
        -writesize 4096
        -provider tcl
    }

    # Socket configuration options
    variable sooptions
    array set sooptions {}

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

    puts "Using $provider sockets"
    set payload [payload $options(-payload)]

    # IMPORTANT:
    # Do NOT do any operations on $payload other than writing it
    # since [string length] etc. will all shimmer it.
    set so [$options(-socketcommand) $options(-server) [dict get $options(-dataports) $provider]]
    fconfigure $so {*}[array get sooptions]

    set start [clock microseconds]
    if {[info exists options(-count)]} {
        set i 0
        while {$i < $count} {
            # Note: DON'T flush here because that should be controlled
            # via -buffering option
            puts -nonewline $so $payload
            incr i
        }
        set sent [expr {$i * $options(-writesize)}]
    } else {
        set sent 0
        set duration [expr {$options(-duration) * 1000000}]
        while {$duration > 0} {
            puts -nonewline $so $payload
            set duration [expr {$duration - ([clock microseconds] - $start)}]
            puts $duration
            incr sent $options(-writesize)
        }
    }
    set end [clock microseconds]

    set configuration [fconfigure $so]
    close $so
    return [list Sent $sent \
                Duration [expr {$end - $start}] \
                Socket $configuration]
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

proc client::run {args} {
    variable options
    variable sooptions

    array set opts [dict merge {
        -server 127.0.0.1
        -port 10101
        -provider tcl
        -writesize 4096

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
        # Default to -translation assuming no imcompatible options specified
        if {(![info exists opts(-encoding)] || $opts(-encoding) eq "binary") &&
            (![info exists opts(-eofchar)] || $opts(-eofchar) eq "")} {
            set opts(-translation) binary
        }
    }

    foreach opt {-buffering -encoding -eofchar -translation} {
        if {[info exists opts($opt)]} {
            set sooptions($opt) $opts($opt)
            unset opts($opt)
        }
    }

    array set options [array get opts]
    
    if {[info exists options(-payload)]} {
        if {$options(-payload) ni {text binary}} {
            error "Invalid -payload value \"$options(-payload)\"."
        }
    } else {
        if {$sooptions(-translation) eq "binary"} {
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
            set options(-duration) 10
        }
    }

    set options(-socketcommand) [socket_command $opts(-provider)]

    # Control channel always uses Tcl sockets
    set control_channel [socket $opts(-server) $opts(-port)]
    fconfigure $control_channel -buffering line
    puts $control_channel PORTS
    lassign [gets $control_channel] status data_ports
    if {$status ne "OK"} {
        error "Server failure: $status $data_ports"
    }
    puts stdout "Server data ports: $data_ports"

    puts $control_channel [list SOCONFIG [array get sooptions]]
    gets $control_channel line
    if {$line ne "OK"} {
        error "Server failure: $line"
    }

    if {![dict exists $data_ports $opts(-provider)]} {
        error "Server does not support $opts(-provider)."
    }
    set options(-dataports) $data_ports

    set result [bench $opts(-provider)]
    puts $result
    set sockname [dict get $result Socket -sockname]
    set id [list [lindex $sockname 0] [lindex $sockname 2]]
    puts $control_channel [list FINISH $id]
    puts stdout [gets $control_channel]
    close $control_channel

    puts Result:
    puts $result
}

################################################################
# SERVER

namespace eval server {}
proc server::run {args} {
    global listeners;           # Listening sockets
    global listening_ports;     # Corresponding ports
    global soconfig;            # Socket config options

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
    global listeners;           # Listening sockets
    global listening_ports
    global soconfig;            # Socket config options
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
        switch -exact -- $command {
            PORTS {
                puts $so [list OK [array get listening_ports]]
            }
            SOCONFIG {
                set soconfig $opts
                puts $so OK
            }
            FINISH {
                global sockets
                global clients
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
                puts stderr "Unknown command \"$command\"."
            }
        }
    }
}

proc server::accept_data {so addr port} {
    global soconfig
    global sockets
    global clients

    set opts [list -blocking 0]
    foreach opt {-buffering -translation -encoding} {
        if {[info exists soconfig($opt)]} {
            lappend opts $opt $soconfig($opt)
        }
    }
    puts stdout "Data connection from $addr/$port. Setting socket to $opts."
    fconfigure $so {*}$opts

    fileevent $so readable [list [namespace current]::read_data $so]
    set sockets($so) [dict create Remote [list $addr $port] Received 0 Sent 0 Start [clock microseconds]]
    dict set clients $addr $port $so
}

proc server::read_data {so} {
    global sockets

    if {[catch {
        read $so
    } data]} {
        puts stderr "Read error on socket ([dict get $sockets($so) Remote]): $data"
        # Our command protocol does not like newlines :-()
        # Note: do not close socket until client retrieves status.
        dict set sockets($so) Error [string map [list \n " "] $data]
        fileevent $so readable {}
        return
    }
    set len [string length $data]
    if {$len} {
        dict incr sockets($so) Received $len
    } else {
        if {[eof $so]} {
            # Note: Do not close socket since we do not want port reused until
            # client collects statistics
            fileevent $so readable {}
            dict set sockets($so) End [clock microseconds]
        } else {
            # No data available
        }
    }
}

if {[string equal -nocase [file normalize [info script]/...] [file normalize $argv0/...]]} {
    if {[llength $argv] == 0} {
        server::run
    } else {
        switch -exact -- [lindex $argv 0] {
            client {
                client::run {*}[lrange $argv 1 end]
            }
            server {
                server::run {*}[lrange $argv 1 end]
            }
            default {
                usage
            }
        }
    }
}
