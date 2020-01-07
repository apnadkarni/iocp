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

#ifndef _TCLWINIOCP
#define _TCLWINIOCP

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "tcl.h"

#ifdef BUILD_iocp
# undef TCL_STORAGE_CLASS
# define TCL_STORAGE_CLASS DLLEXPORT
#endif

#ifdef _MSC_VER
# define IOCP_INLINE __inline
#else
# define IOCP_INLINE static inline
#endif

#define IOCP_ASSERT(cond_) (void) 0 /* TBD */

/* Typedef just to make it obvious a function is returning a TCL_xxx return code */
typedef int IocpResultCode;

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
typedef struct IocpTsd         IocpTsd;
typedef struct IocpChannel     IocpChannel;
typedef struct IocpChannelVtbl IocpChannelVtbl;
typedef struct IocpDataBuffer  IocpDataBuffer;
typedef struct IocpBuffer      IocpBuffer;

/*
 * Typedefs used by one-time initialization utilities.
 */
typedef volatile LONG Iocp_DoOnceState;
typedef IocpResultCode Iocp_DoOnceProc(void *);

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
 * IocpTsd holds per-thread state. This is not directly stored via
 * Tcl_GetThreadData because it may still need to be accessible from the IOCP
 * completion thread after the Tcl thread has ended and thread-local data
 * deallocated. Instead the structure is dynamically allocated and only
 * a pointer to it stored in thread-local storage.
 *
 * Storage deallocation is controlled through reference counting. References may
 * be held by the pointer from Tcl_GetThreadData and any IocpChannel structures
 * corresponding to channels that are owned by that thread.
 *
 * Locking:
 *
 * Multi-thread synchronization: access to fields of the structure is controlled
 * through a single *global* lock accessed through IocpTsdLock/IocpTsdUnlock.
 * A single global lock is preferred to a per-IocpTsd lock because it greatly
 * simplifies the code without a cost in contention (if any). If a per-IocpTsd
 * lock were used, it would require the following steps when enqueuing a
 * IocpChannel on the IocpTsd readyChannels field:
 *   - obtain the IocpChannel lock
 *   - read its IocpTsd pointer to locate its owning IocpTsd
 *   - unlock the IocpChannel lock (required for lock hierarchy reasons)
 *   - lock the IocpTsd
 *   - relock the IocpChannel (again, following lock hierarchy)
 *   - check its state was not changed while it was unlocked
 *   - append to IocpTsd.readyChannels
 *   - unlock all locks
 * A global lock simplfies this to
 *   - lock the (global) IocpTsd lock
 *   - lock the IocpChannel (following the lock hierarchy)
 *   - append to IocpTsd.readyChannels
 *   - unlock all
 * The cost of contention from a global lock is mitigated by the fact that
 * it is held only for a minimal time and involves fewer lock/unlock operations.
 *
 * Primary functions:
 *   IocpTsdNew           - allocates and initializes
 *   IocpTsdUnlinkThread  - Disassociates a thread from the TSD
 *   IocpTsdAddChannel    - Add a IocpChannel on the ready channel queue
 *   IocpTsdUnlinkChannel - Removes a IocpChannel from the ready channel queue
 *   IocpTsdLock          - Locks a IocpTsd for write access
 *   IocpTsdUnlock        - Releases a lock on a IocpTsd
 */
typedef struct IocpTsd {
    IocpList     readyChannels; /* List of channels that have some event to
                                 * be reported back to the Tcl channel system.
                                 * The completion thread will enqueue the
                                 * channel while the Tcl channel driver call
                                 * will dequeue elements. The readyLink field in
                                 * IocpChannel is used for linking.
                                 */
    Tcl_ThreadId threadId;      /* Thread id of owning thread */
    long         numRefs;       /* Reference count */
} IocpTsd;
typedef IocpTsd *IocpTsdPtr;
#define IOCP_TSD_INIT(keyPtr)                                   \
    (IocpTsdPtr *)Tcl_GetThreadData((keyPtr), sizeof(IocpTsdPtr))


/*
 * Common data shared across the IOCP implementation. This structure is
 * initialized once per process and referenced from both Tcl threads as well
 * as the IOCP worker threads.
 */
typedef struct IocpModuleState {
    HANDLE completion_port;   /* The completion port around which life revolves */
    HANDLE completion_thread; /* Handle of thread servicing completions */
    IocpLock tsd_lock;        /* Global lock for TSD's - see IocpTsd comments */
    int    initialized;       /* Whether initialized */
} IocpSubSystem;
extern IocpSubSystem iocpModuleState;

IOCP_INLINE void IocpTsdLock(void) {
    IocpLockAcquireExclusive(&iocpModuleState.tsd_lock);
}
IOCP_INLINE void IocpTsdUnlock(void) {
    IocpLockReleaseExclusive(&iocpModuleState.tsd_lock);
}

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

/* Values used in IocpBuffer.operation */
enum IocpBufferOp {
    IOCP_BUFFER_OP_NONE,
    IOCP_BUFFER_OP_READ,       /* Input operation */
    IOCP_BUFFER_OP_WRITE,      /* Output operation */
};

/*
 * An IocpBuffer is used to queue I/O requests and pass data between
 * the channel, Windows, and the completion thread. Each instance is
 * associated with a specific channel as indicated by its channel field and
 * also a data area that holds the actual I/O bytes.
 *
 * As such, the IocpBuffer may be referenced from the Tcl thread via
 * the IocpChannel structure, the Windows kernel within the overlapped I/O
 * execution, and the completion thread but only from one of these at
 * any instant.
 *
 * During I/O, it is passed to the Windows function (WSASend etc.) as
 * the OVERLAP parameter. On completion of the I/O, it is recovered by the
 * I/O completion thread.
 *
 * There is no need for explicit locking of the structure. When a IocpBuffer is
 * allocated, it is only accessible from the allocating thread. Once passed to
 * the Win32 I/O routines, it is only accessed from the kernel. The Tcl and IOCP
 * completion thread do not access and in fact do not even hold a pointer to it
 * from any other structure. When the I/O call completes, it is retrieved by the
 * completion thread at which point that is the only thread that has access to
 * it. Finally, the completion thread frees the buffer or enqueues to the
 * channel input queue under the channel lock after which point it is only
 * accessed from the Tcl thread owning the channel under the control of the
 * channel lock. Then the cycle (potentially) repeats. Since only one thread at
 * any point has access to it, there is no need for a lock.
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
        WSAOVERLAPPED wsaOverlap; /* Used for WinSock API */
        OVERLAPPED    overlap;     /* Used for general Win32 API's */
    } u;
    IocpChannel      *channelPtr;  /* Holds a counted reference to the
                                    * associated IocpChannel */
    IocpDataBuffer    data;        /* Data buffer */
    DWORD             winError;    /* Error code (0 on success) */
    enum IocpBufferOp operation;   /* I/O operation */
    int               flags;
    #define IOCP_BUFFER_F_WINSOCK 0x1 /* Buffer used for a Winsock operation.
                                      *  (meaning wsaOverlap, not overlap) */
} IocpBuffer;

/*
 * IocpChannel contains the generic portion of Tcl IOCP channels that is
 * common to all IocpChannel implementations. Its primary purpose is to
 * encapsulate state and functions that is common to all IOCP channel types
 * including
 *  - state pertaining to Tcl's channel layer.
 *  - functions interfacing to the IOCP threads
 *  - buffer queueing
 * The structures holding state for the concrete IOCP channel types inherit
 * from this structure (by inclusion).
 *
 * General working and lifetime:
 * - A IocpChannel is allocated through the iocp::socket (or similar) call.
 * It starts with a reference count of 1 corresponding to the reference from
 * the Tcl channel subsystem. The corresponding reference decrement will happen
 * when the channel is closed through IocpCloseProc.
 * - The Tcl channel layer then calls the IocpThreadActionProc function
 * which attaches it to a thread. The IocpChannel is linked to the thread's
 * IocpTsd incrementing the latter's reference count. Note the IocpTsd itself
 * does not reference the IocpChannel whose reference count therefore does not
 * change. This also ensures no circular references for reference counting
 * purposes.
 * - For I/O, IocpBuffer's are allocated and posted to the Win32 I/O API.
 * The IocpBuffer points back to the associated IocpChannel and thus increments
 * the latter's reference count. The corresponding reference decrement happens
 * when the IocpBuffer is freed.
 * - When a read I/O completes, the IOCP completion thread retrieves the
 * IocpBuffer and places it on the IocpChannel's inputBuffers queue. This
 * additional reference also increments the IocpChannel reference count.
 * - The completion thread then places the IocpChannel on the corresponding
 * IocpTsd's readyChannels queue if not already there. This linkage results in
 * both the IocpChannel as well as the IocpTsd reference counts being
 * incremented. The owning Tcl thread is notified.
 * - The notified Tcl thread removes the IocpChannel from the IocpTsd
 * readyChannels queue, decrementing both reference counts.
 * - The IocpBuffer's on the IocpChannel inputBuffers queue are processed
 * when the Tcl channel layer calls IocpInputProc to read data. The buffers
 * may then be reused or freed in which case the IocpChannel reference count is
 * decremented.
 * - Before closing the Tcl channel layer calls the IocpThreadActionProc to
 * detach the channel. At that point the IocpChannel tsdPtr is set to NULL and
 * the IocpTsd's reference count decremented accordingly.
 * - On the actual close, the IocpCloseProc function is called which decrements
 * the IocpChannel reference count corresponding to the initial allocation
 * reference. NOTE this does not mean the IocpChannel structure itself is freed
 * since its reference count may not have reached 0 due to existing references
 * from IocpBuffer's queued for I/O. When those buffers complete, they will be
 * freed at which point the IocpChannel reference count will also drop to 0
 * resulting in it being freed.
 *
 * For blocked operations, e.g. a blocking connect or read, the Tcl thread will
 * wait on the cv condition variable which will be triggered by the I/O
 * completion thread when the operation completes.
 *
 * Access to the structure is synchronized through the
 * IocpChannelLock/IocpChannelUnlock functions.
 */
typedef struct IocpChannel {
    const IocpChannelVtbl *vtblPtr; /* Dispatch for specific IocpChannel types */
    Tcl_Channel channel;            /* Tcl channel. TBD - needed ? */
    IocpList  inputBuffers; /* Input buffers whose data is to be
                             * passed up to the Tcl channel layer. */
    IocpLink  readyLink;    /* Links entries on a IocpTsd readyChannels list */
    IocpTsd  *tsdPtr;       /* Pointer to owning thread data. Needed so we know
                             * which thread to notify of I/O completions. */
    IocpLock  lock;         /* Synchronization */
    CONDITION_VARIABLE cv;  /* Used to wake up blocked Tcl thread waiting on an I/O
                             * completion */
    LONG      numRefs;      /* Reference count */
    int       flags;
    int outstanding_reads;  /* Number of outstanding posted reads */
    int outstanding_writes; /* Number of outstanding posted writes */
#define IOCP_CHAN_F_BLOCKED_FOR_IO 0x1 /* Set to indicate Tcl thread blocked
                                        * pending an I/O completion TBD - used? */

} IocpChannel;
IOCP_INLINE void IocpChannelLock(IocpChannel *chanPtr) {
    IocpLockAcquireExclusive(&chanPtr->lock);
}
IOCP_INLINE void IocpChannelUnlock(IocpChannel *chanPtr) {
    IocpLockReleaseExclusive(&chanPtr->lock);
}

/*
 * IocpChannelVtbl serves as a poor man's C++ virtual table dispatcher.
 * Concrete Iocp channel types define their own tables to handle type-specific
 * functionality which is called from common IocpChannel code.
 */
typedef struct IocpChannelVtbl {
    /*
     * initialize() is called to initialize the type-specific parts of the
     * IocpChannel. On entry, the base IocpChannel will be initialized but the
     * structure will not be locked as no other thread will hold references.
     */
    void (*initialize)(
        Tcl_Interp *interp,  /* May be NULL */
        IocpChannel *chanPtr /* Unlocked on entry */
        );        /* May be NULL */
    /*
     * finalizer() is called when the IocpChannel is about to be
     * deallocated. It should do any final type-specific cleanup required.
     * On entry, the structure will be unlocked and no other references to the
     * structure will exist and hence no thread synchronization is required.
     * After the function returns, the memory will be deallocated.
     */
    void (*finalize)(            /* May be NULL */
        Tcl_Interp *interp,    /* May be NULL */
        IocpChannel *chanPtr); /* Unlocked on entry */

    int   allocationSize; /* Size of structure. Used by IocpChannelNew to
                           * allocate memory */

} IocpChannelVtbl;

/*
 * Tcl channel function dispatch structure.
 */
extern Tcl_ChannelType IocpChannelDispatch;

/*
 * Prototypes for IOCP internal functions.
 */


/* List utilities */
void IocpListAppend(IocpList *listPtr, IocpLink *linkPtr);
void IocpListPrepend(IocpList *listPtr, IocpLink *linkPtr);
void IocpLinkDetach(IocpList *listPtr, IocpLink *linkPtr);

/* Error utilities */
Tcl_Obj *Iocp_MapWindowsError(DWORD error, HANDLE moduleHandle);
IocpResultCode Iocp_ReportWindowsError(Tcl_Interp *interp, DWORD winerr);
IocpResultCode Iocp_ReportLastWindowsError(Tcl_Interp *interp);

/* Buffer utilities */
IocpBuffer *IocpBufferNew(int capacity);
void IocpBufferFree(IocpBuffer *bufPtr);
IOCP_INLINE void IocpInitWSABUF(WSABUF *wsaPtr, const IocpBuffer *bufPtr) {
    wsaPtr->buf = bufPtr->data.bytes;
    wsaPtr->len = bufPtr->data.capacity;
}

/* Generic channel functions */
IocpChannel *IocpChannelNew(Tcl_Interp *interp, const IocpChannelVtbl *vtblPtr);
void IocpChannelAwaitCompletion(IocpChannel *lockedChanPtr);
void IocpChannelWakeAfterCompletion(IocpChannel *lockedChanPtr);
void IocpChannelDrop(Tcl_Interp *interp, IocpChannel *lockedChanPtr);

/* Tcl commands */
Tcl_ObjCmdProc	Iocp_SocketObjCmd;

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

#endif

/*
 * Prototypes for IOCP exported functions.
 */
EXTERN int Iocp_Init(Tcl_Interp *interp);

#endif /* _TCLWINIOCP */
