# This file contains code to generate the documentation for the iocp package.
# It also defines dummy procedures that document commands implemented in C.

package require ruff
package require iocp
package require iocp_bt

namespace eval iocp {
    variable _preamble {
        The `iocp` set of packages implements channels and related utility
        commands on Windows platforms. The package requires Windows 7
        or later.

        The base `iocp` package implements the core functionality, based on
        Windows I/O Completion Ports (IOCP), that is used by the other packages.
        The `iocp` namespace itself does not contain any exported commands.

        The `iocp_inet` package implements the [::iocp::inet::socket] command which has
        the same functionality as the Tcl `socket` command but with several
        times the performance.

        The `iocp_bt` package implements Bluetooth channels (currently client-only)
        along with supporting commands for device and service discovery.
    }
}

namespace eval iocp::inet {
    # Dummy procs to document C commands

    variable _ruff_preamble {
        The `iocp::inet` namespace implements communication over
        TCP/IP. It provides the same interfaces as the Tcl core `socket`
        command but with greater performance.
    }

    proc socket args {
        # Returns a client or server TCP/IP channel.
        #   args - see the Tcl core `socket` command
        # The command provides the same interface as the Tcl core 
        # [socket](http://www.tcl-lang.org/man/tcl8.6/TclCmd/socket.htm)
        # command. Refer to the Tcl 
        # [documentation](http://www.tcl-lang.org/man/tcl8.6/TclCmd/socket.htm)
        # for details.
        #
        # The only enhancement offered by this command is significantly 
        # improved performance with reduced CPU load.
        #
        # The returned channel must be closed with the Tcl `close`
        # or `chan close` command.
    }
}

namespace eval iocp::bt {

    variable _ruff_preamble {
        The `iocp::bt` namespace implements commands for communicating
        over Bluetooth. 

        The commands are broken into the following namespaces:

        ::iocp::bt        - Core commands for Bluetooth communication.
        ::iocp::bt::sdr   - Commands for handling Bluetooth service
                              discovery records.
        ::iocp::bt::names - Commands for mapping Bluetooth UUIDs and names.

        Note the current limitations:

        * Only client-side communication is implemented in this release.
        Server-side functionality will be added in some future release
        based on demand.

        * Only RFCOMM channels are supported. There is no support for L2CAP
        or other protocols as these are not exposed at the Win32 API level.

        * Bluetooth LE is not supported for the same reason.

        ## Device discovery

        Remote Bluetooth devices are discovered through [devices] command. It is
        generally recommended that a new device inquiry be initiated with the
        `-inquire` option when using this command as otherwise newly reachable
        devices will not be discovered. The [device printn] command will print
        the information related to each device in human-readable form.

        Bluetooth radios on the local system can be enumerated with the
        [radios] command. There is however rarely a need to do this as it
        is not required for a establishing a Bluetooth connection.

        ## Service discovery

        A device will generally host multiple services. The [device services] commands
        will retrieve information about the services advertised by the device.
        This information is in the form of *service discovery records*. Commands
        for parsing these records are contained in the [sdr] namespace.

        Services and service classes are identified with UUID's. Most commands
        will however accept mnemonics for services defined in the standard as
        they are easier to remember than the UUID's. The
        [names::print] command will print the list of mnemonics
        and the corresponding UUID's.

        ## Connection establishment

        Establishing a Bluetooth connection involves the following steps.

        First, the device name has to be mapped to its physical address. Unlike
        the TCP sockets in Tcl, Bluetooth sockets require physical addresses to
        be specified as device names are ambiguous. The [device address] command
        can be used to obtain the physical addresses corresponding to a name.
        Note that there can be multiple devices with the same name so the
        command returns a list of addresses, one per device. When the list
        contains more than one address, generally the user needs to be prompted
        to pick one though below we just assume there is a single address in the
        list.

        ``````
        set addrs [iocp::bt::device_address "APN Phone"]
        set addr  [lindex $addrs 0]
        ``````

        Next, the port the service is listening on needs to be resolved with the
        [device port] command. In the example below, `OBEXObjectPush` is
        the service of interest.

        ``````
        set port [iocp::bt::service_port $addr OBEXObjectPush]
        ``````

        Finally, a connection is established to the service using the
        [socket] command.

        ``````
        set so [iocp::bt::socket $addr $port]
        ``````

        Other commands in the namespace provide supporting functions such
        as device and service discovery.

    }

    # Dummy procs to document C commands
    proc socket {args} {
        # Returns a client Bluetooth RFCOMM channel.
        #   args - see below.
        # The command takes the form
        #
        #     socket ?-async? device port
        #
        # where `device` is the Bluetooth hardware address of the
        # remote device and `port` is the RFCOMM port (channel).
        # The `-async` option has the same effect as in the Tcl
        # [socket](http://www.tcl-lang.org/man/tcl8.6/TclCmd/socket.htm)
        # command. It returns immediately without waiting for the
        # connection to complete.
        # 
        # Once the connection is established, Bluetooth channel operation
        # is identical to that of Tcl sockets except that half closes
        # are not supported.
        #
        # The returned channel must be closed with the Tcl `close`
        # or `chan close` command.
    }
}

namespace eval iocp::bt::sdr {
    variable _ruff_preamble {
        The `iocp::bt::sdr` namespace contains commands for parsing Bluetooth
        **Service Discovery Records** (SDR) as returned by the
        [::iocp::bt::device services] and
        [::iocp::bt::device service_references]
        commands. These return a list of binary records which must be
        first parsed with the [decode] command. Individual service attributes
        can then be retrieved from the decoded records using the other
        commands in the namespace.

        ``````
        set dev  [iocp::bt::device_address "APN Phone"]
        set recs [iocp::bt::services $dev]
        set sdr  [iocp::bt::sdr::decode [lindex $recs 0]]
        set service_classes [iocp::bt::sdr::service_classes $sdr]
        ``````
    }
}

namespace eval iocp::bt::names {
    variable _ruff_preamble {
        Most objects in Bluetooth services are identified using UUIDs or
        in some cases numeric identifiers. The `iocp::bt::names` namespace
        contains utility commands to map these to human readable names.
    }

}

proc iocp::Document {outfile args} {
    # Generates documentation for the actor package
    #  outfile - name of output file
    #  args - additional arguments to be passed to `ruff::document`.
    # The documentation is generated in HTML format. The `ruff` 
    # documentation generation package must be installed.
    #
    # Warning: any existing file will be overwritten.
    variable _preamble

    set ns [namespace current]
    set namespaces [list ${ns}::inet ${ns}::bt ${ns}::bt::sdr ${ns}::bt::names]
    ruff::document $namespaces -autopunctuate 1 -excludeprocs {^[_A-Z]} \
        -recurse 0 -preamble $_preamble -pagesplit namespace \
        -output $outfile -includesource 1 \
        -title "iocp package reference (V[package present iocp])" \
        {*}$args
}

if {[file normalize $argv0] eq [file normalize [info script]]} {
    cd [file dirname [info script]]
    iocp::Document iocp.html {*}$argv
}
