/*
 * tclWinIocpUtil.c --
 *
 *	Utility routines used in IOCP implementation.
 *
 * Copyright (c) 2019 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include "tclWinIocp.h"

/*
 * Returns a Tcl_Obj containing the message corresponding to a Windows
 * error code.
 *  error  - the Windows error code
 *  moduleHandle - Handle to the module containing the error string. If NULL,
 *           assumed to be a system message.
 */
Tcl_Obj *Iocp_MapWindowsError(DWORD error, HANDLE moduleHandle)
{
    int   length;
    DWORD flags;
    WCHAR *msgPtr = NULL;
    Tcl_Obj *objPtr;

    flags = moduleHandle ? FORMAT_MESSAGE_FROM_HMODULE : FORMAT_MESSAGE_FROM_SYSTEM;
    flags |=
        FORMAT_MESSAGE_ALLOCATE_BUFFER /* So we do not worry about length */
        | FORMAT_MESSAGE_IGNORE_INSERTS /* Ignore message arguments */
        | FORMAT_MESSAGE_MAX_WIDTH_MASK;/* Ignore soft line breaks */

    length = FormatMessageW(flags, moduleHandle, error,
                            0, /* Lang id */
                            (WCHAR *) &msgPtr,
                            0, NULL);
    if (length > 0) {
        /* Strip trailing CR LF if any */
        if (msgPtr[length-1] == L'\n')
            --length;
        if (length > 0) {
            if (msgPtr[length-1] == L'\r')
                --length;
        }
        objPtr = Tcl_NewUnicodeObj(msgPtr, length);
        LocalFree(msgPtr);
        return objPtr;
    }

    return Tcl_ObjPrintf("Windows error: %ld", error);
}

/*
 * Stores the Windows error message corresponding to winerr
 * in the passed interpreter.
 *
 * Always returns TCL_ERROR (convenient for caller to just return).
 */
IocpResultCode Iocp_ReportWindowsError(Tcl_Interp *interp, DWORD winerr)
{
    if (interp) {
        Tcl_SetObjResult(interp,
                         Iocp_MapWindowsError(winerr, NULL));
    }
    return TCL_ERROR;
}

/*
 * Stores the Windows error message corresponding to GetLastError()
 * in the passed interpreter.
 *
 * Always returns TCL_ERROR (convenient for caller to just return).
 */
IocpResultCode Iocp_ReportLastWindowsError(Tcl_Interp *interp)
{
    return Iocp_ReportWindowsError(interp, GetLastError());
}

/*
 *------------------------------------------------------------------------
 *
 * IocpListAppend --
 *
 *    Appends an element to a list.
 *
 * Side effects:
 *    None
 *
 *------------------------------------------------------------------------
 */
void IocpListAppend(IocpList *listPtr, IocpLink *linkPtr)
{
    if (listPtr->headPtr == NULL) {
        /* Empty list */
        linkPtr->nextPtr = NULL;
        linkPtr->prevPtr = NULL;
        listPtr->headPtr = linkPtr;
        listPtr->tailPtr = linkPtr;
    }
    else {
        linkPtr->nextPtr = NULL;
        linkPtr->prevPtr = listPtr->tailPtr;
        listPtr->tailPtr->nextPtr = linkPtr;
        listPtr->tailPtr = linkPtr;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpListPrepend --
 *
 *    Prepends an element to a list.
 *
 * Side effects:
 *    None
 *
 *------------------------------------------------------------------------
 */
void IocpListPrepend(IocpList *listPtr, IocpLink *linkPtr)
{
    if (listPtr->headPtr == NULL) {
        /* Empty list */
        linkPtr->nextPtr = NULL;
        linkPtr->prevPtr = NULL;
        listPtr->headPtr = linkPtr;
        listPtr->tailPtr = linkPtr;
    }
    else {
        linkPtr->prevPtr = NULL;
        linkPtr->nextPtr = listPtr->headPtr;
        listPtr->headPtr->prevPtr = linkPtr;
        listPtr->headPtr = linkPtr;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpLinkDetach --
 *
 *    Removes an IocpLink from a IocpList it was attached to.
 *
 * Results:
 *    The list pointed to by listPtr is updated.
 *
 * Side effects:
 *
 *------------------------------------------------------------------------
 */
void IocpLinkDetach(IocpList *listPtr, IocpLink *linkPtr)
{
    if (linkPtr->prevPtr == NULL)
        listPtr->headPtr = linkPtr->nextPtr; /* First element */
    else
        linkPtr->prevPtr->nextPtr = linkPtr->nextPtr;
    if (linkPtr->nextPtr == NULL)
        listPtr->tailPtr = linkPtr->prevPtr; /* Last element */
    else
        linkPtr->nextPtr->prevPtr = linkPtr->prevPtr;
}
