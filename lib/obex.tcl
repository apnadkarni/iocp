#
# Copyright (c) 2020, Ashok P. Nadkarni
# All rights reserved.
#
# See the file LICENSE for license

namespace eval obex {

    namespace eval request {
        namespace path [namespace parent]
        namespace export encode decode encode_setpath
        namespace ensemble create

        variable OpNames
        variable OpCodes
    }

    namespace eval response {
        namespace path [namespace parent]
        namespace export encode decode
        namespace ensemble create

        variable StatusNames
        variable StatusCodes
    }

    namespace eval header {
        namespace path [namespace parent]
        namespace export encode decode find
        namespace ensemble create

        variable Ids
        variable Names
    }
}


proc obex::request::encode {op args} {
    set op [OpCode $op]
    if {$op == 0x80} {
        return [EncodeConnect {*}$args]
    } elseif {$op == 0x87} {
        return [encode_setpath 0 0 {*}$args]
    }
    # Generic request encoder
    set headers [header encode {*}$args]
    # Packet is opcode, 2 bytes length, followed by headers
    set len [expr {3+[string length $headers]}]
    append packet [binary format cSu $op $len] $headers
    return $packet

}

proc obex::request::decode {packet} {
    if {[binary scan $packet cuSu op len] != 2 ||
        $len > [string length $packet]} {
        return [list Status error \
                    ErrorMessage "Truncated OBEX packet."]
    }
    if {$op == 0x80} {
        # CONNECT request
        # Packet is opcode 0x80, 2 bytes length, version (1.0->0x10),
        # flags (0), 2 bytes max len followed by headers
        if {[binary scan $packet x3cucuSu version flags maxlen] != 3} {
            return [list Status error ErrorMessage "Truncated OBEX request."]
        }
        return [list \
                    OpCode $op \
                    Final  [expr {($op & 0x80) == 0x80}] \
                    OpName [OpName $op] \
                    MajorVersion [expr {$version >> 4}] \
                    MinorVersion [expr {$version & 0xf}] \
                    Flags  $flags \
                    MaxLength $maxlen \
                    Headers [header decode $packet 7] \
                   ]
    } elseif {$op == 0x87} {
        # SETPATH request
        if {[binary scan $packet x3cucu flags constants] != 2} {
            return [list Status error ErrorMessage "Truncated OBEX request."]
        }
        return [list \
                    OpCode $op \
                    Final  [expr {($op & 0x80) == 0x80}] \
                    OpName [OpName $op] \
                    Flags  $flags \
                    Constants $constants \
                    Headers [header decode $packet 5] \
                   ]
    } else {
        return [list \
                    OpCode $op \
                    OpName [OpName $op] \
                    Final  [expr {($op & 0x80) == 0x80}] \
                    Headers [header decode $packet 3] \
                   ]
    }

}

proc obex::request::EncodeConnect {args} {
    set headers [header encode {*}$args]
    # Packet is opcode 0x80, 2 bytes length, version (1.0->0x10),
    # flags (0), 2 bytes max len
    # followed by headers
    set len [expr {7+[string length $headers]}]
    # Max acceptable packet length 65535
    append packet [binary format cuSucucuSu 0x80 $len 0x10 0 65535] $headers
    return $packet
}

proc obex::request::encode_setpath {flags constants args} {
    set headers [header encode {*}$args]
    # Packet is opcode 0x85, 2 bytes length,
    # flags, constants, # followed by headers
    set len [expr {5+[string length $headers]}]
    append packet [binary format cuSucucu 0x85 $len flags constants] $headers
    return $packet
}


proc obex::response::decode {packet request_op} {
    # Decodes a standard response which has no leading fields other
    # than the opcode and length.
    #  packet - Binary OBEX packet.
    #  request_op - The request opcode corresponding to this response.
    #
    # The returned dictionary has the following keys:
    #  Final        - 1/0 depending on whether the `final` bit was set
    #                 in the response operation code or not.
    #  Headers      - List of headers received in the packet.
    #  Status       - The general status category.
    #  StatusCode   - The numeric response status code from server.

    # TBD - do we need to check for ABORT packet as well?

    if {[binary scan $packet cuSu status len] != 2 ||
        $len > [string length $packet]} {
        return [list StatusCode [StatusCode protocolerror] \
                    Status protocolerror \
                    ErrorMessage "Truncated OBEX response."]
    }

    set request_op [request::OpName $request_op]
    if {$request_op eq "connect"} {
        return [DecodeConnect $packet]
    }

    return [list \
                StatusCode     $status \
                Status [StatusCategory $status] \
                Final          [expr {($status & 0x80) == 0x80}] \
                Headers        [header decode $packet 3] \
               ]
}

proc obex::response::DecodeConnect {packet} {
    # Decodes a OBEX response to a connect request.
    #  packet - Binary OBEX packet.
    #
    # The dictionary returned by the command has the following keys:
    #  ErrorMessage - If set, a human-readable error message.
    #  Final        - 1/0 depending on whether the `final` bit was set
    #                 in the response operation code or not.
    #  Flags        - Currently always 0.
    #  Headers      - List of headers received in the packet.
    #  MaxLength    - Maximum length OBEX packet length the server can
    #                 receive.
    #  Status       - The general status category.
    #  StatusCode   - The numeric status code from server.
    #  MajorVersion - The OBEX protocol major version returned by server.
    #  MinorVersion - The OBEX protocol minor version returned by server.

    # OBEX Section 3.3.18 - 0xa0 success, any other response is a failure.
    # It is still supposed to include same fields
    if {[binary scan $packet cuSucucuSu status len version flags maxlen] != 5 ||
        $len > [string length $packet]} {
        return [list StatusCode [StatusCode protocolerror] \
                    Status protocolerror \
                    ErrorMessage "Truncated OBEX response."]
    }
    return [list \
                StatusCode $status \
                Status [StatusCategory $status] \
                Final  [expr {($status & 0x80) == 0x80}] \
                MajorVersion [expr {$version >> 4}] \
                MinorVersion [expr {$version & 0xf}] \
                Flags  $flags \
                MaxLength $maxlen \
                Headers [header decode $packet 7] \
           ]
}

proc obex::packet_length {packet} {
    # Get the length of a packet.
    #  packet - an OBEX packet or the initial fragment of one with
    #           at least three bytes.
    # Returns the packet length as encoded in its header or 0 if the passed
    # fragment is too short to contain a length field.

    if {[binary scan $packet xSu -> len] != 1} {
        return 0
    }
    return $len
}


proc obex::header::encode {header_name header_value} {
    set hi [Id $header_name]
    # Top 2 bits encode data type
    switch -exact -- [expr {$hi >> 6}] {
        0 {
            # Encode as big-endian unicode
            set unicode_be [ToUnicodeBE $header_value]
            # Add a length field where the length includes 3 bytes for Header byte
            # and 2 bytes for length, and the 2 bytes for terminating Unicode null.
            set hlen [expr {3+[string length $unicode_be]+2}]
            append encoded [binary format cS $hi $hlen] $unicode_be "\0\0"
            return $encoded
        }
        1 {
            # Just an array of bytes. Caller has to ensure that is what it  is.
            set hlen [expr {3 + [string length $header_value]}]
            append encoded [binary format cS $hi $hlen] $header_value
            return $encoded
        }
        2 {
            # Single byte. Always room since space_left check at top
            return [binary format cc $hi $header_value]
        }
        3 {
            # Big endian 4 bytes
            return [binary format cI $hi $header_value]
        }
    }
}

proc obex::header::encoden {args} {
    if {[llength $args] == 1} {
        set args [lindex $args 0]
    }
    set headers {}
    foreach {name value} {
        lappend headers [encode $name $value]
    }
    return $headers
}

proc obex::header::TBDencoden {maxlen args} {
    if {[llength $args] == 1} {
        set args [lindex $args 0]
    }

    set encoded {}
    set unencoded_headers {}
    set space_left $maxlen
    foreach {header_name header_value} $args {
        if {$space_left == 0} {
            lappend unencoded_headers $header_name $header_value
            continue
        }
        set hi [Id $header_name]
        # Top 2 bits encode data type
        switch -exact -- [expr {$hi >> 6}] {
            0 {
                # Encode as big-endian unicode
                set unicode_be [ToUnicodeBE $header_value]
                # Add a length field where the length includes 3 bytes for Header byte
                # and 2 bytes for length, and the 2 bytes for terminating Unicode null.
                set hlen [expr {3+[string length $unicode_be]+2}]
                if {$hlen > $space_left} {
                    lappend unencoded_headers $header_name $header_value
                    set space_left 0; # So remaining headers are ignored as well
                } else {
                    lappend encoded \
                        [binary format cS $hi $hlen] $unicode_be "\0\0"
                    set space_left [expr {$space_left - hlen}]
                }
            }
            1 {
                # Just an array of bytes. Caller has to ensure that is what it  is.
                set hlen [expr {3 + [string length $header_value]}]
                if {$hlen > $space_left} {
                    lappend unencoded_headers $header_name $header_value
                    set space_left 0; # So remaining headers are ignored as well
                } else {
                    lappend encoded \
                        [binary format cS $hi $hlen] $header_value
                    set space_left [expr {$space_left - hlen}]
                }
            }
            2 {
                # Single byte. Always room since space_left check at top
                lappend encoded [binary format cc $hi $header_value]
                incr space_left -1
            }
            3 {
                # Big endian 4 bytes
                if {4 > $space_left} {
                    lappend unencoded_headers $header_name $header_value
                    set space_left 0; # So remaining headers are ignored as well
                } else {
                    lappend encoded [binary format cI $hi $header_value]
                    incr space_left -4
                }
            }
        }
    }

    return [list $encoded $unencoded_headers]
}

proc obex::header::DecodeFirst {bytes start} {
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
    return [list $name $value [expr {$start+$len}]]
}

proc obex::header::decode {bytes start} {
    set nbytes [string length $bytes]
    set headers {}
    while {$start < $nbytes} {
        lassign [DecodeFirst $bytes $start] name value start
        lappend headers $name $value
    }
    return $headers
}

proc obex::header::find {headers key outvar} {
    if {[llength $headers] == 1} {
        set headers [lindex $headers 0]
    }
    set key [Name $key]
    foreach {name val} $headers {
        if {[string equal -nocase $key $name]} {
            upvar 1 $outvar v
            set v $val
            return 1
        }
    }
    return 0
}

if {$::tcl_platform(byteOrder) eq "littleEndian"} {
    proc obex::ToUnicodeBE {s} {
        set be ""
        set le [encoding convertto unicode $s]
        set n [string length $le]
        # TBD - measure alternate ways. E.g split into list
        for {set i 0} {$i < $n} {incr i 2} {
            append be [string index $le $i+1] [string index $le $i]
        }
        return $be
    }
    proc obex::FromUnicodeBE {be} {
        set le ""
        set n [string length $be]
        for {set i 0} {$i < $n} {incr i 2} {
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

proc obex::MakeBinUuid {uuid} {
    if {![regexp {^[[:xdigit:]]{8}-([[:xdigit:]]{4}-){3}[[:xdigit:]]{12}$} $uuid]} {
       error "Invalid UUID."
    }
    return [binary decode hex [string map {- {}} $uuid]]
}

proc obex::request::OpName {op} {
    variable OpNames
    array set OpNames {
        0x80 connect
        0x81 disconnect
        0x02 put
        0x82 putfinal
        0x03 get
        0x83 getfinal
        0x85 setpath
        0x87 session
        0xff abort
    }
    proc OpName {op} {
        variable OpNames
        set op [format 0x%2.2X $op]
        if {[info exists OpNames($op)]} {
            return $OpNames($op)
        }
        return Request_$op
    }
    return [OpName $op]
}

proc obex::request::OpCode {op} {
    variable OpCodes
    # Note OpCodes and OpNames are not symmetrical
    array set OpCodes {
        connect     0x80
        disconnect  0x81
        put         0x02
        putfinal    0x82
        get         0x03
        getfinal    0x83
        setpath     0x85
        session     0x87
        abort       0xff
    }
    proc OpCode {op} {
        variable OpCodes
        if {[string is integer -strict $op]} {
            if {$op > 0 && $op < 256} {
                return $op
            }
        } else {
            if {[info exists OpCodes($op)]} {
                return $OpCodes($op)
            }
        }
        error "Invalid request code \"$op\"."
    }
    return [OpCode $op]
}

proc obex::response::StatusName {op} {
    variable StatusNames
    # Hex -> name
    # Note 0x01 - 0x0f -> not defined in spec. Used by us internally.
    array set StatusNames {
        0x01 protocolerror
        0x10 continue
        0x20 ok
        0x21 created
        0x22 accepted
        0x23 non-authoritative
        0x24 nocontent
        0x25 resetcontent
        0x26 partialcontent
        0x30 multiplechoices
        0x31 movedpermanently
        0x32 movedtemporarily
        0x33 seeother
        0x34 notmodified
        0x35 useproxy
        0x40 badrequest
        0x41 unauthorized
        0x42 paymentrequired
        0x43 forbidden
        0x44 notfound
        0x45 methodnotallowed
        0x46 notacceptable
        0x47 proxyauthenticationrequired
        0x48 requesttimeout
        0x49 conflict
        0x4a gone
        0x4b lengthrequired
        0x4c preconditionfailed
        0x4d requestedentitytoolarge
        0x4e requesturltoolarge
        0x4f unsupportedmediatype
        0x50 internalservererror
        0x51 notimplemented
        0x52 badgateway
        0x53 serviceunavailable
        0x54 gatewaytimeout
        0x55 httpversionnotsupported
        0x60 databasefull
        0x61 databaselocked
    }
    proc StatusName {op} {
        variable StatusNames
        set op [expr {$op & ~0x80}]; # Knock off final bit
        set op [format 0x%2.2X $op]
        if {[info exists StatusNames($op)]} {
            return $StatusNames($op)
        }
        return response_$op
    }
    return [StatusName $op]
}

proc obex::response::StatusCode {name} {
    variable StatusNames
    variable StatusCodes

    StatusName 0x20 ;# Initialize the StatusNames array
    foreach {k v} [array get StatusNames] {
        set StatusCodes($v) $k
    }

    proc StatusCode {name} {
        variable StatusCodes
        if {[info exists StatusCodes($name)]} {
            return $StatusCodes($name)
        }
        if {[string is integer -strict $name]} {
            return $name
        }
        error "Invalid response code \"$name\"."
    }

    return [StatusCode $name]
}

proc obex::response::StatusCategory {status} {
    if {$status < 0x10} {
        return protocolerror
    } elseif {$status < 0x20}
        return informational
    } elseif {$status < 0x30} {
        return success
    } elseif {$status < 0x40} {
        return redirect
    } elseif {$status < 0x50} {
        return clienterror
    } elseif {$status < 0x60} {
        return servererror
    } else {
        return unknown
    }
}

proc obex::header::Id {name} {
    variable Ids
    # Initialize
    array set Ids {
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
    proc Id {name} {variable Ids; return $Ids($name)}
    return [Id $name]
}

proc obex::header::Name {hid} {
    variable Names

    # Initialize Names
    variable Ids
    Id Count;             #Just to ensure Ids initialized 
    foreach {name id} [array get Ids] {
        set Names($id) $name
    }

    # Redefine ourself to do actual work
    proc Name {hid} {
        variable Names
        set hid [format 0x%2.2X $hid]
        if {[info exists Names($hid)]} {
            return $Names($hid)
        }
        return $hid
    }
    return [Name $hid]
}


oo::class create obex::Client {

    # The Obex protocol only allows one request at a time. Accordingly,
    # a Client can be in one of the following states:
    #  IDLE - no request is outstanding.
    #  BUSY - a request is outstanding. state(request) indicates the request type.
    #  ERROR - an error occurred on the conversation. Any request except abort
    #   will raise an exception.
    #
    # The state array contains the following keys:
    # Per connection:
    #  state - IDLE, BUSY, ERROR
    #  max_packet_len - max negotiated length of packet
    #  connection_id - ConnectionId as sent by remote. May not be present.
    #  connection_header - Binary connection header corresponding to $connection_id
    #  target - Target header sent, if any
    #  who    - Who header sent, if any
    # Per request:
    #  input - input buffer for received data
    #  request - current request.
    #  request_headers - headers to send in current request in encoded binary form
    #  request_body - content to send in request. If not present for a PUT,
    #   it is a PUT-DELETE request.
    #  request_body_offset - offset into request_body of next byte to send
    #  response - latest response
    #  response_headers - accumulated headers received in responses to request
    variable state

    constructor args {
        my reset
        if {[llength [self next]]} {
            next {*}$args
        }
    }

    method reset {} {
        # Resets state of the object.
        #
        # The object is placed in the same state as when it was newly constructed.
        # All state information is lost.

        # Connection specific state
        set state(state)  IDLE
        unset -nocomplain state(connection_id)
        unset -nocomplain state(connection_header)
        unset -nocomplain state(target)
        unset -nocomplain state(who)
        set state(max_packet_len) 255; # Assume min unless remote tells otherwise

        # Request specific state
        my ResetRequest
    }

    method input {data} {
        # Process data from the remote server.
        #   data - Binary data as received from remote server.
        # The method takes as input data received from the server as part of
        # the response to a request. The return value from the method is a list
        # of one or two elements. The first element is one of the following:
        #   `done`   - The full response has been received. The application
        #                  can then call any of the retrieval methods or initiate
        #                  another request. If the second element is present and
        #                  and not empty, it is data to be sent to the server.
        #                  The application can call other methods to retrieve
        #                  the result of the request. The application may also
        #                  call methods to initiate the next request.
        #   `continue` - The response has only been partially received. If
        #                  the second element is present and not empty, it is
        #                  data to be sent to the server. In either case, the
        #                  application should read more data from the server
        #                  and invoke the `input` method again passing it the
        #                  read data.
        #   `failed`     - The request has failed. See <Error handling> for
        #                  dealing with errors and failures. If
        #                  the second element is present and not empty, it is
        #                  data to be sent to the server. In either case, the
        #                  application must not invoke additional requests without
        #                  first calling the <reset> method.
        #
        # This method will raise an exception if no request is currently
        # outstanding.

        my AssertState BUSY

        # Append new data to existing
        append inbuf $data

        # Check if entire reponse had been received
        set input_length    [string length $inbuf]
        set response_length [packet_length $inbuf]
        # response_length == 0 => even length field not received yet.
        if {$response_length == 0 || $response_length > $input_length} {
            return continue
        }

        set response [response decode $inbuf $pending_request]
        # TBD - should this be a protocol error if non-0 remaining?
        # Possibly a response followed by an ABORT?
        set inbuf [string range $inbuf $packet_length end]

        # If we have a connection id, the incoming one must match if present
        if {[info exists connection_id]} {
            if {![header find $Headers ConnectionId conn_id] ||
                $connection_id != $conn_id} {
                # TBD - ignore mismatches for now
            }
        }

        # Save as latest response
        set state(response) $response

        # For multipart responses, collect headers
        lappend state(response_headers) {*}[dict get $response Headers]

        # Assume we are now free for more requests
        set state(state) IDLE

        # Do request-specific processing
        return [switch -exact -- $pending_request {
            connect    { my ConnectResponse }
            disconnect { my DisconnectResponse }
            put        { my PutResponse }
            get        { my GetResponse }
            setpath    { my SetPathResponse }
            session    { my SessionResponse }
            abort      { my AbortResponse }
        }]
    }

    method idle {} {
        return [expr {$state eq "IDLE"}]
    }

    method status {} {
        # Returns the status of the last response received.
        #
        # The command will raise an error if no response has been received
        # for any request.
        return [dict get $state(response) Status]
    }

    method status_detail {} {
        # Returns the detailed status of the last response received.
        #
        # The returned dictionary has the following keys:
        #  Status         - The generic status category.
        #  StatusCode     - The numeric status code from server.
        #  StatusName     - Mnemonic form of `StatusCode`
        #  ErrorMessage   - Additional human readable error status message. This
        #                   key may not be present.
        #
        # The command will raise an error if no response has been received
        # for any request.
        dict with state(response) {
            lappend status Status $Status \
                StatusCode $StatusCode \
                StatusName [response::StatusName $Status]
            if {[info exists ErrorMessage]} {
                lappend status ErrorMessage $ErrorMessage
            }
        }
        return $status
    }

    method connect {args} {
        # Generates a Obex connect request.
        #  args - If a single argument, it must be a dictionary mapping names
        #   of <Obex headers> to their values. Otherwise, it must be
        #   a list of alternating header names and values.
        #
        # It is the caller's responsibility to ensure the value associated
        # with the header is formatted as described in <Obex headers> and
        # that the supplied headers if any are acceptable in `connect` request.
        # The following headers are commonly used in connects:
        # `Target`, `Who`, `Count`, `Length` and `Description`.
        if {[info exists state(connection_id)]} {
            error "Already connected."
        }
        set state(request_headers) [header encoden {*}$args]
        # 7 bytes for fixed fields -> 255-7 = 248 for headers
        lassign [my PopHeaders 248] len headers
        if {[llength $state(request_headers)]} {
            # Not all headers fit. Connect request must be a single packet
            my RaiseError "Headers too long for connect request."
        }
        incr len 7;             # Fixed fields
        # Packet is opcode 0x80, 2 bytes length, version (1.0->0x10),
        # flags (0), 2 bytes max len (proposed)
        append packet [binary format cuSucucuSu 0x80 $len 0x10 0 65535] {*}$headers
        my BeginRequest connect
        return [list continue $packet]
    }

    method ConnectResponse {} {
        dict with state(response) {
            if {$StatusCode != 0xA0} {
                # As per Obex 1.3 sec 3.3.1.8 any other code is failure
                set state(state) ERROR
                return failed
            }
            # Store connection id if it exists
            if {[header find $Headers ConnectionId state(connection_id)]} {
                set state(connection_header) \
                    [header encode ConnectionId $state(connection_id)]
            }
            # On init we assume worst case of 255 byte packets. If
            # remote is willing to go higher, use that.
            if {$MaxLength > 255} {
                set state(max_packet_len) $MaxLength
            }
            # TBD - all other headers - Target, Who etc.
        }
        set state(state) IDLE
        return done
    }

    method disconnect {args} {
        # Generates a Obex disconnect request.
        #  args - If a single argument, it must be a dictionary mapping names
        #   of <Obex headers> to their values. Otherwise, it must be
        #   a list of alternating header names and values.
        #
        # It is the caller's responsibility to ensure the value associated
        # with the header is formatted as described in <Obex headers> and
        # that the supplied headers if valid for `disconnect` requests.
        # The `ConnectionId` header is automatically generated as needed
        # and shoould not be included by the caller.
        if {![info exists state(connection_id)]} {
            error "Not connected."
        }
        set state(request_headers) [header encoden {*}$args]
        # Header space is max packet minus packet header of 3 bytes
        lassign [my PopHeaders [expr {$state(max_packet_len) - 3}]] len headers
        if {[llength $state(request_headers)]} {
            # Not all headers fit. Disconnect request must be a single packet
            my RaiseError "Headers too long for disconnect request."
        }
        incr len 3;             # Fixed fields
        append packet [binary format cuSu 0x81 $len] {*}$headers
        my BeginRequest disconnect
        return [list continue $packet]
    }

    method DisconnectResponse {} {
        if {[dict get $state(response) StatusCode] != 0xA0} {
            # As per Obex 1.3 sec 3.3.1.8 any other code is failure
            set state(state) ERROR
            return failed
        } else {
            set state(state) IDLE
            return done
        }
    }

    method put {content args} {
        # Generates a Obex `PUT` request.
        #  content - Content to be sent to server as-is. This must be formatted
        #   appropriately based on the `Type` header, `Http` or `ObjectClass`
        #   headers passed in. If none of these are present,
        #   the server may interpret $content in any
        #   manner it chooses, possibly looking at the `Name` header if present,
        #   some default handling or even rejecting the request.
        #  args - If a single argument, it must be a dictionary mapping names
        #   of <Obex headers> to their values. Otherwise, it must be
        #   a list of alternating header names and values.
        #
        # It is the caller's responsibility to ensure the value associated
        # with the header is formatted as described in <Obex headers> and
        # that the supplied headers if any are acceptable in `put` request.
        # The following headers are commonly used in put operations:
        # `Name`, `Type`, `Http`, `Timestamp` and `Description`.
        # The headers `Body`, `EndOfBody`, `Length` and `ConnectionId`
        # are automatically generated and should not be passed in.
        if {[info exists state(connection_id)]} {
            error "Already connected."
        }
        # TBD - maybe break up content into body headers assuming body space
        #  is packet size - packet header - connection id header. That would
        # simplify Put method
        my BeginRequest put
        set state(request_headers) [header encoden {*}$args Length [string length $content]]
        set state(request_body) $content
        set state(request_body_offset) 0
        return [my Put]
    }

    method put_delete {args} {
        # Generates a Obex `PUT` request to delete an object.
        #  args - If a single argument, it must be a dictionary mapping names
        #   of <Obex headers> to their values. Otherwise, it must be
        #   a list of alternating header names and values.
        #
        # It is the caller's responsibility to ensure the value associated
        # with the header is formatted as described in <Obex headers> and
        # that the supplied headers if any are acceptable in `put` request.
        # The following headers are commonly used in put operations:
        # `Name`, `Type`, `Http`, `Timestamp` and `Description`.
        # The headers `Body`, `EndOfBody`, `Length` should not be present
        # in a delete operation and should not be passed in. Moreover,
        # `ConnectionId` header is automatically generated and should not
        # be passed in.
        if {[info exists state(connection_id)]} {
            error "Already connected."
        }
        my BeginRequest put
        set state(request_headers) [header encoden {*}$args Length [string length $content]]
        return [my Put]
    }

    method Put {} {
        # Put opcode+len occupies 3 bytes
        set space [expr {$state(max_packet_len)-3}]

        lassign [my PopHeaders $space] len headers
        incr len 3;         # For opcode fields
        set space [expr {$space - $len}]
        # If we still have more headers to send or remaining space is not
        # enough for even one byte of content body, or there is no body to
        # send (put-delete) send off what we have.
        if {[llength $state(request_headers)] ||
            $space < 4 ||
            ![info exists state(request_body)]
        } {
            # This is not strictly needed but preferable in most cases
            # that Body headers come last.
            # 0x02 -> PUT without FINAL bit
            append packet [binary format cuSu 0x02 $len] {*}$headers
            return [list continue $packet]
        }

        # We can send at least one byte
        set body_len [expr {
                            [string length $state(request_body)]
                            - $state(request_body_offset)
                        }]
        incr space -3;          # Body header
        if {$space >= $body_len} {
            # We can send the entire body.
            set op 0x82;        # FINAL + PUT
            set hdrop EndOfBody
        } else {
            set body_len $space
            set op 0x02;        # PUT only
            set hdrop Body
        }
        set body_header [header encode $hdrop \
                             [string range $state(request_body) \
                                  $state(request_body_offset) \
                                  [expr {$state(request_body_offset) + $body_len - 1}] \
                                 ]]
        incr state(request_body_offset) $body_len
        incr len [string length $body_header]
        append packet [binary format cuSu $op $len] {*}$headers $body_header
        return [list continue $packet]
    }

    method PutResponse {} {
        set status_code [dict get $state(response) StatusCode]
        if {$status_code == 0xA0} {
            # Success. Double check that all data was sent.
            if {[info exists state(request_body)] &&
                $state(request_body_offset) < [string length $state(request_body)]} {
                set state(state) ERROR
                # TBD - set status, error message to protocol error.
                return failed
            } else {
                # Ready to accept more requests.
                set state(state) IDLE
                return done
            }
        } elseif {$status_code == 0x90} {
            # Send the next packet in the request
            return [my Put]
        } else {
            # Any other response is an error
            ## TBD - send an abort?
            set state(state) ERROR
            return failed
        }
    }

    method get {args} {
        # Generates a Obex `GET` request.
        #  args - If a single argument, it must be a dictionary mapping names
        #   of <Obex headers> to their values. Otherwise, it must be
        #   a list of alternating header names and values.
        #
        # It is the caller's responsibility to ensure the value associated
        # with the header is formatted as described in <Obex headers> and
        # that the supplied headers if any are acceptable in `put` request.
        # The following headers are commonly used in put operations:
        # `Name`, `Type`, `Http`, `Timestamp` and `Description`.
        if {[info exists state(connection_id)]} {
            error "Already connected."
        }
        my BeginRequest get
        set state(request_headers) [header encoden {*}$args]
        return [my Get]
    }

    method Get {} {
        # Get opcode+len occupies 3 bytes
        set space [expr {$state(max_packet_len)-3}]

        lassign [my PopHeaders $space] len headers
        incr len 3;         # For opcode fields

        if {[llength $state(request_headers)]} {
            # Still have headers to send
            set op 0x03;        # GET
        } else {
            set op 0x83;        # GET+FINAL
        }
        append packet [binary format cuSu $op $len] {*}$headers
        return [list continue $packet]
    }

    method GetResponse {} {
        set status_code [dict get $state(response) StatusCode]
        if {$status_code == 0xA0} {
            set state(state) IDLE
            return done
        } elseif {$status_code == 0x90} {
            # Send the next packet in the request
            return [my Get]
        } else {
            # Any other response is an error
            ## TBD - send an abort?
            set state(state) ERROR
            return failed
        }
    }

    method ResetRequest {} {
        set state(request) ""
        set state(input) ""
        set state(request_headers) {}
        unset -nocomplain state(request_body)
        set state(request_body_offset) 0
        set state(response_headers) {}
        unset -nocomplain state(response)
    }

    method BeginRequest {request} {
        my AssertState IDLE
        my ResetRequest
        set state(request) $request
        set state(state) BUSY
    }

    method AssertState {required_state} {
        if {$state(state) ne $required_state} {
            error "Method not allowed in state $state(state)."
        }
    }

    method PopHeaders {space} {
        set headers [list ]
        set total_len 0;        # Track total length of headers
        if {[info exists state(connection_header)]} {
            set hdrlen [string length $state(connection_header)]
            if {$space < $hdrlen} {
                # Should not happen
                error "No room for connection id header."
            }
            lappend headers $state(connection_header)
            set space [expr {$space - $hdrlen}]
            incr total_len $hdrlen
        }
        # Add additional headers
        set i 0
        foreach $header $state(request_headers) {
            set hdrlen [string length $header]
            if {$space < $hdrlen} {
                break
            }
            lappend headers $header
            incr i
            set space [expr {$space - $hdrlen}]
            incr total_len $hdrlen
        }

        if {$i == 0} {
            # Could not even fit one header other than connection id which
            # must be sent in every packet if present. This is bad.
            # TBD - how to report headers
            my RaiseError "A packet header exceeds connection packet size limit."
        }

        # Update state to reflect headers still to be sent.
        set state(request_headers) [lrange $state(request_headers) $i end]

        return [list $total_len $headers]
    }

    method AssertOutgoingPacketLength {packet maxlen} {
        if {[string length $packet] > $maxlen} {
            my RaiseError "Outgoing packet length [string length $packet] exceeds max allowed packet length $maxlen."
        }
    }

    method RaiseError {message} {
        set state(state) ERROR
        return -level 1 -code error $message
    }
}

package provide obex 0.1
