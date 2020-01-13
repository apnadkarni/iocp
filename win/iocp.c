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
static int  IocpEventProcess(Tcl_Event *evPtr, int flags);
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

    bufPtr->chanPtr   = NULL;
    bufPtr->winError  = 0;
    bufPtr->operation = IOCP_BUFFER_OP_NONE;
    bufPtr->flags     = 0;
    IocpLinkInit(&bufPtr->link);

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
    IOCP_ASSERT(bufPtr->chanPtr == NULL);
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
    chanPtr->state   = IOCP_STATE_INIT;
    chanPtr->numRefs = 1;
    chanPtr->vtblPtr = vtblPtr;
    InitializeConditionVariable(&chanPtr->cv);
    IocpLockInit(&chanPtr->lock);
    if (vtblPtr->initialize) {
        vtblPtr->initialize(chanPtr);
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
 *    the tsdPtr field or through the TSD's readyChannels list. The latter
 *    is assured by the reference count.
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
    IocpChannel *lockedChanPtr)       /* Must be locked on entry. Unlocked
                                       * and potentially freed on return */
{
    if (--lockedChanPtr->numRefs <= 0) {
        // TBD assert(lockedChanPtr->tsdPtr == NULL)
        // TBD assert(lockedChanPtr->readyLink.next == lockedChanPtr->readyLink.prev == NULL)
        if (lockedChanPtr->vtblPtr->finalize)
            lockedChanPtr->vtblPtr->finalize(lockedChanPtr);
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
 *    Returns 1 if there was a thread blocked, else 0.
 *
 * Side effects:
 *    The thread (if any) blocked on the channel is awoken.
 *
 *------------------------------------------------------------------------
 */
int IocpChannelWakeAfterCompletion(IocpChannel *lockedChanPtr)   /* Must be locked on entry */
{
    /* Checking the flag, saves a potential unnecessary kernel transition */
    if (lockedChanPtr->flags & IOCP_CHAN_F_BLOCKED) {
        lockedChanPtr->flags &= ~IOCP_CHAN_F_BLOCKED;
        WakeConditionVariable(&lockedChanPtr->cv);
        return 1;
    }
    else {
        return 0;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelAddToReadyQ --
 *
 *    If the IocpChannel is attached to a thread and has any pending
 *    events, it is added to the thread's ready queue if not already
 *    on there.
 *
 *    Caller must be holding the IocpTsdLock() and the IocpChannel lock.
 *    Both are still held when the function returns.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The IocpChannel is placed on the thread's ready queue.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelAddToReadyQ(
    IocpChannel *lockedChanPtr  /* Must be locked on entry */
    )
{
    IocpTsd *tsdPtr = lockedChanPtr->tsdPtr;
    if (tsdPtr == NULL || /* Not attached to a thread */
        lockedChanPtr->flags & IOCP_CHAN_F_ON_READYQ) /* Already on ready q */
        return;
    if (lockedChanPtr->flags & IOCP_CHAN_F_WRITE_DONE ||
        lockedChanPtr->inputBuffers.headPtr) {
        lockedChanPtr->readyLink.nextPtr = NULL;
        lockedChanPtr->readyLink.prevPtr = tsdPtr->readyChannels.tailPtr;
        if (tsdPtr->readyChannels.headPtr == NULL) {
            /* Empty queue */
            tsdPtr->readyChannels.headPtr = &lockedChanPtr->readyLink;
            tsdPtr->readyChannels.tailPtr = &lockedChanPtr->readyLink;
        }
        else {
            IocpLink *tailLinkPtr = tsdPtr->readyChannels.tailPtr;
            IocpChannel *tailChanPtr = CONTAINING_RECORD(tsdPtr->readyChannels.tailPtr, IocpChannel, readyLink);
            IocpChannelLock(tailChanPtr);
            tailLinkPtr->nextPtr = &lockedChanPtr->readyLink;
            tsdPtr->readyChannels.tailPtr = &lockedChanPtr->readyLink;
            IocpChannelUnlock(tailChanPtr);
        }
    }
}
/*
 *------------------------------------------------------------------------
 *
 * IocpChannelNotifyCompletion --
 *
 *    Notifies the Tcl thread owning a channel of the completion of a
 *    posted I/O operation.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The IocpChannel is appended to the Tcl thread's ready q if not already
 *    present and the Tcl thread alerted. Due to the lock hierarchy rules,
 *    *lockedChanPtr may be unlocked and relocked before returning and its
 *    state may have therefore changed.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelNotifyCompletion(
    IocpChannel *lockedChanPtr  /* Must be locked and caller holding a reference
                                 * to ensure it does not go away even if unlocked
                                 */
    )
{
    IOCP_ASSERT(lockedChanPtr->tsdPtr);

    /*
     * Do not add to queue if already there! IocpChannelAddToReadyQ already
     * checks for this but this is an optimization to avoid unnecessary
     * locking/unlocking.
     */
    if ((lockedChanPtr->flags & IOCP_CHAN_F_ON_READYQ) == 0) {
        /* Avoid deadlocks by following lock hierarchy. */
        IocpChannelUnlock(lockedChanPtr);
        IocpTsdLock();
        IocpChannelLock(lockedChanPtr);

        /* lockedChanPtr may have changed state! No matter.
         * IocpChannelAddToReadyQ handles lockedChanPtr->tsdPtr being
         * NULL or already on ready queue.
         */
        IocpChannelAddToReadyQ(lockedChanPtr);
        /*
         * Alert the thread. Be careful in that in case of unlocking/relocking
         * sequence above the tsdPtr may have become NULL if channel is in
         * process of being transferred between threads or just changed.
         */
        if (lockedChanPtr->tsdPtr && lockedChanPtr->tsdPtr->threadId != 0)
            Tcl_ThreadAlert(lockedChanPtr->tsdPtr->threadId);

        IocpTsdUnlock();
    }
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
    IocpChannel *chanPtr = bufPtr->chanPtr;

    IocpChannelLock(chanPtr);

    if (chanPtr->state == IOCP_STATE_CLOSED) {
        bufPtr->chanPtr = NULL;
        IocpChannelDrop(chanPtr); /* Corresponding to bufPtr->chanPtr */
        IocpBufferFree(bufPtr);
        return;
    }

    /*
     * Add the buffer to the input queue. Then if the channel was blocked,
     * awaken the sleeping thread. Otherwise send it a Tcl event notification.
     */
    IocpListAppend(&chanPtr->inputBuffers, &bufPtr->link);
    bufPtr->chanPtr = NULL;
    /*
     * chanPtr->numRefs-- because bufPtr does not refer to it (though it is on
     *                    the inputBuffers queue, that is immaterial)
     * chanPtr->numRefs++ because we still want to access chanPtr below after
     *                    unlocking and relocking.
     * The two cancel out. The latter will be reversed at function exit.
     */

    /*
     * Note that zero bytes read => EOF. That will also be handled in the
     * Tcl thread since in any case it has to be notified of closure.
     */

    if (! IocpChannelWakeAfterCompletion(chanPtr)) {
        /*
         * Owning thread was not sleeping while blocked so need to notify
         * it via the ready queue/event loop.
         */
        /* TBD - do we need to notify on 0 byte read (EOF) even if NOTIFY_INPUT
           was not set ? */
        if (chanPtr->flags & IOCP_CHAN_F_WATCH_INPUT)
            IocpChannelNotifyCompletion(chanPtr);
    }

    /* This drops the reference from bufPtr which was delayed (see above) */
    IocpChannelDrop(chanPtr);
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
 * IocpTsdDrop --
 *
 *    Decrements the reference count for a IocpTsd and frees memory
 *    if no more references.
 *    NOTE: Caller must be holding IocpTsdLock() before calling this function.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Memory is potentially freed. The IocpTsdLock() is released
 *    depending on lockAction.
 *------------------------------------------------------------------------
 */
void IocpTsdDrop(
    IocpTsd *tsdPtr,
    enum IocpLockAction lockAction /* IOCP_RELEASE_LOCK => release
                                    * lock before returning */
    )
{
    tsdPtr->numRefs -= 1;
    if (tsdPtr->numRefs <= 0) {
        /*
         * When invoked, the readyChannels list in the IocpTsd should be empty
         * if the reference count drops to 0, else the function will panic as it
         * implies something's gone wrong in the reference counting.
         */
        if (tsdPtr->readyChannels.headPtr)
            Tcl_Panic("Attempt to free IocpTsd with channels attached.");
        if (lockAction == IOCP_LOCK_RELEASE)
            IocpTsdUnlock();
        ckfree(tsdPtr); /* Do outside of lock to minimize blocking */
    } else {
        if (lockAction == IOCP_LOCK_RELEASE)
            IocpTsdUnlock();
    }
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
        /* Have Drop release lock so memory freeing blocks for less time */
        IocpTsdDrop(tsdPtr, IOCP_LOCK_RELEASE);
    } else {
        IocpTsdUnlock();
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
    ret = (lockedChanPtr->vtblPtr->shutdown)(interp, lockedChanPtr, 
                                             TCL_CLOSE_READ|TCL_CLOSE_WRITE);

    /* Irrespective of errors in above call, we're done with this channel */
    lockedChanPtr->channel = NULL;
    IocpChannelDrop(lockedChanPtr); /* Drops ref count from Tcl channel */
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

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelThreadAction --
 *
 *    Called by the Tcl channel subsystem to attach or detach a channel
 *    to a thread. See Tcl_Channel manpage for expected semantics.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The channel is removed or added to a thread as per the request.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelThreadAction(
    ClientData instanceData,    /* IocpChannel * */
    int        action           /* Whether to remove or add */
    )
{
    IocpTsdPtr *tsdPtrPtr = IOCP_TSD_INIT(&iocpTsdDataKey);
    IocpTsd *tsdPtr;
    IocpChannel *chanPtr = (IocpChannel *)instanceData;

    IOCP_ASSERT(tsdPtrPtr != NULL);

    tsdPtr = *tsdPtrPtr;
    IOCP_ASSERT(tsdPtr != NULL);

    /* Locks must be obtained in the this order */
    IocpTsdLock();
    IocpChannelLock(chanPtr);

    if (action == TCL_CHANNEL_THREAD_INSERT) {
        IOCP_ASSERT(chanPtr->tsdPtr == NULL); /* Should not already be attached */
        IOCP_ASSERT((chanPtr->flags & IOCP_CHAN_F_ON_READYQ) == 0);

        chanPtr->tsdPtr = tsdPtr;
        tsdPtr->numRefs++;
        /*
         *If the channel has notification ready, ensure they are placed
         * the new thread's notification queue.
         */
        IocpChannelAddToReadyQ(chanPtr);

        IocpChannelUnlock(chanPtr);
        IocpTsdUnlock();
    } else {
        IOCP_ASSERT(chanPtr->tsdPtr == tsdPtr);
        chanPtr->tsdPtr = NULL;
        if (chanPtr->flags & IOCP_CHAN_F_ON_READYQ) {
            /*
             * There are two references to the tsd from this channel. One
             * from chanPtr->tsdPtr and one from being on the ready queue.
             */
            IOCP_ASSERT(tsdPtr->numRefs >= 2); /* chanPtr->tsdPtr and from readyq element */
            IocpListRemove(&tsdPtr->readyChannels, &chanPtr->readyLink);
            /*
             * Decrement ref for list removal above. No need for IocpTsdDrop
             * as ref count will not drop to zero. The drop for chanPtr->tsdPtr
             * happens a few lines below.
             */
            tsdPtr->numRefs -= 1;  /* Corresponding to ready queue removal */
            IocpChannelDrop(chanPtr); /* As no longer on ready queue */
        }
        else {
            IocpChannelUnlock(chanPtr);
        }
        IocpTsdDrop(tsdPtr, IOCP_LOCK_RELEASE);    /* Corresponding to chanPtr->tsdPtr */
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpEventSetup  --
 *
 *    Called by Tcl event loop to prepare for event checking.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    If any channels are ready on the thread, sets the blocking time
 *    for the event loop to not wait.
 *------------------------------------------------------------------------
 */
static void
IocpEventSetup(
    ClientData clientData,
    int flags)
{
    IocpTsdPtr *tsdPtrPtr;
    IocpTsd    *tsdPtr;

    if (!(flags & TCL_FILE_EVENTS)) {
	return;
    }

    tsdPtrPtr = IOCP_TSD_INIT(&iocpTsdDataKey);
    IOCP_ASSERT(tsdPtrPtr != NULL);

    tsdPtr = *tsdPtrPtr;
    IOCP_ASSERT(tsdPtr != NULL);

    IocpTsdLock();
    if (tsdPtr->readyChannels.headPtr != NULL) {
        Tcl_Time blockTime = {0, 0};
	Tcl_SetMaxBlockTime(&blockTime);
    }
    IocpTsdUnlock();
}

/*
 *-----------------------------------------------------------------------
 *
 * IocpEventCheck --
 *
 *    Called by Tcl event loop to queue any pending events.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *
 *    Events are added to the event queue.
 *-----------------------------------------------------------------------
 */
static void
IocpEventCheck (
    ClientData clientData,
    int flags)
{
    IocpTsdPtr *tsdPtrPtr;
    IocpTsd    *tsdPtr;
    int         pending_events;

    if (!(flags & TCL_FILE_EVENTS)) {
	return;
    }

    tsdPtrPtr = IOCP_TSD_INIT(&iocpTsdDataKey);
    IOCP_ASSERT(tsdPtrPtr != NULL);

    tsdPtr = *tsdPtrPtr;
    IOCP_ASSERT(tsdPtr != NULL);

    IocpTsdLock();
    pending_events = tsdPtr->readyChannels.headPtr != NULL;
    IocpTsdUnlock();
    if (pending_events) {
        Tcl_Event *evPtr = (Tcl_Event *) ckalloc(sizeof(Tcl_Event));
	evPtr->header.proc = IocpEventProcess;
	Tcl_QueueEvent(evPtr, TCL_QUEUE_TAIL);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpEventProcess --
 *
 *    Called from the event loop. Processes all ready channels and notifies
 *    the Tcl channel subsystem of state changes.
 *
 * Results:
 *    Returns 0 if the event is to be kept on the event loop queue, and
 *    1 if it can be discarded.
 *
 * Side effects:
 *    All channels marked as ready may change state. Tcl channel subsystem
 *    may be notified as appropriate.
 *
 *------------------------------------------------------------------------
 */
int IocpEventProcess(
    Tcl_Event *evPtr,           /* Pointer to the Tcl_Event structure.
                                 * Contents immaterial */
    int        flags            /* TCL_FILE_EVENTS */
    )
{
    IocpTsdPtr *tsdPtrPtr;
    IocpTsd    *tsdPtr;
    int         pending_events;

    if (!(flags & TCL_FILE_EVENTS)) {
        return 0;               /* We are not to process file/network events */
    }

    tsdPtrPtr = IOCP_TSD_INIT(&iocpTsdDataKey);
    IOCP_ASSERT(tsdPtrPtr != NULL);

    tsdPtr = *tsdPtrPtr;
    IOCP_ASSERT(tsdPtr != NULL);

    IocpTsdLock();
    TBD - loop over tsdPtr->readyChannels;
    IocpTsdUnlock();
    return 1;
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



