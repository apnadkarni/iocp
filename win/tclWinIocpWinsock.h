#ifndef TCLIOCPWINSOCK_H
#define TCLIOCPWINSOCK_H

#include "tclWinIocp.h"

/*
 * Winsock related structures shared by Winsock based IOCP modules.
 */

/*
 * Copied from Tcl -
 * This is needed to comply with the strict aliasing rules of GCC, but it also
 * simplifies casting between the different sockaddr types.
 */

typedef union {
    struct sockaddr         sa;
    struct sockaddr_in      sa4;
    struct sockaddr_in6     sa6;
    struct sockaddr_storage sas;
#if IOCP_ENABLE_BLUETOOTH
    SOCKADDR_BTH            sabt;
#endif
} IocpSockaddr;
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

/* TCP client channel state */
typedef struct WinsockClient {
    IocpChannel base;           /* Common IOCP channel structure. Must be
                                 * first because of how structures are
                                 * allocated and freed */
    SOCKET      so;             /* Winsock socket handle */
    /* Union is discriminated based on base.vtblPtr */
    union {
        struct {
            struct addrinfo *remotes; /* List of potential remote addresses */
            struct addrinfo *remote;  /* Remote address in use.
                                       * Points into remoteAddrList */
            struct addrinfo *locals;  /* List of potential local addresses */
            struct addrinfo *local;   /* Local address in use
                                       * Points into localAddrList */
        } inet;                       /* AF_INET or AF_INET6 */
#if IOCP_ENABLE_BLUETOOTH
        struct {
            SOCKADDR_BTH remote;      /* Remote address */
            SOCKADDR_BTH local;       /* Local address */
        } bt;                         /* AF_BTH */
#endif
    } addresses;
    int flags;                        /* Miscellaneous flags */
#define IOCP_WINSOCK_CONNECT_ASYNC 0x1 /* Async connect */
#define IOCP_WINSOCK_HALF_CLOSABLE 0x2 /* socket support unidirectional close */

#define IOCP_WINSOCK_MAX_RECEIVES 3
#define IOCP_WINSOCK_MAX_SENDS    3

} WinsockClient;

IOCP_INLINE IocpChannel *WinsockClientToIocpChannel(WinsockClient *tcpPtr) {
    return (IocpChannel *) tcpPtr;
}
IOCP_INLINE WinsockClient *IocpChannelToWinsockClient(IocpChannel *chanPtr) {
    return (WinsockClient *) chanPtr;
}

/*
 * Winsock socket options. Note order must match order of string option names
 * in the iocpWinsockOptionNames array.
 */
enum IocpWinsockOption {
    IOCP_WINSOCK_OPT_PEERNAME,
    IOCP_WINSOCK_OPT_SOCKNAME,
    IOCP_WINSOCK_OPT_ERROR,
    IOCP_WINSOCK_OPT_CONNECTING,
    IOCP_WINSOCK_OPT_MAXPENDINGREADS,
    IOCP_WINSOCK_OPT_MAXPENDINGWRITES,
    IOCP_WINSOCK_OPT_MAXPENDINGACCEPTS,
    IOCP_WINSOCK_OPT_SOSNDBUF,
    IOCP_WINSOCK_OPT_SORCVBUF,
    IOCP_WINSOCK_OPT_KEEPALIVE,
    IOCP_WINSOCK_OPT_NAGLE,
    IOCP_WINSOCK_OPT_INVALID        /* Must be last */
};
extern const char*iocpWinsockOptionNames[];

extern const char *gSocketOpenErrorMessage;

/*
 * Shared Winsock functions
 */
void WinsockClientInit(IocpChannel *basePtr);
void WinsockClientFinit(IocpChannel *chanPtr);
int  WinsockClientShutdown(Tcl_Interp *, IocpChannel *chanPtr, int flags);
IocpTclCode  WinsockClientGetHandle(IocpChannel *lockedChanPtr,
                                    int direction, ClientData *handlePtr);
IocpWinError WinsockClientPostRead(IocpChannel *);
IocpWinError WinsockClientPostWrite(IocpChannel *, const char *data,
                                    int nbytes, int *countPtr);
IocpWinError WinsockClientAsyncConnected(IocpChannel *lockedChanPtr);
IocpWinError WinsockClientAsyncConnectFailed(IocpChannel *lockedChanPtr);
void         WinsockClientDisconnected(IocpChannel *lockedChanPtr);
IocpWinError WinsockClientTranslateError(IocpChannel *chanPtr,
                                         IocpBuffer *bufPtr);
IocpWinError WinsockListifyAddress(const IocpSockaddr *addr,
                                   int addr_size, int noRDNS,
                                   Tcl_DString *dsPtr);
IocpTclCode  WinsockClientGetOption (IocpChannel *lockedChanPtr,
                                     Tcl_Interp *interp, int optIndex,
                                     Tcl_DString *dsPtr);
IocpTclCode  WinsockClientSetOption (IocpChannel *lockedChanPtr,
                                     Tcl_Interp *interp, int optIndex,
                                     const char *valuePtr);

#if IOCP_ENABLE_BLUETOOTH
/*
 * Bluetooth exports
 */
char *StringFromBLUETOOTH_ADDRESS(
    const BLUETOOTH_ADDRESS *addrPtr,
    char  *bufPtr,
    int    bufSize); /* Should be at least 18 bytes, else truncated */
#endif

#endif /* TCLIOCPWINSOCK_H */
