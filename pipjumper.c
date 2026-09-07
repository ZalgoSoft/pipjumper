#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#define APP_CLASS L"PiPJumperClass"

#define TRAY_ID 1
#define HOTKEY_TOGGLE 1
#define WM_TRAYICON (WM_APP + 1)

#define MENU_EXIT 100
#define MENU_TOGGLE 101
#define MENU_COOLDOWN_UP 103
#define MENU_COOLDOWN_DOWN 104
#define MENU_CLICKTHROUGH 105

#define COOLDOWN_DEFAULT 200
#define EDGE_PADDING 24

#define ALPHA_MIN 16
#define ALPHA_MAX 255
#define ALPHA_STEP 16

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

static HWND g_main;
static HHOOK g_mouseHook;

static BOOL g_enabled = TRUE;
static BOOL g_dragging = FALSE;
static BOOL g_clickThrough = FALSE;
static BOOL g_forwarding = FALSE;

static HWND g_hoverPip = NULL;
static HWND g_clickPip = NULL;

static DWORD g_lastJump = 0;
static DWORD g_cooldown = COOLDOWN_DEFAULT;

static LONG_PTR g_clickOldExStyle = 0;
static BYTE g_clickOldAlpha = ALPHA_MAX;
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
    typedef BOOL(WINAPI* PFN_SET_DPI_CONTEXT)(HANDLE);
    typedef BOOL(WINAPI* PFN_SET_DPI_AWARE)(void);

    HMODULE user32;
    PFN_SET_DPI_CONTEXT setContext;
    PFN_SET_DPI_AWARE setAware;

    user32 = GetModuleHandleW(L"user32.dll");

    if (!user32)
        return;

    setContext = (PFN_SET_DPI_CONTEXT)GetProcAddress(
        user32,
        "SetProcessDpiAwarenessContext"
    );

    if (setContext) {
        if (setContext((HANDLE)-4))
            return;
    }

    setAware = (PFN_SET_DPI_AWARE)GetProcAddress(
        user32,
        "SetProcessDPIAware"
    );

    if (setAware)
        setAware();
}


static HICON TrayIcon(void)
{
    return LoadIconW(
        NULL,
        g_enabled ? IDI_INFORMATION : IDI_WARNING
    );
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

    ex = GetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE
    );

    if (!(ex & WS_EX_TOPMOST))
        return FALSE;

    style = GetWindowLongPtrW(
        hwnd,
        GWL_STYLE
    );

    if (style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX))
        return FALSE;

    cls[0] = 0;
    title[0] = 0;

    GetClassNameW(
        hwnd,
        cls,
        (int)(sizeof(cls) / sizeof(cls[0]))
    );

    if (StrContains(cls, L"MozillaDialogClass") ||
        StrContains(cls, L"MozillaCompositorWindowClass") ||
        StrContains(cls, L"Chrome_WidgetWin"))
        return TRUE;

    GetWindowTextW(
        hwnd,
        title,
        (int)(sizeof(title) / sizeof(title[0]))
    );

    if (!title[0])
        return FALSE;

    LowerCopy(
        lower,
        title,
        (int)(sizeof(lower) / sizeof(lower[0]))
    );

    if (StrContains(lower, L"picture-in-picture") ||
        StrContains(lower, L"picture in picture"))
        return TRUE;

    return FALSE;
}


static HWND PipFromPoint(POINT pt)
{
    HWND hwnd;
    HWND root;

    hwnd = WindowFromPoint(pt);

    if (!hwnd)
        return NULL;

    if (IsPipWindow(hwnd))
        return hwnd;

    root = GetAncestor(
        hwnd,
        GA_ROOT
    );

    if (root && IsPipWindow(root))
        return root;

    return NULL;
}


static void GetWorkArea(HWND hwnd, RECT* out)
{
    HMONITOR monitor;
    MONITORINFO mi;

    monitor = MonitorFromWindow(
        hwnd,
        MONITOR_DEFAULTTONEAREST
    );

    mi.cbSize = sizeof(mi);

    if (GetMonitorInfoW(monitor, &mi)) {
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
    static DWORD state = 0;

    if (!state)
        state = GetTickCount() ^
                (DWORD)(ULONG_PTR)&state;

    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    if (max)
        return state % max;

    return 0;
}


static BOOL GetAlpha(HWND hwnd, BYTE* alpha)
{
    BYTE value = ALPHA_MAX;
    DWORD flags = 0;

    if (!GetLayeredWindowAttributes(
            hwnd,
            NULL,
            &value,
            &flags))
        return FALSE;

    if (!(flags & LWA_ALPHA))
        value = ALPHA_MAX;

    *alpha = value;

    return TRUE;
}


static void SetAlpha(HWND hwnd, BYTE alpha)
{
    LONG_PTR ex;

    if (!IsWindow(hwnd))
        return;

    ex = GetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE
    );

    if (!(ex & WS_EX_LAYERED)) {
        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            ex | WS_EX_LAYERED
        );

        SetWindowPos(
            hwnd,
            NULL,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOZORDER |
            SWP_NOACTIVATE |
            SWP_FRAMECHANGED
        );
    }

    SetLayeredWindowAttributes(
        hwnd,
        0,
        alpha,
        LWA_ALPHA
    );
}


static void ChangeTransparency(HWND hwnd, int wheelDelta)
{
    BYTE alpha;

    if (!IsPipWindow(hwnd))
        return;

    if (!GetAlpha(hwnd, &alpha))
        alpha = ALPHA_MAX;

    if (wheelDelta > 0) {
        if (alpha >= ALPHA_MAX - ALPHA_STEP)
            alpha = ALPHA_MAX;
        else
            alpha = (BYTE)(alpha + ALPHA_STEP);
    }
    else if (wheelDelta < 0) {
        if (alpha <= ALPHA_MIN + ALPHA_STEP)
            alpha = ALPHA_MIN;
        else
            alpha = (BYTE)(alpha - ALPHA_STEP);
    }

    SetAlpha(
        hwnd,
        alpha
    );
}


static void RestoreClickThrough(void)
{
    HWND hwnd = g_clickPip;

    if (!hwnd)
        return;

    if (IsWindow(hwnd) && g_clickSaved) {
        if (g_clickOldExStyle & WS_EX_LAYERED) {
            SetLayeredWindowAttributes(
                hwnd,
                0,
                g_clickOldAlpha,
                LWA_ALPHA
            );
        }

        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            g_clickOldExStyle
        );
    }

    g_clickPip = NULL;
    g_clickSaved = FALSE;
    g_clickOldExStyle = 0;
    g_clickOldAlpha = ALPHA_MAX;
}


static void EnterClickThrough(HWND hwnd)
{
    LONG_PTR ex;
    BYTE alpha = ALPHA_MAX;

    if (!IsPipWindow(hwnd))
        return;

    if (g_clickPip == hwnd)
        return;

    RestoreClickThrough();

    ex = GetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE
    );

    g_clickOldExStyle = ex;

    if (ex & WS_EX_LAYERED)
        GetAlpha(hwnd, &alpha);

    g_clickOldAlpha = alpha;
    g_clickSaved = TRUE;
    g_clickPip = hwnd;

    SetAlpha(
        hwnd,
        ALPHA_MIN
    );
}


static void JumpWindow(HWND hwnd)
{
    DWORD now;

    RECT area;
    RECT wr;

    int width;
    int height;

    int minX;
    int minY;
    int maxX;
    int maxY;

    int x;
    int y;

    if (!g_enabled ||
        g_clickThrough ||
        !IsPipWindow(hwnd))
        return;

    now = GetTickCount();

    if ((DWORD)(now - g_lastJump) < g_cooldown)
        return;

    GetWorkArea(
        hwnd,
        &area
    );

    if (!GetWindowRect(hwnd, &wr))
        return;

    width = wr.right - wr.left;
    height = wr.bottom - wr.top;

    minX = area.left + EDGE_PADDING;
    minY = area.top + EDGE_PADDING;

    maxX = area.right - width - EDGE_PADDING;
    maxY = area.bottom - height - EDGE_PADDING;

    if (maxX <= minX) {
        x = area.left +
            ((area.right - area.left) - width) / 2;
    }
    else {
        x = minX +
            (int)NextRandom(
                (DWORD)(maxX - minX + 1)
            );
    }

    if (maxY <= minY) {
        y = area.top +
            ((area.bottom - area.top) - height) / 2;
    }
    else {
        y = minY +
            (int)NextRandom(
                (DWORD)(maxY - minY + 1)
            );
    }

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

    g_lastJump = now;
}


static void UpdateTray(void)
{
    NOTIFYICONDATAW n;

    MemZero(
        &n,
        sizeof(n)
    );

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


static void ToggleEnabled(void)
{
    g_enabled = !g_enabled;

    g_hoverPip = NULL;

    if (!g_enabled)
        RestoreClickThrough();

    UpdateTray();
}


static void ToggleClickThrough(void)
{
    //g_clickThrough = !g_clickThrough;

    g_hoverPip = NULL;

    if (!g_clickThrough)
        RestoreClickThrough();

    UpdateTray();
}


static HWND FindWindowUnderPip(POINT pt)
{
    HWND pip;
    HWND target;
    BOOL visible;

    pip = g_clickPip;

    if (!pip ||
        !IsWindow(pip))
        return NULL;

    visible = IsWindowVisible(pip);

    if (visible)
        ShowWindow(
            pip,
            SW_HIDE
        );

    target = WindowFromPoint(pt);

    if (visible) {
        ShowWindow(
            pip,
            SW_SHOWNA
        );

        SetWindowPos(
            pip,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOACTIVATE |
            SWP_SHOWWINDOW
        );
    }

    if (!target ||
        target == pip)
        return NULL;

    if (GetAncestor(target, GA_ROOT) == pip)
        return NULL;

    return target;
}


static void ForwardMouse(
    WPARAM msg,
    MSLLHOOKSTRUCT* mouse
)
{
    INPUT input;

    MemZero(
        &input,
        sizeof(input)
    );

    input.type = INPUT_MOUSE;

    switch (msg) {
    case WM_LBUTTONDOWN:
        input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        break;

    case WM_LBUTTONUP:
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        break;

    case WM_RBUTTONDOWN:
        input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        break;

    case WM_RBUTTONUP:
        input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        break;

    case WM_MBUTTONDOWN:
        input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
        break;

    case WM_MBUTTONUP:
        input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        break;

    case WM_XBUTTONDOWN:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = HIWORD(mouse->mouseData);
        break;

    case WM_XBUTTONUP:
        input.mi.dwFlags = MOUSEEVENTF_XUP;
        input.mi.mouseData = HIWORD(mouse->mouseData);
        break;

    default:
        return;
    }

    g_forwarding = TRUE;

    SendInput(
        1,
        &input,
        sizeof(input)
    );

    g_forwarding = FALSE;
}


static BOOL IsClickMessage(WPARAM msg)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return TRUE;
    }

    return FALSE;
}


static LRESULT CALLBACK MouseProc(
    int code,
    WPARAM msg,
    LPARAM lp
)
{
    MSLLHOOKSTRUCT* mouse;
    POINT pt;
    HWND pip;

    if (code < 0) {
        return CallNextHookEx(
            g_mouseHook,
            code,
            msg,
            lp
        );
    }

    mouse = (MSLLHOOKSTRUCT*)lp;
    pt = mouse->pt;

    if (g_forwarding) {
        return CallNextHookEx(
            g_mouseHook,
            code,
            msg,
            lp
        );
    }

    if (!g_enabled) {
        return CallNextHookEx(
            g_mouseHook,
            code,
            msg,
            lp
        );
    }

    if (msg == WM_LBUTTONDOWN) {
        g_dragging = TRUE;
    }
    else if (msg == WM_LBUTTONUP) {
        g_dragging = FALSE;
    }

    if (msg == WM_MOUSEWHEEL) {
        pip = PipFromPoint(pt);

        if (pip) {
            ChangeTransparency(
                pip,
                (short)HIWORD(mouse->mouseData)
            );
        }
    }

    if (msg == WM_MOUSEMOVE && !g_dragging) {
        pip = PipFromPoint(pt);

        if (g_clickThrough) {
            if (pip != g_hoverPip) {
                if (pip) {
                    EnterClickThrough(pip);
                }
                else {
                    RestoreClickThrough();
                }

                g_hoverPip = pip;
            }
        }
        else {
            if (pip != g_hoverPip) {
                g_hoverPip = pip;

                if (pip &&
                    !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
                    JumpWindow(pip);
                }
            }
        }
    }

    if (g_clickThrough &&
        g_clickPip &&
        IsClickMessage(msg)) {

        pip = PipFromPoint(pt);

        if (pip == g_clickPip) {
            HWND target;

            target = FindWindowUnderPip(pt);

            if (target) {
                ForwardMouse(
                    msg,
                    mouse
                );

                return 1;
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


static void ShowTrayMenu(void)
{
    HMENU menu;
    POINT pt;
    WCHAR text[64];

    menu = CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING |
        (g_enabled ? MF_CHECKED : 0),
        MENU_TOGGLE,
        L"Enabled"
    );

    AppendMenuW(
        menu,
        MF_STRING |
        (g_clickThrough ? MF_CHECKED : 0),
        MENU_CLICKTHROUGH,
        L"Click through"
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
        MENU_COOLDOWN_DOWN,
        L"Cooldown - 100 ms"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        MENU_COOLDOWN_UP,
        L"Cooldown + 100 ms"
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

    SetForegroundWindow(
        g_main
    );

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


static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp
)
{
    switch (msg) {

    case WM_CREATE:
    {
        NOTIFYICONDATAW n;

        g_main = hwnd;

        MemZero(
            &n,
            sizeof(n)
        );

        n.cbSize = sizeof(n);
        n.hWnd = hwnd;
        n.uID = TRAY_ID;
        n.uFlags =
            NIF_ICON |
            NIF_MESSAGE |
            NIF_TIP;

        n.uCallbackMessage =
            WM_TRAYICON;

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

        return 0;
    }


    case WM_HOTKEY:

        if (wp == HOTKEY_TOGGLE)
            ToggleEnabled();

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

        case MENU_CLICKTHROUGH:
            ToggleClickThrough();
            break;

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

        RestoreClickThrough();

        UnregisterHotKey(
            hwnd,
            HOTKEY_TOGGLE
        );

        MemZero(
            &n,
            sizeof(n)
        );

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

    SetDpiAwareness();

    inst = GetModuleHandleW(NULL);

    MemZero(
        &wc,
        sizeof(wc)
    );

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
        result < 0
            ? 3
            : (UINT)msg.wParam
    );
}