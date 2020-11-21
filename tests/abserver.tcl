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
puts stderr "Tcl socket listening on 8082"

if {[catch {
    package require iocp_inet
    fconfigure [iocp::inet::socket -server accept 8081] {*}$argv
} result]} {
    puts stderr "iocp::inet disabled: $result"
} else {
    puts stderr "iocp::inet listening on 8081"
}

if {[catch {
    package require Iocpsock
    fconfigure [socket2 -server accept 8082] {*}$argv
} result]} {
    puts stderr "Iocpsock disabled: $result"
} else {
    puts stderr "Iocpsock listening on 8082"
}

puts stderr "Client command examples:"
puts stderr "  ab -n COUNT -c CONCURRENCY http://localhost:8081/"
puts stderr "  ab -t SECS -c CONCURRENCY http://localhost:8081/"

if {[file normalize [info script]/...] eq [file normalize $argv0/...]} {
    vwait forever    
}
