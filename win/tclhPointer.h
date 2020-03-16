#ifndef TCLHPOINTER_H
#define TCLHPOINTER_H

#include "tcl.h"
#include <string.h>

typedef const char *Tclh_TypeTag;

/* Section: Registered Pointers
 *
 * Provides a facility for safely passing pointers, Windows handles etc. to the
 * Tcl script level. The intent of pointer registration is to make use of
 * pointers passed to script level more robust by preventing errors such as use
 * after free, the wrong handle type etc. Each pointer is also optionally typed
 * with a tag and verification can check not only that the pointer is registered
 * but has the right type tag.
 *
 * Pointers can be registered as valid with <Tclh_PointerRegister> before being
 * passed up to the script. When passed in from a script, their validity can be
 * checked with <Tclh_PointerVerify>. Pointers should be marked invalid as
 * appropriate by unregistering them with <Tclh_PointerUnregister>.
 *
 * Pointer Type Tags:
 *
 * Pointers are optionally associated with a type using a type tag
 * so that when checking arguments, the pointer type can be checked as
 * well. The tag is a pointer to a string which is interpreted as
 * the name of the type. It is up to the application or extension to
 * ensure there are no conflicts between type names. Type name do not need
 * to correspond to the actual C type. For example, strdup and getpath
 * both might return char* C types but since they are not the same
 * type in terms of the content and how they are to be freed, the
 * type tags used might be "MALLOCEDPTR" and "PATHPTR".
 *
 * A type tag of NULL or 0 is treated as a type of void* and no
 * type checking is done in that case. Other than this, type tags
 * are only used for comparison and have no semantic meaning.
 */

/* Function: Tclh_PointerLibInit
 * Must be called to initialize the Tcl Helper library before any of
 * the other functions in the library.
 *
 * Parameters:
 * interp - Tcl interpreter in which to initialize.
 *
 * Returns:
 * TCL_OK    - Library was successfully initialized.
 * TCL_ERROR - Initialization failed. Library functions must not be called.
 *             An error message is left in the interpreter result.
 */
int Tclh_PointerLibInit(Tcl_Interp *interp);


/* Function: Tclh_PointerRegister
 * Registers a pointer value as being "valid".
 *
 * The validity of a pointer can then be tested by with
 * <Tclh_PointerVerify> and reversed by calling <Tclh_PointerUnregister>.
 *
 * Parameters:
 * interp  - Tcl interpreter in which the pointer is to be registered.
 * pointer - Pointer value to be registered.
 * tag     - Type tag for the pointer. Pass NULL or 0 for typeless pointers.
 * objPP   - if not NULL, a pointer to a new Tcl_Obj holding the pointer
 *           representation is stored here on success. The Tcl_Obj has
 *           a reference count of 0.
 *
 * Returns:
 * TCL_OK on success else TCL_ERROR with an error message in interp.
 */
int Tclh_PointerRegister(Tcl_Interp *interp, void *pointer,
                         Tclh_TypeTag tag, Tcl_Obj **objPP);

/* Function: Tclh_PointerUnregister
 * Unregisters a previously registered pointer.
 *
 * Parameters:
 * interp   - Tcl interpreter in which the pointer is to be unregistered.
 * pointer  - Pointer value to be unregistered.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *          type is not to be checked.
 *
 * Returns:
 * TCL_OK    - The pointer was successfully unregistered.
 * TCL_ERROR - The pointer was not registered or was registered with a
 *             different type. An error message is left in interp.
 */
int Tclh_PointerUnregister(Tcl_Interp *interp, const void *pointer, Tclh_TypeTag tag);


/* Function: Tclh_PointerVerify
 * Verifies that the passed pointer is registered as a valid pointer
 * of a given type.
 *
 * interp   - Tcl interpreter in which the pointer is to be verified.
 * pointer  - Pointer value to be verified.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *            type is not to be checked.
 *
 * Returns:
 * TCL_OK    - The pointer is registered with the same type tag.
 * TCL_ERROR - Pointer is unregistered or a different type. An error message
 *             is stored in interp.
 */
int Tclh_PointerVerify(Tcl_Interp *interp, const void *voidP, Tclh_TypeTag tag);

/* Function: Tclh_UnwrapPointerTag
 * Returns the pointer type tag for a Tcl_Obj pointer wrapper.
 *
 * Parameters:
 * interp - Interpreter to store errors if any. May be NULL.
 * objP   - Tcl_Obj holding the wrapped pointer.
 * tagPtr - Location to store the type tag.
 *
 * Returns:
 * TCL_OK    - objP holds a valid pointer. *tagPtr will hold the type tag.
 * TCL_ERROR - objP is not a wrapped pointer. interp, if not NULL, will hold
 *             error message.
 */
int Tclh_UnwrapPointerTag(Tcl_Interp *interp, Tcl_Obj *objP, Tclh_TypeTag *tagPtr);

/* Function: Tclh_PointerObjUnregister
 * Unregisters a previously registered pointer passed in as a Tcl_Obj.
 *
 * Parameters:
 * interp   - Tcl interpreter in which the pointer is to be unregistered.
 * objP     - Tcl_Obj containing a pointer value to be unregistered.
 * pointerP - If not NULL, the pointer value from objP is stored here
 *            on success.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *            type is not to be checked.
 *
 * Returns:
 * TCL_OK    - The pointer was successfully unregistered.
 * TCL_ERROR - The pointer was not registered or was registered with a
 *             different type. An error message is left in interp.
 */
int Tclh_PointerObjUnregister(Tcl_Interp *interp, Tcl_Obj *objP,
                              void **pointerP, Tclh_TypeTag tag);

/* Function: Tclh_PointerObjUnregisterAnyOf
 * Unregisters a previously registered pointer passed in as a Tcl_Obj
 * after checking it is one of the specified types.
 *
 * Parameters:
 * interp   - Tcl interpreter in which the pointer is to be unregistered.
 * objP     - Tcl_Obj containing a pointer value to be unregistered.
 * pointerP - If not NULL, the pointer value from objP is stored here
 *            on success.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *            type is not to be checked.
 *
 * Returns:
 * TCL_OK    - The pointer was successfully unregistered.
 * TCL_ERROR - The pointer was not registered or was registered with a
 *             different type. An error message is left in interp.
 */
int Tclh_PointerObjUnregisterAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                                   void **pointerP, ... /* tag, ... , NULL */);

/* Function: Tclh_PointerObjVerify
 * Verifies a Tcl_Obj contains a wrapped pointer that is registered
 * and, optionally, of a specified type.
 *
 * Parameters:
 * interp   - Tcl interpreter in which the pointer is to be verified.
 * objP     - Tcl_Obj containing a pointer value to be verified.
 * pointerP - If not NULL, the pointer value from objP is stored here
 *            on success.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *            type is not to be checked.
 *
 * Returns:
 * TCL_OK    - The pointer was successfully verified.
 * TCL_ERROR - The pointer was not registered or was registered with a
 *             different type. An error message is left in interp.
 */
int Tclh_PointerObjVerify(Tcl_Interp *interp, Tcl_Obj *objP,
                          void **pointerP, Tclh_TypeTag tag);

/* Function: Tclh_PointerObjVerifyAnyOf
 * Verifies a Tcl_Obj contains a wrapped pointer that is registered
 * and one of several allowed types.
 *
 * Parameters:
 * interp   - Tcl interpreter in which the pointer is to be verified.
 * objP     - Tcl_Obj containing a pointer value to be verified.
 * pointerP - If not NULL, the pointer value from objP is stored here
 *            on success.
 * ...      - Remaining arguments are type tags terminated by a NULL argument.
 *            The pointer must be one of these types.
 *
 * Returns:
 * TCL_OK    - The pointer was successfully verified.
 * TCL_ERROR - The pointer was not registered or was registered with a
 *             type that is not one of the passed ones.
 *             An error message is left in interp.
 */
int Tclh_PointerObjVerifyAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                          void **pointerP, ... /* tag, tag, NULL */);

/* Function: Tclh_WrapPointer
 * Wraps a pointer into a Tcl_Obj.
 * The passed pointer is not registered as a valid pointer, nor is
 * any check made that it was previously registered.
 *
 * Parameters:
 * pointer  - Pointer value to be wrapped.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *            is typeless.
 *
 * Returns:
 * Pointer to a Tcl_Obj with reference count 0.
 */
Tcl_Obj *Tclh_WrapPointer(void *pointer, Tclh_TypeTag tag);

/* Function: Tclh_UnwrapPointer
 * Unwraps a Tcl_Obj representing a pointer checking it is of the
 * expected type. No checks are made with respect to its registration.
 *
 * Parameters:
 * interp - Interpreter in which to store error messages. May be NULL.
 * objP   - Tcl_Obj holding the wrapped pointer value.
 * pointerP - if not NULL, location to store unwrapped pointer.
 * tag    - Type tag for the pointer. May be 0 or NULL if pointer
 *          is typeless.
 *
 * Returns:
 * TCL_OK    - Success, with the unwrapped pointer stored in *pointerP.
 * TCL_ERROR - Failure, with interp containing error message.
 */
int Tclh_UnwrapPointer(Tcl_Interp *interp, Tcl_Obj *objP,
                        void **pointerP, Tclh_TypeTag tag);

/* Function: Tclh_UnwrapPointerAnyOf
 * Unwraps a Tcl_Obj representing a pointer checking it is of several
 * possible types. No checks are made with respect to its registration.
 *
 * Parameters:
 * interp   - Interpreter in which to store error messages. May be NULL.
 * objP     - Tcl_Obj holding the wrapped pointer value.
 * pointerP - if not NULL, location to store unwrapped pointer.
 * ...      - List of type tags to match against the pointer. Terminated
 *            by a NULL argument.
 *
 * Returns:
 * Pointer to a Tcl_Obj with reference count 0.
 */

int Tclh_UnwrapPointerAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                           void **pointerP, ... /* tag, ..., NULL */);

#ifdef TCLH_SHORTNAMES

#define PointerRegister           Tclh_PointerRegister
#define PointerUnregister         Tclh_PointerUnregister
#define PointerVerify             Tclh_PointerVerify
#define PointerObjUnregister      Tclh_PointerObjUnregister
#define PointerObjUnregisterAnyOf Tclh_PointerObjUnregisterAnyOf
#define PointerObjVerify          Tclh_PointerObjVerify
#define PointerObjVerifyAnyOf     Tclh_PointerObjVerifyAnyOf
#define WrapPointer               Tclh_WrapPointer
#define UnwrapPointer             Tclh_UnwrapPointer
#define UnwrapPointerTag          Tclh_UnwrapPointerTag
#define UnwrapPointerAnyOf        Tclh_UnwrapPointerAnyOf

#endif

/*
 * IMPLEMENTATION
 */

#ifdef TCLH_IMPL
# define TCLH_POINTER_IMPL
#endif
#ifdef TCLH_POINTER_IMPL
#include "tclhBase.h"

/*
 * Pointer is a Tcl "type" whose internal representation is stored
 * as the pointer value and an associated C pointer/handle type.
 * The Tcl_Obj.internalRep.twoPtrValue.ptr1 holds the C pointer value
 * and Tcl_Obj.internalRep.twoPtrValue.ptr2 holds a Tcl_Obj describing
 * the type. This may be NULL if no type info needs to be associated
 * with the value.
 */
static void DupPointerType(Tcl_Obj *srcP, Tcl_Obj *dstP);
static void FreePointerType(Tcl_Obj *objP);
static void UpdatePointerTypeString(Tcl_Obj *objP);
static int  SetPointerFromAny(Tcl_Interp *interp, Tcl_Obj *objP);

static struct Tcl_ObjType gPointerType = {
    "Pointer",
    FreePointerType,
    DupPointerType,
    UpdatePointerTypeString,
    NULL,
};
TCLH_INLINE void* PointerValueGet(Tcl_Obj *objP) {
    return objP->internalRep.twoPtrValue.ptr1;
}
TCLH_INLINE void PointerValueSet(Tcl_Obj *objP, void *valueP) {
    objP->internalRep.twoPtrValue.ptr1 = valueP;
}
/* May return NULL */
TCLH_INLINE Tclh_TypeTag PointerTypeGet(Tcl_Obj *objP) {
    return objP->internalRep.twoPtrValue.ptr2;
}
TCLH_INLINE void PointerTypeSet(Tcl_Obj *objP, Tclh_TypeTag tag) {
    objP->internalRep.twoPtrValue.ptr2 = (void*)tag;
}
TCLH_INLINE int PointerTypeSame(Tclh_TypeTag taga, Tclh_TypeTag tagb) {
    return strcmp(taga, tagb) == 0;
}

int Tclh_PointerLibInit(Tcl_Interp *interp)
{
    return TCL_OK;
}

Tcl_Obj *Tclh_WrapPointer(void *pointerValue, Tclh_TypeTag tag)
{
    Tcl_Obj *objP;

    objP = Tcl_NewObj();
    Tcl_InvalidateStringRep(objP);
    PointerValueSet(objP, pointerValue);
    if (tag) {
        int len = Tclh_strlen(tag) + 1;
        char *tag2 = ckalloc(len);
        memcpy(tag2, tag, len);
        PointerTypeSet(objP, tag2);
    } else {
        PointerTypeSet(objP, NULL);
    }
    objP->typePtr = &gPointerType;
    return objP;
}

int Tclh_UnwrapPointer(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, Tclh_TypeTag tag)
{
    /* Try converting Tcl_Obj internal rep */
    if (objP->typePtr != &gPointerType) {
        if (SetPointerFromAny(interp, objP) != TCL_OK)
            return TCL_ERROR;
    }

    /* We need to check types only if both object type and caller specified
       type are not void */
    if (tag && tag[0] == 0)
        tag = NULL;
    if (tag) {
        Tclh_TypeTag tag2 = PointerTypeGet(objP);
        if (tag2) {
            if (!PointerTypeSame(tag, tag2)) {
                if (interp) {
                    Tcl_AppendResult(interp, "Unexpected type '", tag2, "', expected '",
                                     tag, "'.", NULL);
                }
                return TCL_ERROR;
            }
        }
    }

    *pvP = PointerValueGet(objP);
    return TCL_OK;
}

int Tclh_UnwrapPointerTag(Tcl_Interp *interp, Tcl_Obj *objP, Tclh_TypeTag *tagPtr)
{
    /* Try converting Tcl_Obj internal rep */
    if (objP->typePtr != &gPointerType) {
        if (SetPointerFromAny(interp, objP) != TCL_OK)
            return TCL_ERROR;
    }
    *tagPtr = PointerTypeGet(objP);
    return TCL_OK;
}

int UnwrapPointerAnyOfVA(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, va_list args)
{
    Tclh_TypeTag tag;

    while ((tag = va_arg(args, Tclh_TypeTag)) != NULL) {
        if (Tclh_UnwrapPointer(NULL, objP, pvP, tag) == TCL_OK) {
            return TCL_OK;
        }
    }

    if (interp)
        Tcl_SetResult(interp, "Unexpected type.", TCL_STATIC);
    return TCL_ERROR;
}

int Tclh_UnwrapPointerAnyOf(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, ...)
{
    int     tclResult;
    va_list args;

    va_start(args, pvP);
    tclResult = UnwrapPointerAnyOfVA(interp, objP, pvP, args);
    va_end(args);
    return tclResult;
}

static void UpdatePointerTypeString(Tcl_Obj *objP)
{
    Tcl_Obj *objs[2];
    Tcl_Obj *listObj;
    const char *s;
    Tclh_TypeTag tag;
    int len;

    TCLH_ASSERT(objP->bytes == NULL);
    TCLH_ASSERT(objP->typePtr == &gPointerType);

    /* TBD - change this to use DString? */

    /* Construct a list string representation */
    objs[0] = Tcl_NewWideIntObj((Tcl_WideInt) PointerValueGet(objP));
    tag = PointerTypeGet(objP);
    if (tag)
        objs[1] = Tcl_NewStringObj(tag, -1);
    else
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

static void FreePointerType(Tcl_Obj *objP)
{
    Tclh_TypeTag tag = PointerTypeGet(objP);
    if (tag)
        ckfree(tag);
    PointerTypeSet(objP, NULL);
    PointerValueSet(objP, NULL);
    objP->typePtr = NULL;
}

static void DupPointerType(Tcl_Obj *srcP, Tcl_Obj *dstP)
{
    Tclh_TypeTag tag;

    dstP->typePtr = &gPointerType;
    PointerValueSet(dstP, PointerValueGet(srcP));
    tag = PointerTypeGet(srcP);
    if (tag) {
        int len = Tclh_strlen(tag) + 1;
        char *tag2 = ckalloc(len);
        memcpy(tag2, tag, len);
        PointerTypeSet(dstP, tag2);
    } else
        PointerTypeSet(dstP, NULL);
}

static int SetPointerFromAny(Tcl_Interp *interp, Tcl_Obj *objP)
{
    Tcl_Obj     **objs;
    int           nobjs;
    void         *pv;
    char         *tag;
    char         *s;
    Tcl_WideInt   value;
    int           len;

    if (objP->typePtr == &gPointerType)
        return TCL_OK;

    /* Should be a two element list */
    if (Tcl_ListObjGetElements(NULL, objP, &nobjs, &objs) != TCL_OK ||
        nobjs != 2)
        goto invalid_value;
    if (Tcl_GetWideIntFromObj(NULL, objs[0], &value) != TCL_OK)
            goto invalid_value;
    pv = (void*) value;
    s = Tcl_GetStringFromObj(objs[1], &len);
    if (len == 0)
        tag = NULL;
    else {
        tag = ckalloc(len+1);
        memcpy((void *)tag, s, len+1);
    }

    /* OK, valid opaque rep. Convert the passed object's internal rep */
    if (objP->typePtr && objP->typePtr->freeIntRepProc) {
        objP->typePtr->freeIntRepProc(objP);
    }
    objP->typePtr = &gPointerType;
    PointerValueSet(objP, pv);
    PointerTypeSet(objP, tag);

    return TCL_OK;

invalid_value: /* s must point to value */
    if (interp)
        Tcl_AppendResult(interp, "Invalid pointer or opaque value '",
                         Tcl_GetString(objP), "'.", NULL);
    return TCL_ERROR;
}

/*
 * Pointer registry implementation.
 */

static int PointerTypeError(
    Tcl_Interp *interp,
    Tclh_TypeTag registered,
    Tclh_TypeTag tag)
{
    if (interp) {
        Tcl_SetObjResult(interp,
                         Tcl_ObjPrintf("Pointer type mismatch. Current type %s, registered type %s.",
                                       tag, registered));
    }
    return TCL_ERROR;
}

int PointerNotRegisteredError(Tcl_Interp *interp, const void *p, Tclh_TypeTag tag) {
    /* Tcl_ObjPrintf does not support wide ints or pointers so sprintf */
    if (interp) {
        char buf[100];
        snprintf(buf, sizeof(buf), "Pointer %p of type %s is not registered.", p, tag);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));
    }
    return TCL_ERROR;
}

static void TclhCleanupPointerRegistry(ClientData clientData, Tcl_Interp *interp)
{
    Tcl_HashTable *hTblPtr = (Tcl_HashTable *)clientData;

#if 0
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch hSearch;
    /*
     * Really, nothing to clean up in the hash entries as of now as
     * nothing is allocated except the entries themselves which are
     * taken care of by Tcl_DeleteHashTable below
     */
    for (hPtr = Tcl_FirstHashEntry(hTblPtr, &hSearch);
         hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
    }
#endif

    Tcl_DeleteHashTable(hTblPtr);
    ckfree(hTblPtr);
}

static Tcl_HashTable *TclhInitPointerRegistry(Tcl_Interp *interp)
{
    Tcl_HashTable *hTblPtr;
    static const char *const pointerTableKey = TCLH_EMBEDDER "PointerTable";
    hTblPtr = Tcl_GetAssocData(interp, pointerTableKey, NULL);
    if (hTblPtr == NULL) {
	hTblPtr = ckalloc(sizeof(Tcl_HashTable));
	Tcl_InitHashTable(hTblPtr, TCL_ONE_WORD_KEYS);
	Tcl_SetAssocData(interp, pointerTableKey, TclhCleanupPointerRegistry, hTblPtr);
    }
    return hTblPtr;
}

int Tclh_PointerRegister(Tcl_Interp *interp, void *pointer,
                         Tclh_TypeTag tag, Tcl_Obj **objPP)
{
    Tcl_HashTable *hTblPtr;
    Tcl_HashEntry *he;
    int            newEntry;

    if (pointer == NULL) {
        Tcl_SetResult(interp, "Attempt to register null pointer", TCL_STATIC);
    }

    hTblPtr = TclhInitPointerRegistry(interp);
    he = Tcl_CreateHashEntry(hTblPtr, pointer, &newEntry);

    if (he) {
        if (newEntry) {
            Tcl_SetHashValue(he, tag);
            if (objPP) {
                *objPP = Tclh_WrapPointer(pointer, tag);
            }
            return TCL_OK;
        } else {
            Tcl_SetResult(interp, "Pointer is already registered.", TCL_STATIC);
        }
    } else {
        Tcl_Panic("Failed to allocate hash table entry");
    }
    return TCL_ERROR;
}

static int PointerVerifyOrUnregister(Tcl_Interp *interp,
                                     const void *pointer,
                                     Tclh_TypeTag tag,
                                     int delete)
{
    Tcl_HashTable *hTblPtr;
    Tcl_HashEntry *he;

    hTblPtr = TclhInitPointerRegistry(interp);
    he = Tcl_FindHashEntry(hTblPtr, pointer);
    if (he) {
        /* If type tag specified, need to check it. */
        if (tag != NULL) {
            Tclh_TypeTag heTag = Tcl_GetHashValue(he);
            if (!PointerTypeSame(heTag, tag)) {
                return PointerTypeError(interp, heTag, tag);
            }
        }
        if (delete)
            Tcl_DeleteHashEntry(he);
        return TCL_OK;
    }
    return PointerNotRegisteredError(interp, pointer, tag);
}

int Tclh_PointerUnregister(Tcl_Interp *interp, const void *pointer, Tclh_TypeTag tag)
{
    return PointerVerifyOrUnregister(interp, pointer, tag, 1);
}

int Tclh_PointerVerify(Tcl_Interp *interp, const void *pointer, Tclh_TypeTag tag)
{
    return PointerVerifyOrUnregister(interp, pointer, tag, 0);
}

int Tclh_PointerObjUnregister(Tcl_Interp *interp, Tcl_Obj *objP,
                              void **pointerP, Tclh_TypeTag tag)
{
    void *pv;
    int   tclResult;

    tclResult = Tclh_UnwrapPointer(interp, objP, &pv, tag);
    if (tclResult == TCL_OK) {
        tclResult = Tclh_PointerUnregister(interp, pv, tag);
        if (tclResult == TCL_OK) {
            if (pointerP)
                *pointerP = pv;
        }
    }
    return tclResult;
}

static int PointerObjAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                           void **pointerP, int unregister, va_list args)
{
    int tclResult;
    void *pv;
    Tclh_TypeTag tag;

    tclResult = UnwrapPointerAnyOfVA(interp, objP, &pv, args);
    if (tclResult == TCL_OK) {
        tclResult = Tclh_UnwrapPointerTag(interp, objP, &tag);
        if (tclResult == TCL_OK) {
            if (unregister)
                tclResult = Tclh_PointerUnregister(interp, pv, tag);
            else
                tclResult = Tclh_PointerVerify(interp, pv, tag);
            if (tclResult == TCL_OK) {
                if (pointerP)
                    *pointerP = pv;
                return TCL_OK;
            }
        }
    }
    return tclResult;
}

int Tclh_PointerObjUnregisterAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                                   void **pointerP, ... /* tag, ... , NULL */)
{
    int tclResult;
    va_list args;

    va_start(args, pointerP);
    tclResult = PointerObjAnyOf(interp, objP, pointerP, 1, args);
    va_end(args);
    return tclResult;
}

int Tclh_PointerObjVerify(Tcl_Interp *interp, Tcl_Obj *objP,
                          void **pointerP, Tclh_TypeTag tag)
{
    void *pv;
    int   tclResult;

    tclResult = Tclh_UnwrapPointer(interp, objP, &pv, tag);
    if (tclResult == TCL_OK) {
        tclResult = Tclh_PointerVerify(interp, pv, tag);
        if (tclResult == TCL_OK) {
            if (pointerP)
                *pointerP = pv;
        }
    }
    return tclResult;
}

int Tclh_PointerObjVerifyAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                               void **pointerP, ... /* tag, tag, NULL */)
{
    int tclResult;
    va_list args;

    va_start(args, pointerP);
    tclResult = PointerObjAnyOf(interp, objP, pointerP, 0, args);
    va_end(args);
    return tclResult;
}

#endif /* TCLH_IMPL */

#endif /* TCLHPOINTER_H */
