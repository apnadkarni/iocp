# Build instructions

## Visual C++

Run `release.cmd` in the top level directory after editing the paths at
top of the file. This will create a distribution in the dist/latest directory
with support for all combinations of Tcl 8/9 and x86/x64.

## MingW

Needs to be manually done. To build one combination, say Tcl 8 for x64,
from the MingW64 shell,

```
../../configure --enable-threads --enable-64bit --with-tcl=d:/tcl/86/mingw-x64-debug/lib --with-tclinclude=d:/tcl/86/mingw-x64-debug/include --libdir=/d/temp/iocp
make -s
make -s install-strip
```

Repeat above for Tcl 9. Then similarly for x86 from the MinW86 shell.

/d/temp/iocp/iocpVERSION will then contain the distribution.
