# Copyright (c) 2020 Ashok P. Nadkarni
# All rights reserved.
# See LICENSE file for details.
# For instructions:
#   tclsh netrate.tcl help
#

namespace eval client {
    variable client_code {
        namespace eval {client} {
            # Array -
            #  limit - limit on number of connections
            #  closed - number closed
            variable counts
        }
        proc socket_command {provider} {
            if {$provider eq "tcl"} {
                return socket
            } elseif {$provider eq "iocp"} {
                uplevel #0 package require iocp_inet
                return iocp::inet::socket
            } elseif {$provider eq "iocpsock"} {
                uplevel #0 package require Iocpsock
                return socket2
            } else {
                error "Unknown socket provider $provider."
            }
        }
        proc client::on_read {so} {
            variable counts
            variable done
            set in [read $so 1]
            close $so
            if {[string length $in] == 0} {
                puts "Failed"
            }
            if {[incr counts(closed)] >= $counts(limit)} {
                set done 1
            }
        }
        proc client::connect {provider addr port} {
            set so [[socket_command $provider] -async $addr $port]
            fileevent $so readable [list [namespace current]::on_read $so]
        }
        proc client::run {provider addr port count} {
            variable counts
            set counts(limit)  $count
            set counts(closed) 0
            set start [clock microseconds]
            for {set i 0} {$i < $count} {incr i} {
                connect $provider $addr $port
            }
            vwait [namespace current]::done
            set end [clock microseconds]
            return [list $counts(closed) $start $end]
        }
    }
}

namespace eval server {
    proc socket_command {provider} {
        if {$provider eq "tcl"} {
            return socket
        } elseif {$provider eq "iocp"} {
            uplevel #0 package require iocp_inet
            return iocp::inet::socket
        } elseif {$provider eq "iocpsock"} {
            uplevel #0 package require Iocpsock
            return socket2
        } else {
            error "Unknown socket provider $provider."
        }
    }

}

namespace eval client {}

proc client::client {args} {
    variable client_code
    uplevel #0 $client_code

    set addr 127.0.0.1
    if {[dict exists $args -server]} {
        set addr [dict get $args -server]
    }
    set port 10102
    if {[dict exists $args -port]} {
        set port [dict get $args -port]
    }
    set count 100
    if {[dict exists $args -count]} {
        set count [dict get $args -count]
    }
    set provider tcl
    if {[dict exists $args -provider]} {
        set provider [dict get $args -provider]
    }
    lassign [run $provider $addr $port $count] count start end
    puts "[expr {(double($count) * 1000000)/($end - $start)}] C/s"
}

proc server::accept {so addr port} {
    puts -nonewline $so x
    close $so
}

proc server::server {args} {
    set port 10102
    if {[dict exists $args -port]} {
        set port [dict get $args -port]
    }
    set provider tcl
    if {[dict exists $args -provider]} {
        set provider [dict get $args -provider]
    }
    set listener [[socket_command $provider] -server [namespace current]::accept $port]
    vwait forever
}

if {[string equal -nocase [file normalize [info script]/...] [file normalize $argv0/...]]} {
    if {[llength $argv] == 0} {
        usage
    } else {
        switch -exact -- [lindex $argv 0] {
            client {
                client::client {*}[lrange $argv 1 end]
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
