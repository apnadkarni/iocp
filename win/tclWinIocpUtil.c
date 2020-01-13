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
 * Support for one-time initialization. The function Iocp_DoOnce
 * can be called to execute any function that should be run exactly one
 * time within a process. It is thread safe and when called from multiple
 * threads (concurrently even) will execute the passed function
 * exactly once while also blocking other threads calling it until the
 * function completes.
 *
 * Parameters:
 *   stateP     - pointer to global variable of opaque type IocpOneTimeInitState
 *                and associated with the initialization function (*once_fn)
 *   once_fn    - the initialization function to call. This should take one
 *                parameter (passed clientdata) and return TCL_OK if successful
 *                and TCL_ERROR on failure. Note any failures will also cause
 *                further calls to fail.
 *   clientdata - the value to pass on to (*once_fn)
 *
 * Return value:
 *    TCL_OK    - initialization done
 *    TCL_ERROR - initialization failed
 */
IocpResultCode Iocp_DoOnce(Iocp_DoOnceState *stateP, Iocp_DoOnceProc *once_fn, ClientData clientdata)
{
    Iocp_DoOnceState prev_state;
    enum {
        IOCP_INITSTATE_INIT = 0, /* Must be 0 corresponding globals initialization */
        IOCP_INITSTATE_IN_PROGRESS,
        IOCP_INITSTATE_DONE,
        IOCP_INITSTATE_ERROR,
    };

    /* Init unless already done. */
    switch (InterlockedCompareExchange(stateP,
                                       IOCP_INITSTATE_IN_PROGRESS,
                                       IOCP_INITSTATE_INIT)) {
    case IOCP_INITSTATE_DONE:
        return TCL_OK;               /* Already done */

    case IOCP_INITSTATE_IN_PROGRESS:
        /* Loop waiting for initialization to be done in other thread */
        while (1) {
            prev_state = InterlockedCompareExchange(stateP,
                                                    IOCP_INITSTATE_IN_PROGRESS,
                                                    IOCP_INITSTATE_IN_PROGRESS);
            if (prev_state == IOCP_INITSTATE_DONE)
                return TCL_OK;       /* Done after waiting */

            if (prev_state != IOCP_INITSTATE_IN_PROGRESS)
                break; /* Error but do not know what - someone else was
                             initializing */

            /*
             * Someone is initializing, wait in a spin
             * Note the Sleep() will yield to other threads, including
             * the one doing the init, so this is not a hard loop
             */
            Sleep(1);
        }
        break;

    case IOCP_INITSTATE_INIT:
        /* We need to do the init */
        if (once_fn(clientdata) == TCL_OK) {
            InterlockedExchange(stateP, IOCP_INITSTATE_DONE);
            return TCL_OK;               /* We init'ed successfully */
        }
        InterlockedExchange(stateP, IOCP_INITSTATE_ERROR);
        break;

    case IOCP_INITSTATE_ERROR:
        /* State was already in error. No way to recover safely */
        break;
    }

    return TCL_ERROR; /* Failed either in this thread or another */
}

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
 * IocpSetTclErrnoFromWin32 --
 *
 *    Sets Tcl's errno value based on a Win32 error code.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Sets Tcl's internal errno value.
 *
 *------------------------------------------------------------------------
 */
void IocpSetTclErrnoFromWin32(DWORD winError)
{
    IOCP_TCL85_INTERNAL_PLATFORM_STUB(tclWinConvertError)(winError);
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
 * IocpListRemove --
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
void IocpListRemove(IocpList *listPtr, IocpLink *linkPtr)
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
