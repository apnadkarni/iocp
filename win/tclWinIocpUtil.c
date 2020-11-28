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
#define TCLH_IMPL
#define TCLH_EMBEDDER "iocp"
#include "tclWinIocp.h"
#include <stdarg.h>

/* Used to name the interp-associated hash table for accept callback scripts */
static const char *iocpAcceptCallbackHashName = "iocpAcceptCallbacks";

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
IocpTclCode Iocp_DoOnce(Iocp_DoOnceState *stateP, Iocp_DoOnceProc *once_fn, ClientData clientdata)
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
Tcl_Obj *Iocp_MapWindowsError(
    DWORD winError,             /* Windows error code */
    HANDLE moduleHandle,        /* Handle to module containing error string.
                                 * If NULL, assumed to be system message. */
    const char *msgPtr)         /* Message prefix. May be NULL. */
{
    int   length;
    DWORD flags;
    WCHAR *winErrorMessagePtr = NULL;
    Tcl_Obj *objPtr;

    objPtr = Tcl_NewStringObj(msgPtr ? msgPtr : "", -1);

    flags = moduleHandle ? FORMAT_MESSAGE_FROM_HMODULE : FORMAT_MESSAGE_FROM_SYSTEM;
    flags |=
        FORMAT_MESSAGE_ALLOCATE_BUFFER /* So we do not worry about length */
        | FORMAT_MESSAGE_IGNORE_INSERTS /* Ignore message arguments */
        | FORMAT_MESSAGE_MAX_WIDTH_MASK;/* Ignore soft line breaks */

    length = FormatMessageW(flags, moduleHandle, winError,
                            0, /* Lang id */
                            (WCHAR *) &winErrorMessagePtr,
                            0, NULL);
    if (length > 0) {
        /* Strip trailing CR LF if any */
        if (winErrorMessagePtr[length-1] == L'\n')
            --length;
        if (length > 0) {
            if (winErrorMessagePtr[length-1] == L'\r')
                --length;
        }
        Tcl_AppendUnicodeToObj(objPtr, winErrorMessagePtr, length);
        LocalFree(winErrorMessagePtr);
    } else {
        Tcl_AppendPrintfToObj(objPtr, "Windows error code %ld", winError);
    }
    return objPtr;
}

/*
 * Stores the Windows error message corresponding to winerr
 * in the passed interpreter.
 *
 * Always returns TCL_ERROR (convenient for caller to just return).
 */
IocpTclCode Iocp_ReportWindowsError(
    Tcl_Interp *interp,         /* Interpreter to store message */
    DWORD winerr,               /* Windows error code */
    const char *msgPtr)         /* Message prefix. May be NULL. */
{
    if (interp) {
        Tcl_SetObjResult(interp, Iocp_MapWindowsError(winerr, NULL, msgPtr));
    }
    return TCL_ERROR;
}

/*
 * Stores the Windows error message corresponding to GetLastError()
 * in the passed interpreter.
 *
 * Always returns TCL_ERROR (convenient for caller to just return).
 */
IocpTclCode Iocp_ReportLastWindowsError(
    Tcl_Interp *interp,         /* Interpreter to store message */
    const char *msgPtr)         /* Message prefix. May be NULL. */
{
    return Iocp_ReportWindowsError(interp, GetLastError(), msgPtr);
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
void IocpSetTclErrnoFromWin32(IocpWinError winError)
{
    IOCP_TCL85_INTERNAL_PLATFORM_STUB(tclWinConvertError)(winError);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpSetInterpPosixErrorFromWin32 --
 *
 *    Sets Tcl's errno value and stores POSIX error message based on a Win32 error code.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Sets Tcl's internal errno value and interp result.
 *
 *------------------------------------------------------------------------
 */
void IocpSetInterpPosixErrorFromWin32(
    Tcl_Interp *interp,         /* May be NULL */
    IocpWinError winError,      /* Win32 error code */
    const char *messagePrefix)  /* A message prefix if any. May be NULL */
{
    IocpSetTclErrnoFromWin32(winError);
    if (interp != NULL) {
        const char *posixMessage = Tcl_PosixError(interp);
        if (messagePrefix == NULL) {
            Tcl_SetResult(interp, (char *)posixMessage, TCL_STATIC);
        }
        else {
            Tcl_AppendResult(interp, messagePrefix, posixMessage, NULL);
        }
    }
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

/*
 *------------------------------------------------------------------------
 *
 * IocpListPopFront --
 *
 *    Removes and returns the first element of the list.
 *
 * Results:
 *    Pointer to the first element or NULL if list was empty.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
IocpLink *IocpListPopFront(
    IocpList *listPtr
    )
{
    IocpLink *firstPtr = listPtr->headPtr;
    if (firstPtr) {
        listPtr->headPtr = firstPtr->nextPtr;
        if (listPtr->headPtr)
            listPtr->headPtr->prevPtr = NULL;
        if (listPtr->tailPtr == firstPtr)
            listPtr->tailPtr = NULL;
    }
    return firstPtr;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpListPopAll --
 *
 *    Removes and returns ALL elements as a list clearing out the original
 *    list.
 *
 * Results:
 *    New list header containing elements of original list.
 *
 * Side effects:
 *    Original passed in list is reset to empty.
 *
 *------------------------------------------------------------------------
 */
IocpList IocpListPopAll(
    IocpList *listPtr
    )
{
    IocpList temp = *listPtr;
    listPtr->headPtr = NULL;
    listPtr->tailPtr = NULL;
    return temp;
}

/*
 *----------------------------------------------------------------------
 *
 * IocpAcceptCallbacksDelete --
 *
 *	Assocdata cleanup routine called when an interpreter is being deleted
 *	to set the interp field of all the accept callback records registered
 *	with the interpreter to NULL. This will prevent the interpreter from
 *	being used in the future to eval accept scripts.
 *
 *      Mutated from TcpAcceptCallbacksDeleteProc from Tcl core with the
 *      difference that it is used for IOCP accepts, not just Tcp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deallocates memory and sets the interp field of all the accept
 *	callback records to NULL to prevent this interpreter from being used
 *	subsequently to eval accept scripts.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static void
IocpAcceptCallbacksDelete(
    ClientData clientData,	/* Data which was passed when the assocdata
				 * was registered. */
    Tcl_Interp *interp)		/* Interpreter being deleted - not used. */
{
    Tcl_HashTable *hTblPtr = clientData;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch hSearch;

    for (hPtr = Tcl_FirstHashEntry(hTblPtr, &hSearch);
	    hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
	IocpAcceptCallback *acceptCallbackPtr = Tcl_GetHashValue(hPtr);

	acceptCallbackPtr->interp = NULL;
    }
    Tcl_DeleteHashTable(hTblPtr);
    ckfree(hTblPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * IocpRegisterAcceptCallbackCleanup --
 *
 *	Registers an accept callback record to have its interp field set to
 *	NULL when the interpreter is deleted.
 *
 *      Mutated from RegisterTcpServerInterpCleanup from Tcl core with the
 *      difference that it is used for IOCP accepts, not just Tcp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	When, in the future, the interpreter is deleted, the interp field of
 *	the accept callback data structure will be set to NULL. This will
 *	prevent attempts to eval the accept script in a deleted interpreter.
 *
 *----------------------------------------------------------------------
 */

void
IocpRegisterAcceptCallbackCleanup(
    Tcl_Interp *interp,		/* Interpreter for which we want to be
				 * informed of deletion. */
    IocpAcceptCallback *acceptCallbackPtr)
				/* The accept callback record whose interp
				 * field we want set to NULL when the
				 * interpreter is deleted. */
{
    Tcl_HashTable *hTblPtr;	/* Hash table for accept callback records to
				 * smash when the interpreter will be
				 * deleted. */
    Tcl_HashEntry *hPtr;	/* Entry for this record. */
    int isNew;			/* Is the entry new? */

    hTblPtr = Tcl_GetAssocData(interp, iocpAcceptCallbackHashName, NULL);

    if (hTblPtr == NULL) {
	hTblPtr = ckalloc(sizeof(Tcl_HashTable));
	Tcl_InitHashTable(hTblPtr, TCL_ONE_WORD_KEYS);
	Tcl_SetAssocData(interp, iocpAcceptCallbackHashName,
		IocpAcceptCallbacksDelete, hTblPtr);
    }

    hPtr = Tcl_CreateHashEntry(hTblPtr, acceptCallbackPtr, &isNew);
    if (!isNew) {
	Iocp_Panic("IocpRegisterAcceptCallbackCleanup: damaged accept record table");
    }
    Tcl_SetHashValue(hPtr, acceptCallbackPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * IocpUnregisterAcceptCallbackCleanup --
 *
 *	Unregister a previously registered accept callback record. The interp
 *	field of this record will no longer be set to NULL in the future when
 *	the interpreter is deleted.
 *
 *      Mutated from UnregisterTcpServerInterpCleanup from Tcl core with the
 *      difference that it is used for IOCP accepts, not just Tcp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Prevents the interp field of the accept callback record from being set
 *	to NULL in the future when the interpreter is deleted.
 *
 *----------------------------------------------------------------------
 */

void
IocpUnregisterAcceptCallbackCleanup(
    Tcl_Interp *interp,		/* Interpreter in which the accept callback
				 * record was registered. */
    IocpAcceptCallback *acceptCallbackPtr)
				/* The record for which to delete the
				 * registration. */
{
    Tcl_HashTable *hTblPtr;
    Tcl_HashEntry *hPtr;

    hTblPtr = Tcl_GetAssocData(interp, iocpAcceptCallbackHashName, NULL);
    if (hTblPtr == NULL) {
	return;
    }

    hPtr = Tcl_FindHashEntry(hTblPtr, (char *) acceptCallbackPtr);
    if (hPtr != NULL) {
	Tcl_DeleteHashEntry(hPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * IocpUnregisterAcceptCallbackOnClose --
 *
 *	This callback is called when a IOCP listener channel for which it was
 *	registered is being closed. It informs the interpreter in which the
 *	accept script is evaluated (if that interpreter still exists) that
 *	this channel no longer needs to be informed if the interpreter is
 *	deleted.
 *
 *      Mutated from TcpServerCloseProc from Tcl core with the
 *      difference that it is used for IOCP accepts, not just Tcp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	In the future, if the interpreter is deleted this channel will no
 *	longer be informed.
 *
 *----------------------------------------------------------------------
 */

void
IocpUnregisterAcceptCallbackCleanupOnClose(
    ClientData callbackData)	/* The data passed in the call to
				 * Tcl_CreateCloseHandler. */
{
    IocpAcceptCallback *acceptCallbackPtr = callbackData;
				/* The actual data. */

    if (acceptCallbackPtr->interp != NULL) {
	IocpUnregisterAcceptCallbackCleanup(acceptCallbackPtr->interp,
		acceptCallbackPtr);
    }
    Tcl_EventuallyFree(acceptCallbackPtr->script, TCL_DYNAMIC);
    ckfree(acceptCallbackPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpCreateTclChannel --
 *
 *    Creates a Tcl Channel for the passed IocpChannel.
 *
 * Results:
 *    Returns the created Tcl_Channel on success, NULL on failure.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
Tcl_Channel IocpCreateTclChannel(
    IocpChannel *chanPtr,
    const char *namePrefix,     /* Prefix to use for channel name.
                                 * Should be less than 70 chars */
    int flags                   /* More flags TCL_READABLE | TCL_WRITABLE
                                 * passsed to Tcl_CreateChannel */
    )
{
    /* TBD - replace this use with IocpMakeTclChannel instead */
    char channelName[100];
    sprintf_s(channelName,
              sizeof(channelName)/sizeof(channelName[0]),
              "%s%p", namePrefix, chanPtr);
    return Tcl_CreateChannel(&IocpChannelDispatch, channelName,
                             chanPtr, flags);

}

/*
 *------------------------------------------------------------------------
 *
 * IocpMakeTclChannel --
 *
 *    Creates a Tcl Channel for the passed IocpChannel and links to it.
 * 
 * Results:
 *    Returns the created Tcl_Channel on success, NULL on failure with
 *    an error message in interp if not NULL.
 *
 * Side effects:
 *    The reference count on the IocpChannel is incremented corresponding
 *    to its reference from Tcl channel subsystem (only on success).
 *
 *------------------------------------------------------------------------
 */
Tcl_Channel
IocpMakeTclChannel(
    Tcl_Interp  *interp,        /* For error messages. May be NULL */
    IocpChannel *lockedChanPtr, /* Must be locked on entry and is locked
                                 * on return but state may change in the
                                 * meanwhile as unlocked when calling Tcl */
    const char  *namePrefix,    /* Prefix to use for channel name.
                                 * Should be less than 70 chars */
    int         flags           /* More flags TCL_READABLE | TCL_WRITABLE
                                 * passsed to Tcl_CreateChannel */
)
{
    char channelName[100];
    Tcl_Channel chan;
    sprintf_s(channelName,
              sizeof(channelName)/sizeof(channelName[0]),
              "%s%p", namePrefix, lockedChanPtr);
    /*
     * Unlock since Tcl_CreateChannel can recurse back into us. However,
     * since caller will be holding a reference, lockedChanPtr itself
     * will not be invalidated. The state may change but that's ok,
     * Handled by upper layers once the channel is created.
     */
    IocpChannelUnlock(lockedChanPtr);
    chan = Tcl_CreateChannel(&IocpChannelDispatch, channelName,
                             lockedChanPtr, flags);
    IocpChannelLock(lockedChanPtr);
    if (chan) {
        /* Link the two. */
        lockedChanPtr->channel = chan;
        lockedChanPtr->numRefs += 1; /* Reversed through Tcl_Close */
    } else {
        if (interp) {
            Tcl_AppendResult(
                interp, "Could not create channel ", channelName, ".", NULL);
        }
    }
    return chan;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpSetChannelDefaults --
 *
 *    Sets the default -translation and -eofchar values  for channels
 *    as "auto crlf" and "" to match Tcl sockets.
 *
 * Results:
 *    TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
IocpTclCode
IocpSetChannelDefaults(
    Tcl_Channel channel
    )
{
    if (Tcl_SetChannelOption(NULL, channel,
                             "-translation", "auto crlf") == TCL_ERROR ||
        Tcl_SetChannelOption(NULL, channel, "-eofchar", "") == TCL_ERROR) {
        return TCL_ERROR;
    }
    return TCL_OK;
}


#ifdef IOCP_ENABLE_TRACE
/*
 *------------------------------------------------------------------------
 *
 * IocpTraceString --
 *
 *    Writes a single string to an ETW trace.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
void IocpTraceString(const char *buf)
{
    if (iocpEnableTrace == 1000) {
        IocpLockAcquireExclusive(&iocpTraceLock);
        printf("[%d] %s", GetCurrentThreadId(), buf);
        IocpLockReleaseExclusive(&iocpTraceLock);
    } else if (iocpEnableTrace == 1001) {
        IocpDebuggerOut("[%d] %s", GetCurrentThreadId(), buf);
    } else if (iocpEnableTrace){
        /* TraceLogging API already includes thread id, no need to add it */
        TraceLoggingWrite(iocpWinTraceProvider,
                          "TclIocpTrace",
                          TraceLoggingString(buf, "Trace Message")
            );
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTrace --
 *
 *    Writes a formatted string to an ETW trace.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
void __cdecl IocpTrace(
    const char *formatStr,
    ...
    )
{
    char buf[2048];
    va_list args;

    va_start(args, formatStr);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, formatStr, args);
    va_end(args);
    buf[sizeof(buf)-1] = '\0';

    IocpTraceString(buf);
}

#endif /* IOCP_ENABLE_TRACE */

/*
 *------------------------------------------------------------------------
 *
 * IocpDebuggerOut --
 *
 *    Writes a formatted string to a Windows debugger console.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
void __cdecl IocpDebuggerOut(
    const char *formatStr,
    ...
    )
{
    char buf[2048];
    va_list args;

    va_start(args, formatStr);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, formatStr, args);
    va_end(args);
    buf[sizeof(buf)-1] = '\0';

    OutputDebugStringA(buf);
}

/*
 *------------------------------------------------------------------------
 *
 * Iocp_Panic --
 *
 *    Causes a debugger break if a debugger is attached, otherwise calls
 *    Iocp_Panic.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Either traps into debugger or exits the process via Iocp_Panic.
 *
 *------------------------------------------------------------------------
 */
void __cdecl Iocp_Panic(
    const char *formatStr,
    ...
    )
{
    va_list args;

    va_start(args, formatStr);
    if (IsDebuggerPresent()) {
        char buf[1024];
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, formatStr, args);
        va_end(args);
        buf[sizeof(buf)-1] = '\0';
        OutputDebugStringA(buf);
        __debugbreak();
    } else {
        Tcl_PanicVA(formatStr, args);
        va_end(args); /* Not reached but... */
    }
}


