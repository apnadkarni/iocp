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
#else
typedef SRWLOCK IocpLock;
#define IocpLockInit(lockPtr)             InitializeSRWLock(lockPtr)
#define IocpLockAcquireShared(lockPtr)    AcquireSRWLockShared(lockPtr)
#define IocpLockAcquireExclusive(lockPtr) AcquireSRWLockExclusive(lockPtr)
#define IocpLockReleaseShared(lockPtr)    ReleaseSRWLockShared(lockPtr)
#define IocpLockReleaseExclusive(lockPtr) ReleaseSRWLockExclusive(lockPtr)
#define IocpLockDelete(lockPtr)           (void) 0
#endif

/*
 * Forward declarations for structures
 */
typedef struct IocpList       IocpList;
typedef struct IocpTsd        IocpTsd;
typedef struct IocpChannel    IocpChannel;
typedef struct IocpDataBuffer IocpDataBuffer;
typedef struct IocpBuffer     IocpBuffer;

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
 * Multi-thread synchronization: access to fields of the structure is controlled
 * through IocpTsdLock/IocpTsdUnlock.
 *
 * Primary functions:
 *   IocpTsdNew           - allocates and initializes
 *   IocpTsdDrop          - drops a reference and deallocates if last reference.
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
    IocpLock     lock;          /* Synchronize access */
    Tcl_ThreadId threadId;      /* Thread id of owning thread */
    long         numRefs;       /* Reference count */
} IocpTsd;
typedef IocpTsd *IocpTsdPtr;
#define IOCP_TSD_INIT(keyPtr)                                   \
    (IocpTsdPtr *)Tcl_GetThreadData((keyPtr), sizeof(IocpTsdPtr))

IOCP_INLINE void IocpTsdLock(IocpTsd *tsdPtr) {
    /* readyChannels.lock does double duty to lock the tsd as a whole */
    IocpLockAcquireExclusive(&tsdPtr->lock);
}
IOCP_INLINE void IocpTsdUnlock(IocpTsd *tsdPtr) {
    IocpLockReleaseExclusive(&tsdPtr->lock);
}

/*
 * Common data shared across the IOCP implementation. This structure is
 * initialized once per process and referenced from both Tcl threads as well
 * as the IOCP worker threads.
 */
typedef struct IocpModuleState {
    HANDLE completion_port; /* The completion port around which life revolves */
    HANDLE completion_thread; /* Handle of thread servicing completions */
} IocpSubSystem;

/*
 * Structure to hold the actual data being passed around. This is always owned
 * by a IocpBuffer and allocated/freed with that owning IocpBuffer.
 */
typedef struct IocpDataBuffer {
    char *dataPtr;     /* Pointer to storage area */
    int   capacity;    /* Capacity of storage area */
    int   begin;       /* Offset where data begins */
    int   len;         /* Number of bytes of data */
} IocpDataBuffer;

/*
 * An IocpBuffer is used to queue I/O requests and pass data between
 * the channel, Windows, and the completion thread. Each instance is
 * associated with a specific channel as indicated by its channel field and
 * also a data area that holds the actual I/O bytes.
 *
 * As such, the IocpBuffer may be referenced from the Tcl thread via
 * the IocpChannel structure, the Windows kernel within the overlapped I/O
 * execution, and the completion thread. A reference count is used to ensure
 * it is not deallocated while a reference to it still exists.
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
        WSAOVERLAPPED wsa_overlap; /* Used for WinSock API */
        OVERLAPPED    overlap;     /* Used for general Win32 API's */
    } u;
    IocpChannel   *channelPtr;  /* Holds a counted reference to the
                                   associated IocpChannel */
    IocpDataBuffer data;        /* Data buffer */
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
 * since its reference count may not have reached 0 due to existingg references
 * from IocpBuffer's queued for I/O. When those buffers complete, they will be
 * freed at which point the IocpChannel reference count will also drop to 0
 * resulting in it being freed.
 *
 * Access to the structure is synchronized through the
 * IocpChannelLock/IocpChannelUnlock functions.
 */
typedef struct IocpChannel {
    IocpList  inputBuffers; /* Input buffers whose data is to be
                             * passed up to the Tcl channel layer. The list
                             * lock also serves as the lock for the IocpChannel
                             * fields */
    IocpTsd  *tsdPtr;       /* Pointer to owning thread data. Needed so we know
                             * which thread to notify of I/O completions. */
    IocpLock  lock;         /* Synchronization */
    LONG      numRefs;      /* Reference count */
} IocpChannel;
IOCP_INLINE void IocpChannelLock(IocpChannel *chanPtr) {
    /* inputBuffers.lock does double duty to lock the channel as a whole */
    IocpLockAcquireExclusive(&chanPtr->lock);
}
IOCP_INLINE void IocpChannelUnlock(IocpChannel *chanPtr) {
    IocpLockReleaseExclusive(&chanPtr->lock);
}


/*
 * Prototypes for IOCP internal functions.
 */


/* List utilities */
void IocpListAppend(IocpList *listPtr, IocpLink *linkPtr);
void IocpListPrepend(IocpList *listPtr, IocpLink *linkPtr);
void IocpLinkDetach(IocpList *listPtr, IocpLink *linkPtr);

/* Error utilities */
Tcl_Obj *Iocp_MapWindowsError(DWORD error, HANDLE moduleHandle);
IocpResultCode Iocp_ReportLastError(Tcl_Interp *interp);

/* Generic channel functions */
void IocpChannelInit(IocpChannel *chanPtr);

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
