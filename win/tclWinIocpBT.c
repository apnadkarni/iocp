#include <tclWinIocp.h>

#include <Bthsdpdef.h>
#include <BluetoothAPIs.h>

#include <stdio.h> /* For snprintf_s */

#define STRING_LITERAL_OBJ(s) Tcl_NewStringObj(s, sizeof(s)-1)

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
        Tcl_WrongNumArgs(interp, 1, objv, "HRADIO");
        return TCL_ERROR;
    }

    tclResult = PointerObjUnregisterAnyOf(interp, objv[1], &handle, "HRADIO", NULL);
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

    if (findHandle == NULL)
        return Iocp_ReportLastWindowsError(interp, "Could not locate Bluetooth radios: ");

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

static Tcl_Obj *ObjFromBLUETOOTH_ADDRESS (const BLUETOOTH_ADDRESS *addrPtr)
{
    Tcl_Obj *objP;
    char *bytes = ckalloc(18);
    /* NOTE: *p must be unsigned for the snprintf to format correctly */
    const unsigned char *p = addrPtr->rgBytes;

    /*
     * Addresses are little endian. Hence order of storage. This matches
     * what's displayed via Device Manager in Windows.
     */
    _snprintf_s(bytes, 18, _TRUNCATE, "%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
                p[5], p[4], p[3], p[2], p[1], p[0]);

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
IocpTclCode BT_ModuleInitialize (Tcl_Interp *interp)
{
    Tcl_CreateObjCommand(interp, "iocp::bt::CloseHandle", BT_CloseHandleObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::FindFirstRadio", BT_FindFirstRadioObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::FindNextRadio", BT_FindNextRadioObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::FindFirstRadioClose", BT_FindFirstRadioCloseObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::GetRadioInfo", BT_GetRadioInfoObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::FindFirstDevice", BT_FindFirstDeviceObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::FindFirstDeviceClose", BT_FindFirstDeviceCloseObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::FindNextDevice", BT_FindNextDeviceObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::GetDeviceInfo", BT_GetDeviceInfoObjCmd, 0L, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::EnableDiscovery", BT_ConfigureRadioObjCmd, (ClientData) BT_ENABLE_DISCOVERY, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::EnableIncomingConnections", BT_ConfigureRadioObjCmd, (ClientData) BT_ENABLE_INCOMING, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::IsDiscoverable", BT_RadioStatusObjCmd, (ClientData) BT_STATUS_DISCOVERY, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::IsConnectable", BT_RadioStatusObjCmd, (ClientData) BT_STATUS_INCOMING, 0L);
    Tcl_CreateObjCommand(interp, "iocp::bt::EnumerateInstalledServices", BT_EnumerateInstalledServicesObjCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "iocp::bt::RemoveDevice", BT_RemoveDeviceObjCmd, NULL, NULL);

#ifdef IOCP_DEBUG
    Tcl_CreateObjCommand(interp, "iocp::bt::FormatAddress", BT_FormatAddressObjCmd, 0L, 0L);
#endif
    return TCL_OK;
}

