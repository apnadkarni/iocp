#ifndef _TCLWINIOCP
#define _TCLWINIOCP
/*
 * tclWinIocp.h --
 *
 *	Declarations of Windows-specific IOCP related variables and procedures.
 *
 * Copyright (c) 2019 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */


#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <ws2bth.h>
#include <windows.h>
#include <Bthsdpdef.h>
#include <BluetoothAPIs.h>

#include "tcl.h"
#include <errno.h>

#include "tclhPointer.h"
#include "tclhUuid.h"

#ifdef BUILD_iocp
# undef TCL_STORAGE_CLASS
# define TCL_STORAGE_CLASS DLLEXPORT
#endif

#ifdef _MSC_VER
# define IOCP_INLINE __inline
#else
# define IOCP_INLINE static inline
#endif

#ifndef IOCP_ASSERT
# ifdef IOCP_ENABLE_ASSERT
#  define IOCP_ASSERT(bool_) (void)( (bool_) || (Iocp_Panic("Assertion (%s) failed at line %d in file %s.", #bool_, __LINE__, __FILE__), 0) )
# else
#  define IOCP_ASSERT(bool_) (void) 0
# endif
#endif

/* Typedefs just to make return value semantics obvious */
typedef int   IocpTclCode;      /* TCL_OK etc. */
typedef DWORD IocpWinError;     /* Windows error codes */
typedef int   IocpPosixError;   /* POSIX error codes */
typedef int   IocpSizeT;        /* Signed type to hold Tcl string lengths */

/*
 * We may use either critical sections or SRWLocks. The latter are lighter
 * and faster with multiple readers. The former allow recursive locking
 * and are available on older Windows platforms as well.
 */
#ifdef IOCP_USE_CRITICAL_SECTION
typedef CRITICAL_SECTION IocpLock;
#define IocpLockInit(lockPtr)             InitializeCriticalSection(lockPtr)
#define IocpLockAcquireShared(lockPtr)    EnterCriticalSection(lockPtr)
#define IocpLockAcquireExclusive(lockPtr) EnterCriticalSection(lockPtr)
#define IocpLockReleaseShared(lockPtr)    LeaveCriticalSection(lockPtr)
#define IocpLockReleaseExclusive(lockPtr) LeaveCriticalSection(lockPtr)
#define IocpLockDelete(lockPtr)           DeleteCriticalSection(lockPtr)
IOCP_INLINE BOOL IocpConditionVariableWaitShared(
    PCONDITION_VARIABLE cvPtr,
    PCRITICAL_SECTION lockPtr,
    DWORD timeout) {
    return SleepConditionVariableCS(cvPtr, lockPtr, timeout);
}
IOCP_INLINE BOOL IocpConditionVariableWaitExclusive(
    PCONDITION_VARIABLE cvPtr,
    PCRITICAL_SECTION lockPtr,
    DWORD timeout) {
    return SleepConditionVariableCS(cvPtr, lockPtr, timeout);
}
#else
typedef SRWLOCK IocpLock;
#define IocpLockInit(lockPtr)             InitializeSRWLock(lockPtr)
#define IocpLockAcquireShared(lockPtr)    AcquireSRWLockShared(lockPtr)
#define IocpLockAcquireExclusive(lockPtr) AcquireSRWLockExclusive(lockPtr)
#define IocpLockReleaseShared(lockPtr)    ReleaseSRWLockShared(lockPtr)
#define IocpLockReleaseExclusive(lockPtr) ReleaseSRWLockExclusive(lockPtr)
#define IocpLockDelete(lockPtr)           (void) 0
IOCP_INLINE BOOL IocpConditionVariableWaitShared(
    PCONDITION_VARIABLE cvPtr,
    PSRWLOCK lockPtr,
    DWORD timeout) {
    return SleepConditionVariableSRW(cvPtr, lockPtr, timeout, CONDITION_VARIABLE_LOCKMODE_SHARED);
}
IOCP_INLINE BOOL IocpConditionVariableWaitExclusive(
    PCONDITION_VARIABLE cvPtr,
    PSRWLOCK lockPtr,
    DWORD timeout) {
    return SleepConditionVariableSRW(cvPtr, lockPtr, timeout, 0);
}
#endif

/*
 * Forward declarations for structures
 */
typedef struct IocpList        IocpList;
typedef struct IocpThreadData  IocpThreadData;
typedef struct IocpChannel     IocpChannel;
typedef struct IocpChannelVtbl IocpChannelVtbl;
typedef struct IocpDataBuffer  IocpDataBuffer;
typedef struct IocpBuffer      IocpBuffer;

/*
 * Typedefs used by one-time initialization utilities.
 */
typedef volatile LONG Iocp_DoOnceState;
typedef IocpTclCode Iocp_DoOnceProc(void *);

/*
 * Doubly linked list structures. Locking is all external.
 */
typedef struct IocpLink {
    struct IocpLink *nextPtr;
    struct IocpLink *prevPtr;
} IocpLink;
IOCP_INLINE void IocpLinkInit(IocpLink *linkPtr) {
    linkPtr->nextPtr = NULL;
    linkPtr->prevPtr = NULL;
}

typedef struct IocpList {
    IocpLink *headPtr;
    IocpLink *tailPtr;
} IocpList;

IOCP_INLINE void IocpListInit(IocpList *listPtr) {
    listPtr->headPtr = NULL;
    listPtr->tailPtr = NULL;
}

/*
 * Common data shared across the IOCP implementation. This structure is
 * initialized once per process and referenced from both Tcl threads as well
 * as the IOCP worker threads. Should not be written to after initialization.
 */
typedef struct {
    HANDLE completion_port;   /* The completion port around which life revolves */
    HANDLE completion_thread; /* Handle of thread servicing completions */
    DWORD  tlsIndex;          /* TLS index for thread data */
    int    initialized;       /* Whether initialized */
} IocpModuleState;
extern IocpModuleState iocpModuleState;

/*
 * Callback structure for accept script callback in a listener.
 */
typedef struct IocpAcceptCallback {
    char *script;		/* Script to invoke. */
    Tcl_Interp *interp;		/* Interpreter in which to run it. */
} IocpAcceptCallback;

/*
 * Structure to hold the actual data being passed around. This is always owned
 * by a IocpBuffer and allocated/freed with that owning IocpBuffer.
 */
typedef struct IocpDataBuffer {
    char *bytes;       /* Pointer to storage area */
    int   capacity;    /* Capacity of storage area */
    int   begin;       /* Offset where data begins */
    int   len;         /* Number of bytes of data */
} IocpDataBuffer;
#define IOCP_BUFFER_DEFAULT_SIZE 4096
IOCP_INLINE int IocpDataBufferLength(const IocpDataBuffer *dataBufPtr) {
    return dataBufPtr->len;
}

/* Values used in IocpBuffer.operation */
enum IocpBufferOp {
    IOCP_BUFFER_OP_READ,
    IOCP_BUFFER_OP_WRITE,
    IOCP_BUFFER_OP_CONNECT,
    IOCP_BUFFER_OP_DISCONNECT,
    IOCP_BUFFER_OP_ACCEPT,
};

/*
 * An IocpBuffer is used to queue I/O requests and pass data between
 * the channel, Windows, and the completion thread. Each instance is
 * associated with a specific channel as indicated by its channel field and
 * also a data area that holds the actual I/O bytes.
 *
 * As such, the IocpBuffer may be referenced from one of the following
 * contexts:
 *   * the Tcl thread via the IocpChannel structure, 
 *   * the Windows kernel within the overlapped I/O execution, and
 *   * the completion thread after the I/O completes
 *
 * Key point to note: it is only accessed from one context at a time and
 * there is **no need for explicit locking of the structure**. When a
 * IocpBuffer is allocated, it is only accessible from the allocating
 * thread. Once passed to the Win32 I/O routines, it is only accessed from
 * the kernel. The Tcl and IOCP completion thread do not access and in fact
 * do not even hold a pointer to it. When the I/O call completes, it is
 * retrieved by the completion thread at which point that is the only thread
 * that has access to it. Finally, the completion thread either frees the
 * buffer or enqueues to the channel input queue under the channel lock
 * after which point it is only accessed from the Tcl thread owning the
 * channel under the control of the channel lock. Then the cycle
 * (potentially) repeats. Since only one thread at any point has access to
 * it, there is no need for a lock.
 *
 * For the same reason, there is no reason for a reference count. Only one
 * component holds a reference to it at a time:
 * - the Tcl thread on initial allocation
 * - the Win32 I/O calls (which never free it)
 * - the completion thread
 * - the IocpChannel input queue
 * It is up to the currently owning component to either free the buffer or hand
 * it off to the next component.
 */
typedef struct IocpBuffer {
    union {
        WSAOVERLAPPED wsaOverlap;  /* Used for WinSock API */
        OVERLAPPED    overlap;     /* Used for general Win32 API's */
    } u;
    IocpChannel      *chanPtr;     /* Holds a counted reference to the
                                    * associated IocpChannel */
    IocpDataBuffer    data;        /* Data buffer */
    IocpLink          link;        /* Links buffers in a queue */
    IocpWinError      winError;    /* Error code (0 on success) */
    union {
        int    i;
        HANDLE h;
        SOCKET so;
        void * ptr;
    } context[2];                  /* For buffer users. Not initialized and
                                    * not used by buffer functions */
    enum IocpBufferOp operation;   /* I/O operation */
    int               flags;
#define IOCP_BUFFER_F_WINSOCK 0x1 /* Buffer used for a Winsock operation.
                                   *  (meaning wsaOverlap, not overlap) */
} IocpBuffer;

/* State values for IOCP channels. Used as bit masks. */
enum IocpState {
    IOCP_STATE_INIT           = 0x01, /* Just allocated */
    IOCP_STATE_LISTENING      = 0x02, /* Listening server socket */
    IOCP_STATE_CONNECTING     = 0x04, /* Connect sent, awaiting completion */
    IOCP_STATE_CONNECTED      = 0x08, /* Connect succeeded */
    IOCP_STATE_CONNECT_RETRY  = 0x10, /* Connect failed, retrying */
    IOCP_STATE_OPEN           = 0x20, /* Open for data transfer */
    IOCP_STATE_DISCONNECTING  = 0x40, /* Local end has initiated disconnection */
    IOCP_STATE_DISCONNECTED   = 0x80, /* Remote end has disconnected */
    IOCP_STATE_CONNECT_FAILED = 0x100, /* All connect attempts failed */
    IOCP_STATE_CLOSED         = 0x200, /* Channel closed from both ends */
};
IOCP_INLINE int IocpStateConnectionInProgress(enum IocpState state) {
    return (state & ( IOCP_STATE_CONNECTING | IOCP_STATE_CONNECTED | IOCP_STATE_CONNECT_RETRY)) != 0;
}

/*
 * IocpChannel contains the generic portion of Tcl IOCP channels that is
 * common to all IocpChannel implementations. Its primary purpose is to
 * encapsulate state and functions common to all IOCP channel types including
 *  - state pertaining to Tcl's channel layer.
 *  - functions interfacing to the IOCP threads
 *  - buffer queueing
 * The structures holding state for the concrete IOCP channel types inherit
 * from this structure (by inclusion).
 *
 * General working and lifetime:
 * 
 * - A IocpChannel is allocated through the socket (or similar) call. It
 *   starts with a reference count of 1 corresponding to the reference from
 *   the Tcl channel subsystem. The corresponding reference decrement will
 *   happen when the channel is closed through IocpCloseProc.
 * 
 * - The Tcl channel layer then calls the IocpThreadActionProc function
 *   which attaches it to a thread. The thread id is stored in
 *   IocpChannel.threadId and completion notifications are sent to that
 *   thread.
 * 
 * - For I/O, IocpBuffer's are allocated and posted to the Win32 I/O API.
 *   The IocpBuffer points back to the associated IocpChannel and thus
 *   increments the latter's reference count. The corresponding reference
 *   decrement happens when the IocpBuffer is freed.
 * 
 * - When a read I/O completes, the IOCP completion thread retrieves the
 *   IocpBuffer and places it on the IocpChannel's inputBuffers queue.
 * 
 * - The completion thread then sends a Tcl_Event notification referencing
 *   the IocpChannel to the thread owning the channel. The IocpChannel
 *   reference count is incremented corresponding to this reference.
 * 
 * - The notified Tcl thread receives the event and processes the
 *   completion. The IocpChannel reference count is decremented as it is no
 *   longer referenced from the Tcl event queue.
 * 
 * - The IocpBuffer's on the IocpChannel inputBuffers queue are processed
 *   when the Tcl channel layer calls IocpInputProc to read data.
 * 
 * - Before closing the Tcl channel layer calls the IocpThreadActionProc to
 *   detach the channel.
 * 
 * - On the actual close, the IocpCloseProc function is called which
 *   decrements the IocpChannel reference count corresponding to the initial
 *   allocation reference. NOTE this does not mean the IocpChannel structure
 *   itself is freed since its reference count may not have reached 0 due to
 *   existing references from IocpBuffer's queued for I/O. When those
 *   buffers complete, they will be freed at which point the IocpChannel
 *   reference count will also drop to 0 resulting in it being freed.
 *
 * For blocked operations, e.g. a blocking connect or read, the Tcl thread
 * will wait on the IocpChannel.cv condition variable which will be
 * triggered by the I/O completion thread when the operation completes.
 *
 * Access to the structure is synchronized through the
 * IocpChannelLock/IocpChannelUnlock functions.
 */
typedef struct IocpChannel {
    const IocpChannelVtbl *vtblPtr; /* Dispatch for specific IocpChannel types */
    Tcl_Channel  channel;      /* Tcl channel */
    IocpList     inputBuffers; /* Input buffers whose data is to be
                                * passed up to the Tcl channel layer. */
    CONDITION_VARIABLE cv;     /* Used to wake up blocked Tcl thread waiting
                                * a completion */
    IocpLock           lock;   /* Synchronization */
    struct IocpThreadData *owningTsdPtr;    /* Thread data for owning thread */
    Tcl_ThreadId owningThread; /* Owning thread. */
    Tcl_ThreadId readyQThread; /* If non-0, the thread on whose ready queue the
                                  channel is present. Possibly != owningThread
                                  since channels can change threads. Used
                                  as an optimization to prevent unnecessary
                                  multiple enqueues to the same thread */
    Tcl_ThreadId eventQThread; /* Similar to above except pertains to the
                                  Tcl event queue */
    LONG      numRefs;         /* Reference count */

    enum IocpState state;      /* IOCP_STATE_* */
    IocpWinError  winError;    /* Last error code on I/O */

    int pendingReads;                 /* Number of outstanding posted reads */
    int maxPendingReads;              /* Max number of outstanding posted reads */
#define IOCP_MAX_PENDING_READS_DEFAULT 3
    int pendingWrites;                /* Number of pending posted writes */
    int maxPendingWrites;             /* Max allowed pending posted writes */
#define IOCP_MAX_PENDING_WRITES_DEFAULT 3

    int       flags;

#define IOCP_CHAN_F_NOTIFY_WRITES   0x0004 /* One or more writes notified */
#define IOCP_CHAN_F_WATCH_INPUT     0x0008 /* Notify Tcl on data arrival */
#define IOCP_CHAN_F_WATCH_OUTPUT    0x0010 /* Notify Tcl on output unblocking */
#define IOCP_CHAN_F_READONLY        0x0020 /* Channel input disabled */
#define IOCP_CHAN_F_WRITEONLY       0x0040 /* Channel output disabled */
#define IOCP_CHAN_F_REMOTE_EOF      0x0080 /* Remote end closed connection */
#define IOCP_CHAN_F_NONBLOCKING     0x0100 /* Channel is in non-blocking mode */
#define IOCP_CHAN_F_WATCH_ACCEPT    0x0200 /* Notify on connection accepts */
#define IOCP_CHAN_F_BLOCKED_READ    0x0400 /* Blocked for read completion */
#define IOCP_CHAN_F_BLOCKED_WRITE   0x0800 /* Blocked for write completion */
#define IOCP_CHAN_F_BLOCKED_CONNECT 0x1000 /* Blocked for connect completion */
#define IOCP_CHAN_F_BLOCKED_MASK \
    (IOCP_CHAN_F_BLOCKED_READ | IOCP_CHAN_F_BLOCKED_WRITE | IOCP_CHAN_F_BLOCKED_CONNECT)
} IocpChannel;
IOCP_INLINE void IocpChannelLock(IocpChannel *chanPtr) {
    IocpLockAcquireExclusive(&chanPtr->lock);
}
IOCP_INLINE void IocpChannelUnlock(IocpChannel *lockedChanPtr) {
    IocpLockReleaseExclusive(&lockedChanPtr->lock);
}
IOCP_INLINE void IocpChannelCVWait(IocpChannel *lockedChanPtr) {
    IocpConditionVariableWaitExclusive(&lockedChanPtr->cv, &lockedChanPtr->lock, INFINITE);
}

/*
 * IocpChannelVtbl serves as a poor man's virtual table dispatcher.
 * Concrete Iocp channel types define their own tables to handle type-specific
 * functionality which is called from common IocpChannel code.
 */
typedef struct IocpChannelVtbl {
    /*
     * initialize() is called to initialize the type-specific parts of the
     * IocpChannel. On entry, the base IocpChannel will be initialized but the
     * structure will not be locked as no other thread will hold references.
     */
    void (*initialize)(         /* May be NULL */
        IocpChannel *chanPtr    /* Unlocked on entry */
        );
    /*
     * finalizer() is called when the IocpChannel is about to be
     * deallocated. It should do any final type-specific cleanup required.
     * On entry, the structure will be unlocked and no other references to the
     * structure will exist and hence no thread synchronization is required.
     * After the function returns, the memory will be deallocated.
     *
     * Note in particular that if the channel type makes use of the IocpBuffer
     * context area in the inputBuffers or outputBuffers queues of the
     * the channel that store resource handles, memory etc. the finalizer()
     * should clean up those queues since the default queue cleanup will
     * ignore the context areas of IocpBuffers.
     */
    void (*finalize)(           /* May be NULL */
        IocpChannel *chanPtr);  /* Unlocked on entry */

    /*
     * shutdown() is called to close the underlying OS handle. Should return
     * a POSIX error code as required by the CloseProc definition in the
     * Tcl Channel subsystem.
     */
    int (*shutdown)(                /* Must not be NULL */
        Tcl_Interp  *interp,        /* May be NULL */
        IocpChannel *lockedChanPtr, /* Locked on entry. Must be locked on
                                     * return. */
        int          directions);   /* Combination of TCL_CLOSE_{READ,WRITE} */

    /*
     * accept() is called when an incoming connection is received on a listener.
     */
    IocpWinError (*accept)(          /* May be NULL */
        IocpChannel *lockedChanPtr); /* Locked on entry. Must be locked on
                                      * return. */

    /*
     * blockingconnect() is called from the IOCP layer to synchronously connect
     * to the remote end for connection based channels. It should return 0
     * on a successful connect or a Windows error code on failure.
     */
    IocpWinError (*blockingconnect)( /* May be NULL */
        IocpChannel *lockedChanPtr);     /* Locked on entry. Must be locked on
                                          * return. */

    /*
     * connected() is called when a async connection request succeeds.
     * Driver may take any action required and should return 0 on success
     * or a Windows error code. The IOCP channel base will transition
     * to an OPEN state or to a DISCONNECTED state accordingly.
     */
    IocpWinError (*connected)(       /* May be NULL */
        IocpChannel *lockedChanPtr); /* Locked on entry. Must be locked on
                                      * return. */

    /*
     * connectfailed() is called when a async connection fails to go through.
     * It is up to the driver to retry or close the channel. Should return
     * 0 if another connect was initiated or an Windows error code.
     */
    IocpWinError (*connectfailed)( /* May be NULL */
        IocpChannel *lockedChanPtr);   /* Locked on entry. Must be locked on
                                        * return. */
    /*
     * disconnected() is called from the completion thread when a
     * when a disconnection request is completed. It may take any
     * appropriate action such as closing sockets.
     */
    void (*disconnected)(       /* May be NULL */
        IocpChannel *lockedChanPtr); /* Locked on entry. Must be locked on
                                      * return. */

    /*
     * postread() is called to post an I/O call to read more data.
     * Should return 0 on success and a Windows error code on error.
     */
    DWORD (*postread)( /* May be NULL if channel is created without TCL_READABLE */
        IocpChannel *lockedChanPtr); /* Locked on entry, locked on return */

    /*
     * postwrite() is called to write data to the device. If any data is written,
     * the function should return 0 and store the count of bytes written in
     * *countPtr. If no data can be written because the device would block,
     * the function should return 0 and store 0 in *countPtr. On error,
     * the function return a Windows error code. countPtr is ignored.
     */
    IocpWinError (*postwrite)( /* May be NULL if channel is created without TCL_WRITABLE */
        IocpChannel *lockedChanPtr, /* Locked on entry, locked on return */
        const char  *bytesPtr,      /* Pointer to data buffer */
        int          nbytes,        /* Number of bytes to write */
        int         *countPtr);     /* Where to store number written */

    /*
     * gethandle() retrieves the operating system handle associated with the
     * channel. On success, the function should return TCL_OK and store the
     * handle in *handlePtr. On error, it should return TCL_ERROR.
     */
    IocpTclCode (*gethandle)(
        IocpChannel *lockedChanPtr, /* Locked on entry, locked on return */
        int direction,              /* TCL_READABLE or TCL_WRITABLE */
        ClientData *handlePtr);     /* Where to store the handle */

    /*
     * getoption() retrieves the value of a specified option. On success,
     * should return TCL_OK with value stored in *dsPtr. On error, return
     * TCL_ERROR with error message in interp if not NULL.
     */
    IocpTclCode (*getoption)(       /* May be NULL */
        IocpChannel *lockedChanPtr, /* Locked on entry, locked on return */
        Tcl_Interp  *interp,        /* For error reporting - can be NULL */
        int          optIndex,       /* Index into optionNames[] */
        Tcl_DString *dsPtr);        /* Where to store the computed
                                     * value; initialized by caller. */

    /*
     * setoption() sets the value of the specified option. On success, should
     * return TCL_OK. On error, TCL_ERROR with error message in interp
     * if not NULL.
     */
    IocpTclCode (*setoption)(
        IocpChannel *lockedChanPtr, /* Locked on entry, locked on return */
        Tcl_Interp  *interp,        /* For error reporting - can be NULL */
        int          optIndex,      /* Index into optionNames[] */
        const char*  value);        /* Option value to set */

    /*
     * translateerror() permits a specific channel type to replace a generic
     * Win32 error with a device specific error code. For example, for Winsock
     * errors returned by WSAGetLastError are preferred to GetLastError.
     * The function should return a device-specific error if possible or
     * the existing value in bufPtr->winError.
     */
    IocpWinError (*translateerror)( /* May be NULL */
        IocpChannel *lockedChanPtr, /* Locked on entry and return */
        IocpBuffer *bufPtr);  /* The IocpBuffer containing an OVERLAP
                               * structure as returned on a IOCP completion */

    /* START of DATA fields */

    const char **optionNames;  /* Array of option names terminated with
                                *  NULL. May be NULL if no options */

    int allocationSize;   /* Size of IocpChannel subclass structure . Used to
                           * allocate memory */

} IocpChannelVtbl;

/* Used to notify the thread owning a channel through Tcl event loop */
typedef struct IocpTclEvent {
    Tcl_Event    event;         /* Must be first field */
    IocpChannel *chanPtr;       /* Channel associated with this event */
} IocpTclEvent;

/*
 * Tcl channel function dispatch structure.
 */
extern Tcl_ChannelType IocpChannelDispatch;

/*
 * Statistics for sanity checking etc.
 */
typedef struct IocpStats {
    volatile long IocpChannelAllocs;
    volatile long IocpChannelFrees;
    volatile long IocpBufferAllocs;
    volatile long IocpBufferFrees;
    volatile long IocpDataBufferAllocs;
    volatile long IocpDataBufferFrees;
} IocpStats;
extern IocpStats iocpStats;
/* Wrapper in case we switch to 64bit counters in the future */
#define IOCP_STATS_GET(field_) Tcl_NewLongObj(iocpStats.field_)

#ifdef IOCP_DEBUG
#define IOCP_STATS_INCR(field_) InterlockedIncrement(&iocpStats.field_)
#else
#define IOCP_STATS_INCR(field_) (void) 0
#endif

#ifdef BUILD_iocp

/*
 * We need to access platform-dependent internal stubs. For
 * example, the Tcl channel system relies on specific values to be used
 * for EAGAIN, EWOULDBLOCK etc. These are actually compiler-dependent.
 * so the only way to make sure we are using a consistent Win32->Posix
 * mapping is to use the internal Tcl mapping function.
 */
struct IocpTcl85IntPlatStubs {
    int   magic;
    void *hooks;
    void (*tclWinConvertError) (DWORD errCode); /* 0 */
    int (*fn2[29])(); /* Totally 30 fns, (index 0-29) */
};
extern struct IocpTcl85IntPlatStubs *tclIntPlatStubsPtr;
#define IOCP_TCL85_INTERNAL_PLATFORM_STUB(fn_) (((struct IocpTcl85IntPlatStubs *)tclIntPlatStubsPtr)->fn_)

#endif /* BUILD_iocp */

/*
 * Prototypes for IOCP internal functions.
 */
IocpTclCode Iocp_DoOnce(Iocp_DoOnceState *stateP, Iocp_DoOnceProc *once_fn, ClientData clientdata);


/* List utilities */
void IocpListAppend(IocpList *listPtr, IocpLink *linkPtr);
void IocpListPrepend(IocpList *listPtr, IocpLink *linkPtr);
void IocpListRemove(IocpList *listPtr, IocpLink *linkPtr);
IocpLink *IocpListPopFront(IocpList *listPtr);
IocpList IocpListPopAll(IocpList *listPtr);

/* Error utilities */
Tcl_Obj *Iocp_MapWindowsError(DWORD error, HANDLE moduleHandle, const char *msgPtr);
IocpTclCode Iocp_ReportWindowsError(Tcl_Interp *interp, DWORD winerr, const char *msgPtr);
IocpTclCode Iocp_ReportLastWindowsError(Tcl_Interp *interp, const char *msgPtr);
void IocpSetTclErrnoFromWin32(IocpWinError winError);
void IocpSetInterpPosixErrorFromWin32(Tcl_Interp *interp, IocpWinError winError, const char *prefix);
void __cdecl Iocp_Panic(const char *formatStr, ...);
void __cdecl IocpDebuggerOut(const char *formatStr, ...);
#ifdef IOCP_ENABLE_TRACE
void IocpTraceInit(void);
void __cdecl IocpTrace(const char *formatStr, ...);
void IocpTraceString(const char *s);
# define IOCP_TRACE(params_) do { IocpTrace params_; } while (0)
#else
# define IOCP_TRACE(params_) (void) 0
#endif

/* Buffer utilities */
int IocpDataBufferMoveOut(IocpDataBuffer *bufPtr, char *outPtr, int len);
IOCP_INLINE void IocpDataBufferCopyIn(IocpDataBuffer *bufPtr, const char *inPtr, int len) {
    IOCP_ASSERT(bufPtr->capacity >= len);
    memcpy(bufPtr->bytes, inPtr, len);
    bufPtr->len = len;
}
IocpBuffer *IocpBufferNew(int capacity, enum IocpBufferOp, int);
void IocpBufferFree(IocpBuffer *bufPtr);
IOCP_INLINE int IocpBufferLength(const IocpBuffer *bufPtr) {
    return IocpDataBufferLength(&bufPtr->data);
}
IOCP_INLINE int IocpBufferMoveOut(IocpBuffer *bufPtr, char *outPtr, int len) {
    return IocpDataBufferMoveOut(&bufPtr->data, outPtr, len);
}
IOCP_INLINE void IocpBufferCopyIn(IocpBuffer *bufPtr, const char *inPtr, int len) {
    IocpDataBufferCopyIn(&bufPtr->data, inPtr, len);
}

/* IOCP wrappers */
IOCP_INLINE HANDLE IocpAttachDefaultPort(HANDLE h) {
    return CreateIoCompletionPort(h,
                                  iocpModuleState.completion_port,
                                  0, /* Completion key - unused */
                                  0);
}

/* Callback utilities */
void IocpRegisterAcceptCallbackCleanup(Tcl_Interp *, IocpAcceptCallback *);
void IocpUnregisterAcceptCallbackCleanup(Tcl_Interp *, IocpAcceptCallback *);
void IocpUnregisterAcceptCallbackCleanupOnClose(ClientData callbackData);

/* Generic channel functions */
Tcl_Channel  IocpCreateTclChannel(IocpChannel*, const char*, int);
Tcl_Channel  IocpMakeTclChannel(Tcl_Interp *,IocpChannel* lockedChanPtr, const char*, int);
IocpChannel *IocpChannelNew(const IocpChannelVtbl *vtblPtr);
void         IocpChannelAwaitCompletion(IocpChannel *lockedChanPtr, int flags);
int          IocpChannelWakeAfterCompletion(IocpChannel *lockedChanPtr, int blockMask);
void         IocpChannelDrop(IocpChannel *lockedChanPtr);
DWORD        IocpChannelPostReads(IocpChannel *lockedChanPtr);
void         IocpChannelNudgeThread(IocpChannel *lockedChanPtr, int blockMask, int force);

IocpTclCode IocpSetChannelDefaults(Tcl_Channel channel);

/* Completion thread */
DWORD WINAPI IocpCompletionThread (LPVOID lpParam);

#ifdef TBD
/* Currently not included as too much bloat */
const char *BT_MapiCompanyIdToName(
    unsigned int companyId
    );
#endif

/* If building as an extension, polyfill internal Tcl routines. */
#ifdef BUILD_iocp
#define TclGetString Tcl_GetString
int TclCreateSocketAddress(Tcl_Interp *interp,
                           struct addrinfo **addrlist,
                           const char *host,
                           int port,
                           int willBind,
                           const char **errorMsgPtr);
int TclSockGetPort(Tcl_Interp *interp,
                   const char *string,
                   const char *proto,
                   int *portPtr);

void AcceptCallbackProc(ClientData callbackData, Tcl_Channel chan,
                        char *address, int port);
#endif

/* Module initializations */
IocpTclCode Tcp_ModuleInitialize(Tcl_Interp *interp);
IocpTclCode BT_ModuleInitialize(Tcl_Interp *interp);

/* Script level commands */
Tcl_ObjCmdProc	Iocp_StatsObjCmd;
Tcl_ObjCmdProc	Iocp_TraceOutputObjCmd;
Tcl_ObjCmdProc	Iocp_TraceConfigureObjCmd;

/*
 * Prototypes for IOCP exported functions.
 */
EXTERN int Iocp_Init(Tcl_Interp *interp);

#endif /* _TCLWINIOCP */
