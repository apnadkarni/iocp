namespace eval iocp::bt::names {
    namespace path [namespace parent]
    # Maps 16bit hex UUID to Service class name
    # https://www.bluetooth.com/specifications/assigned-numbers/service-discovery/
   variable service_class_names {
        1000 ServiceDiscoveryServerServiceClassID
        1001 BrowseGroupDescriptorServiceClassID
        1002 PublicBrowseRoot
        1101 SerialPort
        1102 LANAccessUsingPPP
        1103 DialupNetworking
        1104 IrMCSync
        1105 OBEXObjectPush
        1106 OBEXFileTransfer
        1107 IrMCSyncCommand
        1108 Headset
        1109 CordlessTelephony
        110A AudioSource
        110B AudioSink
        110C A/V_RemoteControlTarget
        110D AdvancedAudioDistribution
        110E A/V_RemoteControl
        110F A/V_RemoteControlController
        1110 Intercom
        1111 Fax
        1112 {Headset - Audio Gateway (AG)}
        1115 PANU
        1116 NAP
        1117 GN
        1118 DirectPrinting
        1119 ReferencePrinting
        111A {Basic Imaging Profile}
        111B ImagingResponder
        111C ImagingAutomaticArchive
        111D ImagingReferencedObjects
        111E Handsfree
        111F HandsfreeAudioGateway
        1120 DirectPrintingReferenceObjectsService
        1121 ReflectedUI
        1122 BasicPrinting
        1123 PrintingStatus
        1124 HumanInterfaceDeviceService
        1125 HardcopyCableReplacement
        1126 HCR_Print
        1127 HCR_Scan
        1128 Common_ISDN_Access
        112D SIM_Access
        112E {Phonebook Access - PCE}
        112F {Phonebook Access - PSE}
        1130 {Phonebook Access}
        1131 {Headset - HS}
        1132 {Message Access Server}
        1133 {Message Notification Server}
        1134 {Message Access Profile}
        1135 GNSS
        1136 GNSS_Server
        1137 {3D Display}
        1138 {3D Glasses}
        1139 {3D Synchronization}
        113A {MPS Profile UUID}
        113B {MPS SC UUID}
        113C {CTN Access Service}
        113D {CTN Notification Service}
        113E {CTN Profile}
        1200 PnPInformation
        1201 GenericNetworking
        1202 GenericFileTransfer
        1203 GenericAudio
        1204 GenericTelephony
        1303 VideoSource
        1304 VideoSink
        1305 VideoDistribution
        1400 HDP
        1401 {HDP Source}
        1402 {HDP Sink}
    }

    # https://www.bluetooth.com/specifications/assigned-numbers/service-discovery/
    variable protocol_names { 
        0001 SDP
        0002 UDP
        0003 RFCOMM
        0004 TCP
        0005 TCS-BIN
        0006 TCS-AT
        0007 ATT
        0008 OBEX
        0009 IP
        000A FTP
        000C HTTP
        000E WSP
        000F BNEP
        0010 UPNP
        0011 HIDP
        0012 HardcopyControlChannel
        0014 HardcopyDataChannel
        0016 HardcopyNotification
        0017 AVCTP
        0019 AVDTP
        001B CMTP
        001E MCAPControlChannel
        001F MCAPDataChannel
        0100 L2CAP
    }

    # Universal attribute names
    variable attribute_names
    set attribute_names { 
        ServiceRecordHandle     0
        ServiceClassIDList      1
        ServiceRecordState      2
        ServiceID               3
        ProtocolDescriptorList  4
        BrowseGroupList         5 
        LanguageBaseAttributeIDList  6
        ServiceInfoTimeToLive   7
        ServiceAvailability     8
        BluetoothProfileDescriptorList 9
        DocumentationURL       10
        ClientExecutableURL    11
        IconURL                12 
        AdditionalProtocolDescriptorLists 13
        ServiceName           256
        ServiceDescription    257
        ProviderName          258
    }

}

proc iocp::bt::names::name {uuid} {
    # Map a UUID to a name.
    #  uuid - Bluetooth UUID
    # The command attempts to map the UUID as a service name or protocol
    # name in that order.
    # 
    # Returns the mapped name or the uuid if no mapping is possible.

    set name [service_class_name $uuid]
    if {$name ne $uuid} {
        return $name
    }
    return [protocol_name $uuid]
}

proc iocp::bt::names::service_class_name {uuid} {
    variable service_class_names
    set name [MapUuidToName $uuid service_class_names]
    if {$name ne $uuid} {
        return $name
    }
    # Well known private uuids
    if {[string equal -nocase "02030302-1d19-415f-86f2-22a2106a0a77" $uuid]} {
        return "Wireless iAP v2"; # iPod Accessory Protocol
    }
}

proc iocp::bt::names::service_class_uuid {name} {
    variable service_class_names
    return [MapNameToUuid $name service_class_names]
}

proc iocp::bt::names::profile_name {uuid} {
    return [service_class_name $uuid]
}

proc iocp::bt::names::protocol_name {uuid} {
    variable protocol_names
    return [MapUuidToName $uuid protocol_names]
}

proc iocp::bt::names::protocol_uuid {name} {
    variable protocol_names
    return [MapNameToUuid $name protocol_names]
}

proc iocp::bt::names::attribute_name {attr_id} {
    variable attribute_names
    dict for {name id} $attribute_names {
        if {$attr_id == $id} {
            return $name
        }
    }
    return $attr_id
}

proc iocp::bt::names::attribute_id {name} {
    variable attribute_names
    return [dict get $attribute_names $name]
}

proc iocp::bt::names::MapUuidToName {uuid dictvar} {
    upvar 1 $dictvar names

    if {![IsUuid $uuid]} {
        error "Invalid Bluetooth UUID \"$uuid\""
    }
    if {[string range $uuid 0 3] ne "0000"} {
        # Top bits must be 0's
        return $uuid
    }
    set uuid16 [string toupper [string range $uuid 4 7]]
    if {[dict exists $names $uuid16]} {
        return [dict get $names $uuid16]
    }
    return $uuid
}

proc iocp::bt::names::MapNameToUuid {name dictvar} {
    upvar 1 $dictvar names

    if {[IsUuid $name]} {
        return $name
    }
    dict for {uuid16 mapped_name} $names {
        if {[string equal -nocase $name $mapped_name]} {
            return "0000$uuid16-0000-1000-8000-00805f9b34fb"
        }
    }
    error "Name \"$name\" could not be mapped to a UUID"
}

proc iocp::bt::names::Asciify {svar} {
    # Replaces non-ascii or non-printable chars with Tcl sequences
    #
    # Transformed string is stored back in svar.

    upvar 1 $svar s
    set ascii ""
    set was_escaped 0
    # Split can be expensive for large strings so string index
    set len [string length $s]
    for {set i 0} {$i < $len} {incr i} {
        set char [string index $s $i]
        scan $char %c codepoint
        if {$codepoint < 32 || $codepoint >= 126} {
            append ascii "\\u[format %.4x $codepoint]"
            set was_escaped 1
        } else {
            append ascii $char
        }
    }
    set s $ascii
    return $was_escaped
}

proc iocp::bt::names::ClipSpec {{use_list 0}} {
    package require twapi

    set csv [twapi::read_clipboard_text]
    if {$use_list} {
        set tcl "\[list \n"
    } else {
        set tcl "\{ \n"
    }
    foreach line [split $csv \n] {
        lassign [split $line \t] name uuid description
        set name [string trim $name]
        set uuid [string trim $uuid]
        if {![regexp {^0x[[:xdigit:]]{4}} $uuid]} continue
        if {$use_list} {
            # Unicode chars, needs escaping
            Asciify name
            append tcl "        " [list [string range $uuid 2 end]] " "  "\"$name\"" " \\" \n
        } else {
            if {[Asciify name]} {
                # Need to use the list command. Just restart
                return [ScrapeBTSpec 1]
            }
            append tcl "        " [string range $uuid 2 end] " "  "{$name}" \n
        }
    }
    if {$use_list} {
        append tcl "\]\n"
    } else {
        append tcl "\}"
    }

    twapi::write_clipboard_text $tcl
}

