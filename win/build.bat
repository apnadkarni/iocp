cd %~dp0
IF NOT DEFINED VSCMD_ARG_TGT_ARCH call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
IF "x%INSTALLDIR%" == "x" echo INSTALL not set
IF "x%INSTALLDIR%" == "x" set INSTALLDIR=D:\Tcl\90\%VSCMD_ARG_TGT_ARCH%-debug
nmake /s /f makefile.vc INSTALLDIR=%INSTALLDIR% OPTS=pdbs cdebug="-Zi -Od" %*

