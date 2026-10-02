/*
 * Per-profile taskbar buttons (part of the Taskbar Module: see LICENSE).
 *
 * Every Claude window carries Claude's package identity, so the taskbar puts
 * all profiles under one Claude button. The watcher (--watch <folder>), started
 * when Claude Desktop Profiles Manager opens a profile, gives that profile's
 * windows, through their shell property store, the AppUserModelID its
 * shortcuts carry with a relaunch command, name and badged icon: they get their
 * own button, and pinning it pins the profile. It colors the profile's
 * notification-area icon (tray.c), acts only on events, exits with Claude, and
 * after a Claude update opens the profile again (docs/HOW-IT-WORKS.md). A
 * profile opened again while its watcher is ending goes to that watcher. At
 * uninstall it gives the windows and the icon back to Claude.
 *
 * After a new badge, a pinned profile's button follows its pin
 * (taskbar-pin.c); a button showing one of the profile's shortcuts, with none
 * of its windows in a snap group, is told the shortcut changed; otherwise the
 * windows go through a temporary ID, which moves the button to the end.
 */
#include "app.h"
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>
#include <uiautomation.h>

#define WATCH_CLASS       L"ClaudeDesktopProfilesManagerWatch"   /* the watcher's window, titled with its folder */
#define WM_WATCH_REFRESH  (WM_APP + 1)   /* the badge or the windows' relaunch values changed (wParam: the shortcuts too) */
#define WM_WATCH_APPS     (WM_APP + 2)   /* the Apps folder changed */
#define WM_WATCH_TRAY     (WM_APP + 3)   /* the notification-area icon needs its color */
#define WM_WATCH_RETAGGED (WM_APP + 4)   /* the taskbar shows the temporary ID's button */
#define WM_WATCH_REOPENED (WM_APP + 5)   /* from a newer watcher: the profile opened again (wParam: its Claude's pid) */
#define TIMER_RETAG       1
#define TIMER_APPS        2
#define RETAG_MS          1500   /* without UI Automation: time for the taskbar to show the temporary ID's button */
#define RETAG_SAFETY_MS   5000   /* with it: in case its event never comes */
#define APPS_WAIT_MS      8000   /* at most, for the Apps folder to take the shortcuts' change before the taskbar is told */
#define START_WAIT_MS     90000  /* Claude takes a moment to start */
#define START_RECHECK_MS  100    /* one more look after its folder changed: its window follows at once */
#define UPDATE_WAIT_MS    120000 /* Windows installs the new package after closing Claude */
#define REOPEN_WAIT_MS    60000  /* Windows opens Claude again by itself (measured: 1 to 33 s) */
#define WATCH_STOP_MS     10000  /* includes a watcher's bounded notification-area calls */
#define WATCH_REGISTRATION_MUTEX L"Local\\ClaudeDesktopProfilesManager.WatcherRegistration"
#define MAX_LINKS         64
#define REG_ADVANCED      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"
#define INTERIM_SUFFIX    L".refresh"
#define BUTTON_ID_FORMAT  L"Appid: %s"   /* an ID's taskbar button, as UI Automation identifies it */

typedef struct WindowTag {
    WCHAR folder[FOLDER_CCH];
    WCHAR aumid[AUMID_CCH];
    WCHAR interimAumid[AUMID_CCH + ARRAYSIZE(INTERIM_SUFFIX)];   /* the temporary ID a new badge goes through */
    WCHAR claudeAumid[RTL_FIELD_SIZE(ClaudePackage, aumid) / sizeof(WCHAR)];   /* Claude's own package ID: where the window goes back */
    WCHAR command[MAX_PATH + FOLDER_CCH + 32];
    WCHAR name[LABEL_CCH + 16];
    WCHAR icon[MAX_PATH + 4];
    DWORD pid;
} WindowTag;

typedef WCHAR ShortcutPath[MAX_PATH];

/* The watcher's state: the WinEvent callbacks have no context parameter.
 * g_watch.pid is 0 while no Claude of the profile is watched. */
static WindowTag g_watch;
static ClaudePackage g_watchPkg;
static HWND g_watchWindow;
static UINT g_taskbarCreated;
static BOOL g_waitingForApps, g_trayScheduled;
static HANDLE g_started;                /* the profile's Claude has started */
static WCHAR g_startedDir[MAX_PATH];    /* ...the data folder it runs */
static HANDLE g_appsChanged;            /* the Apps folder changed */
static HANDLE g_reopened;               /* set while a newer start of the profile waits for this watcher... */
static DWORD g_reopenedPid;             /* ...with the Claude process started for it (0: unknown) */
/* In the manager, once an uninstall has stopped the watchers: their quit
 * request, asserted until this process exits so that no watcher starts after it. */
static HANDLE g_uninstalledQuit;

/* ------------------------------------------- the temporary ID's button */

/* A new taskbar button shows in UI Automation with an identifier naming its
 * ID: the temporary ID's button means the windows took that ID. */
static IUIAutomation *g_uia;
static IUIAutomationElement *g_taskbarElements[4];
static int g_taskbarElementCount;
static BOOL g_retagging;
static WCHAR g_retagButton[ARRAYSIZE(BUTTON_ID_FORMAT) + AUMID_CCH + ARRAYSIZE(INTERIM_SUFFIX)];
static SRWLOCK g_retagButtonLock = SRWLOCK_INIT;   /* read on a UI Automation thread */

static HRESULT STDMETHODCALLTYPE HandlerQueryInterface(IUIAutomationStructureChangedEventHandler *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IUIAutomationStructureChangedEventHandler)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE HandlerAddRef(IUIAutomationStructureChangedEventHandler *self)
{
    (void)self;
    return 2;   /* a static object */
}

static ULONG STDMETHODCALLTYPE HandlerRelease(IUIAutomationStructureChangedEventHandler *self)
{
    (void)self;
    return 1;
}

/* Called on a UI Automation thread: it only posts to the watcher. */
static HRESULT STDMETHODCALLTYPE HandlerEvent(IUIAutomationStructureChangedEventHandler *self, IUIAutomationElement *sender,
                                              enum StructureChangeType change, SAFEARRAY *runtimeId)
{
    BSTR id = NULL;
    BOOL retagged;
    (void)self;
    (void)runtimeId;
    if (change == StructureChangeType_ChildAdded && sender &&
        SUCCEEDED(IUIAutomationElement_get_CurrentAutomationId(sender, &id)) && id) {
        AcquireSRWLockShared(&g_retagButtonLock);
        retagged = g_retagButton[0] && Core_EqualsI(id, g_retagButton);
        ReleaseSRWLockShared(&g_retagButtonLock);
        if (retagged) PostMessageW(g_watchWindow, WM_WATCH_RETAGGED, 0, 0);
        SysFreeString(id);
    }
    return S_OK;
}

static IUIAutomationStructureChangedEventHandlerVtbl g_handlerVtbl = { HandlerQueryInterface, HandlerAddRef, HandlerRelease, HandlerEvent };
static IUIAutomationStructureChangedEventHandler g_handler = { &g_handlerVtbl };

static void SetRetagButton(const WCHAR *automationId)
{
    AcquireSRWLockExclusive(&g_retagButtonLock);
    if (FAILED(StringCchCopyW(g_retagButton, ARRAYSIZE(g_retagButton), automationId))) g_retagButton[0] = 0;
    ReleaseSRWLockExclusive(&g_retagButtonLock);
}

static void StopWatchingButtons(void)
{
    while (g_taskbarElementCount > 0) {
        IUIAutomationElement *taskbar = g_taskbarElements[--g_taskbarElementCount];
        IUIAutomation_RemoveStructureChangedEventHandler(g_uia, taskbar, &g_handler);
        IUIAutomationElement_Release(taskbar);
    }
    SetRetagButton(L"");
}

/* Watches every taskbar (one per monitor) for the temporary ID's button.
 * FALSE when UI Automation cannot. */
static BOOL WatchForButton(const WCHAR *interim)
{
    const WCHAR *classes[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" };
    WCHAR automationId[ARRAYSIZE(g_retagButton)];
    HWND taskbar;
    size_t c;
    StopWatchingButtons();
    if (FAILED(StringCchPrintfW(automationId, ARRAYSIZE(automationId), BUTTON_ID_FORMAT, interim))) return FALSE;
    SetRetagButton(automationId);
    if (!g_uia && FAILED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&g_uia)))
        return FALSE;
    for (c = 0; c < ARRAYSIZE(classes); c++) {
        for (taskbar = NULL;
             g_taskbarElementCount < (int)ARRAYSIZE(g_taskbarElements) && (taskbar = FindWindowExW(NULL, taskbar, classes[c], NULL)) != NULL;) {
            IUIAutomationElement *element = NULL;
            if (FAILED(IUIAutomation_ElementFromHandle(g_uia, taskbar, &element)) || !element) continue;
            if (SUCCEEDED(IUIAutomation_AddStructureChangedEventHandler(g_uia, element, TreeScope_Subtree, NULL, &g_handler)))
                g_taskbarElements[g_taskbarElementCount++] = element;
            else
                IUIAutomationElement_Release(element);
        }
    }
    return g_taskbarElementCount > 0;
}

static void SetValue(IPropertyStore *store, REFPROPERTYKEY key, const WCHAR *value)
{
    PROPVARIANT variant;
    PropVariantInit(&variant);
    if (value) {
        size_t bytes = (wcslen(value) + 1) * sizeof(WCHAR);
        variant.vt = VT_LPWSTR;
        variant.pwszVal = (LPWSTR)CoTaskMemAlloc(bytes);
        if (!variant.pwszVal) return;
        memcpy(variant.pwszVal, value, bytes);
    }
    IPropertyStore_SetValue(store, key, &variant);   /* VT_EMPTY empties it (and reports an error) */
    PropVariantClear(&variant);
}

/* The window's `key` is `value`: `exactly` for text the user reads, ignoring
 * case for IDs and paths. */
static BOOL HasValue(IPropertyStore *store, REFPROPERTYKEY key, const WCHAR *value, BOOL exactly)
{
    PROPVARIANT variant;
    BOOL same;
    PropVariantInit(&variant);
    if (FAILED(IPropertyStore_GetValue(store, key, &variant))) return FALSE;
    same = variant.vt == VT_LPWSTR && variant.pwszVal &&
           (exactly ? wcscmp(variant.pwszVal, value) == 0 : Core_EqualsI(variant.pwszVal, value));
    PropVariantClear(&variant);
    return same;
}

static BOOL HasId(IPropertyStore *store, const WCHAR *aumid)
{
    return HasValue(store, &PKEY_AppUserModel_ID, aumid, FALSE);
}

/* The window shows the tag's relaunch icon and name (a rename may change only
 * the name's case). */
static BOOL RelaunchCurrent(IPropertyStore *store, const WindowTag *tag)
{
    return HasValue(store, &PKEY_AppUserModel_RelaunchIconResource, tag->icon, FALSE) &&
           HasValue(store, &PKEY_AppUserModel_RelaunchDisplayNameResource, tag->name, TRUE);
}

/* The windows that get a taskbar button: top level, no owner, not a tool window. */
static BOOL IsButtonWindow(HWND window)
{
    return IsWindow(window) && GetAncestor(window, GA_ROOT) == window && GetWindow(window, GW_OWNER) == NULL &&
           !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
}

typedef enum TagMode {
    TAG_IF_NEEDED,   /* a window without the profile's ID gets it */
    TAG_RELAUNCH,    /* a window with the profile's ID gets the current relaunch values */
    TAG_REFRESH,     /* new relaunch values under a temporary ID... */
    TAG_FINISH,      /* ...then, once the taskbar shows it, the profile's ID again */
    TAG_REMOVE       /* back to Claude */
} TagMode;

static void SetRelaunch(IPropertyStore *store, const WindowTag *tag)
{
    /* Read by the taskbar when the ID changes: they go first. */
    SetValue(store, &PKEY_AppUserModel_RelaunchIconResource, tag->icon);
    SetValue(store, &PKEY_AppUserModel_RelaunchDisplayNameResource, tag->name);
    SetValue(store, &PKEY_AppUserModel_RelaunchCommand, tag->command);
}

/* TRUE when the window carries the profile's ID, or the temporary one while
 * it goes through it, with relaunch values other than the current ones
 * (TAG_RELAUNCH: then gets them): the taskbar does not read them by itself. */
static BOOL TagWindow(HWND window, const WindowTag *tag, TagMode mode)
{
    IPropertyStore *store = NULL;
    const WCHAR *interim = tag->interimAumid;
    BOOL stale = FALSE;
    if (!IsButtonWindow(window)) return FALSE;
    if (FAILED(SHGetPropertyStoreForWindow(window, &IID_IPropertyStore, (void **)&store))) return FALSE;
    switch (mode) {
    case TAG_REMOVE:
        if (HasId(store, tag->aumid) || HasId(store, interim)) {
            SetValue(store, &PKEY_AppUserModel_RelaunchCommand, NULL);
            SetValue(store, &PKEY_AppUserModel_RelaunchDisplayNameResource, NULL);
            SetValue(store, &PKEY_AppUserModel_RelaunchIconResource, NULL);
            if (tag->claudeAumid[0]) SetValue(store, &PKEY_AppUserModel_ID, tag->claudeAumid);
        }
        break;
    case TAG_REFRESH:
        SetRelaunch(store, tag);
        SetValue(store, &PKEY_AppUserModel_ID, interim);
        break;
    case TAG_FINISH:
        if (HasId(store, interim)) SetValue(store, &PKEY_AppUserModel_ID, tag->aumid);
        break;
    case TAG_RELAUNCH:
        if ((HasId(store, tag->aumid) || (g_retagging && HasId(store, interim))) && !RelaunchCurrent(store, tag)) {
            SetRelaunch(store, tag);
            stale = TRUE;
        }
        break;
    default:
        if (HasId(store, tag->aumid)) {
            stale = !RelaunchCurrent(store, tag);
        } else if (!g_retagging || !HasId(store, interim)) {
            /* A temporary ID left by a watcher stopped mid-way ends here too. */
            SetRelaunch(store, tag);
            SetValue(store, &PKEY_AppUserModel_ID, tag->aumid);
        }
        break;
    }
    IPropertyStore_Release(store);
    return stale;
}

typedef struct TagAllContext {
    const WindowTag *tag;
    TagMode mode;
    BOOL stale;
} TagAllContext;

static BOOL CALLBACK TagAllProc(HWND window, LPARAM parameter)
{
    TagAllContext *context = (TagAllContext *)parameter;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == context->tag->pid && TagWindow(window, context->tag, context->mode)) context->stale = TRUE;
    return TRUE;
}

static BOOL TagProcessWindows(const WindowTag *tag, TagMode mode)
{
    TagAllContext context;
    context.tag = tag;
    context.mode = mode;
    context.stale = FALSE;
    EnumWindows(TagAllProc, (LPARAM)&context);
    return context.stale;
}

/* The installed copy, which shortcuts and pins start; this exe when there is none. */
static void ProgramExe(WCHAR *out, size_t cch)
{
    if (!Util_InstallExe(out, cch) || !Util_FileExists(out)) Util_SelfExe(out, cch);
}

/* The tag of the profile's windows: its current name, color and icon. */
static BOOL PrepareTag(const ClaudePackage *pkg, const Profile *p, WindowTag *tag)
{
    WCHAR exe[MAX_PATH], icon[MAX_PATH];
    ZeroMemory(tag, sizeof *tag);
    StringCchCopyW(tag->folder, ARRAYSIZE(tag->folder), p->folder);
    Core_ProfileAumid(p->folder, tag->aumid, ARRAYSIZE(tag->aumid));
    StringCchPrintfW(tag->interimAumid, ARRAYSIZE(tag->interimAumid), L"%s" INTERIM_SUFFIX, tag->aumid);
    StringCchCopyW(tag->claudeAumid, ARRAYSIZE(tag->claudeAumid), pkg->aumid);
    ProgramExe(exe, ARRAYSIZE(exe));
    StringCchPrintfW(tag->command, ARRAYSIZE(tag->command), L"\"%s\" --launch \"%s\"", exe, p->folder);
    StringCchPrintfW(tag->name, ARRAYSIZE(tag->name), L"Claude (%s)", p->name);
    if (!Icons_Ensure(pkg, p, icon, ARRAYSIZE(icon))) return FALSE;
    StringCchPrintfW(tag->icon, ARRAYSIZE(tag->icon), L"%s,0", icon);
    tag->pid = p->pid;
    return TRUE;
}

/* A new name, color or badge: the profile's watcher, if it has one, updates
 * its windows and icon (it ignores the notice while no Claude of the profile
 * runs). `linksChanged`: its shortcuts changed too. */
void Taskbar_Refresh(const Profile *profile, BOOL linksChanged)
{
    HWND watcher = FindWindowW(WATCH_CLASS, profile->folder);
    if (watcher) PostMessageW(watcher, WM_WATCH_REFRESH, (WPARAM)linksChanged, 0);
}

static BOOL WatchMutexName(const WCHAR *folder, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, WATCH_MUTEX_PREFIX L"%08lX", (unsigned long)Core_HashIgnoringCase(folder)));
}

BOOL Taskbar_IsWatched(const Profile *profile)
{
    WCHAR name[80];
    HANDLE mutex;
    if (!WatchMutexName(profile->folder, name, ARRAYSIZE(name))) return FALSE;
    mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (mutex) CloseHandle(mutex);
    return mutex != NULL;
}

/* Starts the watcher for a profile just opened; `profile->pid` is its Claude
 * process. A watcher already running for it takes this start over. */
void Taskbar_Watch(const Profile *profile)
{
    WCHAR exe[MAX_PATH], args[FOLDER_CCH + 32];
    ProgramExe(exe, ARRAYSIZE(exe));
    if (FAILED(StringCchPrintfW(args, ARRAYSIZE(args), L"--watch \"%s\" %lu", profile->folder, (unsigned long)profile->pid))) return;
    if (!Util_Spawn(exe, args, NULL)) Util_Log(L"could not start the taskbar watcher for %s (error %lu)", profile->folder, GetLastError());
}

typedef struct WatcherStops {
    HANDLE process[MAX_PROFILES];
    DWORD pid[MAX_PROFILES];
    DWORD count;
    DWORD error;
} WatcherStops;

/* Registration exposes the stop events and watcher window together, so an
 * initializing watcher cannot be omitted from a stop operation; a watcher
 * ends under it too, so a newer watcher of its profile is never lost. */
static HANDLE LockWatcherRegistration(void)
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, WATCH_REGISTRATION_MUTEX);
    DWORD wait, error;
    if (!mutex) return NULL;
    wait = WaitForSingleObject(mutex, WATCH_STOP_MS);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return mutex;
    error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    CloseHandle(mutex);
    SetLastError(error);
    return NULL;
}

static void UnlockWatcherRegistration(HANDLE mutex)
{
    if (!mutex) return;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

/* Handles of a watcher's wait for the registration lock before it ends, by index. */
enum { ENDING_QUIT, ENDING_HANDOVER, ENDING_REGISTRATION, ENDING_HANDLE_COUNT };

/* The registration lock for a watcher about to end. NULL when a stop request
 * comes first (whoever stops the watchers holds the lock) or the lock cannot
 * be had. */
static HANDLE LockRegistrationToEnd(HANDLE quit, HANDLE handover)
{
    HANDLE handles[ENDING_HANDLE_COUNT];
    DWORD wait;
    handles[ENDING_QUIT] = quit;
    handles[ENDING_HANDOVER] = handover;
    handles[ENDING_REGISTRATION] = CreateMutexW(NULL, FALSE, WATCH_REGISTRATION_MUTEX);
    if (!handles[ENDING_REGISTRATION]) {
        Util_Log(L"taskbar watcher for %s: could not lock watcher registration to end (error %lu)", g_watch.folder, GetLastError());
        return NULL;
    }
    wait = WaitForMultipleObjects(ENDING_HANDLE_COUNT, handles, FALSE, WATCH_STOP_MS);
    if (wait == WAIT_OBJECT_0 + ENDING_REGISTRATION || wait == WAIT_ABANDONED_0 + ENDING_REGISTRATION)
        return handles[ENDING_REGISTRATION];
    if (wait == WAIT_TIMEOUT || wait == WAIT_FAILED)
        Util_Log(L"taskbar watcher for %s: could not lock watcher registration to end (error %lu)", g_watch.folder,
                 wait == WAIT_FAILED ? GetLastError() : (DWORD)ERROR_TIMEOUT);
    CloseHandle(handles[ENDING_REGISTRATION]);
    return NULL;
}

static BOOL CALLBACK CollectWatcher(HWND window, LPARAM context)
{
    WatcherStops *stops = (WatcherStops *)context;
    WCHAR name[80];
    DWORD pid = 0, i;
    HANDLE process;
    if (!GetClassNameW(window, name, ARRAYSIZE(name)) ||
        CompareStringOrdinal(name, -1, WATCH_CLASS, -1, FALSE) != CSTR_EQUAL) return TRUE;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return TRUE;
    for (i = 0; i < stops->count; i++)
        if (stops->pid[i] == pid) return TRUE;
    if (stops->count == ARRAYSIZE(stops->process)) {
        stops->error = ERROR_TOO_MANY_OPEN_FILES;
        return FALSE;
    }
    process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        DWORD error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER) stops->error = error;   /* gone meanwhile */
        return TRUE;
    }
    stops->pid[stops->count] = pid;
    stops->process[stops->count++] = process;
    return TRUE;
}

/* On success the stop request stays asserted until the watchers' processes
 * have exited: a busy watcher finishes giving its windows and icon back
 * before the caller removes their shortcuts. An uninstall's stays asserted
 * until this process exits, made when no watcher runs, so that none starts
 * after it. A stop that fails is withdrawn, or never made when the watchers
 * cannot all be found: the caller gives up, and a watcher started later must
 * not see it. */
BOOL Taskbar_StopWatchers(BOOL giveBack)
{
    WatcherStops stops;
    HANDLE registration = LockWatcherRegistration(), stopEvent;
    DWORD i, wait, openError;
    BOOL ok;
    if (!registration) {
        Util_Log(L"could not lock watcher registration to stop the watchers (error %lu)", GetLastError());
        return FALSE;
    }
    stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, giveBack ? WATCH_QUIT_EVENT : WATCH_HANDOVER_EVENT);
    openError = stopEvent ? ERROR_SUCCESS : GetLastError();
    if (!stopEvent && openError == ERROR_FILE_NOT_FOUND && giveBack) {
        stopEvent = CreateEventW(NULL, TRUE, FALSE, WATCH_QUIT_EVENT);
        if (!stopEvent) {
            Util_Log(L"no watcher runs, but none can be kept from starting after the uninstall (error %lu)", GetLastError());
            UnlockWatcherRegistration(registration);
            return TRUE;
        }
    }
    if (!stopEvent) {
        UnlockWatcherRegistration(registration);
        if (openError == ERROR_FILE_NOT_FOUND) return TRUE;
        Util_Log(L"could not open the watcher stop event (error %lu)", openError);
        return FALSE;
    }
    ZeroMemory(&stops, sizeof stops);
    if (!EnumWindows(CollectWatcher, (LPARAM)&stops) && !stops.error) {
        DWORD error = GetLastError();
        stops.error = error ? error : ERROR_GEN_FAILURE;
    }
    ok = !stops.error && SetEvent(stopEvent);
    if (!ok && !stops.error) stops.error = GetLastError();
    if (ok && stops.count) {
        wait = WaitForMultipleObjects(stops.count, stops.process, TRUE, WATCH_STOP_MS);
        if (wait >= WAIT_OBJECT_0 + stops.count) stops.error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    }
    ok = ok && !stops.error;
    if (ok && giveBack) {
        if (g_uninstalledQuit) CloseHandle(g_uninstalledQuit);
        g_uninstalledQuit = stopEvent;
        stopEvent = NULL;
    } else if (!ResetEvent(stopEvent) && ok) {
        stops.error = GetLastError();
        ok = FALSE;
    }
    for (i = 0; i < stops.count; i++) CloseHandle(stops.process[i]);
    if (stopEvent) CloseHandle(stopEvent);
    UnlockWatcherRegistration(registration);
    if (!ok) Util_Log(L"watchers did not finish stopping (error %lu)", stops.error);
    return ok;
}

/* The notification-area icon after an event, painted once Claude has drawn
 * its own (Tray_Apply waits for Claude's thread). Events in a burst paint it once. */
static void ScheduleTray(void)
{
    if (g_watchWindow && !g_trayScheduled) g_trayScheduled = PostMessageW(g_watchWindow, WM_WATCH_TRAY, 0, 0);
}

static void PaintTray(void)
{
    ProfileList list;
    int i;
    g_trayScheduled = FALSE;
    Profiles_Load(&list, &g_watchPkg);
    i = Profiles_Find(&list, g_watch.folder);
    if (i < 0) return;
    list.items[i].running = TRUE;
    list.items[i].pid = g_watch.pid;
    Tray_Apply(&g_watchPkg, &list.items[i]);
}

/* The profile's tag and Claude's package, read again: the name, color or
 * badge may have changed while it runs. */
static void ReloadTag(void)
{
    ProfileList list;
    ClaudePackage pkg;
    int i;
    if (!Claude_FindPackage(&pkg)) return;
    Profiles_Load(&list, &pkg);
    if ((i = Profiles_Find(&list, g_watch.folder)) >= 0) {
        WindowTag fresh;
        list.items[i].pid = g_watch.pid;
        if (PrepareTag(&pkg, &list.items[i], &fresh)) g_watch = fresh;
        g_watchPkg = pkg;
    }
}

/* The taskbar shows snap groups on its buttons (on by default). */
static BOOL SnapGroupsShown(void)
{
    BOOL arranging = FALSE;
    DWORD shown = 1;
    if (!SystemParametersInfoW(SPI_GETWINARRANGING, 0, &arranging, 0) || !arranging) return FALSE;
    return !Util_RegGetDword(HKEY_CURRENT_USER, REG_ADVANCED, L"EnableTaskGroups", &shown) || shown != 0;
}

/* The window belongs to a snap group. */
static BOOL InSnapGroup(HWND window)
{
    WCHAR path[160], group[64], windowName[16];
    DWORD session = 0, i, cch;
    HKEY key, sub;
    LSTATUS rc;
    BOOL hit = FALSE;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\%lu\\TaskGroups", session)) ||
        FAILED(StringCchPrintfW(windowName, ARRAYSIZE(windowName), L"%lu", (unsigned long)(ULONG_PTR)window)) ||
        RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    for (i = 0; !hit; i++) {
        cch = ARRAYSIZE(group);
        rc = RegEnumKeyExW(key, i, group, &cch, NULL, NULL, NULL, NULL);
        if (rc == ERROR_MORE_DATA) continue;   /* too long for a group's name */
        if (rc != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(key, group, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        hit = Util_RegKeyExists(sub, windowName);
        RegCloseKey(sub);
    }
    RegCloseKey(key);
    return hit;
}

static BOOL CALLBACK SnappedProc(HWND window, LPARAM parameter)
{
    BOOL *snapped = (BOOL *)parameter;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == g_watch.pid && IsButtonWindow(window) && InSnapGroup(window)) *snapped = TRUE;
    return !*snapped;
}

static BOOL AnyWindowSnapped(void)
{
    BOOL snapped = FALSE;
    if (SnapGroupsShown()) EnumWindows(SnappedProc, (LPARAM)&snapped);
    return snapped;
}

/* The profile's shortcuts the button can show: on a desktop or in a Start
 * menu, carrying the profile's ID. A list to free with HeapFree, or NULL. */
static ShortcutPath *ButtonShortcuts(DWORD *count)
{
    const GUID *roots[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop, &FOLDERID_StartMenu, &FOLDERID_CommonStartMenu };
    WCHAR rootDirs[ARRAYSIZE(roots)][MAX_PATH];
    ShortcutPath *paths;
    DWORD i, n, kept = 0;
    size_t r;
    *count = 0;
    paths = (ShortcutPath *)HeapAlloc(GetProcessHeap(), 0, MAX_LINKS * sizeof *paths);
    if (!paths) return NULL;
    for (r = 0; r < ARRAYSIZE(roots); r++)
        if (!Util_KnownFolder(roots[r], rootDirs[r], ARRAYSIZE(rootDirs[r]))) rootDirs[r][0] = 0;
    n = Shortcut_ListOurs(g_watch.folder, paths, MAX_LINKS);
    for (i = 0; i < n; i++) {
        BOOL inRoot = FALSE;
        for (r = 0; r < ARRAYSIZE(roots) && !inRoot; r++) inRoot = rootDirs[r][0] && Core_PathUnder(paths[i], rootDirs[r]);
        if (inRoot && Shortcut_HasAppId(paths[i], g_watch.aumid)) {
            if (kept != i) StringCchCopyW(paths[kept], MAX_PATH, paths[i]);
            kept++;
        }
    }
    *count = kept;
    return paths;
}

/* The taskbar reloads the button's shortcut. */
static void NotifyShortcuts(void)
{
    ShortcutPath *paths;
    DWORD i, count;
    g_waitingForApps = FALSE;
    KillTimer(g_watchWindow, TIMER_APPS);
    paths = ButtonShortcuts(&count);
    for (i = 0; i < count; i++) TaskbarPin_TellShortcutChanged(paths[i]);
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
}

static BOOL HasButtonShortcut(void)
{
    DWORD count;
    ShortcutPath *paths = ButtonShortcuts(&count);
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    return count > 0;
}

static void FinishRetag(BOOL shown)
{
    KillTimer(g_watchWindow, TIMER_RETAG);
    if (!g_retagging) return;
    g_retagging = FALSE;
    StopWatchingButtons();
    TagProcessWindows(&g_watch, TAG_FINISH);
    Util_Log(L"%s has its own taskbar button again (%s)", g_watch.folder, shown ? L"its button was shown" : L"after a delay");
}

/* A new badge. */
static void StartRefresh(BOOL linksChanged)
{
    BOOL stale;
    ReloadTag();
    stale = TagProcessWindows(&g_watch, TAG_RELAUNCH);
    ScheduleTray();
    /* A retag under way ends with the profile's ID again: the taskbar reads
     * the new values then. */
    if (g_retagging || (!stale && !linksChanged)) return;
    if (TaskbarPin_HasAppId(g_watch.aumid)) return;
    if (!AnyWindowSnapped() && HasButtonShortcut()) {
        g_waitingForApps = TRUE;
        SetTimer(g_watchWindow, TIMER_APPS, APPS_WAIT_MS, NULL);
        return;
    }
    /* The profile's ID comes back once the taskbar shows the temporary ID's
     * button, watched before the windows take that ID. */
    g_retagging = TRUE;
    SetTimer(g_watchWindow, TIMER_RETAG, WatchForButton(g_watch.interimAumid) ? RETAG_SAFETY_MS : RETAG_MS, NULL);
    TagProcessWindows(&g_watch, TAG_REFRESH);
}

static LRESULT CALLBACK WatchProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_WATCH_REOPENED) {
        g_reopenedPid = (DWORD)wParam;
        if (g_reopened) SetEvent(g_reopened);
        return 0;
    }
    if (!g_watch.pid && (message == WM_WATCH_TRAY || message == WM_WATCH_REFRESH)) {   /* no Claude to paint */
        if (message == WM_WATCH_TRAY) g_trayScheduled = FALSE;
        return 0;
    }
    if (message == WM_WATCH_TRAY) {
        PaintTray();
        return 0;
    }
    if ((message == WM_TIMER && wParam == TIMER_RETAG) || message == WM_WATCH_RETAGGED) {
        FinishRetag(message == WM_WATCH_RETAGGED);
        return 0;
    }
    if ((message == WM_TIMER && wParam == TIMER_APPS) || message == WM_WATCH_APPS) {
        if (message == WM_WATCH_APPS) {
            PIDLIST_ABSOLUTE *pidls = NULL;
            LONG event = 0;
            HANDLE lock = SHChangeNotification_Lock((HANDLE)wParam, (DWORD)lParam, &pidls, &event);
            if (lock) SHChangeNotification_Unlock(lock);
            if (g_appsChanged) SetEvent(g_appsChanged);   /* a new Claude package shows there too */
        }
        if (g_waitingForApps) NotifyShortcuts();
        else if (message == WM_TIMER) KillTimer(window, TIMER_APPS);
        return 0;
    }
    if (message == WM_WATCH_REFRESH) {
        StartRefresh((BOOL)wParam);
        return 0;
    }
    /* A new display scale changes the notification area's icon size. This
     * hidden window gets no WM_DPICHANGED for it (measured), but
     * WM_DISPLAYCHANGE; the size is read from the taskbar's scale. */
    if ((g_taskbarCreated && message == g_taskbarCreated) || message == WM_DISPLAYCHANGE ||
        (message == WM_SETTINGCHANGE && lParam && Core_EqualsI((const WCHAR *)lParam, L"ImmersiveColorSet"))) {
        ScheduleTray();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/* A hidden top-level window: broadcasts only reach those. NULL, with the
 * error set, when it cannot be made. */
static HWND CreateWatchWindow(const WCHAR *folder)
{
    WNDCLASSEXW windowClass;
    ZeroMemory(&windowClass, sizeof windowClass);
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = WatchProc;
    windowClass.hInstance = g_hInst;
    windowClass.lpszClassName = WATCH_CLASS;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    return CreateWindowExW(WS_EX_TOOLWINDOW, WATCH_CLASS, folder, WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
}

/* Claude created its notification-area icon (the window that holds it). */
static void CALLBACK TrayHostCreated(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)thread; (void)time;
    if (object == OBJID_WINDOW && child == CHILDID_SELF && window && Tray_IsHost(window)) ScheduleTray();
}

/* A window of the profile's Claude was shown: it gets the profile's ID (the
 * tag is kept current by refresh notices). */
static void CALLBACK WindowShown(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)thread; (void)time;
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !IsButtonWindow(window)) return;
    if (TagWindow(window, &g_watch, TAG_IF_NEEDED)) PostMessageW(g_watchWindow, WM_WATCH_REFRESH, 0, 0);
}

/* Waits for one of `handles`, at most `ms`, handling the watcher's messages
 * and WinEvents meanwhile: the index of the handle signaled, WAIT_TIMEOUT or
 * WAIT_FAILED. */
static DWORD PumpUntil(const HANDLE *handles, DWORD count, DWORD ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        DWORD left = INFINITE, wait;
        MSG msg;
        if (ms != INFINITE) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) return WAIT_TIMEOUT;
            left = (DWORD)(deadline - now);
        }
        wait = MsgWaitForMultipleObjects(count, handles, FALSE, left, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 + count) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        if (wait < WAIT_OBJECT_0 + count) return wait - WAIT_OBJECT_0;
        return wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : WAIT_FAILED;
    }
}

/* A process created its Chrome_MessageWindow, titled with its data folder
 * (see Claude_UpdateRunning): the profile's Claude has started. */
static void CALLBACK MessageWindowCreated(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    WCHAR windowClass[32], title[MAX_PATH];
    (void)hook; (void)event; (void)thread; (void)time;
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !window || !GetClassNameW(window, windowClass, ARRAYSIZE(windowClass)) ||
        CompareStringOrdinal(windowClass, -1, L"Chrome_MessageWindow", -1, FALSE) != CSTR_EQUAL)
        return;
    if (GetWindowTextW(window, title, ARRAYSIZE(title)) > 0 && Core_PathEquals(title, g_startedDir)) SetEvent(g_started);
}

/* Every process's window creations, where the profile's Claude shows its
 * start when nothing narrower can be watched. */
static HWINEVENTHOOK WatchEveryWindowCreation(const WCHAR *folder)
{
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, MessageWindowCreated, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) Util_Log(L"taskbar watcher for %s: window creations cannot be watched (error %lu)", folder, GetLastError());
    return hook;
}

/* Handles of the wait for the profile's start, by index. */
enum { STARTUP_LOOK, STARTUP_QUIT, STARTUP_HANDOVER, STARTUP_LAUNCHED_EXIT, STARTUP_HANDLE_COUNT };

/* Waits, at most `ms`, for the profile's Claude to run: its index in `list`,
 * or -1 when it does not run in time or the profile is gone (logged unless
 * `ms` is 0, a single look); -1 with `interrupted` when the watcher is told to
 * stop or the wait fails (logged).
 * With the `pid` of the Claude just started, the window creations of that
 * process are watched and its exit ends the wait, after one more look.
 * Without it (Windows opened Claude itself), or when that process cannot be
 * watched, the profile's folder is: a starting Claude creates its lock file
 * there right before its window and keeps writing under it, and each change
 * brings a look, then one more a moment later. Only a folder that cannot be
 * watched leaves every process's window creations to watch. */
static int WaitForProfile(const WCHAR *folder, DWORD pid, ProfileList *list, HANDLE quit, HANDLE handover, DWORD ms,
                          BOOL *interrupted)
{
    HANDLE handles[STARTUP_HANDLE_COUNT], process = NULL, change = INVALID_HANDLE_VALUE;
    HWINEVENTHOOK hook = NULL;
    ULONGLONG deadline = GetTickCount64() + ms;
    DWORD count = STARTUP_LAUNCHED_EXIT, recheckMs = INFINITE, openError;
    BOOL exited = FALSE;
    int i;
    if (interrupted) *interrupted = FALSE;
    Profiles_Load(list, &g_watchPkg);
    i = Profiles_Find(list, folder);
    if (i < 0) Util_Log(L"taskbar watcher for %s: the profile no longer exists", folder);
    if (i < 0 || list->items[i].running) return i;
    if (!ms) return -1;
    handles[STARTUP_LOOK] = g_started;
    handles[STARTUP_QUIT] = quit;
    handles[STARTUP_HANDOVER] = handover;
    ResetEvent(g_started);
    StringCchCopyW(g_startedDir, ARRAYSIZE(g_startedDir), list->items[i].dataDir);
    if (pid) {
        process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        openError = process ? ERROR_SUCCESS : GetLastError();
        if (process) {
            handles[count++] = process;
            hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, MessageWindowCreated, pid, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (!hook)
                Util_Log(L"taskbar watcher for %s: the windows of Claude pid %lu cannot be watched (error %lu): its folder is watched instead",
                         folder, pid, GetLastError());
        } else if (openError == ERROR_INVALID_PARAMETER) {
            exited = TRUE;                  /* already gone */
        } else {
            Util_Log(L"taskbar watcher for %s: Claude pid %lu cannot be waited for (error %lu): its folder is watched instead",
                     folder, pid, openError);
        }
    }
    if (!exited && !hook) {
        change = FindFirstChangeNotificationW(list->items[i].storageDir, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (change != INVALID_HANDLE_VALUE) {
            handles[STARTUP_LOOK] = change;
        } else {
            Util_Log(L"taskbar watcher for %s: its folder cannot be watched (error %lu)", folder, GetLastError());
            hook = WatchEveryWindowCreation(folder);
        }
    }
    /* Armed before looking, so a start in between still counts. */
    for (;;) {
        ULONGLONG now;
        DWORD wait;
        if (Claude_IsRunning(&list->items[i])) break;
        now = GetTickCount64();
        if (exited) {
            Util_Log(L"taskbar watcher for %s: Claude pid %lu ended without starting the profile", folder, pid);
            i = -1;
            break;
        }
        if (now >= deadline) {
            Util_Log(L"taskbar watcher for %s: its Claude did not start within %lu s", folder, ms / 1000);
            i = -1;
            break;
        }
        wait = PumpUntil(handles, count, (DWORD)min(deadline - now, (ULONGLONG)recheckMs));
        recheckMs = INFINITE;
        if (wait == STARTUP_LOOK && change != INVALID_HANDLE_VALUE) {
            if (FindNextChangeNotification(change)) {
                recheckMs = START_RECHECK_MS;
            } else {
                Util_Log(L"taskbar watcher for %s: its folder can no longer be watched (error %lu)", folder, GetLastError());
                FindCloseChangeNotification(change);
                change = INVALID_HANDLE_VALUE;
                handles[STARTUP_LOOK] = g_started;
                hook = WatchEveryWindowCreation(folder);
            }
        } else if (wait == STARTUP_LAUNCHED_EXIT) {
            exited = TRUE;
        } else if (wait == STARTUP_QUIT || wait == STARTUP_HANDOVER || wait == WAIT_FAILED) {
            if (wait == WAIT_FAILED) Util_Log(L"taskbar watcher for %s: waiting for Claude to start failed (error %lu)", folder, GetLastError());
            if (interrupted) *interrupted = TRUE;
            i = -1;
            break;
        }
    }
    if (hook) UnhookWinEvent(hook);
    if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
    if (process) CloseHandle(process);
    if (i < 0) return -1;
    Profiles_Load(list, &g_watchPkg);
    i = Profiles_Find(list, folder);
    if (i >= 0 && list->items[i].running) return i;
    Util_Log(L"taskbar watcher for %s: its Claude was gone again at once", folder);
    return -1;
}

/* Handles of the watch of the profile's running Claude, by index. */
enum { RUNNING_CLAUDE_EXIT, RUNNING_QUIT, RUNNING_HANDOVER, RUNNING_HANDLE_COUNT };

/* Starts handed to this watcher and still queued are recorded. */
static void TakeQueuedStarts(void)
{
    MSG message;
    while (PeekMessageW(&message, g_watchWindow, WM_WATCH_REOPENED, WM_WATCH_REOPENED, PM_REMOVE)) DispatchMessageW(&message);
}

/* Watches the profile's running Claude (g_watch) until it exits: TRUE then,
 * also when it exited before it could be watched; FALSE when the watcher is
 * told to stop or cannot watch it. `ran` gets the package that Claude runs. */
static BOOL WatchClaude(HANDLE quit, HANDLE handover, WCHAR *ran, UINT32 ranCch)
{
    HANDLE process, handles[RUNNING_HANDLE_COUNT];
    HWINEVENTHOOK show, create;
    UINT32 cch = ranCch;
    DWORD wait, waitError;

    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_watch.pid);
    if (!process) process = OpenProcess(SYNCHRONIZE, FALSE, g_watch.pid);
    if (!process) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            Util_Log(L"taskbar watcher for %s: Claude pid %lu exited before it could be watched", g_watch.folder, g_watch.pid);
            StringCchCopyW(ran, ranCch, g_watchPkg.fullName);
        } else {
            Util_Log(L"taskbar watcher for %s: Claude pid %lu cannot be watched (error %lu)", g_watch.folder, g_watch.pid, error);
        }
        g_watch.pid = 0;
        return error == ERROR_INVALID_PARAMETER;
    }
    /* The starts that came to this watcher until now started this Claude or reached it. */
    TakeQueuedStarts();
    ResetEvent(g_reopened);
    if (GetPackageFullName(process, &cch, ran) != ERROR_SUCCESS) StringCchCopyW(ran, ranCch, g_watchPkg.fullName);
    show = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, WindowShown, g_watch.pid, 0, WINEVENT_OUTOFCONTEXT);
    if (!show) Util_Log(L"taskbar watcher for %s: Claude's windows cannot be watched (error %lu)", g_watch.folder, GetLastError());
    create = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, TrayHostCreated, g_watch.pid, 0, WINEVENT_OUTOFCONTEXT);
    if (!create)
        Util_Log(L"taskbar watcher for %s: Claude's notification-area icon cannot be watched (error %lu)", g_watch.folder, GetLastError());
    /* A window tagged by an earlier watcher (before an update) may show an
     * older badge. */
    if (TagProcessWindows(&g_watch, TAG_IF_NEEDED)) PostMessageW(g_watchWindow, WM_WATCH_REFRESH, 0, 0);
    ScheduleTray();                                    /* its icon may be there already */
    Util_Log(L"taskbar watcher for %s (Claude pid %lu)", g_watch.folder, g_watch.pid);

    handles[RUNNING_CLAUDE_EXIT] = process;
    handles[RUNNING_QUIT] = quit;
    handles[RUNNING_HANDOVER] = handover;
    wait = PumpUntil(handles, RUNNING_HANDLE_COUNT, INFINITE);
    waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    /* What was under way for those windows ends with them; nothing tags them
     * again while they go back to Claude. */
    if (show) UnhookWinEvent(show);
    if (create) UnhookWinEvent(create);
    KillTimer(g_watchWindow, TIMER_APPS);
    g_waitingForApps = FALSE;
    if (wait == RUNNING_QUIT) {                                 /* uninstall */
        TagProcessWindows(&g_watch, TAG_REMOVE);
        Tray_GiveBack(&g_watchPkg, g_watch.pid);
    } else if (wait != RUNNING_CLAUDE_EXIT && g_retagging) {   /* handover: the new watcher takes over */
        TagProcessWindows(&g_watch, TAG_FINISH);
    }
    KillTimer(g_watchWindow, TIMER_RETAG);
    g_retagging = FALSE;
    StopWatchingButtons();
    if (wait == WAIT_FAILED) Util_Log(L"taskbar watcher for %s: waiting for Claude failed (error %lu)", g_watch.folder, waitError);
    g_watch.pid = 0;
    CloseHandle(process);
    return wait == RUNNING_CLAUDE_EXIT;
}

/* A newer start of the profile, come to this watcher, is still under way:
 * the profile runs (when `profile` is given), or the Claude started for it
 * lives on. */
static BOOL ReopenedClaudeLives(const Profile *profile)
{
    HANDLE process;
    BOOL alive;
    if (!g_reopenedPid || (profile && Claude_IsRunning(profile))) return TRUE;
    process = OpenProcess(SYNCHRONIZE, FALSE, g_reopenedPid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;   /* only a process gone is known to have ended */
    alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

/* Handles of the wait for a new Claude package, by index. */
enum { PACKAGE_LIST_CHANGED, PACKAGE_APPS_CHANGED, PACKAGE_QUIT, PACKAGE_HANDOVER, PACKAGE_REOPENED, PACKAGE_HANDLE_COUNT };

/* The profile's Claude, which ran package `ran`, has exited. When it closed
 * for an update, the profile opens again once the new package is installed:
 * TRUE, with `pid` the Claude started for it (0 when Windows started it).
 * Windows itself opens the stock profile again: that one is given a moment
 * first. The user opening the profile meanwhile ends the wait. */
static BOOL ReopenAfterUpdate(const WCHAR *folder, const WCHAR *ran, HANDLE quit, HANDLE handover, DWORD *pid)
{
    ProfileList list;
    ClaudePackage pkg;
    HANDLE handles[PACKAGE_HANDLE_COUNT];
    HKEY packages = NULL;
    ULONGLONG deadline;
    BOOL updated = FALSE, rearm = TRUE, interrupted = FALSE, identity = FALSE;
    LSTATUS rc;
    HRESULT hr;
    int i;

    *pid = 0;
    Profiles_Load(&list, &g_watchPkg);
    i = Profiles_Find(&list, folder);
    if (i < 0 || !Claude_ClosedForUpdate(&g_watchPkg, &list.items[i])) return FALSE;
    Util_Log(L"%s closed for a Claude update", folder);

    /* The new package: a change to the user's package list or to the Apps
     * folder wakes the check. */
    handles[PACKAGE_LIST_CHANGED] = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!handles[PACKAGE_LIST_CHANGED]) {
        Util_Log(L"taskbar watcher for %s cannot wait for the Claude update (error %lu)", folder, GetLastError());
        return FALSE;
    }
    handles[PACKAGE_APPS_CHANGED] = g_appsChanged;
    handles[PACKAGE_QUIT] = quit;
    handles[PACKAGE_HANDOVER] = handover;
    handles[PACKAGE_REOPENED] = g_reopened;
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, REG_PACKAGES, 0, KEY_NOTIFY, &packages);
    if (rc != ERROR_SUCCESS) {
        packages = NULL;
        Util_Log(L"taskbar watcher for %s: the package list cannot be watched (error %ld), only the Apps folder", folder, rc);
    }
    ResetEvent(g_appsChanged);
    deadline = GetTickCount64() + UPDATE_WAIT_MS;
    for (;;) {
        ULONGLONG now;
        DWORD wait;
        /* Armed before looking, so a change in between still wakes the wait. */
        if (rearm && packages &&
            (rc = RegNotifyChangeKeyValue(packages, FALSE, REG_NOTIFY_CHANGE_NAME, handles[PACKAGE_LIST_CHANGED], TRUE)) != ERROR_SUCCESS) {
            Util_Log(L"taskbar watcher for %s: the package list cannot be watched (error %ld), only the Apps folder", folder, rc);
            RegCloseKey(packages);
            packages = NULL;
        }
        rearm = FALSE;
        if (Claude_FindPackage(&pkg) && !Core_EqualsI(pkg.fullName, ran)) {
            updated = TRUE;
            break;
        }
        now = GetTickCount64();
        if (now >= deadline) {
            Util_Log(L"no new Claude package after %s closed for an update", folder);
            break;
        }
        wait = PumpUntil(handles, PACKAGE_HANDLE_COUNT, (DWORD)(deadline - now));
        if (wait == PACKAGE_LIST_CHANGED) {
            rearm = TRUE;
        } else if (wait == PACKAGE_REOPENED) {
            if (ReopenedClaudeLives(&list.items[i])) {
                Util_Log(L"%s was opened again during the Claude update", folder);
                break;
            }
            ResetEvent(g_reopened);
        } else if (wait == PACKAGE_QUIT || wait == PACKAGE_HANDOVER) {
            Util_Log(L"the watcher for %s was stopped during a Claude update", folder);
            break;
        } else if (wait == WAIT_FAILED) {
            Util_Log(L"taskbar watcher for %s: waiting for the Claude update failed (error %lu)", folder, GetLastError());
            break;
        }
    }
    if (packages) RegCloseKey(packages);
    CloseHandle(handles[PACKAGE_LIST_CHANGED]);
    if (!updated) return FALSE;
    g_watchPkg = pkg;
    Util_Log(L"taskbar watcher for %s: Claude %s is installed", folder, pkg.version);

    if (WaitForProfile(folder, 0, &list, quit, handover, list.items[i].isStock ? REOPEN_WAIT_MS : 0, &interrupted) >= 0) {
        Util_Log(L"%s is open again", folder);
        return TRUE;
    }
    if (interrupted || (i = Profiles_Find(&list, folder)) < 0) return FALSE;
    hr = Claude_Launch(&g_watchPkg, &list.items[i], NULL, pid, &identity);
    if (FAILED(hr)) {
        Util_Log(L"could not open %s again after the Claude update (0x%08lX)", folder, (unsigned long)hr);
        return FALSE;
    }
    Util_Log(L"opened %s again after the Claude update (pid %lu)%s", folder, *pid, identity ? L"" : L" without package identity");
    return TRUE;
}

/* The profile's mutex exists: this start goes to its watcher, which may be
 * about to end. The caller holds the registration lock, under which a
 * watcher's window and mutex come and go together: without a window, only a
 * passing look at the profile holds the mutex, and FALSE lets this watcher go
 * on. */
static BOOL HandStartToWatcher(const WCHAR *folder, DWORD pid)
{
    HWND watcher = FindWindowW(WATCH_CLASS, folder);
    if (!watcher) {
        Util_Log(L"taskbar watcher for %s: no watcher of the profile answers, this one watches it", folder);
        return FALSE;
    }
    if (PostMessageW(watcher, WM_WATCH_REOPENED, (WPARAM)pid, 0)) return TRUE;
    Util_Log(L"taskbar watcher for %s: Claude pid %lu could not go to the profile's watcher (error %lu), this one watches it",
             folder, pid, GetLastError());
    return FALSE;
}

/* Under the registration lock, before the watcher ends: a newer start of the
 * profile came to it and is still under way (`*pid` its Claude, 0 when
 * unknown). */
static BOOL TakeReopenedClaude(DWORD *pid)
{
    ProfileList list;
    int i;
    TakeQueuedStarts();
    if (WaitForSingleObject(g_reopened, 0) != WAIT_OBJECT_0) return FALSE;
    ResetEvent(g_reopened);
    if (!ReopenedClaudeLives(NULL)) {
        Profiles_Load(&list, &g_watchPkg);
        i = Profiles_Find(&list, g_watch.folder);
        if (i < 0 || !Claude_IsRunning(&list.items[i])) {
            Util_Log(L"taskbar watcher for %s: the start it was given (Claude pid %lu) has already ended", g_watch.folder, g_reopenedPid);
            return FALSE;
        }
    }
    *pid = g_reopenedPid;
    return TRUE;
}

/* `pid`: the Claude process just started for the profile, 0 when unknown.
 * `claudeClosed` runs each time the profile's Claude has closed. */
int Taskbar_WatchRun(const WCHAR *folder, DWORD pid, void (*claudeClosed)(const WCHAR *folder))
{
    WCHAR name[80], ran[ARRAYSIZE(g_watchPkg.fullName)];
    ProfileList list;
    HANDLE mutex = NULL, quit = NULL, handover = NULL, registration;
    PIDLIST_ABSOLUTE apps = NULL;
    ULONG appsNotify = 0;
    HRESULT hr;
    int i, rc = 1;

    StringCchCopyW(g_watch.folder, ARRAYSIZE(g_watch.folder), folder);   /* the log names it */
    if (!Claude_FindPackage(&g_watchPkg)) {
        Util_Log(L"taskbar watcher for %s: Claude Desktop is not installed", folder);
        return 1;
    }
    registration = LockWatcherRegistration();
    if (!registration) {
        Util_Log(L"taskbar watcher for %s: could not lock watcher registration to start (error %lu)", folder, GetLastError());
        return 1;
    }
    if (!WatchMutexName(folder, name, ARRAYSIZE(name))) {
        Util_Log(L"taskbar watcher for %s could not start: no name for its mutex", folder);
        goto done;
    }
    mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex) {
        Util_Log(L"taskbar watcher for %s could not start: no mutex (error %lu)", folder, GetLastError());
        goto done;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS && HandStartToWatcher(folder, pid)) {
        rc = 0;
        goto done;
    }
    /* Asserted while no stop holds the registration lock: the program was uninstalled. */
    quit = CreateEventW(NULL, TRUE, FALSE, WATCH_QUIT_EVENT);
    if (!quit) {
        Util_Log(L"taskbar watcher for %s could not start: no quit request (error %lu)", folder, GetLastError());
        goto done;
    }
    if (WaitForSingleObject(quit, 0) == WAIT_OBJECT_0) {
        Util_Log(L"taskbar watcher for %s: not started after the uninstall", folder);
        rc = 0;
        goto done;
    }
    if ((handover = CreateEventW(NULL, TRUE, FALSE, WATCH_HANDOVER_EVENT)) == NULL ||
        (g_started = CreateEventW(NULL, FALSE, FALSE, NULL)) == NULL ||
        (g_appsChanged = CreateEventW(NULL, FALSE, FALSE, NULL)) == NULL ||
        (g_reopened = CreateEventW(NULL, TRUE, FALSE, NULL)) == NULL ||
        (g_watchWindow = CreateWatchWindow(folder)) == NULL) {
        Util_Log(L"taskbar watcher for %s could not start: no events or window (error %lu)", folder, GetLastError());
        goto done;
    }
    UnlockWatcherRegistration(registration);
    registration = NULL;
    rc = 0;
    hr = SHGetKnownFolderIDList(&FOLDERID_AppsFolder, 0, NULL, &apps);
    if (SUCCEEDED(hr)) {
        SHChangeNotifyEntry entry;
        entry.pidl = apps;
        entry.fRecursive = FALSE;
        appsNotify = SHChangeNotifyRegister(g_watchWindow, SHCNRF_ShellLevel | SHCNRF_NewDelivery, SHCNE_UPDATEDIR,
                                            WM_WATCH_APPS, 1, &entry);
        if (!appsNotify) Util_Log(L"taskbar watcher for %s: the Apps folder's changes cannot be followed", folder);
    } else {
        Util_Log(L"taskbar watcher for %s: the Apps folder cannot be found (0x%08lX)", folder, (unsigned long)hr);
    }
    for (;;) {
        i = WaitForProfile(folder, pid, &list, quit, handover, START_WAIT_MS, NULL);
        if (i >= 0 && !PrepareTag(&g_watchPkg, &list.items[i], &g_watch)) {
            Util_Log(L"taskbar watcher for %s: its badged icon could not be made", folder);
        } else if (i >= 0 && WatchClaude(quit, handover, ran, ARRAYSIZE(ran))) {
            if (claudeClosed) claudeClosed(folder);
            if (ReopenAfterUpdate(folder, ran, quit, handover, &pid)) continue;
        }
        /* The watcher ends, unless the profile was opened again meanwhile; a
         * stop request ends it at once. */
        registration = LockRegistrationToEnd(quit, handover);
        if (!registration || !TakeReopenedClaude(&pid)) break;
        UnlockWatcherRegistration(registration);
        registration = NULL;
        Util_Log(L"taskbar watcher for %s goes on: the profile was opened again (Claude pid %lu)", folder, pid);
    }
    if (appsNotify) SHChangeNotifyDeregister(appsNotify);
    if (apps) CoTaskMemFree(apps);
    if (g_uia) IUIAutomation_Release(g_uia);

done:
    if (g_watchWindow) DestroyWindow(g_watchWindow);
    if (g_started) CloseHandle(g_started);
    if (g_appsChanged) CloseHandle(g_appsChanged);
    if (g_reopened) CloseHandle(g_reopened);
    if (quit) CloseHandle(quit);
    if (handover) CloseHandle(handover);
    if (mutex) CloseHandle(mutex);
    UnlockWatcherRegistration(registration);
    return rc;
}
