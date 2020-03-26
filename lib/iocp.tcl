proc iocp::bt::radios {} {
    # Enumerate Bluetooth radios on the local system.
    #
    # Each radio element of the returned list contains the following keys:
    # Address - The Bluetooth address of the radio.
    # Name - Name assigned to the local system as advertised by the radio.
    # Class - Device class.
    # Subversion - Integer value whose interpretation is manufacturer-specific.
    # Manufacturer - Integer identifier assigned to the manufacturer.
    #
    # Returns a list of radio information elements.

    set pair [FindFirstRadio]
    if {[llength $pair] == 0} {
        return {}
    }
    lassign $pair finder hradio
    set radios {}
    try {
        while {1} {
            lappend radios [GetRadioInfo $hradio] 
            CloseHandle $hradio
            set hradio [FindNextRadio $finder]
        }
    } finally {
        FindFirstRadioClose $finder
    }
    return $radios
}

proc iocp::bt::devices {args} {
    # Enumerate known Bluetooth devices.
    # -authenticated - filter for authenticated devices
    # -remembered    - filter for remembered devices
    # -unknown       - filter for unknown devices
    # -connected     - filter for connected devices
    # -inquire       - issue a new inquiry
    # -timeout MS    - timeout for the inquiry in milliseconds
    # -radio RADIOH  - limit to devices associated with the radio 
    #                  identified by the RADIOH handle
    # Each device information element is returned as a dictionary with
    # the following keys:
    # Name - Human readable name of the device
    # Address - Bluetooth address of the devicec
    # Class - Device class
    # Connected - Boolean value indicating whether the device is connected
    # Remembered - Boolean value indicating whether the device is connected
    # Authenticated - Boolean value indicating whether the device has
    #                 been authenticated
    # LastSeen - Time when device was last seen. The format is a list of
    #            year, month, day, hour, minutes, seconds and milliseconds.
    # LastUsed - Time when device was last used. The format is a list of
    #            year, month, day, hour, minutes, seconds and milliseconds.
    # 
    # The filtering options may be specified to limit the devices
    # returned. If none are specified, all devices are returned.
    #
    # Returns a list of device information dictionaries.

    set pair [FindFirstDevice {*}$args]
    if {[llength $pair] == 0} {
        # No devices found
        return {} 
    }
    lassign $pair finder device
    set devices [list $device]
    try {
        while {1} {
            lappend devices [FindNextDevice $finder]
        }
    } finally {
        FindFirstDeviceClose $finder
    }
    return $devices
}

proc iocp::bt::resolve_device {name args} {
    # Returns a list of Bluetooth addresses for a given name.
    # name - name of device of interest
    # args - Options to control device enquiry. See [devices].

    set addresses [lmap device [devices] {
        if {[string compare -nocase $name [dict get $device Name]]} {
            continue
        }
        dict get $device Address
    }]
    # Also resolve local system radios
    foreach radio [radios] {
        if {[string equal -nocase $name [dict get $radio Name]]} {
            lappend addresses [dict get $radio Address]
        }
    }
    return $addresses
}

proc iocp::bt::device_service_info {device service} {
    # Retrieve information about a service from a device.
    #  device - Bluetooth address or name of a device. If specified as a name,
    #           it must resolve to a single address.
    #  service - the UUID of a service or service class.
    # The return value is a list of service information elements with the
    # following keys:
    #   ServiceInstanceName - the human-friendly name of the service
    #   RemoteAddress - the Bluetooth address of the device
    #   
    # 

}
proc iocp::bt::device_services {device} {
    # Get installed services on a device.
    #  device - Bluetooth address or name of device. If specified as a name,
    #           it must resolve to a single address.
    # Returns a list GUIDS identifying the services.
    if {![IsAddress $device]} {
        set addrs [resolve_device $device]
        if {[llength $addrs] == 1} {
            set device [lindex $addrs 0]
        } elseif {[llength $addrs] == 0} {
            error "Could not resolve device name \"$device\"."
        } else {
            error "Device \"$device\" resolves to multiple addresses."
        }
    }

    return [EnumerateInstalledServices $device]
}

proc iocp::bt::IsAddress {addr} {
    # Returns boolean indicating whether addr is a valid Bluetooth address
    # addr - value to be checked

    # TBD - implement in C?
    return [regexp {^[[:xdigit:]]{2}(:[[:xdigit:]]{2}){5}$} $addr]

}

proc iocp::bt::IsUuid {s} {

    # Check passed parameter for UUID syntax

    # Only checks syntax. Not RFC 4122 validity
    # TBD - implement in C?
    return [regexp {^[[:xdigit:]]{8}-([[:xdigit]]{4}-){3}[[:xdigit:]]{12}$} $s]
}

proc iocp::bt::Extract {bin} {
    # Extracts leading element from an SDP record.
    # bin - tail of SDP record
    # Returns a pair consisting of the extracted element and
    # the remaining part of $bin.

    set binlen [string length $bin]
    if {$binlen == 0} {
        return {}
    }
    binary scan $bin c header
    set type [expr {($header >> 3) & 0xff}]
    set lenbits [expr {$header & 0x7}]

    # The nil type is formatted slightly differently. Treat as a special case.
    if {$type == 0} {
        if {$lenbits != 0} {
            error "Invalid non-0 length for nil type."
        }
        return [list nil]
    }

    if {$lenbits < 5} {
        set len [expr {1 << $lenbits}]
        set dataoffset 1
    } else {
        if {$lenbits == 5} {
            set dataoffset 2
            set fmt c
            set mask 0xff
        } elseif {$lenbits == 6} {
            set dataoffset 3
            set fmt S
            set mask 0xffff
        } else {
            # lenbits == 7
            set dataoffset 5
            set fmt I
            set mask 0xffffffff
        }
        # Ensure input length enough for at least length field
        if {$binlen < $dataoffset} {
            error "Truncated binary header."
        }
        binary scan $bin @1$fmt len
        set len [expr {$len & $mask}]; # Since binary scan is signed
    }
    if {$binlen < ($dataoffset+$len)} {
        error "Truncated binary data."
    }
    set elem [string range $bin $dataoffset [expr {$dataoffset+$len-1}]]
    switch -exact $type {
        1 {
            set type uint
            if {$len == 1} {
                binary scan $elem cu elem
            } elseif {$len == 2} {
                binary scan $elem Su elem
            } elseif {$len == 4} {
                binary scan $elem Iu elem
            } elseif {$len == 8} {
                binary scan $elem Wu elem
            } elseif {$len == 16} {
                binary scan $elem WuWu hi lo
                # TBD - check this
                # (2**64)*hi + lo
                set elem [expr {(18446744073709551616 * $hi) + $lo}]
            } else {
                error "Invalid integer width."
            }
        }
        2 {
            set type int
            if {$len == 1} {
                binary scan $elem c elem
            } elseif {$len == 2} {
                binary scan $elem S elem
            } elseif {$len == 4} {
                binary scan $elem I elem
            } elseif {$len == 8} {
                binary scan $elem W elem
            } elseif {$len == 16} {
                # Note hi is signed, lo unsigned
                binary scan $elem WWu hi lo
                # TBD - check this
                # (2**64)*hi + lo
                set elem [expr {(18446744073709551616 * $hi) + $lo}]
            } else {
                error "Invalid integer width."
            }
        }
        3 {
            set type uuid
            if {$len == 2} {
                set elem [binary encode hex $elem]
                set elem "0000$elem-0000-1000-8000-00805f9b34fb"
            } elseif {$len == 4} {
                set elem [binary encode hex $elem]
                set elem "$elem-0000-1000-8000-00805f9b34fb"
            } elseif {$len == 16} {
                set hex [binary encode hex $elem]
                set elem [string range $hex 0 7]
                append elem - [string range $hex 8 11]
                append elem - [string range $hex 12 15]
                append elem - [string range $hex 16 19]
                append elem - [string range 20 end]
            } else {
                error "Invalid length $len for UUID."
            }
        }
        4 {
            set type text
            # TBD - keep elem as binary as we do not know encoding
        }
        5 {
            set type boolean
            if {$len != 1} {
                error "Invalid length for boolean type."
            }
            binary scan $elem c elem
            set elem [expr {!!$elem}] ; # Convert to 0/1
        }
        6 {
            set type sequence
            # elem remains the same. Caller will have to recurse to parse
        }
        7 {
            set type choice
            # elem remains the same. Caller will have to recurse to parse
        }
        8 {
            set type url
            # elem remains same. Encoding unknown. TBD
        }
        default {
            set type unknown
            # elem remains same
        }
    }

    return [list $type $elem [string range $bin $dataoffset+$len end]]
}

proc iocp::bt::BinToHex {bin {start 0} {end end}} {
    regexp -inline -all .. [binary encode hex [string range $bin $start $end]]
}