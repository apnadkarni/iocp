# Build instructions

## Visual C++

Run `release.cmd` in the top level directory after editing the paths at
top of the file. This will create a distribution in the dist/VERSION directory
with support for all combinations of Tcl 8/9 and x86/x64.

## MinGW64

Same as above but run `release-mingw.cmd` instead, again from a DOS prompt
(not a MinGW shell)

## Docs

To generate documentation,

   cd doc
   tclsh doc.tcl

The ruff and iocp packages need to be in Tcl's auto_path.
