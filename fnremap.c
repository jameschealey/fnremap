/*
 * FnRemap - Apple Magic Keyboard Fn-to-Ctrl Remapper
 *
 * User-mode application that reads raw USB reports via WinUSB,
 * remaps the Apple Fn key to Left Control, and injects keyboard
 * events via SendInput.
 *
 * Requires WinUSB as the function driver for MI_01 (see fnremap.inf).
 *
 * Compile:
 *   cl /nologo fnremap.c /link winusb.lib setupapi.lib user32.lib wtsapi32.lib userenv.lib advapi32.lib
 */

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

/* Device interface GUID - must match fnremap.inf */
static const GUID DEVICE_GUID =
    {0xa1c1ef05,0x7b20,0x4c2f,{0x8b,0x5a,0x2e,0x2c,0x4d,0x1e,0x3f,0x06}};

/* Keyboard report layout (10 bytes from USB interrupt endpoint):
 *   byte 0: report ID (1)
 *   byte 1: modifier bitmap (standard 8 modifiers)
 *   byte 2: reserved
 *   bytes 3-8: key codes (up to 6 simultaneous keys)
 *   byte 9: Apple vendor bits (Fn, Eject, etc.)
 *
 * Consumer report layout (2 bytes):
 *   byte 0: report ID (various)
 *   byte 1: button bitmap
 */

#define KBD_REPORT_ID    1
#define KBD_REPORT_SIZE  10
#define MOD_OFFSET       1
#define KEY_OFFSET       3
#define KEY_COUNT        6
#define APPLE_OFFSET     9

/* Modifier bits in byte 1 */
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

/* We don't know the exact Fn bit yet - we'll detect it */
static BYTE g_fnBit = 0;

/* Modifier VKeys (indexed by bit position 0-7) */
static const WORD MOD_VKEYS[8] = {
    VK_LCONTROL, VK_LSHIFT, VK_LMENU, VK_LWIN,
    VK_RCONTROL, VK_RSHIFT, VK_RMENU, VK_RWIN
};

/* HID usage (page 0x07) to Windows virtual key code.
 * Entries with 0 = no mapping. */
static WORD HidToVKey(BYTE usage)
{
    if (usage >= 0x04 && usage <= 0x1D)
        return 'A' + (usage - 0x04);
    if (usage >= 0x1E && usage <= 0x26)
        return '1' + (usage - 0x1E);
    if (usage == 0x27) return '0';
    if (usage == 0x28) return VK_RETURN;
    if (usage == 0x29) return VK_ESCAPE;
    if (usage == 0x2A) return VK_BACK;
    if (usage == 0x2B) return VK_TAB;
    if (usage == 0x2C) return VK_SPACE;
    if (usage == 0x2D) return VK_OEM_MINUS;
    if (usage == 0x2E) return VK_OEM_PLUS;
    if (usage == 0x2F) return VK_OEM_4;      /* [ */
    if (usage == 0x30) return VK_OEM_6;      /* ] */
    if (usage == 0x31) return VK_OEM_5;      /* \ */
    if (usage == 0x33) return VK_OEM_1;      /* ; */
    if (usage == 0x34) return VK_OEM_7;      /* ' */
    if (usage == 0x35) return VK_OEM_3;      /* ` */
    if (usage == 0x36) return VK_OEM_COMMA;
    if (usage == 0x37) return VK_OEM_PERIOD;
    if (usage == 0x38) return VK_OEM_2;      /* / */
    if (usage == 0x39) return VK_CAPITAL;
    if (usage >= 0x3A && usage <= 0x45)
        return VK_F1 + (usage - 0x3A);
    if (usage == 0x46) return VK_SNAPSHOT;
    if (usage == 0x47) return VK_SCROLL;
    if (usage == 0x48) return VK_PAUSE;
    if (usage == 0x49) return VK_INSERT;
    if (usage == 0x4A) return VK_HOME;
    if (usage == 0x4B) return VK_PRIOR;
    if (usage == 0x4C) return VK_DELETE;
    if (usage == 0x4D) return VK_END;
    if (usage == 0x4E) return VK_NEXT;
    if (usage == 0x4F) return VK_RIGHT;
    if (usage == 0x50) return VK_LEFT;
    if (usage == 0x51) return VK_DOWN;
    if (usage == 0x52) return VK_UP;
    if (usage == 0x53) return VK_NUMLOCK;
    /* Numpad */
    if (usage == 0x54) return VK_DIVIDE;
    if (usage == 0x55) return VK_MULTIPLY;
    if (usage == 0x56) return VK_SUBTRACT;
    if (usage == 0x57) return VK_ADD;
    if (usage == 0x58) return VK_RETURN;     /* numpad enter */
    if (usage >= 0x59 && usage <= 0x61)
        return VK_NUMPAD1 + (usage - 0x59);
    if (usage == 0x62) return VK_NUMPAD0;
    if (usage == 0x63) return VK_DECIMAL;
    if (usage == 0x64) return VK_OEM_102;    /* non-US \ */
    if (usage == 0x65) return VK_APPS;       /* context menu */
    return 0;
}

static BOOL IsExtendedKey(WORD vk)
{
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        case VK_SNAPSHOT: case VK_APPS:
        case VK_DIVIDE:
            return TRUE;
    }
    return FALSE;
}

static BOOL g_verbose = FALSE;
static BOOL g_serviceMode = FALSE;

static void SwitchToInputDesktop(void)
{
    static HDESK s_hDesk = NULL;
    static ULONGLONG s_lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastCheck < 2000) return;
    s_lastCheck = now;

    HDESK hNew = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hNew) return;
    if (SetThreadDesktop(hNew)) {
        if (s_hDesk) CloseDesktop(s_hDesk);
        s_hDesk = hNew;
    } else {
        CloseDesktop(hNew);
    }
}

static void SendKey(WORD vk, BOOL down)
{
    INPUT inp = {0};
    UINT ret;
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    inp.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    inp.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (IsExtendedKey(vk))
        inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    ret = SendInput(1, &inp, sizeof(INPUT));
    if (g_verbose)
        printf("  SendKey: vk=0x%02X scan=0x%02X %s ret=%u\n",
               vk, inp.ki.wScan, down ? "DOWN" : "UP", ret);
}

/* Track which keys are currently held (by HID usage code) */
static BYTE g_heldKeys[KEY_COUNT] = {0};
static BYTE g_heldModifiers = 0;
static BOOL g_fnHeld = FALSE;

static void ProcessKeyboardReport(const BYTE *report)
{
    BYTE modifiers = report[MOD_OFFSET];
    BYTE appleByte = report[APPLE_OFFSET];
    BOOL fnNow = (g_fnBit != 0) && (appleByte & g_fnBit);

    /* Remap physical Left Ctrl → Left GUI (Windows key) */
    if (modifiers & MOD_LCTRL) {
        modifiers &= ~MOD_LCTRL;
        modifiers |= MOD_LGUI;
    }

    /* Fn → Left Ctrl */
    if (fnNow)
        modifiers |= MOD_LCTRL;

    /* Process modifier changes */
    BYTE modDiff = modifiers ^ g_heldModifiers;
    for (int i = 0; i < 8; i++) {
        if (modDiff & (1 << i)) {
            BOOL pressed = (modifiers & (1 << i)) != 0;
            SendKey(MOD_VKEYS[i], pressed);
        }
    }
    g_heldModifiers = modifiers;

    /* Handle Fn state tracking (for logging) */
    if (fnNow != g_fnHeld) {
        g_fnHeld = fnNow;
    }

    /* Process key array changes.
     * Keys in the previous array but not in the current → released.
     * Keys in the current array but not in the previous → pressed. */

    BYTE newKeys[KEY_COUNT];
    for (int i = 0; i < KEY_COUNT; i++)
        newKeys[i] = report[KEY_OFFSET + i];

    /* Release keys that are no longer in the array */
    for (int i = 0; i < KEY_COUNT; i++) {
        BYTE old = g_heldKeys[i];
        if (old == 0) continue;
        BOOL found = FALSE;
        for (int j = 0; j < KEY_COUNT; j++) {
            if (newKeys[j] == old) { found = TRUE; break; }
        }
        if (!found) {
            WORD vk = HidToVKey(old);
            if (vk) SendKey(vk, FALSE);
        }
    }

    /* Press keys that are newly in the array */
    for (int i = 0; i < KEY_COUNT; i++) {
        BYTE cur = newKeys[i];
        if (cur == 0) continue;
        /* Check for rollover error (0x01 = ErrorRollOver) */
        if (cur == 0x01) continue;
        BOOL found = FALSE;
        for (int j = 0; j < KEY_COUNT; j++) {
            if (g_heldKeys[j] == cur) { found = TRUE; break; }
        }
        if (!found) {
            WORD vk = HidToVKey(cur);
            if (vk) SendKey(vk, TRUE);
        }
    }

    memcpy(g_heldKeys, newKeys, KEY_COUNT);
}

static void ProcessConsumerReport(const BYTE *report, DWORD len)
{
    /* Consumer buttons - inject media VKeys.
     * Report ID 82 (0x52): media transport controls
     * The exact bitmap layout depends on the report descriptor.
     * We'll handle the common media keys. */

    if (len < 2) return;

    static BYTE prevConsumer = 0;
    BYTE cur = report[1];
    BYTE diff = cur ^ prevConsumer;

    if (diff == 0) return;

    /* Bit mapping for consumer report (to be verified):
     * bit 0: Play/Pause (0x00CD)
     * bit 1: Fast Forward (0x00B3)
     * bit 2: Rewind (0x00B4)
     * bit 3: Next Track (0x00B5)
     * bit 4: Prev Track (0x00B6) */

    static const WORD consumerVKeys[] = {
        VK_MEDIA_PLAY_PAUSE,
        0,  /* fast forward - no standard VKey */
        0,  /* rewind - no standard VKey */
        VK_MEDIA_NEXT_TRACK,
        VK_MEDIA_PREV_TRACK,
    };

    for (int i = 0; i < 5; i++) {
        if ((diff & (1 << i)) && consumerVKeys[i]) {
            BOOL pressed = (cur & (1 << i)) != 0;
            SendKey(consumerVKeys[i], pressed);
        }
    }

    prevConsumer = cur;
}

static HANDLE OpenWinUsbDevice(WINUSB_INTERFACE_HANDLE *phWinUsb)
{
    HDEVINFO devInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD idx;
    HANDLE hDev = INVALID_HANDLE_VALUE;

    devInfo = SetupDiGetClassDevsW(&DEVICE_GUID, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    ifData.cbSize = sizeof(ifData);

    for (idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &DEVICE_GUID, idx, &ifData); idx++) {
        DWORD reqSize = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;

        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &reqSize, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, NULL, NULL)) {
            printf("Found device: %ls\n", detail->DevicePath);
            hDev = CreateFileW(detail->DevicePath,
                               GENERIC_WRITE | GENERIC_READ,
                               FILE_SHARE_WRITE | FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                               NULL);

            if (hDev != INVALID_HANDLE_VALUE) {
                if (WinUsb_Initialize(hDev, phWinUsb)) {
                    printf("WinUSB initialized.\n");
                    free(detail);
                    break;
                }
                printf("WinUsb_Initialize failed: %lu\n", GetLastError());
                CloseHandle(hDev);
                hDev = INVALID_HANDLE_VALUE;
            } else {
                printf("CreateFile failed: %lu\n", GetLastError());
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return hDev;
}

static BOOL FindInterruptPipe(WINUSB_INTERFACE_HANDLE hWinUsb,
                               UCHAR *pPipeIn, UCHAR *pPipeOut)
{
    USB_INTERFACE_DESCRIPTOR ifDesc;
    WINUSB_PIPE_INFORMATION pipeInfo;

    if (!WinUsb_QueryInterfaceSettings(hWinUsb, 0, &ifDesc)) {
        printf("QueryInterfaceSettings failed: %lu\n", GetLastError());
        return FALSE;
    }

    printf("Interface: class=%d subclass=%d protocol=%d endpoints=%d\n",
           ifDesc.bInterfaceClass, ifDesc.bInterfaceSubClass,
           ifDesc.bInterfaceProtocol, ifDesc.bNumEndpoints);

    *pPipeIn = 0;
    *pPipeOut = 0;

    for (UCHAR i = 0; i < ifDesc.bNumEndpoints; i++) {
        if (!WinUsb_QueryPipe(hWinUsb, 0, i, &pipeInfo)) continue;

        printf("  Pipe %d: type=%d id=0x%02X maxPacket=%d\n",
               i, pipeInfo.PipeType, pipeInfo.PipeId, pipeInfo.MaximumPacketSize);

        if (pipeInfo.PipeType == UsbdPipeTypeInterrupt) {
            if (pipeInfo.PipeId & 0x80)
                *pPipeIn = pipeInfo.PipeId;
            else
                *pPipeOut = pipeInfo.PipeId;
        }
    }

    return (*pPipeIn != 0);
}

/* Auto-detect which bit in the Apple byte is the Fn key.
 * Reads reports and watches for changes in byte 9 while
 * only byte 9 changes (no other keys pressed). */
static void DetectFnBit(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR pipeIn)
{
    BYTE buf[64];
    DWORD bytesRead;
    BYTE baseline = 0;
    int samples = 0;

    printf("\n=== Fn bit detection ===\n");
    printf("Press ONLY the Fn key (no other keys) 3 times...\n\n");

    /* Read baseline (no keys pressed) */
    if (WinUsb_ReadPipe(hWinUsb, pipeIn, buf, sizeof(buf), &bytesRead, NULL) &&
        bytesRead >= KBD_REPORT_SIZE && buf[0] == KBD_REPORT_ID) {
        baseline = buf[APPLE_OFFSET];
        printf("Baseline Apple byte: 0x%02X\n", baseline);
    }

    while (samples < 3) {
        if (!WinUsb_ReadPipe(hWinUsb, pipeIn, buf, sizeof(buf), &bytesRead, NULL))
            continue;
        if (bytesRead < KBD_REPORT_SIZE || buf[0] != KBD_REPORT_ID)
            continue;

        BYTE apple = buf[APPLE_OFFSET];
        BYTE mods = buf[MOD_OFFSET];
        BOOL anyKeys = FALSE;
        for (int i = 0; i < KEY_COUNT; i++)
            if (buf[KEY_OFFSET + i] != 0) anyKeys = TRUE;

        if (apple != baseline && mods == 0 && !anyKeys) {
            BYTE diff = apple ^ baseline;
            printf("  Fn press detected! Apple byte: 0x%02X (diff: 0x%02X)\n", apple, diff);
            g_fnBit = diff;
            samples++;
        }
    }

    printf("\nFn bit mask: 0x%02X\n", g_fnBit);
    printf("=== Detection complete ===\n\n");
}

static volatile BOOL g_running = TRUE;

static BOOL WINAPI ConsoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_running = FALSE;
        return TRUE;
    }
    return FALSE;
}

static void ReleaseAllKeys(void)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        if (g_heldKeys[i]) {
            WORD vk = HidToVKey(g_heldKeys[i]);
            if (vk) SendKey(vk, FALSE);
            g_heldKeys[i] = 0;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (g_heldModifiers & (1 << i))
            SendKey(MOD_VKEYS[i], FALSE);
    }
    g_heldModifiers = 0;
    g_fnHeld = FALSE;
}

/* === Windows service: launches the remapper in the user's session === */

#define SVC_NAME L"FnRemap"

static SERVICE_STATUS g_svcStatus;
static SERVICE_STATUS_HANDLE g_svcStatusHandle;
static HANDLE g_svcStopEvent;
static HANDLE g_childProcess;

static void ReportSvcStatus(DWORD state, DWORD exitCode, DWORD waitHint)
{
    static DWORD checkpoint = 1;
    g_svcStatus.dwCurrentState = state;
    g_svcStatus.dwWin32ExitCode = exitCode;
    g_svcStatus.dwWaitHint = waitHint;
    g_svcStatus.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    g_svcStatus.dwControlsAccepted =
        (state == SERVICE_START_PENDING) ? 0 :
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
}

static void WINAPI SvcCtrlHandler(DWORD ctrl)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        ReportSvcStatus(SERVICE_STOP_PENDING, 0, 3000);
        if (g_childProcess)
            TerminateProcess(g_childProcess, 0);
        SetEvent(g_svcStopEvent);
    }
}

static BOOL LaunchWorker(HANDLE *phProcess)
{
    DWORD sid = WTSGetActiveConsoleSessionId();
    if (sid == 0xFFFFFFFF) return FALSE;

    HANDLE hSysToken = NULL, hDup = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hSysToken))
        return FALSE;

    if (!DuplicateTokenEx(hSysToken, MAXIMUM_ALLOWED, NULL,
                          SecurityImpersonation, TokenPrimary, &hDup)) {
        CloseHandle(hSysToken);
        return FALSE;
    }
    CloseHandle(hSysToken);

    if (!SetTokenInformation(hDup, TokenSessionId, &sid, sizeof(sid))) {
        CloseHandle(hDup);
        return FALSE;
    }

    LPVOID pEnv = NULL;
    CreateEnvironmentBlock(&pEnv, hDup, FALSE);

    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    WCHAR cmd[MAX_PATH + 64];
    wsprintfW(cmd, L"\"%s\" --fn-bit=0x02 --background", exe);

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessAsUserW(hDup, NULL, cmd,
        NULL, NULL, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        pEnv, NULL, &si, &pi);

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hDup);

    if (ok) {
        CloseHandle(pi.hThread);
        *phProcess = pi.hProcess;
    }
    return ok;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;

    g_svcStatusHandle = RegisterServiceCtrlHandlerW(SVC_NAME, SvcCtrlHandler);
    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ReportSvcStatus(SERVICE_RUNNING, 0, 0);

    g_svcStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    while (WaitForSingleObject(g_svcStopEvent, 0) != WAIT_OBJECT_0) {
        if (LaunchWorker(&g_childProcess)) {
            HANDLE h[2] = { g_childProcess, g_svcStopEvent };
            DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            CloseHandle(g_childProcess);
            g_childProcess = NULL;
            if (w == WAIT_OBJECT_0 + 1) break;
            Sleep(2000);
        } else {
            if (WaitForSingleObject(g_svcStopEvent, 3000) == WAIT_OBJECT_0)
                break;
        }
    }

    ReportSvcStatus(SERVICE_STOPPED, 0, 0);
}

int main(int argc, char *argv[])
{
    HANDLE hMutex = NULL;
    BOOL skipDetect = FALSE;

    BOOL svcDispatch = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--fn-bit=", 9) == 0) {
            g_fnBit = (BYTE)strtol(argv[i] + 9, NULL, 0);
            skipDetect = TRUE;
        }
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            g_verbose = TRUE;
        if (strcmp(argv[i], "--service") == 0)
            svcDispatch = TRUE;
        if (strcmp(argv[i], "--background") == 0)
            g_serviceMode = TRUE;
    }

    if (svcDispatch) {
        SERVICE_TABLE_ENTRYW table[] = {
            { SVC_NAME, ServiceMain },
            { NULL, NULL }
        };
        StartServiceCtrlDispatcherW(table);
        return 0;
    }

    if (g_serviceMode) {
        FreeConsole();
        hMutex = CreateMutexW(NULL, TRUE, L"Global\\FnRemapMutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(hMutex);
            return 0;
        }
        skipDetect = TRUE;
        if (g_fnBit == 0) g_fnBit = 0x02;
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    printf("FnRemap - Apple Magic Keyboard Fn-to-Ctrl Remapper\n");
    printf("===================================================\n\n");

    if (g_fnBit)
        printf("Using Fn bit mask: 0x%02X\n", g_fnBit);

    while (g_running) {
        HANDLE hDev;
        WINUSB_INTERFACE_HANDLE hWinUsb = NULL;
        UCHAR pipeIn = 0, pipeOut = 0;
        BYTE buf[64];
        DWORD bytesRead;

        hDev = OpenWinUsbDevice(&hWinUsb);
        if (hDev == INVALID_HANDLE_VALUE) {
            if (!g_serviceMode) {
                printf("\nKeyboard not found. Make sure:\n");
                printf("  1. The keyboard is plugged in\n");
                printf("  2. fnremap.inf is installed (WinUSB driver for MI_01)\n");
                return 1;
            }
            Sleep(3000);
            continue;
        }

        if (!FindInterruptPipe(hWinUsb, &pipeIn, &pipeOut)) {
            printf("No interrupt IN endpoint found.\n");
            WinUsb_Free(hWinUsb);
            CloseHandle(hDev);
            if (!g_serviceMode) return 1;
            Sleep(3000);
            continue;
        }

        printf("Interrupt IN pipe: 0x%02X\n", pipeIn);

        ULONG timeout = 100;
        WinUsb_SetPipePolicy(hWinUsb, pipeIn, PIPE_TRANSFER_TIMEOUT,
                              sizeof(timeout), &timeout);

        if (!skipDetect) {
            timeout = 0;
            WinUsb_SetPipePolicy(hWinUsb, pipeIn, PIPE_TRANSFER_TIMEOUT,
                                  sizeof(timeout), &timeout);
            DetectFnBit(hWinUsb, pipeIn);
            timeout = 100;
            WinUsb_SetPipePolicy(hWinUsb, pipeIn, PIPE_TRANSFER_TIMEOUT,
                                  sizeof(timeout), &timeout);
        }

        if (g_fnBit == 0) {
            printf("WARNING: Fn bit not detected. Running without Fn remapping.\n");
            printf("         Use --fn-bit=0xNN to set manually.\n\n");
        }

        printf("Remapping active.\n");

        if (g_serviceMode)
            SwitchToInputDesktop();

        int errCount = 0;
        while (g_running) {
            if (!WinUsb_ReadPipe(hWinUsb, pipeIn, buf, sizeof(buf), &bytesRead, NULL)) {
                DWORD err = GetLastError();
                if (err == ERROR_SEM_TIMEOUT) continue;
                if (err == ERROR_DEVICE_NOT_CONNECTED || ++errCount > 10) {
                    printf("Keyboard disconnected (err=%lu).\n", err);
                    break;
                }
                Sleep(10);
                continue;
            }
            errCount = 0;

            if (g_serviceMode)
                SwitchToInputDesktop();

            if (bytesRead < 2) continue;

            if (g_verbose) {
                printf("Report (%lu):", bytesRead);
                for (DWORD j = 0; j < bytesRead && j < 16; j++)
                    printf(" %02X", buf[j]);
                printf("\n");
            }

            switch (buf[0]) {
            case KBD_REPORT_ID:
                if (bytesRead >= KBD_REPORT_SIZE)
                    ProcessKeyboardReport(buf);
                break;
            default:
                if (bytesRead >= 2)
                    ProcessConsumerReport(buf, bytesRead);
                break;
            }
        }

        ReleaseAllKeys();
        WinUsb_Free(hWinUsb);
        CloseHandle(hDev);

        if (!g_serviceMode) break;
        Sleep(2000);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    printf("Exiting.\n");
    return 0;
}
