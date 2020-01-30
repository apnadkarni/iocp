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
#define SOCK_TEMPLATE           SOCK_CHAN_NAME_PREFIX "%p"


/*
 * Copied from Tcl - 
 * This is needed to comply with the strict aliasing rules of GCC, but it also
 * simplifies casting between the different sockaddr types.
 */

typedef union {
    struct sockaddr sa;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
    struct sockaddr_storage sas;
} IocpInetAddress;
#ifndef IN6_ARE_ADDR_EQUAL
#define IN6_ARE_ADDR_EQUAL IN6_ADDR_EQUAL
#endif
/*
 * Make sure to remove the redirection defines set in tclWinPort.h that is in
 * use in other sections of the core, except for us.
 */

#undef getservbyname
#undef getsockopt
#undef setsockopt
/* END Copied from Tcl */

/*
 * Tcp socket options. Note order must match order of string option names
 * in the iocpTcpOptionNames array.
 */
enum IocpTcpOption {
    IOCP_TCP_OPT_PEERNAME,
    IOCP_TCP_OPT_SOCKNAME,
    IOCP_TCP_OPT_ERROR,
    IOCP_TCP_OPT_CONNECTING,
    IOCP_TCP_OPT_INVALID        /* Must be last */
};

/*
 * Options supported by TCP sockets. Note the order MUST match IocpTcpOptions
 */
static const char *iocpTcpOptionNames[] = {
    "-peername",
    "-sockname",
    "-error",
    "-connecting",
    NULL
};

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

static void             IocpTcpChannelInit(IocpChannel *basePtr);
static void             IocpTcpChannelFinit(IocpChannel *chanPtr);
static int              IocpTcpChannelShutdown(Tcl_Interp *,
                                               IocpChannel *chanPtr, int flags);
static IocpWinError IocpTcpPostConnect(IocpTcpChannel *chanPtr);
static IocpWinError IocpTcpBlockingConnect(IocpChannel *);
static IocpWinError IocpTcpPostRead(IocpChannel *);
static IocpWinError IocpTcpPostWrite(IocpChannel *, const char *data,
                                         int nbytes, int *countPtr);
static IocpWinError IocpTcpAsyncConnectFailed(IocpChannel *lockedChanPtr);
static IocpWinError IocpTcpAsyncConnected(IocpChannel *lockedChanPtr);
static IocpTclCode      IocpTcpGetHandle(IocpChannel *lockedChanPtr,
                                         int direction, ClientData *handlePtr);
static IocpTclCode      IocpTcpGetOption (IocpChannel *lockedChanPtr,
                                              Tcl_Interp *interp, int optIndex,
                                              Tcl_DString *dsPtr);
static IocpWinError IocpTcpListifyAddress(const IocpInetAddress *addr,
                                              int addr_size, int noRDNS,
                                              Tcl_DString *dsPtr);
static void IocpTcpFreeAddresses(IocpTcpChannel *tcpPtr);

static IocpChannelVtbl tcpVtbl =  {
    /* "Virtual" functions */
    IocpTcpChannelInit,
    IocpTcpChannelFinit,
    IocpTcpChannelShutdown,
    IocpTcpBlockingConnect,
    IocpTcpAsyncConnected,
    IocpTcpAsyncConnectFailed,
    IocpTcpPostRead,
    IocpTcpPostWrite,
    IocpTcpGetHandle,
    IocpTcpGetOption,
    NULL,                       /* SetOption */
    /* Data members */
    iocpTcpOptionNames,
    sizeof(IocpTcpChannel)
};


/*
 *------------------------------------------------------------------------
 *
 * IocpTcpListifyAddress --
 *
 *    Converts a IP4/IP6 SOCKADDR to a list consisting of the IP address
 *    the name and the port number. If the address cannot be mapped to a
 *    name name, the numeric address is returned in that field.
 *
 * Results:
 *    Returns 0 on success with *dsPtr filled in or a Windows error code
 *    on failure.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
IocpWinError IocpTcpListifyAddress(
    const IocpInetAddress *addrPtr,    /* Address to listify */
    int                    addrSize,   /* Size of address structure */
    int                    noRDNS,     /* If true, no name lookup is done */
    Tcl_DString           *dsPtr)      /* Caller-initialized location for output */
{
    int  flags;
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    int  dsLen;

    if (getnameinfo(&addrPtr->sa, addrSize, host, sizeof(host)/sizeof(host[0]),
                    service, sizeof(service)/sizeof(service[0]),
                    NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
        return WSAGetLastError();
    }

    /* Note original length in case we need to revert after modification */
    dsLen = Tcl_DStringLength(dsPtr);

    Tcl_DStringStartSublist(dsPtr);
    Tcl_DStringAppendElement(dsPtr, host);

    if (noRDNS) {
        /* No reverse lookup, so just repeat in numeric address form */
        Tcl_DStringAppendElement(dsPtr, host);
    }
    else {
        flags = noRDNS ? NI_NUMERICHOST|NI_NUMERICSERV : NI_NUMERICSERV;

        /*
         * Based on comments in 8.6 Tcl winsock, do not try resolving
         * INADDR_ANY and sin6addr_any as they sometimes cause problems.
         * (no mention of what problems)
         */
        if (addrPtr->sa.sa_family == AF_INET) {
            if (addrPtr->sa4.sin_addr.s_addr == INADDR_ANY)
                flags |= NI_NUMERICHOST; /* Turn off lookup */
        }
        else {
            IOCP_ASSERT(addrPtr->sa.sa_family == AF_INET6);
            if ((IN6_ARE_ADDR_EQUAL(&addrPtr->sa6.sin6_addr,
				    &in6addr_any)) ||
                (IN6_IS_ADDR_V4MAPPED(&addrPtr->sa6.sin6_addr)
                 && addrPtr->sa6.sin6_addr.s6_addr[12] == 0
                 && addrPtr->sa6.sin6_addr.s6_addr[13] == 0
                 && addrPtr->sa6.sin6_addr.s6_addr[14] == 0
                 && addrPtr->sa6.sin6_addr.s6_addr[15] == 0)) {
                flags |= NI_NUMERICHOST;
            }
        }
        if (getnameinfo(&addrPtr->sa, addrSize, host,
                        sizeof(host)/sizeof(host[0]),
                        NULL, 0, flags) != 0) {
            Tcl_DStringTrunc(dsPtr, dsLen); /* Restore */
            return WSAGetLastError();
        }

        Tcl_DStringAppendElement(dsPtr, host);
    }

    Tcl_DStringAppendElement(dsPtr, service);
    Tcl_DStringEndSublist(dsPtr);

    return 0;
}

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
 * IocpTcpGetOption --
 *
 *    Returns the value of the given option.
 *
 * Results:
 *    Returns TCL_OK on succes and TCL_ERROR on failure.
 *
 * Side effects:
 *    On success the value of the option is stored in *dsPtr.
 *
 *------------------------------------------------------------------------
 */
IocpTclCode IocpTcpGetOption(
    IocpChannel *lockedChanPtr, /* Locked on entry, locked on exit */
    Tcl_Interp  *interp,        /* For error reporting. May be NULL */
    int          opt,           /* Index into option table for option of interest */
    Tcl_DString *dsPtr)         /* Where to store the value */
{
    IocpTcpChannel *lockedTcpPtr  = IocpChannelToTcpChannel(lockedChanPtr);
    IocpInetAddress addr;
    IocpWinError    winError;
    int  addrSize;
    int  noRDNS = 0;
#define SUPPRESS_RDNS_VAR "::tcl::unsupported::noReverseDNS"

    if (lockedTcpPtr->so == INVALID_SOCKET) {
        if (interp)
            Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
        return TCL_ERROR;
    }
    if (interp != NULL && Tcl_GetVar(interp, SUPPRESS_RDNS_VAR, 0) != NULL) {
	noRDNS = NI_NUMERICHOST;
    }

    switch (opt) {
    case IOCP_TCP_OPT_CONNECTING:
        Tcl_DStringAppend(dsPtr,
                          lockedTcpPtr->base.state == IOCP_STATE_CONNECTING ? "1" : "0",
                          1);
        return TCL_OK;
    case IOCP_TCP_OPT_ERROR:
        /* As per Tcl winsock, do not report errors in connecting state */
        if (lockedTcpPtr->base.state != IOCP_STATE_CONNECTING &&
            lockedTcpPtr->base.winError != ERROR_SUCCESS) {
            Tcl_Obj *objPtr = Iocp_MapWindowsError(lockedTcpPtr->base.winError,
                                                   NULL, NULL);
            int      nbytes;
            char    *emessage = Tcl_GetStringFromObj(objPtr, &nbytes);
            Tcl_DStringAppend(dsPtr, emessage, nbytes);
            Tcl_DecrRefCount(objPtr);
        }
        return TCL_OK;
    case IOCP_TCP_OPT_PEERNAME:
    case IOCP_TCP_OPT_SOCKNAME:
        addrSize = sizeof(addr);
        if ((opt==IOCP_TCP_OPT_PEERNAME?getpeername:getsockname)(lockedTcpPtr->so, &addr.sa, &addrSize) != 0)
            winError = WSAGetLastError();
        else
            winError = IocpTcpListifyAddress(&addr, addrSize, noRDNS, dsPtr);
        if (winError == 0)
            return TCL_OK;
        else 
            return Iocp_ReportWindowsError(interp, winError, NULL);
    default:
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Internal error: invalid socket option index %d", opt));
        return TCL_ERROR;
    }
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
static IocpWinError IocpTcpPostConnect(
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
static IocpWinError IocpTcpPostRead(IocpChannel *lockedChanPtr)
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
static IocpWinError IocpTcpPostWrite(
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
 * IocpTcpFreeAddresses --
 *
 *    Frees the address lists associated with a channel.
 *
 *    Caller must ensure no other thread can access tcpPtr during this call.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Address lists are freed.
 *
 *------------------------------------------------------------------------
 */
static void IocpTcpFreeAddresses(
    IocpTcpChannel *tcpPtr)     /* Pointer to Tcp channel state. Caller should
                                 * ensure no other threads can access. */
{
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
static IocpWinError IocpTcpBlockingConnect(
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
    tcpPtr->base.state    = IOCP_STATE_DISCONNECTED;
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
static IocpWinError IocpTcpInitiateConnection(
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
    tcpPtr->base.state = IOCP_STATE_DISCONNECTED;
    if (tcpPtr->base.winError == 0)
        tcpPtr->base.winError = winError;
    if (so != INVALID_SOCKET)
        closesocket(so);
    return tcpPtr->base.winError;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpAsyncConnected --
 *
 *    Called to handle connection establishment on an async attempt.
 *    Completes local connection set up.
 *    Follows the API defined by connectfail() in IocpChannel vtbl.
 *
 * Results:
 *    Returns 0 if connection set up is successful or a Windows error code which
 *    is also stored in lockedChanPtr->winError.
 *
 * Side effects:
 *
 *------------------------------------------------------------------------
 */
IocpWinError IocpTcpAsyncConnected(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    IocpTcpChannel *tcpPtr = IocpChannelToTcpChannel(lockedChanPtr);

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_CONNECTED);
    /*
     * As per documentation this is required for complete context for the
     * socket to be set up. Otherwise certain Winsock calls like getpeername
     * getsockname shutdown will not work.
     */
    if (setsockopt(tcpPtr->so, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) {
        tcpPtr->base.winError = WSAGetLastError();
        closesocket(tcpPtr->so);
        tcpPtr->so = INVALID_SOCKET;
        return tcpPtr->base.winError;
    }
    else {
        return 0;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpAsyncConnectFailed --
 *
 *    Called to handle connection failure on an async attempt. Attempts
 *    to initiate a connection using another address or reports failure.
 *    Follows the API defined by connectfail() in IocpChannel vtbl.
 *
 * Results:
 *    Returns 0 if a new connection attempt is successfully posted,
 *    or a Windows error code which is also stored in lockedChanPtr->winError.
 *
 * Side effects:
 *
 *------------------------------------------------------------------------
 */
IocpWinError IocpTcpAsyncConnectFailed(
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
    IocpWinError winError;

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
        IocpTcpFreeAddresses(tcpPtr);
        /* TBD - do we need to ++numRefs since tcpPtr is "pointed" from completion
           port. Actually we should just pass NULL instead of tcpPtr since it is
           anyways accessible from the bufferPtr.
        */

        if (CreateIoCompletionPort((HANDLE) tcpPtr->so,
                                   iocpModuleState.completion_port,
                                   (ULONG_PTR) tcpPtr,
                                   0) == NULL) {
            Iocp_ReportLastWindowsError(interp, "couldn't attach socket to completion port: ");
            goto fail;
        }
        /*
         * NOTE Connection now open but note no other thread has access to tcpPtr
         * yet and hence no locking needed. But once Reads are posted that
         * will be no longer true as the completion thread may also access tcpPtr.
         */
        winError = IocpChannelPostReads(TcpChannelToIocpChannel(tcpPtr));
        if (winError) {
            Iocp_ReportWindowsError(interp, winError, "couldn't post read on socket: ");
            goto fail;
        }
    }

    /*
     * At this point, the completion thread may have modified tcpPtr and
     * even changed its state from CONNECTING (in case of async connects)
     * or OPEN to something else. That's ok, we return a channel, the
     * state change will be handled appropriately in the next I/O call
     * or notifier callback which handles notifications from the completion
     * thread.
     *
     * HOWEVER, THIS DOES MEAN DO NOT TAKE ANY FURTHER ACTION BASED ON
     * STATE OF TCPPTR WITHOUT LOCKING IT AND CHECKING THE STATE.
     */

    /*
     * CREATE a Tcl channel that points back to this.
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
