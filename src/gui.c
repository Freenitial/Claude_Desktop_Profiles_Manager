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
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#define WM_APP_UNINSTALL (WM_APP + 1)
#define WM_APP_RELAYOUT  (WM_APP + 2)
#define WM_APP_FIXFOCUS  (WM_APP + 3)
#define WM_APP_DPI       (WM_APP + 4)   /* a dialog moved to a monitor with another scale */
#define WM_APP_SHOW      (WM_APP + 5)   /* a second launch asked for the manager itself */
#define WM_APP_SETUPLINKS (WM_APP + 6)  /* just installed: offer to set up claude:// links */
#define WM_APP_UPDATE     (WM_APP + 7)  /* the latest release is known */
#define WM_APP_DOWNLOADED (WM_APP + 8)  /* wParam: how the download went (UpdateResult) */
#define WM_APP_SELECTION  (WM_APP + 9)  /* the list selection changed */
#define WM_APP_SHORTCUTS  (WM_APP + 10) /* the desktop, Start menu or taskbar pins folder changed */
#define WM_APP_OUTSIDE    (WM_APP + 11) /* something the window shows changed outside it (g_outside) */

/* What changed outside the window, told by the thread that waits for it. */
enum { CHANGE_PROFILES = 1, CHANGE_LINKS = 2, CHANGE_PACKAGES = 4 };

typedef enum FixAction { FIX_NONE, FIX_GET_CLAUDE, FIX_LINKS } FixAction;

typedef struct MainState {
    HWND        dlg;
    HWND        list, listArea;   /* the profiles list, and the view it scrolls in */
    ProfileList profiles;
    ClaudePackage pkg;
    FixAction   fix;
    WCHAR       checkedFolder[FOLDER_CCH]; /* profile whose shortcut and pin state is cached */
    BOOL        onDesktop;
    BOOL        pinned;
    BOOL        inStartMenu;
    ULONG       shortcutsNotify;  /* SHChangeNotifyRegister on the folders those buttons read */
    int         groupColor;       /* badge shown in the "Shortcuts for" label */
    HANDLE      outsideThread;    /* waits for outside changes (WatchOutside) */
    HANDLE      outsideStop;
    HICON       bigIcon;
    HICON       smallIcon;
    BOOL        uninstalled;      /* nothing may touch the registry any more */
    BOOL        busy;             /* uninstall in progress: no refresh */
    BOOL        closing;          /* asked to close (installer update) */
    BOOL        pendingUninstall; /* uninstall asked while a dialog was open */
    BOOL        uninstallOnly;    /* started from Settings to uninstall: no manager window */
    BOOL        updating;         /* the newer release is downloading or installing */
    BOOL        selectionPending; /* WM_APP_SELECTION posted */
} MainState;

static MainState g;
static volatile LONG g_outside;   /* CHANGE_* not handled yet: set by the thread, taken by the window */

static BOOL Same(const WCHAR *a, const WCHAR *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

/* While uninstalling or closing, nothing may reload state or write the
 * registry. */
static BOOL Quiet(void)
{
    return g.uninstalled || g.busy || g.closing;
}

static const Profile *Selected(void)
{
    int i = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    return (i >= 0 && i < g.profiles.count) ? &g.profiles.items[i] : NULL;
}

static const WCHAR *StockName(void)
{
    int i = Profiles_Find(&g.profiles, STOCK_FOLDER);
    return i >= 0 ? g.profiles.items[i].name : STOCK_DEFAULT_NAME;
}

static void Enable(int id, BOOL on)
{
    HWND w = GetDlgItem(g.dlg, id);
    if (!on && GetFocus() == w) SendMessageW(g.dlg, WM_NEXTDLGCTL, 0, FALSE);
    EnableWindow(w, on);
}

static void SetTextIfChanged(int id, const WCHAR *text)
{
    WCHAR now[512];
    GetDlgItemTextW(g.dlg, id, now, ARRAYSIZE(now));
    if (wcscmp(now, text) != 0) SetDlgItemTextW(g.dlg, id, text);
}

/* ------------------------------------------------------------------ list */

static void RoleText(const Profile *p, WCHAR *out, size_t cch)
{
    BOOL def = Same(p->folder, g.profiles.defaultFolder);
    if (p->isStock && def)
        StringCchCopyW(out, cch, L"Claude icon, default");
    else if (p->isStock)
        StringCchCopyW(out, cch, L"Claude icon");
    else if (def)
        StringCchCopyW(out, cch, L"Default");
    else
        out[0] = 0;
}

static void UpdateRow(int i)
{
    const Profile *p = &g.profiles.items[i];
    WCHAR text[MAX_PATH];
    ListView_SetItemText(g.list, i, 0, (LPWSTR)p->name);
    RoleText(p, text, ARRAYSIZE(text));
    ListView_SetItemText(g.list, i, 1, text);
    StringCchPrintfW(text, ARRAYSIZE(text), L"%%APPDATA%%\\%s", p->folder);
    ListView_SetItemText(g.list, i, 2, text);
}

/* The size of a taskbar button's icon, so the initial shows in the badge. */
static int IconPixels(void)
{
    return MulDiv(24, (int)GetDpiForWindow(g.dlg), 96);
}

static void RebuildImages(void)
{
    int px = IconPixels(), i;
    HIMAGELIST il = ImageList_Create(px, px, ILC_COLOR32, g.profiles.count, 1), old;
    if (!il) return;
    for (i = 0; i < g.profiles.count; i++) {
        HICON icon = Icons_Create(&g.pkg, &g.profiles.items[i], px);
        if (!icon) icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, px, px, 0);
        ImageList_AddIcon(il, icon);
        if (icon) DestroyIcon(icon);
    }
    old = ListView_SetImageList(g.list, il, LVSIL_SMALL);
    if (old) ImageList_Destroy(old);
}

static void LayoutColumns(void)
{
    RECT rc;
    int w;
    GetClientRect(g.list, &rc);
    w = rc.right - rc.left;
    ListView_SetColumnWidth(g.list, 0, w * 27 / 100);
    ListView_SetColumnWidth(g.list, 1, w * 23 / 100);
    ListView_SetColumnWidth(g.list, 2, w - (w * 27 / 100) - (w * 23 / 100));
}

static void UpdateButtons(void);

static void FillList(const WCHAR *select)
{
    LVITEMW it;
    int i, sel = -1;
    SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.list);
    RebuildImages();
    for (i = 0; i < g.profiles.count; i++) {
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = i;
        it.iImage = i;
        it.pszText = g.profiles.items[i].name;
        ListView_InsertItem(g.list, &it);
        UpdateRow(i);
        if (select && Same(g.profiles.items[i].folder, select)) sel = i;
    }
    if (sel < 0 && g.profiles.count > 0) sel = 0;
    if (sel >= 0) {
        ListView_SetItemState(g.list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, sel, FALSE);
    }
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    LayoutColumns();
    InvalidateRect(g.list, NULL, TRUE);
    g.checkedFolder[0] = 0;
    UpdateButtons();
}

static BOOL SameProfiles(const ProfileList *a, const ProfileList *b)
{
    int i;
    if (a->count != b->count) return FALSE;
    for (i = 0; i < a->count; i++) {
        if (!Same(a->items[i].folder, b->items[i].folder) || wcscmp(a->items[i].name, b->items[i].name) != 0 ||
            a->items[i].color != b->items[i].color)
            return FALSE;
    }
    return TRUE;
}

/* --------------------------------------------------------------- status */

/* "2.2553.13.0" -> "2.2553.13", as Claude shows it. */
static void ShortVersion(WCHAR *out, size_t cch)
{
    size_t n;
    StringCchCopyW(out, cch, g.pkg.version);
    n = wcslen(out);
    if (n > 2 && out[n - 2] == L'.' && out[n - 1] == L'0') out[n - 2] = 0;
}

static void UpdateStatus(void)
{
    WCHAR text[256], version[32];
    FixAction fix = FIX_NONE;

    ShortVersion(version, ARRAYSIZE(version));

    if (!g.pkg.found) {
        StringCchCopyW(text, ARRAYSIZE(text), L"Claude Desktop is not installed.");
        fix = FIX_GET_CLAUDE;
    } else if (Handler_UserChoice() != USERCHOICE_OURS) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"Claude Desktop %s \x00B7 claude:// links are not set up yet", version);
        fix = FIX_LINKS;
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text), L"Claude Desktop %s \x00B7 sign-in links open in the right window", version);
    }
    SetTextIfChanged(IDC_STATUS, text);
    if (fix != g.fix) {
        HWND button = GetDlgItem(g.dlg, IDC_FIX);
        if (fix == FIX_NONE && GetFocus() == button) {
            /* Never hide the focused control: focus goes back to the list. */
            SendMessageW(g.dlg, WM_NEXTDLGCTL, (WPARAM)g.list, TRUE);
            SendMessageW(g.dlg, DM_SETDEFID, IDC_OPEN, 0);
        }
        g.fix = fix;
        SetDlgItemTextW(g.dlg, IDC_FIX, fix == FIX_GET_CLAUDE ? L"Get Claude" : L"Set up links");
        ShowWindow(GetDlgItem(g.dlg, IDC_FIX), fix != FIX_NONE ? SW_SHOW : SW_HIDE);
        /* The status line, on the right, ends before that button while it shows. */
        {
            RECT r = { 92, 10, fix != FIX_NONE ? 328 : 410, 20 };
            MapDialogRect(g.dlg, &r);
            SetWindowPos(GetDlgItem(g.dlg, IDC_STATUS), NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

/* Text of a static that names a profile: a long name is cut (with an
 * ellipsis) until the whole text fits `box` (client coordinates). `fmt` has
 * one %s. Returns the height the text takes. */
static int FitNameIn(HWND control, const RECT *box, const WCHAR *fmt, const WCHAR *name, WCHAR *text, size_t cch)
{
    WCHAR shown[LABEL_CCH + 2];
    RECT need;
    HDC dc;
    HFONT old;
    size_t len = wcslen(name), keep;

    dc = GetDC(control);
    old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    for (keep = len; ; keep--) {
        StringCchCopyNW(shown, ARRAYSIZE(shown), name, keep);
        if (keep < len) StringCchCatW(shown, ARRAYSIZE(shown), L"\x2026");
        StringCchPrintfW(text, cch, fmt, shown);
        need = *box;
        DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        if (keep <= 1 || (need.bottom - need.top <= box->bottom - box->top && need.right <= box->right)) break;
    }
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    return need.bottom - need.top;
}

static void FitName(HWND control, const WCHAR *fmt, const WCHAR *name, WCHAR *text, size_t cch)
{
    RECT box;
    GetClientRect(control, &box);
    FitNameIn(control, &box, fmt, name, text, cch);
}

/* The note beside the buttons names the stock profile. It ends where the
 * profiles list ends, "Set as default" just above it. */
static void UpdateNote(void)
{
    WCHAR text[256];
    HWND note = GetDlgItem(g.dlg, IDC_NOTE), button = GetDlgItem(g.dlg, IDC_DEFAULT);
    RECT area = { 334, 0, 412, 0 }, list, above, box, gap = { 0, 0, 0, 7 }, reserve = { 0, 0, 0, 10 + 15 + 7 };
    int height;
    MapDialogRect(g.dlg, &area);
    MapDialogRect(g.dlg, &gap);
    MapDialogRect(g.dlg, &reserve);
    GetWindowRect(g.listArea, &list);
    MapWindowPoints(NULL, g.dlg, (POINT *)&list, 2);
    /* At most up to "Set as default" 10 units under Delete. */
    GetWindowRect(GetDlgItem(g.dlg, IDC_DELETE), &above);
    MapWindowPoints(NULL, g.dlg, (POINT *)&above, 2);
    area.top = above.bottom + reserve.bottom;
    area.bottom = list.bottom;
    box.left = box.top = 0;
    box.right = area.right - area.left;
    box.bottom = area.bottom - area.top;
    height = FitNameIn(note, &box, L"The default profile opens claude:// links while Claude is closed.\n\nThe regular Claude icon opens \x201C%s\x201D.",
                       StockName(), text, ARRAYSIZE(text));
    height = min(height, box.bottom);
    SetWindowPos(note, NULL, area.left, area.bottom - height, box.right, height, SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(button, &box);
    MapWindowPoints(NULL, g.dlg, (POINT *)&box, 2);
    SetWindowPos(button, NULL, box.left, area.bottom - height - gap.bottom - (box.bottom - box.top), 0, 0,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    SetTextIfChanged(IDC_NOTE, text);
}

static void SetKeepText(HWND d, const ProfileList *list)
{
    WCHAR text[256];
    int i = Profiles_Find(list, STOCK_FOLDER);
    FitName(GetDlgItem(d, IDC_U_KEEP),
            L"Claude Desktop and its \x201C%s\x201D profile (the one the regular Claude icon opens) are not touched.",
            i >= 0 ? list->items[i].name : STOCK_DEFAULT_NAME, text, ARRAYSIZE(text));
    SetDlgItemTextW(d, IDC_U_KEEP, text);
}

static void UpdateButtons(void)
{
    const Profile *p = Selected();
    WCHAR text[256];
    BOOL isDefault = p && Same(p->folder, g.profiles.defaultFolder);

    if (p && !Same(p->folder, g.checkedFolder)) {
        StringCchCopyW(g.checkedFolder, ARRAYSIZE(g.checkedFolder), p->folder);
        g.onDesktop = Shortcut_FindOnDesktop(p, NULL, 0);
        g.pinned = TaskbarPin_IsPinned(p);
        g.inStartMenu = Shortcut_IsInStartMenu(p);
    }
    Enable(IDC_OPEN, p && g.pkg.found);
    Enable(IDC_NEW, g.profiles.count < MAX_PROFILES);
    Enable(IDC_EDIT, p != NULL);
    Enable(IDC_DELETE, p && !p->isStock);
    Enable(IDC_DEFAULT, p && !isDefault);
    Enable(IDC_SC_DESKTOP, p && !g.onDesktop);
    Enable(IDC_SC_SAVEAS, p != NULL);
    Enable(IDC_SC_PIN, p && !g.pinned);
    Enable(IDC_SC_START, p != NULL);
    /* A button off because it is done says so. */
    SetTextIfChanged(IDC_SC_DESKTOP, p && g.onDesktop ? L"Shortcut on desktop" : L"Create shortcut on desktop");
    SetTextIfChanged(IDC_SC_PIN, p && g.pinned ? L"Pinned" : L"Pin to taskbar");
    SetTextIfChanged(IDC_SC_START, p && g.inStartMenu ? L"Remove from Start menu" : L"Add to Start menu");

    if (p)
        StringCchPrintfW(text, ARRAYSIZE(text), L"Shortcuts for \x201C%s\x201D", p->name);
    else
        StringCchCopyW(text, ARRAYSIZE(text), L"Shortcuts");
    SetTextIfChanged(IDC_SC_GROUP, text);
    if (p && p->color != g.groupColor) {
        g.groupColor = p->color;
        InvalidateRect(GetDlgItem(g.dlg, IDC_SC_GROUP), NULL, TRUE);
    }
    UpdateNote();
}

/* Shortcuts for "(badge) Name": owner-drawn to show the profile's
 * badge before its name. The window text stays the plain sentence (screen
 * readers read it). */
static void DrawGroupLabel(const DRAWITEMSTRUCT *di)
{
    static const WCHAR lead[] = L"Shortcuts for \x201C", close[] = L"\x201D";
    const Profile *p = Selected();
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    HFONT old;
    HICON badge;
    TEXTMETRICW tm;
    SIZE ext;
    WCHAR shown[LABEL_CCH + 2];
    size_t len, keep;
    int x, top, cap, dot, gap, closeWidth;

    SetTextColor(dc, Theme_Color(THEME_TEXT));
    FillRect(dc, &rc, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &tm);
    top = rc.top + (rc.bottom - rc.top - tm.tmHeight) / 2;
    if (!p) {
        TextOutW(dc, rc.left, top, L"Shortcuts", 9);
        SelectObject(dc, old);
        return;
    }

    x = rc.left;
    GetTextExtentPoint32W(dc, lead, (int)wcslen(lead), &ext);
    TextOutW(dc, x, top, lead, (int)wcslen(lead));
    x += ext.cx;

    /* The badge, as tall as the capitals and centred on them. */
    cap = tm.tmAscent - tm.tmInternalLeading;
    dot = cap + 2;
    GetTextExtentPoint32W(dc, L" ", 1, &ext);
    gap = ext.cx;
    badge = Icons_CreateBadge(p->color, dot);
    if (badge) {
        DrawIconEx(dc, x + 1, top + tm.tmAscent - (cap + dot + 1) / 2, badge, dot, dot, 0, NULL, DI_NORMAL);
        DestroyIcon(badge);
        x += 1 + dot + gap;
    }

    /* A long name is cut, with an ellipsis, so the closing quote still shows. */
    GetTextExtentPoint32W(dc, close, 1, &ext);
    closeWidth = ext.cx;
    len = wcslen(p->name);
    for (keep = len; ; keep--) {
        StringCchCopyNW(shown, ARRAYSIZE(shown), p->name, keep);
        if (keep < len) StringCchCatW(shown, ARRAYSIZE(shown), L"\x2026");
        GetTextExtentPoint32W(dc, shown, (int)wcslen(shown), &ext);
        if (keep <= 1 || x + ext.cx + closeWidth <= rc.right) break;
    }
    TextOutW(dc, x, top, shown, (int)wcslen(shown));
    TextOutW(dc, x + ext.cx, top, close, 1);
    SelectObject(dc, old);
}

/* A profile's shortcuts, taskbar pins and open windows get its current icon,
 * name and description. */
static void ApplyBadge(const Profile *before, const Profile *after, const WCHAR *icon)
{
    BOOL links = Shortcut_Refresh(before, after, icon);
    TaskbarPin_Refresh(before, after, icon);
    Taskbar_Refresh(after, links);   /* its open windows, if it runs */
    Icons_DeleteStale(after, icon);
}

/* Shortcuts, pins and open windows showing an icon made while Claude was
 * missing, or by an older drawing of the badge, get the current icon. */
static void HealIcons(const ProfileList *list)
{
    WCHAR icon[MAX_PATH];
    int i;
    for (i = 0; i < list->count; i++) {
        const Profile *p = &list->items[i];
        if (Icons_IsStale(&g.pkg, p) && Icons_Ensure(&g.pkg, p, icon, ARRAYSIZE(icon))) ApplyBadge(p, p, icon);
    }
}

static void Refresh(BOOL rescanPackage)
{
    ProfileList fresh;
    WCHAR sel[FOLDER_CCH] = L"";
    const Profile *p = Selected();
    BOOL hadClaude = g.pkg.found;
    int i;

    if (p) StringCchCopyW(sel, ARRAYSIZE(sel), p->folder);
    if (rescanPackage) Claude_FindPackage(&g.pkg);
    Profiles_Load(&fresh);
    if (rescanPackage) HealIcons(&fresh);
    if (!SameProfiles(&fresh, &g.profiles) || hadClaude != g.pkg.found) {
        g.profiles = fresh;
        FillList(sel);
    } else {
        g.profiles = fresh;
        for (i = 0; i < g.profiles.count; i++) UpdateRow(i);
        UpdateButtons();
    }
    UpdateStatus();
}

/* --------------------------------------------------------- profile dialog */

typedef struct ProfileDialog {
    BOOL           isNew;
    const Profile *existing;
    const Profile *copyFrom;   /* new profile: whose settings it can start with */
    WCHAR          name[LABEL_CCH];
    int            color;
    BOOL           openNow;
    BOOL           startup;
    BOOL           copy;
    HICON          preview;
} ProfileDialog;

/* The edit dialog has no rows for opening now and copying settings: the
 * buttons move up and the dialog shrinks by those rows. */
static void DropProfileRows(HWND d)
{
    const int ids[] = { IDOK, IDCANCEL };
    RECT rows = { 0, 0, 0, 26 }, r;
    size_t i;
    MapDialogRect(d, &rows);
    for (i = 0; i < ARRAYSIZE(ids); i++) {
        HWND c = GetDlgItem(d, ids[i]);
        GetWindowRect(c, &r);
        MapWindowPoints(NULL, d, (POINT *)&r, 2);
        SetWindowPos(c, NULL, r.left, r.top - rows.bottom, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    GetWindowRect(d, &r);
    SetWindowPos(d, NULL, 0, 0, r.right - r.left, r.bottom - r.top - rows.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL ValidateProfileDialog(HWND d, ProfileDialog *s, BOOL showError)
{
    WCHAR raw[128], clean[LABEL_CCH], folder[FOLDER_CCH], info[MAX_PATH + 96], appData[MAX_PATH], dir[MAX_PATH];
    const WCHAR *err = NULL;
    BOOL ok;

    GetDlgItemTextW(d, IDC_P_NAME, raw, ARRAYSIZE(raw));
    if (s->isNew) {
        ok = Core_ValidateNewName(raw, clean, ARRAYSIZE(clean), folder, ARRAYSIZE(folder), &err);
        if (ok && Util_AppData(appData, ARRAYSIZE(appData)) &&
            SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, folder)) &&
            GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
            ok = FALSE;
            err = L"A profile folder with this name already exists.";
        }
        StringCchPrintfW(info, ARRAYSIZE(info), L"Its data will be kept in %%APPDATA%%\\%s.", ok ? folder : PROFILE_PREFIX L"<name>");
    } else {
        ok = Core_ValidateLabel(raw, clean, ARRAYSIZE(clean), &err);
        StringCchPrintfW(info, ARRAYSIZE(info), L"Data folder: %%APPDATA%%\\%s%s", s->existing->folder,
                         s->existing->isStock ? L"\nThe regular Claude icon opens this profile." : L"");
    }
    /* The error takes the place of the folder line while there is one. */
    SetDlgItemTextW(d, IDC_P_FOLDER, info);
    SetDlgItemTextW(d, IDC_P_ERROR, (!ok && showError && err) ? err : L"");
    ShowWindow(GetDlgItem(d, IDC_P_FOLDER), (!ok && showError && err) ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(d, IDC_P_ERROR), (!ok && showError && err) ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(d, IDOK), ok);
    if (ok) StringCchCopyW(s->name, ARRAYSIZE(s->name), clean);
    return ok;
}

static void UpdatePreview(HWND d, ProfileDialog *s)
{
    Profile tmp;
    HICON icon;
    int sel = (int)SendDlgItemMessageW(d, IDC_P_COLOR, CB_GETCURSEL, 0, 0);
    ZeroMemory(&tmp, sizeof tmp);
    GetDlgItemTextW(d, IDC_P_NAME, tmp.name, ARRAYSIZE(tmp.name));
    tmp.color = sel >= 0 && sel < PALETTE_SIZE ? sel : 0;
    icon = Icons_Create(&g.pkg, &tmp, MulDiv(32, (int)GetDpiForWindow(d), 96));
    SendDlgItemMessageW(d, IDC_P_PREVIEW, STM_SETICON, (WPARAM)icon, 0);
    if (s->preview) DestroyIcon(s->preview);
    s->preview = icon;
}

static INT_PTR CALLBACK ProfileProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    ProfileDialog *s = (ProfileDialog *)GetWindowLongPtrW(d, DWLP_USER);
    int i;

    switch (msg) {
    case WM_INITDIALOG:
        s = (ProfileDialog *)lp;
        SetWindowLongPtrW(d, DWLP_USER, lp);
        SetWindowTextW(d, s->isNew ? L"New profile" : L"Edit profile");
        SendDlgItemMessageW(d, IDC_P_NAME, EM_LIMITTEXT, MAX_LABEL, 0);
        for (i = 0; i < PALETTE_SIZE; i++) SendDlgItemMessageW(d, IDC_P_COLOR, CB_ADDSTRING, 0, (LPARAM)g_ColorNames[i]);
        SendDlgItemMessageW(d, IDC_P_COLOR, CB_SETCURSEL, (WPARAM)(s->color >= 0 && s->color < PALETTE_SIZE ? s->color : 0), 0);
        if (!s->isNew) SetDlgItemTextW(d, IDC_P_NAME, s->existing->name);
        CheckDlgButton(d, IDC_P_OPEN, s->openNow ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(GetDlgItem(d, IDC_P_OPEN), s->isNew && g.pkg.found ? SW_SHOW : SW_HIDE);
        CheckDlgButton(d, IDC_P_STARTUP, s->startup ? BST_CHECKED : BST_UNCHECKED);
        if (s->isNew && s->copyFrom) {
            WCHAR label[LABEL_CCH + 64];
            FitName(GetDlgItem(d, IDC_P_COPY), L"Copy &settings from \x201C%s\x201D", s->copyFrom->name, label, ARRAYSIZE(label));
            SetDlgItemTextW(d, IDC_P_COPY, label);
        } else {
            ShowWindow(GetDlgItem(d, IDC_P_COPY), SW_HIDE);
        }
        if (!s->isNew) DropProfileRows(d);
        ValidateProfileDialog(d, s, FALSE);
        UpdatePreview(d, s);
        SendDlgItemMessageW(d, IDC_P_NAME, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(d, IDC_P_NAME));
        return FALSE;

    case WM_COMMAND:
        if (!s) break;
        switch (LOWORD(wp)) {
        case IDC_P_NAME:
            if (HIWORD(wp) == EN_CHANGE) {
                ValidateProfileDialog(d, s, TRUE);
                UpdatePreview(d, s);
            }
            return TRUE;
        case IDC_P_COLOR:
            if (HIWORD(wp) == CBN_SELCHANGE) UpdatePreview(d, s);
            return TRUE;
        case IDOK:
            if (!ValidateProfileDialog(d, s, TRUE)) return TRUE;
            i = (int)SendDlgItemMessageW(d, IDC_P_COLOR, CB_GETCURSEL, 0, 0);
            s->color = i >= 0 && i < PALETTE_SIZE ? i : 0;
            s->openNow = IsDlgButtonChecked(d, IDC_P_OPEN) == BST_CHECKED;
            s->startup = IsDlgButtonChecked(d, IDC_P_STARTUP) == BST_CHECKED;
            s->copy = s->isNew && s->copyFrom && IsDlgButtonChecked(d, IDC_P_COPY) == BST_CHECKED;
            EndDialog(d, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(msg, wp, lp, IDC_P_FOLDER);

    case WM_DPICHANGED:
        PostMessageW(d, WM_APP_DPI, 0, 0);
        break;

    case WM_APP_DPI:
        if (s) UpdatePreview(d, s);
        return TRUE;

    case WM_DESTROY:
        if (s && s->preview) {
            DestroyIcon(s->preview);
            s->preview = NULL;
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
    int  map[MAX_PROFILES];
    int  count;
} UninstallDialog;

/* As wide as the list: a longer row shows whole in its tooltip. */
static void FitUninstallColumn(HWND list)
{
    RECT rc;
    GetClientRect(list, &rc);
    ListView_SetColumnWidth(list, 0, rc.right - rc.left);
}

static INT_PTR CALLBACK UninstallProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    UninstallDialog *u = (UninstallDialog *)GetWindowLongPtrW(d, DWLP_USER);
    HWND list;
    WCHAR text[MAX_PATH + 64];
    LVCOLUMNW col;
    LVITEMW it;
    RECT rc;
    int i;

    switch (msg) {
    case WM_INITDIALOG:
        u = (UninstallDialog *)lp;
        SetWindowLongPtrW(d, DWLP_USER, lp);
        SetKeepText(d, u->list);
        list = u->rows = GetDlgItem(d, IDC_U_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        GetClientRect(list, &rc);
        ZeroMemory(&col, sizeof col);
        col.mask = LVCF_WIDTH;
        col.cx = rc.right - rc.left;
        ListView_InsertColumn(list, 0, &col);   /* resized below, once the items are in */
        u->count = 0;
        for (i = 0; i < u->list->count; i++) {
            WCHAR target[MAX_PATH];
            if (u->list->items[i].isStock) continue;
            if (Profiles_LinkTarget(&u->list->items[i], target, ARRAYSIZE(target)))
                StringCchPrintfW(text, ARRAYSIZE(text), L"%s   (linked folder, kept: %s)", u->list->items[i].name, target);
            else
                StringCchPrintfW(text, ARRAYSIZE(text), L"%s   (%%APPDATA%%\\%s)", u->list->items[i].name, u->list->items[i].folder);
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_TEXT;
            it.iItem = u->count;
            it.pszText = text;
            ListView_InsertItem(list, &it);
            ListView_SetCheckState(list, u->count, TRUE);
            u->map[u->count++] = i;
        }
        FitUninstallColumn(list);
        list = Theme_SmoothView(list);   /* from here, the view that has its place */
        if (u->count == 0) {
            ShowWindow(GetDlgItem(d, IDC_U_LABEL), SW_HIDE);
            ShowWindow(list, SW_HIDE);
            ShowWindow(GetDlgItem(d, IDC_U_HINT), SW_HIDE);
        }
        return TRUE;

    case WM_DPICHANGED:
        PostMessageW(d, WM_APP_DPI, 0, 0);
        break;

    case WM_APP_DPI:
        if (u) {
            FitUninstallColumn(u->rows);
            SetKeepText(d, u->list);
        }
        return TRUE;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(msg, wp, lp, IDC_U_HINT);

    case WM_COMMAND:
        if (!u) break;
        if (LOWORD(wp) == IDOK) {
            WCHAR ask[512] = L"";
            int drop = 0, links = 0;
            list = u->rows;
            ZeroMemory(u->removeData, sizeof u->removeData);
            for (i = 0; i < u->count; i++) {
                const Profile *p = &u->list->items[u->map[i]];
                u->removeData[u->map[i]] = !ListView_GetCheckState(list, i);
                if (!u->removeData[u->map[i]]) continue;
                if (Profiles_IsLinked(p)) links++;
                else drop++;
            }
            if (drop > 0)
                StringCchPrintfW(ask, ARRAYSIZE(ask),
                                 L"The data of %d profile(s) will be moved to the Recycle Bin: their sign-in, local history, Claude Code and Cowork files.\n\n",
                                 drop);
            if (links > 0) {
                WCHAR more[256];
                StringCchPrintfW(more, ARRAYSIZE(more),
                                 L"%d linked profile(s) will be removed from " APP_NAME L"; the folders they link to stay on disk.\n\n", links);
                StringCchCatW(ask, ARRAYSIZE(ask), more);
            }
            if (drop + links > 0) {
                StringCchCatW(ask, ARRAYSIZE(ask), L"Continue?");
                if (Util_Message(d, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2, L"%s", ask) != IDOK) return TRUE;
            }
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(d, IDCANCEL);
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
    if (!g.pkg.found) {
        Util_Message(g.dlg, MB_ICONWARNING, L"Claude Desktop is not installed.");
        return;
    }
    hr = Launcher_Open(&g.pkg, &p, NULL, NULL, &identity);
    if (FAILED(hr)) {
        Util_Message(g.dlg, MB_ICONERROR, L"Claude could not be started (error 0x%08lX).", (unsigned long)hr);
        return;
    }
    Util_Log(L"opened %s from the manager%s", p.folder, identity ? L"" : L" without package identity");
}

static void DoOpen(void)
{
    const Profile *sel = Selected();
    if (sel) OpenProfile(sel);
}

static int FreeColor(void)
{
    BOOL used[PALETTE_SIZE] = { 0 };
    int i;
    for (i = 0; i < g.profiles.count; i++)
        if (g.profiles.items[i].color >= 0 && g.profiles.items[i].color < PALETTE_SIZE) used[g.profiles.items[i].color] = TRUE;
    for (i = 0; i < PALETTE_SIZE; i++)
        if (!used[i]) return i;
    return g.profiles.count % PALETTE_SIZE;
}

static void DoNew(void)
{
    ProfileDialog d;
    Profile source;
    WCHAR folder[FOLDER_CCH], err[256];
    int i;
    if (g.profiles.count >= MAX_PROFILES) {
        Util_Message(g.dlg, MB_ICONINFORMATION, APP_NAME L" handles up to %d profiles.", MAX_PROFILES);
        return;
    }
    ZeroMemory(&d, sizeof d);
    d.isNew = TRUE;
    d.color = FreeColor();
    d.openNow = g.pkg.found;
    /* Settings come from the selected profile, else the default one. */
    if (Selected()) source = *Selected();
    else if (g.profiles.count > 0) source = g.profiles.items[Profiles_DefaultIndex(&g.profiles)];
    else ZeroMemory(&source, sizeof source);
    d.copyFrom = source.folder[0] ? &source : NULL;
    if (Ui_Dialog(g.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&d) != IDOK || Quiet()) return;
    if (!Profiles_Create(d.name, d.color, folder, ARRAYSIZE(folder), err, ARRAYSIZE(err))) {
        Util_Message(g.dlg, MB_ICONWARNING, L"%s", err);
        return;
    }
    Profiles_Load(&g.profiles);
    FillList(folder);
    i = Profiles_Find(&g.profiles, folder);
    if (i < 0) {
        Util_Message(g.dlg, MB_ICONWARNING, L"The profile was created but cannot be listed: " APP_NAME L" handles up to %d profiles.", MAX_PROFILES);
        return;
    }
    if (d.copy) Profiles_CopySettings(&source, &g.profiles.items[i]);
    if (d.startup) Shortcut_SetStartup(&g.pkg, &g.profiles.items[i], TRUE);
    if (d.openNow) OpenProfile(&g.profiles.items[i]);
}

static void DoEdit(void)
{
    const Profile *sel = Selected();
    Profile before, after;
    ProfileDialog d;
    WCHAR icon[MAX_PATH];
    BOOL atStartup;
    if (!sel) return;
    before = *sel;
    ZeroMemory(&d, sizeof d);
    d.existing = &before;
    d.color = before.color;
    d.startup = atStartup = Shortcut_IsAtStartup(&before);
    if (Ui_Dialog(g.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&d) != IDOK || Quiet()) return;
    if (!Profiles_Update(before.folder, d.name, d.color)) {
        Util_Message(g.dlg, MB_ICONERROR, L"The profile could not be saved.");
        return;
    }
    after = before;
    StringCchCopyW(after.name, ARRAYSIZE(after.name), d.name);
    after.color = d.color;
    if ((wcscmp(before.name, after.name) != 0 || before.color != after.color) &&
        Icons_Ensure(&g.pkg, &after, icon, ARRAYSIZE(icon)))
        ApplyBadge(&before, &after, icon);
    if (d.startup != atStartup && FAILED(Shortcut_SetStartup(&g.pkg, &after, d.startup)))
        Util_Message(g.dlg, MB_ICONWARNING, L"The Windows sign-in setting of \x201C%s\x201D could not be changed.", after.name);
    Refresh(FALSE);
}

static BOOL ConfirmDelete(const Profile *p)
{
    WCHAR text[512 + MAX_PATH], target[MAX_PATH];
    if (Profiles_LinkTarget(p, target, ARRAYSIZE(target)))
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"Delete the profile \x201C%s\x201D?\n\nIts folder is a link to\n%s\nThe data in that folder stays on disk. The link, the profile's shortcuts and Claude's local files for it (logs and cache) are removed.",
                         p->name, target);
    else
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"Delete the profile \x201C%s\x201D?\n\nIts data folder (sign-in, local history, Claude Code and Cowork files) goes to the Recycle Bin and its shortcuts are removed.",
                         p->name);
    return Ui_Ask(g.dlg, IDI_WARNING, text, L"Delete profile", L"Cancel", TRUE);
}

static BOOL RefuseIfRunning(const Profile *p)
{
    if (!Claude_IsRunning(p)) return FALSE;
    Util_Message(g.dlg, MB_ICONWARNING,
                 L"Quit Claude for \x201C%s\x201D first (right-click its icon in the notification area, then Quit), then try again.",
                 p->name);
    return TRUE;
}

static void DoDelete(void)
{
    const Profile *sel = Selected();
    Profile p;
    if (!sel || sel->isStock) return;
    p = *sel;
    if (RefuseIfRunning(&p)) return;
    if (!ConfirmDelete(&p) || Quiet()) return;
    /* It may have been started while the question was open. */
    if (RefuseIfRunning(&p)) {
        Refresh(FALSE);
        return;
    }
    /* Cancelled: the user answered No to Windows' "delete permanently?". */
    if (Profiles_Delete(g.dlg, &p) == REMOVE_FAILED)
        Util_Message(g.dlg, MB_ICONWARNING,
                     L"\x201C%s\x201D could not be deleted completely. Make sure Claude is closed for this profile and try again.",
                     p.name);
    Refresh(FALSE);
}

static void DoSetDefault(void)
{
    const Profile *sel = Selected();
    if (!sel) return;
    Profiles_SetDefault(sel->folder);
    Refresh(FALSE);
}

static void CreateShortcutAt(const Profile *p, const WCHAR *path)
{
    HRESULT hr = Shortcut_CreateForProfile(&g.pkg, p, path);
    if (FAILED(hr))
        Util_Message(g.dlg, MB_ICONERROR, L"The shortcut could not be created (error 0x%08lX).", (unsigned long)hr);
    g.checkedFolder[0] = 0;
    UpdateButtons();
}

static void DoDesktopShortcut(void)
{
    const Profile *sel = Selected();
    Profile p;
    WCHAR path[MAX_PATH];
    if (!sel) return;
    p = *sel;
    if (!Shortcut_DesktopPathFor(&p, path, ARRAYSIZE(path))) {
        Util_Message(g.dlg, MB_ICONERROR, L"The desktop folder is not available.");
        return;
    }
    CreateShortcutAt(&p, path);
}

static void DoPinTaskbar(void)
{
    const Profile *sel = Selected();
    Profile p;
    HRESULT hr;
    if (!sel) return;
    p = *sel;
    hr = TaskbarPin_Pin(&g.pkg, &p);
    if (FAILED(hr))
        Util_Message(g.dlg, MB_ICONERROR, L"\x201C%s\x201D could not be pinned to the taskbar (error 0x%08lX).",
                     p.name, (unsigned long)hr);
    g.checkedFolder[0] = 0;
    UpdateButtons();
}

/* The profiles and the sessions share the window: Sessions swaps them, and
 * becomes "< Back" in the same place. */
static const int kProfileControls[] = { IDC_LIST, IDC_OPEN, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT, IDC_NOTE,
                                        IDC_SC_GROUP, IDC_SC_DESKTOP, IDC_SC_SAVEAS, IDC_SC_PIN, IDC_SC_START };

static void ToggleSessions(void)
{
    BOOL sessions = !SessionsView_Shown();
    size_t i;
    if (sessions) {
        const Profile *p = Selected();
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(g.dlg, kProfileControls[i]), SW_HIDE);
        SessionsView_Enter(&g.pkg, p ? p->folder : NULL);
        SendMessageW(g.dlg, DM_SETDEFID, IDOK, 0);   /* no default button there: Enter opens the session (SessionsView_Command) */
    } else {
        WCHAR folder[FOLDER_CCH];
        StringCchCopyW(folder, ARRAYSIZE(folder), SessionsView_Profile());
        SessionsView_Leave();
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(g.dlg, kProfileControls[i]), SW_SHOW);
        FillList(folder);
        SendMessageW(g.dlg, DM_SETDEFID, IDC_OPEN, 0);
        SetFocus(g.list);
    }
    SetDlgItemTextW(g.dlg, IDC_SESSIONS, sessions ? L"<  &Back" : L"&Sessions  >");
}

/* Adds the profile to the Start menu, or takes it out: looked up again
 * first, the entry may have changed since the button was drawn. */
static void DoStartMenu(void)
{
    const Profile *sel = Selected();
    Profile p;
    HRESULT hr;
    if (!sel) return;
    p = *sel;
    if (Shortcut_IsInStartMenu(&p)) {
        Shortcut_RemoveFromStartMenu(&p);
    } else if (FAILED(hr = Shortcut_AddToStartMenu(&g.pkg, &p))) {
        Util_Message(g.dlg, MB_ICONERROR, L"\x201C%s\x201D could not be added to the Start menu (error 0x%08lX).",
                     p.name, (unsigned long)hr);
    }
    g.checkedFolder[0] = 0;
    UpdateButtons();
}

/* The folders the shortcut, pin and Start menu buttons read: a change there
 * checks those buttons again, whoever made it. */
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
        g.shortcutsNotify = SHChangeNotifyRegister(g.dlg, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                                   SHCNE_CREATE | SHCNE_DELETE | SHCNE_RENAMEITEM | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR,
                                                   WM_APP_SHORTCUTS, n, entries);
    for (i = 0; i < (size_t)n; i++) CoTaskMemFree((void *)entries[i].pidl);
}

/* What the window shows can change outside it: a profile folder made or
 * removed, a name or color written by another copy, the claude:// app picked
 * in Windows, Claude installed or updated. A thread waits for the matching
 * registry and folder notifications and tells the window, which refreshes
 * once for a burst of them. */
typedef struct OutsideKey {
    const WCHAR *path;
    BOOL         subtree;
    DWORD        filter;
    LONG         change;
} OutsideKey;

static const OutsideKey kOutsideKeys[] = {
    { REG_ROOT, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PROFILES },
    { L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations", TRUE,
      REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_LINKS },
    { REG_PACKAGES, FALSE, REG_NOTIFY_CHANGE_NAME, CHANGE_PACKAGES },
};

static DWORD WINAPI WatchOutside(void *param)
{
    HWND dlg = (HWND)param;
    HANDLE handles[ARRAYSIZE(kOutsideKeys) + 2], dirs = INVALID_HANDLE_VALUE;
    HKEY keys[ARRAYSIZE(kOutsideKeys) + 2] = { 0 };
    LONG changes[ARRAYSIZE(kOutsideKeys) + 2];
    const OutsideKey *which[ARRAYSIZE(kOutsideKeys) + 2] = { 0 };
    WCHAR appData[MAX_PATH];
    DWORD n = 0, i, w;
    size_t k;

    handles[n] = g.outsideStop;
    changes[n++] = 0;
    for (k = 0; k < ARRAYSIZE(kOutsideKeys); k++) {
        HKEY key;
        HANDLE event;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kOutsideKeys[k].path, 0, KEY_NOTIFY, &key) != ERROR_SUCCESS) continue;
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!event || RegNotifyChangeKeyValue(key, kOutsideKeys[k].subtree, kOutsideKeys[k].filter, event, TRUE) != ERROR_SUCCESS) {
            if (event) CloseHandle(event);
            RegCloseKey(key);
            continue;
        }
        handles[n] = event;
        keys[n] = key;
        which[n] = &kOutsideKeys[k];
        changes[n++] = kOutsideKeys[k].change;
    }
    /* Profile folders sit directly in %APPDATA%. */
    if (Util_AppData(appData, ARRAYSIZE(appData)) &&
        (dirs = FindFirstChangeNotificationW(appData, FALSE, FILE_NOTIFY_CHANGE_DIR_NAME)) != INVALID_HANDLE_VALUE) {
        handles[n] = dirs;
        changes[n++] = CHANGE_PROFILES;
    }

    for (;;) {
        w = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + n) break;
        i = w - WAIT_OBJECT_0;
        /* Armed again first, so a change during the refresh still counts. */
        if (handles[i] == dirs) FindNextChangeNotification(dirs);
        else RegNotifyChangeKeyValue(keys[i], which[i]->subtree, which[i]->filter, handles[i], TRUE);
        if (InterlockedOr(&g_outside, changes[i]) == 0) PostMessageW(dlg, WM_APP_OUTSIDE, 0, 0);
    }

    for (i = 1; i < n; i++) {
        if (handles[i] == dirs) {
            FindCloseChangeNotification(dirs);
        } else {
            RegCloseKey(keys[i]);
            CloseHandle(handles[i]);
        }
    }
    return 0;
}

static void StartWatchingOutside(HWND d)
{
    g.outsideStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g.outsideStop) g.outsideThread = CreateThread(NULL, 0, WatchOutside, d, 0, NULL);
}

static void StopWatchingOutside(void)
{
    if (g.outsideStop) SetEvent(g.outsideStop);
    if (g.outsideThread) {
        WaitForSingleObject(g.outsideThread, 2000);
        CloseHandle(g.outsideThread);
    }
    if (g.outsideStop) CloseHandle(g.outsideStop);
    g.outsideThread = g.outsideStop = NULL;
}

static void DoSaveShortcut(void)
{
    const Profile *sel = Selected();
    Profile p;
    IFileSaveDialog *fd = NULL;
    IShellItem *folder = NULL, *item = NULL;
    COMDLG_FILTERSPEC spec = { L"Shortcut (*.lnk)", L"*.lnk" };
    WCHAR name[MAX_PATH], path[MAX_PATH];
    PWSTR chosen = NULL;
    FILEOPENDIALOGOPTIONS opts = 0;

    if (!sel) return;
    p = *sel;
    if (FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&fd)))
        return;
    Core_ShortcutFileName(p.name, 1, name, ARRAYSIZE(name));
    IFileSaveDialog_SetTitle(fd, L"Create shortcut");
    IFileSaveDialog_SetFileTypes(fd, 1, &spec);
    IFileSaveDialog_SetDefaultExtension(fd, L"lnk");
    IFileSaveDialog_SetFileName(fd, name);
    if (SUCCEEDED(IFileSaveDialog_GetOptions(fd, &opts)))
        IFileSaveDialog_SetOptions(fd, opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(SHCreateItemInKnownFolder(&FOLDERID_Desktop, 0, NULL, &IID_IShellItem, (void **)&folder))) {
        IFileSaveDialog_SetDefaultFolder(fd, folder);
        IShellItem_Release(folder);
    }
    if (SUCCEEDED(IFileSaveDialog_Show(fd, g.dlg)) && !Quiet() && SUCCEEDED(IFileSaveDialog_GetResult(fd, &item))) {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &chosen)) && chosen) {
            if (SUCCEEDED(StringCchCopyW(path, ARRAYSIZE(path), chosen)) &&
                (Core_EndsWithI(path, L".lnk") || SUCCEEDED(StringCchCatW(path, ARRAYSIZE(path), L".lnk"))))
                CreateShortcutAt(&p, path);
            CoTaskMemFree(chosen);
        }
        IShellItem_Release(item);
    }
    IFileSaveDialog_Release(fd);
}

/* Works on a private copy of the list: the dialog's choices are indexed on
 * it, and nothing reloads it until the end. */
static void DoUninstall(void)
{
    UninstallDialog u;
    ProfileList snap;
    WCHAR stock[LABEL_CCH];
    int s;

    if (g.busy || g.uninstalled) return;
    Refresh(TRUE);
    snap = g.profiles;
    ZeroMemory(&u, sizeof u);
    u.list = &snap;
    g.busy = TRUE;
    if (Ui_Dialog(g.dlg, IDD_UNINSTALL, UninstallProc, (LPARAM)&u) != IDOK || g.closing) {
        g.busy = FALSE;
        return;
    }
    Claude_UpdateRunning(&snap);
    s = Profiles_Find(&snap, STOCK_FOLDER);
    StringCchCopyW(stock, ARRAYSIZE(stock), s >= 0 ? snap.items[s].name : STOCK_DEFAULT_NAME);
    if (!Install_Uninstall(g.dlg, &snap, u.removeData)) {
        g.busy = FALSE;
        Refresh(TRUE);
        return;
    }
    g.uninstalled = TRUE;
    g.busy = FALSE;
    Util_Message(g.dlg, MB_ICONINFORMATION,
                 APP_NAME L" has been removed.\n\nClaude Desktop and its \x201C%s\x201D profile are untouched.", stock);
    EndDialog(g.dlg, 0);
}

/* Dialogs this window owns (directly or through another dialog). */
static BOOL CALLBACK CloseOwnedProc(HWND w, LPARAM lp)
{
    HWND owner;
    for (owner = GetWindow(w, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER)) {
        if (owner == (HWND)lp) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            break;
        }
    }
    return TRUE;
}

static void Close(HWND d)
{
    g.closing = TRUE;
    EnumThreadWindows(GetCurrentThreadId(), CloseOwnedProc, (LPARAM)d);
    EndDialog(d, 0);
}

/* Only the user can make Claude Desktop Profiles Manager the app for
 * claude:// links, in Windows' own chooser: say what to pick, then let Windows
 * ask. */
static void SetUpLinks(void)
{
    WCHAR exe[MAX_PATH];
    if (!g.pkg.found || Handler_UserChoice() == USERCHOICE_OURS) return;
    if (!Ui_Ask(g.dlg, IDI_INFORMATION,
                L"Windows now asks which app opens Claude links.\n\n"
                L"Choose \x201C" APP_NAME L"\x201D, then Always (or Set default). Sign-ins then always come back to the window that started them.",
                L"Continue", L"Later", FALSE))
        return;
    if (Util_InstallExe(exe, ARRAYSIZE(exe))) Handler_Register(exe);
    if (!Handler_AskUser())
        Util_Message(g.dlg, MB_ICONWARNING,
                     L"Windows kept the app it uses for Claude links.\n\n"
                     L"Open Settings > Apps > Default apps > Choose defaults by link type, find CLAUDE and choose \x201C" APP_NAME L"\x201D.");
    UpdateStatus();
}

static void DoFix(void)
{
    if (g.fix == FIX_GET_CLAUDE) Util_OpenUrl(APP_DOWNLOAD_URL);
    else if (g.fix == FIX_LINKS) SetUpLinks();
}

/* ------------------------------------------------------------ new release */

/* A newer release shows next to the version, with its Update button. */
static void ShowUpdate(void)
{
    WCHAR version[32], text[128];
    RECT r = { 94, 249, 266, 259 };
    if (g.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    StringCchPrintfW(text, ARRAYSIZE(text), L"Version " APP_VERSION_WSTR L" \x00B7 version %s is available", version);
    SetTextIfChanged(IDC_ABOUT, text);
    MapDialogRect(g.dlg, &r);
    SetWindowPos(GetDlgItem(g.dlg, IDC_ABOUT), NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(GetDlgItem(g.dlg, IDC_UPDATE), SW_SHOW);
}

static void DoUpdate(void)
{
    WCHAR version[32], text[128];
    if (g.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    g.updating = TRUE;
    Enable(IDC_UPDATE, FALSE);
    StringCchPrintfW(text, ARRAYSIZE(text), L"Downloading version %s\x2026", version);
    SetTextIfChanged(IDC_ABOUT, text);
    Update_Download(g.dlg, WM_APP_DOWNLOADED);
}

/* The new release installs itself and closes this window. */
static void Downloaded(UpdateResult result)
{
    if (result == UPDATE_READY && Update_Run()) {
        SetTextIfChanged(IDC_ABOUT, L"Installing the new version\x2026");
        return;
    }
    g.updating = FALSE;
    Enable(IDC_UPDATE, TRUE);
    if (result == UPDATE_NOT_SIGNED)
        Util_Message(g.dlg, MB_ICONWARNING,
                     L"The new version was not installed: the downloaded file is not signed by the author of " APP_NAME L".");
    else if (Util_Message(g.dlg, MB_ICONWARNING | MB_YESNO, L"The new version could not be downloaded.\n\nOpen its download page?") == IDYES)
        Util_OpenUrl(APP_RELEASES_URL);
    ShowUpdate();
}

/* ----------------------------------------------------------- main window */

static void SetIcons(HWND d)
{
    UINT dpi = GetDpiForWindow(d);
    HICON big = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), 0);
    HICON little = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
    SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)little);
    if (g.bigIcon) DestroyIcon(g.bigIcon);
    if (g.smallIcon) DestroyIcon(g.smallIcon);
    g.bigIcon = big;
    g.smallIcon = little;
}

static void AddColumns(void)
{
    static const WCHAR *const titles[] = { L"Profile", L"Role", L"Data folder" };
    LVCOLUMNW c;
    int i;
    ZeroMemory(&c, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < (int)ARRAYSIZE(titles); i++) {
        c.pszText = (LPWSTR)titles[i];
        c.cx = 60;
        ListView_InsertColumn(g.list, i, &c);
    }
    LayoutColumns();
}

/* The profile buttons start level with the list's first row, under its
 * header, and keep their spacing (dialog units from the Open button). */
static void LayoutSideButtons(void)
{
    static const struct { int id, offset; } kButtons[] = { { IDC_OPEN, 0 }, { IDC_NEW, 21 }, { IDC_EDIT, 40 }, { IDC_DELETE, 59 } };
    HWND header = ListView_GetHeader(g.list);
    RECT top;
    int i;
    if (!header) return;
    GetWindowRect(header, &top);
    MapWindowPoints(NULL, g.dlg, (POINT *)&top, 2);
    for (i = 0; i < (int)ARRAYSIZE(kButtons); i++) {
        HWND button = GetDlgItem(g.dlg, kButtons[i].id);
        RECT r, offset = { 0, 0, 0, kButtons[i].offset };
        GetWindowRect(button, &r);
        MapWindowPoints(NULL, g.dlg, (POINT *)&r, 2);
        MapDialogRect(g.dlg, &offset);
        SetWindowPos(button, NULL, r.left, top.bottom + offset.bottom, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    }
}

static INT_PTR CALLBACK MainProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        g.dlg = d;
        g.list = GetDlgItem(d, IDC_LIST);
        g.groupColor = -1;
        SetIcons(d);
        ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        AddColumns();
        g.listArea = Theme_SmoothView(g.list);
        SessionsView_Init(d);
        Theme_SetStrong(GetDlgItem(d, IDC_SESSIONS));   /* it leads to the other view */
        Theme_Apply(d);
        LayoutSideButtons();
        SetDlgItemTextW(d, IDC_ABOUT, L"Version " APP_VERSION_WSTR L" \x00B7 by <a href=\"" APP_AUTHOR_URL L"\">Freenitial</a>, not affiliated with Anthropic");
        Claude_FindPackage(&g.pkg);
        Profiles_Load(&g.profiles);
        TaskbarPin_RepairOurs();
        HealIcons(&g.profiles);
        FillList(NULL);
        UpdateStatus();
        WatchShortcutFolders();
        StartWatchingOutside(d);
        /* Started to uninstall (Settings > Apps): only the uninstall dialog
         * shows, and the manager never opens (wParam 1). */
        if (lp == GUI_UNINSTALL) {
            g.uninstallOnly = TRUE;
            PostMessageW(d, WM_APP_UNINSTALL, 1, 0);
        } else if (lp == GUI_SET_UP_LINKS) {
            PostMessageW(d, WM_APP_SETUPLINKS, 0, 0);
        }
        if (lp != GUI_UNINSTALL) Update_Check(d, WM_APP_UPDATE);
        SetFocus(g.list);
        return FALSE;

    case WM_APP_OUTSIDE: {
        LONG changes = InterlockedExchange(&g_outside, 0);
        if (!Quiet()) {
            Refresh((changes & CHANGE_PACKAGES) != 0);
            if (changes & CHANGE_PROFILES) SessionsView_Reload();
        }
        return TRUE;
    }

    case WM_APP_SESSIONS:
        if (!Quiet()) SessionsView_Reload();
        return TRUE;

    case WM_ACTIVATE:
        /* Back to the window: the desktop shortcut or the pin may have been
         * removed meanwhile, so their buttons are checked again. */
        if (LOWORD(wp) != WA_INACTIVE && g.list && !Quiet()) {
            g.checkedFolder[0] = 0;
            Refresh(TRUE);
            SessionsView_Reload();   /* which profiles run may have changed */
            /* The dialog manager now gives focus back to the control that had
             * it, which the refresh may just have hidden. */
            PostMessageW(d, WM_APP_FIXFOCUS, 0, 0);
        }
        break;

    case WM_APP_FIXFOCUS: {
        HWND button = GetDlgItem(d, IDC_FIX);
        if (GetFocus() == button && !IsWindowVisible(button)) {
            BOOL sessions = SessionsView_Shown();
            SendMessageW(d, WM_NEXTDLGCTL, (WPARAM)(sessions ? GetDlgItem(d, IDC_S_TREE) : g.list), TRUE);
            SendMessageW(d, DM_SETDEFID, sessions ? IDOK : IDC_OPEN, 0);
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        const NMHDR *h = (const NMHDR *)lp;
        if (h->idFrom == IDC_ABOUT && (h->code == NM_CLICK || h->code == NM_RETURN)) {
            Util_OpenUrl(APP_AUTHOR_URL);   /* the only link there */
            return TRUE;
        }
        if (h->idFrom != IDC_LIST) {
            LRESULT result;
            if (!SessionsView_Notify(h, lp, &result)) break;
            SetWindowLongPtrW(d, DWLP_MSGRESULT, result);
            return TRUE;
        }
        if (h->code == NM_SETFOCUS || h->code == NM_KILLFOCUS) InvalidateRect(g.list, NULL, FALSE);
        if (h->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *nm = (const NMLISTVIEW *)lp;
            /* Selecting another row first deselects the old one: the buttons
             * follow once, when the list is done. */
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED) && !g.selectionPending) {
                g.selectionPending = TRUE;
                PostMessageW(d, WM_APP_SELECTION, 0, 0);
            }
        } else if (h->code == NM_DBLCLK) {
            if (((const NMITEMACTIVATE *)lp)->iItem >= 0) DoOpen();
        } else if (h->code == LVN_KEYDOWN) {
            if (((const NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) DoDelete();
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN:       DoOpen(); return TRUE;
        case IDC_NEW:        DoNew(); return TRUE;
        case IDC_EDIT:       DoEdit(); return TRUE;
        case IDC_DELETE:     DoDelete(); return TRUE;
        case IDC_DEFAULT:    DoSetDefault(); return TRUE;
        case IDC_SC_DESKTOP: DoDesktopShortcut(); return TRUE;
        case IDC_SC_SAVEAS:  DoSaveShortcut(); return TRUE;
        case IDC_SC_PIN:     DoPinTaskbar(); return TRUE;
        case IDC_SC_START:   DoStartMenu(); return TRUE;
        case IDC_SESSIONS:   ToggleSessions(); return TRUE;
        case IDC_UNINSTALL:  DoUninstall(); return TRUE;
        case IDC_FIX:        DoFix(); return TRUE;
        case IDC_UPDATE:     DoUpdate(); return TRUE;
        case IDCANCEL:
            /* Esc in a search box that holds text clears it. */
            if (!SessionsView_ClearSearch()) Close(d);
            return TRUE;
        }
        if (SessionsView_Command(wp)) return TRUE;
        break;

    case WM_CLOSE:
        /* Also sent by an installer updating the program: close any dialog
         * that is open too, so the process really exits. */
        Close(d);
        return TRUE;

    case WM_APP_UNINSTALL:
        if (Quiet()) return TRUE;
        if (!IsWindowEnabled(d)) {
            g.pendingUninstall = TRUE;   /* a dialog is open: after it closes */
            return TRUE;
        }
        DoUninstall();
        if (wp && g.uninstallOnly && !g.uninstalled && !g.closing) EndDialog(d, 0);
        return TRUE;

    case WM_APP_SELECTION:
        g.selectionPending = FALSE;
        UpdateButtons();
        return TRUE;

    case WM_APP_SHORTCUTS: {
        PIDLIST_ABSOLUTE *pidls = NULL;
        LONG event = 0;
        HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &event);
        if (lock) SHChangeNotification_Unlock(lock);
        if (!Quiet()) {
            g.checkedFolder[0] = 0;
            UpdateButtons();
        }
        return TRUE;
    }

    case WM_APP_UPDATE:
        ShowUpdate();
        return TRUE;

    case WM_APP_DOWNLOADED:
        Downloaded((UpdateResult)wp);
        return TRUE;

    case WM_APP_SETUPLINKS:
        if (!Quiet() && IsWindowEnabled(d)) {
            /* The question that follows must be seen, with the manager behind it. */
            if (!IsWindowVisible(d)) ShowWindow(d, SW_SHOWNORMAL);
            SetForegroundWindow(d);
            SetUpLinks();
        }
        return TRUE;

    case WM_APP_SHOW:
        /* Opening the manager while a Settings uninstall is on screen: show
         * the manager once that dialog is cancelled. */
        g.uninstallOnly = FALSE;
        return TRUE;

    case WM_ENABLE:
        if (wp && g.pendingUninstall) {
            g.pendingUninstall = FALSE;
            PostMessageW(d, WM_APP_UNINSTALL, 0, 0);
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
        if (GetDlgCtrlID((HWND)lp) == IDC_S_PROFILES) {   /* the sessions' side bar */
            SetTextColor((HDC)wp, Theme_Color(THEME_TEXT));
            SetBkColor((HDC)wp, Theme_Color(THEME_FACE));
            return (INT_PTR)Theme_Brush(THEME_FACE);
        }
        return Theme_CtlColor(msg, wp, lp, IDC_ABOUT);
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return Theme_CtlColor(msg, wp, lp, IDC_ABOUT);

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        Theme_Follow(d, msg, wp, lp);
        break;

    case WM_DPICHANGED:
        PostMessageW(d, WM_APP_RELAYOUT, 0, 0);
        break;

    case WM_APP_RELAYOUT: {
        const Profile *p = Selected();
        WCHAR sel[FOLDER_CCH] = L"";
        if (p) StringCchCopyW(sel, ARRAYSIZE(sel), p->folder);
        SetIcons(d);
        LayoutSideButtons();
        FillList(sel);
        SessionsView_Relayout();
        return TRUE;
    }

    case WM_DESTROY:
        Theme_Forget(d);
        StopWatchingOutside();
        SessionsView_Destroy();
        if (g.shortcutsNotify) SHChangeNotifyDeregister(g.shortcutsNotify);
        g.shortcutsNotify = 0;
        if (g.bigIcon) DestroyIcon(g.bigIcon);
        if (g.smallIcon) DestroyIcon(g.smallIcon);
        g.bigIcon = g.smallIcon = NULL;
        break;
    }
    return FALSE;
}

int Gui_Run(GuiStart start)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    HANDLE mutex;
    HWND existing;
    WCHAR exe[MAX_PATH];

    if (!Install_IsInstalledCopy()) {
        if (start != GUI_UNINSTALL) return Install_Run(TRUE) ? 0 : 1;
        if (Util_InstallExe(exe, ARRAYSIZE(exe)) && Util_FileExists(exe)) {
            ShellExecuteW(NULL, L"open", exe, L"--uninstall", NULL, SW_SHOWNORMAL);
            return 0;
        }
        Util_Message(NULL, MB_ICONINFORMATION, APP_NAME L" is not installed.");
        return 1;
    }

    mutex = CreateMutexW(NULL, FALSE, MANAGER_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
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
        return 0;
    }

    Install_Repair();
    SetCurrentProcessExplicitAppUserModelID(APP_AUMID_PREFIX L"Manager");

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefDlgProcW;
    wc.cbWndExtra = DLGWINDOWEXTRA;
    wc.hInstance = g_hInst;
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = APP_WINDOW_CLASS;
    RegisterClassExW(&wc);

    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainProc, (LPARAM)start);
    if (mutex) CloseHandle(mutex);
    if (g.uninstalled) Install_FinishUninstall();
    return 0;
}
