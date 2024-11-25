# Packages iocp, iocp\_inet, iocp\_bt

Tcl extension implementing TCP and Bluetooth channels based on
I/O completion ports. The iocp_inet package is API-compatible
with Tcl sockets while offering much higher performance.

Requires Tcl 8.6 or Tcl 9 and Windows 7 or later.

Binary downloads are at https://sourceforge.net/projects/magicsplat/files/iocp/.
The single package supports all combinations of Tcl 8.6/9 and
32/64-bit Intel platforms.

Documentation at https://iocp.magicsplat.com.

## Changes in 2.0.2

- Support for Tcl 9
- Added `-authenticate` option to `::iocp::bt::socket`

No functional or API changes.

## Changes in 1.1.0

- enable lazy loading of Bluetooth so TCP sockets can still be used on servers
without Bluetooth libraries installed
- fixed crash in Bluetooth asynchronous connect
- fixed reversed sense of -nagle option to TCP sockets

