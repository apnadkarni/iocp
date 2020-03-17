proc iocp::bt::devices {args} {
    # Returns a list of known devices.
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

proc iocp::bt::resolve_device_name {name args} {
    # Returns a list of Bluetooth addresses for a given name.
    # name - name of device of interest
    # args - Options to control device enquiry. See [devices].

    return [lmap device [devices] {
        if {[string compare -nocase $name [dict get $device Name]]} {
            continue
        }
        dict get $device Address
    }]
}

