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
    # Class - Device class as a numeric value.
    # DeviceClasses - Human readable list of general device class categories
    # Subversion - Integer value whose interpretation is manufacturer-specific.
    # Manufacturer - Integer identifier assigned to the manufacturer.
    # MajorClassName - Human readable major device class name.
    # MinorClassName - Human readable minor device class name.
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
            set radio [GetRadioInfo $hradio]
            lappend radios [dict merge $radio [DeviceClass [dict get $radio Class]]]
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
    # -connected     - filter for connected devices
    # -inquire       - issue a new inquiry. Without this option, devices that are
    #                  not already known to the system will not be discovered.
    # -radio RADIOH  - filter to devices associated with the radio
    #                  identified by the RADIOH handle
    # -remembered    - filter for remembered devices
    # -timeout MS    - timeout for the inquiry in milliseconds. Defaults to 10240ms.
    #                  Ignored if `-inquire` is not specified.
    # -unknown       - filter for unknown devices
    # Each device information element is returned as a dictionary with
    # the following keys:
    # Authenticated - Boolean value indicating whether the device has
    #                 been authenticated
    # Address - Bluetooth address of the devicec
    # Class - Device class as a numeric value
    # Connected - Boolean value indicating whether the device is connected
    # DeviceClasses - Human readable list of general device class categories
    # LastSeen - Time when device was last seen. The format is a list of
    #            year, month, day, hour, minutes, seconds and milliseconds.
    # LastUsed - Time when device was last used. The format is a list of
    #            year, month, day, hour, minutes, seconds and milliseconds.
    # MajorClassName - Human readable major device class name.
    # MinorClassName - Human readable minor device class name.
    # Name - Human readable name of the device
    # Remembered - Boolean value indicating whether the device is connected
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
    set device [dict merge $device [DeviceClass [dict get $device Class]]]
    set devices [list $device]
    try {
        while {1} {
            set device [FindNextDevice $finder]
            set device [dict merge $device [DeviceClass [dict get $device Class]]]
            lappend devices $device
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

proc iocp::bt::resolve_port {device service} {
    # Resolve the port for a Bluetooth service running over RFCOMM.
    #  device - Bluetooth address or name of a device. If specified as a name,
    #           it must resolve to a single address.
    #  service - UUID or name of service of interest.
    # Returns the port number for the service or raises an error if it
    # cannot be resolved.

    set h [LookupServiceBegin \
               [ResolveDeviceUnique $device] \
               [names::service_class_uuid $service]]
    try {
        while {1} {
            # 0x100 -> LUP_RETURN_ADDR
            set rec [LookupServiceNext $h 0x100]
            if {[dict exists $rec RemoteAddress] &&
                [dict get $rec RemoteAddress AddressFamily] == 32} {
                # 32 -> AF_BTH (Bluetooth)
                # Further we are looking for RFCOMM (protocol 3)
                if {[dict exists $rec Protocol] &&
                    [dict get $rec Protocol] == 3} {
                    return [dict get $rec RemoteAddress Port]
                }
            }
        }
    } finally {
        LookupServiceEnd $h
    }
    error "Could not resolve service \"$service\" to a port on device \"$device\"."
}

proc iocp::bt::remove_device {device} {
    # Removes cached authentication information for a device from the system cache.
    #  device - bluetooth address or name of a device. if specified as a name,
    #           it must resolve to a single address.

    RemoveDevice [ResolveDeviceUnique $device]
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

    set h [LookupServiceBegin \
               [ResolveDeviceUnique $device] \
               [names::service_class_uuid $service]]
    set recs {}
    try {
        while {1} {
            # 0x0200 -> LUP_RETURN_BLOB. Returns {Blob BINDATA}
            lappend recs [lindex [LookupServiceNext $h 0x200] 1]
        }
    } finally {
        LookupServiceEnd $h
    }
    return $recs
}

proc iocp::bt::browse_services {device} {
    # Retrieve the service discovery records for top level services.
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

    # TBD - add a browse group parameter
    # TBD - perhaps check that the sdr acually refernces browse group in
    # the appropriate attribute
    return [get_service_references $device 00001002-0000-1000-8000-00805f9b34fb]
}

# TBD - is this needed? Less functional version of browse_services
proc iocp::bt::device_services {device} {
    # Get installed services on a device.
    #  device - Bluetooth address or name of device. If specified as a name,
    #           it must resolve to a single address.
    # Returns a list of service UUID's.
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

proc DeviceClass {class} {
    if {[expr {$class & 3}] != 0} {
        # Not format type that we understand
        return {}
    }

    # https://www.bluetooth.com/specifications/assigned-numbers/baseband/

    set minor_class [expr {($class >> 2) & 0x3f}]
    set major_class [expr {($class >> 8) & 0x1f}]

    if {$major_class == 0} {
        set major_name "Miscellaneous"
        set minor_name "Miscellaneous"
    } elseif {$major_class == 1} {
        set major_name Computer
        set minor_name [lindex {
            Uncategorized Desktop Server Laptop "Handheld PC/PDA" "Palm-size PC/PDA" Wearable Tablet
        } $minor_class]
    } elseif {$major_class == 2} {
        set major_name Phone
        set minor_name [lindex {
            Uncategorized Cellular Cordless Smartphone "Wired modem"  ISDN
        } $minor_class]
    } elseif {$major_class == 3} {
        set major_name "LAN/Network Access point"
        set minor_name [lindex {
            0% 1-17% 17-33% 33-50% 50-67% 67-83% 83-99% 100%
        } $minor_class]
    } elseif {$major_class == 4} {
        set major_name "Audio/Video"
        set minor_name [lindex {
            Uncategorized "Wearable Headset Device" "Hands-free Device" Reserved
            Microphone Loudspeaker Headphones "Portable Audio" "Car Audio"
            "Set-top box" "HiFi Audio Device" VCR Camcorder "Video Monitor"
            "Video Display and Loudspeaker" "Video Conferencing" Reserved
            "Gaming/Toy"
        } $minor_class]
    } elseif {$major_class == 5} {
        set major_name Peripheral
        set bits [expr {$minor_class >> 4}]
        set minor_name [lindex {
            Other Keyboard "Pointing device" "Combo keyboard/pointing device"
        } $bits]
        set bits [expr {$minor_class & 0xf}]
        if {$bits < 10} {
            append minor_name /[lindex {
                Uncategorized Joystick Gamepad "Remote control" "Sensing device"
                "Digitizer tabler" "Card reader" "Digital pen" "Handheld scanner"
                "Handheld gesture input device"
            }]
        }
    } elseif {$major_class == 6} {
        set major_name Imaging
        set minor_name {}
        if {$minor_class & 4} {lappend minor_name Display}
        if {$minor_class & 8} {lappend minor_name Camera}
        if {$minor_class & 16} {lappend minor_name Scanner}
        if {$minor_class & 32} {lappend minor_name Printer}
        set minor_name [join $minor_name /]
    } elseif {$major_class == 7} {
        set major_name Wearable
        set minor_name [lindex {
            Wristwatch Pager Jacket Helmet Glasses
        } $minor_class]
    } elseif {$major_class == 8} {
        set major_name Toy
        set minor_name [lindex {
            Robot Vehicle Doll Controller Game
        } $minor_class]
    } elseif {$major_class == 9} {
        set major_name Health
        set minor_name [lindex {
            Undefined "Blood Pressure Monitor" Thermometer "Weighing Scale"
            "Glucose Meter" "Pulse Oximeter" "Heart/Pulse Rate Monitor"
            "Health Data Display" "Step Counter" "Body Composition Analyzer"
            "Peak Flow Monitor" "Medication Monitor" "Knee Prosthesis"
            "Andle Prosthesis" "Generic Health Manager" "Personal Mobility Device"
        } $minor_class]
    } elseif {$major_class == 31} {
        set major_name Uncategorized
        set minor_name ""
    } else {
        set major_name Reserved
        set minor_name ""
    }

    set service_class {}
    if {$class & (1<<13)} {lappend service_class "Limited Discoverable Mode"}
    if {$class & (1<<16)} {lappend service_class "Positioning (Location identification)"}
    if {$class & (1<<17)} {lappend service_class "Networking"}
    if {$class & (1<<18)} {lappend service_class "Rendering"}
    if {$class & (1<<19)} {lappend service_class "Capturing"}
    if {$class & (1<<20)} {lappend service_class "Object Transfer"}
    if {$class & (1<<21)} {lappend service_class "Audio"}
    if {$class & (1<<22)} {lappend service_class "Telephony"}
    if {$class & (1<<23)} {lappend service_class "Information"}

    return [dict create MajorClassName $major_name \
                MinorClassName $minor_name \
                DeviceClasses $service_class]
}

proc iocp::bt::ResolveDeviceUnique {device} {
    if {[IsAddress $device]} {
        return $device
    }
    set addrs [resolve_device $device]
    if {[llength $addrs] == 0} {
        error "Could not resolve Bluetooth device name \"$device.\""
    } elseif {[llength $addrs] > 1} {
        error "Device \"$device\" resolves to multiple addresses."
    }
    return [lindex $addrs 0]
}

package provide iocp_bt $iocp::bt::version
