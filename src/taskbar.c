/*
 * Per-profile taskbar buttons (part of the Taskbar Module: see LICENSE).
 *
 * Every Claude window carries Claude's package identity, so the taskbar puts
 * all profiles under one Claude button, with Claude's icon. A window's
 * AppUserModelID can be set from another process, through the window's shell
 * property store. Each profile's windows get the ID its shortcuts carry, plus
 * a relaunch command, name and badged icon: they get their own taskbar button
 * showing the badge, and pinning that button pins the profile.
 *
 * A watcher (--watch <folder>), started whenever Claude Desktop Profiles
 * Manager opens a profile, does this for as long as that profile's Claude
 * runs: it tags the windows already open and every window Claude shows later,
 * gives the profile's notification-area icon its color (tray.c) when Claude
 * creates it
 * and after Explorer restarts or the theme changes, then exits with Claude.
 * When Claude went down for an update, the watcher waits for the new package
 * instead, opens the profile again and watches it (docs/HOW-IT-WORKS.md,
 * "Claude updates"). It only acts on events: nothing is polled. At uninstall
 * the watchers give the windows and the icon back to Claude and exit.
 *
 * After a new badge, a pinned profile's button follows its pin
 * (taskbar-pin.c); a button showing one of the profile's shortcuts is told
 * that the shortcut changed; otherwise the windows go through a temporary ID
 * until the taskbar shows that ID's button, which moves the button to the end.
 * A window goes back to Claude by getting Claude's own package ID.
 */
#include "app.h"
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <propsys.h>
#include <uiautomation.h>

#define WATCH_CLASS      L"ClaudeDesktopProfilesManagerWatch"   /* the watcher's window, titled with its folder */
#define WM_WATCH_REFRESH (WM_APP + 1)             /* from the manager: the badge changed (wParam: shortcuts too) */
#define WM_WATCH_APPS    (WM_APP + 2)             /* the app resolver read the shortcuts again */
#define WM_WATCH_TRAY    (WM_APP + 3)             /* the notification-area icon needs its color */
#define WM_WATCH_RETAGGED (WM_APP + 4)            /* the taskbar shows the temporary ID's button */
#define TIMER_RETAG      2
#define TIMER_APPS       3
#define RETAG_MS         1500   /* without UI Automation: one resolve between the two IDs */
#define RETAG_SAFETY_MS  5000   /* with it: in case its event never comes */
#define APPS_WAIT_MS     8000
#define START_WAIT_MS    90000  /* Claude takes a moment to start */
#define UPDATE_WAIT_MS   120000 /* Windows installs the new package after closing Claude */
#define REOPEN_WAIT_MS   60000  /* Windows opens Claude again by itself (measured: 1 to 33 s) */
#define MAX_LINKS        64
#define REG_ADVANCED     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"

static const GUID kAppUserModel = { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } };
enum { PID_RELAUNCH_COMMAND = 2, PID_RELAUNCH_ICON = 3, PID_RELAUNCH_NAME = 4, PID_APP_ID = 5 };

typedef struct Tag {
    WCHAR folder[FOLDER_CCH];
    WCHAR aumid[64];
    WCHAR claudeAumid[256];   /* Claude's own package ID: where the window goes back */
    WCHAR command[MAX_PATH + FOLDER_CCH + 32];
    WCHAR name[LABEL_CCH + 16];
    WCHAR icon[MAX_PATH + 4];
    DWORD pid;
} Tag;

/* The watcher's state: the WinEvent callbacks have no context parameter.
 * g_watch.pid is 0 while no Claude of the profile is watched. */
static Tag g_watch;
static ClaudePackage g_watchPkg;
static HWND g_watchWindow;
static UINT g_taskbarCreated;
static BOOL g_waitingForApps;
static HANDLE g_started;                /* the profile's Claude has started */
static WCHAR g_startedDir[MAX_PATH];    /* ...the data folder it runs */
static HANDLE g_appsChanged;            /* the Apps folder changed */

/* ------------------------------------------- the temporary ID's button */

/* The taskbar reports each new button through UI Automation, with its
 * group's ID as AutomationId ("Appid: <ID>"): the temporary ID's button
 * means the taskbar has resolved the windows under that ID. */
static IUIAutomation *g_uia;
static IUIAutomationElement *g_bars[4];
static int g_barCount;
static BOOL g_retagging;
static WCHAR g_retagButton[96];

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
    (void)self;
    (void)runtimeId;
    if (change == StructureChangeType_ChildAdded && sender &&
        SUCCEEDED(IUIAutomationElement_get_CurrentAutomationId(sender, &id)) && id) {
        if (CompareStringOrdinal(id, -1, g_retagButton, -1, TRUE) == CSTR_EQUAL) PostMessageW(g_watchWindow, WM_WATCH_RETAGGED, 0, 0);
        SysFreeString(id);
    }
    return S_OK;
}

static IUIAutomationStructureChangedEventHandlerVtbl g_handlerVtbl = { HandlerQueryInterface, HandlerAddRef, HandlerRelease, HandlerEvent };
static IUIAutomationStructureChangedEventHandler g_handler = { &g_handlerVtbl };

static void StopWatchingButtons(void)
{
    while (g_barCount > 0) {
        IUIAutomationElement *bar = g_bars[--g_barCount];
        IUIAutomation_RemoveStructureChangedEventHandler(g_uia, bar, &g_handler);
        IUIAutomationElement_Release(bar);
    }
}

/* Watches every taskbar (one per monitor) for the temporary ID's button.
 * FALSE when UI Automation cannot. */
static BOOL WatchForButton(const WCHAR *interim)
{
    const WCHAR *classes[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" };
    HWND bar;
    size_t c;
    StopWatchingButtons();
    if (FAILED(StringCchPrintfW(g_retagButton, ARRAYSIZE(g_retagButton), L"Appid: %s", interim))) return FALSE;
    if (!g_uia && FAILED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&g_uia)))
        return FALSE;
    for (c = 0; c < ARRAYSIZE(classes); c++) {
        for (bar = NULL; g_barCount < (int)ARRAYSIZE(g_bars) && (bar = FindWindowExW(NULL, bar, classes[c], NULL)) != NULL;) {
            IUIAutomationElement *e = NULL;
            if (FAILED(IUIAutomation_ElementFromHandle(g_uia, bar, &e)) || !e) continue;
            if (SUCCEEDED(IUIAutomation_AddStructureChangedEventHandler(g_uia, e, TreeScope_Subtree, NULL, &g_handler)))
                g_bars[g_barCount++] = e;
            else
                IUIAutomationElement_Release(e);
        }
    }
    return g_barCount > 0;
}

static void SetValue(IPropertyStore *ps, DWORD id, const WCHAR *value)
{
    PROPERTYKEY key;
    PROPVARIANT pv;
    key.fmtid = kAppUserModel;
    key.pid = id;
    PropVariantInit(&pv);
    if (value) {
        size_t bytes = (wcslen(value) + 1) * sizeof(WCHAR);
        pv.vt = VT_LPWSTR;
        pv.pwszVal = (LPWSTR)CoTaskMemAlloc(bytes);
        if (!pv.pwszVal) return;
        memcpy(pv.pwszVal, value, bytes);
    }
    IPropertyStore_SetValue(ps, &key, &pv);   /* VT_EMPTY empties it (and reports an error) */
    PropVariantClear(&pv);
}

static BOOL HasValue(IPropertyStore *ps, DWORD id, const WCHAR *value)
{
    PROPERTYKEY key;
    PROPVARIANT pv;
    BOOL same;
    key.fmtid = kAppUserModel;
    key.pid = id;
    PropVariantInit(&pv);
    if (FAILED(IPropertyStore_GetValue(ps, &key, &pv))) return FALSE;
    same = pv.vt == VT_LPWSTR && pv.pwszVal && CompareStringOrdinal(pv.pwszVal, -1, value, -1, TRUE) == CSTR_EQUAL;
    PropVariantClear(&pv);
    return same;
}

/* The windows that get a taskbar button: top level, no owner, not a tool window. */
static BOOL IsButtonWindow(HWND h)
{
    return IsWindow(h) && GetAncestor(h, GA_ROOT) == h && GetWindow(h, GW_OWNER) == NULL &&
           !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
}

typedef enum TagMode {
    TAG_IF_NEEDED,   /* a window without the profile's ID gets it */
    TAG_RELAUNCH,    /* a window with the profile's ID gets the current relaunch values */
    TAG_REFRESH,     /* new relaunch values under a temporary ID... */
    TAG_FINISH,      /* ...then, once the taskbar shows it, the profile's ID again */
    TAG_REMOVE       /* back to Claude */
} TagMode;

static void SetRelaunch(IPropertyStore *ps, const Tag *t)
{
    /* Read by the taskbar when the ID changes: they go first. */
    SetValue(ps, PID_RELAUNCH_ICON, t->icon);
    SetValue(ps, PID_RELAUNCH_NAME, t->name);
    SetValue(ps, PID_RELAUNCH_COMMAND, t->command);
}

/* TRUE when the window carries the profile's ID with relaunch values other
 * than the current ones (TAG_RELAUNCH: then gets them): the taskbar does not
 * read them by itself. */
static BOOL TagWindow(HWND h, const Tag *t, TagMode mode)
{
    IPropertyStore *ps = NULL;
    WCHAR interim[80];
    BOOL stale = FALSE;
    if (!IsButtonWindow(h)) return FALSE;
    if (FAILED(SHGetPropertyStoreForWindow(h, &IID_IPropertyStore, (void **)&ps))) return FALSE;
    if (FAILED(StringCchPrintfW(interim, ARRAYSIZE(interim), L"%s.refresh", t->aumid))) interim[0] = 0;
    switch (mode) {
    case TAG_REMOVE:
        if (HasValue(ps, PID_APP_ID, t->aumid) || (interim[0] && HasValue(ps, PID_APP_ID, interim))) {
            SetValue(ps, PID_RELAUNCH_COMMAND, NULL);
            SetValue(ps, PID_RELAUNCH_NAME, NULL);
            SetValue(ps, PID_RELAUNCH_ICON, NULL);
            if (t->claudeAumid[0]) SetValue(ps, PID_APP_ID, t->claudeAumid);
        }
        break;
    case TAG_REFRESH:
        if (!interim[0]) break;
        SetRelaunch(ps, t);
        SetValue(ps, PID_APP_ID, interim);
        break;
    case TAG_FINISH:
        if (interim[0] && HasValue(ps, PID_APP_ID, interim)) SetValue(ps, PID_APP_ID, t->aumid);
        break;
    case TAG_RELAUNCH:
        if (HasValue(ps, PID_APP_ID, t->aumid) &&
            (!HasValue(ps, PID_RELAUNCH_ICON, t->icon) || !HasValue(ps, PID_RELAUNCH_NAME, t->name))) {
            SetRelaunch(ps, t);
            stale = TRUE;
        }
        break;
    default:
        if (HasValue(ps, PID_APP_ID, t->aumid)) {
            stale = !HasValue(ps, PID_RELAUNCH_ICON, t->icon);
        } else if (!interim[0] || !HasValue(ps, PID_APP_ID, interim)) {
            SetRelaunch(ps, t);
            SetValue(ps, PID_APP_ID, t->aumid);
        }
        break;
    }
    IPropertyStore_Release(ps);
    return stale;
}

typedef struct TagAll {
    const Tag *tag;
    TagMode mode;
    BOOL stale;
} TagAll;

static BOOL CALLBACK TagAllProc(HWND h, LPARAM lp)
{
    TagAll *a = (TagAll *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == a->tag->pid && TagWindow(h, a->tag, a->mode)) a->stale = TRUE;
    return TRUE;
}

static BOOL TagProcessWindows(const Tag *t, TagMode mode)
{
    TagAll a;
    a.tag = t;
    a.mode = mode;
    a.stale = FALSE;
    EnumWindows(TagAllProc, (LPARAM)&a);
    return a.stale;
}

/* The profile's current name, colour and icon (they can change while it runs). */
static BOOL Prepare(const ClaudePackage *pkg, const Profile *p, Tag *t)
{
    WCHAR exe[MAX_PATH], icon[MAX_PATH];
    ZeroMemory(t, sizeof *t);
    StringCchCopyW(t->folder, ARRAYSIZE(t->folder), p->folder);
    Core_ProfileAumid(p->folder, t->aumid, ARRAYSIZE(t->aumid));
    StringCchCopyW(t->claudeAumid, ARRAYSIZE(t->claudeAumid), pkg->aumid);
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_FileExists(exe)) Util_SelfExe(exe, ARRAYSIZE(exe));
    StringCchPrintfW(t->command, ARRAYSIZE(t->command), L"\"%s\" --launch \"%s\"", exe, p->folder);
    StringCchPrintfW(t->name, ARRAYSIZE(t->name), L"Claude (%s)", p->name);
    if (!Icons_Ensure(pkg, p, icon, ARRAYSIZE(icon))) return FALSE;
    StringCchPrintfW(t->icon, ARRAYSIZE(t->icon), L"%s,0", icon);
    t->pid = p->pid;
    return TRUE;
}

/* After a rename, a colour change or a new drawing of the badge: the profile's
 * watcher gives its windows the new badge and its notification-area icon the
 * new color. `linksChanged`: the profile's shortcuts changed too. A
 * profile without a watcher (opened with the regular Claude icon) keeps
 * Claude's button. */
void Taskbar_Refresh(const Profile *profile, BOOL linksChanged)
{
    HWND w;
    if (!profile->running) return;
    w = FindWindowW(WATCH_CLASS, profile->folder);
    if (w) PostMessageW(w, WM_WATCH_REFRESH, (WPARAM)linksChanged, 0);
}

static BOOL WatchMutexName(const WCHAR *folder, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, WATCH_MUTEX_PREFIX L"%08lX", (unsigned long)Core_Hash(folder)));
}

BOOL Taskbar_IsWatched(const Profile *profile)
{
    WCHAR name[80];
    HANDLE m;
    if (!WatchMutexName(profile->folder, name, ARRAYSIZE(name))) return FALSE;
    m = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (m) CloseHandle(m);
    return m != NULL;
}

/* Starts the watcher for a profile Claude Desktop Profiles Manager has just
 * opened. A watcher already running for it keeps the job; the new one exits
 * at once. */
void Taskbar_Watch(const Profile *profile)
{
    WCHAR exe[MAX_PATH], args[FOLDER_CCH + 16];
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_FileExists(exe)) Util_SelfExe(exe, ARRAYSIZE(exe));
    if (FAILED(StringCchPrintfW(args, ARRAYSIZE(args), L"--watch \"%s\"", profile->folder))) return;
    if (!Util_Spawn(exe, args)) Util_Log(L"could not start the taskbar watcher for %s (error %lu)", profile->folder, GetLastError());
}

/* Every watcher exits: at uninstall it gives its windows and icon back to
 * Claude first; for an update it leaves them as they are, for the new
 * watchers to take over. The signal is then withdrawn, so watchers started
 * afterwards run. */
void Taskbar_StopWatchers(BOOL giveBack)
{
    HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, giveBack ? WATCH_QUIT_EVENT : WATCH_HANDOVER_EVENT);
    if (!quit) return;
    SetEvent(quit);
    Sleep(500);
    ResetEvent(quit);
    CloseHandle(quit);
}

/* The notification-area icon after an event, painted once Claude has drawn
 * its own (Tray_Apply waits for Claude's thread). */
static void ScheduleTray(void)
{
    if (g_watchWindow) PostMessageW(g_watchWindow, WM_WATCH_TRAY, 0, 0);
}

static void PaintTray(void)
{
    ProfileList list;
    int i;
    Profiles_Load(&list);
    i = Profiles_Find(&list, g_watch.folder);
    if (i < 0) return;
    list.items[i].running = TRUE;
    list.items[i].pid = g_watch.pid;
    Tray_Apply(&g_watchPkg, &list.items[i]);
}

/* The profile's current name, colour and icon, read again: they may have
 * changed while it runs. */
static void Reload(void)
{
    ProfileList list;
    ClaudePackage pkg;
    int i;
    Profiles_Load(&list);
    i = Profiles_Find(&list, g_watch.folder);
    if (i >= 0 && Claude_FindPackage(&pkg)) {
        Tag fresh;
        list.items[i].pid = g_watch.pid;
        if (Prepare(&pkg, &list.items[i], &fresh)) g_watch = fresh;
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
static BOOL InSnapGroup(HWND h)
{
    WCHAR path[160], group[64], window[16];
    DWORD session = 0, i, cch;
    HKEY key, sub;
    BOOL hit = FALSE;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\%lu\\TaskGroups", session)) ||
        FAILED(StringCchPrintfW(window, ARRAYSIZE(window), L"%lu", (unsigned long)(ULONG_PTR)h)) ||
        RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    for (i = 0; !hit; i++) {
        cch = ARRAYSIZE(group);
        if (RegEnumKeyExW(key, i, group, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(key, group, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        hit = Util_RegKeyExists(sub, window);
        RegCloseKey(sub);
    }
    RegCloseKey(key);
    return hit;
}

static BOOL CALLBACK SnappedProc(HWND h, LPARAM lp)
{
    BOOL *snapped = (BOOL *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_watch.pid && IsButtonWindow(h) && InSnapGroup(h)) *snapped = TRUE;
    return !*snapped;
}

static BOOL AnyWindowSnapped(void)
{
    BOOL snapped = FALSE;
    if (SnapGroupsShown()) EnumWindows(SnappedProc, (LPARAM)&snapped);
    return snapped;
}

/* The profile's shortcuts the button can show. */
static DWORD ButtonShortcuts(WCHAR (*paths)[MAX_PATH], DWORD max)
{
    const GUID *roots[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop, &FOLDERID_StartMenu, &FOLDERID_CommonStartMenu };
    WCHAR dir[MAX_PATH];
    DWORD i, n, kept = 0;
    size_t r;
    n = Shortcut_ListOurs(g_watch.folder, paths, max);
    for (i = 0; i < n; i++) {
        BOOL inRoot = FALSE;
        for (r = 0; r < ARRAYSIZE(roots) && !inRoot; r++)
            inRoot = Util_KnownFolder(roots[r], dir, ARRAYSIZE(dir)) && Core_PathUnder(paths[i], dir);
        if (inRoot && Shortcut_HasAppId(paths[i], g_watch.aumid)) {
            if (kept != i) StringCchCopyW(paths[kept], MAX_PATH, paths[i]);
            kept++;
        }
    }
    return kept;
}

/* The taskbar reloads the button's shortcut. */
static void NotifyShortcuts(void)
{
    WCHAR (*paths)[MAX_PATH];
    DWORD i, n;
    g_waitingForApps = FALSE;
    KillTimer(g_watchWindow, TIMER_APPS);
    paths = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, MAX_LINKS * sizeof *paths);
    if (!paths) return;
    n = ButtonShortcuts(paths, MAX_LINKS);
    for (i = 0; i < n; i++) {
        SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSH, paths[i], paths[i]);
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, paths[i], NULL);
    }
    HeapFree(GetProcessHeap(), 0, paths);
}

static BOOL HasButtonShortcut(void)
{
    WCHAR (*paths)[MAX_PATH];
    DWORD n = 0;
    paths = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, MAX_LINKS * sizeof *paths);
    if (paths) {
        n = ButtonShortcuts(paths, MAX_LINKS);
        HeapFree(GetProcessHeap(), 0, paths);
    }
    return n > 0;
}

static void FinishRetag(BOOL shown)
{
    if (!g_retagging) return;
    g_retagging = FALSE;
    KillTimer(g_watchWindow, TIMER_RETAG);
    StopWatchingButtons();
    TagProcessWindows(&g_watch, TAG_FINISH);
    Util_Log(L"%s has its own taskbar button again (%s)", g_watch.folder, shown ? L"its button was shown" : L"after a delay");
}

/* A new badge. */
static void StartRefresh(BOOL linksChanged)
{
    WCHAR interim[80];
    BOOL stale;
    Reload();
    stale = TagProcessWindows(&g_watch, TAG_RELAUNCH);
    ScheduleTray();
    if (!stale && !linksChanged) return;
    if (TaskbarPin_HasAppId(g_watch.aumid)) return;
    if (!AnyWindowSnapped() && HasButtonShortcut()) {
        g_waitingForApps = TRUE;
        SetTimer(g_watchWindow, TIMER_APPS, APPS_WAIT_MS, NULL);
        return;
    }
    /* The profile's ID comes back once the taskbar shows the temporary ID's
     * button, watched before the windows take that ID. */
    if (FAILED(StringCchPrintfW(interim, ARRAYSIZE(interim), L"%s.refresh", g_watch.aumid))) return;
    g_retagging = TRUE;
    SetTimer(g_watchWindow, TIMER_RETAG, WatchForButton(interim) ? RETAG_SAFETY_MS : RETAG_MS, NULL);
    TagProcessWindows(&g_watch, TAG_REFRESH);
}

/* A hidden top-level window: broadcasts only reach those. */
static LRESULT CALLBACK WatchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_watch.pid && (msg == WM_WATCH_TRAY || msg == WM_WATCH_REFRESH)) return 0;   /* no Claude to paint */
    if (msg == WM_WATCH_TRAY) {
        PaintTray();
        return 0;
    }
    if ((msg == WM_TIMER && wp == TIMER_RETAG) || msg == WM_WATCH_RETAGGED) {
        FinishRetag(msg == WM_WATCH_RETAGGED);
        return 0;
    }
    if ((msg == WM_TIMER && wp == TIMER_APPS) || msg == WM_WATCH_APPS) {
        if (msg == WM_WATCH_APPS) {
            PIDLIST_ABSOLUTE *pidls = NULL;
            LONG event = 0;
            HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &event);
            if (lock) SHChangeNotification_Unlock(lock);
            if (g_appsChanged) SetEvent(g_appsChanged);   /* a new Claude package shows there too */
        }
        if (g_waitingForApps) NotifyShortcuts();
        else if (msg == WM_TIMER) KillTimer(h, TIMER_APPS);
        return 0;
    }
    if (msg == WM_WATCH_REFRESH) {
        StartRefresh((BOOL)wp);
        return 0;
    }
    if ((g_taskbarCreated && msg == g_taskbarCreated) ||
        (msg == WM_SETTINGCHANGE && lp &&
         CompareStringOrdinal((const WCHAR *)lp, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)) {
        ScheduleTray();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND CreateWatchWindow(const WCHAR *folder)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WatchProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = WATCH_CLASS;
    RegisterClassExW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    return CreateWindowExW(WS_EX_TOOLWINDOW, WATCH_CLASS, folder, WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
}

/* Claude created its notification-area icon (the window that holds it). */
static void CALLBACK CreateProc(HWINEVENTHOOK hook, DWORD event, HWND h, LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)thread; (void)time;
    if (object == OBJID_WINDOW && child == CHILDID_SELF && h && Tray_IsHost(h)) ScheduleTray();
}

static void CALLBACK ShowProc(HWINEVENTHOOK hook, DWORD event, HWND h, LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)thread; (void)time;
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !IsButtonWindow(h)) return;
    Reload();
    if (TagWindow(h, &g_watch, TAG_IF_NEEDED)) PostMessageW(g_watchWindow, WM_WATCH_REFRESH, 0, 0);
}

/* Waits for one of `handles`, at most `ms`, handling the watcher's messages
 * and WinEvents meanwhile: the index of the handle signaled, WAIT_TIMEOUT or
 * WAIT_FAILED. */
static DWORD Pump(const HANDLE *handles, DWORD count, DWORD ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        DWORD left = INFINITE, w;
        MSG msg;
        if (ms != INFINITE) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) return WAIT_TIMEOUT;
            left = (DWORD)(deadline - now);
        }
        w = MsgWaitForMultipleObjects(count, handles, FALSE, left, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0 + count) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        if (w < WAIT_OBJECT_0 + count) return w - WAIT_OBJECT_0;
        return w == WAIT_TIMEOUT ? WAIT_TIMEOUT : WAIT_FAILED;
    }
}

/* A process created its Chrome_MessageWindow, titled with its data folder
 * (see Claude_UpdateRunning): the profile's Claude has started. */
static void CALLBACK StartedProc(HWINEVENTHOOK hook, DWORD event, HWND h, LONG object, LONG child, DWORD thread, DWORD time)
{
    WCHAR cls[32], title[MAX_PATH];
    (void)hook; (void)event; (void)thread; (void)time;
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !h || !GetClassNameW(h, cls, ARRAYSIZE(cls)) ||
        CompareStringOrdinal(cls, -1, L"Chrome_MessageWindow", -1, FALSE) != CSTR_EQUAL)
        return;
    if (GetWindowTextW(h, title, ARRAYSIZE(title)) > 0 && Core_PathEquals(title, g_startedDir)) SetEvent(g_started);
}

/* Waits, at most `ms`, for the profile's Claude to run: its index in `list`,
 * or -1 when it does not run in time, the profile is gone or the watcher is
 * told to stop (`stopped`). */
static int WaitForProfile(const WCHAR *folder, ProfileList *list, HANDLE quit, HANDLE handover, DWORD ms, BOOL *stopped)
{
    HANDLE handles[3];
    HWINEVENTHOOK hook;
    ULONGLONG deadline = GetTickCount64() + ms;
    int i;
    if (stopped) *stopped = FALSE;
    handles[0] = g_started;
    handles[1] = quit;
    handles[2] = handover;
    ResetEvent(g_started);
    /* Hooked before looking, so a start in between still counts. */
    hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, StartedProc, 0, 0,
                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    for (;;) {
        ULONGLONG now;
        DWORD w;
        Profiles_Load(list);
        i = Profiles_Find(list, folder);
        if (i < 0 || list->items[i].running) break;
        StringCchCopyW(g_startedDir, ARRAYSIZE(g_startedDir), list->items[i].dataDir);
        now = GetTickCount64();
        w = hook && now < deadline ? Pump(handles, ARRAYSIZE(handles), (DWORD)(deadline - now)) : WAIT_TIMEOUT;
        if (w != 0) {
            if (stopped) *stopped = w != WAIT_TIMEOUT;
            i = -1;
            break;
        }
    }
    if (hook) UnhookWinEvent(hook);
    return i;
}

typedef enum WatchEnd { END_EXITED, END_UNINSTALL, END_HANDOVER, END_FAILED } WatchEnd;

/* Watches the profile's running Claude (g_watch) until it exits or the
 * watcher is told to stop. `ran` gets the package that Claude runs. */
static WatchEnd WatchClaude(HANDLE quit, HANDLE handover, WCHAR *ran, UINT32 ranCch)
{
    HANDLE process, handles[3];
    HWINEVENTHOOK show, create;
    UINT32 cch = ranCch;
    WatchEnd end;
    DWORD w;

    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_watch.pid);
    if (!process) process = OpenProcess(SYNCHRONIZE, FALSE, g_watch.pid);
    if (!process) return END_FAILED;
    if (GetPackageFullName(process, &cch, ran) != ERROR_SUCCESS) StringCchCopyW(ran, ranCch, g_watchPkg.fullName);
    show = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, ShowProc, g_watch.pid, 0, WINEVENT_OUTOFCONTEXT);
    create = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, CreateProc, g_watch.pid, 0, WINEVENT_OUTOFCONTEXT);
    /* A window tagged by an earlier watcher (before an update) may show an
     * older badge. */
    if (TagProcessWindows(&g_watch, TAG_IF_NEEDED)) PostMessageW(g_watchWindow, WM_WATCH_REFRESH, 0, 0);
    ScheduleTray();                                    /* its icon may be there already */
    Util_Log(L"taskbar watcher for %s (Claude pid %lu)", g_watch.folder, g_watch.pid);

    handles[0] = process;
    handles[1] = quit;
    handles[2] = handover;
    w = Pump(handles, ARRAYSIZE(handles), INFINITE);
    if (w == 0) {
        end = END_EXITED;
    } else if (w == 1) {                               /* uninstall */
        TagProcessWindows(&g_watch, TAG_REMOVE);
        Tray_GiveBack(&g_watchPkg, g_watch.pid);
        end = END_UNINSTALL;
    } else {
        end = w == 2 ? END_HANDOVER : END_FAILED;      /* handover: the new watcher takes over */
    }

    /* What was under way for those windows ends with them. */
    if (show) UnhookWinEvent(show);
    if (create) UnhookWinEvent(create);
    KillTimer(g_watchWindow, TIMER_RETAG);
    KillTimer(g_watchWindow, TIMER_APPS);
    g_retagging = FALSE;
    g_waitingForApps = FALSE;
    StopWatchingButtons();
    g_watch.pid = 0;
    CloseHandle(process);
    return end;
}

/* The profile's Claude, which ran package `ran`, has exited. When it went
 * down for an update, the profile opens again once the new package is
 * installed: TRUE, the watcher then waits for it. Windows opens Claude again
 * by itself, without arguments, which starts the stock profile: that one is
 * left a moment to come back so. */
static BOOL ReopenAfterUpdate(const WCHAR *folder, const WCHAR *ran, HANDLE quit, HANDLE handover)
{
    ProfileList list;
    ClaudePackage pkg;
    HANDLE handles[4];
    HKEY packages = NULL;
    ULONGLONG deadline;
    BOOL updated = FALSE, arm = TRUE, stopped = FALSE, identity = FALSE;
    DWORD pid = 0;
    HRESULT hr;
    int i;

    Profiles_Load(&list);
    i = Profiles_Find(&list, folder);
    if (i < 0 || !Claude_ClosedForUpdate(&g_watchPkg, &list.items[i])) return FALSE;
    Util_Log(L"%s closed for a Claude update", folder);

    /* The new package: a change to the user's package list or to the Apps
     * folder wakes the check. */
    handles[0] = CreateEventW(NULL, FALSE, FALSE, NULL);
    handles[1] = g_appsChanged;
    handles[2] = quit;
    handles[3] = handover;
    if (!handles[0]) return FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PACKAGES, 0, KEY_NOTIFY, &packages) != ERROR_SUCCESS) packages = NULL;
    ResetEvent(g_appsChanged);
    deadline = GetTickCount64() + UPDATE_WAIT_MS;
    for (;;) {
        ULONGLONG now;
        DWORD w;
        /* Armed before looking, so a change in between still wakes the wait. */
        if (arm && packages) RegNotifyChangeKeyValue(packages, FALSE, REG_NOTIFY_CHANGE_NAME, handles[0], TRUE);
        arm = FALSE;
        if (Claude_FindPackage(&pkg) && CompareStringOrdinal(pkg.fullName, -1, ran, -1, TRUE) != CSTR_EQUAL) {
            updated = TRUE;
            break;
        }
        now = GetTickCount64();
        if (now >= deadline) break;
        w = Pump(handles, ARRAYSIZE(handles), (DWORD)(deadline - now));
        if (w == 0) arm = TRUE;
        else if (w != 1 && w != WAIT_TIMEOUT) break;
    }
    if (packages) RegCloseKey(packages);
    CloseHandle(handles[0]);
    if (!updated) {
        Util_Log(L"no new Claude package after %s closed: the watcher stops", folder);
        return FALSE;
    }
    g_watchPkg = pkg;
    Util_Log(L"Claude %s is installed", pkg.version);

    if (WaitForProfile(folder, &list, quit, handover, list.items[i].isStock ? REOPEN_WAIT_MS : 0, &stopped) >= 0) {
        Util_Log(L"%s is open again", folder);
        return TRUE;
    }
    if (stopped || (i = Profiles_Find(&list, folder)) < 0) return FALSE;
    hr = Claude_Launch(&g_watchPkg, &list.items[i], NULL, &pid, &identity);
    if (FAILED(hr)) {
        Util_Log(L"could not open %s again after the Claude update (0x%08lX)", folder, (unsigned long)hr);
        return FALSE;
    }
    Util_Log(L"opened %s again after the Claude update (pid %lu)%s", folder, pid, identity ? L"" : L" without package identity");
    return TRUE;
}

int Taskbar_WatchRun(const WCHAR *folder)
{
    WCHAR name[80], ran[ARRAYSIZE(g_watchPkg.fullName)];
    ProfileList list;
    HANDLE mutex, quit, handover = NULL;
    PIDLIST_ABSOLUTE apps = NULL;
    ULONG appsNotify = 0;
    int i, rc = 0;

    if (!WatchMutexName(folder, name, ARRAYSIZE(name))) return 1;
    mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    quit = CreateEventW(NULL, TRUE, FALSE, WATCH_QUIT_EVENT);
    handover = CreateEventW(NULL, TRUE, FALSE, WATCH_HANDOVER_EVENT);
    g_started = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_appsChanged = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!quit || !handover || !g_started || !g_appsChanged || !Claude_FindPackage(&g_watchPkg) ||
        (g_watchWindow = CreateWatchWindow(folder)) == NULL) {
        rc = 1;
        goto done;
    }
    if (SUCCEEDED(SHGetKnownFolderIDList(&FOLDERID_AppsFolder, 0, NULL, &apps))) {
        SHChangeNotifyEntry entry;
        entry.pidl = apps;
        entry.fRecursive = FALSE;
        appsNotify = SHChangeNotifyRegister(g_watchWindow, SHCNRF_ShellLevel | SHCNRF_NewDelivery, SHCNE_UPDATEDIR,
                                            WM_WATCH_APPS, 1, &entry);
    }
    for (;;) {
        i = WaitForProfile(folder, &list, quit, handover, START_WAIT_MS, NULL);
        if (i < 0 || !Prepare(&g_watchPkg, &list.items[i], &g_watch)) break;
        if (WatchClaude(quit, handover, ran, ARRAYSIZE(ran)) != END_EXITED || !ReopenAfterUpdate(folder, ran, quit, handover))
            break;
    }
    if (appsNotify) SHChangeNotifyDeregister(appsNotify);
    if (apps) CoTaskMemFree(apps);
    if (g_uia) IUIAutomation_Release(g_uia);

done:
    if (g_watchWindow) DestroyWindow(g_watchWindow);
    if (g_started) CloseHandle(g_started);
    if (g_appsChanged) CloseHandle(g_appsChanged);
    if (quit) CloseHandle(quit);
    if (handover) CloseHandle(handover);
    CloseHandle(mutex);
    return rc;
}
