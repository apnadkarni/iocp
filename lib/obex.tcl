#
# Copyright (c) 2020, Ashok P. Nadkarni
# All rights reserved.
#
# See the file LICENSE for license

namespace eval obex {
    variable HeaderIds
    proc HeaderId {name} {
        variable HeaderIds
        # Initialize
        array set HeaderIds {
            Count                  0xC0
            Name                   0x01
            Type                   0x42
            Length                 0xC3
            Timestamp              0x44
            Timestamp4             0xC4
            Description            0x05
            Target                 0x46
            Http                   0x47
            Body                   0x48
            EndOfBody              0x49
            Who                    0x4A
            ConnectionId           0xCB
            Parameters             0x4C
            AuthChallenge          0x4D
            AuthResponse           0x4E
            CreatorId              0xCF
            WanUuid                0x50
            ObjectClass            0x51
            SessionParameters      0x52
            SessionSequenceNumber  0x93
        }
        # Redefine ourself to do actual work
        proc HeaderId {name} {variable HeaderIds; return $HeaderIds($name)}
        return [HeaderId $name]
    }

    variable HeaderNames
    proc HeaderName {hid} {
        variable HeaderNames

        # Initialize HeaderNames
        variable HeaderIds
        HeaderId Count;         #Just to ensure HeaderIds initialized 
        foreach {name id} [array get $HeaderIds] {
            set HeaderNames($id) $name
        }

        # Redefine ourself to do actual work
        proc HeaderName {hid} {
            variable HeaderNames;
            set hid [format 0x%2.2X $hid]
            if {[info exists HeaderNames($hid)]} {
                return $HeaderNames($hid)
            }
            return $hid
        }
        return [HeaderName $hid]
    }
}

proc obex::connect_packet {args} {
    set headers [HeaderEncode {*}$args]
    # Packet is opcode 0x80, 2 bytes length, version (1.0->0x10),
    # flags (0), 2 bytes max len
    # followed by headers
    set len [expr {7+[string length $headers]}]
    append packet [binary format cSuccSu 0x80 $len 0x10 0 8192] $headers
    return $packet
}

proc obex::disconnect_packet {args} {
    set headers [HeaderEncode {*}$args]
    # Packet is opcode 0x81, 2 bytes length, followed by headers
    set len [expr {3+[string length $headers]}]
    append packet [binary format cSu 0x81 $len] $headers
    return $packet
}

proc obex::HeaderEncode {args} {
    variable HI
    if {[llength $args] == 0} {
        set args [lindex $args 0]
    }

    set encoded ""
    foreach {header_name header_value} $args {
        set hi $HI($header_name)
        # Top 2 bits encode data type
        switch -exact -- [expr {$hi >> 6}] {
            0 {
                # Encode as big-endian unicode
                set unicode_be [ToUnicodeBE $header_value]
                # Add a length field where the length includes 3 bytes for Header byte
                # and 2 bytes for length, and the 2 bytes for terminating Unicode null.
                append encoded \
                    [binary format cS $hi [expr {3+[string length $unicode_be]+2}]] \
                    $unicode_be "\0\0"
            }
            1 {
                # Just an array of bytes. Caller has to ensure that is what it  is.
                append encoded \
                    [binary format cS $hi [expr {3 + [string length $header_value]}]] \
                    $header_value
            }
            2 {
                # Single byte
                append encoded [binary format cc $hi $header_value]
            }
            3 {
                # Big endian 4 bytes
                append encoded [binary format cI $hi $header_value]
            }
        }
    }

    return $encoded
}

proc obex::HeaderDecode {bytes start} {
    if {[binary scan $bytes x${start}cu hid] != 1} {
        error "Empty Obex header"
    }
    set trailing_len [expr {[string length $bytes] - $start}]
    set name [HeaderName $hid]
    switch -exact -- [expr {$hid >> 6}] {
        0 {
            # Null-terminated Unicode string
            # Length must be at least 5 bytes - header id,2-byte len and terminating \0\0
            # Also, the string must have enough bytes for the indicated header length.
            # Skip $start bytes, skip hid byte
            if {[binary scan $bytes x${start}xSu len] != 1 ||
                $len < 5 ||
                $len > $trailing_len} {
                error "Invalid Obex header length."
            }
            # Note last byte of unicode is 3 bytes from end
            # (2 null bytes and 0-based index)
            set value [FromUnicodeBE [string range $bytes [expr {$start+3}] [expr {$start+$len-3}]]]
        }
        1 {
            # Raw bytes
            # Length must be at least 3 bytes - header id,2-byte len.
            # Also, the string must have enough bytes for the indicated header length.
            # Skip $start bytes, skip hid byte
            if {[binary scan $bytes x${start}xSu len] != 1 ||
                $len < 3 ||
                $len > $trailing_len} {
                error "Invalid Obex header length."
            }
            set value [string range $bytes [expr {$start+3}] [expr {$start+$len-1}]]
        }
        2 {
            # Single byte value. Always treated as unsigned 8 bits
            # Caller will need to convert to signed 8-bits if desired.
            if {2 > $trailing_len} {
                error "Invalid Obex header length."
            }
            set len 2
            binary scan $bytes x${start}xc value
        }
        3 {
            # 4-byte byte value. Always treated as signed 32 bits
            # Caller will need to convert to signed 32-bits if desired.
            if {5 > $trailing_len} {
                error "Invalid Obex header length."
            }
            set len 5
            binary scan $bytes x${start}xI value
        }
    }

    # Return the name, the value and the new offset for next header element
    return $name $value [expr {$start+$len}]
}


if {$::tcl_platform(byteOrder) "littleEndian"} {
    proc obex::ToUnicodeBE {s} {
        set be ""
        set le [encoding convertto unicode $s]
        set n [expr {[string length $le] / 2}]
        # TBD - measure alternate ways. E.g split into list
        for {set i 0} {$i < $n} {incr i} {
            append be [string index $le $i+1] [string index $le $i]
        }
        return $be
    }
    proc obex::FromUnicodeBE {be} {
        set le ""
        set n [expr {[string length $be]/2}]
        for {set i 0} {$i < $n} {incr i} {
            append le [string index $be $i+1] [string index $be $i]
        }
        return [encoding convertfrom unicode $le]
    }
} else {
    proc obex::ToUnicodeBE {s} {
        return [encoding convertto unicode $s]
    }
    proc obex::FromUnicodeBE {be} {
        return [encoding convertfrom unicode $be]
    }
}
