/*
 * tclWinIocpTcp.c --
 *
 *	TCP support for Windows IOCP.
 *
 * Copyright (c) 2019 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include "tcl.h"
#include "tclWinIocp.h"


/*
 * Copied from Tcl -
 * "sock" + a pointer in hex + \0
 */
#if defined(BUILD_iocp)
# define SOCK_CHAN_NAME_PREFIX   "tcp"
#else
# define SOCK_CHAN_NAME_PREFIX   "sock"
#endif
#define SOCK_CHAN_LENGTH        (sizeof(SOCK_CHAN_NAME_PREFIX)-1 + 2*sizeof(void *) + 1)
#define SOCK_TEMPLATE           "sock%p"

typedef struct IocpTcpChannel {
    IocpChannel base;           /* Common IOCP channel structure. Must be
                                 * first because of how structures are
                                 * allocated and freed */
    SOCKET      so;             /* Winsock socket handle */
    struct addrinfo *remoteAddrList; /* List of potential remote addresses */
    struct addrinfo *remoteAddr;     /* Remote address in use.
                                      * Points into remoteAddrList */
    struct addrinfo *localAddrList;  /* List of potential local addresses */
    struct addrinfo *localAddr;      /* Local address in use
                                      * Points into localAddrList */
    int flags;     /* Miscellaneous flags */
#define IOCP_TCP_CONNECT_ASYNC 0x1

#define IOCP_TCP_MAX_RECEIVES 3
#define IOCP_TCP_MAX_SENDS    3

} IocpTcpChannel;

IOCP_INLINE IocpChannel *TcpChannelToIocpChannel(IocpTcpChannel *tcpPtr) {
    return (IocpChannel *) tcpPtr;
}
IOCP_INLINE IocpTcpChannel *IocpChannelToTcpChannel(IocpChannel *chanPtr) {
    return (IocpTcpChannel *) chanPtr;
}

static void  IocpTcpChannelInit(IocpChannel *basePtr);
static void  IocpTcpChannelFinit(IocpChannel *chanPtr);
static int   IocpTcpChannelShutdown(Tcl_Interp *, IocpChannel *chanPtr, int flags);
static IocpWindowsError IocpTcpPostConnect(IocpTcpChannel *chanPtr);
static IocpWindowsError IocpTcpBlockingConnect(IocpChannel *);
static IocpWindowsError IocpTcpPostRead(IocpChannel *);
static IocpWindowsError IocpTcpPostWrite(IocpChannel *, const char *data, int nbytes, int *countPtr);
static IocpWindowsError IocpTcpAsyncConnectFail(IocpChannel *lockedChanPtr);
static IocpTclCode      IocpTcpGetHandle(IocpChannel *lockedChanPtr, int direction, ClientData *handlePtr);

static IocpChannelVtbl tcpVtbl =  {
    IocpTcpChannelInit,
    IocpTcpChannelFinit,
    IocpTcpChannelShutdown,
    IocpTcpBlockingConnect,
    IocpTcpAsyncConnectFail,
    IocpTcpPostRead,
    IocpTcpPostWrite,
    IocpTcpGetHandle,
    sizeof(IocpTcpChannel)
};

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpChannelInit --
 *
 *    Initializes the IocpTcpChannel part of a IocpChannel structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static void IocpTcpChannelInit(IocpChannel *chanPtr)
{
    IocpTcpChannel *tcpPtr = IocpChannelToTcpChannel(chanPtr);
    tcpPtr->so             = INVALID_SOCKET;
    tcpPtr->remoteAddrList = NULL;
    tcpPtr->remoteAddr     = NULL;
    tcpPtr->localAddrList  = NULL;
    tcpPtr->localAddr      = NULL;
    tcpPtr->flags          = 0;

    tcpPtr->base.maxPendingReads  = IOCP_TCP_MAX_RECEIVES;
    tcpPtr->base.maxPendingWrites = IOCP_TCP_MAX_SENDS;
}


/*
 *------------------------------------------------------------------------
 *
 * IocpTcpChannelFinit --
 *
 *    Finalizer for a Tcp channel. Cleans up any resources.
 *    Caller has to ensure synchronization either by holding a lock
 *    on the containing IocpChannel or ensuring the structure is not
 *    accessible from any other thread.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Any allocated addresses are freed and socket closed if open.
 *
 *------------------------------------------------------------------------
 */
static void IocpTcpChannelFinit(IocpChannel *chanPtr)
{
    IocpTcpChannel *tcpPtr = IocpChannelToTcpChannel(chanPtr);

    if (tcpPtr->remoteAddrList) {
        freeaddrinfo(tcpPtr->remoteAddrList);
        tcpPtr->remoteAddrList = NULL;
        tcpPtr->remoteAddr     = NULL;
    }
    if (tcpPtr->localAddrList) {
        freeaddrinfo(tcpPtr->localAddrList);
        tcpPtr->localAddrList = NULL;
        tcpPtr->localAddr     = NULL;
    }

    if (tcpPtr->so != INVALID_SOCKET) {
        closesocket(tcpPtr->so);
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpChannelShutdown --
 *
 *    Conforms to the IocpChannel shutdown interface.
 *
 * Results:
 *    0 on success, else a POSIX error code.
 *
 * Side effects:
 *    The Windows socket is closed in direction(s) specified by flags.
 *
 *------------------------------------------------------------------------
 */
static int IocpTcpChannelShutdown(
    Tcl_Interp   *interp,        /* May be NULL */
    IocpChannel *lockedChanPtr, /* Locked pointer to the base IocpChannel */
    int          flags)         /* Combination of TCL_CLOSE_{READ,WRITE} */
{
    IocpTcpChannel *lockedTcpPtr = IocpChannelToTcpChannel(lockedChanPtr);
    if (lockedTcpPtr->so != INVALID_SOCKET) {
        int wsaStatus;
        switch (flags & (TCL_CLOSE_READ|TCL_CLOSE_WRITE)) {
        case TCL_CLOSE_READ:
            wsaStatus = shutdown(lockedTcpPtr->so, SD_RECEIVE);
            break;
        case TCL_CLOSE_WRITE:
            wsaStatus = shutdown(lockedTcpPtr->so, SD_SEND);
            break;
        case TCL_CLOSE_READ|TCL_CLOSE_WRITE:
            wsaStatus = closesocket(lockedTcpPtr->so);
            break;
        default:                /* Not asked to close either */
            return 0;
        }
        lockedTcpPtr->so = INVALID_SOCKET;
        if (wsaStatus == SOCKET_ERROR) {
            /* TBD - do we need to set a error string in interp? */
            IocpSetTclErrnoFromWin32(WSAGetLastError());
            return Tcl_GetErrno();
        }
    }
    return 0;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpGetHandle --
 *
 *    Implements IocpChannel's gethandle(). See comments there.
 *
 * Results:
 *    TCL_OK on success, TCL_ERROR on error.
 *
 * Side effects:
 *    The socket handle is returned in *handlePtr.
 *
 *------------------------------------------------------------------------
 */
static IocpTclCode IocpTcpGetHandle(
    IocpChannel *lockedChanPtr,
    int direction,
    ClientData *handlePtr)
{
    SOCKET so = IocpChannelToTcpChannel(lockedChanPtr)->so;
    if (so == INVALID_SOCKET)
        return TCL_ERROR;
    *handlePtr = (ClientData) so;
    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpPostConnect --
 *
 *    Posts a connect request to the IO completion port for a Tcp channel.
 *    The local and remote addresses are those specified in the tcpPtr
 *    localAddr and remoteAddr fields. Neither must be NULL.
 *
 * Results:
 *    Returns 0 on success or a Windows error code.
 *
 * Side effects:
 *    A connection request buffer is posted to the completion port.
 *
 *------------------------------------------------------------------------
 */
static IocpWindowsError IocpTcpPostConnect(
    IocpTcpChannel *tcpPtr  /* Channel pointer, may or may not be locked
                             * but caller has to ensure no interference */
    )
{
    static GUID     ConnectExGuid = WSAID_CONNECTEX;
    LPFN_CONNECTEX  fnConnectEx;
    IocpBuffer     *bufPtr;
    DWORD           nbytes;
    DWORD           winError;


    /* Bind local address. Required for ConnectEx */
    if (bind(tcpPtr->so, tcpPtr->localAddr->ai_addr,
             (int) tcpPtr->localAddr->ai_addrlen) != 0) {
        return WSAGetLastError();
    }

    /*
     * Retrieve the ConnectEx function pointer. We do not cache
     * because strictly speaking it depends on the socket and
     * address family that map to a protocol driver.
     */
    if (WSAIoctl(tcpPtr->so, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &ConnectExGuid, sizeof(GUID),
                 &fnConnectEx,
                 sizeof(fnConnectEx),
                 &nbytes, NULL, NULL) != 0 ||
        fnConnectEx == NULL) {
        return WSAGetLastError();
    }

    if (CreateIoCompletionPort((HANDLE) tcpPtr->so,
                               iocpModuleState.completion_port,
                               (ULONG_PTR) tcpPtr,
                               0) == NULL) {
        return GetLastError(); /* NOT WSAGetLastError() ! */
    }

    bufPtr = IocpBufferNew(0, IOCP_BUFFER_OP_CONNECT, IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->chanPtr    = TcpChannelToIocpChannel(tcpPtr);
    tcpPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

    if (fnConnectEx(tcpPtr->so, tcpPtr->remoteAddr->ai_addr,
                    (int) tcpPtr->remoteAddr->ai_addrlen, NULL, 0, &nbytes, &bufPtr->u.wsaOverlap) == FALSE) {
        winError = WSAGetLastError();
        if (winError != WSA_IO_PENDING) {
            IocpBufferFree(bufPtr);
            return winError;
        }
    }

    tcpPtr->base.state = IOCP_STATE_CONNECTING;
    return 0;
}


/*
 *------------------------------------------------------------------------
 *
 * IocpTcpPostRead --
 *
 *    Allocates a receive buffer and posts it to the socket associated
 *    with lockedTcpPtr. Implements the behavior defined for postread()
 *    in IocpChannel vtbl.
 *
 * Results:
 *    Returns 0 on success or a Windows error code.
 *
 * Side effects:
 *    The receive buffer is queued to the socket from where it will
 *    retrieved by the IO completion thread. The pending reads count
 *    in the IocpChannel is incremented.
 *
 *------------------------------------------------------------------------
 */
static IocpWindowsError IocpTcpPostRead(IocpChannel *lockedChanPtr)
{
    IocpTcpChannel *lockedTcpPtr = IocpChannelToTcpChannel(lockedChanPtr);
    IocpBuffer *bufPtr;
    WSABUF      wsaBuf;
    DWORD       flags;
    DWORD       wsaError;

    IOCP_ASSERT(lockedTcpPtr->base.state == IOCP_STATE_OPEN);
    bufPtr = IocpBufferNew(IOCP_BUFFER_DEFAULT_SIZE, IOCP_BUFFER_OP_READ,
                           IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->chanPtr    = lockedChanPtr;
    lockedTcpPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

    wsaBuf.buf = bufPtr->data.bytes;
    wsaBuf.len = bufPtr->data.capacity;
    flags      = 0;
    if (WSARecv(lockedTcpPtr->so,
                 &wsaBuf,       /* Buffer array */
                 1,             /* Number of elements in array */
                 NULL,          /* Not used */
                 &flags,
                 &bufPtr->u.wsaOverlap, /* Overlap structure for return status */
                 NULL) != 0        /* Not used */
        && (wsaError = WSAGetLastError()) != WSA_IO_PENDING) {
        /* Not good. */
        lockedTcpPtr->base.numRefs -= 1;
        bufPtr->chanPtr    = NULL;
        IocpBufferFree(bufPtr);
        return wsaError;
    }
    lockedChanPtr->pendingReads++;

    return 0;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpPostWrite --
 *
 *    Allocates a receive buffer, copies passed data to it and posts
 *    it to the socket associated with lockedTcpPtr. Implements the behaviour
 *    expected of the postwrite() function in IocpChannel vtbl.
 *
 * Results:
 *    If data is successfully written, return 0 and stores written count
 *    into *countPtr. If no data could be written because device would block
 *    returns 0 and stores 0 in *countPtr. On error, returns a Windows error.
 *
 * Side effects:
 *    The write buffer is queued to the socket from where it will
 *    retrieved by the IO completion thread. The pending writes count
 *    in the IocpChannel is incremented.
 *
 *------------------------------------------------------------------------
 */
static IocpWindowsError IocpTcpPostWrite(
    IocpChannel *lockedChanPtr, /* Must be locked on entry */
    const char  *bytes,         /* Pointer to data to write */
    int          nbytes,        /* Number of data bytes to write */
    int         *countPtr)      /* Output - Number of bytes written */
{
    IocpTcpChannel *lockedTcpPtr = IocpChannelToTcpChannel(lockedChanPtr);
    IocpBuffer *bufPtr;
    WSABUF      wsaBuf;
    DWORD       wsaError;
    DWORD       written;

    IOCP_ASSERT(lockedTcpPtr->base.state == IOCP_STATE_OPEN);

    /* If we have already too many outstanding writes */
    if (lockedChanPtr->pendingWrites >= lockedChanPtr->maxPendingWrites) {
        /* Not an error but indicate nothing written */
        *countPtr = 0;
        return ERROR_SUCCESS;
    }

    bufPtr = IocpBufferNew(nbytes, IOCP_BUFFER_OP_WRITE, IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS; /* TBD - should we treat this as above? But this is more serious (and should be rarer) */

    IocpBufferCopyIn(bufPtr, bytes, nbytes);

    bufPtr->chanPtr    = lockedChanPtr;
    lockedChanPtr->numRefs += 1; /* Reversed when buffer is unlinked from channel */
    wsaBuf.buf = bufPtr->data.bytes;
    wsaBuf.len = bufPtr->data.len;
    if (WSASend(lockedTcpPtr->so,
                &wsaBuf,       /* Buffer array */
                1,             /* Number of elements in array */
                &written,      /* Number of bytes sent - only valid if data sent
                                *  immediately so not reliable */
                0,             /*  Flags - not used */
                &bufPtr->u.wsaOverlap, /* Overlap structure for return status */
                NULL)                  /* Completion routine - Not used */
        != 0
        && (wsaError = WSAGetLastError()) != WSA_IO_PENDING) {
        /* Not good. */
        lockedChanPtr->numRefs -= 1;
        bufPtr->chanPtr    = NULL;
        IocpBufferFree(bufPtr);
        *countPtr = -1;
        return wsaError;
    }
    *countPtr = nbytes;
    lockedChanPtr->pendingWrites++;

    return 0;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpEnableTransfer --
 *
 *    Sets up a TCP channel for data transfer:
 *    - Address lists are freed as no longer needed
 *    - The associated socket is tied to the I/O completion port
 *    - Receive buffers are queued to it
 *
 *    The caller must not be holding locks on the passed tcpPtr but should
 *    also ensure it is not accessible from any other threads. When the
 *    function returns, the tcpPtr will be unlocked but may be accessible
 *    from the completion thread as receive buffers are enqueued by this
 *    function.
 *
 *    Caller must also be aware that the tcpPtr state may change asynchronously
 *    once this function is called.
 *
 * Results:
 *    TCL_OK on success, TCL_ERROR on errors.
 *
 * Side effects:
 *    I/O buffers are queued on the completion port.
 *
 *------------------------------------------------------------------------
 */
static IocpTclCode IocpTcpEnableTransfer(
    Tcl_Interp *interp,
    IocpTcpChannel *tcpPtr)     /* Pointer to Tcp channel state. Must not
                                 * locked on entry, will not be locked on return
                                 * and its state may have changed.
                                 */
{
    DWORD wsaError;

    /* Potential addresses to use no longer needed when connection is open. */
    if (tcpPtr->remoteAddrList) {
        freeaddrinfo(tcpPtr->remoteAddrList);
        tcpPtr->remoteAddrList = NULL;
        tcpPtr->remoteAddr     = NULL;
    }
    if (tcpPtr->localAddrList) {
        freeaddrinfo(tcpPtr->localAddrList);
        tcpPtr->localAddrList = NULL;
        tcpPtr->localAddr     = NULL;
    }

    /* TBD - do we need to ++numRefs since tcpPtr is "pointed" from completion
       port. Actually we should just pass NULL instead of tcpPtr since it is
       anyways accessible from the bufferPtr.
    */

    if (CreateIoCompletionPort((HANDLE) tcpPtr->so,
                               iocpModuleState.completion_port,
                               (ULONG_PTR) tcpPtr,
                               0) == NULL) {
        return Iocp_ReportLastWindowsError(interp, "couldn't attach socket to completion port: ");
    }

    IocpChannelLock(TcpChannelToIocpChannel(tcpPtr));
    // TBD assert tcpPtr->state == OPEN
    wsaError = IocpChannelPostReads(TcpChannelToIocpChannel(tcpPtr));
    IocpChannelUnlock(TcpChannelToIocpChannel(tcpPtr));
    if (wsaError)
        return Iocp_ReportWindowsError(interp, wsaError, "couldn't post read on socket: ");
    else
        return TCL_OK;
}


/*
 *------------------------------------------------------------------------
 *
 * IocpTcpBlockingConnect --
 *
 *    Attempt connection through each source destination address pair in
 *    blocking mode until one succeeds or all fail.
 *
 * Results:
 *    0 on success, other Windows error code.
 *
 * Side effects:
 *    On success, a TCP connection is establised. The tcpPtr state is changed
 *    to OPEN and an initialized socket is stored in it.
 *
 *    On failure, tcpPtr state is changed to CONNECT_FAILED and the returned
 *    error is also stored in tcpPtr->base.winError.
 *
 *------------------------------------------------------------------------
 */
static IocpWindowsError IocpTcpBlockingConnect(
    IocpChannel *chanPtr) /* May or may not be locked but caller must ensure
                           * exclusivity */
{
    IocpTcpChannel *tcpPtr = IocpChannelToTcpChannel(chanPtr);
    struct addrinfo *localAddr;
    struct addrinfo *remoteAddr;
    DWORD  winError = WSAHOST_NOT_FOUND; /* In case address lists are empty */
    SOCKET so = INVALID_SOCKET;

    for (remoteAddr = tcpPtr->remoteAddrList; remoteAddr; remoteAddr = remoteAddr->ai_next) {
        for (localAddr = tcpPtr->localAddrList; localAddr; localAddr = localAddr->ai_next) {
            if (remoteAddr->ai_family != localAddr->ai_family)
                continue;
            so = socket(localAddr->ai_family, SOCK_STREAM, 0);
            /* Note socket call, unlike WSASocket is overlapped by default */

            if (so != INVALID_SOCKET) {
                /* Sockets should not be inherited by children */
                SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);

                /* Bind local address. */
                if (bind(so, localAddr->ai_addr, (int) localAddr->ai_addrlen) == 0 &&
                    connect(so, remoteAddr->ai_addr, (int) remoteAddr->ai_addrlen) == 0) {
                    tcpPtr->base.state = IOCP_STATE_OPEN;
                    tcpPtr->so    = so;
                    return TCL_OK;
                }
            }
            winError = WSAGetLastError();
            /* No joy. Keep trying. */
            if (so != INVALID_SOCKET) {
                closesocket(so);
                so = INVALID_SOCKET;
            }
        }
    }


    /* Failed to connect. Return an error */
    tcpPtr->base.state    = IOCP_STATE_CONNECT_FAILED;
    tcpPtr->base.winError = winError;

    if (so != INVALID_SOCKET)
        closesocket(so);
    return winError;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpInitiateConnection --
 *
 *    Initiates an asynchronous TCP connection from the local address
 *    pointed to by tcpPtr->localAddr to the remote address tcpPtr->remoteAddr.
 *    One or both are updated to point to the next address (pair) to try
 *    next in case this attempt does not succeed.
 *
 * Results:
 *    0 if the connection was initiated successfully, otherwise Windows error
 *    code which is also stored in tcpPtr->base.winError.
 *
 * Side effects:
 *    A connect request is initiated and a completion buffer posted.
 *
 *------------------------------------------------------------------------
 */
static IocpWindowsError IocpTcpInitiateConnection(
    IocpTcpChannel *tcpPtr)  /* Caller must ensure exclusivity either by locking
                              * or ensuring no other thread can access */
{
    DWORD  winError = WSAHOST_NOT_FOUND; /* In case address lists are empty */
    SOCKET so = INVALID_SOCKET;

    IOCP_ASSERT(tcpPtr->base.state == IOCP_STATE_INIT || IOCP_STATE_CONNECTING);
    IOCP_ASSERT(tcpPtr->so == INVALID_SOCKET);

    tcpPtr->base.state = IOCP_STATE_CONNECTING;

    for ( ; tcpPtr->remoteAddr; tcpPtr->remoteAddr = tcpPtr->remoteAddr->ai_next) {
        for ( ; tcpPtr->localAddr; tcpPtr->localAddr = tcpPtr->localAddr->ai_next) {
            if (tcpPtr->remoteAddr->ai_family != tcpPtr->localAddr->ai_family)
                continue;
            so = socket(tcpPtr->localAddr->ai_family, SOCK_STREAM, 0);
            /* Note socket call, unlike WSASocket is overlapped by default */

            if (so != INVALID_SOCKET) {
                /* Sockets should not be inherited by children */
                SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);
                tcpPtr->so = so;
                winError = IocpTcpPostConnect(tcpPtr);
                if (winError == 0)
                    return 0;
                closesocket(so);
                so         = INVALID_SOCKET;
                tcpPtr->so = INVALID_SOCKET;
            } else {
                winError = WSAGetLastError();
                /* No joy. Keep trying. */
            }
            /* Keep trying pairing this remote addr with next local addr. */
        }
        /* No joy with this remote addr. Reset local to try with next remote */
        tcpPtr->localAddr = tcpPtr->localAddrList;
    }

    /*
     * Failed. We report the stored error in preference to error in current call.
     */
    if (tcpPtr->base.winError == 0)
        tcpPtr->base.winError = winError;
    if (so != INVALID_SOCKET)
        closesocket(so);
    return tcpPtr->base.winError;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpAsyncConnectFail --
 *
 *    Called to handle connection failure on an async attempt. Attempts
 *    to initiate a connection using another address or reports failure.
 *
 * Results:
 *    Returns TCL_OK if a new connection attempt is successfully posted,
 *    TCL_ERROR with a Windows error code stored in lockedChanPtr->winError.
 *
 * Side effects:
 *
 *------------------------------------------------------------------------
 */
IocpWindowsError IocpTcpAsyncConnectFail(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    IocpTcpChannel *tcpPtr = IocpChannelToTcpChannel(lockedChanPtr);

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_CONNECT_FAILED);
    if (tcpPtr->so != INVALID_SOCKET) {
        closesocket(tcpPtr->so);
        tcpPtr->so = INVALID_SOCKET;
    }
    return IocpTcpInitiateConnection(tcpPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Iocp_OpenTcpClient --
 *
 *	Opens a TCP client socket and creates a channel around it.
 *
 * Results:
 *	The channel or NULL if failed. An error message is returned in the
 *	interpreter on failure.
 *
 * Side effects:
 *	Opens a client socket and creates a new channel.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Iocp_OpenTcpClient(
    Tcl_Interp *interp,		/* For error reporting; can be NULL. */
    int port,			/* Port number to open. */
    const char *host,		/* Host on which to open port. */
    const char *myaddr,		/* Client-side address */
    int myport,			/* Client-side port */
    int async)			/* If nonzero, attempt to do an asynchronous
				 * connect. Otherwise we do a blocking
				 * connect. */
{
    const char *errorMsg = NULL;
    struct addrinfo *remoteAddrs = NULL, *localAddrs = NULL;
    char channelName[SOCK_CHAN_LENGTH];
    IocpTcpChannel *tcpPtr;
    Tcl_Channel     channel;
    IocpWindowsError winError;

#ifdef TBD

    if (TclpHasSockets(interp) != TCL_OK) {
	return NULL;
    }

    /*
     * Check that WinSock is initialized; do not call it if not, to prevent
     * system crashes. This can happen at exit time if the exit handler for
     * WinSock ran before other exit handlers that want to use sockets.
     */

    if (!SocketsEnabled()) {
	return NULL;
    }
#endif

    /*
     * Do the name lookups for the local and remote addresses.
     */

    if (!TclCreateSocketAddress(interp, &remoteAddrs, host, port, 0, &errorMsg)
            || !TclCreateSocketAddress(interp, &localAddrs, myaddr, myport, 1,
                    &errorMsg)) {
        if (interp != NULL) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                    "couldn't resolve addresses: %s", errorMsg));
        }
        goto fail;
    }

    tcpPtr = (IocpTcpChannel *) IocpChannelNew(interp, &tcpVtbl);
    if (tcpPtr == NULL) {
        if (interp != NULL) {
            Tcl_SetResult(interp, "couldn't allocate IocpTcpChannel", TCL_STATIC);
        }
        goto fail;
    }
    tcpPtr->remoteAddrList = remoteAddrs;
    tcpPtr->remoteAddr     = remoteAddrs; /* First in remote address list */
    tcpPtr->localAddrList  = localAddrs;
    tcpPtr->localAddr      = localAddrs;  /* First in local address list */

    if (async) {
        winError = IocpTcpInitiateConnection(tcpPtr);
        if (winError != ERROR_SUCCESS) {
            Iocp_ReportWindowsError(interp, winError, "couldn't open socket: ");
            goto fail;
        }
    }
    else {
        winError = IocpTcpBlockingConnect(TcpChannelToIocpChannel(tcpPtr));
        if (winError != ERROR_SUCCESS) {
            Iocp_ReportWindowsError(interp, winError, "couldn't open socket: ");
            goto fail;
        }
        /*
         * Connection now open but note no other thread has access to tcpPtr
         * yet and hence no locking needed. But once IocpTcpEnableTransfer
         * is called, that will be no longer true as the completion thread
         * may also access tcpPtr.
         */
        if (IocpTcpEnableTransfer(interp, tcpPtr) != TCL_OK)
            goto fail;
    }

    /*
     * At this point, the completion thread may have modified tcpPtr and
     * even changed its state from CONNECTING (in case of async connects)
     * or OPEN to something else. That's ok, we return a channel, the
     * state change will be handled appropriately in the next I/O call
     * or notifier callback which handles notifications from the completion
     * thread.
     *
     * However, this does mean do not take any further action based on
     * state of tcpPtr without locking it and checking the state.
     */

    /*
     * Create a Tcl channel that points back to this.
     * tcpPtr->numRefs++ since Tcl channel points to this
     * tcpPtr->numRefs-- since this function no longer needs a reference to it.
     * The two cancel and numRefs stays at 1 as allocated. The corresponding
     * unref will be via IocpCloseProc when Tcl closes the channel.
     */
    sprintf_s(channelName, sizeof(channelName)/sizeof(channelName[0]), SOCK_TEMPLATE, tcpPtr);
    channel = Tcl_CreateChannel(&IocpChannelDispatch, channelName,
                                        tcpPtr, (TCL_READABLE | TCL_WRITABLE));
    tcpPtr->base.channel = channel;
    /*
     * Do not access tcpPtr beyond this point in case calls into Tcl result
     * in recursive callbacks through the Tcl channel subsystem to us that
     * result in tcpPtr being freed. (Remember our ref count is reversed via
     * IocpCloseProc).
     */
    tcpPtr = NULL;
    if (TCL_ERROR == Tcl_SetChannelOption(NULL, channel,
                                          "-translation", "auto crlf")) {
	Tcl_Close(NULL, channel);
	return NULL;
    } else if (TCL_ERROR == Tcl_SetChannelOption(NULL, channel,
                                                 "-eofchar", "")) {
	Tcl_Close(NULL, channel);
	return NULL;
    }
    return channel;

fail:
    /*
     * Failure exit. If tcpPtr is allocated, rely on it to clean up
     * else we have to free up address list ourselves.
     * NOTE: tcpPtr (if not NULL) must NOT be locked when jumping here.
     */
    if (tcpPtr) {
        IocpChannel *chanPtr = TcpChannelToIocpChannel(tcpPtr);
        IocpChannelLock(chanPtr); /* Because IocpChannelDrop expects that */
        IocpChannelDrop(chanPtr); /* Will also free attached {local,remote}Addrs */
    }
    else {
        if (remoteAddrs != NULL) {
            freeaddrinfo(remoteAddrs);
        }
        if (localAddrs != NULL) {
            freeaddrinfo(localAddrs);
        }
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * IocpOpenTcpServer --
 *
 *	Opens a TCP server socket and creates a channel around it.
 *
 * Results:
 *	The channel or NULL if failed. If an error occurred, an error message
 *	is left in the interp's result if interp is not NULL.
 *
 * Side effects:
 *	Opens a server socket and creates a new channel.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
IocpOpenTcpServer(
    Tcl_Interp *interp,		/* For error reporting - may be NULL. */
    int port,			/* Port number to open. */
    const char *myHost,		/* Name of local host. */
    Tcl_TcpAcceptProc *acceptProc,
				/* Callback for accepting connections from new
				 * clients. */
    ClientData acceptProcData)	/* Data for the callback. */
{
    return 0;
#ifdef TBD
    SOCKET sock = INVALID_SOCKET;
    unsigned short chosenport = 0;
    struct addrinfo *addrlist = NULL;
    struct addrinfo *addrPtr;	/* Socket address to listen on. */
    TcpState *statePtr = NULL;	/* The returned value. */
    char channelName[SOCK_CHAN_LENGTH];
    u_long flag = 1;		/* Indicates nonblocking mode. */
    const char *errorMsg = NULL;

    if (TclpHasSockets(interp) != TCL_OK) {
	return NULL;
    }

    /*
     * Check that WinSock is initialized; do not call it if not, to prevent
     * system crashes. This can happen at exit time if the exit handler for
     * WinSock ran before other exit handlers that want to use sockets.
     */

    if (!SocketsEnabled()) {
	return NULL;
    }

    /*
     * Construct the addresses for each end of the socket.
     */

    if (!TclCreateSocketAddress(interp, &addrlist, myHost, port, 1, &errorMsg)) {
	goto error;
    }

    for (addrPtr = addrlist; addrPtr != NULL; addrPtr = addrPtr->ai_next) {
	sock = socket(addrPtr->ai_family, addrPtr->ai_socktype,
                addrPtr->ai_protocol);
	if (sock == INVALID_SOCKET) {
	    TclWinConvertError((DWORD) WSAGetLastError());
	    continue;
	}

	/*
	 * Win-NT has a misfeature that sockets are inherited in child
	 * processes by default. Turn off the inherit bit.
	 */

	SetHandleInformation((HANDLE) sock, HANDLE_FLAG_INHERIT, 0);

	/*
	 * Set kernel space buffering
	 */

	TclSockMinimumBuffers((void *)sock, TCP_BUFFER_SIZE);

	/*
	 * Make sure we use the same port when opening two server sockets
	 * for IPv4 and IPv6.
	 *
	 * As sockaddr_in6 uses the same offset and size for the port
	 * member as sockaddr_in, we can handle both through the IPv4 API.
	 */

	if (port == 0 && chosenport != 0) {
	    ((struct sockaddr_in *) addrPtr->ai_addr)->sin_port =
		htons(chosenport);
	}

	/*
	 * Bind to the specified port. Note that we must not call
	 * setsockopt with SO_REUSEADDR because Microsoft allows addresses
	 * to be reused even if they are still in use.
	 *
	 * Bind should not be affected by the socket having already been
	 * set into nonblocking mode. If there is trouble, this is one
	 * place to look for bugs.
	 */

	if (bind(sock, addrPtr->ai_addr, addrPtr->ai_addrlen)
	    == SOCKET_ERROR) {
	    TclWinConvertError((DWORD) WSAGetLastError());
	    closesocket(sock);
	    continue;
	}
	if (port == 0 && chosenport == 0) {
	    address sockname;
	    socklen_t namelen = sizeof(sockname);

	    /*
	     * Synchronize port numbers when binding to port 0 of multiple
	     * addresses.
	     */

	    if (getsockname(sock, &sockname.sa, &namelen) >= 0) {
		chosenport = ntohs(sockname.sa4.sin_port);
	    }
	}

	/*
	 * Set the maximum number of pending connect requests to the max
	 * value allowed on each platform (Win32 and Win32s may be
	 * different, and there may be differences between TCP/IP stacks).
	 */

	if (listen(sock, SOMAXCONN) == SOCKET_ERROR) {
	    TclWinConvertError((DWORD) WSAGetLastError());
	    closesocket(sock);
	    continue;
	}

	if (statePtr == NULL) {
	    /*
	     * Add this socket to the global list of sockets.
	     */
	    statePtr = NewSocketInfo(sock);
	} else {
	    AddSocketInfoFd( statePtr, sock );
	}
    }

error:
    if (addrlist != NULL) {
	freeaddrinfo(addrlist);
    }

    if (statePtr != NULL) {
	ThreadSpecificData *tsdPtr = TclThreadDataKeyGet(&dataKey);

	statePtr->acceptProc = acceptProc;
	statePtr->acceptProcData = acceptProcData;
	sprintf(channelName, SOCK_TEMPLATE, statePtr);
	statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
		statePtr, 0);
	/*
	 * Set up the select mask for connection request events.
	 */

	statePtr->selectEvents = FD_ACCEPT;

	/*
	 * Register for interest in events in the select mask. Note that this
	 * automatically places the socket into non-blocking mode.
	 */

	ioctlsocket(sock, (long) FIONBIO, &flag);
	SendMessageW(tsdPtr->hwnd, SOCKET_SELECT, (WPARAM) SELECT,
		    (LPARAM) statePtr);
	if (Tcl_SetChannelOption(interp, statePtr->channel, "-eofchar", "")
	    == TCL_ERROR) {
	    Tcl_Close(NULL, statePtr->channel);
	    return NULL;
	}
	return statePtr->channel;
    }

    if (interp != NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"couldn't open socket: %s",
		(errorMsg ? errorMsg : Tcl_PosixError(interp))));
    }

    if (sock != INVALID_SOCKET) {
	closesocket(sock);
    }
    return NULL;
#endif
}

int
Iocp_SocketObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    static const char *const socketOptions[] = {
	"-async", "-myaddr", "-myport", "-server", NULL
    };
    enum socketOptions {
	SKT_ASYNC, SKT_MYADDR, SKT_MYPORT, SKT_SERVER
    };
    int optionIndex, a, server = 0, port, myport = 0, async = 0;
    const char *host, *script = NULL, *myaddr = NULL;
    Tcl_Channel chan;

#ifdef TBD
    if (TclpHasSockets(interp) != TCL_OK) {
	return TCL_ERROR;
    }
#endif

    for (a = 1; a < objc; a++) {
	const char *arg = TclGetString(objv[a]);

	if (arg[0] != '-') {
	    break;
	}
	if (Tcl_GetIndexFromObj(interp, objv[a], socketOptions, "option",
		TCL_EXACT, &optionIndex) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch ((enum socketOptions) optionIndex) {
	case SKT_ASYNC:
	    if (server == 1) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"cannot set -async option for server sockets", -1));
		return TCL_ERROR;
	    }
	    async = 1;
	    break;
	case SKT_MYADDR:
	    a++;
	    if (a >= objc) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"no argument given for -myaddr option", -1));
		return TCL_ERROR;
	    }
	    myaddr = TclGetString(objv[a]);
	    break;
	case SKT_MYPORT: {
	    const char *myPortName;

	    a++;
	    if (a >= objc) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"no argument given for -myport option", -1));
		return TCL_ERROR;
	    }
	    myPortName = TclGetString(objv[a]);
	    if (TclSockGetPort(interp, myPortName, "tcp", &myport) != TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	}
	case SKT_SERVER:
	    if (async == 1) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"cannot set -async option for server sockets", -1));
		return TCL_ERROR;
	    }
	    server = 1;
	    a++;
	    if (a >= objc) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"no argument given for -server option", -1));
		return TCL_ERROR;
	    }
	    script = TclGetString(objv[a]);
	    break;
	default:
	    Tcl_Panic("Tcl_SocketObjCmd: bad option index to SocketOptions");
	}
    }
    if (server) {
	host = myaddr;		/* NULL implies INADDR_ANY */
	if (myport != 0) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "option -myport is not valid for servers", -1));
	    return TCL_ERROR;
	}
    } else if (a < objc) {
	host = TclGetString(objv[a]);
	a++;
    } else {
    wrongNumArgs:
	Tcl_WrongNumArgs(interp, 1, objv,
		"?-myaddr addr? ?-myport myport? ?-async? host port");
#ifndef BUILD_iocp
	((Interp *)interp)->flags |= INTERP_ALTERNATE_WRONG_ARGS;
#endif
	Tcl_WrongNumArgs(interp, 1, objv,
		"-server command ?-myaddr addr? port");
	return TCL_ERROR;
    }

    if (a == objc-1) {
	if (TclSockGetPort(interp, TclGetString(objv[a]), "tcp",
		&port) != TCL_OK) {
	    return TCL_ERROR;
	}
    } else {
	goto wrongNumArgs;
    }

    if (server) {
#ifdef TBD
	AcceptCallback *acceptCallbackPtr =
		ckalloc(sizeof(AcceptCallback));
	unsigned len = strlen(script) + 1;
	char *copyScript = ckalloc(len);

	memcpy(copyScript, script, len);
	acceptCallbackPtr->script = copyScript;
	acceptCallbackPtr->interp = interp;
	chan = Tcl_OpenTcpServer(interp, port, host, AcceptCallbackProc,
		acceptCallbackPtr);
	if (chan == NULL) {
	    ckfree(copyScript);
	    ckfree(acceptCallbackPtr);
	    return TCL_ERROR;
	}

	/*
	 * Register with the interpreter to let us know when the interpreter
	 * is deleted (by having the callback set the interp field of the
	 * acceptCallbackPtr's structure to NULL). This is to avoid trying to
	 * eval the script in a deleted interpreter.
	 */

	RegisterTcpServerInterpCleanup(interp, acceptCallbackPtr);

	/*
	 * Register a close callback. This callback will inform the
	 * interpreter (if it still exists) that this channel does not need to
	 * be informed when the interpreter is deleted.
	 */

	Tcl_CreateCloseHandler(chan, TcpServerCloseProc, acceptCallbackPtr);
#else
        chan = 0;
#endif
    } else {
	chan = Iocp_OpenTcpClient(interp, port, host, myaddr, myport, async);
	if (chan == NULL) {
	    return TCL_ERROR;
	}
    }

    Tcl_RegisterChannel(interp, chan);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(chan), -1));
    return TCL_OK;
}
