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
    IocpChannel base;           /* Common IOCP channel structure */
    SOCKET      so;             /* Winsock socket handle */
    struct addrinfo *remoteAddrList; /* List of potential remote addresses */
    struct addrinfo *remoteAddr;     /* Remote address in use.
                                      * Points into remoteAddrList */
    struct addrinfo *localAddrList;  /* List of potential local addresses */
    struct addrinfo *localAddr;      /* Local address in use
                                      * Points into localAddrList */
    int              flags;     /* Miscellaneous flags */
#define IOCP_TCP_CONNECT_ASYNC 0x1
} IocpTcpChannel;

void IocpTcpChannelInit(Tcl_Interp *interp, IocpChannel *basePtr);
void IocpTcpChannelFinit(Tcl_Interp *interp, IocpChannel *chanPtr);

static IocpChannelVtbl tcpVtbl =  {
    IocpTcpChannelInit,
    IocpTcpChannelFinit,
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
void IocpTcpChannelInit(Tcl_Interp *interp, IocpChannel *basePtr)
{
    IocpTcpChannel *chanPtr = (IocpTcpChannel *) basePtr;
    chanPtr->so             = INVALID_SOCKET;
    chanPtr->remoteAddrList = NULL;
    chanPtr->remoteAddr     = NULL;
    chanPtr->localAddrList  = NULL;
    chanPtr->localAddr      = NULL;
    chanPtr->flags          = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * IocpOpenTcpClient --
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
IocpOpenTcpClient(
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
    IocpTcpChannel *chanPtr;

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

    chanPtr = (IocpTcpChannel *) IocpChannelNew(interp, &tcpVtbl);
    if (chanPtr == NULL) {
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
    chanPtr->remoteAddrList = remoteAddrs;
    chanPtr->remoteAddr     = remoteAddrs; /* First in remote address list */
    chanPtr->localAddrList  = localAddrs;
    chanPtr->localAddr      = localAddrs; /* First in local address list */

    if (async)
        chanPtr->flags |= IOCP_TCP_CONNECT_ASYNC;


#ifdef TBD
    statePtr = NewSocketInfo(INVALID_SOCKET);
    statePtr->addrlist = addrlist;
    statePtr->localAddrs = myaddrlist;
    if (async) {
	statePtr->flags |= TCP_ASYNC_CONNECT;
    }

    /*
     * Create a new client socket and wrap it in a channel.
     */
    if (TcpConnect(interp, statePtr) != TCL_OK) {
	TcpCloseProc(statePtr, NULL);
	return NULL;
    }

    sprintf(channelName, SOCK_TEMPLATE, statePtr);

    statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    statePtr, (TCL_READABLE | TCL_WRITABLE));
    if (TCL_ERROR == Tcl_SetChannelOption(NULL, statePtr->channel,
	    "-translation", "auto crlf")) {
	Tcl_Close(NULL, statePtr->channel);
	return NULL;
    } else if (TCL_ERROR == Tcl_SetChannelOption(NULL, statePtr->channel,
	    "-eofchar", "")) {
	Tcl_Close(NULL, statePtr->channel);
	return NULL;
    }
    return statePtr->channel;
#endif
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
#ifndef BUILD_iocp
	Interp *iPtr;
#endif
    wrongNumArgs:
	Tcl_WrongNumArgs(interp, 1, objv,
		"?-myaddr addr? ?-myport myport? ?-async? host port");
#ifndef BUILD_iocp
	iPtr = (Interp *) interp;
	iPtr->flags |= INTERP_ALTERNATE_WRONG_ARGS;
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
	chan = Tcl_OpenTcpClient(interp, port, host, myaddr, myport, async);
	if (chan == NULL) {
	    return TCL_ERROR;
	}
    }

    Tcl_RegisterChannel(interp, chan);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(chan), -1));
    return TCL_OK;
}
