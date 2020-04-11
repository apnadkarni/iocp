#
# Copyright (c) 2020, Ashok P. Nadkarni
# All rights reserved.
#
# See the file LICENSE for license

namespace eval iocp::bt::sdr {
    namespace path [namespace parent]
}

proc iocp::bt::sdr::parse {binsdr} {
    # Parses a binary service discovery record
    # binsdr - a raw Bluetooth service discovery record in binary
    #       form.
    #
    # The returned value should be treated as opaque and accessed
    # through the `sdr` commands.
    # 
    # Returns a container of attributes stored in the record.

    # A SDR record is a single data element which is a sequence
    # containing nested data elements.
    # {{sequence {nested data elements}}}
    set rec [lindex [ParseElements $binsdr] 0 1]

    # $rec contains alternating attribute value pairs. The result
    # is built as a list as its faster to do that and shimmer
    # to a dict on access than to build a dictionary to begin with.
    set sdr {}
    foreach {attr val} $rec {
        lappend sdr [lindex $attr 1] $val
    }
    return $sdr
}

proc iocp::bt::sdr::attribute_type {sdr} {
    # Returns the type of an attribute in a service discovery record or
    # the empty string if the attribute does not exist.
    if {[string is integer -strict $attr_id]} {
        set key [expr {$attr_id + 0}]; # Force decimal rep. Faster than format
    } else {
        set key [names::attribute_id $attr_id]; # name -> id
    }

    if {[dict exists $sdr $key]} {
        if {$varname ne ""} {
            upvar 1 $varname value
            set value [dict get $sdr $attr_id]
        }
        return 1
    }

    return 0

}

proc iocp::bt::sdr::has_attribute {sdr attr_id {varname {}}} {
    # Checks if an attribute exists in a service discovery record
    # sdr - a parsed service discovery record in the form returned by
    #       sdr_parse.
    # attr_id - attribute integer id or Bluetooth universal attribute name
    # varname - optional. If not the empty string, the raw attribute value is
    #           stored in a variable of this name in the caller's context.
    # 
    # Returns 1 if the attribute exists, and 0 otherwise.

    if {[string is integer -strict $attr_id]} {
        set key [expr {$attr_id + 0}]; # Force decimal rep. Faster than format
    } else {
        puts ID:$attr_id
        set key [names::attribute_id $attr_id]; # name -> id
    }

    if {[dict exists $sdr $key]} {
        if {$varname ne ""} {
            upvar 1 $varname value
            set value [dict get $sdr $attr_id]
        }
        return 1
    }

    return 0
}

proc iocp::bt::sdr::attributes {sdr} {
    # Get the list of attributes in a service discovery record
    # sdr - a parsed service discovery record in the form returned by
    #       sdr_parse.
    # Returns a list of numeric attribute ids.
    return [lmap {attr val} $sdr {set attr}]
}

proc iocp::bt::sdr::attribute {sdr attr_id} {
    # Get an attribute value from an service discovery record.
    # sdr - a parsed service discovery record in the form returned by
    #       sdr_parse.
    # attr_id   - attribute integer id
    # The command will raise an error if the attribute does not exist
    # in the sdr and no default is specified by the caller.
    # 
    # Returns the attribute value as a pair consisting of the type and
    # the raw data element value.

    if {[has_attribute $sdr $attr_id value]} {
        return $value
    } else {
        error "Attribute with id \"$attr_id\" not found."
    }
}

proc iocp::bt::sdr::handle {sdr {varname {}}} {
    # Retrieve the handle for a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The service record handle corresponds to the ServiceRecordHandle
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the service record handle.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 0 $varname
}

proc iocp::bt::sdr::service_classes {sdr {varname {}}} {
    # Retrieve the service classes in a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The retrieved value is a list of service class descriptors
    # corresponding to the ServiceClassIDList
    # universal attribute in the Bluetooth Core specification.
    # Each descriptor is a dictionary with keys `Uuid` and `Name` containing
    # the UUID of the service class and its mnemonic. If the latter
    # is not known, the `Name` key will contain the UUID as well.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the list of service class descriptors.

    # Tailcall so varname context is correct
    tailcall Uuids $sdr 1 $varname
}

proc iocp::bt::sdr::language_offsets {sdr {varname {}}} {
    # Retrieve the language base attributes in a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The retrieved value is a nested dictionary
    # corresponding to the `LanguageBaseAttributeIDList`
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # The dictionary value returned or stored is indexed by language
    # identifiers as defined in ISO639, e.g. `en`, `fr` etc.
    # The corresponding value is itself a dictionary with keys `BaseOffset`
    # and `Encoding`. See the Bluetooth specification for the former.
    # The latter is either a Tcl encoding name or in case the encoding
    # is not supported by Tcl, an integer value that identifies an encoding
    # as per the Bluetooth specification. fs
    #
    # Returns a boolean or a dictionary of language attributes.
    if {![AttributeValue $sdr 6 sequence]} {
        if {$varname eq ""} {
            error "Attribute not found."
        } else {
            return 0
        }
    }

    # attr is {sequence {langid encoding baseoffset ...}}
    set langs [dict create]
    foreach {langid enc offset} $sequence {
        # langid is ISO 639 language code
        set langid [lindex $langid 1]
        set langid "[format %c [expr {$langid >> 8}]][format %c [expr {$langid &0xff}]]"
        dict set langs $langid Encoding [encoding_name [lindex $enc 1]]
        dict set langs $langid BaseOffset [lindex $offset 1]
    }
    if {$varname eq ""} {
        return $langs
    } else {
        upvar 1 $varname var
        set var $langs
        return 1
    }
}

proc iocp::bt::sdr::service_name {sdr lang {varname {}}} {
    # Returns the service name from a service discovery record.
    #  sdr - parsed service discovery record
    #  lang - language identifier as specified in iso639, e.g. `en` for
    #         english, `fr` for french etc. or the keyword `primary`.
    #  varname - optional name of variable in caller's context.
    #
    # The service name corresponds to the ServiceRecordHandle
    # universal attribute in the Bluetooth Core specification.
    #
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    #

    # tailcall so upvar works in caller's context
     tailcall TextAttribute $sdr 0 $lang $varname
}

proc iocp::bt::sdr::description {sdr lang {varname {}}} {
    # Returns the service description from a service discovery record.
    #  sdr - parsed service discovery record
    #  lang - language identifier as specified in iso639, e.g. `en` for
    #         english, `fr` for french etc. or the keyword `primary`.
    #  varname - optional name of variable in caller's context.
    #
    # The service description corresponds to the ServiceDescription
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 

    # tailcall so upvar works in caller's context
    tailcall TextAttribute $sdr 1 $lang $varname
}

proc iocp::bt::sdr::provider_name {sdr lang {varname {}}} {
    # Returns the provider name from a service discovery record.
    #  sdr - parsed service discovery record
    #  lang - language identifier as specified in iso639, e.g. `en` for
    #         english, `fr` for french etc. or the keyword `primary`.
    #  varname - optional name of variable in caller's context.
    #
    # The provider name corresponds to the ProviderName
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 

    # tailcall so upvar works in caller's context
    tailcall TextAttribute $sdr 2 $lang $varname
}

proc iocp::bt::sdr::state_indicator {sdr {varname {}}} {
    # Retrieve the state indicator for a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The state indicator corresponds to the ServiceRecordState universal
    # attribute in the Bluetooth Core specification. If present, its value
    # changes every time the service record is changed in any manner. See
    # the Bluetooth specification for more details 
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the service record handle.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 2 $varname
}

proc iocp::bt::sdr::service_id {sdr {varname {}}} {
    # Retrieve the service id for a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The service id is a UUID and corresponds to the ServiceId universal
    # attribute in the Bluetooth Core specification. If present, it
    # universally and uniquely identifies a particular service instance.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the service id.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 3 $varname
}

proc iocp::bt::sdr::protocols {sdr {varname {}}} {
    # Retrieve the protocol information from a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # Information about the protocols that may be used to access the
    # service described by SDR is stored in the ProtocolDescriptorList
    # universal attribute as described in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # The returned information is in the form of a list each element of
    # which describes a protocol stack that may be used to access the 
    # service. Each such element is itself a list whose elements correspond
    # to layers in that protocol stack starting with the lowest layer first.
    # Each layer is described as a dictionary with the following keys:
    #  Uuid - the UUID of the protocol for the layer
    #  Name - the name of the protocol
    #  Params - the protocol parameters. This is a list of raw data elements
    #           of the form `{type value}` the interpretation of which is
    #           protocol dependent. 
    #  
    # Returns a boolean or the list of protocols.

    tailcall Protocols $sdr 4 $varname
}

proc iocp::bt::sdr::browse_groups {sdr {varname {}}} {
    # Retrieve the browse groups attribute in a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The retrieved value is a list of browse descriptors
    # corresponding to the BrowseGroupList
    # universal attribute in the Bluetooth Core specification.
    # Each descriptor is a dictionary with keys `Uuid` and `Name` containing
    # the UUID of the browse group and its mnemonic. If the latter
    # is not known, the `Name` key will contain the UUID as well.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the list of browse group UUID descriptors.

    # Tailcall so varname context is correct
    tailcall Uuids $sdr 5 $varname
}

proc iocp::bt::sdr::profiles {sdr {varname {}}} {
    # Retrieve the service classes in a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The retrieved value is a list of profile descriptors
    # corresponding to the BluetoothProfileDescriptorList
    # universal attribute in the Bluetooth Core specification.
    # Each descriptor is a dictionary with the following keys:
    #  `Uuid` - the UUID of the profile
    #  `Name` - the mnemonic name of the profile or its UUID if the
    #           UUID cannot be mapped to a name
    #  `MajorVersion` - the profile's major version
    #  `MinorVersion` - the profile's minor version
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the list of service class descriptors.

    if {! [has_attribute $sdr 9 attr]} {
        if {$varname eq ""} {
            error "Attribute not found."
        } else {
            return 0
        }
    }

    # Sequence of profile descriptors. Each profile descriptor
    # is sequence of profile uuid and version.
    set profiles {}
    foreach profile [lindex $attr 1] {
        set uuid [lindex $profile 1 0 1]
        set version [lindex $profile 1 1 1]
        lappend profiles [dict create \
            Uuid $uuid \
            Name [names::name $uuid] \
            MajorVersion [expr {$version >> 8}] \
            MinorVersion [expr {$version & 0xff}]
        ]
    }

    if {$varname eq ""} {
        return $profiles
    } else {
        upvar 1 $varname var
        set var $profiles
        return 1
    }

}

proc iocp::bt::sdr::additional_protocols {sdr {varname {}}} {
    # Returns protocol information from the additional protocols field
    # in a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    # This command is identical to [protocols] except it extracts
    # protocol information from the `AdditionalProtocolDescriptorList` 
    # attribute instead of `ProtocolDescriptorList`. 
    tailcall Protocols $sdr 13 $varname
}

proc iocp::bt::sdr::time_to_live {sdr {varname {}}} {
    # Retrieve the time-to-live attribute for a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The returned value corresponds to the ServiceInfoTimeToLive
    # universal attribute in the Bluetooth Core specification. It is
    # a hint as to the number of seconds from the time of reception the
    # information # in the service discovery record is expected to be valid.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the integer time-to-live value.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 7 $varname
}

proc iocp::bt::sdr::availability {sdr {varname {}}} {
    # Retrieve the service availability attribute from a service 
    # discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The service availability corresponds to the ServiceAvailability
    # universal attribute in the Bluetooth Core specification. It
    # is a value in the range 0-255 that indicates the relative
    # availability of the service in terms of the number of additional
    # clients it can accept.
    #
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the service availability value.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 8 $varname
}

proc iocp::bt::sdr::documentation_url {sdr {varname {}}} {
    # Retrieve the documentation url from a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The service record handle corresponds to the DocumentationURL
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the documentation url.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 10 $varname
}

proc iocp::bt::sdr::client_executable_url {sdr {varname {}}} {
    # Retrieve the client executable URL from a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The service record handle corresponds to the ClientExecutableURL
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the client executable URL.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 11 $varname
}

proc iocp::bt::sdr::icon_url {sdr {varname {}}} {
    # Retrieve the icon URL from a service discovery record.
    #  sdr - parsed service discovery record
    #  varname - optional name of variable in caller's context.
    #
    # The service record handle corresponds to the IconURL
    # universal attribute in the Bluetooth Core specification.
    # 
    # If $varname is not the empty string, it should be the name of
    # a variable in the caller's context.
    #  - If the attribute is present, the command returns `1` and
    #    stores the attribute value in the variable.
    #  - If the attribute is not present, the command returns 0.
    #
    # If $varname is an empty string,
    #  - If the attribute is present, the command returns its value.
    #  - If the attribute is not present, the command raises an error.
    # 
    # Returns a boolean or the icon URL.

    # Tailcall so varname context is correct
    tailcall AttributeValue $sdr 12 $varname
}

proc iocp::bt::sdr::Uuids {sdr attr_id {varname {}}} {
    if {! [has_attribute $sdr $attr_id attrval]} {
        if {$varname eq ""} {
            error "Attribute not found."
        } else {
            return 0
        }
    }

    set uuids [lmap elem [lindex $attrval 1] {
        set uuid [lindex $elem 1]
        dict create Uuid $uuid Name [names::name $uuid]
    }]

    if {$varname eq ""} {
        return $uuids
    } else {
        upvar 1 $varname var
        set var $uuids
        return 1
    }
}

proc iocp::bt::sdr::Protocols {sdr attr_id {varname {}}} {

    if {! [has_attribute $sdr $attr_id attrval]} {
        if {$varname eq ""} {
            error "Attribute not found."
        } else {
            return 0
        }
    }

    # The attribute value may either be a single protocol stack encoded
    # as a sequence, or multiple stacks encoded as a selection of sequences.
    if {[lindex $attrval 0] eq "sequence"} {
        # Single protocol stack
        set stacks [list [lindex $attrval 1]]
    } else {
        # selection - Multiple stacks
        set stacks [lindex $attrval 1]
    }

    set protocols [lmap stack $stacks {
        # stack -> {protocol_descriptor protocol_descriptor ...}
        # protocol_descriptor -> {sequence {data_element ...}}
        lmap protocol_descriptor $stack {
            # Each descriptor is a sequence, the first element identifying the
            # protocol and the remaining being (optional) parameters.
            set elements [lindex $protocol_descriptor 1]
            set protocol_uuid [lindex $elements 0 1]
            dict create \
                Protocol $protocol_uuid \
                ProtocolName [names::protocol_name $protocol_uuid] \
                ProtocolParams [lrange $elements 1 end]
        }
    }]

    if {$varname eq ""} {
        return $protocols
    } else {
        upvar 1 $varname var
        set var $protocols
        return 1
    }
}

proc iocp::bt::sdr::AttributeValue {sdr attr_id {varname {}}} {
    if {$varname eq ""} {
        return [lindex [attribute $sdr $attr_id] 1]
    } else {
        if {[has_attribute $sdr $attr_id attrval]} {
            upvar 1 $varname value
            set value [lindex $attrval 1]
            return 1
        } else {
            return 0
        }
    }
}

proc iocp::bt::sdr::TextAttribute {sdr attr_offset lang {varname {}}} {

    # Assume that if sdr does not explicitly specify encoding, it is UTF8
    set enc utf-8
    set base_offset 256
    
    # Primary language's ServiceName attribute index always 256
    # Otherwise get the offset from the base index 256
    if {$lang ne "primary"} {
        # See if the specified language has an entry in the languages table
        if {[sdr_language_offsets $sdr base_offsets] &&
            [dict exists $base_offsets $lang]} {
            # Yes, so the language-specific text will be at an offset
            # from the base attribute index
            set base_offset [dict get $base_offsets $lang BaseOffset]
            set enc [dict get $base_offsets $lang Encoding]
        }
    }
    set attr_index [expr {$base_offset + $attr_offset}]
    if {! [has_attribute $sdr $attr_index name]} {
        # This language does not exist. Return primary language if possible.
        # If base_offset was 256, that's already primary so don't retry
        if {$base_offset == 256 ||
            ![has_attribute $sdr [expr {256 + $attr_offset}] name] } {
            if {$varname ne ""} {
                return 0
            } else {
                error "Attribute not found in SDR."
            }
        }
    }

    set name [lindex $name 1]

    # $name is encoded. If Tcl supports the encoding, well and good.
    # Else return a hex form
    if {[string is integer $enc]} {
        # SDR specified an encoding but not one that Tcl knows about
        set text [binary encode hex $name]
    } else {
        set text [encoding convertfrom $enc $name]
    }

    if {$varname ne ""} {
        upvar 1 $varname var
        set var $text
        return 1
    } else {
        return $text
    }
}

proc iocp::bt::sdr::ParseElements {delem} {
    # Parses a Data Element as defined in the Bluetooth specification.
    #  delem - Data element in binary format

    set parsed {}
    while {[string length $delem]} {
        lassign [ExtractFirstElement $delem] type value delem
        switch -exact -- $type {
            sequence { lappend parsed [list sequence [ParseElements $value]]}
            selection { lappend parsed [list selection [ParseElements $value]]}
            default {
                lappend parsed [list $type $value]
            }
        }
    }
    return $parsed
}

proc iocp::bt::sdr::ExtractFirstElement {bin} {
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
        return [list nil "" [string range $bin 1 end]]
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
                append elem - [string range $hex 20 end]
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
            set type selection
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

proc iocp::bt::sdr::print {rec} {
    # Prints a SDP record to a more human readable form.
    # rec - a binary SDP record in the form returned by [browse_services] or
    #       [get_service_references].
    #

    # Alternating attribute value pairs
    set rec [parse $rec]
    foreach attr [attributes $rec] {
        set attrname [names::attribute_name $attr]
        if {[string is integer -strict $attrname]} {
            # For easier matching with Bluetooth specs
            set attrname [format 0x%x $attrname]
        }
        switch -exact -- $attrname {
            ServiceRecordHandle {
                puts "$attrname: [handle $rec]"
            }
            ServiceClassIDList {
                puts "$attrname: sequence"
                foreach elem [service_classes $rec] {
                    dict with elem {
                        if {$Name eq $Uuid} {
                            puts "    $Uuid"
                        } else {
                            puts "    $Uuid $Name"
                        }
                    }
                }
            }
            ServiceRecordState {
                puts "$attrname: [state_indicator $rec]"
            }
            ServiceId {
                puts "$attrname: [service_id $rec]"

            }
            AdditionalProtocolDescriptorList -
            ProtocolDescriptorList {
                puts "$attrname:"
                foreach protocol [protocols $rec] {
                    puts "    ProtocolStack:"
                    foreach layer $protocol {
                        set name [dict get $layer ProtocolName]
                        set uuid [dict get $layer Protocol]
                        set params [lmap param [dict get $layer ProtocolParams] {
                            PrintableElement $param
                        }]
                        if {$name eq ""} {
                            puts "        $uuid ([join $params {, }])"
                        } else {
                            puts "        $uuid $name ([join $params {, }])"
                        }
                    }
                }
            }
            BrowseGroupList {
                puts "$attrname: sequence"
                foreach elem [browse_groups $rec] {
                    dict with elem {
                        if {$Name eq $Uuid} {
                            puts "    $Uuid"
                        } else {
                            puts "    $Uuid $Name"
                        }
                    }
                }
            }
            LanguageBaseAttributeIDList {
                puts "$attrname: sequence"
                dict for {lang val} [language_offsets $rec] {
                    puts "    $lang: [dict get $val Encoding], [dict get $val BaseOffset]"
                }
            }
            ServiceInfoTimeToLive {
                puts "$attrname: [time_to_live $rec]"
            }
            ServiceAvailability {
                puts "$attrname: [availability $rec]"
            }
            BluetoothProfileDescriptorList {
                puts "$attrname: sequence"
                foreach profile [profiles $rec] {
                    # Sequence of profiles. Each profile is a sequence of
                    # profile uuid and version.
                    # puts profile:$profile
                    dict with profile {
                        if {$Uuid eq $Name} {
                            puts "    $Uuid v$MajorVersion.$MinorVersion"
                        } else {
                            puts "    $Uuid $Name v$MajorVersion.$MinorVersion"
                        }
                    }
                }
            }
            DocumentationURL {
                puts "$attrname: [documentation_url $rec]"
            }
            ClientExecutableURL {
                puts "$attrname: [client_executable_url $rec]"
            }
            IconURL {
                puts "$attrname: [icon_url $rec]"
            }
            ServiceName {
                puts "$attrname: [service_name $rec primary]"
            }
            ServiceDescription {
                puts "$attrname: [description $rec primary]"
            }
            ProviderName {
                puts "$attrname: [provider_name $rec primary]"
            }
            default {
                # TBD - special handling for 0x100-0x1ff attributes by
                # looking up languages table
                puts "$attrname: [PrintableElement [attribute $rec $attr]]"
            }
        }
    }
}

proc iocp::bt::sdr::PrintableElement {delem {parent_indent {}}} {
    lassign $delem type val
    switch -exact -- $type {
        nil -
        uint -
        int { return $val }
        uuid { 
            set name [names::name $val]
            if {$name eq $val} {
                return $val
            } else {
                return "$val $name"
            }
        }
        text -
        url {
            # Assume UTF-8 binary form
            # Strip terminating null. TBD - is null alwys present?
            set printable [encoding convertfrom utf-8 $val]
        }
        boolean { return [expr {$val ? "true" : "false"}] }
        selection -
        sequence {
            set printable [list $type]
            set new_indent "${parent_indent}    "
            foreach elem $val {
                lappend printable "${new_indent}[PrintableElement $elem $new_indent]"
            }
            set printable [join $printable \n]
        }
        unknown {
            set printable [BinToHex $val]
        }
    }
    return $printable
}
