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
#include <windows.h>
#include "tcl.h"
#include "tclWinIocp.h"


/*
 * Copied from Tcl - 
 * "sock" + a pointer in hex + \0
 */
#define SOCK_CHAN_LENGTH        (4 + sizeof(void *) * 2 + 1)
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

static void IocpTcpChannelInit(IocpChannel *basePtr);
static void IocpTcpChannelFinit(IocpChannel *chanPtr);
static int IocpTcpChannelShutdown(Tcl_Interp *, IocpChannel *chanPtr, int flags);
static DWORD IocpTcpPostRead(IocpChannel *lockedChanPtr);

static IocpChannelVtbl tcpVtbl =  {
    IocpTcpChannelInit,
    IocpTcpChannelFinit,
    IocpTcpChannelShutdown,
    IocpTcpPostRead,
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
 * IocpTcpPostRead --
 *
 *    Allocates a receive buffer and posts it to the socket associated
 *    with lockedTcpPtr.
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
static DWORD IocpTcpPostRead(IocpChannel *lockedChanPtr)
{
    IocpTcpChannel *lockedTcpPtr = IocpChannelToTcpChannel(lockedChanPtr);
    IocpBuffer *bufPtr;
    WSABUF      wsaBuf;
    DWORD       flags;
    DWORD       wsaError;

    IOCP_ASSERT(lockedTcpPtr->base.state == IOCP_STATE_OPEN);
    bufPtr = IocpBufferNew(IOCP_BUFFER_DEFAULT_SIZE);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->winError   = 0;
    bufPtr->operation  = IOCP_BUFFER_OP_READ;
    bufPtr->flags     |= IOCP_BUFFER_F_WINSOCK;
    bufPtr->chanPtr    = TcpChannelToIocpChannel(lockedTcpPtr);
    lockedTcpPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

    IocpInitWSABUF(&wsaBuf, bufPtr);
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
static IocpResultCode IocpTcpEnableTransfer(
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
        return Iocp_ReportLastWindowsError(interp);
    }

    IocpChannelLock(TcpChannelToIocpChannel(tcpPtr));
    // TBD assert tcpPtr->state == OPEN
    wsaError = IocpChannelPostReads(TcpChannelToIocpChannel(tcpPtr));
    IocpChannelUnlock(TcpChannelToIocpChannel(tcpPtr));
    if (wsaError)
        return Iocp_ReportWindowsError(interp, wsaError);
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
 *    The passed tcpPtr should be in state INIT with no references from
 *    other threads and no attached sockets. It must not be locked on entry
 *    and will not be locked on return. Because this function does not do
 *    anything that will provide access to any other thread to tcpPtr,
 *    no synchronization is needed. Aside: this is the reason we do not
 *    call IocpTcpTransitionOpen from here.
 *
 * Results:
 *    TCL_OK on success, else TCL_ERROR with an error message in interp.
 *
 * Side effects:
 *    On success, a TCP connection is establised. The tcpPtr state is changed
 *    to OPEN and an initialized socket is stored in it.
 *
 *    On failure, tcpPtr state is changed to CONNECT_FAILED.
 *
 *------------------------------------------------------------------------
 */
static IocpResultCode IocpTcpBlockingConnect(
    Tcl_Interp *interp,
    IocpTcpChannel *tcpPtr /* Must NOT be locked on entry, NOT locked on return */
)
{
    struct addrinfo *localAddr;
    struct addrinfo *remoteAddr;
    SOCKET so = INVALID_SOCKET;

    for (remoteAddr = tcpPtr->remoteAddrList; remoteAddr; remoteAddr->ai_next) {
        for (localAddr = tcpPtr->localAddrList; localAddr; localAddr->ai_next) {
            if (remoteAddr->ai_family != localAddr->ai_family)
                continue;
            so = socket(localAddr->ai_family, SOCK_STREAM, 0);
            /* Note socket call, unlike WSASocket is overlapped by default */

            /* Sockets should not be inherited by children */
            SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);

            /* Bind local address. */
            if (bind(so, localAddr->ai_addr, localAddr->ai_addrlen) == 0 &&
                connect(so, remoteAddr->ai_addr, remoteAddr->ai_addrlen) == 0) {
                tcpPtr->base.state = IOCP_STATE_OPEN;
                tcpPtr->so    = so;
                return TCL_OK;
            }
            /* No joy. Keep trying. */
            closesocket(so);
            so = INVALID_SOCKET;
        }
    }

    /* Failed to connect. Return an error */
    /* TBD - do we need to map WSA error to Windows error */
    Iocp_ReportLastWindowsError(interp);
    if (so != INVALID_SOCKET)
        closesocket(so);
    return TCL_ERROR;
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
        if (remoteAddrs != NULL) {
            freeaddrinfo(remoteAddrs);
        }
        if (localAddrs != NULL) {
            freeaddrinfo(localAddrs);
        }
        if (interp != NULL) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                    "couldn't resolve addresses: %s", errorMsg));
        }
        return NULL;
    }

    tcpPtr = (IocpTcpChannel *) IocpChannelNew(interp, &tcpVtbl);
    if (tcpPtr == NULL) {
        if (remoteAddrs != NULL) {
            freeaddrinfo(remoteAddrs);
        }
        if (localAddrs != NULL) {
            freeaddrinfo(localAddrs);
        }
        if (interp != NULL) {
            Tcl_SetResult(interp, "couldn't allocate IocpTcpChannel", TCL_STATIC);
        }
        return NULL;
    }
    tcpPtr->remoteAddrList = remoteAddrs;
    tcpPtr->remoteAddr     = remoteAddrs; /* First in remote address list */
    tcpPtr->localAddrList  = localAddrs;
    tcpPtr->localAddr      = localAddrs;  /* First in local address list */

    if (async) {
#ifdef TBD
        if (IocpTcpInitiateConnect(interp, tcpPtr) != TCL_OK) {
            goto fail;
        }
#endif
    }
    else {
        if (IocpTcpBlockingConnect(interp, tcpPtr) != TCL_OK)
            goto fail;
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
    sprintf(channelName, SOCK_TEMPLATE, tcpPtr);
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
     * NOTE: tcpPtr must NOT be locked when jumping here.
     */
    if (tcpPtr) {
        IocpChannel *chanPtr = TcpChannelToIocpChannel(tcpPtr);
        IocpChannelDrop(chanPtr);
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
