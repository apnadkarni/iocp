package require Thread

proc accept args {}

set victim [thread::create]
thread::send $victim [list proc accept args {}]
set so [socket -server accept 12345]
thread::transfer $victim $so
update
while {1} {
    set client [socket 127.0.0.1 12345]
    update
    close $client
}
