#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#define APP_CLASS           L"PiPJumperClass"
#define TRAY_ID             1

#define HOTKEY_MODE         1

#define TIMER_SCAN          1
#define WM_TRAYICON         (WM_APP + 1)

#define SCAN_INTERVAL_MS    500
#define COOLDOWN_MS         200
#define EDGE_PADDING        24
#define CLICK_ALPHA         16

#define MENU_ENABLED        101
#define MENU_CLICKTHROUGH  102
#define MENU_DISABLED       103
#define MENU_EXIT           104

#define MODE_ENABLED        0
#define MODE_CLICKTHROUGH  1
#define MODE_DISABLED       2

#define MENU_ABOUT 105

#define APP_NAME L"PiPjumper"
#define APP_VERSION L"1.1"
#define APP_BUILD_DATE L"07.09.2026"
#define TOOLTIP_HELP L"| Ctrl+Shift+P\nhold Alt: temporarily disable jumping\nmouse scroll: change PIP transparency"

#define VIRTUAL_OFFSET      100
static HANDLE g_mutex = NULL;

#if defined(_WIN32) && !defined(_WIN64)

void* memset(void* dest, int ch, size_t count)
{
    unsigned char* p = (unsigned char*)dest;

    while (count--)
        *p++ = (unsigned char)ch;

    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    while (count--)
        *d++ = *s++;

    return dest;
}

#endif

static HWND g_main = NULL;
static HHOOK g_mouseHook = NULL;

static int g_mode = MODE_ENABLED;
static BOOL g_dragging = FALSE;
static BOOL g_insidePip = FALSE;

static HWND g_lastJumpPip = NULL;
static DWORD g_lastJump = 0;

static HWND g_clickPip = NULL;
static RECT g_clickRect;
static LONG_PTR g_clickOldExStyle = 0;
static BYTE g_clickOldAlpha = 255;
static BOOL g_clickSaved = FALSE;

static void MemZero(void* ptr, SIZE_T size)
{
    unsigned char* p = (unsigned char*)ptr;

    while (size--)
        *p++ = 0;
}

static BOOL StrContains(const WCHAR* str, const WCHAR* sub)
{
    if (!*sub)
        return TRUE;

    while (*str) {
        const WCHAR* a = str;
        const WCHAR* b = sub;

        while (*a && *b) {
            WCHAR ca = *a;
            WCHAR cb = *b;

            if (ca >= L'A' && ca <= L'Z')
                ca = (WCHAR)(ca + 32);

            if (cb >= L'A' && cb <= L'Z')
                cb = (WCHAR)(cb + 32);

            if (ca != cb)
                break;

            ++a;
            ++b;
        }

        if (!*b)
            return TRUE;

        ++str;
    }

    return FALSE;
}

static void LowerCopy(WCHAR* dst, const WCHAR* src, int capacity)
{
    int i = 0;

    if (capacity <= 0)
        return;

    while (i < capacity - 1 && src[i]) {
        WCHAR c = src[i];

        if (c >= L'A' && c <= L'Z')
            c = (WCHAR)(c + 32);

        dst[i++] = c;
    }

    dst[i] = 0;
}

static void SetDpiAwareness(void)
{
    typedef BOOL (WINAPI *PFN_SET_DPI_CONTEXT)(HANDLE);
    typedef BOOL (WINAPI *PFN_SET_DPI_AWARE)(void);

    HMODULE user32;
    PFN_SET_DPI_CONTEXT setContext;
    PFN_SET_DPI_AWARE setAware;

    user32 = GetModuleHandleW(L"user32.dll");

    if (!user32)
        return;

    setContext = (PFN_SET_DPI_CONTEXT)
        GetProcAddress(user32, "SetProcessDpiAwarenessContext");

    if (setContext) {
        if (setContext((HANDLE)-4))
            return;
    }

    setAware = (PFN_SET_DPI_AWARE)
        GetProcAddress(user32, "SetProcessDPIAware");

    if (setAware)
        setAware();
}

static HICON TrayIcon(void)
{
    if (g_mode == MODE_ENABLED)
        return LoadIconW(NULL, IDI_INFORMATION);

    if (g_mode == MODE_CLICKTHROUGH)
        return LoadIconW(NULL, IDI_QUESTION);

    return LoadIconW(NULL, IDI_WARNING);
}

static const WCHAR* ModeText(void)
{
    if (g_mode == MODE_ENABLED)
        return L"ON";

    if (g_mode == MODE_CLICKTHROUGH)
        return L"CLICK THROUGH";

    return L"OFF";
}

static BOOL IsPipWindow(HWND hwnd)
{
    LONG_PTR ex;
    LONG_PTR style;
    WCHAR cls[96];
    WCHAR title[256];
    WCHAR lower[256];

    if (!hwnd ||
        hwnd == g_main ||
        !IsWindow(hwnd) ||
        !IsWindowVisible(hwnd))
        return FALSE;

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!(ex & WS_EX_TOPMOST))
        return FALSE;

    style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    if (style & WS_MAXIMIZEBOX)
        return FALSE;

    if (style & WS_MINIMIZEBOX)
        return FALSE;

    cls[0] = 0;
    title[0] = 0;

    GetClassNameW(
        hwnd,
        cls,
        (int)(sizeof(cls) / sizeof(cls[0]))
    );

    GetWindowTextW(
        hwnd,
        title,
        (int)(sizeof(title) / sizeof(title[0]))
    );

    if (StrContains(cls, L"MozillaDialogClass") ||
        StrContains(cls, L"MozillaCompositorWindowClass") ||
        StrContains(cls, L"Chrome_WidgetWin"))
        return TRUE;

    if (title[0]) {
        LowerCopy(lower, title, 256);

        if (StrContains(lower, L"picture-in-picture") ||
            StrContains(lower, L"picture in picture"))
            return TRUE;
    }

    return FALSE;
}

static BOOL PointInWindow(HWND hwnd, POINT pt)
{
    RECT r;

    if (!GetWindowRect(hwnd, &r))
        return FALSE;

    return PtInRect(&r, pt);
}

static HWND PipFromPoint(POINT pt)
{
    HWND under;
    HWND root;

    under = WindowFromPoint(pt);

    if (!under)
        return NULL;

    root = GetAncestor(under, GA_ROOT);

    if (IsPipWindow(root))
        return root;

    return NULL;
}

static void GetWindowWorkArea(HWND hwnd, RECT* out)
{
    HMONITOR mon;
    MONITORINFO mi;

    mon = MonitorFromWindow(
        hwnd,
        MONITOR_DEFAULTTONEAREST
    );

    mi.cbSize = sizeof(mi);

    if (GetMonitorInfoW(mon, &mi)) {
        *out = mi.rcWork;
        return;
    }

    SystemParametersInfoW(
        SPI_GETWORKAREA,
        0,
        out,
        0
    );
}

static DWORD NextRandom(DWORD max)
{
    static DWORD s = 0;

    if (!s)
        s = GetTickCount() ^ (DWORD)(ULONG_PTR)&s;

    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;

    return max ? s % max : 0;
}

static void ChangeTransparency(HWND hwnd, int wheelDelta)
{
    LONG_PTR ex;
    BYTE alpha;

    if (g_mode != MODE_ENABLED || !IsPipWindow(hwnd))
        return;

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!(ex & WS_EX_LAYERED)) {
        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            ex | WS_EX_LAYERED
        );

        alpha = 255;
    } else {
        alpha = 255;

        if (!GetLayeredWindowAttributes(
                hwnd,
                NULL,
                &alpha,
                NULL))
            alpha = 255;
    }

    if (wheelDelta > 0) {
        if (alpha < 239)
            alpha = (BYTE)(alpha + 16);
        else
            alpha = 255;
    } else if (wheelDelta < 0) {
        if (alpha > 32)
            alpha = (BYTE)(alpha - 16);
        else
            alpha = 16;
    }

    SetLayeredWindowAttributes(
        hwnd,
        0,
        alpha,
        LWA_ALPHA
    );
}

static void JumpWindow(HWND hwnd)
{
    DWORD now;
    RECT area;
    RECT wr;
    int w;
    int h;
    int minX;
    int minY;
    int maxX;
    int maxY;
    int x;
    int y;

    if (g_mode != MODE_ENABLED ||
        !IsPipWindow(hwnd))
        return;

    now = GetTickCount();

    if (hwnd == g_lastJumpPip &&
        (DWORD)(now - g_lastJump) < COOLDOWN_MS)
        return;

    GetWindowWorkArea(hwnd, &area);

    if (!GetWindowRect(hwnd, &wr))
        return;

    w = wr.right - wr.left;
    h = wr.bottom - wr.top;

    minX = area.left + EDGE_PADDING;
    minY = area.top + EDGE_PADDING;

    maxX = area.right - w - EDGE_PADDING;
    maxY = area.bottom - h - EDGE_PADDING;

    if (maxX <= minX)
        x = area.left + ((area.right - area.left) - w) / 2;
    else
        x = minX +
            (int)NextRandom(
                (DWORD)(maxX - minX + 1)
            );

    if (maxY <= minY)
        y = area.top + ((area.bottom - area.top) - h) / 2;
    else
        y = minY +
            (int)NextRandom(
                (DWORD)(maxY - minY + 1)
            );

    SetWindowPos(
        hwnd,
        NULL,
        x,
        y,
        0,
        0,
        SWP_NOSIZE |
        SWP_NOZORDER |
        SWP_NOACTIVATE |
        SWP_ASYNCWINDOWPOS
    );

    g_lastJumpPip = hwnd;
    g_lastJump = now;
}

static void RestoreClickThrough(void)
{
    HWND hwnd;
    LONG_PTR ex;

    if (!g_clickSaved)
        return;

    hwnd = g_clickPip;

    if (!hwnd || !IsWindow(hwnd)) {
        g_clickPip = NULL;
        g_clickSaved = FALSE;
        return;
    }

    ex = g_clickOldExStyle;

    SetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE,
        ex
    );

    if (ex & WS_EX_LAYERED) {
        SetLayeredWindowAttributes(
            hwnd,
            0,
            g_clickOldAlpha,
            LWA_ALPHA
        );
    }

    SetWindowPos(
        hwnd,
        NULL,
        g_clickRect.left,
        g_clickRect.top,
        0,
        0,
        SWP_NOSIZE |
        SWP_NOZORDER |
        SWP_NOACTIVATE |
        SWP_ASYNCWINDOWPOS
    );

    g_clickPip = NULL;
    g_clickSaved = FALSE;
}

static void EnterClickThrough(HWND hwnd)
{
    LONG_PTR ex;
    BYTE alpha;
    RECT vr;
    int w;
    int h;
    int x;
    int y;

    if (g_mode != MODE_CLICKTHROUGH ||
        !IsPipWindow(hwnd) ||
        g_clickSaved)
        return;

    if (!GetWindowRect(hwnd, &g_clickRect))
        return;

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    alpha = 255;

    if (ex & WS_EX_LAYERED) {
        if (!GetLayeredWindowAttributes(
                hwnd,
                NULL,
                &alpha,
                NULL))
            alpha = 255;
    }

    g_clickPip = hwnd;
    g_clickOldExStyle = ex;
    g_clickOldAlpha = alpha;
    g_clickSaved = TRUE;

    if (!(ex & WS_EX_LAYERED)) {
        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            ex | WS_EX_LAYERED
        );
    }

    SetLayeredWindowAttributes(
        hwnd,
        0,
        CLICK_ALPHA,
        LWA_ALPHA
    );

    vr.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vr.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vr.right = vr.left +
        GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vr.bottom = vr.top +
        GetSystemMetrics(SM_CYVIRTUALSCREEN);

    w = g_clickRect.right - g_clickRect.left;
    h = g_clickRect.bottom - g_clickRect.top;

    x = vr.left - w - VIRTUAL_OFFSET;
    y = vr.top - h - VIRTUAL_OFFSET;

    SetWindowPos(
        hwnd,
        NULL,
        x,
        y,
        0,
        0,
        SWP_NOSIZE |
        SWP_NOZORDER |
        SWP_NOACTIVATE |
        SWP_ASYNCWINDOWPOS
    );
}

static void UpdateTray(void)
{
    NOTIFYICONDATAW n;

    MemZero(&n, sizeof(n));

    n.cbSize = sizeof(n);
    n.hWnd = g_main;
    n.uID = TRAY_ID;
    n.uFlags = NIF_ICON | NIF_TIP;
    n.hIcon = TrayIcon();

    wsprintfW(
        n.szTip,
        L"PiPjumper: %s %s",
        ModeText(),
        TOOLTIP_HELP
    );

    Shell_NotifyIconW(
        NIM_MODIFY,
        &n
    );
}

static void SetMode(int mode)
{
    POINT pt;
    HWND pip;

    if (mode == g_mode)
        return;

    if (g_mode == MODE_CLICKTHROUGH)
        RestoreClickThrough();

    g_mode = mode;
    g_insidePip = FALSE;

    UpdateTray();

    if (g_mode == MODE_CLICKTHROUGH) {
        GetCursorPos(&pt);
        pip = PipFromPoint(pt);

        if (pip) {
            EnterClickThrough(pip);
            g_insidePip = TRUE;
        }
    } else if (g_mode == MODE_ENABLED) {
        GetCursorPos(&pt);

        if (PipFromPoint(pt))
            g_insidePip = TRUE;
    }
}

static void CycleMode(void)
{
    int mode;

    mode = g_mode + 1;

    if (mode > MODE_DISABLED)
        mode = MODE_ENABLED;

    SetMode(mode);
}

void ShowAbout(void)
{
    WCHAR text[256];

    wsprintfW(
        text,
        L"PiPjumper\n\nTiny tool to allow Picture-in-Picture window to move with mouse and change it's transparency.\nVersion: %s\nBuild date: %s\n\nSupport:\nhttps://github.com/ZalgoSoft/pipjumper",
        APP_VERSION,
        APP_BUILD_DATE
    );

    MessageBoxW(
        g_main,
        text,
        L"About PiPjumper",
        MB_OK | MB_ICONINFORMATION
    );
}

static void ShowTrayMenu(void)
{
    HMENU menu;
    POINT pt;

    menu = CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING |
        (g_mode == MODE_ENABLED ? MF_CHECKED : 0),
        MENU_ENABLED,
        L"Enabled"
    );

    AppendMenuW(
        menu,
        MF_STRING |
        (g_mode == MODE_CLICKTHROUGH ? MF_CHECKED : 0),
        MENU_CLICKTHROUGH,
        L"Click through"
    );

    AppendMenuW(
        menu,
        MF_STRING |
        (g_mode == MODE_DISABLED ? MF_CHECKED : 0),
        MENU_DISABLED,
        L"Disabled"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        NULL
    );

AppendMenuW(menu, MF_STRING, MENU_ABOUT, L"About");
AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(
        menu,
        MF_STRING,
        MENU_EXIT,
        L"Exit"
    );

    GetCursorPos(&pt);

    SetForegroundWindow(g_main);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON |
        TPM_BOTTOMALIGN,
        pt.x,
        pt.y,
        0,
        g_main,
        NULL
    );

    PostMessageW(
        g_main,
        WM_NULL,
        0,
        0
    );

    DestroyMenu(menu);
}

static void ProcessMouseMove(POINT pt)
{
    HWND pip;

    if (g_mode == MODE_CLICKTHROUGH &&
                !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
        if (g_clickSaved) {
            if (PtInRect(&g_clickRect, pt)) {
                g_insidePip = TRUE;
                return;
            }

            RestoreClickThrough();
            g_insidePip = FALSE;
        }

        pip = PipFromPoint(pt);

        if (pip) {
            EnterClickThrough(pip);
            g_insidePip = TRUE;
        }

        return;
    }

    pip = PipFromPoint(pt);

    if (pip) {
        if (!g_insidePip) {
            if (g_mode == MODE_ENABLED &&
                !(GetAsyncKeyState(VK_MENU) & 0x8000))
                JumpWindow(pip);

            g_insidePip = TRUE;
        }
    } else {
        g_insidePip = FALSE;
    }
}

static LRESULT CALLBACK MouseProc(
    int code,
    WPARAM msg,
    LPARAM lp)
{
    if (code >= 0) {
        MSLLHOOKSTRUCT* m;

        m = (MSLLHOOKSTRUCT*)lp;

        if (msg == WM_LBUTTONDOWN) {
            g_dragging = TRUE;
        }
        else if (msg == WM_LBUTTONUP) {
            g_dragging = FALSE;
        }
        else if (msg == WM_MOUSEWHEEL) {
            if (g_mode == MODE_ENABLED) {
                HWND pip;

                pip = PipFromPoint(m->pt);

                if (pip) {
                    ChangeTransparency(
                        pip,
                        (short)HIWORD(m->mouseData)
                    );
                }
            }
        }
        else if (msg == WM_MOUSEMOVE &&
                 !g_dragging) {
            static POINT last = { -1, -1 };

            if (m->pt.x != last.x ||
                m->pt.y != last.y) {

                last = m->pt;
                ProcessMouseMove(m->pt);
            }
        }
    }

    return CallNextHookEx(
        g_mouseHook,
        code,
        msg,
        lp
    );
}

static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
    {
        NOTIFYICONDATAW n;

        g_main = hwnd;

        MemZero(&n, sizeof(n));

        n.cbSize = sizeof(n);
        n.hWnd = hwnd;
        n.uID = TRAY_ID;
        n.uFlags =
            NIF_ICON |
            NIF_MESSAGE |
            NIF_TIP;

        n.uCallbackMessage = WM_TRAYICON;
        n.hIcon = TrayIcon();

        wsprintfW(
            n.szTip,
            L"PiPjumper: ON %s",
	    TOOLTIP_HELP
        );

        Shell_NotifyIconW(
            NIM_ADD,
            &n
        );

        RegisterHotKey(
            hwnd,
            HOTKEY_MODE,
            MOD_CONTROL | MOD_SHIFT,
            'P'
        );

        SetTimer(
            hwnd,
            TIMER_SCAN,
            SCAN_INTERVAL_MS,
            NULL
        );

        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_SCAN) {
            if (g_clickSaved &&
                (!IsWindow(g_clickPip) ||
                 !IsWindowVisible(g_clickPip)))
                RestoreClickThrough();
        }

        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_MODE)
            CycleMode();

        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK)
            CycleMode();
        else if (lp == WM_RBUTTONUP)
            ShowTrayMenu();

        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case MENU_ENABLED:
            SetMode(MODE_ENABLED);
            break;

        case MENU_CLICKTHROUGH:
            SetMode(MODE_CLICKTHROUGH);
            break;

        case MENU_DISABLED:
            SetMode(MODE_DISABLED);
            break;

	case MENU_ABOUT:
	    ShowAbout();
            break;

        case MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }

        return 0;

    case WM_DESTROY:
    {
        NOTIFYICONDATAW n;

        KillTimer(
            hwnd,
            TIMER_SCAN
        );

        RestoreClickThrough();

        UnregisterHotKey(
            hwnd,
            HOTKEY_MODE
        );

        MemZero(&n, sizeof(n));

        n.cbSize = sizeof(n);
        n.hWnd = hwnd;
        n.uID = TRAY_ID;

        Shell_NotifyIconW(
            NIM_DELETE,
            &n
        );

        if (g_mouseHook) {
            UnhookWindowsHookEx(
                g_mouseHook
            );

            g_mouseHook = NULL;
        }

        PostQuitMessage(0);

        return 0;
    }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wp,
        lp
    );
}

void WINAPI EntryPoint(void)
{
    HINSTANCE inst;
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    int result;

    g_mutex = CreateMutexW(NULL, TRUE, L"PiPJumper_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ExitProcess(0);
    }

    SetDpiAwareness();

    inst = GetModuleHandleW(NULL);

    MemZero(&wc, sizeof(wc));

    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_CLASS;
    wc.hCursor = LoadCursorW(
        NULL,
        IDC_ARROW
    );

    if (!RegisterClassW(&wc))
        ExitProcess(1);

    hwnd = CreateWindowExW(
        0,
        APP_CLASS,
        L"PiP jumper",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        1,
        1,
        NULL,
        NULL,
        inst,
        NULL
    );

    if (!hwnd)
        ExitProcess(1);

    g_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL,
        MouseProc,
        inst,
        0
    );

    if (!g_mouseHook) {
        DestroyWindow(hwnd);
        ExitProcess(2);
    }

    for (;;) {
        result = (int)GetMessageW(
            &msg,
            NULL,
            0,
            0
        );

        if (result <= 0)
            break;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ExitProcess(
        result < 0 ? 3 : (UINT)msg.wParam
    );
}
