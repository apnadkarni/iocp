#ifndef OPAQUE_H
#define OPAQUE_H

#include "tcl.h"

Tcl_Obj *ObjFromOpaque(void *pointerValue, char *typeName);
int ObjToOpaque(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, const char *name);
int ObjToOpaqueAny(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, ...);

#endif
