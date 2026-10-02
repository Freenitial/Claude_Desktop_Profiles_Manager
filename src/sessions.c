/*
 * The sessions view of the manager window: each profile's Claude Code
 * sessions. On the left the profiles, with their badge and how many sessions
 * they list; in the middle the chosen profile's sessions as a tree, the
 * starred ones first, then by project (every row drawn here), or why it has
 * none; on the right the selected session: where it lives, then each
 * profile with what is going on there and an Actions box (open, rename,
 * star, remove, keep when its removal waits, or share and copy where it is
 * not listed), what is not listed, and Delete everywhere at the bottom. The
 * tree's context menu holds the entry's actions in the profile shown,
 * opening, sharing and copying in the others, the folder and Delete
 * everywhere; the other profiles' entries change from their Actions box.
 * What they do is in sessionedit.c; colors, fonts, rows, buttons and boxes
 * come from theme.c, as everywhere.
 *
 * It follows the disk without polling: a thread waits for folder
 * notifications on each profile's session entries and config.json and on
 * the transcripts folder (while one of them is missing, on its nearest
 * existing parent, for the next folder down) and posts WM_APP_SESSIONS to the
 * window, once for a burst of them. Another thread reads the snapshot and
 * posts WM_APP_SESSIONS_READY; a notification during a read reads again a
 * moment after that read is published. A snapshot that changes nothing the
 * tree shows leaves the tree alone (no flicker, no scroll); one that does
 * keeps the selection, the folded folders and the row on top of the view in
 * its place.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <limits.h>
#include <shellapi.h>
#include <windowsx.h>

#define MAX_COLLAPSED  128
#define MAX_CHIPS      96
#define NOTE_CCH       1024
#define NAMES_CCH      (MAX_PROFILES * (LABEL_CCH + 16))
#define WATCH_MAX      (2 * MAX_PROFILES + 1)
#define NODE_STARRED   (-1)             /* tree lParam: >= 0 a row, else a folder (-2 - group) */
#define NODE_NONE      ((LPARAM)INT_MIN)   /* no row of the tree */
#define STARRED_KEY    L"*starred"      /* folder keys that are no path start with '*' (GroupKey) */
#define IDM_ACTION     0x5000           /* menu commands: IDM_ACTION + action * MAX_PROFILES + profile */
#define SLOW_LOAD_MS   1000             /* a snapshot read slower than this is logged even when nothing changed */
#define READ_PACE_MS   500              /* the least time from a snapshot to the next one a notification asks for */
#define PROFILE_ICON_DIPS      32       /* a profile's icon in the side bar */
#define SPACING_DIPS           6        /* the padding of the side bar, the tree and the details (g_view.pad) */
#define TREE_INDENT_DIPS       18       /* a tree level: a folder's arrow */
#define FOLDER_ROW_LINE_TENTHS 19       /* a folder's row: 1.9 lines of text, at least a heading's line and the padding */
#define BADGE_LINE_QUARTERS    3        /* a profile's badge in the details: three quarters of a line of text */
#define FIRST_PART_PAD_THIRDS  5        /* the first profile's part below the details' line: five thirds of the padding */
#define UNIX_EPOCH_FILETIME    116444736000000000ULL   /* 1970-01-01 in FILETIME units (100 ns from 1601) */

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

typedef struct ChipRef {                /* a control of the details by what it does: drawn again, it moves */
    HWND   window;                      /* the window it is drawn in; NULL: none */
    Action action;
    int    profile;
} ChipRef;

typedef struct TreeNodeKey {            /* a row of the tree, found again after a refill */
    WCHAR key[MAX_PATH];                /* a session's key, or a folder's (GroupKey); "" for none */
    BOOL  folder;
    BOOL  inStarred;                    /* a session's row under Starred */
} TreeNodeKey;

typedef struct WatchedFolder {          /* one folder notification of the watcher */
    WCHAR dir[MAX_PATH];
    BOOL  subtree;
    DWORD filter;
    /* For a folder watched for a deeper one or for one of its files, what a
     * notification must change to ask for a snapshot: the nearest existing
     * folder of `nearest`, and the size and time of `file` ("" for neither:
     * every notification does). */
    WCHAR nearest[MAX_PATH];
    WCHAR file[MAX_PATH];
} WatchedFolder;

typedef struct WatchPlan {
    int           count;
    WatchedFolder folder[WATCH_MAX];
} WatchPlan;

typedef struct SessionLoader {
    SRWLOCK     lock;
    HANDLE      thread, stop, request, cancel;
    HWND        dlg;
    ProfileList profiles;               /* a snapshot from the manager's UI thread */
    ULONG       generation;             /* raised when a read in progress is out of date (RequestLoad) */
    ULONG       readyGeneration;
    SessionSet *ready;                  /* owned here until the UI takes it */
    ULONGLONG   readyTreeHash;          /* TreeContentHash of `ready` */
    WatchPlan   plan;                   /* what the snapshot being read asks to watch (the loader thread's) */
    WatchPlan   readyPlan;              /* what `ready` asks to watch */
    BOOL        failed;
    BOOL        failureLogged;          /* the reads failing since the last one that worked are logged (the loader thread's) */
    int         loggedEntries, loggedRows, loggedGroups;   /* the last snapshot logged (the loader thread's) */
} SessionLoader;

typedef struct SessionsView {
    HWND          dlg, profiles, tree, search, archived, details;
    HWND          parts;                /* in the details, each profile's part: what scrolls */
    HWND          treeArea;             /* the view the tree scrolls in */
    BOOL          shown, loaded, filling, showArchived;
    BOOL          warmed, dirty, rendered, loading, loadFailed, accepting;
    int           actionDepth;
    ProfileList   profilesToRead;       /* the profiles as the manager last gave them, for the next read */
    SessionLoader *loader;
    SessionSet    set;
    ULONGLONG     treeHash;             /* what the tree shows of `set` (TreeContentHash) */
    const ClaudePackage *pkg;           /* the manager's, which it keeps current */
    WCHAR         filter[128];
    TreeNodeKey   selection;            /* the tree's selected row: its session is the one in the details */
    BOOL          keepPlace;            /* the next refill keeps the view where it is (RememberTopRow) */
    TreeNodeKey   topRow;               /* the row then on top of the view ("" at the very top) */
    int           topRowOffset;         /* how far it was above the view's top, px */
    TreeNodeKey   topRowNext;           /* the row below it: on top instead when it is gone */
    /* The sessions just below and above the selected one when the tree was
     * last filled: the selection moves to one of them when its session goes.
     * Taken before the snapshot the tree was filled from is replaced, the
     * tree's rows then naming that snapshot's rows (RememberNeighbors). */
    TreeNodeKey   neighbor[2];
    BOOL          neighborsTaken;
    BOOL          folderLookedUp;       /* selectedFolder is current for the working folder below */
    WCHAR         folderLookedUpFor[MAX_PATH];   /* that working folder */
    WCHAR         selectedFolder[MAX_PATH];   /* the selected session's folder for Explorer; "" when not on disk */
    int           sessionCount[MAX_PROFILES];   /* the sessions each profile lists, as the side bar counts them */
    int          *folderCount;          /* the sessions each folder of the tree shows, by group */
    int           folderCountCapacity, starredCount;
    WCHAR         folder[FOLDER_CCH];   /* the profile shown */
    HIMAGELIST    icons, badges;        /* profile icons for the list; badges for the details */
    int           iconSize, badgeSize, pad, indent, unit;
    ThemeFonts    fonts;
    int           fontHeight[THEME_FONTS], fontAscent[THEME_FONTS], controlHeight, actionsWidth;
    UINT          measuredDpi;
    HFONT         measuredFont;
    int           measuredLanguage;
    ULONGLONG     imageListHash;        /* what the icons and badges were made for */
    ChipSet       fixed, scrolled;      /* the controls of the details, and of their part that scrolls */
    ChipRef       hot, pressed, open;   /* the control under the mouse, pressed, with its menu open */
    TreeNodeKey   pressedSelection;     /* the selection when a control was pressed: its session is the one acted on */
    int           scroll;               /* how far the profiles' parts are scrolled, px */
    HANDLE        watchThread, watchStop;
    WatchPlan     watched;              /* what the watcher thread watches */
    WatchPlan     planned;              /* what the snapshot shown asks to watch */
    WCHAR         collapsed[MAX_COLLAPSED][MAX_PATH];   /* folders the user folded, by GroupKey */
    int           collapsedCount;
    ULONGLONG     waitingChangesTried[MAX_PROFILES];   /* each profile's waiting changes as last tried in vain (WaitingChangesState) */
} SessionsView;

static SessionsView g_view;
static volatile LONG g_reloadMessagePending;   /* a WM_APP_SESSIONS is posted and no snapshot asked for since */

static const int kControls[] = { IDC_S_PROFILES, IDC_S_SEARCH, IDC_S_ARCHIVED, IDC_S_TREE, IDC_S_DETAILS };

static void RequestLoad(BOOL restart);

/* ------------------------------------------------------------- formatting */

/* A time in milliseconds since 1970 as the user's short date and time;
 * FALSE for none. */
static BOOL FormatWhen(ULONGLONG ms, WCHAR *date, size_t dateCch, WCHAR *time, size_t timeCch)
{
    ULARGE_INTEGER ticks;
    FILETIME fileTime;
    SYSTEMTIME utc, local;
    date[0] = time[0] = 0;
    if (!ms) return FALSE;
    ticks.QuadPart = ms * 10000ULL + UNIX_EPOCH_FILETIME;
    fileTime.dwLowDateTime = ticks.LowPart;
    fileTime.dwHighDateTime = ticks.HighPart;
    return FileTimeToSystemTime(&fileTime, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local) &&
           GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, (int)dateCch, NULL) &&
           GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, time, (int)timeCch);
}

static void FormatSize(ULONGLONG bytes, WCHAR *out, size_t cch)
{
    if (bytes >= 1024ULL * 1024ULL)
        StringCchPrintfW(out, cch, TR(L"%lu.%lu MB"), (unsigned long)(bytes / (1024ULL * 1024ULL)),
                         (unsigned long)((bytes % (1024ULL * 1024ULL)) * 10 / (1024ULL * 1024ULL)));
    else
        StringCchPrintfW(out, cch, TR(L"%lu KB"), (unsigned long)((bytes + 1023) / 1024));
}

/* ------------------------------------------------------------------ rows */

static int ShownProfileIndex(void)
{
    int i = Profiles_Find(&g_view.set.profiles, g_view.folder);
    return i >= 0 ? i : (g_view.set.profiles.count > 0 ? 0 : -1);
}

static const Profile *ProfileAt(int p)
{
    return &g_view.set.profiles.items[p];
}

/* The manager's Claude package, which it keeps current; none before the
 * view first shows. */
static const ClaudePackage *Package(void)
{
    static const ClaudePackage none = { 0 };
    return g_view.pkg ? g_view.pkg : &none;
}

static const SessionEntry *EntryOf(const SessionRow *row, int profile)
{
    return profile >= 0 && row->entry[profile] >= 0 ? &g_view.set.entries[row->entry[profile]] : NULL;
}

/* A session's title in a profile, a new one waiting included; "" when it
 * has none anywhere. */
static const WCHAR *TitleIn(const SessionRow *row, int profile)
{
    const SessionEntry *entry = EntryOf(row, profile);
    if (entry && entry->pendingTitle[0]) return entry->pendingTitle;
    return entry && entry->title[0] ? entry->title : SessionStore_RowTitle(&g_view.set, row);
}

/* The same as it is shown: an untitled session says so. */
static const WCHAR *ShownTitle(const SessionRow *row, int profile)
{
    const WCHAR *title = TitleIn(row, profile);
    return title[0] ? title : TR(L"Untitled session");
}

/* Starred in a profile, a change waiting included. */
static BOOL StarredIn(const SessionEntry *entry)
{
    return entry && (entry->pendingStar >= 0 ? entry->pendingStar == 1 : entry->starred);
}

/* Listed, and not about to be removed: a removal waiting for the profile to
 * close leaves nothing to open, rename or remove there. */
static BOOL ListedAndKept(const SessionEntry *entry)
{
    return entry && !entry->pendingRemove;
}

static BOOL IsScratch(const SessionRow *row)
{
    return g_view.set.groups[row->group].scratchOf >= 0;
}

/* The search matches what the tree shows: the title, the folder's name, or
 * the working folder. */
static BOOL Matches(const SessionRow *row, int profile)
{
    WCHAR name[MAX_PATH];
    if (!g_view.filter[0]) return TRUE;
    SessionStore_GroupName(&g_view.set, row->group, name, ARRAYSIZE(name));
    return Core_ContainsI(ShownTitle(row, profile), g_view.filter) || Core_ContainsI(name, g_view.filter) || Core_ContainsI(row->cwd, g_view.filter);
}

static BOOL RowVisibleIn(const SessionRow *row, int profile)
{
    const SessionEntry *entry = EntryOf(row, profile);
    return entry && (g_view.showArchived || !entry->archived) && Matches(row, profile);
}

static int SelectedRow(void)
{
    return g_view.selection.key[0] && !g_view.selection.folder ? SessionStore_FindRow(&g_view.set, g_view.selection.key) : -1;
}

/* What a folder of the tree is remembered by (folded, on top, selected),
 * whatever the language and the profiles' names: its path, else its owner's
 * folder for a profile's "no folder". */
static void GroupKey(int group, WCHAR *out, size_t cch)
{
    const SessionGroup *sessionGroup = &g_view.set.groups[group];
    if (sessionGroup->scratchOf >= 0) StringCchPrintfW(out, cch, L"*no folder:%s", ProfileAt(sessionGroup->scratchOf)->folder);
    else if (sessionGroup->path[0]) StringCchCopyW(out, cch, sessionGroup->path);
    else StringCchCopyW(out, cch, L"*unknown");
}

/* The name of the Starred folder, in the current language. */
static const WCHAR *StarredName(void)
{
    return TR(L"\x2605  Starred");
}

static int NodeGroup(LPARAM node)
{
    return node == NODE_STARRED ? -1 : (int)(-2 - node);
}

static int BitCount(DWORD bits)
{
    int n = 0;
    for (; bits; bits &= bits - 1) n++;
    return n;
}

/* The profiles that list session `row`, one bit each. */
static DWORD ProfilesListing(const SessionRow *row)
{
    DWORD bits = 0;
    int p;
    for (p = 0; p < g_view.set.profiles.count; p++)
        if (row->entry[p] >= 0) bits |= 1u << p;
    return bits;
}

/* The profiles other than `except` that keep session `row`: they list it
 * and no removal of it waits there. */
static DWORD ProfilesKeeping(const SessionRow *row, int except)
{
    DWORD bits = 0;
    int p;
    for (p = 0; p < g_view.set.profiles.count; p++)
        if (p != except && ListedAndKept(EntryOf(row, p))) bits |= 1u << p;
    return bits;
}

/* What the tree shows of a set: its profiles and why one lists nothing, its
 * rows in their order with what titles them, what the search reads, their
 * stars, archives and changes waiting, and their folders. A snapshot
 * that keeps it only redraws the side bar and the details. Pure: the loader
 * thread computes it. */
static ULONGLONG TreeContentHash(const SessionSet *set)
{
    ULONGLONG hash = CORE_HASH_START;
    int i, p;
    hash = Core_HashBytes(hash, &set->profiles.count, sizeof set->profiles.count);
    for (i = 0; i < set->profiles.count; i++) {
        const Profile *profile = &set->profiles.items[i];
        const SessionSource *source = &set->source[i];
        int reasons[3] = { source->signedIn, source->unreadable, source->elsewhere };
        hash = Core_HashText(hash, profile->folder);
        hash = Core_HashText(hash, profile->name);
        hash = Core_HashBytes(hash, &profile->color, sizeof profile->color);
        hash = Core_HashBytes(hash, reasons, sizeof reasons);
    }
    hash = Core_HashBytes(hash, &set->rowCount, sizeof set->rowCount);
    for (i = 0; i < set->rowCount; i++) {
        const SessionRow *row = &set->rows[i];
        hash = Core_HashText(hash, row->key);
        hash = Core_HashText(hash, row->cwd);
        hash = Core_HashText(hash, SessionStore_RowTitle(set, row));
        hash = Core_HashBytes(hash, &row->group, sizeof row->group);
        hash = Core_HashBytes(hash, &row->transcript, sizeof row->transcript);
        for (p = 0; p < set->profiles.count; p++) {
            const SessionEntry *entry = row->entry[p] >= 0 ? &set->entries[row->entry[p]] : NULL;
            int flags[6] = { entry != NULL, entry && entry->starred, entry && entry->archived, entry ? entry->pendingStar : -1,
                             entry && entry->pendingRemove, entry && entry->pending };
            hash = Core_HashBytes(hash, flags, sizeof flags);
            if (entry) {
                hash = Core_HashText(hash, entry->title);
                hash = Core_HashText(hash, entry->pendingTitle);
            }
        }
    }
    hash = Core_HashBytes(hash, &set->groupCount, sizeof set->groupCount);
    for (i = 0; i < set->groupCount; i++) {
        hash = Core_HashText(hash, set->groups[i].name);
        hash = Core_HashText(hash, set->groups[i].path);
        hash = Core_HashBytes(hash, &set->groups[i].scratchOf, sizeof set->groups[i].scratchOf);
    }
    return hash;
}

/* ---------------------------------------------------------- measurements */

static HFONT DialogFont(void)
{
    return (HFONT)SendMessageW(g_view.dlg, WM_GETFONT, 0, 0);
}

static HFONT Font(ThemeFont role)
{
    return g_view.fonts.font[role] ? g_view.fonts.font[role] : DialogFont();
}

/* Each font's height and ascent, the details' control height (a button's)
 * and their Actions boxes' width: measured once for the window's scale and
 * language, read by every paint. */
static void MeasureFonts(void)
{
    TEXTMETRICW metrics;
    HDC dc = GetDC(g_view.dlg);
    HGDIOBJ old;
    int role;
    g_view.actionsWidth = Theme_DropDownWidth(g_view.dlg, DialogFont(), TR(Theme_SessionsCaption(SESSIONS_ACTIONS)));
    if (!dc) return;
    old = SelectObject(dc, DialogFont());
    for (role = 0; role < THEME_FONTS; role++) {
        SelectObject(dc, Font((ThemeFont)role));
        GetTextMetricsW(dc, &metrics);
        g_view.fontHeight[role] = metrics.tmHeight;
        g_view.fontAscent[role] = metrics.tmAscent;
    }
    SelectObject(dc, DialogFont());
    GetTextMetricsW(dc, &metrics);
    g_view.controlHeight = metrics.tmHeight + g_view.pad;
    SelectObject(dc, old);
    ReleaseDC(g_view.dlg, dc);
}

static int LineHeight(ThemeFont role)
{
    return g_view.fontHeight[role];
}

static int TextWidth(HDC dc, ThemeFont role, const WCHAR *text)
{
    SIZE extent = { 0, 0 };
    HGDIOBJ old = SelectObject(dc, Font(role));
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &extent);
    SelectObject(dc, old);
    return extent.cx;
}

static RECT ChildRect(HWND child)
{
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(NULL, g_view.dlg, (POINT *)&r, 2);
    return r;
}

/* An image appended to a list of the profiles' images (image p: profile
 * p): a blank one when the profile's icon could not be made, so that the
 * next profiles' images keep their index. */
static void AddProfileImage(HIMAGELIST list, HICON icon, int size)
{
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP blank;
    if (!list || (icon && ImageList_AddIcon(list, icon) >= 0)) return;
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if ((blank = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0)) == NULL) return;
    ZeroMemory(bits, (size_t)size * (size_t)size * 4);   /* transparent */
    ImageList_Add(list, blank, NULL);
    DeleteObject(blank);
}

/* Fonts, icon sizes and row heights for the window's scale. Where the
 * controls go is Theme_LayoutMain's. */
static void Measure(void)
{
    UINT dpi = GetDpiForWindow(g_view.dlg);
    HFONT baseFont = DialogFont();
    int language = Localize_EffectiveLanguage();
    ULONGLONG imageListHash;
    int line, folderRow, i;
    if (!g_view.fonts.font[THEME_FONT_TEXT] || g_view.measuredDpi != dpi || g_view.measuredFont != baseFont || g_view.measuredLanguage != language) {
        Theme_CreateFonts(g_view.dlg, &g_view.fonts);
        g_view.measuredDpi = dpi;
        g_view.measuredFont = baseFont;
        g_view.measuredLanguage = language;
    }
    g_view.iconSize = MulDiv(PROFILE_ICON_DIPS, (int)dpi, 96);
    g_view.pad = MulDiv(SPACING_DIPS, (int)dpi, 96);
    MeasureFonts();
    line = LineHeight(THEME_FONT_TEXT);
    g_view.badgeSize = line * BADGE_LINE_QUARTERS / 4;
    SendMessageW(g_view.search, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search this profile's sessions"));
    SendMessageW(g_view.profiles, LB_SETITEMHEIGHT, 0, MAKELPARAM(max(g_view.iconSize, 2 * line) + 2 * g_view.pad, 0));
    folderRow = max(line * FOLDER_ROW_LINE_TENTHS / 10, LineHeight(THEME_FONT_HEADING) + g_view.pad);
    g_view.unit = max(1, folderRow / FOLDER_UNITS);
    TreeView_SetItemHeight(g_view.tree, g_view.unit);
    TreeView_SetIndent(g_view.tree, MulDiv(TREE_INDENT_DIPS, (int)dpi, 96));
    g_view.indent = (int)TreeView_GetIndent(g_view.tree);
    Theme_SetScrollRow(g_view.tree, SESSION_UNITS * g_view.unit);   /* a wheel line: a session */
    Theme_SetScrollRow(g_view.parts, line);
    imageListHash = Core_HashBytes(CORE_HASH_START, &dpi, sizeof dpi);
    imageListHash = Core_HashBytes(imageListHash, &g_view.iconSize, sizeof g_view.iconSize);
    imageListHash = Core_HashBytes(imageListHash, &g_view.badgeSize, sizeof g_view.badgeSize);   /* follows the font */
    imageListHash = Core_HashText(imageListHash, Package()->exe);
    imageListHash = Core_HashBytes(imageListHash, &g_view.set.profiles.count, sizeof g_view.set.profiles.count);
    for (i = 0; i < g_view.set.profiles.count; i++) {
        const Profile *profile = ProfileAt(i);
        imageListHash = Core_HashText(imageListHash, profile->name);
        imageListHash = Core_HashBytes(imageListHash, &profile->color, sizeof profile->color);
    }
    if (g_view.icons && g_view.badges && imageListHash == g_view.imageListHash) return;
    g_view.imageListHash = imageListHash;
    if (g_view.icons) ImageList_Destroy(g_view.icons);
    if (g_view.badges) ImageList_Destroy(g_view.badges);
    g_view.icons = ImageList_Create(g_view.iconSize, g_view.iconSize, ILC_COLOR32, g_view.set.profiles.count, 1);
    g_view.badges = ImageList_Create(g_view.badgeSize, g_view.badgeSize, ILC_COLOR32, g_view.set.profiles.count, 1);
    for (i = 0; i < g_view.set.profiles.count; i++) {
        HICON icon = Icons_Create(Package(), ProfileAt(i), g_view.iconSize);
        HICON badge = Icons_CreateBadge(ProfileAt(i)->color, g_view.badgeSize);
        AddProfileImage(g_view.icons, icon, g_view.iconSize);
        AddProfileImage(g_view.badges, badge, g_view.badgeSize);
        if (icon) DestroyIcon(icon);
        if (badge) DestroyIcon(badge);
    }
}

/* ------------------------------------------------------------------ text */

/* `text` in `rc` (one line, cut with an ellipsis), vertically centered. */
static void TextLine(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, RECT *rc, UINT extra)
{
    HGDIOBJ old = SelectObject(dc, Font(role));
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, rc, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS | extra |
              ((extra & DT_PATH_ELLIPSIS) ? 0 : Localize_ReadingFlags()));
    SelectObject(dc, old);
}

/* `text` from x to at most `right`, its baseline on `baseline` (texts of two
 * sizes on one line share it). Returns where it ended. */
static int TextAt(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, int x, int right, int baseline)
{
    RECT r;
    HGDIOBJ old = SelectObject(dc, Font(role));
    r.left = x;
    r.right = min(right, x + TextWidth(dc, role, text));
    r.top = baseline - g_view.fontAscent[role];
    r.bottom = r.top + g_view.fontHeight[role];
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | Localize_ReadingFlags());
    SelectObject(dc, old);
    return r.right;
}

/* A baseline for text of `role` centered in `rc`. */
static int CenteredBaseline(const RECT *rc, ThemeFont role)
{
    return rc->top + (rc->bottom - rc->top - g_view.fontHeight[role]) / 2 + g_view.fontAscent[role];
}

/* `text` wrapped from `top`, on at most `lines` lines (the last cut with an
 * ellipsis). Returns the bottom. */
static int Paragraph(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, int left, int right, int top, int lines)
{
    RECT need = { left, top, right, top }, draw;
    HGDIOBJ old = SelectObject(dc, Font(role));
    DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | Localize_ReadingFlags());
    SetRect(&draw, left, top, right, top + min(need.bottom - need.top, lines * LineHeight(role)));
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &draw, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS | Localize_ReadingFlags());
    SelectObject(dc, old);
    return draw.bottom;
}

/* -------------------------------------------------------------- profiles */

/* How many sessions each profile lists, as the side bar shows them: the
 * archived ones only while they are shown. */
static void CountSessions(void)
{
    int r, p;
    ZeroMemory(g_view.sessionCount, sizeof g_view.sessionCount);
    for (r = 0; r < g_view.set.rowCount; r++)
        for (p = 0; p < g_view.set.profiles.count; p++) {
            const SessionEntry *entry = EntryOf(&g_view.set.rows[r], p);
            if (entry && (g_view.showArchived || !entry->archived)) g_view.sessionCount[p]++;
        }
}

/* The side bar filled again: item i is profile i, by its name (screen
 * readers read it, typing selects it), up to one that cannot be added.
 * Selected while drawing is off, the profile shown is not scrolled to: a
 * refill keeps the side bar's place (SessionsView_Enter shows it). */
static void FillProfiles(void)
{
    int i, pick = ShownProfileIndex();
    SendMessageW(g_view.profiles, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_view.profiles, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_view.set.profiles.count; i++)
        if (SendMessageW(g_view.profiles, LB_ADDSTRING, 0, (LPARAM)ProfileAt(i)->name) < 0) break;
    SendMessageW(g_view.profiles, LB_SETCURSEL, (WPARAM)pick, 0);
    SendMessageW(g_view.profiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_view.profiles, NULL, TRUE);
}

/* Icon, name, then whether it is open and how many sessions it lists, in
 * gray, on the window's background (a side bar: no frame). */
static void DrawProfile(const DRAWITEMSTRUCT *item)
{
    WCHAR second[64];
    ThemeBuffer buffer;
    RECT rc = item->rcItem, line;
    UINT state = (item->itemState & ODS_SELECTED) ? THEME_ROW_SELECTED : 0;
    COLORREF text;
    HDC dc;
    int p = (int)item->itemID, lineHeight = LineHeight(THEME_FONT_TEXT), count, top;
    if (item->itemID == (UINT)-1 || p < 0 || p >= g_view.set.profiles.count) return;
    dc = Theme_BufferBegin(&buffer, item->hDC, &rc);
    text = Theme_DrawRow(item->hwndItem, dc, &rc, state, Theme_Color(THEME_FACE));
    if (g_view.icons) ImageList_Draw(g_view.icons, p, dc, rc.left + g_view.pad, rc.top + (rc.bottom - rc.top - g_view.iconSize) / 2, ILD_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    top = rc.top + (rc.bottom - rc.top - 2 * lineHeight) / 2;
    SetRect(&line, rc.left + g_view.pad * 2 + g_view.iconSize, top, rc.right - g_view.pad, top + lineHeight);
    TextLine(dc, THEME_FONT_STRONG, text, ProfileAt(p)->name, &line, 0);
    count = g_view.sessionCount[p];
    if (!g_view.loaded && g_view.loading)
        StringCchCopyW(second, ARRAYSIZE(second), TR(L"Loading sessions\x2026"));
    else if (!g_view.loaded)   /* not read: no count to give */
        StringCchCopyW(second, ARRAYSIZE(second), ProfileAt(p)->running ? TR(L"open") : L"");
    else
        StringCchPrintfW(second, ARRAYSIZE(second), count == 1 ? TR(L"%s%d session") : TR(L"%s%d sessions"),
                         ProfileAt(p)->running ? TR(L"open \x00B7 ") : L"", count);
    OffsetRect(&line, 0, lineHeight);
    TextLine(dc, THEME_FONT_TEXT, Theme_RowMuted(state), second, &line, 0);
    if (item->itemState & ODS_FOCUS) Theme_DrawFocusCue(item->hwndItem, dc, &rc);
    Theme_BufferEnd(&buffer);
}

/* ------------------------------------------------------------------ tree */

static BOOL WasCollapsed(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_view.collapsedCount; i++)
        if (Core_EqualsI(g_view.collapsed[i], key)) return TRUE;
    return FALSE;
}

static void SetCollapsed(const WCHAR *key, BOOL collapsed)
{
    int i;
    for (i = 0; i < g_view.collapsedCount; i++) {
        if (!Core_EqualsI(g_view.collapsed[i], key)) continue;
        if (!collapsed) {
            g_view.collapsedCount--;
            if (i < g_view.collapsedCount)
                StringCchCopyW(g_view.collapsed[i], ARRAYSIZE(g_view.collapsed[i]), g_view.collapsed[g_view.collapsedCount]);
        }
        return;
    }
    if (collapsed && g_view.collapsedCount < MAX_COLLAPSED) {
        StringCchCopyW(g_view.collapsed[g_view.collapsedCount], ARRAYSIZE(g_view.collapsed[g_view.collapsedCount]), key);
        g_view.collapsedCount++;
    }
}

static LPARAM NodeParam(HTREEITEM item)
{
    TVITEMW query;
    ZeroMemory(&query, sizeof query);
    query.mask = TVIF_PARAM;
    query.hItem = item;
    return item && TreeView_GetItem(g_view.tree, &query) ? query.lParam : NODE_NONE;
}

/* The key of tree node `node`; `inStarred`: a session's row under Starred. */
static void NodeKey(LPARAM node, BOOL inStarred, TreeNodeKey *out)
{
    out->key[0] = 0;
    out->folder = node < 0;
    out->inStarred = node >= 0 && inStarred;
    if (node == NODE_STARRED) StringCchCopyW(out->key, ARRAYSIZE(out->key), STARRED_KEY);
    else if (node < 0 && node != NODE_NONE && NodeGroup(node) < g_view.set.groupCount) GroupKey(NodeGroup(node), out->key, ARRAYSIZE(out->key));
    else if (node >= 0 && node < g_view.set.rowCount) StringCchCopyW(out->key, ARRAYSIZE(out->key), g_view.set.rows[node].key);
}

static void ItemKey(HTREEITEM item, TreeNodeKey *out)
{
    NodeKey(NodeParam(item), NodeParam(TreeView_GetParent(g_view.tree, item)) == NODE_STARRED, out);
}

static BOOL SameNode(const TreeNodeKey *one, const TreeNodeKey *other)
{
    return one->key[0] && one->folder == other->folder && one->inStarred == other->inStarred && Core_EqualsI(one->key, other->key);
}

/* Two rows of one session (in its folder or under Starred). */
static BOOL SameSession(const TreeNodeKey *one, const TreeNodeKey *other)
{
    return one->key[0] && !one->folder && !other->folder && Core_EqualsI(one->key, other->key);
}

/* The selection, as the tree shows it. Another session (or a folder): the
 * details show its profiles from the top (its folder is looked up again only
 * when it is another one: SelectedFolder). */
static void SetSelection(const TreeNodeKey *key)
{
    if (!SameSession(key, &g_view.selection)) g_view.scroll = 0;
    g_view.selection = *key;
}

/* The next refill keeps the view where it is (a snapshot read in the
 * background, a new scale): the row on top of the view stays on top, as far
 * above the view's top as it starts now, whatever rows are added or removed
 * above it (gone, the row below it takes its place); at the very top, the
 * view stays there, where new rows show. */
static void RememberTopRow(void)
{
    TVHITTESTINFO hit;
    RECT rc;
    POINT top = { 0, 0 };
    HTREEITEM item;
    g_view.keepPlace = TRUE;
    g_view.topRow.key[0] = g_view.topRowNext.key[0] = 0;
    g_view.topRowOffset = 0;
    MapWindowPoints(g_view.treeArea, g_view.tree, &top, 1);   /* the view's top, in the tree */
    ZeroMemory(&hit, sizeof hit);
    hit.pt = top;
    if ((item = TreeView_HitTest(g_view.tree, &hit)) == NULL || !TreeView_GetItemRect(g_view.tree, item, &rc, FALSE)) return;
    if (item == TreeView_GetRoot(g_view.tree) && top.y <= rc.top) return;
    ItemKey(item, &g_view.topRow);
    g_view.topRowOffset = top.y - rc.top;
    if ((item = TreeView_GetNextVisible(g_view.tree, item)) != NULL) ItemKey(item, &g_view.topRowNext);
}

/* `item` on top of the view again, as far above its top as it was. */
static void PutBackOnTop(HTREEITEM item)
{
    RECT rc;
    if (GetWindowLongW(g_view.tree, GWL_STYLE) & WS_VSCROLL) {
        TreeView_SelectSetFirstVisible(g_view.tree, item);   /* too tall for its view, the tree scrolls itself by rows */
        return;
    }
    /* The tree is as tall as its rows, and less than 32767 px: its view scrolls it. */
    if (TreeView_GetItemRect(g_view.tree, item, &rc, FALSE))
        SendMessageW(g_view.treeArea, WM_VSCROLL,
                     MAKEWPARAM(SB_THUMBPOSITION, (WORD)min(max(0, rc.top + g_view.topRowOffset), SHRT_MAX)), 0);
}

static HTREEITEM AddNode(HTREEITEM parent, const WCHAR *text, LPARAM param)
{
    TVINSERTSTRUCTW ins;
    ZeroMemory(&ins, sizeof ins);
    ins.hParent = parent;
    ins.hInsertAfter = TVI_FIRST;
    ins.itemex.mask = TVIF_TEXT | TVIF_PARAM | TVIF_INTEGRAL;
    ins.itemex.pszText = (LPWSTR)text;
    ins.itemex.lParam = param;
    ins.itemex.iIntegral = param < 0 ? FOLDER_UNITS + GAP_UNITS : SESSION_UNITS;
    return TreeView_InsertItem(g_view.tree, &ins);
}

static void ExpandUnlessFolded(HTREEITEM folder)
{
    TreeNodeKey key;
    if (!folder) return;
    ItemKey(folder, &key);
    if (!WasCollapsed(key.key)) TreeView_Expand(g_view.tree, folder, TVE_EXPAND);
}

/* How many sessions each folder of the tree shows for profile `p`, and the
 * starred ones: counted once per fill, drawn with every folder row. */
static void CountFolders(int p)
{
    int r;
    g_view.starredCount = 0;
    if (g_view.set.groupCount > g_view.folderCountCapacity) {
        size_t bytes = (size_t)g_view.set.groupCount * sizeof(int);
        int *grown = (int *)(g_view.folderCount ? HeapReAlloc(GetProcessHeap(), 0, g_view.folderCount, bytes)
                                                : HeapAlloc(GetProcessHeap(), 0, bytes));
        if (grown) {
            g_view.folderCount = grown;
            g_view.folderCountCapacity = g_view.set.groupCount;
        }
    }
    if (g_view.folderCount) ZeroMemory(g_view.folderCount, (size_t)g_view.folderCountCapacity * sizeof(int));
    if (p < 0) return;
    for (r = 0; r < g_view.set.rowCount; r++) {
        const SessionRow *row = &g_view.set.rows[r];
        if (!RowVisibleIn(row, p)) continue;
        if (row->group < g_view.folderCountCapacity) g_view.folderCount[row->group]++;
        if (StarredIn(EntryOf(row, p))) g_view.starredCount++;
    }
}

/* The sessions a folder node shows; NODE_STARRED: the starred ones. */
static int FolderCount(LPARAM node)
{
    int group = NodeGroup(node);
    if (node == NODE_STARRED) return g_view.starredCount;
    return group >= 0 && group < g_view.folderCountCapacity ? g_view.folderCount[group] : 0;
}

typedef struct RefillMatch {            /* what a refill finds again of the tree before */
    HTREEITEM   selected;               /* the row selected */
    HTREEITEM   sameSession;            /* its session's other row (in its folder or under Starred) */
    HTREEITEM   neighbor[2];            /* the sessions below and above it (g_view.neighbor) */
    HTREEITEM   top;                    /* the row on top of the view */
    HTREEITEM   topNext;                /* the row below it */
} RefillMatch;

/* The sessions just below and just above the selected session's row, taken
 * once from the tree as last filled, while the snapshot it was filled from
 * is still the one shown: its rows name that snapshot's rows. */
static void RememberNeighbors(void)
{
    HTREEITEM selected = TreeView_GetSelection(g_view.tree), item;
    int direction;
    if (g_view.neighborsTaken) return;
    g_view.neighborsTaken = TRUE;
    g_view.neighbor[0].key[0] = g_view.neighbor[1].key[0] = 0;
    if (!selected || NodeParam(selected) < 0) return;
    for (direction = 0; direction < 2; direction++)
        for (item = selected; (item = direction == 0 ? TreeView_GetNextVisible(g_view.tree, item)
                                                     : TreeView_GetPrevVisible(g_view.tree, item)) != NULL;)
            if (NodeParam(item) >= 0) {
                ItemKey(item, &g_view.neighbor[direction]);
                break;
            }
}

/* No neighbor to move to: the next fill shows another profile, or another
 * search, whose rows the previous ones do not follow. */
static void ForgetNeighbors(void)
{
    g_view.neighborsTaken = TRUE;
    g_view.neighbor[0].key[0] = g_view.neighbor[1].key[0] = 0;
}

static void MatchNode(RefillMatch *match, HTREEITEM item, LPARAM node, BOOL inStarred)
{
    TreeNodeKey key;
    int direction;
    NodeKey(node, inStarred, &key);
    if (SameNode(&key, &g_view.selection)) match->selected = item;
    else if (SameSession(&key, &g_view.selection)) match->sameSession = item;
    for (direction = 0; direction < 2; direction++)
        if (SameNode(&key, &g_view.neighbor[direction])) match->neighbor[direction] = item;
    if (SameNode(&key, &g_view.topRow)) match->top = item;
    if (SameNode(&key, &g_view.topRowNext)) match->topNext = item;
}

/* The first session of the first folder open: what the details show while
 * nothing was selected (a folder folded meanwhile stays folded). */
static HTREEITEM FirstShownSession(void)
{
    HTREEITEM folder;
    for (folder = TreeView_GetRoot(g_view.tree); folder; folder = TreeView_GetNextSibling(g_view.tree, folder))
        if (TreeView_GetItemState(g_view.tree, folder, TVIS_EXPANDED) & TVIS_EXPANDED) return TreeView_GetChild(g_view.tree, folder);
    return NULL;
}

/* The tree filled again for the shown profile. The row selected stays
 * selected (its session's other row when it is gone: unstarred, moved; the
 * session below it when its session is gone, else the one above it), else
 * the first session shown; folded folders stay folded. The view keeps its
 * place (RememberTopRow), else shows the selection: the search, the profile
 * or the archived sessions shown changed. */
static void FillTree(void)
{
    WCHAR name[MAX_PATH];
    RefillMatch match;
    TreeNodeKey key;
    HTREEITEM folder = NULL, item, pick;
    int p = ShownProfileIndex(), r, group = -1;
    ZeroMemory(&match, sizeof match);
    RememberNeighbors();   /* a fill from the same snapshot: from the tree as it is */
    g_view.filling = TRUE;
    SendMessageW(g_view.tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_view.tree);
    CountFolders(p);
    if (p >= 0) {
        /* Prepending in reverse order avoids the tree walking the existing
         * siblings for every insertion into a large project. */
        for (r = g_view.set.rowCount - 1; r >= 0; r--) {
            const SessionRow *row = &g_view.set.rows[r];
            if (!RowVisibleIn(row, p)) continue;
            if (row->group != group) {
                ExpandUnlessFolded(folder);
                group = row->group;
                SessionStore_GroupName(&g_view.set, group, name, ARRAYSIZE(name));
                folder = AddNode(TVI_ROOT, name, -2 - group);
                MatchNode(&match, folder, -2 - group, FALSE);
            }
            item = AddNode(folder, ShownTitle(row, p), r);
            MatchNode(&match, item, r, FALSE);
        }
        ExpandUnlessFolded(folder);
        if (g_view.starredCount) {
            folder = AddNode(TVI_ROOT, StarredName(), NODE_STARRED);
            MatchNode(&match, folder, NODE_STARRED, FALSE);
            for (r = g_view.set.rowCount - 1; r >= 0; r--) {
                const SessionRow *row = &g_view.set.rows[r];
                if (!RowVisibleIn(row, p) || !StarredIn(EntryOf(row, p))) continue;
                item = AddNode(folder, ShownTitle(row, p), r);
                MatchNode(&match, item, r, TRUE);
            }
            ExpandUnlessFolded(folder);
        }
        if (TreeView_GetRoot(g_view.tree)) {
            TVITEMEXW top;
            ZeroMemory(&top, sizeof top);
            top.mask = TVIF_INTEGRAL;
            top.hItem = TreeView_GetRoot(g_view.tree);
            top.iIntegral = FOLDER_UNITS;
            SendMessageW(g_view.tree, TVM_SETITEMW, 0, (LPARAM)&top);
        }
    }
    pick = match.selected ? match.selected : match.sameSession ? match.sameSession :
           match.neighbor[0] ? match.neighbor[0] : match.neighbor[1] ? match.neighbor[1] : FirstShownSession();
    ZeroMemory(&key, sizeof key);
    if (pick) {
        HTREEITEM parent = TreeView_GetParent(g_view.tree, pick);
        TreeView_SelectItem(g_view.tree, pick);   /* opens the folder it is in, if folded */
        if (parent) {
            ItemKey(parent, &key);
            SetCollapsed(key.key, FALSE);
        }
        ItemKey(pick, &key);
    }
    SetSelection(&key);
    SendMessageW(g_view.tree, WM_SETREDRAW, TRUE, 0);   /* the view takes the tree's new height */
    /* At the very top, the view stays there: a tree too tall for its view
     * has scrolled itself to the selection. */
    if (match.top) {
        PutBackOnTop(match.top);
    } else if (match.topNext) {
        g_view.topRowOffset = 0;
        PutBackOnTop(match.topNext);
    } else if (g_view.keepPlace && !g_view.topRow.key[0] && TreeView_GetRoot(g_view.tree)) {
        PutBackOnTop(TreeView_GetRoot(g_view.tree));
    } else if (pick && !g_view.keepPlace) {
        TreeView_EnsureVisible(g_view.tree, pick);
    }
    g_view.keepPlace = FALSE;
    g_view.topRow.key[0] = g_view.topRowNext.key[0] = 0;
    g_view.neighborsTaken = FALSE;   /* the tree's rows name the snapshot shown again */
    InvalidateRect(g_view.tree, NULL, TRUE);
    g_view.filling = FALSE;
}

/* Drawing and tooltip measurement share the title's space, including the
 * folder count or a session's archived/waiting suffix. */
static RECT TreeTitleBounds(HDC dc, const RECT *row, int level, LPARAM node, WCHAR *tail, size_t cch)
{
    RECT title = *row;
    title.left += (level + 1) * g_view.indent + g_view.pad / 2;
    title.right -= g_view.pad;
    tail[0] = 0;
    if (node < 0) {
        StringCchPrintfW(tail, cch, L"%d", FolderCount(node));
        title.right -= TextWidth(dc, THEME_FONT_TEXT, tail) + g_view.pad;
    } else if (node < g_view.set.rowCount) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[node], ShownProfileIndex());
        StringCchCopyW(tail, cch, entry && entry->pending ? TR(L"  \x00B7  waiting") :
                                    entry && entry->archived ? TR(L"  \x00B7  archived") : L"");
        title.right -= TextWidth(dc, THEME_FONT_TEXT, tail);
    }
    return title;
}

/* One row of the tree, in the theme's rows: folders in the heading font
 * with their count in gray, sessions in the text font; in gray a session
 * with no conversation on disk, archived, or with a change waiting. The
 * keyboard's row has the focus cue. */
static void DrawTreeItem(const NMTVCUSTOMDRAW *customDraw)
{
    WCHAR tail[64], name[MAX_PATH];
    HDC dc = customDraw->nmcd.hdc;
    HTREEITEM item = (HTREEITEM)customDraw->nmcd.dwItemSpec;
    LPARAM node = customDraw->nmcd.lItemlParam;
    UINT itemState = TreeView_GetItemState(g_view.tree, item, TVIS_SELECTED | TVIS_EXPANDED), state = 0;
    BOOL hot = (customDraw->nmcd.uItemState & CDIS_HOT) != 0;
    COLORREF text, muted, field = Theme_Color(THEME_FIELD);
    RECT rc = customDraw->nmcd.rc, cell, title;
    int p = ShownProfileIndex(), left, right, baseline;

    if (itemState & TVIS_SELECTED) state |= THEME_ROW_SELECTED;
    if (hot) state |= THEME_ROW_HOT;
    /* A folder's room above it stays blank, out of its selection. */
    if (node < 0 && rc.bottom - rc.top > FOLDER_UNITS * g_view.unit) {
        RECT room = rc;
        room.bottom = rc.top = rc.bottom - FOLDER_UNITS * g_view.unit;
        Theme_DrawRow(g_view.tree, dc, &room, 0, field);
    }
    text = Theme_DrawRow(g_view.tree, dc, &rc, state, field);
    muted = Theme_RowMuted(state);
    SetBkMode(dc, TRANSPARENT);
    cell = rc;
    cell.left = rc.left + customDraw->iLevel * g_view.indent;
    cell.right = cell.left + g_view.indent;
    right = rc.right - g_view.pad;
    title = TreeTitleBounds(dc, &rc, customDraw->iLevel, node, tail, ARRAYSIZE(tail));

    if (node < 0) {
        if (node == NODE_STARRED) StringCchCopyW(name, ARRAYSIZE(name), StarredName());
        else SessionStore_GroupName(&g_view.set, NodeGroup(node), name, ARRAYSIZE(name));
        Theme_DrawTreeGlyph(g_view.tree, dc, &cell, state, (itemState & TVIS_EXPANDED) != 0);
        baseline = CenteredBaseline(&rc, THEME_FONT_HEADING);
        left = TextAt(dc, THEME_FONT_HEADING, text, name, title.left, title.right, baseline);
        TextAt(dc, THEME_FONT_TEXT, muted, tail, left + g_view.pad, right, baseline);
    } else if (node < g_view.set.rowCount) {
        const SessionRow *row = &g_view.set.rows[node];
        const SessionEntry *entry = EntryOf(row, p);
        baseline = CenteredBaseline(&rc, THEME_FONT_TEXT);
        left = TextAt(dc, THEME_FONT_TEXT, !row->transcript || (entry && (entry->archived || entry->pendingRemove)) ? muted : text,
                      ShownTitle(row, p), title.left, title.right, baseline);
        if (tail[0]) TextAt(dc, THEME_FONT_TEXT, muted, tail, left, right, baseline);
    }
    if (item == TreeView_GetSelection(g_view.tree) && GetFocus() == g_view.tree) Theme_DrawFocusCue(g_view.tree, dc, &rc);
}

/* Why the tree is empty. */
static void EmptyText(WCHAR *out, size_t cch)
{
    int p = ShownProfileIndex(), r, listed = 0, archived = 0;
    const SessionSource *source;
    const WCHAR *name;
    if (!g_view.loaded && g_view.loadFailed) {
        StringCchCopyW(out, cch, TR(L"Sessions could not be loaded."));
        return;
    }
    if (!g_view.loaded && g_view.loading) {
        StringCchCopyW(out, cch, TR(L"Loading sessions\x2026"));
        return;
    }
    if (p < 0) {
        StringCchCopyW(out, cch, TR(L"No profile yet."));
        return;
    }
    source = &g_view.set.source[p];
    name = ProfileAt(p)->name;
    for (r = 0; r < g_view.set.rowCount; r++) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[r], p);
        if (!entry) continue;
        listed++;
        if (entry->archived) archived++;
    }
    if (listed && g_view.filter[0])
        StringCchPrintfW(out, cch, TR(L"No session of \x201C%s\x201D matches \x201C%s\x201D."), name, g_view.filter);
    else if (listed && archived == listed && !g_view.showArchived)
        StringCchPrintfW(out, cch, TR(L"All the sessions of \x201C%s\x201D are archived.\nCheck Show archived to see them."), name);
    else if (source->elsewhere)
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D only has sessions run over SSH, in WSL or in the cloud.\n"
                                      L"Their conversation is not on this PC, so they are not listed."), name);
    else if (source->unreadable)
        StringCchPrintfW(out, cch, TR(L"The sessions of \x201C%s\x201D could not be read."), name);
    else if (!source->signedIn)
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D is not signed in to Claude yet.\n"
                                      L"Once it is, the sessions started in its Code tab show here."), name);
    else
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D has no Claude Code sessions yet.\n"
                                      L"The sessions started in its Code tab show here."), name);
}

static void DrawEmptyTree(HDC dc)
{
    WCHAR text[512];
    RECT rc;
    HGDIOBJ old = SelectObject(dc, Font(THEME_FONT_TEXT));
    GetClientRect(g_view.tree, &rc);
    InflateRect(&rc, -g_view.pad * 3, -g_view.pad * 3);
    EmptyText(text, ARRAYSIZE(text));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Theme_Color(THEME_MUTED));
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | Localize_ReadingFlags());
    SelectObject(dc, old);
}

static LRESULT TreeCustomDraw(NMTVCUSTOMDRAW *customDraw)
{
    switch (customDraw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT:
        /* An empty rectangle requests the row's font and title bounds for
         * label and tooltip measurement, without drawing it. */
        if (IsRectEmpty(&customDraw->nmcd.rc)) {
            RECT row = { 0 };
            WCHAR tail[64];
            HTREEITEM parent;
            int level = 0;
            SelectObject(customDraw->nmcd.hdc, Font(customDraw->nmcd.lItemlParam < 0 ? THEME_FONT_HEADING : THEME_FONT_TEXT));
            if (GetClientRect(g_view.tree, &row)) {
                for (parent = TreeView_GetParent(g_view.tree, (HTREEITEM)customDraw->nmcd.dwItemSpec); parent;
                     parent = TreeView_GetParent(g_view.tree, parent)) level++;
                customDraw->nmcd.rc = TreeTitleBounds(customDraw->nmcd.hdc, &row, level, customDraw->nmcd.lItemlParam, tail, ARRAYSIZE(tail));
            }
            return CDRF_NEWFONT;
        }
        DrawTreeItem(customDraw);
        return CDRF_SKIPDEFAULT;
    case CDDS_POSTPAINT:
        if (!TreeView_GetCount(g_view.tree)) DrawEmptyTree(customDraw->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* --------------------------------------------------------------- details */

/* What the shown profile's sessions lack: sessions not listed, or no
 * conversation on disk at all. */
static int Notes(WCHAR notes[][NOTE_CCH], int max)
{
    WCHAR projects[MAX_PATH];
    int p = ShownProfileIndex(), n = 0;
    const SessionSource *source = p >= 0 ? &g_view.set.source[p] : NULL;
    if (g_view.set.noTranscripts && n < max && SessionStore_ProjectsDir(projects, ARRAYSIZE(projects))) {
        StringCchPrintfW(notes[n], ARRAYSIZE(notes[n]),
                         TR(L"Claude Code's folder %s was not found: no session has its conversation on this PC."), projects);
        n++;
    }
    if (source && source->elsewhere && n < max) {
        StringCchPrintfW(notes[n], ARRAYSIZE(notes[n]), source->elsewhere == 1
                             ? TR(L"%d session run over SSH, in WSL or in the cloud is not listed: its conversation is not on this PC.")
                             : TR(L"%d sessions run over SSH, in WSL or in the cloud are not listed: their conversation is not on this PC."),
                         source->elsewhere);
        n++;
    }
    if (source && source->unreadable && n < max) {
        StringCchPrintfW(notes[n], ARRAYSIZE(notes[n]), source->unreadable == 1
                             ? TR(L"%d session entry could not be read: that session may be missing here.")
                             : TR(L"%d session entries could not be read: those sessions may be missing here."),
                         source->unreadable);
        n++;
    }
    return n;
}

/* The notes (Notes) from `y`, in gray; returns the bottom. */
static int DrawNotes(HDC dc, int left, int right, int y)
{
    WCHAR notes[3][NOTE_CCH];
    int count = Notes(notes, ARRAYSIZE(notes)), i;
    for (i = 0; i < count; i++) y = Paragraph(dc, THEME_FONT_TEXT, Theme_Color(THEME_MUTED), notes[i], left, right, y + g_view.pad, 4);
    return y;
}

/* The profiles of `bits` by name: "Personal", "Personal and Work",
 * "Personal, Work and Test". */
static void NamesOf(DWORD bits, WCHAR *out, size_t cch)
{
    int p, total = BitCount(bits), seen = 0;
    out[0] = 0;
    for (p = 0; p < g_view.set.profiles.count; p++) {
        if (!(bits & (1u << p))) continue;
        if (seen) StringCchCatW(out, cch, seen == total - 1 ? TR(L" and ") : TR(L", "));
        StringCchCatW(out, cch, ProfileAt(p)->name);
        seen++;
    }
}

/* The place of an Actions box at the right end of `line`, in its middle: the
 * details' controls are as high as a button, and the boxes all as wide, down
 * the right edge (MeasureFonts). */
static RECT ActionsBoxAt(const RECT *line)
{
    RECT box;
    box.left = line->right - g_view.actionsWidth;
    box.top = line->top + (line->bottom - line->top - g_view.controlHeight) / 2;
    box.right = line->right;
    box.bottom = box.top + g_view.controlHeight;
    return box;
}

static ChipSet *ChipsOf(HWND window)
{
    return window == g_view.parts ? &g_view.scrolled : &g_view.fixed;
}

/* The details and their part that scrolls, drawn again. */
static void RedrawDetails(BOOL commit)
{
    UINT flags = RDW_INVALIDATE | RDW_ALLCHILDREN;
    if (commit) flags |= RDW_UPDATENOW;
    RedrawWindow(g_view.details, NULL, NULL, flags);
}

static BOOL IsChip(const ChipRef *ref, HWND owner, Action action, int profile)
{
    return ref->window == owner && ref->action == action && ref->profile == profile;
}

static BOOL SameChip(const ChipRef *one, const ChipRef *other)
{
    return one->window == other->window && (!one->window || (one->action == other->action && one->profile == other->profile));
}

/* A control of the details drawn in `owner` at `rc`: a button, or
 * (ACT_MENU) the Actions box of `profile`, drawn pressed while its menu
 * shows; the folder's button (ACT_SHOW_FOLDER) shows its path on its left.
 * One scrolled out of `owner` is not there to click. */
static void DrawChip(HWND owner, HDC dc, Action action, int profile, const WCHAR *text, const RECT *rc)
{
    ChipSet *set = ChipsOf(owner);
    RECT client;
    int i = set->count;
    UINT state = 0;
    GetClientRect(owner, &client);
    if (i >= MAX_CHIPS || !IntersectRect(&set->chip[i].rc, rc, &client)) return;
    set->chip[i].action = action;
    set->chip[i].profile = profile;
    if (IsChip(&g_view.open, owner, action, profile) ||
        (IsChip(&g_view.hot, owner, action, profile) && IsChip(&g_view.pressed, owner, action, profile)))
        state = THEME_BUTTON_PRESSED;
    else if (IsChip(&g_view.hot, owner, action, profile))
        state = THEME_BUTTON_HOT;
    if (action == ACT_MENU) {
        Theme_DrawDropDown(owner, dc, rc, text, DialogFont(), state);
    } else if (action == ACT_SHOW_FOLDER) {
        RECT label = *rc;
        InflateRect(&label, -g_view.pad - g_view.pad / 2, 0);
        Theme_DrawButton(owner, dc, rc, L"", DialogFont(), state, DT_SINGLELINE);
        TextLine(dc, THEME_FONT_TEXT, GetTextColor(dc), text, &label, DT_PATH_ELLIPSIS);   /* in the color the button's label takes */
    } else {
        Theme_DrawButton(owner, dc, rc, text, DialogFont(), state, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    set->count++;
}

/* A line between two parts of the details, with room above and below;
 * returns where the next part starts. */
static int DrawSeparator(HDC dc, int left, int right, int y)
{
    RECT line;
    SetRect(&line, left, y + g_view.pad, right, y + g_view.pad + 1);
    FillRect(dc, &line, Theme_Brush(THEME_SEPARATOR));
    return line.bottom + g_view.pad;
}

/* One profile's part of the details. On its first line its badge, its name
 * (underlined for the profile shown, struck out where the session is not
 * listed), a star where it is starred, and its Actions box; below, its
 * title there when it is not the one the tree shows, what is going on there
 * and the changes waiting. */
static int DrawProfilePart(HDC dc, const SessionRow *row, int p, int left, int right, int y)
{
    WCHAR text[512];
    const SessionEntry *entry = EntryOf(row, p);
    const Profile *profile = ProfileAt(p);
    int shown = ShownProfileIndex();
    ThemeFont role = !entry ? THEME_FONT_ABSENT : p == shown ? THEME_FONT_CURRENT : THEME_FONT_STRONG;
    COLORREF color = Theme_Color(THEME_TEXT), muted = Theme_Color(THEME_MUTED);
    RECT line = { left, y, right, y + max(g_view.controlHeight, LineHeight(THEME_FONT_STRONG)) }, box = ActionsBoxAt(&line);
    int inner = left + g_view.badgeSize + g_view.pad, star = StarredIn(entry) ? TextWidth(dc, THEME_FONT_TEXT, L" \x2605") : 0, baseline, end;

    if (g_view.badges) ImageList_Draw(g_view.badges, p, dc, left, line.top + (line.bottom - line.top - g_view.badgeSize) / 2, ILD_NORMAL);
    baseline = CenteredBaseline(&line, role);
    end = TextAt(dc, role, entry ? color : muted, profile->name, inner, box.left - g_view.pad - star, baseline);
    if (star) TextAt(dc, THEME_FONT_TEXT, color, L" \x2605", end, box.left - g_view.pad, baseline);
    DrawChip(g_view.parts, dc, ACT_MENU, p, TR(Theme_SessionsCaption(SESSIONS_ACTIONS)), &box);
    y = line.bottom;
    if (!entry) return y;
    if (p != shown && wcscmp(TitleIn(row, p), TitleIn(row, shown)) != 0)
        y = Paragraph(dc, THEME_FONT_ITALIC, entry->archived || entry->pendingRemove ? muted : color, ShownTitle(row, p), inner, right, y, 2);
    text[0] = 0;
    if (profile->running) StringCchCatW(text, ARRAYSIZE(text), TR(L"open"));
    if (row->live & (1u << p)) StringCchCatW(text, ARRAYSIZE(text), text[0] ? TR(L" \x00B7 in use") : TR(L"in use"));
    if (entry->archived) StringCchCatW(text, ARRAYSIZE(text), text[0] ? TR(L" \x00B7 archived") : TR(L"archived"));
    if (text[0]) y = Paragraph(dc, THEME_FONT_TEXT, muted, text, inner, right, y, 1);
    if (entry->pending) {
        StringCchPrintfW(text, ARRAYSIZE(text), entry->pendingRemove ? TR(L"Removed when %s closes") : TR(L"Changes made when %s closes"),
                         profile->name);
        y = Paragraph(dc, THEME_FONT_TEXT, muted, text, inner, right, y, 2);
    }
    return y;
}

/* How far the profiles' parts scroll: `content` px to show in `view` px. The
 * scroll bar shows only when they do not fit. FALSE when they must be drawn
 * again: the place shown moved (the content got shorter), or the scroll bar
 * came or went (the width changed). */
static BOOL SetDetailsScroll(int content, int view)
{
    SCROLLINFO si;
    int last = max(0, content - view);
    BOOL had = (GetWindowLongW(g_view.parts, GWL_STYLE) & WS_VSCROLL) != 0;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMax = content > view ? content - 1 : 0;
    si.nPage = content > view ? (UINT)view : 0;
    si.nPos = min(g_view.scroll, last);
    SetScrollInfo(g_view.parts, SB_VERT, &si, TRUE);
    if (si.nPos == g_view.scroll && had == ((GetWindowLongW(g_view.parts, GWL_STYLE) & WS_VSCROLL) != 0)) return TRUE;
    g_view.scroll = si.nPos;
    return FALSE;
}

/* The profiles' parts scrolled by a scroll bar action (WM_VSCROLL), or the
 * smooth wheel's SB_THUMBPOSITION. */
static void ScrollDetails(WPARAM wp)
{
    SCROLLINFO si;
    int pos = Theme_ScrollTarget(g_view.parts, wp, LineHeight(THEME_FONT_TEXT));
    if (pos == g_view.scroll) return;
    g_view.scroll = pos;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(g_view.parts, SB_VERT, &si, TRUE);
    if (g_view.hot.window == g_view.parts) g_view.hot.window = NULL;   /* the controls moved under the mouse */
    InvalidateRect(g_view.parts, NULL, FALSE);
}

/* The selected session's folder as Explorer opens it, "" when it is not on
 * disk: looked up once per working folder selected, not at every paint, for
 * every session of a folder nor with every snapshot (a folder on a share
 * that went away answers slowly). */
static const WCHAR *SelectedFolder(void)
{
    int r = SelectedRow();
    const WCHAR *cwd = r >= 0 ? g_view.set.rows[r].cwd : L"";
    if (g_view.folderLookedUp && wcscmp(cwd, g_view.folderLookedUpFor) == 0) return g_view.selectedFolder;
    g_view.folderLookedUp = TRUE;
    StringCchCopyW(g_view.folderLookedUpFor, ARRAYSIZE(g_view.folderLookedUpFor), cwd);
    g_view.selectedFolder[0] = 0;
    if (cwd[0] && (!SessionStore_WorkingDir(&g_view.set, cwd, g_view.selectedFolder, ARRAYSIZE(g_view.selectedFolder)) ||
                   !Util_DirExists(g_view.selectedFolder)))
        g_view.selectedFolder[0] = 0;
    return g_view.selectedFolder;
}

/* The selected session's details that stay: from the tree's top (the
 * search box's row above stays empty), its folder's path (a button that
 * shows the folder), when it was last used, how big its conversation is, a
 * warning when it is open in two profiles at once; at the bottom, Delete
 * session everywhere. Between their two lines, the part that scrolls
 * (DrawParts) is placed. With no session selected: why, and the notes. */
static void DrawDetails(const DRAWITEMSTRUCT *item)
{
    WCHAR text[4096], date[64], time[32], names[NAMES_CCH];
    ThemeBuffer buffer;
    RECT rc = item->rcItem, line, remove, tree = ChildRect(g_view.treeArea), self = ChildRect(g_view.details), was;
    int r = SelectedRow(), left = rc.left, right = rc.right, y = rc.top, top, bottom;
    const SessionRow *row = r >= 0 ? &g_view.set.rows[r] : NULL;
    HDC dc = Theme_BufferBegin(&buffer, item->hDC, &rc);
    COLORREF color = Theme_Color(THEME_TEXT), muted = Theme_Color(THEME_MUTED);

    g_view.fixed.count = 0;
    FillRect(dc, &rc, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    if (!row) {
        ShowWindow(g_view.parts, SW_HIDE);
        if (TreeView_GetCount(g_view.tree))
            y = Paragraph(dc, THEME_FONT_TEXT, muted, TR(L"Select a session to see it in each profile."), left, right, y, 3);
        DrawNotes(dc, left, right, y);
        Theme_BufferEnd(&buffer);
        return;
    }
    y = rc.top + tree.top - self.top;
    if (row->cwd[0]) {
        SetRect(&line, left, y, right, y + g_view.controlHeight);
        if (SelectedFolder()[0]) {
            line.right = min(right, left + TextWidth(dc, THEME_FONT_TEXT, row->cwd) + 3 * g_view.pad);
            DrawChip(g_view.details, dc, ACT_SHOW_FOLDER, -1, row->cwd, &line);
        } else {
            TextLine(dc, THEME_FONT_TEXT, muted, row->cwd, &line, DT_PATH_ELLIPSIS);   /* gone: nothing to show */
        }
        y = line.bottom + g_view.pad / 2;
    }
    if (FormatWhen(row->lastActivity, date, ARRAYSIZE(date), time, ARRAYSIZE(time))) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s  -  %s", date, time);
        y = Paragraph(dc, THEME_FONT_TEXT, muted, text, left, right, y, 1);
    }
    if (row->transcript) FormatSize(row->transcriptBytes, text, ARRAYSIZE(text));
    else StringCchCopyW(text, ARRAYSIZE(text), TR(L"No conversation on disk"));
    y = Paragraph(dc, THEME_FONT_TEXT, muted, text, left, right, y, 1);
    if (BitCount(row->live) >= 2) {
        NamesOf(row->live, names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text), BitCount(row->live) > 2
                         ? TR(L"Open in %s at once: go on in one of them only. The others keep an older copy until the session is closed there.")
                         : TR(L"Open in %s at once: go on in one of them only. "
                              L"The other one keeps an older copy until the session is closed there."),
                         names);
        y = Paragraph(dc, THEME_FONT_STRONG, color, text, left, right, y + g_view.pad, 6);
    }
    top = DrawSeparator(dc, left, right, y) - g_view.pad;   /* under its line */
    SetRect(&remove, left, rc.bottom - g_view.controlHeight, right, rc.bottom);
    bottom = remove.top - g_view.pad - 1;                  /* over the bottom line */
    DrawSeparator(dc, left, right, bottom - g_view.pad);
    DrawChip(g_view.details, dc, ACT_DELETE_ALL, -1, TR(Theme_SessionsCaption(SESSIONS_DELETE_EVERYWHERE)), &remove);
    Theme_BufferEnd(&buffer);

    /* The part that scrolls, between the two lines. */
    if (!g_view.parts || !GetWindowRect(g_view.parts, &was)) return;
    MapWindowPoints(NULL, g_view.details, (POINT *)&was, 2);
    if (was.left != left || was.top != top || was.right != right || was.bottom != max(top, bottom) || !IsWindowVisible(g_view.parts))
        SetWindowPos(g_view.parts, NULL, left, top, right - left, max(0, bottom - top), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* Each profile's part of the selected session, then the notes: what scrolls
 * between the details' two lines. */
static void DrawParts(const DRAWITEMSTRUCT *item)
{
    ThemeBuffer buffer;
    RECT rc = item->rcItem;
    int r = SelectedRow(), p, left = rc.left, right = rc.right, top = rc.top - g_view.scroll, y = top + g_view.pad * FIRST_PART_PAD_THIRDS / 3;
    const SessionRow *row = r >= 0 ? &g_view.set.rows[r] : NULL;
    HDC dc = Theme_BufferBegin(&buffer, item->hDC, &rc);

    g_view.scrolled.count = 0;
    FillRect(dc, &rc, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    if (row)
        for (p = 0; p < g_view.set.profiles.count; p++) y = DrawProfilePart(dc, row, p, left, right, p == 0 ? y : DrawSeparator(dc, left, right, y));
    y = DrawNotes(dc, left, right, y);
    Theme_BufferEnd(&buffer);
    if (!SetDetailsScroll(y - top + g_view.pad, rc.bottom - rc.top)) InvalidateRect(g_view.parts, NULL, FALSE);
}

/* The control of `window` at `pt`, and its rectangle; FALSE (found->window NULL) where there is none. */
static BOOL ChipAt(HWND window, POINT pt, ChipRef *found, RECT *rc)
{
    const ChipSet *set = ChipsOf(window);
    int i;
    found->window = NULL;
    for (i = 0; i < set->count; i++)
        if (PtInRect(&set->chip[i].rc, pt)) {
            found->window = window;
            found->action = set->chip[i].action;
            found->profile = set->chip[i].profile;
            if (rc) *rc = set->chip[i].rc;
            return TRUE;
        }
    return FALSE;
}

static void RunAction(Action action, int p);
static void ProfileMenu(int p, const RECT *box);

/* The controls of the details and of their part that scrolls: under the
 * mouse, pressed, clicked; an Actions box opens its menu as soon as it is
 * pressed, as a drop-down list does. The details also draw that part. */
static LRESULT CALLBACK DetailsSubclass(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    POINT pt;
    ChipRef chip, released;
    RECT box;
    (void)ref;
    switch (msg) {
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT *)lp)->hwndItem == g_view.parts) {
            DrawParts((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        ChipAt(window, pt, &chip, NULL);
        if (!SameChip(&chip, &g_view.hot)) {
            TRACKMOUSEEVENT track = { sizeof track, TME_LEAVE, window, 0 };
            if (g_view.hot.window && g_view.hot.window != window) InvalidateRect(g_view.hot.window, NULL, FALSE);
            g_view.hot = chip;
            TrackMouseEvent(&track);
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (g_view.hot.window == window && GetCapture() != window) {
            g_view.hot.window = NULL;
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        g_view.pressed.window = NULL;   /* a press anywhere ends any press before */
        if (ChipAt(window, pt, &chip, &box) && chip.action == ACT_MENU) {
            ProfileMenu(chip.profile, &box);
        } else if (chip.window) {
            g_view.pressed = chip;
            g_view.pressedSelection = g_view.selection;
            SetCapture(window);
        }
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        released = g_view.pressed;
        g_view.pressed.window = NULL;
        if (GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, NULL, FALSE);
        /* Run only over the control pressed, for the session selected then:
         * a snapshot may have drawn the details again meanwhile. */
        if (released.window == window && ChipAt(window, pt, &chip, NULL) && SameChip(&chip, &released) &&
            SameSession(&g_view.pressedSelection, &g_view.selection)) {
            g_view.hot.window = NULL;
            RunAction(released.action, released.profile);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_view.pressed.window == window) {
            g_view.pressed.window = NULL;
            InvalidateRect(window, NULL, FALSE);
        }
        break;
    case WM_VSCROLL:
        if (window == g_view.parts) ScrollDetails(wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* drawn whole, off screen */
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, DetailsSubclass, id);
        break;
    }
    return DefSubclassProc(window, msg, wp, lp);
}

/* --------------------------------------------------------------- actions */

typedef struct TitleDialog {
    const WCHAR *profile;
    WCHAR        title[SESSION_TITLE_CCH];
} TitleDialog;

/* The title typed in the dialog, without the blanks around it: where it
 * starts in `typed`, and how long it is (0: only blanks). */
static size_t TypedTitle(HWND dialog, WCHAR *typed, size_t cch, const WCHAR **start)
{
    size_t length;
    GetDlgItemTextW(dialog, IDC_T_TITLE, typed, (int)cch);
    for (*start = typed; **start == L' ' || **start == L'\t'; (*start)++) {}
    for (length = wcslen(*start); length > 0 && ((*start)[length - 1] == L' ' || (*start)[length - 1] == L'\t'); length--) {}
    return length;
}

static INT_PTR CALLBACK TitleProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    TitleDialog *request = (TitleDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    WCHAR label[128], typed[SESSION_TITLE_CCH];
    const WCHAR *start;
    size_t length;
    switch (msg) {
    case WM_INITDIALOG:
        request = (TitleDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(label, ARRAYSIZE(label), TR(L"Title in \x201C%s\x201D:"), request->profile);
        SetDlgItemTextW(dialog, IDC_T_LABEL, label);
        SendDlgItemMessageW(dialog, IDC_T_TITLE, EM_LIMITTEXT, SESSION_TITLE_CCH - 1, 0);
        SetDlgItemTextW(dialog, IDC_T_TITLE, request->title);
        SendDlgItemMessageW(dialog, IDC_T_TITLE, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, IDC_T_TITLE));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_T_TITLE && HIWORD(wp) == EN_CHANGE) {
            EnableWindow(GetDlgItem(dialog, IDOK), TypedTitle(dialog, typed, ARRAYSIZE(typed), &start) > 0);   /* only blanks: no title */
        } else if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            if (LOWORD(wp) == IDOK) {
                if ((length = TypedTitle(dialog, typed, ARRAYSIZE(typed), &start)) == 0) return TRUE;
                StringCchCopyNW(request->title, ARRAYSIZE(request->title), start, length);
            }
            EndDialog(dialog, LOWORD(wp));
        }
        return TRUE;
    }
    return FALSE;
}

/* "Work opens it now" with what happens first when it is closed. */
static void OpensNow(int p, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, ProfileAt(p)->running ? TR(L"%s opens it now") : TR(L"%s starts and opens it"), ProfileAt(p)->name);
}

static BOOL CanOpenConversation(const SessionRow *row, int target)
{
    if (!row->transcript || !Core_IsUuid(row->key)) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"This session has no conversation on disk yet: "
                                                      L"it can only be opened from the Code tab of the profile where it was started."));
        return FALSE;
    }
    if (target >= 0 && !g_view.set.source[target].signedIn) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."),
                   ProfileAt(target)->name);
        return FALSE;
    }
    if (!Package()->found) {
        Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"Claude Desktop is not installed."));
        return FALSE;
    }
    return TRUE;
}

/* Session `key` opened in profile `p` by its Claude (started first when
 * closed); FALSE, the user told, when it could not be. */
static BOOL OpenConversationIn(int p, const WCHAR *key)
{
    HRESULT hr = SessionEdit_Open(Package(), ProfileAt(p), key);
    if (SUCCEEDED(hr)) return TRUE;
    Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be opened in \x201C%s\x201D (error 0x%08lX)."), ProfileAt(p)->name,
               (unsigned long)hr);
    return FALSE;
}

static void MakeEdit(PendingEdit *edit, PendingOp op, const WCHAR *key, const WCHAR *value)
{
    ZeroMemory(edit, sizeof *edit);
    edit->op = op;
    StringCchCopyW(edit->key, ARRAYSIZE(edit->key), key);
    StringCchCopyW(edit->value, ARRAYSIZE(edit->value), value);
}

/* `reason`: why, when SessionEdit says ("" when it does not). */
static void TellNotChanged(int p, const WCHAR *reason)
{
    if (reason[0]) Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be changed in \x201C%s\x201D. %s"), ProfileAt(p)->name, reason);
    else Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be changed in \x201C%s\x201D."), ProfileAt(p)->name);
}

/* The title session `key` keeps in `target` once Claude lists it there:
 * queued in `carried` (its key "" when none is: an untitled session, or a
 * failure the user is told about). */
static void CarryTitle(int target, const WCHAR *key, const WCHAR *title, PendingEdit *carried)
{
    BOOL waiting;
    ZeroMemory(carried, sizeof *carried);
    if (!title[0]) return;
    MakeEdit(carried, PENDING_TITLE, key, title);
    if (SessionEdit_Change(g_view.dlg, ProfileAt(target), NULL, carried, &waiting, NULL, 0)) return;
    carried->key[0] = 0;
    Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The session's title could not be set in \x201C%s\x201D."), ProfileAt(target)->name);
}

/* Session `key` opened in `target` with the title it carries; a title
 * carried for a session that did not open waits for nothing. FALSE, the
 * user told, when it did not open. */
static BOOL OpenWithTitle(int target, const WCHAR *key, const WCHAR *title)
{
    PendingEdit carried;
    CarryTitle(target, key, title, &carried);
    if (OpenConversationIn(target, key)) return TRUE;
    if (carried.key[0]) SessionEdit_Cancel(ProfileAt(target), &carried, 1);
    return FALSE;
}

/* The end of the Share question when the session is open somewhere else:
 * shared with `target`, it opens there too, so in several profiles at once. */
static void SharedOpenWarning(const SessionRow *row, int target, WCHAR *out, size_t cch)
{
    WCHAR names[NAMES_CCH];
    DWORD open = row->live | (1u << target);
    out[0] = 0;
    if (BitCount(open) < 2) return;
    NamesOf(open, names, ARRAYSIZE(names));
    StringCchPrintfW(out, cch, BitCount(open) > 2
                     ? TR(L"\n\nOnce shared, it is open in %s at once: go on in one of them only. "
                          L"The others keep an older copy until the session is closed there.")
                     : TR(L"\n\nOnce shared, it is open in %s at once: go on in one of them only. "
                          L"The other one keeps an older copy until the session is closed there."),
                     names);
}

static BOOL OpenSession(int r, int p)
{
    return CanOpenConversation(&g_view.set.rows[r], -1) && OpenConversationIn(p, g_view.set.rows[r].key);
}

/* Share and Copy say when the session's title is set in `target`. An
 * untitled session carries none (CarryTitle): its question ends with how it
 * opens, and its format leaves out the last argument. */
static BOOL ShareSession(int r, int target)
{
    const SessionRow *row = &g_view.set.rows[r];
    const WCHAR *title = TitleIn(row, ShownProfileIndex()), *shown = ShownTitle(row, ShownProfileIndex());
    const WCHAR *folder = wcsrchr(row->cwd, L'\\'), *name = ProfileAt(target)->name;
    WCHAR text[4096], opens[256], warning[NOTE_CCH + NAMES_CCH];
    if (!CanOpenConversation(row, target)) return FALSE;
    OpensNow(target, opens, ARRAYSIZE(opens));
    if (IsScratch(row))
        StringCchPrintfW(text, ARRAYSIZE(text), title[0]
                         ? TR(L"\x201C%s\x201D has no folder. Shared with \x201C%s\x201D, it shows there in a folder named %s, not under No folder, "
                              L"and both profiles go on with the same conversation.\n\nFor a separate copy under No folder in \x201C%s\x201D, use Copy instead.\n\n"
                              L"%s, and its title there is set when %s closes.")
                         : TR(L"\x201C%s\x201D has no folder. Shared with \x201C%s\x201D, it shows there in a folder named %s, not under No folder, "
                              L"and both profiles go on with the same conversation.\n\nFor a separate copy under No folder in \x201C%s\x201D, use Copy instead.\n\n"
                              L"%s."),
                         shown, name, folder ? folder + 1 : row->cwd, name, opens, name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), title[0]
                         ? TR(L"Share \x201C%s\x201D with \x201C%s\x201D?\n\nBoth profiles then go on with the same conversation. "
                              L"%s, and its title there is set when %s closes.")
                         : TR(L"Share \x201C%s\x201D with \x201C%s\x201D?\n\nBoth profiles then go on with the same conversation. %s."),
                         shown, name, opens, name);
    SharedOpenWarning(row, target, warning, ARRAYSIZE(warning));
    StringCchCatW(text, ARRAYSIZE(text), warning);
    if (!Ui_Ask(g_view.dlg, IDI_QUESTION, text, TR(L"Share"), TR(L"Cancel"), FALSE)) return FALSE;
    return OpenWithTitle(target, row->key, title);
}

/* A copy that does not open is listed nowhere: what the copy made goes again. */
static BOOL CopySession(int r, int target)
{
    const SessionRow *row = &g_view.set.rows[r];
    const WCHAR *title = TitleIn(row, ShownProfileIndex()), *shown = ShownTitle(row, ShownProfileIndex()), *name = ProfileAt(target)->name;
    WCHAR text[4096], opens[256], id[SESSION_ID_CCH], error[1024], left[LONG_PATH_CCH];
    CopyResult copied;
    if (!CanOpenConversation(row, target)) return FALSE;
    if (IsScratch(row) && !g_view.set.source[target].scratchDir[0]) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"\x201C%s\x201D has no Claude Code sessions yet: open its Code tab once, then copy again."),
                   name);
        return FALSE;
    }
    OpensNow(target, opens, ARRAYSIZE(opens));
    if (IsScratch(row))
        StringCchPrintfW(text, ARRAYSIZE(text), title[0]
                         ? TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation and of its working folder, "
                              L"under No folder in \x201C%s\x201D. The copy then goes on separately. %s, and its title there is set when %s closes.")
                         : TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation and of its working folder, "
                              L"under No folder in \x201C%s\x201D. The copy then goes on separately. %s."),
                         shown, name, name, name, opens, name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), title[0]
                         ? TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation, which then goes on separately. "
                              L"%s, and its title there is set when %s closes.")
                         : TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation, which then goes on separately. "
                              L"%s."),
                         shown, name, name, opens, name);
    if (!Ui_Ask(g_view.dlg, IDI_QUESTION, text, TR(L"Copy"), TR(L"Cancel"), FALSE)) return FALSE;
    copied = SessionEdit_CopyConversation(g_view.dlg, &g_view.set, r, target, id, ARRAYSIZE(id), error, ARRAYSIZE(error));
    if (copied == COPY_FAILED) Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be copied. %s"), error);
    if (copied != COPY_MADE) return FALSE;   /* cancelled in Windows' progress: nothing to say */
    if (!OpenWithTitle(target, id, title) && !SessionEdit_RemoveCopy(&g_view.set, r, target, id, left, ARRAYSIZE(left)) && left[0])
        Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The copy could not be opened, and its files could not all be deleted: %s"), left);
    return TRUE;
}

/* A change to the entry of profile `p`, made now or when it closes; FALSE,
 * the user told, when it could not be. */
static BOOL ChangeEntry(int r, int p, PendingOp op, const WCHAR *value)
{
    PendingEdit edit;
    WCHAR reason[LONG_PATH_CCH + NOTE_CCH];
    BOOL waiting;
    MakeEdit(&edit, op, g_view.set.rows[r].key, value);
    if (SessionEdit_Change(g_view.dlg, ProfileAt(p), EntryOf(&g_view.set.rows[r], p), &edit, &waiting, reason, ARRAYSIZE(reason)))
        return TRUE;
    TellNotChanged(p, reason);
    return FALSE;
}

static BOOL RenameEntry(int r, int p)
{
    TitleDialog request;
    ZeroMemory(&request, sizeof request);
    request.profile = ProfileAt(p)->name;
    StringCchCopyW(request.title, ARRAYSIZE(request.title), TitleIn(&g_view.set.rows[r], p));
    if (Ui_Dialog(g_view.dlg, IDD_TITLE, TitleProc, (LPARAM)&request) != IDOK) return FALSE;
    /* The same title written again would only mark it as chosen: Claude would stop naming the session. */
    if (wcscmp(request.title, TitleIn(&g_view.set.rows[r], p)) == 0) return FALSE;
    return ChangeEntry(r, p, PENDING_TITLE, request.title);
}

static BOOL RemoveEntry(int r, int p)
{
    const SessionRow *row = &g_view.set.rows[r];
    const WCHAR *title = ShownTitle(row, p), *name = ProfileAt(p)->name;
    WCHAR text[4096], names[NAMES_CCH];
    DWORD keeping = ProfilesKeeping(row, p);
    NamesOf(keeping, names, ARRAYSIZE(names));
    if (ProfileAt(p)->running && keeping)
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Remove \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry there goes to the Recycle Bin when %s closes "
                            L"(it keeps it until then). The conversation stays for %s."), title, name, name, names);
    else if (ProfileAt(p)->running)
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Remove \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry there goes to the Recycle Bin when %s closes "
                            L"(it keeps it until then). The conversation stays on disk: Delete session everywhere removes it too."), title, name, name);
    else if (keeping)
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Remove \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry there goes to the Recycle Bin. The conversation stays for %s."),
                         title, name, names);
    else
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Remove \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry there goes to the Recycle Bin. "
                            L"The conversation stays on disk: Delete session everywhere removes it too."), title, name);
    return Ui_Ask(g_view.dlg, IDI_QUESTION, text, TR(L"Remove"), TR(L"Cancel"), FALSE) && ChangeEntry(r, p, PENDING_REMOVE, L"");
}

/* The waiting removal of the entries of profile `p` dropped: queued under
 * the session's id, or under the own id of one of its entries there (made
 * before the session's first message), in one go. */
static BOOL KeepEntry(int r, int p)
{
    const SessionRow *row = &g_view.set.rows[r];
    PendingEdit *edits;
    int entry, count = 1;
    BOOL kept;
    for (entry = row->entry[p]; entry >= 0; entry = g_view.set.entries[entry].duplicate) count++;
    if ((edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *edits)) == NULL) {
        TellNotChanged(p, L"");
        return FALSE;
    }
    count = 0;
    MakeEdit(&edits[count++], PENDING_REMOVE, row->key, L"");
    for (entry = row->entry[p]; entry >= 0; entry = g_view.set.entries[entry].duplicate)
        MakeEdit(&edits[count++], PENDING_REMOVE, g_view.set.entries[entry].localId, L"");
    kept = SessionEdit_Cancel(ProfileAt(p), edits, count);
    HeapFree(GetProcessHeap(), 0, edits);
    if (!kept) TellNotChanged(p, L"");
    return kept;
}

/* Its conversation, its entries and, for a session without a folder, its
 * working folder to the Recycle Bin. Whatever runs it is said before the
 * question; SessionEdit checks again before anything goes. */
static BOOL DeleteSessionEverywhere(int r)
{
    const SessionRow *row = &g_view.set.rows[r];
    const WCHAR *question;
    WCHAR text[4096], names[NAMES_CCH], error[LONG_PATH_CCH + NOTE_CCH], working[MAX_PATH];
    DWORD listing = ProfilesListing(row);
    RemoveResult result;
    BOOL several = BitCount(listing) > 1, folder;
    if (!SessionEdit_CanDelete(&g_view.set, r, error, ARRAYSIZE(error))) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, L"%s", error);
        return FALSE;
    }
    folder = SessionEdit_RemovesWorkingFolder(&g_view.set, r, working, ARRAYSIZE(working));
    NamesOf(listing, names, ARRAYSIZE(names));
    if (row->transcript && folder)
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation, its working folder and its entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation, its working folder and its entry in %s go to the Recycle Bin.");
    else if (row->transcript)
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation and its entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation and its entry in %s go to the Recycle Bin.");
    else if (folder)
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts working folder and its entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts working folder and its entry in %s go to the Recycle Bin.");
    else
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts entry in %s goes to the Recycle Bin.");
    StringCchPrintfW(text, ARRAYSIZE(text), question, ShownTitle(row, ShownProfileIndex()), names);
    if (!Ui_Ask(g_view.dlg, IDI_WARNING, text, TR(L"Delete"), TR(L"Cancel"), TRUE)) return FALSE;
    result = SessionEdit_DeleteEverywhere(g_view.dlg, &g_view.set, r, error, ARRAYSIZE(error));
    if (result == REMOVE_FAILED) Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The session could not be deleted. %s"), error);
    return result != REMOVE_CANCELLED;
}

/* `folder` shown in Explorer; the user told when it cannot be. */
static void ShowFolder(const WCHAR *folder)
{
    INT_PTR result = (INT_PTR)ShellExecuteW(g_view.dlg, L"explore", folder, NULL, NULL, SW_SHOWNORMAL);
    if (result > 32) return;   /* ShellExecute's success */
    Util_Log(L"sessions: %s could not be shown (code %ld)", folder, (long)result);
    Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The folder %s could not be shown (error %ld)."), folder, (long)result);
}

/* Once an action's dialogs and menus have closed: the snapshot they held
 * back is taken through the manager's message, which checks that the
 * manager may still change what it shows (it may be closing). */
static void TakeHeldSnapshot(void)
{
    if (!g_view.actionDepth) PostMessageW(g_view.dlg, WM_APP_SESSIONS_READY, 0, 0);
}

/* An action on the selected session in profile `p` (-1: the profile shown).
 * What it changed is read again at once: a read begun before is out of date.
 * What Claude does once a session opens, the watcher sees. */
static void RunAction(Action action, int p)
{
    int r = SelectedRow();
    BOOL changed = FALSE;
    if (r < 0) return;
    if (p < 0) p = ShownProfileIndex();
    if (p < 0 || p >= g_view.set.profiles.count) return;
    g_view.actionDepth++;
    switch (action) {
    case ACT_OPEN:
        if (ListedAndKept(EntryOf(&g_view.set.rows[r], p))) OpenSession(r, p);
        break;
    case ACT_RENAME:      changed = RenameEntry(r, p); break;
    case ACT_STAR:        changed = ChangeEntry(r, p, PENDING_STAR, StarredIn(EntryOf(&g_view.set.rows[r], p)) ? L"0" : L"1"); break;
    case ACT_REMOVE:      changed = RemoveEntry(r, p); break;
    case ACT_KEEP:        changed = KeepEntry(r, p); break;
    case ACT_SHARE:       changed = ShareSession(r, p); break;
    case ACT_COPY:        changed = CopySession(r, p); break;
    case ACT_SHOW_FOLDER:
        if (SelectedFolder()[0]) ShowFolder(SelectedFolder());
        break;
    case ACT_DELETE_ALL:  changed = DeleteSessionEverywhere(r); break;
    default: break;
    }
    g_view.actionDepth--;
    if (changed) RequestLoad(TRUE);
    TakeHeldSnapshot();
}

/* A profile's name for a menu: an & shown as itself. */
static void EscapeAmpersands(const WCHAR *text, WCHAR *out, size_t cch)
{
    const WCHAR *ampersand;
    out[0] = 0;
    while ((ampersand = wcschr(text, L'&')) != NULL) {
        if (FAILED(StringCchCatNW(out, cch, text, (size_t)(ampersand - text) + 1)) || FAILED(StringCchCatW(out, cch, L"&"))) return;
        text = ampersand + 1;
    }
    StringCchCatW(out, cch, text);
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

/* "Open in Work", the menu's default (`name` as a menu shows it). */
static void AppendOpenItem(HMENU menu, int p, const WCHAR *name)
{
    WCHAR text[LABEL_CCH * 2 + 48];
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"&Open in %s"), name);
    AppendMenuW(menu, MF_STRING, MenuId(ACT_OPEN, p), text);
    SetMenuDefaultItem(menu, MenuId(ACT_OPEN, p), FALSE);
}

/* What can be done to the entry of session `row` in profile `p` (`name` as
 * a menu shows it): rename, star, remove (from that profile when others
 * keep it), or keep it when its removal waits. `keys`: with the keys that do
 * it in the tree. */
static void AppendEntryActions(HMENU menu, const SessionRow *row, int p, const WCHAR *name, BOOL keys)
{
    WCHAR text[LABEL_CCH * 2 + 48];
    const SessionEntry *entry = EntryOf(row, p);
    if (entry->pendingRemove) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"&Keep in %s"), name);
        AppendMenuW(menu, MF_STRING, MenuId(ACT_KEEP, p), text);
        return;
    }
    AppendMenuW(menu, MF_STRING, MenuId(ACT_RENAME, p), keys ? TR(L"&Rename\x2026\tF2") : TR(L"&Rename\x2026"));
    AppendMenuW(menu, MF_STRING, MenuId(ACT_STAR, p), StarredIn(entry) ? TR(L"Uns&tar") : TR(L"S&tar"));
    if (ProfilesKeeping(row, p)) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Re&move from %s\x2026%s"), name, keys ? TR(L"\tDel") : L"");
    else StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Re&move\x2026%s"), keys ? TR(L"\tDel") : L"");
    AppendMenuW(menu, MF_STRING, MenuId(ACT_REMOVE, p), text);
}

/* The menu of the Actions box of profile `p` (`box`, in the part that
 * scrolls), under the box, which shows pressed meanwhile: open, rename,
 * star and remove where the session is listed (keep instead when its
 * removal waits), share and copy where it is not. */
static void ProfileMenu(int p, const RECT *box)
{
    WCHAR name[LABEL_CCH * 2];
    int r = SelectedRow();
    RECT screen = *box;
    const SessionEntry *entry;
    HMENU menu;
    UINT cmd;
    if (r < 0 || p < 0 || p >= g_view.set.profiles.count || (menu = CreatePopupMenu()) == NULL) return;
    g_view.actionDepth++;
    entry = EntryOf(&g_view.set.rows[r], p);
    EscapeAmpersands(ProfileAt(p)->name, name, ARRAYSIZE(name));
    if (!entry) {
        AppendMenuW(menu, MF_STRING, MenuId(ACT_SHARE, p), TR(L"&Share with\x2026"));
        AppendMenuW(menu, MF_STRING, MenuId(ACT_COPY, p), TR(L"&Copy to\x2026"));
    } else {
        if (ListedAndKept(entry)) AppendOpenItem(menu, p, name);
        AppendEntryActions(menu, &g_view.set.rows[r], p, name, FALSE);
    }
    MapWindowPoints(g_view.parts, NULL, (POINT *)&screen, 2);
    g_view.open.window = g_view.parts;
    g_view.open.action = ACT_MENU;
    g_view.open.profile = p;
    RedrawWindow(g_view.parts, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    cmd = Theme_TrackDropDown(g_view.dlg, menu, &screen);
    DestroyMenu(menu);
    g_view.open.window = g_view.hot.window = NULL;
    InvalidateRect(g_view.parts, NULL, FALSE);
    RunCommand(cmd);
    g_view.actionDepth--;
    TakeHeldSnapshot();
}

/* The selected session's actions, at `pt` (screen). A profile whose entry
 * waits for its removal is not one to open it in, nor to share it with. */
static void ShowMenu(POINT pt)
{
    WCHAR text[LABEL_CCH * 2 + 48], name[LABEL_CCH * 2];
    HMENU menu = CreatePopupMenu(), openIn = CreatePopupMenu(), shareWith = CreatePopupMenu(), copyTo = CreatePopupMenu();
    int r = SelectedRow(), p = ShownProfileIndex(), otherProfile, openable = 0, unlisted = 0;
    const SessionRow *row;
    const SessionEntry *entry;
    UINT cmd;
    if (!menu || !openIn || !shareWith || !copyTo || r < 0 || p < 0) {
        if (menu) DestroyMenu(menu);
        if (openIn) DestroyMenu(openIn);
        if (shareWith) DestroyMenu(shareWith);
        if (copyTo) DestroyMenu(copyTo);
        return;
    }
    row = &g_view.set.rows[r];
    entry = EntryOf(row, p);
    EscapeAmpersands(ProfileAt(p)->name, name, ARRAYSIZE(name));
    if (ListedAndKept(entry)) AppendOpenItem(menu, p, name);
    for (otherProfile = 0; otherProfile < g_view.set.profiles.count; otherProfile++) {
        const SessionEntry *otherEntry = EntryOf(row, otherProfile);
        if (otherProfile == p) continue;
        EscapeAmpersands(ProfileAt(otherProfile)->name, text, ARRAYSIZE(text));
        if (ListedAndKept(otherEntry)) {
            AppendMenuW(openIn, MF_STRING, MenuId(ACT_OPEN, otherProfile), text);
            openable++;
        } else if (!otherEntry) {
            AppendMenuW(shareWith, MF_STRING, MenuId(ACT_SHARE, otherProfile), text);
            unlisted++;
        }
        AppendMenuW(copyTo, MF_STRING, MenuId(ACT_COPY, otherProfile), text);
    }
    AppendMenuW(menu, MF_POPUP | (openable ? 0 : MF_GRAYED), (UINT_PTR)openIn, TR(L"Open i&n"));
    AppendMenuW(menu, MF_POPUP | (unlisted ? 0 : MF_GRAYED), (UINT_PTR)shareWith, TR(L"S&hare with"));
    AppendMenuW(menu, MF_POPUP | (g_view.set.profiles.count > 1 ? 0 : MF_GRAYED), (UINT_PTR)copyTo, TR(L"&Copy to"));
    if (entry) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendEntryActions(menu, row, p, name, TRUE);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (SelectedFolder()[0] ? 0 : MF_GRAYED), MenuId(ACT_SHOW_FOLDER, p), TR(L"&Show folder"));
    AppendMenuW(menu, MF_STRING, MenuId(ACT_DELETE_ALL, p), TR(L"&Delete session everywhere\x2026"));
    g_view.actionDepth++;
    cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | (Localize_IsRTL() ? TPM_LAYOUTRTL : 0), pt.x, pt.y, g_view.dlg, NULL);
    DestroyMenu(menu);   /* its submenus with it */
    RunCommand(cmd);
    g_view.actionDepth--;
    TakeHeldSnapshot();
}

/* --------------------------------------------------------------- watcher */

#define WATCH_EVERYTHING (FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE)

typedef struct WatcherParameters {      /* what the watcher thread owns */
    HWND      dlg;
    HANDLE    stop;
    WatchPlan plan;
} WatcherParameters;

typedef struct WatchHandle {
    HWND                 dlg;
    HANDLE               change, wait, stop;
    const WatchedFolder *folder;
    ULONGLONG            state;         /* WatchedState as last told */
} WatchHandle;

static BOOL Probed(const WatchedFolder *folder)
{
    return folder->nearest[0] || folder->file[0];
}

/* What a notification on a probed folder must change to ask for a snapshot:
 * the nearest existing folder of `nearest`, the size and time of `file`. */
static ULONGLONG WatchedState(const WatchedFolder *folder)
{
    WCHAR nearest[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    ULONGLONG state = CORE_HASH_START;
    if (folder->nearest[0]) {
        if (!Util_ExistingDir(folder->nearest, nearest, ARRAYSIZE(nearest))) nearest[0] = 0;
        state = Core_HashText(state, nearest);
    }
    if (folder->file[0]) {
        if (!GetFileAttributesExW(folder->file, GetFileExInfoStandard, &file)) ZeroMemory(&file, sizeof file);
        state = Core_HashBytes(state, &file.ftLastWriteTime, sizeof file.ftLastWriteTime);
        state = Core_HashBytes(state, &file.nFileSizeHigh, sizeof file.nFileSizeHigh);
        state = Core_HashBytes(state, &file.nFileSizeLow, sizeof file.nFileSizeLow);
    }
    return state;
}

/* One WM_APP_SESSIONS for a burst of notifications: RequestLoad clears the flag. */
static void PostReload(HWND dlg)
{
    if (InterlockedExchange(&g_reloadMessagePending, 1) == 0) PostMessageW(dlg, WM_APP_SESSIONS, 0, 0);
}

/* Re-armed first, so a change while it runs notifies again (and a stopping
 * watcher is not called back at once for the same change). A probed folder
 * asks for a snapshot only when what it watches changed: Chromium writes its
 * own files next to config.json all the time. A notification lost stops the
 * watcher; the snapshot it asks for watches again. */
static void CALLBACK WatchChanged(void *param, BOOLEAN timedOut)
{
    WatchHandle *watch = (WatchHandle *)param;
    BOOL rearmed = FindNextChangeNotification(watch->change);
    DWORD error = GetLastError();
    (void)timedOut;
    if (WaitForSingleObject(watch->stop, 0) == WAIT_OBJECT_0) return;
    if (!rearmed) {
        Util_Log(L"sessions: %s is no longer watched (error %lu)", watch->folder->dir, error);
        SetEvent(watch->stop);
    } else if (Probed(watch->folder)) {
        ULONGLONG state = WatchedState(watch->folder);
        if (state == watch->state) return;
        watch->state = state;
    }
    PostReload(watch->dlg);
}

static DWORD WINAPI WatchProc(void *param)
{
    WatcherParameters *parameters = (WatcherParameters *)param;
    WatchHandle handles[WATCH_MAX];
    int i, count = 0;
    ZeroMemory(handles, sizeof handles);
    for (i = 0; i < parameters->plan.count; i++) {
        const WatchedFolder *folder = &parameters->plan.folder[i];
        WatchHandle *watch = &handles[count];
        watch->change = FindFirstChangeNotificationW(folder->dir, folder->subtree, folder->filter);
        if (watch->change == INVALID_HANDLE_VALUE) {
            Util_Log(L"sessions: %s cannot be watched (error %lu)", folder->dir, GetLastError());
            continue;
        }
        watch->dlg = parameters->dlg;
        watch->stop = parameters->stop;
        watch->folder = folder;
        if (Probed(folder)) watch->state = WatchedState(folder);   /* once notified: nothing between is missed */
        if (!RegisterWaitForSingleObject(&watch->wait, watch->change, WatchChanged, watch, INFINITE, WT_EXECUTEINWAITTHREAD)) {
            Util_Log(L"sessions: %s cannot be watched (error %lu)", folder->dir, GetLastError());
            FindCloseChangeNotification(watch->change);
            continue;
        }
        count++;
    }
    /* The first snapshot follows registration too, so a write between the
     * directory lookup and this thread starting cannot leave an old cache. */
    PostReload(parameters->dlg);
    WaitForSingleObject(parameters->stop, INFINITE);
    for (i = 0; i < count; i++) {
        UnregisterWaitEx(handles[i].wait, INVALID_HANDLE_VALUE);
        FindCloseChangeNotification(handles[i].change);
    }
    HeapFree(GetProcessHeap(), 0, parameters);
    return 0;
}

static void StopWatching(void)
{
    if (g_view.watchStop) SetEvent(g_view.watchStop);
    if (g_view.watchThread) {
        WaitForSingleObject(g_view.watchThread, INFINITE);
        CloseHandle(g_view.watchThread);
    }
    if (g_view.watchStop) CloseHandle(g_view.watchStop);
    g_view.watchThread = g_view.watchStop = NULL;
    g_view.watched.count = 0;
}

static BOOL SamePath(const WCHAR *one, const WCHAR *other)
{
    return (!one[0] && !other[0]) || Core_PathEquals(one, other);
}

/* The watcher runs, and watches `plan` already: not one that lost a
 * notification and is stopping. */
static BOOL WatchingThese(const WatchPlan *plan)
{
    int i;
    if (!g_view.watchThread || WaitForSingleObject(g_view.watchThread, 0) != WAIT_TIMEOUT ||
        WaitForSingleObject(g_view.watchStop, 0) != WAIT_TIMEOUT ||
        g_view.watched.count != plan->count)
        return FALSE;
    for (i = 0; i < plan->count; i++) {
        const WatchedFolder *watching = &g_view.watched.folder[i], *planned = &plan->folder[i];
        if (!SamePath(watching->dir, planned->dir) || watching->subtree != planned->subtree || watching->filter != planned->filter ||
            !SamePath(watching->nearest, planned->nearest) || !SamePath(watching->file, planned->file))
            return FALSE;
    }
    return TRUE;
}

static void AddWatch(WatchPlan *plan, const WCHAR *dir, BOOL subtree, DWORD filter, const WCHAR *nearest, const WCHAR *file)
{
    WatchedFolder *folder;
    if (plan->count >= WATCH_MAX) return;
    folder = &plan->folder[plan->count];
    if (FAILED(StringCchCopyW(folder->dir, ARRAYSIZE(folder->dir), dir)) ||
        FAILED(StringCchCopyW(folder->nearest, ARRAYSIZE(folder->nearest), nearest)) ||
        FAILED(StringCchCopyW(folder->file, ARRAYSIZE(folder->file), file)))
        return;
    folder->subtree = subtree;
    folder->filter = filter;
    plan->count++;
}

/* `target` watched at any depth once it exists; until then its nearest
 * existing folder, alone, for the next folder down: a folder watched with
 * all it holds wakes on every write there (a profile's folder holds
 * Chromium's cache). */
static void WatchFolderOrParent(WatchPlan *plan, const WCHAR *target, const WCHAR *nearest)
{
    if (Core_PathEquals(nearest, target)) AddWatch(plan, target, TRUE, WATCH_EVERYTHING, L"", L"");
    else AddWatch(plan, nearest, FALSE, FILE_NOTIFY_CHANGE_DIR_NAME, target, L"");
}

/* What to watch for the profiles of a snapshot: each one's session entries
 * and its own folder (its config.json names the account shown; its entries
 * folder appears there), and Claude Code's transcripts. Planned with every
 * snapshot, on the loader thread (the folders may be on a share): the watch
 * follows folders as they are made. */
static void PlanWatch(const ProfileList *profiles, WatchPlan *plan)
{
    WCHAR target[MAX_PATH], nearest[MAX_PATH], config[MAX_PATH];
    int i;
    plan->count = 0;
    for (i = 0; i < profiles->count; i++) {
        const Profile *profile = &profiles->items[i];
        if (!SessionStore_SessionsDir(profile, target, ARRAYSIZE(target))) continue;
        if (SessionStore_WatchDir(profile, nearest, ARRAYSIZE(nearest)) && !Core_PathEquals(nearest, profile->storageDir))
            WatchFolderOrParent(plan, target, nearest);
        if (Util_DirExists(profile->storageDir) &&
            SUCCEEDED(StringCchPrintfW(config, ARRAYSIZE(config), L"%s\\" CLAUDE_APP_SETTINGS, profile->storageDir)))
            AddWatch(plan, profile->storageDir, FALSE, WATCH_EVERYTHING, target, config);
    }
    if (SessionStore_ProjectsDir(target, ARRAYSIZE(target)) && Util_ExistingDir(target, nearest, ARRAYSIZE(nearest)))
        WatchFolderOrParent(plan, target, nearest);
}

/* The watcher watching `plan` (the snapshot's): a new watcher asks for a
 * snapshot once it watches. */
static void Watch(const WatchPlan *plan)
{
    WatcherParameters *parameters;
    DWORD error;
    if (WatchingThese(plan)) return;
    StopWatching();
    parameters = (WatcherParameters *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *parameters);
    if (parameters && (g_view.watchStop = CreateEventW(NULL, TRUE, FALSE, NULL)) != NULL) {
        parameters->dlg = g_view.dlg;
        parameters->stop = g_view.watchStop;
        parameters->plan = *plan;
        g_view.watched = *plan;
        g_view.watchThread = CreateThread(NULL, 0, WatchProc, parameters, 0, NULL);
        if (g_view.watchThread) return;
    }
    error = parameters ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;   /* HeapAlloc sets none */
    if (parameters) HeapFree(GetProcessHeap(), 0, parameters);
    if (g_view.watchStop) CloseHandle(g_view.watchStop);
    g_view.watchStop = NULL;
    g_view.watched.count = 0;
    Util_Log(L"sessions: the folder watcher cannot start (error %lu)", error);
}

/* ---------------------------------------------------------------- loader */

static void FreeSnapshot(SessionSet *set)
{
    if (!set) return;
    SessionStore_Free(set);
    HeapFree(GetProcessHeap(), 0, set);
}

/* `fresh` (a snapshot, or NULL for a read that failed) handed to the UI,
 * unless it went out of date while it was read (RequestLoad's restart) or
 * the loader stops: then it is freed here. */
static void Publish(SessionLoader *loader, ULONG generation, SessionSet *fresh, ULONGLONG treeHash)
{
    SessionSet *old = NULL;
    BOOL publish = FALSE;
    AcquireSRWLockExclusive(&loader->lock);
    if (loader->generation == generation && WaitForSingleObject(loader->stop, 0) != WAIT_OBJECT_0) {
        old = loader->ready;
        loader->ready = fresh;
        loader->readyTreeHash = treeHash;
        if (fresh) loader->readyPlan = loader->plan;
        loader->readyGeneration = generation;
        loader->failed = fresh == NULL;
        publish = TRUE;
    }
    ReleaseSRWLockExclusive(&loader->lock);
    FreeSnapshot(old);
    if (publish) PostMessageW(loader->dlg, WM_APP_SESSIONS_READY, 0, 0);
    else FreeSnapshot(fresh);
}

/* A snapshot is logged when what it holds changed or it was slow, a failed
 * read once until one works: reads follow every write to a transcript, and
 * the log is kept short. */
static void LogSnapshot(SessionLoader *loader, const SessionSet *set, DWORD elapsedMs)
{
    if (!set) {
        if (!loader->failureLogged) Util_Log(L"sessions could not be read (%lu ms)", elapsedMs);
        loader->failureLogged = TRUE;
        return;
    }
    loader->failureLogged = FALSE;
    if (elapsedMs < SLOW_LOAD_MS && set->entryCount == loader->loggedEntries && set->rowCount == loader->loggedRows &&
        set->groupCount == loader->loggedGroups)
        return;
    Util_Log(L"sessions loaded in %lu ms: %d entries, %d conversations, %d folders", elapsedMs, set->entryCount, set->rowCount,
             set->groupCount);
    loader->loggedEntries = set->entryCount;
    loader->loggedRows = set->rowCount;
    loader->loggedGroups = set->groupCount;
}

/* Reads a snapshot, and what it asks to watch, for each request: a request
 * during a read is read once that read is published, a read that failed
 * included; a cancelled read is dropped. */
static DWORD WINAPI LoadProc(void *param)
{
    SessionLoader *loader = (SessionLoader *)param;
    HANDLE events[2] = { loader->stop, loader->request }, paced[2] = { loader->stop, loader->cancel };
    for (;;) {
        ProfileList profiles;
        SessionSet *fresh;
        ULONG generation;
        ULONGLONG started;
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        AcquireSRWLockExclusive(&loader->lock);
        profiles = loader->profiles;
        generation = loader->generation;
        ResetEvent(loader->cancel);
        ReleaseSRWLockExclusive(&loader->lock);
        if (WaitForSingleObject(loader->stop, 0) == WAIT_OBJECT_0) break;
        started = GetTickCount64();
        fresh = (SessionSet *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *fresh);
        if (fresh) {
            Claude_UpdateRunning(&profiles);
            if (SessionStore_LoadProfilesCancel(fresh, &profiles, loader->cancel)) {
                PlanWatch(&fresh->profiles, &loader->plan);
            } else {
                HeapFree(GetProcessHeap(), 0, fresh);
                fresh = NULL;
                if (WaitForSingleObject(loader->cancel, 0) == WAIT_OBJECT_0) continue;   /* out of date: read again at once */
            }
        }
        LogSnapshot(loader, fresh, (DWORD)(GetTickCount64() - started));
        Publish(loader, generation, fresh, fresh ? TreeContentHash(fresh) : 0);
        /* While Claude Code runs, its transcript is written all the time: the
         * next read waits a little, unless a change of ours needs it now. */
        if (WaitForMultipleObjects(ARRAYSIZE(paced), paced, FALSE, READ_PACE_MS) == WAIT_OBJECT_0) break;
    }
    return 0;
}

static void FreeLoader(SessionLoader *loader)
{
    if (loader->thread) CloseHandle(loader->thread);
    if (loader->stop) CloseHandle(loader->stop);
    if (loader->request) CloseHandle(loader->request);
    if (loader->cancel) CloseHandle(loader->cancel);
    FreeSnapshot(loader->ready);
    HeapFree(GetProcessHeap(), 0, loader);
}

static BOOL StartLoader(void)
{
    SessionLoader *loader;
    if (g_view.loader) return TRUE;
    loader = (SessionLoader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *loader);
    if (!loader) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);   /* HeapAlloc sets none */
        return FALSE;
    }
    InitializeSRWLock(&loader->lock);
    loader->dlg = g_view.dlg;
    loader->loggedEntries = loader->loggedRows = loader->loggedGroups = -1;   /* the first snapshot is logged */
    loader->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    loader->request = CreateEventW(NULL, FALSE, FALSE, NULL);
    loader->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (loader->stop && loader->request && loader->cancel)
        loader->thread = CreateThread(NULL, 0, LoadProc, loader, 0, NULL);
    if (!loader->thread) {
        DWORD error = GetLastError();
        FreeLoader(loader);
        SetLastError(error);
        return FALSE;
    }
    g_view.loader = loader;
    return TRUE;
}

static void StopLoader(void)
{
    SessionLoader *loader = g_view.loader;
    if (!loader) return;
    SetEvent(loader->stop);
    SetEvent(loader->cancel);
    WaitForSingleObject(loader->thread, INFINITE);
    FreeLoader(loader);
    g_view.loader = NULL;
}

/* No snapshot could be read: the tree and the side bar say so. */
static void ShowLoadFailed(void)
{
    g_view.loadFailed = TRUE;
    g_view.loading = FALSE;
    InvalidateRect(g_view.tree, NULL, FALSE);
    InvalidateRect(g_view.profiles, NULL, FALSE);
}

/* A snapshot asked for. `restart`: what is being read is out of date (the
 * profiles changed, or an action of ours changed the entries), so that read
 * stops and no snapshot from before is shown. Otherwise (a notification, the
 * window activated) the read in progress is published and another follows:
 * notifications coming faster than a read cannot keep every read from
 * finishing. */
static void RequestLoad(BOOL restart)
{
    SessionLoader *loader;
    if (!g_view.warmed) return;
    if (!StartLoader()) {
        if (!g_view.loadFailed) Util_Log(L"sessions: the reader cannot start (error %lu)", GetLastError());
        ShowLoadFailed();
        return;
    }
    InterlockedExchange(&g_reloadMessagePending, 0);
    loader = g_view.loader;
    AcquireSRWLockExclusive(&loader->lock);
    loader->profiles = g_view.profilesToRead;
    if (restart) {
        loader->generation++;
        SetEvent(loader->cancel);
    }
    SetEvent(loader->request);
    ReleaseSRWLockExclusive(&loader->lock);
    if (g_view.loadFailed) {   /* loading again, no longer failed */
        InvalidateRect(g_view.tree, NULL, FALSE);
        InvalidateRect(g_view.profiles, NULL, FALSE);
    }
    g_view.loading = TRUE;
    g_view.loadFailed = FALSE;
}

/* ------------------------------------------------------------------- API */

static BOOL SameProfiles(const ProfileList *one, const ProfileList *other)
{
    int i;
    if (one->count != other->count) return FALSE;
    for (i = 0; i < one->count; i++) {
        const Profile *oneProfile = &one->items[i], *otherProfile = &other->items[i];
        if (wcscmp(oneProfile->folder, otherProfile->folder) != 0 || wcscmp(oneProfile->name, otherProfile->name) != 0 ||
            wcscmp(oneProfile->dataDir, otherProfile->dataDir) != 0 || wcscmp(oneProfile->storageDir, otherProfile->storageDir) != 0 ||
            oneProfile->color != otherProfile->color || oneProfile->isStock != otherProfile->isStock ||
            oneProfile->running != otherProfile->running || oneProfile->pid != otherProfile->pid) return FALSE;
    }
    return TRUE;
}

void SessionsView_SetProfiles(const ProfileList *profiles)
{
    if (!profiles || SameProfiles(&g_view.profilesToRead, profiles)) return;
    g_view.profilesToRead = *profiles;
    if (!g_view.loaded) g_view.set.profiles = *profiles;
    RequestLoad(TRUE);
}

void SessionsView_Warm(const ProfileList *profiles)
{
    SessionsView_SetProfiles(profiles);
    if (g_view.warmed) return;
    g_view.warmed = TRUE;
    RequestLoad(FALSE);                /* the first snapshot starts the watcher, which asks for another once it watches */
}

void SessionsView_Init(HWND dlg)
{
    g_view.dlg = dlg;
    g_view.profiles = GetDlgItem(dlg, IDC_S_PROFILES);
    g_view.tree = GetDlgItem(dlg, IDC_S_TREE);
    g_view.search = GetDlgItem(dlg, IDC_S_SEARCH);
    g_view.archived = GetDlgItem(dlg, IDC_S_ARCHIVED);
    g_view.details = GetDlgItem(dlg, IDC_S_DETAILS);
    g_view.treeArea = Theme_SmoothView(g_view.tree);
    Theme_SmoothView(g_view.profiles);
    g_view.parts = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VSCROLL | SS_OWNERDRAW | SS_NOTIFY, 0, 0, 0, 0, g_view.details,
                                   (HMENU)(INT_PTR)IDC_S_PARTS, g_hInst, NULL);
    if (!g_view.parts) Util_Log(L"sessions: the details' scrolling part cannot be made (error %lu)", GetLastError());
    SetWindowSubclass(g_view.details, DetailsSubclass, 1, 0);
    if (g_view.parts) SetWindowSubclass(g_view.parts, DetailsSubclass, 1, 0);
    SendMessageW(g_view.search, EM_LIMITTEXT, ARRAYSIZE(g_view.filter) - 1, 0);
}

BOOL SessionsView_Shown(void)
{
    return g_view.shown;
}

const WCHAR *SessionsView_Profile(void)
{
    int p = ShownProfileIndex();
    return p >= 0 ? ProfileAt(p)->folder : g_view.folder;
}

/* Controls are filled only while visible. A warm snapshot does no hidden
 * tree insertion or icon rendering on the manager's startup path. */
static void Render(BOOL force)
{
    if (!g_view.shown) return;
    if (g_view.rendered && !force && !g_view.dirty) {
        InvalidateRect(g_view.profiles, NULL, FALSE);
        RedrawDetails(FALSE);
        return;
    }
    Measure();
    FillProfiles();
    FillTree();
    RedrawDetails(FALSE);
    g_view.rendered = TRUE;
    g_view.dirty = FALSE;
}

/* What the waiting changes of profile `p` of `set` are tried with: its
 * queue file as it is now and how many changes its entries can take. */
static ULONGLONG WaitingChangesState(const SessionSet *set, int p)
{
    WCHAR path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    ULONGLONG state = Core_HashText(CORE_HASH_START, set->profiles.items[p].folder);
    ZeroMemory(&file, sizeof file);
    if (!SessionStore_PendingPath(&set->profiles.items[p], path, ARRAYSIZE(path)) ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &file))
        ZeroMemory(&file, sizeof file);
    state = Core_HashBytes(state, &file.ftLastWriteTime, sizeof file.ftLastWriteTime);
    state = Core_HashBytes(state, &file.nFileSizeHigh, sizeof file.nFileSizeHigh);
    state = Core_HashBytes(state, &file.nFileSizeLow, sizeof file.nFileSizeLow);
    return Core_HashBytes(state, &set->source[p].pending, sizeof set->source[p].pending);
}

/* The changes waiting for a profile that has closed, made now (the UI owns
 * them: the reader thread never writes entries; SessionEdit checks the
 * profile is still closed). A profile's count holds only the changes its
 * entries can take; when none of them can be made (their queue cannot be
 * written, for one), they are tried again only once the queue or that count
 * changed. TRUE when any was made: the snapshot is out of date. */
static BOOL MakeWaitingChanges(const SessionSet *set)
{
    BOOL made = FALSE;
    int p;
    for (p = 0; p < set->profiles.count; p++) {
        ULONGLONG state;
        if (!set->source[p].pending || set->profiles.items[p].running) continue;
        state = WaitingChangesState(set, p);
        if (state == g_view.waitingChangesTried[p]) continue;
        if (SessionEdit_ApplyPending(g_view.dlg, &set->profiles.items[p]) > 0) made = TRUE;
        else g_view.waitingChangesTried[p] = state;
    }
    return made;
}

/* The snapshot the loader published, shown: unless it went out of date
 * since it was read (RequestLoad's restart). */
static void AcceptSnapshot(void)
{
    SessionLoader *loader = g_view.loader;
    SessionSet *fresh;
    ULONGLONG treeHash;
    BOOL failed;
    AcquireSRWLockExclusive(&loader->lock);
    fresh = loader->ready;
    treeHash = loader->readyTreeHash;
    failed = loader->failed && loader->readyGeneration == loader->generation;
    loader->ready = NULL;
    loader->failed = FALSE;
    if (loader->readyGeneration != loader->generation) {
        FreeSnapshot(fresh);
        fresh = NULL;
    }
    if (fresh) g_view.planned = loader->readyPlan;
    ReleaseSRWLockExclusive(&loader->lock);
    if (failed) ShowLoadFailed();
    if (!fresh) return;
    if (MakeWaitingChanges(fresh)) {
        FreeSnapshot(fresh);
        RequestLoad(TRUE);
        return;
    }
    /* While the tree's rows still name the rows of the snapshot shown. */
    if (g_view.shown && g_view.loaded && treeHash != g_view.treeHash) RememberTopRow();
    if (g_view.rendered) RememberNeighbors();
    g_view.dirty = g_view.dirty || !g_view.loaded || treeHash != g_view.treeHash;
    SessionStore_Free(&g_view.set);
    g_view.set = *fresh;
    HeapFree(GetProcessHeap(), 0, fresh);
    g_view.loaded = TRUE;
    g_view.loading = FALSE;
    g_view.treeHash = treeHash;
    CountSessions();
    Watch(&g_view.planned);
    Render(FALSE);
}

void SessionsView_Ready(BOOL allowChanges)
{
    if (!allowChanges || g_view.actionDepth || g_view.accepting) return;
    g_view.accepting = TRUE;
    if (g_view.loader) AcceptSnapshot();
    /* A notification handled while the manager was busy asked for no
     * snapshot: ask now (it also lets the watcher notify again). */
    if (InterlockedCompareExchange(&g_reloadMessagePending, 0, 0)) RequestLoad(FALSE);
    g_view.accepting = FALSE;
}

void SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder)
{
    size_t i;
    int shownProfile;
    g_view.pkg = pkg;
    if (folder && !Core_EqualsI(folder, g_view.folder)) ForgetNeighbors();
    if (folder) StringCchCopyW(g_view.folder, ARRAYSIZE(g_view.folder), folder);
    SessionsView_Warm(&g_view.profilesToRead);
    SessionsView_Ready(TRUE);          /* before the view shows: it renders once, below */
    g_view.shown = TRUE;
    Render(TRUE);
    for (i = 0; i < ARRAYSIZE(kControls); i++) ShowWindow(GetDlgItem(g_view.dlg, kControls[i]), SW_SHOW);
    /* The profile shown, in sight in the side bar (the tree's selection is: FillTree). */
    if ((shownProfile = ShownProfileIndex()) >= 0) SendMessageW(g_view.profiles, LB_SETCARETINDEX, (WPARAM)shownProfile, FALSE);
}

void SessionsView_Leave(void)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(kControls); i++) ShowWindow(GetDlgItem(g_view.dlg, kControls[i]), SW_HIDE);
    g_view.shown = FALSE;
}

void SessionsView_Reload(void)
{
    RequestLoad(FALSE);
}

/* Fonts or scale changed: everything measured and filled again, the row on
 * top of the view kept there. */
void SessionsView_Relayout(void)
{
    if (!g_view.shown) return;
    RememberTopRow();
    Render(TRUE);
}

/* Resizing keeps the rows, selection, expansion and scroll position. */
void SessionsView_Resize(void)
{
    if (!g_view.shown) return;
    RedrawDetails(FALSE);   /* painted with the rest of the window, not in the middle of its layout */
}

BOOL SessionsView_ClearSearch(void)
{
    if (!g_view.shown || GetFocus() != g_view.search || GetWindowTextLengthW(g_view.search) == 0) return FALSE;
    SetWindowTextW(g_view.search, L"");
    return TRUE;
}

BOOL SessionsView_Command(WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDOK:   /* Enter */
        if (g_view.shown && GetFocus() == g_view.tree && SelectedRow() >= 0) RunAction(ACT_OPEN, -1);
        return g_view.shown;
    case IDC_S_SEARCH:
        if (HIWORD(wp) == EN_CHANGE && g_view.shown) {
            GetWindowTextW(g_view.search, g_view.filter, ARRAYSIZE(g_view.filter));
            ForgetNeighbors();
            FillTree();
            RedrawDetails(FALSE);
        }
        return TRUE;
    case IDC_S_ARCHIVED:
        if (HIWORD(wp) == BN_CLICKED && g_view.shown) {
            g_view.showArchived = SendMessageW(g_view.archived, BM_GETCHECK, 0, 0) == BST_CHECKED;
            CountSessions();
            InvalidateRect(g_view.profiles, NULL, FALSE);
            FillTree();
            RedrawDetails(FALSE);
        }
        return TRUE;
    case IDC_S_PROFILES:
        if (HIWORD(wp) == LBN_SELCHANGE && g_view.shown) {
            LRESULT i = SendMessageW(g_view.profiles, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < g_view.set.profiles.count) {
                StringCchCopyW(g_view.folder, ARRAYSIZE(g_view.folder), ProfileAt((int)i)->folder);
                ForgetNeighbors();
                FillTree();
                RedrawDetails(FALSE);
            }
        }
        return TRUE;
    }
    return FALSE;
}

/* WM_CONTEXTMENU: the tree's menu, at the mouse or (from the keyboard) at
 * the selected row, kept on the view when that row is scrolled out of it. */
BOOL SessionsView_ContextMenu(HWND from, LPARAM pos)
{
    POINT pt;
    if (!g_view.shown || from != g_view.tree) return FALSE;
    if (pos == (LPARAM)-1) {
        RECT rc, view;
        HTREEITEM selected = TreeView_GetSelection(g_view.tree);
        if (!selected || !TreeView_GetItemRect(g_view.tree, selected, &rc, TRUE)) return TRUE;
        pt.x = rc.left;
        pt.y = rc.bottom;
        ClientToScreen(g_view.tree, &pt);
        GetWindowRect(g_view.treeArea, &view);
        pt.x = max(view.left, min(pt.x, view.right));
        pt.y = max(view.top, min(pt.y, view.bottom));
    } else {
        TVHITTESTINFO hit;
        pt.x = GET_X_LPARAM(pos);
        pt.y = GET_Y_LPARAM(pos);
        ZeroMemory(&hit, sizeof hit);
        hit.pt = pt;
        ScreenToClient(g_view.tree, &hit.pt);
        if (!TreeView_HitTest(g_view.tree, &hit) || !(hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT | TVHT_ONITEMBUTTON)))
            return TRUE;   /* below the rows: no session to act on */
        TreeView_SelectItem(g_view.tree, hit.hItem);
    }
    if (NodeParam(TreeView_GetSelection(g_view.tree)) >= 0) ShowMenu(pt);
    return TRUE;
}

/* The tree's notifications. Nothing once the view is destroyed: the tree
 * can still notify while the dialog's children go. */
BOOL SessionsView_Notify(const NMHDR *header, LRESULT *result)
{
    *result = 0;
    if (!g_view.dlg || header->idFrom != IDC_S_TREE) return FALSE;
    switch (header->code) {
    case NM_CUSTOMDRAW:
        *result = TreeCustomDraw((NMTVCUSTOMDRAW *)header);
        return TRUE;
    case NM_DBLCLK: {
        /* Only a double-click on the selected session's own row opens it,
         * not one on a folder's row or arrow. */
        DWORD position = GetMessagePos();
        TVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = GET_X_LPARAM(position);
        hit.pt.y = GET_Y_LPARAM(position);
        ScreenToClient(g_view.tree, &hit.pt);
        if (TreeView_HitTest(g_view.tree, &hit) && (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT)) &&
            hit.hItem == TreeView_GetSelection(g_view.tree) && NodeParam(hit.hItem) >= 0) {
            RunAction(ACT_OPEN, -1);
            *result = 1;
        }
        return TRUE;
    }
    case TVN_KEYDOWN: {
        const NMTVKEYDOWN *key = (const NMTVKEYDOWN *)header;
        int r = SelectedRow();
        const SessionEntry *entry = r >= 0 ? EntryOf(&g_view.set.rows[r], ShownProfileIndex()) : NULL;
        if (ListedAndKept(entry) && key->wVKey == VK_F2) RunAction(ACT_RENAME, -1);
        else if (ListedAndKept(entry) && key->wVKey == VK_DELETE) RunAction(ACT_REMOVE, -1);
        return TRUE;
    }
    case TVN_SELCHANGEDW: {
        const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
        TreeNodeKey key;
        if (!g_view.filling) {
            NodeKey(change->itemNew.lParam, NodeParam(TreeView_GetParent(g_view.tree, change->itemNew.hItem)) == NODE_STARRED, &key);
            SetSelection(&key);
            g_view.hot.window = g_view.pressed.window = NULL;
            RedrawDetails(FALSE);
        }
        return TRUE;
    }
    case TVN_ITEMEXPANDEDW: {
        const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
        TreeNodeKey key;
        if (!g_view.filling && change->itemNew.lParam < 0 && (change->action == TVE_COLLAPSE || change->action == TVE_EXPAND)) {
            NodeKey(change->itemNew.lParam, FALSE, &key);
            SetCollapsed(key.key, change->action == TVE_COLLAPSE);
        }
        return TRUE;
    }
    case TVN_GETINFOTIPW: {
        NMTVGETINFOTIPW *tip = (NMTVGETINFOTIPW *)header;
        WCHAR date[64], time[32];
        if (tip->cchTextMax <= 0) return TRUE;
        if (tip->lParam >= 0 && tip->lParam < g_view.set.rowCount) {
            const SessionRow *row = &g_view.set.rows[tip->lParam];
            const WCHAR *title = ShownTitle(row, ShownProfileIndex());
            const WCHAR *noConversation = row->transcript ? L"" : TR(L"\nNo conversation on disk");
            if (FormatWhen(row->lastActivity, date, ARRAYSIZE(date), time, ARRAYSIZE(time)))
                StringCchPrintfW(tip->pszText, (size_t)tip->cchTextMax, TR(L"%s\nLast used %s %s%s"), title, date, time, noConversation);
            else
                StringCchPrintfW(tip->pszText, (size_t)tip->cchTextMax, L"%s%s", title, noConversation);
        } else if (tip->lParam < NODE_STARRED && tip->lParam != NODE_NONE && NodeGroup(tip->lParam) < g_view.set.groupCount) {
            int group = NodeGroup(tip->lParam);
            if (g_view.set.groups[group].path[0]) StringCchCopyW(tip->pszText, (size_t)tip->cchTextMax, g_view.set.groups[group].path);
            else SessionStore_GroupName(&g_view.set, group, tip->pszText, (size_t)tip->cchTextMax);
        }
        return TRUE;
    }
    }
    return FALSE;
}

BOOL SessionsView_DrawItem(const DRAWITEMSTRUCT *item)
{
    if (!g_view.dlg) return FALSE;
    if (item->CtlID == IDC_S_PROFILES) {
        DrawProfile(item);
        return TRUE;
    }
    if (item->CtlID == IDC_S_DETAILS) {
        DrawDetails(item);
        return TRUE;
    }
    return FALSE;
}

void SessionsView_Destroy(void)
{
    StopWatching();
    StopLoader();
    SessionStore_Free(&g_view.set);
    Theme_FreeFonts(&g_view.fonts);
    if (g_view.icons) ImageList_Destroy(g_view.icons);
    if (g_view.badges) ImageList_Destroy(g_view.badges);
    if (g_view.folderCount) HeapFree(GetProcessHeap(), 0, g_view.folderCount);
    ZeroMemory(&g_view, sizeof g_view);
    InterlockedExchange(&g_reloadMessagePending, 0);
}
