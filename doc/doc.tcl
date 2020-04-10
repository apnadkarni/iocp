# This file contains code to generate the documentation for the iocp package.
# It also defines dummy procedures that document commands implemented in C.

package require ruff
if {[catch {package require iocp}]} {
    source [file join [file dirname [info script]] .. lib iocp.tcl]
}

namespace eval iocp {
    variable _preamble {
        The `iocp` package implements channels and related utility
        commands on Windows platforms. The package requires Windows 7
        or later.

        The implementation uses I/O Completion Ports (IOCP) and affords
        greatly improved performance over equivalent functionality in
        the Tcl core. In addition, new channel types are supported.

        The package commands are broken up into the following namespaces
        based on functional areas:
        
        [::iocp::inet] - Commands for TCP/IP channels
        [::iocp::bt]   - Core commands for Bluetooth communication
        [::iocp::bt::sdr] - Commands for Bluetooth service discovery records
        [::iocp::bt::names] - Commands for mapping Bluetooth UUIDs to names
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
        over Bluetooth. In addition, it provides commands for
        device and service discovery. The related child namespaces
        [sdr] and [names] provide additional utilities.

        *Note: currently only client-side communication is implemented.*
    }

    
    # Dummy procs to document C commands
    proc socket {args} {
        # Returns a client Bluetooth RFCOMM channel.
        #   args - see below.
        # The command takes the form
        #    socket ?-async? device port
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
        [iocp::bt::browse_services] and [iocp::bt::get_service_references]
        commands. These return a list of binary records which must be
        first parsed with the [parse] commands. Individual service attributes
        can then be retrieved from the parsed commands using the other
        commands in the namespace.
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
    set namespaces [list $ns ${ns}::inet ${ns}::bt ${ns}::bt::sdr ${ns}::bt::names]
    ruff::document $namespaces -autopunctuate 1 -excludeprocs {^[_A-Z]} \
        -recurse 0 -preamble $_preamble -pagesplit namespace \
        -output $outfile -includesource 1 \
        -title "iocp package reference (V[package present iocp])" \
        {*}$args
}
