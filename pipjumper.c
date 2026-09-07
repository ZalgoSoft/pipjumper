// pipjumper.c
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#define APP_CLASS           L"PiPJumperClass"
#define TRAY_ID             1

#define HOTKEY_TOGGLE       1
#define HOTKEY_JUMP         2
#define HOTKEY_TRANSPARENCY 3

#define TIMER_SCAN          1
#define TIMER_TRANSPARENCY  2
#define WM_TRAYICON         (WM_APP + 1)

#define MAX_PIP_WINDOWS     64
#define SCAN_INTERVAL_MS    500
#define COOLDOWN_MS         200
#define EDGE_PADDING        24

#define MENU_EXIT           100
#define MENU_TOGGLE         101
#define MENU_JUMP           102
#define MENU_COOLDOWN_UP    103
#define MENU_COOLDOWN_DOWN  104



#if defined(_WIN32) && !defined(_WIN64)
    #pragma message(">>> 32-bit build: custom memset/memcpy included")

    void* memset(void* dest, int ch, size_t count) {
        unsigned char* p = (unsigned char*)dest;
        while (count--) {
            *p++ = (unsigned char)ch;
        }
        return dest;
    }

    void* memcpy(void* dest, const void* src, size_t count) {
        unsigned char* d = (unsigned char*)dest;
        const unsigned char* s = (const unsigned char*)src;
        while (count--) {
            *d++ = *s++;
        }
        return dest;
    }
#else
    #pragma message(">>> 64-bit build: using standard memset/memcpy")
#endif

/* ------------------------------------------------------------------------- */
/* Global state */

static HWND  g_pip[MAX_PIP_WINDOWS];
static DWORD g_lastJump[MAX_PIP_WINDOWS];
static int   g_pipCount = 0;

static HWND  g_main = NULL;
static HHOOK g_mouseHook = NULL;

static BOOL  g_enabled = TRUE;
static BOOL  g_dragging = FALSE;
static BOOL  g_insidePip = FALSE;
static BOOL  g_transparencyActive = TRUE;

/* Per-PiP transparency state.  Only windows modified by us are restored. */
static BOOL  g_transModified[MAX_PIP_WINDOWS];
static LONG_PTR g_transOldExStyle[MAX_PIP_WINDOWS];
static BYTE g_transOldAlpha[MAX_PIP_WINDOWS];
static DWORD g_transOldFlags[MAX_PIP_WINDOWS];

static DWORD g_cooldown = COOLDOWN_MS;


/* ------------------------------------------------------------------------- */
/* Tiny CRT replacements */

/*
 * Explicit byte-wise zeroing.
 *
 * This is intentionally written as a loop so the source itself does not
 * require memset().
 */
static void MemZero(void *ptr, SIZE_T size)
{
    unsigned char *p = (unsigned char *)ptr;

    while (size--)
        *p++ = 0;
}


/*
 * Explicit byte-wise copy.
 *
 * Used instead of memcpy().
 */
static void MemCopy(void *dst, const void *src, SIZE_T size)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (size--)
        *d++ = *s++;
}


/*
 * Case-insensitive ASCII substring search for WCHAR strings.
 *
 * This replaces wcsstr() for the strings actually used by PiP Jumper.
 *
 * Browser class names and "picture-in-picture" are ASCII, therefore
 * full Unicode case folding is unnecessary here.
 */
static BOOL StrContains(const WCHAR *str, const WCHAR *sub)
{
    if (!*sub)
        return TRUE;

    while (*str) {
        const WCHAR *a = str;
        const WCHAR *b = sub;

        while (*a && *b) {
            WCHAR ca = *a;
            WCHAR cb = *b;

            if (ca >= L'A' && ca <= L'Z')
                ca = (WCHAR)(ca + (L'a' - L'A'));

            if (cb >= L'A' && cb <= L'Z')
                cb = (WCHAR)(cb + (L'a' - L'A'));

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


/*
 * Copy a string and convert ASCII letters to lower case.
 *
 * Replaces:
 *
 *     wcsncpy_s()
 *     _wcslwr_s()
 */
static void LowerCopy(WCHAR *dst, const WCHAR *src, int capacity)
{
    int i = 0;

    if (capacity <= 0)
        return;

    while (i < capacity - 1 && src[i]) {
        WCHAR c = src[i];

        if (c >= L'A' && c <= L'Z')
            c = (WCHAR)(c + (L'a' - L'A'));

        dst[i++] = c;
    }

    dst[i] = 0;
}


/* ------------------------------------------------------------------------- */
/* DPI awareness */

/*
 * DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
 *
 * We resolve the function dynamically so the EXE can still start on
 * older Windows versions where SetProcessDpiAwarenessContext does not exist.
 *
 * IMPORTANT:
 *
 * This function is called BEFORE CreateWindowExW().
 */
static void SetDpiAwareness(void)
{
    typedef BOOL (WINAPI *PFN_SET_DPI_CONTEXT)(HANDLE);
    typedef BOOL (WINAPI *PFN_SET_DPI_AWARE)(void);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (!user32)
        return;

    {
        PFN_SET_DPI_CONTEXT setContext;

        setContext = (PFN_SET_DPI_CONTEXT)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");

        if (setContext) {
            if (setContext((HANDLE)-4))
                return;
        }
    }

    {
        PFN_SET_DPI_AWARE setAware;

        setAware = (PFN_SET_DPI_AWARE)
            GetProcAddress(user32, "SetProcessDPIAware");

        if (setAware)
            setAware();
    }
}


/* ------------------------------------------------------------------------- */

static HICON TrayIcon(void)
{
    return LoadIconW(
        NULL,
        g_enabled ? IDI_INFORMATION : IDI_WARNING
    );
}


/* ------------------------------------------------------------------------- */

static BOOL IsPipWindow(HWND hwnd)
{
    LONG_PTR ex;
    WCHAR cls[96];
    WCHAR title[256];

    if (!hwnd ||
        hwnd == g_main ||
        !IsWindow(hwnd) ||
        !IsWindowVisible(hwnd))
        return FALSE;

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!(ex & WS_EX_TOPMOST))
        return FALSE;

   ex = GetWindowLongPtrW(hwnd, GWL_STYLE);

    if (ex & WS_MAXIMIZEBOX)
        return FALSE;
    if (ex & WS_MINIMIZEBOX)
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

    /*
     * Chromium / Firefox PiP and compositor windows.
     */
    if (StrContains(cls, L"MozillaDialogClass") ||
        StrContains(cls, L"MozillaCompositorWindowClass") ||
        StrContains(cls, L"Chrome_WidgetWin"))
        return TRUE;

    /*
     * Generic browser PiP windows often expose a title.
     */
    if (title[0]) {
        WCHAR lower[256];

        LowerCopy(lower, title, 256);

        if (StrContains(lower, L"picture-in-picture") ||
            StrContains(lower, L"picture in picture"))
            return TRUE;
    }

    return FALSE;
}


/* ------------------------------------------------------------------------- */

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
{
    (void)lp;

    if (g_pipCount >= MAX_PIP_WINDOWS)
        return FALSE;

    if (!IsPipWindow(hwnd))
        return TRUE;

    for (int i = 0; i < g_pipCount; ++i) {
        if (g_pip[i] == hwnd)
            return TRUE;
    }

g_pip[g_pipCount] = hwnd;
g_lastJump[g_pipCount] = 0;

g_transModified[g_pipCount] = FALSE;
g_transOldExStyle[g_pipCount] = 0;
g_transOldAlpha[g_pipCount] = 255;
g_transOldFlags[g_pipCount] = 0;

++g_pipCount;
    return TRUE;
}


/* ------------------------------------------------------------------------- */

static void UpdatePipList(void)
{
    HWND old[MAX_PIP_WINDOWS];
    DWORD oldTime[MAX_PIP_WINDOWS];
    BOOL oldModified[MAX_PIP_WINDOWS];
    LONG_PTR oldExStyle[MAX_PIP_WINDOWS];
    BYTE oldAlpha[MAX_PIP_WINDOWS];
    DWORD oldFlags[MAX_PIP_WINDOWS];
    int oldCount = g_pipCount;

    /*
     * Preserve the old list and cooldown timestamps.
     */
    if (oldCount > 0) {
        MemCopy(
            old,
            g_pip,
            (SIZE_T)oldCount * sizeof(old[0])
        );

        MemCopy(
            oldTime,
            g_lastJump,
            (SIZE_T)oldCount * sizeof(oldTime[0])
        );
        MemCopy(
            oldModified,
            g_transModified,
            (SIZE_T)oldCount * sizeof(oldModified[0])
        );
        MemCopy(
            oldExStyle,
            g_transOldExStyle,
            (SIZE_T)oldCount * sizeof(oldExStyle[0])
        );
        MemCopy(
            oldAlpha,
            g_transOldAlpha,
            (SIZE_T)oldCount * sizeof(oldAlpha[0])
        );
        MemCopy(
            oldFlags,
            g_transOldFlags,
            (SIZE_T)oldCount * sizeof(oldFlags[0])
        );
    }

    g_pipCount = 0;

    EnumWindows(EnumProc, 0);

    /*
     * Restore cooldown timestamps for windows which survived the scan.
     */
    for (int i = 0; i < g_pipCount; ++i) {
        for (int j = 0; j < oldCount; ++j) {
            if (g_pip[i] == old[j]) {
                g_lastJump[i] = oldTime[j];
                g_transModified[i] = oldModified[j];
                g_transOldExStyle[i] = oldExStyle[j];
                g_transOldAlpha[i] = oldAlpha[j];
                g_transOldFlags[i] = oldFlags[j];
                break;
            }
        }
    }
}


/* ------------------------------------------------------------------------- */

static int FindPip(HWND hwnd)
{
    for (int i = 0; i < g_pipCount; ++i) {
        if (g_pip[i] == hwnd)
            return i;
    }

    return -1;
}


/* ------------------------------------------------------------------------- */

static BOOL PointInWindow(HWND hwnd, POINT pt)
{
    RECT r;

    if (!GetWindowRect(hwnd, &r))
        return FALSE;

    return PtInRect(&r, pt);
}


/* ------------------------------------------------------------------------- */

static HWND PipFromPoint(POINT pt)
{
    HWND under = WindowFromPoint(pt);

    if (!under)
        return NULL;

    for (int i = 0; i < g_pipCount; ++i) {
        HWND pip = g_pip[i];

        if (!IsWindow(pip) || !IsWindowVisible(pip))
            continue;

        if (under == pip ||
            GetAncestor(under, GA_ROOT) == pip ||
            PointInWindow(pip, pt))
            return pip;
    }

    return NULL;
}


/* ------------------------------------------------------------------------- */

static void GetWindowWorkArea(HWND hwnd, RECT *out)
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


/* ------------------------------------------------------------------------- */
/* Tiny xorshift PRNG */

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


/* ------------------------------------------------------------------------- */
/*
 * Temporary mouse-wheel transparency.
 *
 * Ctrl+Shift+X enables this mode for 5 seconds.  While active:
 *   wheel up   -> more opaque
 *   wheel down -> more transparent
 *
 * The original layered-window state is restored when the 5 seconds expire.
 */
static void RestoreTransparency(void)
{
    int i;

    for (i = 0; i < g_pipCount; ++i) {
        HWND hwnd = g_pip[i];

        if (!g_transModified[i] || !IsWindow(hwnd))
            continue;

        if (g_transOldExStyle[i] & WS_EX_LAYERED) {
            if (g_transOldFlags[i] & LWA_ALPHA)
                SetLayeredWindowAttributes(
                    hwnd, 0, g_transOldAlpha[i], LWA_ALPHA
                );
            SetWindowLongPtrW(
                hwnd, GWL_EXSTYLE,
                g_transOldExStyle[i]
            );
        } else {
            SetWindowLongPtrW(
                hwnd, GWL_EXSTYLE,
                g_transOldExStyle[i]
            );
        }

        g_transModified[i] = FALSE;
    }

    g_transparencyActive = FALSE;
}

static void EnableTransparency(void)
{
    g_transparencyActive = TRUE;
//    SetTimer(g_main, TIMER_TRANSPARENCY, 5000, NULL);
}

static void ChangeTransparency(HWND hwnd, int wheelDelta)
{
    int idx;
    LONG_PTR ex;
    BYTE alpha;
    BYTE oldAlpha = 255;

    if (!g_transparencyActive || !IsWindow(hwnd))
        return;

    idx = FindPip(hwnd);
    if (idx < 0)
        return;

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!g_transModified[idx]) {
        g_transOldExStyle[idx] = ex;
        g_transOldAlpha[idx] = 255;
        g_transOldFlags[idx] = 0;

        if (ex & WS_EX_LAYERED) {
            BYTE queriedAlpha;
            DWORD flags;

            queriedAlpha = 255;
            flags = 0;

            if (GetLayeredWindowAttributes(
                    hwnd, NULL, &queriedAlpha, &flags)) {
                g_transOldFlags[idx] = flags;
                if (flags & LWA_ALPHA)
                    g_transOldAlpha[idx] = queriedAlpha;
            }
        }

        g_transModified[idx] = TRUE;
        oldAlpha = g_transOldAlpha[idx];

        if (!(ex & WS_EX_LAYERED)) {
            SetWindowLongPtrW(
                hwnd,
                GWL_EXSTYLE,
                ex | WS_EX_LAYERED
            );
        }
    } else {
        oldAlpha = 255;
        if (!GetLayeredWindowAttributes(
                hwnd, NULL, &oldAlpha, NULL))
            oldAlpha = 255;
    }

    alpha = oldAlpha;

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

    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
}

/* ------------------------------------------------------------------------- */

static void JumpWindow(HWND hwnd)
{
    int idx;
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

    if (!g_enabled || !IsWindow(hwnd))
        return;

    idx = FindPip(hwnd);

    if (idx < 0)
        return;

    now = GetTickCount();

    if ((DWORD)(now - g_lastJump[idx]) < g_cooldown)
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

    g_lastJump[idx] = now;
}


/* ------------------------------------------------------------------------- */

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
        L"PiPjumper: %s | Ctrl+Shift+P",
        g_enabled ? L"ON" : L"OFF"
    );

    Shell_NotifyIconW(
        NIM_MODIFY,
        &n
    );
}


/* ------------------------------------------------------------------------- */

static void ToggleEnabled(void)
{
    g_enabled = !g_enabled;
    g_insidePip = FALSE;

    UpdateTray();

    if (g_enabled) {
        UpdatePipList();
    } else {
        g_pipCount = 0;
    }
}


/* ------------------------------------------------------------------------- */

//static void JumpAll(void)
//{
//    if (!g_enabled)
//        return;
//
//    UpdatePipList();
//
//    for (int i = 0; i < g_pipCount; ++i)
//        JumpWindow(g_pip[i]);
//}


/* ------------------------------------------------------------------------- */

static void ShowTrayMenu(void)
{
    HMENU menu;
    WCHAR text[64];
    POINT pt;

    menu = CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING | (g_enabled ? MF_CHECKED : 0),
        MENU_TOGGLE,
        L"Enabled"
    );

//    AppendMenuW(
//        menu,
//        MF_STRING,
//        MENU_JUMP,
//        L"Jump all PiP windows"
//    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        NULL
    );

    AppendMenuW(
        menu,
        MF_STRING,
        MENU_COOLDOWN_DOWN,
        L"Cooldown - 100 ms"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        MENU_COOLDOWN_UP,
        L"Cooldown + 100 ms"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        NULL
    );

    wsprintfW(
        text,
        L"Cooldown: %lu ms",
        g_cooldown
    );

    AppendMenuW(
        menu,
        MF_STRING | MF_DISABLED,
        0,
        text
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        NULL
    );

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
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
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


/* ------------------------------------------------------------------------- */
/*
 * Low-level mouse hook.
 *
 * Important:
 * WH_MOUSE_LL executes the callback in this process.
 * No DLL injection is required.
 */

static LRESULT CALLBACK MouseProc(
    int code,
    WPARAM msg,
    LPARAM lp)
{
    if (code >= 0 && g_enabled) {
        MSLLHOOKSTRUCT *m =
            (MSLLHOOKSTRUCT *)lp;

        if (msg == WM_LBUTTONDOWN) {
            g_dragging = TRUE;
        }
        else if (msg == WM_LBUTTONUP) {
            g_dragging = FALSE;
        }
        else if (msg == WM_MOUSEWHEEL && g_transparencyActive) {
            HWND pip = PipFromPoint(m->pt);
            if (pip)
                ChangeTransparency(
                    pip,
                    (short)HIWORD(m->mouseData)
                );
        }
        else if (msg == WM_MOUSEMOVE && !g_dragging) {
            static POINT last = { -1, -1 };

            if (m->pt.x != last.x ||
                m->pt.y != last.y) {

                HWND pip;
                BOOL inside;

                last = m->pt;

                pip = PipFromPoint(m->pt);
                inside = pip != NULL;

                /*
                 * Trigger only on ENTER.
                 *
                 * This prevents repeated jumping while the cursor
                 * remains inside the same PiP window.
                 */
                if (inside && !g_insidePip) {
                    /*
                     * Holding Alt means "interact with the PiP window":
                     * do not jump it while entering it.
                     */
                    if (!(GetAsyncKeyState(VK_MENU) & 0x8000))
                        JumpWindow(pip);
                }

                g_insidePip = inside;
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


/* ------------------------------------------------------------------------- */

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

        /*
         * DPI awareness is intentionally NOT initialized here.
         *
         * It is already configured in EntryPoint(), BEFORE
         * CreateWindowExW().
         */

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
            L"PiPjumper: ON | Ctrl+Shift+P"
        );

        Shell_NotifyIconW(
            NIM_ADD,
            &n
        );

        RegisterHotKey(
            hwnd,
            HOTKEY_TOGGLE,
            MOD_CONTROL | MOD_SHIFT,
            'P'
        );

//        RegisterHotKey(
//            hwnd,
//            HOTKEY_JUMP,
//            MOD_CONTROL | MOD_SHIFT,
//            'J'
//        );

//        RegisterHotKey(
//            hwnd,
//            HOTKEY_TRANSPARENCY,
//            MOD_CONTROL | MOD_SHIFT,
//            'X'
//        );

        UpdatePipList();

        SetTimer(
            hwnd,
            TIMER_SCAN,
            SCAN_INTERVAL_MS,
            NULL
        );

        return 0;
    }


    case WM_TIMER:

        if (wp == TIMER_SCAN && g_enabled)
            UpdatePipList();
//        else if (wp == TIMER_TRANSPARENCY) {
//            KillTimer(hwnd, TIMER_TRANSPARENCY);
//            RestoreTransparency();
//        }

        return 0;


    case WM_HOTKEY:

        if (wp == HOTKEY_TOGGLE)
            ToggleEnabled();
//        else if (wp == HOTKEY_JUMP)
//            JumpAll();
//        else if (wp == HOTKEY_TRANSPARENCY)
//            EnableTransparency();

        return 0;


    case WM_TRAYICON:

        if (lp == WM_LBUTTONDBLCLK)
            ToggleEnabled();
        else if (lp == WM_RBUTTONUP)
            ShowTrayMenu();

        return 0;


    case WM_COMMAND:

        switch (LOWORD(wp)) {

        case MENU_TOGGLE:
            ToggleEnabled();
            break;

//        case MENU_JUMP:
//            JumpAll();
//            break;

        case MENU_COOLDOWN_UP:

            if (g_cooldown < 10000)
                g_cooldown += 100;

            break;

        case MENU_COOLDOWN_DOWN:

            if (g_cooldown > 100)
                g_cooldown -= 100;

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
//        KillTimer(
//            hwnd,
//            TIMER_TRANSPARENCY
//        );

        if (g_transparencyActive)
            RestoreTransparency();

        UnregisterHotKey(
            hwnd,
            HOTKEY_TOGGLE
        );

//        UnregisterHotKey(
//            hwnd,
//            HOTKEY_JUMP
//        );
//        UnregisterHotKey(
//            hwnd,
//            HOTKEY_TRANSPARENCY
//        );

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


/* ------------------------------------------------------------------------- */
/*
 * CRT-free entry point.
 *
 * This is the actual PE entry point when linked with:
 *
 *     /ENTRY:EntryPoint
 *     /NODEFAULTLIB
 *
 * Therefore there is no WinMainCRTStartup and no MSVC CRT initialization.
 */
void WINAPI EntryPoint(void)
{
    HINSTANCE inst;
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    int result;

    /*
     * MUST happen before creating any HWND.
     *
     * This preserves the DPI-aware coordinate behavior.
     */
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

    /*
     * WH_MOUSE_LL is a global low-level mouse hook.
     * The callback executes inside this process.
     */
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

    /*
     * Message loop.
     *
     * GetMessageW():
     *   > 0  message
     *   = 0  WM_QUIT
     *   < 0  error
     */
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