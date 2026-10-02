/*
 * The manager window: the profile list with Open / New / Edit / Delete / Set
 * as default, the shortcut, taskbar pin and Start menu buttons, and Uninstall;
 * Sessions turns the same window to the sessions view (sessions.c). Also the
 * profile and uninstall dialogs. Everything it shows follows events (see
 * WatchOutside and WatchShortcutFolders); nothing polls.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#define WM_APP_UNINSTALL       (WM_APP + 1)
#define WM_APP_RELAYOUT        (WM_APP + 2)
#define WM_APP_RESTORE_FOCUS   (WM_APP + 3)   /* back to the window: the focus may be on a control the refresh hid */
#define WM_APP_FIT_NAMES       (WM_APP + 4)   /* a dialog was fitted (opened, or moved to another scale): cut names to it */
#define WM_APP_SHOW            (WM_APP + 5)   /* a second launch asked for the manager itself */
#define WM_APP_SETUPLINKS      (WM_APP + 6)   /* just installed: offer to set up claude:// links */
#define WM_APP_UPDATE          (WM_APP + 7)   /* the latest release is known */
#define WM_APP_DOWNLOADED      (WM_APP + 8)   /* wParam: how the download went (UpdateResult), lParam: Windows' error */
#define WM_APP_SELECTION       (WM_APP + 9)   /* the list selection changed */
#define WM_APP_SHORTCUTS       (WM_APP + 10)  /* the desktop, Start menu or taskbar pins folder changed */
#define WM_APP_OUTSIDE         (WM_APP + 11)  /* something the window shows changed outside it (g_outside) */
#define WM_APP_SHORTCUTS_CHECK (WM_APP + 14)  /* once for a burst of WM_APP_SHORTCUTS (12 and 13 are the sessions view's) */

#define ROW_ICON_DIPS         24   /* a taskbar button's icon size, so the initial shows in the row's badge */
#define GROUP_BADGE_GAP_DIPS  5    /* between the "Shortcuts for" badge and its text */
#define GROUP_BADGE_OVERHANG  2    /* pixels the badge is taller than the label's capitals */
#define PREVIEW_ICON_DIPS     32   /* the profile dialog's icon */
#define FIRST_COLUMN_WIDTH    60   /* any: Gui_LayoutProfileColumns sizes the columns */

/* Set once the manager's window exists: a second launch waits for it while
 * the first one still repairs its install. */
#define MANAGER_READY_EVENT      L"Local\\ClaudeDesktopProfilesManager.ManagerReady"
#define MANAGER_READY_TIMEOUT_MS 15000
#define SHELL_WORK_WAIT_MS       60000   /* the language's shortcuts and pins take about a second */

/* What changed outside the window, told by the thread that waits for it. */
enum { CHANGE_PROFILES = 1, CHANGE_LINKS = 2, CHANGE_PACKAGES = 4, CHANGE_PINS = 8 };

/* What the button beside the status does (IDC_STATUS_ACTION). */
typedef enum StatusAction { STATUS_ACTION_NONE, STATUS_ACTION_GET_CLAUDE, STATUS_ACTION_SET_UP_LINKS } StatusAction;

typedef struct MainState {
    HWND         dlg;
    HWND         list;
    ProfileList  profiles;
    ClaudePackage pkg;
    StatusAction statusAction;
    WCHAR        shortcutStateFolder[FOLDER_CCH]; /* profile whose shortcut and pin state is cached */
    BOOL         onDesktop;
    BOOL         pinned;
    BOOL         inStartMenu;
    ULONG        shortcutsNotify;  /* SHChangeNotifyRegister on the folders those buttons read */
    int          groupColor;       /* badge shown in the "Shortcuts for" label */
    HICON        groupBadge;       /* that badge as last drawn, in groupBadgeColor at groupBadgeSize pixels */
    int          groupBadgeColor;
    int          groupBadgeSize;
    HANDLE       outsideThread;    /* waits for outside changes (WatchOutside) */
    HANDLE       outsideStop;
    HANDLE       readyEvent;       /* MANAGER_READY_EVENT */
    HICON        bigIcon;
    HICON        smallIcon;
    BOOL         uninstalled;      /* nothing may touch the registry any more */
    BOOL         uninstallInProgress; /* its dialog or the uninstall itself: no refresh */
    BOOL         closing;          /* asked to close (installer update) */
    BOOL         pendingUninstall; /* uninstall asked while a dialog was open */
    BOOL         pendingSetUpLinks; /* links setup asked while a dialog was open */
    BOOL         uninstallOnly;    /* started from Settings to uninstall: no manager window */
    BOOL         updating;         /* the newer release is downloading or installing */
    BOOL         installing;
    BOOL         selectionPending; /* WM_APP_SELECTION posted */
    BOOL         shortcutsCheckPending; /* WM_APP_SHORTCUTS_CHECK posted */
    BOOL         layoutReady, layingOut;
    HANDLE       shellWork;        /* the thread writing shortcuts and pins in a new language */
} MainState;

static MainState g_manager;
static volatile LONG g_outside;   /* CHANGE_* not handled yet: set by the thread, taken by the window */

/* While uninstalling or closing, nothing may reload state or write the
 * registry. */
static BOOL StateChangesBlocked(void)
{
    return g_manager.uninstalled || g_manager.uninstallInProgress || g_manager.closing;
}

static const Profile *SelectedProfile(void)
{
    int i = ListView_GetNextItem(g_manager.list, -1, LVNI_SELECTED);
    return (i >= 0 && i < g_manager.profiles.count) ? &g_manager.profiles.items[i] : NULL;
}

static const WCHAR *StockProfileName(void)
{
    int i = Profiles_Find(&g_manager.profiles, STOCK_FOLDER);
    return i >= 0 ? g_manager.profiles.items[i].name : STOCK_DEFAULT_NAME;
}

/* A control turned off while it has the focus gives it to the next one. */
static void EnableControl(int id, BOOL enabled)
{
    HWND control = GetDlgItem(g_manager.dlg, id);
    if (!enabled && GetFocus() == control) SendMessageW(g_manager.dlg, WM_NEXTDLGCTL, 0, FALSE);
    EnableWindow(control, enabled);
}

static void SetTextIfChanged(int id, const WCHAR *text)
{
    WCHAR current[512];
    GetDlgItemTextW(g_manager.dlg, id, current, ARRAYSIZE(current));
    if (wcscmp(current, text) != 0) SetDlgItemTextW(g_manager.dlg, id, text);
}

/* Posts `message` unless it already waits in the queue: a burst of events
 * is handled once. */
static void PostOnce(BOOL *pending, UINT message)
{
    if (*pending) return;
    *pending = TRUE;
    PostMessageW(g_manager.dlg, message, 0, 0);
}

static void LayoutMainControls(void);

/* The status action and Update buttons take room only while they show: the
 * window is laid out again when one of them appears or goes. */
static void ShowOptionalButton(int id, BOOL shown)
{
    HWND button = GetDlgItem(g_manager.dlg, id);
    if (((GetWindowLongW(button, GWL_STYLE) & WS_VISIBLE) != 0) == shown) return;
    ShowWindow(button, shown ? SW_SHOW : SW_HIDE);
    LayoutMainControls();
}

/* The focus and the default button of the view shown: the profiles list and
 * Open, or the sessions tree and IDOK (SessionsView_Command opens the
 * session). */
static void FocusView(HWND dialog, BOOL sessions)
{
    SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, sessions ? IDC_S_TREE : IDC_LIST), TRUE);
    SendMessageW(dialog, DM_SETDEFID, sessions ? IDOK : IDC_OPEN, 0);
}

/* ------------------------------------------------------------------ list */

static void UpdateRow(int i)
{
    const Profile *p = &g_manager.profiles.items[i];
    WCHAR text[MAX_PATH];
    ListView_SetItemText(g_manager.list, i, 0, (LPWSTR)p->name);
    ListView_SetItemText(g_manager.list, i, 1,
                         (LPWSTR)TR(Theme_ProfileRole(p->isStock, Core_EqualsI(p->folder, g_manager.profiles.defaultFolder))));
    StringCchPrintfW(text, ARRAYSIZE(text), L"%%APPDATA%%\\%s", p->folder);
    ListView_SetItemText(g_manager.list, i, 2, text);
}

static int IconPixels(void)
{
    return MulDiv(ROW_ICON_DIPS, (int)GetDpiForWindow(g_manager.dlg), 96);
}

/* Each row's image, I_IMAGENONE for a profile whose icon could not be made. */
static void RebuildImages(int *rowImages)
{
    int pixels = IconPixels(), i;
    HIMAGELIST images = ImageList_Create(pixels, pixels, ILC_COLOR32, g_manager.profiles.count, 1), previous;
    for (i = 0; i < g_manager.profiles.count; i++) rowImages[i] = I_IMAGENONE;
    if (!images) return;
    for (i = 0; i < g_manager.profiles.count; i++) {
        HICON icon = Icons_Create(&g_manager.pkg, &g_manager.profiles.items[i], pixels);
        if (!icon) icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, pixels, pixels, 0);
        if (icon) {
            int added = ImageList_AddIcon(images, icon);
            if (added >= 0) rowImages[i] = added;
            DestroyIcon(icon);
        }
    }
    previous = ListView_SetImageList(g_manager.list, images, LVSIL_SMALL);
    if (previous) ImageList_Destroy(previous);
}

/* Default widths are independent of profile names and the selected language.
 * A user's adjustments persist; the data column follows the available space
 * (the window's minimum size keeps it at least its own minimum). */
void Gui_LayoutProfileColumns(HWND list)
{
    int profileWidth, roleWidth, dataMinimum;
    int actualProfile = ListView_GetColumnWidth(list, 0), actualRole = ListView_GetColumnWidth(list, 1);
    Theme_ProfileColumnWidths(list, &profileWidth, &roleWidth, &dataMinimum);
    if (Theme_ColumnResizeIsManual(list, 0)) profileWidth = actualProfile;
    if (Theme_ColumnResizeIsManual(list, 1)) roleWidth = actualRole;
    Theme_SetColumnWidth(list, 0, profileWidth);
    Theme_SetColumnWidth(list, 1, roleWidth);
    /* The data column: the rest, never narrower than the data folder (past it, the list scrolls sideways). */
    Theme_FitLastColumn(list, dataMinimum);
}

static void LayoutColumns(void)
{
    Gui_LayoutProfileColumns(g_manager.list);
}

static void UpdateButtons(void);
static void FinishShellWork(void);
static void UpdateNote(void);
static void ReflowMain(void);
static void ShowVersion(void);

/* Selects the row of `folder`, else the first one: its index, -1 in an
 * empty list. */
static int SetSelectedRow(const WCHAR *folder)
{
    int row = Profiles_Find(&g_manager.profiles, folder);
    if (row < 0 && g_manager.profiles.count > 0) row = 0;
    if (row >= 0) ListView_SetItemState(g_manager.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    return row;
}

/* The program moves the selection: the row comes into sight. */
static void SelectProfileRow(const WCHAR *folder)
{
    int row = SetSelectedRow(folder);
    if (row >= 0) ListView_EnsureVisible(g_manager.list, row, FALSE);
}

/* A refill keeps the view where it was (the viewport follows a row brought
 * into sight even during a refill): only a selection that had to move, its
 * profile gone, comes into sight once drawing is back on. */
static void FillList(const WCHAR *select)
{
    LVITEMW item;
    int rowImages[MAX_PROFILES], i, row;
    BOOL selectionMoved = select && select[0] && Profiles_Find(&g_manager.profiles, select) < 0;
    SendMessageW(g_manager.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_manager.list);
    RebuildImages(rowImages);
    for (i = 0; i < g_manager.profiles.count; i++) {
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.iImage = rowImages[i];
        item.pszText = g_manager.profiles.items[i].name;
        ListView_InsertItem(g_manager.list, &item);
        UpdateRow(i);
    }
    row = SetSelectedRow(select);
    SendMessageW(g_manager.list, WM_SETREDRAW, TRUE, 0);
    if (selectionMoved && row >= 0) ListView_EnsureVisible(g_manager.list, row, FALSE);
    LayoutColumns();
    InvalidateRect(g_manager.list, NULL, TRUE);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
    UpdateNote();
}

/* Whether the list shows the same rows: folders, names and colors. */
static BOOL SameProfileRows(const ProfileList *a, const ProfileList *b)
{
    int i;
    if (a->count != b->count) return FALSE;
    for (i = 0; i < a->count; i++) {
        if (!Core_EqualsI(a->items[i].folder, b->items[i].folder) || wcscmp(a->items[i].name, b->items[i].name) != 0 ||
            a->items[i].color != b->items[i].color)
            return FALSE;
    }
    return TRUE;
}

/* --------------------------------------------------------------- status */

/* "2.16120.0.0" -> "2.16120.0", as Claude shows it. */
static void ShortVersion(WCHAR *out, size_t cch)
{
    size_t n;
    StringCchCopyW(out, cch, g_manager.pkg.version);
    n = wcslen(out);
    if (n > 2 && out[n - 2] == L'.' && out[n - 1] == L'0') out[n - 2] = 0;
}

static void UpdateStatus(void)
{
    WCHAR text[256], version[32];
    StatusAction action = STATUS_ACTION_NONE;

    ShortVersion(version, ARRAYSIZE(version));

    if (!g_manager.pkg.found) {
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"Claude Desktop is not installed."));
        action = STATUS_ACTION_GET_CLAUDE;
    } else if (Handler_UserChoice() != USERCHOICE_OURS) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Claude Desktop %s \x00B7 claude:// links are not set up yet"), version);
        action = STATUS_ACTION_SET_UP_LINKS;
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Claude Desktop %s \x00B7 claude:// links are routed correctly"), version);
    }
    SetTextIfChanged(IDC_STATUS, text);
    SetTextIfChanged(IDC_STATUS_ACTION, TR(Theme_MainCaption(IDC_STATUS_ACTION, action != STATUS_ACTION_GET_CLAUDE)));
    /* Never hide the focused control. */
    if (action == STATUS_ACTION_NONE && GetFocus() == GetDlgItem(g_manager.dlg, IDC_STATUS_ACTION))
        FocusView(g_manager.dlg, SessionsView_Shown());
    g_manager.statusAction = action;
    ShowOptionalButton(IDC_STATUS_ACTION, action != STATUS_ACTION_NONE);
}

/* Text of a static that names a profile: a long name is cut (with an
 * ellipsis) until the whole text fits `box` (client coordinates). `format`
 * has one %s. Returns the height the text takes. */
static int FitNameIn(HWND control, const RECT *box, const WCHAR *format, const WCHAR *name, WCHAR *text, size_t cch)
{
    RECT need;
    HDC dc;
    HFONT old;
    size_t keep;

    dc = GetDC(control);
    old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    for (keep = wcslen(name); ; keep = Localize_ShorterCut(name, keep)) {
        Localize_FormatCutName(format, name, keep, text, cch);
        need = *box;
        DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | Localize_ReadingFlags());
        if (keep <= 1 || (need.bottom - need.top <= box->bottom - box->top && need.right <= box->right)) break;
    }
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    return need.bottom - need.top;
}

static void FitName(HWND control, const WCHAR *format, const WCHAR *name, WCHAR *text, size_t cch)
{
    RECT box;
    GetClientRect(control, &box);
    FitNameIn(control, &box, format, name, text, cch);
}

/* The note under Set as default names the profile the regular Claude icon
 * opens; it depends on that name, the language and the scale only. */
static void UpdateNote(void)
{
    Theme_LayoutSidebarNote(GetDlgItem(g_manager.dlg, IDC_NOTE),
                            TR(Theme_MainNote()),
                            StockProfileName());
}

static void SetKeepText(HWND dialog, const ProfileList *list)
{
    WCHAR text[1024];
    int i = Profiles_Find(list, STOCK_FOLDER);
    FitName(GetDlgItem(dialog, IDC_U_KEEP),
            TR(L"Claude Desktop and its \x201C%s\x201D profile (the one the regular Claude icon opens) are not touched."),
            i >= 0 ? list->items[i].name : STOCK_DEFAULT_NAME, text, ARRAYSIZE(text));
    SetDlgItemTextW(dialog, IDC_U_KEEP, text);
}

static void UpdateButtons(void)
{
    const Profile *p = SelectedProfile();
    WCHAR text[256];
    BOOL isDefault = p && Core_EqualsI(p->folder, g_manager.profiles.defaultFolder);

    if (p && !Core_EqualsI(p->folder, g_manager.shortcutStateFolder)) {
        StringCchCopyW(g_manager.shortcutStateFolder, ARRAYSIZE(g_manager.shortcutStateFolder), p->folder);
        g_manager.onDesktop = Shortcut_IsOnDesktop(p);
        g_manager.pinned = TaskbarPin_IsPinned(p);
        g_manager.inStartMenu = Shortcut_IsInStartMenu(p);
    }
    EnableControl(IDC_OPEN, p && g_manager.pkg.found);
    EnableControl(IDC_NEW, g_manager.profiles.count < MAX_PROFILES);
    EnableControl(IDC_EDIT, p != NULL);
    EnableControl(IDC_DELETE, p && !p->isStock);
    EnableControl(IDC_DEFAULT, p && !isDefault);
    EnableControl(IDC_SC_DESKTOP, p && !g_manager.onDesktop);
    EnableControl(IDC_SC_SAVEAS, p != NULL);
    EnableControl(IDC_SC_PIN, p && !g_manager.pinned);
    EnableControl(IDC_SC_START, p != NULL);
    /* A button off because it is done says so. */
    SetTextIfChanged(IDC_SC_DESKTOP, TR(Theme_MainCaption(IDC_SC_DESKTOP, p && g_manager.onDesktop)));
    SetTextIfChanged(IDC_SC_PIN, TR(Theme_MainCaption(IDC_SC_PIN, p && g_manager.pinned)));
    SetTextIfChanged(IDC_SC_START, TR(Theme_MainCaption(IDC_SC_START, p && g_manager.inStartMenu)));

    if (p)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Shortcuts for \x201C%s\x201D"), p->name);
    else
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"Shortcuts"));
    SetTextIfChanged(IDC_SC_GROUP, text);
    if (p && p->color != g_manager.groupColor) {
        g_manager.groupColor = p->color;
        InvalidateRect(GetDlgItem(g_manager.dlg, IDC_SC_GROUP), NULL, TRUE);
    }
}

/* The badge of the "Shortcuts for" label, made again only for another color
 * or size. */
static HICON GroupBadge(int color, int size)
{
    if (g_manager.groupBadge && (g_manager.groupBadgeColor != color || g_manager.groupBadgeSize != size)) {
        DestroyIcon(g_manager.groupBadge);
        g_manager.groupBadge = NULL;
    }
    if (!g_manager.groupBadge) {
        g_manager.groupBadge = Icons_CreateBadge(color, size);
        g_manager.groupBadgeColor = color;
        g_manager.groupBadgeSize = size;
    }
    return g_manager.groupBadge;
}

/* Shortcuts for "(badge) Name": owner-drawn to show the profile's badge
 * before its name, on the left in every language as the list's icons are.
 * The window text stays the plain sentence (screen readers read it). */
static void DrawGroupLabel(const DRAWITEMSTRUCT *item)
{
    const Profile *p = SelectedProfile();
    HDC dc = item->hDC;
    RECT rc = item->rcItem;
    HFONT old;
    HICON badge;
    TEXTMETRICW metrics;
    WCHAR text[512];
    int top, dot, gap;

    SetTextColor(dc, Theme_Color(THEME_TEXT));
    FillRect(dc, &rc, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &metrics);
    top = rc.top + (rc.bottom - rc.top - metrics.tmHeight) / 2;
    if (!p) {
        DrawTextW(dc, TR(L"Shortcuts"), -1, &rc,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | Localize_ReadingFlags());
        SelectObject(dc, old);
        return;
    }

    dot = metrics.tmAscent - metrics.tmInternalLeading + GROUP_BADGE_OVERHANG;
    gap = MulDiv(GROUP_BADGE_GAP_DIPS, (int)GetDpiForWindow(item->hwndItem), 96);
    badge = GroupBadge(p->color, dot);
    if (badge) {
        DrawIconEx(dc, rc.left, top, badge, dot, dot, 0, NULL, DI_NORMAL);
        rc.left += dot + gap;
    }

    FitNameIn(item->hwndItem, &rc, TR(L"Shortcuts for \x201C%s\x201D"), p->name, text, ARRAYSIZE(text));
    DrawTextW(dc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | Localize_ReadingFlags());
    SelectObject(dc, old);
}

/* A profile's shortcuts, taskbar pins and open windows get its current icon,
 * name and description. */
static void ApplyBadge(const Profile *before, const Profile *after, const WCHAR *icon)
{
    BOOL links;
    FinishShellWork();
    links = Shortcut_Refresh(before, after, icon);
    TaskbarPin_Refresh(before, after, icon);
    Taskbar_Refresh(after, links);   /* its open windows, if it runs */
    Icons_DeleteStale(after, icon);
}

/* Shortcuts, pins and open windows with different artwork or badge drawing
 * parameters get the current icon. */
static void HealIcons(const ProfileList *list)
{
    WCHAR icon[MAX_PATH];
    int i;
    for (i = 0; i < list->count; i++) {
        const Profile *p = &list->items[i];
        if (Icons_IsStale(&g_manager.pkg, p) && Icons_Ensure(&g_manager.pkg, p, icon, ARRAYSIZE(icon))) ApplyBadge(p, p, icon);
    }
}

/* The rows' texts and the buttons again, for the rows the list shows. */
static void UpdateRowsAndButtons(void)
{
    int i;
    for (i = 0; i < g_manager.profiles.count; i++) UpdateRow(i);
    UpdateButtons();
}

static void Refresh(BOOL rescanPackage)
{
    ProfileList fresh;
    WCHAR selected[FOLDER_CCH] = L"";
    const Profile *p = SelectedProfile();
    BOOL hadClaude = g_manager.pkg.found, sameRows;

    if (p) StringCchCopyW(selected, ARRAYSIZE(selected), p->folder);
    if (rescanPackage) Claude_FindPackage(&g_manager.pkg);
    Profiles_Load(&fresh, &g_manager.pkg);
    if (rescanPackage) HealIcons(&fresh);
    sameRows = SameProfileRows(&fresh, &g_manager.profiles) && hadClaude == g_manager.pkg.found;
    g_manager.profiles = fresh;
    if (sameRows) UpdateRowsAndButtons();
    else FillList(selected);
    SessionsView_SetProfiles(&g_manager.profiles);
    if (!StateChangesBlocked()) SessionsView_Ready(TRUE);
    UpdateStatus();
}

/* What changed shows at once, before work that keeps the window from
 * painting meanwhile: shortcuts and pins take about a second to write. */
static void ShowChanges(void)
{
    RedrawWindow(g_manager.dlg, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* What Windows shows of the program, in a new language, written on a thread
 * of its own from a copy of the profiles. */
typedef struct LanguageWork {
    ClaudePackage pkg;
    ProfileList   profiles;
} LanguageWork;

static DWORD WINAPI ApplyLanguageWork(void *param)
{
    LanguageWork *work = (LanguageWork *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Install_ApplyLanguage(&work->pkg, &work->profiles);
    if (SUCCEEDED(hr)) CoUninitialize();
    HeapFree(GetProcessHeap(), 0, work);
    return 0;
}

static void StartLanguageWork(void)
{
    LanguageWork *work = (LanguageWork *)HeapAlloc(GetProcessHeap(), 0, sizeof *work);
    if (work) {
        work->pkg = g_manager.pkg;
        work->profiles = g_manager.profiles;
        g_manager.shellWork = CreateThread(NULL, 0, ApplyLanguageWork, work, 0, NULL);
        if (g_manager.shellWork) return;
        HeapFree(GetProcessHeap(), 0, work);
    }
    Install_ApplyLanguage(&g_manager.pkg, &g_manager.profiles);   /* no thread: at once, then */
}

/* Shortcuts and pins have one writer at a time: what writes them waits for
 * the language's work first. */
static void FinishShellWork(void)
{
    if (!g_manager.shellWork) return;
    if (WaitForSingleObject(g_manager.shellWork, SHELL_WORK_WAIT_MS) != WAIT_OBJECT_0)
        Util_Log(L"the shortcuts and pins of the new language were still being written");
    CloseHandle(g_manager.shellWork);
    g_manager.shellWork = NULL;
}

/* --------------------------------------------------------- profile dialog */

typedef struct ProfileDialog {
    const Profile *existing;   /* NULL: a new profile */
    const Profile *copyFrom;   /* new profile: whose settings it can start with */
    WCHAR          name[LABEL_CCH];
    int            color;
    BOOL           openNow;
    BOOL           startup;
    BOOL           copy;
    HICON          preview;
    WCHAR          previewInitial;   /* what `preview` shows, so typing redraws it only when this changes */
    int            previewColor;
    int            previewSize;
} ProfileDialog;

/* The "Copy settings from" check box: a long profile name is cut (with an
 * ellipsis) until the caption and the box's glyph fit the control. */
static void SetCopyLabel(HWND checkBox, const WCHAR *name)
{
    WCHAR text[512];
    RECT box;
    SIZE need;
    size_t keep;
    GetClientRect(checkBox, &box);
    for (keep = wcslen(name); ; keep = Localize_ShorterCut(name, keep)) {
        Localize_FormatCutCaption(TR(L"Copy &settings from \x201C%s\x201D"), name, keep, text, ARRAYSIZE(text));
        SetWindowTextW(checkBox, text);
        if (keep <= 1 || !Theme_CheckBoxSize(checkBox, &need) || need.cx <= box.right - box.left) break;
    }
}

static BOOL ValidateProfileDialog(HWND dialog, ProfileDialog *state, BOOL showError)
{
    WCHAR raw[128], clean[LABEL_CCH], folder[FOLDER_CCH], info[MAX_PATH + 96], appData[MAX_PATH], dir[MAX_PATH];
    WCHAR placeholder[FOLDER_CCH];
    const WCHAR *error = NULL;   /* English, translated where it shows */
    BOOL ok, errorShown;

    GetDlgItemTextW(dialog, IDC_P_NAME, raw, ARRAYSIZE(raw));
    if (!state->existing) {
        ok = Core_ValidateNewName(raw, clean, ARRAYSIZE(clean), folder, ARRAYSIZE(folder), &error);
        if (ok && Util_AppData(appData, ARRAYSIZE(appData)) &&
            SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, folder)) &&
            GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
            ok = FALSE;
            error = L"A profile folder with this name already exists.";
        }
        StringCchPrintfW(placeholder, ARRAYSIZE(placeholder), PROFILE_PREFIX L"%s", TR(L"<name>"));
        StringCchPrintfW(info, ARRAYSIZE(info), TR(L"Its data will be kept in %%APPDATA%%\\%s."), ok ? folder : placeholder);
    } else {
        ok = Core_ValidateLabel(raw, clean, ARRAYSIZE(clean), &error);
        StringCchPrintfW(info, ARRAYSIZE(info), TR(L"Data folder: %%APPDATA%%\\%s%s"), state->existing->folder,
                         state->existing->isStock ? TR(L"\nThe regular Claude icon opens this profile.") : L"");
    }
    /* The error takes the place of the folder line while there is one. */
    errorShown = !ok && showError && error;
    SetDlgItemTextW(dialog, IDC_P_FOLDER, info);
    SetDlgItemTextW(dialog, IDC_P_ERROR, errorShown ? TR(error) : L"");
    ShowWindow(GetDlgItem(dialog, IDC_P_FOLDER), errorShown ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(dialog, IDC_P_ERROR), errorShown ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(dialog, IDOK), ok);
    if (ok) StringCchCopyW(state->name, ARRAYSIZE(state->name), clean);
    return ok;
}

static void UpdatePreview(HWND dialog, ProfileDialog *state)
{
    Profile sample;
    HICON icon;
    int color = (int)SendDlgItemMessageW(dialog, IDC_P_COLOR, CB_GETCURSEL, 0, 0), size;
    WCHAR initial;
    ZeroMemory(&sample, sizeof sample);
    GetDlgItemTextW(dialog, IDC_P_NAME, sample.name, ARRAYSIZE(sample.name));
    sample.color = color >= 0 && color < PALETTE_SIZE ? color : 0;
    size = MulDiv(PREVIEW_ICON_DIPS, (int)GetDpiForWindow(dialog), 96);
    initial = Icons_ProfileInitial(sample.name);
    if (state->preview && state->previewInitial == initial && state->previewColor == sample.color && state->previewSize == size)
        return;
    icon = Icons_Create(&g_manager.pkg, &sample, size);
    SendDlgItemMessageW(dialog, IDC_P_PREVIEW, STM_SETICON, (WPARAM)icon, 0);
    if (state->preview) DestroyIcon(state->preview);
    state->preview = icon;
    state->previewInitial = initial;
    state->previewColor = sample.color;
    state->previewSize = size;
}

static INT_PTR CALLBACK ProfileProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ProfileDialog *state = (ProfileDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    int i;

    switch (message) {
    case WM_INITDIALOG:
        state = (ProfileDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, state->existing ? TR(L"Edit profile") : TR(L"New profile"));
        SendDlgItemMessageW(dialog, IDC_P_NAME, EM_LIMITTEXT, state->existing ? MAX_LABEL : MAX_NAME, 0);
        for (i = 0; i < PALETTE_SIZE; i++) SendDlgItemMessageW(dialog, IDC_P_COLOR, CB_ADDSTRING, 0, (LPARAM)TR(g_ColorNames[i]));
        SendDlgItemMessageW(dialog, IDC_P_COLOR, CB_SETCURSEL, (WPARAM)(state->color >= 0 && state->color < PALETTE_SIZE ? state->color : 0), 0);
        if (state->existing) SetDlgItemTextW(dialog, IDC_P_NAME, state->existing->name);
        CheckDlgButton(dialog, IDC_P_STARTUP, state->startup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_P_OPEN, state->openNow ? BST_CHECKED : BST_UNCHECKED);
        /* A check box the dialog does not offer leaves no empty row (Theme_FitDialog closes it). */
        ShowWindow(GetDlgItem(dialog, IDC_P_OPEN), !state->existing && g_manager.pkg.found ? SW_SHOW : SW_HIDE);
        if (!state->existing && state->copyFrom) SetCopyLabel(GetDlgItem(dialog, IDC_P_COPY), state->copyFrom->name);
        else ShowWindow(GetDlgItem(dialog, IDC_P_COPY), SW_HIDE);
        ValidateProfileDialog(dialog, state, FALSE);
        UpdatePreview(dialog, state);
        SendDlgItemMessageW(dialog, IDC_P_NAME, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, IDC_P_NAME));
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);   /* Ui_Dialog fits the dialog after this message */
        return FALSE;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_P_NAME:
            if (HIWORD(wp) == EN_CHANGE) {
                ValidateProfileDialog(dialog, state, TRUE);
                UpdatePreview(dialog, state);
            }
            return TRUE;
        case IDC_P_COLOR:
            if (HIWORD(wp) == CBN_SELCHANGE) UpdatePreview(dialog, state);
            return TRUE;
        case IDOK:
            if (!ValidateProfileDialog(dialog, state, TRUE)) return TRUE;
            i = (int)SendDlgItemMessageW(dialog, IDC_P_COLOR, CB_GETCURSEL, 0, 0);
            state->color = i >= 0 && i < PALETTE_SIZE ? i : 0;
            state->openNow = IsDlgButtonChecked(dialog, IDC_P_OPEN) == BST_CHECKED;
            state->startup = IsDlgButtonChecked(dialog, IDC_P_STARTUP) == BST_CHECKED;
            state->copy = !state->existing && state->copyFrom && IsDlgButtonChecked(dialog, IDC_P_COPY) == BST_CHECKED;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_P_FOLDER);

    case WM_DPICHANGED:
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);
        break;

    case WM_APP_FIT_NAMES:
        if (state) {
            if (!state->existing && state->copyFrom) SetCopyLabel(GetDlgItem(dialog, IDC_P_COPY), state->copyFrom->name);
            UpdatePreview(dialog, state);
        }
        return TRUE;

    case WM_DESTROY:
        if (state && state->preview) {
            DestroyIcon(state->preview);
            state->preview = NULL;
        }
        break;
    }
    return FALSE;
}

/* ---------------------------------------------------------- uninstall dialog */

typedef struct UninstallDialog {
    const ProfileList *list;
    HWND rows;   /* the profiles to keep, as check boxes (in the view it scrolls in, which has its id) */
    BOOL removeData[MAX_PROFILES];
    int  rowProfile[MAX_PROFILES];   /* each row's index in list */
    int  rowCount;
} UninstallDialog;

static INT_PTR CALLBACK UninstallProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    UninstallDialog *state = (UninstallDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    HWND list;
    WCHAR text[LABEL_CCH + MAX_PATH + 256];
    LVCOLUMNW column;
    LVITEMW item;
    int i;

    switch (message) {
    case WM_INITDIALOG:
        state = (UninstallDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetKeepText(dialog, state->list);
        list = state->rows = GetDlgItem(dialog, IDC_U_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(list, 0, &column);   /* the view it scrolls in gives it the list's width */
        state->rowCount = 0;
        for (i = 0; i < state->list->count; i++) {
            const Profile *p = &state->list->items[i];
            WCHAR target[MAX_PATH];
            if (p->isStock) continue;
            if (Profiles_LinkTarget(p, target, ARRAYSIZE(target)))
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (linked folder, kept: %s)"), p->name, target);
            else
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (%%APPDATA%%\\%s)"), p->name, p->folder);
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT;
            item.iItem = state->rowCount;
            item.pszText = text;
            ListView_InsertItem(list, &item);
            ListView_SetCheckState(list, state->rowCount, TRUE);
            state->rowProfile[state->rowCount++] = i;
        }
        list = Theme_SmoothView(list);   /* from here, the view that has its place */
        if (state->rowCount == 0) {   /* nothing to keep: their rows close (Theme_FitDialog) */
            ShowWindow(GetDlgItem(dialog, IDC_U_LABEL), SW_HIDE);
            ShowWindow(list, SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_U_HINT), SW_HIDE);
        }
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);   /* Ui_Dialog fits the dialog after this message */
        return TRUE;

    case WM_DPICHANGED:
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);
        break;

    case WM_APP_FIT_NAMES:
        if (state) SetKeepText(dialog, state->list);
        return TRUE;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_U_HINT);

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) == IDOK) {
            WCHAR ask[2048] = L"";
            int drop = 0, links = 0;
            list = state->rows;
            ZeroMemory(state->removeData, sizeof state->removeData);
            for (i = 0; i < state->rowCount; i++) {
                int index = state->rowProfile[i];
                state->removeData[index] = !ListView_GetCheckState(list, i);
                if (!state->removeData[index]) continue;
                if (Profiles_IsLinked(&state->list->items[index])) links++;
                else drop++;
            }
            if (drop > 0)
                StringCchPrintfW(ask, ARRAYSIZE(ask), drop == 1
                    ? TR(L"The data of %d profile will be moved to the Recycle Bin: its sign-in, local history, Claude Code and Cowork files.\n\n")
                    : TR(L"The data of %d profiles will be moved to the Recycle Bin: their sign-in, local history, Claude Code and Cowork files.\n\n"),
                    drop);
            if (links > 0) {
                WCHAR more[1024];
                StringCchPrintfW(more, ARRAYSIZE(more), links == 1
                    ? TR(L"%d linked profile will be removed from " APP_NAME L"; the folder it links to stays on disk.\n\n")
                    : TR(L"%d linked profiles will be removed from " APP_NAME L"; the folders they link to stay on disk.\n\n"),
                    links);
                StringCchCatW(ask, ARRAYSIZE(ask), more);
            }
            if (drop + links > 0) {
                StringCchCatW(ask, ARRAYSIZE(ask), TR(L"Continue?"));
                if (Ui_Message(dialog, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2, L"%s", ask) != IDOK) return TRUE;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- actions */

static void OpenProfile(const Profile *profile)
{
    Profile p = *profile;
    BOOL identity = FALSE;
    HRESULT hr;
    if (!g_manager.pkg.found) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"Claude Desktop is not installed."));
        return;
    }
    hr = Launcher_Open(&g_manager.pkg, &p, NULL, NULL, &identity);
    if (FAILED(hr)) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Claude could not be started (error 0x%08lX)."), (unsigned long)hr);
        return;
    }
    Util_Log(L"opened %s from the manager%s", p.folder, identity ? L"" : L" without package identity");
}

static void DoOpen(void)
{
    const Profile *selected = SelectedProfile();
    if (selected) OpenProfile(selected);
}

static int FreeColor(void)
{
    BOOL used[PALETTE_SIZE] = { 0 };
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (g_manager.profiles.items[i].color >= 0 && g_manager.profiles.items[i].color < PALETTE_SIZE)
            used[g_manager.profiles.items[i].color] = TRUE;
    for (i = 0; i < PALETTE_SIZE; i++)
        if (!used[i]) return i;
    return g_manager.profiles.count % PALETTE_SIZE;
}

static void SetProfileStartup(const Profile *profile, BOOL enabled)
{
    if (FAILED(Shortcut_SetStartup(&g_manager.pkg, profile, enabled)))
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The Windows sign-in setting of \x201C%s\x201D could not be changed."), profile->name);
}

static void DoNew(void)
{
    ProfileDialog dialog;
    Profile source, created;
    WCHAR folder[FOLDER_CCH], error[256];
    int i;
    if (g_manager.profiles.count >= MAX_PROFILES) {
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(APP_NAME L" handles up to %d profiles."), MAX_PROFILES);
        return;
    }
    ZeroMemory(&dialog, sizeof dialog);
    dialog.color = FreeColor();
    dialog.openNow = g_manager.pkg.found;
    /* Settings come from the selected profile, else the default one. */
    ZeroMemory(&source, sizeof source);
    if (SelectedProfile()) source = *SelectedProfile();
    else if ((i = Profiles_DefaultIndex(&g_manager.profiles)) >= 0) source = g_manager.profiles.items[i];
    dialog.copyFrom = source.folder[0] ? &source : NULL;
    if (Ui_Dialog(g_manager.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&dialog) != IDOK || StateChangesBlocked()) return;
    FinishShellWork();
    if (!Profiles_Create(dialog.name, dialog.color, folder, ARRAYSIZE(folder), error, ARRAYSIZE(error))) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s", error);
        return;
    }
    Refresh(FALSE);
    i = Profiles_Find(&g_manager.profiles, folder);
    if (i < 0) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The profile was created but cannot be listed: " APP_NAME L" handles up to %d profiles."), MAX_PROFILES);
        return;
    }
    SelectProfileRow(folder);
    ShowChanges();
    created = g_manager.profiles.items[i];
    if (dialog.copy) Profiles_CopySettings(&source, &created);
    if (dialog.startup) SetProfileStartup(&created, TRUE);
    if (dialog.openNow && !StateChangesBlocked()) OpenProfile(&created);
}

static void DoEdit(void)
{
    const Profile *selected = SelectedProfile();
    Profile before, after;
    ProfileDialog dialog;
    WCHAR icon[MAX_PATH];
    BOOL atStartup;
    if (!selected) return;
    before = *selected;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.existing = &before;
    dialog.color = before.color;
    dialog.startup = atStartup = Shortcut_IsAtStartup(&before);
    if (Ui_Dialog(g_manager.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&dialog) != IDOK || StateChangesBlocked()) return;
    FinishShellWork();
    if (!Profiles_Update(before.folder, dialog.name, dialog.color)) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The profile could not be saved."));
        return;
    }
    after = before;
    StringCchCopyW(after.name, ARRAYSIZE(after.name), dialog.name);
    after.color = dialog.color;
    /* The list shows the new name and color first; its shortcuts and pins follow. */
    Refresh(FALSE);
    ShowChanges();
    if ((wcscmp(before.name, after.name) != 0 || before.color != after.color) &&
        Icons_Ensure(&g_manager.pkg, &after, icon, ARRAYSIZE(icon)))
        ApplyBadge(&before, &after, icon);
    if (dialog.startup != atStartup) SetProfileStartup(&after, dialog.startup);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

static BOOL ConfirmDelete(const Profile *p)
{
    WCHAR text[512 + MAX_PATH], target[MAX_PATH];
    if (Profiles_LinkTarget(p, target, ARRAYSIZE(target)))
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Delete the profile \x201C%s\x201D?\n\nIts folder is a link to\n%s\nThe data in that folder stays on disk. The link, the profile's shortcuts and Claude's local files for it (logs and cache) are removed."),
                         p->name, target);
    else
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Delete the profile \x201C%s\x201D?\n\nIts data folder (sign-in, local history, Claude Code and Cowork files) goes to the Recycle Bin and its shortcuts are removed."),
                         p->name);
    return Ui_Ask(g_manager.dlg, IDI_WARNING, text, TR(L"Delete profile"), TR(L"Cancel"), TRUE);
}

static BOOL RefuseIfRunning(const Profile *p)
{
    if (!Claude_IsRunning(p)) return FALSE;
    Ui_Message(g_manager.dlg, MB_ICONWARNING,
               TR(L"Quit Claude for the \x201C%s\x201D profile first (right-click its icon in the notification area and choose Quit), then try again."),
               p->name);
    return TRUE;
}

static void DoDelete(void)
{
    const Profile *selected = SelectedProfile();
    Profile p;
    if (!selected || selected->isStock) return;
    p = *selected;
    if (RefuseIfRunning(&p)) return;
    if (!ConfirmDelete(&p) || StateChangesBlocked()) return;
    /* It may have been started while the question was open. */
    if (RefuseIfRunning(&p)) {
        Refresh(FALSE);
        return;
    }
    FinishShellWork();
    /* REMOVE_CANCELLED (the user answered No to Windows' "delete
     * permanently?") needs no message. */
    if (Profiles_Delete(g_manager.dlg, &p) == REMOVE_FAILED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"\x201C%s\x201D could not be deleted completely. Make sure Claude is closed for this profile and try again."),
                   p.name);
    Refresh(FALSE);
}

/* The registry watch reloads the profiles after the write (WatchOutside):
 * the change shows here at once, without a second reload. */
static void DoSetDefault(void)
{
    const Profile *selected = SelectedProfile();
    if (!selected) return;
    if (!Profiles_SetDefault(selected->folder)) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The default profile could not be changed."));
        return;
    }
    StringCchCopyW(g_manager.profiles.defaultFolder, ARRAYSIZE(g_manager.profiles.defaultFolder), selected->folder);
    UpdateRowsAndButtons();
    SessionsView_SetProfiles(&g_manager.profiles);
}

static void CreateShortcutAt(const Profile *p, const WCHAR *path)
{
    HRESULT hr;
    FinishShellWork();
    hr = Shortcut_CreateForProfile(&g_manager.pkg, p, path);
    if (FAILED(hr))
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

static void DoDesktopShortcut(void)
{
    const Profile *selected = SelectedProfile();
    Profile p;
    WCHAR path[MAX_PATH];
    if (!selected) return;
    p = *selected;
    if (!Shortcut_DesktopPathFor(&p, path, ARRAYSIZE(path))) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The desktop folder is not available."));
        return;
    }
    CreateShortcutAt(&p, path);
}

static void DoPinTaskbar(void)
{
    const Profile *selected = SelectedProfile();
    Profile p;
    HRESULT hr;
    if (!selected) return;
    p = *selected;
    FinishShellWork();
    hr = TaskbarPin_Pin(&g_manager.pkg, &p);
    if (FAILED(hr))
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"\x201C%s\x201D could not be pinned to the taskbar (error 0x%08lX)."),
                   p.name, (unsigned long)hr);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

/* The profiles and the sessions share the window: Sessions swaps them, and
 * becomes "< Back" in the same place. */
static const int kProfileControls[] = { IDC_LIST, IDC_OPEN, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT, IDC_NOTE,
                                        IDC_SC_GROUP, IDC_SC_DESKTOP, IDC_SC_SAVEAS, IDC_SC_PIN, IDC_SC_START };

static const WCHAR *SessionsButtonCaption(BOOL sessionsShown)
{
    return TR(Theme_MainCaption(IDC_SESSIONS, sessionsShown));
}

/* The view shown gets the focus before the other one hides. */
void Gui_ShowSessions(HWND dialog, const ClaudePackage *package, BOOL showSessions, const WCHAR *folder)
{
    size_t i;
    if (showSessions) {
        SessionsView_Enter(package, folder);
        FocusView(dialog, TRUE);
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_HIDE);
    } else {
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_SHOW);
        FocusView(dialog, FALSE);
        SessionsView_Leave();
    }
    SetDlgItemTextW(dialog, IDC_SESSIONS, SessionsButtonCaption(showSessions));
    Theme_LayoutMain(dialog);
    SessionsView_Resize();
}

static void ToggleSessions(void)
{
    if (!SessionsView_Shown()) {
        const Profile *p = SelectedProfile();
        Gui_ShowSessions(g_manager.dlg, &g_manager.pkg, TRUE, p ? p->folder : NULL);
    } else {
        WCHAR folder[FOLDER_CCH];
        StringCchCopyW(folder, ARRAYSIZE(folder), SessionsView_Profile());
        Gui_ShowSessions(g_manager.dlg, &g_manager.pkg, FALSE, folder);
        SelectProfileRow(folder);   /* the profile the sessions view showed */
    }
}

/* Adds the profile to the Start menu, or takes it out: looked up again
 * first, the entry may have changed since the button was drawn. */
static void DoStartMenu(void)
{
    const Profile *selected = SelectedProfile();
    Profile p;
    HRESULT hr;
    if (!selected) return;
    p = *selected;
    FinishShellWork();
    if (Shortcut_IsInStartMenu(&p)) {
        if (FAILED(hr = Shortcut_RemoveFromStartMenu(&p)))
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"\x201C%s\x201D could not be removed from the Start menu (error 0x%08lX)."),
                       p.name, (unsigned long)hr);
    } else if (FAILED(hr = Shortcut_AddToStartMenu(&g_manager.pkg, &p))) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"\x201C%s\x201D could not be added to the Start menu (error 0x%08lX)."),
                   p.name, (unsigned long)hr);
    }
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

/* The folders the shortcut, pin and Start menu buttons read: a change there
 * checks those buttons again, whoever made it (interrupt level: also a file
 * written without the shell). */
static void WatchShortcutFolders(void)
{
    const GUID *known[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop, &FOLDERID_Programs };
    SHChangeNotifyEntry entries[ARRAYSIZE(known) + 2];
    WCHAR pins[MAX_PATH], start[MAX_PATH];
    int n = 0;
    size_t i;
    for (i = 0; i < ARRAYSIZE(known); i++) {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(SHGetKnownFolderIDList(known[i], 0, NULL, &pidl)) && pidl) {
            entries[n].pidl = pidl;
            entries[n++].fRecursive = FALSE;
        }
    }
    if (TaskbarPin_Dir(pins, ARRAYSIZE(pins)) && (entries[n].pidl = ILCreateFromPathW(pins)) != NULL) entries[n++].fRecursive = FALSE;
    if (Shortcut_StartMenuDir(start, ARRAYSIZE(start)) && (entries[n].pidl = ILCreateFromPathW(start)) != NULL)
        entries[n++].fRecursive = FALSE;
    if (n > 0)
        g_manager.shortcutsNotify = SHChangeNotifyRegister(g_manager.dlg, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                                           SHCNE_CREATE | SHCNE_DELETE | SHCNE_RENAMEITEM | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR,
                                                           WM_APP_SHORTCUTS, n, entries);
    /* Those buttons are then checked again only when the window is activated. */
    if (!g_manager.shortcutsNotify)
        Util_Log(L"the shortcut folders are not watched (%d of %d found)", n, (int)ARRAYSIZE(entries));
    for (i = 0; i < (size_t)n; i++) CoTaskMemFree((void *)entries[i].pidl);
}

/* What the window shows can change outside it: a profile folder made or
 * removed, a name or color written by another copy, the default profile, the
 * claude:// app picked in Windows, Claude installed or updated, a taskbar pin
 * added or removed. A thread waits for the matching registry and folder
 * notifications and tells the window, which refreshes once for a burst of
 * them. */
typedef struct OutsideKey {
    const WCHAR *path;
    BOOL         subtree;
    DWORD        filter;
    LONG         change;
} OutsideKey;

static const OutsideKey kOutsideKeys[] = {
    /* Its own values and subkeys only: Update and Shortcuts are the manager's own records. */
    { REG_ROOT, FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PROFILES },
    { REG_PROFILES, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PROFILES },
    { L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations", TRUE,
      REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_LINKS },
    { REG_PACKAGES, FALSE, REG_NOTIFY_CHANGE_NAME, CHANGE_PACKAGES },
    { REG_TASKBAND, FALSE, REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PINS },
};

/* Opens the key when needed and arms its notification. A key that does not
 * exist or was deleted stays closed and is tried again after the next change
 * the thread sees: for Profiles (missing before the first profile), REG_ROOT's
 * watch reports the change that creates it. */
static void ArmOutsideKey(const OutsideKey *source, HKEY *key, HANDLE event)
{
    if (!*key && RegOpenKeyExW(HKEY_CURRENT_USER, source->path, 0, KEY_NOTIFY, key) != ERROR_SUCCESS) {
        *key = NULL;
        return;
    }
    if (RegNotifyChangeKeyValue(*key, source->subtree, source->filter, event, TRUE) != ERROR_SUCCESS) {
        RegCloseKey(*key);
        *key = NULL;
    }
}

static DWORD WINAPI WatchOutside(void *param)
{
    HWND dialog = (HWND)param;
    HANDLE handles[ARRAYSIZE(kOutsideKeys) + 2], dirs = INVALID_HANDLE_VALUE;
    HKEY keys[ARRAYSIZE(kOutsideKeys) + 2] = { 0 };
    LONG changes[ARRAYSIZE(kOutsideKeys) + 2];
    const OutsideKey *which[ARRAYSIZE(kOutsideKeys) + 2] = { 0 };
    WCHAR appData[MAX_PATH];
    DWORD n = 0, i, k, signaled;

    handles[n] = g_manager.outsideStop;
    changes[n++] = 0;
    for (k = 0; k < ARRAYSIZE(kOutsideKeys); k++) {
        HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!event) continue;
        handles[n] = event;
        which[n] = &kOutsideKeys[k];
        changes[n] = kOutsideKeys[k].change;
        ArmOutsideKey(which[n], &keys[n], event);
        n++;
    }
    /* Profile folders sit directly in %APPDATA%. */
    if (Util_AppData(appData, ARRAYSIZE(appData)) &&
        (dirs = FindFirstChangeNotificationW(appData, FALSE, FILE_NOTIFY_CHANGE_DIR_NAME)) != INVALID_HANDLE_VALUE) {
        handles[n] = dirs;
        changes[n++] = CHANGE_PROFILES;
    }

    for (;;) {
        signaled = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
        if (signaled == WAIT_OBJECT_0 || signaled >= WAIT_OBJECT_0 + n) break;
        i = signaled - WAIT_OBJECT_0;
        /* Armed again first, so a change during the refresh still counts;
         * a key missing until now may exist. A folder watch that cannot be
         * armed again would stay signaled: it is dropped (it is the last). */
        if (handles[i] == dirs && !FindNextChangeNotification(dirs)) {
            Util_Log(L"the profile folders are no longer watched (error %lu)", GetLastError());
            FindCloseChangeNotification(dirs);
            dirs = INVALID_HANDLE_VALUE;
            n--;
        }
        for (k = 1; k < n; k++)
            if (which[k] && (k == i || !keys[k])) ArmOutsideKey(which[k], &keys[k], handles[k]);
        if (InterlockedOr(&g_outside, changes[i]) == 0) PostMessageW(dialog, WM_APP_OUTSIDE, 0, 0);
    }

    for (i = 1; i < n; i++) {
        if (handles[i] == dirs) {
            FindCloseChangeNotification(dirs);
        } else {
            if (keys[i]) RegCloseKey(keys[i]);
            CloseHandle(handles[i]);
        }
    }
    return 0;
}

static void StartWatchingOutside(HWND dialog)
{
    g_manager.outsideStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_manager.outsideStop) g_manager.outsideThread = CreateThread(NULL, 0, WatchOutside, dialog, 0, NULL);
}

static void StopWatchingOutside(void)
{
    if (g_manager.outsideStop) SetEvent(g_manager.outsideStop);
    if (g_manager.outsideThread) {
        /* The worker posts UI notifications and never waits for this thread. */
        WaitForSingleObject(g_manager.outsideThread, INFINITE);
        CloseHandle(g_manager.outsideThread);
    }
    if (g_manager.outsideStop) CloseHandle(g_manager.outsideStop);
    g_manager.outsideThread = g_manager.outsideStop = NULL;
}

static void DoSaveShortcut(void)
{
    const Profile *selected = SelectedProfile();
    Profile p;
    IFileSaveDialog *saveDialog = NULL;
    IShellItem *desktop = NULL, *result = NULL;
    COMDLG_FILTERSPEC filter = { TR(L"Shortcut (*.lnk)"), L"*.lnk" };
    WCHAR name[MAX_PATH], path[MAX_PATH];
    PWSTR chosen = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    HRESULT hr;

    if (!selected) return;
    p = *selected;
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&saveDialog);
    if (FAILED(hr)) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
        return;
    }
    Core_ShortcutFileName(p.name, 1, name, ARRAYSIZE(name));
    IFileSaveDialog_SetTitle(saveDialog, TR(L"Create shortcut"));
    IFileSaveDialog_SetFileTypes(saveDialog, 1, &filter);
    IFileSaveDialog_SetDefaultExtension(saveDialog, L"lnk");
    IFileSaveDialog_SetFileName(saveDialog, name);
    if (SUCCEEDED(IFileSaveDialog_GetOptions(saveDialog, &options)))
        IFileSaveDialog_SetOptions(saveDialog, options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(SHCreateItemInKnownFolder(&FOLDERID_Desktop, 0, NULL, &IID_IShellItem, (void **)&desktop))) {
        IFileSaveDialog_SetDefaultFolder(saveDialog, desktop);
        IShellItem_Release(desktop);
    }
    hr = IFileSaveDialog_Show(saveDialog, g_manager.dlg);
    if (SUCCEEDED(hr) && !StateChangesBlocked() && SUCCEEDED(hr = IFileSaveDialog_GetResult(saveDialog, &result))) {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &chosen))) {
            if (FAILED(StringCchCopyW(path, ARRAYSIZE(path), chosen)) ||
                (!Core_EndsWithI(path, L".lnk") && FAILED(StringCchCatW(path, ARRAYSIZE(path), L".lnk"))))
                Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The path is too long."));
            else
                CreateShortcutAt(&p, path);
            CoTaskMemFree(chosen);
        }
        IShellItem_Release(result);
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
    IFileSaveDialog_Release(saveDialog);
}

/* An uninstall cancelled, or refused by Install_Uninstall (which said why):
 * the window shows the state again. */
static void EndUninstallAttempt(void)
{
    g_manager.uninstallInProgress = FALSE;
    if (StateChangesBlocked()) return;
    g_manager.shortcutStateFolder[0] = 0;
    Refresh(TRUE);
    ShowVersion();
}

/* Works on a private copy of the list: the dialog's choices are indexed on
 * it, and nothing reloads it until the end. Not while the new release
 * downloads or installs: its thread still writes the download. */
static void DoUninstall(void)
{
    UninstallDialog dialog;
    ProfileList snapshot;
    WCHAR stock[LABEL_CCH];
    int stockIndex;

    if (g_manager.uninstallInProgress || g_manager.uninstalled) return;
    if (g_manager.updating) {
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(APP_NAME L" is being updated. Try again once the update is done."));
        return;
    }
    FinishShellWork();
    Refresh(TRUE);
    snapshot = g_manager.profiles;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.list = &snapshot;
    g_manager.uninstallInProgress = TRUE;
    if (Ui_Dialog(g_manager.dlg, IDD_UNINSTALL, UninstallProc, (LPARAM)&dialog) != IDOK || g_manager.closing) {
        EndUninstallAttempt();
        return;
    }
    Claude_UpdateRunning(&snapshot);
    stockIndex = Profiles_Find(&snapshot, STOCK_FOLDER);
    StringCchCopyW(stock, ARRAYSIZE(stock), stockIndex >= 0 ? snapshot.items[stockIndex].name : STOCK_DEFAULT_NAME);
    if (!Install_Uninstall(g_manager.dlg, &snapshot, dialog.removeData)) {
        EndUninstallAttempt();
        return;
    }
    g_manager.uninstalled = TRUE;
    g_manager.uninstallInProgress = FALSE;
    Ui_Message(g_manager.dlg, MB_ICONINFORMATION,
               TR(APP_NAME L" has been removed.\n\nClaude Desktop and its \x201C%s\x201D profile are untouched."), stock);
    EndDialog(g_manager.dlg, 0);
}

/* Whether `owner` owns `window`, directly or through other windows. */
static BOOL OwnedBy(HWND window, HWND owner)
{
    HWND above;
    for (above = window ? GetWindow(window, GW_OWNER) : NULL; above; above = GetWindow(above, GW_OWNER))
        if (above == owner) return TRUE;
    return FALSE;
}

static BOOL CALLBACK CloseOwnedProc(HWND window, LPARAM owner)
{
    if (OwnedBy(window, (HWND)owner)) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}

static void Close(HWND dialog)
{
    g_manager.closing = TRUE;
    EnumThreadWindows(GetCurrentThreadId(), CloseOwnedProc, (LPARAM)dialog);
    EndDialog(dialog, 0);
}

/* Only the user can make Claude Desktop Profiles Manager the app for
 * claude:// links, in Windows' own chooser: say what to pick, then let Windows
 * ask. */
static void SetUpLinks(void)
{
    WCHAR exe[MAX_PATH];
    if (!g_manager.pkg.found || Handler_UserChoice() == USERCHOICE_OURS) return;
    if (!Ui_Ask(g_manager.dlg, IDI_INFORMATION,
                TR(L"Windows now asks which app opens claude:// links.\n\n"
                   L"Choose \x201C" APP_NAME L"\x201D, then Always (or Set default). Sign-ins then always come back to the window that started them."),
                TR(L"Continue"), TR(L"Later"), FALSE))
        return;
    /* Unregistered, the program would not be in the list the user is sent to. */
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Handler_Register(exe)) {
        Util_Log(L"could not register for claude:// links (error %lu)", GetLastError());
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(APP_NAME L" could not be registered as an app for claude:// links."));
        return;
    }
    if (!Handler_AskUser())
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"Windows kept the app it uses for claude:// links.\n\n"
                      L"Open Settings > Apps > Default apps > Choose defaults by link type, find CLAUDE and choose \x201C" APP_NAME L"\x201D."));
    UpdateStatus();
}

static void DoStatusAction(void)
{
    if (g_manager.statusAction == STATUS_ACTION_GET_CLAUDE) Util_OpenUrl(APP_DOWNLOAD_URL);
    else if (g_manager.statusAction == STATUS_ACTION_SET_UP_LINKS) SetUpLinks();
}

/* ------------------------------------------------------------ new release */

/* The footer while the new release downloads or installs. */
static void ShowUpdateProgress(void)
{
    WCHAR version[32], text[128];
    if (g_manager.installing) {
        SetTextIfChanged(IDC_ABOUT, TR(Theme_MainFooter(MAIN_FOOTER_INSTALLING)));
    } else if (Update_Available(version, ARRAYSIZE(version))) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainFooter(MAIN_FOOTER_DOWNLOADING)), version);
        SetTextIfChanged(IDC_ABOUT, text);
    }
}

/* A newer release shows next to the version, with its Update button. */
static void ShowUpdate(void)
{
    WCHAR version[32], text[256];
    if (g_manager.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainFooter(MAIN_FOOTER_AVAILABLE)), APP_VERSION_WSTR, version);
    SetTextIfChanged(IDC_ABOUT, text);
    EnableControl(IDC_UPDATE, TRUE);   /* an update that failed or was dropped turned it off */
    ShowOptionalButton(IDC_UPDATE, TRUE);
}

static void DoUpdate(void)
{
    WCHAR version[32];
    if (g_manager.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    g_manager.updating = TRUE;
    EnableControl(IDC_UPDATE, FALSE);
    ShowUpdateProgress();
    Update_Download(g_manager.dlg, WM_APP_DOWNLOADED);
}

static void ShowVersion(void)
{
    WCHAR text[512];
    if (g_manager.updating) {
        ShowUpdateProgress();
        return;
    }
    StringCchPrintfW(text, ARRAYSIZE(text),
                     TR(Theme_MainFooter(MAIN_FOOTER_CREDITS)),
                     APP_VERSION_WSTR, APP_AUTHOR_URL);
    SetTextIfChanged(IDC_ABOUT, text);
    ShowUpdate();
}

static void SetColumnTitles(void)
{
    LVCOLUMNW column;
    const WCHAR *title;
    int i;
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_TEXT;
    for (i = 0; (title = Theme_ProfileColumnTitle(i)) != NULL; i++) {
        column.pszText = (LPWSTR)TR(title);
        ListView_SetColumn(g_manager.list, i, &column);
    }
}

static void DoLanguage(void)
{
    HMENU menu;
    RECT button;
    UINT command;
    int i, choice, languageBefore = Localize_EffectiveLanguage();
    if (StateChangesBlocked()) return;
    menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (Localize_CurrentLanguage() < 0 ? MF_CHECKED : 0), 1, Localize_LanguageName(-1));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < Localize_LanguageCount(); i++)
        AppendMenuW(menu, MF_STRING | (Localize_CurrentLanguage() == i ? MF_CHECKED : 0),
                    (UINT_PTR)(i + 2), Localize_LanguageName(i));
    GetWindowRect(GetDlgItem(g_manager.dlg, IDC_LANGUAGE), &button);
    command = Theme_TrackDropDown(g_manager.dlg, menu, &button);
    DestroyMenu(menu);
    /* The window may have been asked to close while the menu was open. */
    if (!command || StateChangesBlocked()) return;
    choice = command == 1 ? -1 : (int)command - 2;
    FinishShellWork();   /* the previous language's, written in that language */
    if (!Localize_SetLanguage(choice, TRUE)) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The language preference could not be saved."));
        return;
    }
    Localize_Window(g_manager.dlg);
    Theme_Apply(g_manager.dlg);
    SetColumnTitles();
    SetTextIfChanged(IDC_SESSIONS, SessionsButtonCaption(SessionsView_Shown()));
    /* The registry watch reloads the profiles after the write: only the texts change here. */
    UpdateRowsAndButtons();
    UpdateStatus();
    ShowVersion();
    ReflowMain();
    SessionsView_Reload();
    /* What Windows shows of the program speaks the new language too, in
     * the background, once the window does. */
    if (Localize_EffectiveLanguage() != languageBefore) {
        ShowChanges();
        StartLanguageWork();
    }
}

/* The new release installs itself and closes this window. `error`: Windows'
 * answer that goes with `result`. */
static void Downloaded(UpdateResult result, DWORD error)
{
    BOOL openPage = FALSE;
    if (StateChangesBlocked()) {
        g_manager.updating = FALSE;
        g_manager.installing = FALSE;
        Update_RemoveDownload();
        return;
    }
    if (result == UPDATE_READY) {
        FinishShellWork();   /* the new copy replaces this one */
        result = Update_Run(&error);
    }
    if (result == UPDATE_STARTED) {
        g_manager.installing = TRUE;
        ShowUpdateProgress();
        return;
    }
    g_manager.updating = FALSE;
    g_manager.installing = FALSE;
    if (result == UPDATE_NOT_SIGNED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The new version was not installed: the downloaded file is not signed by the author of " APP_NAME L"."));
    else if (result == UPDATE_NOT_VERIFIED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The new version was not installed: Windows could not check its signature (error 0x%08lX). Try again later."),
                   (unsigned long)error);
    else if (result == UPDATE_WRONG_VERSION)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The new version was not installed: the downloaded file is not the version that was announced."));
    else if (result == UPDATE_NOT_STARTED)
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version could not be started (error %lu).\n\nOpen its download page?"), error) == IDYES;
    else if (error != ERROR_SUCCESS)
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version could not be downloaded (error %lu).\n\nOpen its download page?"), error) == IDYES;
    else
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version could not be downloaded.\n\nOpen its download page?")) == IDYES;
    if (openPage) Util_OpenUrl(APP_RELEASES_URL);
    ShowVersion();
}

/* ----------------------------------------------------------- main window */

static void SetIcons(HWND dialog)
{
    UINT dpi = GetDpiForWindow(dialog);
    HICON big = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), 0);
    HICON little = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
    SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)little);
    if (g_manager.bigIcon) DestroyIcon(g_manager.bigIcon);
    if (g_manager.smallIcon) DestroyIcon(g_manager.smallIcon);
    g_manager.bigIcon = big;
    g_manager.smallIcon = little;
}

static void AddColumns(void)
{
    LVCOLUMNW column;
    int i;
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_WIDTH;
    column.cx = FIRST_COLUMN_WIDTH;
    for (i = 0; Theme_ProfileColumnTitle(i); i++) ListView_InsertColumn(g_manager.list, i, &column);
    SetColumnTitles();
    LayoutColumns();
}

static void LayoutMainControls(void)
{
    if (!g_manager.layoutReady || g_manager.layingOut) return;
    g_manager.layingOut = TRUE;
    Theme_LayoutMain(g_manager.dlg);
    LayoutColumns();
    SessionsView_Resize();
    g_manager.layingOut = FALSE;
}

/* Fits the window to its controls in the current language, fonts and scale. */
static void ReflowMain(void)
{
    Theme_FitDialog(g_manager.dlg);   /* places the controls too */
    g_manager.layoutReady = TRUE;
    LayoutColumns();
    UpdateNote();
    SessionsView_Resize();
    SessionsView_Relayout();
}

static BOOL MainMinimumFrame(HWND dialog, SIZE *size)
{
    SIZE client;
    RECT frame;
    MONITORINFO monitor = { sizeof monitor };
    if (!Theme_MainMinimum(dialog, &client)) return FALSE;
    SetRect(&frame, 0, 0, client.cx, client.cy);
    if (!AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dialog, GWL_STYLE), GetMenu(dialog) != NULL,
                                  (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog))) return FALSE;
    size->cx = frame.right - frame.left;
    size->cy = frame.bottom - frame.top;
    if (GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
        size->cx = min(size->cx, monitor.rcWork.right - monitor.rcWork.left);
        size->cy = min(size->cy, monitor.rcWork.bottom - monitor.rcWork.top);
    }
    return TRUE;
}

/* WM_GETMINMAXINFO and WM_DPICHANGED of a window made from IDD_MAIN: its
 * minimum frame, and its place on a monitor with another scale. Returns
 * whether the message was handled. */
BOOL Gui_MainWindowGeometry(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SIZE minimum = { 0, 0 };
    (void)wp;
    if (message == WM_GETMINMAXINFO) {
        MINMAXINFO *info = (MINMAXINFO *)lp;
        if (!info || !MainMinimumFrame(dialog, &minimum)) return FALSE;
        info->ptMinTrackSize.x = minimum.cx;
        info->ptMinTrackSize.y = minimum.cy;
        return TRUE;
    }
    if (message == WM_DPICHANGED) {
        const RECT *suggested = (const RECT *)lp;
        Theme_Apply(dialog);
        MainMinimumFrame(dialog, &minimum);
        if (suggested) {
            MONITORINFO monitor = { sizeof monitor };
            int width = max(minimum.cx, suggested->right - suggested->left);
            int height = max(minimum.cy, suggested->bottom - suggested->top);
            int x = suggested->left, y = suggested->top;
            if (GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor)) {
                width = min(width, monitor.rcWork.right - monitor.rcWork.left);
                height = min(height, monitor.rcWork.bottom - monitor.rcWork.top);
                x = min(max(x, monitor.rcWork.left), monitor.rcWork.right - width);
                y = min(max(y, monitor.rcWork.top), monitor.rcWork.bottom - height);
            }
            SetWindowPos(dialog, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK MainProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_INITDIALOG:
        g_manager.dlg = dialog;
        Localize_Window(dialog);
        g_manager.list = GetDlgItem(dialog, IDC_LIST);
        g_manager.groupColor = -1;
        SetIcons(dialog);
        ListView_SetExtendedListViewStyle(g_manager.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        AddColumns();
        Theme_SmoothView(g_manager.list);
        SessionsView_Init(dialog);
        Theme_SetStrong(GetDlgItem(dialog, IDC_SESSIONS));   /* it leads to the other view */
        Theme_Apply(dialog);
        Theme_RememberLayout(dialog);
        ShowVersion();
        Claude_FindPackage(&g_manager.pkg);
        Profiles_Load(&g_manager.profiles, &g_manager.pkg);
        SessionsView_SetProfiles(&g_manager.profiles);
        TaskbarPin_RepairOurs();
        HealIcons(&g_manager.profiles);
        FillList(NULL);
        UpdateStatus();
        ReflowMain();
        WatchShortcutFolders();
        StartWatchingOutside(dialog);
        /* Started to uninstall (Settings > Apps): only the uninstall dialog
         * shows, and the manager never opens (wParam 1). */
        if (lp == GUI_UNINSTALL) {
            g_manager.uninstallOnly = TRUE;
            PostMessageW(dialog, WM_APP_UNINSTALL, 1, 0);
        } else if (lp == GUI_SET_UP_LINKS) {
            PostMessageW(dialog, WM_APP_SETUPLINKS, 0, 0);
        }
        if (lp != GUI_UNINSTALL) Update_Check(dialog, WM_APP_UPDATE);
        if (g_manager.readyEvent) SetEvent(g_manager.readyEvent);   /* a second launch can now find this window */
        SetFocus(g_manager.list);
        return FALSE;

    case WM_SHOWWINDOW:
        if (wp && !g_manager.uninstallOnly) SessionsView_Warm(&g_manager.profiles);
        break;

    case WM_APP_SESSIONS_READY:
        SessionsView_Ready(!StateChangesBlocked());
        return TRUE;

    case WM_APP_OUTSIDE: {
        LONG changes = InterlockedExchange(&g_outside, 0);
        if (!StateChangesBlocked()) {
            if (changes & CHANGE_PINS) g_manager.shortcutStateFolder[0] = 0;
            /* The sessions view reloads by itself when its profiles change. */
            Refresh((changes & CHANGE_PACKAGES) != 0);
        }
        return TRUE;
    }

    case WM_APP_SESSIONS:
        if (!StateChangesBlocked()) SessionsView_Reload();
        return TRUE;

    case WM_ACTIVATE:
        /* Back from another window: which profiles run may have changed, and
         * the sessions view shows it; the shortcut, pin and Start menu state
         * is read again (a folder that did not exist at start is not
         * watched). Back from one of its own dialogs, the action that opened
         * it refreshes what it changed. */
        if (LOWORD(wp) != WA_INACTIVE && g_manager.list && !StateChangesBlocked() && !OwnedBy((HWND)lp, dialog)) {
            g_manager.shortcutStateFolder[0] = 0;
            Refresh(FALSE);
            SessionsView_Reload();
            /* The dialog manager then gives the focus back to the control
             * that had it, which the refresh may just have hidden. */
            PostMessageW(dialog, WM_APP_RESTORE_FOCUS, 0, 0);
        }
        break;

    case WM_APP_RESTORE_FOCUS: {
        HWND button = GetDlgItem(dialog, IDC_STATUS_ACTION);
        if (GetFocus() == button && !IsWindowVisible(button)) FocusView(dialog, SessionsView_Shown());
        return TRUE;
    }

    case WM_NOTIFY: {
        const NMHDR *header = (const NMHDR *)lp;
        if (header->idFrom == IDC_ABOUT && (header->code == NM_CLICK || header->code == NM_RETURN)) {
            Util_OpenUrl(APP_AUTHOR_URL);   /* the only link there */
            return TRUE;
        }
        if (header->idFrom != IDC_LIST) {
            LRESULT result;
            if (!SessionsView_Notify(header, &result)) break;
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
            return TRUE;
        }
        if (header->code == NM_SETFOCUS || header->code == NM_KILLFOCUS) InvalidateRect(g_manager.list, NULL, FALSE);
        if (header->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            /* Selecting another row first deselects the old one: the buttons
             * follow once, when the list is done. */
            if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
                PostOnce(&g_manager.selectionPending, WM_APP_SELECTION);
        } else if (header->code == NM_DBLCLK) {
            if (((const NMITEMACTIVATE *)lp)->iItem >= 0) DoOpen();
        } else if (header->code == LVN_KEYDOWN) {
            if (((const NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) DoDelete();
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN:          DoOpen(); return TRUE;
        case IDC_NEW:           DoNew(); return TRUE;
        case IDC_EDIT:          DoEdit(); return TRUE;
        case IDC_DELETE:        DoDelete(); return TRUE;
        case IDC_DEFAULT:       DoSetDefault(); return TRUE;
        case IDC_SC_DESKTOP:    DoDesktopShortcut(); return TRUE;
        case IDC_SC_SAVEAS:     DoSaveShortcut(); return TRUE;
        case IDC_SC_PIN:        DoPinTaskbar(); return TRUE;
        case IDC_SC_START:      DoStartMenu(); return TRUE;
        case IDC_SESSIONS:      ToggleSessions(); return TRUE;
        case IDC_UNINSTALL:     DoUninstall(); return TRUE;
        case IDC_STATUS_ACTION: DoStatusAction(); return TRUE;
        case IDC_UPDATE:        DoUpdate(); return TRUE;
        case IDC_LANGUAGE:      DoLanguage(); return TRUE;
        case IDCANCEL:
            /* Esc in a search box that holds text clears it. */
            if (!SessionsView_ClearSearch()) Close(dialog);
            return TRUE;
        }
        if (SessionsView_Command(wp)) return TRUE;
        break;

    case WM_CLOSE:
        /* Also sent by an installer updating the program: close any dialog
         * that is open too, so the process really exits. */
        Close(dialog);
        return TRUE;

    case WM_APP_UNINSTALL:
        if (StateChangesBlocked()) return TRUE;
        if (!IsWindowEnabled(dialog)) {
            g_manager.pendingUninstall = TRUE;   /* a dialog is open: after it closes */
            return TRUE;
        }
        DoUninstall();
        if (wp && g_manager.uninstallOnly && !g_manager.uninstalled && !g_manager.closing) EndDialog(dialog, 0);
        return TRUE;

    case WM_APP_SELECTION:
        g_manager.selectionPending = FALSE;
        UpdateButtons();
        return TRUE;

    case WM_APP_SHORTCUTS: {
        PIDLIST_ABSOLUTE *pidls = NULL;
        LONG event = 0;
        HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &event);
        if (lock) SHChangeNotification_Unlock(lock);
        PostOnce(&g_manager.shortcutsCheckPending, WM_APP_SHORTCUTS_CHECK);
        return TRUE;
    }

    case WM_APP_SHORTCUTS_CHECK:
        g_manager.shortcutsCheckPending = FALSE;
        if (!StateChangesBlocked()) {
            g_manager.shortcutStateFolder[0] = 0;
            UpdateButtons();
        }
        return TRUE;

    case WM_APP_UPDATE:
        ShowUpdate();
        return TRUE;

    case WM_APP_DOWNLOADED:
        Downloaded((UpdateResult)wp, (DWORD)lp);
        return TRUE;

    case WM_APP_SETUPLINKS:
        if (StateChangesBlocked()) return TRUE;
        if (!IsWindowEnabled(dialog)) {
            g_manager.pendingSetUpLinks = TRUE;   /* a dialog is open: after it closes */
            return TRUE;
        }
        /* The question that follows must be seen, with the manager behind it. */
        if (!IsWindowVisible(dialog)) ShowWindow(dialog, SW_SHOWNORMAL);
        SetForegroundWindow(dialog);
        SetUpLinks();
        return TRUE;

    case WM_APP_SHOW:
        /* Opening the manager while a Settings uninstall is on screen: show
         * the manager once that dialog is cancelled. */
        g_manager.uninstallOnly = FALSE;
        return TRUE;

    case WM_ENABLE:
        if (wp && g_manager.pendingUninstall) {
            g_manager.pendingUninstall = FALSE;
            PostMessageW(dialog, WM_APP_UNINSTALL, 0, 0);
        }
        if (wp && g_manager.pendingSetUpLinks) {
            g_manager.pendingSetUpLinks = FALSE;
            PostMessageW(dialog, WM_APP_SETUPLINKS, 0, 0);
        }
        break;

    case WM_DRAWITEM:
        if (wp == IDC_SC_GROUP) {
            DrawGroupLabel((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        if (SessionsView_DrawItem((const DRAWITEMSTRUCT *)lp)) return TRUE;
        break;

    case WM_CONTEXTMENU:
        if (SessionsView_ContextMenu((HWND)wp, lp)) return TRUE;
        break;

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return Theme_CtlColor(message, wp, lp, IDC_ABOUT);

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        Theme_Follow(dialog, message, wp, lp);
        break;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) LayoutMainControls();
        return TRUE;

    case WM_GETMINMAXINFO:
        if (Gui_MainWindowGeometry(dialog, message, wp, lp)) return TRUE;
        break;

    case WM_DPICHANGED:
        Gui_MainWindowGeometry(dialog, message, wp, lp);
        PostMessageW(dialog, WM_APP_RELAYOUT, 0, 0);
        return TRUE;

    case WM_APP_RELAYOUT: {
        const Profile *p = SelectedProfile();
        WCHAR selected[FOLDER_CCH] = L"";
        if (p) StringCchCopyW(selected, ARRAYSIZE(selected), p->folder);
        SetIcons(dialog);
        ReflowMain();
        FillList(selected);
        return TRUE;
    }

    case WM_DESTROY:
        g_manager.layoutReady = FALSE;
        FinishShellWork();
        Localize_ForgetWindow(dialog);
        StopWatchingOutside();
        SessionsView_Destroy();
        if (g_manager.shortcutsNotify) SHChangeNotifyDeregister(g_manager.shortcutsNotify);
        g_manager.shortcutsNotify = 0;
        if (g_manager.bigIcon) DestroyIcon(g_manager.bigIcon);
        if (g_manager.smallIcon) DestroyIcon(g_manager.smallIcon);
        if (g_manager.groupBadge) DestroyIcon(g_manager.groupBadge);
        g_manager.bigIcon = g_manager.smallIcon = g_manager.groupBadge = NULL;
        break;
    }
    return FALSE;
}

int Gui_Run(GuiStart start)
{
    INITCOMMONCONTROLSEX controls;
    WNDCLASSEXW windowClass;
    HANDLE mutex;
    HWND existing;
    WCHAR exe[MAX_PATH];
    INT_PTR dialogResult;

    if (!Install_IsInstalledCopy()) {
        if (start != GUI_UNINSTALL) return Install_Run(TRUE) ? 0 : 1;
        if (Util_InstallExe(exe, ARRAYSIZE(exe)) && Util_FileExists(exe)) {
            DWORD error;
            if ((INT_PTR)ShellExecuteW(NULL, L"open", exe, L"--uninstall", NULL, SW_SHOWNORMAL) > 32) return 0;
            error = GetLastError();
            Util_Log(L"could not start %s to uninstall (error %lu)", exe, error);
            Ui_Message(NULL, MB_ICONERROR,
                       TR(APP_NAME L" was installed but could not be started (error %lu):\n%s\n\nSecurity software may be blocking it."),
                       error, exe);
            return 1;
        }
        Ui_Message(NULL, MB_ICONINFORMATION, TR(APP_NAME L" is not installed."));
        return 1;
    }

    g_manager.readyEvent = CreateEventW(NULL, TRUE, FALSE, MANAGER_READY_EVENT);
    mutex = CreateMutexW(NULL, FALSE, MANAGER_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_manager.readyEvent) WaitForSingleObject(g_manager.readyEvent, MANAGER_READY_TIMEOUT_MS);
        existing = FindWindowW(APP_WINDOW_CLASS, NULL);
        if (existing) {
            /* A dialog open in the manager disables it: bring that forward. */
            HWND top;
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            top = GetLastActivePopup(existing);
            if (!top || !IsWindowVisible(top) || !IsWindowEnabled(top)) top = existing;
            SetForegroundWindow(top);
            PostMessageW(existing, start == GUI_UNINSTALL ? WM_APP_UNINSTALL : WM_APP_SHOW, 0, 0);
            if (start == GUI_SET_UP_LINKS) PostMessageW(existing, WM_APP_SETUPLINKS, 0, 0);
        }
        CloseHandle(mutex);
        if (g_manager.readyEvent) CloseHandle(g_manager.readyEvent);
        g_manager.readyEvent = NULL;
        return 0;
    }

    Install_Repair();
    SetCurrentProcessExplicitAppUserModelID(APP_MANAGER_AUMID);

    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&controls);

    ZeroMemory(&windowClass, sizeof windowClass);
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = DefDlgProcW;
    windowClass.cbWndExtra = DLGWINDOWEXTRA;
    windowClass.hInstance = g_hInst;
    windowClass.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    windowClass.lpszClassName = APP_WINDOW_CLASS;
    if (!RegisterClassExW(&windowClass)) Util_Log(L"could not register the manager's window class (error %lu)", GetLastError());

    dialogResult = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainProc, (LPARAM)start);
    if (dialogResult == -1) Util_Log(L"could not create the manager's window (error %lu)", GetLastError());
    if (mutex) CloseHandle(mutex);
    if (g_manager.readyEvent) CloseHandle(g_manager.readyEvent);
    g_manager.readyEvent = NULL;
    if (g_manager.uninstalled) Install_FinishUninstall();
    return dialogResult == -1 ? 1 : 0;
}
