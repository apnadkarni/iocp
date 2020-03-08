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
# define IOCP_SOCK_NAME_PREFIX   "tcp"
#else
# define IOCP_SOCK_NAME_PREFIX   "sock"
#endif

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
 * TBD - is this even necessary any more?
 */

#undef getservbyname
#undef getsockopt
#undef setsockopt
/* END Copied from Tcl */

/* Just to ensure consistency */
const char *gSocketOpenErrorMessage = "couldn't open socket: ";

/*
 * Tcp socket options. Note order must match order of string option names
 * in the iocpTcpOptionNames array.
 */
enum IocpTcpOption {
    IOCP_TCP_OPT_PEERNAME,
    IOCP_TCP_OPT_SOCKNAME,
    IOCP_TCP_OPT_ERROR,
    IOCP_TCP_OPT_CONNECTING,
    IOCP_TCP_OPT_MAXPENDINGREADS,
    IOCP_TCP_OPT_MAXPENDINGWRITES,
    IOCP_TCP_OPT_MAXPENDINGACCEPTS,
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
    "-maxpendingreads",
    "-maxpendingwrites",
    "-maxpendingaccepts",
    NULL
};

/****************************************************************
 * TCP client channel structures
 ****************************************************************/

/* TCP client channel state */
typedef struct TcpClient {
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

} TcpClient;

/*
 * Prototypes for TCP client implementation
 */

IOCP_INLINE IocpChannel *TcpClientToIocpChannel(TcpClient *tcpPtr) {
    return (IocpChannel *) tcpPtr;
}
IOCP_INLINE TcpClient *IocpChannelToTcpClient(IocpChannel *chanPtr) {
    return (TcpClient *) chanPtr;
}

static void         TcpClientInit(IocpChannel *basePtr);
static void         TcpClientFinit(IocpChannel *chanPtr);
static int          TcpClientShutdown(Tcl_Interp *,
                                      IocpChannel *chanPtr, int flags);
static IocpWinError TcpClientPostConnect(TcpClient *chanPtr);
static IocpWinError TcpClientBlockingConnect(IocpChannel *);
static IocpWinError TcpClientPostRead(IocpChannel *);
static IocpWinError TcpClientPostWrite(IocpChannel *, const char *data,
                                         int nbytes, int *countPtr);
static IocpWinError TcpClientPostDisconnect(TcpClient *chanPtr);
static IocpWinError TcpClientAsyncConnectFailed(IocpChannel *lockedChanPtr);
static IocpWinError TcpClientAsyncConnected(IocpChannel *lockedChanPtr);
static void         TcpClientDisconnected(IocpChannel *lockedChanPtr);
static IocpTclCode  TcpClientGetHandle(IocpChannel *lockedChanPtr,
                                       int direction, ClientData *handlePtr);
static IocpTclCode  TcpClientGetOption (IocpChannel *lockedChanPtr,
                                              Tcl_Interp *interp, int optIndex,
                                              Tcl_DString *dsPtr);
static IocpTclCode  TcpClientSetOption (IocpChannel *lockedChanPtr,
                                        Tcl_Interp *interp, int optIndex,
                                        const char *valuePtr);
static IocpWinError TcpClientTranslateError(IocpChannel *chanPtr,
                                            IocpBuffer *bufPtr);
static IocpWinError IocpTcpListifyAddress(const IocpInetAddress *addr,
                                          int addr_size, int noRDNS,
                                          Tcl_DString *dsPtr);
static void         TcpClientFreeAddresses(TcpClient *tcpPtr);

static IocpChannelVtbl tcpClientVtbl =  {
    /* "Virtual" functions */
    TcpClientInit,
    TcpClientFinit,
    TcpClientShutdown,
    NULL,                       /* Accept */
    TcpClientBlockingConnect,
    TcpClientAsyncConnected,
    TcpClientAsyncConnectFailed,
    TcpClientDisconnected,
    TcpClientPostRead,
    TcpClientPostWrite,
    TcpClientGetHandle,
    TcpClientGetOption,
    TcpClientSetOption,
    TcpClientTranslateError,
    /* Data members */
    iocpTcpOptionNames,
    sizeof(TcpClient)
};


/****************************************************************
 * TCP listener structures
 ****************************************************************/

/*
 * Holds information about a single socket associated with a listening channel.
 */
typedef struct TcpListeningSocket {
    SOCKET                    so;
    LPFN_ACCEPTEX	      _AcceptEx; /* Winsock function to post accepts */
    LPFN_GETACCEPTEXSOCKADDRS _GetAcceptExSockaddrs; /* Winsock function to
                                                      * retrieve addresses */
    int                       aiFamily;   /* Address family info stored ... */
    int                       aiSocktype; /* ... to create sockets ... */
    int                       aiProtocol; /* ... passed to _AcceptEx */
    int                       pendingAcceptPosts; /* #queued accepts posts */
    int                       maxPendingAcceptPosts; /* Loose max of above */
#define IOCP_TCP_MAX_ACCEPTS 3  /* TBD */
} TcpListeningSocket;

typedef struct TcpAcceptBuffer {
#define IOCP_ACCEPT_ADDRESS_LEN (16*sizeof(IocpInetAddress))
    char addresses[2*IOCP_ACCEPT_ADDRESS_LEN]; /* Will be used to retrieve
                                                * connection addresses as
                                                * part of AcceptEx call. See
                                                * MSDN docs for sizing */
    int  listenerIndex;                       /* Index into TcpListener.listeners[] */
} TcpAcceptBuffer;

/* TCP listener channel state */
typedef struct TcpListener {
    IocpChannel         base;           /* Common IOCP channel structure. Must be
                                         * first because of how structures are
                                         * allocated and freed */
    Tcl_TcpAcceptProc  *acceptProc;     /* Callback to notify of new accept */
    ClientData          acceptProcData; /* Data for the callback. */
    TcpListeningSocket *listeners;      /* Array of listening sockets */
    int                 numListeners;   /* Only 0..numListeners-1 elements of
                                         * listeners[] should be examined and
                                         * can have value INVALID_SOCKET */
    int                 maxPendingAcceptPosts;
} TcpListener;

/*
 * Prototypes for TCP client implementation
 */

IOCP_INLINE IocpChannel *TcpListenerToIocpChannel(TcpListener *tcpPtr) {
    return (IocpChannel *) tcpPtr;
}
IOCP_INLINE TcpListener *IocpChannelToTcpListener(IocpChannel *chanPtr) {
    return (TcpListener *) chanPtr;
}

static IocpWinError TcpListenerPostAccepts(TcpListener *lockedTcpPtr,
                                          int listenerIndex);

static void         TcpListenerInit(IocpChannel *basePtr);
static void         TcpListenerFinit(IocpChannel *chanPtr);
static int          TcpListenerShutdown(Tcl_Interp *,
                                      IocpChannel *chanPtr, int flags);
static IocpWinError TcpListenerAccept(IocpChannel *lockedChanPtr);
static IocpTclCode  TcpListenerGetHandle(IocpChannel *lockedChanPtr,
                                       int direction, ClientData *handlePtr);
static IocpTclCode  TcpListenerGetOption (IocpChannel *lockedChanPtr,
                                              Tcl_Interp *interp, int optIndex,
                                              Tcl_DString *dsPtr);
static IocpChannelVtbl tcpListenerVtbl = {
    /* "Virtual" functions */
    TcpListenerInit,
    TcpListenerFinit,
    TcpListenerShutdown,
    TcpListenerAccept,
    NULL, /* BlockingConnect */
    NULL, /* AsyncConnected */
    NULL, /* AsyncConnectFailed */
    NULL, /* Disconnected */
    NULL, /* PostRead */
    NULL, /* PostWrite */
    NULL, // TBD TcpListenerGetHandle,
    TcpListenerGetOption,
    NULL,                       /* SetOption */
    NULL,                       /* translateerror */
    /* Data members */
    iocpTcpOptionNames,
    sizeof(TcpListener)
};


/*
 * Function implementation
 */

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpSetChannelDefaults --
 *
 *    Sets the default -translation and -eofchar values  for TCP channels.
 *
 * Results:
 *    TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
IocpTclCode IocpTcpSetChannelDefaults(
    Tcl_Channel channel
    )
{
    if (Tcl_SetChannelOption(NULL, channel,
                             "-translation", "auto crlf") == TCL_ERROR ||
        Tcl_SetChannelOption(NULL, channel, "-eofchar", "") == TCL_ERROR) {
        return TCL_ERROR;
    }
    return TCL_OK;
}


/*
 *------------------------------------------------------------------------
 *
 * IocpTcpListifyAddress --
 *
 *    Converts a IP4/IP6 SOCKADDR to a list consisting of the IP address
 *    the name and the port number. If the address cannot be mapped to a
 *    name name, the numeric address is returned in that field.
 *
 *    Assumes caller will wrap call with a Tcl_DString{Start,End}Sublist pair.
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

    if (getnameinfo(&addrPtr->sa, addrSize, host, sizeof(host)/sizeof(host[0]),
                    service, sizeof(service)/sizeof(service[0]),
                    NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
        return WSAGetLastError();
    }

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
            return WSAGetLastError();
        }

        Tcl_DStringAppendElement(dsPtr, host);
    }

    Tcl_DStringAppendElement(dsPtr, service);

    return ERROR_SUCCESS;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientInit --
 *
 *    Initializes the TcpClient part of a IocpChannel structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static void TcpClientInit(IocpChannel *chanPtr)
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(chanPtr);
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
 * TcpClientFinit --
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
static void TcpClientFinit(IocpChannel *chanPtr)
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(chanPtr);

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
 * TcpClientShutdown --
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
static int TcpClientShutdown(
    Tcl_Interp   *interp,        /* May be NULL */
    IocpChannel *lockedChanPtr, /* Locked pointer to the base IocpChannel */
    int          flags)         /* Combination of TCL_CLOSE_{READ,WRITE} */
{
    TcpClient *lockedTcpPtr = IocpChannelToTcpClient(lockedChanPtr);

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
            /*
             * Doing just a closesocket seems to result in a TCP RESET
             * instead of graceful close. Do use DisconnectEx if
             * available.
             */
            if (TcpClientPostDisconnect(lockedTcpPtr) != ERROR_SUCCESS) {
                wsaStatus = closesocket(lockedTcpPtr->so);
                lockedTcpPtr->so = INVALID_SOCKET;
            }
            break;
        default:                /* Not asked to close either */
            return 0;
        }
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
 * TcpClientGetHandle --
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
static IocpTclCode TcpClientGetHandle(
    IocpChannel *lockedChanPtr,
    int direction,
    ClientData *handlePtr)
{
    SOCKET so = IocpChannelToTcpClient(lockedChanPtr)->so;
    if (so == INVALID_SOCKET)
        return TCL_ERROR;
    *handlePtr = (ClientData) so;
    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientGetOption --
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
IocpTclCode TcpClientGetOption(
    IocpChannel *lockedChanPtr, /* Locked on entry, locked on exit */
    Tcl_Interp  *interp,        /* For error reporting. May be NULL */
    int          opt,           /* Index into option table for option of interest */
    Tcl_DString *dsPtr)         /* Where to store the value */
{
    TcpClient *lockedTcpPtr  = IocpChannelToTcpClient(lockedChanPtr);
    IocpInetAddress addr;
    IocpWinError    winError;
    int  addrSize;
    char integerSpace[TCL_INTEGER_SPACE];
    int  noRDNS = 0;
#define SUPPRESS_RDNS_VAR "::tcl::unsupported::noReverseDNS"

    switch (opt) {
    case IOCP_TCP_OPT_CONNECTING:
        Tcl_DStringAppend(dsPtr,
                          lockedTcpPtr->base.state == IOCP_STATE_CONNECTING ? "1" : "0",
                          1);
        return TCL_OK;
    case IOCP_TCP_OPT_ERROR:
        /* As per Tcl winsock, do not report errors in connecting state */
        if (lockedTcpPtr->base.state != IOCP_STATE_CONNECTING &&
            lockedTcpPtr->base.state != IOCP_STATE_CONNECT_RETRY &&
            lockedTcpPtr->base.winError != ERROR_SUCCESS) {
#if 1
            IocpSetTclErrnoFromWin32(lockedTcpPtr->base.winError);
            Tcl_DStringAppend(dsPtr, Tcl_ErrnoMsg(Tcl_GetErrno()), -1);
            lockedTcpPtr->base.winError = ERROR_SUCCESS; /* As per socket man page */
#else
            /* This would give more detail but Tcl test suite demands Posix error */
            Tcl_Obj *objPtr = Iocp_MapWindowsError(lockedTcpPtr->base.winError,
                                                   NULL, NULL);
            int      nbytes;
            char    *emessage = Tcl_GetStringFromObj(objPtr, &nbytes);
            Tcl_DStringAppend(dsPtr, emessage, nbytes);
            Tcl_DecrRefCount(objPtr);
            lockedTcpPtr->base.winError = ERROR_SUCCESS; /* As per socket man page */
#endif
        }
        return TCL_OK;
    case IOCP_TCP_OPT_PEERNAME:
    case IOCP_TCP_OPT_SOCKNAME:
        if (lockedTcpPtr->base.state == IOCP_STATE_CONNECTING ||
            lockedTcpPtr->base.state == IOCP_STATE_CONNECT_RETRY) {
            /* As per TIP 427, empty string to be returned in these states */
            return TCL_OK;
        }
        if (lockedTcpPtr->so == INVALID_SOCKET) {
            if (interp)
                Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
            return TCL_ERROR;
        }
        if (interp != NULL && Tcl_GetVar(interp, SUPPRESS_RDNS_VAR, 0) != NULL) {
            noRDNS = NI_NUMERICHOST;
        }
        addrSize = sizeof(addr);
        if ((opt==IOCP_TCP_OPT_PEERNAME?getpeername:getsockname)(lockedTcpPtr->so, &addr.sa, &addrSize) != 0)
            winError = WSAGetLastError();
        else {
            /* Note original length in case we need to revert after modification */
            int dsLen = Tcl_DStringLength(dsPtr);
            winError = IocpTcpListifyAddress(&addr, addrSize, noRDNS, dsPtr);
            if (winError != ERROR_SUCCESS)
                Tcl_DStringTrunc(dsPtr, dsLen); /* Restore */
        }
        if (winError == 0)
            return TCL_OK;
        else
            return Iocp_ReportWindowsError(interp, winError, NULL);
    case IOCP_TCP_OPT_MAXPENDINGREADS:
    case IOCP_TCP_OPT_MAXPENDINGWRITES:
        sprintf_s(integerSpace, sizeof(integerSpace),
                  "%d",
                  opt == IOCP_TCP_OPT_MAXPENDINGREADS ?
                  lockedChanPtr->maxPendingReads : lockedChanPtr->maxPendingWrites);
        Tcl_DStringAppend(dsPtr, integerSpace, -1);
        return TCL_OK;
    case IOCP_TCP_OPT_MAXPENDINGACCEPTS:
        Tcl_DStringAppend(dsPtr, "0", 1);
        return TCL_OK;
    default:
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Internal error: invalid socket option index %d", opt));
        return TCL_ERROR;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientSetOption --
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
IocpTclCode TcpClientSetOption(
    IocpChannel *lockedChanPtr, /* Locked on entry, locked on exit */
    Tcl_Interp  *interp,        /* For error reporting. May be NULL */
    int          opt,           /* Index into option table for option of interest */
    const char  *valuePtr)      /* Option value */
{
    TcpClient *lockedTcpPtr = IocpChannelToTcpClient(lockedChanPtr);
    int        intValue;

    if (lockedTcpPtr->so == INVALID_SOCKET) {
        if (interp)
            Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
        return TCL_ERROR;
    }

    switch (opt) {
    case IOCP_TCP_OPT_MAXPENDINGREADS:
    case IOCP_TCP_OPT_MAXPENDINGWRITES:
        if (Tcl_GetInt(interp, valuePtr, &intValue) != TCL_OK) {
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        if (intValue <= 0 || intValue > 20) {
            if (interp)
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("Integer value %d out of range.", intValue));
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        if (opt == IOCP_TCP_OPT_MAXPENDINGREADS)
            lockedChanPtr->maxPendingReads = intValue;
        else
            lockedChanPtr->maxPendingWrites = intValue;
        return TCL_OK;
    case IOCP_TCP_OPT_CONNECTING:
    case IOCP_TCP_OPT_ERROR:
    case IOCP_TCP_OPT_PEERNAME:
    case IOCP_TCP_OPT_SOCKNAME:
    case IOCP_TCP_OPT_MAXPENDINGACCEPTS:
        return Tcl_BadChannelOption(interp, iocpTcpOptionNames[opt], "-maxpendingreads -maxpendingwrites");
    default:
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Internal error: invalid socket option index %d", opt));
        Tcl_SetErrno(EINVAL);
        return TCL_ERROR;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientTranslateError --
 *
 *    Conforms to the IocpChannel vtbl translateerror API.
 *
 * Results:
 *    A Winsock-specific error code if possible, else bufPtr->winError.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError TcpClientTranslateError(IocpChannel *chanPtr, IocpBuffer *bufPtr)
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(chanPtr);
    if (bufPtr->winError != ERROR_SUCCESS) {
        DWORD flags = 0 ;
        DWORD nbytes = 0;
        if (WSAGetOverlappedResult(tcpPtr->so, &bufPtr->u.wsaOverlap, &nbytes, FALSE, &flags) == 0) {
            return WSAGetLastError();
        }
    }
    return bufPtr->winError;
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
static IocpWinError TcpClientPostConnect(
    TcpClient *tcpPtr  /* Channel pointer, may or may not be locked
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
                               0, /* Completion key - unused */
                               0) == NULL) {
        return GetLastError(); /* NOT WSAGetLastError() ! */
    }

    bufPtr = IocpBufferNew(0, IOCP_BUFFER_OP_CONNECT, IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->chanPtr    = TcpClientToIocpChannel(tcpPtr);
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
 * TcpClientPostDisconnect --
 *
 *    Initiates a disconnect on a connection.
 *
 * Results:
 *    ERROR_SUCCESS if disconnect request was successfully posted or an
 *    Winsock error code.
 *
 * Side effects:
 *    The Disconnect is queued on the IOCP completion port.
 *
 *------------------------------------------------------------------------
 */
IocpWinError TcpClientPostDisconnect(
    TcpClient *lockedTcpPtr     /* Must be locked */
    )
{
    static GUID     DisconnectExGuid = WSAID_DISCONNECTEX;
    LPFN_DISCONNECTEX  fnDisconnectEx;
    DWORD nbytes;

    if (WSAIoctl(lockedTcpPtr->so, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &DisconnectExGuid, sizeof(GUID),
                 &fnDisconnectEx,
                 sizeof(fnDisconnectEx),
                 &nbytes, NULL, NULL) != 0) {
        return WSAGetLastError();
    }
    else {
        IocpBuffer *bufPtr = IocpBufferNew(0, IOCP_BUFFER_OP_DISCONNECT, IOCP_BUFFER_F_WINSOCK);
        if (bufPtr == NULL)
            return WSAENOBUFS;
        bufPtr->chanPtr    = TcpClientToIocpChannel(lockedTcpPtr);
        lockedTcpPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

        if (fnDisconnectEx(lockedTcpPtr->so, &bufPtr->u.wsaOverlap, 0, 0) == FALSE) {
            IocpWinError    winError = WSAGetLastError();
            if (winError != WSA_IO_PENDING) {
                lockedTcpPtr->base.numRefs -= 1; /* Reverse above increment */
                bufPtr->chanPtr = NULL;          /* Else IocpBufferFree will assert */
                IocpBufferFree(bufPtr);
                return winError;
            }
        }
        return ERROR_SUCCESS;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientPostRead --
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
static IocpWinError TcpClientPostRead(IocpChannel *lockedChanPtr)
{
    TcpClient *lockedTcpPtr = IocpChannelToTcpClient(lockedChanPtr);
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
 * TcpClientPostWrite --
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
static IocpWinError TcpClientPostWrite(
    IocpChannel *lockedChanPtr, /* Must be locked on entry */
    const char  *bytes,         /* Pointer to data to write */
    int          nbytes,        /* Number of data bytes to write */
    int         *countPtr)      /* Output - Number of bytes written */
{
    TcpClient *lockedTcpPtr = IocpChannelToTcpClient(lockedChanPtr);
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
 * TcpClientFreeAddresses --
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
static void TcpClientFreeAddresses(
    TcpClient *tcpPtr)     /* Pointer to Tcp channel state. Caller should
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
 * TcpClientBlockingConnect --
 *
 *    Attempt connection through each source destination address pair in
 *    blocking mode until one succeeds or all fail.
 *
 * Results:
 *    0 on success, other Windows error code.
 *
 * Side effects:
 *    On success, a TCP connection is establised. The tcpPtr state is changed
 *    to OPEN and an initialized socket is stored in it. The IOCP completion
 *    port is associated with it.
 *
 *    On failure, tcpPtr state is changed to CONNECT_FAILED and the returned
 *    error is also stored in tcpPtr->base.winError.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError TcpClientBlockingConnect(
    IocpChannel *chanPtr) /* May or may not be locked but caller must ensure
                           * exclusivity */
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(chanPtr);
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

            if (so != INVALID_SOCKET &&
                bind(so, localAddr->ai_addr, (int) localAddr->ai_addrlen) == 0 &&
                connect(so, remoteAddr->ai_addr, (int) remoteAddr->ai_addrlen) == 0) {

                /* Sockets should not be inherited by children */
                SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);

                if (CreateIoCompletionPort((HANDLE) so,
                                           iocpModuleState.completion_port,
                                           0, /* Completion key - unused */
                                           0) != NULL) {
                    tcpPtr->base.state = IOCP_STATE_OPEN;
                    tcpPtr->so = so;
                    /*
                     * Clear any error stored during -async operation prior to
                     * blocking connect
                     */
                    tcpPtr->base.winError = ERROR_SUCCESS;
                    return ERROR_SUCCESS;
                }
                else
                    winError = GetLastError();
            }
            else
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
 * TcpClientInitiateConnection --
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
static IocpWinError TcpClientInitiateConnection(
    TcpClient *tcpPtr)  /* Caller must ensure exclusivity either by locking
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
                winError = TcpClientPostConnect(tcpPtr);
                if (winError == ERROR_SUCCESS) {
                    /* Update so next attempt will be with next local addr */
                    tcpPtr->localAddr = tcpPtr->localAddr->ai_next;
                    return ERROR_SUCCESS;
                }
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
    tcpPtr->base.state = IOCP_STATE_CONNECT_FAILED;
    if (tcpPtr->base.winError == 0)
        tcpPtr->base.winError = winError;
    if (so != INVALID_SOCKET)
        closesocket(so);
    return tcpPtr->base.winError;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientAsyncConnected --
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
static IocpWinError TcpClientAsyncConnected(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(lockedChanPtr);

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
 * TcpClientAsyncConnectFailed --
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
static IocpWinError TcpClientAsyncConnectFailed(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(lockedChanPtr);

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_CONNECT_RETRY);
    if (tcpPtr->so != INVALID_SOCKET) {
        closesocket(tcpPtr->so);
        tcpPtr->so = INVALID_SOCKET;
    }
    return TcpClientInitiateConnection(tcpPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * TcpClientDisconnected --
 *
 *    Closes the socket associated with the channel.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
static void TcpClientDisconnected(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    TcpClient *tcpPtr = IocpChannelToTcpClient(lockedChanPtr);

    if (tcpPtr->so != INVALID_SOCKET) {
        closesocket(tcpPtr->so);
        tcpPtr->so = INVALID_SOCKET;
    }
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
    TcpClient *tcpPtr = NULL;
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

    tcpPtr = (TcpClient *) IocpChannelNew(&tcpClientVtbl);
    if (tcpPtr == NULL) {
        if (interp != NULL) {
            Tcl_SetResult(interp, "couldn't allocate TcpClient", TCL_STATIC);
        }
        goto fail;
    }
    tcpPtr->remoteAddrList = remoteAddrs;
    tcpPtr->remoteAddr     = remoteAddrs; /* First in remote address list */
    tcpPtr->localAddrList  = localAddrs;
    tcpPtr->localAddr      = localAddrs;  /* First in local address list */

    if (async) {
        winError = TcpClientInitiateConnection(tcpPtr);
        if (winError != ERROR_SUCCESS) {
            IocpSetInterpPosixErrorFromWin32(interp, winError, gSocketOpenErrorMessage);
            goto fail;
        }
    }
    else {
        winError = TcpClientBlockingConnect( TcpClientToIocpChannel(tcpPtr) );
        if (winError != ERROR_SUCCESS) {
            IocpSetInterpPosixErrorFromWin32(interp, winError, gSocketOpenErrorMessage);
            goto fail;
        }
        TcpClientFreeAddresses(tcpPtr);

        /*
         * NOTE Connection now open but note no other thread has access to tcpPtr
         * yet and hence no locking needed. But once Reads are posted that
         * will be no longer true as the completion thread may also access tcpPtr.
         */
        winError = IocpChannelPostReads(TcpClientToIocpChannel(tcpPtr));
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
    channel = IocpCreateTclChannel(TcpClientToIocpChannel(tcpPtr),
                                   IOCP_SOCK_NAME_PREFIX,
                                   (TCL_READABLE | TCL_WRITABLE));
    if (channel == NULL) {
        if (interp) {
            Tcl_SetResult(interp, "Could not create channel.", TCL_STATIC);
        }
        goto fail;
    }

    tcpPtr->base.channel = channel;
    /*
     * Call into Tcl to set channel default configuration.
     * Do not access tcpPtr beyond this point in case calls into Tcl result
     * in recursive callbacks through the Tcl channel subsystem to us that
     * result in tcpPtr being freed. (Remember our ref count is reversed via
     * IocpCloseProc).
     */
    tcpPtr = NULL;
    if (IocpTcpSetChannelDefaults(channel) == TCL_ERROR) {
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
        IocpChannel *chanPtr = TcpClientToIocpChannel(tcpPtr);
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
 *------------------------------------------------------------------------
 *
 * TcpListenerCloseSockets --
 *
 *    Closes all listening sockets associated with this channel.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    The allocated listeners array is freed after closing sockets.
 *
 *------------------------------------------------------------------------
 */
void TcpListenerCloseSockets(
    TcpListener *tcpPtr         /* Must be locked or otherwise race-free */
    )
{
    if (tcpPtr->listeners) {
        int i;
        for (i = 0; i < tcpPtr->numListeners; ++i) {
            if (tcpPtr->listeners[i].so != INVALID_SOCKET)
                closesocket(tcpPtr->listeners[i].so);
        }
        ckfree(tcpPtr->listeners);
        tcpPtr->listeners = NULL;
        tcpPtr->numListeners = 0;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * TcpListenerInit --
 *
 *    Initializes the TcpListener part of a IocpChannel structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static void TcpListenerInit(IocpChannel *chanPtr)
{
    TcpListener *tcpPtr = IocpChannelToTcpListener(chanPtr);
    tcpPtr->numListeners = 0;
    tcpPtr->listeners = NULL;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpListenerFinit --
 *
 *    Finalizer for a Tcp listener. Cleans up any resources.
 *    Caller has to ensure synchronization either by holding a lock
 *    on the containing IocpChannel or ensuring the structure is not
 *    accessible from any other thread.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    All associated listening sockets are closed.
 *
 *------------------------------------------------------------------------
 */
static void TcpListenerFinit(
    IocpChannel *chanPtr)       /* Must be locked or otherwise made race-free */
{
    TcpListener *tcpPtr = IocpChannelToTcpListener(chanPtr);
    IocpLink *linkPtr;

    /* Flush any accepted sockets that are still pending */
    while ((linkPtr = IocpListPopFront(&chanPtr->inputBuffers)) != NULL) {
        IocpBuffer  *bufPtr = CONTAINING_RECORD(linkPtr, IocpBuffer, link);
        if (bufPtr->context[0].so != INVALID_SOCKET)
            closesocket(bufPtr->context[0].so);
        IocpBufferFree(bufPtr);
    }

    TcpListenerCloseSockets(tcpPtr);
}

/*
 *------------------------------------------------------------------------
 *
 * TcpListenerShutdown --
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
static int TcpListenerShutdown(
    Tcl_Interp   *interp,       /* May be NULL */
    IocpChannel *lockedChanPtr, /* Locked pointer to the base IocpChannel */
    int          flags)         /* Combination of TCL_CLOSE_{READ,WRITE} */
{
    TcpListener *lockedTcpPtr = IocpChannelToTcpListener(lockedChanPtr);
    TcpListenerCloseSockets(lockedTcpPtr);
    return 0;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpListenerAccept --
 *
 *    Processes all accepted connections queued by the completion thread.
 *    New channels are constructed for these connections and the
 *    application callback invoked.
 *
 *    Conforms to the IocpChannel accept interface.
 *
 * Results:
 *    0 on success or a Windows error code.
 *
 * Side effects:
 *    The accept callback script for the listening server socket is
 *    invoked for every new connection.
 *
 *------------------------------------------------------------------------
 */
IocpWinError TcpListenerAccept(
    IocpChannel *lockedChanPtr  /* Locked pointer to the base channel */
    )
{
    TcpListener *lockedTcpPtr = IocpChannelToTcpListener(lockedChanPtr);
    IocpLink *linkPtr;

    if (lockedChanPtr->channel == NULL) {
        /*
         * Channel was closed. Nothing to do really. When caller drops
         * channel pending queues will be cleaned up.
         */
        return 0;
    }

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_LISTENING); /* Else logic awry */

    /* Accepts are queued on the input queue. */
    while ((linkPtr = IocpListPopFront(&lockedChanPtr->inputBuffers)) != NULL) {
        IocpBuffer *bufPtr = CONTAINING_RECORD(linkPtr, IocpBuffer, link);
        SOCKET      connSocket;
        SOCKADDR   *localAddrPtr, *remoteAddrPtr;
        int         localAddrLen, remoteAddrLen;
        int         listenerIndex = bufPtr->context[1].i;
        TcpClient  *dataChanPtr;
        Tcl_Channel channel;
        TcpListeningSocket *listenerPtr;
        IocpWinError        winError;
        IocpInetAddress     localAddr, remoteAddr;

        IOCP_ASSERT(bufPtr->operation == IOCP_BUFFER_OP_ACCEPT);

        /*
         * Although the lockedChanPtr will be valid because of the reference
         * caller is supposed to be holding, because it is unlocked during
         * the accept callback, the listening socket(s) may have been closed.
         */
        /* TBD - do we need to check both listeners AND numListeners? */
        if (lockedTcpPtr->listeners == NULL ||
            listenerIndex >= lockedTcpPtr->numListeners) {
            IocpBufferFree(bufPtr);
            break;
        }

        /* The listener that did the accept */
        listenerPtr = &lockedTcpPtr->listeners[listenerIndex];

        IOCP_ASSERT(listenerPtr->pendingAcceptPosts > 0);
        listenerPtr->pendingAcceptPosts -= 1;

        /* connSocket is the socket for a new connection. */
        connSocket  = bufPtr->context[0].so;
        bufPtr->context[0].so = INVALID_SOCKET;

        /* Retrieve the connection addresses */
        listenerPtr->_GetAcceptExSockaddrs(
            bufPtr->data.bytes,
            0,
            IOCP_ACCEPT_ADDRESS_LEN,
            IOCP_ACCEPT_ADDRESS_LEN,
            &localAddrPtr, &localAddrLen,
            &remoteAddrPtr, &remoteAddrLen
            );

        /*
         * Copy these before freeing bufPtr as localAddrPtr etc. point into
         * bufPtr data buffer. Note the memcpy needed because structures
         * can't just be assigned since although GetAcceptExSockaddres
         * params are SOCKADDR*, they actually are not (can be biffer for ipv6).
         */
        IOCP_ASSERT(sizeof(localAddr) >= localAddrLen);
        IOCP_ASSERT(sizeof(remoteAddr) >= remoteAddrLen);
        memcpy(&localAddr, localAddrPtr, localAddrLen);
        memcpy(&remoteAddr, remoteAddrPtr, remoteAddrLen);

        /* Required so future getsockname and getpeername work */
        setsockopt(connSocket, SOL_SOCKET,
                   SO_UPDATE_ACCEPT_CONTEXT, (char *)&listenerPtr->so,
                   sizeof(SOCKET));

        /* TBD - reuse the buffer for the next accept. May use outputBuffers as queue */
        IocpBufferFree(bufPtr);
        bufPtr = NULL;

        dataChanPtr = (TcpClient *) IocpChannelNew(&tcpClientVtbl);
        if (dataChanPtr == NULL) {
            /* TBD - notify background error ? */
            closesocket(connSocket);
            continue;
        }
        dataChanPtr->so = connSocket;
        dataChanPtr->base.state = IOCP_STATE_OPEN;

        if (CreateIoCompletionPort((HANDLE) connSocket,
                                   iocpModuleState.completion_port,
                                   0, /* Completion key - unused */
                                   0) == NULL) {
            /* TBD - notify background error ? */
            closesocket(connSocket);
            continue;
        }
        winError = IocpChannelPostReads(TcpClientToIocpChannel(dataChanPtr));
        if (winError != ERROR_SUCCESS) {
            /* TBD - notify background error ? */
            closesocket(connSocket);
            continue;
        }

        /* Create a new open channel */
        channel = IocpCreateTclChannel(TcpClientToIocpChannel(dataChanPtr),
                                       IOCP_SOCK_NAME_PREFIX,
                                       (TCL_READABLE | TCL_WRITABLE));
        if (channel == NULL) {
            closesocket(connSocket);
            dataChanPtr->so = INVALID_SOCKET;
            IocpChannelLock(TcpClientToIocpChannel(dataChanPtr)); /* Need for Drop */
            IocpChannelDrop(TcpClientToIocpChannel(dataChanPtr));
            continue;
        }
        /* Note dataChanPtr not locked at this point but no other thread has access */
        dataChanPtr->base.channel = channel;

        if (IocpTcpSetChannelDefaults(channel) != TCL_OK) {
            Tcl_Close(NULL, channel); /* Will free dataChanPtr as well */
            continue;
        }

        /* Post new accepts for listenerIndex */
        winError = TcpListenerPostAccepts(lockedTcpPtr, listenerIndex);
        if (winError != 0) {
            /* TBD - post a background error */
            /* Note: we still do the accept callbacks below */
        }

        /* Invoke the server callbacks */
        if (lockedTcpPtr->acceptProc != NULL) {
            char host[NI_MAXHOST], port[NI_MAXSERV];
            getnameinfo(&remoteAddr.sa, remoteAddrLen, host, sizeof(host),
                        port, sizeof(port), NI_NUMERICHOST|NI_NUMERICSERV);
            /*
             * Need to unlock before calling acceptProc as that can recurse
             * and call us back to close the channel.
             */
            IocpChannelUnlock(lockedChanPtr);
            lockedTcpPtr->acceptProc(lockedTcpPtr->acceptProcData, channel,
                                 host, atoi(port));
            /*
             * Re-lock before returning. This is safe (i.e. lockedTcpPtr would
             * not have been freed) as our caller (event handler) is holding
             * a reference to it from having been placed on the event queue
             * which will only be dropped on return.
             */
            IocpChannelLock(lockedChanPtr);
        }

    }
    return 0;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpListenerPostAccepts --
 *
 *    Posts as many overlapped accept requests on a listening socket as
 *    permitted. Since this call will post accepts to the IOCP which
 *    may be asynchronously handled by the completion thread, caller
 *    should hold a lock on lockedTcpPtr even for new channels.
 *
 * Results:
 *    Returns 0 on success, else a Windows error code. Success is defined
 *    as at least one accept post is queued, either in this call or
 *    already present.
 *
 * Side effects:
 *    Accept buffers are queued to the IOCP and the reference count for
 *    lockedTcpPtr bumped accordingly.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError TcpListenerPostAccepts(
    TcpListener *lockedTcpPtr,        /* Caller must ensure exclusivity */
    int          listenerIndex  /* Index of listening socket */
    )
{
    DWORD       winError;
    TcpListeningSocket *listenerPtr = &lockedTcpPtr->listeners[listenerIndex];

    IOCP_ASSERT(lockedTcpPtr->base.state == IOCP_STATE_LISTENING);

    while (listenerPtr->pendingAcceptPosts < listenerPtr->maxPendingAcceptPosts) {
        IocpBuffer *bufPtr;
        SOCKET      so;
        DWORD       nbytes;

        so = socket(listenerPtr->aiFamily,
                    listenerPtr->aiSocktype, listenerPtr->aiProtocol);
        if (so == INVALID_SOCKET) {
            winError = WSAGetLastError();
            break;
        }
        /* Do not pass on to children */
        SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);

        bufPtr = IocpBufferNew(sizeof(TcpAcceptBuffer),
                               IOCP_BUFFER_OP_ACCEPT, IOCP_BUFFER_F_WINSOCK);
        if (bufPtr == NULL) {
            winError = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        /* The buffer needs to hold context of the listening socket */
        bufPtr->context[0].so = so;
        bufPtr->context[1].i  = listenerIndex;
        bufPtr->chanPtr       = TcpListenerToIocpChannel(lockedTcpPtr);
        lockedTcpPtr->base.numRefs += 1; /* Reversed when bufPtr is unlinked from channel */

        if (listenerPtr->_AcceptEx(
                listenerPtr->so, /* Listening socket */
                so,              /* Socket used for new connection */
                bufPtr->data.bytes, /* Pointer to data area */
                0,                  /* Number of data bytes to read */
                IOCP_ACCEPT_ADDRESS_LEN, /* Size of local address */
                IOCP_ACCEPT_ADDRESS_LEN, /* Size of remote address */
                &nbytes,                  /* Not used */
                &bufPtr->u.wsaOverlap    /* OVERLAP */
                ) == FALSE
            &&
            (winError = WSAGetLastError()) != ERROR_IO_PENDING
            ) {
            lockedTcpPtr->base.numRefs -= 1;
            bufPtr->chanPtr       = NULL;
            IocpBufferFree(bufPtr);
            closesocket(so);
            break;
        }

        listenerPtr->pendingAcceptPosts += 1;
    }

    /* Return error only if no pending accepts */
    return listenerPtr->pendingAcceptPosts == 0 ? winError : 0;
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTcpListen --
 *
 *    Creates a listening socket on the specified address and adds it to
 *    the list of sockets associated with the TcpListener.
 *
 * Results:
 *    0 on success, else a Windows error code.
 *
 * Side effects:
 *    If successful, the tcpPtr->listeners array and numListeners fields
 *    are updated. The socket is associated with the I/O completion port.
 *    NOTE: the call does NOT post AcceptEx requests to the I/O completion port.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError IocpTcpListen(
    TcpListener     *tcpPtr,    /* Caller should ensure exclusive access */
    struct addrinfo *addrPtr,   /* Address to bind to */
    int port,                   /* Original port specified */
    int chosenport              /* Port to use if original port unspecified.
                                 * See comments below */
    )
{
    DWORD        nbytes;
    SOCKET       so;
    IocpWinError winError;
    int          listenerIndex;
    static GUID AcceptExGuid             = WSAID_ACCEPTEX;
    static GUID GetAcceptExSockaddrsGuid = WSAID_GETACCEPTEXSOCKADDRS;

    so = socket(addrPtr->ai_family,
                addrPtr->ai_socktype, addrPtr->ai_protocol);
    if (so == INVALID_SOCKET) {
        return WSAGetLastError();
    }

    listenerIndex = tcpPtr->numListeners;
    /*
     * Retrieve the AcceptEx function pointer. We do not cache
     * because strictly speaking it depends on the socket and
     * address family that map to a protocol driver.
     */
    if (WSAIoctl(so, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &AcceptExGuid, sizeof(GUID),
                 &tcpPtr->listeners[listenerIndex]._AcceptEx,
                 sizeof(tcpPtr->listeners[listenerIndex]._AcceptEx),
                 &nbytes, NULL, NULL) != 0 ||
        tcpPtr->listeners[listenerIndex]._AcceptEx == NULL ||
        WSAIoctl(so, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &GetAcceptExSockaddrsGuid, sizeof(GUID),
                 &tcpPtr->listeners[listenerIndex]._GetAcceptExSockaddrs,
                 sizeof(tcpPtr->listeners[listenerIndex]._GetAcceptExSockaddrs),
                 &nbytes, NULL, NULL) != 0 ||
        tcpPtr->listeners[listenerIndex]._GetAcceptExSockaddrs == NULL
        ) {
        winError = WSAGetLastError();
        closesocket(so);
        return winError;
    }

    /* Do not want children to inherit sockets */
    SetHandleInformation((HANDLE) so, HANDLE_FLAG_INHERIT, 0);

    /* TBD - why needed ?
       TclSockMinimumBuffers((void *)so, TCP_BUFFER_SIZE);
    */

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

    /* Attach to completion port */
    if (CreateIoCompletionPort((HANDLE)so,
                               iocpModuleState.completion_port,
                               0, /* Completion key - unused */
                               0) == NULL) {
        winError = GetLastError();
        closesocket(so);
        return winError;
    }
    /*
     *
     * When binding do not setsockopt(SO_REUSEADDR) because Microsoft
     * allows address reuse.
     *
     * TBD - should max pending be SOMAXCONN or something else?
     */

    if (bind(so, addrPtr->ai_addr, (int) addrPtr->ai_addrlen) == SOCKET_ERROR ||
        listen(so, SOMAXCONN) == SOCKET_ERROR) {

        winError = WSAGetLastError();
        closesocket(so);
        return winError;
    }

    tcpPtr->listeners[listenerIndex].so         = so;
    tcpPtr->listeners[listenerIndex].aiFamily   = addrPtr->ai_family;
    tcpPtr->listeners[listenerIndex].aiSocktype = addrPtr->ai_socktype;
    tcpPtr->listeners[listenerIndex].aiProtocol = addrPtr->ai_protocol;
    tcpPtr->listeners[listenerIndex].pendingAcceptPosts     = 0;
    tcpPtr->listeners[listenerIndex].maxPendingAcceptPosts  = IOCP_TCP_MAX_ACCEPTS;
    tcpPtr->numListeners += 1;
    return 0;
}

/*
 *------------------------------------------------------------------------
 *
 * TcpListenerGetOption --
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
IocpTclCode TcpListenerGetOption(
    IocpChannel *lockedChanPtr, /* Locked on entry, locked on exit */
    Tcl_Interp  *interp,        /* For error reporting. May be NULL */
    int          opt,           /* Index into option table for option of interest */
    Tcl_DString *dsPtr)         /* Where to store the value */
{
    TcpListener *lockedTcpPtr  = IocpChannelToTcpListener(lockedChanPtr);
    IocpInetAddress addr;
    IocpWinError    winError;
    int  dsLen;
    int  listenerIndex;
    int  addrSize;
    char integerSpace[TCL_INTEGER_SPACE];
    int  noRDNS = 0;
#define SUPPRESS_RDNS_VAR "::tcl::unsupported::noReverseDNS"

    if (lockedTcpPtr->numListeners == 0) {
        if (interp)
            Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
        return TCL_ERROR;
    }
    if (interp != NULL && Tcl_GetVar(interp, SUPPRESS_RDNS_VAR, 0) != NULL) {
	noRDNS = NI_NUMERICHOST;
    }

    switch (opt) {
    case IOCP_TCP_OPT_CONNECTING:
        Tcl_DStringAppend(dsPtr, "0", 1);
        return TCL_OK;
    case IOCP_TCP_OPT_ERROR:
        /* As per Tcl winsock, do not report errors in connecting state */
        if (lockedTcpPtr->base.winError != ERROR_SUCCESS) {
            Tcl_Obj *objPtr = Iocp_MapWindowsError(lockedTcpPtr->base.winError,
                                                   NULL, NULL);
            int      nbytes;
            char    *emessage = Tcl_GetStringFromObj(objPtr, &nbytes);
            Tcl_DStringAppend(dsPtr, emessage, nbytes);
            Tcl_DecrRefCount(objPtr);
        }
        return TCL_OK;
    case IOCP_TCP_OPT_PEERNAME:
        if (interp) {
            Tcl_SetResult(interp, "can't get peername: socket is not connected", TCL_STATIC);
        }
        return TCL_ERROR;
    case IOCP_TCP_OPT_SOCKNAME:
        winError = 0;
        dsLen = Tcl_DStringLength(dsPtr);
        for (listenerIndex = 0; listenerIndex < lockedTcpPtr->numListeners; ++listenerIndex) {
            addrSize = sizeof(addr);
            if (getsockname(lockedTcpPtr->listeners[listenerIndex].so,
                            &addr.sa, &addrSize) != 0) {
                winError = WSAGetLastError();
                break;
            }
            winError = IocpTcpListifyAddress(&addr, addrSize, noRDNS, dsPtr);
            if (winError != ERROR_SUCCESS)
                break;
        }
        if (winError != ERROR_SUCCESS) {
            Tcl_DStringTrunc(dsPtr, dsLen); /* Restore original length */
            return Iocp_ReportWindowsError(interp, winError, NULL);
        } else {
            return TCL_OK;
        }
    case IOCP_TCP_OPT_MAXPENDINGREADS:
    case IOCP_TCP_OPT_MAXPENDINGWRITES:
        Tcl_DStringAppend(dsPtr, "0", 1);
        return TCL_OK;
    case IOCP_TCP_OPT_MAXPENDINGACCEPTS:
        sprintf_s(integerSpace, sizeof(integerSpace),
                  "%d", lockedTcpPtr->maxPendingAcceptPosts);
        Tcl_DStringAppend(dsPtr, integerSpace, -1);
        return TCL_OK;
    default:
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Internal error: invalid socket option index %d", opt));
        return TCL_ERROR;
    }
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
Iocp_OpenTcpServer(
    Tcl_Interp *interp,		/* For error reporting - may be NULL. */
    int port,			/* Port number to open. */
    const char *myHost,		/* Name of local host. */
    Tcl_TcpAcceptProc *acceptProc,
				/* Callback for accepting connections from new
				 * clients. */
    ClientData acceptProcData)	/* Data for the callback. */
{
    const char      *errorMsg   = NULL;
    TcpListener     *tcpPtr = NULL;
    Tcl_Channel      channel;
    IocpWinError     winError;
    struct addrinfo *localAddrs = NULL;
    struct addrinfo *addrPtr;
    int              nsockets;
    int              chosenPort = 0;
    int              i;

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

    if (!TclCreateSocketAddress(interp, &localAddrs, myHost, port, 1, &errorMsg)) {
        if (interp != NULL) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                                 "couldn't resolve local addresses: %s", errorMsg));
        }
        goto fail;
    }

    tcpPtr = (TcpListener *) IocpChannelNew(&tcpListenerVtbl);
    if (tcpPtr == NULL) {
        if (interp != NULL) {
            Tcl_SetResult(interp, "couldn't allocate TcpListener", TCL_STATIC);
        }
        goto fail;
    }

    for (nsockets = 0, addrPtr = localAddrs; addrPtr; addrPtr = addrPtr->ai_next) {
        ++nsockets;
    }
    tcpPtr->listeners = ckalloc(nsockets * sizeof(*tcpPtr->listeners));
    tcpPtr->numListeners = 0;
    for (addrPtr = localAddrs; addrPtr; addrPtr = addrPtr->ai_next) {

        winError = IocpTcpListen(tcpPtr, addrPtr, port, chosenPort);
        if (winError != 0)
            continue;
        IOCP_ASSERT(tcpPtr->numListeners > 0);

        /*
         * If original port was not specified, get the port allocated by OS and
         * make that the chosen one for further bindings. This is to ensure that
         * IPv6 and IPv4 bind to same OS-specified port
         */
	if (port == 0 && chosenPort == 0) {
	    IocpInetAddress sockname;
	    socklen_t       namelen = sizeof(sockname);

	    /*
	     * Synchronize port numbers when binding to port 0 of multiple
	     * addresses.
	     */

	    if (getsockname(tcpPtr->listeners[tcpPtr->numListeners-1].so, &sockname.sa, &namelen) == 0) {
		chosenPort = ntohs(sockname.sa4.sin_port);
	    }
	}
    }
    freeaddrinfo(localAddrs);
    localAddrs = NULL;          /* To prevent double frees in case of errors */

    if (tcpPtr->numListeners == 0) {
        /* No addresses could be bound. Report the last error */
#if 0
        /* Do as below for Tcl socket error message test compatibility */
        Iocp_ReportWindowsError(interp, winError, NULL);
#else
        IocpSetInterpPosixErrorFromWin32(interp, winError, gSocketOpenErrorMessage);
#endif
        goto fail;
    }

    tcpPtr->base.state = IOCP_STATE_LISTENING;
    tcpPtr->base.flags |= IOCP_CHAN_F_WATCH_ACCEPT;

    tcpPtr->acceptProc = acceptProc;
    tcpPtr->acceptProcData = acceptProcData;

    /*
     * Now post outstanding accepts on all the sockets
     */
    IocpChannelLock(TcpListenerToIocpChannel(tcpPtr));
    for (i = 0; i < tcpPtr->numListeners; ++i) {
        winError = TcpListenerPostAccepts(tcpPtr, i);
        if (winError != 0) {
            IocpChannelUnlock(TcpListenerToIocpChannel(tcpPtr));
            Iocp_ReportWindowsError(interp, winError, "Could not post accepts.");
            goto fail;
        }
    }

    /* Now finally create the Tcl channel */

    /*
     * NOTE - WILL CALL BACK INTO US THROUGH THREAD ACTION. Thus need to unlock.
     */
    IocpChannelUnlock(TcpListenerToIocpChannel(tcpPtr));
    channel = IocpCreateTclChannel(TcpListenerToIocpChannel(tcpPtr),
                                   IOCP_SOCK_NAME_PREFIX,
                                   0);
    if (channel == NULL)  {
        goto fail;
    }
    /* We know tcpPtr is still valid because we are holding a ref count on it */
    IocpChannelLock(TcpListenerToIocpChannel(tcpPtr));
    tcpPtr->base.channel = channel;
    IocpChannelUnlock(TcpListenerToIocpChannel(tcpPtr));

    /*
     * tcpPtr->numRefs++ since Tcl channel points to this
     * tcpPtr->numRefs-- since this function no longer needs a reference to it.
     * The two cancel and numRefs stays at 1 as allocated. The corresponding
     * unref will be via IocpCloseProc when Tcl closes the channel.
     */

    /* TBD - why do we even have to set -eofchar on a server channel ? Original Tcl code did */
    if (TCL_ERROR == Tcl_SetChannelOption(NULL, channel,
                                                 "-eofchar", "")) {
        /* Do NOT goto fail here. The Tcl_Close will drop tcpPtr via CloseProc */
	Tcl_Close(NULL, channel);
	return NULL;
    }

    return channel;

fail: /* tcpPtr must NOT be locked, interp must contain error message already */
    if (localAddrs)
        freeaddrinfo(localAddrs);
    /* We'll just let any allocated sockets be freed when the tcpPtr is freed */
    if (tcpPtr) {
        IocpChannel *chanPtr = TcpListenerToIocpChannel(tcpPtr);
        IocpChannelLock(chanPtr); /* Because IocpChannelDrop expects that */
        chanPtr->state = IOCP_STATE_CLOSED;
        IocpChannelDrop(chanPtr); /* Will also close allocated sockets etc. */
    }

    return NULL;

}

/*
 *------------------------------------------------------------------------
 *
 * Iocp_SocketObjCmd --
 *
 *    Implements the socket command. See 'socket' documentation as to description
 *    and options.
 *
 * Results:
 *    A standard Tcl result with the channel handle stored in interp result.
 *
 * Side effects:
 *    A new server or client socket is created.
 *
 *------------------------------------------------------------------------
 */
IocpTclCode
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
	    Iocp_Panic("Tcl_SocketObjCmd: bad option index to SocketOptions");
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
#ifdef BUILD_iocp
        /*
         * Hard code to match Tcl socket error. Can't use code below because
         * it uses internal Tcl structures.
         */
        Tcl_SetResult(interp, "wrong # args: should be \"socket ?-myaddr addr? ?-myport myport? ?-async? host port\" or \"socket -server command ?-myaddr addr? port\"", TCL_STATIC);
#else
	Tcl_WrongNumArgs(interp, 1, objv,
		"?-myaddr addr? ?-myport myport? ?-async? host port");
	((Interp *)interp)->flags |= INTERP_ALTERNATE_WRONG_ARGS;
	Tcl_WrongNumArgs(interp, 1, objv,
		"-server command ?-myaddr addr? port");
#endif
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
	IocpAcceptCallback *acceptCallbackPtr;
	IocpSizeT           len;
	char               *copyScript;

        len        = (IocpSizeT) strlen(script)+1;
        copyScript = ckalloc(len);
	memcpy(copyScript, script, len);
        acceptCallbackPtr         = ckalloc(sizeof(*acceptCallbackPtr));
	acceptCallbackPtr->script = copyScript;
	acceptCallbackPtr->interp = interp;

	chan = Iocp_OpenTcpServer(interp, port, host, AcceptCallbackProc,
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

	IocpRegisterAcceptCallbackCleanup(interp, acceptCallbackPtr);

	/*
	 * Register a close callback. This callback will inform the
	 * interpreter (if it still exists) that this channel does not need to
	 * be informed when the interpreter is deleted.
	 */

	Tcl_CreateCloseHandler(chan, IocpUnregisterAcceptCallbackCleanupOnClose,
                               acceptCallbackPtr);

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
