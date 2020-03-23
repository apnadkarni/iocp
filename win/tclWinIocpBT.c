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


static Tcl_Obj *ObjFromSYSTEMTIME(const SYSTEMTIME *timeP);
static Tcl_Obj *ObjFromBLUETOOTH_ADDRESS (const BLUETOOTH_ADDRESS *addrPtr);
static IocpTclCode ObjToBLUETOOTH_ADDRESS(
    Tcl_Interp *interp,
    Tcl_Obj *objP,
    BLUETOOTH_ADDRESS *btAddrPtr);
static Tcl_Obj *ObjFromBLUETOOTH_RADIO_INFO (const BLUETOOTH_RADIO_INFO *infoPtr);
static Tcl_Obj *ObjFromBLUETOOTH_DEVICE_INFO (const BLUETOOTH_DEVICE_INFO *infoPtr);


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

/* Outputs a string using Windows OutputDebugString */
static IocpTclCode
BT_SelectObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    /* TBD */
    BLUETOOTH_SELECT_DEVICE_PARAMS btparams;
    memset(&btparams, 0, sizeof(btparams));
    btparams.dwSize = sizeof(btparams);
    if (BluetoothSelectDevices(&btparams)) {
        BluetoothSelectDevicesFree(&btparams);
        return TCL_OK;
    }
    else {
        Iocp_ReportLastWindowsError(interp, "Could not select Bluetooth device: ");
        return TCL_ERROR;
    }
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
    btParams.dwSize = sizeof(btParams);
    findHandle = BluetoothFindFirstRadio(&btParams, &radioHandle);

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
        BluetoothFindRadioClose(findHandle);
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
    tclResult = PointerObjUnregister(interp, objv[1],
                                     &findHandle, "HBLUETOOTH_RADIO_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (BluetoothFindRadioClose(findHandle) != TRUE)
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
    tclResult = PointerObjVerify(interp, objv[1], &findHandle, "HBLUETOOTH_RADIO_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (BluetoothFindNextRadio(findHandle, &radioHandle) != TRUE) {
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
    tclResult = PointerObjVerify(interp, objv[1], &radioHandle, "HRADIO");
    if (tclResult != TCL_OK)
        return tclResult;

    info.dwSize = sizeof(info);
    if (BluetoothGetRadioInfo(radioHandle,&info) != ERROR_SUCCESS)
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
        "-timeout", "-radio", NULL
    };
    enum Opts {
        AUTHENTICATED, REMEMBERED, UNKNOWN, CONNECTED, INQUIRE, TIMEOUT, RADIO
    };
    int i, opt, timeout, tclResult;
    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    BLUETOOTH_DEVICE_INFO          info;
    HBLUETOOTH_DEVICE_FIND         findHandle;
    Tcl_Obj *objs[2];

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
    findHandle = BluetoothFindFirstDevice(&params, &info);
    /* TBD - what is returned if there are no bluetooth devices */
    if (findHandle == NULL)
        return Iocp_ReportLastWindowsError(interp, "Bluetooth device search failed: ");

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

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_DEVICE_FIND");
        return TCL_ERROR;
    }
    tclResult = PointerObjUnregister(interp, objv[1], &findHandle, "HBLUETOOTH_DEVICE_FIND");
    if (tclResult == TCL_OK) {
        if (BluetoothFindDeviceClose(findHandle) != TRUE)
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

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_DEVICE_FIND");
        return TCL_ERROR;
    }
    tclResult = PointerObjVerify(interp, objv[1], &findHandle, "HBLUETOOTH_DEVICE_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    info.dwSize = sizeof(info);
    if (BluetoothFindNextDevice(findHandle, &info) != TRUE) {
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
    tclResult = PointerObjVerify(interp, objv[1], &radioHandle, "HRADIO");
    if (tclResult != TCL_OK)
        return tclResult;

    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[2], &info.Address);
    if (tclResult != TCL_OK)
        return tclResult;

    info.dwSize = sizeof(info);
    if (BluetoothGetDeviceInfo(radioHandle,&info) != ERROR_SUCCESS)
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
    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[1], &btAddress);
    if (tclResult != TCL_OK)
        return tclResult;

    winError = BluetoothRemoveDevice(&btAddress);
    if (winError != ERROR_SUCCESS) {
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
    findHandle = BluetoothFindFirstDevice(&params, &info);
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
    } while (BluetoothFindNextDevice(findHandle, &info) == TRUE);

    winError = GetLastError();
    if (winError != ERROR_NO_MORE_ITEMS) {
        Tcl_DecrRefCount(resultObj);
        tclResult = Iocp_ReportWindowsError(interp, winError, NULL);
    } else {
        Tcl_SetObjResult(interp, resultObj);
        tclResult = TCL_OK;
    }
    BluetoothFindDeviceClose(findHandle);
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
        changed = BluetoothEnableDiscovery(radioHandle, enable);
    } else if (command == BT_ENABLE_INCOMING) {
        changed = BluetoothEnableIncomingConnections(radioHandle, enable);
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

    radioHandle = NULL;
    if (objc > 1) {
        tclResult = PointerObjVerify(interp, objv[2], &radioHandle, "HRADIO");
        if (tclResult != TCL_OK)
            return tclResult;
    }

    if (command == BT_STATUS_DISCOVERY) {
        status = BluetoothIsDiscoverable(radioHandle);
    } else if (command == BT_STATUS_INCOMING) {
        status = BluetoothIsConnectable(radioHandle);
    } else {
        Tcl_Panic("Unexpected clientData parameter value %d", command);
        status = 0;            /* NOTREACHED - Keep compiler happy */
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status));
    return TCL_OK;

}

#ifdef OBSOLETE
Tcl_Obj *WrapUuid(UUID *guid) {
  char guid_string[37];
  snprintf(
      guid_string, sizeof(guid_string) / sizeof(guid_string[0]),
      "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      guid->Data1, guid->Data2, guid->Data3,
      guid->Data4[0], guid->Data4[1], guid->Data4[2],
      guid->Data4[3], guid->Data4[4], guid->Data4[5],
      guid->Data4[6], guid->Data4[7]);
  // remove when VC++7.1 is no longer supported
  guid_string[sizeof(guid_string) / sizeof(guid_string[0]) - 1] = L'\0';
  return Tcl_NewStringObj(guid_string, -1);
}
#endif

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

    info.dwSize = sizeof(info);
    tclResult = ObjToBLUETOOTH_ADDRESS(interp, objv[1], &info.Address);
    if (tclResult != TCL_OK)
        return tclResult;

    if (objc > 2) {
        tclResult = PointerObjVerify(interp, objv[2], &radioH, "HRADIO");
        if (tclResult != TCL_OK)
            return tclResult;
    }

    serviceP = services;
    count    = sizeof(services)/sizeof(services[0]);
    /* Loop while error is lack of buffer space */
    while ((winError = BluetoothEnumerateInstalledServices(
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
    objs[3] = Tcl_NewUnicodeObj(infoPtr->szName, -1);
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
    objs[3] = Tcl_NewUnicodeObj(infoPtr->szName, -1);
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
     * Use temp storage for two reasons:
     *  - so as to not modify btAddrPtr[] on error
     */
    unsigned char bytes[6];
    unsigned char trailer; /* Used to ensure no trailing characters */
    if (sscanf_s(Tcl_GetString(objP),
               "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx%c",
                 &bytes[5], &bytes[4], &bytes[3], &bytes[2], &bytes[1], &bytes[0], &trailer, 1) == 6) {
        int i;
        for (i = 0; i < 6; ++i)
            btAddrPtr->rgBytes[i] = bytes[i];
        return TCL_OK;
    } else {
        Tcl_AppendResult(interp, "Invalid Bluetooth address ", Tcl_GetString(objP), ".", NULL);
        return TCL_ERROR;
    }
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
    DWORD          winError;
    SOCKET         so = INVALID_SOCKET;

    IOCP_ASSERT(IocpIsBtClient(chanPtr));

    /* Note socket call, unlike WSASocket is overlapped by default */
    so = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (so != INVALID_SOCKET) {
        if (connect(so, (SOCKADDR *)&btPtr->addresses.bt.remote, sizeof(SOCKADDR_BTH)) == 0) {
            /* Sockets should not be inherited by children */
            SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);

            if (CreateIoCompletionPort((HANDLE)so,
                                       iocpModuleState.completion_port,
                                       0, /* Completion key - unused */
                                       0)
                != NULL) {
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
    btAddress.addressFamily = AF_BTH;
    btAddress.btAddr        = 0;
    btAddress.serviceClassId = GUID_NULL;
    btAddress.port           = 0;

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

    if (CreateIoCompletionPort((HANDLE) btPtr->so,
                               iocpModuleState.completion_port,
                               0, /* Completion key - unused */
                               0) == NULL) {
        return GetLastError(); /* NOT WSAGetLastError() ! */
    }

    bufPtr = IocpBufferNew(0, IOCP_BUFFER_OP_CONNECT, IOCP_BUFFER_F_WINSOCK);
    if (bufPtr == NULL)
        return WSAENOBUFS;

    bufPtr->chanPtr    = WinsockClientToIocpChannel(btPtr);
    btPtr->base.numRefs += 1; /* Reversed when buffer is unlinked from channel */

    if (fnConnectEx(btPtr->so, btPtr->addresses.inet.remote->ai_addr,
                    (int) btPtr->addresses.inet.remote->ai_addrlen,
                    NULL, 0, &nbytes, &bufPtr->u.wsaOverlap) == FALSE) {
        winError = WSAGetLastError();
        bufPtr->chanPtr = NULL;
        IOCP_ASSERT(btPtr->base.numRefs > 1); /* Since caller also holds ref */
        btPtr->base.numRefs -= 1;
        if (winError != WSA_IO_PENDING) {
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
    WinsockClient *btPtr)  /* Caller must ensure exclusivity either by locking
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
        /* Sockets should not be inherited by children */
        SetHandleInformation((HANDLE)so, HANDLE_FLAG_INHERIT, 0);
        btPtr->so = so;
        winError  = BtClientPostConnect(btPtr);
        if (winError == ERROR_SUCCESS)
            return ERROR_SUCCESS;

        /* Oomph, failed. */
        closesocket(so);
        btPtr->so = INVALID_SOCKET;
    } else {
        winError = WSAGetLastError();
        /* No joy. Keep trying. */
    }

    /*
     * Failed. We report the stored error in preference to error in current call.
     */
    btPtr->base.state = IOCP_STATE_CONNECT_FAILED;
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

Tcl_Channel
Iocp_OpenBTClient(
    Tcl_Interp *interp, /* Interpreter */
    GUID *serviceGuidP, /* BT service guid */
    BLUETOOTH_ADDRESS *btAddress, /* Address of device */
    int async) /* Async connect or not */
{
    WinsockClient *btPtr;
    IocpWinError   winError;
    Tcl_Channel    channel;

    btPtr = (WinsockClient *)IocpChannelNew(&btClientVtbl);
    if (btPtr == NULL) {
        if (interp != NULL) {
            Tcl_SetResult(interp, "couldn't allocate WinsockClient", TCL_STATIC);
        }
        goto fail;
    }
    btPtr->addresses.bt.remote.addressFamily = AF_BTH;
    btPtr->addresses.bt.remote.btAddr    = btAddress->ullLong;
    btPtr->addresses.bt.remote.port      = 0;
    btPtr->addresses.bt.remote.serviceClassId = *serviceGuidP;

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
        winError = BtClientBlockingConnect( WinsockClientToIocpChannel(btPtr) );
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
    GUID              service;
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
            authenticate = 1;
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

    /* Last arg is always service for both */
    if (UnwrapUuid(interp, objv[objc-1], &service) != TCL_OK) 
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

        chan = Iocp_OpenBTClient(interp, &service, &btAddress, async);
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
    int         len;
    Tclh_UUID   serviceUuid;
    LPWSTR      serviceName = NULL;
    Tcl_Obj    *objP;
    WSAQUERYSETW      qs;
    BLUETOOTH_ADDRESS btAddr;

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

    ZeroMemory(&qs, sizeof(qs));

    /* Note we extract the Unicode string last, so no shimmering issues. */
    if (objc == 4)
        qs.lpszQueryString = Tcl_GetUnicodeFromObj(objv[3], &len);

    /*
     * Only these fields need to be set for Bluetooth.
     * See https://docs.microsoft.com/en-us/windows/win32/bluetooth/bluetooth-and-wsalookupservicebegin-for-service-discovery
     */
    qs.dwSize              = sizeof(qs);
    qs.lpServiceClassId    = &serviceUuid;
    qs.dwNameSpace         = NS_BTH;
    qs.lpszContext         = Tcl_GetUnicodeFromObj(objv[1], &len);

    flags = LUP_FLUSHCACHE | LUP_RETURN_ADDR | LUP_RETURN_NAME | LUP_RETURN_COMMENT;
    if (WSALookupServiceBeginW(&qs, flags, &lookupH) != 0)
        return Iocp_ReportWindowsError(
            interp, WSAGetLastError(), "Bluetooth service search failed.");
    if (PointerRegister(interp, lookupH, "HWSALOOKUPSERVICE", &objP) != TCL_OK) {
        CloseHandle(lookupH);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, objP);
    return TCL_OK;
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
int BT_LookupServiceNextObjCmd (
    ClientData notUsed,
    Tcl_Interp *interp,    /* Current interpreter. */
    int objc,              /* Number of arguments. */
    Tcl_Obj *const objv[]) /* Argument objects. */
{
    HANDLE        lookupH;
    WSAQUERYSETW *qsP;
    int           qsLen;
    IocpWinError  winError;
    IocpTclCode   tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HWSALOOKUPSERVICE");
        return TCL_ERROR;
    }

    if (UnwrapPointer(interp, objv[1], &lookupH, "HWSALOOKUPSERVICE") != TCL_OK)
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
        /*
         * As per Bluetooth-specific SDK docs https://docs.microsoft.com/en-us/windows/win32/bluetooth/bluetooth-and-wsalookupservicebegin-for-service-discovery
         *     LUP_RETURN_TYPE  
         * is only relevant for WSALookupServiceBegin, not Next.
         */
        DWORD flags = 
            LUP_RETURN_ADDR | /* Address in lpcsaBuffer */
            LUP_RETURN_NAME ; /* Name in lpszServiceInstanceName */
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
        Tcl_Obj *objs[4];
        objs[0] = STRING_LITERAL_OBJ("ServiceInstanceName");
        if (qsP->lpszServiceInstanceName)
            objs[1] = Tcl_NewUnicodeObj(qsP->lpszServiceInstanceName, -1);
        else
            objs[1] = Tcl_NewObj();
        objs[2] = STRING_LITERAL_OBJ("Addresses");
        if (qsP->lpcsaBuffer) {
            Tcl_Obj *     addrObjs[4];
            SOCKADDR_BTH *soAddr;
            addrObjs[0] = STRING_LITERAL_OBJ("Remote");
            soAddr  = (SOCKADDR_BTH *)qsP->lpcsaBuffer->RemoteAddr.lpSockaddr;
            addrObjs[1] = ObjFromSOCKADDR_BTH(soAddr);
            addrObjs[2] = STRING_LITERAL_OBJ("Local");
            soAddr  = (SOCKADDR_BTH *)qsP->lpcsaBuffer->LocalAddr.lpSockaddr;
            addrObjs[3] = ObjFromSOCKADDR_BTH(soAddr);
            objs[3]     = Tcl_NewListObj(4, addrObjs);
        }
        else
            objs[3] = Tcl_NewObj();
        Tcl_SetObjResult(interp, Tcl_NewListObj(4, objs));
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
                         "iocp::bt::EnableIncomingConnections",
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

