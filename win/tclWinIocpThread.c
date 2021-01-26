/*
 * tclWinIocpThread.c --
 *
 *	Implements the IOCP completion thread.
 *
 * Copyright (c) 2020 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include "tclWinIocp.h"


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
    IOCP_TRACE(("IocpCompleteConnect Enter: lockedChanPtr=%p. state=0x%x\n", lockedChanPtr, lockedChanPtr->state));
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
 *    Handles completion of disconnect operations from IOCP. The
 *    channel-specific disconnect handler is called if specified.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The channel may be dropped.
 *
 *------------------------------------------------------------------------
 */
static void IocpCompleteDisconnect(
    IocpChannel *lockedChanPtr, /* Locked channel, will be dropped */
    IocpBuffer *bufPtr)         /* I/O completion buffer */
{
    IOCP_TRACE(("IocpCompleteDisconenct Enter: lockedChanPtr=%p. state=0x%x\n", lockedChanPtr, lockedChanPtr->state));
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
    IOCP_TRACE(("IocpCompleteRead Enter: lockedChanPtr=%p state=0x%x bufPtr=%p datalen=%d\n", lockedChanPtr, lockedChanPtr->state, bufPtr, bufPtr->data.len));

    IOCP_ASSERT(lockedChanPtr->pendingReads > 0);
    lockedChanPtr->pendingReads--;

    if (lockedChanPtr->state == IOCP_STATE_CLOSED) {
        bufPtr->chanPtr = NULL;
        IocpChannelDrop(lockedChanPtr); /* Corresponding to bufPtr->chanPtr */
        IocpBufferFree(bufPtr);
        IOCP_TRACE(("IocpCompleteRead buffer discarded: lockedChanPtr=%p state=0x%x\n", lockedChanPtr, lockedChanPtr->state));
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
        lockedChanPtr->flags |= IOCP_CHAN_F_NOTIFY_WRITES;
        /* TBD - optimize under which conditions we need to nudge thread */
        IocpChannelNudgeThread(lockedChanPtr, IOCP_CHAN_F_BLOCKED_WRITE, 0);
    }
    IocpChannelDrop(lockedChanPtr); /* Corresponding to bufPtr->chanPtr */
}

DWORD WINAPI
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
            IOCP_TRACE(("IocpCompletionThread: chanPtr=%p, chanPtr->state=0x%x, bufPtr=%p, bufPtr->operation=%d, bufPtr->winError=%d\n", chanPtr, chanPtr->state, bufPtr, bufPtr->operation, bufPtr->winError));
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

            if (chanPtr->flags & (IOCP_CHAN_F_SEND_DISCONNECT|IOCP_CHAN_F_RECV_DISCONNECT)) {
                /* Complete disconnect for closed side */
                WinsockClientGracefulDisconnect(chanPtr, 0);
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
