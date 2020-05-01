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


proc obex::header::encode {args} {
    if {[llength $args] == 1} {
        set args [lindex $args 0]
    }

    set encoded ""
    foreach {header_name header_value} $args {
        set hi [Id $header_name]
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
    #  target - Target header sent, if any
    #  who    - Who header sent, if any
    # Per request:
    #  input - input buffer for received data
    #  request - current request. Only valid in BUSY or ERROR states.
    #  response - latest response
    #  headers - headers received for latest request
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
        unset -nocomplain state(target)
        unset -nocomplain state(who)
        set state(max_packet_len) 255; # Assume min unless remote tells otherwise

        # Request specific state
        my ResetRequest
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
        my BeginRequest connect
        return [request encode connect {*}$args]
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
        my BeginRequest disconnect
        return [request encode disconnect [my Headers {*}$args]]
    }

    method input {data} {
        # Process data from the remote server.
        #   data - Binary data as received from remote server.
        # The method takes as input data received from the server as part of
        # the response to a request. The return value from the method is one
        # of the following:
        #   `complete`   - The full response has been received. The application
        #                  can then call any of the retrieval methods or initiate
        #                  another request.
        #   `incomplete` - The response has only been partially received. The
        #                  application should read more data from the server and
        #                  invoke the method again.
        #   `failed`     - The request has failed. See <Error handling> for
        #                  dealing with errors and failures.
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
            return incomplete
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
        lappend state(headers) {*}[dict get $response Headers]

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

    method ResetRequest {} {
        set state(request) ""
        unset -nocomplain state(response)
        set state(input) ""
        set state(headers) {}
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

    method Headers {args} {
        # Inserts any automatic headers into the passed headers.
        if {[llength $args] == 1} {
            set headers [lindex $args 0]
        } else {
            set headers $args
        }
        if {[info exists state(connection_id)]} {
            return [linsert $headers 0 ConnectionId $state(connection_id)]
        }
        return $headers
    }


    method ConnectResponse {} {
        dict with state(response) {
            if {$StatusCode != 0xA0} {
                # As per Obex 1.3 sec 3.3.1.8 any other code is failure
                set state(state) ERROR
                return failed
            }
            # Store connection id if it exists
            header find $Headers ConnectionId state(connection_id)
            # On init we assume worst case of 255 byte packets. If
            # remote is willing to go higher, use that.
            if {$MaxLength > 255} {
                set state(max_packet_len) $MaxLength
            }
            # TBD - all other headers - Target, Who etc.
        }
        set state(state) IDLE
        return complete
    }

    method DisconnectResponse {} {
        if {[dict get $state(response) StatusCode] != 0xA0} {
            # As per Obex 1.3 sec 3.3.1.8 any other code is failure
            set state(state) ERROR
            return failed
        } else {
            set state(state) IDLE
            return complete
        }
    }
}

package provide obex 0.1
