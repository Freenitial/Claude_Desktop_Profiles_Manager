/*
 * Notification-area icons.
 *
 * Each Claude shows its own icon in the notification area (while its tray
 * setting is on), always the same white Claude glyph. The icon belongs to the
 * Claude process's hidden Electron_NotifyIconHostWindow, and
 * Shell_NotifyIcon(NIM_MODIFY) on that window and the icon's ID changes it
 * from another process: the profile's watcher gives it the profile's color
 * and name.
 *
 * Measured on Claude Desktop 2.9939 and Windows 11 26200: Electron numbers a
 * process's tray icons from 3 up (a new number each time Claude re-creates
 * its icon), and NIM_MODIFY with no flags only tells whether an ID exists.
 * Claude sets its own image again when Explorer restarts and when the app
 * theme changes; the watcher paints the icon again after those events (see
 * taskbar.c). Turning Claude's tray setting off and on again while it runs
 * brings the white icon back until the next of those events.
 */
#include "app.h"
#include <shellapi.h>

#define TRAY_FIRST_ID    3
#define TRAY_LAST_ID     64
#define TRAY_HOST_CLASS  L"Electron_NotifyIconHostWindow"
#define REG_PERSONALIZE  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

BOOL Tray_IsHost(HWND h)
{
    WCHAR cls[64];
    return GetClassNameW(h, cls, ARRAYSIZE(cls)) && CompareStringOrdinal(cls, -1, TRAY_HOST_CLASS, -1, FALSE) == CSTR_EQUAL;
}

typedef struct HostSearch {
    DWORD pid;
    HWND found;
} HostSearch;

static BOOL CALLBACK HostProc(HWND h, LPARAM lp)
{
    HostSearch *s = (HostSearch *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid || !Tray_IsHost(h)) return TRUE;
    s->found = h;
    return FALSE;
}

static HWND FindHost(DWORD pid)
{
    HostSearch s;
    s.pid = pid;
    s.found = NULL;
    if (pid) EnumWindows(HostProc, (LPARAM)&s);
    return s.found;
}

static UINT FindId(HWND host)
{
    NOTIFYICONDATAW n;
    UINT id;
    ZeroMemory(&n, sizeof n);
    n.cbSize = sizeof n;
    n.hWnd = host;
    for (id = TRAY_FIRST_ID; id <= TRAY_LAST_ID; id++) {
        n.uID = id;
        if (Shell_NotifyIconW(NIM_MODIFY, &n)) return id;   /* no flags: changes nothing */
    }
    return 0;
}

static BOOL SetIcon(HWND host, UINT id, HICON icon, const WCHAR *tip)
{
    NOTIFYICONDATAW n;
    ZeroMemory(&n, sizeof n);
    n.cbSize = sizeof n;
    n.hWnd = host;
    n.uID = id;
    n.uFlags = NIF_ICON | NIF_TIP;
    n.hIcon = icon;
    StringCchCopyW(n.szTip, ARRAYSIZE(n.szTip), tip);
    return Shell_NotifyIconW(NIM_MODIFY, &n);   /* the taskbar keeps its own copy of the icon */
}

static BOOL LightSetting(const WCHAR *value)
{
    DWORD light = 1;
    Util_RegGetDword(HKEY_CURRENT_USER, REG_PERSONALIZE, value, &light);
    return light != 0;
}

static int IconSize(void)
{
    return GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
}

/* Returns once the thread that owns Claude's icon has finished what it was
 * doing: its own notification-area calls are then done. */
static void WaitForClaude(HWND host)
{
    DWORD_PTR result;
    SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &result);
}

/* A running profile's icon gets its color and name. FALSE when the profile's
 * Claude shows no icon (yet). */
BOOL Tray_Apply(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR tip[LABEL_CCH + 16];
    HWND host;
    HICON icon;
    UINT id = 0;
    int step;
    BOOL ok;
    if (!profile->running) return FALSE;
    host = FindHost(profile->pid);
    if (!host) return FALSE;
    /* Right after an event Claude may still be adding its icon: each wait lets
     * its thread finish one more message. */
    for (step = 0; step < 3 && !id; step++) {
        WaitForClaude(host);
        id = FindId(host);
    }
    if (!id) return FALSE;
    icon = Icons_CreateTray(pkg, profile, IconSize(), !LightSetting(L"SystemUsesLightTheme"));
    if (!icon) return FALSE;
    StringCchPrintfW(tip, ARRAYSIZE(tip), L"Claude (%s)", profile->name);
    ok = SetIcon(host, id, icon, tip);
    DestroyIcon(icon);
    return ok;
}

/* Uninstall: Claude's own image and name, as Claude sets them. */
void Tray_GiveBack(const ClaudePackage *pkg, DWORD pid)
{
    WCHAR path[MAX_PATH];
    HWND host = FindHost(pid);
    HICON icon;
    UINT id = host ? FindId(host) : 0;
    int size = IconSize();
    if (!id || !pkg->found) return;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\app\\resources\\%s", pkg->installDir,
                                LightSetting(L"AppsUseLightTheme") ? L"Tray-Win32.ico" : L"Tray-Win32-Dark.ico")))
        return;
    icon = (HICON)LoadImageW(NULL, path, IMAGE_ICON, size, size, LR_LOADFROMFILE);
    if (!icon) return;
    SetIcon(host, id, icon, L"Claude");
    DestroyIcon(icon);
}
