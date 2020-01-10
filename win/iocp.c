/*
 * tclWinIocp.c --
 *
 *	Main module of Windows-specific IOCP related variables and procedures.
 *
 * Copyright (c) 2019 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include "tclWinIocp.h"

static void IocpEventSetup(ClientData clientData, int flags);
static void IocpEventCheck(ClientData clientData, int flags);
static void IocpThreadExitHandler (ClientData clientData);

/*
 * Static data
 */
static Tcl_ThreadDataKey iocpTsdDataKey;

/* Holds global IOCP state */
IocpSubSystem iocpModuleState;


/*
 * Initializes a IocpDataBuffer to be able to hold capacity bytes worth of
 * data.
 * dataBuf  - pointer to uninitialized raw memory
 * capacity - requested buffer capacity
 *
 * Returns a pointer to the raw storage area allocated. A return value of
 * NULL indicates either storage allocation failed or capacity of 0 bytes
 * was requested in which case no storage area is allocated.
 */
char *IocpDataBufferInit(IocpDataBuffer *dataBuf, int capacity)
{
    dataBuf->capacity = capacity;
    dataBuf->begin    = 0;
    dataBuf->len      = 0;
    if (capacity) {
        dataBuf->bytes = attemptckalloc(capacity);
        if (dataBuf->bytes == NULL)
            dataBuf->capacity = 0;
    } else
        dataBuf->bytes = NULL;
    return dataBuf->bytes;
}

/*
 * Releases any resources allocated for the IocpDataBuf. The structure
 * should not be accessed again without calling IocpDataBufferInit on
 * it first.
 *  dataBufPtr - pointer to structure to finalize
 */
void IocpDataBufferFini(IocpDataBuffer *dataBufPtr)
{
    if (dataBufPtr->bytes)
        ckfree(dataBufPtr->bytes);
}

/*
 * Copies bytes from a IocpDataBuffer. The source buffer is updated to
 * reflect the copied bytes being removed.
 * dataBuf - source buffer
 * outPtr  - destination address
 * len     - number of bytes to copy
 *
 * Returns the number of bytes copied which may be less than len
 * if the source buffer does not have sufficient data.
 */
int IocpDataBufferMove(IocpDataBuffer *dataBuf, char *outPtr, int len)
{
    int numCopied = dataBuf->len > len ? len : dataBuf->len;
    if (dataBuf->bytes)
        memcpy(outPtr, dataBuf->begin + dataBuf->bytes, numCopied);
    dataBuf->begin += numCopied;
    dataBuf->len   -= numCopied;
    return numCopied;
}

/*
 * Allocates and initializes an IocpBuffer associated with a channel and of
 * a specified capacity.
 *  channelPtr - pointer to the IocpChannel structure to associate.
 *               Its reference count is incremented. May be NULL.
 *  capacity   - the capacity of the data buffer
 *
 * The reference count of the returned IocpBuffer is 1.
 *
 * On success, returns a pointer that should be freed by calling
 * IocpBufferDrop. On failure, returns NULL.
 */
IocpBuffer *IocpBufferNew(int capacity)
{
    IocpBuffer *bufPtr = attemptckalloc(sizeof(*bufPtr));
    if (bufPtr == NULL)
        return NULL;
    memset(&bufPtr->u, 0, sizeof(bufPtr->u));

    bufPtr->winError  = 0;
    bufPtr->operation = IOCP_BUFFER_OP_NONE;

    if (IocpDataBufferInit(&bufPtr->data, capacity) == NULL) {
        ckfree(bufPtr);
        return NULL;
    }
    return bufPtr;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpBufferFree --
 *
 *    Frees a IocpBuffer structure releasing allocated resources.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The underlying data buffer is also freed.
 *
 *------------------------------------------------------------------------
 */
void IocpBufferFree(IocpBuffer *bufPtr)
{
    IocpDataBufferFini(&bufPtr->data);
    ckfree(bufPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelNew --
 *
 *    Allocates and initializes a IocpChannel structure. The reference count of
 *    the structure is initialized to 1. 
 *
 * Results:
 *    Returns a pointer to the allocated IocpChannel.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
IocpChannel *IocpChannelNew(
    Tcl_Interp *interp,
    const IocpChannelVtbl *vtblPtr) /* See IocpChannelVtbl definition comments.
                                     * Must point to static memory */
{
    IocpChannel *chanPtr;
    chanPtr          = ckalloc(vtblPtr->allocationSize);
    IocpListInit(&chanPtr->inputBuffers);
    IocpLinkInit(&chanPtr->readyLink);
    chanPtr->channel = NULL;
    chanPtr->tsdPtr  = NULL;
    chanPtr->numRefs = 1;
    chanPtr->vtblPtr = vtblPtr;
    InitializeConditionVariable(&chanPtr->cv);
    IocpLockInit(&chanPtr->lock);
    if (vtblPtr->initialize) {
        vtblPtr->initialize(interp, chanPtr);
    }
    return chanPtr;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelDrop --
 *
 *    Decrements the reference count for a IocpChannel. If no more references
 *    are outstanding, the IocpChannel's finalizer is called and all resources
 *    freed. The IocpChannel must NOT linked to any IocpTsd, either through
 *    the tsdPtr field or through the TSD's readyChannels list.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The IocpChannel is unlocked and potentially freed.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelDrop(
    Tcl_Interp *interp,
    IocpChannel *lockedChanPtr)       /* Must be locked on entry. Unlocked
                                       * and potentially freed on return */
{
    if (--lockedChanPtr->numRefs <= 0) {
        // TBD assert(lockedChanPtr->tsdPtr == NULL)
        // TBD assert(lockedChanPtr->readyLink.next == lockedChanPtr->readyLink.prev == NULL)
        if (lockedChanPtr->vtblPtr->finalize)
            lockedChanPtr->vtblPtr->finalize(interp, lockedChanPtr);
        IocpChannelUnlock(lockedChanPtr);
        IocpLockDelete(&lockedChanPtr->lock);
        ckfree(lockedChanPtr);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelSleepForIO --
 *
 *    Releases the lock on a IocpChannel and then blocks until an I/O
 *    completion is signaled. On returning the IocpChannel lock is reacquired.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Because the lock on the IocpChannel is released and reacquired,
 *    the channel state might have changed before returning.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelAwaitCompletion(IocpChannel *lockedChanPtr)    /* Must be locked on entry */
{
    lockedChanPtr->flags   |= IOCP_CHAN_F_BLOCKED;
    IocpConditionVariableWaitShared(&lockedChanPtr->cv, &lockedChanPtr->lock, INFINITE);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelWakeForIO --
 *
 *    Wakes up a thread (if any) blocked on some I/O operation to complete.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The thread (if any) blocked on the channel is awoken.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelWakeAfterCompletion(IocpChannel *lockedChanPtr)   /* Must be locked on entry */
{
    /* Checking the flag, saves a potential unnecessary kernel transition */
    if (lockedChanPtr->flags & IOCP_CHAN_F_BLOCKED) {
        lockedChanPtr->flags &= ~IOCP_CHAN_F_BLOCKED;
        WakeConditionVariable(&lockedChanPtr->cv);
    }
}


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

static void IocpCompletionFailure(IocpBuffer *bufPtr)
{
    // TBD
}

/*
 *------------------------------------------------------------------------
 *
 * IocpCompletionRead --
 *
 *    Handles completion of read operations from IOCP.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    On successful reads the passed bufPtr is enqueued on the owning
 *    IocpChannel and the Tcl thread notified via the event loop. If the
 *    Tcl thread is blocked on this channel, it is woken up.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompletionRead(
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    //TBD
}

static DWORD WINAPI
IocpCompletionThread (LPVOID lpParam)
{
    IocpBuffer *bufPtr;
    HANDLE      iocpPort = (HANDLE) lpParam;
    DWORD       nbytes;
    ULONG_PTR   key;
    OVERLAPPED *overlapPtr;
    BOOL        ok;
    DWORD       error;

    __try {
        while (1) {
            ok = GetQueuedCompletionStatus(iocpPort, &nbytes, &key,
                                           &overlapPtr, INFINITE);
            if (!ok) {
                if (overlapPtr) {
                    bufPtr = CONTAINING_RECORD(overlapPtr, IocpBuffer, u);
                    bufPtr->data.len = nbytes;
                    bufPtr->winError = GetLastError();
                    IocpCompletionFailure(bufPtr);
                }
                else {
                    /* TBD - how to signal or log error? */
                    break;      /* iocp port probably no longer valid */
                }
            } else {
                if (overlapPtr == NULL)
                    break;      /* Exit signal */
                IOCP_ASSERT(overlapPtr);
                bufPtr = CONTAINING_RECORD(overlapPtr, IocpBuffer, u);
                bufPtr->data.len = nbytes;
                bufPtr->winError = 0;
                switch (bufPtr->operation) {
                case IOCP_BUFFER_OP_READ:
                    IocpCompletionRead(bufPtr);
                    break;
                }
            }
        }
    }
    __except (error = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        Tcl_Panic("Tcl IOCP thread died with exception %#x\n", error);
    }

    return error;
}

/*
 * Finalization function to be called exactly once *per process*.
 * Caller responsible to ensure it's called only once in a thread-safe manner.
 * Essentially reverses effects of IocpProcessInit.
 *
 * Returns TCL_OK on success, TCL_ERROR on error.
 */
Iocp_DoOnceState iocpProcessCleanupFlag;
static IocpResultCode IocpProcessCleanup(ClientData clientdata)
{
    if (iocpModuleState.initialized) {
        /* Tell completion port thread to exit and wait for it */
        PostQueuedCompletionStatus(iocpModuleState.completion_port, 0, 0, 0);
        if (WaitForSingleObject(iocpModuleState.completion_thread, 500)
            == WAIT_TIMEOUT) {
            /* 0xdead - exit code for thread */
            TerminateThread(iocpModuleState.completion_thread, 0xdead);
        }
        CloseHandle(iocpModuleState.completion_thread);
        iocpModuleState.completion_thread = NULL;

        CloseHandle(iocpModuleState.completion_port);
        iocpModuleState.completion_port = NULL;


        IocpLockDelete(iocpModuleState.tsd_lock);

        WSACleanup();
    }
    return TCL_OK;
}

static void IocpProcessExitHandler(ClientData clientdata)
{
    (void) Iocp_DoOnce(&iocpProcessCleanupFlag, IocpProcessCleanup, NULL);
}

/*
 * Initialization function to be called exactly once *per process*.
 * Caller responsible to ensure it's called only once in a thread-safe manner.
 * It initializes Winsock, creates the I/O completion port and thread.
 *
 * Returns TCL_OK on success, TCL_ERROR on error.
 */
Iocp_DoOnceState iocpProcessInitFlag;
static IocpResultCode IocpProcessInit(ClientData clientdata)
{
    WSADATA wsa_data;
    Tcl_Interp *interp = (Tcl_Interp *)clientdata;

#define WSA_VERSION_REQUESTED    MAKEWORD(2,2)

    iocpModuleState.completion_port =
        CreateIoCompletionPort(
            INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, 0);
    if (iocpModuleState.completion_port == NULL) {
        Iocp_ReportLastWindowsError(interp);
        return TCL_ERROR;
    }
    if (WSAStartup(WSA_VERSION_REQUESTED, &wsa_data) != 0) {
        CloseHandle(iocpModuleState.completion_port);
        iocpModuleState.completion_port = NULL;
        Tcl_SetResult(interp, "Could not load winsock.", TCL_STATIC);
        return TCL_ERROR;
    }

    iocpModuleState.completion_thread =
        CreateThread(NULL, 0, IocpCompletionThread, iocpModuleState.completion_port, 0, NULL);
    if (iocpModuleState.completion_thread == NULL) {
        Iocp_ReportLastWindowsError(interp);
        CloseHandle(iocpModuleState.completion_port);
        iocpModuleState.completion_port = NULL;
        WSACleanup();
        return TCL_ERROR;
    }
    IocpLockInit(&iocpModuleState.tsd_lock);
    iocpModuleState.initialized = 1;

    Tcl_CreateExitHandler(IocpProcessExitHandler, NULL);

    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTsdNew --
 *
 *    Allocates a new IocpTsd structure and initalizes it.
 *
 * Results:
 *    Returns an allocated IocpTsd structure initialized
 *    with a reference count of 1.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static IocpTsd *IocpTsdNew()
{
    IocpTsdPtr tsdPtr = ckalloc(sizeof(*tsdPtr));
    IocpListInit(&tsdPtr->readyChannels);
    tsdPtr->threadId = Tcl_GetCurrentThread();
    /* TBD - This reference will be cancelled via IocpTsdUnlinkThread when thread exits */
    tsdPtr->numRefs  = 1;
    return tsdPtr;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTsdUnlinkThread --
 *
 *    Disassociates the IocpTsd for the current thread. It must NOT have
 *    been locked when the function is called.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The associated IocpTsd is potentially freed.
 *
 *------------------------------------------------------------------------
 */
void IocpTsdUnlinkThread()
{
    IocpTsdPtr *tsdPtrPtr = IOCP_TSD_INIT(&iocpTsdDataKey);
    if (tsdPtrPtr && *tsdPtrPtr) {
        IocpTsd *tsdPtr = *tsdPtrPtr;
        *tsdPtrPtr = NULL;
        IocpTsdLock();
        tsdPtr->threadId = 0;
        tsdPtr->numRefs -= 1;
        if (tsdPtr->numRefs <= 0) {
            /*
             * When invoked, the readyChannels list in the IocpTsd should be empty
             * if the reference count drops to 0, else the function will panic as it
             * implies something's gone wrong in the reference counting.
             * TBD
             */
            if (tsdPtr->readyChannels.headPtr)
                Tcl_Panic("Attempt to free IocpTsd with channels attached.");
            IocpTsdUnlock();
            ckfree(tsdPtr); /* Do outside of lock */
        } else {
            IocpTsdUnlock();
        }
    }
}

/*
 * Thread initialization. May be called multiple times as multiple interpreters
 * may be set up within a thread. However, no synchronization between threads
 * needed as it only initializes thread-specific data.
 *
 * Returns TCL_OK on success, TCL_ERROR on error.
 */
IocpResultCode IocpThreadInit(Tcl_Interp *interp)
{
    IocpTsdPtr *tsdPtrPtr = IOCP_TSD_INIT(&iocpTsdDataKey);
    if (tsdPtrPtr == NULL) {
        *tsdPtrPtr = IocpTsdNew();
	Tcl_CreateEventSource(IocpEventSetup, IocpEventCheck, NULL);
	Tcl_CreateThreadExitHandler(IocpThreadExitHandler, NULL);
    }
    return TCL_OK;
}

static void
IocpThreadExitHandler (ClientData notUsed)
{
    IocpTsdUnlinkThread();
    Tcl_DeleteEventSource(IocpEventSetup, IocpEventCheck, NULL);
}

/*
 * IocpChannelDispatch
 * Defines the dispatch table for IOCP data channels.
 */
static Tcl_DriverCloseProc	IocpChannelClose;
static Tcl_DriverInputProc	IocpChannelInput;
static Tcl_DriverOutputProc	IocpChannelOutput;
static Tcl_DriverSetOptionProc	IocpChannelSetOption;
static Tcl_DriverGetOptionProc	IocpChannelGetOption;
static Tcl_DriverWatchProc	IocpChannelWatch;
static Tcl_DriverGetHandleProc	IocpChannelGetHandle;
static Tcl_DriverBlockModeProc	IocpChannelBlockMode;
static Tcl_DriverClose2Proc	IocpChannelClose2;
static Tcl_DriverThreadActionProc IocpChannelThreadAction;

Tcl_ChannelType IocpChannelDispatch = {
    "iocpconnection", /* Channel type name */
    TCL_CHANNEL_VERSION_4,
    IocpChannelClose,
    IocpChannelInput,
    IocpChannelOutput,
    NULL, /* No Seek ability */
    IocpChannelSetOption,
    IocpChannelGetOption,
    IocpChannelWatch,
    IocpChannelGetHandle,
    IocpChannelClose2,
    IocpChannelBlockMode,
    NULL, /* FlushProc. Must be NULL as per Tcl docs */
    NULL, /* HandlerProc. Only valid for stacked channels */
    NULL, /* WideSeekProc. */
    IocpChannelThreadAction,
};

static int
IocpChannelClose (
    ClientData instanceData,	/* The channel to close. */
    Tcl_Interp *interp)		/* Unused. */
{
    IocpChannel *lockedChanPtr = (IocpChannel*)instanceData;
    int          ret;

    IocpChannelLock(lockedChanPtr);

    /*
     * Before this function is called, IocpThreadAction should have been
     * called to remove the channel from all threads. However, just for
     * the record in case of future changes, we do not need to remove from
     * the TSD ready queue in any case since when the ready notification
     * is dequeued a check is made to see if channel is already detached.
     */

    /* Call specific IOCP type to close OS handles */
    ret = (lockedChanPtr->vtblPtr->shutdown)(interp, lockedChanPtr, 0);

    /* Irrespective of errors in above call, we're done with this channel */
    lockedChanPtr->channel = NULL;
    IocpChannelDrop(interp, lockedChanPtr); /* Drops ref count from Tcl channel */
    /* Do NOT refer to lockedChanPtr beyond this point */

    return ret;
}

static int
IocpChannelInput (
    ClientData instanceData,	/* The socket state. */
    char *buf,			/* Where to store data. */
    int toRead,			/* Maximum number of bytes to read. */
    int *errorCodePtr)		/* Where to store error codes. */
{
    // TBD
    *errorCodePtr = EINVAL;
    return -1;
}

static int
IocpChannelOutput (
    ClientData instanceData,	/* The socket state. */
    CONST char *buf,		/* Where to get data. */
    int toWrite,		/* Maximum number of bytes to write. */
    int *errorCodePtr)		/* Where to store error codes. */
{
    // TBD
    *errorCodePtr = EINVAL;
    return -1;
}

static int
IocpChannelSetOption (
    ClientData instanceData,	/* Socket state. */
    Tcl_Interp *interp,		/* For error reporting - can be NULL. */
    CONST char *optionName,	/* Name of the option to set. */
    CONST char *value)		/* New value for option. */
{
    // TBD
    return TCL_ERROR;
}

static int
IocpChannelGetOption (
    ClientData instanceData,	/* Socket state. */
    Tcl_Interp *interp,		/* For error reporting - can be NULL */
    CONST char *optionName,	/* Name of the option to
				 * retrieve the value for, or
				 * NULL to get all options and
				 * their values. */
    Tcl_DString *dsPtr)		/* Where to store the computed
				 * value; initialized by caller. */
{
    // TBD
    return TCL_ERROR;
}

static void
IocpChannelWatch (
    ClientData instanceData,	/* The socket state. */
    int mask)			/* Events of interest; an OR-ed
				 * combination of TCL_READABLE,
				 * TCL_WRITABLE and TCL_EXCEPTION. */
{
    // TBD
    return;
}

static int
IocpChannelClose2 (
    ClientData instanceData,	/* The socket to close. */
    Tcl_Interp *interp,		/* Unused. */
    int flags)
{
    /* TBD */
    return 0;
}

static int
IocpChannelBlockMode (
    ClientData instanceData,	/* The socket state. */
    int mode)			/* TCL_MODE_BLOCKING or
                                 * TCL_MODE_NONBLOCKING. */
{
    // TBD
    return 0;
}

static int
IocpChannelGetHandle (
    ClientData instanceData,	/* The socket state. */
    int direction,		/* TCL_READABLE or TCL_WRITABLE */
    ClientData *handlePtr)	/* Where to store the handle.  */
{
    // TBD
    return TCL_ERROR;
}

static void
IocpChannelThreadAction (ClientData instanceData, int action)
{
    // TBD
}

/*
 *-----------------------------------------------------------------------
 * IocpEventSetup --
 *
 *  Happens before the event loop is to wait in the notifier.
 *
 *-----------------------------------------------------------------------
 */
static void
IocpEventSetup(
    ClientData clientData,
    int flags)
{
#ifdef TBD
    ThreadSpecificData *tsdPtr = InitSockets();
    Tcl_Time blockTime = {0, 0};

    if (!(flags & TCL_FILE_EVENTS)) {
	return;
    }

    /*
     * If any ready events exist now, don't let the notifier go into it's
     * wait state.  This function call is very inexpensive.
     */

    if (IocpLLIsNotEmpty(tsdPtr->readySockets) ||
        0 /*TODO: IocpLLIsNotEmpty(tsdPtr->deadSockets)*/) {
	Tcl_SetMaxBlockTime(&blockTime);
    }
#endif
}

/*
 *-----------------------------------------------------------------------
 * IocpEventCheck --
 *
 *  Happens after the notifier has waited.
 *
 *-----------------------------------------------------------------------
 */
static void
IocpEventCheck (
    ClientData clientData,
    int flags)
{
#ifdef TBD
    ThreadSpecificData *tsdPtr = InitSockets();
    SocketInfo *infoPtr;
    SocketEvent *evPtr;
    int evCount;

    if (!(flags & TCL_FILE_EVENTS)) {
	/* Don't be greedy. */
	return;
    }

    /*
     * Sockets that are EOF, but not yet closed, are considered readable.
     * Because Tcl historically requires that EOF channels shall still
     * fire readable and writable events until closed and our alert
     * semantics are such that we'll never get repeat notifications after
     * EOF, we place this poll condition here.
     */

    /* TODO: evCount = IocpLLGetCount(tsdPtr->deadSockets); */

    /*
     * Do we have any jobs to queue?  Take a snapshot of the count as
     * of now.
     */

    evCount = IocpLLGetCount(tsdPtr->readySockets);

    while (evCount--) {
	EnterCriticalSection(&tsdPtr->readySockets->lock);
	infoPtr = IocpLLPopFront(tsdPtr->readySockets,
		IOCP_LL_NOLOCK | IOCP_LL_NODESTROY, 0);
	/*
	 * Flop the markedReady toggle.  This is used to improve event
	 * loop efficiency to avoid unneccesary events being queued into
	 * the readySockets list.
	 */
	if (infoPtr) InterlockedExchange(&infoPtr->markedReady, 0);
	LeaveCriticalSection(&tsdPtr->readySockets->lock);

	/*
	 * Safety check. Somehow the count of what is and what actually
	 * is, is less (!?)..  whatever...  
	 */
	if (!infoPtr) continue;

	/*
	 * The socket isn't ready to be serviced.  accept() in the Tcl
	 * layer hasn't happened yet while reads on the new socket are
	 * coming in or the socket is in the middle of doing an async
	 * close.
	 */
	if (infoPtr->channel == NULL) {
	    continue;
	}

	evPtr = (SocketEvent *) ckalloc(sizeof(SocketEvent));
	evPtr->header.proc = IocpEventProc;
	evPtr->infoPtr = infoPtr;
	Tcl_QueueEvent((Tcl_Event *) evPtr, TCL_QUEUE_TAIL);
    }
#endif
}


int
Iocp_Init (Tcl_Interp *interp)
{
#ifdef USE_TCL_STUBS
    if (Tcl_InitStubs(interp, "8.6", 0) == NULL) {
	return TCL_ERROR;
    }
#endif

    if (Iocp_DoOnce(&iocpProcessInitFlag, IocpProcessInit, interp) != TCL_OK) {
        if (Tcl_GetCharLength(Tcl_GetObjResult(interp)) == 0) {
            Tcl_SetResult(interp, "Unable to do one-time initialization for " PACKAGE_NAME ".", TCL_STATIC);
        }
        return TCL_ERROR;
    }

    if (IocpThreadInit(interp) != TCL_OK) {
        if (Tcl_GetCharLength(Tcl_GetObjResult(interp)) == 0) {
            Tcl_SetResult(interp, "Unable to do thread initialization for " PACKAGE_NAME ".", TCL_STATIC);
        }
        return TCL_ERROR;
    }

    Tcl_CreateObjCommand(interp, "iocp::socket", Iocp_SocketObjCmd, 0L, 0L);
    Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION);
    return TCL_OK;
}



