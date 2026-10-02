/*
 * Notification-area icons.
 *
 * Each Claude shows its own icon in the notification area (while its tray
 * setting is on): Claude's white or dark glyph, after the apps' light or dark
 * choice. The icon belongs to the Claude process's hidden
 * Electron_NotifyIconHostWindow, and Shell_NotifyIcon(NIM_MODIFY) on that
 * window and the icon's ID changes it from another process: the profile's
 * watcher gives it the profile's color and name.
 *
 * Measured (docs/HOW-IT-WORKS.md says on which versions): Electron numbers a
 * process's tray icons from 3 up (a new number each time Claude re-creates
 * its icon), and NIM_MODIFY with no flags only tells whether an ID exists.
 * Claude sets its own image again when Explorer restarts and when the app
 * theme changes; the watcher paints the icon again after those events, and
 * at the size of a new display scale (see taskbar.c). Turning Claude's tray
 * setting off and on again while it runs brings Claude's own icon back until
 * the next of those events.
 */
#include "app.h"
#include <shellapi.h>

#define TRAY_FIRST_ID        3
#define TRAY_ID_SPAN         64     /* IDs looked at past the last one found: icons re-created since */
#define TRAY_HOST_CLASS      L"Electron_NotifyIconHostWindow"
#define CLAUDE_BUSY_WAIT_MS  2000   /* Claude's thread that owns the icon: longer, it is hung */
#define ICON_LOOKUPS         3      /* each after one more message of Claude's thread */

/* The ID found last, for that host: the search starts there, IDs only grow. */
static HWND g_lastHost;
static UINT g_lastId;

/* Claude's own notification-area image, the one it shows for the apps' light
 * or dark choice. */
BOOL Tray_ClaudeImagePath(const ClaudePackage *pkg, BOOL lightApps, WCHAR *out, size_t cch)
{
    return pkg && pkg->found &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\app\\resources\\Tray-Win32%s.ico", pkg->installDir, lightApps ? L"" : L"-Dark"));
}

BOOL Tray_IsHost(HWND window)
{
    WCHAR className[64];
    return GetClassNameW(window, className, ARRAYSIZE(className)) &&
           CompareStringOrdinal(className, -1, TRAY_HOST_CLASS, -1, FALSE) == CSTR_EQUAL;
}

typedef struct HostSearch {
    DWORD processId;
    HWND  found;
} HostSearch;

static BOOL CALLBACK HostVisitor(HWND window, LPARAM parameter)
{
    HostSearch *search = (HostSearch *)parameter;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || !Tray_IsHost(window)) return TRUE;
    search->found = window;
    return FALSE;
}

static HWND FindHost(DWORD processId)
{
    HostSearch search;
    search.processId = processId;
    search.found = NULL;
    if (processId) EnumWindows(HostVisitor, (LPARAM)&search);
    return search.found;
}

/* The ID of the icon Claude shows now, or 0. */
static UINT FindClaudeIconId(HWND host)
{
    NOTIFYICONDATAW icon;
    UINT id, first = (host == g_lastHost && g_lastId) ? g_lastId : TRAY_FIRST_ID;
    ZeroMemory(&icon, sizeof icon);
    icon.cbSize = sizeof icon;
    icon.hWnd = host;
    for (id = first; id <= first + TRAY_ID_SPAN; id++) {
        icon.uID = id;
        if (Shell_NotifyIconW(NIM_MODIFY, &icon)) {   /* no flags: changes nothing */
            g_lastHost = host;
            g_lastId = id;
            return id;
        }
    }
    return 0;
}

static BOOL SetClaudeIcon(HWND host, UINT id, HICON image, const WCHAR *tip)
{
    NOTIFYICONDATAW icon;
    ZeroMemory(&icon, sizeof icon);
    icon.cbSize = sizeof icon;
    icon.hWnd = host;
    icon.uID = id;
    icon.uFlags = NIF_ICON | NIF_TIP;
    icon.hIcon = image;
    StringCchCopyW(icon.szTip, ARRAYSIZE(icon.szTip), tip);
    return Shell_NotifyIconW(NIM_MODIFY, &icon);   /* the taskbar keeps its own copy of the image */
}

/* `value` of REG_PERSONALIZE: 1 is light. `missingIsLight` is what Windows
 * shows without it (Windows 10 1809 has no SystemUsesLightTheme: a dark
 * taskbar). */
static BOOL UsesLightTheme(const WCHAR *value, BOOL missingIsLight)
{
    DWORD light;
    if (!Util_RegGetDword(HKEY_CURRENT_USER, REG_PERSONALIZE, value, &light)) return missingIsLight;
    return light != 0;
}

/* The notification area's icon size at the taskbar's current scale (the
 * system DPI stays the one of the sign-in). */
static int NotificationIconSize(void)
{
    HWND taskbar = FindWindowW(TASKBAR_WINDOW_CLASS, NULL);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    return GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
}

/* Returns once the thread that owns Claude's icon has finished what it was
 * doing: its own notification-area calls are then done. */
static void WaitForClaude(HWND host)
{
    DWORD_PTR result;
    SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, CLAUDE_BUSY_WAIT_MS, &result);
}

/* A running profile's icon gets its color and name, once its Claude shows
 * one. Claude shows none while its tray setting is off. */
void Tray_Apply(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR tip[LABEL_CCH + 16];
    HWND host = FindHost(profile->pid);
    HICON image;
    UINT id = 0;
    int lookup;
    /* Right after an event Claude may still be adding its icon. */
    for (lookup = 0; host && lookup < ICON_LOOKUPS && !id; lookup++) {
        WaitForClaude(host);
        id = FindClaudeIconId(host);
    }
    if (!id) {
        Util_Log(L"%s shows no notification-area icon to color", profile->folder);
        return;
    }
    image = Icons_CreateTray(pkg, profile, NotificationIconSize(), !UsesLightTheme(L"SystemUsesLightTheme", FALSE));
    if (!image) {
        Util_Log(L"could not draw the notification-area icon of %s", profile->folder);
        return;
    }
    StringCchPrintfW(tip, ARRAYSIZE(tip), L"Claude (%s)", profile->name);
    if (!SetClaudeIcon(host, id, image, tip)) Util_Log(L"could not color the notification-area icon of %s", profile->folder);
    DestroyIcon(image);
}

/* Uninstall: Claude's own image and name, as Claude sets them. */
void Tray_GiveBack(const ClaudePackage *pkg, DWORD pid)
{
    WCHAR path[MAX_PATH];
    HWND host;
    HICON image;
    UINT id;
    int size;
    if (!Tray_ClaudeImagePath(pkg, UsesLightTheme(L"AppsUseLightTheme", TRUE), path, ARRAYSIZE(path)) ||
        (host = FindHost(pid)) == NULL || (id = FindClaudeIconId(host)) == 0)
        return;
    size = NotificationIconSize();
    image = (HICON)LoadImageW(NULL, path, IMAGE_ICON, size, size, LR_LOADFROMFILE);
    if (!image) {
        Util_Log(L"could not load Claude's notification-area icon %s (error %lu)", path, GetLastError());
        return;
    }
    if (!SetClaudeIcon(host, id, image, L"Claude")) Util_Log(L"could not give Claude's notification-area icon back to process %lu", pid);
    DestroyIcon(image);
}
