/*
 * The program's dialogs and manager window as theme.c lays them out, with
 * private mock data, in every interface language and at four font and
 * geometry scales (96, 144, 192 and 288: a resource and its font scaled
 * together; the monitor's own DPI is left as it is):
 *  - every visible control inside its dialog and every dialog inside the
 *    monitor's work area, without overlaps, each caption whole (wrapped
 *    labels, buttons, check boxes, links, edits, drop-down lists);
 *  - the manager window: one client size for every language, both views and
 *    every status and footer, level with the buttons beside them; its table
 *    columns, profile note and shortcut row; live language changes in the
 *    same window and in its modal dialogs;
 *    its responsive layout from the minimum to wide, tall and maximized
 *    frames, and its native frame messages;
 *  - the sessions view driven by sessions.c on private profiles, entries and
 *    transcripts: resizes, refills and language changes keep its rows, its
 *    selection (a starred row included), its folded folders and the row
 *    on top; its disk watcher; double-clicks;
 *  - the themed message box (Ui_Message), as the program shows it.
 * Built and run by build.cmd with the program's resources and manifest. Its
 * windows show briefly; the maximized one, the modal profile dialog and the
 * message boxes take the foreground. The log and the queued session changes
 * go to a private folder (Util_SetStateDir), and the manager window's class
 * is one of the tests' own: the program never finds a fixture.
 */
#include "../src/app.h"
#include "../src/resource.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdio.h>
#include <stdlib.h>

/* The manager window's layout contract is theme.c's (THEME_MAIN_*, app.h);
 * these bounds are the tests' own, in DIPs. */
#define SESSION_PROFILES_MAX_DIPS 200    /* the sessions' side bar stays narrow, */
#define SESSION_DETAILS_MAX_DIPS  300    /* and so do the details */

/* A session snapshot is read on another thread: generous for a loaded machine. */
#define SNAPSHOT_TIMEOUT_MS 10000
/* How long a write the watcher ignores is given to ask for a snapshot anyway. */
#define IGNORED_WRITE_MS    1000

enum { SCALED_FONT_SUBCLASS = 1, FRAME_PROBE_SUBCLASS, MESSAGE_OWNER_SUBCLASS };
enum { TRANSCRIPT_STREAM_TIMER = 1 };

/* The first scale is the monitor's own: the checks made at it need it to fit. */
static const int kFontScales[] = { 96, 144, 192, 288 };
static BOOL g_scaleFits[ARRAYSIZE(kFontScales)];

typedef enum LinkStatus { LINKS_NOT_SET_UP, LINKS_ROUTED, CLAUDE_MISSING, LINK_STATUSES } LinkStatus;

typedef struct LayoutFixture {
    int resource, language, fontScale;
    const WCHAR *noteName;              /* the default profile's name; NULL: one MAX_LABEL long */
    const int *hiddenControls;          /* hidden before the dialog is fitted, as their dialog hides them */
    int hiddenCount;
    HFONT font;                         /* the scaled resource font, owned */
    HWND table, tableViewport, tree, treeViewport, profilesViewport;
    BOOL sessions;                      /* the sessions view shows */
    BOOL realSessions;                  /* sessions.c drives that view */
    LinkStatus links;
    MainFooter footer;
    /* What sessions.c did through the window. */
    int reloadRequests, openPrompts;
    HWND lastPrompt;
    WCHAR promptText[512];
} LayoutFixture;

static int g_checks, g_failures;

static void Check(const LayoutFixture *fixture, HWND control, const char *name, BOOL ok)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    printf("  FAIL resource=%d mode=%s language=%ls font-scale=%d control=%d: %s\n", fixture->resource,
           fixture->sessions ? "Sessions" : "Profiles", Localize_LanguageCode(fixture->language), fixture->fontScale,
           control ? GetDlgCtrlID(control) : 0, name);
}

static int Language(const WCHAR *code)
{
    return Localize_LanguageForCode(code);
}

/* The thread's pending messages, handled. A window on screen whose thread
 * reads no message for five seconds is "not responding": Windows then
 * ignores ShowWindow on it (a maximized one stays maximized) and puts it back
 * where its ghost was when it hides. Every step that keeps a fixture on
 * screen for long pumps. */
static void PumpMessages(void)
{
    MSG message;
    int handled = 0;
    while (handled++ < 4096 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

typedef BOOL (*Condition)(const void *context);

/* Messages handled until `done` holds, at most `timeoutMs`: woken by
 * messages, never spinning. A NULL `done` waits the whole time. */
static BOOL PumpUntil(Condition done, const void *context, DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        ULONGLONG now;
        PumpMessages();
        if (done && done(context)) return TRUE;
        now = GetTickCount64();
        if (now >= deadline) return FALSE;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static BOOL IsClass(HWND window, const WCHAR *name)
{
    WCHAR actual[64];
    return GetClassNameW(window, actual, ARRAYSIZE(actual)) && _wcsicmp(actual, name) == 0;
}

static RECT RelativeRect(HWND dialog, HWND control)
{
    RECT rect = { 0, 0, 0, 0 };
    GetWindowRect(control, &rect);
    MapWindowPoints(NULL, dialog, (POINT *)&rect, 2);
    return rect;
}

static BOOL InsideWorkArea(HWND window)
{
    RECT frame;
    MONITORINFO monitor = { sizeof monitor };
    return GetWindowRect(window, &frame) && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
           frame.left >= monitor.rcWork.left && frame.top >= monitor.rcWork.top &&
           frame.right <= monitor.rcWork.right && frame.bottom <= monitor.rcWork.bottom;
}

static void LongName(WCHAR *name, size_t cch)
{
    static const WCHAR kPhrase[] = L"Long profile name ";
    size_t i;
    for (i = 0; i < MAX_LABEL && i + 1 < cch; i++) name[i] = kPhrase[i % (ARRAYSIZE(kPhrase) - 1)];
    name[i] = 0;
}

static void FixtureName(const LayoutFixture *fixture, WCHAR *name, size_t cch)
{
    if (fixture->noteName) StringCchCopyW(name, cch, fixture->noteName);
    else LongName(name, cch);
}

/* Font and resource geometry are scaled together; the monitor DPI is unchanged. */
static BOOL ScaleFixture(HWND dialog, LayoutFixture *fixture)
{
    typedef struct Position { HWND window; RECT rect; int dropHeight; } Position;
    Position positions[128];
    HWND child;
    LOGFONTW font;
    RECT frame, client;
    MONITORINFO monitor = { sizeof monitor };
    HFONT native = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    int count = 0, i, width, height;
    if (!native || !GetObjectW(native, sizeof font, &font)) return FALSE;
    GetWindowRect(dialog, &frame);
    GetClientRect(dialog, &client);
    for (child = GetWindow(dialog, GW_CHILD); child && count < (int)ARRAYSIZE(positions); child = GetWindow(child, GW_HWNDNEXT)) {
        RECT dropped;
        positions[count].window = child;
        positions[count].rect = RelativeRect(dialog, child);
        /* A drop-down list's resource height is its list's, as the dialog manager sizes it. */
        positions[count].dropHeight = IsClass(child, WC_COMBOBOXW) && SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&dropped)
                                      ? dropped.bottom - dropped.top : 0;
        count++;
    }
    font.lfHeight = MulDiv(font.lfHeight, fixture->fontScale, 96);
    fixture->font = CreateFontIndirectW(&font);
    if (!fixture->font) return FALSE;
    SendMessageW(dialog, WM_SETFONT, (WPARAM)fixture->font, FALSE);
    width = MulDiv(client.right, fixture->fontScale, 96) + frame.right - frame.left - client.right;
    height = MulDiv(client.bottom, fixture->fontScale, 96) + frame.bottom - frame.top - client.bottom;
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowPos(dialog, NULL, max(monitor.rcWork.left, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2),
                 max(monitor.rcWork.top, monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2),
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    for (i = 0; i < count; i++) {
        RECT rect = positions[i].rect;
        SendMessageW(positions[i].window, WM_SETFONT, (WPARAM)fixture->font, FALSE);
        SetWindowPos(positions[i].window, NULL, MulDiv(rect.left, fixture->fontScale, 96), MulDiv(rect.top, fixture->fontScale, 96),
                     MulDiv(rect.right - rect.left, fixture->fontScale, 96),
                     MulDiv(positions[i].dropHeight ? positions[i].dropHeight : rect.bottom - rect.top, fixture->fontScale, 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return TRUE;
}

/* A native template font already has its monitor's size. A synthetic scale
 * supplies that same initial WM_GETFONT result while the theme reads it: the
 * theme's dialog subclass, installed after this one, answers with the font
 * it made from it once it has one. */
static LRESULT CALLBACK ScaledResourceFont(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR font)
{
    (void)id;
    if (message == WM_GETFONT) return (LRESULT)font;
    return DefSubclassProc(window, message, wp, lp);
}

static void CheckInitialFont(HWND dialog, const LayoutFixture *fixture)
{
    LOGFONTW expected = { 0 }, actual = { 0 };
    HFONT font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    BOOL available = GetObjectW(fixture->font, sizeof expected, &expected) && GetObjectW(font, sizeof actual, &actual);
    Check(fixture, NULL, "initialized dialog retains its requested native resource font scale", available &&
          expected.lfHeight == actual.lfHeight && expected.lfWeight == actual.lfWeight);
    Check(fixture, NULL, "initialized dialog font covers the selected script", available && wcscmp(actual.lfFaceName, Localize_FontFace()) == 0);
}

/* The profile table's columns and one row (gui.c sizes the columns later). */
static void PopulateProfileTable(HWND table, const WCHAR *name)
{
    LVCOLUMNW column;
    LVITEMW item;
    RECT rect;
    int i;
    GetClientRect(table, &rect);
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < 3; i++) {
        column.pszText = (WCHAR *)TR(Theme_ProfileColumnTitle(i));
        column.cx = rect.right / 3;
        ListView_InsertColumn(table, i, &column);
    }
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (WCHAR *)name;
    ListView_InsertItem(table, &item);
    ListView_SetItemText(table, 0, 1, (WCHAR *)TR(Theme_ProfileRole(TRUE, TRUE)));
    ListView_SetItemText(table, 0, 2, L"%APPDATA%\\Claude");
}

/* The uninstall dialog's profiles to keep, as gui.c makes them: check boxes in
 * a column the view sizes, in a smooth view; with none (`rows` 0) the dialog
 * hides the view (the fixture's hidden controls). */
static void PopulateKeptProfiles(HWND dialog, const WCHAR *name, int rows)
{
    HWND list = GetDlgItem(dialog, IDC_U_LIST);
    WCHAR text[LABEL_CCH + 64];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    for (i = 0; i < rows; i++) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (%%APPDATA%%\\%s)"), name, L"Claude-Private");
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, i, TRUE);
    }
    Theme_SmoothView(list);
}

/* The status line and its button, and the footer, as gui.c shows them. */
static void MainCaptions(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR text[2048];
    HWND statusAction = GetDlgItem(dialog, IDC_STATUS_ACTION);
    if (fixture->links == CLAUDE_MISSING) {
        SetDlgItemTextW(dialog, IDC_STATUS, TR(L"Claude Desktop is not installed."));
        SetWindowTextW(statusAction, TR(Theme_MainCaption(IDC_STATUS_ACTION, 0)));
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text), fixture->links == LINKS_ROUTED
            ? TR(L"Claude Desktop %s \x00B7 claude:// links are routed correctly")
            : TR(L"Claude Desktop %s \x00B7 claude:// links are not set up yet"), L"2.16120.0");
        SetDlgItemTextW(dialog, IDC_STATUS, text);
        SetWindowTextW(statusAction, TR(Theme_MainCaption(IDC_STATUS_ACTION, 1)));
    }
    ShowWindow(statusAction, fixture->links == LINKS_ROUTED ? SW_HIDE : SW_SHOW);
    if (fixture->footer == MAIN_FOOTER_AVAILABLE)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainFooter(MAIN_FOOTER_AVAILABLE)), APP_VERSION_WSTR, L"9.8.7");
    else if (fixture->footer == MAIN_FOOTER_DOWNLOADING)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainFooter(MAIN_FOOTER_DOWNLOADING)), L"9.8.7");
    else if (fixture->footer == MAIN_FOOTER_INSTALLING)
        StringCchCopyW(text, ARRAYSIZE(text), TR(Theme_MainFooter(MAIN_FOOTER_INSTALLING)));
    else StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainFooter(MAIN_FOOTER_CREDITS)), APP_VERSION_WSTR, APP_AUTHOR_URL);
    SetDlgItemTextW(dialog, IDC_ABOUT, text);
    ShowWindow(GetDlgItem(dialog, IDC_UPDATE), fixture->footer != MAIN_FOOTER_CREDITS ? SW_SHOW : SW_HIDE);
}

static void FillMock(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR name[LABEL_CCH], text[4096];
    FixtureName(fixture, name, ARRAYSIZE(name));
    switch (fixture->resource) {
    case IDD_MAIN:
        MainCaptions(dialog, fixture);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainNote()), name);
        SetDlgItemTextW(dialog, IDC_NOTE, text);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Shortcuts for \x201C%s\x201D"), name);
        SetDlgItemTextW(dialog, IDC_SC_GROUP, text);
        PopulateProfileTable(GetDlgItem(dialog, IDC_LIST), name);
        break;
    case IDD_PROFILE: {
        HWND combo = GetDlgItem(dialog, IDC_P_COLOR);
        int color;
        SetWindowTextW(dialog, TR(L"Edit profile"));
        SetDlgItemTextW(dialog, IDC_P_NAME, name);
        for (color = 0; color < PALETTE_SIZE; color++) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)TR(g_ColorNames[color]));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Data folder: %%APPDATA%%\\%s%s"), L"Claude",
                         TR(L"\nThe regular Claude icon opens this profile."));
        SetDlgItemTextW(dialog, IDC_P_FOLDER, text);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Copy &settings from \x201C%s\x201D"), name);
        SetDlgItemTextW(dialog, IDC_P_COPY, text);
        CheckDlgButton(dialog, IDC_P_STARTUP, BST_CHECKED);
        CheckDlgButton(dialog, IDC_P_COPY, BST_CHECKED);
        CheckDlgButton(dialog, IDC_P_OPEN, BST_CHECKED);
        break;
    }
    case IDD_TITLE:
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Title in \x201C%s\x201D:"), name);
        SetDlgItemTextW(dialog, IDC_T_LABEL, text);
        SetDlgItemTextW(dialog, IDC_T_TITLE, L"A private mock session title that remains user data in every language");
        break;
    case IDD_MESSAGE:
        StringCchPrintfW(text, ARRAYSIZE(text),
            TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation and of its working folder, "
               L"under No folder in \x201C%s\x201D. The copy then goes on separately. %s, and its title there is set when %s closes."),
            L"A private conversation", name, name, name, TR(L"Continue?"), name);
        SetDlgItemTextW(dialog, IDC_M_TEXT, text);
        SetDlgItemTextW(dialog, IDOK, TR(L"Copy"));
        break;
    case IDD_UNINSTALL:
        StringCchPrintfW(text, ARRAYSIZE(text),
            TR(L"Claude Desktop and its \x201C%s\x201D profile (the one the regular Claude icon opens) are not touched."), name);
        SetDlgItemTextW(dialog, IDC_U_KEEP, text);
        PopulateKeptProfiles(dialog, name, fixture->hiddenCount ? 0 : 1);
        break;
    }
}

/* Only captions and private mock choices change. Native HWNDs and user text
 * remain in place while the same dialog visits several writing systems. */
static void TransitionCaptions(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR text[512];
    if (fixture->resource == IDD_MAIN) {
        WCHAR name[LABEL_CCH];
        FixtureName(fixture, name, ARRAYSIZE(name));
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainNote()), name);
        SetDlgItemTextW(dialog, IDC_NOTE, text);
        SetDlgItemTextW(dialog, IDC_SESSIONS, TR(Theme_MainCaption(IDC_SESSIONS, fixture->sessions)));
        SetDlgItemTextW(dialog, IDC_SC_GROUP, TR(L"Shortcuts"));
        MainCaptions(dialog, fixture);
    } else if (fixture->resource == IDD_PROFILE) {
        HWND combo = GetDlgItem(dialog, IDC_P_COLOR);
        int color;
        SetWindowTextW(dialog, TR(L"Edit profile"));
        SetDlgItemTextW(dialog, IDC_P_NAME, L"Private profile");
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Copy &settings from \x201C%s\x201D"), L"Private profile");
        SetDlgItemTextW(dialog, IDC_P_COPY, text);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (color = 0; color < PALETTE_SIZE; color++) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)TR(g_ColorNames[color]));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    } else if (fixture->resource == IDD_TITLE) {
        SetDlgItemTextW(dialog, IDC_T_TITLE, L"Private user text");
    } else if (fixture->resource == IDD_MESSAGE) {
        SetDlgItemTextW(dialog, IDC_M_TEXT, TR(L"Continue?"));
        SetDlgItemTextW(dialog, IDOK, TR(L"Copy"));
    }
}

/* What gui.c does to its window before theming it: the table's styles and
 * smooth view, the sessions view's controls in theirs, "Sessions >"
 * semibold. */
static void PrepareMainWindow(HWND dialog, LayoutFixture *fixture)
{
    fixture->table = GetDlgItem(dialog, IDC_LIST);
    fixture->tree = GetDlgItem(dialog, IDC_S_TREE);
    ListView_SetExtendedListViewStyle(fixture->table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    fixture->tableViewport = Theme_SmoothView(fixture->table);
    if (fixture->realSessions) SessionsView_Init(dialog);
    fixture->treeViewport = Theme_SmoothView(fixture->tree);
    fixture->profilesViewport = Theme_SmoothView(GetDlgItem(dialog, IDC_S_PROFILES));
    Theme_SetStrong(GetDlgItem(dialog, IDC_SESSIONS));
}

/* The note under "Set as default" names the default profile (gui.c's UpdateNote). */
static void LayoutMockNote(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR name[LABEL_CCH];
    FixtureName(fixture, name, ARRAYSIZE(name));
    Theme_LayoutSidebarNote(GetDlgItem(dialog, IDC_NOTE), TR(Theme_MainNote()), name);
}

/* The sessions view in place of the profiles, as Gui_ShowSessions swaps
 * them, without sessions.c: theme.c places every control of both views. */
static void ShowMockSessions(HWND dialog)
{
    static const int kProfileControls[] = { IDC_LIST, IDC_OPEN, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT, IDC_NOTE,
                                            IDC_SC_GROUP, IDC_SC_DESKTOP, IDC_SC_SAVEAS, IDC_SC_PIN, IDC_SC_START };
    static const int kSessionControls[] = { IDC_S_PROFILES, IDC_S_SEARCH, IDC_S_ARCHIVED, IDC_S_TREE, IDC_S_DETAILS };
    size_t i;
    for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_HIDE);
    for (i = 0; i < ARRAYSIZE(kSessionControls); i++) ShowWindow(GetDlgItem(dialog, kSessionControls[i]), SW_SHOW);
    SetDlgItemTextW(dialog, IDC_SESSIONS, TR(Theme_MainCaption(IDC_SESSIONS, 1)));
    SendDlgItemMessageW(dialog, IDC_S_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search this profile's sessions"));
    Theme_LayoutMain(dialog);
}

/* A fixture made as the program makes its dialog, at the fixture's scale:
 * resource labels, then mock data (`fill`), then the theme. */
static void InitializeFixture(HWND dialog, LayoutFixture *fixture, void (*fill)(HWND, const LayoutFixture *))
{
    int hidden;
    Localize_Window(dialog);
    Check(fixture, NULL, "font and geometry scale created", ScaleFixture(dialog, fixture));
    fill(dialog, fixture);
    for (hidden = 0; hidden < fixture->hiddenCount; hidden++) ShowWindow(GetDlgItem(dialog, fixture->hiddenControls[hidden]), SW_HIDE);
    if (fixture->resource == IDD_MAIN) {
        SIZE minimum;
        MINMAXINFO tracking = { 0 };
        Check(fixture, NULL, "main minimum is unavailable before layout initialization", !Theme_MainMinimum(dialog, &minimum));
        Check(fixture, NULL, "native tracking falls back before main initialization",
              !Gui_MainWindowGeometry(dialog, WM_GETMINMAXINFO, 0, (LPARAM)&tracking));
        PrepareMainWindow(dialog, fixture);
    }
    SetWindowSubclass(dialog, ScaledResourceFont, SCALED_FONT_SUBCLASS, (DWORD_PTR)fixture->font);
    Theme_Apply(dialog);
    if (fixture->resource == IDD_MAIN) Theme_RememberLayout(dialog);
    Theme_FitDialog(dialog);
    RemoveWindowSubclass(dialog, ScaledResourceFont, SCALED_FONT_SUBCLASS);
    if (fixture->resource == IDD_MAIN) {
        Gui_LayoutProfileColumns(fixture->table);
        LayoutMockNote(dialog, fixture);
    }
    if (fixture->sessions) ShowMockSessions(dialog);
    CheckInitialFont(dialog, fixture);
}

/* A message box of the sessions view (opening a session without Claude asks
 * for it, and never starts it), read and closed once it waits for the user. */
static void ClosePrompt(LayoutFixture *fixture, HWND prompt)
{
    if (!prompt || prompt == fixture->lastPrompt) return;
    fixture->lastPrompt = prompt;
    fixture->openPrompts++;
    GetDlgItemTextW(prompt, IDC_M_TEXT, fixture->promptText, ARRAYSIZE(fixture->promptText));
    PostMessageW(prompt, WM_COMMAND, IDOK, 0);
}

static INT_PTR CALLBACK FixtureProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    LayoutFixture *fixture = (LayoutFixture *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        fixture = (LayoutFixture *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        InitializeFixture(dialog, fixture, FillMock);
        return TRUE;
    }
    if (!fixture) return FALSE;
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
        return Theme_CtlColor(message, wp, lp, fixture->resource == IDD_MAIN ? IDC_ABOUT : 0);
    if (fixture->resource == IDD_MAIN && (message == WM_GETMINMAXINFO || message == WM_DPICHANGED))
        return Gui_MainWindowGeometry(dialog, message, wp, lp);
    if (fixture->realSessions) {
        LRESULT result;
        switch (message) {
        case WM_APP_SESSIONS:
            fixture->reloadRequests++;
            SessionsView_Reload();
            return TRUE;
        case WM_APP_SESSIONS_READY:
            SessionsView_Ready(TRUE);
            return TRUE;
        case WM_NOTIFY:
            if (!SessionsView_Notify((const NMHDR *)lp, &result)) break;
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
            return TRUE;
        case WM_DRAWITEM:
            if (SessionsView_DrawItem((const DRAWITEMSTRUCT *)lp)) return TRUE;
            break;
        case WM_COMMAND:
            if (SessionsView_Command(wp)) return TRUE;
            break;
        case WM_ENTERIDLE:
            if (wp == MSGF_DIALOGBOX) ClosePrompt(fixture, (HWND)lp);
            break;
        }
    }
    if (message == WM_SIZE && wp != SIZE_MINIMIZED && fixture->resource == IDD_MAIN) {
        SIZE minimum;
        if (Theme_MainMinimum(dialog, &minimum)) {
            Theme_LayoutMain(dialog);
            Gui_LayoutProfileColumns(fixture->table);
            if (fixture->realSessions) SessionsView_Resize();
        }
    }
    if (message == WM_DESTROY) Localize_ForgetWindow(dialog);
    return FALSE;
}

/* IDD_MAIN with a window class of the tests' own: the program finds its
 * manager window by its class (a second launch brings it forward, an install
 * closes it), and must never find a fixture. The class name keeps its
 * length, so the rest of the template keeps its layout. */
static void *g_mainTemplate;
static WCHAR g_fixtureClass[64];

static BOOL PrepareMainTemplate(void)
{
    HRSRC found = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), (LPCWSTR)RT_DIALOG);
    HGLOBAL loaded = found ? LoadResource(g_hInst, found) : NULL;
    const BYTE *source = loaded ? (const BYTE *)LockResource(loaded) : NULL;
    DWORD size = found ? SizeofResource(g_hInst, found) : 0;
    size_t length = wcslen(APP_WINDOW_CLASS), menuWords;
    WNDCLASSEXW window;
    WCHAR *menu, *windowClass;
    /* DLGTEMPLATEEX: its fixed part, then the menu and the class. */
    if (!source || size < 26 || ((const WORD *)source)[1] != 0xFFFF || length < 4 || length >= ARRAYSIZE(g_fixtureClass)) return FALSE;
    if ((g_mainTemplate = HeapAlloc(GetProcessHeap(), 0, size)) == NULL) return FALSE;
    CopyMemory(g_mainTemplate, source, size);
    menu = (WCHAR *)((BYTE *)g_mainTemplate + 26);
    menuWords = menu[0] == 0 ? 1 : menu[0] == 0xFFFF ? 2 : wcsnlen(menu, (size - 26) / sizeof(WCHAR)) + 1;
    windowClass = menu + menuWords;
    if ((BYTE *)(windowClass + length + 1) > (BYTE *)g_mainTemplate + size || wcsncmp(windowClass, APP_WINDOW_CLASS, length + 1) != 0) return FALSE;
    StringCchCopyW(g_fixtureClass, ARRAYSIZE(g_fixtureClass), APP_WINDOW_CLASS);
    CopyMemory(g_fixtureClass + length - 4, L"Test", 4 * sizeof(WCHAR));
    CopyMemory(windowClass, g_fixtureClass, length * sizeof(WCHAR));
    ZeroMemory(&window, sizeof window);
    window.cbSize = sizeof window;
    window.lpfnWndProc = DefDlgProcW;
    window.cbWndExtra = DLGWINDOWEXTRA;
    window.hInstance = g_hInst;
    window.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window.lpszClassName = g_fixtureClass;
    return RegisterClassExW(&window) != 0;
}

/* One of the program's dialogs, the manager window with the tests' class. */
static HWND CreateResourceDialog(int resource, DLGPROC proc, LPARAM param)
{
    if (resource == IDD_MAIN) return CreateDialogIndirectParamW(g_hInst, (LPCDLGTEMPLATEW)g_mainTemplate, NULL, proc, param);
    return CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(resource), NULL, proc, param);
}

static HWND CreateFixture(LayoutFixture *fixture)
{
    Localize_SetLanguage(fixture->language, FALSE);
    return CreateResourceDialog(fixture->resource, FixtureProc, (LPARAM)fixture);
}

static void DestroyFixture(HWND dialog, LayoutFixture *fixture)
{
    if (dialog) DestroyWindow(dialog);
    if (fixture->font) DeleteObject(fixture->font);
    fixture->font = NULL;
}

static LayoutFixture MainFixture(int language, int fontScale, BOOL sessions)
{
    LayoutFixture fixture;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_MAIN;
    fixture.language = language;
    fixture.fontScale = fontScale;
    fixture.sessions = sessions;
    fixture.noteName = L"Personal";
    fixture.links = LINKS_ROUTED;
    return fixture;
}

/* ------------------------------------------------------------ rendering */

/* What `control` paints in `region` of its client (WM_PRINTCLIENT over the
 * window's face), as 32-bit pixels. */
static BOOL RenderClient(HWND control, const RECT *region, DWORD *pixels)
{
    BITMAPINFO bitmap = { 0 };
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP image;
    HGDIOBJ previous;
    void *surface = NULL;
    RECT area;
    int width = region->right - region->left, height = region->bottom - region->top;
    if (!dc || width <= 0 || height <= 0) {
        if (dc) DeleteDC(dc);
        return FALSE;
    }
    bitmap.bmiHeader.biSize = sizeof bitmap.bmiHeader;
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    image = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &surface, NULL, 0);
    if (!image || !surface) {
        if (image) DeleteObject(image);
        DeleteDC(dc);
        return FALSE;
    }
    previous = SelectObject(dc, image);
    SetRect(&area, 0, 0, width, height);
    FillRect(dc, &area, Theme_Brush(THEME_FACE));
    SetViewportOrgEx(dc, -region->left, -region->top, NULL);
    IntersectClipRect(dc, region->left, region->top, region->right, region->bottom);
    SendMessageW(control, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    CopyMemory(pixels, surface, (size_t)width * (size_t)height * sizeof *pixels);
    SelectObject(dc, previous);
    DeleteObject(image);
    DeleteDC(dc);
    return TRUE;
}

static DWORD *AllocatePixels(int width, int height)
{
    return (DWORD *)malloc((size_t)max(1, width) * (size_t)max(1, height) * sizeof(DWORD));
}

/* Every Role caption is drawn whole: each pixel of the cell matches the same
 * row drawn in a wider column. */
static void CheckNativeRoles(const LayoutFixture *fixture)
{
    static const struct { BOOL stock, isDefault; } kRoles[] = { { TRUE, TRUE }, { TRUE, FALSE }, { FALSE, TRUE } };
    HWND table = fixture->table;
    RECT actual, wide, client;
    WCHAR original[LABEL_CCH];
    int role, originalWidth = ListView_GetColumnWidth(table, 1);
    ListView_GetItemText(table, 0, 1, original, ARRAYSIZE(original));
    GetClientRect(table, &client);
    for (role = 0; role < (int)ARRAYSIZE(kRoles); role++) {
        DWORD *shown = NULL, *reference = NULL, *blank = NULL;
        size_t pixel, referenceInk = 0;
        int width, referenceWidth, height;
        BOOL painted = FALSE, complete = TRUE;
        ListView_SetItemText(table, 0, 1, (WCHAR *)TR(Theme_ProfileRole(kRoles[role].stock, kRoles[role].isDefault)));
        if (!ListView_GetSubItemRect(table, 0, 1, LVIR_BOUNDS, &actual)) {
            Check(fixture, table, "native Role subitem bounds available", FALSE);
            continue;
        }
        width = actual.right - actual.left;
        height = actual.bottom - actual.top;
        ListView_SetColumnWidth(table, 1, originalWidth + MulDiv(64, fixture->fontScale, 96));
        ListView_GetSubItemRect(table, 0, 1, LVIR_BOUNDS, &wide);
        referenceWidth = wide.right - wide.left;
        shown = AllocatePixels(width, height);
        reference = AllocatePixels(referenceWidth, height);
        blank = AllocatePixels(referenceWidth, height);
        if (shown && reference && blank && width > 0 && height > 0 && referenceWidth > width && wide.right <= client.right) {
            painted = RenderClient(table, &wide, reference);
            ListView_SetItemText(table, 0, 1, L"");
            painted = RenderClient(table, &wide, blank) && painted;
            ListView_SetColumnWidth(table, 1, originalWidth);
            ListView_SetItemText(table, 0, 1, (WCHAR *)TR(Theme_ProfileRole(kRoles[role].stock, kRoles[role].isDefault)));
            painted = RenderClient(table, &actual, shown) && painted;
            if (painted) for (pixel = 0; pixel < (size_t)referenceWidth * (size_t)height; pixel++) {
                size_t x = pixel % (size_t)referenceWidth, y = pixel / (size_t)referenceWidth;
                if ((reference[pixel] & 0xffffff) == (blank[pixel] & 0xffffff)) continue;
                referenceInk++;
                if (x >= (size_t)width || (shown[y * (size_t)width + x] & 0xffffff) != (reference[pixel] & 0xffffff)) complete = FALSE;
            }
        }
        ListView_SetColumnWidth(table, 1, originalWidth);
        Check(fixture, table, "native Role row has a complete wider reference render", painted && referenceInk > 0);
        Check(fixture, table, "every Role glyph matches the wider native row without ellipsis", painted && referenceInk > 0 && complete);
        free(blank);
        free(reference);
        free(shown);
    }
    ListView_SetItemText(table, 0, 1, original);
}

/* gui.c draws the shortcut heading on one line (its badge, then the
 * profile's name cut to fit): that line must fit its height. */
static void CheckShortcutHeading(HWND dialog, const LayoutFixture *fixture)
{
    HWND heading = GetDlgItem(dialog, IDC_SC_GROUP);
    TEXTMETRICW metrics = { 0 };
    RECT client;
    BOOL measured = FALSE;
    HDC dc = GetDC(heading);
    if (dc) {
        HGDIOBJ previous = SelectObject(dc, (HFONT)SendMessageW(heading, WM_GETFONT, 0, 0));
        measured = GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        ReleaseDC(heading, dc);
    }
    GetClientRect(heading, &client);
    Check(fixture, heading, "shortcut heading holds one line of its font", measured && client.bottom >= metrics.tmHeight);
}

static void CheckNativeNote(HWND dialog, const LayoutFixture *fixture)
{
    HWND note = GetDlgItem(dialog, IDC_NOTE);
    HFONT font = (HFONT)SendMessageW(note, WM_GETFONT, 0, 0);
    const WCHAR *format = TR(Theme_MainNote()), *slot = wcsstr(format, L"%s");
    WCHAR text[1024], expectedName[LABEL_CCH];
    RECT client, position, calculated = { 0, 0, 0, 0 }, area;
    HDC dc;
    TEXTMETRICW metrics = { 0 };
    DWORD *shown = NULL, *empty = NULL, *reference = NULL, *referenceEmpty = NULL;
    size_t shownPixels, referencePixels, pixel, shownInk = 0, completeInk = 0, missingInk = 0;
    int width, height, referenceHeight, lastInkRow = -1;
    BOOL rendered, metricsReady = FALSE, fits;
    GetWindowTextW(note, text, ARRAYSIZE(text));
    FixtureName(fixture, expectedName, ARRAYSIZE(expectedName));
    if (slot) {
        size_t prefix = (size_t)(slot - format), suffix = wcslen(slot + 2), length = wcslen(text);
        BOOL prose = length >= prefix + suffix && wcsncmp(text, format, prefix) == 0 && wcscmp(text + length - suffix, slot + 2) == 0;
        Check(fixture, note, "localized note retains every fixed word around the profile name", prose);
        if (prose) {
            size_t expected = wcslen(expectedName), shownName = length - prefix - suffix;
            BOOL same = shownName == expected && wcsncmp(text + prefix, expectedName, expected) == 0;
            BOOL shortened = shownName >= 2 && shownName - 1 < expected && text[prefix + shownName - 1] == L'\x2026' &&
                             wcsncmp(text + prefix, expectedName, shownName - 1) == 0;
            Check(fixture, note, "profile name is preserved or shortened only by a final ellipsis", same || shortened);
        }
    } else Check(fixture, note, "localized note format contains its profile name argument", FALSE);
    GetClientRect(note, &client);
    position = RelativeRect(dialog, note);
    width = client.right;
    height = client.bottom;
    dc = GetDC(note);
    if (dc && font) {
        HGDIOBJ previous = SelectObject(dc, font);
        metricsReady = GetTextMetricsW(dc, &metrics);
        calculated.right = client.right;
        DrawTextW(dc, text, -1, &calculated, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | Localize_ReadingFlags());
        SelectObject(dc, previous);
    }
    if (dc) ReleaseDC(note, dc);
    Check(fixture, note, "native note font metrics are available", metricsReady && metrics.tmHeight > 0);
    if (!metricsReady || metrics.tmHeight <= 0) return;
    Check(fixture, note, "wrapped note has no horizontally cropped word", calculated.right <= width);
    /* The note drawn taller, by the same native control: every pixel of its
     * text is there, with two clear lines below. */
    referenceHeight = max(height, calculated.bottom) + 2 * metrics.tmHeight;
    if (width <= 0 || height <= 0) {
        Check(fixture, note, "native note has a nonempty drawing area", FALSE);
        return;
    }
    shownPixels = (size_t)width * (size_t)height;
    referencePixels = (size_t)width * (size_t)referenceHeight;
    shown = AllocatePixels(width, height);
    empty = AllocatePixels(width, height);
    reference = AllocatePixels(width, referenceHeight);
    referenceEmpty = AllocatePixels(width, referenceHeight);
    if (!shown || !empty || !reference || !referenceEmpty) {
        Check(fixture, note, "native note raster buffers allocated", FALSE);
        goto done;
    }
    SetRect(&area, 0, 0, width, height);
    rendered = RenderClient(note, &area, shown);
    SetWindowTextW(note, L"");
    rendered = RenderClient(note, &area, empty) && rendered;
    SetWindowPos(note, NULL, 0, 0, width, referenceHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    area.bottom = referenceHeight;
    rendered = RenderClient(note, &area, referenceEmpty) && rendered;
    SetWindowTextW(note, text);
    rendered = RenderClient(note, &area, reference) && rendered;
    SetWindowPos(note, NULL, position.left, position.top, position.right - position.left,
                 position.bottom - position.top, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(fixture, note, "native STATIC caption renders for its visible and reference heights", rendered);
    if (!rendered) goto done;
    fits = TRUE;
    for (pixel = 0; pixel < referencePixels; pixel++) {
        DWORD ink = reference[pixel] & 0xffffff, blank = referenceEmpty[pixel] & 0xffffff;
        if (ink != blank) {
            completeInk++;
            lastInkRow = (int)(pixel / (size_t)width);
            if (pixel >= shownPixels || (shown[pixel] & 0xffffff) != ink || (empty[pixel] & 0xffffff) != blank) {
                missingInk++;
                fits = FALSE;
            }
        }
        if (pixel < shownPixels && (shown[pixel] & 0xffffff) != (empty[pixel] & 0xffffff)) shownInk++;
    }
    Check(fixture, note, "native reference has visible text and two clear font rows below its final line",
          completeInk > 0 && lastInkRow < referenceHeight - 2 * metrics.tmHeight);
    Check(fixture, note, "every localized note glyph remains visible in the native STATIC", fits && shownInk == completeInk);
    if (!fits || shownInk != completeInk)
        printf("        name=%ls area=%dx%d CALCRECT-height=%ld native-final-ink-row=%d font-height=%ld shown-ink=%zu complete-ink=%zu missing-ink=%zu\n",
               fixture->noteName ? fixture->noteName : L"MAX_LABEL", width, height, calculated.bottom, lastInkRow, metrics.tmHeight,
               shownInk, completeInk, missingInk);
done:
    free(referenceEmpty);
    free(reference);
    free(empty);
    free(shown);
}

static void CheckNoteGeometry(HWND dialog, const LayoutFixture *fixture)
{
    HWND note = GetDlgItem(dialog, IDC_NOTE);
    RECT client, table = RelativeRect(dialog, fixture->tableViewport), placed = RelativeRect(dialog, note);
    RECT setDefault = RelativeRect(dialog, GetDlgItem(dialog, IDC_DEFAULT)), remove = RelativeRect(dialog, GetDlgItem(dialog, IDC_DELETE));
    GetClientRect(dialog, &client);
    Check(fixture, note, "note stays beside the table and inside the dialog",
          placed.left >= table.right && placed.top >= 0 && placed.right <= client.right && placed.bottom <= client.bottom);
    Check(fixture, note, "note fits the table's vertical band", placed.bottom <= table.bottom);
    Check(fixture, GetDlgItem(dialog, IDC_DEFAULT), "default button fits between Delete and the complete note",
          setDefault.top >= remove.bottom && setDefault.bottom <= placed.top);
}

static BOOL IntentionalEllipsis(HWND control)
{
    LONG style = GetWindowLongW(control, GWL_STYLE);
    return IsClass(control, WC_STATICW) && (style & (SS_ENDELLIPSIS | SS_PATHELLIPSIS | SS_WORDELLIPSIS));
}

/* What of a push button `box` of `owner` its caption may take: inside the
 * frame Windows' button theme draws (its content margins). */
static RECT PushButtonTextArea(HWND owner, const RECT *box)
{
    RECT area = *box;
    HTHEME theme = OpenThemeData(owner, L"Button");
    HDC dc = GetDC(owner);
    if (theme && dc) GetThemeBackgroundContentRect(theme, dc, BP_PUSHBUTTON, PBS_NORMAL, box, &area);
    else InflateRect(&area, -GetSystemMetricsForDpi(SM_CXEDGE, GetDpiForWindow(owner)), -GetSystemMetricsForDpi(SM_CYEDGE, GetDpiForWindow(owner)));
    if (dc) ReleaseDC(owner, dc);
    if (theme) CloseThemeData(theme);
    return area;
}

static void CheckText(const LayoutFixture *fixture, HWND control)
{
    WCHAR text[4096];
    RECT client, measured = { 0, 0, 0, 0 };
    BOOL button = IsClass(control, WC_BUTTONW), label = IsClass(control, WC_STATICW);
    BOOL edit = IsClass(control, WC_EDITW), combo = IsClass(control, WC_COMBOBOXW), link = IsClass(control, WC_LINK);
    LONG style = GetWindowLongW(control, GWL_STYLE);
    HDC dc;
    HGDIOBJ previous;
    TEXTMETRICW metrics;
    UINT flags = Localize_ReadingFlags();
    int type = button ? style & BS_TYPEMASK : style & SS_TYPEMASK;
    if (!button && !label && !edit && !combo && !link) return;
    if (label && (type == SS_ICON || type == SS_BITMAP || type == SS_OWNERDRAW)) return;
    GetWindowTextW(control, text, ARRAYSIZE(text));
    GetClientRect(control, &client);
    dc = GetDC(control);
    previous = SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &metrics);
    if (link) {
        SIZE ideal = { 0, 0 };
        BOOL available = (BOOL)SendMessageW(control, LM_GETIDEALSIZE, client.right, (LPARAM)&ideal);
        Check(fixture, control, "native link ideal size is available", available && ideal.cy > 0);
        Check(fixture, control, "credit link lines fit their actual height", available && ideal.cy <= client.bottom);
        Check(fixture, control, "credit link lines fit their actual width", available && ideal.cx <= client.right);
    } else if (combo) {
        /* Its choices open in a menu (theme.c): the closed box, a drop-down
         * button showing the choice, is what shows. */
        HFONT font = (HFONT)SendMessageW(control, WM_GETFONT, 0, 0);
        int row, rows = (int)SendMessageW(control, CB_GETCOUNT, 0, 0);
        for (row = 0; row < rows; row++) {
            SendMessageW(control, CB_GETLBTEXT, row, (LPARAM)text);
            Check(fixture, control, "every choice fits the closed drop-down list", Theme_DropDownWidth(control, font, text) <= client.right);
        }
        Check(fixture, control, "closed drop-down list is as tall as its script font", client.bottom >= metrics.tmHeight);
    } else if (edit) {
        RECT textArea;
        SendMessageW(control, EM_GETRECT, 0, (LPARAM)&textArea);
        Check(fixture, control, "horizontally scrolling edit retains full font height", textArea.bottom - textArea.top >= metrics.tmHeight);
    } else if (text[0]) {
        BOOL wrap = label && type == SS_LEFT && !IntentionalEllipsis(control);
        if (button && (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX)) {
            SIZE ideal = { 0, 0 };
            Check(fixture, control, "native checkbox glyph and caption fit the allocated control",
                  Theme_CheckBoxSize(control, &ideal) && ideal.cx <= client.right && ideal.cy <= client.bottom);
        } else {
            BOOL pushButton = button && (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON);
            RECT area = pushButton ? PushButtonTextArea(control, &client) : client;
            measured.right = max(1, client.right);
            DrawTextW(dc, text, -1, &measured, DT_CALCRECT | (wrap ? DT_WORDBREAK : DT_SINGLELINE) |
                      ((label && (style & SS_NOPREFIX)) ? DT_NOPREFIX : 0) | flags);
            Check(fixture, control, "caption font and wrapped lines fit actual height", measured.bottom <= client.bottom);
            if (pushButton) Check(fixture, control, "button caption fits inside its frame", measured.right <= area.right - area.left);
            else if (!IntentionalEllipsis(control)) Check(fixture, control, "caption fits actual width", measured.right <= client.right);
        }
    }
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
}

static void CheckShortcutRows(HWND dialog, const LayoutFixture *fixture)
{
    static const int kShortcuts[] = { IDC_SC_DESKTOP, IDC_SC_SAVEAS, IDC_SC_PIN, IDC_SC_START };
    RECT buttons[ARRAYSIZE(kShortcuts)], group = RelativeRect(dialog, GetDlgItem(dialog, IDC_SC_GROUP));
    int i, gap = MulDiv(THEME_MAIN_GAP_DIPS, (int)GetDpiForWindow(dialog), 96);
    for (i = 0; i < (int)ARRAYSIZE(kShortcuts); i++) buttons[i] = RelativeRect(dialog, GetDlgItem(dialog, kShortcuts[i]));
    Check(fixture, GetDlgItem(dialog, IDC_SC_DESKTOP), "shortcut row starts at its group's left edge", buttons[0].left == group.left);
    Check(fixture, GetDlgItem(dialog, IDC_SC_START), "shortcut row finishes at its group's right edge", buttons[3].right == group.right);
    if (buttons[0].top == buttons[3].top) {
        for (i = 1; i < (int)ARRAYSIZE(kShortcuts); i++) {
            int actualGap = buttons[i].left - buttons[i - 1].right, firstGap = buttons[1].left - buttons[0].right;
            Check(fixture, GetDlgItem(dialog, kShortcuts[i]), "single shortcut row distributes its free space evenly",
                  actualGap >= gap && abs(actualGap - firstGap) <= 1);
            Check(fixture, GetDlgItem(dialog, kShortcuts[i]), "single shortcut row shares a baseline",
                  buttons[i].top == buttons[0].top && buttons[i].bottom == buttons[0].bottom);
        }
    } else {
        Check(fixture, GetDlgItem(dialog, IDC_SC_SAVEAS), "first fallback row uses its group's full width", buttons[1].right == group.right);
        Check(fixture, GetDlgItem(dialog, IDC_SC_PIN), "second fallback row uses its group's full width", buttons[2].left == group.left);
        Check(fixture, GetDlgItem(dialog, IDC_SC_SAVEAS), "first fallback row retains its minimum gap", buttons[1].left - buttons[0].right >= gap);
        Check(fixture, GetDlgItem(dialog, IDC_SC_START), "second fallback row retains its minimum gap", buttons[3].left - buttons[2].right >= gap);
    }
}

/* The middle of the first line of `label`'s text (drawn at its top), in the
 * dialog's client. */
static int FirstLineMiddle(HWND dialog, HWND label)
{
    TEXTMETRICW metrics = { 0 };
    RECT rect = RelativeRect(dialog, label);
    HDC dc = GetDC(label);
    if (dc) {
        HGDIOBJ previous = SelectObject(dc, (HFONT)SendMessageW(label, WM_GETFONT, 0, 0));
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        ReleaseDC(label, dc);
    }
    return rect.top + metrics.tmHeight / 2;
}

/* The status and the version link read on the same line as the buttons
 * beside them (a button centers its caption, a label draws at its top). */
static void CheckLabelsLevelWithButtons(HWND dialog, const LayoutFixture *fixture)
{
    static const struct { int label, button; const char *name; } kRows[] = {
        { IDC_STATUS, IDC_LANGUAGE, "the status's text is level with the header buttons' captions" },
        { IDC_ABOUT, IDC_UNINSTALL, "the version link's first line is level with the footer buttons' captions" }
    };
    size_t i;
    for (i = 0; i < ARRAYSIZE(kRows); i++) {
        HWND label = GetDlgItem(dialog, kRows[i].label);
        RECT button = RelativeRect(dialog, GetDlgItem(dialog, kRows[i].button));
        Check(fixture, label, kRows[i].name, abs(FirstLineMiddle(dialog, label) - (button.top + button.bottom) / 2) <= 1);
    }
}

/* Every visible control inside the client, captioned whole and apart from
 * the others; the dialog inside its monitor's work area. */
static void CheckGeometry(HWND dialog, const LayoutFixture *fixture)
{
    HWND controls[128], child;
    RECT client;
    int count = 0, i, j;
    GetClientRect(dialog, &client);
    Check(fixture, NULL, "dialog stays inside monitor work area", InsideWorkArea(dialog));
    for (child = GetWindow(dialog, GW_CHILD); child && count < (int)ARRAYSIZE(controls); child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect;
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        rect = RelativeRect(dialog, child);
        Check(fixture, child, "visible control stays inside dialog client", rect.left >= 0 && rect.top >= 0 &&
              rect.right <= client.right && rect.bottom <= client.bottom);
        CheckText(fixture, child);
        controls[count++] = child;
    }
    for (i = 0; i < count; i++) {
        RECT first = RelativeRect(dialog, controls[i]);
        for (j = i + 1; j < count; j++) {
            RECT second = RelativeRect(dialog, controls[j]), overlap;
            BOOL clear = !IntersectRect(&overlap, &first, &second) || overlap.right - overlap.left <= 1 || overlap.bottom - overlap.top <= 1;
            Check(fixture, controls[i], "visible controls do not overlap", clear);
            if (!clear) printf("        other=%d overlap=%ldx%ld\n", GetDlgCtrlID(controls[j]), overlap.right - overlap.left, overlap.bottom - overlap.top);
        }
    }
    if (fixture->resource == IDD_MAIN && !fixture->sessions) CheckShortcutRows(dialog, fixture);
    if (fixture->resource == IDD_MAIN) CheckLabelsLevelWithButtons(dialog, fixture);
}

/* The table's columns as gui.c sizes them by default, and every Role
 * caption whole. */
static void CheckProfileTable(HWND dialog, const LayoutFixture *fixture)
{
    HWND table = fixture->table;
    RECT client;
    int first = ListView_GetColumnWidth(table, 0), second = ListView_GetColumnWidth(table, 1);
    GetClientRect(table, &client);
    Check(fixture, table, "default Profile column is its fixed width at the window's DPI",
          first == MulDiv(THEME_PROFILE_COLUMN_DIPS, (int)GetDpiForWindow(table), 96));
    Check(fixture, table, "default Data column consumes exactly the remaining client width",
          ListView_GetColumnWidth(table, 2) == client.right - first - second);
    Check(fixture, table, "default columns do not introduce a horizontal scroll bar", !(GetWindowLongW(table, GWL_STYLE) & WS_HSCROLL));
    CheckNativeRoles(fixture);
    CheckShortcutHeading(dialog, fixture);
}

static void CheckHiddenProfileNote(HWND dialog, const LayoutFixture *fixture)
{
    struct { HWND window; RECT rect; } controls[128];
    HWND child, note = GetDlgItem(dialog, IDC_NOTE);
    RECT client, frame, actual;
    WCHAR name[LABEL_CCH], expected[1024], text[1024];
    const WCHAR *format = TR(Theme_MainNote());
    BOOL unchanged = TRUE;
    int count = 0, i;
    Check(fixture, note, "the sessions view hides the profile note", !(GetWindowLongW(note, GWL_STYLE) & WS_VISIBLE));
    FixtureName(fixture, name, ARRAYSIZE(name));
    StringCchPrintfW(expected, ARRAYSIZE(expected), format, name);
    GetClientRect(dialog, &client);
    GetWindowRect(dialog, &frame);
    for (child = GetWindow(dialog, GW_CHILD); child && count < (int)ARRAYSIZE(controls); child = GetWindow(child, GW_HWNDNEXT)) {
        controls[count].window = child;
        controls[count++].rect = RelativeRect(dialog, child);
    }
    SetWindowTextW(note, L"Private stale note");
    Theme_LayoutSidebarNote(note, format, name);
    GetClientRect(dialog, &actual);
    unchanged = EqualRect(&client, &actual);
    GetWindowRect(dialog, &actual);
    unchanged = unchanged && EqualRect(&frame, &actual);
    for (i = 0; i < count; i++) {
        actual = RelativeRect(dialog, controls[i].window);
        unchanged = unchanged && EqualRect(&controls[i].rect, &actual);
    }
    Check(fixture, note, "hidden profile note leaves dialog and control geometry unchanged", unchanged);
    GetWindowTextW(note, text, ARRAYSIZE(text));
    Check(fixture, note, "hidden profile note receives its complete current localized caption", wcscmp(text, expected) == 0);
}

/* The details' widest captions fit their column: "Delete session
 * everywhere..." on its button as wide as the column (sessions.c), and the
 * Actions box with its arrow. */
static void CheckSessionActionWidths(HWND dialog, const LayoutFixture *fixture)
{
    HWND details = GetDlgItem(dialog, IDC_S_DETAILS);
    HFONT font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    RECT client, area, caption = { 0, 0, 0, 0 };
    HDC dc = GetDC(details);
    HGDIOBJ previous = SelectObject(dc, font);
    int actionWidth;
    GetClientRect(details, &client);
    area = PushButtonTextArea(details, &client);
    DrawTextW(dc, TR(Theme_SessionsCaption(SESSIONS_DELETE_EVERYWHERE)), -1, &caption, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | Localize_ReadingFlags());
    Check(fixture, details, "destructive action caption fits its button without ellipsis", caption.right <= area.right - area.left);
    if (caption.right > area.right - area.left) printf("        available=%ld required=%ld\n", area.right - area.left, caption.right);
    actionWidth = Theme_DropDownWidth(dialog, font, TR(Theme_SessionsCaption(SESSIONS_ACTIONS)));
    Check(fixture, details, "an Actions box as wide as its caption fits its details column", actionWidth <= client.right);
    SelectObject(dc, previous);
    ReleaseDC(details, dc);
}

/* ------------------------------------------------------- live languages */

typedef struct FontState {
    BOOL available;
    LOGFONTW logical;
    TEXTMETRICW metrics;
} FontState;

typedef struct LayoutState {
    RECT client;
    struct { HWND window; RECT rect; } children[128];
    struct { HWND window; FontState font; } fonts[128];
    FontState dialogFont, roles[THEME_FONTS];
    int childCount, fontCount;
} LayoutState;

static FontState ReadFont(HWND window, HFONT font)
{
    FontState state;
    HDC dc;
    HGDIOBJ previous;
    ZeroMemory(&state, sizeof state);
    if (!font || !GetObjectW(font, sizeof state.logical, &state.logical)) return state;
    dc = GetDC(window);
    if (!dc) return state;
    previous = SelectObject(dc, font);
    state.available = GetTextMetricsW(dc, &state.metrics);
    SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return state;
}

static BOOL SameFont(const FontState *a, const FontState *b)
{
    return a->available && b->available &&
           a->logical.lfHeight == b->logical.lfHeight && a->logical.lfWidth == b->logical.lfWidth &&
           a->logical.lfWeight == b->logical.lfWeight && a->logical.lfCharSet == b->logical.lfCharSet &&
           a->logical.lfItalic == b->logical.lfItalic && a->logical.lfUnderline == b->logical.lfUnderline &&
           a->logical.lfStrikeOut == b->logical.lfStrikeOut &&
           wcscmp(a->logical.lfFaceName, b->logical.lfFaceName) == 0 &&
           a->metrics.tmHeight == b->metrics.tmHeight && a->metrics.tmAscent == b->metrics.tmAscent &&
           a->metrics.tmDescent == b->metrics.tmDescent && a->metrics.tmAveCharWidth == b->metrics.tmAveCharWidth;
}

static BOOL CALLBACK ReadChildFont(HWND child, LPARAM param)
{
    LayoutState *state = (LayoutState *)param;
    FontState font = ReadFont(child, (HFONT)SendMessageW(child, WM_GETFONT, 0, 0));
    if (font.available && state->fontCount < (int)ARRAYSIZE(state->fonts)) {
        state->fonts[state->fontCount].window = child;
        state->fonts[state->fontCount++].font = font;
    }
    return TRUE;
}

static void ReadLayout(HWND dialog, LayoutState *state)
{
    HWND child;
    ThemeFonts roles = { 0 };
    int i;
    ZeroMemory(state, sizeof *state);
    GetClientRect(dialog, &state->client);
    state->dialogFont = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0));
    for (child = GetWindow(dialog, GW_CHILD); child && state->childCount < (int)ARRAYSIZE(state->children);
         child = GetWindow(child, GW_HWNDNEXT)) {
        state->children[state->childCount].window = child;
        state->children[state->childCount++].rect = RelativeRect(dialog, child);
    }
    EnumChildWindows(dialog, ReadChildFont, (LPARAM)state);
    Theme_CreateFonts(dialog, &roles);
    for (i = 0; i < THEME_FONTS; i++) state->roles[i] = ReadFont(dialog, roles.font[i]);
    Theme_FreeFonts(&roles);
}

static void CompareLayout(HWND dialog, const LayoutFixture *fixture, const LayoutState *expected)
{
    LayoutState actual;
    int i;
    ReadLayout(dialog, &actual);
    Check(fixture, NULL, "same-language dialog font returns to its original face, size, weight and metrics",
          SameFont(&expected->dialogFont, &actual.dialogFont));
    for (i = 0; i < expected->fontCount; i++) {
        HWND child = expected->fonts[i].window;
        FontState font = ReadFont(child, (HFONT)SendMessageW(child, WM_GETFONT, 0, 0));
        Check(fixture, child, "same control font survives language round trips without changing metrics",
              IsChild(dialog, child) && SameFont(&expected->fonts[i].font, &font));
    }
    for (i = 0; i < THEME_FONTS; i++)
        Check(fixture, NULL, "derived text roles retain their original size, face and metrics", SameFont(&expected->roles[i], &actual.roles[i]));
    Check(fixture, NULL, "same-language client size is independent of previous languages", EqualRect(&expected->client, &actual.client));
    Check(fixture, NULL, "language changes retain the original direct controls", expected->childCount == actual.childCount);
    for (i = 0; i < expected->childCount; i++) {
        HWND child = expected->children[i].window;
        RECT rect = RelativeRect(dialog, child);
        Check(fixture, child, "same-language control geometry is independent of previous languages",
              GetParent(child) == dialog && EqualRect(&expected->children[i].rect, &rect));
    }
}

static INT_PTR CALLBACK TransitionProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        LayoutFixture *fixture = (LayoutFixture *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        InitializeFixture(dialog, fixture, TransitionCaptions);
        return TRUE;
    }
    return FixtureProc(dialog, message, wp, lp);
}

typedef struct FrameProbe {
    RECT client;
    int changes;
} FrameProbe;

static LRESULT CALLBACK ObserveFrame(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    FrameProbe *probe = (FrameProbe *)data;
    (void)id;
    if (message == WM_WINDOWPOSCHANGED) {
        RECT client;
        GetClientRect(window, &client);
        if (!EqualRect(&client, &probe->client)) probe->changes++;
    }
    return DefSubclassProc(window, message, wp, lp);
}

/* What gui.c does when the user picks another language, on a mock window. */
static void TransitionLanguage(HWND dialog, LayoutFixture *fixture, int language)
{
    FontState before = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0)), after;
    RECT previous, current;
    FrameProbe probe;
    GetClientRect(dialog, &previous);
    probe.client = previous;
    probe.changes = 0;
    SetWindowSubclass(dialog, ObserveFrame, FRAME_PROBE_SUBCLASS, (DWORD_PTR)&probe);
    fixture->language = language;
    Check(fixture, NULL, "live transition language selected for this process only", Localize_SetLanguage(language, FALSE));
    Localize_Window(dialog);
    Theme_Apply(dialog);
    TransitionCaptions(dialog, fixture);
    Theme_FitDialog(dialog);
    if (fixture->resource == IDD_MAIN) {
        Gui_LayoutProfileColumns(fixture->table);
        if (fixture->sessions) ShowMockSessions(dialog);
        else LayoutMockNote(dialog, fixture);
    }
    after = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0));
    GetClientRect(dialog, &current);
    if (fixture->resource == IDD_MAIN) {
        Check(fixture, NULL, "switching languages preserves the common main client size", EqualRect(&previous, &current));
        Check(fixture, NULL, "switching languages causes no intermediate native frame resize", probe.changes == 0);
    }
    RemoveWindowSubclass(dialog, ObserveFrame, FRAME_PROBE_SUBCLASS);
    Check(fixture, NULL, "dialog face covers the selected script", after.available && wcscmp(after.logical.lfFaceName, Localize_FontFace()) == 0);
    Check(fixture, NULL, "changing scripts preserves logical height, width, weight and charset", before.available && after.available &&
          before.logical.lfHeight == after.logical.lfHeight && before.logical.lfWidth == after.logical.lfWidth &&
          before.logical.lfWeight == after.logical.lfWeight && before.logical.lfCharSet == after.logical.lfCharSet);
}

typedef struct ModalCapture {
    LayoutFixture fixture;
    LayoutState *state;
} ModalCapture;

#define WM_MODAL_CAPTURE (WM_APP + 241)

/* The point size of a dialog template's FONT (app.rc): in a DLGTEMPLATEEX,
 * after its fixed part (13 words), its menu, class and title. */
static int ResourceFontPoints(int resource)
{
    HRSRC found = FindResourceW(g_hInst, MAKEINTRESOURCEW(resource), (LPCWSTR)RT_DIALOG);
    HGLOBAL loaded = found ? LoadResource(g_hInst, found) : NULL;
    const WORD *words = loaded ? (const WORD *)LockResource(loaded) : NULL;
    size_t count = found ? SizeofResource(g_hInst, found) / sizeof(WORD) : 0, at = 13, field;
    DWORD style;
    if (!words || count < 16 || words[1] != 0xFFFF) return 0;
    style = (DWORD)words[6] | ((DWORD)words[7] << 16);
    for (field = 0; field < 3 && at < count; field++) {
        if (field < 2 && words[at] == 0xFFFF) at += 2;   /* an ordinal */
        else {
            while (at < count && words[at]) at++;
            at++;
        }
    }
    return (style & DS_SETFONT) && at < count ? (int)words[at] : 0;
}

static INT_PTR CALLBACK ModalCaptureProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ModalCapture *capture = (ModalCapture *)GetWindowLongPtrW(dialog, DWLP_USER);
    (void)wp;
    if (message == WM_INITDIALOG) {
        BOOL queued;
        capture = (ModalCapture *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        TransitionCaptions(dialog, &capture->fixture);
        queued = PostMessageW(dialog, WM_MODAL_CAPTURE, 0, 0);
        Check(&capture->fixture, NULL, "private modal capture queued after shared dialog initialization", queued);
        if (!queued) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_MODAL_CAPTURE && capture) {
        int dpi = (int)GetDpiForWindow(dialog);
        ReadLayout(dialog, capture->state);
        Check(&capture->fixture, NULL, "shared modal shell retains its own native resource font size",
              capture->state->dialogFont.available && ResourceFontPoints(IDD_PROFILE) > 0 &&
              capture->state->dialogFont.logical.lfHeight == -MulDiv(ResourceFontPoints(IDD_PROFILE), dpi, 72));
        Check(&capture->fixture, NULL, "shared modal shell font covers the current script", capture->state->dialogFont.available &&
              wcscmp(capture->state->dialogFont.logical.lfFaceName, Localize_FontFace()) == 0);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}

static void ReadPrivateModal(HWND owner, const LayoutFixture *ownerFixture, LayoutState *state)
{
    ModalCapture capture;
    INT_PTR result;
    ZeroMemory(&capture, sizeof capture);
    ZeroMemory(state, sizeof *state);
    capture.fixture.resource = IDD_PROFILE;
    capture.fixture.language = ownerFixture->language;
    capture.fixture.fontScale = 96;
    capture.state = state;
    result = Ui_Dialog(owner, IDD_PROFILE, ModalCaptureProc, (LPARAM)&capture);
    Check(&capture.fixture, NULL, "private modal opens and closes through the shared dialog shell", result == IDOK);
}

/* A modal dialog of the manager uses the current language's script and its
 * own resource font, whatever its owner's scale and earlier languages. */
static void CheckReopenedModal(HWND owner, LayoutFixture *fixture, const LayoutState *french)
{
    LayoutState initial, script, reopened;
    int i, chinese = Language(L"zh-CN");
    ReadPrivateModal(owner, fixture, &initial);
    TransitionLanguage(owner, fixture, chinese);
    ReadPrivateModal(owner, fixture, &script);
    Check(fixture, NULL, "closed modal uses the current script independently of its owner's resource font",
          script.dialogFont.available && wcscmp(script.dialogFont.logical.lfFaceName, Localize_FontFaceAt(chinese)) == 0 &&
          initial.dialogFont.logical.lfHeight == script.dialogFont.logical.lfHeight);
    TransitionLanguage(owner, fixture, Language(L"fr"));
    ReadPrivateModal(owner, fixture, &reopened);
    Check(fixture, NULL, "reopened French modal keeps its own original font and metrics", SameFont(&initial.dialogFont, &reopened.dialogFont));
    Check(fixture, NULL, "reopened French modal returns to its original client size", EqualRect(&initial.client, &reopened.client));
    Check(fixture, NULL, "reopened French modal retains the same resource control count", initial.childCount == reopened.childCount);
    if (initial.childCount == reopened.childCount) for (i = 0; i < initial.childCount; i++)
        Check(fixture, NULL, "reopened French modal retains its original resource geometry",
              EqualRect(&initial.children[i].rect, &reopened.children[i].rect));
    CompareLayout(owner, fixture, french);
}

/* The same dialog goes from French to scripts of every shape and back: its
 * fonts, size and controls come back exactly. */
static void CheckLanguageRoundTrips(void)
{
    static const int kResources[] = { IDD_MAIN, IDD_PROFILE, IDD_TITLE, IDD_MESSAGE, IDD_UNINSTALL };
    static const WCHAR *const kVisited[] = { L"zh-CN", L"hi", L"bn", L"ar", L"de" };
    size_t resource, scale, visited;
    int view, round, french = Language(L"fr");
    for (resource = 0; resource < ARRAYSIZE(kResources); resource++) for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++)
        for (view = 0; view < (kResources[resource] == IDD_MAIN ? 2 : 1); view++) {
            LayoutFixture fixture;
            LayoutState frenchLayout;
            HWND dialog;
            if (!g_scaleFits[scale]) continue;
            ZeroMemory(&fixture, sizeof fixture);
            fixture.resource = kResources[resource];
            fixture.language = french;
            fixture.fontScale = kFontScales[scale];
            fixture.sessions = view != 0;
            fixture.links = LINKS_ROUTED;
            Localize_SetLanguage(french, FALSE);
            dialog = CreateResourceDialog(fixture.resource, TransitionProc, (LPARAM)&fixture);
            Check(&fixture, NULL, "live transition private resource dialog created", dialog != NULL);
            if (dialog) {
                ReadLayout(dialog, &frenchLayout);
                for (round = 0; round < 2; round++) for (visited = 0; visited < ARRAYSIZE(kVisited); visited++) {
                    LayoutState translated;
                    TransitionLanguage(dialog, &fixture, Language(kVisited[visited]));
                    ReadLayout(dialog, &translated);
                    TransitionLanguage(dialog, &fixture, Language(kVisited[visited]));
                    CompareLayout(dialog, &fixture, &translated);
                    TransitionLanguage(dialog, &fixture, french);
                    CompareLayout(dialog, &fixture, &frenchLayout);
                }
                if (fixture.resource == IDD_MAIN) CheckReopenedModal(dialog, &fixture, &frenchLayout);
            }
            DestroyFixture(dialog, &fixture);
        }
}

/* ------------------------------------------------------ the main window */

/* Whether the dialogs scaled by each font scale fit this monitor's work
 * area: the manager window, the largest, needs its minimum. A smaller
 * desktop (a hosted build runner) checks the scales that fit, and says so. */
static void MeasureScalesAgainstWorkArea(void)
{
    size_t scale;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
        LayoutFixture fixture = MainFixture(Language(L"en"), kFontScales[scale], FALSE);
        HWND dialog = CreateFixture(&fixture);
        SIZE minimum = { 0, 0 };
        RECT frame;
        MONITORINFO monitor = { sizeof monitor };
        g_scaleFits[scale] = FALSE;
        if (dialog && Theme_MainMinimum(dialog, &minimum) && GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
            SetRect(&frame, 0, 0, minimum.cx, minimum.cy);
            AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dialog, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE),
                                     GetDpiForWindow(dialog));
            g_scaleFits[scale] = frame.right - frame.left <= monitor.rcWork.right - monitor.rcWork.left &&
                                 frame.bottom - frame.top <= monitor.rcWork.bottom - monitor.rcWork.top;
            if (!g_scaleFits[scale])
                printf("  skip  font scale %d: the manager window needs %ldx%ld, more than this monitor's %ldx%ld work area\n",
                       kFontScales[scale], frame.right - frame.left, frame.bottom - frame.top,
                       monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top);
        }
        Check(&fixture, NULL, "main window measured at each font scale", dialog && minimum.cx > 0);
        DestroyFixture(dialog, &fixture);
    }
}

/* The checks made at the monitor's own scale (kFontScales[0]) need it to fit. */
static BOOL NativeScaleFits(const char *what)
{
    if (g_scaleFits[0]) return TRUE;
    printf("  skip  %s: font scale %d does not fit this monitor's work area\n", what, kFontScales[0]);
    return FALSE;
}

/* A dialog's rows hidden before it is fitted. */
typedef struct HiddenRowsCase {
    int resource, rowAbove, lastHidden;   /* the hidden band: below rowAbove's bottom, down to lastHidden's */
    int hidden[3], hiddenCount;
    const char *name;
} HiddenRowsCase;

/* Rows a dialog hides close up, the gap above them kept: the profile dialog
 * without "Copy settings" (no profile to copy), without "Open it now" (no
 * Claude) or without both (a profile edited), the uninstall dialog without
 * its profiles (only Claude's own). Every row below the band moves up by it,
 * the rows above stay; a second fit changes nothing. */
static void CheckHiddenRows(void)
{
    static const HiddenRowsCase kCases[] = {
        { IDD_PROFILE, IDC_P_STARTUP, IDC_P_OPEN, { IDC_P_COPY, IDC_P_OPEN, 0 }, 2, "the profile dialog without its last check boxes" },
        { IDD_PROFILE, IDC_P_STARTUP, IDC_P_COPY, { IDC_P_COPY, 0, 0 }, 1, "the profile dialog without Copy settings" },
        { IDD_PROFILE, IDC_P_COPY, IDC_P_OPEN, { IDC_P_OPEN, 0, 0 }, 1, "the profile dialog without Open it now" },
        { IDD_UNINSTALL, IDC_U_KEEP, IDC_U_HINT, { IDC_U_LABEL, IDC_U_LIST, IDC_U_HINT }, 3, "the uninstall dialog without its profiles" },
    };
    size_t i, scale;
    for (i = 0; i < ARRAYSIZE(kCases); i++) for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
        LayoutFixture whole, closed;
        HWND wholeDialog, closedDialog;
        char name[200];
        if (!g_scaleFits[scale]) continue;
        ZeroMemory(&whole, sizeof whole);
        whole.resource = kCases[i].resource;
        whole.language = Language(L"en");
        whole.fontScale = kFontScales[scale];
        closed = whole;
        closed.hiddenControls = kCases[i].hidden;
        closed.hiddenCount = kCases[i].hiddenCount;
        wholeDialog = CreateFixture(&whole);
        closedDialog = CreateFixture(&closed);
        Check(&closed, NULL, "hidden rows fixtures created", wholeDialog && closedDialog);
        if (wholeDialog && closedDialog) {
            RECT aboveWhole = RelativeRect(wholeDialog, GetDlgItem(wholeDialog, kCases[i].rowAbove));
            RECT lastWhole = RelativeRect(wholeDialog, GetDlgItem(wholeDialog, kCases[i].lastHidden));
            RECT clientWhole, clientClosed, clientAgain;
            LayoutState before, after;
            HWND child, twin;
            int band = lastWhole.bottom - aboveWhole.bottom, moved = 0, kept = 0, childIndex;
            BOOL rowsFollow = TRUE, same = TRUE;
            GetClientRect(wholeDialog, &clientWhole);
            GetClientRect(closedDialog, &clientClosed);
            /* The same template: the same controls, in the same order (labels share an id). */
            for (child = GetWindow(closedDialog, GW_CHILD), twin = GetWindow(wholeDialog, GW_CHILD); child && twin;
                 child = GetWindow(child, GW_HWNDNEXT), twin = GetWindow(twin, GW_HWNDNEXT)) {
                RECT closedRect, wholeRect;
                if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
                closedRect = RelativeRect(closedDialog, child);
                wholeRect = RelativeRect(wholeDialog, twin);
                if (wholeRect.top >= lastWhole.bottom) {
                    moved++;
                    if (closedRect.top != wholeRect.top - band) rowsFollow = FALSE;
                } else {
                    kept++;
                    if (closedRect.top != wholeRect.top) rowsFollow = FALSE;
                }
            }
            StringCchPrintfA(name, ARRAYSIZE(name), "%s moves the rows below them up by their band, from the row above them", kCases[i].name);
            Check(&closed, NULL, name, band > 0 && moved > 0 && kept > 0 && rowsFollow);
            StringCchPrintfA(name, ARRAYSIZE(name), "%s is shorter by that band", kCases[i].name);
            Check(&closed, NULL, name, clientWhole.bottom - clientClosed.bottom == band);
            ReadLayout(closedDialog, &before);
            Theme_FitDialog(closedDialog);
            ReadLayout(closedDialog, &after);
            GetClientRect(closedDialog, &clientAgain);
            for (childIndex = 0; childIndex < before.childCount && childIndex < after.childCount; childIndex++)
                same = same && EqualRect(&before.children[childIndex].rect, &after.children[childIndex].rect);
            StringCchPrintfA(name, ARRAYSIZE(name), "%s fitted again keeps its layout", kCases[i].name);
            Check(&closed, NULL, name, same && before.childCount == after.childCount && EqualRect(&clientClosed, &clientAgain));
            CheckGeometry(closedDialog, &closed);
        }
        DestroyFixture(wholeDialog, &whole);
        DestroyFixture(closedDialog, &closed);
    }
}

/* The dialogs other than the manager window, in every language and scale. */
static void CheckDialogs(void)
{
    static const int kResources[] = { IDD_PROFILE, IDD_TITLE, IDD_MESSAGE, IDD_UNINSTALL };
    size_t resource, scale;
    int language;
    for (language = 0; language < Localize_LanguageCount(); language++)
        for (resource = 0; resource < ARRAYSIZE(kResources); resource++) for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
            LayoutFixture fixture;
            HWND dialog;
            if (!g_scaleFits[scale]) continue;
            ZeroMemory(&fixture, sizeof fixture);
            fixture.resource = kResources[resource];
            fixture.language = language;
            fixture.fontScale = kFontScales[scale];
            dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "resource fixture dialog created", dialog != NULL);
            if (dialog) CheckGeometry(dialog, &fixture);
            DestroyFixture(dialog, &fixture);
        }
}

/* A second fit of a fitted window changes nothing: its frame stays. */
static void CheckSecondFitKeepsFrame(HWND dialog, const LayoutFixture *fixture)
{
    RECT frame, after;
    GetWindowRect(dialog, &frame);
    Theme_FitDialog(dialog);
    GetWindowRect(dialog, &after);
    Check(fixture, NULL, "a second fit keeps the native frame", EqualRect(&frame, &after));
}

typedef struct MessageCheck { LayoutFixture fixture; BOOL single; HWND seen; int checked; } MessageCheck;

/* The message box's owner: each box, once it waits for the user, is checked
 * and closed. */
static LRESULT CALLBACK CheckWaitingMessage(HWND owner, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    MessageCheck *check = (MessageCheck *)reference;
    (void)id;
    if (message == WM_ENTERIDLE && wp == MSGF_DIALOGBOX && (HWND)lp != check->seen) {
        HWND box = (HWND)lp, ok = GetDlgItem(box, IDOK), cancel = GetDlgItem(box, IDCANCEL);
        RECT okPlace = RelativeRect(box, ok), cancelPlace = RelativeRect(box, cancel);
        check->seen = box;
        check->checked++;
        CheckGeometry(box, &check->fixture);
        if (check->single)
            Check(&check->fixture, ok, "a one-button message box shows its button alone, in the second one's place",
                  IsWindowVisible(ok) && !IsWindowVisible(cancel) && okPlace.right == cancelPlace.right && okPlace.top == cancelPlace.top);
        else Check(&check->fixture, cancel, "a two-button message box shows both buttons", IsWindowVisible(ok) && IsWindowVisible(cancel));
        Check(&check->fixture, GetDlgItem(box, IDC_M_ICON), "a message box shows its icon",
              SendDlgItemMessageW(box, IDC_M_ICON, STM_GETICON, 0, 0) != 0 && IsWindowVisible(GetDlgItem(box, IDC_M_ICON)));
        PostMessageW(box, WM_COMMAND, IDOK, 0);
    }
    return DefSubclassProc(owner, message, wp, lp);
}

/* The themed message box as the program shows it (Ui_Message, at the
 * monitor's scale), with one button and with two, in every language. */
static void CheckMessageBoxes(void)
{
    HWND owner;
    int language, variant;
    if (!NativeScaleFits("the message boxes")) return;
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, WC_STATICW, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_hInst, NULL);
    if (!owner) {
        printf("  FAIL  message box owner created\n");
        g_checks++;
        g_failures++;
        return;
    }
    for (language = 0; language < Localize_LanguageCount(); language++) for (variant = 0; variant < 2; variant++) {
        MessageCheck check;
        WCHAR text[1024];
        ZeroMemory(&check, sizeof check);
        check.fixture.resource = IDD_MESSAGE;
        check.fixture.language = language;
        check.fixture.fontScale = kFontScales[0];
        check.single = variant == 0;
        Localize_SetLanguage(language, FALSE);
        StringCchPrintfW(text, ARRAYSIZE(text),
            TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\n%s gets its own copy of the conversation and of its working folder, "
               L"under No folder in \x201C%s\x201D. The copy then goes on separately. %s, and its title there is set when %s closes."),
            L"A private conversation", L"Personal", L"Personal", L"Personal", TR(L"Continue?"), L"Personal");
        SetWindowSubclass(owner, CheckWaitingMessage, MESSAGE_OWNER_SUBCLASS, (DWORD_PTR)&check);
        Ui_Message(owner, check.single ? MB_OK | MB_ICONINFORMATION : MB_YESNO | MB_ICONQUESTION, L"%s", text);
        RemoveWindowSubclass(owner, CheckWaitingMessage, MESSAGE_OWNER_SUBCLASS);
        Check(&check.fixture, NULL, "the message box waited for the user once, and was checked", check.checked == 1);
    }
    DestroyWindow(owner);
}

/* Captions too wide for one row take two, each row filling the group. */
static void CheckShortcutFallback(void)
{
    static const int kShortcuts[] = { IDC_SC_DESKTOP, IDC_SC_SAVEAS, IDC_SC_PIN, IDC_SC_START };
    LayoutFixture fixture = MainFixture(Language(L"en"), 96, FALSE);
    HWND dialog;
    HFONT larger = NULL;
    LOGFONTW font;
    size_t i;
    if (!NativeScaleFits("the shortcuts' two-row fallback")) return;
    dialog = CreateFixture(&fixture);
    Check(&fixture, NULL, "shortcut fallback private fixture created", dialog != NULL);
    if (dialog && GetObjectW((HFONT)SendDlgItemMessageW(dialog, IDC_SC_SAVEAS, WM_GETFONT, 0, 0), sizeof font, &font)) {
        font.lfHeight = MulDiv(font.lfHeight, 3, 2);
        larger = CreateFontIndirectW(&font);
    }
    if (larger) {
        for (i = 0; i < ARRAYSIZE(kShortcuts); i++) SendDlgItemMessageW(dialog, kShortcuts[i], WM_SETFONT, (WPARAM)larger, FALSE);
        Theme_FitDialog(dialog);
        Check(&fixture, NULL, "larger shortcut captions take the two-row fallback",
              RelativeRect(dialog, GetDlgItem(dialog, IDC_SC_DESKTOP)).top != RelativeRect(dialog, GetDlgItem(dialog, IDC_SC_START)).top);
        CheckShortcutRows(dialog, &fixture);
        for (i = 0; i < ARRAYSIZE(kShortcuts); i++) CheckText(&fixture, GetDlgItem(dialog, kShortcuts[i]));
    } else Check(&fixture, NULL, "larger shortcut caption font created", FALSE);
    DestroyFixture(dialog, &fixture);
    if (larger) DeleteObject(larger);
}

/* The manager window in every language, view and scale, through every link
 * status and footer: one client size, every control in place. Its table,
 * note and details are checked once per window. */
static void CheckMainStates(void)
{
    size_t scale;
    int language, view, links, footer;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
        RECT common = { 0, 0, 0, 0 };
        int commonRole = -1;
        if (!g_scaleFits[scale]) continue;
        for (language = 0; language < Localize_LanguageCount(); language++) for (view = 0; view < 2; view++) {
            LayoutFixture fixture = MainFixture(language, kFontScales[scale], view != 0);
            HWND dialog;
            RECT client;
            fixture.links = LINKS_NOT_SET_UP;
            dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "private main fixture created", dialog != NULL);
            if (!dialog) {
                DestroyFixture(dialog, &fixture);
                continue;
            }
            GetClientRect(dialog, &client);
            if (IsRectEmpty(&common)) common = client;
            Check(&fixture, NULL, "main size is identical across all languages and both views", EqualRect(&common, &client));
            if (!fixture.sessions) {
                if (commonRole < 0) commonRole = ListView_GetColumnWidth(fixture.table, 1);
                Check(&fixture, fixture.table, "default Role column has one width for every language",
                      ListView_GetColumnWidth(fixture.table, 1) == commonRole);
                CheckProfileTable(dialog, &fixture);
                CheckNoteGeometry(dialog, &fixture);
                CheckNativeNote(dialog, &fixture);
                CheckSecondFitKeepsFrame(dialog, &fixture);
            } else {
                CheckSessionActionWidths(dialog, &fixture);
                CheckHiddenProfileNote(dialog, &fixture);
            }
            for (links = 0; links < LINK_STATUSES; links++) for (footer = 0; footer < MAIN_FOOTERS; footer++) {
                fixture.links = (LinkStatus)links;
                fixture.footer = (MainFooter)footer;
                MainCaptions(dialog, &fixture);
                Theme_LayoutMain(dialog);
                GetClientRect(dialog, &client);
                Check(&fixture, NULL, "status and footer changes keep the main client size", EqualRect(&common, &client));
                CheckGeometry(dialog, &fixture);
            }
            DestroyFixture(dialog, &fixture);
        }
    }
}

/* A note naming profiles short and long (spaced or not), in a fresh window
 * per language and then in one window through every language. */
static void CheckProfileNotes(void)
{
    WCHAR longName[LABEL_CCH], unbroken[LABEL_CCH];
    const WCHAR *names[] = { L"Personal", L"Work", longName, unbroken };
    size_t scale, name, character;
    int language, round;
    LongName(longName, ARRAYSIZE(longName));
    for (character = 0; character < MAX_LABEL; character++) unbroken[character] = L'W';
    unbroken[character] = 0;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) for (name = 0; name < ARRAYSIZE(names); name++) {
        LayoutFixture fixture;
        HWND dialog;
        if (!g_scaleFits[scale]) continue;
        for (language = 0; language < Localize_LanguageCount(); language++) {
            fixture = MainFixture(language, kFontScales[scale], FALSE);
            fixture.noteName = names[name];
            dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "fresh-language private profile note fixture created", dialog != NULL);
            if (dialog) {
                CheckNoteGeometry(dialog, &fixture);
                CheckNativeNote(dialog, &fixture);
            }
            DestroyFixture(dialog, &fixture);
        }
        fixture = MainFixture(Language(L"en"), kFontScales[scale], FALSE);
        fixture.noteName = names[name];
        dialog = CreateFixture(&fixture);
        Check(&fixture, NULL, "private profile note fixture created", dialog != NULL);
        if (dialog) for (round = 0; round < 2; round++) for (language = 0; language < Localize_LanguageCount(); language++) {
            TransitionLanguage(dialog, &fixture, language);
            CheckNoteGeometry(dialog, &fixture);
            CheckNativeNote(dialog, &fixture);
        }
        DestroyFixture(dialog, &fixture);
    }
}

/* A column sized as the user sizes it: its header divider dragged with the
 * native header's tracking (the left button held in this thread's keyboard
 * state only) and released at `width`. TRUE when the column has it. */
static BOOL DragDivider(HWND table, int column, int width)
{
    HWND header = ListView_GetHeader(table);
    RECT edge;
    BYTE saved[256], keys[256];
    int distance = width - ListView_GetColumnWidth(table, column), y;
    BOOL ok;
    if (!Header_GetItemRect(header, column, &edge) || !GetKeyboardState(saved)) return FALSE;
    CopyMemory(keys, saved, sizeof keys);
    keys[VK_LBUTTON] = 0x80;
    ok = SetKeyboardState(keys);
    y = (edge.top + edge.bottom) / 2;
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(edge.right - 1, y));
    SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(edge.right - 1 + distance, y));
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(edge.right - 1 + distance, y));
    if (GetCapture() == header) ReleaseCapture();
    ok = SetKeyboardState(saved) && ok;
    return ok && ListView_GetColumnWidth(table, column) == width;
}

/* Columns the user sized keep their widths through layouts and every
 * language; the data column follows the room left until it is sized too. */
static void CheckColumnInteractions(HWND dialog, LayoutFixture *fixture)
{
    HWND table = fixture->table;
    int profile = ListView_GetColumnWidth(table, 0), role = ListView_GetColumnWidth(table, 1);
    int data = ListView_GetColumnWidth(table, 2), language;
    WCHAR name[LABEL_CCH];
    LongName(name, ARRAYSIZE(name));
    ListView_SetItemText(table, 0, 0, name);
    ListView_SetItemState(table, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Gui_LayoutProfileColumns(table);
    Check(fixture, table, "name and selection changes retain the default fixed columns",
          ListView_GetColumnWidth(table, 0) == profile && ListView_GetColumnWidth(table, 1) == role && ListView_GetColumnWidth(table, 2) == data);
    Check(fixture, table, "the profile column's divider dragged by the user sizes it", DragDivider(table, 0, profile + 20));
    Check(fixture, table, "the role column's divider dragged by the user sizes it", DragDivider(table, 1, role + 10));
    Gui_LayoutProfileColumns(table);
    {
        int defaultProfile, defaultRole, dataMinimum;
        Theme_ProfileColumnWidths(table, &defaultProfile, &defaultRole, &dataMinimum);
        Check(fixture, table, "manual first-column widths survive layout and the data column follows the remainder, down to its minimum",
              ListView_GetColumnWidth(table, 0) == profile + 20 && ListView_GetColumnWidth(table, 1) == role + 10 &&
              ListView_GetColumnWidth(table, 2) == max(dataMinimum, data - 30));
    }
    for (language = 0; language < Localize_LanguageCount(); language++) {
        TransitionLanguage(dialog, fixture, language);
        Check(fixture, table, "manual fixed-column adjustments survive every language change",
              ListView_GetColumnWidth(table, 0) == profile + 20 && ListView_GetColumnWidth(table, 1) == role + 10);
    }
    Check(fixture, table, "the data column's divider dragged by the user sizes it", DragDivider(table, 2, data - 40));
    Gui_LayoutProfileColumns(table);
    Check(fixture, table, "an explicit data-column adjustment is retained", ListView_GetColumnWidth(table, 2) == data - 40);
}

/* The window frame of a `width` x `height` client, at the work area's corner. */
static void ResizeMainFixture(HWND dialog, LayoutFixture *fixture, int width, int height)
{
    RECT frame, actual;
    MONITORINFO monitor = { sizeof monitor };
    SetRect(&frame, 0, 0, width, height);
    Check(fixture, NULL, "main resize frame computed from native styles",
          AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dialog, GWL_STYLE), FALSE,
                                   (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog)));
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowPos(dialog, NULL, monitor.rcWork.left, monitor.rcWork.top, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    GetClientRect(dialog, &actual);
    Check(fixture, NULL, "native resize reaches the requested client size", actual.right == width && actual.bottom == height);
    if (actual.right != width || actual.bottom != height) printf("        requested=%dx%d actual=%ldx%ld\n", width, height, actual.right, actual.bottom);
}

/* A client size of `widthDips` x `heightDips`, at least the minimum and at
 * most what the work area holds. */
static SIZE ClientWithin(HWND dialog, SIZE minimum, int widthDips, int heightDips)
{
    RECT frame = { 0, 0, 0, 0 }, client;
    MONITORINFO monitor = { sizeof monitor };
    int dpi = (int)GetDpiForWindow(dialog);
    SIZE size;
    GetWindowRect(dialog, &frame);
    GetClientRect(dialog, &client);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    size.cx = min(max(minimum.cx, MulDiv(widthDips, dpi, 96)), monitor.rcWork.right - monitor.rcWork.left - (frame.right - frame.left - client.right));
    size.cy = min(max(minimum.cy, MulDiv(heightDips, dpi, 96)), monitor.rcWork.bottom - monitor.rcWork.top - (frame.bottom - frame.top - client.bottom));
    return size;
}

static void DescribeFrame(const char *what, HWND dialog, const RECT *expected)
{
    RECT actual;
    WINDOWPLACEMENT placement = { sizeof placement };
    MONITORINFO monitor = { sizeof monitor };
    GetWindowRect(dialog, &actual);
    GetWindowPlacement(dialog, &placement);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    printf("        %s: zoomed=%d hung=%d frame=%ld,%ld,%ld,%ld expected=%ld,%ld,%ld,%ld normal=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld\n", what,
           IsZoomed(dialog), IsHungAppWindow(dialog), actual.left, actual.top, actual.right, actual.bottom,
           expected->left, expected->top, expected->right, expected->bottom, placement.rcNormalPosition.left, placement.rcNormalPosition.top,
           placement.rcNormalPosition.right, placement.rcNormalPosition.bottom, monitor.rcWork.left, monitor.rcWork.top,
           monitor.rcWork.right, monitor.rcWork.bottom);
}

/* The content has a bounded reading width (THEME_MAIN_READING_WIDTH_DIPS, or
 * the minimum when wider): a client up to it is filled within the margins,
 * a wider one keeps that width, centered. Its widest row is the shortcuts'. */
static void CheckReadingWidth(HWND dialog, const LayoutFixture *fixture, SIZE minimum, const char *name)
{
    HWND row = GetDlgItem(dialog, IDC_SC_GROUP);
    RECT client, group = RelativeRect(dialog, row);
    int dpi = (int)GetDpiForWindow(dialog), margin = MulDiv(THEME_MAIN_MARGIN_DIPS, dpi, 96);
    int reading = max(minimum.cx, MulDiv(THEME_MAIN_READING_WIDTH_DIPS, dpi, 96)), left, right;
    BOOL bounded;
    GetClientRect(dialog, &client);
    left = group.left;
    right = client.right - group.right;
    if (client.right <= reading) bounded = left == margin && right == margin;
    else bounded = group.right - group.left == reading - 2 * margin && abs(left - right) <= 1;
    Check(fixture, row, name, bounded);
    if (!bounded) printf("        client %ld, content %ld..%ld, reading width %d, margin %d\n", client.right, group.left, group.right, reading, margin);
}

/* WM_DPICHANGED's suggested frames, then a native maximize, language changes
 * while maximized and a native restore, on a window of its own. The window
 * is on screen meanwhile: messages are handled between steps (PumpMessages). */
static void CheckMainFrameMessages(void)
{
    static const int kActions[] = { IDC_OPEN, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT };
    LayoutFixture fixture = MainFixture(Language(L"fr"), 96, FALSE);
    HWND dialog, child;
    RECT saved, requested, actual, client, actionRects[ARRAYSIZE(kActions)];
    SIZE minimum, size;
    MONITORINFO monitor = { sizeof monitor };
    HMENU system;
    int dpi, i;
    if (!NativeScaleFits("the manager window's frame messages")) return;
    dialog = CreateFixture(&fixture);
    Check(&fixture, NULL, "frame message private fixture created", dialog != NULL);
    if (!dialog || !Theme_MainMinimum(dialog, &minimum)) {
        DestroyFixture(dialog, &fixture);
        return;
    }
    size = ClientWithin(dialog, minimum, THEME_MAIN_READING_WIDTH_DIPS, 0);
    ResizeMainFixture(dialog, &fixture, size.cx, size.cy);
    dpi = (int)GetDpiForWindow(dialog);
    system = GetSystemMenu(dialog, FALSE);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    GetWindowRect(dialog, &saved);
    for (i = 0; i < (int)ARRAYSIZE(kActions); i++) actionRects[i] = RelativeRect(dialog, GetDlgItem(dialog, kActions[i]));
    Check(&fixture, NULL, "native system menu offers Size and Maximize",
          system && GetMenuState(system, SC_SIZE, MF_BYCOMMAND) != (UINT)-1 && GetMenuState(system, SC_MAXIMIZE, MF_BYCOMMAND) != (UINT)-1);
    requested = saved;
    Check(&fixture, NULL, "current-DPI suggested frame message is handled by production geometry",
          Gui_MainWindowGeometry(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&requested));
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "a valid suggested frame preserves its size and position", EqualRect(&requested, &actual));
    SetRect(&requested, monitor.rcWork.right - 40, monitor.rcWork.bottom - 40, monitor.rcWork.right - 38, monitor.rcWork.bottom - 38);
    Check(&fixture, NULL, "undersized suggested frame is handled by production geometry",
          Gui_MainWindowGeometry(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&requested));
    GetClientRect(dialog, &client);
    Check(&fixture, NULL, "suggested frame clamps to the common client minimum and current work area",
          client.right == minimum.cx && client.bottom == minimum.cy && InsideWorkArea(dialog));
    requested = saved;
    Gui_MainWindowGeometry(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&requested);
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "the chosen normal frame is back before maximizing", EqualRect(&saved, &actual));
    ShowWindow(dialog, SW_MAXIMIZE);
    PumpMessages();
    Check(&fixture, NULL, "native maximize changes the main window state", IsZoomed(dialog));
    GetWindowRect(dialog, &requested);
    GetClientRect(dialog, &client);
    Theme_FitDialog(dialog);
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "fitting preserves the maximized native frame", IsZoomed(dialog) && EqualRect(&actual, &requested));
    CheckReadingWidth(dialog, &fixture, minimum, "maximized content keeps its reading width, centered");
    for (i = 0; i < (int)ARRAYSIZE(kActions); i++) {
        RECT action = RelativeRect(dialog, GetDlgItem(dialog, kActions[i]));
        Check(&fixture, GetDlgItem(dialog, kActions[i]), "maximized actions keep their height and vertical grouping",
              action.top == actionRects[i].top && action.bottom == actionRects[i].bottom);
    }
    for (child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT placed;
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        placed = RelativeRect(dialog, child);
        Check(&fixture, child, "maximized visible control stays inside the client",
              placed.left >= 0 && placed.top >= 0 && placed.right <= client.right && placed.bottom <= client.bottom);
    }
    CheckNativeNote(dialog, &fixture);
    PumpMessages();
    TransitionLanguage(dialog, &fixture, Language(L"zh-CN"));
    PumpMessages();
    TransitionLanguage(dialog, &fixture, Language(L"fr"));
    PumpMessages();
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "language round trip preserves maximized state and frame", IsZoomed(dialog) && EqualRect(&actual, &requested));
    ShowWindow(dialog, SW_RESTORE);
    PumpMessages();
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "native restore returns to the chosen normal frame", !IsZoomed(dialog) && EqualRect(&saved, &actual));
    if (IsZoomed(dialog) || !EqualRect(&saved, &actual)) DescribeFrame("after restore", dialog, &saved);
    ShowWindow(dialog, SW_HIDE);
    PumpMessages();
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "hiding keeps the restored frame", EqualRect(&saved, &actual));
    DestroyFixture(dialog, &fixture);
}

static BOOL SaveFixtureFile(const WCHAR *root, const WCHAR *path, const char *text, BOOL replace)
{
    HANDLE file;
    DWORD length = (DWORD)strlen(text), written = 0;
    BOOL ok;
    if (!Core_PathUnder(path, root)) return FALSE;
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, replace ? CREATE_ALWAYS : CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, text, length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

static BOOL RemoveFixtureTree(const WCHAR *root, const WCHAR *path)
{
    WCHAR pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE files;
    BOOL ok = TRUE;
    if ((!Core_PathEquals(root, path) && !Core_PathUnder(path, root)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", path))) return FALSE;
    files = FindFirstFileW(pattern, &found);
    if (files != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                FAILED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", path, found.cFileName))) { ok = FALSE; break; }
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = RemoveFixtureTree(root, child) && ok;
            else ok = DeleteFileW(child) && ok;
        } while (FindNextFileW(files, &found));
        FindClose(files);
    }
    return ok && RemoveDirectoryW(path);
}

/* A private root folder; CLAUDE_CONFIG_DIR (Claude Code's transcripts)
 * points into it until RestoreConfigDir. */
typedef struct PrivateRoot {
    WCHAR path[MAX_PATH], savedConfigDir[MAX_PATH];
    BOOL hadConfigDir;
} PrivateRoot;

static BOOL OpenPrivateRoot(PrivateRoot *root)
{
    WCHAR temp[MAX_PATH];
    DWORD saved;
    ZeroMemory(root, sizeof *root);
    saved = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root->savedConfigDir, ARRAYSIZE(root->savedConfigDir));
    root->hadConfigDir = saved > 0;
    return saved < ARRAYSIZE(root->savedConfigDir) && GetTempPathW(ARRAYSIZE(temp), temp) &&
           GetTempFileNameW(temp, L"cdm", 0, root->path) && DeleteFileW(root->path) && CreateDirectoryW(root->path, NULL);
}

static void ClosePrivateRoot(PrivateRoot *root, const LayoutFixture *fixture)
{
    Check(fixture, NULL, "private transcript environment restored",
          SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root->hadConfigDir ? root->savedConfigDir : NULL));
    Check(fixture, NULL, "private session folders removed without the Recycle Bin", RemoveFixtureTree(root->path, root->path));
}

/* The real sessions view (sessions.c) with no profile, shown and left in a
 * window of its own: the shared client size and every control in place. */
static void CheckRealSessionsTransition(int language)
{
    LayoutFixture fixture = MainFixture(language, 96, FALSE);
    PrivateRoot root;
    WCHAR projects[MAX_PATH];
    ProfileList profiles;
    ClaudePackage package;
    RECT initial, returned;
    HWND dialog = NULL;
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    ZeroMemory(&package, sizeof package);
    fixture.realSessions = TRUE;
    ready = OpenPrivateRoot(&root);
    ready = ready && SUCCEEDED(StringCchPrintfW(projects, ARRAYSIZE(projects), L"%s\\projects", root.path)) && CreateDirectoryW(projects, NULL);
    Check(&fixture, NULL, "real sessions transition private transcript root prepared", ready);
    if (ready) {
        SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root.path);
        dialog = CreateFixture(&fixture);
        Check(&fixture, NULL, "real sessions transition fixture created", dialog != NULL);
    }
    if (dialog) {
        GetClientRect(dialog, &initial);
        SessionsView_SetProfiles(&profiles);
        Gui_ShowSessions(dialog, &package, TRUE, NULL);
        fixture.sessions = TRUE;
        Check(&fixture, fixture.tableViewport, "real Sessions transition hides the profile table's viewport",
              !(GetWindowLongW(fixture.tableViewport, GWL_STYLE) & WS_VISIBLE));
        CheckGeometry(dialog, &fixture);
        Theme_FitDialog(dialog);
        SessionsView_Resize();
        SessionsView_Relayout();
        CheckGeometry(dialog, &fixture);
        GetClientRect(dialog, &returned);
        Check(&fixture, NULL, "real Sessions rendering and fitting retain the shared client size", EqualRect(&initial, &returned));
        Gui_ShowSessions(dialog, &package, FALSE, NULL);
        fixture.sessions = FALSE;
        Theme_FitDialog(dialog);
        LayoutMockNote(dialog, &fixture);
        CheckGeometry(dialog, &fixture);
        GetClientRect(dialog, &returned);
        Check(&fixture, NULL, "real Profiles return preserves the same client size", EqualRect(&initial, &returned));
        Check(&fixture, fixture.tableViewport, "real Profiles return shows the same original viewport",
              (GetWindowLongW(fixture.tableViewport, GWL_STYLE) & WS_VISIBLE) != 0);
        SessionsView_Destroy();
    }
    DestroyFixture(dialog, &fixture);
    if (root.path[0]) ClosePrivateRoot(&root, &fixture);
}

/* The manager window resized natively from its minimum to medium, wide,
 * ultrawide, tall and short-wide clients, in every language and view. */
static void CheckResponsiveMain(void)
{
    static const int kSizes[][2] = { { 0, 0 }, { 1000, 650 }, { 1400, 700 }, { 1900, 850 }, { 1000, 1100 }, { 1200, 0 } };
    int language, view, size;
    SIZE commonMinimum = { 0, 0 };
    if (!NativeScaleFits("the manager window's responsive layout")) return;
    for (language = 0; language < Localize_LanguageCount(); language++) for (view = 0; view < 2; view++) {
        LayoutFixture fixture = MainFixture(language, 96, view != 0);
        HWND dialog = CreateFixture(&fixture);
        SIZE minimum = { 0, 0 };
        RECT initialNote, initialDefault;
        int dpi, role, footer;
        Check(&fixture, NULL, "responsive private fixture created", dialog != NULL);
        if (!dialog) {
            DestroyFixture(dialog, &fixture);
            continue;
        }
        Check(&fixture, NULL, "main exposes its intrinsic minimum", Theme_MainMinimum(dialog, &minimum));
        if (!language && !view) commonMinimum = minimum;
        Check(&fixture, NULL, "minimum is independent of language and view", minimum.cx == commonMinimum.cx && minimum.cy == commonMinimum.cy);
        Check(&fixture, NULL, "main native style supports resizing and maximizing",
              (GetWindowLongW(dialog, GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == (WS_THICKFRAME | WS_MAXIMIZEBOX));
        Check(&fixture, NULL, "the central main layout owns DPI geometry", (GetDialogDpiChangeBehavior(dialog) & DDC_DISABLE_ALL) != 0);
        {
            MINMAXINFO tracking = { 0 };
            RECT native = { 0, 0, 0, 0 };
            native.right = minimum.cx;
            native.bottom = minimum.cy;
            AdjustWindowRectExForDpi(&native, (DWORD)GetWindowLongW(dialog, GWL_STYLE), FALSE,
                                     (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog));
            SendMessageW(dialog, WM_GETMINMAXINFO, 0, (LPARAM)&tracking);
            Check(&fixture, NULL, "native tracking enforces the shared client minimum including its frame",
                  tracking.ptMinTrackSize.x == native.right - native.left && tracking.ptMinTrackSize.y == native.bottom - native.top);
        }
        initialNote = RelativeRect(dialog, GetDlgItem(dialog, IDC_NOTE));
        initialDefault = RelativeRect(dialog, GetDlgItem(dialog, IDC_DEFAULT));
        role = ListView_GetColumnWidth(fixture.table, 1);
        dpi = (int)GetDpiForWindow(dialog);
        for (size = 0; size < (int)ARRAYSIZE(kSizes); size++) {
            RECT before, after, table, group, note, setDefault, sessionProfiles, details, tree, tableClient;
            SIZE client = ClientWithin(dialog, minimum, kSizes[size][0], kSizes[size][1]);

            ResizeMainFixture(dialog, &fixture, client.cx, client.cy);
            GetWindowRect(dialog, &before);
            Theme_FitDialog(dialog);
            LayoutMockNote(dialog, &fixture);
            GetWindowRect(dialog, &after);
            Check(&fixture, NULL, "fitting and note updates retain the user's frame and position", EqualRect(&before, &after));

            table = RelativeRect(dialog, fixture.tableViewport);
            group = RelativeRect(dialog, GetDlgItem(dialog, IDC_SC_GROUP));
            note = RelativeRect(dialog, GetDlgItem(dialog, IDC_NOTE));
            setDefault = RelativeRect(dialog, GetDlgItem(dialog, IDC_DEFAULT));
            sessionProfiles = RelativeRect(dialog, fixture.profilesViewport);
            details = RelativeRect(dialog, GetDlgItem(dialog, IDC_S_DETAILS));
            tree = RelativeRect(dialog, fixture.treeViewport);
            Check(&fixture, fixture.tableViewport, "table starts where the content does", table.left == group.left);
            CheckReadingWidth(dialog, &fixture, minimum, "content fills the client within its margins up to its reading width, then stays centered");
            Check(&fixture, GetDlgItem(dialog, IDC_NOTE), "sidebar prose stays grouped with its actions on tall windows",
                  note.top == initialNote.top && note.bottom - note.top == initialNote.bottom - initialNote.top &&
                  setDefault.top == initialDefault.top && setDefault.bottom - setDefault.top == initialDefault.bottom - initialDefault.top);
            Check(&fixture, fixture.profilesViewport, "session sidebar keeps its intrinsic width",
                  sessionProfiles.right - sessionProfiles.left <= MulDiv(SESSION_PROFILES_MAX_DIPS, dpi, 96));
            Check(&fixture, GetDlgItem(dialog, IDC_S_DETAILS), "session details keep a bounded width",
                  details.right - details.left <= MulDiv(SESSION_DETAILS_MAX_DIPS, dpi, 96));
            Check(&fixture, fixture.treeViewport, "session tree takes the space between bounded side panes",
                  tree.left > sessionProfiles.right && tree.right < details.left);
            Check(&fixture, fixture.table, "fixed table columns keep their widths at every window size",
                  ListView_GetColumnWidth(fixture.table, 0) == MulDiv(THEME_PROFILE_COLUMN_DIPS, dpi, 96) && ListView_GetColumnWidth(fixture.table, 1) == role);
            GetClientRect(fixture.table, &tableClient);
            Check(&fixture, fixture.table, "data column is the exact table remainder",
                  ListView_GetColumnWidth(fixture.table, 0) + ListView_GetColumnWidth(fixture.table, 1) +
                  ListView_GetColumnWidth(fixture.table, 2) == tableClient.right);
            CheckGeometry(dialog, &fixture);
            if (!fixture.sessions) {
                CheckNoteGeometry(dialog, &fixture);
                CheckNativeNote(dialog, &fixture);
            } else CheckSessionActionWidths(dialog, &fixture);
            for (footer = 0; footer < MAIN_FOOTERS; footer++) {
                fixture.footer = (MainFooter)footer;
                MainCaptions(dialog, &fixture);
                Theme_FitDialog(dialog);
                CheckText(&fixture, GetDlgItem(dialog, IDC_ABOUT));
                GetWindowRect(dialog, &after);
                Check(&fixture, NULL, "footer variants retain the resized frame", EqualRect(&before, &after));
            }
            fixture.footer = MAIN_FOOTER_CREDITS;
            MainCaptions(dialog, &fixture);
            Theme_LayoutMain(dialog);
        }
        {
            RECT resized, after;
            int original = fixture.language, other = original == Language(L"zh-CN") ? Language(L"fr") : Language(L"zh-CN");
            GetWindowRect(dialog, &resized);
            TransitionLanguage(dialog, &fixture, other);
            TransitionLanguage(dialog, &fixture, original);
            GetWindowRect(dialog, &after);
            Check(&fixture, NULL, "language round trip retains user position and size", EqualRect(&resized, &after));
        }
        if (!language && !view) {
            HWND header = ListView_GetHeader(fixture.table);
            HFONT original = (HFONT)SendMessageW(header, WM_GETFONT, 0, 0), larger;
            LOGFONTW font;
            SIZE altered, returned;
            DWORD gdiBefore, gdiAfter;
            int repeat;
            GetObjectW(original, sizeof font, &font);
            font.lfHeight *= 2;
            larger = CreateFontIndirectW(&font);
            Check(&fixture, header, "header-only larger test font created", larger != NULL);
            if (larger) {
                SendMessageW(header, WM_SETFONT, (WPARAM)larger, FALSE);
                Check(&fixture, header, "header-only font change invalidates the intrinsic budget",
                      Theme_MainMinimum(dialog, &altered) && altered.cy > minimum.cy);
                SendMessageW(header, WM_SETFONT, (WPARAM)original, FALSE);
                Check(&fixture, header, "header font round trip restores the common minimum",
                      Theme_MainMinimum(dialog, &returned) && returned.cx == minimum.cx && returned.cy == minimum.cy);
                DeleteObject(larger);
            }
            gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (repeat = 0; repeat < 32; repeat++) Theme_MainMinimum(dialog, &returned);
            gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            Check(&fixture, NULL, "repeated minimum queries retain no new GDI resources", gdiBefore == gdiAfter);
        }
        if (!view && language == Language(L"fr")) CheckColumnInteractions(dialog, &fixture);
        DestroyFixture(dialog, &fixture);
    }
}

/* ------------------------------------------------- the real sessions view */

#define SESSION_ROWS     80    /* half in each of two folders */
#define STARRED_ROW      7
#define UNKNOWN_ROW      SESSION_ROWS   /* one more session, with no folder recorded */
#define FIXTURE_TIME     1700000000000ULL

typedef struct SessionsFixture {
    LayoutFixture layout;
    PrivateRoot root;
    HWND dialog, tree;
    ProfileList profiles;
    ClaudePackage package;             /* not found: opening a session asks for Claude, never starts it */
    WCHAR entries[MAX_PATH], transcripts[MAX_PATH], folders[3][MAX_PATH];
    int streamWrites;
} SessionsFixture;

static void SessionId(int index, WCHAR *id, size_t cch)
{
    StringCchPrintfW(id, cch, L"00000001-0000-4000-8000-%012d", index);
}

/* An entry of the first profile and its transcript; `folder` NULL: none recorded. */
static BOOL WriteSession(SessionsFixture *sessions, int index, const WCHAR *folder, const WCHAR *title, ULONGLONG lastActivity, BOOL starred)
{
    WCHAR id[SESSION_ID_CCH], path[MAX_PATH];
    char json[4096], quoted[MAX_PATH * 6 + 4], cwd[MAX_PATH * 6 + 16] = "";
    SessionId(index, id, ARRAYSIZE(id));
    if (folder) {
        if (!Core_JsonQuote(folder, quoted, sizeof quoted)) return FALSE;
        StringCchPrintfA(cwd, sizeof cwd, ",\"cwd\":%s", quoted);
    }
    StringCchPrintfA(json, sizeof json,
        "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\"%s,\"title\":\"%ls\",\"lastActivityAt\":%llu%s}",
        id, id, cwd, title, lastActivity, starred ? ",\"isStarred\":true" : "");
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\local_%s.json", sessions->entries, id);
    if (!SaveFixtureFile(sessions->root.path, path, json, FALSE)) return FALSE;
    StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"%ls\"%s,\"type\":\"user\"}\n", id, cwd);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s.jsonl", sessions->transcripts, id);
    return SaveFixtureFile(sessions->root.path, path, json, FALSE);
}

static void RowTitle(int index, WCHAR *title, size_t cch)
{
    StringCchPrintfW(title, cch, L"Private conversation %02d", index);
}

/* Two private profiles: the first lists SESSION_ROWS sessions in two
 * folders, one of them starred, and one with no folder; the second has a
 * config.json but no entries folder yet. Shown in a real manager layout. */
static BOOL OpenSessionsFixture(SessionsFixture *sessions, int language)
{
    WCHAR title[64], path[MAX_PATH], code[MAX_PATH], projects[MAX_PATH];
    LayoutFixture *fixture = &sessions->layout;
    BOOL ready;
    int i, p;
    ZeroMemory(sessions, sizeof *sessions);
    *fixture = MainFixture(language, 96, FALSE);
    fixture->realSessions = TRUE;
    ready = OpenPrivateRoot(&sessions->root);
    Check(fixture, NULL, "populated Sessions private root created", ready);
    if (!ready) return FALSE;
    sessions->profiles.count = 2;
    for (p = 0; p < sessions->profiles.count; p++) {
        Profile *profile = &sessions->profiles.items[p];
        StringCchPrintfW(profile->folder, ARRAYSIZE(profile->folder), L"Fixture-layout-%08lx-%d", GetCurrentProcessId(), p);
        StringCchCopyW(profile->name, ARRAYSIZE(profile->name), p ? L"Second profile" : L"Private profile");
        StringCchPrintfW(profile->dataDir, ARRAYSIZE(profile->dataDir), L"%s\\profile%d", sessions->root.path, p);
        StringCchCopyW(profile->storageDir, ARRAYSIZE(profile->storageDir), profile->dataDir);
        profile->color = p;
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\config.json", profile->storageDir);
        ready = ready && Util_EnsureDir(profile->storageDir) &&
                SaveFixtureFile(sessions->root.path, path, "{\"lastKnownAccountUuid\":\"account-ui\"}", FALSE);
    }
    StringCchCopyW(sessions->profiles.defaultFolder, ARRAYSIZE(sessions->profiles.defaultFolder), sessions->profiles.items[0].folder);
    StringCchPrintfW(sessions->entries, ARRAYSIZE(sessions->entries), L"%s\\claude-code-sessions\\account-ui\\organization-ui",
                     sessions->profiles.items[0].storageDir);
    StringCchPrintfW(code, ARRAYSIZE(code), L"%s\\code", sessions->root.path);
    StringCchPrintfW(projects, ARRAYSIZE(projects), L"%s\\projects", code);
    StringCchPrintfW(sessions->transcripts, ARRAYSIZE(sessions->transcripts), L"%s\\fixture", projects);
    StringCchPrintfW(sessions->folders[0], ARRAYSIZE(sessions->folders[0]), L"%s\\Workspace Alpha", sessions->root.path);
    StringCchPrintfW(sessions->folders[1], ARRAYSIZE(sessions->folders[1]), L"%s\\Workspace Beta", sessions->root.path);
    StringCchPrintfW(sessions->folders[2], ARRAYSIZE(sessions->folders[2]), L"%s\\Workspace Gamma", sessions->root.path);
    ready = ready && Util_EnsureDir(sessions->entries) && Util_EnsureDir(sessions->transcripts);
    for (i = 0; i < 3; i++) ready = ready && Util_EnsureDir(sessions->folders[i]);
    /* Newer first: the first folder above the second, rows in index order. */
    for (i = 0; ready && i <= UNKNOWN_ROW; i++) {
        RowTitle(i, title, ARRAYSIZE(title));
        ready = WriteSession(sessions, i, i == UNKNOWN_ROW ? NULL : sessions->folders[i < SESSION_ROWS / 2 ? 0 : 1], title,
                             FIXTURE_TIME - (ULONGLONG)i * 1000, i == STARRED_ROW);
    }
    Check(fixture, NULL, "private profiles, entries and transcripts prepared", ready);
    if (!ready) return FALSE;
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", code);
    {
        SessionSet set = { 0 };
        Check(fixture, NULL, "private fixture reader resolves every row and folder",
              SessionStore_LoadProfiles(&set, &sessions->profiles) && set.rowCount == SESSION_ROWS + 1 && set.groupCount == 3);
        SessionStore_Free(&set);
    }
    sessions->dialog = CreateFixture(fixture);
    Check(fixture, NULL, "populated Sessions native dialog created", sessions->dialog != NULL);
    if (!sessions->dialog) return FALSE;
    sessions->tree = fixture->tree;
    SessionsView_SetProfiles(&sessions->profiles);
    Gui_ShowSessions(sessions->dialog, &sessions->package, TRUE, sessions->profiles.items[0].folder);
    fixture->sessions = TRUE;
    ShowWindow(sessions->dialog, SW_SHOWNOACTIVATE);
    return TRUE;
}

static void CloseSessionsFixture(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    if (sessions->dialog) {
        Gui_ShowSessions(sessions->dialog, &sessions->package, FALSE, NULL);
        fixture->sessions = FALSE;
        SessionsView_Destroy();
        fixture->realSessions = FALSE;
        ShowWindow(sessions->dialog, SW_HIDE);
    }
    DestroyFixture(sessions->dialog, fixture);
    sessions->dialog = NULL;
    if (sessions->root.path[0]) ClosePrivateRoot(&sessions->root, fixture);
}

typedef struct TreeWait { HWND tree; UINT count; const WCHAR *title; } TreeWait;

static HTREEITEM FindTitle(HWND tree, HTREEITEM item, const WCHAR *title)
{
    for (; item; item = TreeView_GetNextSibling(tree, item)) {
        WCHAR text[256];
        TVITEMW query = { 0 };
        HTREEITEM found;
        query.mask = TVIF_TEXT;
        query.hItem = item;
        query.pszText = text;
        query.cchTextMax = ARRAYSIZE(text);
        if (TreeView_GetItem(tree, &query) && wcscmp(text, title) == 0) return item;
        if ((found = FindTitle(tree, TreeView_GetChild(tree, item), title)) != NULL) return found;
    }
    return NULL;
}

static BOOL TreeShows(const void *context)
{
    const TreeWait *wait = (const TreeWait *)context;
    if (wait->title) return FindTitle(wait->tree, TreeView_GetRoot(wait->tree), wait->title) != NULL;
    return TreeView_GetCount(wait->tree) >= wait->count;
}

/* The tree once a snapshot showing `title` is in it. */
static BOOL AwaitTitle(SessionsFixture *sessions, const WCHAR *title)
{
    TreeWait wait;
    wait.tree = sessions->tree;
    wait.count = 0;
    wait.title = title;
    return PumpUntil(TreeShows, &wait, SNAPSHOT_TIMEOUT_MS);
}

/* Every snapshot the fixture's writes so far asked for has been read: a
 * session written now (`title`, the `marker`th, in the first folder) shows in
 * the tree, read after all of them. A read still to come reads the same
 * folders: the watcher has nothing new to watch, and asks for nothing. */
static BOOL AwaitWritesRead(SessionsFixture *sessions, int marker, const WCHAR *title)
{
    return WriteSession(sessions, UNKNOWN_ROW + 10 + marker, sessions->folders[0], title, FIXTURE_TIME - 1000000 - (ULONGLONG)marker * 1000, FALSE) &&
           AwaitTitle(sessions, title);
}

typedef struct CountWait { const int *counter; int above; } CountWait;

static BOOL CountAbove(const void *context)
{
    const CountWait *wait = (const CountWait *)context;
    return *wait->counter > wait->above;
}

static BOOL AwaitReloadRequest(const LayoutFixture *fixture, int seen, DWORD timeoutMs)
{
    CountWait wait;
    wait.counter = &fixture->reloadRequests;
    wait.above = seen;
    return PumpUntil(CountAbove, &wait, timeoutMs);
}

static HTREEITEM NthChild(HWND tree, HTREEITEM parent, int n)
{
    HTREEITEM item = parent ? TreeView_GetChild(tree, parent) : TreeView_GetRoot(tree);
    while (item && n-- > 0) item = TreeView_GetNextSibling(tree, item);
    return item;
}

static BOOL Folded(HWND tree, HTREEITEM folder)
{
    return folder && !(TreeView_GetItemState(tree, folder, TVIS_EXPANDED) & TVIS_EXPANDED);
}

/* Folded as the user folds it, with the keyboard: the tree notifies. */
static void FoldFolder(HWND tree, HTREEITEM folder)
{
    TreeView_SelectItem(tree, folder);
    SendMessageW(tree, WM_KEYDOWN, VK_LEFT, 0);
    SendMessageW(tree, WM_KEYUP, VK_LEFT, 0);
}

static BOOL ItemTitle(HWND tree, HTREEITEM item, WCHAR *text, int cch)
{
    TVITEMW query = { 0 };
    text[0] = 0;
    query.mask = TVIF_TEXT;
    query.hItem = item;
    query.pszText = text;
    query.cchTextMax = cch;
    return item && TreeView_GetItem(tree, &query);
}

/* The roots of the fixture's tree: Starred (its node value), the two
 * folders by name, and the sessions with no folder (named in the current
 * language). */
typedef struct FixtureRoots { HTREEITEM starred, alpha, beta, unknown; } FixtureRoots;

static FixtureRoots ReadRoots(HWND tree)
{
    FixtureRoots roots;
    HTREEITEM item;
    ZeroMemory(&roots, sizeof roots);
    for (item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item)) {
        WCHAR text[256];
        TVITEMW query = { 0 };
        query.mask = TVIF_TEXT | TVIF_PARAM;
        query.hItem = item;
        query.pszText = text;
        query.cchTextMax = ARRAYSIZE(text);
        if (!TreeView_GetItem(tree, &query)) continue;
        if (query.lParam == -1) roots.starred = item;
        else if (wcscmp(text, L"Workspace Alpha") == 0) roots.alpha = item;
        else if (wcscmp(text, L"Workspace Beta") == 0) roots.beta = item;
        else if (wcscmp(text, TR(L"Unknown folder")) == 0) roots.unknown = item;
    }
    return roots;
}

/* A posted mouse message sets GetMessagePos to its own coordinates: the
 * tree's double-click notification reads them as the screen point (sessions.c).
 * The double-click itself is sent, in client coordinates. FALSE when this
 * Windows does not report them so. */
static BOOL DoubleClickTree(HWND dialog, LayoutFixture *fixture, HWND tree, POINT point)
{
    POINT screen = point;
    MSG message;
    DWORD position;
    ClientToScreen(tree, &screen);
    PumpMessages();
    fixture->lastPrompt = NULL;   /* a new message box can reuse a closed one's handle */
    PostMessageW(dialog, WM_MOUSEMOVE, 0, MAKELPARAM(screen.x, screen.y));
    if (!PeekMessageW(&message, dialog, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) return FALSE;
    position = GetMessagePos();
    if ((short)LOWORD(position) != screen.x || (short)HIWORD(position) != screen.y) return FALSE;
    SendMessageW(tree, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(tree, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    PumpMessages();
    return TRUE;
}

typedef struct PrivateTreeItem {
    HTREEITEM item;
    LPARAM value;
    UINT expanded;
} PrivateTreeItem;

static void ReadPrivateTree(HWND tree, HTREEITEM item, PrivateTreeItem *items, int *count, int capacity)
{
    for (; item && *count < capacity; item = TreeView_GetNextSibling(tree, item)) {
        TVITEMW value = { 0 };
        value.hItem = item;
        value.mask = TVIF_PARAM | TVIF_STATE;
        value.stateMask = TVIS_EXPANDED;
        TreeView_GetItem(tree, &value);
        items[*count].item = item;
        items[*count].value = value.lParam;
        items[*count].expanded = value.state & TVIS_EXPANDED;
        (*count)++;
        ReadPrivateTree(tree, TreeView_GetChild(tree, item), items, count, capacity);
    }
}

static void CheckDetailHover(HWND control, const LayoutFixture *fixture, int x, int y, const char *name)
{
    HDC dc;
    COLORREF before, after;
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
    UpdateWindow(control);
    dc = GetDC(control);
    before = GetPixel(dc, x, y);
    ReleaseDC(control, dc);
    SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    UpdateWindow(control);
    dc = GetDC(control);
    after = GetPixel(dc, x, y);
    ReleaseDC(control, dc);
    Check(fixture, control, name, before != CLR_INVALID && after != CLR_INVALID && before != after);
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
    UpdateWindow(control);
}

/* Native resizes keep the populated tree: its items, expansion, selection
 * and scroll position; the details follow the new size. */
static void CheckSessionsResize(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree, details = GetDlgItem(dialog, IDC_S_DETAILS);
    PrivateTreeItem items[128];
    FixtureRoots roots = ReadRoots(tree);
    HTREEITEM selected;
    SCROLLINFO scroll = { sizeof scroll, SIF_ALL };
    SIZE minimum = { 0, 0 }, sizes[3];
    int count = 0, size, i;
    Theme_MainMinimum(dialog, &minimum);
    sizes[0] = ClientWithin(dialog, minimum, 1000, 650);
    sizes[1] = ClientWithin(dialog, minimum, 1900, 650);
    sizes[2] = minimum;
    FoldFolder(tree, roots.beta);
    selected = NthChild(tree, roots.alpha, 2);
    TreeView_SelectItem(tree, selected);
    PumpMessages();
    ReadPrivateTree(tree, TreeView_GetRoot(tree), items, &count, ARRAYSIZE(items));
    SendMessageW(fixture->treeViewport, WM_VSCROLL, MAKEWPARAM(SB_LINEDOWN, 0), 0);
    GetScrollInfo(fixture->treeViewport, SB_VERT, &scroll);
    Check(fixture, fixture->treeViewport, "private session tree has a genuine nonzero scroll position", scroll.nPos > 0);
    for (size = 0; size < (int)ARRAYSIZE(sizes); size++) {
        SCROLLINFO after = { sizeof after, SIF_ALL };
        HWND parts = GetDlgItem(details, IDC_S_PARTS);
        RECT part, detail, inside;
        ResizeMainFixture(dialog, fixture, sizes[size].cx, sizes[size].cy);
        for (i = 0; i < count; i++) {
            TVITEMW item = { 0 };
            item.hItem = items[i].item;
            item.mask = TVIF_PARAM | TVIF_STATE;
            item.stateMask = TVIS_EXPANDED;
            Check(fixture, tree, "resize retains each native tree item and its expansion",
                  TreeView_GetItem(tree, &item) && item.lParam == items[i].value && (item.state & TVIS_EXPANDED) == items[i].expanded);
        }
        Check(fixture, tree, "resize retains the selected native item", TreeView_GetSelection(tree) == selected);
        GetScrollInfo(fixture->treeViewport, SB_VERT, &after);
        Check(fixture, fixture->treeViewport, "resize retains the native scroll position while content still exceeds the page",
              after.nPos == scroll.nPos);
        CheckGeometry(dialog, fixture);
        part = RelativeRect(details, parts);
        GetClientRect(details, &detail);
        Check(fixture, details, "selected session details render their bounded scrolling part",
              parts && (GetWindowLongW(parts, GWL_STYLE) & WS_VISIBLE) && part.left >= 0 && part.top > 0 &&
              part.right <= detail.right && part.bottom < detail.bottom);
        CheckDetailHover(details, fixture, 10, detail.bottom - 5, "resized bottom action hit region is current before its hover paint");
        GetClientRect(parts, &inside);
        CheckDetailHover(parts, fixture, inside.right - 8, 15, "resized Actions menu hit region matches its native drawing");
        CheckSessionActionWidths(dialog, fixture);
    }
}

/* A snapshot that changes the tree refills it: folded folders stay folded
 * and the selected row stays selected, a starred row under Starred
 * included; a language change too (folders are remembered by path, not by
 * their translated names). */
static void CheckSessionsRefillKeepsState(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree;
    WCHAR title[64], starred[64], shown[256];
    FixtureRoots roots = ReadRoots(tree);
    HTREEITEM starredItem;
    int original = fixture->language, other = original == Language(L"zh-CN") ? Language(L"fr") : Language(L"zh-CN"), pass;
    RowTitle(STARRED_ROW, starred, ARRAYSIZE(starred));
    if (!Folded(tree, roots.beta)) FoldFolder(tree, roots.beta);
    FoldFolder(tree, roots.unknown);
    starredItem = NthChild(tree, roots.starred, 0);
    TreeView_SelectItem(tree, starredItem);
    PumpMessages();
    Check(fixture, tree, "the starred row under Starred is selected", ItemTitle(tree, TreeView_GetSelection(tree), shown, ARRAYSIZE(shown)) &&
          wcscmp(shown, starred) == 0 && TreeView_GetParent(tree, TreeView_GetSelection(tree)) == roots.starred);
    StringCchCopyW(title, ARRAYSIZE(title), L"Refill conversation");
    Check(fixture, NULL, "a newer session written", WriteSession(sessions, UNKNOWN_ROW + 1, sessions->folders[0], title, FIXTURE_TIME + 1000, FALSE));
    Check(fixture, tree, "a snapshot with a new session refills the tree", AwaitTitle(sessions, title));
    for (pass = 0; pass < 3; pass++) {
        const char *after = pass == 0 ? "a refill" : pass == 1 ? "a language change" : "the language's return";
        char name[160];
        if (pass) {
            int language = pass == 1 ? other : original;
            fixture->language = language;
            Localize_SetLanguage(language, FALSE);
            Localize_Window(dialog);
            Theme_Apply(dialog);
            Theme_FitDialog(dialog);
            SessionsView_Resize();
            SessionsView_Relayout();
            PumpMessages();
        }
        roots = ReadRoots(tree);
        StringCchPrintfA(name, ARRAYSIZE(name), "a folded folder stays folded after %s", after);
        Check(fixture, tree, name, Folded(tree, roots.beta));
        StringCchPrintfA(name, ARRAYSIZE(name), "the folder of sessions with no folder stays folded after %s", after);
        Check(fixture, tree, name, Folded(tree, roots.unknown) && TreeView_GetChild(tree, roots.unknown) != NULL);
        StringCchPrintfA(name, ARRAYSIZE(name), "the starred row under Starred stays selected after %s", after);
        Check(fixture, tree, name, ItemTitle(tree, TreeView_GetSelection(tree), shown, ARRAYSIZE(shown)) && wcscmp(shown, starred) == 0 &&
              TreeView_GetParent(tree, TreeView_GetSelection(tree)) == roots.starred);
    }
}

/* The row on top of the view stays there, as far above the view's top,
 * when a snapshot adds rows above it (a newer folder, a newer session). */
static void CheckSessionsRefillKeepsTopRow(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND tree = sessions->tree, viewport = fixture->treeViewport;
    WCHAR topTitle[64], title[64];
    FixtureRoots roots = ReadRoots(tree);
    HTREEITEM top = NthChild(tree, roots.alpha, 20), below;
    RECT row;
    SCROLLINFO scroll = { sizeof scroll, SIF_POS };
    int offset = 3, before;
    if (!ItemTitle(tree, top, topTitle, ARRAYSIZE(topTitle)) || !TreeView_GetItemRect(tree, top, &row, FALSE)) {
        Check(fixture, tree, "a row to keep on top found", FALSE);
        return;
    }
    SendMessageW(viewport, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, row.top + offset), 0);
    below = TreeView_GetNextSibling(tree, TreeView_GetNextSibling(tree, top));
    TreeView_SelectItem(tree, below);   /* in sight: the view has no reason to move */
    PumpMessages();
    GetScrollInfo(viewport, SB_VERT, &scroll);
    before = scroll.nPos;
    Check(fixture, viewport, "the view starts scrolled to the chosen row", before == row.top + offset);
    StringCchCopyW(title, ARRAYSIZE(title), L"Newer folder conversation");
    Check(fixture, NULL, "sessions written above the row on top",
          WriteSession(sessions, UNKNOWN_ROW + 2, sessions->folders[2], title, FIXTURE_TIME + 3000, FALSE) &&
          WriteSession(sessions, UNKNOWN_ROW + 3, sessions->folders[0], L"Newer conversation", FIXTURE_TIME + 2000, FALSE));
    Check(fixture, tree, "a snapshot adds rows above the row on top", AwaitTitle(sessions, title) && AwaitTitle(sessions, L"Newer conversation"));
    PumpMessages();
    top = FindTitle(tree, TreeView_GetRoot(tree), topTitle);
    GetScrollInfo(viewport, SB_VERT, &scroll);
    Check(fixture, viewport, "the row on top stays on top, as far above the view's top",
          top && TreeView_GetItemRect(tree, top, &row, FALSE) && scroll.nPos == row.top + offset);
    if (!top || scroll.nPos != row.top + offset) printf("        view=%d row=%ld offset=%d\n", scroll.nPos, row.top, offset);
}

/* The watcher reads a profile's own folder for its config.json and for its
 * entries folder appearing: Chromium's other writes there ask for nothing. */
static void CheckSessionsWatcher(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    WCHAR path[MAX_PATH];
    int seen, write;
    BOOL written = TRUE;
    Check(fixture, NULL, "every write so far is read before the profile's other files change", AwaitWritesRead(sessions, 0, L"Settled conversation A"));
    seen = fixture->reloadRequests;
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Preferences", sessions->profiles.items[0].storageDir);
    for (write = 0; write < 3; write++) written = SaveFixtureFile(sessions->root.path, path, write % 2 ? "{}" : "{\"a\":1}", TRUE) && written;
    PumpUntil(NULL, NULL, IGNORED_WRITE_MS);
    Check(fixture, NULL, "other writes in a profile's folder ask for no snapshot", written && fixture->reloadRequests == seen);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\config.json", sessions->profiles.items[0].storageDir);
    written = SaveFixtureFile(sessions->root.path, path, "{\"lastKnownAccountUuid\":\"account-ui\",\"fixture\":1}", TRUE);
    Check(fixture, NULL, "a changed config.json asks for a snapshot", written && AwaitReloadRequest(fixture, seen, SNAPSHOT_TIMEOUT_MS));
    Check(fixture, NULL, "every write so far is read again", AwaitWritesRead(sessions, 1, L"Settled conversation B"));
    seen = fixture->reloadRequests;
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\claude-code-sessions", sessions->profiles.items[1].storageDir);
    Check(fixture, NULL, "a profile's entries folder appearing asks for a snapshot",
          CreateDirectoryW(path, NULL) && AwaitReloadRequest(fixture, seen, SNAPSHOT_TIMEOUT_MS));
}

typedef struct TranscriptStream { SessionsFixture *sessions; WCHAR path[MAX_PATH]; } TranscriptStream;
static TranscriptStream g_stream;

/* One more line in a transcript: what a running Claude Code does. */
static void CALLBACK StreamTranscript(HWND window, UINT message, UINT_PTR id, DWORD time)
{
    HANDLE file;
    DWORD written;
    static const char kLine[] = "{\"type\":\"assistant\"}\n";
    (void)window; (void)message; (void)id; (void)time;
    file = CreateFileW(g_stream.path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (WriteFile(file, kLine, (DWORD)strlen(kLine), &written, NULL)) g_stream.sessions->streamWrites++;
    CloseHandle(file);
}

/* Notifications that never stop (a transcript written all the time) still
 * let snapshots through: a session added meanwhile shows. */
static void CheckSessionsUnderStream(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    WCHAR id[SESSION_ID_CCH];
    UINT_PTR timer;
    BOOL shown;
    int writes;
    g_stream.sessions = sessions;
    SessionId(0, id, ARRAYSIZE(id));
    StringCchPrintfW(g_stream.path, ARRAYSIZE(g_stream.path), L"%s\\%s.jsonl", sessions->transcripts, id);
    timer = SetTimer(sessions->dialog, TRANSCRIPT_STREAM_TIMER, USER_TIMER_MINIMUM, StreamTranscript);
    Check(fixture, NULL, "a transcript stream started", timer != 0);
    if (!timer) return;
    PumpUntil(NULL, NULL, 200);
    writes = sessions->streamWrites;
    Check(fixture, NULL, "a session added during the stream written",
          WriteSession(sessions, UNKNOWN_ROW + 4, sessions->folders[0], L"Streamed conversation", FIXTURE_TIME + 4000, FALSE));
    shown = AwaitTitle(sessions, L"Streamed conversation");
    Check(fixture, sessions->tree, "a snapshot shows while transcripts change continuously", shown && sessions->streamWrites > writes);
    KillTimer(sessions->dialog, TRANSCRIPT_STREAM_TIMER);
    Check(fixture, NULL, "every write of the stream is read", AwaitWritesRead(sessions, 2, L"Settled conversation C"));
}

/* A double-click opens the selected session only on its row: below the last
 * row it opens nothing. Without Claude, an open asks for it (a message box
 * the fixture closes) and starts nothing. */
static void CheckSessionsDoubleClick(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree, search = GetDlgItem(dialog, IDC_S_SEARCH);
    WCHAR title[64], shown[256];
    HTREEITEM row;
    RECT rowRect = { 0, 0, 0, 0 }, client;
    POINT point;
    int prompts;
    RowTitle(5, title, ARRAYSIZE(title));
    SetWindowTextW(search, L"conversation 05");
    PumpMessages();
    row = FindTitle(tree, TreeView_GetRoot(tree), title);
    if (row) TreeView_SelectItem(tree, row);
    PumpMessages();
    GetClientRect(tree, &client);
    Check(fixture, tree, "a search leaves one session, selected, with room below it",
          row && TreeView_GetSelection(tree) == row && ItemTitle(tree, row, shown, ARRAYSIZE(shown)) &&
          TreeView_GetItemRect(tree, row, &rowRect, FALSE) && rowRect.bottom + 20 < client.bottom);
    if (!row || TreeView_GetSelection(tree) != row) {
        SetWindowTextW(search, L"");
        return;
    }
    prompts = fixture->openPrompts;
    point.x = (rowRect.left + rowRect.right) / 2;
    point.y = (rowRect.top + rowRect.bottom) / 2;
    if (!DoubleClickTree(dialog, fixture, tree, point)) {
        printf("  skip  sessions double-click: this Windows reports a posted mouse message's position otherwise\n");
        SetWindowTextW(search, L"");
        return;
    }
    Check(fixture, tree, "a double-click on the selected session's row opens it", fixture->openPrompts == prompts + 1 &&
          wcscmp(fixture->promptText, TR(L"Claude Desktop is not installed.")) == 0);
    prompts = fixture->openPrompts;
    point.y = (rowRect.bottom + client.bottom) / 2;
    DoubleClickTree(dialog, fixture, tree, point);
    Check(fixture, tree, "a double-click below the last row opens nothing", fixture->openPrompts == prompts && TreeView_GetSelection(tree) == row);
    SetWindowTextW(search, L"");
    PumpMessages();
}

/* Hovering `x` lights the Actions box under `inside` (the box lights whole). */
static BOOL ActionsBoxLit(HWND parts, int x, POINT inside)
{
    COLORREF before, after;
    HDC dc;
    SendMessageW(parts, WM_MOUSELEAVE, 0, 0);
    UpdateWindow(parts);
    dc = GetDC(parts);
    before = GetPixel(dc, inside.x, inside.y);
    ReleaseDC(parts, dc);
    SendMessageW(parts, WM_MOUSEMOVE, 0, MAKELPARAM(x, inside.y));
    UpdateWindow(parts);
    dc = GetDC(parts);
    after = GetPixel(dc, inside.x, inside.y);
    ReleaseDC(parts, dc);
    SendMessageW(parts, WM_MOUSELEAVE, 0, 0);
    UpdateWindow(parts);
    return before != CLR_INVALID && after != CLR_INVALID && before != after;
}

/* A profile's Actions box in the details is exactly a drop-down button for
 * its caption (Theme_DropDownWidth): its edges are where hovering stops
 * lighting it. */
static void CheckSessionsActionsWidth(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, parts = GetDlgItem(GetDlgItem(dialog, IDC_S_DETAILS), IDC_S_PARTS);
    RECT client;
    POINT inside;
    int low, high, left, expected;
    if (!parts || !GetClientRect(parts, &client)) {
        Check(fixture, parts, "the details' scrolling part found", FALSE);
        return;
    }
    inside.x = client.right - 8;
    inside.y = 15;
    if (!ActionsBoxLit(parts, inside.x, inside)) {
        Check(fixture, parts, "the details' Actions box lights up under the mouse", FALSE);
        return;
    }
    for (low = 0, high = inside.x; low < high;) {
        int middle = (low + high) / 2;
        if (ActionsBoxLit(parts, middle, inside)) high = middle;
        else low = middle + 1;
    }
    left = low;
    for (low = inside.x, high = client.right; low + 1 < high;) {
        int middle = (low + high) / 2;
        if (ActionsBoxLit(parts, middle, inside)) low = middle;
        else high = middle;
    }
    expected = Theme_DropDownWidth(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0), TR(Theme_SessionsCaption(SESSIONS_ACTIONS)));
    Check(fixture, parts, "the details' Actions box is as wide as a drop-down button for its caption", high - left == expected);
    if (high - left != expected) printf("        box %d..%d, expected width %d\n", left, high, expected);
}

/* The side bar's list keeps the profiles' names (screen readers read them). */
static void CheckSessionsProfileNames(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND list = GetWindow(fixture->profilesViewport, GW_CHILD);
    BOOL named = list && (int)SendMessageW(list, LB_GETCOUNT, 0, 0) == sessions->profiles.count;
    int p;
    for (p = 0; named && p < sessions->profiles.count; p++) {
        WCHAR text[LABEL_CCH];
        named = SendMessageW(list, LB_GETTEXTLEN, (WPARAM)p, 0) < (LRESULT)ARRAYSIZE(text) &&
                SendMessageW(list, LB_GETTEXT, (WPARAM)p, (LPARAM)text) != LB_ERR && wcscmp(text, sessions->profiles.items[p].name) == 0;
    }
    Check(fixture, list, "the side bar's list gives each profile's name", named);
}

/* gui.c gives the shown view the focus; sessions.c entering its view
 * moves none. */
static void CheckSessionsFocus(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, button = GetDlgItem(dialog, IDC_SESSIONS);
    const WCHAR *folder = sessions->profiles.items[0].folder;
    Gui_ShowSessions(dialog, &sessions->package, FALSE, NULL);
    fixture->sessions = FALSE;
    SetFocus(button);
    if (GetFocus() == button) {
        SessionsView_Enter(&sessions->package, folder);
        Check(fixture, button, "entering the sessions view leaves the focus where it is", GetFocus() == button);
        SessionsView_Leave();
    } else printf("  skip  sessions focus: the focus could not be taken here\n");
    Gui_ShowSessions(dialog, &sessions->package, TRUE, folder);
    fixture->sessions = TRUE;
    Check(fixture, sessions->tree, "showing the sessions view gives its tree the focus", GetFocus() == sessions->tree);
    PumpMessages();
}

/* The sessions view of a real manager layout, filled by sessions.c from
 * private profiles: resizes in three languages, and in one of them its
 * refills, watcher and double-clicks. */
static void CheckPopulatedSessions(void)
{
    static const WCHAR *const kLanguages[] = { L"fr", L"es", L"zh-CN" };
    size_t language;
    if (!NativeScaleFits("the populated sessions view")) return;
    for (language = 0; language < ARRAYSIZE(kLanguages); language++) {
        SessionsFixture sessions;
        TreeWait wait;
        if (OpenSessionsFixture(&sessions, Language(kLanguages[language]))) {
            LayoutFixture *fixture = &sessions.layout;
            FixtureRoots roots;
            wait.tree = sessions.tree;
            wait.count = SESSION_ROWS + 1 + 1 + 4;   /* every session, the starred one's second row, four roots */
            wait.title = NULL;
            Check(fixture, sessions.tree, "event-driven private snapshot populates the native tree", PumpUntil(TreeShows, &wait, SNAPSHOT_TIMEOUT_MS));
            roots = ReadRoots(sessions.tree);
            Check(fixture, sessions.tree, "native tree holds Starred, two folders and the sessions with no folder",
                  roots.starred && roots.alpha && roots.beta && roots.unknown && roots.unknown != roots.beta &&
                  TreeView_GetCount(sessions.tree) == wait.count);
            CheckSessionsResize(&sessions);
            CheckSessionsActionsWidth(&sessions);
            if (language == 0) {
                CheckSessionsProfileNames(&sessions);
                CheckSessionsRefillKeepsState(&sessions);
                CheckSessionsRefillKeepsTopRow(&sessions);
                CheckSessionsWatcher(&sessions);
                CheckSessionsUnderStream(&sessions);
                CheckSessionsDoubleClick(&sessions);
                CheckSessionsFocus(&sessions);
            }
        }
        CloseSessionsFixture(&sessions);
    }
}

int wmain(void)
{
    INITCOMMONCONTROLSEX controls = { sizeof controls, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    WCHAR temp[MAX_PATH], state[MAX_PATH];
    LayoutFixture none;
    int language;
    setvbuf(stdout, NULL, _IONBF, 0);
    g_hInst = GetModuleHandleW(NULL);
    /* As the program runs (app.res): common controls 6 and per-monitor DPI. */
    if (!InitCommonControlsEx(&controls) ||
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        printf("test_layout must be linked with the program's manifest (app.res, /MANIFEST:NO).\n");
        return 1;
    }
    /* The log and the queued session changes, in a private folder before anything is logged. */
    if (!GetTempPathW(ARRAYSIZE(temp), temp) || !GetTempFileNameW(temp, L"cdl", 0, state) || !DeleteFileW(state) || !CreateDirectoryW(state, NULL)) {
        printf("Layout test private state folder could not be made.\n");
        return 1;
    }
    Util_SetStateDir(state);
    Theme_Init();
    if (!PrepareMainTemplate()) {
        printf("Layout test class could not be registered.\n");
        RemoveFixtureTree(state, state);
        return 1;
    }
    MeasureScalesAgainstWorkArea();
    CheckDialogs();
    CheckMessageBoxes();
    CheckHiddenRows();
    CheckMainStates();
    CheckShortcutFallback();
    CheckProfileNotes();
    CheckLanguageRoundTrips();
    CheckResponsiveMain();
    CheckMainFrameMessages();
    if (NativeScaleFits("the real sessions view's transitions")) for (language = 0; language < Localize_LanguageCount(); language++) {
        const WCHAR *code = Localize_LanguageCode(language);
        if (wcscmp(code, L"fr") == 0 || wcscmp(code, L"es") == 0 || wcscmp(code, L"zh-CN") == 0) CheckRealSessionsTransition(language);
    }
    CheckPopulatedSessions();
    UnregisterClassW(g_fixtureClass, g_hInst);
    HeapFree(GetProcessHeap(), 0, g_mainTemplate);
    ZeroMemory(&none, sizeof none);
    Check(&none, NULL, "private state folder removed without the Recycle Bin", RemoveFixtureTree(state, state));
    printf("Layout tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
