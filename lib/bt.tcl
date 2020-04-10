#
# Copyright (c) 2020, Ashok P. Nadkarni
# All rights reserved.
#
# See the file LICENSE for license

namespace eval iocp::bt {
    variable script_dir [file dirname [info script]]
    variable version
}

set iocp::bt::version [package require iocp]
source [file join $iocp::bt::script_dir btsdr.tcl]
source [file join $iocp::bt::script_dir btnames.tcl]

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

proc iocp::bt::get_service_references {device service} {
    # Retrieve service discovery records that refer to a specified service.
    #  device - Bluetooth address or name of a device. If specified as a name,
    #           it must resolve to a single address.
    #  service - the UUID of a service or service class or its mnemonic.
    # The command will return all service discovery records that contain
    # an attribute referring to the specified service.
    # The returned service discovery records should be treated as 
    # opaque and accessed through the service record decoding commands.
    #
    # Returns a list of service discovery records.
    set h [LookupServiceBegin $device [names::service_class_uuid $service]]
    set recs {}
    try {
        while {1} {
            lappend recs [LookupServiceNext $h]
        }
    } finally {
        LookupServiceEnd $h
    }
    return $recs
}

proc iocp::bt::browse_services {device} {
    # Retrieve the service discovery records for top level services
    # advertised by a device.
    #  device - Bluetooth address or name of a device. If specified as a name,
    #           it must resolve to a single address.
    # 
    # The command will return all service discovery records that reference
    # the `PublicBrowseRoot` service class.
    #
    # The returned service discovery records should be treated as 
    # opaque and accessed through the service record decoding commands.
    #
    # Returns a list of service dscovery records.
    return [get_service_references $device 00001002-0000-1000-8000-00805f9b34fb]
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
        error "Invalid SDP record."
    # addr - value to be checked

    # TBD - implement in C?
    return [regexp {^[[:xdigit:]]{2}(:[[:xdigit:]]{2}){5}$} $addr]

}

proc iocp::bt::uuid16 {uuid16} {
    if {![regexp {^[[:xdigit:]]{4}$} $uuid16]} {
        error "Not a valid 16 bit UUID"
    }
    return 0000${uuid16}-0000-1000-8000-00805f9b34fb
}

proc iocp::bt::IsUuid {uuid} {

    # Check passed parameter for UUID syntax

    # Only checks syntax. Not RFC 4122 validity
    # TBD - implement in C?
    return [regexp {^[[:xdigit:]]{8}-([[:xdigit:]]{4}-){3}[[:xdigit:]]{12}$} $uuid]
}

proc iocp::bt::IsBluetoothUuid {uuid} {

    # Check passed parameter for UUID syntax

    # TBD - implement in C?
    return [regexp {^[[:xdigit:]]{8}-0000-1000-8000-00805f9b34fb$} $uuid]
}


proc iocp::bt::BinToHex {bin {start 0} {end end}} {
    regexp -inline -all .. [binary encode hex [string range $bin $start $end]]
}

proc iocp::bt::encoding_name {mibenum} {
    set encoding_map {
        3 ascii
        2026 big5
        437 cp437
        2087 cp775
        2009 cp850
        2010 cp852
        2046 cp855
        2047 cp857
        2048 cp860
        2049 cp861
        2013 cp862
        2050 cp863
        2051 cp864
        2052 cp865
        2086 cp866
        2054 cp869
        2109 cp874
        113 cp936
        2250 cp1250
        2251 cp1251
        2252 cp1252
        2253 cp1253
        2254 cp1254
        2255 cp1255
        2256 cp1256
        2257 cp1257
        2258 cp1258
        2088 ebcdic
        18 euc-jp
        38 euc-kr
        56 gb1988
        2025 gb2312
        57 gb2312-raw
        39 iso2022-jp
        37 iso2022-kr
        4 iso8859-1
        5 iso8859-2
        6 iso8859-3
        7 iso8859-4
        8 iso8859-5
        9 iso8859-6
        10 iso8859-7
        11 iso8859-8
        12 iso8859-9
        13 iso8859-10
        109 iso8859-13
        110 iso8859-14
        111 iso8859-15
        112 iso8859-16
        15 jis0201
        63 jis0208
        98 jis0212
        2084 koi8-r
        2088 koi8-u
        36 ksc5601
        17 shiftjis
        2259 tis-620
        106 utf-8
    }
    incr mibenum 0; # Convert to canonical decimal string
    if {[dict exists $encoding_map $mibenum]} {
        return [dict get $encoding_map $mibenum]
    }
    return $mibenum
}

proc iocp::AddressFamilyName {af} {
    if {$af == 2} {
        return IPv4
    } elseif {$af == 23} {
        return IPv6
    } elseif {$af == 32} {
        return Bluetooth
    } elseif {$af == 1} {
        return Unix
    } elseif {$af == 6} {
        return IPX
    } elseif {$af == 17} {
        return Netbios
    } else {
        return AF$af
    }
}

package provide iocp_bt $iocp::bt::version
