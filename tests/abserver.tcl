package require iocp_inet
package require Iocpsock

proc readrequest {chan} {
    read $chan
    puts -nonewline $chan "HTTP/1.1 200 OK\r\n\r\n<!doctype html><html></html>"
    close $chan
}

proc accept {chan addr port} {
    fconfigure $chan -buffering none -blocking 0
    fileevent $chan readable [list readrequest $chan]
}

fconfigure [socket -server accept 8080] {*}$argv
fconfigure [iocp::inet::socket -server accept 8081] {*}$argv -maxpendingaccepts 5
fconfigure [socket2 -server accept 8082] {*}$argv

if {[file normalize [info script]/...] eq [file normalize $argv0/...]} {
    vwait forever    
}
