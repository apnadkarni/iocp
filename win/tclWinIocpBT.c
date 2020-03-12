#include <tclWinIocp.h>

#include <Bthsdpdef.h>
#include <BluetoothAPIs.h>

#include <stdio.h> /* For snprintf_s */

#define STRING_LITERAL_OBJ(s) Tcl_NewStringObj(s, sizeof(s)-1)

static Tcl_Obj *ObjFromSYSTEMTIME(const SYSTEMTIME *timeP);
static Tcl_Obj *ObjFromBLUETOOTH_ADDRESS (const BLUETOOTH_ADDRESS *addrPtr);
static Tcl_Obj *ObjFromBLUETOOTH_RADIO_INFO (const BLUETOOTH_RADIO_INFO *infoPtr);
static Tcl_Obj *ObjFromBLUETOOTH_DEVICE_INFO (const BLUETOOTH_DEVICE_INFO *infoPtr);

IocpTclCode
BT_CloseHandleObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HANDLE handle;
    int tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HRADIO");
        return TCL_ERROR;
    }

    tclResult = ObjToOpaqueAny(interp, objv[1], &handle, "HRADIO", NULL);
    if (tclResult != TCL_OK)
        return tclResult;

    if (CloseHandle(handle) != TRUE)
        return Iocp_ReportLastWindowsError(interp, "Could not close Bluetooth radio handle: ");

    return TCL_OK;
}

/* Outputs a string using Windows OutputDebugString */
IocpTclCode
BT_SelectObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
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

IocpTclCode
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

    objs[0] = ObjFromOpaque(findHandle, "HBLUETOOTH_RADIO_FIND");
    objs[1] = ObjFromOpaque(radioHandle, "HRADIO");
    Tcl_SetObjResult(interp, Tcl_NewListObj(2,objs));

    return TCL_OK;
}

IocpTclCode
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
    tclResult = ObjToOpaque(interp, objv[1], &findHandle, "HBLUETOOTH_RADIO_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (BluetoothFindRadioClose(findHandle) != TRUE)
        return Iocp_ReportLastWindowsError(interp, "Could not close Bluetooth radio search handle: ");

    return TCL_OK;
}

IocpTclCode
BT_FindNextRadioObjCmd (
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    HBLUETOOTH_RADIO_FIND findHandle;
    HANDLE radioHandle;
    int tclResult;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "HBLUETOOTH_RADIO_FIND");
        return TCL_ERROR;
    }
    tclResult = ObjToOpaque(interp, objv[1], &findHandle, "HBLUETOOTH_RADIO_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (BluetoothFindNextRadio(findHandle, &radioHandle) != TRUE) {
        DWORD winError = GetLastError();
        if (winError == ERROR_NO_MORE_ITEMS)
            return TCL_BREAK;
        else
            return Iocp_ReportWindowsError(interp, winError, "Error fetching next radio: ");
    }

    Tcl_SetObjResult(interp, ObjFromOpaque(radioHandle, "HRADIO"));
    return TCL_OK;
}

IocpTclCode
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
    tclResult = ObjToOpaque(interp, objv[1], &radioHandle, "HRADIO");
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
int BT_FindFirstDeviceObjCmd(
    ClientData notUsed,			/* Not used. */
    Tcl_Interp *interp,			/* Current interpreter. */
    int objc,				/* Number of arguments. */
    Tcl_Obj *CONST objv[])		/* Argument objects. */
{
    static const char *const opts[] = {
        "-authenticated", "-remembered", "-unknown", "-connected", "-inquire",
        "-timeout", "-radio", NULL
    };
    enum Opts {
        AUTHENTICATED, REMEMBERED, UNKNOWN, CONNECTED, INQUIRE, TIMEOUT, RADIO
    };
    int i, opt, timeout;
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
        case REMEMBERED: params.fReturnRemembered = 1; break;
        case UNKNOWN: params.fReturnUnknown = 1; break;
        case CONNECTED: params.fReturnConnected = 1; break;
        case INQUIRE: params.fIssueInquiry = 1; break;
        case TIMEOUT:
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
            if (ObjToOpaque(interp, objv[i], &params.hRadio, "HRADIO") != TCL_OK)
                return TCL_ERROR;
        }
    }

    info.dwSize = sizeof(info);
    findHandle = BluetoothFindFirstDevice(&params, &info);
    /* TBD - what is returned if there are no bluetooth devices */
    if (findHandle == NULL)
        return Iocp_ReportLastWindowsError(interp, "Bluetooth device search failed: ");

    objs[0] = ObjFromOpaque(findHandle, "HBLUETOOTH_DEVICE_FIND");
    objs[1] = ObjFromBLUETOOTH_DEVICE_INFO(&info);
    Tcl_SetObjResult(interp, Tcl_NewListObj(2, objs));
    return TCL_OK;
}

IocpTclCode
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
    tclResult = ObjToOpaque(interp, objv[1], &findHandle, "HBLUETOOTH_DEVICE_FIND");
    if (tclResult != TCL_OK)
        return tclResult;

    if (BluetoothFindDeviceClose(findHandle) != TRUE)
        return Iocp_ReportLastWindowsError(interp, "Could not close Bluetooth device search handle: ");

    return TCL_OK;
}

IocpTclCode
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
    tclResult = ObjToOpaque(interp, objv[1], &findHandle, "HBLUETOOTH_DEVICE_FIND");
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
