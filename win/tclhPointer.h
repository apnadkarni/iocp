#ifndef TCLHELPERS_H
#define TCLHELPERS_H

#include "tcl.h"

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
 * Params:
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
 * Params:
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
 * Params:
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

/* Function: Tclh_PointerObjUnregister
 * Unregisters a previously registered pointer passed in as a Tcl_Obj.
 *
 * Params:
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
 * Params:
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
 * Params:
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
 * Params:
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

/* Function: Tclh_PointerWrap
 * Wraps a pointer into a Tcl_Obj.
 * The passed pointer is not registered as a valid pointer, nor is
 * any check made that it was previously registered.
 *
 * Params:
 * pointer  - Pointer value to be wrapped.
 * tag      - Type tag for the pointer. May be 0 or NULL if pointer
 *            is typeless.
 *
 * Returns:
 * Pointer to a Tcl_Obj with reference count 0.
 */
Tcl_Obj *Tclh_PointerWrap(void *pointer, Tclh_TypeTag tag);

/* Function: Tclh_PointerUnwrap
 * Unwraps a Tcl_Obj representing a pointer checking it is of the
 * expected type. No checks are made with respect to its registration.
 *
 * Params:
 * interp - Interpreter in which to store error messages. May be NULL.
 * objP   - Tcl_Obj holding the wrapped pointer value.
 * pointerP - if not NULL, location to store unwrapped pointer.
 * tag    - Type tag for the pointer. May be 0 or NULL if pointer
 *          is typeless.
 *
 * Returns:
 * Pointer to a Tcl_Obj with reference count 0.
 */
int Tclh_PointerUnwrap(Tcl_Interp *interp, Tcl_Obj *objP,
                        void **pointerP, Tclh_TypeTag tag);

/* Function: Tclh_PointerUnwrapAnyOf
 * Unwraps a Tcl_Obj representing a pointer checking it is of several
 * possible types. No checks are made with respect to its registration.
 *
 * Params:
 * interp   - Interpreter in which to store error messages. May be NULL.
 * objP     - Tcl_Obj holding the wrapped pointer value.
 * pointerP - if not NULL, location to store unwrapped pointer.
 * ...      - List of type tags to match against the pointer. Terminated
 *            by a NULL argument.
 *
 * Returns:
 * Pointer to a Tcl_Obj with reference count 0.
 */

int Tclh_PointerUnwrapAnyOf(Tcl_Interp *interp, Tcl_Obj *objP,
                           void **pointerP, ... /* tag, ..., NULL */);

#ifdef TCLH_SHORTNAMES

#define PointerRegister           Tcl_PointerRegister
#define PointerUnregister         Tclh_PointerUnregister
#define PointerVerify             Tclh_PointerVerify
#define PointerObjUnregister      Tclh_PointerObjUnregister
#define PointerObjUnregisterAnyOf Tclh_PointerObjUnregisterAnyOf
#define PointerObjVerify          Tclh_PointerObjVerify
#define PointerObjVerifyAnyOf     Tclh_PointerObjVerifyAnyOf
#define PointerWrap               Tclh_PointerWrap
#define PointerUnwrap             Tclh_PointerUnwrap
#define PointerUnwrapAnyOf        Tclh_PointerUnwrapAnyOf

#endif

/*
 * IMPLEMENTATION
 */

#ifdef TCLH_IMPL

#ifndef TCLH_EMBEDDER
#error TCLH_EMBEDDER not defined. Please #define this to name of your package.
#endif

#undef TCLH_INLINE
#ifdef _MSC_VER
# define TCLH_INLINE __inline
#else
# define TCLH_INLINE static inline
#endif

#ifndef TCLH_PANIC
#define TCLH_PANIC Tcl_Panic
#endif

#ifndef TCLH_ASSERT
# ifdef NDEBUG
#  define TCLH_ASSERT(bool_) (void) 0
# else
#  define TCLH_ASSERT(bool_) (void)( (bool_) || (TCLH_PANIC("Assertion (%s) failed at line %d in file %s.", #bool_, __LINE__, __FILE__), 0) )
# endif
#endif

/*
 * Pointer is a Tcl "type" whose internal representation is stored
 * as the pointer value and an associated C pointer/handle type.
 * The Tcl_Obj.internalRep.twoPtrValue.ptr1 holds the C pointer value
 * and Tcl_Obj.internalRep.twoPtrValue.ptr2 holds a Tcl_Obj describing
 * the type. This may be NULL if no type info needs to be associated
 * with the value
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
TCLH_INLINE Tcl_Obj *PointerTypeGet(Tcl_Obj *objP) {
    return objP->internalRep.twoPtrValue.ptr2;
}
TCLH_INLINE void PointerTypeSet(Tcl_Obj *objP, Tcl_Obj *typeP) {
    objP->internalRep.twoPtrValue.ptr2 = typeP;
}

int Tclh_PointerLibInit(Tcl_Interp *interp)
{
    return TCL_OK;
}

static void TclhCleanupPointerRegistry(ClientData *clientData, Tcl_Interp *interp)
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

Tcl_Obj *Tclh_PointerWrap(void *pointerValue, Tclh_TypeTag tag)
{
    Tcl_Obj *objP;

    objP = Tcl_NewObj();
    Tcl_InvalidateStringRep(objP);
    PointerValueSet(objP, pointerValue);
    if (tag) {
        Tcl_Obj *typeObj = Tcl_NewStringObj(tag, -1);
        Tcl_IncrRefCount(typeObj);
        PointerTypeSet(objP, typeObj);
    } else {
        PointerTypeSet(objP, NULL);
    }
    objP->typePtr = &gPointerType;
    return objP;
}

int Tclh_PointerUnwrap(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, Tclh_TypeTag tag)
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
        Tcl_Obj *typeObj = PointerTypeGet(objP);
        if (typeObj) {
            const char *s = Tcl_GetString(typeObj);
            if (strcmp(tag, s)) {
                if (interp) {
                    Tcl_AppendResult(interp, "Unexpected type '", s, "', expected '",
                                     tag, "'.", NULL);
                }
                return TCL_ERROR;
            }
        }
    }

    *pvP = PointerValueGet(objP);
    return TCL_OK;
}

int Tclh_PointerUnwrapAnyOf(Tcl_Interp *interp, Tcl_Obj *objP, void **pvP, ...)
{
    Tclh_TypeTag tag;
    va_list     args;

    va_start(args, pvP);
    while ((tag = va_arg(args, Tclh_TypeTag)) != NULL) {
        if (PointerUnwrap(NULL, objP, pvP, tag) == TCL_OK) {
            va_end(args);
            return TCL_OK;
        }
    }
    va_end(args);

    if (interp)
        Tcl_SetResult(interp, "Unexpected type.", TCL_STATIC);
    return TCL_ERROR;
}

static void UpdatePointerTypeString(Tcl_Obj *objP)
{
    Tcl_Obj *objs[2];
    Tcl_Obj *listObj;
    const char *s;
    int len;

    TCLH_ASSERT(objP->bytes == NULL);
    TCLH_ASSERT(objP->typePtr == &gPointerType);

    /* Construct a list string representation */
    objs[0] = Tcl_NewWideIntObj((Tcl_WideInt) PointerValueGet(objP));
    objs[1] = PointerTypeGet(objP);
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

static void FreePointerType(Tcl_Obj *objP)
{
    Tcl_Obj *typeObj = PointerTypeGet(objP);
    if (typeObj)
        Tcl_DecrRefCount(typeObj);
    PointerTypeSet(objP, NULL);
    PointerValueSet(objP, NULL);
    objP->typePtr = NULL;
}

static void DupPointerType(Tcl_Obj *srcP, Tcl_Obj *dstP)
{
    Tcl_Obj *typeObj;
    dstP->typePtr = &gPointerType;
    PointerValueSet(dstP, PointerValueGet(srcP));
    typeObj = PointerTypeGet(srcP);
    if (typeObj)
        Tcl_IncrRefCount(typeObj);
    PointerTypeSet(dstP, typeObj);
}

static int SetPointerFromAny(Tcl_Interp *interp, Tcl_Obj *objP)
{
    Tcl_Obj **objs;
    int nobjs;
    void *pv;
    Tcl_Obj *ctype;
    char *s;
    Tcl_WideInt value;

    if (objP->typePtr == &gPointerType)
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
    objP->typePtr = &gPointerType;
    PointerValueSet(objP, pv);
    PointerTypeSet(objP, ctype);

    return TCL_OK;

invalid_value: /* s must point to value */
    if (interp)
        Tcl_AppendResult(interp, "Invalid pointer or opaque value '",
                         Tcl_GetString(objP), "'.", NULL);
    return TCL_ERROR;
}


#endif /* TCLH_IMPL */

#endif /* TCLHELPERS_H */
