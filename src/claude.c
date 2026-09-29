/*
 * Everything that talks to the installed Claude Desktop package.
 *
 * Claude Desktop ships as an MSIX package. Two facts drive this file:
 *  - A process started straight from the WindowsApps exe has no package
 *    identity; Claude then picks Electron's Squirrel updater and every update
 *    check fails ("Can not find Squirrel"). IApplicationActivationManager
 *    starts it exactly like the Start menu does (with identity) and still
 *    passes our arguments through to argv.
 *  - With identity, Claude writes to AppData through its package's private
 *    %LOCALAPPDATA%\Packages\<family>\LocalCache: an --user-data-dir that does
 *    not exist yet is created in LocalCache\Roaming (an existing one is used
 *    in place, so a profile folder is always created before its first
 *    launch), and a profile's logs can land in LocalCache\Local.
 */
#include "app.h"
#include <appmodel.h>
#include <shobjidl.h>
#include <string.h>

/* Sideloaded (claude.ai/download) and Microsoft Store package families. */
static const WCHAR *const kFamilies[] = {
    L"Claude_pzs8sxrjxfjjc",
    L"AnthropicPBC.Claude_fnn82j28hfe8t",
};

/* ------------------------------------------------------------ discovery */

static void VersionOf(const WCHAR *fullName, WCHAR *ver, size_t cch)
{
    const WCHAR *a = wcschr(fullName, L'_'), *b;
    ver[0] = 0;
    if (!a) return;
    a++;
    b = wcschr(a, L'_');
    StringCchCopyNW(ver, cch, a, b ? (size_t)(b - a) : wcslen(a));
}

static ULONGLONG VersionKey(const WCHAR *ver)
{
    ULONGLONG key = 0;
    int part = 0;
    const WCHAR *p = ver;
    while (part < 4) {
        ULONGLONG v = 0;
        while (*p >= L'0' && *p <= L'9') { v = v * 10 + (ULONGLONG)(*p - L'0'); p++; }
        key = (key << 16) | (v & 0xFFFF);
        part++;
        if (*p == L'.') p++;
        else break;
    }
    while (part++ < 4) key <<= 16;
    return key;
}

static void ResolveAumid(ClaudePackage *pkg)
{
    PACKAGE_INFO_REFERENCE ref = NULL;
    UINT32 bytes = 0, count = 0, i;
    BYTE *buf;

    StringCchPrintfW(pkg->aumid, ARRAYSIZE(pkg->aumid), L"%s!Claude", pkg->family);
    if (OpenPackageInfoByFullName(pkg->fullName, 0, &ref) != ERROR_SUCCESS) return;
    if (GetPackageApplicationIds(ref, &bytes, NULL, &count) == ERROR_INSUFFICIENT_BUFFER && bytes) {
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
        if (buf && GetPackageApplicationIds(ref, &bytes, buf, &count) == ERROR_SUCCESS) {
            const PCWSTR *ids = (const PCWSTR *)buf;
            for (i = 0; i < count; i++) {
                if (ids[i] && Core_EndsWithI(ids[i], L"!Claude")) {
                    StringCchCopyW(pkg->aumid, ARRAYSIZE(pkg->aumid), ids[i]);
                    break;
                }
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    ClosePackageInfo(ref);
}

static void Consider(const WCHAR *fullName, ClaudePackage *best)
{
    ClaudePackage c;
    UINT32 len;

    ZeroMemory(&c, sizeof c);
    if (FAILED(StringCchCopyW(c.fullName, ARRAYSIZE(c.fullName), fullName))) return;
    len = ARRAYSIZE(c.installDir);
    if (GetPackagePathByFullName(fullName, &len, c.installDir) != ERROR_SUCCESS) return;
    if (FAILED(StringCchPrintfW(c.exe, ARRAYSIZE(c.exe), L"%s\\app\\Claude.exe", c.installDir))) return;
    if (!Util_FileExists(c.exe)) return;
    len = ARRAYSIZE(c.family);
    if (PackageFamilyNameFromFullName(fullName, &len, c.family) != ERROR_SUCCESS) return;
    VersionOf(fullName, c.version, ARRAYSIZE(c.version));
    if (best->found && VersionKey(c.version) <= VersionKey(best->version)) return;
    ResolveAumid(&c);
    c.found = TRUE;
    *best = c;
}

static void ConsiderFamily(const WCHAR *family, ClaudePackage *best)
{
    UINT32 count = 0, chars = 0, i;
    PWSTR *names;
    WCHAR *buffer;
    const UINT32 filter = PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT;

    if (FindPackagesByPackageFamily(family, filter, &count, NULL, &chars, NULL, NULL) != ERROR_INSUFFICIENT_BUFFER ||
        count == 0 || chars == 0)
        return;
    names = (PWSTR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(PWSTR));
    buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, chars * sizeof(WCHAR));
    if (names && buffer &&
        FindPackagesByPackageFamily(family, filter, &count, names, &chars, buffer, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < count; i++) Consider(names[i], best);
    }
    if (names) HeapFree(GetProcessHeap(), 0, names);
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
}

/* Any other package whose name is Claude (a new publisher id, say). */
static void ConsiderRepository(ClaudePackage *best)
{
    HKEY key;
    WCHAR name[256];
    DWORD i, cch;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PACKAGES, 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS) return;
    for (i = 0;; i++) {
        cch = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (CompareStringOrdinal(name, 7, L"Claude_", 7, TRUE) == CSTR_EQUAL || Core_ContainsI(name, L".Claude_"))
            Consider(name, best);
    }
    RegCloseKey(key);
}

BOOL Claude_FindPackage(ClaudePackage *pkg)
{
    size_t i;
    ZeroMemory(pkg, sizeof *pkg);
    for (i = 0; i < ARRAYSIZE(kFamilies); i++) ConsiderFamily(kFamilies[i], pkg);
    if (!pkg->found) ConsiderRepository(pkg);
    return pkg->found;
}

/* -------------------------------------------------------------- launching */

static HRESULT Activate(const WCHAR *aumid, const WCHAR *args, DWORD *pid)
{
    IApplicationActivationManager *mgr = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_LOCAL_SERVER,
                                  &IID_IApplicationActivationManager, (void **)&mgr);
    if (FAILED(hr))
        hr = CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IApplicationActivationManager, (void **)&mgr);
    if (FAILED(hr)) return hr;
    CoAllowSetForegroundWindow((IUnknown *)mgr, NULL);
    hr = IApplicationActivationManager_ActivateApplication(mgr, aumid, args, AO_NOERRORUI, pid);
    IApplicationActivationManager_Release(mgr);
    return hr;
}

static BOOL StartDirect(const WCHAR *exe, const WCHAR *args, DWORD *pid)
{
    WCHAR cmd[ARGS_CCH + MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (FAILED(StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\" %s", exe, args))) return FALSE;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

HRESULT Claude_Launch(const ClaudePackage *pkg, const Profile *profile, const WCHAR *url,
                      DWORD *pid, BOOL *withIdentity)
{
    WCHAR args[ARGS_CCH];
    DWORD p = 0;
    HRESULT hr;

    if (pid) *pid = 0;
    if (withIdentity) *withIdentity = FALSE;
    if (!pkg->found) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    /* The stock profile never gets a flag and its folder is never created
     * here: that is exactly what the Start menu does. */
    if (!profile->isStock && !Util_EnsureDir(profile->dataDir)) {
        DWORD err = GetLastError();
        Util_Log(L"cannot create %s (error %lu)", profile->dataDir, err);
        return HRESULT_FROM_WIN32(err ? err : ERROR_PATH_NOT_FOUND);
    }
    if (!Core_BuildLaunchArgs(profile->isStock ? NULL : profile->dataDir, url, args, ARRAYSIZE(args)))
        return E_INVALIDARG;

    AllowSetForegroundWindow(ASFW_ANY);
    hr = Activate(pkg->aumid, args, &p);
    if (SUCCEEDED(hr)) {
        if (withIdentity) *withIdentity = TRUE;
    } else {
        Util_Log(L"activation of %s failed (0x%08lX); starting the exe directly", pkg->aumid, (unsigned long)hr);
        if (!StartDirect(pkg->exe, args, &p)) {
            DWORD err = GetLastError();
            return HRESULT_FROM_WIN32(err ? err : ERROR_GEN_FAILURE);
        }
        hr = S_OK;
    }
    if (pid) *pid = p;
    return hr;
}

/* ------------------------------------------------------- running profiles */

/* Chromium's single-instance lock: the running browser process of a profile
 * owns a message-only window of class Chrome_MessageWindow whose title is the
 * user data directory. A second launch finds it the same way. */
void Claude_UpdateRunning(ProfileList *list)
{
    HWND h = NULL;
    WCHAR title[MAX_PATH];
    int i;

    for (i = 0; i < list->count; i++) {
        list->items[i].running = FALSE;
        list->items[i].pid = 0;
    }
    while ((h = FindWindowExW(HWND_MESSAGE, h, L"Chrome_MessageWindow", NULL)) != NULL) {
        if (GetWindowTextW(h, title, ARRAYSIZE(title)) <= 0) continue;
        for (i = 0; i < list->count; i++) {
            if (!list->items[i].running && Core_PathEquals(title, list->items[i].dataDir)) {
                list->items[i].running = TRUE;
                GetWindowThreadProcessId(h, &list->items[i].pid);
                break;
            }
        }
    }
}

/* The profile's Claude runs right now (it may have started or ended since
 * `profile` was read). */
BOOL Claude_IsRunning(const Profile *profile)
{
    ProfileList one;
    ZeroMemory(&one, sizeof one);
    one.items[0] = *profile;
    one.count = 1;
    Claude_UpdateRunning(&one);
    return one.items[0].running;
}

typedef struct TopmostSearch {
    const ProfileList *list;
    int found;
} TopmostSearch;

static BOOL CALLBACK TopmostProc(HWND h, LPARAM lp)
{
    TopmostSearch *s = (TopmostSearch *)lp;
    DWORD pid = 0;
    int i;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL) return TRUE;
    GetWindowThreadProcessId(h, &pid);
    for (i = 0; i < s->list->count; i++) {
        if (s->list->items[i].running && s->list->items[i].pid == pid) {
            s->found = i;
            return FALSE;
        }
    }
    return TRUE;
}

/* EnumWindows walks top-level windows in Z order: the first Claude window it
 * meets is the one the user touched last. */
int Claude_TopmostProfile(const ProfileList *list)
{
    TopmostSearch s;
    s.list = list;
    s.found = -1;
    EnumWindows(TopmostProc, (LPARAM)&s);
    return s.found;
}

/* ------------------------------------------------------------- sign-ins */

/* Claude writes a profile's logs to %LOCALAPPDATA%\<data folder name>\logs
 * (`which` 0), or to the package's LocalCache\Local when that folder did not
 * exist yet (`which` 1). The end of that main.log, or NULL. */
#define LOG_COPIES 2
static char *ReadLog(const ClaudePackage *pkg, const Profile *profile, int which, DWORD *len)
{
    WCHAR local[MAX_PATH], path[MAX_PATH];
    HRESULT hr;
    *len = 0;
    if (!Util_LocalAppData(local, ARRAYSIZE(local))) return NULL;
    if (which == 0)
        hr = StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s\\logs\\main.log", local, profile->folder);
    else if (pkg && pkg->found)
        hr = StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Packages\\%s\\LocalCache\\Local\\%s\\logs\\main.log",
                              local, pkg->family, profile->folder);
    else
        return NULL;
    return SUCCEEDED(hr) ? Util_ReadFile(path, 256 * 1024, TRUE, len) : NULL;
}

BOOL Claude_LastSignInStart(const ClaudePackage *pkg, const Profile *profile, ULONGLONG *ticks)
{
    int which;
    *ticks = 0;
    for (which = 0; which < LOG_COPIES; which++) {
        DWORD len;
        SYSTEMTIME st;
        char *text = ReadLog(pkg, profile, which, &len);
        if (!text) continue;
        if (Core_LatestSignInStart(text, len, &st)) {
            ULONGLONG t = Core_SystemTimeTicks(&st);
            if (t > *ticks) *ticks = t;
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    return *ticks != 0;
}

/* The profile's Claude, which has just exited, went down for an update: the
 * last quit line of its main.log is its updater's or Windows closing it for
 * a new package, and was written a moment ago (an older one means Claude
 * ended without logging why, a crash say). */
#define QUIT_RECENT_TICKS (5ULL * 60ULL * 10000000ULL)
BOOL Claude_ClosedForUpdate(const ClaudePackage *pkg, const Profile *profile)
{
    ULONGLONG latest = 0, now = Util_LocalNowTicks();
    BOOL update = FALSE;
    int which;
    for (which = 0; which < LOG_COPIES; which++) {
        DWORD len;
        SYSTEMTIME st;
        BOOL forUpdate = FALSE;
        char *text = ReadLog(pkg, profile, which, &len);
        if (!text) continue;
        if (Core_LastQuit(text, len, &st, &forUpdate)) {
            ULONGLONG t = Core_SystemTimeTicks(&st);
            if (t > latest) {
                latest = t;
                update = forUpdate;
            }
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    return update && latest + QUIT_RECENT_TICKS >= now;
}
