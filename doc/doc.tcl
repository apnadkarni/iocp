# This file contains code to generate the documentation for the iocp package.
# It also defines dummy procedures that document commands implemented in C.

package require ruff
package require iocp
package require iocp_bt

namespace eval iocp {
    variable _preamble {

        The `iocp` Tcl extension implements channels and related utility
        commands on Windows platforms based on Windows I/O Completion Ports
        (IOCP). The extension requires Windows 7 or later.

        The extension includes the following packages:

        * The `iocp_inet` package implements communication channels over TCP/IP.

        * The `iocp_bt` package implements Bluetooth channels (currently client-only)
        along with supporting commands for device and service discovery.

        ## Downloads

        The extension may be downloaded from
        <https://sourceforge.net/projects/magicsplat/files/iocp/>.

        ## Source code

        The source code is available from the Github
        [repository](https://github.com/apnadkarni/iocp).
    }
}

namespace eval iocp::inet {
    # Dummy procs to document C commands

    variable _ruff_preamble {
        The `iocp::inet` namespace implements communication over
        TCP/IP. It provides the same interface as the Tcl core `socket`
        command but with greater performance.

        The package is loaded as

            package require iocp_inet
    }

    proc socket args {
        # Returns a client or server TCP/IP channel.
        #   args - see the Tcl core `socket` command
        #
        # The command provides the same interface as the Tcl core
        # [socket](http://www.tcl-lang.org/man/tcl8.6/TclCmd/socket.htm) command
        # except for the additional socket options listed below. Refer to the
        # documentation of the Tcl
        # [socket](http://www.tcl-lang.org/man/tcl8.6/TclCmd/socket.htm)
        # command for details.
        #
        # The only functional enhancement offered by this command is
        # significantly improved performance with reduced CPU load.
        #
        # In addition to the standard configuration options supported
        # by the Tcl `socket` command, the following additional configuration
        # options are supported through the Tcl `fconfigure` and
        # `chan configure` commands. They can be read as well as set.
        #
        #  -keepalive BOOL - Controls the socket `SO_KEEPALIVE` option.
        #  -maxpendingaccepts COUNT - Maximum number of pending accepts to post
        #    on the socket (listening socket only).
        #  -maxpendingreads COUNT - Maximum number of pending reads to post
        #    on the socket.
        #  -maxpendingwrites COUNT - Maximum number of pending writes to post
        #    on the socket.
        #  -nagle BOOL - Controls the socket `TCL_NODELAY` option
        #  -sorcvbuf BUFSIZE - Size of Winsock socket receive buffer.
        #  -sosndbuf BUFSIZE - Size of Winsock socket send buffer.
        #
        # It is recommended these be left at their default values except
        # in cases where performance needs to be fine tuned for specific
        # traffic patterns. The `netbench` utility may be used for the
        # purpose.
        #
        # The returned channel must be closed with the Tcl `close`
        # or `chan close` command.
    }
}

namespace eval iocp::bt {

    variable _ruff_preamble {
        The `iocp_bt` package implements Bluetooth support and is loaded as

            package require iocp_bt

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

        * Bluetooth LE is not currently supported.

        This documentation is a reference for the package. For an introductory
        guide, see the [tutorials](https://www.magicsplat.com/blog/tags/bluetooth/).

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
        be specified as device names are ambiguous. The [device addresses] command
        can be used to obtain the physical addresses corresponding to a name.
        Note that there can be multiple devices with the same name so the
        command returns a list of addresses, one per device. When the list
        contains more than one address, generally the user needs to be prompted
        to pick one though below we just assume there is a single address in the
        list.

        ``````
        set addrs [iocp::bt::device addresses "APN Phone"]
        set addr  [lindex $addrs 0]
        ``````

        Next, the port the service is listening on needs to be resolved with the
        [device port] command. In the example below, `OBEXObjectPush` is
        the service of interest.

        ``````
        set port [iocp::bt::service port $addr OBEXObjectPush]
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
        #     socket ?-async? ?-authenticate? device port
        #
        # where `device` is the Bluetooth hardware address of the
        # remote device and `port` is the RFCOMM port (channel).
        #
        # The `-async` option has the same effect as in the Tcl
        # [socket](http://www.tcl-lang.org/man/tcl8.6/TclCmd/socket.htm)
        # command. It returns immediately without waiting for the
        # connection to complete.
        #
        # The `-authenticate` option actively initiates authentication if
        # the remote device was not previously authenticated.
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
        set dev  [iocp::bt::device address "APN Phone"]
        set recs [iocp::bt::device services $dev]
        set sdr  [iocp::bt::sdr::decode [lindex $recs 0]]
        set service_classes [iocp::bt::sdr::attribute get $sdr ServiceClassIDList]
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
        -navigation sticky \
        -outdir html -outfile $outfile -includesource 0 \
        -title "iocp" \
        -version "[package present iocp]" \
        {*}$args
}

if {[file normalize $argv0] eq [file normalize [info script]]} {
    cd [file dirname [info script]]
    iocp::Document iocp.html -copyright "Ashok P. Nadkarni" {*}$argv
}
