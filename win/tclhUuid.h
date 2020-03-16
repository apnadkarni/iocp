#ifndef TCLHWRAP_H
#define TCLHWRAP_H

#include "tcl.h"

#ifdef _WIN32
# include <rpc.h>
  typedef UUID Tclh_UUID;
#else
# include <uuid/uuid.h>
  typedef uuid_t Tclh_UUID;
#endif

/* Function: Tclh_NewUuidObj
 * Generate a new UUID. The UUID is not guaranteed to be cryptographically
 * secure.
 *
 * Returns:
 * Pointer to a Tcl_Obj wrapping a generated UUID.
 */
Tcl_Obj *Tclh_NewUuidObj ();

/* Function: Tclh_WrapUuid
 * Wraps a Tclh_UUID as a Tcl_Obj.
 *
 * Parameters:
 * uuidP - pointer to a binary UUID.
 *
 * Returns:
 * The Tcl_Obj containing the wrapped pointer.
 */
Tcl_Obj *Tclh_WrapUuid (Tclh_UUID *uuidP);

/* Function: Tclh_UnwrapUuid
 * Unwraps a Tcl_Obj containing a UUID.
 *
 * Parameters:
 * interp - Pointer to interpreter
 * objP   - pointer to the Tcl_Obj wrapping the UUID.
 * uuidP  - Location to store UUID.
 *
 * Returns:
 * TCL_OK    - Success, UUID stored in *uuidP.
 * TCL_ERROR - Failure, error message stored in interp.
 */
int Tclh_UnwrapUuid (Tcl_Interp *interp, Tcl_Obj *objP, Tclh_UUID *uuidP);

#ifdef TCLH_IMPL
# define TCLH_UUID_IMPL
#endif

#ifdef TCLH_SHORTNAMES
#define NewUuidObj Tclh_NewUuidObj
#define WrapUuid   Tclh_WrapUuid
#define UnwrapUuid Tclh_UnwrapUuid
#endif


#ifdef TCLH_UUID_IMPL

#include "tclhBase.h"


/*
 * Uuid: Tcl_Obj custom type
 * Implements custom Tcl_Obj wrapper for Uuid.
 */
static void DupUuidObj(Tcl_Obj *srcObj, Tcl_Obj *dstObj);
static void FreeUuidObj(Tcl_Obj *objP);
static void StringFromUuidObj(Tcl_Obj *objP);
static int  SetUuidObjFromAny(Tcl_Obj *objP);

static struct Tcl_ObjType gUuidVtbl = {
    "Tclh_Uuid",
    FreeUuidObj,
    DupUuidObj,
    StringFromUuidObj,
    NULL
};
TCLH_INLINE Tclh_UUID *IntrepGetUuid(Tcl_Obj *objP) {
    return (Tclh_UUID *) objP->internalRep.twoPtrValue.ptr1;
}
TCLH_INLINE void IntrepSetUuid(Tcl_Obj *objP, Tclh_UUID *value) {
    objP->internalRep.twoPtrValue.ptr1 = (void *) value;
}

static void DupUuidObj(Tcl_Obj *srcObj, Tcl_Obj *dstObj)
{
    Tclh_UUID *uuidP = (Tclh_UUID *) ckalloc(16);
    memcpy(uuidP, IntrepGetUuid(srcObj), 16);
    IntrepSetUuid(dstObj, uuidP);
    dstObj->typePtr = &gUuidVtbl;
}

static void FreeUuidObj(Tcl_Obj *objP)
{
    ckfree(IntrepGetUuid(objP));
    IntrepSetUuid(objP, NULL);
}

static void StringFromUuidObj(Tcl_Obj *objP)
{
#ifdef _WIN32
    UUID *uuidP = IntrepGetUuid(objP);
    char *uuidStr;
    int  len;
    if (UuidToStringA(uuidP, &uuidStr) != RPC_S_OK) {
        TCLH_PANIC("Out of memory stringifying UUID.");
    }
    len          = Tclh_strlen(uuidStr);
    objP->bytes  = Tclh_strdupn(uuidStr, len);
    objP->length = len;
    RpcStringFreeA(&uuidStr);
#else
    objP->bytes = ckalloc(37); /* Number of bytes for string rep */
    objP->length = 36;         /* Not counting terminating \0 */
    uuid_unparse_lower(IntrepGetUuid(objP), objP->bytes);
#endif
}

static int  SetUuidObjFromAny(Tcl_Obj *objP)
{
    Tclh_UUID *uuidP;

    if (objP->typePtr == &gUuidVtbl)
        return TCL_OK;
    uuidP = ckalloc(sizeof(*uuidP));
#ifdef _WIN32
    RPC_STATUS rpcStatus = UuidFromStringA((unsigned char *) Tcl_GetString(objP), 
                                           uuidP);
    if (rpcStatus != RPC_S_OK) {
        ckfree(uuidP);
        return TCL_ERROR;
    }
#else
    if (uuid_parse(Tcl_GetString(objP), uuidP) != 0) {
        ckfree(uuidP);
        return TCL_ERROR;
    }
#endif /* _WIN32 */

    IntrepSetUuid(objP, uuidP);
    objP->typePtr = &gUuidVtbl;
    return TCL_OK; 
}

Tcl_Obj *Tclh_WrapUuid (Tclh_UUID *from) 
{
    Tcl_Obj *objP;
    Tclh_UUID *uuidP;

    objP = Tcl_NewObj();
    Tcl_InvalidateStringRep(objP);
    uuidP = ckalloc(sizeof(*uuidP));
    memcpy(uuidP, from, sizeof(*uuidP));
    IntrepSetUuid(objP, uuidP);
    objP->typePtr = &gUuidVtbl;
    return objP;
}

int Tclh_UnwrapUuid (Tcl_Interp *interp, Tcl_Obj *objP, Tclh_UUID *uuidP)
{
    if (SetUuidObjFromAny(objP) != TCL_OK) {
        Tcl_SetResult(interp, "Invalid UUID format.", TCL_STATIC);
        return TCL_ERROR;
    }

    memcpy(uuidP, IntrepGetUuid(objP), sizeof(*uuidP));
    return TCL_OK;
}

Tcl_Obj *Tclh_NewUuidObj (Tcl_Interp *ip)
{
    Tcl_Obj *objP;
    Tclh_UUID *uuidP = ckalloc(sizeof(*uuidP));
#ifdef _WIN32
    if (UuidCreate(uuidP) != RPC_S_OK) {
        if (UuidCreateSequential(uuidP) != RPC_S_OK) {
            TCLH_PANIC("Unable to create UUID.");
        }
    }
#else
    uuid_generate(uuidP);
#endif /* _WIN32 */

    objP = Tcl_NewObj();
    Tcl_InvalidateStringRep(objP);
    IntrepSetUuid(objP, uuidP);
    objP->typePtr = &gUuidVtbl;
    return objP;
}

#endif /* TCLH_UUID_IMPL */

#endif /* TCLHWRAP_H */