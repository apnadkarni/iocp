# From https://wiki.tcl-lang.org/page/Socket%20Performance%20Analysis
#
# On server -
# set s [socket -server serv 7777]
#
# On client -
# Setup
# Bench utf-8
# Bench binary
# Bench iso8859-1

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
proc Bench {encoding {socket_command socket}} {
    puts "Using $socket_command"
    set c [$socket_command localhost 7777]
    fconfigure $c -encoding $encoding -buffering line
    puts $c $encoding
    set t [lindex [time {puts -nonewline $c $::s; flush $c}] 0]
    puts [expr {(double($::l)/1024/1024)/(double($t)/1000/1000)}]
    close $c
    after 1000; # let the server settle down before we proceed
}
