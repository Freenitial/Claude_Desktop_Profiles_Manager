/*
 * The sessions view of the manager window: each profile's Claude Code
 * sessions. On the left the profiles, with their badge and how many sessions
 * they list; in the middle the chosen profile's favorites, projects and
 * sessions as a tree (every row drawn here), or why it has none; on the
 * right the selected session: where it lives, then each profile with what
 * is going on there and an Actions box (open, rename, favorite, remove, or
 * share and copy where it is not listed), what is not listed, and Delete
 * everywhere at the bottom. The same actions are in the tree's context menu.
 * What they do is in sessionedit.c; colors, fonts, rows, buttons and boxes
 * come from theme.c, as everywhere.
 *
 * While it shows, it follows the disk without polling: a thread waits for
 * folder notifications on each profile's session entries and on the
 * transcripts folder and posts WM_APP_SESSIONS to the window, once for a
 * burst of them. A reload that changes nothing the tree shows leaves the
 * tree alone (no flicker, no scroll); one that does keeps the scroll place.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#define MAX_COLLAPSED  128
#define MAX_CHIPS      96
#define WATCH_MAX      (MAX_PROFILES + 1)
#define NODE_FAVORITES (-1)             /* tree lParam: >= 0 a row, else a folder (-2 - group) */
#define FAVORITES_KEY  L"\x2605"
#define IDM_ACTION     0x5000           /* menu commands: IDM_ACTION + action * MAX_PROFILES + profile */

/* Tree rows are measured in units (the tree's item height): a session is
 * SESSION_UNITS high, a folder FOLDER_UNITS, plus GAP_UNITS of room above it
 * after the first folder. */
#define SESSION_UNITS  5
#define FOLDER_UNITS   6
#define GAP_UNITS      2

typedef enum Action {
    ACT_OPEN, ACT_RENAME, ACT_STAR, ACT_REMOVE, ACT_KEEP, ACT_SHARE, ACT_COPY, ACT_SHOW_FOLDER, ACT_DELETE_ALL,
    ACT_MENU,                           /* a profile's Actions box: its menu */
    ACTIONS
} Action;

typedef struct Chip {                   /* a control drawn in the details */
    RECT   rc;
    Action action;
    int    profile;
} Chip;

typedef struct ChipSet {                /* the controls a window of the details drew, as last drawn */
    Chip chip[MAX_CHIPS];
    int  count;
} ChipSet;

typedef struct SessionsView {
    HWND          dlg, profiles, tree, search, archived, details;
    HWND          parts;                /* in the details, each profile's part: what scrolls */
    HWND          treeArea, profilesArea;   /* the views the tree and the profiles scroll in */
    BOOL          shown, loaded, filling, showArchived;
    SessionSet    set;
    ULONGLONG     shape;                /* what the tree shows of `set` (TreeShape) */
    ClaudePackage pkg;
    WCHAR         filter[128];
    WCHAR         selectedKey[SESSION_ID_CCH];   /* the session in the details */
    WCHAR         folder[FOLDER_CCH];   /* the profile shown */
    WCHAR         firstKey[MAX_PATH];   /* the row on top of the tree before a reload */
    BOOL          firstIsFolder, firstInFavorites;
    HIMAGELIST    icons, dots;          /* profile icons for the list; badges for the details */
    int           iconSize, dotSize, pad, indent, unit;
    ThemeFonts    fonts;
    ChipSet       fixed, scrolled;      /* the controls of the details, and of their part that scrolls */
    HWND          hotIn, pressedIn, openIn;   /* where the control under the mouse, pressed, with its menu open is */
    int           hotChip, pressedChip, openChip;
    int           scroll;               /* how far the profiles' parts are scrolled, px */
    HANDLE        watchThread, watchStop;
    WCHAR         watched[MAX_PROFILES][FOLDER_CCH];
    int           watchedCount;
    WCHAR         collapsed[MAX_COLLAPSED][MAX_PATH];   /* folders the user folded */
    int           collapsedCount;
} SessionsView;

static SessionsView v;
static volatile LONG g_pending;

static const int kControls[] = { IDC_S_PROFILES, IDC_S_SEARCH, IDC_S_ARCHIVED, IDC_S_TREE, IDC_S_DETAILS };

static void Load(BOOL force);

/* ------------------------------------------------------------- formatting */

/* A time in milliseconds since 1970 as the user's short date and time;
 * FALSE for none. */
static BOOL FormatWhen(ULONGLONG ms, WCHAR *date, int dateCch, WCHAR *time, int timeCch)
{
    ULARGE_INTEGER u;
    FILETIME ft;
    SYSTEMTIME utc, local;
    date[0] = time[0] = 0;
    if (!ms) return FALSE;
    u.QuadPart = ms * 10000ULL + 116444736000000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local) &&
           GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, dateCch, NULL) &&
           GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, time, timeCch);
}

static void FormatSize(ULONGLONG bytes, WCHAR *out, size_t cch)
{
    if (bytes >= 1024ULL * 1024ULL)
        StringCchPrintfW(out, cch, L"%lu.%lu MB", (unsigned long)(bytes / (1024ULL * 1024ULL)),
                         (unsigned long)((bytes % (1024ULL * 1024ULL)) * 10 / (1024ULL * 1024ULL)));
    else
        StringCchPrintfW(out, cch, L"%lu KB", (unsigned long)((bytes + 1023) / 1024));
}

/* ------------------------------------------------------------------ rows */

static int ProfileIndex(void)
{
    int i = Profiles_Find(&v.set.profiles, v.folder);
    return i >= 0 ? i : (v.set.profiles.count > 0 ? 0 : -1);
}

static const Profile *ProfileAt(int p)
{
    return &v.set.profiles.items[p];
}

static const SessionEntry *EntryOf(const SessionRow *row, int profile)
{
    return profile >= 0 && row->entry[profile] >= 0 ? &v.set.entries[row->entry[profile]] : NULL;
}

/* A session's title in a profile, a new one waiting included. */
static const WCHAR *TitleIn(const SessionRow *row, int profile)
{
    const SessionEntry *e = EntryOf(row, profile);
    if (e && e->pendingTitle[0]) return e->pendingTitle;
    return e && e->title[0] ? e->title : SessionStore_RowTitle(&v.set, row);
}

/* A favorite in a profile, a change waiting included. */
static BOOL StarredIn(const SessionEntry *e)
{
    return e && (e->pendingStar >= 0 ? e->pendingStar == 1 : e->starred);
}

static BOOL IsScratch(const SessionRow *row)
{
    return v.set.groups[row->group].scratchOf >= 0;
}

static BOOL Matches(const SessionRow *row, int profile)
{
    if (!v.filter[0]) return TRUE;
    return Core_ContainsI(TitleIn(row, profile), v.filter) || Core_ContainsI(v.set.groups[row->group].name, v.filter) ||
           Core_ContainsI(row->cwd, v.filter);
}

static BOOL Shown(const SessionRow *row, int profile)
{
    const SessionEntry *e = EntryOf(row, profile);
    return e && (v.showArchived || !e->archived) && Matches(row, profile);
}

static int SelectedRow(void)
{
    int r;
    if (!v.selectedKey[0]) return -1;
    for (r = 0; r < v.set.rowCount; r++)
        if (CompareStringOrdinal(v.set.rows[r].key, -1, v.selectedKey, -1, TRUE) == CSTR_EQUAL) return r;
    return -1;
}

static const WCHAR *GroupKey(int g)
{
    return g < 0 ? FAVORITES_KEY : v.set.groups[g].path[0] ? v.set.groups[g].path : v.set.groups[g].name;
}

static int NodeGroup(LPARAM node)
{
    return node == NODE_FAVORITES ? -1 : (int)(-2 - node);
}

static int BitCount(DWORD bits)
{
    int n = 0;
    for (; bits; bits &= bits - 1) n++;
    return n;
}

/* What the tree shows of a set: its profiles, its rows in their order with
 * their titles, favorites, archives, changes waiting and folders. A reload
 * that keeps it only redraws the side bar and the details. */
static ULONGLONG TreeShape(const SessionSet *s)
{
    ULONGLONG h = CORE_HASH_START;
    int i, p;
    h = Core_HashBytes(h, &s->profiles.count, sizeof s->profiles.count);
    for (i = 0; i < s->profiles.count; i++) {
        const Profile *pr = &s->profiles.items[i];
        h = Core_HashBytes(h, pr->folder, (wcslen(pr->folder) + 1) * sizeof(WCHAR));
        h = Core_HashBytes(h, pr->name, (wcslen(pr->name) + 1) * sizeof(WCHAR));
        h = Core_HashBytes(h, &pr->color, sizeof pr->color);
    }
    h = Core_HashBytes(h, s->source, sizeof s->source);
    h = Core_HashBytes(h, &s->noTranscripts, sizeof s->noTranscripts);
    h = Core_HashBytes(h, &s->rowCount, sizeof s->rowCount);
    for (i = 0; i < s->rowCount; i++) {
        const SessionRow *row = &s->rows[i];
        h = Core_HashBytes(h, row->key, (wcslen(row->key) + 1) * sizeof(WCHAR));
        h = Core_HashBytes(h, &row->group, sizeof row->group);
        h = Core_HashBytes(h, &row->transcript, sizeof row->transcript);
        for (p = 0; p < s->profiles.count; p++) {
            const SessionEntry *e = row->entry[p] >= 0 ? &s->entries[row->entry[p]] : NULL;
            int flags[5] = { e != NULL, e && e->starred, e && e->archived, e ? e->pendingStar : -1, e && e->pendingRemove };
            h = Core_HashBytes(h, flags, sizeof flags);
            if (e) {
                h = Core_HashBytes(h, e->title, (wcslen(e->title) + 1) * sizeof(WCHAR));
                h = Core_HashBytes(h, e->pendingTitle, (wcslen(e->pendingTitle) + 1) * sizeof(WCHAR));
            }
        }
    }
    for (i = 0; i < s->groupCount; i++) {
        h = Core_HashBytes(h, s->groups[i].name, (wcslen(s->groups[i].name) + 1) * sizeof(WCHAR));
        h = Core_HashBytes(h, &s->groups[i].scratchOf, sizeof s->groups[i].scratchOf);
    }
    return h;
}

/* ---------------------------------------------------------- measurements */

static HFONT Font(ThemeFont role)
{
    return v.fonts.font[role] ? v.fonts.font[role] : (HFONT)SendMessageW(v.dlg, WM_GETFONT, 0, 0);
}

static HFONT DialogFont(void)
{
    return (HFONT)SendMessageW(v.dlg, WM_GETFONT, 0, 0);
}

static int FontHeight(HFONT font)
{
    TEXTMETRICW tm;
    HDC dc = GetDC(v.dlg);
    HGDIOBJ old = SelectObject(dc, font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(v.dlg, dc);
    return tm.tmHeight;
}

static int LineHeight(ThemeFont role)
{
    return FontHeight(Font(role));
}

static int WidthOf(HDC dc, HFONT font, const WCHAR *text)
{
    SIZE ext = { 0, 0 };
    HGDIOBJ old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &ext);
    SelectObject(dc, old);
    return ext.cx;
}

static int TextWidth(HDC dc, ThemeFont role, const WCHAR *text)
{
    return WidthOf(dc, Font(role), text);
}

static RECT ChildRect(HWND child)
{
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(NULL, v.dlg, (POINT *)&r, 2);
    return r;
}

/* "Show archived" ends a little before the tree does (room before the
 * details' folder), the search box fills the rest. */
static void LayoutTop(void)
{
    WCHAR text[64];
    RECT tree = ChildRect(v.treeArea), box = ChildRect(v.archived), search = ChildRect(v.search), gap = { 0, 0, 6, 0 }, label = { 0 };
    UINT dpi = GetDpiForWindow(v.dlg);
    SIZE ideal = { 0, 0 };
    int width, end = tree.right - MulDiv(5, (int)dpi, 96);
    MapDialogRect(v.dlg, &gap);
    if (Theme_IsDark()) {
        /* As theme.c draws a dark check box: glyph, 4 px, caption. */
        HDC dc = GetDC(v.archived);
        HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(v.archived, WM_GETFONT, 0, 0));
        GetWindowTextW(v.archived, text, ARRAYSIZE(text));
        DrawTextW(dc, text, -1, &label, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, old);
        ReleaseDC(v.archived, dc);
        width = MulDiv(13 + 4, (int)dpi, 96) + (label.right - label.left) + 2;
    } else {
        width = SendMessageW(v.archived, BCM_GETIDEALSIZE, 0, (LPARAM)&ideal) && ideal.cx > 0 ? ideal.cx : box.right - box.left;
    }
    SetWindowPos(v.archived, NULL, end - width, box.top, width, box.bottom - box.top, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(v.search, NULL, search.left, search.top, end - width - gap.right - search.left, search.bottom - search.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Fonts, icon sizes and row heights for the window's scale. */
static void Measure(void)
{
    UINT dpi = GetDpiForWindow(v.dlg);
    int line, folderRow, i;
    Theme_CreateFonts(v.dlg, &v.fonts);
    v.iconSize = MulDiv(32, (int)dpi, 96);
    v.pad = MulDiv(6, (int)dpi, 96);
    line = LineHeight(THEME_FONT_TEXT);
    v.dotSize = line * 3 / 4;
    SendMessageW(v.profiles, LB_SETITEMHEIGHT, 0, MAKELPARAM(max(v.iconSize, 2 * line) + 2 * v.pad, 0));
    folderRow = max(line * 19 / 10, LineHeight(THEME_FONT_HEADING) + v.pad);
    v.unit = max(1, folderRow / FOLDER_UNITS);
    TreeView_SetItemHeight(v.tree, v.unit);
    TreeView_SetIndent(v.tree, MulDiv(18, (int)dpi, 96));
    v.indent = (int)TreeView_GetIndent(v.tree);
    Theme_SetScrollRow(v.tree, SESSION_UNITS * v.unit);   /* a wheel line: a session */
    Theme_SetScrollRow(v.parts, line);
    LayoutTop();
    if (v.icons) ImageList_Destroy(v.icons);
    if (v.dots) ImageList_Destroy(v.dots);
    v.icons = ImageList_Create(v.iconSize, v.iconSize, ILC_COLOR32, v.set.profiles.count, 1);
    v.dots = ImageList_Create(v.dotSize, v.dotSize, ILC_COLOR32, v.set.profiles.count, 1);
    for (i = 0; i < v.set.profiles.count; i++) {
        HICON icon = Icons_Create(&v.pkg, ProfileAt(i), v.iconSize);
        HICON dot = Icons_CreateBadge(ProfileAt(i)->color, v.dotSize);
        if (v.icons) ImageList_AddIcon(v.icons, icon);
        if (v.dots) ImageList_AddIcon(v.dots, dot);
        if (icon) DestroyIcon(icon);
        if (dot) DestroyIcon(dot);
    }
}

/* ------------------------------------------------------------------ text */

/* `text` in `rc` (one line, cut with an ellipsis), vertically centered. */
static void TextLine(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, RECT *rc, UINT extra)
{
    HGDIOBJ old = SelectObject(dc, Font(role));
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, rc, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS | extra);
    SelectObject(dc, old);
}

static int Ascent(HDC dc, ThemeFont role, int *height)
{
    TEXTMETRICW tm;
    HGDIOBJ old = SelectObject(dc, Font(role));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    if (height) *height = tm.tmHeight;
    return tm.tmAscent;
}

/* `text` from x to at most `right`, its baseline on `baseline` (texts of two
 * sizes on one line share it). Returns where it ended. */
static int TextAt(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, int x, int right, int baseline)
{
    int height, ascent = Ascent(dc, role, &height);
    RECT r;
    HGDIOBJ old = SelectObject(dc, Font(role));
    r.left = x;
    r.right = min(right, x + TextWidth(dc, role, text));
    r.top = baseline - ascent;
    r.bottom = r.top + height;
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    return r.right;
}

/* A baseline for text of `role` centered in `rc`. */
static int CenteredBaseline(HDC dc, const RECT *rc, ThemeFont role)
{
    int height, ascent = Ascent(dc, role, &height);
    return rc->top + (rc->bottom - rc->top - height) / 2 + ascent;
}

/* `text` wrapped from `top`, on at most `lines` lines (the last cut with an
 * ellipsis). Returns the bottom. */
static int Paragraph(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, int left, int right, int top, int lines)
{
    RECT need = { left, top, right, top }, draw;
    int lh = LineHeight(role);
    HGDIOBJ old = SelectObject(dc, Font(role));
    DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SetRect(&draw, left, top, right, top + min(need.bottom - need.top, lines * lh));
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &draw, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    return draw.bottom;
}

/* -------------------------------------------------------------- profiles */

static int SessionCount(int profile)
{
    int r, n = 0;
    for (r = 0; r < v.set.rowCount; r++) {
        const SessionEntry *e = EntryOf(&v.set.rows[r], profile);
        if (e && (v.showArchived || !e->archived)) n++;
    }
    return n;
}

static void FillProfiles(void)
{
    int i, pick = ProfileIndex();
    SendMessageW(v.profiles, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v.profiles, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < v.set.profiles.count; i++) SendMessageW(v.profiles, LB_ADDSTRING, 0, (LPARAM)i);
    SendMessageW(v.profiles, LB_SETCURSEL, (WPARAM)pick, 0);
    SendMessageW(v.profiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v.profiles, NULL, TRUE);
}

/* Icon, name, then "open · 12 sessions" in grey, on the window's background
 * (a side bar: no frame). */
static void DrawProfile(const DRAWITEMSTRUCT *di)
{
    WCHAR second[64];
    ThemeBuffer buffer;
    RECT rc = di->rcItem, line;
    UINT state = (di->itemState & ODS_SELECTED) ? THEME_ROW_SELECTED : 0;
    COLORREF text;
    HDC dc;
    int p = (int)di->itemData, lh, n, top;
    if (di->itemID == (UINT)-1 || p < 0 || p >= v.set.profiles.count) return;
    dc = Theme_BufferBegin(&buffer, di->hDC, &rc);
    text = Theme_DrawRow(dc, &rc, state, Theme_Color(THEME_FACE));
    if (v.icons) ImageList_Draw(v.icons, p, dc, rc.left + v.pad, rc.top + (rc.bottom - rc.top - v.iconSize) / 2, ILD_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    lh = LineHeight(THEME_FONT_TEXT);
    top = rc.top + (rc.bottom - rc.top - 2 * lh) / 2;
    SetRect(&line, rc.left + v.pad * 2 + v.iconSize, top, rc.right - v.pad, top + lh);
    TextLine(dc, THEME_FONT_STRONG, text, ProfileAt(p)->name, &line, 0);
    n = SessionCount(p);
    StringCchPrintfW(second, ARRAYSIZE(second), L"%s%d session%s", ProfileAt(p)->running ? L"open \x00B7 " : L"", n,
                     n == 1 ? L"" : L"s");
    OffsetRect(&line, 0, lh);
    TextLine(dc, THEME_FONT_TEXT, Theme_RowMuted(state), second, &line, 0);
    Theme_BufferEnd(&buffer);
}

/* ------------------------------------------------------------------ tree */

static BOOL WasCollapsed(const WCHAR *key)
{
    int i;
    for (i = 0; i < v.collapsedCount; i++)
        if (CompareStringOrdinal(v.collapsed[i], -1, key, -1, TRUE) == CSTR_EQUAL) return TRUE;
    return FALSE;
}

static void SetCollapsed(const WCHAR *key, BOOL collapsed)
{
    int i;
    for (i = 0; i < v.collapsedCount; i++) {
        if (CompareStringOrdinal(v.collapsed[i], -1, key, -1, TRUE) != CSTR_EQUAL) continue;
        if (!collapsed) {
            v.collapsed[i][0] = 0;
            if (i < v.collapsedCount - 1) StringCchCopyW(v.collapsed[i], MAX_PATH, v.collapsed[v.collapsedCount - 1]);
            v.collapsedCount--;
        }
        return;
    }
    if (collapsed && v.collapsedCount < MAX_COLLAPSED) StringCchCopyW(v.collapsed[v.collapsedCount++], MAX_PATH, key);
}

#define NODE_NONE (NODE_FAVORITES - 1000000)

static LPARAM NodeParam(HTREEITEM item)
{
    TVITEMW tv;
    ZeroMemory(&tv, sizeof tv);
    tv.mask = TVIF_PARAM;
    tv.hItem = item;
    return item && TreeView_GetItem(v.tree, &tv) ? tv.lParam : NODE_NONE;
}

/* The row on top of the tree, to put back there after a reload. */
static void RememberFirstVisible(void)
{
    HTREEITEM first = TreeView_GetFirstVisible(v.tree);
    LPARAM node = NodeParam(first);
    v.firstKey[0] = 0;
    if (!first || node == NODE_NONE) return;
    v.firstIsFolder = node < 0;
    v.firstInFavorites = node == NODE_FAVORITES || NodeParam(TreeView_GetParent(v.tree, first)) == NODE_FAVORITES;
    if (node >= 0 && node < v.set.rowCount) StringCchCopyW(v.firstKey, ARRAYSIZE(v.firstKey), v.set.rows[node].key);
    else if (node < 0 && NodeGroup(node) < v.set.groupCount) StringCchCopyW(v.firstKey, ARRAYSIZE(v.firstKey), GroupKey(NodeGroup(node)));
}

static BOOL WasFirst(LPARAM node, BOOL inFavorites)
{
    if (!v.firstKey[0] || (node < 0) != v.firstIsFolder || inFavorites != v.firstInFavorites) return FALSE;
    return CompareStringOrdinal(node < 0 ? GroupKey(NodeGroup(node)) : v.set.rows[node].key, -1, v.firstKey, -1, TRUE) == CSTR_EQUAL;
}

static HTREEITEM AddNode(HTREEITEM parent, const WCHAR *text, LPARAM param)
{
    TVINSERTSTRUCTW ins;
    ZeroMemory(&ins, sizeof ins);
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.itemex.mask = TVIF_TEXT | TVIF_PARAM | TVIF_INTEGRAL;
    ins.itemex.pszText = (LPWSTR)text;
    ins.itemex.lParam = param;
    ins.itemex.iIntegral = param < 0 ? FOLDER_UNITS + (TreeView_GetCount(v.tree) > 0 ? GAP_UNITS : 0) : SESSION_UNITS;
    return TreeView_InsertItem(v.tree, &ins);
}

static void ExpandUnlessFolded(HTREEITEM node, const WCHAR *key)
{
    if (node && !WasCollapsed(key)) TreeView_Expand(v.tree, node, TVE_EXPAND);
}

/* The sessions a folder node holds, as shown; NODE_FAVORITES: the favorites. */
static int ShownIn(LPARAM node)
{
    int r, n = 0, p = ProfileIndex();
    for (r = 0; r < v.set.rowCount; r++) {
        const SessionRow *row = &v.set.rows[r];
        if (Shown(row, p) && (node == NODE_FAVORITES ? StarredIn(EntryOf(row, p)) : row->group == NodeGroup(node))) n++;
    }
    return n;
}

static void FillTree(void)
{
    HTREEITEM node = NULL, pick = NULL, leaf, first = NULL;
    int p = ProfileIndex(), r, g = -1;
    v.filling = TRUE;
    SendMessageW(v.tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(v.tree);
    if (p >= 0) {
        if (ShownIn(NODE_FAVORITES)) {
            node = AddNode(TVI_ROOT, L"Favorites", NODE_FAVORITES);
            if (WasFirst(NODE_FAVORITES, TRUE)) first = node;
            for (r = 0; r < v.set.rowCount; r++) {
                const SessionRow *row = &v.set.rows[r];
                if (!Shown(row, p) || !StarredIn(EntryOf(row, p))) continue;
                leaf = AddNode(node, TitleIn(row, p), r);
                if (!pick && CompareStringOrdinal(row->key, -1, v.selectedKey, -1, TRUE) == CSTR_EQUAL) pick = leaf;
                if (WasFirst(r, TRUE)) first = leaf;
            }
            ExpandUnlessFolded(node, FAVORITES_KEY);
            node = NULL;
        }
        for (r = 0; r < v.set.rowCount; r++) {
            const SessionRow *row = &v.set.rows[r];
            if (!Shown(row, p)) continue;
            if (row->group != g) {
                ExpandUnlessFolded(node, g >= 0 ? GroupKey(g) : L"");
                g = row->group;
                node = AddNode(TVI_ROOT, v.set.groups[g].name, -2 - g);
                if (WasFirst(-2 - g, FALSE)) first = node;
            }
            leaf = AddNode(node, TitleIn(row, p), r);
            /* The project's row rather than the favorites' copy. */
            if (CompareStringOrdinal(row->key, -1, v.selectedKey, -1, TRUE) == CSTR_EQUAL) pick = leaf;
            if (WasFirst(r, FALSE)) first = leaf;
        }
        ExpandUnlessFolded(node, g >= 0 ? GroupKey(g) : L"");
    }
    /* The session selected before, else the first one listed: the details
     * always show a session while there is one. */
    if (!pick && (pick = TreeView_GetChild(v.tree, TreeView_GetRoot(v.tree))) != NULL) {
        LPARAM param = NodeParam(pick);
        if (param >= 0 && param < v.set.rowCount) StringCchCopyW(v.selectedKey, ARRAYSIZE(v.selectedKey), v.set.rows[param].key);
    }
    if (pick) TreeView_SelectItem(v.tree, pick);
    else v.selectedKey[0] = 0;
    /* The same place after a reload; else the selection in view. */
    if (first) TreeView_SelectSetFirstVisible(v.tree, first);
    else if (pick) TreeView_EnsureVisible(v.tree, pick);
    v.firstKey[0] = 0;
    SendMessageW(v.tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v.tree, NULL, TRUE);
    v.filling = FALSE;
}

/* The expand arrow of a folder, from the tree's theme. */
static void DrawGlyph(HDC dc, const RECT *cell, BOOL open, BOOL hot, COLORREF color)
{
    HTHEME theme = OpenThemeData(v.tree, L"TreeView");
    if (theme) {
        int part = hot ? TVP_HOTGLYPH : TVP_GLYPH, state = open ? GLPS_OPENED : GLPS_CLOSED;
        SIZE size = { v.indent / 2, v.indent / 2 };
        RECT box;
        GetThemePartSize(theme, dc, part, state, NULL, TS_DRAW, &size);
        box.left = cell->left + (cell->right - cell->left - size.cx) / 2;
        box.top = cell->top + (cell->bottom - cell->top - size.cy) / 2;
        box.right = box.left + size.cx;
        box.bottom = box.top + size.cy;
        DrawThemeBackground(theme, dc, part, state, &box, NULL);
        CloseThemeData(theme);
    } else {
        RECT r = *cell;
        SetTextColor(dc, color);
        DrawTextW(dc, open ? L"\x25BE" : L"\x25B8", -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }
}

/* One row of the tree, in the theme's rows: folders in the heading font
 * with their count in grey, sessions in the text font; in grey a session
 * with no conversation on disk, archived, or with a change waiting. */
static void DrawTreeItem(const NMTVCUSTOMDRAW *cd)
{
    WCHAR count[16];
    HDC dc = cd->nmcd.hdc;
    HTREEITEM item = (HTREEITEM)cd->nmcd.dwItemSpec;
    LPARAM node = cd->nmcd.lItemlParam;
    UINT tv = TreeView_GetItemState(v.tree, item, TVIS_SELECTED | TVIS_EXPANDED), state = 0;
    BOOL hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
    COLORREF text, muted, field = Theme_Color(THEME_FIELD);
    RECT rc = cd->nmcd.rc, cell;
    int p = ProfileIndex(), left, right, baseline;

    if (tv & TVIS_SELECTED) state |= THEME_ROW_SELECTED;
    if (hot) state |= THEME_ROW_HOT;
    /* A folder's room above it stays blank, out of its selection. */
    if (node < 0 && rc.bottom - rc.top > FOLDER_UNITS * v.unit) {
        RECT room = rc;
        room.bottom = rc.top = rc.bottom - FOLDER_UNITS * v.unit;
        Theme_DrawRow(dc, &room, 0, field);
    }
    text = Theme_DrawRow(dc, &rc, state, field);
    muted = Theme_RowMuted(state);
    SetBkMode(dc, TRANSPARENT);
    cell = rc;
    cell.left = rc.left + cd->iLevel * v.indent;
    cell.right = cell.left + v.indent;
    left = cell.right + v.pad / 2;
    right = rc.right - v.pad;

    if (node < 0) {
        const WCHAR *name = node == NODE_FAVORITES ? L"\x2605  Favorites" : v.set.groups[NodeGroup(node)].name;
        DrawGlyph(dc, &cell, (tv & TVIS_EXPANDED) != 0, hot, muted);
        StringCchPrintfW(count, ARRAYSIZE(count), L"%d", ShownIn(node));
        baseline = CenteredBaseline(dc, &rc, THEME_FONT_HEADING);
        left = TextAt(dc, THEME_FONT_HEADING, text, name, left, right - TextWidth(dc, THEME_FONT_TEXT, count) - v.pad, baseline);
        TextAt(dc, THEME_FONT_TEXT, muted, count, left + v.pad, right, baseline);
    } else if (node < v.set.rowCount) {
        const SessionRow *row = &v.set.rows[node];
        const SessionEntry *e = EntryOf(row, p);
        const WCHAR *suffix = e && e->pending ? L"  \x00B7  waiting" : e && e->archived ? L"  \x00B7  archived" : L"";
        baseline = CenteredBaseline(dc, &rc, THEME_FONT_TEXT);
        left = TextAt(dc, THEME_FONT_TEXT, !row->transcript || (e && (e->archived || e->pendingRemove)) ? muted : text,
                      TitleIn(row, p), left, right - TextWidth(dc, THEME_FONT_TEXT, suffix), baseline);
        if (suffix[0]) TextAt(dc, THEME_FONT_TEXT, muted, suffix, left, right, baseline);
    }
}

/* Why the tree is empty. */
static void EmptyText(WCHAR *out, size_t cch)
{
    int p = ProfileIndex(), r, listed = 0, archived = 0;
    const SessionSource *source;
    const WCHAR *name;
    if (p < 0) {
        StringCchCopyW(out, cch, L"No profile yet.");
        return;
    }
    source = &v.set.source[p];
    name = ProfileAt(p)->name;
    for (r = 0; r < v.set.rowCount; r++) {
        const SessionEntry *e = EntryOf(&v.set.rows[r], p);
        if (!e) continue;
        listed++;
        if (e->archived) archived++;
    }
    if (listed && v.filter[0])
        StringCchPrintfW(out, cch, L"No session of \x201C%s\x201D matches \x201C%s\x201D.", name, v.filter);
    else if (listed && archived == listed && !v.showArchived)
        StringCchPrintfW(out, cch, L"All the sessions of \x201C%s\x201D are archived.\nCheck Show archived to see them.", name);
    else if (source->elsewhere)
        StringCchPrintfW(out, cch, L"\x201C%s\x201D only has sessions run over SSH, in WSL or in the cloud.\n"
                                   L"Their conversation is not on this PC, so they are not listed.", name);
    else if (source->unreadable)
        StringCchPrintfW(out, cch, L"The sessions of \x201C%s\x201D could not be read.", name);
    else if (!source->signedIn)
        StringCchPrintfW(out, cch, L"\x201C%s\x201D is not signed in to Claude yet.\n"
                                   L"Once it is, the sessions started in its Code tab show here.", name);
    else
        StringCchPrintfW(out, cch, L"\x201C%s\x201D has no Claude Code sessions yet.\n"
                                   L"The sessions started in its Code tab show here.", name);
}

static void DrawEmptyTree(HDC dc)
{
    WCHAR text[512];
    RECT rc;
    HGDIOBJ old = SelectObject(dc, Font(THEME_FONT_TEXT));
    GetClientRect(v.tree, &rc);
    InflateRect(&rc, -v.pad * 3, -v.pad * 3);
    EmptyText(text, ARRAYSIZE(text));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Theme_Color(THEME_MUTED));
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

static LRESULT TreeCustomDraw(LPARAM lp)
{
    const NMTVCUSTOMDRAW *cd = (const NMTVCUSTOMDRAW *)lp;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT:
        /* The tree also asks with no rectangle, on a screen DC, only to learn
         * the font it measures a label with (hover, tooltips): nothing is
         * drawn then. */
        if (IsRectEmpty(&cd->nmcd.rc)) {
            SelectObject(cd->nmcd.hdc, Font(cd->nmcd.lItemlParam < 0 ? THEME_FONT_HEADING : THEME_FONT_TEXT));
            return CDRF_NEWFONT;
        }
        DrawTreeItem(cd);
        return CDRF_SKIPDEFAULT;
    case CDDS_POSTPAINT:
        if (!TreeView_GetCount(v.tree)) DrawEmptyTree(cd->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* --------------------------------------------------------------- details */

/* What the shown profile's sessions lack: sessions not listed, or no
 * conversation on disk at all. */
static int Notes(WCHAR notes[][320], int max)
{
    WCHAR projects[MAX_PATH];
    int p = ProfileIndex(), n = 0;
    const SessionSource *source = p >= 0 ? &v.set.source[p] : NULL;
    if (v.set.noTranscripts && n < max && SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)))
        StringCchPrintfW(notes[n++], 320, L"Claude Code's folder %s was not found: no session has its conversation on this PC.",
                         projects);
    if (source && source->elsewhere && n < max)
        StringCchPrintfW(notes[n++], 320, source->elsewhere == 1
                             ? L"%d session run over SSH, in WSL or in the cloud is not listed: its conversation is not on this PC."
                             : L"%d sessions run over SSH, in WSL or in the cloud are not listed: their conversation is not on this PC.",
                         source->elsewhere);
    if (source && source->unreadable && n < max)
        StringCchPrintfW(notes[n++], 320, source->unreadable == 1
                             ? L"%d session entry could not be read: that session may be missing here."
                             : L"%d session entries could not be read: those sessions may be missing here.",
                         source->unreadable);
    return n;
}

/* The profiles of `bits` by name: "Personal", "Personal and Work",
 * "Personal, Work and Test". */
static void NamesOf(DWORD bits, WCHAR *out, size_t cch)
{
    int p, total = BitCount(bits), seen = 0;
    out[0] = 0;
    for (p = 0; p < v.set.profiles.count; p++) {
        if (!(bits & (1u << p))) continue;
        if (seen) StringCchCatW(out, cch, seen == total - 1 ? L" and " : L", ");
        StringCchCatW(out, cch, ProfileAt(p)->name);
        seen++;
    }
}

/* The details' controls: as high as a button; the Actions boxes all as
 * wide, down the right edge. */
static int ControlHeight(void)
{
    return FontHeight(DialogFont()) + v.pad;
}

static int ControlWidth(HDC dc)
{
    return WidthOf(dc, DialogFont(), L"Actions") + 2 * v.pad + GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(v.dlg));
}

/* The place of a control at the right end of `line`, in its middle. */
static RECT ControlAt(HDC dc, const RECT *line)
{
    RECT control;
    int h = ControlHeight();
    SetRect(&control, line->right - ControlWidth(dc), line->top + (line->bottom - line->top - h) / 2, line->right, 0);
    control.bottom = control.top + h;
    return control;
}

static ChipSet *ChipsOf(HWND h)
{
    return h == v.parts ? &v.scrolled : &v.fixed;
}

/* The details and their part that scrolls, drawn again. */
static void RedrawDetails(void)
{
    RedrawWindow(v.details, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

/* A control of the details drawn in `owner` at `rc`: a button, or
 * (ACT_MENU) the Actions box of `profile`, drawn pressed while its menu
 * shows; the folder's button (ACT_SHOW_FOLDER) shows its path on its left.
 * One scrolled out of `owner` is not there to click. */
static void AddChip(HWND owner, HDC dc, Action action, int profile, const WCHAR *text, const RECT *rc)
{
    ChipSet *set = ChipsOf(owner);
    RECT client;
    int i = set->count;
    UINT state = 0;
    GetClientRect(owner, &client);
    if (i >= MAX_CHIPS || !IntersectRect(&set->chip[i].rc, rc, &client)) return;
    set->chip[i].action = action;
    set->chip[i].profile = profile;
    if ((owner == v.openIn && i == v.openChip) || (owner == v.hotIn && i == v.hotChip && owner == v.pressedIn && i == v.pressedChip))
        state = THEME_BUTTON_PRESSED;
    else if (owner == v.hotIn && i == v.hotChip)
        state = THEME_BUTTON_HOT;
    if (action == ACT_MENU) {
        Theme_DrawDropDown(owner, dc, rc, text, DialogFont(), state);
    } else if (action == ACT_SHOW_FOLDER) {
        RECT label = *rc;
        InflateRect(&label, -v.pad - v.pad / 2, 0);
        Theme_DrawButton(owner, dc, rc, L"", DialogFont(), state, DT_SINGLELINE);
        TextLine(dc, THEME_FONT_TEXT, GetTextColor(dc), text, &label, DT_PATH_ELLIPSIS);   /* in the color the button's label takes */
    } else {
        Theme_DrawButton(owner, dc, rc, text, DialogFont(), state, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    set->count++;
}

/* A line between two parts of the details, with room above and below;
 * returns where the next part starts. */
static int Separator(HDC dc, int left, int right, int y)
{
    RECT line;
    SetRect(&line, left, y + v.pad, right, y + v.pad + 1);
    SetDCBrushColor(dc, Theme_Color(THEME_FIELD));
    FillRect(dc, &line, (HBRUSH)GetStockObject(DC_BRUSH));
    return line.bottom + v.pad;
}

/* One profile's part of the details. On its first line its badge, its name
 * (underlined for the profile shown, struck out where the session is not
 * listed), a star where it is a favorite, and its Actions box; below, its
 * title there when it is not the one the tree shows, what is going on there
 * and the changes waiting. */
static int DrawProfilePart(HDC dc, const SessionRow *row, int p, int left, int right, int y)
{
    WCHAR text[LABEL_CCH + 64];
    const SessionEntry *e = EntryOf(row, p);
    const Profile *pr = ProfileAt(p);
    ThemeFont role = !e ? THEME_FONT_ABSENT : p == ProfileIndex() ? THEME_FONT_CURRENT : THEME_FONT_STRONG;
    COLORREF color = Theme_Color(THEME_TEXT), muted = Theme_Color(THEME_MUTED);
    RECT line = { left, y, right, y + max(ControlHeight(), LineHeight(THEME_FONT_STRONG)) }, box = ControlAt(dc, &line);
    int inner = left + v.dotSize + v.pad, star = StarredIn(e) ? TextWidth(dc, THEME_FONT_TEXT, L" \x2605") : 0, baseline, end;
    const WCHAR *title;

    if (v.dots) ImageList_Draw(v.dots, p, dc, left, line.top + (line.bottom - line.top - v.dotSize) / 2, ILD_NORMAL);
    baseline = CenteredBaseline(dc, &line, role);
    end = TextAt(dc, role, e ? color : muted, pr->name, inner, box.left - v.pad - star, baseline);
    if (star) TextAt(dc, THEME_FONT_TEXT, color, L" \x2605", end, box.left - v.pad, baseline);
    AddChip(v.parts, dc, ACT_MENU, p, L"Actions", &box);
    y = line.bottom;
    if (!e) return y;
    title = TitleIn(row, p);
    if (p != ProfileIndex() && CompareStringOrdinal(title, -1, TitleIn(row, ProfileIndex()), -1, FALSE) != CSTR_EQUAL)
        y = Paragraph(dc, THEME_FONT_ITALIC, e->archived || e->pendingRemove ? muted : color, title, inner, right, y, 2);
    text[0] = 0;
    if (pr->running) StringCchCatW(text, ARRAYSIZE(text), L"open");
    if (row->live & (1u << p)) StringCchCatW(text, ARRAYSIZE(text), text[0] ? L" \x00B7 in use" : L"in use");
    if (e->archived) StringCchCatW(text, ARRAYSIZE(text), text[0] ? L" \x00B7 archived" : L"archived");
    if (text[0]) y = Paragraph(dc, THEME_FONT_TEXT, muted, text, inner, right, y, 1);
    if (e->pending) {
        StringCchPrintfW(text, ARRAYSIZE(text), e->pendingRemove ? L"Removed when %s closes" : L"Changes made when %s closes", pr->name);
        y = Paragraph(dc, THEME_FONT_TEXT, muted, text, inner, right, y, 2);
    }
    return y;
}

/* How far the profiles' parts scroll: `content` px to show in `view` px. The
 * scroll bar shows only when they do not fit; FALSE when the place shown
 * moved. */
static BOOL SetDetailsScroll(int content, int view)
{
    SCROLLINFO si;
    int last = max(0, content - view);
    BOOL had = (GetWindowLongW(v.parts, GWL_STYLE) & WS_VSCROLL) != 0;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMax = content > view ? content - 1 : 0;
    si.nPage = content > view ? (UINT)view : 0;
    si.nPos = min(v.scroll, last);
    SetScrollInfo(v.parts, SB_VERT, &si, TRUE);
    if (si.nPos == v.scroll && had == ((GetWindowLongW(v.parts, GWL_STYLE) & WS_VSCROLL) != 0)) return TRUE;
    v.scroll = si.nPos;
    return FALSE;
}

/* The profiles' parts scrolled by a scroll bar action (WM_VSCROLL), or the
 * smooth wheel's SB_THUMBPOSITION. */
static void ScrollDetails(WPARAM wp)
{
    SCROLLINFO si;
    int pos, line = LineHeight(THEME_FONT_TEXT);
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(v.parts, SB_VERT, &si)) return;
    switch (LOWORD(wp)) {
    case SB_LINEUP:        pos = si.nPos - line; break;
    case SB_LINEDOWN:      pos = si.nPos + line; break;
    case SB_PAGEUP:        pos = si.nPos - (int)si.nPage; break;
    case SB_PAGEDOWN:      pos = si.nPos + (int)si.nPage; break;
    case SB_THUMBTRACK:    pos = si.nTrackPos; break;
    case SB_THUMBPOSITION: pos = HIWORD(wp); break;
    case SB_TOP:           pos = 0; break;
    case SB_BOTTOM:        pos = si.nMax; break;
    default:               return;
    }
    pos = max(0, min(pos, si.nMax - (int)si.nPage + 1));
    if (pos == v.scroll) return;
    v.scroll = pos;
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(v.parts, SB_VERT, &si, TRUE);
    if (v.hotIn == v.parts) v.hotChip = -1;   /* the controls moved under the mouse */
    InvalidateRect(v.parts, NULL, FALSE);
}

/* The selected session's details that stay: from the tree's top (the
 * search box's row above stays empty), its folder's path (a button that
 * shows the folder), when it was last used, how big its conversation is, a
 * warning when it is open in two profiles at once; at the bottom, Delete
 * session everywhere. Between their two lines, the part that scrolls
 * (DrawParts) is placed. With no session selected: why, and the notes. */
static void DrawDetails(const DRAWITEMSTRUCT *di)
{
    WCHAR text[MAX_PATH * 2], date[64], time[32], names[160], notes[3][320];
    ThemeBuffer buffer;
    RECT rc = di->rcItem, line, remove, tree = ChildRect(v.treeArea), self = ChildRect(v.details), was;
    int r = SelectedRow(), n, i, left = rc.left, right = rc.right, y = rc.top, top, bottom;
    const SessionRow *row = r >= 0 ? &v.set.rows[r] : NULL;
    HDC dc = Theme_BufferBegin(&buffer, di->hDC, &rc);
    COLORREF color = Theme_Color(THEME_TEXT), muted = Theme_Color(THEME_MUTED);

    v.fixed.count = 0;
    FillRect(dc, &rc, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    if (!row) {
        ShowWindow(v.parts, SW_HIDE);
        if (TreeView_GetCount(v.tree)) y = Paragraph(dc, THEME_FONT_TEXT, muted, L"Select a session to see it in each profile.", left, right, y, 3);
        n = Notes(notes, ARRAYSIZE(notes));
        for (i = 0; i < n; i++) y = Paragraph(dc, THEME_FONT_TEXT, muted, notes[i], left, right, y + v.pad, 4);
        Theme_BufferEnd(&buffer);
        return;
    }
    y = rc.top + tree.top - self.top;
    if (row->cwd[0]) {
        SetRect(&line, left, y, right, y + ControlHeight());
        if (Util_DirExists(row->cwd)) {
            line.right = min(right, left + TextWidth(dc, THEME_FONT_TEXT, row->cwd) + 3 * v.pad);
            AddChip(v.details, dc, ACT_SHOW_FOLDER, -1, row->cwd, &line);
        } else {
            TextLine(dc, THEME_FONT_TEXT, muted, row->cwd, &line, DT_PATH_ELLIPSIS);   /* gone: nothing to show */
        }
        y = line.bottom + v.pad / 2;
    }
    if (FormatWhen(row->lastActivity, date, ARRAYSIZE(date), time, ARRAYSIZE(time))) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s  -  %s", date, time);
        y = Paragraph(dc, THEME_FONT_TEXT, muted, text, left, right, y, 1);
    }
    if (row->transcript) FormatSize(row->transcriptBytes, text, ARRAYSIZE(text));
    else StringCchCopyW(text, ARRAYSIZE(text), L"No conversation on disk");
    y = Paragraph(dc, THEME_FONT_TEXT, muted, text, left, right, y, 1);
    if (BitCount(row->live) >= 2) {
        NamesOf(row->live, names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"Open in %s at once: go on in one of them only. The other one keeps an older copy until the session is closed there.",
                         names);
        y = Paragraph(dc, THEME_FONT_STRONG, color, text, left, right, y + v.pad, 6);
    }
    top = Separator(dc, left, right, y) - v.pad;   /* under its line */
    SetRect(&remove, left, rc.bottom - ControlHeight(), right, rc.bottom);
    bottom = remove.top - v.pad - 1;              /* over the bottom line */
    Separator(dc, left, right, bottom - v.pad);
    AddChip(v.details, dc, ACT_DELETE_ALL, -1, L"Delete session everywhere\x2026", &remove);
    Theme_BufferEnd(&buffer);

    /* The part that scrolls, between the two lines. */
    GetWindowRect(v.parts, &was);
    MapWindowPoints(NULL, v.details, (POINT *)&was, 2);
    if (was.left != left || was.top != top || was.right != right || was.bottom != max(top, bottom) || !IsWindowVisible(v.parts))
        SetWindowPos(v.parts, NULL, left, top, right - left, max(0, bottom - top), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* Each profile's part of the selected session, then the notes: what scrolls
 * between the details' two lines. */
static void DrawParts(const DRAWITEMSTRUCT *di)
{
    WCHAR notes[3][320];
    ThemeBuffer buffer;
    RECT rc = di->rcItem;
    int r = SelectedRow(), p, n, i, left = rc.left, right = rc.right, top = rc.top - v.scroll, y = top + v.pad + v.pad * 2 / 3;
    const SessionRow *row = r >= 0 ? &v.set.rows[r] : NULL;
    HDC dc = Theme_BufferBegin(&buffer, di->hDC, &rc);

    v.scrolled.count = 0;
    FillRect(dc, &rc, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    if (row)
        for (p = 0; p < v.set.profiles.count; p++) y = DrawProfilePart(dc, row, p, left, right, p == 0 ? y : Separator(dc, left, right, y));
    n = Notes(notes, ARRAYSIZE(notes));
    for (i = 0; i < n; i++) y = Paragraph(dc, THEME_FONT_TEXT, Theme_Color(THEME_MUTED), notes[i], left, right, y + v.pad, 4);
    Theme_BufferEnd(&buffer);
    /* Drawn again when the scroll bar comes or goes, or the place shown
     * moved (the content got shorter): the width or the offset changed. */
    if (!SetDetailsScroll(y - top + v.pad, rc.bottom - rc.top)) InvalidateRect(v.parts, NULL, FALSE);
}

static int ChipAt(HWND h, POINT pt)
{
    const ChipSet *set = ChipsOf(h);
    int i;
    for (i = 0; i < set->count; i++)
        if (PtInRect(&set->chip[i].rc, pt)) return i;
    return -1;
}

static void RunAction(Action action, int profile);
static void ProfileMenu(int chip);

/* The controls of the details and of their part that scrolls: under the
 * mouse, pressed, clicked; an Actions box opens its menu as soon as it is
 * pressed, as a drop-down list does. The details also draw that part. */
static LRESULT CALLBACK DetailsSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    POINT pt;
    int chip;
    (void)ref;
    switch (msg) {
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT *)lp)->hwndItem == v.parts) {
            DrawParts((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if ((chip = ChipAt(h, pt)) != v.hotChip || v.hotIn != h) {
            TRACKMOUSEEVENT track = { sizeof track, TME_LEAVE, h, 0 };
            if (v.hotIn && v.hotIn != h) InvalidateRect(v.hotIn, NULL, FALSE);
            v.hotIn = h;
            v.hotChip = chip;
            TrackMouseEvent(&track);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (v.hotIn == h && v.hotChip >= 0 && GetCapture() != h) {
            v.hotChip = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if ((chip = ChipAt(h, pt)) >= 0 && h == v.parts && v.scrolled.chip[chip].action == ACT_MENU) {
            ProfileMenu(chip);
        } else if (chip >= 0) {
            v.pressedIn = h;
            v.pressedChip = chip;
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        chip = v.pressedIn == h ? v.pressedChip : -1;
        v.pressedChip = -1;
        if (GetCapture() == h) ReleaseCapture();
        InvalidateRect(h, NULL, FALSE);
        if (chip >= 0 && ChipAt(h, pt) == chip) {
            Chip c = ChipsOf(h)->chip[chip];
            v.hotChip = -1;
            RunAction(c.action, c.profile);
        }
        return 0;
    case WM_VSCROLL:
        if (h == v.parts) ScrollDetails(wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* drawn whole, off screen */
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, DetailsSubclass, id);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* --------------------------------------------------------------- actions */

typedef struct TitleDialog {
    const WCHAR *profile;
    WCHAR        title[SESSION_TITLE_CCH];
} TitleDialog;

static INT_PTR CALLBACK TitleProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    TitleDialog *t = (TitleDialog *)GetWindowLongPtrW(d, DWLP_USER);
    WCHAR label[128];
    switch (msg) {
    case WM_INITDIALOG:
        t = (TitleDialog *)lp;
        SetWindowLongPtrW(d, DWLP_USER, lp);
        StringCchPrintfW(label, ARRAYSIZE(label), L"Title in \x201C%s\x201D:", t->profile);
        SetDlgItemTextW(d, IDC_T_LABEL, label);
        SendDlgItemMessageW(d, IDC_T_TITLE, EM_LIMITTEXT, SESSION_TITLE_CCH - 1, 0);
        SetDlgItemTextW(d, IDC_T_TITLE, t->title);
        SendDlgItemMessageW(d, IDC_T_TITLE, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(d, IDC_T_TITLE));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_T_TITLE && HIWORD(wp) == EN_CHANGE) {
            EnableWindow(GetDlgItem(d, IDOK), GetWindowTextLengthW(GetDlgItem(d, IDC_T_TITLE)) > 0);
        } else if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            if (LOWORD(wp) == IDOK) {
                WCHAR *s, *e;
                GetDlgItemTextW(d, IDC_T_TITLE, t->title, ARRAYSIZE(t->title));
                for (s = t->title; *s == L' ' || *s == L'\t'; s++) {}
                for (e = s + wcslen(s); e > s && (e[-1] == L' ' || e[-1] == L'\t'); e--) {}
                *e = 0;
                memmove(t->title, s, ((size_t)(e - s) + 1) * sizeof(WCHAR));
                if (!t->title[0]) return TRUE;
            }
            EndDialog(d, LOWORD(wp));
        }
        return TRUE;
    }
    return FALSE;
}

/* "Work opens it now" with what happens first when it is closed. */
static void OpensNow(int p, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, ProfileAt(p)->running ? L"%s opens it now" : L"%s starts and opens it", ProfileAt(p)->name);
}

static BOOL Ready(const SessionRow *row, int target)
{
    if (!row->transcript || !Core_IsSessionId(row->key)) {
        Util_Message(v.dlg, MB_ICONINFORMATION, L"This session has no conversation on disk yet: open it in the profile where it was started.");
        return FALSE;
    }
    if (target >= 0 && !v.set.source[target].signedIn) {
        Util_Message(v.dlg, MB_ICONINFORMATION, L"\x201C%s\x201D is not signed in to Claude yet: sign in there first.", ProfileAt(target)->name);
        return FALSE;
    }
    if (!v.pkg.found) {
        Util_Message(v.dlg, MB_ICONWARNING, L"Claude Desktop is not installed.");
        return FALSE;
    }
    return TRUE;
}

static void Open(int r, int p)
{
    HRESULT hr;
    if (!Ready(&v.set.rows[r], -1)) return;
    hr = SessionEdit_Open(&v.pkg, ProfileAt(p), v.set.rows[r].key);
    if (FAILED(hr)) Util_Message(v.dlg, MB_ICONERROR, L"Claude could not be started (error 0x%08lX).", (unsigned long)hr);
}

/* The title the session keeps in `target`, when Claude has added it there. */
static void CarryTitle(int target, const WCHAR *key, const WCHAR *title)
{
    PendingEdit edit;
    BOOL waiting;
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), key);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), title);
    SessionEdit_Change(ProfileAt(target), NULL, &edit, &waiting);
}

static void Share(int r, int target)
{
    const SessionRow *row = &v.set.rows[r];
    const WCHAR *title = TitleIn(row, ProfileIndex()), *folder = wcsrchr(row->cwd, L'\\');
    WCHAR text[1024], opens[128], names[160];
    HRESULT hr;
    if (!Ready(row, target)) return;
    OpensNow(target, opens, ARRAYSIZE(opens));
    if (IsScratch(row))
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"\x201C%s\x201D has no folder. Shared with \x201C%s\x201D, it shows there in a folder named %s, not under No folder, "
                         L"and both profiles go on with the same conversation.\n\nFor a separate copy under %s's No folder, use Copy instead.\n\n"
                         L"%s, and its title there is set when %s closes.",
                         title, ProfileAt(target)->name, folder ? folder + 1 : row->cwd, ProfileAt(target)->name, opens, ProfileAt(target)->name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"Share \x201C%s\x201D with \x201C%s\x201D?\n\nBoth profiles then go on with the same conversation. "
                         L"%s, and its title there is set when %s closes.",
                         title, ProfileAt(target)->name, opens, ProfileAt(target)->name);
    if (row->live) {
        NamesOf(row->live, names, ARRAYSIZE(names));
        StringCchCatW(text, ARRAYSIZE(text), L"\n\nIt is open in ");
        StringCchCatW(text, ARRAYSIZE(text), names);
        StringCchCatW(text, ARRAYSIZE(text), L" now: go on in one profile at a time. The other one keeps an older copy until the session is closed there.");
    }
    if (!Ui_Ask(v.dlg, IDI_QUESTION, text, L"Share", L"Cancel", FALSE)) return;
    CarryTitle(target, row->key, title);
    hr = SessionEdit_Open(&v.pkg, ProfileAt(target), row->key);
    if (FAILED(hr)) Util_Message(v.dlg, MB_ICONERROR, L"Claude could not be started (error 0x%08lX).", (unsigned long)hr);
}

static void Copy(int r, int target)
{
    const SessionRow *row = &v.set.rows[r];
    const WCHAR *title = TitleIn(row, ProfileIndex());
    WCHAR text[1024], opens[128], id[SESSION_ID_CCH], error[256];
    HRESULT hr;
    if (!Ready(row, target)) return;
    OpensNow(target, opens, ARRAYSIZE(opens));
    StringCchPrintfW(text, ARRAYSIZE(text),
                     L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation%s, which then goes on separately. "
                     L"%s, and its title there is set when %s closes.",
                     title, ProfileAt(target)->name, ProfileAt(target)->name,
                     IsScratch(row) ? L" and of its working folder, under its No folder" : L"", opens, ProfileAt(target)->name);
    if (!Ui_Ask(v.dlg, IDI_QUESTION, text, L"Copy", L"Cancel", FALSE)) return;
    if (!SessionEdit_CopyConversation(&v.set, r, target, id, ARRAYSIZE(id), error, ARRAYSIZE(error))) {
        Util_Message(v.dlg, MB_ICONERROR, L"The session could not be copied. %s", error);
        return;
    }
    CarryTitle(target, id, title);
    hr = SessionEdit_Open(&v.pkg, ProfileAt(target), id);
    if (FAILED(hr)) Util_Message(v.dlg, MB_ICONERROR, L"Claude could not be started (error 0x%08lX).", (unsigned long)hr);
}

/* A change to the entry of profile `p`, made now or when it closes. */
static void Change(int r, int p, PendingOp op, const WCHAR *value)
{
    PendingEdit edit;
    BOOL waiting = FALSE;
    ZeroMemory(&edit, sizeof edit);
    edit.op = op;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), v.set.rows[r].key);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), value);
    if (!SessionEdit_Change(ProfileAt(p), EntryOf(&v.set.rows[r], p), &edit, &waiting))
        Util_Message(v.dlg, MB_ICONERROR, L"The session could not be changed in \x201C%s\x201D.", ProfileAt(p)->name);
}

static void Rename(int r, int p)
{
    TitleDialog t;
    ZeroMemory(&t, sizeof t);
    t.profile = ProfileAt(p)->name;
    StringCchCopyW(t.title, ARRAYSIZE(t.title), TitleIn(&v.set.rows[r], p));
    if (Ui_Dialog(v.dlg, IDD_TITLE, TitleProc, (LPARAM)&t) == IDOK)
        Change(r, p, PENDING_TITLE, t.title);
}

static void Remove(int r, int p)
{
    const SessionRow *row = &v.set.rows[r];
    WCHAR text[1024], names[160], when[128];
    DWORD others = 0;
    int q;
    for (q = 0; q < v.set.profiles.count; q++)
        if (q != p && row->entry[q] >= 0) others |= 1u << q;
    NamesOf(others, names, ARRAYSIZE(names));
    when[0] = 0;
    if (ProfileAt(p)->running) StringCchPrintfW(when, ARRAYSIZE(when), L" when %s closes (it keeps it until then)", ProfileAt(p)->name);
    StringCchPrintfW(text, ARRAYSIZE(text), L"Remove \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry there goes to the Recycle Bin%s. %s%s%s",
                     TitleIn(row, p), ProfileAt(p)->name, when,
                     others ? L"The conversation stays for " : L"The conversation stays on disk: Delete session everywhere removes it too.",
                     others ? names : L"", others ? L"." : L"");
    if (Ui_Ask(v.dlg, IDI_QUESTION, text, L"Remove", L"Cancel", FALSE)) Change(r, p, PENDING_REMOVE, L"");
}

static void Keep(int r, int p)
{
    PendingEdit edit;
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_REMOVE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), v.set.rows[r].key);
    SessionEdit_Cancel(ProfileAt(p), &edit);
}

static void DeleteEverywhere(int r)
{
    const SessionRow *row = &v.set.rows[r];
    WCHAR text[1024], names[160], error[256];
    DWORD listed = 0;
    int p;
    for (p = 0; p < v.set.profiles.count; p++)
        if (row->entry[p] >= 0) listed |= 1u << p;
    NamesOf(listed, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text), L"Delete \x201C%s\x201D everywhere?\n\nIts entry in %s and its conversation go to the Recycle Bin.",
                     TitleIn(row, ProfileIndex()), names);
    if (!Ui_Ask(v.dlg, IDI_WARNING, text, L"Delete", L"Cancel", TRUE)) return;
    if (SessionEdit_DeleteEverywhere(v.dlg, &v.set, r, error, ARRAYSIZE(error)) == REMOVE_FAILED)
        Util_Message(v.dlg, MB_ICONWARNING, L"The session could not be deleted. %s", error);
}

static void RunAction(Action action, int p)
{
    int r = SelectedRow();
    if (r < 0) return;
    if (p < 0) p = ProfileIndex();
    if (p < 0 || p >= v.set.profiles.count) return;
    switch (action) {
    case ACT_OPEN:        Open(r, p); break;
    case ACT_RENAME:      Rename(r, p); break;
    case ACT_STAR:        Change(r, p, PENDING_STAR, StarredIn(EntryOf(&v.set.rows[r], p)) ? L"0" : L"1"); break;
    case ACT_REMOVE:      Remove(r, p); break;
    case ACT_KEEP:        Keep(r, p); break;
    case ACT_SHARE:       Share(r, p); break;
    case ACT_COPY:        Copy(r, p); break;
    case ACT_SHOW_FOLDER: ShellExecuteW(v.dlg, L"explore", v.set.rows[r].cwd, NULL, NULL, SW_SHOWNORMAL); break;
    case ACT_DELETE_ALL:  DeleteEverywhere(r); break;
    default: break;
    }
    Load(FALSE);
}

/* A profile's name for a menu (an & shown as itself). */
static void MenuName(const WCHAR *name, WCHAR *out, size_t cch)
{
    size_t n = 0;
    for (; *name && n + 2 < cch; name++) {
        if (*name == L'&') out[n++] = L'&';
        out[n++] = *name;
    }
    out[n] = 0;
}

static UINT MenuId(Action action, int p)
{
    return IDM_ACTION + (UINT)action * MAX_PROFILES + (UINT)p;
}

/* A menu's choice, run. */
static void RunCommand(UINT cmd)
{
    if (cmd >= IDM_ACTION && cmd < IDM_ACTION + ACTIONS * MAX_PROFILES)
        RunAction((Action)((cmd - IDM_ACTION) / MAX_PROFILES), (int)((cmd - IDM_ACTION) % MAX_PROFILES));
}

/* What can be done to the entry of session `row` in profile `p` (`name` as
 * a menu shows it): rename, favorite, remove (from that profile when others
 * list it too), or keep it when its removal waits. `keys`: with the keys
 * that do it in the tree. */
static void AppendEntryActions(HMENU menu, const SessionRow *row, int p, const WCHAR *name, BOOL keys)
{
    WCHAR text[LABEL_CCH * 2 + 48];
    const SessionEntry *e = EntryOf(row, p);
    int q, others = 0;
    for (q = 0; q < v.set.profiles.count; q++)
        if (q != p && row->entry[q] >= 0) others++;
    if (e->pendingRemove) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"&Keep in %s", name);
        AppendMenuW(menu, MF_STRING, MenuId(ACT_KEEP, p), text);
        return;
    }
    AppendMenuW(menu, MF_STRING, MenuId(ACT_RENAME, p), keys ? L"&Rename\x2026\tF2" : L"&Rename\x2026");
    AppendMenuW(menu, MF_STRING, MenuId(ACT_STAR, p), StarredIn(e) ? L"Unpin &favorites" : L"Add to &favorites");
    if (others) StringCchPrintfW(text, ARRAYSIZE(text), L"Re&move from %s\x2026%s", name, keys ? L"\tDel" : L"");
    else StringCchPrintfW(text, ARRAYSIZE(text), L"Re&move\x2026%s", keys ? L"\tDel" : L"");
    AppendMenuW(menu, MF_STRING, MenuId(ACT_REMOVE, p), text);
}

/* Takes out of the queue the mouse press that closed a menu, when it was on
 * `rc` of the details. */
static void DropClickOn(const RECT *rc)
{
    static const UINT kPresses[] = { WM_LBUTTONDOWN, WM_LBUTTONDBLCLK };
    MSG m;
    size_t i;
    for (i = 0; i < ARRAYSIZE(kPresses); i++) {
        POINT at;
        if (!PeekMessageW(&m, v.parts, kPresses[i], kPresses[i], PM_NOREMOVE)) continue;
        at.x = (short)LOWORD(m.lParam);
        at.y = (short)HIWORD(m.lParam);
        if (PtInRect(rc, at)) PeekMessageW(&m, v.parts, kPresses[i], kPresses[i], PM_REMOVE);
    }
}

/* The menu of a profile's Actions box, under the box, which shows pressed
 * meanwhile: open, rename, favorite and remove where the session is listed,
 * share and copy where it is not. */
static void ProfileMenu(int chip)
{
    WCHAR text[LABEL_CCH * 2 + 48], name[LABEL_CCH * 2];
    int r = SelectedRow(), p = v.scrolled.chip[chip].profile;
    RECT box = v.scrolled.chip[chip].rc;
    TPMPARAMS around;
    const SessionEntry *e;
    HMENU menu;
    UINT cmd;
    if (r < 0 || p < 0 || p >= v.set.profiles.count || (menu = CreatePopupMenu()) == NULL) return;
    e = EntryOf(&v.set.rows[r], p);
    MenuName(ProfileAt(p)->name, name, ARRAYSIZE(name));
    if (!e) {
        AppendMenuW(menu, MF_STRING, MenuId(ACT_SHARE, p), L"&Share with\x2026");
        AppendMenuW(menu, MF_STRING, MenuId(ACT_COPY, p), L"&Copy to\x2026");
    } else {
        if (!e->pendingRemove) {
            StringCchPrintfW(text, ARRAYSIZE(text), L"&Open in %s", name);
            AppendMenuW(menu, MF_STRING, MenuId(ACT_OPEN, p), text);
            SetMenuDefaultItem(menu, MenuId(ACT_OPEN, p), FALSE);
        }
        AppendEntryActions(menu, &v.set.rows[r], p, name, FALSE);
    }
    MapWindowPoints(v.parts, NULL, (POINT *)&box, 2);
    ZeroMemory(&around, sizeof around);
    around.cbSize = sizeof around;
    around.rcExclude = box;   /* above the box when there is no room below */
    v.openIn = v.parts;
    v.openChip = chip;
    RedrawWindow(v.parts, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_VERTICAL, box.right, box.bottom, v.dlg, &around);
    DestroyMenu(menu);
    DropClickOn(&v.scrolled.chip[chip].rc);   /* a click on the box closed the menu: it does not open it again */
    v.openChip = v.hotChip = -1;
    v.openIn = NULL;
    InvalidateRect(v.parts, NULL, FALSE);
    RunCommand(cmd);
}

/* The selected session's actions, at `pt` (screen). */
static void ShowMenu(POINT pt)
{
    WCHAR text[LABEL_CCH * 2 + 48], name[LABEL_CCH * 2];
    HMENU menu = CreatePopupMenu(), openIn = CreatePopupMenu(), shareWith = CreatePopupMenu(), copyTo = CreatePopupMenu();
    int r = SelectedRow(), p = ProfileIndex(), q, others = 0, unlisted = 0;
    const SessionRow *row;
    const SessionEntry *e;
    UINT cmd;
    if (!menu || !openIn || !shareWith || !copyTo || r < 0 || p < 0) {
        if (menu) DestroyMenu(menu);
        if (openIn) DestroyMenu(openIn);
        if (shareWith) DestroyMenu(shareWith);
        if (copyTo) DestroyMenu(copyTo);
        return;
    }
    row = &v.set.rows[r];
    e = EntryOf(row, p);
    MenuName(ProfileAt(p)->name, name, ARRAYSIZE(name));
    if (e && !e->pendingRemove) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"&Open in %s", name);
        AppendMenuW(menu, MF_STRING, MenuId(ACT_OPEN, p), text);
        SetMenuDefaultItem(menu, MenuId(ACT_OPEN, p), FALSE);
    }
    for (q = 0; q < v.set.profiles.count; q++) {
        if (q == p) continue;
        MenuName(ProfileAt(q)->name, text, ARRAYSIZE(text));
        if (row->entry[q] >= 0) {
            AppendMenuW(openIn, MF_STRING, MenuId(ACT_OPEN, q), text);
            others++;
        } else {
            AppendMenuW(shareWith, MF_STRING, MenuId(ACT_SHARE, q), text);
            unlisted++;
        }
        AppendMenuW(copyTo, MF_STRING, MenuId(ACT_COPY, q), text);
    }
    AppendMenuW(menu, MF_POPUP | (others ? 0 : MF_GRAYED), (UINT_PTR)openIn, L"Open i&n");
    AppendMenuW(menu, MF_POPUP | (unlisted ? 0 : MF_GRAYED), (UINT_PTR)shareWith, L"S&hare with");
    AppendMenuW(menu, MF_POPUP | (v.set.profiles.count > 1 ? 0 : MF_GRAYED), (UINT_PTR)copyTo, L"&Copy to");
    if (e) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendEntryActions(menu, row, p, name, TRUE);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (row->cwd[0] && Util_DirExists(row->cwd) ? 0 : MF_GRAYED), MenuId(ACT_SHOW_FOLDER, p), L"Show &folder");
    AppendMenuW(menu, MF_STRING, MenuId(ACT_DELETE_ALL, p), L"&Delete session everywhere\x2026");
    cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, v.dlg, NULL);
    DestroyMenu(menu);   /* its submenus with it */
    RunCommand(cmd);
}

/* --------------------------------------------------------------- watcher */

typedef struct WatchPlan {
    HWND   dlg;
    HANDLE stop;
    int    count;
    WCHAR  dirs[WATCH_MAX][MAX_PATH];
    BOOL   subtree[WATCH_MAX];
    DWORD  filter[WATCH_MAX];
} WatchPlan;

static DWORD WINAPI WatchProc(void *param)
{
    WatchPlan *plan = (WatchPlan *)param;
    HANDLE handles[WATCH_MAX + 1];
    DWORD n = 0, w;
    int i;
    handles[n++] = plan->stop;
    for (i = 0; i < plan->count; i++) {
        HANDLE h = FindFirstChangeNotificationW(plan->dirs[i], plan->subtree[i], plan->filter[i]);
        if (h != INVALID_HANDLE_VALUE) handles[n++] = h;
    }
    for (;;) {
        w = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + n) break;
        FindNextChangeNotification(handles[w - WAIT_OBJECT_0]);
        if (InterlockedExchange(&g_pending, 1) == 0) PostMessageW(plan->dlg, WM_APP_SESSIONS, 0, 0);
    }
    for (w = 1; w < n; w++) FindCloseChangeNotification(handles[w]);
    HeapFree(GetProcessHeap(), 0, plan);
    return 0;
}

static void StopWatching(void)
{
    if (v.watchStop) SetEvent(v.watchStop);
    if (v.watchThread) {
        WaitForSingleObject(v.watchThread, 2000);
        CloseHandle(v.watchThread);
    }
    if (v.watchStop) CloseHandle(v.watchStop);
    v.watchThread = v.watchStop = NULL;
    v.watchedCount = 0;
}

static BOOL WatchingThese(void)
{
    int i;
    if (!v.watchThread || v.watchedCount != v.set.profiles.count) return FALSE;
    for (i = 0; i < v.watchedCount; i++)
        if (CompareStringOrdinal(v.watched[i], -1, ProfileAt(i)->folder, -1, TRUE) != CSTR_EQUAL) return FALSE;
    return TRUE;
}

/* Each profile's session entries (or its folder, until Claude makes them)
 * and the transcripts folder, where new and removed files count. */
static void Watch(void)
{
    WatchPlan *plan;
    int i;
    if (WatchingThese()) return;
    StopWatching();
    plan = (WatchPlan *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *plan);
    v.watchStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!plan || !v.watchStop) {
        if (plan) HeapFree(GetProcessHeap(), 0, plan);
        return;
    }
    plan->dlg = v.dlg;
    plan->stop = v.watchStop;
    for (i = 0; i < v.set.profiles.count; i++) {
        const Profile *p = ProfileAt(i);
        WCHAR *dir = plan->dirs[plan->count];
        if (SessionStore_SessionsDir(p, dir, MAX_PATH) && Util_DirExists(dir)) {
            plan->subtree[plan->count] = TRUE;
            plan->filter[plan->count] = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
        } else {
            StringCchCopyW(dir, MAX_PATH, p->dataDir);
            plan->filter[plan->count] = FILE_NOTIFY_CHANGE_DIR_NAME;
        }
        plan->count++;
        StringCchCopyW(v.watched[i], FOLDER_CCH, p->folder);
    }
    v.watchedCount = v.set.profiles.count;
    if (SessionStore_ProjectsDir(plan->dirs[plan->count], MAX_PATH)) {
        plan->subtree[plan->count] = TRUE;
        plan->filter[plan->count] = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
        plan->count++;
    }
    v.watchThread = CreateThread(NULL, 0, WatchProc, plan, 0, NULL);
    if (!v.watchThread) {
        HeapFree(GetProcessHeap(), 0, plan);
        CloseHandle(v.watchStop);
        v.watchStop = NULL;
        v.watchedCount = 0;
    }
}

/* ------------------------------------------------------------------- API */

void SessionsView_Init(HWND dlg)
{
    v.dlg = dlg;
    v.profiles = GetDlgItem(dlg, IDC_S_PROFILES);
    v.tree = GetDlgItem(dlg, IDC_S_TREE);
    v.search = GetDlgItem(dlg, IDC_S_SEARCH);
    v.archived = GetDlgItem(dlg, IDC_S_ARCHIVED);
    v.details = GetDlgItem(dlg, IDC_S_DETAILS);
    v.treeArea = Theme_SmoothView(v.tree);
    v.profilesArea = Theme_SmoothView(v.profiles);
    v.hotChip = v.pressedChip = v.openChip = -1;
    TreeView_SetExtendedStyle(v.tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    v.parts = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VSCROLL | SS_OWNERDRAW | SS_NOTIFY, 0, 0, 0, 0, v.details,
                              (HMENU)(INT_PTR)IDC_S_PARTS, g_hInst, NULL);
    SetWindowSubclass(v.details, DetailsSubclass, 1, 0);
    if (v.parts) SetWindowSubclass(v.parts, DetailsSubclass, 1, 0);
    SendMessageW(v.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search this profile's sessions");
    SendMessageW(v.search, EM_LIMITTEXT, ARRAYSIZE(v.filter) - 1, 0);
}

BOOL SessionsView_Shown(void)
{
    return v.shown;
}

const WCHAR *SessionsView_Profile(void)
{
    int p = ProfileIndex();
    return p >= 0 ? ProfileAt(p)->folder : v.folder;
}

/* Reads every profile's sessions again; a closed profile's waiting changes
 * are made first. When nothing the tree shows changed (and `force` is off),
 * only the side bar and the details are redrawn. */
static void Load(BOOL force)
{
    SessionSet fresh;
    ULONGLONG shape;
    BOOL made = FALSE;
    int p;
    InterlockedExchange(&g_pending, 0);
    SessionStore_Load(&fresh);
    for (p = 0; p < fresh.profiles.count; p++)
        if (fresh.source[p].pending && !fresh.profiles.items[p].running && SessionEdit_ApplyPending(&fresh.profiles.items[p]) > 0)
            made = TRUE;
    if (made) {
        SessionStore_Free(&fresh);
        SessionStore_Load(&fresh);
    }
    shape = TreeShape(&fresh);
    if (v.loaded && !force && shape == v.shape) {
        SessionStore_Free(&v.set);
        v.set = fresh;   /* the same rows in the same order: the tree's items still point right */
        InvalidateRect(v.profiles, NULL, FALSE);
        RedrawDetails();
        Watch();
        return;
    }
    if (v.loaded) {
        if (!force) RememberFirstVisible();
        SessionStore_Free(&v.set);
    }
    v.set = fresh;
    v.shape = shape;
    v.loaded = TRUE;
    Measure();
    FillProfiles();
    FillTree();
    RedrawDetails();
    Watch();
}

void SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder)
{
    size_t i;
    v.pkg = *pkg;
    if (folder) StringCchCopyW(v.folder, ARRAYSIZE(v.folder), folder);
    v.shown = TRUE;
    Load(TRUE);
    for (i = 0; i < ARRAYSIZE(kControls); i++) ShowWindow(GetDlgItem(v.dlg, kControls[i]), SW_SHOW);
    SetFocus(v.tree);
}

void SessionsView_Leave(void)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(kControls); i++) ShowWindow(GetDlgItem(v.dlg, kControls[i]), SW_HIDE);
    StopWatching();
    v.shown = FALSE;
}

void SessionsView_Reload(void)
{
    if (v.shown) Load(FALSE);
}

void SessionsView_Relayout(void)
{
    if (!v.shown) return;
    RememberFirstVisible();
    Measure();
    FillProfiles();
    FillTree();
    RedrawDetails();
}

BOOL SessionsView_ClearSearch(void)
{
    if (!v.shown || GetFocus() != v.search || GetWindowTextLengthW(v.search) == 0) return FALSE;
    SetWindowTextW(v.search, L"");
    return TRUE;
}

BOOL SessionsView_Command(WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDOK:   /* Enter */
        if (v.shown && GetFocus() == v.tree && SelectedRow() >= 0) RunAction(ACT_OPEN, -1);
        return v.shown;
    case IDC_S_SEARCH:
        if (HIWORD(wp) == EN_CHANGE && v.shown) {
            GetWindowTextW(v.search, v.filter, ARRAYSIZE(v.filter));
            FillTree();
            RedrawDetails();
        }
        return TRUE;
    case IDC_S_ARCHIVED:
        if (HIWORD(wp) == BN_CLICKED && v.shown) {
            v.showArchived = SendMessageW(v.archived, BM_GETCHECK, 0, 0) == BST_CHECKED;
            InvalidateRect(v.profiles, NULL, FALSE);
            FillTree();
            RedrawDetails();
        }
        return TRUE;
    case IDC_S_PROFILES:
        if (HIWORD(wp) == LBN_SELCHANGE && v.shown) {
            LRESULT i = SendMessageW(v.profiles, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < v.set.profiles.count) {
                StringCchCopyW(v.folder, ARRAYSIZE(v.folder), ProfileAt((int)i)->folder);
                FillTree();
                RedrawDetails();
            }
        }
        return TRUE;
    }
    return FALSE;
}

/* WM_CONTEXTMENU: the tree's menu, at the mouse or (from the keyboard) at
 * the selected row. */
BOOL SessionsView_ContextMenu(HWND from, LPARAM pos)
{
    POINT pt;
    if (!v.shown || from != v.tree) return FALSE;
    if (pos == (LPARAM)-1) {
        RECT rc;
        HTREEITEM sel = TreeView_GetSelection(v.tree);
        if (!sel || !TreeView_GetItemRect(v.tree, sel, &rc, TRUE)) return TRUE;
        pt.x = rc.left;
        pt.y = rc.bottom;
        ClientToScreen(v.tree, &pt);
    } else {
        TVHITTESTINFO hit;
        pt.x = (short)LOWORD(pos);
        pt.y = (short)HIWORD(pos);
        ZeroMemory(&hit, sizeof hit);
        hit.pt = pt;
        ScreenToClient(v.tree, &hit.pt);
        if (TreeView_HitTest(v.tree, &hit) && (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT | TVHT_ONITEMBUTTON)))
            TreeView_SelectItem(v.tree, hit.hItem);
    }
    if (NodeParam(TreeView_GetSelection(v.tree)) >= 0) ShowMenu(pt);
    return TRUE;
}

BOOL SessionsView_Notify(const NMHDR *h, LPARAM lp, LRESULT *result)
{
    *result = 0;
    if (h->idFrom != IDC_S_TREE) return FALSE;
    switch (h->code) {
    case NM_CUSTOMDRAW:
        *result = TreeCustomDraw(lp);
        return TRUE;
    case NM_DBLCLK:
        if (NodeParam(TreeView_GetSelection(v.tree)) >= 0) {
            RunAction(ACT_OPEN, -1);
            *result = 1;
        }
        return TRUE;
    case TVN_KEYDOWN: {
        const NMTVKEYDOWN *key = (const NMTVKEYDOWN *)lp;
        int r = SelectedRow(), p = ProfileIndex();
        const SessionEntry *e = r >= 0 ? EntryOf(&v.set.rows[r], p) : NULL;
        if (e && !e->pendingRemove && key->wVKey == VK_F2) RunAction(ACT_RENAME, -1);
        else if (e && !e->pendingRemove && key->wVKey == VK_DELETE) RunAction(ACT_REMOVE, -1);
        return TRUE;
    }
    case TVN_SELCHANGEDW: {
        const NMTREEVIEWW *nm = (const NMTREEVIEWW *)lp;
        if (!v.filling) {
            if (nm->itemNew.lParam >= 0 && nm->itemNew.lParam < v.set.rowCount)
                StringCchCopyW(v.selectedKey, ARRAYSIZE(v.selectedKey), v.set.rows[nm->itemNew.lParam].key);
            else
                v.selectedKey[0] = 0;   /* a folder */
            v.hotChip = v.pressedChip = -1;
            v.scroll = 0;
            RedrawDetails();
        }
        return TRUE;
    }
    case TVN_ITEMEXPANDEDW: {
        const NMTREEVIEWW *nm = (const NMTREEVIEWW *)lp;
        LPARAM node = nm->itemNew.lParam;
        if (!v.filling && node < 0 && (nm->action == TVE_COLLAPSE || nm->action == TVE_EXPAND))
            SetCollapsed(GroupKey(NodeGroup(node)), nm->action == TVE_COLLAPSE);
        return TRUE;
    }
    case TVN_GETINFOTIPW: {
        NMTVGETINFOTIPW *tip = (NMTVGETINFOTIPW *)lp;
        WCHAR date[64], time[32];
        if (tip->lParam >= 0 && tip->lParam < v.set.rowCount) {
            const SessionRow *row = &v.set.rows[tip->lParam];
            FormatWhen(row->lastActivity, date, ARRAYSIZE(date), time, ARRAYSIZE(time));
            StringCchPrintfW(tip->pszText, (size_t)tip->cchTextMax, L"%s\nLast used %s %s%s", TitleIn(row, ProfileIndex()), date, time,
                             row->transcript ? L"" : L"\nNo conversation on disk");
        } else if (tip->lParam < NODE_FAVORITES && NodeGroup(tip->lParam) < v.set.groupCount) {
            const SessionGroup *g = &v.set.groups[NodeGroup(tip->lParam)];
            StringCchCopyW(tip->pszText, (size_t)tip->cchTextMax, g->path[0] ? g->path : g->name);
        }
        return TRUE;
    }
    }
    return FALSE;
}

BOOL SessionsView_DrawItem(const DRAWITEMSTRUCT *di)
{
    if (di->CtlID == IDC_S_PROFILES) {
        DrawProfile(di);
        return TRUE;
    }
    if (di->CtlID == IDC_S_DETAILS) {
        DrawDetails(di);
        return TRUE;
    }
    return FALSE;
}

void SessionsView_Destroy(void)
{
    StopWatching();
    if (v.loaded) SessionStore_Free(&v.set);
    Theme_FreeFonts(&v.fonts);
    if (v.icons) ImageList_Destroy(v.icons);
    if (v.dots) ImageList_Destroy(v.dots);
    ZeroMemory(&v, sizeof v);
}
