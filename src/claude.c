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
 *    %LOCALAPPDATA%\Packages\<family>\LocalCache: a --user-data-dir that does
 *    not exist yet is created in LocalCache\Roaming (an existing one is used
 *    in place, so a profile folder is always created before its first
 *    launch), and a profile's logs can land in LocalCache\Local.
 */
#include "app.h"
#include <appmodel.h>
#include <shobjidl.h>
#include <string.h>

#define LAUNCH_ARGS_CCH (URL_CCH + 2 * MAX_PATH)   /* --user-data-dir="<folder>" "<link>" */

/* Sideloaded (claude.ai/download) and Microsoft Store package families. */
static const WCHAR *const kFamilies[] = {
    L"Claude_pzs8sxrjxfjjc",
    L"AnthropicPBC.Claude_fnn82j28hfe8t",
};

/* ------------------------------------------------------------ discovery */

/* The version part of a package full name (<name>_<version>_<arch>_...). */
static void VersionOf(const WCHAR *fullName, WCHAR *version, size_t cch)
{
    const WCHAR *start = wcschr(fullName, L'_'), *end;
    version[0] = 0;
    if (!start) return;
    start++;
    end = wcschr(start, L'_');
    StringCchCopyNW(version, cch, start, end ? (size_t)(end - start) : wcslen(start));
}

/* The application Claude's package starts: its "Claude" entry, else its only
 * one (a renamed entry must not lose package identity), else the usual name. */
static void ResolveAumid(ClaudePackage *pkg)
{
    PACKAGE_INFO_REFERENCE reference = NULL;
    UINT32 bytes = 0, count = 0, i;
    BYTE *buffer;

    StringCchPrintfW(pkg->aumid, ARRAYSIZE(pkg->aumid), L"%s!Claude", pkg->family);
    if (OpenPackageInfoByFullName(pkg->fullName, 0, &reference) != ERROR_SUCCESS) return;
    if (GetPackageApplicationIds(reference, &bytes, NULL, &count) == ERROR_INSUFFICIENT_BUFFER && bytes) {
        buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
        if (buffer && GetPackageApplicationIds(reference, &bytes, buffer, &count) == ERROR_SUCCESS) {
            const PCWSTR *ids = (const PCWSTR *)buffer;
            for (i = 0; i < count && !(ids[i] && Core_EndsWithI(ids[i], L"!Claude")); i++) {}
            if (i == count && count == 1) i = 0;
            if (i < count && ids[i]) StringCchCopyW(pkg->aumid, ARRAYSIZE(pkg->aumid), ids[i]);
        }
        if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    }
    ClosePackageInfo(reference);
}

static void Consider(const WCHAR *fullName, ClaudePackage *best)
{
    ClaudePackage candidate;
    DWORD version[4], bestVersion[4];
    UINT32 len;

    ZeroMemory(&candidate, sizeof candidate);
    VersionOf(fullName, candidate.version, ARRAYSIZE(candidate.version));
    if (!Core_ParseVersion(candidate.version, version)) return;
    if (best->found && Core_ParseVersion(best->version, bestVersion) && Core_CompareVersions(version, bestVersion) <= 0) return;
    if (FAILED(StringCchCopyW(candidate.fullName, ARRAYSIZE(candidate.fullName), fullName))) return;
    len = ARRAYSIZE(candidate.installDir);
    if (GetPackagePathByFullName(fullName, &len, candidate.installDir) != ERROR_SUCCESS) return;
    if (FAILED(StringCchPrintfW(candidate.exe, ARRAYSIZE(candidate.exe), L"%s\\app\\Claude.exe", candidate.installDir))) return;
    if (!Util_FileExists(candidate.exe)) return;
    len = ARRAYSIZE(candidate.family);
    if (PackageFamilyNameFromFullName(fullName, &len, candidate.family) != ERROR_SUCCESS) return;
    ResolveAumid(&candidate);
    candidate.found = TRUE;
    *best = candidate;
}

#define PACKAGE_LIST_ATTEMPTS 3   /* the list can grow between asking its size and reading it, during an update */

static void ConsiderFamily(const WCHAR *family, ClaudePackage *best)
{
    const UINT32 filter = PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT;
    int attempt;
    for (attempt = 0; attempt < PACKAGE_LIST_ATTEMPTS; attempt++) {
        UINT32 count = 0, chars = 0, i;
        PWSTR *names;
        WCHAR *buffer;
        LONG rc;
        if (FindPackagesByPackageFamily(family, filter, &count, NULL, &chars, NULL, NULL) != ERROR_INSUFFICIENT_BUFFER ||
            count == 0 || chars == 0)
            return;
        names = (PWSTR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(PWSTR));
        buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, chars * sizeof(WCHAR));
        rc = names && buffer ? FindPackagesByPackageFamily(family, filter, &count, names, &chars, buffer, NULL)
                             : ERROR_NOT_ENOUGH_MEMORY;
        if (rc == ERROR_SUCCESS)
            for (i = 0; i < count; i++) Consider(names[i], best);
        if (names) HeapFree(GetProcessHeap(), 0, names);
        if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
        if (rc != ERROR_INSUFFICIENT_BUFFER) return;
    }
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
        if (Core_NamesClaudePackage(name)) Consider(name, best);
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
    IApplicationActivationManager *manager = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_LOCAL_SERVER,
                                  &IID_IApplicationActivationManager, (void **)&manager);
    if (FAILED(hr))
        hr = CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IApplicationActivationManager, (void **)&manager);
    if (FAILED(hr)) return hr;
    CoAllowSetForegroundWindow((IUnknown *)manager, NULL);
    hr = IApplicationActivationManager_ActivateApplication(manager, aumid, args, AO_NOERRORUI, pid);
    IApplicationActivationManager_Release(manager);
    return hr;
}

HRESULT Claude_Launch(const ClaudePackage *pkg, const Profile *profile, const WCHAR *url,
                      DWORD *pid, BOOL *withIdentity)
{
    WCHAR args[LAUNCH_ARGS_CCH];
    DWORD started = 0;
    HRESULT hr;

    if (pid) *pid = 0;
    if (withIdentity) *withIdentity = FALSE;
    if (!pkg->found) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    /* The stock profile never gets a flag and its folder is never created
     * here: that is exactly what the Start menu does. */
    if (!profile->isStock && !Util_EnsureDir(profile->dataDir)) {
        DWORD error = GetLastError();
        Util_Log(L"cannot create %s (error %lu)", profile->dataDir, error);
        return HRESULT_FROM_WIN32(error ? error : ERROR_PATH_NOT_FOUND);
    }
    if (!Core_BuildLaunchArgs(profile->isStock ? NULL : profile->dataDir, url, args, ARRAYSIZE(args))) {
        Util_Log(L"no command line for %s: its folder or the link cannot be passed safely", profile->folder);
        return E_INVALIDARG;
    }

    AllowSetForegroundWindow(ASFW_ANY);
    hr = Activate(pkg->aumid, args, &started);
    if (SUCCEEDED(hr)) {
        if (withIdentity) *withIdentity = TRUE;
    } else {
        Util_Log(L"activation of %s failed (0x%08lX); starting the exe directly", pkg->aumid, (unsigned long)hr);
        if (!Util_Spawn(pkg->exe, args, &started)) {
            DWORD error = GetLastError();
            Util_Log(L"could not start %s (error %lu)", pkg->exe, error);
            return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        }
        hr = S_OK;
    }
    if (pid) *pid = started;
    return hr;
}

/* ------------------------------------------------------- running profiles */

/* Chromium's single-instance lock: the running browser process of a profile
 * owns a message-only window of class Chrome_MessageWindow whose title is the
 * user data directory. A second launch finds it the same way. The next such
 * window after `after` (NULL: the first), with its title. */
static HWND NextInstanceWindow(HWND after, WCHAR *title, int cch)
{
    while ((after = FindWindowExW(HWND_MESSAGE, after, L"Chrome_MessageWindow", NULL)) != NULL)
        if (GetWindowTextW(after, title, cch) > 0) return after;
    return NULL;
}

void Claude_UpdateRunning(ProfileList *list)
{
    HWND window = NULL;
    WCHAR title[MAX_PATH];
    int i;

    for (i = 0; i < list->count; i++) {
        list->items[i].running = FALSE;
        list->items[i].pid = 0;
    }
    while ((window = NextInstanceWindow(window, title, ARRAYSIZE(title))) != NULL) {
        for (i = 0; i < list->count; i++) {
            if (!list->items[i].running && Core_PathEquals(title, list->items[i].dataDir)) {
                list->items[i].running = TRUE;
                GetWindowThreadProcessId(window, &list->items[i].pid);
                break;
            }
        }
    }
}

/* The single-instance window of the profile's Claude, NULL when none runs. */
static HWND InstanceWindowOf(const Profile *profile)
{
    HWND window = NULL;
    WCHAR title[MAX_PATH];
    while ((window = NextInstanceWindow(window, title, ARRAYSIZE(title))) != NULL)
        if (Core_PathEquals(title, profile->dataDir)) return window;
    return NULL;
}

/* `running` and `pid` of a profile read earlier, as they are right now. */
void Claude_RefreshRunning(Profile *profile)
{
    HWND window = InstanceWindowOf(profile);
    profile->running = window != NULL;
    profile->pid = 0;
    if (window) GetWindowThreadProcessId(window, &profile->pid);
}

/* The profile's Claude runs right now (it may have started or ended since
 * `profile` was read). */
BOOL Claude_IsRunning(const Profile *profile)
{
    return InstanceWindowOf(profile) != NULL;
}

typedef struct TopmostSearch {
    const ProfileList *list;
    int found;
} TopmostSearch;

static BOOL CALLBACK TopmostProc(HWND window, LPARAM parameter)
{
    TopmostSearch *search = (TopmostSearch *)parameter;
    DWORD pid = 0;
    int i;
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL) return TRUE;
    GetWindowThreadProcessId(window, &pid);
    for (i = 0; i < search->list->count; i++) {
        if (search->list->items[i].running && search->list->items[i].pid == pid) {
            search->found = i;
            return FALSE;
        }
    }
    return TRUE;
}

/* EnumWindows walks top-level windows in Z order: the first Claude window it
 * meets is the one the user touched last. */
int Claude_TopmostProfile(const ProfileList *list)
{
    TopmostSearch search;
    search.list = list;
    search.found = -1;
    EnumWindows(TopmostProc, (LPARAM)&search);
    return search.found;
}

/* ------------------------------------------------------------- sign-ins */

/* Claude writes a profile's logs to %LOCALAPPDATA%\<data folder name>\logs,
 * or to the package's LocalCache\Local when that folder did not exist yet. */
typedef enum LogCopy { LOG_IN_LOCAL_APPDATA, LOG_IN_PACKAGE_CACHE, LOG_COPIES } LogCopy;
#define LOG_TAIL_BYTES (256 * 1024)   /* the recent lines are at the end */

/* The end of one copy of the profile's main.log, or NULL. */
static char *ReadLog(const ClaudePackage *pkg, const Profile *profile, LogCopy copy, DWORD *len)
{
    WCHAR local[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    *len = 0;
    if (!Util_LocalAppData(local, ARRAYSIZE(local))) return NULL;
    if (copy == LOG_IN_LOCAL_APPDATA) {
        if (FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", local, profile->folder))) return NULL;
    } else if (!pkg || !pkg->found || !Core_PackageCachePath(local, pkg->family, L"Local", profile->folder, folder, ARRAYSIZE(folder))) {
        return NULL;
    }
    return SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\logs\\main.log", folder))
           ? Util_ReadFile(path, LOG_TAIL_BYTES, TRUE, len) : NULL;
}

/* Finds a time in a log: TRUE with `*when` (and a fact about it in `*flag`). */
typedef BOOL (*LogFinder)(const char *text, size_t len, SYSTEMTIME *when, BOOL *flag);

/* The latest time `find` gives in either copy of the profile's main.log, 0
 * for none; `*flag` (when not NULL) gets what it said of that one. */
static ULONGLONG LatestInLogs(const ClaudePackage *pkg, const Profile *profile, LogFinder find, BOOL *flag)
{
    ULONGLONG latest = 0;
    int copy;
    for (copy = 0; copy < LOG_COPIES; copy++) {
        DWORD len;
        SYSTEMTIME when;
        BOOL reportedFlag = FALSE;
        char *text = ReadLog(pkg, profile, (LogCopy)copy, &len);
        if (!text) continue;
        if (find(text, len, &when, &reportedFlag)) {
            ULONGLONG ticks = Core_SystemTimeTicks(&when);
            if (ticks > latest) {
                latest = ticks;
                if (flag) *flag = reportedFlag;
            }
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    return latest;
}

static BOOL FindSignInStart(const char *text, size_t len, SYSTEMTIME *when, BOOL *unused)
{
    (void)unused;
    return Core_LatestSignInStart(text, len, when);
}

BOOL Claude_LastSignInStart(const ClaudePackage *pkg, const Profile *profile, ULONGLONG *ticks)
{
    *ticks = LatestInLogs(pkg, profile, FindSignInStart, NULL);
    return *ticks != 0;
}

#define QUIT_RECENT_TICKS (5 * 60 * TICKS_PER_SECOND)

/* The profile's Claude, which has just exited, went down for an update: the
 * last quit line of its main.log is its updater's or Windows closing it for
 * a new package, and was written a moment ago (an older one means Claude
 * ended without logging why, a crash say). */
BOOL Claude_ClosedForUpdate(const ClaudePackage *pkg, const Profile *profile)
{
    BOOL forUpdate = FALSE;
    ULONGLONG latest = LatestInLogs(pkg, profile, Core_LastQuit, &forUpdate);
    return forUpdate && latest + QUIT_RECENT_TICKS >= Util_LocalNowTicks();
}
