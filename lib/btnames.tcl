#
# Copyright (c) 2020, Ashok P. Nadkarni
# All rights reserved.
#
# See the file LICENSE for license

namespace eval iocp::bt::names {
    namespace path [namespace parent]
    # Maps UUID to Service class name
    # https://www.bluetooth.com/specifications/assigned-numbers/service-discovery/
    # UUID may be either 16-bit Bluetooth universal UUID as or the full UUID
    # Uses lower case hex.
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
       110a AudioSource
       110b AudioSink
       110c A/V_RemoteControlTarget
       110d AdvancedAudioDistribution
       110e A/V_RemoteControl
       110f A/V_RemoteControlController
       1110 Intercom
       1111 Fax
       1112 {Headset - Audio Gateway (AG)}
       1115 PANU
       1116 NAP
       1117 GN
       1118 DirectPrinting
       1119 ReferencePrinting
       111a {Basic Imaging Profile}
       111b ImagingResponder
       111c ImagingAutomaticArchive
       111d ImagingReferencedObjects
       111e Handsfree
       111f HandsfreeAudioGateway
       1120 DirectPrintingReferenceObjectsService
       1121 ReflectedUI
       1122 BasicPrinting
       1123 PrintingStatus
       1124 HumanInterfaceDeviceService
       1125 HardcopyCableReplacement
       1126 HCR_Print
       1127 HCR_Scan
       1128 Common_ISDN_Access
       112d SIM_Access
       112e {Phonebook Access - PCE}
       112f {Phonebook Access - PSE}
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
       113a {MPS Profile UUID}
       113b {MPS SC UUID}
       113c {CTN Access Service}
       113d {CTN Notification Service}
       113e {CTN Profile}
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
       02030302-1d19-415f-86f2-22a2106a0a77 {Wireless iAP v2}
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
        000a FTP
        000c HTTP
        000e WSP
        000f BNEP
        0010 UPNP
        0011 HIDP
        0012 HardcopyControlChannel
        0014 HardcopyDataChannel
        0016 HardcopyNotification
        0017 AVCTP
        0019 AVDTP
        001b CMTP
        001e MCAPControlChannel
        001f MCAPDataChannel
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
        AdditionalProtocolDescriptorList 13
        ServiceName           256
        ServiceDescription    257
        ProviderName          258
    }

}

proc iocp::bt::names::is_uuid {uuid} {
    regexp {^[[:xdigit:]]{8}-([[:xdigit:]]{4}-){3}[[:xdigit:]]{12}$} $uuid
}

proc iocp::bt::names::to_name {uuid} {
    # Map a UUID to a name.
    #  uuid - Bluetooth UUID
    # The command attempts to map the UUID as a service name or protocol
    # name in that order.
    # 
    # Returns the mapped name or the uuid if no mapping is possible.

    set name [service_class_name $uuid]
    if {$name ne $uuid && ![is_uuid $name]} {
        return $name
    }
    return [protocol_name $uuid]
}

proc iocp::bt::names::to_uuid {name_or_uuid} {
    # Map a name to a UUID.
    #  name_or_uuid - Bluetooth UUID or name
    # The command attempts to map the name as a service class name or protocol
    # name in that order.
    #
    # Returns the mapped uuid raises an error if no mapping is possible.
    variable service_class_names
    if {[MapNameToUuidV $name_or_uuid service_class_names name]} {
        return $name
    }
    return [protocol_uuid $name_or_uuid]
}


proc iocp::bt::names::service_class_name {uuid} {
    # Maps a UUID to a service class name.
    # uuid - UUID to be mapped
    # Returns a human-readable name or the UUID itself
    # if no mapping could be performed.
    variable service_class_names
    set name [MapUuidToName $uuid service_class_names]
    if {$name ne $uuid} {
        return $name
    }
    # Well known private uuids
    if {[string equal -nocase "02030302-1d19-415f-86f2-22a2106a0a77" $uuid]} {
        return "Wireless iAP v2"; # iPod Accessory Protocol
    }
    return $uuid
}

proc iocp::bt::names::service_class_uuid {name} {
    # Maps a service class name to a UUID.
    #  name - Service class name to be mapped.
    # The command raises an error if the name could not be mapped to
    # a UUID.
    #
    # Returns the UUID corresponding to the name.
    variable service_class_names
    return [MapNameToUuid $name service_class_names]
}

proc iocp::bt::names::profile_name {uuid} {
    # Maps a UUID to a Bluetooth profile name.
    # uuid - UUID to be mapped
    # Returns a human-readable name or the UUID itself
    # if no mapping could be performed.

    # Profiles seem to come from same service class UUID space.
    return [service_class_name $uuid]
}

proc iocp::bt::names::protocol_name {uuid} {
    # Maps a UUID to a protocol name.
    # uuid - UUID to be mapped
    # Returns a human-readable name or the UUID itself
    # if no mapping could be performed.
    variable protocol_names
    return [MapUuidToName $uuid protocol_names]
}

proc iocp::bt::names::protocol_uuid {name} {
    # Maps a protocol name to a UUID.
    #  name - Protocol name to be mapped.
    # The command raises an error if the name could not be mapped to
    # a UUID.
    #
    # Returns the UUID corresponding to the name.
    variable protocol_names
    return [MapNameToUuid $name protocol_names]
}

proc iocp::bt::names::attribute_name {attr_id} {
    # Maps a attribute id (numeric or name) to a attribute name.
    #  attr_id - numeric attribute identifier
    # Returns a human-readable name for the attribute or
    # the numeric id itself if no mapping could be performed.
    variable attribute_names
    dict for {name id} $attribute_names {
        if {$attr_id == $id || [string equal -nocase $name $attr_id]} {
            return $name
        }
    }
    if {[string is integer -strict $attr_id]} {
        return $attr_id
    }
    error "Unknown attribute \"$attr_id\"."
}

proc iocp::bt::names::attribute_id {name} {
    # Maps a attribute name to it numeric attribute identifier.
    #  name - Attribute name to be mapped
    # The command will raise an error if the name is unknown.
    # Returns the numeric attribute identifier corresponding to
    # the passed name.
    variable attribute_names
    if {[string is integer -strict $name]} {
        return $name
    }
    return [dict get $attribute_names $name]
}

proc iocp::bt::names::print {} {
    # Prints known UUID's and their mapped mnemonics.
    variable service_class_names
    dict for {uuid16 name} $service_class_names {
        puts "[Uuid16 $uuid16] $name"
    }
}

proc iocp::bt::names::MapUuidToName {uuid dictvar} {
    upvar 1 $dictvar names

    # uuid may be a full UUID or a 16-bit Bluetooth UUID
    if {[IsUuid $uuid]} {
        # See if the exact match exists
        set uuid [string tolower $uuid]
        if {[dict exists $names $uuid]} {
            return [dict get $names $uuid]
        }
        # Nope. Now see if it is a 16-bit Bluetooth UUID
        if {[IsBluetoothUuid $uuid] && [string range $uuid 0 3] eq "0000"} {
            # 16-bit UUID
            set uuid16 [string tolower [string range $uuid 4 7]]
            if {[dict exists $names $uuid16]} {
                return [dict get $names $uuid16]
            }
        }
        # Nope. Cannot be mapped. Return the UUID.
        return $uuid
    }

    # Not a full UUID. Retry as a 16-but UUID. tailcall so upvar works right.
    tailcall MapUuidToName [Uuid16 $uuid] $dictvar
}

proc iocp::bt::names::MapNameToUuidV {name dictvar resultvar} {
    upvar 1 $dictvar   names
    upvar 1 $resultvar result

    if {[IsUuid $name]} {
        set result $name
        return 1
    }
    dict for {uuid mapped_name} $names {
        if {[string equal -nocase $name $mapped_name]} {
            if {[string length $uuid] == 4} {
                # 16-bit UUID -> full UUID
                set result "0000${uuid}-0000-1000-8000-00805f9b34fb"
            } else {
                set result $uuid
            }
            return 1
        }
    }
    return 0
}

proc iocp::bt::names::MapNameToUuid {name dictvar} {
    upvar 1 $dictvar names
    if {[MapNameToUuidV $name names result]} {
        return $result
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

