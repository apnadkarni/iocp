/*
 * tclWinIocp.c --
 *
 *	Main module of Windows-specific IOCP related variables and procedures.
 *
 * Copyright (c) 2019-2020 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include "tclWinIocp.h"

/*
 * Static data
 */

/* Holds global IOCP state */
IocpModuleState iocpModuleState;

/* Structure used for channel ready queue entries */
typedef struct IocpReadyQEntry {
    IocpLink     link;
    IocpChannel *chanPtr;
} IocpReadyQEntry;
/* TBD - placeholders until free list cache is implemented */
IOCP_INLINE IocpReadyQEntry *IocpReadyQEntryAllocate() {
    return (IocpReadyQEntry *) ckalloc(sizeof(IocpReadyQEntry));
}
IOCP_INLINE void IocpReadyQEntryFree(IocpReadyQEntry *rqePtr) {
    ckfree(rqePtr);
}

/*
 * IocpThreadData holds the state specific to a Tcl thread. Note that
 * it is also accessed from the IOCP completion thread so access needs to
 * be synchronized.
 */
typedef struct IocpThreadData {
    IocpLock     lock;          /* Control access to the structure */
    /*
     * Ready channel queue. This holds the list of channels that (potentially)
     * need some action to be taken with respect to Tcl, for example read/write
     * events. Each Tcl thread has one such queue accessible via thread local
     * storage. Channels also hold a pointer to the ready queue corresponding to
     * their owning thread. These pointers are used by the IOCP completion
     * thread to enqueue entries to the queue for the thread owning the channel
     * as IOCP requests complete. Each Tcl thread dequeues and processes these
     * entries via EventSourceSetup/EventSourceCheck.
     */
    IocpList     readyQ;
    LONG         numRefs;   /* Number of references to this structure */
    Tcl_ThreadId threadId;  /* Id of corresponding thread */
} IocpThreadData;
IOCP_INLINE void IocpThreadDataLock(IocpThreadData *tsdPtr) {
    IocpLockAcquireExclusive(&tsdPtr->lock);
}
IOCP_INLINE void IocpThreadDataUnlock(IocpThreadData *tsdPtr) {
    IocpLockReleaseExclusive(&tsdPtr->lock);
}

/* Statistics */
IocpStats iocpStats;

/* Prototypes */
static void IocpRequestEventPoll(IocpChannel *lockedChanPtr);
static void IocpNotifyChannel(IocpChannel *lockedChanPtr);
static int  IocpEventHandler(Tcl_Event *evPtr, int flags);
static int  IocpChannelFileEventMask(IocpChannel *lockedChanPtr);
static void IocpChannelConnectionStep(IocpChannel *lockedChanPtr, int blockable);
static void IocpChannelExitConnectedState(IocpChannel *lockedChanPtr);
static void IocpChannelAwaitConnectCompletion(IocpChannel *lockedChanPtr);
static void IocpThreadInit(void);
static void IocpThreadExitHandler(ClientData unused);
static void IocpThreadDataDrop(IocpThreadData *lockedTsdPtr);
static IocpThreadData *IocpThreadDataGet(void);
void IocpReadyQAdd(IocpChannel *lockedChanPtr, int force);

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
    chanPtr->owningTsdPtr = NULL;
    chanPtr->owningThread  = 0;
    chanPtr->readyQThread = 0;
    chanPtr->eventQThread = 0;
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

                IocpRequestEventPoll(lockedChanPtr);
            }
        } else {
            if (lockedChanPtr->vtblPtr->connectfailed  == NULL ||
                lockedChanPtr->vtblPtr->connectfailed(lockedChanPtr) != 0) {
                /* No means to retry or retry failed. lockedChanPtr->winError is error */
                lockedChanPtr->state = IOCP_STATE_CONNECT_FAILED;
                lockedChanPtr->flags |= IOCP_CHAN_F_REMOTE_EOF;

                IocpRequestEventPoll(lockedChanPtr);
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
    IOCP_TRACE(("IocpChannelConnectionStep return: lockedChanPtr=%p, blockable=%d, state=0x%x\n", lockedChanPtr, blockable, lockedChanPtr->state));
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
    /* Notify channel is writable in case file events registered */
    lockedChanPtr->flags |= IOCP_CHAN_F_NOTIFY_WRITES;

    IocpRequestEventPoll(lockedChanPtr);
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
    IOCP_TRACE(("IocpChannelAwaitCompletion Enter: lockedChanPtr=%p, blockType=0x%x\n", lockedChanPtr, blockType));
    lockedChanPtr->flags &= ~ IOCP_CHAN_F_BLOCKED_MASK;
    lockedChanPtr->flags |= blockType;
    IocpChannelCVWait(lockedChanPtr);
    IOCP_TRACE(("IocpChannelAwaitCompletion Leave: lockedChanPtr=%p, blockType=0x%x\n", lockedChanPtr, blockType));
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
 *----------------------------------------------------------------------
 *
 * IocpEventSourceSetup --
 *
 *	This function is invoked before Tcl_DoOneEvent blocks waiting for an
 *	event. If there are any channels marked ready, it arranges for the
 *      event loop to poll immediately for events.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Adjusts the event loop block time.
 *
 *----------------------------------------------------------------------
 */
static void
IocpEventSourceSetup(
    ClientData data,		/* Not used. */
    int flags)			/* Event flags as passed to Tcl_DoOneEvent. */
{
    IocpThreadData *tsdPtr;

    IOCP_TRACE(("IocpEventSourceSetup Enter (Thread %d): flags=0x%x\n", Tcl_GetCurrentThread(), flags));

    if (!(flags & TCL_FILE_EVENTS)) {
        IOCP_TRACE(("IocpEventSourceSetup return (Thread %d): ! TCL_FILE_EVENTS\n", Tcl_GetCurrentThread()));
	return;
    }

    tsdPtr = IocpThreadDataGet(); /* Note: tsdPtr is locked */
    IOCP_ASSERT(tsdPtr != NULL);

    if (tsdPtr->readyQ.headPtr) {
        /*
         * At least one channel needs to be looked at. Set block time to 0
         * so event loop will poll immediately.
         */
        IOCP_TRACE(("IocpEventSourceSetup (Thread %d): Set block time to 0\n", Tcl_GetCurrentThread()));
        Tcl_Time blockTime = { 0, 0 };
        Tcl_SetMaxBlockTime(&blockTime);
    }
    IocpThreadDataUnlock(tsdPtr);
    IOCP_TRACE(("IocpEventSourceSetup return (Thread %d)\n", Tcl_GetCurrentThread()));
}

/*
 *----------------------------------------------------------------------
 *
 * IocpEventSourceCheck --
 *
 *	This function is called by Tcl_DoOneEvent to generate any events
 *      on ready channels.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May queue one or more events to the event loop.
 *
 *----------------------------------------------------------------------
 */

static void
IocpEventSourceCheck(
    ClientData data,		/* Not used. */
    int flags)			/* Event flags as passed to Tcl_DoOneEvent. */
{
    IocpThreadData *tsdPtr;
    IocpList        readyQ;
    IocpLink       *linkPtr;
    Tcl_ThreadId    threadId;

    IOCP_TRACE(("IocpEventSourceCheck Enter (Thread %d): flags=0x%x\n", Tcl_GetCurrentThread(), flags));

    if (!(flags & TCL_FILE_EVENTS)) {
        IOCP_TRACE(("IocpEventSourceCheck return (Thread %d): ! TCL_FILE_EVENTS\n", Tcl_GetCurrentThread()));
	return;
    }

    threadId = Tcl_GetCurrentThread();
    tsdPtr = IocpThreadDataGet(); /* Note: tsdPtr is locked */
    IOCP_ASSERT(tsdPtr != NULL);  /* As long thread lives! */

    readyQ = IocpListPopAll(&tsdPtr->readyQ);
    IocpThreadDataUnlock(tsdPtr);

    while ((linkPtr = IocpListPopFront(&readyQ)) != NULL) {
        IocpReadyQEntry *rqePtr = CONTAINING_RECORD(linkPtr, IocpReadyQEntry, link);
        if (rqePtr->chanPtr != NULL) {
            IocpChannel *lockedChanPtr = rqePtr->chanPtr;
            IocpChannelLock(lockedChanPtr);
            rqePtr->chanPtr   = NULL;
            lockedChanPtr->readyQThread = 0; /* Enable further enqueing to
                                                ready q for the channel */
            /*
             * Ensure the channel is still attached to this thread and that
             * an event is not already queued to it. This latter is just an
             * optimization.
             */
            if (lockedChanPtr->owningThread == threadId &&
                lockedChanPtr->owningThread != lockedChanPtr->eventQThread) {
                IocpTclEvent *evPtr = ckalloc(sizeof(*evPtr));
                lockedChanPtr->eventQThread = threadId;
                evPtr->event.proc = IocpEventHandler;
                evPtr->chanPtr    = lockedChanPtr;
                /*
                 * The following cancel each other.
                 *    lockedChanPtr->numRefs++ because reference from evPtr
                 *    lockedChanPtr->numRefs-- because dereference from rqePtr
                 */
                IOCP_TRACE(("IocpEventSourceCheck (Thread %d): lockedChanPtr=%p queued to event queue.\n", Tcl_GetCurrentThread(), lockedChanPtr));
                IocpChannelUnlock(lockedChanPtr);
                Tcl_QueueEvent((Tcl_Event *) evPtr, TCL_QUEUE_TAIL);
            }
            else {
                /* Channel not attached to this thread or already queued */
                IOCP_TRACE(("IocpEventSourceCheck (Thread %d): lockedChanPtr=%p not attached to this thread or event already queued.\n", Tcl_GetCurrentThread(), lockedChanPtr));
                IocpChannelDrop(lockedChanPtr); /* Deref from rqePtr */
            }
        }
        IocpReadyQEntryFree(rqePtr);
    }
    IOCP_TRACE(("IocpEventSourceCheck return (Thread %d)\n", Tcl_GetCurrentThread()));
}

/*
 *------------------------------------------------------------------------
 *
 * IocpChannelNudgeThread --
 *
 *    Wakes up the thread owning a channel if it is blocked for I/O
 *    completion or if not, places the channel on the ready queue for
 *    the owning thread and alerts the thread.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    An ready queue entry is added or the condition variable being waited on is
 *    poked. Due to the lock hierarchy rules, *lockedChanPtr may be unlocked and
 *    relocked before returning and its state may have therefore changed.
 *
 *------------------------------------------------------------------------
 */
void IocpChannelNudgeThread(
    IocpChannel *lockedChanPtr,  /* Must be locked and caller holding a reference
                                  * to ensure it does not go away even if unlocked
                                  */
    int          blockMask,      /* Combination of IOCP_CHAN_F_BLOCKED*. Channel
                                  * thread will be woken if waiting on any of these */
    int          force)          /* If true, force an event notification even if
                                  * thread was blocked and is woken up or channel
                                  * was already queued on ready q. Normally,
                                  * the function will not do so in these cases. */
{
    IOCP_TRACE(("IocpChannelNudgeThread Enter: lockedChanPtr=%p, blockMask=0x%x, force=%d, lockedChanPtr->state=0x%x, lockedChanPtr->flags=0x%x\n", lockedChanPtr, blockMask, force, lockedChanPtr->state, lockedChanPtr->flags));
    if (! IocpChannelWakeAfterCompletion(lockedChanPtr, blockMask) || force) {
        /*
         * Owning thread was not woken, either it was not blocked for the right
         * reason or was not blocked at all. Need to alert it via the ready q.
         */
        if ((lockedChanPtr->flags & (IOCP_CHAN_F_WATCH_ACCEPT |
                                     IOCP_CHAN_F_WATCH_INPUT |
                                     IOCP_CHAN_F_WATCH_OUTPUT)) ||
            force) {
            IocpReadyQAdd(lockedChanPtr, force);
        }
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpThreadInit --
 *
 *    Initializes the thread-related structures. Allocates and initializes the
 *    block of thread-specific data for the calling thread if not already done.
 *    Sets up the event source and exit handlers.
 *
 * Results:
 *    Nothing.
 *
 * Side effects:
 *    The returned pointer is also stored in the thread's TLS table at
 *    the index iocpModuleState.tlsIndex.
 *
 *------------------------------------------------------------------------
 */
static void
IocpThreadInit()
{
    IocpThreadData *tsdPtr;
    IOCP_ASSERT(iocpModuleState.tlsIndex != TLS_OUT_OF_INDEXES);
    tsdPtr = TlsGetValue(iocpModuleState.tlsIndex);
    if (tsdPtr == NULL) {
        /* First call for this thread */
        tsdPtr = ckalloc(sizeof(*tsdPtr));
        IocpLockInit(&tsdPtr->lock);
        IocpListInit(&tsdPtr->readyQ);
        tsdPtr->numRefs = 1;    /* Corresponding decrement at thread exit */
        tsdPtr->threadId = Tcl_GetCurrentThread();
        TlsSetValue(iocpModuleState.tlsIndex, tsdPtr);

        Tcl_CreateEventSource(IocpEventSourceSetup, IocpEventSourceCheck, NULL);
        Tcl_CreateThreadExitHandler(IocpThreadExitHandler, NULL);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpThreadDataDrop --
 *
 *    Decrements the reference count on the passed thread-specific data
 *    and deallocates it when it reaches 0.
 *
 *    Caller must not reference the block when this function returns.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The block is unlocked and deallocated.
 *
 *------------------------------------------------------------------------
 */
static void
IocpThreadDataDrop(
    IocpThreadData *lockedTsdPtr) /* Must be locked with ref count > 0 */
{
    IOCP_ASSERT(lockedTsdPtr->numRefs > 0);
    if (lockedTsdPtr->numRefs == 1) {
        /* Final reference. Free up everything */
        IocpList         readyQ;
        IocpLink        *linkPtr;

        readyQ = IocpListPopAll(&lockedTsdPtr->readyQ);
        IocpThreadDataUnlock(lockedTsdPtr);
        ckfree(lockedTsdPtr);

        while ((linkPtr = IocpListPopFront(&readyQ)) != NULL) {
            IocpReadyQEntry *rqePtr = CONTAINING_RECORD(linkPtr, IocpReadyQEntry, link);
            if (rqePtr->chanPtr != NULL) {
                IocpChannelLock(rqePtr->chanPtr);
                IocpChannelDrop(rqePtr->chanPtr);
            }
            IocpReadyQEntryFree(rqePtr);
        }
    } else {
        lockedTsdPtr->numRefs -= 1;
        IocpThreadDataUnlock(lockedTsdPtr);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpThreadDataGet --
 *
 *    Retrieves the thread-specific data block for the calling thread.
 *    This must have been allocated and initialized previously. The
 *    block is locked on return.
 *
 * Results:
 *    Pointer to the thread's IocpThreadData block.
 *
 * Side effects:
 *    The lock is held on the returned block.
 *
 *------------------------------------------------------------------------
 */
static IocpThreadData *
IocpThreadDataGet()
{
    IocpThreadData *tsdPtr;
    IOCP_ASSERT(iocpModuleState.tlsIndex != TLS_OUT_OF_INDEXES);
    tsdPtr = TlsGetValue(iocpModuleState.tlsIndex);
    if (tsdPtr == NULL) {
        /*
         * IOCP not initialized in this thread. This can happen for instance
         * if the main thread creates a IOCP socket and then passes it to
         * another Tcl thread that has not loaded the iocp package.
         * TBD - need a test case for this. See crash.tcl
         */
        IocpThreadInit();
        tsdPtr = TlsGetValue(iocpModuleState.tlsIndex);
        IOCP_ASSERT(tsdPtr != NULL);
    }
    IocpThreadDataLock(tsdPtr);
    IOCP_ASSERT(tsdPtr->numRefs > 0);
    return tsdPtr;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpReadyQAdd --
 *
 *    Adds a channel to the ready queue for the thread owning the channel.
 *    If the channel is already on the ready queue for the thread, an
 *    additional entry is added only if the force parameter is true.
 *
 *    Caller must be holding a lock on lockedChanPtr but NOT ON the the
 *    target thread's TSD (only an issue when called from the same thread)
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Thread is alerted if the ready queue is updated.
 *
 *------------------------------------------------------------------------
 */
void IocpReadyQAdd(
    IocpChannel *lockedChanPtr,  /* Must be locked and caller holding a reference
                                  * to ensure it does not go away even if unlocked
                                  */
    int          force)          /* If true, force even if already queued */
{
    IocpReadyQEntry *rqePtr;
    IocpThreadData  *tsdPtr;

    IOCP_TRACE(("IocpReadyQAdd Enter: lockedChanPtr=%p, force=%d, lockedChanPtr->state=0x%x, lockedChanPtr->flags=0x%x\n", lockedChanPtr, force, lockedChanPtr->state, lockedChanPtr->flags));

    /*
     * If the channel is not attached to a thread, obviously there is no thread
     * to mark as ready. Just return in that case. Requisite actions will be
     * taken when the channel gets attached to a thread.
     */
    if (lockedChanPtr->owningThread == 0) {
        IOCP_TRACE(("IocpReadyQAdd Return (no owning thread): lockedChanPtr=%p\n", lockedChanPtr));
        return;
    }
    /*
     * Unless force is true, enqueue only if there isn't already an entry known
     * to be present for the channel on that thread. As an aside, note that
     * this is only an optimization. No harm if multiple entries land up on
     * the queue. Moreover, no harm if channel ownership is changed after
     * enqueuing of an entry as that ownership is checked again before actual
     * dispatch within target thread and new owner is handled by the channel
     * attach/detach code.
     */
    if (lockedChanPtr->owningThread == lockedChanPtr->readyQThread && !force) {
        IOCP_TRACE(("IocpReadyQAdd Return (already queued and !force): lockedChanPtr=%p\n", lockedChanPtr));
        return;
    }

    tsdPtr = lockedChanPtr->owningTsdPtr;
    IOCP_ASSERT(tsdPtr); /* Since owningThread != 0 */

    rqePtr = IocpReadyQEntryAllocate(); /* Allocate BEFORE locking to minimize lock hold time */
    IocpThreadDataLock(tsdPtr);
    /* We have to further check that the thread is still alive! */
    if (tsdPtr->threadId == 0) {
        /*
         * That thread went away. Presumably the channel will get attached
         * to another thread at some point. Nought to do here.
         */
        IocpThreadDataUnlock(tsdPtr);
        IocpReadyQEntryFree(rqePtr);
        IOCP_TRACE(("IocpReadyQAdd Return (TSD owningThread=0): lockedChanPtr=%p\n", lockedChanPtr));
    } else {
        Tcl_ThreadId tid = tsdPtr->threadId; /* needed after unlocking */

        rqePtr->chanPtr = lockedChanPtr;
        lockedChanPtr->numRefs += 1; /* Will be unrefed on dequeing from readyq */

        /* Remember last thread to which the channel was queued. */
        lockedChanPtr->readyQThread = lockedChanPtr->owningThread;
        IocpListAppend(&tsdPtr->readyQ, &rqePtr->link);

        IocpThreadDataUnlock(tsdPtr);

        /* If not queueing to current thread, need to poke the target thread */
        if (tid != Tcl_GetCurrentThread()) {
            Tcl_ThreadAlert(tid); /* Poke the thread to look for work */
        }

        IOCP_TRACE(("IocpReadyQAdd Return (Entry added to thread %d): lockedChanPtr=%p\n", lockedChanPtr->owningThread, lockedChanPtr));
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpRequestEventPoll --
 *
 *    Marks a channel as ready and requests the event loop in the CURRENT
 *    thread to poll for events.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
static void
IocpRequestEventPoll(
    IocpChannel *lockedChanPtr  /* Must be locked on entry */
    )
{
    Tcl_Time blockTime = { 0, 0 };
    IocpReadyQAdd(lockedChanPtr, 0);
    Tcl_SetMaxBlockTime(&blockTime);
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

static void
IocpThreadExitHandler(ClientData unused)
{
    IocpThreadData *tsdPtr = IocpThreadDataGet();
    if (tsdPtr) {
        /* NOTE: tsdPtr is already LOCKED by IocpThreadDataGet! */
        TlsSetValue(iocpModuleState.tlsIndex, NULL);
        tsdPtr->threadId = 0; /* So IOCP thread knows this is an orphan */
        IocpThreadDataDrop(tsdPtr);
        Tcl_DeleteEventSource(IocpEventSourceSetup, IocpEventSourceCheck, NULL);
    }
}

static void
IocpProcessExitHandler(ClientData unused)
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

    iocpModuleState.tlsIndex = TlsAlloc();
    if (iocpModuleState.tlsIndex == TLS_OUT_OF_INDEXES) {
        Tcl_SetResult(interp, "Could not allocate TLS index.", TCL_STATIC);
        return TCL_ERROR;
    }

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

    BT_InitAPI();

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
    IocpTraceInit();
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
        IOCP_TRACE(("IocpChannelInput connection in progress: chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
        IocpChannelConnectionStep(chanPtr,
                               (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) == 0);
        if (chanPtr->state == IOCP_STATE_CONNECTING ||
            chanPtr->state == IOCP_STATE_CONNECT_RETRY) {
            /* Only possible when above call returns for non-blocking case */
            IOCP_ASSERT(chanPtr->flags & IOCP_CHAN_F_NONBLOCKING);
            *errorCodePtr = EAGAIN;
            bytesRead = -1;
            IOCP_TRACE(("IocpChannelInput returning (EAGAIN): chanPtr=%p\n", chanPtr));
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
        IOCP_TRACE(("IocpChannelInput returning (WRITEONLY channel): chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
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
        if (chanPtr->state != IOCP_STATE_OPEN ||
            (chanPtr->flags & IOCP_CHAN_F_REMOTE_EOF)) {
            /* No longer OPEN so no hope of further data */
            IOCP_TRACE(("IocpChannelInput returning (no input buffers, EOF): chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
            goto vamoose; /* bytesRead is already 0 indicating EOF */
        }
        /* OPEN but no data available. If non-blocking, just return. */
        if (chanPtr->flags & IOCP_CHAN_F_NONBLOCKING) {
            IOCP_TRACE(("IocpChannelInput returning (no input buffers, EAGAIN): chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
            *errorCodePtr = EAGAIN;
            bytesRead     = -1;
            goto vamoose;
        }
        /* Blocking connection. Wait for incoming data or eof */
        winError = IocpChannelPostReads(chanPtr); /* Ensure read is posted */
        if (winError != 0) {
            IOCP_TRACE(("IocpChannelInput returning (error on posting reads): chanPtr=%p, state=0x%x, winError=0x%x\n", chanPtr, chanPtr->state, winError));
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
            IOCP_TRACE(("IocpChannelInput (copying from buffer): chanPtr=%p state=0x%x bufPtr=%p bufPtr->data.len=%d\n", chanPtr, chanPtr->state, bufPtr, bufPtr->data.len));
            numCopied = IocpBufferMoveOut(bufPtr, outPtr, remaining);
            IOCP_TRACE(("IocpChannelInput (buffer copied): chanPtr=%p state=0x%x bufPtr=%p numCopied=%d\n", chanPtr, chanPtr->state, bufPtr, numCopied));
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
            IOCP_TRACE(("IocpChannelInput (buffer holds error): chanPtr=%p, state=0x%x, winError=0x%x\n", chanPtr, chanPtr->state, winError));
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

    IOCP_TRACE(("IocpChannelOutput Enter: chanPtr=%p, state=%d, nbytes=%d\n", chanPtr, chanPtr->state, nbytes));

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
            IOCP_TRACE(("IocpChannelOutput still connecting (EAGAIN): chanPtr=%p, state=0x%x\n", chanPtr, chanPtr->state));
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
            IOCP_TRACE(("IocpChannelOutput Posting write: chanPtr=%p, state=%d, nbytes=%d\n", chanPtr, chanPtr->state, nbytes));
            winError = chanPtr->vtblPtr->postwrite(chanPtr, bytes, nbytes, &written);
            IOCP_TRACE(("IocpChannelOutput Posted write returned: winError=0x%x, chanPtr=%p, state=%d, written=%d\n", winError, chanPtr, chanPtr->state, written));
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

    IOCP_TRACE(("IocpChannelOutput return: chanPtr=%p written=%d", chanPtr, written));
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
 *    3. one of the following two conditions is met
 *       a) end of file, OR
 *       b) input data is available
 *
 *    For writing, inform if
 *    1. Tcl in watching the output side AND
 *    2. the channel is not shutdown for writes AND
 *    3. one of the following two condition is met
 *       a) end of file, OR
 *       b) an unnotified write has completed AND there is room for more writes
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static int
IocpChannelFileEventMask(
    IocpChannel *lockedChanPtr  /* Must be locked on entry */
    )
{
    int readyMask = 0;
    IOCP_TRACE(("IocpChannelFileEventMask Enter: chanPtr=%p chanPtr->flags=0x%x inputBuffers.headPtr=%p pendingWrites=%d maxPendingWrites=%d.\n", lockedChanPtr, lockedChanPtr->flags, lockedChanPtr->inputBuffers.headPtr, lockedChanPtr->pendingWrites, lockedChanPtr->maxPendingWrites));
    if ((lockedChanPtr->flags & IOCP_CHAN_F_WATCH_INPUT) &&
        !(lockedChanPtr->flags & IOCP_CHAN_F_WRITEONLY) &&
        ((lockedChanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) ||
         lockedChanPtr->inputBuffers.headPtr)) {
        readyMask |= TCL_READABLE;
    }
    if ((lockedChanPtr->flags & IOCP_CHAN_F_WATCH_OUTPUT) &&        /* 1 */
        !(lockedChanPtr->flags & IOCP_CHAN_F_READONLY) &&           /* 2 */
        ((lockedChanPtr->flags & IOCP_CHAN_F_REMOTE_EOF) ||         /* 3a */
         ((lockedChanPtr->flags & IOCP_CHAN_F_NOTIFY_WRITES) &&        /* 3b */
          lockedChanPtr->pendingWrites < lockedChanPtr->maxPendingWrites))) {
        readyMask |= TCL_WRITABLE;
    }
    IOCP_TRACE(("IocpChannelFileEventMask return: readyMask=0x%x.\n", readyMask));
    return readyMask;
}

static void
IocpChannelWatch(
    ClientData instanceData,	/* The socket state. */
    int mask)			/* Events of interest; an OR-ed
				 * combination of TCL_READABLE,
				 * TCL_WRITABLE and TCL_EXCEPTION. */
{
    IocpChannel *chanPtr = (IocpChannel*)instanceData;

    IocpChannelLock(chanPtr);

    IOCP_TRACE(("IocpChannelWatch: chanPtr=%p state=%d mask=0x%x\n", chanPtr, chanPtr->state, mask));

    IOCP_ASSERT(chanPtr->owningThread == Tcl_GetCurrentThread());

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
         * write completed and app is watching output.
         */
        chanPtr->flags |= IOCP_CHAN_F_NOTIFY_WRITES | IOCP_CHAN_F_WATCH_OUTPUT;
    }

    /*
     * If any events are pending, mark the channel as ready and
     * tell event loop to poll immediately.
     */
    if (IocpChannelFileEventMask(chanPtr)) {
        IocpRequestEventPoll(chanPtr);
    }

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

    switch (action) {
    case TCL_CHANNEL_THREAD_INSERT:
        IOCP_ASSERT(chanPtr->owningThread == 0);
        chanPtr->owningThread = Tcl_GetCurrentThread();
        chanPtr->owningTsdPtr = IocpThreadDataGet();
        IOCP_ASSERT(chanPtr->owningTsdPtr != NULL);
        /* Note chanPtr->owningTsdPtr is LOCKED */

        /*
         * Link the thread's TSD to the channel so when data is received by
         * the IOCP completion thread, it knows which ready queue is associated
         * with the channel. We do not need to increment the reference count on the
         * TSD because the link from the chanPtr is covered by the lifetime
         * of the thread. Before thread exit the channel and TSD would be unlinked
         * by a call to this function with action THREAD_REMOVE (below).
         * HOWEVER...in case you change this to add a reference, note you
         * will need to modify thread cleanup (via ThreadDataDrop) to deal
         * with ready queue entries dropping channels which in turn point
         * to TSD's.
         */

        /* NOTE: MUST unlock before calling IocpReadyQAdd !!! */
        IocpThreadDataUnlock(chanPtr->owningTsdPtr);

        /*
         * Mark as ready in case any I/O completion notifications pending.
         * No harm if there aren't.
         */
        IocpReadyQAdd(chanPtr, 0);
        break;

    case TCL_CHANNEL_THREAD_REMOVE:
        chanPtr->owningThread = 0;
        chanPtr->owningTsdPtr = NULL;
        break;
    default:
        Tcl_Panic("Unknown channel thread action %d", action);
        break;
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
    if (readyMask == 0) {
        IOCP_TRACE(("IocpNotifyChannel return: chanPtr=%p, readyMask=0x%x\n", lockedChanPtr, readyMask));
        return;                 /* Nothing to notify */
    }

    if (readyMask & TCL_WRITABLE) {
        /*
         * We do not want to keep notifying if we have already notified
         * of writes unless more writes complete. The NOTIFY_WRITES flag
         * is set when the write IOCPs complete. We reset it here if we
         * notify Tcl that channel is writable and then we will not again
         * trigger notification unless more write IOCPs complete.
         */
        lockedChanPtr->flags &= ~ IOCP_CHAN_F_NOTIFY_WRITES;
    }

    /*
     * Unlock before calling Tcl_NotifyChannel which may recurse via the
     * callback script which again calls us through the channel commands.
     * Note the IocpChannel will not be freed when unlocked as caller
     * should hold a reference to it.
     */
    IocpChannelUnlock(lockedChanPtr);
    Tcl_NotifyChannel(channel, readyMask);
    IocpChannelLock(lockedChanPtr);

    IOCP_TRACE(("IocpNotifyChannel after Tcl_NotifyChannel: chanPtr=%p, chanPtr->state=0x%x, chanPtr->flags=0x%x\n", lockedChanPtr, lockedChanPtr->state, lockedChanPtr->flags));

#ifdef TBD  /* Needed? */
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
#endif
    IOCP_TRACE(("IocpNotifyChannel return: chanPtr=%p, chanPtr->state=0x%x, chanPtr->flags=0x%x\n", lockedChanPtr, lockedChanPtr->state, lockedChanPtr->flags));
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
static int
IocpEventHandler(
    Tcl_Event *evPtr,           /* Pointer to the Tcl_Event structure.
                                 * Contents immaterial */
    int        flags            /* TCL_FILE_EVENTS */
    )
{
    IocpChannel *chanPtr;

    IOCP_TRACE(("IocpEventHandler Enter: chanPtr=%p, flags=0x%x.\n", ((IocpTclEvent *)evPtr)->chanPtr, flags));

    if (!(flags & TCL_FILE_EVENTS)) {
        IOCP_TRACE(("IocpEventHandler return: chanPtr=%p, ! TCL_FILE_EVENTS.\n", ((IocpTclEvent *)evPtr)->chanPtr));
        return 0;               /* We are not to process file/network events */
    }

    chanPtr = ((IocpTclEvent *)evPtr)->chanPtr;
    IocpChannelLock(chanPtr);

    /*
     * The channel might have been moved to another thread while this event
     * was in the queue. Check for that and ignore the event if so. Note
     * The channel thread attach/detach would have taken care of generating
     * an event for the newly attached thread.
     * TBD - should we forward the event to the newly attached thread?
     * The channel insert/detach code in ThreadAction should have taken care
     * of that.
     */
    if (chanPtr->owningThread == Tcl_GetCurrentThread()) {
        IOCP_TRACE(("IocpEventHandler: chanPtr=%p, chanPtr->state=0x%x\n", chanPtr, chanPtr->state));
        /* Indicate that another event may be queued (optimization) */
        chanPtr->eventQThread = 0;

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
    }

    /* Drop the reference corresponding to queueing to the event q. */
    ((IocpTclEvent *)evPtr)->chanPtr = NULL;
    IocpChannelDrop(chanPtr);

    IOCP_TRACE(("IocpEventHandler return.\n"));
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

    IocpThreadInit();

    if (Tcp_ModuleInitialize(interp) != TCL_OK)
        return TCL_ERROR;
    if (BT_ModuleInitialize(interp) != TCL_OK)
        return TCL_ERROR;

    Tcl_CreateObjCommand(interp, "iocp::stats", Iocp_StatsObjCmd, 0L, 0L);

    Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION);

#ifdef IOCP_ENABLE_TRACE
    /* Note this is globally shared across all interpreters */
    Tcl_CreateObjCommand(interp, "iocp::trace::output", Iocp_TraceOutputObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::trace::configure", Iocp_TraceConfigureObjCmd, 0L, 0L);
    Tcl_Eval(interp, "namespace eval iocp::trace {namespace export *; namespace ensemble create}");
#endif

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
