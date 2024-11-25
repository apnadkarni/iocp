/*
 * IOCP Bluetooth module
 */

#include "tclWinIocp.h"
#include "tclWinIocpWinsock.h"

#include <stdio.h> /* For snprintf_s */

#define STRING_LITERAL_OBJ(s) Tcl_NewStringObj(s, sizeof(s)-1)

#define IOCP_BT_NAME_PREFIX      "bt"

/*
 * BT_COMMAND enums are used as ClientData passed to commands
 * and distinguish which particular subcommand to execute.
 */
enum BT_COMMAND
{
    BT_ENABLE_DISCOVERY,
    BT_ENABLE_INCOMING,
    BT_STATUS_DISCOVERY,
    BT_STATUS_INCOMING,
};

/*
 * Since not all systems have the DLL installed, we need to load at runtime.
 * This structure holds the pointers.
 */
typedef HBLUETOOTH_RADIO_FIND (WINAPI BluetoothFindFirstRadioProc) (const BLUETOOTH_FIND_RADIO_PARAMS *pbtfrp, HANDLE *phRadio);
typedef BOOL (WINAPI BluetoothFindNextRadioProc) (HBLUETOOTH_RADIO_FIND hFind, HANDLE *phRadio);
typedef BOOL (WINAPI BluetoothFindRadioCloseProc)(HBLUETOOTH_RADIO_FIND hFind);
typedef DWORD (WINAPI BluetoothGetRadioInfoProc) (HANDLE hRadio, PBLUETOOTH_RADIO_INFO pRadioInfo);
typedef HBLUETOOTH_DEVICE_FIND (WINAPI BluetoothFindFirstDeviceProc) (const BLUETOOTH_DEVICE_SEARCH_PARAMS *pbtsp, BLUETOOTH_DEVICE_INFO *pbtdi);
typedef BOOL (WINAPI BluetoothFindNextDeviceProc) (HBLUETOOTH_DEVICE_FIND hFind, BLUETOOTH_DEVICE_INFO *pbtdi);
typedef BOOL (WINAPI BluetoothFindDeviceCloseProc)(HBLUETOOTH_DEVICE_FIND hFind);
typedef DWORD (WINAPI BluetoothGetDeviceInfoProc) (HANDLE hRadio, BLUETOOTH_DEVICE_INFO *pbtdi);
typedef DWORD (WINAPI BluetoothRemoveDeviceProc)(const BLUETOOTH_ADDRESS *pAddress);
typedef DWORD (WINAPI BluetoothEnumerateInstalledServicesProc) (HANDLE hRadio, const BLUETOOTH_DEVICE_INFO *pbtdi, DWORD *pcServiceInout, GUID *pGuidServices);
typedef BOOL (WINAPI BluetoothEnableDiscoveryProc)(HANDLE hRadio, BOOL fEnabled);
typedef BOOL (WINAPI BluetoothIsDiscoverableProc)(HANDLE hRadio);
typedef BOOL (WINAPI BluetoothEnableIncomingConnectionsProc)(HANDLE hRadio, BOOL fEnabled);
typedef BOOL (WINAPI BluetoothIsConnectableProc)(HANDLE hRadio);
static struct BT_API {
    BOOL initialized;
    BluetoothFindFirstRadioProc *pBluetoothFindFirstRadio;
    BluetoothFindNextRadioProc *pBluetoothFindNextRadio;
    BluetoothFindRadioCloseProc *pBluetoothFindRadioClose;
    BluetoothGetRadioInfoProc *pBluetoothGetRadioInfo;
    BluetoothFindFirstDeviceProc *pBluetoothFindFirstDevice;
    BluetoothFindNextDeviceProc *pBluetoothFindNextDevice;
    BluetoothFindDeviceCloseProc *pBluetoothFindDeviceClose;
    BluetoothGetDeviceInfoProc *pBluetoothGetDeviceInfo;
    BluetoothRemoveDeviceProc *pBluetoothRemoveDevice;
    BluetoothEnumerateInstalledServicesProc *pBluetoothEnumerateInstalledServices;
    BluetoothEnableDiscoveryProc *pBluetoothEnableDiscovery;
    BluetoothIsDiscoverableProc *pBluetoothIsDiscoverable;
    BluetoothEnableIncomingConnectionsProc *pBluetoothEnableIncomingConnections;
    BluetoothIsConnectableProc *pBluetoothIsConnectable;
} gBtAPI; /* Default initialized to 0's */

static Tcl_Obj *ObjFromSYSTEMTIME(const SYSTEMTIME *timeP);
static Tcl_Obj *ObjFromBLUETOOTH_ADDRESS (const BLUETOOTH_ADDRESS *addrPtr);
static IocpTclCode ObjToBLUETOOTH_ADDRESS(
    Tcl_Interp *interp,
    Tcl_Obj *objP,
    BLUETOOTH_ADDRESS *btAddrPtr);
static Tcl_Obj *ObjFromBLUETOOTH_RADIO_INFO (const BLUETOOTH_RADIO_INFO *infoPtr);
static Tcl_Obj *ObjFromBLUETOOTH_DEVICE_INFO (const BLUETOOTH_DEVICE_INFO *infoPtr);

/* Checks if API exists, and if not, returns from caller with TCL_ERROR */
#define BTAPICHECK(interp_, fnname_)               \
    do {                                           \
        if (gBtAPI.p##fnname_ == NULL)             \
            return BT_ReportGetProcError(interp_); \
    } while (0)

static IocpTclCode
BT_ReportGetProcError(Tcl_Interp *interp)
{
    return Iocp_ReportWindowsError(
        interp, ERROR_PROC_NOT_FOUND, "Bluetooth API function not available. ");
}

static IocpTclCode
BT_CloseHandleObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *const objv[])		/* Argument objects. */
{
    HANDLE handle;
    int tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
        return TCL_ERROR;
    }

    tclResult = PointerObjUnregisterAnyOf(interp, objv[1], &handle, "HANDLE", "HRADIO", NULL);
    if (tclResult != TCL_OK)
        return tclResult;

    if (CloseHandle(handle) != TRUE)
        return Iocp_ReportLastWindowsError(interp, "Could not close Bluetooth radio handle: ");

    return TCL_OK;
}

static IocpTclCode
BT_FindFirstRadioObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    Tcl_Obj *objs[2];
    HANDLE   radioHandle;
    HBLUETOOTH_RADIO_FIND findHandle;
    BLUETOOTH_FIND_RADIO_PARAMS btParams;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothFindFirstRadio);
    BTAPICHECK(interp, BluetoothFindRadioClose);

    btParams.dwSize = sizeof(btParams);
    findHandle = gBtAPI.pBluetoothFindFirstRadio(&btParams, &radioHandle);

    if (findHandle == NULL) {
        IocpWinError winError = GetLastError();
        if (winError == ERROR_NO_MORE_DEVICES)
            return TCL_OK;
        else
            return Iocp_ReportWindowsError(interp, winError, "Could not locate Bluetooth radios: ");
    }

    if (PointerRegister(interp, findHandle, "HBLUETOOTH_RADIO_FIND", &objs[0])
        == TCL_OK) {
        if (PointerRegister(interp, radioHandle, "HRADIO", &objs[1])
            == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewListObj(2,objs));
            return TCL_OK;
        } else {
            PointerUnregister(interp, findHandle, "HBLUETOOTH_RADIO_FIND");
            Tcl_DecrRefCount(objv[0]);
        }
    }
    if (radioHandle)
        CloseHandle(radioHandle);
    if (findHandle)
        gBtAPI.pBluetoothFindRadioClose(findHandle);
    return TCL_ERROR;
}

static IocpTclCode
BT_FindFirstRadioCloseObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HBLUETOOTH_RADIO_FIND findHandle;
    int tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_RADIO_FIND");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothFindRadioClose);

    tclResult = PointerObjUnregister(interp, objv[1],
                                     &findHandle, "HBLUETOOTH_RADIO_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (gBtAPI.pBluetoothFindRadioClose(findHandle) != TRUE)
        return Iocp_ReportLastWindowsError(interp, "Could not close Bluetooth radio search handle: ");

    return TCL_OK;
}

static IocpTclCode
BT_FindNextRadioObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HBLUETOOTH_RADIO_FIND findHandle;
    HANDLE radioHandle;
    int tclResult;
    Tcl_Obj *objP;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_RADIO_FIND");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothFindNextRadio);

    tclResult = PointerObjVerify(interp, objv[1], &findHandle, "HBLUETOOTH_RADIO_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (gBtAPI.pBluetoothFindNextRadio(findHandle, &radioHandle) != TRUE) {
        DWORD winError = GetLastError();
        if (winError == ERROR_NO_MORE_ITEMS)
            return TCL_BREAK;
        else
            return Iocp_ReportWindowsError(interp, winError, "Error fetching next radio: ");
    }

    tclResult = PointerRegister(interp, radioHandle, "HRADIO", &objP);
    if (tclResult == TCL_OK)
        Tcl_SetObjResult(interp, objP);
    else
        CloseHandle(radioHandle);
    return TCL_OK;
}

static IocpTclCode
BT_GetRadioInfoObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HANDLE radioHandle;
    BLUETOOTH_RADIO_INFO info;
    int tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HRADIO");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothGetRadioInfo);

    tclResult = PointerObjVerify(interp, objv[1], &radioHandle, "HRADIO");
    if (tclResult != TCL_OK)
        return tclResult;

    info.dwSize = sizeof(info);
    if (gBtAPI.pBluetoothGetRadioInfo(radioHandle,&info) != ERROR_SUCCESS)
        return Iocp_ReportLastWindowsError(interp, "Could not get Bluetooth radio information: ");

    Tcl_SetObjResult(interp, ObjFromBLUETOOTH_RADIO_INFO(&info));
    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * BT_FindFirstDeviceObj --
 *
 *    Initiates a search for a Bluetooth device matching specified options.
 *
 * Results:
 *    List of two elements consisting of the search handle and first device
 *    handle. These handles must be closed appropriately.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static IocpTclCode
BT_FindFirstDeviceObjCmd(
    ClientData notUsed,    /* Not used. */
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *CONST objv[]) /* Argument objects. */
{
    static const char *const opts[] = {
        "-authenticated", "-remembered", "-unknown", "-connected", "-inquire",
        "-timeout", "-hradio", NULL
    };
    enum Opts {
        AUTHENTICATED, REMEMBERED, UNKNOWN, CONNECTED, INQUIRE, TIMEOUT, RADIO
    };
    int i, opt, timeout, tclResult;
    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    BLUETOOTH_DEVICE_INFO          info;
    HBLUETOOTH_DEVICE_FIND         findHandle;
    Tcl_Obj *objs[2];

    BTAPICHECK(interp, BluetoothFindFirstDevice);

    memset(&params, 0, sizeof(params));
    params.dwSize = sizeof(params);
    params.cTimeoutMultiplier = 8; /* Default if -inquire is specified, else ignored */
    for (i = 1; i < objc; ++i) {
        if (Tcl_GetIndexFromObj(interp, objv[i], opts, "option",
                                TCL_EXACT, &opt) != TCL_OK) {
            return TCL_ERROR;
        }
        switch ((enum Opts) opt) {
        case AUTHENTICATED: params.fReturnAuthenticated = 1; break;
        case REMEMBERED   : params.fReturnRemembered = 1; break;
        case UNKNOWN      : params.fReturnUnknown = 1; break;
        case CONNECTED    : params.fReturnConnected = 1; break;
        case INQUIRE      : params.fIssueInquiry = 1; break;
        case TIMEOUT      :
            if (++i >= objc) {
                Tcl_SetObjResult(interp,
                                 STRING_LITERAL_OBJ("no argument given for -timeout option"));
                return TCL_ERROR;
            }
            if (Tcl_GetIntFromObj(interp, objv[i], &timeout) != TCL_OK)
                return TCL_ERROR;
            /* timeout is in milliseconds. The cTimeoutMultiplier is units of 1280ms */
            if (timeout <= 0)
                params.cTimeoutMultiplier = 0;
            else if (timeout >= (48*1280))
                params.cTimeoutMultiplier = 48; /* Max permitted value */
            else
                params.cTimeoutMultiplier = (timeout + 1279)/1280;
            break;
        case RADIO:
            if (++i >= objc) {
                Tcl_SetObjResult(interp,
                                 STRING_LITERAL_OBJ("no argument given for -radio option"));
                return TCL_ERROR;
            }
            if (PointerObjVerify(interp, objv[i], &params.hRadio, "HRADIO") != TCL_OK)
                return TCL_ERROR;
        }
    }

    /*
     * If no filters specified, return all.
     */
    if (! (params.fReturnAuthenticated || params.fReturnRemembered
            || params.fReturnUnknown || params.fReturnConnected) ) {
        params.fReturnAuthenticated = 1;
        params.fReturnRemembered    = 1;
        params.fReturnUnknown       = 1;
        params.fReturnConnected     = 1;
    }

    info.dwSize = sizeof(info);
    findHandle = gBtAPI.pBluetoothFindFirstDevice(&params, &info);
    /* TBD - what is returned if there are no bluetooth devices */
    if (findHandle == NULL) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS)
            return TCL_OK;/* Empty list */
        else
            return Iocp_ReportLastWindowsError(
                interp, "Bluetooth device search failed: ");
    }

    tclResult = PointerRegister(interp, findHandle,
                                "HBLUETOOTH_DEVICE_FIND", &objs[0]);
    if (tclResult == TCL_OK) {
        objs[1] = ObjFromBLUETOOTH_DEVICE_INFO(&info);
        Tcl_SetObjResult(interp, Tcl_NewListObj(2, objs));
    }
    return tclResult;
}

static IocpTclCode
BT_FindFirstDeviceCloseObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HBLUETOOTH_DEVICE_FIND findHandle;
    int tclResult;

    BTAPICHECK(interp, BluetoothFindDeviceClose);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_DEVICE_FIND");
        return TCL_ERROR;
    }
    tclResult = PointerObjUnregister(interp, objv[1], &findHandle, "HBLUETOOTH_DEVICE_FIND");
    if (tclResult == TCL_OK) {
        if (gBtAPI.pBluetoothFindDeviceClose(findHandle) != TRUE)
            tclResult = Iocp_ReportLastWindowsError(interp, "Could not close Bluetooth device search handle: ");
    }
    return tclResult;
}

static IocpTclCode
BT_FindNextDeviceObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HBLUETOOTH_DEVICE_FIND findHandle;
    BLUETOOTH_DEVICE_INFO info;
    int tclResult;

    BTAPICHECK(interp, BluetoothFindNextDevice);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_DEVICE_FIND");
        return TCL_ERROR;
    }
    tclResult = PointerObjVerify(interp, objv[1], &findHandle, "HBLUETOOTH_DEVICE_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    info.dwSize = sizeof(info);
    if (gBtAPI.pBluetoothFindNextDevice(findHandle, &info) != TRUE) {
        DWORD winError = GetLastError();
        if (winError == ERROR_NO_MORE_ITEMS)
            return TCL_BREAK;
        else
            return Iocp_ReportWindowsError(interp, winError, "Error fetching next device: ");
    }

    Tcl_SetObjResult(interp, ObjFromBLUETOOTH_DEVICE_INFO(&info));
    return TCL_OK;
}

static IocpTclCode
BT_GetDeviceInfoObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HANDLE radioHandle;
    BLUETOOTH_DEVICE_INFO info;
    int tclResult;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "HRADIO BTADDR");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothGetDeviceInfo);

    tclResult = PointerObjVerify(interp, objv[1], &radioHandle, "HRADIO");
    if (tclResult != TCL_OK)
        return tclResult;

    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[2], &info.Address);
    if (tclResult != TCL_OK)
        return tclResult;

    info.dwSize = sizeof(info);
    if (gBtAPI.pBluetoothGetDeviceInfo(radioHandle,&info) != ERROR_SUCCESS)
        return Iocp_ReportLastWindowsError(interp, "Could not get Bluetooth radio information: ");

    Tcl_SetObjResult(interp, ObjFromBLUETOOTH_DEVICE_INFO(&info));
    return TCL_OK;
}

int BT_RemoveDeviceObjCmd (
    ClientData notUsed,
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *const objv[]) /* Argument objects. */
{
    BLUETOOTH_ADDRESS btAddress;
    int tclResult;
    DWORD winError;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "BTADDR");
        return TCL_ERROR;
    }
    BTAPICHECK(interp, BluetoothRemoveDevice);
    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[1], &btAddress);
    if (tclResult != TCL_OK)
        return tclResult;

    winError = gBtAPI.pBluetoothRemoveDevice(&btAddress);
    if (winError != ERROR_SUCCESS && winError != ERROR_NOT_FOUND) {
        return Iocp_ReportWindowsError(interp, winError, "Could not remove device: ");
    }
    return TCL_OK;
}

#ifdef NOTUSED /* Same functionality implemented at script level albeit slower */

/*
 *------------------------------------------------------------------------
 *
 * BT_EnumerateDevicesObjCmd --
 *
 *    Returns a list of devices.
 *
 *
 * Results:
 *    TCL_OK - List of devices is stored as interp result.
 *    TCL_ERROR - Error message is stored in interp
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static IocpTclCode
BT_EnumerateDevicesObjCmd(
    ClientData  notUsed,
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *CONST objv[]) /* Argument objects. */
{
    static const char *const opts[] = {
        "-authenticated", "-remembered", "-unknown", "-connected", "-inquire",
        "-timeout", "-radio", NULL
    };
    enum Opts {
        AUTHENTICATED, REMEMBERED, UNKNOWN, CONNECTED, INQUIRE, TIMEOUT, RADIO
    };
    int i, opt, timeout, tclResult, winError;
    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    BLUETOOTH_DEVICE_INFO          info;
    HBLUETOOTH_DEVICE_FIND         findHandle;
    Tcl_Obj *resultObj = NULL;

    BTAPICHECK(interp, BluetoothFindFirstDevice);
    BTAPICHECK(interp, BluetoothFindNextDevice);
    BTAPICHECK(interp, BluetoothFindDeviceClose);

    memset(&params, 0, sizeof(params));
    params.dwSize = sizeof(params);
    for (i = 1; i < objc; ++i) {
        const char *optname = Tcl_GetString(objv[i]);
        if (Tcl_GetIndexFromObj(interp, objv[i], opts, "option",
                                TCL_EXACT, &opt) != TCL_OK) {
            return TCL_ERROR;
        }
        switch ((enum Opts) opt) {
        case AUTHENTICATED: params.fReturnAuthenticated = 1; break;
        case REMEMBERED   : params.fReturnRemembered = 1; break;
        case UNKNOWN      : params.fReturnUnknown = 1; break;
        case CONNECTED    : params.fReturnConnected = 1; break;
        case INQUIRE      : params.fIssueInquiry = 1; break;
        case TIMEOUT      :
            if (++i >= objc) {
                Tcl_SetObjResult(interp,
                                 STRING_LITERAL_OBJ("no argument given for -timeout option"));
                return TCL_ERROR;
            }
            if (Tcl_GetIntFromObj(interp, objv[i], &timeout) != TCL_OK)
                return TCL_ERROR;
            /* timeout is in milliseconds. The cTimeoutMultiplier is units of 1280ms */
            if (timeout <= 0)
                params.cTimeoutMultiplier = 0;
            else if (timeout >= (48*1280))
                params.cTimeoutMultiplier = 48; /* Max permitted value */
            else
                params.cTimeoutMultiplier = (timeout + 1279)/1280;
            break;
        case RADIO:
            if (++i >= objc) {
                Tcl_SetObjResult(interp,
                                 STRING_LITERAL_OBJ("no argument given for -radio option"));
                return TCL_ERROR;
            }
            if (PointerObjVerify(interp, objv[i], &params.hRadio, "HRADIO") != TCL_OK)
                return TCL_ERROR;
        }
    }

    info.dwSize = sizeof(info);
    findHandle = gBtAPI.pBluetoothFindFirstDevice(&params, &info);
    if (findHandle == NULL) {
        winError = GetLastError();
        if (winError == ERROR_NO_MORE_ITEMS) {
            return TCL_OK;
        } else {
            return Iocp_ReportWindowsError(interp, winError, NULL);
        }
    }

    resultObj = Tcl_NewListObj(0, NULL);
    do {
        Tcl_ListObjAppendElement(interp, resultObj, ObjFromBLUETOOTH_DEVICE_INFO(&info));
    } while (gBtAPI.pBluetoothFindNextDevice(findHandle, &info) == TRUE);

    winError = GetLastError();
    if (winError != ERROR_NO_MORE_ITEMS) {
        Tcl_DecrRefCount(resultObj);
        tclResult = Iocp_ReportWindowsError(interp, winError, NULL);
    } else {
        Tcl_SetObjResult(interp, resultObj);
        tclResult = TCL_OK;
    }
    gBtAPI.pBluetoothFindDeviceClose(findHandle);
    return tclResult;
}
#endif /* NOTUSED */

static IocpTclCode
BT_ConfigureRadioObjCmd (
    ClientData clientData,      /* discovery or incoming */
    Tcl_Interp *interp,         /* Current interpreter. */
    int objc,                   /* number of arguments. */
    Tcl_Obj *CONST objv[])      /* argument objects. */
{
    HANDLE radioHandle;
    int tclResult;
    int enable;
    int changed;
    enum BT_COMMAND command = (enum BT_COMMAND)clientData;

    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN ?HRADIO?");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothEnableDiscovery);
    BTAPICHECK(interp, BluetoothEnableIncomingConnections);

    tclResult = Tcl_GetBooleanFromObj(interp, objv[1], &enable);
    if (tclResult != TCL_OK)
        return tclResult;

    radioHandle = NULL;
    if (objc > 2) {
        tclResult = PointerObjVerify(interp, objv[2], &radioHandle, "HRADIO");
        if (tclResult != TCL_OK)
            return tclResult;
    }

    if (command == BT_ENABLE_DISCOVERY) {
        changed = gBtAPI.pBluetoothEnableDiscovery(radioHandle, enable);
    } else if (command == BT_ENABLE_INCOMING) {
        changed = gBtAPI.pBluetoothEnableIncomingConnections(radioHandle, enable);
    } else {
        Tcl_Panic("Unexpected clientData parameter value %d", command);
        changed = 0;            /* NOTREACHED - Keep compiler happy */
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(changed));
    return TCL_OK;

}

static IocpTclCode
BT_RadioStatusObjCmd (
    ClientData clientData,      /* BT_STATUS_DISCOVERY/BT_STATUS_INCOMING*/
    Tcl_Interp *interp,         /* Current interpreter. */
    int objc,                   /* number of arguments. */
    Tcl_Obj *CONST objv[])      /* argument objects. */
{
    HANDLE radioHandle;
    int status;
    int tclResult;
    enum BT_COMMAND command = (enum BT_COMMAND)clientData;

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?HRADIO?");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothIsDiscoverable);
    BTAPICHECK(interp, BluetoothIsConnectable);

    radioHandle = NULL;
    if (objc > 1) {
        tclResult = PointerObjVerify(interp, objv[1], &radioHandle, "HRADIO");
        if (tclResult != TCL_OK)
            return tclResult;
    }

    if (command == BT_STATUS_DISCOVERY) {
        status = gBtAPI.pBluetoothIsDiscoverable(radioHandle);
    } else if (command == BT_STATUS_INCOMING) {
        status = gBtAPI.pBluetoothIsConnectable(radioHandle);
    } else {
        Tcl_Panic("Unexpected clientData parameter value %d", command);
        status = 0;            /* NOTREACHED - Keep compiler happy */
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status));
    return TCL_OK;

}

int BT_EnumerateInstalledServicesObjCmd (
    ClientData notUsed,
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *const objv[]) /* Argument objects. */
{
    HANDLE radioH;
    DWORD  count;
    GUID   services[20];
    GUID  *serviceP;
    DWORD  winError;
    int    tclResult;
    BLUETOOTH_DEVICE_INFO info;

    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "BTADDR ?HRADIO?");
        return TCL_ERROR;
    }

    BTAPICHECK(interp, BluetoothEnumerateInstalledServices);

    info.dwSize = sizeof(info);
    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[1], &info.Address);
    if (tclResult != TCL_OK)
        return tclResult;

    if (objc > 2) {
        tclResult = PointerObjVerify(interp, objv[2], &radioH, "HRADIO");
        if (tclResult != TCL_OK)
            return tclResult;
    } else {
        radioH = NULL;
    }

    serviceP = services;
    count    = sizeof(services)/sizeof(services[0]);
    /* Loop while error is lack of buffer space */
    while ((winError = gBtAPI.pBluetoothEnumerateInstalledServices(
                radioH, &info, &count, serviceP)) == ERROR_MORE_DATA) {
        /* Free previous allocation if not the static space and allocate new */
        if (serviceP != services)
            ckfree(serviceP);
        serviceP = ckalloc(sizeof(*serviceP) * count);
    }
    if (winError == ERROR_SUCCESS) {
        Tcl_Obj *resultObj = Tcl_NewListObj(count, NULL);
        unsigned int i;
        for (i = 0; i < count; ++i) {
            Tcl_ListObjAppendElement(interp, resultObj, WrapUuid(&serviceP[i]));
        }
        Tcl_SetObjResult(interp, resultObj);
    } else {
        tclResult = Iocp_ReportWindowsError(
            interp, winError, "Could not retrieve Bluetooth services: ");
    }
    if (serviceP != services)
        ckfree(serviceP);

    return tclResult;
}

static Tcl_Obj *ObjFromBLUETOOTH_RADIO_INFO (const BLUETOOTH_RADIO_INFO *infoPtr)
{
    Tcl_Obj *objs[10];

    objs[0] = STRING_LITERAL_OBJ("Address");
    objs[1] = ObjFromBLUETOOTH_ADDRESS(&infoPtr->address);
    objs[2] = STRING_LITERAL_OBJ("Name");
    objs[3] = IocpNewWincharObj(infoPtr->szName, -1);
    objs[4] = STRING_LITERAL_OBJ("Class");
    objs[5] = Tcl_NewWideIntObj(infoPtr->ulClassofDevice);
    objs[6] = STRING_LITERAL_OBJ("Subversion");
    objs[7] = Tcl_NewIntObj(infoPtr->lmpSubversion);
    objs[8] = STRING_LITERAL_OBJ("Manufacturer");
    objs[9] = Tcl_NewIntObj(infoPtr->manufacturer);
    return Tcl_NewListObj(10, objs);
}


static Tcl_Obj *ObjFromBLUETOOTH_DEVICE_INFO (const BLUETOOTH_DEVICE_INFO *infoPtr)
{
    Tcl_Obj *objs[16];

    objs[0] = STRING_LITERAL_OBJ("Address");
    objs[1] = ObjFromBLUETOOTH_ADDRESS(&infoPtr->Address);
    objs[2] = STRING_LITERAL_OBJ("Name");
    objs[3] = IocpNewWincharObj(infoPtr->szName, -1);
    objs[4] = STRING_LITERAL_OBJ("Class");
    objs[5] = Tcl_NewWideIntObj(infoPtr->ulClassofDevice);
    objs[6] = STRING_LITERAL_OBJ("Connected");
    objs[7] = Tcl_NewBooleanObj(infoPtr->fConnected);
    objs[8] = STRING_LITERAL_OBJ("Remembered");
    objs[9] = Tcl_NewBooleanObj(infoPtr->fRemembered);
    objs[10] = STRING_LITERAL_OBJ("Authenticated");
    objs[11] = Tcl_NewBooleanObj(infoPtr->fAuthenticated);
    objs[12] = STRING_LITERAL_OBJ("LastSeen");
    objs[13] = ObjFromSYSTEMTIME(&infoPtr->stLastSeen);
    objs[14] = STRING_LITERAL_OBJ("LastUsed");
    objs[15] = ObjFromSYSTEMTIME(&infoPtr->stLastUsed);
    return Tcl_NewListObj(16, objs);
}

static Tcl_Obj *ObjFromSYSTEMTIME(const SYSTEMTIME *timeP)
{
    Tcl_Obj *objv[8];

    /* Fields are not in order they occur in SYSTEMTIME struct
       This is intentional for ease of formatting at script level */
    objv[0] = Tcl_NewIntObj(timeP->wYear);
    objv[1] = Tcl_NewIntObj(timeP->wMonth);
    objv[2] = Tcl_NewIntObj(timeP->wDay);
    objv[3] = Tcl_NewIntObj(timeP->wHour);
    objv[4] = Tcl_NewIntObj(timeP->wMinute);
    objv[5] = Tcl_NewIntObj(timeP->wSecond);
    objv[6] = Tcl_NewIntObj(timeP->wMilliseconds);
    objv[7] = Tcl_NewIntObj(timeP->wDayOfWeek);

    return Tcl_NewListObj(8, objv);
}

char *StringFromBLUETOOTH_ADDRESS(
    const BLUETOOTH_ADDRESS *addrPtr,
    char  *bufPtr,
    int    bufSize) /* Should be at least 18 bytes, else truncated */
{
    /* NOTE: *p must be unsigned for the snprintf to format correctly */
    const unsigned char *p = addrPtr->rgBytes;
    /*
     * Addresses are little endian. Hence order of storage. This matches
     * what's displayed via Device Manager in Windows.
     */
    _snprintf_s(bufPtr, bufSize, _TRUNCATE, "%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
                p[5], p[4], p[3], p[2], p[1], p[0]);
    return bufPtr;
}

static Tcl_Obj *ObjFromBLUETOOTH_ADDRESS (const BLUETOOTH_ADDRESS *addrPtr)
{
    Tcl_Obj *objP;
    char *bytes = ckalloc(18);

    StringFromBLUETOOTH_ADDRESS(addrPtr, bytes, 18);

    objP = Tcl_NewObj();
    Tcl_InvalidateStringRep(objP);
    objP->bytes = bytes;
    objP->length = 17;          /* Not counting terminating \0 */

    return objP;
}

static IocpTclCode ObjToBLUETOOTH_ADDRESS(
    Tcl_Interp *interp,
    Tcl_Obj *objP,
    BLUETOOTH_ADDRESS *btAddrPtr)
{
    /*
     * Use temp storage so as to not modify btAddrPtr[] on error
     */
    unsigned char bytes[6];

#if 0
    /* Works with VS2017 but breaks with some versions of Mingw that have
       trouble with hhx.
    */
    unsigned char trailer; /* Used to ensure no trailing characters */
    if (sscanf_s(Tcl_GetString(objP),
               "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx%c",
                 &bytes[5], &bytes[4], &bytes[3], &bytes[2], &bytes[1], &bytes[0], &trailer, 1) == 6) {
        int i;
        btAddrPtr->ullLong = 0;
        for (i = 0; i < 6; ++i)
            btAddrPtr->rgBytes[i] = bytes[i];
        return TCL_OK;
    } else {
        Tcl_AppendResult(interp, "Invalid Bluetooth address ", Tcl_GetString(objP), ".", NULL);
        return TCL_ERROR;
    }
#else

    int i;
    char ch;
    char *s = Tcl_GetString(objP);
    for (i = 5; i >= 0; --i) {
        unsigned int byte;
        ch = *s++;

        /* Get high nibble */
        if (ch >= '0' && ch <= '9') {
            byte = (ch - '0') << 4;
        } else if (ch >= 'A' && ch <= 'F') {
            byte = (ch - 'A' + 10) << 4;
        } else if (ch >= 'a' && ch <= 'f') {
            byte = (ch - 'a' + 10) << 4;
        } else {
            /* \0 or some some other char */
            goto bad_address;
        }

        /* Get low nibble */
        ch = *s++;
        if (ch >= '0' && ch <= '9') {
            byte |= (ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            byte |= (ch - 'A' + 10);
        } else if (ch >= 'a' && ch <= 'f') {
            byte |= (ch - 'a' + 10);
        } else {
            /* \0 or some some other char */
            goto bad_address;
        }
        /* Point to ":" separator or last \0 */
        ch = *s++;
        if (ch != ':' && ch != '-' && ch != 0)
            goto bad_address;
        bytes[i] = byte;
    }

    /* All 6 bytes filled and no leftover chars in string -> OK */
    if (i < 0 && ch == '\0') {
        /*
         * Need to clear out high 2 bytes even though they are unused.
         * Else fails on x86
         */
        btAddrPtr->ullLong = 0;
        /* Copy the remaining 6 bytes into lower bytes of ullLong */
        for (i = 0; i < 6; ++i)
            btAddrPtr->rgBytes[i] = bytes[i];
        return TCL_OK;
    }

bad_address:
    Tcl_AppendResult(interp, "Invalid Bluetooth address ", Tcl_GetString(objP), ".", NULL);
    return TCL_ERROR;

#endif



}

static Tcl_Obj *ObjFromSOCKADDR_BTH(const SOCKADDR_BTH *sockAddr)
{
    Tcl_Obj *objs[8];
    BLUETOOTH_ADDRESS btAddr;

    btAddr.ullLong = sockAddr->btAddr;

    objs[0] = STRING_LITERAL_OBJ("AddressFamily");
    objs[1] = Tcl_NewIntObj(sockAddr->addressFamily);
    objs[2] = STRING_LITERAL_OBJ("Address");
    objs[3] = ObjFromBLUETOOTH_ADDRESS(&btAddr);
    objs[4] = STRING_LITERAL_OBJ("ServiceClassId");
    objs[5] = Tclh_WrapUuid(&sockAddr->serviceClassId);
    objs[6] = STRING_LITERAL_OBJ("Port");
    objs[7] = Tcl_NewIntObj(sockAddr->port);
    return Tcl_NewListObj(8, objs);
}

#ifdef IOCP_DEBUG
static IocpTclCode
BT_FormatAddressObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    int tclResult;
    BLUETOOTH_ADDRESS address;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "ADDRESS");
        return TCL_ERROR;
    }
    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[1], &address);
    if (tclResult != TCL_OK)
        return tclResult;
    Tcl_SetObjResult(interp, ObjFromBLUETOOTH_ADDRESS(&address));
    return TCL_OK;

}
#endif

/*
 * Implementation of Bluetooth channels.
 */
static IocpWinError BtClientBlockingConnect(IocpChannel *);

static IocpChannelVtbl btClientVtbl =  {
    /* "Virtual" functions */
    WinsockClientInit,
    WinsockClientFinit,
    WinsockClientShutdown,
    NULL,                       /* Accept */
    BtClientBlockingConnect,
    WinsockClientAsyncConnected,
    WinsockClientAsyncConnectFailed,
    WinsockClientDisconnected,
    WinsockClientPostRead,
    WinsockClientPostWrite,
    WinsockClientGetHandle,
    WinsockClientGetOption,
    WinsockClientSetOption,
    WinsockClientTranslateError,
    /* Data members */
    iocpWinsockOptionNames,
    sizeof(WinsockClient)
};
IOCP_INLINE int IocpIsBtClient(IocpChannel *chanPtr) {
    return (chanPtr->vtblPtr == &btClientVtbl);
}

/*
 *------------------------------------------------------------------------
 *
 * BTClientBlockingConnect --
 *
 *    Attempt to connect to the specified server. Will block until
 *    the connection succeeds or fails.
 *
 * Results:
 *    0 on success, other Windows error code.
 *
 * Side effects:
 *    On success, a BT connection is establised. The btPtr state is changed
 *    to OPEN and an initialized socket is stored in it. The IOCP completion
 *    port is associated with it.
 *
 *    On failure, btPtr state is changed to CONNECT_FAILED and the returned
 *    error is also stored in btPtr->base.winError.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError
BtClientBlockingConnect(
    IocpChannel *chanPtr) /* May or may not be locked but caller must ensure
                           * exclusivity */
{
    WinsockClient *btPtr = IocpChannelToWinsockClient(chanPtr);
    DWORD          winError = 0;
    SOCKET         so = INVALID_SOCKET;

    IOCP_ASSERT(IocpIsBtClient(chanPtr));

    /* Note socket call, unlike WSASocket is overlapped by default */
    so = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (so != INVALID_SOCKET) {
        if (btPtr->flags & IOCP_WINSOCK_AUTHENTICATE) {
            ULONG ul = 1;
            if (setsockopt(so,
                           SOL_RFCOMM,
                           SO_BTH_AUTHENTICATE,
                           (const char *)&ul,
                           sizeof(ul))
                == SOCKET_ERROR) {
                goto error_handler;
            }
        }
        if (connect(so, (SOCKADDR *)&btPtr->addresses.bt.remote, sizeof(SOCKADDR_BTH)) == 0) {
            /* Sockets should not be inherited by children */
            SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);

            if (IocpAttachDefaultPort((HANDLE)so) != NULL) {
                btPtr->base.state = IOCP_STATE_OPEN;
                btPtr->so         = so;
                /*
                 * Clear any error stored during -async operation prior to
                 * blocking connect
                 */
                btPtr->base.winError = ERROR_SUCCESS;
                return ERROR_SUCCESS;
            } else
                winError = GetLastError(); /* Not WSAGetLastError() */
        }
    }

error_handler:
    if (winError == 0)
        winError = WSAGetLastError(); /* Before calling closesocket */

    if (so != INVALID_SOCKET)
        closesocket(so);

    btPtr->base.state    = IOCP_STATE_CONNECT_FAILED;
    btPtr->base.winError = winError;
    return winError;
}

/*
 *------------------------------------------------------------------------
 *
 * BtClientPostConnect --
 *
 *    Posts a connect request to the IO completion port for a Bluetooth channel.
 *    The local and remote addresses are those specified in the btPtr.
 *
 *    The function does not check or modify the connection state. That is
 *    the caller's responsibility.
 *
 * Results:
 *    Returns 0 on success or a Windows error code.
 *
 * Side effects:
 *    A connection request buffer is posted to the completion port.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError
BtClientPostConnect(
    WinsockClient *btPtr  /* Channel pointer, may or may not be locked
                           * but caller has to ensure no interference */
    )
{
    static GUID     ConnectExGuid = WSAID_CONNECTEX;
    LPFN_CONNECTEX  fnConnectEx;
    IocpBuffer     *bufPtr;
    DWORD           nbytes;
    DWORD           winError;
    SOCKADDR_BTH    btAddress;

    /* Bind local address. Required for ConnectEx */
    /*
      Just memset because gcc does not know GUID_NULL and to lazy to define own
      btAddress.btAddr        = 0;
      btAddress.serviceClassId = GUID_NULL;
      btAddress.port           = 0;
    */
    memset(&btAddress, 0, sizeof(btAddress));
    btAddress.addressFamily = AF_BTH;
    if (bind(btPtr->so, (SOCKADDR *) &btAddress, sizeof(btAddress)) != 0)
        return WSAGetLastError();

    /*
     * Retrieve the ConnectEx function pointer. We do not cache
     * because strictly speaking it depends on the socket and
     * address family that map to a protocol driver.
     */
    if (WSAIoctl(btPtr->so, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &ConnectExGuid, sizeof(GUID),
                 &fnConnectEx,
                 sizeof(fnConnectEx),
                 &nbytes, NULL, NULL) != 0 ||
        fnConnectEx == NULL) {
        return WSAGetLastError();
    }

    if (IocpAttachDefaultPort((HANDLE) btPtr->so) == NULL) {
        return GetLastError(); /* NOT WSAGetLastError() ! */
    }

    bufPtr = IocpBufferNew(0, IOCP_BUFFER_OP_CONNECT, IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->chanPtr    = WinsockClientToIocpChannel(btPtr);
    btPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

    if (fnConnectEx(btPtr->so,
                    (SOCKADDR *)&btPtr->addresses.bt.remote,
                    sizeof(SOCKADDR_BTH),
                    NULL,
                    0,
                    &nbytes,
                    &bufPtr->u.wsaOverlap)
        == FALSE) {
        winError = WSAGetLastError();
        if (winError != WSA_IO_PENDING) {
            bufPtr->chanPtr = NULL;
            IOCP_ASSERT(btPtr->base.numRefs > 1); /* Since caller also holds ref */
            btPtr->base.numRefs -= 1;
            IocpBufferFree(bufPtr);
            return winError;
        }
    }

    return 0;
}



/*
 *------------------------------------------------------------------------
 *
 * BtClientInitiateConnection --
 *
 *    Initiates an asynchronous Bluetooth connection.
 *
 * Results:
 *    0 if the connection was initiated successfully, otherwise Windows error
 *    code which is also stored in tcpPtr->base.winError.
 *
 * Side effects:
 *    A connect request is initiated and a completion buffer posted. State
 *    is changed to CONNECTING on success or CONNECT_FAILED if it could
 *    even be initiated.
 *
 *------------------------------------------------------------------------
 */
static IocpWinError
BtClientInitiateConnection(
    WinsockClient *btPtr) /* Caller must ensure exclusivity either by locking
                           * or ensuring no other thread can access */
{
    DWORD  winError = 0; /* In case address lists are empty */
    SOCKET so = INVALID_SOCKET;

    IOCP_ASSERT(IocpIsBtClient(WinsockClientToIocpChannel(btPtr)));
    IOCP_ASSERT(btPtr->base.state == IOCP_STATE_INIT || btPtr->base.state == IOCP_STATE_CONNECT_RETRY);
    IOCP_ASSERT(btPtr->so == INVALID_SOCKET);

    btPtr->base.state = IOCP_STATE_CONNECTING;

    so = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (so != INVALID_SOCKET) {
        if (btPtr->flags & IOCP_WINSOCK_AUTHENTICATE) {
            ULONG ul = 1;
            if (setsockopt(so,
                           SOL_RFCOMM,
                           SO_BTH_AUTHENTICATE,
                           (const char *)&ul,
                           sizeof(ul))
                == SOCKET_ERROR) {
                goto error_handler;
            }
        }
        /* Sockets should not be inherited by children */
        SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);
        btPtr->so = so;
        winError  = BtClientPostConnect(btPtr);
        if (winError == ERROR_SUCCESS)
            return ERROR_SUCCESS;
    }

error_handler:
    winError = WSAGetLastError();
    if (so != INVALID_SOCKET)
        closesocket(so);
    btPtr->so = INVALID_SOCKET;
    btPtr->base.state = IOCP_STATE_CONNECT_FAILED;
    /* We report the stored error in preference to error in current call.  */
    if (btPtr->base.winError == 0)
        btPtr->base.winError = winError;
    return btPtr->base.winError;
}


/*
 *----------------------------------------------------------------------
 *
 * Iocp_OpenBTClient --
 *
 *	Opens a Bluetooth client socket and creates a channel around it.
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

static Tcl_Channel
Iocp_OpenBTClient(
    Tcl_Interp *interp, /* Interpreter */
    int port, /* Port 1-30 */
    BLUETOOTH_ADDRESS *btAddress, /* Address of device */
    int authenticate, /* Whether to authenticate connection */
    int async) /* Async connect or not */
{
    WinsockClient *btPtr;
    IocpWinError   winError;
    Tcl_Channel    channel;

    if (port < 1 || port > 30) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Invalid RFCOMM port number %d. Must be between 1 and 30.", port));
        return NULL;
    }

    btPtr = (WinsockClient *)IocpChannelNew(&btClientVtbl);
    if (btPtr == NULL) {
        if (interp != NULL) {
            Tcl_SetResult(interp, "couldn't allocate WinsockClient", TCL_STATIC);
        }
        goto fail;
    }
    btPtr->addresses.bt.remote.addressFamily = AF_BTH;
    btPtr->addresses.bt.remote.btAddr    = btAddress->ullLong;
    btPtr->addresses.bt.remote.port      = port;
    if (authenticate)
        btPtr->flags |= IOCP_WINSOCK_AUTHENTICATE;
    memset(&btPtr->addresses.bt.remote.serviceClassId,
           0,
           sizeof(btPtr->addresses.bt.remote.serviceClassId));
    IocpChannelLock(WinsockClientToIocpChannel(btPtr));
    if (async) {
        winError = BtClientInitiateConnection(btPtr);
        if (winError != ERROR_SUCCESS) {
            IocpSetInterpPosixErrorFromWin32(
                interp, winError, gSocketOpenErrorMessage);
            goto fail;
        }
    }
    else {
        winError = BtClientBlockingConnect(WinsockClientToIocpChannel(btPtr));
        if (winError != ERROR_SUCCESS) {
            IocpSetInterpPosixErrorFromWin32(interp, winError, gSocketOpenErrorMessage);
            goto fail;
        }
        winError = IocpChannelPostReads(WinsockClientToIocpChannel(btPtr));
        if (winError) {
            Iocp_ReportWindowsError(
                interp, winError, "couldn't post read on socket: ");
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
     */

    /* CREATE a Tcl channel that points back to this. */
    channel = IocpMakeTclChannel(interp,
                                 WinsockClientToIocpChannel(btPtr),
                                 IOCP_BT_NAME_PREFIX,
                                 (TCL_READABLE | TCL_WRITABLE));
    if (channel == NULL)
        goto fail;

    /*
     * At this point IocpMakeTclChannel has incremented the references
     * on btPtr, so we can drop the one this function is holding from
     * the allocation.
     */

    IocpChannelDrop(WinsockClientToIocpChannel(btPtr));
    btPtr = NULL; /* Ensure not accessed beyond here */

    if (IocpSetChannelDefaults(channel) == TCL_ERROR) {
        Tcl_Close(NULL, channel);
        return NULL;
    }

    return channel;

fail:
    /*
     * Failure exit. If btPtr is allocated, it must be locked when
     * jumping here.
     */
    if (btPtr)
        IocpChannelDrop(WinsockClientToIocpChannel(btPtr));

    return NULL;
}

/*
 *------------------------------------------------------------------------
 *
 * BT_SocketObjCmd --
 *
 *    Implements the socket command for Bluetooth. See 'socket' documentation as
 *    to description and options.
 *
 * Results:
 *    A standard Tcl result with the channel handle stored in interp result.
 *
 * Side effects:
 *    A new server or client socket is created.
 *
 *------------------------------------------------------------------------
 */
static IocpTclCode
BT_SocketObjCmd(ClientData  notUsed,   /* Not used. */
                Tcl_Interp *interp,    /* Current interpreter. */
                int         objc,      /* Number of arguments. */
                Tcl_Obj *CONST objv[]) /* Argument objects. */
{
    static const char *const socketOptions[]
        = {"-async", "-server", "-authenticate", NULL};
    enum socketOptions { SKT_ASYNC, SKT_SERVER, SKT_AUTHENTICATE };
    int               optionIndex, a, server = 0, async = 0, authenticate = 0;
    const char *      script = NULL;
    int               port;
    Tcl_Channel       chan = NULL;

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
        if (Tcl_GetIndexFromObj(interp, objv[a],
                                socketOptions,
                                "option",
                                TCL_EXACT,
                                &optionIndex)
            != TCL_OK) {
            return TCL_ERROR;
        }
        switch ((enum socketOptions)optionIndex) {
        case SKT_ASYNC:
            if (server == 1) {
                Tcl_SetObjResult(
                    interp,
                    Tcl_NewStringObj(
                        "cannot set -async option for server sockets", -1));
                return TCL_ERROR;
            }
            async = 1;
            break;
        case SKT_SERVER:
            if (async == 1) {
                Tcl_SetObjResult(
                    interp,
                    Tcl_NewStringObj(
                        "cannot set -async option for server sockets", -1));
                return TCL_ERROR;
            }
            server = 1;
            a++;
            if (a >= objc) {
                Tcl_SetObjResult(
                    interp,
                    Tcl_NewStringObj("no argument given for -server option",
                                     -1));
                return TCL_ERROR;
            }
            script = TclGetString(objv[a]);
            break;
        case SKT_AUTHENTICATE:
            authenticate = 1; // TBD - need to setsockopt for this
            break;
        default:
            Iocp_Panic("BT_SocketObjCmd: bad option index to SocketOptions");
        }
    }

    /* There should 1 (server) or 2 (client) arguments left over */
    if ((objc-a) != (server ? 1 : 2)) {
        /*
         * Hard code to match Tcl socket error. Can't use code below because
         * it uses internal Tcl structures.
         */
        Tcl_SetResult(interp,
                      "wrong # args: should be \"bt::socket ?-async? device service\""
                      "or "
                      "bt::socket -server command service\"",
                      TCL_STATIC);
        return TCL_ERROR;
    }

    /* Last arg is always port for both */
    if (Tcl_GetIntFromObj(interp, objv[objc-1], &port) != TCL_OK)
        return TCL_ERROR;

    if (server) {
        char *              copyScript;
        IocpAcceptCallback *acceptCallbackPtr;
        Tclh_SSizeT         len;
        len = Tclh_strlen(script)+1;
        copyScript = ckalloc(len);
        memcpy(copyScript, script, len);
        acceptCallbackPtr         = ckalloc(sizeof(*acceptCallbackPtr));
        acceptCallbackPtr->script = copyScript;
        acceptCallbackPtr->interp = interp;

#if 0
        chan = Iocp_OpenBTServer(
            interp, service, AcceptCallbackProc, acceptCallbackPtr);
#endif
        if (chan == NULL) {
            ckfree(copyScript);
            ckfree(acceptCallbackPtr);
            return TCL_ERROR;
        }

        /*
         * Register with the interpreter to let us know when the interpreter is
         * deleted (by having the callback set the interp field of the
         * acceptCallbackPtr's structure to NULL). This is to avoid trying to
         * eval the script in a deleted interpreter.
         */

        IocpRegisterAcceptCallbackCleanup(interp, acceptCallbackPtr);

        /*
         * Register a close callback. This callback will inform the interpreter
         * (if it still exists) that this channel does not need to be informed
         * when the interpreter is deleted.
         */

        Tcl_CreateCloseHandler(chan,
                               IocpUnregisterAcceptCallbackCleanupOnClose,
                               acceptCallbackPtr);

    } else {
        BLUETOOTH_ADDRESS btAddress;
        IOCP_ASSERT(a < (objc-1));
        if (ObjToBLUETOOTH_ADDRESS(interp, objv[a], &btAddress) != TCL_OK) {
            return TCL_ERROR;
        }

        chan = Iocp_OpenBTClient(interp, port, &btAddress, authenticate, async);
        if (chan == NULL) {
            return TCL_ERROR;
        }
    }

    Tcl_RegisterChannel(interp, chan);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(chan), -1));

    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * BT_LookupServiceBeginObjCmd --
 *
 *    Implements the Tcl command BT_LookupServiceBegin.
 *    TBD - make this a generic Winsock function.
 *
 * Results:
 *    TCL_OK    - Success. Returns search handle as interpreter result.
 *    TCL_ERROR - Error.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
int BT_LookupServiceBeginObjCmd (
    ClientData notUsed,
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *const objv[]) /* Argument objects. */
{
    HANDLE      lookupH;
    int         flags;
    Tclh_UUID   serviceUuid;
    Tcl_Obj    *objP;
    WSAQUERYSETW      qs;
    BLUETOOTH_ADDRESS btAddr;
    Tcl_DString ds, ds2;
#if TCL_MAJOR_VERSION > 8
    Tcl_Size utf8len;
    const char *utf8;
#endif
    int tclResult;

    /* LookupServiceBegin device_address service_guid ?service_name? */
    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "DEVICE SERVICEGUID ?SERVICENAME?");
        return TCL_ERROR;
    }

    /* Note btAddr only used to verify syntax */
    if (ObjToBLUETOOTH_ADDRESS(interp, objv[1], &btAddr) != TCL_OK ||
        UnwrapUuid(interp, objv[2], &serviceUuid) != TCL_OK) {
        return TCL_ERROR;
    }

    Tcl_DStringInit(&ds);
    Tcl_DStringInit(&ds2);
    ZeroMemory(&qs, sizeof(qs));

    if (objc == 4) {
#if TCL_MAJOR_VERSION < 9
        /* Note we extract the Unicode string last, so no shimmering issues. */
        qs.lpszQueryString = Tcl_GetUnicodeFromObj(objv[3], NULL);
#else
        utf8 = Tcl_GetStringFromObj(objv[3], &utf8len);
        qs.lpszQueryString = Tcl_UtfToChar16DString(utf8, utf8len, &ds);
#endif
    }

    /*
     * Only these fields need to be set for Bluetooth.
     * See https://docs.microsoft.com/en-us/windows/win32/bluetooth/bluetooth-and-wsalookupservicebegin-for-service-discovery
     */
    qs.dwSize              = sizeof(qs);
    qs.lpServiceClassId    = &serviceUuid;
    qs.dwNameSpace         = NS_BTH;
#if TCL_MAJOR_VERSION < 9
    qs.lpszContext         = Tcl_GetUnicodeFromObj(objv[1], NULL);
#else
    utf8 = Tcl_GetStringFromObj(objv[1], &utf8len);
    qs.lpszContext = Tcl_UtfToChar16DString(utf8, utf8len, &ds2);
#endif

    flags = LUP_FLUSHCACHE;
    if (WSALookupServiceBeginW(&qs, flags, &lookupH) != 0)
        tclResult = Iocp_ReportWindowsError(
            interp, WSAGetLastError(), "Bluetooth service search failed. ");
    else {
        tclResult =
            PointerRegister(interp, lookupH, "HWSALOOKUPSERVICE", &objP);
        if (tclResult != TCL_OK)
            CloseHandle(lookupH);
        else
            Tcl_SetObjResult(interp, objP);
    }
    Tcl_DStringFree(&ds);
    Tcl_DStringFree(&ds2);
    return tclResult;
}

static IocpTclCode
BT_LookupServiceEndObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HANDLE lookupH;
    int tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HWSALOOKUPSERVICE");
        return TCL_ERROR;
    }
    tclResult = PointerObjUnregister(interp, objv[1],
                                     &lookupH, "HWSALOOKUPSERVICE");
    if (tclResult != TCL_OK)
        return tclResult;

    if (WSALookupServiceEnd(lookupH) != 0)
        return Iocp_ReportWindowsError(
            interp,
            WSAGetLastError(),
            "Could not close Bluetooth service lookup handle: ");

    return TCL_OK;
}

/*
 *------------------------------------------------------------------------
 *
 * BT_LookupServiceNextObjCmd --
 *
 *    Implements the Tcl command BT_LookupServiceNext.
 *
 * Results:
 *    TCL_OK    - Success, returns the next service information element
 *                in the interp result.
 *    TCL_ERROR - Error. Interp result contains the error message.
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------
 */
static IocpTclCode
BT_LookupServiceNextObjCmd (
    ClientData notUsed,
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *const objv[]) /* Argument objects. */
{
    HANDLE        lookupH;
    WSAQUERYSETW *qsP;
    DWORD         qsLen;
    IocpWinError  winError;
    IocpTclCode   tclResult;
    DWORD         flags;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "HWSALOOKUPSERVICE FLAGS");
        return TCL_ERROR;
    }

    if (UnwrapPointer(interp, objv[1], &lookupH, "HWSALOOKUPSERVICE") != TCL_OK)
        return TCL_ERROR;

    if (Tcl_GetIntFromObj(interp, objv[2], (int *)&flags) != TCL_OK)
        return TCL_ERROR;

    /*
     * WSAQUERYSET is variable size. We will keep looping until
     * it is big enough.
     */
    qsLen = sizeof(*qsP) + 2000;
    qsP = ckalloc(qsLen);
    ZeroMemory(qsP, sizeof(*qsP)); /* Do not need to zero whole buffer */
    qsP->dwSize              = sizeof(*qsP);
    qsP->dwNameSpace         = NS_BTH;
    while (1) {
        winError = WSALookupServiceNextW(lookupH, flags, &qsLen, qsP);
        if (winError == 0)
            break;
        winError = WSAGetLastError();
        if (winError != WSAEFAULT)
            break;
        ckfree(qsP);
        qsP = ckalloc(qsLen);
        ZeroMemory(qsP, sizeof(*qsP)); /* Do not need to zero whole buffer */
        qsP->dwSize      = sizeof(*qsP);
        qsP->dwNameSpace = NS_BTH;
    }
    if (winError == 0) {
        Tcl_Obj *objs[12];
        int i = 0;

        if (flags & LUP_RETURN_NAME && qsP->lpszServiceInstanceName) {
            objs[i++] = STRING_LITERAL_OBJ("ServiceInstanceName");
            objs[i++] = IocpNewWincharObj(qsP->lpszServiceInstanceName, -1);
        }

        if (flags & LUP_RETURN_ADDR && qsP->lpcsaBuffer) {
            SOCKADDR_BTH *soAddr;
            objs[i++] = STRING_LITERAL_OBJ("RemoteAddress");
            soAddr  = (SOCKADDR_BTH *)qsP->lpcsaBuffer->RemoteAddr.lpSockaddr;
            objs[i++] = ObjFromSOCKADDR_BTH(soAddr);
            objs[i++] = STRING_LITERAL_OBJ("Protocol");
            objs[i++] = Tcl_NewIntObj(qsP->lpcsaBuffer->iProtocol);
        }

        if (flags & LUP_RETURN_COMMENT && qsP->lpszComment) {
            objs[i++] = STRING_LITERAL_OBJ("Comment");
            objs[i++] = IocpNewWincharObj(qsP->lpszComment, -1);
        }

        if (flags & LUP_RETURN_TYPE && qsP->lpServiceClassId) {
            objs[i++] = STRING_LITERAL_OBJ("ServiceClassId");
            objs[i++] = Tclh_WrapUuid(qsP->lpServiceClassId);
        }

        if (flags & LUP_RETURN_BLOB && qsP->lpBlob) {
            objs[i++] = STRING_LITERAL_OBJ("Blob");
            objs[i++]
                = Tcl_NewByteArrayObj(qsP->lpBlob->pBlobData, qsP->lpBlob->cbSize);
        }

        IOCP_ASSERT(i <= (sizeof(objs)/sizeof(objs[0])));
        Tcl_SetObjResult(interp, Tcl_NewListObj(i, objs));
        tclResult = TCL_OK;
    } else if (winError == WSA_E_NO_MORE) {
        /* Not an error, but no more values */
        tclResult = TCL_BREAK;
    }
    else {
         tclResult = Iocp_ReportWindowsError(
            interp,
            winError,
            "Could not retrieve Bluetooth service information: ");
    }
    ckfree(qsP);
    return tclResult;
}

/*
 * Initialization function to be called exactly once *per process* to
 * initialize the Bluetooth module.
 * Caller responsible to ensure it's called only once in a thread-safe manner.
 * It initializes the Bluetooth function table by dynamically loading
 * the bthprops dll and looking up the function addresses.
 *
 * Returns TCL_OK if the table was successfully initialized, TCL_ERROR on error.
 */
Iocp_DoOnceState btProcessInitFlag;
static IocpTclCode BT_InitAPI(ClientData clientdata)
{
    static HMODULE btDllH;

    /* gBtAPI statically 0's so no need to init here */

    /* Note since called exactly once, no need to check for already init'ed */
    btDllH = LoadLibraryA("Bthprops.cpl");
    if (btDllH == NULL) {
        gBtAPI.initialized = -1;
        return TCL_ERROR;
    }

    /*
     * Note that if the DLL is available we mark state as successfully
     * initialized even though some of the calls below might fail. This
     * is because we want at least those calls that are available to be
     * accessible. Conversely, all calls via the function table check to
     * make sure the pointer is not NULL before invocation.
     */
    gBtAPI.initialized = 1;

#define INITPROC(x) \
	gBtAPI.p ## x = (x ## Proc *)(void *)GetProcAddress(btDllH, #x)

    INITPROC(BluetoothFindFirstRadio);
    INITPROC(BluetoothFindNextRadio);
    INITPROC(BluetoothFindRadioClose);
    INITPROC(BluetoothGetRadioInfo);
    INITPROC(BluetoothFindFirstDevice);
    INITPROC(BluetoothFindNextDevice);
    INITPROC(BluetoothFindDeviceClose);
    INITPROC(BluetoothGetDeviceInfo);
    INITPROC(BluetoothRemoveDevice);
    INITPROC(BluetoothEnumerateInstalledServices);
    INITPROC(BluetoothEnableDiscovery);
    INITPROC(BluetoothIsDiscoverable);
    INITPROC(BluetoothEnableIncomingConnections);
    INITPROC(BluetoothIsConnectable);
#undef INITPROC

    return TCL_OK;
}


/*
 *------------------------------------------------------------------------
 *
 * BT_ModuleInitialize --
 *
 *    Initializes the Bluetooth module.
 *
 * Results:
 *    TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *    Creates the Bluetooth related Tcl commands.
 *
 *------------------------------------------------------------------------
 */
IocpTclCode
BT_ModuleInitialize(Tcl_Interp *interp)
{
    if (Iocp_DoOnce(&btProcessInitFlag, BT_InitAPI, NULL) != TCL_OK
        || gBtAPI.initialized < 0) {
        Tcl_SetResult( interp, "Unable to initialize Bluetooth API.", TCL_STATIC);
        return TCL_ERROR;
    }

    Tcl_CreateObjCommand(
        interp, "iocp::bt::CloseHandle", BT_CloseHandleObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::FindFirstRadio", BT_FindFirstRadioObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::FindNextRadio", BT_FindNextRadioObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::FindFirstRadioClose",
                         BT_FindFirstRadioCloseObjCmd,
                         0L,
                         0L);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::GetRadioInfo", BT_GetRadioInfoObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::FindFirstDevice", BT_FindFirstDeviceObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::FindFirstDeviceClose",
                         BT_FindFirstDeviceCloseObjCmd,
                         0L,
                         0L);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::FindNextDevice", BT_FindNextDeviceObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::GetDeviceInfo", BT_GetDeviceInfoObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::EnableDiscovery",
                         BT_ConfigureRadioObjCmd,
                         (ClientData)BT_ENABLE_DISCOVERY,
                         0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::EnableIncoming",
                         BT_ConfigureRadioObjCmd,
                         (ClientData)BT_ENABLE_INCOMING,
                         0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::IsDiscoverable",
                         BT_RadioStatusObjCmd,
                         (ClientData)BT_STATUS_DISCOVERY,
                         0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::IsConnectable",
                         BT_RadioStatusObjCmd,
                         (ClientData)BT_STATUS_INCOMING,
                         0L);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::EnumerateInstalledServices",
                         BT_EnumerateInstalledServicesObjCmd,
                         NULL,
                         NULL);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::RemoveDevice", BT_RemoveDeviceObjCmd, NULL, NULL);
    Tcl_CreateObjCommand(
        interp, "iocp::bt::socket", BT_SocketObjCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::LookupServiceBegin",
                         BT_LookupServiceBeginObjCmd,
                         NULL,
                         NULL);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::LookupServiceEnd",
                         BT_LookupServiceEndObjCmd,
                         NULL,
                         NULL);
    Tcl_CreateObjCommand(interp,
                         "iocp::bt::LookupServiceNext",
                         BT_LookupServiceNextObjCmd,
                         NULL,
                         NULL);

#ifdef IOCP_DEBUG
    Tcl_CreateObjCommand(interp, "iocp::bt::FormatAddress", BT_FormatAddressObjCmd, 0L, 0L);
#endif
    return TCL_OK;
}
