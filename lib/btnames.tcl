namespace eval iocp::btnames {
   variable uuid16_service_class_map {
        1000 ServiceDiscoveryServerServiceClassID
        1001 BrowseGroupDescriptorServiceClassID
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
}
proc iocp::btnames::uuid_to_service_class {uuid} {
    variable uuid16_service_class_map
    if {![regexp {^[[:xdigit:]]{8}-0000-1000-8000-00805f9b34fb$} $uuid]} {
        error "Invalid Bluetooth UUID \"$uuid\""
    }
    set uuid16 [string toupper [string range $uuid 4 7]]
    if {[dict exists $uuid16_service_class_map $uuid16]} {
        return [dict get $uuid16_service_class_map $uuid16]
    }
    return $uuid
}
