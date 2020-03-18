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


