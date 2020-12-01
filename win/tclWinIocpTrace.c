/*
 * tclWinIocpTrace.c --
 *
 *	Tracing module for Windows.
 *
 * Copyright (c) 2020 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include "tclWinIocp.h"

#ifdef IOCP_ENABLE_TRACE

#include <TraceLoggingProvider.h>

enum IocpTraceTarget {
    IOCP_TRACE_OFF,
    IOCP_TRACE_ETW,
    IOCP_TRACE_STDOUT,
    IOCP_TRACE_DEBUGGER
};
int iocpTraceTarget = IOCP_TRACE_OFF;

IocpLock iocpTraceLock;

/*
 * GUID format for traceview
 * 3a674e76-fe96-4450-b634-24fc587b2828
 */
TRACELOGGING_DEFINE_PROVIDER(
    iocpWinTraceProvider,
    "SimpleTraceLoggingProvider",
    (0x3a674e76, 0xfe96, 0x4450, 0xb6, 0x34, 0x24, 0xfc, 0x58, 0x7b, 0x28, 0x28));

/*
 *------------------------------------------------------------------------
 *
 * IocpTraceString --
 *
 *    Writes a single string to an ETW trace.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
void IocpTraceString(const char *buf)
{
    switch (iocpTraceTarget) {
    case IOCP_TRACE_OFF:
        break;
    case IOCP_TRACE_ETW:
        /* TraceLogging API already includes locking and thread id */
        TraceLoggingWrite(iocpWinTraceProvider,
                          "TclIocpTrace",
                          TraceLoggingString(buf, "Trace Message")
            );
        break;
    case IOCP_TRACE_STDOUT:
        IocpLockAcquireExclusive(&iocpTraceLock);
        printf("[%d] %s", GetCurrentThreadId(), buf);
        IocpLockReleaseExclusive(&iocpTraceLock);
        break;
    case IOCP_TRACE_DEBUGGER:
        /* Not sure lock needed but ...*/
        IocpLockAcquireExclusive(&iocpTraceLock);
        IocpDebuggerOut("[%d] %s", GetCurrentThreadId(), buf);
        IocpLockReleaseExclusive(&iocpTraceLock);
        break;
    }
}

/*
 *------------------------------------------------------------------------
 *
 * IocpTrace --
 *
 *    Writes a formatted string to an trace.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    As above.
 *
 *------------------------------------------------------------------------
 */
void __cdecl IocpTrace(
    const char *formatStr,
    ...
    )
{
    char buf[2048];
    va_list args;

    va_start(args, formatStr);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, formatStr, args);
    va_end(args);
    buf[sizeof(buf)-1] = '\0';

    IocpTraceString(buf);
}

IocpTclCode
Iocp_TraceOutputObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    if (objc > 1)
        IocpTraceString(Tcl_GetString(objv[1]));
    return TCL_OK;
}

IocpTclCode
Iocp_TraceConfigureObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    int opt;

    for (opt = 1; opt < objc; ++opt) {
        const char *optName = Tcl_GetString(objv[opt]);
        if (!strcmp(optName, "off")) {
            iocpTraceTarget = IOCP_TRACE_OFF;
        }
        else if (!strcmp(optName, "etw")) {
            iocpTraceTarget = IOCP_TRACE_ETW;
        }
        else if (!strcmp(optName, "stdout")) {
            iocpTraceTarget = IOCP_TRACE_STDOUT;
        }
        else if (!strcmp(optName, "debugger")) {
            iocpTraceTarget = IOCP_TRACE_DEBUGGER;
        }
        else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("Invalid trace configure option \"%s\". Should be one of off, etw, stdout or debugger.", optName));
            return TCL_ERROR;
        }
    }
    return TCL_OK;
}

void IocpTraceInit()
{
    IocpLockInit(&iocpTraceLock);
    /* TBD - do we have to call TraceLoggingUnregister before exiting process */
    TraceLoggingRegister(iocpWinTraceProvider);
}

#endif /* IOCP_ENABLE_TRACE */
