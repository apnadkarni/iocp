#include <string.h>
#include "tcl.h"
#include "opaque.h"

#undef INLINE
#ifdef _MSC_VER
# define INLINE __inline
#else
# define INLINE static inline
#endif

#ifndef OPAQUE_ASSERT
# ifdef NDEBUG
#  define OPAQUE_ASSERT(bool_) (void) 0
# else
#  define OPAQUE_ASSERT(bool_) (void)( (bool_) || (Tcl_Panic("Assertion (%s) failed at line %d in file %s.", #bool_, __LINE__, __FILE__), 0) )
# endif
#endif

/*
 * Opaque is a Tcl "type" whose internal representation is stored
 * as the pointer value and an associated C pointer/handle type.
 * The Tcl_Obj.internalRep.twoPtrValue.ptr1 holds the C pointer value
 * and Tcl_Obj.internalRep.twoPtrValue.ptr2 holds a Tcl_Obj describing
 * the type. This may be NULL if no type info needs to be associated
 * with the value
 */
static void DupOpaqueType(Tcl_Obj *srcP, Tcl_Obj *dstP);
static void FreeOpaqueType(Tcl_Obj *objP);
static void UpdateOpaqueTypeString(Tcl_Obj *objP);
static int  SetOpaqueFromAny(Tcl_Interp *interp, Tcl_Obj *objP);

static struct Tcl_ObjType gOpaqueType = {
    "Opaque",
    FreeOpaqueType,
    DupOpaqueType,
    UpdateOpaqueTypeString,
    NULL,
};
INLINE void* OpaqueValueGet(Tcl_Obj *objP) {
    return objP->internalRep.twoPtrValue.ptr1;
}
INLINE void OpaqueValueSet(Tcl_Obj *objP, void *valueP) {
    objP->internalRep.twoPtrValue.ptr1 = valueP;
}
/* May return NULL */
INLINE Tcl_Obj *OpaqueTypeGet(Tcl_Obj *objP) {
    return objP->internalRep.twoPtrValue.ptr2;
}
INLINE void OpaqueTypeSet(Tcl_Obj *objP, Tcl_Obj *typeP) {
    objP->internalRep.twoPtrValue.ptr2 = typeP;
}

/* Returns an Tcl_Obj with ref count 0 */
Tcl_Obj *ObjFromOpaque(void *pointerValue, char *typeName)
{
    Tcl_Obj *objP;

    objP = Tcl_NewObj();
    Tcl_InvalidateStringRep(objP);
    OpaqueValueSet(objP, pointerValue);
    if (typeName) {
        Tcl_Obj *typeObj = Tcl_NewStringObj(typeName, -1);
        Tcl_IncrRefCount(typeObj);
        OpaqueTypeSet(objP, typeObj);
    } else {
        OpaqueTypeSet(objP, NULL);
    }
    objP->typePtr = &gOpaqueType;
    return objP;
}

int ObjToOpaque(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, const char *name)
{
    /* Try converting Tcl_Obj internal rep */
    if (objP->typePtr != &gOpaqueType) {
        if (SetOpaqueFromAny(interp, objP) != TCL_OK)
            return TCL_ERROR;
    }

    /* We need to check types only if both object type and caller specified
       type are not void */
    if (name && name[0] == 0)
        name = NULL;
    if (name) {
        Tcl_Obj *typeObj = OpaqueTypeGet(objP);
        if (typeObj) {
            const char *s = Tcl_GetString(typeObj);
            if (strcmp(name, s)) {
                if (interp) {
                    Tcl_AppendResult(interp, "Unexpected type '", s, "', expected '",
                                     name, "'.", NULL);
                }
                return TCL_ERROR;
            }
        }
    }

    *pvP = OpaqueValueGet(objP);
    return TCL_OK;
}

int ObjToOpaqueAny(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, ...)
{
    const char *name;
    va_list     args;

    va_start(args, pvP);
    while ((name = va_arg(args, const char *)) != NULL) {
        if (ObjToOpaque(NULL, objP, pvP, name) == TCL_OK) {
            va_end(args);
            return TCL_OK;
        }
    }
    va_end(args);

    if (interp)
        Tcl_SetResult(interp, "Unexpected type.", TCL_STATIC);
    return TCL_ERROR;
}

static void UpdateOpaqueTypeString(Tcl_Obj *objP)
{
    Tcl_Obj *objs[2];
    Tcl_Obj *listObj;
    const char *s;
    int len;

    OPAQUE_ASSERT(objP->bytes == NULL);
    OPAQUE_ASSERT(objP->typePtr == &gOpaqueType);

    /* Construct a list string representation */
    objs[0] = Tcl_NewWideIntObj((Tcl_WideInt) OpaqueValueGet(objP));
    objs[1] = OpaqueTypeGet(objP);
    if (objs[1] == NULL)
        objs[1] = Tcl_NewObj();

    listObj = Tcl_NewListObj(2, objs);

    /* We could just shift the bytes field from listObj to objP resetting
       the former to NULL. But I'm nervous about doing that behind Tcl's back */
    s = Tcl_GetStringFromObj(listObj, &len);
    objP->length = len; /* Note does not include terminating \0 */
    objP->bytes = ckalloc(len+1);
    memcpy(objP->bytes, s, len+1);
    Tcl_DecrRefCount(listObj);
}

static void FreeOpaqueType(Tcl_Obj *objP)
{
    Tcl_Obj *typeObj = OpaqueTypeGet(objP);
    if (typeObj)
        Tcl_DecrRefCount(typeObj);
    OpaqueTypeSet(objP, NULL);
    OpaqueValueSet(objP, NULL);
    objP->typePtr = NULL;
}

static void DupOpaqueType(Tcl_Obj *srcP, Tcl_Obj *dstP)
{
    Tcl_Obj *typeObj;
    dstP->typePtr = &gOpaqueType;
    OpaqueValueSet(dstP, OpaqueValueGet(srcP));
    typeObj = OpaqueTypeGet(srcP);
    if (typeObj)
        Tcl_IncrRefCount(typeObj);
    OpaqueTypeSet(dstP, typeObj);
}

int SetOpaqueFromAny(Tcl_Interp *interp, Tcl_Obj *objP)
{
    Tcl_Obj **objs;
    int nobjs;
    void *pv;
    Tcl_Obj *ctype;
    char *s;
    Tcl_WideInt value;

    if (objP->typePtr == &gOpaqueType)
        return TCL_OK;

    /* Should be a two element list */
    if (Tcl_ListObjGetElements(NULL, objP, &nobjs, &objs) != TCL_OK ||
        nobjs != 2)
        goto invalid_value;
    if (Tcl_GetWideIntFromObj(NULL, objs[0], &value) != TCL_OK)
            goto invalid_value;
    pv = (void*) value;
    s = Tcl_GetString(objs[1]);
    if (s[0] == 0)
        ctype = NULL;
    else {
        ctype = objs[1];
        Tcl_IncrRefCount(ctype);
    }

    /* OK, valid opaque rep. Convert the passed object's internal rep */
    if (objP->typePtr && objP->typePtr->freeIntRepProc) {
        objP->typePtr->freeIntRepProc(objP);
    }
    objP->typePtr = &gOpaqueType;
    OpaqueValueSet(objP, pv);
    OpaqueTypeSet(objP, ctype);

    return TCL_OK;

invalid_value: /* s must point to value */
    if (interp)
        Tcl_AppendResult(interp, "Invalid pointer or opaque value '",
                         Tcl_GetString(objP), "'.", NULL);
    return TCL_ERROR;
}
