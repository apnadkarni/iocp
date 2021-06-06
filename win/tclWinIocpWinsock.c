/*
 * Common implementation for Winsock based modules
 */

#include "tclWinIocp.h"
#include "tclWinIocpWinsock.h"

/*
 * Options supported by TCP sockets. Note the order MUST match
 * the IocpWinsockOptions enum definition.
 */
const char *iocpWinsockOptionNames[] = {
    "-peername",
    "-sockname",
    "-error",
    "-connecting",
    "-maxpendingreads",
    "-maxpendingwrites",
    "-maxpendingaccepts",
    "-sosndbuf",
    "-sorcvbuf",
    "-keepalive",
    "-nagle",
    NULL
};

/* Just to ensure consistency */
const char *gSocketOpenErrorMessage = "couldn't open socket: ";

static IocpWinError WinsockClientPostDisconnect(WinsockClient *chanPtr);

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientInit --
 *
 *    Initializes the WinsockClient part of a IocpChannel structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
void WinsockClientInit(IocpChannel *chanPtr)
{
    WinsockClient *wsPtr = IocpChannelToWinsockClient(chanPtr);

    wsPtr->so             = INVALID_SOCKET;
    memset(&wsPtr->addresses, 0, sizeof(wsPtr->addresses));
    wsPtr->flags          = 0;

    wsPtr->base.maxPendingReads  = IOCP_WINSOCK_MAX_RECEIVES;
    wsPtr->base.maxPendingWrites = IOCP_WINSOCK_MAX_SENDS;
}


/*
 *------------------------------------------------------------------------
 *
 * WinsockClientFinit --
 *
 *    Finalizer for a Winsock channel. Cleans up any resources for the
 *    common part of the Winsock structure. The addresses field is the
 *    responsibility of the specific Winsock Module.
 *
 *    Caller has to ensure synchronization either by holding a lock
 *    on the containing IocpChannel or ensuring the structure is not
 *    accessible from any other thread.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Socket closed if open.
 *
 *------------------------------------------------------------------------
 */
void WinsockClientFinit(IocpChannel *chanPtr)
{
    WinsockClient *wsPtr = IocpChannelToWinsockClient(chanPtr);

    if (wsPtr->so != INVALID_SOCKET) {
        closesocket(wsPtr->so);
        wsPtr->so = INVALID_SOCKET;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientShutdown --
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
int WinsockClientShutdown(
    Tcl_Interp   *interp,        /* May be NULL */
    IocpChannel *lockedChanPtr, /* Locked pointer to the base IocpChannel */
    int          flags)         /* Combination of TCL_CLOSE_{READ,WRITE} */
{
    WinsockClient *lockedWsPtr = IocpChannelToWinsockClient(lockedChanPtr);

    if (lockedWsPtr->so != INVALID_SOCKET) {
        int wsaStatus = 0;
        switch (flags & (TCL_CLOSE_READ|TCL_CLOSE_WRITE)) {
        case TCL_CLOSE_READ:
            if ((lockedWsPtr->flags & IOCP_WINSOCK_HALF_CLOSABLE) == 0) {
                return EINVAL; /* TBD */
            }
            wsaStatus = shutdown(lockedWsPtr->so, SD_RECEIVE);
            break;
        case TCL_CLOSE_WRITE:
            if ((lockedWsPtr->flags & IOCP_WINSOCK_HALF_CLOSABLE) == 0) {
                return EINVAL; /* TBD */
            }
            wsaStatus = shutdown(lockedWsPtr->so, SD_SEND);
            break;
        case TCL_CLOSE_READ|TCL_CLOSE_WRITE:
            /*
             * Doing just a closesocket seems to result in a TCP RESET
             * instead of graceful close. Do use DisconnectEx if
             * available.
             */
            if (WinsockClientPostDisconnect(lockedWsPtr) != ERROR_SUCCESS) {
                wsaStatus = closesocket(lockedWsPtr->so);
                lockedWsPtr->so = INVALID_SOCKET;
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
 * WinsockClientGetHandle --
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
IocpTclCode WinsockClientGetHandle(
    IocpChannel *lockedChanPtr,
    int direction,
    ClientData *handlePtr)
{
    SOCKET so = IocpChannelToWinsockClient(lockedChanPtr)->so;
    if (so == INVALID_SOCKET)
        return TCL_ERROR;
    *handlePtr = (ClientData) so;
    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientPostDisconnect --
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
static IocpWinError
WinsockClientPostDisconnect(
    WinsockClient *lockedWsPtr     /* Must be locked */
    )
{
    static GUID     DisconnectExGuid = WSAID_DISCONNECTEX;
    LPFN_DISCONNECTEX  fnDisconnectEx;
    DWORD nbytes;

    if (WSAIoctl(lockedWsPtr->so, SIO_GET_EXTENSION_FUNCTION_POINTER,
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
        bufPtr->chanPtr    = WinsockClientToIocpChannel(lockedWsPtr);
        lockedWsPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

        if (fnDisconnectEx(lockedWsPtr->so, &bufPtr->u.wsaOverlap, 0, 0) == FALSE) {
            IocpWinError    winError = WSAGetLastError();
            if (winError != WSA_IO_PENDING) {
                lockedWsPtr->base.numRefs -= 1; /* Reverse above increment */
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
 * WinsockClientPostRead --
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
IocpWinError
WinsockClientPostRead(IocpChannel *lockedChanPtr)
{
    WinsockClient *lockedWsPtr = IocpChannelToWinsockClient(lockedChanPtr);
    IocpBuffer *bufPtr;
    WSABUF      wsaBuf;
    DWORD       flags;
    DWORD       wsaError;

    IOCP_ASSERT(lockedWsPtr->base.state == IOCP_STATE_OPEN);
    bufPtr = IocpBufferNew(IOCP_BUFFER_DEFAULT_SIZE, IOCP_BUFFER_OP_READ,
                           IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->chanPtr    = lockedChanPtr;
    lockedWsPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

    wsaBuf.buf = bufPtr->data.bytes;
    wsaBuf.len = bufPtr->data.capacity;
    flags      = 0;

    IOCP_ASSERT(lockedWsPtr->so != INVALID_SOCKET);
    if (WSARecv(lockedWsPtr->so,
                 &wsaBuf,       /* Buffer array */
                 1,             /* Number of elements in array */
                 NULL,          /* Not used */
                 &flags,
                 &bufPtr->u.wsaOverlap, /* Overlap structure for return status */
                 NULL) != 0        /* Not used */
        && (wsaError = WSAGetLastError()) != WSA_IO_PENDING) {
        /* Not good. */
        lockedWsPtr->base.numRefs -= 1;
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
 * WinsockClientPostWrite --
 *
 *    Allocates a receive buffer, copies passed data to it and posts
 *    it to the socket associated with lockedWsPtr. Implements the behaviour
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
IocpWinError
WinsockClientPostWrite(
    IocpChannel *lockedChanPtr, /* Must be locked on entry */
    const char  *bytes,         /* Pointer to data to write */
    int          nbytes,        /* Number of data bytes to write */
    int         *countPtr)      /* Output - Number of bytes written */
{
    WinsockClient *lockedWsPtr = IocpChannelToWinsockClient(lockedChanPtr);
    IocpBuffer *bufPtr;
    WSABUF      wsaBuf;
    DWORD       wsaError;
    DWORD       written;

    IOCP_ASSERT(lockedWsPtr->base.state == IOCP_STATE_OPEN);

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
    if (WSASend(lockedWsPtr->so,
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
 * WinsockClientAsyncConnected --
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
IocpWinError
WinsockClientAsyncConnected(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    WinsockClient *lockedWsPtr = IocpChannelToWinsockClient(lockedChanPtr);

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_CONNECTED);
    /*
     * As per documentation this is required for complete context for the
     * socket to be set up. Otherwise certain Winsock calls like getpeername
     * getsockname shutdown will not work.
     */
    if (setsockopt(lockedWsPtr->so, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) {
        lockedWsPtr->base.winError = WSAGetLastError();
        closesocket(lockedWsPtr->so);
        lockedWsPtr->so = INVALID_SOCKET;
        return lockedWsPtr->base.winError;
    }
    else {
        return 0;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientAsyncConnectFailed --
 *
 *    Called to handle connection failure on an async attempt.
 *    Simply closes socket and reports failure. If a specific Winsock
 *    module can retry, it should override this function.
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
IocpWinError
WinsockClientAsyncConnectFailed(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    WinsockClient *wsPtr = IocpChannelToWinsockClient(lockedChanPtr);

    IOCP_ASSERT(lockedChanPtr->state == IOCP_STATE_CONNECT_RETRY);
    if (wsPtr->so != INVALID_SOCKET) {
        closesocket(wsPtr->so);
        wsPtr->so = INVALID_SOCKET;
    }
    return wsPtr->base.winError ? wsPtr->base.winError : WSAECONNREFUSED;
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientDisconnected --
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
void
WinsockClientDisconnected(
    IocpChannel *lockedChanPtr) /* Must be locked on entry. */
{
    WinsockClient *wsPtr = IocpChannelToWinsockClient(lockedChanPtr);

    if (wsPtr->so != INVALID_SOCKET) {
        closesocket(wsPtr->so);
        wsPtr->so = INVALID_SOCKET;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientTranslateError --
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
IocpWinError
WinsockClientTranslateError(IocpChannel *chanPtr, IocpBuffer *bufPtr)
{
    WinsockClient *wsPtr = IocpChannelToWinsockClient(chanPtr);
    if (bufPtr->winError != ERROR_SUCCESS) {
        DWORD flags = 0 ;
        DWORD nbytes = 0;
        if (WSAGetOverlappedResult(wsPtr->so, &bufPtr->u.wsaOverlap, &nbytes, FALSE, &flags) == 0) {
            return WSAGetLastError();
        }
    }
    return bufPtr->winError;
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockListifyAddress --
 *
 *    Converts a SOCKADDR to a list consisting of the address
 *    the name and the port number. If the address cannot be mapped to a
 *    name, the numeric address is returned in that field.
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
IocpWinError
WinsockListifyAddress(
    const IocpSockaddr *addrPtr,  /* Address to listify */
    int                 addrSize, /* Size of address structure */
    int                 noRDNS,   /* If true, no name lookup is done */
    Tcl_DString        *dsPtr)    /* Caller-initialized location for output */
{

    if (addrPtr->sa.sa_family == AF_INET || addrPtr->sa.sa_family == AF_INET6) {
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

    if (addrPtr->sa.sa_family == AF_BTH) {
        char buf[40];
        /*
         * Could use WSAAddressToStringA but its deprecated and the WSAAddressToStringW
         * version is inconvenient (wide char)
         */
        StringFromBLUETOOTH_ADDRESS((BLUETOOTH_ADDRESS*) &addrPtr->sabt.btAddr,
                                    buf, _countof(buf));

        Tcl_DStringAppendElement(dsPtr, buf);
        Tcl_DStringAppendElement(dsPtr, buf);
        _snprintf_s(buf, _countof(buf), _TRUNCATE, "%u", addrPtr->sabt.port);
        Tcl_DStringAppendElement(dsPtr, buf);
        return ERROR_SUCCESS;
    }

    return WSAEAFNOSUPPORT;

}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientGetOption --
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
IocpTclCode
WinsockClientGetOption(
    IocpChannel *lockedChanPtr, /* Locked on entry, locked on exit */
    Tcl_Interp  *interp,        /* For error reporting. May be NULL */
    int          opt,           /* Index into option table for option of interest */
    Tcl_DString *dsPtr)         /* Where to store the value */
{
    WinsockClient *lockedWsPtr  = IocpChannelToWinsockClient(lockedChanPtr);
    IocpSockaddr addr;
    IocpWinError    winError;
    int  addrSize;
    char integerSpace[TCL_INTEGER_SPACE];
    int  noRDNS = 0;
#define SUPPRESS_RDNS_VAR "::tcl::unsupported::noReverseDNS"
    DWORD dw;

    switch (opt) {
    case IOCP_WINSOCK_OPT_CONNECTING:
        Tcl_DStringAppend(dsPtr,
                          lockedWsPtr->base.state == IOCP_STATE_CONNECTING ? "1" : "0",
                          1);
        return TCL_OK;
    case IOCP_WINSOCK_OPT_ERROR:
        /* As per Tcl winsock, do not report errors in connecting state */
        if (lockedWsPtr->base.state != IOCP_STATE_CONNECTING &&
            lockedWsPtr->base.state != IOCP_STATE_CONNECT_RETRY &&
            lockedWsPtr->base.winError != ERROR_SUCCESS) {
#if 1
            IocpSetTclErrnoFromWin32(lockedWsPtr->base.winError);
            Tcl_DStringAppend(dsPtr, Tcl_ErrnoMsg(Tcl_GetErrno()), -1);
            lockedWsPtr->base.winError = ERROR_SUCCESS; /* As per socket man page */
#else
            /* This would give more detail but Tcl test suite demands Posix error */
            Tcl_Obj *objPtr = Iocp_MapWindowsError(lockedWsPtr->base.winError,
                                                   NULL, NULL);
            int      nbytes;
            char    *emessage = Tcl_GetStringFromObj(objPtr, &nbytes);
            Tcl_DStringAppend(dsPtr, emessage, nbytes);
            Tcl_DecrRefCount(objPtr);
            lockedWsPtr->base.winError = ERROR_SUCCESS; /* As per socket man page */
#endif
        }
        return TCL_OK;
    case IOCP_WINSOCK_OPT_PEERNAME:
    case IOCP_WINSOCK_OPT_SOCKNAME:
        if (lockedWsPtr->base.state == IOCP_STATE_CONNECTING ||
            lockedWsPtr->base.state == IOCP_STATE_CONNECT_RETRY) {
            /* As per TIP 427, empty string to be returned in these states */
            return TCL_OK;
        }
        if (lockedWsPtr->so == INVALID_SOCKET) {
            if (interp)
                Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
            return TCL_ERROR;
        }
        if (interp != NULL && Tcl_GetVar(interp, SUPPRESS_RDNS_VAR, 0) != NULL) {
            noRDNS = NI_NUMERICHOST;
        }
        addrSize = sizeof(addr);
        if ((opt==IOCP_WINSOCK_OPT_PEERNAME?getpeername:getsockname)(lockedWsPtr->so, &addr.sa, &addrSize) != 0)
            winError = WSAGetLastError();
        else {
            /* Note original length in case we need to revert after modification */
            int dsLen = Tcl_DStringLength(dsPtr);
            winError = WinsockListifyAddress(&addr, addrSize, noRDNS, dsPtr);
            if (winError != ERROR_SUCCESS)
                Tcl_DStringTrunc(dsPtr, dsLen); /* Restore */
        }
        if (winError == 0)
            return TCL_OK;
        else
            return Iocp_ReportWindowsError(interp, winError, NULL);
    case IOCP_WINSOCK_OPT_MAXPENDINGREADS:
    case IOCP_WINSOCK_OPT_MAXPENDINGWRITES:
        sprintf_s(integerSpace, sizeof(integerSpace),
                  "%d",
                  opt == IOCP_WINSOCK_OPT_MAXPENDINGREADS ?
                  lockedChanPtr->maxPendingReads : lockedChanPtr->maxPendingWrites);
        Tcl_DStringAppend(dsPtr, integerSpace, -1);
        return TCL_OK;
    case IOCP_WINSOCK_OPT_MAXPENDINGACCEPTS:
        Tcl_DStringAppend(dsPtr, "0", 1);
        return TCL_OK;
    case IOCP_WINSOCK_OPT_SOSNDBUF:
    case IOCP_WINSOCK_OPT_SORCVBUF:
        if (lockedWsPtr->so == INVALID_SOCKET) {
            if (interp)
                Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
            return TCL_ERROR;
        }
        addrSize = sizeof(dw);
        if (getsockopt(lockedWsPtr->so,
                       SOL_SOCKET,
                       opt == IOCP_WINSOCK_OPT_SOSNDBUF ? SO_SNDBUF : SO_RCVBUF,
                       (char *)&dw,
                       &addrSize)) {
            return Iocp_ReportLastWindowsError(interp, "getsockopt failed: ");
        }
        sprintf_s(integerSpace, sizeof(integerSpace), "%u", dw);
        Tcl_DStringAppend(dsPtr, integerSpace, -1);
        return TCL_OK;
    case IOCP_WINSOCK_OPT_KEEPALIVE:
    case IOCP_WINSOCK_OPT_NAGLE:
        if (lockedWsPtr->so == INVALID_SOCKET) {
            if (interp)
                Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
            return TCL_ERROR;
        }
        else {
            int optlen  = sizeof(BOOL);
            BOOL optval = FALSE;
            getsockopt(lockedWsPtr->so,
                       SOL_SOCKET,
                       opt == IOCP_WINSOCK_OPT_KEEPALIVE ? SO_KEEPALIVE
                                                         : TCP_NODELAY,
                       (char *)&optval,
                       &optlen);
            if (opt == IOCP_WINSOCK_OPT_NAGLE)
                optval = !optval;
            Tcl_DStringAppend(dsPtr, optval ? "1" : "0", 1);
        }
        return TCL_OK;
    default:
        if (interp) {
          Tcl_SetObjResult(
            interp,
            Tcl_ObjPrintf("Internal error: invalid socket option index %d",
                          opt));
        }
        return TCL_ERROR;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * WinsockClientSetOption --
 *
 *    Sets the value of the given option.
 *
 * Results:
 *    Returns TCL_OK on succes and TCL_ERROR on failure.
 *
 * Side effects:
 *    On success the value of the option is stored in *dsPtr.
 *
 *------------------------------------------------------------------------
 */
IocpTclCode WinsockClientSetOption(
    IocpChannel *lockedChanPtr, /* Locked on entry, locked on exit */
    Tcl_Interp  *interp,        /* For error reporting. May be NULL */
    int          opt,           /* Index into option table for option of interest */
    const char  *valuePtr)      /* Option value */
{
    WinsockClient *lockedWsPtr = IocpChannelToWinsockClient(lockedChanPtr);
    int        intValue;
    BOOL       bVal;

    if (lockedWsPtr->so == INVALID_SOCKET) {
        if (interp)
            Tcl_SetResult(interp, "No socket associated with channel.", TCL_STATIC);
        return TCL_ERROR;
    }

    switch (opt) {
    case IOCP_WINSOCK_OPT_MAXPENDINGREADS:
    case IOCP_WINSOCK_OPT_MAXPENDINGWRITES:
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
        if (opt == IOCP_WINSOCK_OPT_MAXPENDINGREADS)
            lockedChanPtr->maxPendingReads = intValue;
        else
            lockedChanPtr->maxPendingWrites = intValue;
        return TCL_OK;
    case IOCP_WINSOCK_OPT_SOSNDBUF:
    case IOCP_WINSOCK_OPT_SORCVBUF:
        if (Tcl_GetInt(interp, valuePtr, &intValue) != TCL_OK) {
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        if (intValue < 0) {
            if (interp)
                Tcl_SetObjResult(
                    interp,
                    Tcl_ObjPrintf("Negative buffer space %d specified.",
                                  intValue));
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        if (setsockopt(lockedWsPtr->so,
                       SOL_SOCKET,
                       opt == IOCP_WINSOCK_OPT_SOSNDBUF ? SO_SNDBUF : SO_RCVBUF,
                       (char *)&intValue,
                       sizeof(intValue))) {
            /* Note retrieve Win32 error before any other call */
            Iocp_ReportLastWindowsError(interp, "setsockopt failed: ");
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        return TCL_OK;

    case IOCP_WINSOCK_OPT_CONNECTING:
    case IOCP_WINSOCK_OPT_ERROR:
    case IOCP_WINSOCK_OPT_PEERNAME:
    case IOCP_WINSOCK_OPT_SOCKNAME:
    case IOCP_WINSOCK_OPT_MAXPENDINGACCEPTS:
        return Tcl_BadChannelOption(interp,
                                    iocpWinsockOptionNames[opt],
                                    "-maxpendingreads -maxpendingwrites"
                                    "-sorcvbuf -sosndbuf");
    case IOCP_WINSOCK_OPT_NAGLE:
    case IOCP_WINSOCK_OPT_KEEPALIVE:
        if (Tcl_GetBoolean(interp, valuePtr, &intValue) != TCL_OK) {
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        if (opt == IOCP_WINSOCK_OPT_KEEPALIVE) {
            bVal     = intValue ? TRUE : FALSE;
            intValue = setsockopt(lockedWsPtr->so,
                                  SOL_SOCKET,
                                  SO_KEEPALIVE,
                                  (const char *)&bVal,
                                  sizeof(BOOL));
        }
        else {
            /* Note truth value reversed NAGLE -> NODELAY */
            bVal     = intValue ? FALSE : TRUE;
            intValue = setsockopt(lockedWsPtr->so,
                                  SOL_SOCKET,
                                  TCP_NODELAY,
                                  (const char *)&bVal,
                                  sizeof(BOOL));

        }
        if (intValue != 0) {
            /* Note retrieve Win32 error before any other call */
            Iocp_ReportLastWindowsError(interp, "setsockopt failed: ");
            Tcl_SetErrno(EINVAL);
            return TCL_ERROR;
        }
        return TCL_OK;
    default:
        if (interp)
            Tcl_SetObjResult(
                interp,
                Tcl_ObjPrintf("Internal error: invalid socket option index %d",
                              opt));
        Tcl_SetErrno(EINVAL);
        return TCL_ERROR;
    }
}
