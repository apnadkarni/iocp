proc Setup {} {
    set ::s [string repeat "0123456789" 8192000]
    set ::l [string length $::s]
}

proc serv {ss ip port} {
    global cons ;
    lappend cons $ss;
    set encoding [gets $ss]
    fconfigure $ss -encoding $encoding
    puts "length: [string length [read $ss]]; encoding: $encoding"
    close $ss
}

### client
proc bench {socket_command server port socket_opts} {
    puts "Using $socket_command"
    set so [$socket_command $server $port]
    fconfigure $so {*}$socket_opts
    set t [lindex [time {puts -nonewline $so $::s; flush $c}] 0]
    puts [expr {(double($::l)/1024/1024)/(double($t)/1000/1000)}]
    close $so
}

proc socket_command {provider} {
    if {$provider eq "tcl"} {
        return socket
    } elseif {$provider eq "iocp"} {
        return iocp::inet::socket
    } elseif {$provider eq "iocpsock"} {
        return socket2
    } else {
        error "Unknown socket provider $provider."
    }
}

proc client {args} {
    set opts [dict merge {
        -server 127.0.0.1
        -port 10101
        -buffering none
        -encoding binary
        -translation binary
        -provider tcl
    } $args]

    set socket_opts [list \
        -buffering [dict get $opts -buffering] \
        -encoding [dict get $opts -encoding] \
        -translation [dict get $opts -translation]
    ]
    set socket_command [socket_command $opts(-provider)]

    # Control channel always uses Tcl sockets
    set control_channel [socket [dict get $opts -server] [dict get $opts -port]]
    puts $control_channel PORTS
    lassign [gets $control_channel] status data_ports
    if {$status ne "OK"} {
        error "Server failure: $status $data_ports"
    }
    puts stdout "Server data ports: $data_ports"

    puts $control_channel [list SOCONFIG {*}$socket_opts]
    gets $control_channel line
    if {$line ne "OK"} {
        error "Server failure: $line"
    }

    if {![dict exists $data_ports $opts(-provider)]} {
        error "Server does not support $opts(-provider)."
    }
    bench $socket_command \
        $opts(-server) \
        [dict get $data_ports $opts(-provider)] $socket_opts
}

proc server {args} {
    global listeners;           # Listening sockets
    global listening_ports;     # Corresponding ports
    global soconfig;            # Socket config options

    set port 0
    if {[dict exists $args -port]} {
        set port [dict get $args -port]
    }
    socket -server accept_control $port

    set listeners(tcl) [socket -server accept_data 0]
    set listening_ports(tcl) [lindex [fconfigure $listeners(tcl) -sockname] 2]
    puts stdout "Tcl socket listening on $listening_ports(tcl)."

    if {[catch {uplevel #0 package require iocp_net}]} {
        set listeners_ports(iocp) 0
        puts stderr "Could not load iocp package."
    }
    set listeners(iocp) [iocp::inet::socket -server accept_data 0]
    set listening_ports(iocp) [lindex [fconfigure $listeners(iocp) -sockname] 2]
    puts stdout "iocp socket listening on $listening_ports(iocp)."

    if {[catch {uplevel #0 package require Iocpsock}]} {
        set listeners_ports(iocpsock) 0
        puts stderr "Could not load Iocpsock package."
    }
    set listeners(iocpsock) [socket2 -server accept_data 0]
    set listening_ports(iocpsock) [lindex [fconfigure $listeners(iocpsock) -sockname] 2]
    puts stdout "Iocpsock socket listening on $listening_ports(iocpsock)."
}

proc accept_control {so addr port} {
    fconfigure $so -blocking 0 -buffering line
    fileevent $so readable [list read_control $so]
}

proc read_control {so} {
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
        # Received a command
        set opts [lassign [split $line] command]
        switch -exact -- $command {
            PORTS {
                puts $so [list OK tcl $listening_ports(tcl) \
                              iocp $listening_ports(iocp) \
                              iocpsock $listening_ports(iocpsock)]
            }
            SOCONFIG {
                set soconfig $opts
                puts $so OK
            }
            default {
                puts stderr "Unknown command \"$command\"."
            }
        }
    }
}

if {[string equal -nocase [file normalize [info script]/...] [file normalize $argv0/...]]} {
    if {[llength $argv] == 0} {
        server
    }
    switch -exact -- [lindex $argv 0] {
        client - 
        server {
            {*}$argv
        }
        default {
            usage
        }
    }
}
