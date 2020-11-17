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

static void IocpNotifyChannel(IocpChannel *lockedChanPtr);
static int  IocpEventHandler(Tcl_Event *evPtr, int flags);
static int IocpChannelFileEventMask(IocpChannel *lockedChanPtr);
static void IocpChannelConnectionStep(IocpChannel *lockedChanPtr, int blockable);
static void IocpChannelExitConnectedState(IocpChannel *lockedChanPtr);
static void IocpChannelAwaitConnectCompletion(IocpChannel *lockedChanPtr);

Tcl_ObjCmdProc	Iocp_DebugOutObjCmd;
Tcl_ObjCmdProc	Iocp_StatsObjCmd;

/*
 * Static data
 */

/* Holds global IOCP state */
IocpSubSystem iocpModuleState;

/* Statistics */
IocpStats iocpStats;

#ifdef IOCP_ENABLE_TRACE
/* Enable/disable tracing */
int iocpEnableTrace = IOCP_ENABLE_TRACE;

/*
 * GUID format for traceview
 * 3a674e76-fe96-4450-b634-24fc587b2828
 */
TRACELOGGING_DEFINE_PROVIDER(
    iocpWinTraceProvider,
    "SimpleTraceLoggingProvider",
    (0x3a674e76, 0xfe96, 0x4450, 0xb6, 0x34, 0x24, 0xfc, 0x58, 0x7b, 0x28, 0x28));
#endif

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
        else {
            IOCP_STATS_INCR(IocpDataBufferAllocs);
        }
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
    if (dataBufPtr->bytes) {
        IOCP_STATS_INCR(IocpDataBufferFrees);
        ckfree(dataBufPtr->bytes);
    }
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
IocpBuffer *IocpBufferNew(
    int          capacity,        /* Capacity requested */
    enum IocpBufferOp op,         /* IOCP_BUFFER_OP_READ etc. */
    int          flags            /* IOCP_BUFFER_F_WINSOCK => wsaOverlap header,
                                  *  otherwise Overlap header */
    )
{
    IocpBuffer *bufPtr = attemptckalloc(sizeof(*bufPtr));
    if (bufPtr == NULL)
        return NULL;
    IOCP_STATS_INCR(IocpBufferAllocs);

    memset(&bufPtr->u, 0, sizeof(bufPtr->u));

    bufPtr->chanPtr   = NULL; /* Not included in param 'cause then we have to
                                 worry about reference counts etc.*/
    bufPtr->winError  = 0;
    bufPtr->operation = op;
    bufPtr->flags     = flags;
    IocpLinkInit(&bufPtr->link);

    if (IocpDataBufferInit(&bufPtr->data, capacity) == NULL && capacity != 0) {
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
    IOCP_STATS_INCR(IocpBufferFrees);
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
 *    Returns a pointer to the allocated IocpChannel or NULL on failure.
 *
 * Side effects:
 *    Will cause process exit if memory allocation fails.
 *
 *------------------------------------------------------------------------
 */
IocpChannel *IocpChannelNew(
    const IocpChannelVtbl *vtblPtr) /* See IocpChannelVtbl definition comments.
                                     * Must point to static memory */
{
    IocpChannel *chanPtr;
    chanPtr          = ckalloc(vtblPtr->allocationSize);
    IOCP_STATS_INCR(IocpChannelAllocs);
    IocpListInit(&chanPtr->inputBuffers);
    chanPtr->owningThread  = 0;
    chanPtr->channel  = NULL;
    chanPtr->state    = IOCP_STATE_INIT;
    chanPtr->flags    = 0;
    chanPtr->winError = 0;
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
        IocpLink *linkPtr;

        if (lockedChanPtr->vtblPtr->finalize)
            lockedChanPtr->vtblPtr->finalize(lockedChanPtr);

        /* If finalize did not free buffers, do our default frees. */
        while ((linkPtr = IocpListPopFront(&lockedChanPtr->inputBuffers)) != NULL) {
            IocpBuffer  *bufPtr = CONTAINING_RECORD(linkPtr, IocpBuffer, link);
            IocpBufferFree(bufPtr);
        }

        IocpChannelUnlock(lockedChanPtr);
        IocpLockDelete(&lockedChanPtr->lock);
        IOCP_STATS_INCR(IocpChannelFrees);
        ckfree(lockedChanPtr);
    }
    else {
        IocpChannelUnlock(lockedChanPtr);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelConnectionStep --
 *
 *    Executes one step in a async connection. lockedChanPtr must be in one
 *    of the connecting states, CONNECTING, CONNECT_RETRY or CONNECTED.
 *
 *    If the blockable parameter is true, the function will return when the
 *    connection is open or fails completely. The channel will then be in OPEN,
 *    CONNECT_FAILED or DISCONNECTED states.
 *
 *    If blockable is false, the function will transition the channel to
 *    the next appropriate state if possible or remain in the same state.
 *    A CONNECTING state will remain as is as it indicates a connection attempt
 *    is still in progress and we just need to wait for it to complete. A
 *    CONNECT_RETRY state indicates the completion thread signalled the previous
 *    attempt failed. In this case, a new attempt is initiated if there are
 *    additional addresses to try and the channel is transitioned to CONNECTING.
 *    If not, the channel state is set to CONNECT_FAILED. Finally, a CONNECTED
 *    state indicates the completion thread signalled a successful connect, and
 *    the channel state is set to OPEN.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The channel state may change. If a blocking channel, the function may
 *    block until connection completes or fails. In case of errors,
 *    lockedChanPtr->winError is set. If channel state transitions to OPEN,
 *    reads are posted on socket and Tcl channel is notified if required.
 *
 *------------------------------------------------------------------------
 */
static void IocpChannelConnectionStep(
    IocpChannel *lockedChanPtr, /* Must be locked, will be locked on return, may
                                 * be unlocked interim. Caller must be holding reference
                                 * to ensure it is not deallocated */
    int blockable)              /* If true, the function is allowed to block. */
{
    IOCP_TRACE(("IocpChannelConnectionStep Enter: lockedChanPtr=%p, blockable=%d, state=0x%x\n", lockedChanPtr, blockable, lockedChanPtr->state));

    switch (lockedChanPtr->state) {
    case IOCP_STATE_CONNECTED:
        /* IOCP thread has already signalled completion, transition to OPEN */
        IocpChannelExitConnectedState(lockedChanPtr);
        break;
    case IOCP_STATE_CONNECTING:
        /*
         * If blockable we just wait for connection to complete. If non-blockable,
         * nought to do until the IOCP thread completes the connection.
         */
        if (blockable) {
            /* Note this will also call IocpChannelExitConnectedState if needed */
            IocpChannelAwaitConnectCompletion(lockedChanPtr);
        }
        break;
    case IOCP_STATE_CONNECT_RETRY:
        if (blockable) {
            if (lockedChanPtr->vtblPtr->blockingconnect) {
                lockedChanPtr->vtblPtr->blockingconnect(lockedChanPtr);
                /* Don't care about success. Caller responsible to check state */
                IocpNotifyChannel(lockedChanPtr);
            }
        } else {
            if (lockedChanPtr->vtblPtr->connectfailed  == NULL ||
                lockedChanPtr->vtblPtr->connectfailed(lockedChanPtr) != 0) {
                /* No means to retry or retry failed. lockedChanPtr->winError is error */
                lockedChanPtr->state = IOCP_STATE_CONNECT_FAILED;
                lockedChanPtr->flags |= IOCP_CHAN_F_REMOTE_EOF;
                IocpNotifyChannel(lockedChanPtr);
            }
            else {
                /* Revert to CONNECTING state when retry ongoing */
                lockedChanPtr->state = IOCP_STATE_CONNECTING;
            }
        }
        break;

    default:
        Iocp_Panic("IocpChanConnectionStep: unexpected state %d",
                  lockedChanPtr->state);
        break;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelExitConnectedState --
 *
 *    Called when a connection has completed. Calls the channel type-specific
 *    handler and depending on its return value, transitions into OPEN
 *    or DISCONNECTED state. If channel event notifications are registered,
 *    the callbacks may further change state before this connection returns.
 *
 *    In the case Tcl has to be notified, lockedChanPtr has to be unlocked.
 *    before Tcl_NotifyChannel is called. It is then relocked before returning.
 *    It is up to the caller to ensure the memory is till valid by holding
 *    a reference.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    If registered, channel file events are queued.
 *
 *------------------------------------------------------------------------
 */
static void IocpChannelExitConnectedState(
    IocpChannel *lockedChanPtr
    )
{
    if (lockedChanPtr->vtblPtr->connected &&
        lockedChanPtr->vtblPtr->connected(lockedChanPtr) != 0) {
        lockedChanPtr->state = IOCP_STATE_DISCONNECTED;
    } else {
        lockedChanPtr->state    = IOCP_STATE_OPEN;
        /* Clear any errors stored while cycling through address list */
        lockedChanPtr->winError = ERROR_SUCCESS;
        IocpChannelPostReads(lockedChanPtr);
    }
    lockedChanPtr->flags |= IOCP_CHAN_F_WRITE_DONE; /* In case registered file event */
    IocpNotifyChannel(lockedChanPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelAwaitConnectCompletion --
 *
 *    Waits for an outgoing connection request to complete. Must be called only
 *    on a blocking channel as the function will block. The locked channel will
 *    be unlocked while waiting and may change state.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    chanPtr state is changed depending on whether connection completed
 *    successfully or not.
 *
 *------------------------------------------------------------------------
 */
static void IocpChannelAwaitConnectCompletion(
    IocpChannel *lockedChanPtr  /* Must be locked on entry and will be locked
                                 * on return. But may be unlocked and changed
                                 * state in between. */
    )
{
    IOCP_TRACE(("IocpChannelAwaitConnectCompletion Enter: lockedChanPtr=%p\n", lockedChanPtr));

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_CONNECTING);
    IOCP_ASSERT((lockedChanPtr->flags & IOCP_CHAN_F_NONBLOCKING) == 0);

    IocpChannelAwaitCompletion(lockedChanPtr, IOCP_CHAN_F_BLOCKED_CONNECT);

    if (lockedChanPtr->state == IOCP_STATE_CONNECT_RETRY) {
        /* Retry connecting in blocking mode if possible */
        if (lockedChanPtr->vtblPtr->blockingconnect) {
            lockedChanPtr->vtblPtr->blockingconnect(lockedChanPtr);
            /* Don't care about success. Caller responsible to check state */
        }
    }
    if (lockedChanPtr->state == IOCP_STATE_CONNECTED) {
        IocpChannelExitConnectedState(lockedChanPtr);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelAwaitCompletion --
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
void IocpChannelAwaitCompletion(
    IocpChannel *lockedChanPtr,    /* Must be locked on entry */
    int          blockType)        /* Exactly one of IOCP_CHAN_F_BLOCKED_* values  */
{
    IOCP_TRACE(("IocpChannelAwaitCompletion Enter: lockedChanPtr=%p, blockType=%d\n", lockedChanPtr, blockType));
    lockedChanPtr->flags &= ~ IOCP_CHAN_F_BLOCKED_MASK;
    lockedChanPtr->flags |= blockType;
    IocpChannelCVWait(lockedChanPtr);
    IOCP_TRACE(("IocpChannelAwaitCompletion Leave: lockedChanPtr=%p, blockType=%d\n", lockedChanPtr, blockType));
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelWakeAfterCompletion --
 *
 *    Wakes up a thread (if any) blocked on some I/O operation to complete.
 *
 * Results:
 *    Returns 1 if a blocked thread was woken, else 0.
 *
 * Side effects:
 *    The thread (if any) blocked on the channel is awoken.
 *
 *------------------------------------------------------------------------
 */
int IocpChannelWakeAfterCompletion(
    IocpChannel *lockedChanPtr,   /* Must be locked on entry */
    int          blockMask)      /* Combination of IOCP_CHAN_F_BLOCKED*. Channel
                                  * thread will be woken if waiting on any of these */

{
    IOCP_TRACE(("IocpChannelWakeAfterCompletion Enter: lockedChanPtr=%p, blockMask=0x%x, lockedChanPtr->flags=0x%x\n", lockedChanPtr, blockMask, lockedChanPtr->flags));
    /* Checking the flag, saves a potential unnecessary kernel transition */
    if (lockedChanPtr->flags & blockMask) {
        lockedChanPtr->flags &= ~blockMask;
        IOCP_TRACE(("IocpChannelWakeAfterCompletion: waking condition variable\n"));
        WakeConditionVariable(&lockedChanPtr->cv);
        return 1;
    }
    else {
        IOCP_TRACE(("IocpChannelWakeAfterCompletion: Not waking condition variable\n"));
        return 0;
    }
}

/*
 * IocpEventTimerCallback -
 *
 *    Called back from Tcl timer routines which is used as a trampoline to
 *    to call into the IOCP channel event handler. See IocpChannelEnqueueEvent.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Calls the channel event handler or enqueues another call through the
 *    event loop.
 */
static void IocpEventTimerCallback(ClientData clientdata)
{
    IocpTclEvent *evPtr = (IocpTclEvent *) clientdata;
    IocpChannel  *chanPtr;

    IOCP_TRACE(("IocpEventTimerCallback: Enter\n"));

    /*
     * TBD - we could directly call IocpEventHandler here. However,
     * we do not know if TCL_FILE_EVENTS flag was set in the event loop to
     * allow processing of file / socket events. So we just queue back
     * through the event loop. Not the most efficient.
     */

    /*
     * Note evPtr->chanPtr is safe to access as it was incr-ref'ed when queued
     */
    chanPtr = evPtr->chanPtr;
    IocpChannelLock(chanPtr);
    chanPtr->flags &= ~IOCP_CHAN_F_ON_TIMERQ;
    if ((chanPtr->flags & IOCP_CHAN_F_ON_EVENTQ) == 0) {
        Tcl_ThreadId tid = chanPtr->owningThread;
        chanPtr->flags |= IOCP_CHAN_F_ON_EVENTQ;
        IocpChannelUnlock(chanPtr);
        /* Optimization if enqueuing to current thread */
        // TBD - what if tid is 0 (channel not attached to any thread)
        if (Tcl_GetCurrentThread() == tid) {
            IOCP_TRACE(("IocpEventTimerCallback: queuing to current thread\n"));
            Tcl_QueueEvent((Tcl_Event *) evPtr, TCL_QUEUE_TAIL);
        }
        else {
            IOCP_TRACE(("IocpEventTimerCallback: queuing to another thread\n"));
            Tcl_ThreadQueueEvent(tid,
                                 (Tcl_Event *)evPtr, TCL_QUEUE_TAIL);
            Tcl_ThreadAlert(tid);
        }
    }
    else {
        IocpChannelUnlock(chanPtr);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelEnqueueEvent --
 *
 *    Enqueues a event on Tcl's event queue to notify the thread owning a
 *    channel.
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
void IocpChannelEnqueueEvent(
    IocpChannel *lockedChanPtr,  /* Must be locked and caller holding a reference
                                  * to ensure it does not go away even if unlocked */
    enum IocpEventReason reason, /* Indicates reason for notification */
    int          force           /* If true, queue even if already queued. */
    )
{
    IOCP_TRACE(("IocpChannelEnqueueEvent Enter: lockedChanPtr=%p, reason=%d, force=%d, lockedChanPtr->owningThread=%d, lockedChanPtr->flags=0x%x\n", lockedChanPtr, reason, force, lockedChanPtr->owningThread, lockedChanPtr->flags));
    if (lockedChanPtr->owningThread != 0) {
        /* Unless forced, only add to thread's event queue if not present. */
        if (force || (lockedChanPtr->flags & IOCP_CHAN_F_ON_EVENTQ) == 0) {
            IocpTclEvent *evPtr = ckalloc(sizeof(*evPtr));
            evPtr->event.proc = IocpEventHandler;
            evPtr->chanPtr    = lockedChanPtr;
            evPtr->reason     = reason;
            lockedChanPtr->numRefs++; /* Corresponding to above,
                                         reversed by IocpEventHandler */
            /*
             * If called from the thread owning the channel, call back through
             * the timer, otherwise we run the risk of continually generating
             * events without letting code scheduled via [after] in fileevent
             * callbacks run. See Bug 191 in twapi.
             */
            if (Tcl_GetCurrentThread() == lockedChanPtr->owningThread) {
                if ((lockedChanPtr->flags & IOCP_CHAN_F_ON_TIMERQ) == 0) {
                    lockedChanPtr->flags |= IOCP_CHAN_F_ON_TIMERQ;
                    Tcl_CreateTimerHandler(0, IocpEventTimerCallback, evPtr);
                }
            }
            else {
                lockedChanPtr->flags |= IOCP_CHAN_F_ON_EVENTQ;
                IOCP_TRACE(("IocpChannelEnqueueEvent: queuing to another thread\n"));
                Tcl_ThreadQueueEvent(lockedChanPtr->owningThread,
                                     (Tcl_Event *)evPtr, TCL_QUEUE_TAIL);
                Tcl_ThreadAlert(lockedChanPtr->owningThread);
            }
        }
        else {
            IOCP_TRACE(("IocpChannelEnqueEvent: not queueing as already queued\n"));
        }
    }
    else {
        IOCP_TRACE(("IocpChannelEnqueEvent: owningThread==0\n"));
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelNudgeThread --
 *
 *    Wakes up the thread owning a channel if it is blocked or notifies
 *    it via the event loop if it isn't.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    An event is queued or the condition variable being waited on is poked. Due
 *    to the lock hierarchy rules, *lockedChanPtr may be unlocked and relocked
 *    before returning and its state may have therefore changed.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelNudgeThread(
    IocpChannel *lockedChanPtr,  /* Must be locked and caller holding a reference
                                  * to ensure it does not go away even if unlocked
                                  */
    int          blockMask,      /* Combination of IOCP_CHAN_F_BLOCKED*. Channel
                                  * thread will be woken if waiting on any of these */
    int          forceEvent)     /* If true, force an event notification even if
                                  * thread was blocked and is woken up or if event
                                  * notification was already queued. Normally,
                                  * the function will not queue an event in these
                                  * cases. */
{
    IOCP_TRACE(("IocpChannelNudgeThread Enter: lockedChanPtr=%p, blockMask=%d, forceEvent=%d, lockedChanPtr->state=0x%x, lockedChanPtr->flags=0x%x\n", lockedChanPtr, blockMask, forceEvent, lockedChanPtr->state, lockedChanPtr->flags));
    if (! IocpChannelWakeAfterCompletion(lockedChanPtr, blockMask) || forceEvent) {
        IOCP_TRACE(("IocpChannelNudgeThread: checking if EnqueueEvent to be called\n"));
        /*
         * Owning thread was not woken, either it was not blocked for the
         * right reason or was not blocked at all. Need to notify
         * it via the ready queue/event loop.
         */
        if ((lockedChanPtr->flags & (IOCP_CHAN_F_WATCH_ACCEPT | 
                                     IOCP_CHAN_F_WATCH_INPUT |
                                     IOCP_CHAN_F_WATCH_OUTPUT)) ||
            forceEvent) {
            IocpChannelEnqueueEvent(lockedChanPtr, IOCP_EVENT_IO_COMPLETED, forceEvent);
        }
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpCompleteConnect --
 *
 *    Handles completion of connect operations from IOCP.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The channel state is changed to OPEN or CONNECT_RETRY depending
 *    on buffer status. The passed bufPtr is freed and lockedChanPtr
 *    dropped. The Tcl thread is notified via the event queue or woken
 *    up if blocked.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompleteConnect(
    IocpChannel *lockedChanPtr, /* Locked channel, will be dropped */
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    switch (lockedChanPtr->state) {
    case IOCP_STATE_CONNECTING:
        lockedChanPtr->winError = bufPtr->winError;
        lockedChanPtr->state = bufPtr->winError == 0 ? IOCP_STATE_CONNECTED : IOCP_STATE_CONNECT_RETRY;
        /*
         * Note last param is 1, forcing a thread to be notified even if no
         * WATCH flags are set. This is for async connects where Tcl thread
         * does not do a gets/read but only fconfigure -error.
         */
        IocpChannelNudgeThread(lockedChanPtr, IOCP_CHAN_F_BLOCKED_CONNECT, 1);
        break;

    case IOCP_STATE_CLOSED: /* Ignore, nothing to be done */
        break;

    default: /* TBD - should not happen. How to report logic error */
        break;
    }

    bufPtr->chanPtr = NULL;
    IocpChannelDrop(lockedChanPtr); /* Corresponding to bufPtr->chanPtr */
    IocpBufferFree(bufPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpCompleteDisconnect --
 *
 *    Handles completion of disconnect operations from IOCP.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The associated socket is closed, the passed bufPtr is freed
 *    and lockedChanPtr
 *    dropped. The Tcl thread is notified via the event queue or woken
 *    up if blocked.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompleteDisconnect(
    IocpChannel *lockedChanPtr, /* Locked channel, will be dropped */
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    if (lockedChanPtr->vtblPtr->disconnected) {
        lockedChanPtr->vtblPtr->disconnected(lockedChanPtr);
    }
    bufPtr->chanPtr = NULL;
    IocpChannelDrop(lockedChanPtr); /* Corresponding to bufPtr->chanPtr */
    IocpBufferFree(bufPtr);
}


/*
 *------------------------------------------------------------------------
 *
 * IocpCompleteAccept --
 *
 *    Handles completion of connection accept operations from IOCP.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The passed bufPtr is freed or enqueued on the owning IocpChannel. The
 *    lockedChanPtr is dropped and the Tcl thread notified via the event loop.
 *    If the Tcl thread is blocked on this channel, it is woken up.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompleteAccept(
    IocpChannel *lockedChanPtr, /* Locked channel, will be dropped */
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    IOCP_TRACE(("IocpCompleteAccept Enter: lockedChanPtr=%p. state=0x%x\n", lockedChanPtr, lockedChanPtr->state));

    /*
     * Reminder: unlike reads/writes, the count of pending accept operations
     * is on a per listening socket basis, not per channel. Since we don't
     * deal with channel-type specific operations here, the count is
     * decremented in the channel-specific code in the Tcl thread.
     *
     * So also, deal with the channel states in the Tcl thread. Don't
     * bother with them here.
     *
     * TBD - may be make reads/writes the same for consistency?
     */

    /*
     * Add the buffer to the input queue which also doubles as the accept queue
     * since listeners are disabled for read and write at the channel level.
     * Then if the channel was blocked, awaken the sleeping thread. Otherwise
     * send it a Tcl event notification.
     */
    IocpListAppend(&lockedChanPtr->inputBuffers, &bufPtr->link);
    bufPtr->chanPtr = NULL;
    /*
     * chanPtr->numRefs-- because bufPtr does not refer to it (though it is on
     *                    the inputBuffers queue, that is immaterial)
     * chanPtr->numRefs++ because we still want to access chanPtr below after
     *                    unlocking and relocking.
     * The two cancel out. The latter will be reversed at function exit.
     */

    IocpChannelNudgeThread(lockedChanPtr, 0, 0);

    /* This drops the reference from bufPtr which was delayed (see above) */
    IocpChannelDrop(lockedChanPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpCompleteRead --
 *
 *    Handles completion of read operations from IOCP.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The passed bufPtr is freed or enqueued on the owning IocpChannel. The
 *    lockedChanPtr is dropped and the Tcl thread notified via the event loop.
 *    If the Tcl thread is blocked on this channel, it is woken up.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompleteRead(
    IocpChannel *lockedChanPtr, /* Locked channel, will be dropped */
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    IOCP_TRACE(("IocpCompleteRead Enter: lockedChanPtr=%p. state=0x%x\n", lockedChanPtr, lockedChanPtr->state));

    IOCP_ASSERT(lockedChanPtr->pendingReads > 0);
    lockedChanPtr->pendingReads--;

    if (lockedChanPtr->state == IOCP_STATE_CLOSED) {
        bufPtr->chanPtr = NULL;
        IocpChannelDrop(lockedChanPtr); /* Corresponding to bufPtr->chanPtr */
        IocpBufferFree(bufPtr);
        return;
    }

    /*
     * Add the buffer to the input queue. Then if the channel was blocked,
     * awaken the sleeping thread. Otherwise send it a Tcl event notification.
     */
    IocpListAppend(&lockedChanPtr->inputBuffers, &bufPtr->link);
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
     * Tcl thread since in any case it has to be notified of closure. So
     * also any errors indicated by bufPtr->winError.
     */
    IocpChannelNudgeThread(lockedChanPtr, IOCP_CHAN_F_BLOCKED_READ, 0);

    /* This drops the reference from bufPtr which was delayed (see above) */
    IocpChannelDrop(lockedChanPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpCompleteWrite --
 *
 *    Handles completion of Write operations from IOCP.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The passed bufPtr is freed and lockedChanPtr dropped. The Tcl thread
 *    notified via the event loop. If the Tcl thread is blocked on this channel,
 *    it is woken up.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompleteWrite(
    IocpChannel *lockedChanPtr, /* Locked channel, will be dropped */
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    IOCP_ASSERT(lockedChanPtr->pendingWrites > 0);
    lockedChanPtr->pendingWrites--;

    bufPtr->chanPtr = NULL;
    IocpBufferFree(bufPtr);

    if (lockedChanPtr->state != IOCP_STATE_CLOSED) {
        lockedChanPtr->flags |= IOCP_CHAN_F_WRITE_DONE;
        /* TBD - optimize under which conditions we need to nudge thread */
        IocpChannelNudgeThread(lockedChanPtr, IOCP_CHAN_F_BLOCKED_WRITE, 0);
    }
    IocpChannelDrop(lockedChanPtr); /* Corresponding to bufPtr->chanPtr */
}

static DWORD WINAPI
IocpCompletionThread (LPVOID lpParam)
{
    IocpWinError winError = 0;

#ifdef _MSC_VER
    __try {
#endif
        while (1) {
            IocpBuffer *bufPtr;
            HANDLE      iocpPort = (HANDLE) lpParam;
            DWORD       nbytes;
            ULONG_PTR   key;
            OVERLAPPED *overlapPtr;
            BOOL        ok;
            IocpChannel *chanPtr;

            ok = GetQueuedCompletionStatus(iocpPort, &nbytes, &key,
                                           &overlapPtr, INFINITE);
            IOCP_TRACE(("IocpCompletionThread: GetQueuedCompletionStatus returned %d, overlapPtr=%p\n", ok, overlapPtr));
            if (overlapPtr == NULL) {
                /* If ok, exit signal. Else some error */
                if (! ok) {
                    /* TBD - how to signal or log error? */
                }
                break;          /* Vamoose */
            }
            bufPtr = CONTAINING_RECORD(overlapPtr, IocpBuffer, u);
            bufPtr->data.len = nbytes;
            if (!ok) {
                bufPtr->winError = GetLastError();
                if (bufPtr->winError == 0)
                    bufPtr->winError =  WSAEINVAL; /* TBD - what else? */
            }
            else {
                bufPtr->winError = 0;
            }
            chanPtr = bufPtr->chanPtr;
            IOCP_ASSERT(chanPtr != NULL);
            IocpChannelLock(chanPtr);

            if (bufPtr->winError != 0 &&
                chanPtr->vtblPtr->translateerror != NULL) {
                /* Translate to a more specific error code */
                bufPtr->winError = chanPtr->vtblPtr->translateerror(chanPtr, bufPtr);
            }

            /*
             * NOTE - it is responsibility of called completion routines
             * to dispose of both chanPtr and bufPtr respectively.
             */
            IOCP_TRACE(("IocpCompletionThread: chanPtr=%p, chanPtr->state=0x%x, bufPtr->operation=%d, bufPtr->winError=%d\n", chanPtr, chanPtr->state, bufPtr->operation, bufPtr->winError));
            switch (bufPtr->operation) {
            case IOCP_BUFFER_OP_READ:
                IocpCompleteRead(chanPtr, bufPtr);
                break;
            case IOCP_BUFFER_OP_WRITE:
                IocpCompleteWrite(chanPtr, bufPtr);
                break;
            case IOCP_BUFFER_OP_CONNECT:
                IocpCompleteConnect(chanPtr, bufPtr);
                break;
            case IOCP_BUFFER_OP_DISCONNECT:
                IocpCompleteDisconnect(chanPtr, bufPtr);
                break;
            case IOCP_BUFFER_OP_ACCEPT:
                IocpCompleteAccept(chanPtr, bufPtr);
                break;
            }
        }
#ifdef _MSC_VER
    }
    __except (winError = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        Iocp_Panic("Tcl IOCP thread died with exception %#x\n", winError);
    }
#endif

    IOCP_TRACE(("CompletionThread exiting\n"));

    return winError;
}

/*
 * Finalization function to be called exactly once *per process*.
 * Caller responsible to ensure it's called only once in a thread-safe manner.
 * Essentially reverses effects of IocpProcessInit.
 *
 * Returns TCL_OK on success, TCL_ERROR on error.
 */
Iocp_DoOnceState iocpProcessCleanupFlag;
static IocpTclCode IocpProcessCleanup(ClientData clientdata)
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
static IocpTclCode IocpProcessInit(ClientData clientdata)
{
    WSADATA wsa_data;
    Tcl_Interp *interp = (Tcl_Interp *)clientdata;

#define WSA_VERSION_REQUESTED    MAKEWORD(2,2)

    iocpModuleState.completion_port =
        CreateIoCompletionPort(
            INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, 0);
    if (iocpModuleState.completion_port == NULL) {
        Iocp_ReportLastWindowsError(interp, "couldn't create completion port: ");
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
        Iocp_ReportLastWindowsError(interp, "couldn't create completion thread: ");
        CloseHandle(iocpModuleState.completion_port);
        iocpModuleState.completion_port = NULL;
        WSACleanup();
        return TCL_ERROR;
    }
    iocpModuleState.initialized = 1;

#ifdef IOCP_ENABLE_TRACE
    /* TBD - do we have to call TraceLoggingUnregister before exiting process */
    TraceLoggingRegister(iocpWinTraceProvider);
#endif

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

    /* Call specific IOCP type to close OS handles */
    ret = (chanPtr->vtblPtr->shutdown)(interp, chanPtr, 
                                             TCL_CLOSE_READ|TCL_CLOSE_WRITE);

    /* Irrespective of errors in above call, we're done with this channel */
    chanPtr->state   = IOCP_STATE_CLOSED;
    chanPtr->channel = NULL;
    IocpChannelDrop(chanPtr); /* Drops ref count from Tcl channel */
    /* Do NOT refer to chanPtr beyond this point */

    return ret;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelInput --
 *
 *    Called from the Tcl channel subsystem to read data. See the Tcl
 *    Tcl_CreateChannel manpage section for Tcl_ChannelInputProc for details
 *    on expected behavior.
 *
 * Results:
 *    Number of bytes read into outPtr, 0 on EOF, -1 on error.
 *
 * Side effects:
 *    Fills outPtr with data.
 *
 *------------------------------------------------------------------------
 */
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

    IOCP_TRACE(("IocpChannelInput Enter: chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));

    /*
     * At this point, Tcl channel system is holding a reference. However, if
     * any of the calls below recurse into a fileevent callback, the callback
     * script could close the channel and release that reference causing memory
     * to be deallocated. To prevent losing the memory, add an additional reference
     * while this function is executing.
     */
    chanPtr->numRefs += 1;

    if (IocpStateConnectionInProgress(chanPtr->state)) {
        IocpChannelConnectionStep(chanPtr,
                               (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) == 0);
        if (chanPtr->state == IOCP_STATE_CONNECTING ||
            chanPtr->state == IOCP_STATE_CONNECT_RETRY) {
            /* Only possible when above call returns for non-blocking case */
            IOCP_ASSERT(chanPtr->flags & IOCP_CHAN_F_NONBLOCKING);
            *errorCodePtr = EAGAIN;
            bytesRead = -1;
            goto vamoose;
        }
        IOCP_TRACE(("IocpChannelInput: chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
    }

    /* All these states would have taken early exit or transition above */
    IOCP_ASSERT(chanPtr->state != IOCP_STATE_CONNECTING);
    IOCP_ASSERT(chanPtr->state != IOCP_STATE_CONNECT_RETRY);
    IOCP_ASSERT(chanPtr->state != IOCP_STATE_CONNECTED);

    /*
     * Unless channel is marked as write-only (via shutdown) pass up data
     * irrespective of state. TBD - is this needed or does channel ensure this?
     */
    if (chanPtr->flags & IOCP_CHAN_F_WRITEONLY) {
        goto vamoose; /* bytesRead is already 0 indicating EOF */
    }

    /*
     * Note state may have changed above, but no matter. Taken care of below
     * based on whether there are any input buffers or not. If there are unconsumed
     * buffers, we have to pass them up.
     */

    /*
     * Now we get to copy data out from our buffers to the Tcl buffer.
     * Note that a zero length input buffer signifies EOF.
     */
    if (chanPtr->inputBuffers.headPtr == NULL) {
        IOCP_TRACE(("IocpChannelInput: No input buffers queued, chanPtr=%p\n", chanPtr));
        /* No input buffers. */
        if (chanPtr->state != IOCP_STATE_OPEN ||
            (chanPtr->flags & IOCP_CHAN_F_REMOTE_EOF)) {
            /* No longer OPEN so no hope of further data */
            goto vamoose; /* bytesRead is already 0 indicating EOF */
        }
        /* OPEN but no data available. If non-blocking, just return. */
        if (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) {
            *errorCodePtr = EAGAIN;
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
        IocpChannelAwaitCompletion(chanPtr, IOCP_CHAN_F_BLOCKED_READ); /* Unlocks and relocks! */
        /*
         * State unknown as it might have changed while waiting but don't
         * care. What matters is if input data is available in the buffers.
         * Fall through for processing.
         */
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
            chanPtr->winError = winError; /* TBD - should we check if already stored */
            if (winError == WSAECONNRESET) {
                /* Treat as EOF, not error, although we did store the error above. */
                chanPtr->flags |= IOCP_CHAN_F_REMOTE_EOF;
                break;
            }
            bytesRead = -1; /* TBD - or store 0 and treat as EOF ? */
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
            chanPtr->winError = winError; /* TBD - should we check if already stored */
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

    if (chanPtr->state == IOCP_STATE_CONNECT_FAILED) {
        /* Always treat as error */
        IOCP_ASSERT(chanPtr->inputBuffers.headPtr == NULL);
        *errorCodePtr= ENOTCONN;
        bytesRead = -1;
    }

    IocpChannelDrop(chanPtr);   /* Release the reference held by this function */

    IOCP_TRACE(("IocpChannelInput Returning: %d\n", bytesRead));
    return bytesRead;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelOutput --
 *
 *    Called from the Tcl channel subsystem to write data. See the Tcl
 *    Tcl_CreateChannel manpage section for Tcl_ChannelOutputProc for details
 *    on expected behavior.
 *
 * Results:
 *    Number of bytes written to device, -1 on error.
 *
 * Side effects:
 *    Writes to output device.
 *
 *------------------------------------------------------------------------
 */
static int
IocpChannelOutput (
    ClientData  instanceData,	/* IocpChannel pointer */
    const char *bytes,		/* Buffer */
    int         nbytes,		/* Maximum number of bytes to write. */
    int *errorCodePtr)		/* Where to store error codes. */
{
    DWORD        winError;
    IocpChannel *chanPtr = (IocpChannel *) instanceData;
    int          written = -1;

    IOCP_TRACE(("IocpChannelOutput Enter: chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));

    IocpChannelLock(chanPtr);

    /*
     * At this point, Tcl channel system is holding a reference. However, if
     * any of the calls below recurse into a fileevent callback, the callback
     * script could close the channel and release that reference causing memory
     * to be deallocated. To prevent losing the memory, add an additional reference
     * while this function is executing.
     */
    chanPtr->numRefs += 1;

    if (IocpStateConnectionInProgress(chanPtr->state)) {
        IocpChannelConnectionStep(chanPtr,
                                  (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) == 0);
        if (chanPtr->state == IOCP_STATE_CONNECTING ||
            chanPtr->state == IOCP_STATE_CONNECT_RETRY) {
            /* Only possible when above call returns for non-blocking case */
            IOCP_ASSERT(chanPtr->flags & IOCP_CHAN_F_NONBLOCKING);
            IocpChannelDrop(chanPtr);   /* Release the reference held by this function */
            *errorCodePtr = EAGAIN;
            return -1;
        }
        IOCP_TRACE(("IocpChannelOutput: chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
    }

    /* State may have changed but no matter, handled below */

    if (nbytes == 0) {
        /*
         * TclTLS will do zero byte writes for whatever reason. Guard against
         * that else loop below never stop.
         */
        written = 0;
    } else {
        /*
         * We loop because for blocking case we will keep retrying.
         */
        while (chanPtr->state == IOCP_STATE_OPEN) {
            winError = chanPtr->vtblPtr->postwrite(chanPtr, bytes, nbytes, &written);
            if (winError != ERROR_SUCCESS) {
                IocpSetTclErrnoFromWin32(winError);
                *errorCodePtr = Tcl_GetErrno();
                written = -1;
                break;
            }
            if (written != 0)
                break;              /* Wrote some data */
            /*
             * Would block. If non-blocking, advise caller to try later.
             * If blocking socket, wait for previous writes to complete.
             */
            if (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) {
                *errorCodePtr = EAGAIN;
                written = -1;
                break;
            }
            /*
             * Blocking socket. Wait till room opens up and retry.
             * Completion thread will signal via condition variable when
             * a previus write completes.
             */
            IocpChannelAwaitCompletion(chanPtr, IOCP_CHAN_F_BLOCKED_WRITE);
        }
    }

    /* If nothing was written and state is not OPEN, indicate error */
    if (written <= 0 && chanPtr->state != IOCP_STATE_OPEN) {
        *errorCodePtr = ENOTCONN; /* socket.test expects this error */
        written = -1;
    }

    IocpChannelDrop(chanPtr);   /* Release the reference held by this function */

    return written;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpParseOption --
 *
 *    Maps the passed TCP option name string to it index in passed table.
 *
 * Results:
 *    Index into table or -1 if not found. An error is stored in interp
 *    in case of errors.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static int IocpParseOption(
    Tcl_Interp *interp,         /* Used for error message. May be NULL. */
    const char *optNames[],     /* Array of option names terminated by NULL */
    const char *optName         /* Option name string */
    )
{
    int opt;
    for (opt = 0; optNames[opt]; ++opt) {
        if (strcmp(optName, optNames[opt]) == 0)
            return opt;
    }
    if (interp) {
        Tcl_DString ds;
        Tcl_DStringInit(&ds);
        for (opt = 0; optNames[opt]; ++opt) {
            Tcl_DStringAppendElement(&ds, optNames[opt]);
        }
        Tcl_BadChannelOption(interp, optName, Tcl_DStringValue(&ds));
        Tcl_DStringFree(&ds);
    }
    return -1;
}

static IocpTclCode
IocpChannelSetOption (
    ClientData instanceData,    /* IOCP channel state. */
    Tcl_Interp *interp,         /* For error reporting - can be NULL. */
    const char *optName,        /* Name of the option to set. */
    const char *value)          /* New value for option. */
{
    IocpChannel *chanPtr = (IocpChannel *)instanceData;
    IocpTclCode  ret;
    int          opt;

    IocpChannelLock(chanPtr);

    /*
     * At this point, Tcl channel system is holding a reference. However, if
     * any of the calls below recurse into a fileevent callback, the callback
     * script could close the channel and release that reference causing memory
     * to be deallocated. To prevent losing the memory, add an additional reference
     * while this function is executing.
     */
    chanPtr->numRefs += 1;

    if (IocpStateConnectionInProgress(chanPtr->state)) {
        /* If in connecting state, advance one step. See TIP 427 */
        IocpChannelConnectionStep(chanPtr, 0);
    }

    if (chanPtr->vtblPtr->optionNames && chanPtr->vtblPtr->setoption) {
        opt = IocpParseOption(interp, chanPtr->vtblPtr->optionNames, optName);
        if (opt == -1)
            ret = TCL_ERROR;
        else
            ret = chanPtr->vtblPtr->setoption(chanPtr, interp, opt, value);
    }
    else {
        ret = Tcl_BadChannelOption(interp, optName, "");
    }

    IocpChannelDrop(chanPtr);   /* Release the reference held by this function */
    return ret;
}

static IocpTclCode
IocpChannelGetOption (
    ClientData instanceData,    /* IOCP channel state. */
    Tcl_Interp *interp,         /* For error reporting - can be NULL */
    const char *optName,        /* Name of the option to
                                 * retrieve the value for, or
                                 * NULL to get all options and
                                 * their values. */
    Tcl_DString *dsPtr)         /* Where to store the computed
                                 * value; initialized by caller. */
{
    IocpChannel *chanPtr = (IocpChannel *)instanceData;
    IocpTclCode  ret;
    int          opt;

    IocpChannelLock(chanPtr);

    /*
     * At this point, Tcl channel system is holding a reference. However, if
     * any of the calls below recurse into a fileevent callback, the callback
     * script could close the channel and release that reference causing memory
     * to be deallocated. To prevent losing the memory, add an additional reference
     * while this function is executing.
     */
    chanPtr->numRefs += 1;

    if (IocpStateConnectionInProgress(chanPtr->state)) {
        /* If in connecting state, advance one step. See TIP 427 */
        IocpChannelConnectionStep(chanPtr, 0);
    }

    if (chanPtr->vtblPtr->optionNames && chanPtr->vtblPtr->getoption) {
        /* Channel type supports type-specific options */
        if (optName) {
            /* Return single option value */
            opt = IocpParseOption(interp, chanPtr->vtblPtr->optionNames, optName);
            if (opt == -1)
                ret = TCL_ERROR;
            else
                ret = chanPtr->vtblPtr->getoption(chanPtr, interp, opt, dsPtr);
        }
        else {
            /* Provide list of all option values */
            Tcl_DString optDs;
            Tcl_DStringInit(&optDs);
            opt = 0;
            while (chanPtr->vtblPtr->optionNames[opt]) {
                ret = chanPtr->vtblPtr->getoption(chanPtr, NULL, opt, &optDs);
                /*
                 * We do not treat TCL_ERROR as error. Assume it is a write-only
                 * or inapplicable and ignore it.
                 */
                if (ret == TCL_OK) {
                    Tcl_DStringAppendElement(dsPtr, chanPtr->vtblPtr->optionNames[opt]);
                    Tcl_DStringAppendElement(dsPtr, Tcl_DStringValue(&optDs));
                }
                Tcl_DStringFree(&optDs);
                ++opt;
            }
            ret = TCL_OK;
        }
    } else {
        /* No channel specific options */
        if (optName) {
            ret = Tcl_BadChannelOption(interp, optName, "");
        }
        else {
            /* No problem. Just return the pre-inited empty dsPtr */
            ret = TCL_OK;
        }
    }

    IocpChannelDrop(chanPtr);   /* Release the reference held by this function */
    return ret;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelFileEventMask
 *
 *    If the Tcl channel system has registered interest in file events
 *    and generates the mask to pass to Tcl_NotifyChannel based on current
 *    channel state.
 *
 *    For reading, inform if
 *    1. Tcl in watching the input side AND
 *    2. the channel is not shutdown for reads AND
 *    3. one of the following two condition is met
 *       a) end of file, OR
 *       b) input data is available
 *
 *    For writing, inform if
 *    1. Tcl in watching the output side AND
 *    2. the channel is not shutdown for writes AND
 *    3. one of the following two condition is met
 *       a) end of file, OR
 *       b) a write has completed AND there is room for more writes
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static int IocpChannelFileEventMask(
    IocpChannel *lockedChanPtr  /* Must be locked on entry */
    )
{
    int readyMask = 0;
    if ((lockedChanPtr->flags & IOCP_CHAN_F_WATCH_INPUT) &&
        !(lockedChanPtr->flags & IOCP_CHAN_F_WRITEONLY) &&
        ((lockedChanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) ||
         lockedChanPtr->inputBuffers.headPtr)) {
        readyMask |= TCL_READABLE;
    }
    if ((lockedChanPtr->flags & IOCP_CHAN_F_WATCH_OUTPUT) &&        /* 1 */
        !(lockedChanPtr->flags & IOCP_CHAN_F_READONLY) &&           /* 2 */
        ((lockedChanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) ||         /* 3a */
         ((lockedChanPtr->flags & IOCP_CHAN_F_WRITE_DONE) &&        /* 3b */
          lockedChanPtr->pendingWrites < lockedChanPtr->maxPendingWrites))) {
        readyMask |= TCL_WRITABLE;
        /* So we do not keep notifying of write dones */
        lockedChanPtr->flags &= ~ IOCP_CHAN_F_WRITE_DONE;
    }
    return readyMask;
}

static void
IocpChannelWatch (
    ClientData instanceData,	/* The socket state. */
    int mask)			/* Events of interest; an OR-ed
				 * combination of TCL_READABLE,
				 * TCL_WRITABLE and TCL_EXCEPTION. */
{
    IocpChannel *chanPtr = (IocpChannel*)instanceData;

    IocpChannelLock(chanPtr);
    /*
     * At this point, Tcl channel system is holding a reference. However, if
     * any of the calls below recurse into a fileevent callback, the callback
     * script could close the channel and release that reference causing memory
     * to be deallocated. To prevent losing the memory, add an additional reference
     * while this function is executing.
     */
    chanPtr->numRefs += 1;

    chanPtr->flags &= ~ (IOCP_CHAN_F_WATCH_INPUT | IOCP_CHAN_F_WATCH_OUTPUT);
    if (mask & TCL_READABLE)
        chanPtr->flags |= IOCP_CHAN_F_WATCH_INPUT;
    if (mask & TCL_WRITABLE) {
        /*
         * Writeable event notifications are only posted if a previous
         * write completed and app is watching output. The WRITE_DONE
         * flag fakes a previous write having been completed.
         */
        chanPtr->flags |= IOCP_CHAN_F_WRITE_DONE | IOCP_CHAN_F_WATCH_OUTPUT;
    }
    /*
     * As per WatchProc man page, we will use the event queue to do the
     * actual channel notification.
     */
    if (mask)
        IocpChannelEnqueueEvent(chanPtr, IOCP_EVENT_NOTIFY_CHANNEL, 0);

    IocpChannelDrop(chanPtr);   /* Release the reference held by this function */
}

static int
IocpChannelClose2 (
    ClientData instanceData,	/* The socket to close. */
    Tcl_Interp *interp,		/* Unused. */
    int flags)
{
    IocpChannel *chanPtr = (IocpChannel*)instanceData;
    int          ret;

    /* As per manpage, if flags 0, treat as Close */
    if (flags == 0)
        return IocpChannelClose(instanceData, interp);

    /* As per Tcl Winsock code in 8.6, raise error if both flags set */
    /* TBD - we could treate this as Close as well instead */
    flags &= TCL_CLOSE_READ | TCL_CLOSE_WRITE;
    if (flags == (TCL_CLOSE_READ|TCL_CLOSE_WRITE)) {
        if (interp) {
            Tcl_SetResult(interp, "socket close2proc called bidirectionally", TCL_STATIC);
        }
        return EINVAL;
    }

    IocpChannelLock(chanPtr);
    /*
     * At this point, Tcl channel system is holding a reference. However, if
     * any of the calls below recurse into a fileevent callback, the callback
     * script could close the channel and release that reference causing memory
     * to be deallocated. To prevent losing the memory, add an additional reference
     * while this function is executing.
     */
    chanPtr->numRefs += 1;

    /* Call specific IOCP type to close OS handles */
    ret = (chanPtr->vtblPtr->shutdown)(interp, chanPtr, flags);

    if (flags & TCL_CLOSE_READ)
        chanPtr->flags |= IOCP_CHAN_F_WRITEONLY;
    if (flags & TCL_CLOSE_WRITE)
        chanPtr->flags |= IOCP_CHAN_F_READONLY;

    IocpChannelDrop(chanPtr);   /* Release the reference held by this function */

    return ret;


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

static IocpTclCode
IocpChannelGetHandle (
    ClientData instanceData,	/* The socket state. */
    int direction,		/* TCL_READABLE or TCL_WRITABLE */
    ClientData *handlePtr)	/* Where to store the handle.  */
{
    IocpChannel *chanPtr = (IocpChannel*)instanceData;
    IocpTclCode ret;

    IocpChannelLock(chanPtr);
    ret = (chanPtr->vtblPtr->gethandle)(chanPtr, direction, handlePtr);
    IocpChannelUnlock(chanPtr);
    return ret;
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

    IOCP_TRACE(("IocpChannelThreadAction Enter: chanPtr=%p, action=%d, chanPtr->state=0x%x\n", chanPtr, action, chanPtr->state));

    if (action == TCL_CHANNEL_THREAD_INSERT) {
        chanPtr->owningThread = Tcl_GetCurrentThread();
        /*
         * Notify in case any I/O completion notifications pending. No harm
         * if there aren't
         */
        IocpChannelEnqueueEvent(chanPtr, IOCP_EVENT_THREAD_INSERTED, 1);
    } else {
        chanPtr->owningThread = 0;
    }
    IocpChannelUnlock(chanPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * IocpNotifyChannel --
 *
 *    Notifies the Tcl channel subsystem of file events if it has asked
 *    for them. The notification is made through Tcl_NotifyChannel which
 *    can call back into the driver so lockedChanPtr is unlocked before
 *    calling that function and relocked on return. Caller has to ensure
 *    it is holding a reference to lockedChanPtr so it does not disappear
 *    while unlocked. In case of EOF, Tcl_NotifyChannel will be called
 *    continuously until the callback closes the channel.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    File event callbacks are invoked, the IocpChannel state may change.
 *
 *------------------------------------------------------------------------
 */
static void IocpNotifyChannel(
    IocpChannel *lockedChanPtr  /* Locked on entry, locked on return but
                                 * unlocked in between */
    )
{
    int         readyMask;
    Tcl_Channel channel;

    IOCP_TRACE(("IocpNotifyChannel Enter: chanPtr=%p, chanPtr->state=0x%x, chanPtr->channel=%p\n", lockedChanPtr, lockedChanPtr->state, lockedChanPtr->channel));
    channel = lockedChanPtr->channel; /* Get before unlocking */
    if (channel == NULL)
        return;                 /* Tcl channel has gone away */
    readyMask = IocpChannelFileEventMask(lockedChanPtr);
    IOCP_TRACE(("IocpNotifyChannel : chanPtr=%p, chanPtr->state=0x%x, readyMask=0x%x\n", lockedChanPtr, lockedChanPtr->state, readyMask));
    if (readyMask == 0)
        return;                 /* Nothing to notify */

    /*
     * Unlock before calling Tcl_NotifyChannel which may recurse via the
     * callback script which again calls us through the channel commands.
     * Note the IocpChannel will not be freed when unlocked as caller
     * should hold a reference count to it from evPtr.
     */
    IocpChannelUnlock(lockedChanPtr);
    Tcl_NotifyChannel(channel, readyMask);
    IocpChannelLock(lockedChanPtr);
    
    IOCP_TRACE(("IocpNotifyChannel after Tcl_NotifyChannel return: chanPtr=%p, chanPtr->state=0x%x, chanPtr->flags=0x%x\n", lockedChanPtr, lockedChanPtr->state, lockedChanPtr->flags));

    if (lockedChanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) {
        /*
         * In case of EOF, we have to keep notifying until application closes
         * the channel. Originally we were looping calling Tcl_NotifyChannel.
         * However that does not work in the case of applications that do
         * not close the channel in the callback but rather set some vwait
         * variable and close it after the vwait completes. A hard loop
         * here will not allow them to do that so we post an event to
         * the Tcl event loop instead.
         */

        /* Recompute ready mask in case callback turned off handlers */
        readyMask = IocpChannelFileEventMask(lockedChanPtr);
        IOCP_TRACE(("IocpNotifyChannel EOF: chanPtr=%p, chanPtr->state=0x%x, chanPtr->flags=0x%x, readyMask=0x%x\n", lockedChanPtr, lockedChanPtr->state, lockedChanPtr->flags, readyMask));
        if (readyMask != 0) {
            IocpChannelEnqueueEvent(lockedChanPtr, IOCP_EVENT_NOTIFY_CHANNEL, 1);
        }
    }
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

    IOCP_TRACE(("IocpEventHandler Enter: chanPtr=%p, evPtr->reason=%d\n", ((IocpTclEvent *)evPtr)->chanPtr, ((IocpTclEvent *)evPtr)->reason));

    if (!(flags & TCL_FILE_EVENTS)) {
        return 0;               /* We are not to process file/network events */
    }

    chanPtr = ((IocpTclEvent *)evPtr)->chanPtr;
    IocpChannelLock(chanPtr);

    chanPtr->flags &= ~IOCP_CHAN_F_ON_EVENTQ;
    chanPtr->flags |= IOCP_CHAN_F_IN_EVENT_HANDLER;

    IOCP_TRACE(("IocpEventHandler: chanPtr=%p, chanPtr->state=0x%x\n", chanPtr, chanPtr->state));

    switch (chanPtr->state) {
    case IOCP_STATE_LISTENING:
        if (chanPtr->vtblPtr->accept) {
            chanPtr->vtblPtr->accept(chanPtr);
        }
        break;

    case IOCP_STATE_CONNECTING:
    case IOCP_STATE_CONNECT_RETRY:
    case IOCP_STATE_CONNECTED:
        IocpChannelConnectionStep(chanPtr, 0); /* May change state */
        break;

    case IOCP_STATE_OPEN:
    case IOCP_STATE_CONNECT_FAILED:
    case IOCP_STATE_DISCONNECTED:
        /* Notify Tcl channel subsystem if it has asked for it */
        IocpNotifyChannel(chanPtr); /* May change state */
        break;
    default: /* INIT and CLOSED */
        /* Late arrival */
        break;
    }

    chanPtr->flags &= ~IOCP_CHAN_F_IN_EVENT_HANDLER;

    /* Drop the reference corresponding to queueing to the event q. */
    ((IocpTclEvent *)evPtr)->chanPtr = NULL;
    IocpChannelDrop(chanPtr);

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
    IOCP_TRACE(("IocpChannelPostReads returning with lockedChanPtr=%p, pendingReads=%d\n", lockedChanPtr, lockedChanPtr->pendingReads));
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

    if (Tcp_ModuleInitialize(interp) != TCL_OK)
        return TCL_ERROR;
    if (BT_ModuleInitialize(interp) != TCL_OK)
        return TCL_ERROR;

    Tcl_CreateObjCommand(interp, "iocp::debugout", Iocp_DebugOutObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::stats", Iocp_StatsObjCmd, 0L, 0L);

    Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION);

#ifdef IOCP_ENABLE_TRACE
    /* Note this is globally shared across all interpreters */
    Tcl_LinkVar(interp, "::iocp::enableTrace",
                (char *)&iocpEnableTrace, TCL_LINK_BOOLEAN);
#endif

    return TCL_OK;
}

/* Outputs a string using Windows OutputDebugString */
IocpTclCode
Iocp_DebugOutObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    if (objc > 1)
        OutputDebugStringA(Tcl_GetString(objv[1]));
    return TCL_OK;
}

/* Returns statistics */
IocpTclCode
Iocp_StatsObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    Tcl_Obj *stats[12];
    int n;
#define ADDSTATS(field_) do { \
    stats[n++] = Tcl_NewStringObj(# field_, -1); \
    stats[n++] = IOCP_STATS_GET(Iocp ## field_); \
} while (0)

    n = 0;
    ADDSTATS(ChannelAllocs);
    ADDSTATS(ChannelFrees);
    ADDSTATS(BufferAllocs);
    ADDSTATS(BufferFrees);
    ADDSTATS(DataBufferAllocs);
    ADDSTATS(DataBufferFrees);

    IOCP_ASSERT(n <= sizeof(stats)/sizeof(stats[0]));

    Tcl_SetObjResult(interp, Tcl_NewListObj(n, stats));
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

/*
 * TBD - design change - Instead of going through the event loop to
 * notify fileevents, maybe set up a event source and set maxblocking time
 * to 0 as the core winsock implementation does.
 */
