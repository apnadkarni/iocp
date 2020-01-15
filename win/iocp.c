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

static int  IocpEventHandler(Tcl_Event *evPtr, int flags);
static void IocpThreadExitHandler (ClientData clientData);

/* Used to notify the thread owning a channel of a completion on that channel */
typedef struct IocpTclEvent {
    Tcl_Event    event;         /* Must be first field */
    IocpChannel *chanPtr;       /* Channel associated with this event */
} IocpTclEvent;

/*
 * Static data
 */

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
int IocpDataBufferMoveOut(IocpDataBuffer *dataBuf, char *outPtr, int len)
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
    chanPtr->owningThread  = 0;
    chanPtr->channel = NULL;
    chanPtr->state   = IOCP_STATE_INIT;
    chanPtr->flags   = 0;
    chanPtr->pendingReads     = 0;
    chanPtr->pendingWrites    = 0;
    chanPtr->maxPendingReads  = IOCP_MAX_PENDING_READS_DEFAULT;
    chanPtr->maxPendingWrites = IOCP_MAX_PENDING_WRITES_DEFAULT;
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
 *    freed.
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
    IocpChannel *lockedChanPtr,  /* Must be locked and caller holding a reference
                                  * to ensure it does not go away even if unlocked
                                  */
    int          force          /* If true, queue even if already queued. */
    )
{
    if (lockedChanPtr->owningThread != 0) {
        /* Unless forced, only add to thread's event queue if not present. */
        if (force || (lockedChanPtr->flags & IOCP_CHAN_F_ON_EVENTQ) == 0) {
            IocpTclEvent *evPtr = ckalloc(sizeof(*evPtr));
            evPtr->event.proc = IocpEventHandler;
            evPtr->chanPtr    = lockedChanPtr;
            lockedChanPtr->numRefs++; /* Corresponding to above,
                                         reversed by IocpEventHandler */
            Tcl_ThreadQueueEvent(lockedChanPtr->owningThread,
                                 (Tcl_Event *)evPtr, TCL_QUEUE_TAIL);
            Tcl_ThreadAlert(lockedChanPtr->owningThread);
        }
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
            IocpChannelNotifyCompletion(chanPtr, 0);
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
    iocpModuleState.initialized = 1;

    Tcl_CreateExitHandler(IocpProcessExitHandler, NULL);

    return TCL_OK;
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
    IocpChannel *chanPtr = (IocpChannel*)instanceData;
    int          ret;

    IocpChannelLock(chanPtr);

    /*
     * Before this function is called, IocpThreadAction should have been
     * called to remove the channel from all threads. However, just for
     * the record in case of future changes, we do not need to remove from
     * the TSD ready queue in any case since when the ready notification
     * is dequeued a check is made to see if channel is already detached.
     */

    /* Call specific IOCP type to close OS handles */
    ret = (chanPtr->vtblPtr->shutdown)(interp, chanPtr, 
                                             TCL_CLOSE_READ|TCL_CLOSE_WRITE);

    /* Irrespective of errors in above call, we're done with this channel */
    chanPtr->channel = NULL;
    IocpChannelDrop(chanPtr); /* Drops ref count from Tcl channel */
    /* Do NOT refer to chanPtr beyond this point */

    return ret;
}

static int
IocpChannelInput (
    ClientData instanceData,	/* IocpChannel */
    char      *outPtr,          /* Where to store data. */
    int        maxReadCount,    /* Maximum number of bytes to read. */
    int       *errorCodePtr)	/* Where to store error codes. */
{
    IocpChannel *chanPtr = (IocpChannel*)instanceData;
    int          bytesRead = 0;
    int          remaining;
    DWORD        winError;

    *errorCodePtr = 0;
    IocpChannelLock(chanPtr);

    if (chanPtr->state == IOCP_STATE_CONNECTING) {
        if (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) {
            *errorCodePtr = EWOULDBLOCK;
            bytesRead = -1;
            goto vamoose;
        }
        /* Channel is blocking and we are awaiting async connect to complete */
        IocpChannelAwaitCompletion(chanPtr);
        /* State may have changed ! */
    }
    /*
     * Unless channel is marked as write-only (via shutdown) pass up data
     * irrespective of state. TBD - is this needed or does channel ensure this?
     */
    if (chanPtr->flags & IOCP_CHAN_F_WRITEONLY) {
        goto vamoose; /* bytesRead is already 0 indicating EOF */
    }

    /*
     * Now we get to copy data out from our buffers to the Tcl buffer.
     * Note that a zero length input buffer signifies EOF.
     */
    if (chanPtr->inputBuffers.headPtr == NULL) {
        /* No input buffers. */
        if (chanPtr->state != IOCP_STATE_OPEN ||
            (chanPtr->flags & IOCP_CHAN_F_REMOTE_EOF)) {
            /* Connection not OPEN or closed from remote end */
            goto vamoose; /* bytesRead is already 0 indicating EOF */
        }
        /* If non-blocking, just return. */
        if (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) {
            *errorCodePtr = EWOULDBLOCK;
            bytesRead     = -1;
            goto vamoose;
        }
        /* Blocking connection. Wait for incoming data or eof */
        winError = IocpChannelPostReads(chanPtr); /* Ensure read is posted */
        if (winError != 0) {
            bytesRead = -1;
            IocpSetTclErrnoFromWin32(winError);
            *errorCodePtr = Tcl_GetErrno();
            goto vamoose;
        }
        /* Wait for a posted read to complete */
        IocpChannelAwaitCompletion(chanPtr);
        /* Fall through for processing */
    }

    /*
     * At this point, a NULL inputBuffers queue implies error or eof. Note
     * there is no need to check channel state. As long as there is input data
     * we will pass it up.
     */
    remaining = maxReadCount;
    while (remaining && chanPtr->inputBuffers.headPtr) {
        IocpBuffer *bufPtr = CONTAINING_RECORD(chanPtr->inputBuffers.headPtr, IocpBuffer, link);
        int numCopied;
        winError = bufPtr->winError;
        if (winError == 0) {
            numCopied = IocpBufferMoveOut(bufPtr, outPtr, remaining);
            outPtr    += numCopied;
            remaining -= numCopied;
            bytesRead += numCopied;
            if (IocpBufferLength(bufPtr) == 0) {
                /* No more bytes in that buffer. Free it after removing from list */
                IocpListPopFront(&chanPtr->inputBuffers);
                /* TBD - optimize to reuse buffer for next receive */
                IocpBufferFree(bufPtr);
            }
            if (numCopied == 0) {
                chanPtr->flags |= IOCP_CHAN_F_REMOTE_EOF;
                break;
            }
        }
        else {
            /*
             * Buffer indicates an error. If we already have some data, pass it
             * up. Keep the buffer on the queue so it is handled on the next call.
             */
            if (bytesRead > 0)
                break;
            IocpListPopFront(&chanPtr->inputBuffers);
            IocpBufferFree(bufPtr);
            if (winError == WSAECONNRESET) {
                /* Treat as EOF, not error */
                chanPtr->flags |= IOCP_CHAN_F_REMOTE_EOF;
                break;
            }
            bytesRead = -1;
            /* TBD - should we try to distinguish transient errors ? */
            IocpSetTclErrnoFromWin32(winError);
            *errorCodePtr = Tcl_GetErrno();
            goto vamoose;
        }
    }

    if ((chanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) == 0 &&
        chanPtr->state == IOCP_STATE_OPEN) {
        /* No remote EOF seen, post additional reads. */
        winError = IocpChannelPostReads(chanPtr);
        /*
         * Ignore errors if we are returning some data. If not transient,
         * it will be handled eventually on future reads.
         */
        if (winError != 0 && bytesRead == 0) {
            bytesRead = -1;
            IocpSetTclErrnoFromWin32(winError);
            *errorCodePtr = Tcl_GetErrno();
        }
    }

vamoose:
    /*
     * When jumping here,
     * IocpChannel should be locked.
     * bytesRead should contain return value:
     *  0  - eof
     *  -1 - error, *errorCodePtr should contain POSIX error.
     *  >0 - bytes read
     */
    IocpChannelUnlock(chanPtr);
    return bytesRead;
}

static int
IocpChannelOutput (
    ClientData instanceData,	/* IocpChannel pointer */
    CONST char *bufPtr,		/* Buffer */
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
    IocpChannel *chanPtr = (IocpChannel*)instanceData;

    IocpChannelLock(chanPtr);
    if (mode == TCL_MODE_NONBLOCKING) {
        chanPtr->flags |= IOCP_CHAN_F_NONBLOCKING;
    } else {
        chanPtr->flags &= ~ IOCP_CHAN_F_NONBLOCKING;
    }
    IocpChannelUnlock(chanPtr);
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
    IocpChannel *chanPtr = (IocpChannel *)instanceData;
    IocpChannelLock(chanPtr);

    if (action == TCL_CHANNEL_THREAD_INSERT) {
        chanPtr->owningThread = Tcl_GetCurrentThread();
        /*
         * Notify in case any I/O completion notifications pending. No harm
         * if there aren't
         */
        IocpChannelNotifyCompletion(chanPtr, 1);
    } else {
        chanPtr->owningThread = 0;
    }
    IocpChannelUnlock(chanPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpEventHandler --
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
int IocpEventHandler(
    Tcl_Event *evPtr,           /* Pointer to the Tcl_Event structure.
                                 * Contents immaterial */
    int        flags            /* TCL_FILE_EVENTS */
    )
{
    IocpChannel *chanPtr;
    Tcl_Channel  channel;
    int          readyMask = 0; /* Which ready notifications to pass to Tcl */

    if (!(flags & TCL_FILE_EVENTS)) {
        return 0;               /* We are not to process file/network events */
    }

    chanPtr = ((IocpTclEvent *)evPtr)->chanPtr;
    IocpChannelLock(chanPtr);
    channel = chanPtr->channel;

    switch (chanPtr->state) {
    case IOCP_STATE_OPEN:
        /*
         * If Tcl wants to be notified of input and has not shutdown the read
         * side, notify if either data is available or EOF has been reached.
         */
        if ((chanPtr->flags & IOCP_CHAN_F_WATCH_INPUT) &&
            !(chanPtr->flags & IOCP_CHAN_F_WRITEONLY) &&
            ((chanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) ||
             chanPtr->inputBuffers.headPtr)) {
            readyMask |= TCL_READABLE;
        }
        /* On the write side, inform if limit on pending writes not exceeded */
        if ((chanPtr->flags & IOCP_CHAN_F_WRITE_DONE) &&
            chanPtr->pendingWrites < chanPtr->maxPendingWrites) {
            // TBD - also on remote eof?
            readyMask |= TCL_WRITABLE;
            chanPtr->flags &= ~ IOCP_CHAN_F_WRITE_DONE;
        }
        break;
    }

    /*
     * Drop the reference corresponding to queueing to the event q
     * Do this before calling Tcl_NotifyChannel which may recurse (?)
     * Note the IocpChannel will not be freed as long as the Tcl channel
     * system still has a reference to it.
     */
    ((IocpTclEvent *)evPtr)->chanPtr = NULL;
    IocpChannelDrop(chanPtr);

    if (channel != NULL && readyMask != 0)
        Tcl_NotifyChannel(channel, readyMask);

    return 1;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelPostReads --
 *
 *    Calls the channel specific code to post reads to the OS handle
 *    up to the maximum allowed to be outstanding for the channel or until
 *    the posting fails. In the latter case, an error is returned only
 *    if no reads are outstanding.
 *
 * Results:
 *    0 on success or a Windows error code.
 *
 * Side effects:
 *    Reads are posted up to the limit for the channel and the pending
 *    reads count in the IocpChannel is updated accordingly.
 *
 *------------------------------------------------------------------------
 */
DWORD IocpChannelPostReads(
    IocpChannel *lockedChanPtr  /* Must be locked */
    )
{
    DWORD winError = 0;
    while (lockedChanPtr->pendingReads < lockedChanPtr->maxPendingReads) {
        winError = lockedChanPtr->vtblPtr->postread(lockedChanPtr);
        if (winError)
            break;
    }
    return (lockedChanPtr->pendingReads > 0) ? 0 : winError;
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

    Tcl_CreateObjCommand(interp, "iocp::socket", Iocp_SocketObjCmd, 0L, 0L);
    Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION);
    return TCL_OK;
}



/*
 * TBD - design change - would it be faster to post zero-byte read and then
 * copy from WSArecv to channel buffer in InputProc ? That may potentially
 * save one buffer copy (from IocpBuffer to channel buffer) although we
 * may potentially incur a copy from the packet into the kernel buffer since
 * we are not supplying a buffer. Zero byte read also has the disadvantage
 * of an additional kernel transition. On the other hand, it is less memory
 * intensive. See https://microsoft.public.win32.programmer.networks.narkive.com/FRa81Gzo/about-zero-byte-receive-iocp-server
 * for a discussion.
 *
 * Can only resolve by trying both and measuring.
 */
