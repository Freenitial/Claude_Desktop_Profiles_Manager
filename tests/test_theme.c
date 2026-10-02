/*
 * What theme.c draws and does, compared with Windows' own drawing wherever
 * Windows draws the same thing, in the mode Windows is set to (dark or
 * light apps):
 *  - rows: the list views' selected and hot rows (the main, bright and pale
 *    blues), rounded inside their bounds; rounded control regions at the
 *    window's DPI;
 *  - report tables: their native glyphs and check boxes, header dividers on
 *    the rows' column lines and no outer separator, drawing that stays right
 *    as Windows scrolls them and shifts their columns, the columns the user
 *    sized (a divider dragged or double-clicked);
 *  - push buttons and check boxes, clipped-cell and info tooltips, edits
 *    (text centered, the theme's selection, one paint per input, printing),
 *    drop-down buttons and lists (choices in the native menu), scroll bars
 *    (to the edges, dark), headers' titles;
 *  - scrolling: the wheel, views scrolled by the pixel (Page Up and Page
 *    Down, the keys that scroll a tree, the keyboard's row brought into
 *    sight, a place kept through refills, resizes and new fonts, a list too
 *    tall for a view, a wheel stopped), the last column's width;
 *  - links, the focus cue and secondary text, the profile note's name cut,
 *    off-screen drawing, and what a themed window leaves once destroyed.
 * Built and run by build.cmd with the program's resources and manifest
 * (common controls 6, per-monitor DPI). A check that needs dark mode, a
 * visual style, the focus or an image Windows does not give here is skipped
 * with its reason.
 */
#include "../src/app.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdio.h>
#include <stdlib.h>

/* The longest a wheel animation may take to reach its target, on a loaded
 * machine; it is over as soon as it does. */
#define ANIMATION_TIMEOUT_MS 2000
/* An animation stopped stays where it is: the next frame of one still going
 * would come within one timer period. */
#define STOPPED_ANIMATION_MS 250

enum {
    NATIVE_ROWS_SUBCLASS = 1, TREE_TIP_SUBCLASS, TIP_WINDOW_SUBCLASS, THEMED_COLORS_SUBCLASS, EDIT_PROBE_SUBCLASS,
    EDIT_PAUSE_SUBCLASS, CHOICE_PROBE_SUBCLASS, SCROLL_TRACE_SUBCLASS, TABLE_DESTROY_SUBCLASS,
    PRINT_PROBE_SUBCLASS, SELECTION_COUNT_SUBCLASS, FRAME_SAMPLE_SUBCLASS, DRAG_WATCH_SUBCLASS
};

static int g_failures, g_checks;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    printf("  FAIL  %s\n", name);
}

static void Skip(const char *what)
{
    printf("  skip  %s\n", what);
}

static void CheckColor(const char *name, COLORREF expected, COLORREF actual)
{
    Check(name, expected == actual);
    if (expected != actual)
        printf("        expected #%02X%02X%02X, got #%02X%02X%02X\n", GetRValue(expected), GetGValue(expected), GetBValue(expected),
               GetRValue(actual), GetGValue(actual), GetBValue(actual));
}

/* A 32-bit bitmap to draw on and read back. */
typedef struct Canvas {
    HDC      dc;
    HBITMAP  bitmap;
    HGDIOBJ  previousBitmap;
    DWORD   *pixels;
    int      width, height;
} Canvas;

static BOOL CanvasOpen(Canvas *canvas, int width, int height, COLORREF fill)
{
    BITMAPINFO info;
    RECT all;
    void *bits = NULL;
    ZeroMemory(canvas, sizeof *canvas);
    ZeroMemory(&info, sizeof info);
    if (width <= 0 || height <= 0) return FALSE;
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if ((canvas->dc = CreateCompatibleDC(NULL)) == NULL) return FALSE;
    if ((canvas->bitmap = CreateDIBSection(canvas->dc, &info, DIB_RGB_COLORS, &bits, NULL, 0)) == NULL) {
        DeleteDC(canvas->dc);
        ZeroMemory(canvas, sizeof *canvas);
        return FALSE;
    }
    canvas->previousBitmap = SelectObject(canvas->dc, canvas->bitmap);
    canvas->pixels = (DWORD *)bits;
    canvas->width = width;
    canvas->height = height;
    SetRect(&all, 0, 0, width, height);
    SetDCBrushColor(canvas->dc, fill);
    FillRect(canvas->dc, &all, (HBRUSH)GetStockObject(DC_BRUSH));
    GdiFlush();
    return TRUE;
}

static COLORREF PixelAt(const Canvas *canvas, int x, int y)
{
    DWORD pixel;
    GdiFlush();
    pixel = canvas->pixels[y * canvas->width + x];
    return RGB((pixel >> 16) & 0xFF, (pixel >> 8) & 0xFF, pixel & 0xFF);
}

static void CanvasClose(Canvas *canvas)
{
    if (!canvas->dc) return;
    SelectObject(canvas->dc, canvas->previousBitmap);
    DeleteObject(canvas->bitmap);
    DeleteDC(canvas->dc);
    ZeroMemory(canvas, sizeof *canvas);
}

/* `source` of `window`'s client (or of its whole window), copied to
 * (`x`, `y`) of the canvas: what the window drew, whatever covers it. */
static BOOL CopyFromWindow(HWND window, BOOL whole, const RECT *source, Canvas *canvas, int x, int y)
{
    HDC dc = whole ? GetWindowDC(window) : GetDC(window);
    BOOL copied = dc && BitBlt(canvas->dc, x, y, source->right - source->left, source->bottom - source->top, dc, source->left, source->top, SRCCOPY);
    if (dc) ReleaseDC(window, dc);
    GdiFlush();
    return copied;
}

static BOOL CopyClient(HWND window, Canvas *canvas)
{
    RECT client;
    GetClientRect(window, &client);
    return CopyFromWindow(window, FALSE, &client, canvas, 0, 0);
}

/* Windows gave an image of the window: not every pixel alike (no desktop to
 * draw on gives none). Says so when it did not. */
static BOOL HasImage(const Canvas *canvas, const char *test)
{
    int i;
    char line[160];
    GdiFlush();
    for (i = 1; i < canvas->width * canvas->height; i++)
        if ((canvas->pixels[i] & 0xffffff) != (canvas->pixels[0] & 0xffffff)) return TRUE;
    StringCchPrintfA(line, ARRAYSIZE(line), "%s: Windows gave no image of the window", test);
    Skip(line);
    return FALSE;
}

/* How many pixels of `area` (inside a one-pixel margin) are light: a dark
 * surface has few. */
static BOOL MostlyDark(const Canvas *canvas, const RECT *area, int *bright, int *samples)
{
    int x, y;
    *bright = *samples = 0;
    for (y = max(0, area->top + 1); y < min(canvas->height, area->bottom - 1); y++)
        for (x = max(0, area->left + 1); x < min(canvas->width, area->right - 1); x++) {
            COLORREF color = PixelAt(canvas, x, y);
            if ((GetRValue(color) + GetGValue(color) + GetBValue(color)) / 3 > 150) (*bright)++;
            (*samples)++;
        }
    return *samples > 0 && *bright * 4 < *samples;
}

static BOOL HighContrastOn(void)
{
    HIGHCONTRASTW contrast;
    ZeroMemory(&contrast, sizeof contrast);
    contrast.cbSize = sizeof contrast;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

static BOOL SkipInLight(const char *test)
{
    char line[160];
    if (Theme_IsDark()) return FALSE;
    StringCchPrintfA(line, ARRAYSIZE(line), "%s: Windows is set to light apps", test);
    Skip(line);
    return TRUE;
}

/* ------------------------------------------------------------- messages */

/* The thread's pending messages, handled. */
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

/* The keyboard focus on `control`. A thread that cannot take it here skips
 * what needs it, and says so. */
static BOOL TakeFocus(HWND control, const char *what)
{
    char line[200];
    SetFocus(control);
    if (GetFocus() == control) return TRUE;
    StringCchPrintfA(line, ARRAYSIZE(line), "%s: the focus could not be taken here", what);
    Skip(line);
    return FALSE;
}

/* ------------------------------------------------------------------ rows */

/* Theme_DrawRow against the list view item of Windows' theme, drawn over
 * the same field: fill, frame and corners. */
static void TestRows(void)
{
    static const struct { int state; UINT flags; const char *name; } kStates[] = {
        { LISS_SELECTED, THEME_ROW_SELECTED, "a selected row" },
        { LISS_HOT, THEME_ROW_HOT, "a row under the mouse" },
    };
    static const struct { int x, y; const char *where; } kPoints[] = {
        { 30, 12, "native fill" }, { 59, 23, "rounded corner" }, { 0, 0, "other rounded corner" },
    };
    const WCHAR *themeClass = Theme_IsDark() ? L"DarkMode_Explorer::ListView" : L"Explorer::ListView";
    HTHEME theme;
    COLORREF field = Theme_Color(THEME_FIELD);
    RECT row = { 0, 0, 60, 24 };
    char name[160];
    size_t state, point;
    if (HighContrastOn()) {
        Skip("rows: contrast colors use the system's rectangular selection");
        return;
    }
    if ((theme = OpenThemeData(NULL, themeClass)) == NULL) {
        StringCchPrintfA(name, ARRAYSIZE(name), "rows: no %ls visual style", themeClass);
        Skip(name);
        return;
    }
    for (state = 0; state < ARRAYSIZE(kStates); state++) {
        Canvas native = { 0 }, ours = { 0 };
        if (!CanvasOpen(&native, 60, 24, field) || !CanvasOpen(&ours, 60, 24, field)) {
            CanvasClose(&native);
            CanvasClose(&ours);
            Check("rows: canvases created", FALSE);
            continue;
        }
        DrawThemeBackground(theme, native.dc, LVP_LISTITEM, kStates[state].state, &row, NULL);
        Theme_DrawRow(NULL, ours.dc, &row, kStates[state].flags, field);
        for (point = 0; point < ARRAYSIZE(kPoints); point++) {
            StringCchPrintfA(name, ARRAYSIZE(name), "%s is drawn as the list views draw it: %s", kStates[state].name, kPoints[point].where);
            CheckColor(name, point == 0 ? PixelAt(&native, kPoints[point].x, kPoints[point].y) : field,
                       PixelAt(&ours, kPoints[point].x, kPoints[point].y));
        }
        StringCchPrintfA(name, ARRAYSIZE(name), "%s retains its top edge away from the rounded corner", kStates[state].name);
        Check(name, PixelAt(&ours, 30, 0) != field);
        StringCchPrintfA(name, ARRAYSIZE(name), "%s retains its side away from the rounded corner", kStates[state].name);
        Check(name, PixelAt(&ours, 0, 12) != field);
        CanvasClose(&native);
        CanvasClose(&ours);
    }
    {
        Canvas plain;
        if (CanvasOpen(&plain, 10, 10, RGB(1, 2, 3))) {
            RECT square = { 0, 0, 10, 10 };
            Theme_DrawRow(NULL, plain.dc, &square, 0, field);
            CheckColor("a row neither selected nor under the mouse is the list's background", field, PixelAt(&plain, 5, 5));
            CanvasClose(&plain);
        }
    }
    CloseThemeData(theme);
}

static void TestRoundedRowBounds(void)
{
    static const SIZE kSizes[] = { { 1, 1 }, { 2, 2 }, { 4, 4 }, { 8, 8 }, { 40, 24 } };
    size_t i;
    if (HighContrastOn()) {
        Skip("rounded rows: contrast colors use rectangular selections");
        return;
    }
    for (i = 0; i < ARRAYSIZE(kSizes); i++) {
        Canvas canvas = { 0 };
        RECT row;
        COLORREF guard = RGB(1, 2, 3);
        BOOL bounded = TRUE;
        int x, y;
        char name[160];
        SetRect(&row, 4, 4, 4 + kSizes[i].cx, 4 + kSizes[i].cy);
        if (!CanvasOpen(&canvas, row.right + 4, row.bottom + 4, guard)) {
            Check("rounded rows: private canvas", FALSE);
            continue;
        }
        Theme_DrawRow(NULL, canvas.dc, &row, THEME_ROW_SELECTED, Theme_Color(THEME_FIELD));
        for (y = 0; y < canvas.height; y++) for (x = 0; x < canvas.width; x++)
            if ((x < row.left || x >= row.right || y < row.top || y >= row.bottom) && PixelAt(&canvas, x, y) != guard) bounded = FALSE;
        StringCchPrintfA(name, ARRAYSIZE(name), "rounded rows: a %ld by %ld row stays inside its raw bounds", kSizes[i].cx, kSizes[i].cy);
        Check(name, bounded);
        if (kSizes[i].cx >= 8 && kSizes[i].cy >= 8) {
            StringCchPrintfA(name, ARRAYSIZE(name), "rounded rows: a %ld by %ld row's corner reveals the surrounding field", kSizes[i].cx, kSizes[i].cy);
            CheckColor(name, Theme_Color(THEME_FIELD), PixelAt(&canvas, row.left, row.top));
            StringCchPrintfA(name, ARRAYSIZE(name), "rounded rows: a %ld by %ld row's center has the palette fill", kSizes[i].cx, kSizes[i].cy);
            CheckColor(name, Theme_Color(THEME_MAIN_BLUE), PixelAt(&canvas, (row.left + row.right) / 2, (row.top + row.bottom) / 2));
        }
        CanvasClose(&canvas);
    }
}

/* --------------------------------------------------------------- windows */

static HWND g_themedHost;

/* The controls' window: on screen (Windows only composes what is), but
 * transparent to the eye and the mouse, and never activated. */
static HWND ThemedHost(void)
{
    if (!g_themedHost) {
        g_themedHost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, WC_STATICW, L"",
                                       WS_POPUP | WS_CLIPCHILDREN, 0, 0, 500, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (g_themedHost) SetLayeredWindowAttributes(g_themedHost, 0, 1, LWA_ALPHA);
    }
    return g_themedHost;
}

static HFONT DialogFont(void)
{
    LOGFONTW font;
    ZeroMemory(&font, sizeof font);
    font.lfHeight = -12;
    StringCchCopyW(font.lfFaceName, ARRAYSIZE(font.lfFaceName), L"Segoe UI");
    return CreateFontIndirectW(&font);
}

/* A color that clearly differs from `background`. */
static BOOL StandsOut(COLORREF color, COLORREF background)
{
    return abs((int)GetRValue(color) - (int)GetRValue(background)) > 16 || abs((int)GetGValue(color) - (int)GetGValue(background)) > 16 ||
           abs((int)GetBValue(color) - (int)GetBValue(background)) > 16;
}

/* The host answers its controls' colors as a dialog does. */
static LRESULT CALLBACK ThemedColors(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    (void)id;
    (void)reference;
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) return Theme_CtlColor(message, wp, lp, 0);
    return DefSubclassProc(window, message, wp, lp);
}

static void TestRoundedControlRegions(void)
{
    static const struct { const WCHAR *windowClass; DWORD style; int height; const char *name; } kControls[] = {
        { WC_EDITW, ES_AUTOHSCROLL, 28, "a one-line edit" },
        { WC_EDITW, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 100, "a multi-line edit" },
        { WC_EDITW, ES_READONLY | ES_AUTOHSCROLL, 28, "a read-only edit" },
        { WC_LISTVIEWW, LVS_REPORT, 100, "a list view in a view" },
        { WC_TREEVIEWW, TVS_HASBUTTONS, 100, "a tree in a view" },
        { WC_LISTBOXW, LBS_NOINTEGRALHEIGHT, 100, "a list box in a view" },
    };
    static const char *const kPhases[] = { "as made", "widened", "four pixels square" };
    HWND host = ThemedHost();
    size_t kind;
    int phase;
    if (HighContrastOn()) {
        Skip("rounded controls: contrast colors keep the native regions");
        return;
    }
    for (kind = 0; kind < ARRAYSIZE(kControls); kind++) {
        HWND control = CreateWindowExW(0, kControls[kind].windowClass, kind < 3 ? L"Native text" : L"",
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | kControls[kind].style, 10, 10, 160, kControls[kind].height,
                                       host, NULL, GetModuleHandleW(NULL), NULL);
        HRGN actual = CreateRectRgn(0, 0, 0, 0);
        if (control && kind >= 3) control = Theme_SmoothView(control);
        if (!control || !actual) {
            Check("rounded controls: private fixture created", FALSE);
            if (control) DestroyWindow(control);
            if (actual) DeleteObject(actual);
            continue;
        }
        Theme_Apply(host);
        for (phase = 0; phase < (int)ARRAYSIZE(kPhases); phase++) {
            RECT window;
            HRGN expected;
            char name[160];
            int width, height, radius;
            if (phase) SetWindowPos(control, NULL, 0, 0, phase == 1 ? 220 : 4, phase == 1 ? 72 : 4, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(control, &window);
            width = window.right - window.left;
            height = window.bottom - window.top;
            radius = min(MulDiv(THEME_CORNER_RADIUS_DIPS, (int)GetDpiForWindow(control), 96), min(width, height) / 2);
            expected = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
            StringCchPrintfA(name, ARRAYSIZE(name), "rounded controls: %s, %s, has the shared corners at its DPI", kControls[kind].name, kPhases[phase]);
            Check(name, expected && GetWindowRgn(control, actual) != ERROR && EqualRgn(expected, actual));
            if (expected) DeleteObject(expected);
        }
        DestroyWindow(control);
        DeleteObject(actual);
    }
    {
        /* The caller's region covers the top left corner the shared shape rounds off. */
        HWND edit = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL, 10, 10, 160, 28, host, NULL, GetModuleHandleW(NULL), NULL);
        int diameter = edit ? 2 * MulDiv(THEME_CORNER_RADIUS_DIPS, (int)GetDpiForWindow(edit), 96) : 0;
        HRGN original = CreateRectRgn(0, 0, 120, 25), caller = CreateRectRgn(0, 0, 0, 0), actual = CreateRectRgn(0, 0, 0, 0);
        HRGN expected = CreateRoundRectRgn(0, 0, 161, 29, diameter, diameter);
        BOOL kept = FALSE, read = FALSE;
        if (edit && original && caller && actual && expected) {
            CombineRgn(caller, original, NULL, RGN_COPY);
            if (SetWindowRgn(edit, original, FALSE)) original = NULL;
            Theme_Apply(host);
            CombineRgn(expected, expected, caller, RGN_AND);
            read = GetWindowRgn(edit, actual) != ERROR;
            kept = read && EqualRgn(expected, actual) && !EqualRgn(caller, actual);
        }
        Check("rounded controls: caller clipping crossing a corner is cut by the shared shape", kept);
        if (edit) DestroyWindow(edit);
        if (original) DeleteObject(original);
        if (caller) DeleteObject(caller);
        if (actual) DeleteObject(actual);
        if (expected) DeleteObject(expected);
    }
}

/* ---------------------------------------------------------------- tables */

static LRESULT CALLBACK NativeReportRows(HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    (void)id;
    if (message == WM_NOTIFY && lp && ((const NMHDR *)lp)->hwndFrom == (HWND)reference && ((const NMHDR *)lp)->code == NM_CUSTOMDRAW)
        return CDRF_DODEFAULT;
    return DefSubclassProc(host, message, wp, lp);
}

/* The pixels a check box change repaints when Windows draws the list. */
static BOOL NativeCheckDifference(HWND list, RECT *difference)
{
    HWND host = GetParent(list);
    RECT client;
    Canvas checked = { 0 }, unchecked = { 0 };
    BOOL captured;
    int x, y;
    GetClientRect(list, &client);
    SetRect(difference, client.right, client.bottom, 0, 0);
    if (!SetWindowSubclass(host, NativeReportRows, NATIVE_ROWS_SUBCLASS, (DWORD_PTR)list)) return FALSE;
    captured = CanvasOpen(&checked, client.right, client.bottom, RGB(1, 2, 3)) && CanvasOpen(&unchecked, client.right, client.bottom, RGB(1, 2, 3));
    if (captured) {
        ListView_SetCheckState(list, 0, TRUE);
        RedrawWindow(list, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        captured = CopyClient(list, &checked);
        ListView_SetCheckState(list, 0, FALSE);
        RedrawWindow(list, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        captured = CopyClient(list, &unchecked) && captured;
        if (captured) for (y = 0; y < checked.height; y++) for (x = 0; x < checked.width; x++)
            if (PixelAt(&checked, x, y) != PixelAt(&unchecked, x, y)) {
                difference->left = min(difference->left, x);
                difference->top = min(difference->top, y);
                difference->right = max(difference->right, x + 1);
                difference->bottom = max(difference->bottom, y + 1);
            }
    }
    RemoveWindowSubclass(host, NativeReportRows, NATIVE_ROWS_SUBCLASS);
    CanvasClose(&checked);
    CanvasClose(&unchecked);
    return captured && !IsRectEmpty(difference);
}

/* A report row's own glyphs survive the themed row: the image list's icon,
 * the native label, the check box Windows draws, subitem images. */
static void TestReportGlyphs(void)
{
    static const char *const kModes[] = { "with the focus", "without its image", "disabled" };
    HWND host = ThemedHost(), list;
    HFONT font = DialogFont();
    HIMAGELIST icons = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
    Canvas image = { 0 }, painted = { 0 };
    LVCOLUMNW column = { 0 };
    LVITEMW item = { 0 };
    RECT icon, label, client;
    UINT before;
    int i, x, y, green = 0, textInk = 0;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                           10, 10, 300, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list || !icons || !CanvasOpen(&image, 16, 16, RGB(0, 200, 0))) {
        Check("report glyphs: private fixture created", FALSE);
        if (list) DestroyWindow(list);
        if (icons) ImageList_Destroy(icons);
        DeleteObject(font);
        CanvasClose(&image);
        return;
    }
    /* Opaque: a 32-bit image list draws a pixel's alpha. */
    for (i = 0; i < 16 * 16; i++) image.pixels[i] = 0xFF00C800;
    SelectObject(image.dc, image.previousBitmap);
    Check("report glyphs: the private image-list asset is accepted", ImageList_Add(icons, image.bitmap, NULL) == 0);
    SelectObject(image.dc, image.bitmap);
    CanvasClose(&image);
    ListView_SetImageList(list, icons, LVSIL_SMALL);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
    column.mask = LVCF_WIDTH;
    column.cx = 280;
    ListView_InsertColumn(list, 0, &column);
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.pszText = (LPWSTR)L"Native glyphs";
    item.iImage = 0;
    ListView_InsertItem(list, &item);
    ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED | INDEXTOSTATEIMAGEMASK(2), LVIS_SELECTED | LVIS_FOCUSED | LVIS_STATEIMAGEMASK);
    before = ListView_GetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED | LVIS_STATEIMAGEMASK);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    GetClientRect(list, &client);
    ListView_GetItemRect(list, 0, &icon, LVIR_ICON);
    ListView_GetItemRect(list, 0, &label, LVIR_LABEL);
    if (!CanvasOpen(&painted, client.right, client.bottom, RGB(1, 2, 3)) || !CopyClient(list, &painted) || !HasImage(&painted, "report glyphs")) {
        CanvasClose(&painted);
        DestroyWindow(list);
        ImageList_Destroy(icons);
        DeleteObject(font);
        ShowWindow(host, SW_HIDE);
        return;
    }
    for (y = max(0, icon.top); y < min(icon.bottom, painted.height); y++) for (x = max(0, icon.left); x < min(icon.right, painted.width); x++) {
        COLORREF color = PixelAt(&painted, x, y);
        if (GetGValue(color) > GetRValue(color) + 20 && GetGValue(color) > GetBValue(color) + 20) green++;
    }
    for (y = max(0, label.top); y < min(label.bottom, painted.height); y++) for (x = max(0, label.left); x < min(label.right - 20, painted.width); x++) {
        COLORREF color = PixelAt(&painted, x, y);
        if (Theme_IsDark() ? GetRValue(color) > 180 && GetGValue(color) > 180 && GetBValue(color) > 180 :
            GetRValue(color) < 80 && GetGValue(color) < 80 && GetBValue(color) < 80) textInk++;
    }
    Check("report glyphs: the image-list glyph survives rounded background painting", green > 100);
    Check("report glyphs: system text rendering retains visible native labels", textInk > 20);
    Check("report glyphs: painting preserves native selection, focus and check model",
          before == ListView_GetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED | LVIS_STATEIMAGEMASK));
    for (i = 0; i < (int)ARRAYSIZE(kModes); i++) {
        Canvas toggled = { 0 };
        int changed = 0;
        BOOL reference;
        RECT actual, native;
        char name[160];
        SetRect(&actual, painted.width, painted.height, 0, 0);
        if (i == 0 && !TakeFocus(list, "report glyphs with the focus")) continue;
        if (i > 0) SetFocus(host);
        if (i == 1) {
            LVITEMW noImage = { 0 };
            ListView_SetImageList(list, NULL, LVSIL_SMALL);
            noImage.mask = LVIF_IMAGE;
            noImage.iImage = I_IMAGENONE;
            ListView_SetItem(list, &noImage);
        }
        if (i == 2) EnableWindow(list, FALSE);
        ListView_SetCheckState(list, 0, TRUE);
        RedrawWindow(list, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        CopyClient(list, &painted);
        ListView_SetCheckState(list, 0, FALSE);
        RedrawWindow(list, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (CanvasOpen(&toggled, client.right, client.bottom, RGB(1, 2, 3)) && CopyClient(list, &toggled))
            for (y = 0; y < painted.height; y++) for (x = 0; x < painted.width; x++) if (PixelAt(&painted, x, y) != PixelAt(&toggled, x, y)) {
                changed++;
                actual.left = min(actual.left, x);
                actual.top = min(actual.top, y);
                actual.right = max(actual.right, x + 1);
                actual.bottom = max(actual.bottom, y + 1);
            }
        reference = NativeCheckDifference(list, &native);
        StringCchPrintfA(name, ARRAYSIZE(name), "report glyphs: a check box cleared %s changes the pixels Windows' own drawing changes", kModes[i]);
        Check(name, changed > 0 && reference && EqualRect(&actual, &native));
        if (changed > 0 && reference && !EqualRect(&actual, &native))
            printf("        check box painted=%ld,%ld..%ld,%ld native=%ld,%ld..%ld,%ld\n", actual.left, actual.top, actual.right, actual.bottom,
                   native.left, native.top, native.right, native.bottom);
        StringCchPrintfA(name, ARRAYSIZE(name), "report glyphs: a check change %s keeps the selected and focused item state", kModes[i]);
        Check(name, ListView_GetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED) == (before & (LVIS_SELECTED | LVIS_FOCUSED)));
        CanvasClose(&toggled);
    }
    EnableWindow(list, TRUE);
    ListView_SetImageList(list, icons, LVSIL_SMALL);
    ListView_SetColumnWidth(list, 0, 160);
    column.cx = 120;
    ListView_InsertColumn(list, 1, &column);
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.iItem = 0;
    item.iSubItem = 1;
    item.iImage = 0;
    item.pszText = (LPWSTR)L"Subitem glyph";
    ListView_SetItem(list, &item);
    for (i = 0; i < 2; i++) {
        RECT subitem;
        int subitemGreen = 0;
        BOOL copied;
        DWORD extended = ListView_GetExtendedListViewStyle(list);
        ListView_SetExtendedListViewStyle(list, i ? extended | LVS_EX_SUBITEMIMAGES : extended & ~LVS_EX_SUBITEMIMAGES);
        RedrawWindow(list, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        ListView_GetSubItemRect(list, 0, 1, LVIR_LABEL, &subitem);
        copied = CopyClient(list, &painted);
        for (y = max(0, subitem.top); copied && y < min(subitem.bottom, painted.height); y++)
            for (x = max(0, subitem.left); x < min(subitem.right, painted.width); x++)
                if (PixelAt(&painted, x, y) == RGB(0, 200, 0)) subitemGreen++;
        Check(i ? "report glyphs: SUBITEMIMAGES displays the subitem asset" : "report glyphs: disabling SUBITEMIMAGES hides the subitem asset",
              copied && (i ? subitemGreen > 100 : subitemGreen == 0));
    }
    CanvasClose(&painted);
    ListView_SetImageList(list, NULL, LVSIL_SMALL);
    DestroyWindow(list);
    ImageList_Destroy(icons);
    DeleteObject(font);
    ShowWindow(host, SW_HIDE);
}

/* The list's client and its header, at their places in the host. */
static BOOL CaptureListSurface(HWND host, HWND list, Canvas *canvas)
{
    HWND header = ListView_GetHeader(list);
    RECT client, top, visible, source;
    BOOL copied;
    GetClientRect(list, &client);
    MapWindowPoints(list, host, (POINT *)&client, 2);
    SetRect(&source, 0, 0, client.right - client.left, client.bottom - client.top);
    copied = CopyFromWindow(list, FALSE, &source, canvas, client.left, client.top);
    if (header && GetWindowRect(header, &top)) {
        MapWindowPoints(NULL, host, (POINT *)&top, 2);
        if (IntersectRect(&visible, &top, &client)) {
            SetRect(&source, visible.left - top.left, visible.top - top.top, visible.right - top.left, visible.bottom - top.top);
            copied = CopyFromWindow(header, FALSE, &source, canvas, visible.left, visible.top) && copied;
        }
    }
    return copied;
}

/* Each interior divider of the header, in the host's canvas, stands out
 * from the header there and the column line below it from its row. */
static BOOL ProjectedColumnLines(const Canvas *canvas, HWND host, HWND list, int headerY, int rowY)
{
    HWND header = ListView_GetHeader(list);
    RECT viewport;
    int count = Header_GetItemCount(header), order, last = -1, checked = 0;
    BOOL aligned = TRUE;
    GetClientRect(list, &viewport);
    MapWindowPoints(list, host, (POINT *)&viewport, 2);
    for (order = 0; order < count; order++) {
        RECT item;
        if (Header_GetItemRect(header, Header_OrderToIndex(header, order), &item) && item.right > item.left) last = order;
    }
    for (order = 0; order < last; order++) {
        RECT item;
        int index = Header_OrderToIndex(header, order), x;
        if (!Header_GetItemRect(header, index, &item) || item.right <= item.left) continue;
        MapWindowPoints(header, host, (POINT *)&item, 2);
        x = item.right - 2;
        if (x < viewport.left + 4 || x >= viewport.right - 2) continue;
        checked++;
        if (!StandsOut(PixelAt(canvas, x, headerY), PixelAt(canvas, x - 3, headerY)) || !StandsOut(PixelAt(canvas, x, rowY), PixelAt(canvas, x - 3, rowY))) {
            printf("        projected column=%d x=%d header=%06lx/%06lx body=%06lx/%06lx\n", index, x,
                   PixelAt(canvas, x, headerY), PixelAt(canvas, x - 3, headerY), PixelAt(canvas, x, rowY), PixelAt(canvas, x - 3, rowY));
            aligned = FALSE;
        }
    }
    return checked > 0 && aligned;
}

/* The notifications a native divider drag sends its list, around the new
 * width: HDN_BEGINTRACK, then HDN_ENDTRACK with that width. For widths a
 * drag cannot reach here (a divider outside the header). */
static void ReportDividerDrag(HWND list, int column, int width)
{
    NMHEADERW change = { 0 };
    HDITEMW item = { 0 };
    change.hdr.hwndFrom = ListView_GetHeader(list);
    change.hdr.idFrom = (UINT_PTR)GetDlgCtrlID(change.hdr.hwndFrom);
    change.hdr.code = HDN_BEGINTRACKW;
    change.iItem = column;
    item.mask = HDI_WIDTH;
    item.cxy = width;
    change.pitem = &item;
    SendMessageW(list, WM_NOTIFY, change.hdr.idFrom, (LPARAM)&change);
    Theme_SetColumnWidth(list, column, width);
    change.hdr.code = HDN_ENDTRACKW;
    SendMessageW(list, WM_NOTIFY, change.hdr.idFrom, (LPARAM)&change);
}

/* The header's notifications to its list during a divider drag. */
typedef struct DragWatch { HWND header; int tracks, changes; } DragWatch;

static LRESULT CALLBACK WatchHeaderDrag(HWND list, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    DragWatch *watch = (DragWatch *)reference;
    (void)id;
    if (message == WM_NOTIFY && lp && ((const NMHDR *)lp)->hwndFrom == watch->header) {
        UINT code = ((const NMHDR *)lp)->code;
        if (code == HDN_TRACKW || code == HDN_TRACKA) watch->tracks++;
        if (code == HDN_ITEMCHANGINGW || code == HDN_ITEMCHANGINGA) watch->changes++;
    }
    return DefSubclassProc(list, message, wp, lp);
}

/* A divider dragged `distance` px with the native header's own tracking
 * (the left button held in this thread's keyboard state), released there or,
 * `back`, where it started. TRUE when the column ended at the width the
 * release gives, and grew during the drag when the header changed it then
 * (an item change notified during the move: Windows resizes live where it
 * shows window contents while dragging, else once released). */
static BOOL DragHeaderDivider(HWND list, int column, int distance, BOOL back)
{
    HWND header = ListView_GetHeader(list);
    RECT edge;
    BYTE saved[256], keys[256];
    DragWatch watch;
    int original = ListView_GetColumnWidth(list, column), during, final, y, release, changesDuring;
    BOOL ok, captured;
    if (!Header_GetItemRect(header, column, &edge) || !GetKeyboardState(saved)) return FALSE;
    ZeroMemory(&watch, sizeof watch);
    watch.header = header;
    if (!SetWindowSubclass(list, WatchHeaderDrag, DRAG_WATCH_SUBCLASS, (DWORD_PTR)&watch)) return FALSE;
    CopyMemory(keys, saved, sizeof keys);
    keys[VK_LBUTTON] = 0x80;
    ok = SetKeyboardState(keys);
    y = (edge.top + edge.bottom) / 2;
    release = edge.right - 1 + (back ? 0 : distance);
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(edge.right - 1, y));
    captured = GetCapture() == header;
    SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(edge.right - 1 + distance, y));
    during = ListView_GetColumnWidth(list, column);
    changesDuring = watch.changes;
    SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(release, y));
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(release, y));
    if (GetCapture() == header) ReleaseCapture();
    ok = SetKeyboardState(saved) && ok;
    RemoveWindowSubclass(list, WatchHeaderDrag, DRAG_WATCH_SUBCLASS);
    final = ListView_GetColumnWidth(list, column);
    if (!ok || (changesDuring && during <= original) || final != original + (back ? 0 : distance))
        printf("        divider drag: keyboard state %d, captured %d, full drag %d, tracks %d, changes during the move %d (all %d), width %d, during %d, after %d\n",
               ok, captured, (GetWindowLongW(header, GWL_STYLE) & HDS_FULLDRAG) != 0, watch.tracks, changesDuring, watch.changes, original, during, final);
    return ok && (!changesDuring || during > original) && final == original + (back ? 0 : distance);
}

/* A divider pressed and released with no move between, by the native header. */
static BOOL ClickHeaderDivider(HWND list, int column)
{
    HWND header = ListView_GetHeader(list);
    RECT edge;
    BYTE saved[256], keys[256];
    BOOL ok;
    int y;
    if (!Header_GetItemRect(header, column, &edge) || !GetKeyboardState(saved)) return FALSE;
    CopyMemory(keys, saved, sizeof keys);
    keys[VK_LBUTTON] = 0x80;
    ok = SetKeyboardState(keys);
    y = (edge.top + edge.bottom) / 2;
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(edge.right - 1, y));
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(edge.right - 1, y));
    if (GetCapture() == header) ReleaseCapture();
    return SetKeyboardState(saved) && ok;
}

/* A dark table ends at its last column's right edge in the surface's colors
 * (that boundary still resizes the column), and goes on past what shows when
 * it is wider; its header dividers fall on the rows' column lines; the
 * selected row keeps its fill. In a list and in a view, through column
 * changes, horizontal scrolling and focus. */
static void TestTableEdges(void)
{
    static const char *const kPhases[] = {
        "as made", "after its last column widened and a scroll right", "after it narrowed and a scroll back",
        "after a divider drag there and back", "after its last column emptied", "with the focus", "after the focus left"
    };
    HWND host = ThemedHost();
    int mode;
    if (SkipInLight("table edges")) return;
    for (mode = 0; mode < 2; mode++) {
        HWND list, view;
        HFONT font = DialogFont();
        LVCOLUMNW column = { 0 };
        LVITEMW item = { 0 };
        int phase, i;
        list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               10, 10, 300, 190, host, NULL, GetModuleHandleW(NULL), NULL);
        if (!list) {
            Check("table edges: private list created", FALSE);
            DeleteObject(font);
            continue;
        }
        SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        column.mask = LVCF_WIDTH | LVCF_TEXT;
        column.pszText = (LPWSTR)L"";
        for (i = 0; i < 3; i++) {
            column.cx = i == 2 ? 80 : 110;
            ListView_InsertColumn(list, i, &column);
        }
        item.mask = LVIF_TEXT;
        item.pszText = (LPWSTR)L"";
        for (i = 0; i < 3; i++) {
            item.iItem = i;
            ListView_InsertItem(list, &item);
        }
        ListView_SetItemState(list, 0, LVIS_SELECTED, LVIS_SELECTED);
        view = mode ? Theme_SmoothView(list) : list;
        Theme_Apply(host);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        PumpMessages();
        for (phase = 0; phase < (int)ARRAYSIZE(kPhases); phase++) {
            Canvas canvas = { 0 };
            RECT header, selected, other, client;
            COLORREF headerFill;
            int x, width, headerY, selectedY, otherY, emptyY;
            BOOL blank = TRUE, ready;
            char name[200];
            const char *where = mode ? "in a view" : "alone";
            if (phase == 1) { ReportDividerDrag(list, 2, 500); ListView_Scroll(list, 1, 0); }
            else if (phase == 2) { ReportDividerDrag(list, 2, 60); ListView_Scroll(list, -1, 0); }
            else if (phase == 3) {
                StringCchPrintfA(name, ARRAYSIZE(name), "table edges: a list %s follows a native divider drag", where);
                Check(name, DragHeaderDivider(list, 2, 120, TRUE));
            }
            else if (phase == 4) ReportDividerDrag(list, 2, 0);
            else if (phase == 5 && !TakeFocus(list, "table edges with the focus")) continue;
            else if (phase == 6) SetFocus(host);
            PumpMessages();
            RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            GetWindowRect(ListView_GetHeader(list), &header);
            MapWindowPoints(NULL, host, (POINT *)&header, 2);
            GetClientRect(list, &client);
            width = client.right;
            MapWindowPoints(list, host, (POINT *)&client, 2);
            ListView_GetItemRect(list, 0, &selected, LVIR_BOUNDS);
            ListView_GetItemRect(list, 1, &other, LVIR_BOUNDS);
            headerY = (header.top + header.bottom) / 2;
            selectedY = client.top + (selected.top + selected.bottom) / 2;
            otherY = client.top + (other.top + other.bottom) / 2;
            emptyY = client.bottom - 10;
            ready = CanvasOpen(&canvas, 500, 300, RGB(1, 2, 3)) && CaptureListSurface(host, list, &canvas);
            if (!ready || !HasImage(&canvas, "table edges")) {
                Check("table edges: list surface captured", ready);
                CanvasClose(&canvas);
                break;
            }
            headerFill = PixelAt(&canvas, client.left + 3, headerY);
            if (selected.right > width) {
                /* The table goes on past what shows: its rows go on to the edge, as its content. */
                StringCchPrintfA(name, ARRAYSIZE(name), "table edges: a list %s %s goes on with its selected row to the edge", where, kPhases[phase]);
                CheckColor(name, Theme_Color(THEME_MAIN_BLUE), PixelAt(&canvas, client.right - 1, selectedY));
            } else {
                for (x = client.right - 2; x < client.right; x++)
                    if (PixelAt(&canvas, x, headerY) != headerFill || PixelAt(&canvas, x, selectedY) != Theme_Color(THEME_FIELD) ||
                        PixelAt(&canvas, x, otherY) != Theme_Color(THEME_FIELD) || PixelAt(&canvas, x, emptyY) != Theme_Color(THEME_FIELD)) blank = FALSE;
                StringCchPrintfA(name, ARRAYSIZE(name), "table edges: a list %s %s has no outer right ink in its header, rows or empty area", where, kPhases[phase]);
                Check(name, blank);
                if (!blank) printf("        right edge header=%06lx/%06lx selected=%06lx other=%06lx empty=%06lx\n", PixelAt(&canvas, client.right - 1, headerY),
                                   headerFill, PixelAt(&canvas, client.right - 1, selectedY), PixelAt(&canvas, client.right - 1, otherY), PixelAt(&canvas, client.right - 1, emptyY));
            }
            StringCchPrintfA(name, ARRAYSIZE(name), "table edges: a list %s %s has its header dividers on the rows' column lines", where, kPhases[phase]);
            Check(name, ProjectedColumnLines(&canvas, host, list, headerY, selectedY));
            StringCchPrintfA(name, ARRAYSIZE(name), "table edges: a list %s %s keeps the selected row's fill", where, kPhases[phase]);
            CheckColor(name, Theme_Color(THEME_MAIN_BLUE), PixelAt(&canvas, client.left + (min(selected.right, width) - 2) / 2, selectedY));
            CanvasClose(&canvas);
        }
        ShowWindow(host, SW_HIDE);
        DestroyWindow(view);
        DeleteObject(font);
    }
}

/* Programmatic widths never mark a column; a divider dragged or
 * double-clicked does, and layout then keeps that column's width. */
static void TestColumnOwnership(void)
{
    HWND host = ThemedHost(), list, view, header;
    LVCOLUMNW column = { 0 };
    LVITEMW item = { 0 };
    HDHITTESTINFO hit = { 0 };
    RECT client, edge;
    int i, width;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 240, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("column ownership: private list created", FALSE);
        return;
    }
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.pszText = (LPWSTR)L"";
    column.cx = 60;
    for (i = 0; i < 3; i++) ListView_InsertColumn(list, i, &column);
    view = Theme_SmoothView(list);
    Theme_Apply(host);
    Theme_SetColumnWidth(list, 2, 123);
    Check("column ownership: programmatic sizing never marks the column manual", !Theme_ColumnResizeIsManual(list, 2));
    SetWindowPos(view, NULL, 0, 0, 300, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    GetClientRect(list, &client);
    Check("column ownership: an automatic last column uses the exact remaining viewport", ListView_GetColumnWidth(list, 2) == client.right - 120);
    Check("column ownership: a divider dragged natively and released where it started is followed",
          DragHeaderDivider(list, 0, 30, TRUE));
    Check("column ownership: a divider released where it started sizes nothing", !Theme_ColumnResizeIsManual(list, 0));
    Check("column ownership: a divider clicked without a move is accepted", ClickHeaderDivider(list, 0));
    Check("column ownership: a divider clicked without a move sizes nothing", !Theme_ColumnResizeIsManual(list, 0) && ListView_GetColumnWidth(list, 0) == 60);
    Check("column ownership: a divider dragged natively sizes its column", DragHeaderDivider(list, 1, 30, FALSE));
    Check("column ownership: a native divider drag marks its column sized by the user", Theme_ColumnResizeIsManual(list, 1));
    ReportDividerDrag(list, 2, 420);
    Check("column ownership: tracking notifications with a new width mark the column manual", Theme_ColumnResizeIsManual(list, 2));
    SetWindowPos(view, NULL, 0, 0, 260, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    Check("column ownership: explicit user sizing survives viewport changes",
          ListView_GetColumnWidth(list, 2) == 420 && ListView_GetColumnWidth(list, 1) == 90);
    DestroyWindow(view);

    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 240, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("column ownership: five-column fixture created", FALSE);
        return;
    }
    for (i = 0; i < 5; i++) ListView_InsertColumn(list, i, &column);
    view = Theme_SmoothView(list);
    Theme_Apply(host);
    ReportDividerDrag(list, 4, 420);
    Check("column ownership: a fifth column's tracking notifications are recorded", Theme_ColumnResizeIsManual(list, 4));
    SetWindowPos(view, NULL, 0, 0, 340, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    Check("column ownership: a manual fifth column survives a smooth view resize", ListView_GetColumnWidth(list, 4) == 420);
    DestroyWindow(view);

    /* A double-click on a divider: the list fits the column to its content. */
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 240, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("column ownership: double-click fixture created", FALSE);
        return;
    }
    for (i = 0; i < 3; i++) ListView_InsertColumn(list, i, &column);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"Row";
    ListView_InsertItem(list, &item);
    ListView_SetItemText(list, 0, 1, L"A cell much wider than its column");
    ListView_SetItemText(list, 0, 2, L"Another cell wider than its column");
    view = Theme_SmoothView(list);
    Theme_Apply(host);
    PumpMessages();
    /* The last column narrower than the room left, so its divider is inside the header. */
    Theme_SetColumnWidth(list, 2, 80);
    header = ListView_GetHeader(list);
    for (i = 2; i > 0; i--) {   /* the last first: fitting the middle one pushes it past the header */
        char name[160];
        int before = ListView_GetColumnWidth(list, i), after, fitted;
        Header_GetItemRect(header, i, &edge);
        hit.pt.x = edge.right - 1;
        hit.pt.y = (edge.top + edge.bottom) / 2;
        StringCchPrintfA(name, ARRAYSIZE(name), "column ownership: column %d's divider is where the header hit-tests one", i);
        Check(name, (int)SendMessageW(header, HDM_HITTEST, 0, (LPARAM)&hit) == i && (hit.flags & HHT_ONDIVIDER) != 0);
        SendMessageW(header, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(hit.pt.x, hit.pt.y));
        SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(hit.pt.x, hit.pt.y));
        if (GetCapture() == header) ReleaseCapture();
        PumpMessages();
        after = ListView_GetColumnWidth(list, i);
        /* Windows' own fit of the column to its content, asked once more: it changes nothing. */
        ListView_SetColumnWidth(list, i, LVSCW_AUTOSIZE);
        fitted = ListView_GetColumnWidth(list, i);
        StringCchPrintfA(name, ARRAYSIZE(name), "column ownership: a double-click on column %d's divider fits it to its content as Windows does", i);
        Check(name, after != before && after == fitted);
        if (after == before || after != fitted) printf("        width before %d, after %d, Windows' fit %d\n", before, after, fitted);
        StringCchPrintfA(name, ARRAYSIZE(name), "column ownership: a double-click on column %d's divider marks it sized by the user", i);
        Check(name, Theme_ColumnResizeIsManual(list, i));
    }
    width = ListView_GetColumnWidth(list, 2);
    SetWindowPos(view, NULL, 0, 0, 380, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    Check("column ownership: a last column fitted by a double-click keeps its width in a wider view", ListView_GetColumnWidth(list, 2) == width);
    DestroyWindow(view);
}

static void TestTableComponentStyles(void)
{
    HWND host = ThemedHost();
    HWND owned = CreateWindowExW(WS_EX_NOPARENTNOTIFY, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 10, 10, 200, 100,
                                 host, NULL, GetModuleHandleW(NULL), NULL);
    HWND caller = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_COMPOSITED, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 220, 10, 200, 100,
                                  host, NULL, GetModuleHandleW(NULL), NULL);
    if (!owned || !caller) {
        Check("table component: private style fixtures created", FALSE);
        if (owned) DestroyWindow(owned);
        if (caller) DestroyWindow(caller);
        return;
    }
    Theme_Apply(host);
    /* UpdateWindow does not paint a composed window (WS_EX_COMPOSITED), only
     * the queue does: the table would lag behind a resize. */
    Check("table component: a report table is not composed", !(GetWindowLongW(owned, GWL_EXSTYLE) & WS_EX_COMPOSITED));
    Check("table component: unrelated caller extended style is preserved", (GetWindowLongW(owned, GWL_EXSTYLE) & WS_EX_NOPARENTNOTIFY) != 0);
    Check("table component: caller composition is preserved", (GetWindowLongW(caller, GWL_EXSTYLE) & WS_EX_COMPOSITED) != 0);
    SetWindowLongW(caller, GWL_STYLE, (GetWindowLongW(caller, GWL_STYLE) & ~LVS_TYPEMASK) | LVS_ICON);
    Theme_Apply(host);
    Check("table component: non-report mode keeps caller composition", (GetWindowLongW(caller, GWL_EXSTYLE) & WS_EX_COMPOSITED) != 0);
    DestroyWindow(owned);
    DestroyWindow(caller);
}

typedef struct TableDestroyFixture {
    UINT message;
    BOOL fired;
} TableDestroyFixture;

static LRESULT CALLBACK DestroyDuringTableCall(HWND list, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    TableDestroyFixture *fixture = (TableDestroyFixture *)reference;
    (void)id;
    if (fixture->message && message == fixture->message) {
        fixture->message = 0;
        fixture->fired = TRUE;
        DestroyWindow(list);
        return 0;
    }
    return DefSubclassProc(list, message, wp, lp);
}

static void TestTableReentrantDestroy(void)
{
    static const struct { UINT message; const char *name; } kMessages[] = {
        { LVM_SETCOLUMNWIDTH, "a column width change" }, { WM_SETREDRAW, "a redraw pause" }
    };
    HWND host = ThemedHost();
    size_t i;
    for (i = 0; i < ARRAYSIZE(kMessages); i++) {
        TableDestroyFixture fixture = { 0 };
        LVCOLUMNW column = { 0 };
        HWND header, list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 10, 10, 240, 120,
                                            host, NULL, GetModuleHandleW(NULL), NULL);
        char name[160];
        if (!list) {
            Check("table lifetime: private list created", FALSE);
            continue;
        }
        column.mask = LVCF_WIDTH;
        column.cx = 100;
        ListView_InsertColumn(list, 0, &column);
        header = ListView_GetHeader(list);
        if (!SetWindowSubclass(list, DestroyDuringTableCall, TABLE_DESTROY_SUBCLASS, (DWORD_PTR)&fixture)) {
            Check("table lifetime: private destruction callback installed", FALSE);
            DestroyWindow(list);
            continue;
        }
        Theme_Apply(host);
        fixture.message = kMessages[i].message;
        SendMessageW(list, kMessages[i].message, 0, kMessages[i].message == LVM_SETCOLUMNWIDTH ? 200 : 0);
        StringCchPrintfA(name, ARRAYSIZE(name), "table lifetime: the list and its header can be destroyed during %s", kMessages[i].name);
        Check(name, fixture.fired && !IsWindow(list) && (!header || !IsWindow(header)));
        if (IsWindow(list)) DestroyWindow(list);
    }
}

/* ----------------------------------------------- horizontal scroll frames */

/* Windows scrolls a table and shifts its columns by copying its pixels.
 * What the theme draws belongs to the content, so once painted the table
 * looks as drawn whole. Empty captions isolate borders from text rendering. */
#define SCROLL_DRAG_VERIFY (WM_APP + 0x51A)

typedef struct ScrollTrace {
    HWND list, view;
    RECT visible;                       /* what shows of the table, in its client coordinates */
    POINT drag;
    int tracks, next, forward, backward, lastPosition;
    BOOL dragging, aborted;
} ScrollTrace;

static ScrollTrace *g_scrollDrag;

/* What shows of the table (its rows and its header), from the table's own
 * pixels: what Windows composes next, whatever covers the window. */
static BOOL CaptureVisibleTable(const ScrollTrace *trace, Canvas *canvas)
{
    HWND header = ListView_GetHeader(trace->list);
    RECT top, visible, source;
    BOOL copied;
    if (!CanvasOpen(canvas, trace->visible.right - trace->visible.left, trace->visible.bottom - trace->visible.top, RGB(1, 2, 3))) return FALSE;
    copied = CopyFromWindow(trace->list, FALSE, &trace->visible, canvas, 0, 0);
    if (header && (GetWindowLongW(header, GWL_STYLE) & WS_VISIBLE) && GetWindowRect(header, &top)) {
        MapWindowPoints(NULL, trace->list, (POINT *)&top, 2);
        if (IntersectRect(&visible, &top, &trace->visible)) {
            SetRect(&source, visible.left - top.left, visible.top - top.top, visible.right - top.left, visible.bottom - top.top);
            copied = CopyFromWindow(header, FALSE, &source, canvas, visible.left - trace->visible.left, visible.top - trace->visible.top) && copied;
        }
    }
    if (!copied) CanvasClose(canvas);
    return copied;
}

static BOOL SameCanvas(const Canvas *a, const Canvas *b)
{
    return a->width == b->width && a->height == b->height && memcmp(a->pixels, b->pixels, (size_t)a->width * a->height * sizeof *a->pixels) == 0;
}

static LRESULT CALLBACK ScrollTraceProc(HWND list, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    ScrollTrace *trace = (ScrollTrace *)reference;
    LRESULT result;
    (void)id;
    if (message == SCROLL_DRAG_VERIFY) {
        if (trace->dragging && trace->tracks == (int)wp) {
            trace->aborted = TRUE;
            trace->dragging = FALSE;
            PostMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(trace->drag.x, trace->drag.y));
        }
        return 0;
    }
    result = DefSubclassProc(list, message, wp, lp);
    if (message == WM_HSCROLL && LOWORD(wp) == SB_THUMBTRACK) {
        int position = GetScrollPos(list, SB_HORZ);
        if (position > trace->lastPosition) trace->forward++;
        if (position < trace->lastPosition) trace->backward++;
        trace->lastPosition = position;
        trace->tracks++;
        if (trace->dragging) {
            int offset = trace->next < 4 ? (trace->next + 1) * 20 : (7 - trace->next) * 20;
            if (trace->next < 8) {
                PostMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(trace->drag.x + offset, trace->drag.y));
                trace->next++;
            } else {
                PostMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(trace->drag.x, trace->drag.y));
                trace->dragging = FALSE;
            }
        }
    }
    return result;
}

/* Only this thread's queued fixture messages and keyboard state participate;
 * neither the system cursor nor another application's input is changed. */
static LRESULT CALLBACK ScrollQueuedMouse(int code, WPARAM mode, LPARAM value)
{
    MSG *message = (MSG *)value;
    ScrollTrace *trace = g_scrollDrag;
    if (code >= 0 && mode == PM_REMOVE && trace && message->hwnd == trace->list &&
        (message->message == WM_MOUSEMOVE || message->message == WM_LBUTTONUP)) {
        BYTE keys[256];
        if (GetKeyboardState(keys)) {
            keys[VK_LBUTTON] = message->message == WM_MOUSEMOVE ? 0x80 : 0;
            SetKeyboardState(keys);
        }
        message->pt.x = (short)LOWORD(message->lParam);
        message->pt.y = (short)HIWORD(message->lParam);
        ClientToScreen(trace->list, &message->pt);
        if (message->message == WM_MOUSEMOVE && trace->dragging)
            PostMessageW(trace->list, SCROLL_DRAG_VERIFY, (WPARAM)trace->tracks, 0);
    }
    return CallNextHookEx(NULL, code, mode, value);
}

/* The horizontal scroll bar's thumb dragged right and back by the native
 * scroll bar's own tracking loop. */
static BOOL ScrollNativeDrag(ScrollTrace *trace)
{
    SCROLLBARINFO bar = { sizeof bar };
    BYTE saved[256], keys[256];
    HHOOK hook;
    POINT click;
    BOOL ok;
    if (!GetScrollBarInfo(trace->list, OBJID_HSCROLL, &bar) || (bar.rgstate[0] & STATE_SYSTEM_INVISIBLE) ||
        bar.xyThumbBottom <= bar.xyThumbTop || !GetKeyboardState(saved)) return FALSE;
    hook = SetWindowsHookExW(WH_GETMESSAGE, ScrollQueuedMouse, NULL, GetCurrentThreadId());
    if (!hook) return FALSE;
    click.x = bar.rcScrollBar.left + (bar.xyThumbTop + bar.xyThumbBottom) / 2;
    click.y = (bar.rcScrollBar.top + bar.rcScrollBar.bottom) / 2;
    trace->drag = click;
    ScreenToClient(trace->list, &trace->drag);
    trace->dragging = TRUE;
    trace->next = 0;
    trace->aborted = FALSE;
    g_scrollDrag = trace;
    CopyMemory(keys, saved, sizeof keys);
    keys[VK_LBUTTON] = 0x80;
    ok = SetKeyboardState(keys) && PostMessageW(trace->list, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(trace->drag.x + 20, trace->drag.y));
    if (ok) SendMessageW(trace->list, WM_NCLBUTTONDOWN, HTHSCROLL, MAKELPARAM(click.x, click.y));
    if (GetCapture() == trace->list) ReleaseCapture();
    ok = UnhookWindowsHookEx(hook) && ok;
    ok = SetKeyboardState(saved) && ok;
    g_scrollDrag = NULL;
    trace->dragging = FALSE;
    return ok && !trace->aborted;
}

/* Once painted, the table looks as drawn whole: drawing everything again
 * changes nothing. */
static void ScrollSettled(ScrollTrace *trace, const char *name)
{
    Canvas before = { 0 }, after = { 0 };
    BOOL captured;
    PumpMessages();
    captured = CaptureVisibleTable(trace, &before);
    RedrawWindow(trace->view, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    captured = CaptureVisibleTable(trace, &after) && captured;
    Check(name, captured && SameCanvas(&before, &after));
    if (captured && !SameCanvas(&before, &after)) {
        RECT changed;
        int x, y;
        SetRect(&changed, after.width, after.height, 0, 0);
        for (y = 0; y < after.height; y++) for (x = 0; x < after.width; x++) if (PixelAt(&before, x, y) != PixelAt(&after, x, y)) {
            changed.left = min(changed.left, x);
            changed.top = min(changed.top, y);
            changed.right = max(changed.right, x + 1);
            changed.bottom = max(changed.bottom, y + 1);
        }
        printf("        drawn again: %ld,%ld..%ld,%ld changed (position %d)\n", changed.left, changed.top, changed.right, changed.bottom,
               GetScrollPos(trace->list, SB_HORZ));
    }
    CanvasClose(&before);
    CanvasClose(&after);
}

/* A column mutation of the table, done one way then undone; TRUE when the
 * list accepted both. */
static BOOL MutateColumns(HWND list, int mutation, int fittedWidth)
{
    HWND header = ListView_GetHeader(list);
    int reordered[] = { 2, 0, 1 }, normal[] = { 0, 1, 2 };
    LVCOLUMNW wide = { 0 };
    LVCOLUMNA narrow = { 0 };
    HDITEMW wideItem = { 0 };
    HDITEMA narrowItem = { 0 };
    BOOL accepted = FALSE;
    wide.mask = LVCF_WIDTH;
    wide.cx = 160;
    narrow.mask = LVCF_WIDTH;
    narrow.cx = 160;
    wideItem.mask = HDI_WIDTH;
    wideItem.cxy = 180;
    narrowItem.mask = HDI_WIDTH;
    narrowItem.cxy = 180;
    switch (mutation) {
    case 0:
        accepted = ListView_SetColumnWidth(list, 0, 160);
        accepted = ListView_SetColumnWidth(list, 0, 100) && accepted;
        break;
    case 1:
        accepted = (BOOL)SendMessageW(list, LVM_SETCOLUMNW, 0, (LPARAM)&wide);
        wide.cx = 100;
        accepted = (BOOL)SendMessageW(list, LVM_SETCOLUMNW, 0, (LPARAM)&wide) && accepted;
        break;
    case 2:
        accepted = (BOOL)SendMessageW(list, LVM_SETCOLUMNA, 0, (LPARAM)&narrow);
        narrow.cx = 100;
        accepted = (BOOL)SendMessageW(list, LVM_SETCOLUMNA, 0, (LPARAM)&narrow) && accepted;
        break;
    case 3:
        wide.cx = 80;
        accepted = ListView_InsertColumn(list, 0, &wide) == 0;
        accepted = ListView_DeleteColumn(list, 0) && accepted;
        break;
    case 4:
        accepted = ListView_SetColumnOrderArray(list, 3, reordered);
        accepted = ListView_SetColumnOrderArray(list, 3, normal) && accepted;
        break;
    case 5:
        accepted = Header_SetItem(header, 1, &wideItem);
        wideItem.cxy = 100;
        accepted = Header_SetItem(header, 1, &wideItem) && accepted;
        break;
    case 6:
        accepted = (BOOL)SendMessageW(header, HDM_SETITEMA, 1, (LPARAM)&narrowItem);
        narrowItem.cxy = 100;
        accepted = (BOOL)SendMessageW(header, HDM_SETITEMA, 1, (LPARAM)&narrowItem) && accepted;
        break;
    case 7:
        accepted = Header_SetOrderArray(header, 3, reordered);
        accepted = Header_SetOrderArray(header, 3, normal) && accepted;
        break;
    case 8:
        accepted = DragHeaderDivider(list, 0, 40, TRUE);
        break;
    case 9: {
        /* The last column fitted to the view: its divider is still the user's. */
        RECT edge = { 0, 0, 0, 0 };
        HDHITTESTINFO hit = { 0 };
        accepted = ListView_SetColumnWidth(list, 2, fittedWidth) && Header_GetItemRect(header, 2, &edge);
        hit.pt.x = edge.right - 1;
        hit.pt.y = (edge.top + edge.bottom) / 2;
        accepted = accepted && (int)SendMessageW(header, HDM_HITTEST, 0, (LPARAM)&hit) == 2 && (hit.flags & HHT_ONDIVIDER) != 0;
        accepted = DragHeaderDivider(list, 2, 40, TRUE) && accepted;
        accepted = ListView_SetColumnWidth(list, 2, 900) && accepted;
        break;
    }
    }
    return accepted;
}

/* A table in its view, wider than the view (its last column as wide as a
 * user made it): after a thumb drag, column changes and programmatic
 * scrolls, once painted, it looks as drawn whole. */
static void TestHorizontalScrollFrames(void)
{
    static const char *const kMutations[] = {
        "a column width set", "a column set (W)", "a column set (A)", "a column inserted and deleted", "a column order set",
        "a header item set (W)", "a header item set (A)", "a header order set", "a divider dragged", "the fitted last column's divider dragged"
    };
    static const char *const kFocus[] = { "with the focus on the list", "with another row selected", "without the focus" };
    HWND host = ThemedHost(), list, view;
    HFONT font;
    LVCOLUMNW column = { 0 };
    LVITEMW item = { 0 };
    ScrollTrace trace = { 0 };
    RECT viewport;
    Canvas first = { 0 };
    int i, phase;
    BOOL imaged;
    char name[200];
    if (SkipInLight("horizontal scroll frames")) return;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                           10, 10, 300, 190, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("horizontal scroll: private list created", FALSE);
        return;
    }
    font = DialogFont();
    SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = (LPWSTR)L"";
    column.cx = 100;
    for (i = 0; i < 3; i++) ListView_InsertColumn(list, i, &column);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"";
    for (i = 0; i < 2; i++) {
        item.iItem = i;
        ListView_InsertItem(list, &item);
    }
    ListView_SetItemState(list, 0, LVIS_SELECTED, LVIS_SELECTED);
    view = Theme_SmoothView(list);
    if (!view || view == list) {
        Check("horizontal scroll: private smooth view created", FALSE);
        DestroyWindow(list);
        DeleteObject(font);
        return;
    }
    trace.list = list;
    trace.view = view;
    /* Drives the thumb drag (ScrollNativeDrag) and counts its steps. */
    if (!SetWindowSubclass(list, ScrollTraceProc, SCROLL_TRACE_SUBCLASS, (DWORD_PTR)&trace)) {
        Check("horizontal scroll: drag trace installed", FALSE);
        DestroyWindow(view);
        DeleteObject(font);
        return;
    }
    Theme_Apply(host);
    ReportDividerDrag(list, 2, 900);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    PumpMessages();
    GetClientRect(list, &trace.visible);
    GetClientRect(view, &viewport);
    MapWindowPoints(view, list, (POINT *)&viewport, 2);
    IntersectRect(&trace.visible, &trace.visible, &viewport);
    RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    imaged = CaptureVisibleTable(&trace, &first) && HasImage(&first, "horizontal scroll frames");
    CanvasClose(&first);
    if (imaged) {
        for (phase = 0; phase < (int)ARRAYSIZE(kFocus); phase++) {
            int oldTracks = trace.tracks, oldForward = trace.forward, oldBackward = trace.backward;
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(list, phase == 1 ? 1 : 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            if (phase == 0 && !TakeFocus(list, "horizontal scroll with the focus on the list")) continue;
            if (phase == 2) SetFocus(host);
            RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            StringCchPrintfA(name, ARRAYSIZE(name), "horizontal scroll: a table %s completes a native thumb drag", kFocus[phase]);
            Check(name, ScrollNativeDrag(&trace));
            StringCchPrintfA(name, ARRAYSIZE(name), "horizontal scroll: a table %s tracks both directions", kFocus[phase]);
            Check(name, trace.tracks - oldTracks >= 5 && trace.forward > oldForward && trace.backward > oldBackward);
            StringCchPrintfA(name, ARRAYSIZE(name), "horizontal scroll: a table %s looks as drawn whole once painted", kFocus[phase]);
            ScrollSettled(&trace, name);
        }
        for (phase = 0; phase < (int)ARRAYSIZE(kMutations); phase++) {
            StringCchPrintfA(name, ARRAYSIZE(name), "table geometry: a table accepts %s", kMutations[phase]);
            Check(name, MutateColumns(list, phase, 100));
            StringCchPrintfA(name, ARRAYSIZE(name), "table geometry: after %s, a table looks as drawn whole once painted", kMutations[phase]);
            ScrollSettled(&trace, name);
        }
        ListView_Scroll(list, 100, 0);
        ScrollSettled(&trace, "horizontal scroll: after a programmatic scroll, a table looks as drawn whole once painted");
        ListView_Scroll(list, -100, 0);
        ScrollSettled(&trace, "horizontal scroll: after a programmatic scroll back, a table looks as drawn whole once painted");
    }
    RemoveWindowSubclass(list, ScrollTraceProc, SCROLL_TRACE_SUBCLASS);
    ShowWindow(host, SW_HIDE);
    DestroyWindow(view);
    DeleteObject(font);
}

/* ------------------------------------------------------------ check boxes */

static void TestCheckBoxMetrics(void)
{
    static const struct { const WCHAR *caption, *language; const char *script; } kScripts[] = {
        { L"&Archived", L"en", "Latin" },
        { L"\x5DF2\x5F52\x6863", L"zh-CN", "Chinese" },
        { L"\x0627\x0644\x0645\x0624\x0631\x0634\x0641\x0629", L"ar", "Arabic" },
        { L"\x0938\x0902\x0917\x094D\x0930\x0939\x093F\x0924", L"hi", "Devanagari" },
        { L"\x0986\x09B0\x09CD\x0995\x09BE\x0987\x09AD", L"bn", "Bengali" },
    };
    static const int kScales[] = { 96, 144, 192 };
    static const char *const kStates[] = { "unchecked", "checked", "disabled", "disabled and checked" };
    HWND host = ThemedHost(), box = CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                    10, 10, 300, 60, host, NULL, GetModuleHandleW(NULL), NULL);
    HFONT base = DialogFont();
    LOGFONTW original;
    SIZE size;
    size_t script, scale;
    BOOL imaged = TRUE;
    Check("checkbox metrics: a missing control or size is rejected", !Theme_CheckBoxSize(NULL, &size) && !Theme_CheckBoxSize(box, NULL));
    Check("checkbox metrics: a window that is no check box is rejected", !Theme_CheckBoxSize(host, &size));
    if (!box || !GetObjectW(base, sizeof original, &original)) {
        Check("checkbox metrics: private fixture created", FALSE);
        if (box) DestroyWindow(box);
        DeleteObject(base);
        return;
    }
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    for (script = 0; imaged && script < ARRAYSIZE(kScripts); script++) for (scale = 0; imaged && scale < ARRAYSIZE(kScales); scale++) {
        LOGFONTW font = original;
        HFONT made;
        int state;
        font.lfHeight = MulDiv(original.lfHeight, kScales[scale], 96);
        StringCchCopyW(font.lfFaceName, ARRAYSIZE(font.lfFaceName), Localize_FontFaceAt(Localize_LanguageForCode(kScripts[script].language)));
        made = CreateFontIndirectW(&font);
        if (!made) {
            Check("checkbox metrics: script font created", FALSE);
            continue;
        }
        SendMessageW(box, WM_SETFONT, (WPARAM)made, FALSE);
        SetWindowTextW(box, kScripts[script].caption);
        for (state = 0; imaged && state < (int)ARRAYSIZE(kStates); state++) {
            SIZE ideal = { 0 };
            Canvas compact = { 0 }, wide = { 0 }, blank = { 0 };
            BOOL captured, same = TRUE;
            RECT area;
            int x, y, firstDifference = -1, lastInk = -1;
            char name[200];
            EnableWindow(box, state < 2);
            SendMessageW(box, BM_SETCHECK, state & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
            captured = Theme_CheckBoxSize(box, &ideal) && ideal.cx > 0 && ideal.cy > 0 &&
                       CanvasOpen(&compact, ideal.cx, ideal.cy, RGB(1, 2, 3)) && CanvasOpen(&wide, ideal.cx + 40, ideal.cy, RGB(1, 2, 3)) &&
                       CanvasOpen(&blank, ideal.cx + 40, ideal.cy, RGB(1, 2, 3));
            if (captured) {
                SetWindowPos(box, NULL, 0, 0, ideal.cx, ideal.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                RedrawWindow(box, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                SetRect(&area, 0, 0, ideal.cx, ideal.cy);
                captured = CopyFromWindow(box, FALSE, &area, &compact, 0, 0);
                SetWindowPos(box, NULL, 0, 0, ideal.cx + 40, ideal.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                RedrawWindow(box, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                area.right = wide.width;
                captured = CopyFromWindow(box, FALSE, &area, &wide, 0, 0) && captured;
                /* The same box without its caption: what lies past the caption. */
                SetWindowTextW(box, L"");
                RedrawWindow(box, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                captured = CopyFromWindow(box, FALSE, &area, &blank, 0, 0) && captured;
                SetWindowTextW(box, kScripts[script].caption);
                if (captured && !HasImage(&wide, "checkbox metrics")) imaged = captured = FALSE;
                for (y = 0; captured && y < ideal.cy; y++) for (x = 0; x < ideal.cx; x++)
                    if (PixelAt(&compact, x, y) != PixelAt(&wide, x, y)) { same = FALSE; if (firstDifference < 0) firstDifference = x; }
                /* A native check box clips its caption without an ellipsis:
                 * ink past the measured width is what would be cut. */
                for (y = 0; captured && y < ideal.cy; y++) for (x = ideal.cx; x < wide.width; x++)
                    if (PixelAt(&wide, x, y) != PixelAt(&blank, x, y)) { same = FALSE; lastInk = max(lastInk, x); }
            }
            if (imaged) {
                StringCchPrintfA(name, ARRAYSIZE(name), "checkbox metrics: a %s caption at font scale %d, %s, fits its measured size",
                                 kScripts[script].script, kScales[scale], kStates[state]);
                Check(name, captured && same);
                if (captured && !same) printf("        checkbox ideal=%ld/%ld first-difference-x=%d last-outside-ink-x=%d\n", ideal.cx, ideal.cy, firstDifference, lastInk);
            }
            CanvasClose(&compact);
            CanvasClose(&wide);
            CanvasClose(&blank);
        }
        SendMessageW(box, WM_SETFONT, (WPARAM)base, FALSE);
        DeleteObject(made);
    }
    DestroyWindow(box);
    DeleteObject(base);
    ShowWindow(host, SW_HIDE);
}

/* -------------------------------------------------------------- tooltips */

static HFONT g_tipHeading;
static int g_tipRequests, g_tipReserved;

/* The tree's parent, as sessions.c is: details for each session's info tip;
 * a heading (a negative item value) in its own font; asked to measure a row
 * (an item prepaint with an empty rectangle), the row's font selected and
 * its title's bounds (the row less what the parent draws after the title,
 * g_tipReserved), with CDRF_NEWFONT. */
static LRESULT CALLBACK ProbeTreeTip(HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    HWND tree = (HWND)reference;
    (void)id;
    if (message == WM_NOTIFY && lp && ((const NMHDR *)lp)->hwndFrom == tree && ((const NMHDR *)lp)->code == TVN_GETINFOTIPW) {
        NMTVGETINFOTIPW *info = (NMTVGETINFOTIPW *)lp;
        g_tipRequests++;
        if (info->lParam >= 0) StringCchPrintfW(info->pszText, (size_t)info->cchTextMax, L"Session %Id: full details", info->lParam);
        return 0;
    }
    if (message == WM_NOTIFY && lp && ((const NMHDR *)lp)->hwndFrom == tree && ((const NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        NMTVCUSTOMDRAW *draw = (NMTVCUSTOMDRAW *)lp;
        HFONT font = draw->nmcd.lItemlParam < 0 && g_tipHeading ? g_tipHeading : (HFONT)SendMessageW(tree, WM_GETFONT, 0, 0);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && IsRectEmpty(&draw->nmcd.rc)) {
            SelectObject(draw->nmcd.hdc, font);
            GetClientRect(tree, &draw->nmcd.rc);
            draw->nmcd.rc.right -= g_tipReserved;
            return CDRF_NEWFONT;
        }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && draw->nmcd.lItemlParam < 0 && g_tipHeading) {
            SelectObject(draw->nmcd.hdc, g_tipHeading);
            return CDRF_NEWFONT;
        }
    }
    return DefSubclassProc(host, message, wp, lp);
}

static void HoverCell(HWND control, const RECT *cell)
{
    SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(cell->left + 12, (cell->top + cell->bottom) / 2));
}

/* The pointer at rest where HoverCell put it (the hover time elapsed). */
static void RestOnCell(HWND control, const RECT *cell)
{
    SendMessageW(control, WM_MOUSEHOVER, 0, MAKELPARAM(cell->left + 12, (cell->top + cell->bottom) / 2));
}

typedef struct TipProbe { int shown, hidden; } TipProbe;

static LRESULT CALLBACK ProbeTipWindow(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    TipProbe *probe = (TipProbe *)reference;
    (void)id;
    if (message == WM_SHOWWINDOW) {
        if (wp) probe->shown++;
        else probe->hidden++;
    }
    return DefSubclassProc(window, message, wp, lp);
}

static BOOL TipVisible(const void *tips)
{
    return IsWindowVisible((HWND)tips);
}

/* A tooltip that left stays hidden while the pointer stays still: watched
 * for as long as a native tooltip could take to show again (the hover time,
 * or a tooltip's initial delay, the double-click time). */
static BOOL DepartedTipStaysHidden(HWND tips)
{
    UINT hover = 400;
    SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hover, 0);
    return !IsWindowVisible(tips) && !PumpUntil(TipVisible, tips, max(hover, GetDoubleClickTime()) + 150);
}

static BOOL TipReads(HWND tips, const WCHAR *expected)
{
    WCHAR text[2048];
    GetWindowTextW(tips, text, ARRAYSIZE(text));
    return wcscmp(text, expected) == 0;
}

static void TestCellTips(void)
{
    static const struct { const WCHAR *title, *language; const char *script; } kTitles[] = {
        { L"\x4F1A\x8BDD", L"zh-CN", "Chinese" }, { L"\x062C\x0644\x0633\x0629", L"ar", "Arabic" }, { L"Session", L"en", "Latin" },
        { L"\x0938\x0924\x094D\x0930", L"hi", "Devanagari" }, { L"\x09B8\x09C7\x09B6\x09A8", L"bn", "Bengali" }
    };
    static const struct { const WCHAR *language; BOOL rightToLeft; } kDirections[] = { { L"fr", FALSE }, { L"ar", TRUE }, { L"fr", FALSE } };
    HWND host = ThemedHost(), list, tree, tips, treeTips;
    HFONT font = DialogFont();
    LVCOLUMNW column = { 0 };
    LVITEMW item = { 0 };
    TVINSERTSTRUCTW insert = { 0 };
    RECT cell, shortCell, treeCell, nextCell, plainCell;
    HTREEITEM node, next, plain;
    TipProbe tipProbe = { 0 };
    int i;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 300, 140, host, NULL, GetModuleHandleW(NULL), NULL);
    tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_FULLROWSELECT | TVS_INFOTIP, 10, 160, 300, 110,
                           host, NULL, GetModuleHandleW(NULL), NULL);
    Check("cell tips: private list and tree created", list && tree);
    if (!list || !tree) {
        if (list) DestroyWindow(list);
        if (tree) DestroyWindow(tree);
        DeleteObject(font);
        return;
    }
    SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(tree, WM_SETFONT, (WPARAM)font, FALSE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_LABELTIP | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.pszText = (LPWSTR)L"";
    column.cx = 100;
    ListView_InsertColumn(list, 0, &column);
    ListView_InsertColumn(list, 1, &column);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"A long profile name that exceeds its cell";
    for (i = 0; i < 2; i++) {
        item.iItem = i;
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, i, 1, L"Role");
    }
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (LPWSTR)L"Session";
    insert.item.lParam = 17;
    node = TreeView_InsertItem(tree, &insert);
    insert.item.pszText = (LPWSTR)L"Next session";
    insert.item.lParam = 18;
    next = TreeView_InsertItem(tree, &insert);
    /* A row whose parent gives no info tip (a negative value). */
    insert.item.pszText = (LPWSTR)L"Plain";
    insert.item.lParam = -2;
    plain = TreeView_InsertItem(tree, &insert);
    SetWindowSubclass(host, ProbeTreeTip, TREE_TIP_SUBCLASS, (DWORD_PTR)tree);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    tips = ListView_GetToolTips(list);
    treeTips = TreeView_GetToolTips(tree);
    SetWindowSubclass(tips, ProbeTipWindow, TIP_WINDOW_SUBCLASS, (DWORD_PTR)&tipProbe);
    Check("cell tips: list and tree use zero initial and reshow delay",
          SendMessageW(tips, TTM_GETDELAYTIME, TTDT_INITIAL, 0) == 0 && SendMessageW(tips, TTM_GETDELAYTIME, TTDT_RESHOW, 0) == 0 &&
          SendMessageW(treeTips, TTM_GETDELAYTIME, TTDT_INITIAL, 0) == 0 && SendMessageW(treeTips, TTM_GETDELAYTIME, TTDT_RESHOW, 0) == 0);
    ListView_GetSubItemRect(list, 0, 0, LVIR_LABEL, &cell);
    ListView_GetSubItemRect(list, 0, 1, LVIR_BOUNDS, &shortCell);
    HoverCell(list, &cell);
    Check("cell tips: an unselected clipped label appears during its mouse event", IsWindowVisible(tips));
    Check("cell tips: the visible tooltip contains the full cell text", TipReads(tips, item.pszText));
    {
        TOOLINFOW current = { 0 };
        int shown = tipProbe.shown, hidden = tipProbe.hidden;
        current.cbSize = sizeof current;
        HoverCell(list, &cell);
        Check("cell tips: an unselected cell keeps the explicit tracking tool without reactivation",
              IsWindowVisible(tips) && tipProbe.shown == shown && tipProbe.hidden == hidden &&
              SendMessageW(tips, TTM_GETCURRENTTOOLW, 0, (LPARAM)&current) && (current.uFlags & TTF_TRACK));
    }
    HoverCell(list, &shortCell);
    Check("cell tips: leaving for a fitting cell hides immediately", !IsWindowVisible(tips));
    Check("cell tips: a departed cell stays hidden while the pointer remains still", DepartedTipStaysHidden(tips));
    ListView_SetItemState(list, 0, LVIS_SELECTED, LVIS_SELECTED);
    HoverCell(list, &cell);
    Check("cell tips: a selected clipped label has the same immediate tooltip", IsWindowVisible(tips));
    {
        int shown = tipProbe.shown, hidden = tipProbe.hidden;
        TOOLINFOW current = { 0 };
        current.cbSize = sizeof current;
        HoverCell(list, &cell);
        SendMessageW(list, WM_MOUSEMOVE, 0, MAKELPARAM(cell.left + 15, (cell.top + cell.bottom) / 2));
        Check("cell tips: moves within the same selected cell keep a stable tracking popup",
              IsWindowVisible(tips) && tipProbe.shown == shown && tipProbe.hidden == hidden &&
              SendMessageW(tips, TTM_GETCURRENTTOOLW, 0, (LPARAM)&current) && (current.uFlags & TTF_TRACK));
        RedrawWindow(list, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        Check("cell tips: repainting the same cell keeps its tooltip visible", IsWindowVisible(tips));
    }
    ListView_SetItemText(list, 0, 0, (LPWSTR)item.pszText);
    Check("cell tips: a change of the list's content hides its tip", !IsWindowVisible(tips));
    HoverCell(list, &cell);
    Check("cell tips: the changed cell's tip shows again on the next move", IsWindowVisible(tips));
    SendMessageW(list, WM_MOUSELEAVE, 0, 0);
    Check("cell tips: leaving the list hides immediately", !IsWindowVisible(tips));
    HoverCell(list, &cell);
    ListView_SetColumnWidth(list, 0, 120);
    Check("cell tips: column resizing hides stale tooltip geometry", !IsWindowVisible(tips));
    /* Wider than the list: it scrolls sideways. */
    ListView_SetColumnWidth(list, 1, 400);
    HoverCell(list, &cell);
    SendMessageW(list, WM_HSCROLL, SB_LINERIGHT, 0);
    Check("cell tips: scrolling hides the displayed cell", GetScrollPos(list, SB_HORZ) > 0 && !IsWindowVisible(tips));
    SendMessageW(list, WM_HSCROLL, SB_LEFT, 0);
    ListView_SetColumnWidth(list, 1, 100);
    HoverCell(list, &cell);
    SendMessageW(list, WM_CANCELMODE, 0, 0);
    Check("cell tips: cancellation hides the displayed cell", !IsWindowVisible(tips));
    HoverCell(list, &cell);
    SendMessageW(host, WM_ACTIVATE, WA_INACTIVE, 0);
    Check("cell tips: owner deactivation hides immediately", !IsWindowVisible(tips));
    HoverCell(list, &cell);
    EnableWindow(host, FALSE);
    Check("cell tips: opening a modal dialog cannot retain the owner's tooltip", !IsWindowVisible(tips));
    HoverCell(list, &cell);
    Check("cell tips: a pointer move cannot reopen a disabled owner's tooltip", !IsWindowVisible(tips));
    EnableWindow(host, TRUE);

    TreeView_GetItemRect(tree, node, &treeCell, FALSE);
    TreeView_GetItemRect(tree, next, &nextCell, FALSE);
    g_tipRequests = 0;
    HoverCell(tree, &treeCell);
    Check("cell tips: a fully visible tree title stays hidden without requesting details", !IsWindowVisible(treeTips) && g_tipRequests == 0);
    /* Resting on a title that fits: its info tip, then at once the next row's, as Windows' own info tips do. */
    RestOnCell(tree, &treeCell);
    Check("cell tips: resting on a tree title that fits shows its info tip",
          IsWindowVisible(treeTips) && g_tipRequests == 1 && TipReads(treeTips, L"Session 17: full details"));
    HoverCell(tree, &nextCell);
    Check("cell tips: once an info tip shows, the next row's shows at once",
          IsWindowVisible(treeTips) && g_tipRequests == 2 && TipReads(treeTips, L"Session 18: full details"));
    SendMessageW(tree, WM_MOUSELEAVE, 0, 0);
    HoverCell(tree, &treeCell);
    Check("cell tips: after the pointer left, a title that fits waits for the pointer to rest again", !IsWindowVisible(treeTips));
    SendMessageW(tree, WM_MOUSELEAVE, 0, 0);
    TreeView_GetItemRect(tree, plain, &plainCell, FALSE);
    g_tipRequests = 0;
    HoverCell(tree, &plainCell);
    RestOnCell(tree, &plainCell);
    Check("cell tips: a title that shows whole and has no info tip gets no tip", g_tipRequests == 1 && !IsWindowVisible(treeTips));
    SendMessageW(tree, WM_MOUSELEAVE, 0, 0);
    {
        TVITEMW change = { 0 };
        g_tipRequests = 0;
        change.mask = TVIF_HANDLE | TVIF_TEXT;
        change.hItem = node;
        change.pszText = (LPWSTR)L"Session title with enough words to exceed the visible tree viewport by a comfortable margin";
        TreeView_SetItem(tree, &change);
        HoverCell(tree, &treeCell);
        Check("cell tips: a clipped tree title activates its details immediately", IsWindowVisible(treeTips) && g_tipRequests == 1);
        Check("cell tips: tree infotip notifications supply the full details", TipReads(treeTips, L"Session 17: full details"));
        change.pszText = (LPWSTR)L"Session";
        TreeView_SetItem(tree, &change);
        Check("cell tips: a change of the tree's content hides its tip", !IsWindowVisible(treeTips));
        g_tipReserved = 260;
        HoverCell(tree, &treeCell);
        Check("cell tips: counters and suffixes reduce the title's available space", IsWindowVisible(treeTips) && g_tipRequests == 2);
        g_tipReserved = 0;
    }
    SendMessageW(tree, WM_MOUSELEAVE, 0, 0);
    Check("cell tips: leaving the tree hides immediately", !IsWindowVisible(treeTips));
    Check("cell tips: a departed tree item stays hidden without another pointer event", DepartedTipStaysHidden(treeTips));
    for (i = 0; i < (int)ARRAYSIZE(kTitles); i++) {
        LOGFONTW logical;
        HFONT script;
        TVITEMW change = { 0 };
        char name[160];
        GetObjectW(font, sizeof logical, &logical);
        StringCchCopyW(logical.lfFaceName, ARRAYSIZE(logical.lfFaceName), Localize_FontFaceAt(Localize_LanguageForCode(kTitles[i].language)));
        script = CreateFontIndirectW(&logical);
        SendMessageW(tree, WM_SETFONT, (WPARAM)script, FALSE);
        change.mask = TVIF_HANDLE | TVIF_TEXT;
        change.hItem = node;
        change.pszText = (LPWSTR)kTitles[i].title;
        TreeView_SetItem(tree, &change);
        TreeView_GetItemRect(tree, node, &treeCell, FALSE);
        g_tipRequests = 0;
        HoverCell(tree, &treeCell);
        StringCchPrintfA(name, ARRAYSIZE(name), "cell tips: a visible %s title stays hidden", kTitles[i].script);
        Check(name, !IsWindowVisible(treeTips) && g_tipRequests == 0);
        g_tipReserved = 290;
        HoverCell(tree, &treeCell);
        StringCchPrintfA(name, ARRAYSIZE(name), "cell tips: a clipped %s title gets its details immediately", kTitles[i].script);
        Check(name, IsWindowVisible(treeTips) && g_tipRequests == 1);
        g_tipReserved = 0;
        SendMessageW(tree, WM_MOUSELEAVE, 0, 0);
        SendMessageW(tree, WM_SETFONT, (WPARAM)font, FALSE);
        DeleteObject(script);
    }
    {
        int language = Localize_EffectiveLanguage();
        TOOLINFOW current = { 0 };
        UINT_PTR toolId;
        WCHAR buffer[2048];
        ListView_GetSubItemRect(list, 0, 0, LVIR_LABEL, &cell);
        HoverCell(list, &cell);
        current.cbSize = sizeof current;
        SendMessageW(tips, TTM_GETCURRENTTOOLW, 0, (LPARAM)&current);
        toolId = current.uId;
        for (i = 0; i < (int)ARRAYSIZE(kDirections); i++) {
            TOOLINFOW registered = { 0 };
            char name[160];
            Localize_SetLanguage(Localize_LanguageForCode(kDirections[i].language), FALSE);
            Theme_Apply(host);
            registered.cbSize = sizeof registered;
            registered.hwnd = list;
            registered.uId = toolId;
            registered.lpszText = buffer;
            StringCchPrintfA(name, ARRAYSIZE(name), "cell tips: the tooltip reads %s after the language becomes %ls",
                             kDirections[i].rightToLeft ? "right to left" : "left to right", kDirections[i].language);
            Check(name, SendMessageW(tips, TTM_GETTOOLINFOW, 0, (LPARAM)&registered) &&
                  ((registered.uFlags & TTF_RTLREADING) != 0) == kDirections[i].rightToLeft);
        }
        Localize_SetLanguage(language, FALSE);
        Theme_Apply(host);
    }
    {
        LOGFONTW heading;
        GetObjectW(font, sizeof heading, &heading);
        heading.lfHeight = -36;
        heading.lfWeight = FW_SEMIBOLD;
        g_tipHeading = CreateFontIndirectW(&heading);
        TreeView_DeleteItem(tree, next);   /* two rows of the heading's height fit the tree */
        TreeView_DeleteItem(tree, plain);
        insert.item.pszText = (LPWSTR)L"Starred";
        insert.item.lParam = -1;
        node = TreeView_InsertItem(tree, &insert);
        TreeView_SetItemHeight(tree, 42);
        SetWindowPos(tree, NULL, 0, 0, 100, 110, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        TreeView_GetItemRect(tree, node, &treeCell, FALSE);
        HoverCell(tree, &treeCell);
        Check("cell tips: a tree label's fallback uses its custom heading font", IsWindowVisible(treeTips));
        Check("cell tips: a clipped heading keeps its full literal title", TipReads(treeTips, L"Starred"));
        SendMessageW(tree, WM_MOUSELEAVE, 0, 0);
    }
    {
        HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
        LVITEMW image = { 0 };
        HDC dc = GetDC(list);
        HGDIOBJ previous = SelectObject(dc, (HFONT)SendMessageW(list, WM_GETFONT, 0, 0));
        SIZE extent;
        int mode;
        GetTextExtentPoint32W(dc, L"Exact fit", 9, &extent);
        SelectObject(dc, previous);
        ReleaseDC(list, dc);
        if (images) ImageList_SetImageCount(images, 1);
        ListView_SetImageList(list, images, LVSIL_SMALL);
        image.mask = LVIF_IMAGE;
        image.iSubItem = 1;
        image.iImage = 0;
        ListView_SetItem(list, &image);
        ListView_SetItemText(list, 0, 1, L"Exact fit");
        for (mode = 0; mode < 2; mode++) {
            DWORD style = ListView_GetExtendedListViewStyle(list);
            RECT text;
            int width = 0;
            ListView_SetExtendedListViewStyle(list, mode ? style | LVS_EX_SUBITEMIMAGES : style & ~LVS_EX_SUBITEMIMAGES);
            /* The column that gives the text exactly its extent where the theme draws it. */
            ListView_SetColumnWidth(list, 1, 100);
            if (Theme_TableCellText(list, 0, 1, &text)) width = 100 + extent.cx - (text.right - text.left);
            ListView_SetColumnWidth(list, 1, width);
            Check(mode ? "cell tips: an exact-fit subitem with image has its text's extent to draw in" :
                         "cell tips: an exact-fit subitem without image has its text's extent to draw in",
                  width > 0 && Theme_TableCellText(list, 0, 1, &text) && text.right - text.left == extent.cx);
            ListView_GetSubItemRect(list, 0, 1, LVIR_LABEL, &shortCell);
            HoverCell(list, &shortCell);
            Check(mode ? "cell tips: an exact-fit subitem with image stays hidden" : "cell tips: an exact-fit subitem without image stays hidden",
                  !IsWindowVisible(tips));
            ListView_SetColumnWidth(list, 1, width - 1);
            ListView_GetSubItemRect(list, 0, 1, LVIR_LABEL, &shortCell);
            HoverCell(list, &shortCell);
            Check(mode ? "cell tips: one-pixel clipping with image activates immediately" : "cell tips: one-pixel clipping without image activates immediately",
                  IsWindowVisible(tips));
        }
        ListView_SetImageList(list, NULL, LVSIL_SMALL);
        if (images) ImageList_Destroy(images);
    }
    RemoveWindowSubclass(tips, ProbeTipWindow, TIP_WINDOW_SUBCLASS);
    RemoveWindowSubclass(host, ProbeTreeTip, TREE_TIP_SUBCLASS);
    ShowWindow(host, SW_HIDE);
    DestroyWindow(list);
    DestroyWindow(tree);
    if (g_tipHeading) DeleteObject(g_tipHeading);
    g_tipHeading = NULL;
    DeleteObject(font);
}

typedef struct FindTip { HWND control, tips; } FindTip;

static BOOL CALLBACK FindNativeTip(HWND window, LPARAM data)
{
    FindTip *find = (FindTip *)data;
    WCHAR windowClass[64], text[2048];
    int count, i;
    GetClassNameW(window, windowClass, ARRAYSIZE(windowClass));
    if (wcscmp(windowClass, TOOLTIPS_CLASSW) != 0) return TRUE;
    count = (int)SendMessageW(window, TTM_GETTOOLCOUNT, 0, 0);
    for (i = 0; i < count; i++) {
        TOOLINFOW tool = { 0 };
        tool.cbSize = sizeof tool;
        tool.lpszText = text;
        if (SendMessageW(window, TTM_ENUMTOOLSW, i, (LPARAM)&tool) && tool.hwnd == find->control) {
            find->tips = window;
            return FALSE;
        }
    }
    return TRUE;
}

static void TestStatusTip(void)
{
    static const WCHAR kLong[] = L"The profile is ready, but its protocol handler requires the default application chooser.";
    HWND host = ThemedHost(), status;
    HFONT font = DialogFont();
    FindTip find = { 0 };
    RECT cell = { 0, 0, 90, 24 };
    WCHAR text[2048] = { 0 };
    HDC dc;
    HGDIOBJ previous;
    SIZE extent;
    status = CreateWindowExW(0, WC_STATICW, L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
                             10, 10, 90, 24, host, NULL, GetModuleHandleW(NULL), NULL);
    Check("status tip: private ellipsized label created", status != NULL);
    if (!status) {
        DeleteObject(font);
        return;
    }
    SendMessageW(status, WM_SETFONT, (WPARAM)font, FALSE);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    find.control = status;
    EnumThreadWindows(GetCurrentThreadId(), FindNativeTip, (LPARAM)&find);
    Check("status tip: the label uses the shared native tracking tooltip", find.tips != NULL);
    if (!find.tips) {
        DestroyWindow(status);
        ShowWindow(host, SW_HIDE);
        DeleteObject(font);
        return;
    }
    HoverCell(status, &cell);
    Check("status tip: a fully visible status remains hidden", !IsWindowVisible(find.tips));
    SetWindowTextW(status, kLong);
    HoverCell(status, &cell);
    Check("status tip: a clipped status appears in its pointer event", IsWindowVisible(find.tips));
    if (IsWindowVisible(find.tips) && !SkipInLight("status tip's dark surface")) {
        RECT window, area;
        Canvas surface = { 0 };
        int bright, samples;
        GetWindowRect(find.tips, &window);
        SetRect(&area, 0, 0, window.right - window.left, window.bottom - window.top);
        Check("status tip: the owned native tooltip shares the dark surface",
              CanvasOpen(&surface, area.right, area.bottom, RGB(1, 2, 3)) && CopyFromWindow(find.tips, TRUE, &area, &surface, 0, 0) &&
              InflateRect(&area, -1, -1) && MostlyDark(&surface, &area, &bright, &samples));
        CanvasClose(&surface);
    }
    GetWindowTextW(find.tips, text, ARRAYSIZE(text));
    Check("status tip: all of the status text is available", wcscmp(text, kLong) == 0);
    SendMessageW(status, WM_MOUSELEAVE, 0, 0);
    Check("status tip: leaving hides immediately and stays hidden", !IsWindowVisible(find.tips) && DepartedTipStaysHidden(find.tips));
    SetWindowTextW(status, L"Exact fit");
    dc = GetDC(status);
    previous = SelectObject(dc, (HFONT)SendMessageW(status, WM_GETFONT, 0, 0));
    GetTextExtentPoint32W(dc, L"Exact fit", 9, &extent);
    SelectObject(dc, previous);
    ReleaseDC(status, dc);
    SetWindowPos(status, NULL, 0, 0, extent.cx + 1, 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(status, &cell);
    HoverCell(status, &cell);
    Check("status tip: an extent plus one pixel is fully visible", !IsWindowVisible(find.tips));
    SetWindowPos(status, NULL, 0, 0, extent.cx - 1, 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(status, &cell);
    HoverCell(status, &cell);
    Check("status tip: an extent minus one pixel is clipped", IsWindowVisible(find.tips));
    SetWindowTextW(status, L"Ready");
    Check("status tip: a status update hides old tooltip text", !IsWindowVisible(find.tips));
    DestroyWindow(status);
    Check("status tip: destroying the label destroys its owned tooltip", !IsWindow(find.tips));
    ShowWindow(host, SW_HIDE);
    DeleteObject(font);
}

/* ----------------------------------------------------------------- edits */

/* A one-line edit's text sits in the middle of the box, at the font it has
 * and after a new one. */
static void TestEdit(void)
{
    static const int kHeights[] = { 24, 40 };
    HWND host = ThemedHost(), edit;
    HFONT font = DialogFont(), big;
    LOGFONTW logical;
    size_t i;
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"Personal", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 200, kHeights[0], host,
                           NULL, GetModuleHandleW(NULL), NULL);
    if (!edit) {
        Check("edit: created", FALSE);
        DeleteObject(font);
        return;
    }
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, FALSE);
    Theme_Apply(host);
    GetObjectW(font, sizeof logical, &logical);
    logical.lfHeight = -20;
    big = CreateFontIndirectW(&logical);
    for (i = 0; i < ARRAYSIZE(kHeights); i++) {
        RECT window, client;
        TEXTMETRICW metrics;
        HDC dc;
        HGDIOBJ previous;
        int above, below;
        if (i > 0) {
            SetWindowPos(edit, NULL, 0, 0, 200, kHeights[i], SWP_NOMOVE | SWP_NOZORDER);
            SendMessageW(edit, WM_SETFONT, (WPARAM)big, FALSE);
        }
        dc = GetDC(edit);
        previous = SelectObject(dc, (HFONT)SendMessageW(edit, WM_GETFONT, 0, 0));
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        ReleaseDC(edit, dc);
        GetWindowRect(edit, &window);
        GetClientRect(edit, &client);
        MapWindowPoints(edit, NULL, (POINT *)&client, 2);
        above = client.top - window.top;
        below = window.bottom - (client.top + metrics.tmHeight);
        Check(i == 0 ? "an edit's text is centered in its box" : "an edit's text is centered in its box after a new font and height",
              above - below >= -1 && above - below <= 1);
        if (above - below < -1 || above - below > 1) printf("        above %d, below %d\n", above, below);
        {
            /* The margins around the text, outside the edit's client, are still the edit. */
            int x = (window.left + window.right) / 2;
            LRESULT top = SendMessageW(edit, WM_NCHITTEST, 0, MAKELPARAM(x, (window.top + client.top) / 2));
            LRESULT bottom = SendMessageW(edit, WM_NCHITTEST, 0, MAKELPARAM(x, (client.bottom + window.bottom) / 2));
            Check(i == 0 ? "a one-line edit's margins above and below its text take the mouse" :
                           "a one-line edit's margins take the mouse after a new font and height",
                  client.top > window.top && window.bottom > client.bottom && top == HTCLIENT && bottom == HTCLIENT);
        }
    }
    DestroyWindow(edit);
    DeleteObject(font);
    DeleteObject(big);
}

typedef struct EditProbe { int frames, systemBlue, unpaused; } EditProbe;

/* Installed below the theme: this sees native input before the themed
 * commit. The edit's drawing is off meanwhile (WM_SETREDRAW takes its
 * WS_VISIBLE), so what shows is the frame before. */
static LRESULT CALLBACK ProbeEdit(HWND edit, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    EditProbe *probe = (EditProbe *)reference;
    BOOL drawing = (GetWindowLongW(edit, GWL_STYLE) & WS_VISIBLE) != 0;
    LRESULT result = DefSubclassProc(edit, message, wp, lp);
    (void)id;
    if (message == WM_MOUSEMOVE && (wp & MK_LBUTTON)) {
        Canvas shown = { 0 };
        RECT client;
        int x, y;
        probe->frames++;
        if (drawing) probe->unpaused++;
        GetClientRect(edit, &client);
        if (CanvasOpen(&shown, max(1, client.right), max(1, client.bottom), RGB(0, 0, 0))) {
            CopyClient(edit, &shown);
            for (y = 0; y < client.bottom; y++) for (x = 0; x < client.right; x++)
                if (PixelAt(&shown, x, y) == GetSysColor(COLOR_HIGHLIGHT)) probe->systemBlue++;
            CanvasClose(&shown);
        }
    }
    return result;
}

typedef struct EditPauseFixture { HWND edit; BOOL fired; } EditPauseFixture;

static LRESULT CALLBACK PauseEditOnChange(HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    EditPauseFixture *fixture = (EditPauseFixture *)reference;
    (void)id;
    if (message == WM_COMMAND && HIWORD(wp) == EN_CHANGE && (HWND)lp == fixture->edit) {
        fixture->fired = TRUE;
        SendMessageW(fixture->edit, WM_SETREDRAW, FALSE, 0);
        return 0;
    }
    return DefSubclassProc(host, message, wp, lp);
}

/* A caller that pauses an edit's drawing from its EN_CHANGE keeps it paused
 * after the theme's own update, until it resumes. */
static void TestEditReentrantPause(void)
{
    HWND host = ThemedHost();
    EditPauseFixture fixture = { 0 };
    fixture.edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   10, 10, 220, 28, host, (HMENU)17, GetModuleHandleW(NULL), NULL);
    if (!fixture.edit) {
        Check("edit pause: private fixture created", FALSE);
        return;
    }
    Theme_Apply(host);
    if (!SetWindowSubclass(host, PauseEditOnChange, EDIT_PAUSE_SUBCLASS, (DWORD_PTR)&fixture)) {
        Check("edit pause: native notification fixture installed", FALSE);
        DestroyWindow(fixture.edit);
        return;
    }
    SetWindowTextW(fixture.edit, L"Native text change");
    Check("edit pause: EN_CHANGE is delivered synchronously", fixture.fired);
    /* A window whose drawing is off has lost WS_VISIBLE (WM_SETREDRAW). */
    Check("edit pause: the caller's redraw pause holds after the themed update", !(GetWindowLongW(fixture.edit, GWL_STYLE) & WS_VISIBLE));
    SendMessageW(fixture.edit, WM_SETREDRAW, TRUE, 0);
    Check("edit pause: the caller's resume draws the edit again", (GetWindowLongW(fixture.edit, GWL_STYLE) & WS_VISIBLE) != 0);
    RemoveWindowSubclass(host, PauseEditOnChange, EDIT_PAUSE_SUBCLASS);
    DestroyWindow(fixture.edit);
}

static BOOL BrightInk(const Canvas *canvas, RECT *ink)
{
    int x, y;
    SetRect(ink, canvas->width, canvas->height, 0, 0);
    for (y = 0; y < canvas->height; y++) for (x = 0; x < canvas->width; x++) {
        COLORREF color = PixelAt(canvas, x, y);
        if (GetRValue(color) > 180 && GetGValue(color) > 180 && GetBValue(color) > 180) {
            ink->left = min(ink->left, x);
            ink->right = max(ink->right, x + 1);
            ink->top = min(ink->top, y);
            ink->bottom = max(ink->bottom, y + 1);
        }
    }
    return ink->right > ink->left;
}

static void TestEditInput(void)
{
    static const struct { const WCHAR *text; const char *script; } kSamples[] = {
        { L"Personal", "Latin" },
        { L"\x0645\x0644\x0641 \x0634\x062E\x0635\x064A", "Arabic" },
        { L"\x0935\x094D\x092F\x0915\x094D\x0924\x093F\x0917\x0924 \x092A\x094D\x0930\x094B\x092B\x093C\x093E\x0907\x0932", "Devanagari" },
        { L"\x09AC\x09CD\x09AF\x0995\x09CD\x09A4\x09BF\x0997\x09A4 \x09AA\x09CD\x09B0\x09CB\x09AB\x09BE\x0987\x09B2", "Bengali" },
    };
    HWND host = ThemedHost(), edit, reference;
    HFONT font = DialogFont();
    EditProbe probe = { 0, 0, 0 };
    size_t sample;
    RECT client;
    DWORD start = 0, end = 0;
    int step;
    BOOL dark = !SkipInLight("edit input's selection colors");
    SetWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS, 0);
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, kSamples[0].text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
                           10, 10, 220, 32, host, NULL, GetModuleHandleW(NULL), NULL);
    Check("edit input: private control created", edit != NULL);
    if (!edit) {
        RemoveWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS);
        DeleteObject(font);
        return;
    }
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, FALSE);
    SetWindowSubclass(edit, ProbeEdit, EDIT_PROBE_SUBCLASS, (DWORD_PTR)&probe);
    Theme_Apply(host);
    reference = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, kSamples[0].text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                250, 10, 220, 32, host, NULL, GetModuleHandleW(NULL), NULL);
    Check("edit input: native reference created", reference != NULL);
    if (!reference) goto done;
    SendMessageW(reference, WM_SETFONT, (WPARAM)font, FALSE);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    if (TakeFocus(edit, "edit input's mouse selection")) {
        LRESULT position;
        SendMessageW(edit, EM_SETSEL, 0, 0);
        position = SendMessageW(edit, EM_POSFROMCHAR, 0, 0);
        SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((short)LOWORD(position), (short)HIWORD(position) + 3));
        for (step = 2; step <= 6; step += 2) {
            position = SendMessageW(edit, EM_POSFROMCHAR, step, 0);
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM((short)LOWORD(position), (short)HIWORD(position) + 3));
        }
        SendMessageW(edit, WM_LBUTTONUP, 0, 0);
        SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        Check("edit input: mouse drag retains native selection", start != end);
        Check("edit input: every native drag update is inside one paint transaction", probe.frames == 3 && probe.unpaused == 0);
        if (dark) Check("edit input: no system-blue frame is exposed before the themed commit", probe.systemBlue == 0);
        Check("edit input: drawing does not hide the control", IsWindowVisible(edit));
    }
    for (sample = 0; dark && sample < ARRAYSIZE(kSamples); sample++) {
        Canvas themed = { 0 }, native = { 0 }, printed = { 0 };
        RECT themedInk, nativeInk, window;
        BOOL inkThemed, inkNative;
        int x, y, wrong = 0, blue = 0, system = 0, palette = 0;
        char name[160];
        SetWindowTextW(edit, kSamples[sample].text);
        SetWindowTextW(reference, kSamples[sample].text);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SendMessageW(reference, EM_SETSEL, 0, -1);
        GetClientRect(edit, &client);
        if (!CanvasOpen(&themed, max(1, client.right), max(1, client.bottom), Theme_Color(THEME_FIELD)) ||
            !CanvasOpen(&native, max(1, client.right), max(1, client.bottom), Theme_Color(THEME_FIELD))) {
            Check("edit input: rendering canvases created", FALSE);
            CanvasClose(&themed);
            CanvasClose(&native);
            break;
        }
        SendMessageW(edit, WM_PRINTCLIENT, (WPARAM)themed.dc, PRF_CLIENT);
        SendMessageW(reference, WM_PRINTCLIENT, (WPARAM)native.dc, PRF_CLIENT);
        inkThemed = BrightInk(&themed, &themedInk);
        inkNative = BrightInk(&native, &nativeInk);
        for (y = 0; y < themed.height; y++) for (x = 0; x < themed.width; x++) {
            COLORREF pixel = PixelAt(&themed, x, y);
            if (pixel == GetSysColor(COLOR_HIGHLIGHT)) wrong++;
            if (pixel == Theme_Color(THEME_MAIN_BLUE)) blue++;
        }
        StringCchPrintfA(name, ARRAYSIZE(name), "edit input: selected %s text has one palette background", kSamples[sample].script);
        Check(name, wrong == 0 && blue > 0);
        StringCchPrintfA(name, ARRAYSIZE(name), "edit input: selected %s glyphs keep their native baseline and height", kSamples[sample].script);
        Check(name, inkThemed && inkNative && themedInk.top == nativeInk.top && themedInk.bottom == nativeInk.bottom);
        if (!inkThemed || !inkNative || themedInk.top != nativeInk.top || themedInk.bottom != nativeInk.bottom)
            printf("        themed ink=%d (%ld,%ld)-(%ld,%ld), native ink=%d (%ld,%ld)-(%ld,%ld)\n", inkThemed, themedInk.left, themedInk.top,
                   themedInk.right, themedInk.bottom, inkNative, nativeInk.left, nativeInk.top, nativeInk.right, nativeInk.bottom);
        GetWindowRect(edit, &window);
        if (CanvasOpen(&printed, window.right - window.left, window.bottom - window.top, RGB(0, 0, 0))) {
            SendMessageW(edit, WM_PRINT, (WPARAM)printed.dc, PRF_CLIENT | PRF_NONCLIENT);
            for (y = 0; y < printed.height; y++) for (x = 0; x < printed.width; x++) {
                COLORREF pixel = PixelAt(&printed, x, y);
                if (pixel == GetSysColor(COLOR_HIGHLIGHT)) system++;
                if (pixel == Theme_Color(THEME_MAIN_BLUE)) palette++;
            }
            StringCchPrintfA(name, ARRAYSIZE(name), "edit input: WM_PRINT of selected %s text uses the client's selection palette", kSamples[sample].script);
            Check(name, system == 0 && palette > 0);
        } else Check("edit input: whole-window print canvas created", FALSE);
        CanvasClose(&printed);
        CanvasClose(&themed);
        CanvasClose(&native);
    }
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(edit, L"Batch");
    Check("edit input: an external redraw batch stays paused", !(GetWindowLongW(edit, GWL_STYLE) & WS_VISIBLE));
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    Check("edit input: an external batch can resume", (GetWindowLongW(edit, GWL_STYLE) & WS_VISIBLE) != 0);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SendMessageW(edit, WM_CHAR, L'X', 1);
    {
        WCHAR text[32];
        GetWindowTextW(edit, text, ARRAYSIZE(text));
        Check("edit input: keyboard replacement uses the native edit model", wcscmp(text, L"X") == 0);
    }
    DestroyWindow(reference);
done:
    ShowWindow(host, SW_HIDE);
    RemoveWindowSubclass(edit, ProbeEdit, EDIT_PROBE_SUBCLASS);
    DestroyWindow(edit);
    RemoveWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS);
    DeleteObject(font);
}

typedef struct PrintProbe { int clientPaints, clientPrints; } PrintProbe;

/* Below the theme: what reaches the native edit. */
static LRESULT CALLBACK ProbePrint(HWND edit, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    PrintProbe *probe = (PrintProbe *)reference;
    (void)id;
    if (message == WM_PRINTCLIENT) probe->clientPaints++;
    if (message == WM_PRINT && (lp & PRF_CLIENT)) probe->clientPrints++;
    return DefSubclassProc(edit, message, wp, lp);
}

/* How many pixels of `area` still have the guard color. */
static int GuardPixels(const Canvas *canvas, const RECT *area, COLORREF guard)
{
    int x, y, count = 0;
    for (y = max(0, area->top); y < min(canvas->height, area->bottom); y++)
        for (x = max(0, area->left); x < min(canvas->width, area->right); x++)
            if (PixelAt(canvas, x, y) == guard) count++;
    return count;
}

/* How many pixels of `window`'s frame (its window, at the canvas' origin,
 * less its client, inside the shape of its window region) still have the
 * guard color. */
static int UnpaintedFrame(const Canvas *canvas, HWND window, const RECT *client, COLORREF guard)
{
    HRGN shape = CreateRectRgn(0, 0, 0, 0);
    BOOL shaped = shape && GetWindowRgn(window, shape) != ERROR;
    int x, y, count = 0;
    for (y = 0; y < canvas->height; y++)
        for (x = 0; x < canvas->width; x++) {
            POINT point;
            point.x = x;
            point.y = y;
            if (PtInRect(client, point) || (shaped && !PtInRegion(shape, x, y))) continue;
            if (PixelAt(canvas, x, y) == guard) count++;
        }
    if (shape) DeleteObject(shape);
    return count;
}

/* WM_PRINT of a themed edit: nothing for a hidden one asked with
 * PRF_CHECKVISIBLE; its client painted once, by the native edit, where its
 * client is, as WM_PRINTCLIENT paints it; what is not asked for (the frame
 * or the client) left as it was. */
static void TestEditPrinting(void)
{
    HWND host = ThemedHost(), edit;
    HFONT font = DialogFont();
    PrintProbe probe = { 0, 0 };
    COLORREF guard = RGB(1, 2, 3);
    RECT window, client, whole;
    POINT origin = { 0, 0 };
    Canvas printed = { 0 }, painted = { 0 };
    int x, y, differences = 0, total, clientArea, frameArea, missing;
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"Printed text", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 220, 28, host, NULL,
                           GetModuleHandleW(NULL), NULL);
    if (!edit) {
        Check("edit printing: private edit created", FALSE);
        DeleteObject(font);
        return;
    }
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, FALSE);
    SetWindowSubclass(edit, ProbePrint, PRINT_PROBE_SUBCLASS, (DWORD_PTR)&probe);
    SetWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS, 0);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    GetWindowRect(edit, &window);
    GetClientRect(edit, &client);
    ClientToScreen(edit, &origin);
    OffsetRect(&client, origin.x - window.left, origin.y - window.top);
    SetRect(&whole, 0, 0, window.right - window.left, window.bottom - window.top);
    total = whole.right * whole.bottom;
    clientArea = (client.right - client.left) * (client.bottom - client.top);
    frameArea = total - clientArea;

    ShowWindow(edit, SW_HIDE);
    if (CanvasOpen(&printed, whole.right, whole.bottom, guard)) {
        SendMessageW(edit, WM_PRINT, (WPARAM)printed.dc, PRF_CHECKVISIBLE | PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        Check("edit printing: a hidden edit asked with PRF_CHECKVISIBLE prints nothing",
              GuardPixels(&printed, &whole, guard) == total && probe.clientPaints == 0);
    } else Check("edit printing: canvas created", FALSE);
    CanvasClose(&printed);
    ShowWindow(edit, SW_SHOW);

    probe.clientPaints = probe.clientPrints = 0;
    if (CanvasOpen(&printed, whole.right, whole.bottom, guard) && CanvasOpen(&painted, client.right - client.left, client.bottom - client.top, guard)) {
        SendMessageW(edit, WM_PRINT, (WPARAM)printed.dc, PRF_CLIENT | PRF_NONCLIENT);
        Check("edit printing: the native edit paints its client once per print",
              probe.clientPaints == 1 && probe.clientPrints == 0);
        SendMessageW(edit, WM_PRINTCLIENT, (WPARAM)painted.dc, PRF_CLIENT);
        for (y = 0; y < painted.height; y++) for (x = 0; x < painted.width; x++)
            if (PixelAt(&painted, x, y) != PixelAt(&printed, client.left + x, client.top + y)) differences++;
        Check("edit printing: the printed client is the client's own painting, where the client is", differences == 0);
        if (differences) printf("        %d of %d client pixels differ\n", differences, painted.width * painted.height);
        missing = UnpaintedFrame(&printed, edit, &client, guard);
        Check("edit printing: a full print draws the whole frame inside the edit's shape", missing == 0);
        if (missing) printf("        %d frame pixels left unpainted\n", missing);
    } else Check("edit printing: canvases created", FALSE);
    CanvasClose(&printed);
    CanvasClose(&painted);

    if (CanvasOpen(&printed, whole.right, whole.bottom, guard)) {
        SendMessageW(edit, WM_PRINT, (WPARAM)printed.dc, PRF_NONCLIENT);
        missing = UnpaintedFrame(&printed, edit, &client, guard);
        Check("edit printing: a frame print leaves the client as it was", GuardPixels(&printed, &client, guard) == clientArea);
        Check("edit printing: a frame print draws the whole frame inside the edit's shape", missing == 0);
        if (missing) printf("        %d frame pixels left unpainted\n", missing);
    } else Check("edit printing: canvas created", FALSE);
    CanvasClose(&printed);

    if (CanvasOpen(&printed, whole.right, whole.bottom, guard)) {
        SendMessageW(edit, WM_PRINT, (WPARAM)printed.dc, PRF_CLIENT);
        Check("edit printing: a client print leaves the frame as it was",
              GuardPixels(&printed, &whole, guard) - GuardPixels(&printed, &client, guard) == frameArea);
        Check("edit printing: a client print paints the whole client", GuardPixels(&printed, &client, guard) == 0);
    } else Check("edit printing: canvas created", FALSE);
    CanvasClose(&printed);

    ShowWindow(host, SW_HIDE);
    RemoveWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS);
    RemoveWindowSubclass(edit, ProbePrint, PRINT_PROBE_SUBCLASS);
    DestroyWindow(edit);
    DeleteObject(font);
}

/* ---------------------------------------------------------- drop-downs */

/* The box around what stands out from `back` in x0..x1, y0..y1 of
 * `canvas`; FALSE when nothing does. */
/* Where `canvas` differs from `under` (the same drawing without what is
 * looked for), between (x0, y0) and (x1, y1). */
static BOOL DifferenceBox(const Canvas *canvas, const Canvas *under, int x0, int y0, int x1, int y1, RECT *box)
{
    int x, y;
    SetRect(box, x1, y1, x0, y0);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (StandsOut(PixelAt(canvas, x, y), PixelAt(under, x, y))) {
                box->left = min(box->left, x);
                box->top = min(box->top, y);
                box->right = max(box->right, x + 1);
                box->bottom = max(box->bottom, y + 1);
            }
    return box->left < box->right;
}

static BOOL InkBox(const Canvas *canvas, int x0, int y0, int x1, int y1, COLORREF back, RECT *box)
{
    int x, y;
    SetRect(box, x1, y1, x0, y0);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (StandsOut(PixelAt(canvas, x, y), back)) {
                box->left = min(box->left, x);
                box->top = min(box->top, y);
                box->right = max(box->right, x + 1);
                box->bottom = max(box->bottom, y + 1);
            }
    return box->left < box->right;
}

/* A drop-down button (the sessions view's Actions) has the face of the push
 * buttons next to it in every state (one painter), and the arrow Windows
 * draws on a drop-down list, in the middle of its height. */
static void TestDropDownButton(void)
{
    static const struct { UINT state; const char *name; } kStates[] = {
        { 0, "at rest" }, { THEME_BUTTON_HOT, "under the mouse" }, { THEME_BUTTON_PRESSED, "pressed" }, { THEME_BUTTON_DISABLED, "disabled" },
    };
    const WCHAR *themeClass = Theme_IsDark() ? L"DarkMode_CFD::Combobox" : L"Combobox";
    HWND host = ThemedHost();
    HTHEME theme;
    HFONT font = DialogFont();
    COLORREF face = Theme_Color(THEME_FACE);
    RECT box = { 0, 0, 120, 24 }, ours = { 0 }, theirs = { 0 }, label;
    Canvas button = { 0 }, dropDown = { 0 }, glyph = { 0 };
    int arrowWidth = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(host)), arrowLeft;
    char name[160];
    size_t i;
    /* The arrow's cell begins where the label ends. */
    Theme_DropDownLabel(host, &box, &label);
    arrowLeft = label.right;
    for (i = 0; i < ARRAYSIZE(kStates); i++) {
        int x, y, differences = 0;
        if (!CanvasOpen(&button, 120, 24, face) || !CanvasOpen(&dropDown, 120, 24, face)) {
            CanvasClose(&button);
            Check("drop-down: canvases created", FALSE);
            continue;
        }
        Theme_DrawButton(host, button.dc, &box, L"", font, kStates[i].state, DT_SINGLELINE);
        Theme_DrawDropDown(host, dropDown.dc, &box, L"", font, kStates[i].state);
        for (y = 0; y < box.bottom; y++) for (x = 0; x < arrowLeft; x++)
            if (PixelAt(&button, x, y) != PixelAt(&dropDown, x, y)) differences++;
        StringCchPrintfA(name, ARRAYSIZE(name), "a drop-down button %s has the face of a push button left of its arrow", kStates[i].name);
        Check(name, differences == 0);
        if (differences) printf("        %d pixels differ\n", differences);
        CanvasClose(&button);
        CanvasClose(&dropDown);
    }
    if (HighContrastOn()) {
        Skip("drop-down arrow: contrast colors draw the arrow in the system's colors");
        DeleteObject(font);
        return;
    }
    if ((theme = OpenThemeDataForDpi(NULL, themeClass, GetDpiForWindow(host))) == NULL) {
        StringCchPrintfA(name, ARRAYSIZE(name), "drop-down arrow: no %ls visual style", themeClass);
        Skip(name);
        DeleteObject(font);
        return;
    }
    /* The arrow: what the drop-down button adds to a push button, whose
     * rounded corners reach into the arrow's cell. */
    if (CanvasOpen(&dropDown, 120, 24, face) && CanvasOpen(&button, 120, 24, face) && CanvasOpen(&glyph, 40, 24, RGB(0, 0, 0))) {
        RECT cell = { 0, 0, 0, 24 };
        COLORREF fill;
        BOOL found, same, middle;
        cell.right = arrowWidth;
        Theme_DrawDropDown(host, dropDown.dc, &box, L"", font, 0);
        Theme_DrawButton(host, button.dc, &box, L"", font, 0, DT_SINGLELINE);
        fill = PixelAt(&dropDown, 60, 12);
        SetDCBrushColor(glyph.dc, fill);
        FillRect(glyph.dc, &cell, (HBRUSH)GetStockObject(DC_BRUSH));
        DrawThemeBackground(theme, glyph.dc, CP_DROPDOWNBUTTONRIGHT, CBXSR_NORMAL, &cell, NULL);
        found = DifferenceBox(&dropDown, &button, arrowLeft, 0, arrowLeft + arrowWidth, 24, &ours) &&
                InkBox(&glyph, 0, 0, arrowWidth, 24, fill, &theirs);
        same = found && ours.right - ours.left == theirs.right - theirs.left && ours.bottom - ours.top == theirs.bottom - theirs.top;
        middle = found && abs((ours.top + ours.bottom) - 24) <= 2;
        Check("a drop-down button shows the arrow Windows draws on a drop-down list", same);
        Check("a drop-down button's arrow is in the middle of its height", middle);
        if (found && (!same || !middle))
            printf("        arrow %ldx%ld at y %ld (Windows': %ldx%ld at y %ld)\n", ours.right - ours.left, ours.bottom - ours.top, ours.top,
                   theirs.right - theirs.left, theirs.bottom - theirs.top, theirs.top);
        CanvasClose(&glyph);
    } else Check("drop-down arrow: canvases created", FALSE);
    CanvasClose(&button);
    CanvasClose(&dropDown);
    CloseThemeData(theme);
    DeleteObject(font);
}

/* Theme_DropDownWidth: a drop-down button that wide gives its label room for
 * the whole text (Theme_DropDownLabel, where Theme_DrawDropDown draws it),
 * and drawn so its text is whole; a box whose label has one pixel less than
 * the text cuts it. */
static void TestDropDownWidth(void)
{
    HWND host = ThemedHost();
    HFONT font = DialogFont();
    int width, narrowWidth, room, x, y, differences;
    RECT text = { 0, 0, 0, 0 }, box, label, narrowLabel;
    Canvas exact = { 0 }, wide = { 0 }, narrow = { 0 };
    HDC dc = GetDC(host);
    if (dc) {
        HGDIOBJ previous = SelectObject(dc, font);
        DrawTextW(dc, L"Actions", -1, &text, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
        ReleaseDC(host, dc);
    }
    width = Theme_DropDownWidth(host, font, L"Actions");
    SetRect(&box, 0, 0, width, 24);
    Theme_DropDownLabel(host, &box, &label);
    room = label.right - label.left;
    Check("drop-down width: a box that wide has room for its whole text", text.right > 0 && room >= text.right);
    /* The label grows and shrinks with the box: one pixel less than the text. */
    narrowWidth = width - (room - (text.right - 1));
    SetRect(&box, 0, 0, narrowWidth, 24);
    Theme_DropDownLabel(host, &box, &narrowLabel);
    Check("drop-down width: a box one pixel too narrow leaves its label one pixel less than the text",
          narrowLabel.right - narrowLabel.left == text.right - 1);
    if (room < text.right || narrowLabel.right - narrowLabel.left != text.right - 1)
        printf("        text %ld px, label %d px at width %d, %ld px at width %d\n", text.right, room, width,
               narrowLabel.right - narrowLabel.left, narrowWidth);
    if (CanvasOpen(&exact, width, 24, RGB(1, 2, 3)) && CanvasOpen(&wide, width + 60, 24, RGB(1, 2, 3)) && narrowWidth > 0 &&
        CanvasOpen(&narrow, narrowWidth, 24, RGB(1, 2, 3))) {
        SetRect(&box, 0, 0, width, 24);
        Theme_DrawDropDown(host, exact.dc, &box, L"Actions", font, 0);
        box.right = width + 60;
        Theme_DrawDropDown(host, wide.dc, &box, L"Actions", font, 0);
        box.right = narrowWidth;
        Theme_DrawDropDown(host, narrow.dc, &box, L"Actions", font, 0);
        for (differences = 0, y = 0; y < 24; y++) for (x = 0; x < label.right; x++)
            if (PixelAt(&exact, x, y) != PixelAt(&wide, x, y)) differences++;
        Check("drop-down width: at its width the text shows whole", differences == 0);
        for (differences = 0, y = 0; y < 24; y++) for (x = 0; x < narrowLabel.right; x++)
            if (PixelAt(&narrow, x, y) != PixelAt(&wide, x, y)) differences++;
        Check("drop-down width: one pixel too narrow, the text is cut", differences > 0);
    } else Check("drop-down width: canvases created", FALSE);
    CanvasClose(&exact);
    CanvasClose(&wide);
    CanvasClose(&narrow);
    DeleteObject(font);
}

/* A dialog's drop-down list looks like a drop-down button showing its
 * choice, and never shows its choice in the selection color, with the focus
 * or without. */
static void TestDropDownList(void)
{
    static const struct { UINT state; const char *name; } kPasses[] = {
        { 0, "at rest" }, { 0, "with the focus" }, { THEME_BUTTON_HOT, "under the mouse" }, { THEME_BUTTON_DISABLED, "disabled" }
    };
    HWND host = ThemedHost(), combo;
    HFONT font = DialogFont();
    COLORREF face = Theme_Color(THEME_FACE), highlight = GetSysColor(COLOR_HIGHLIGHT);
    RECT window, box;
    Canvas shown = { 0 }, drawn = { 0 };
    int pass;
    combo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                            10, 10, 120, 200, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!combo) {
        Check("drop-down list: created", FALSE);
        DeleteObject(font);
        return;
    }
    SendMessageW(combo, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Blue");
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    for (pass = 0; pass < (int)ARRAYSIZE(kPasses); pass++) {
        int width, height, x, y, differ = 0, selected = 0;
        char name[160];
        if (pass == 1 && !TakeFocus(combo, "drop-down list with the focus")) continue;
        if (pass == 2) {
            SetFocus(host);
            SendMessageW(combo, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
        } else if (pass == 3) EnableWindow(combo, FALSE);
        RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        GetWindowRect(combo, &window);
        width = window.right - window.left;
        height = window.bottom - window.top;
        SetRect(&box, 0, 0, width, height);
        /* Its own pixels, as Windows composes them next. */
        if (!CanvasOpen(&shown, width, height, RGB(0, 0, 0)) || !CanvasOpen(&drawn, width, height, face) ||
            !CopyFromWindow(combo, TRUE, &box, &shown, 0, 0)) {
            CanvasClose(&shown);
            CanvasClose(&drawn);
            Check("drop-down list: captured", FALSE);
            break;
        }
        if (!HasImage(&shown, "drop-down list")) {
            CanvasClose(&shown);
            CanvasClose(&drawn);
            break;
        }
        Theme_DrawDropDown(host, drawn.dc, &box, L"Blue", font, kPasses[pass].state);
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++) {
                COLORREF color = PixelAt(&shown, x, y);
                if (color == highlight) selected++;
                if (pass != 1 && color != PixelAt(&drawn, x, y)) differ++;
            }
        if (pass != 1) {
            StringCchPrintfA(name, ARRAYSIZE(name), "a dialog's choice %s has the full Actions button pixels", kPasses[pass].name);
            Check(name, differ == 0);
            if (differ) printf("        %d of %d pixels differ\n", differ, width * height);
        }
        if (pass < 2) {
            StringCchPrintfA(name, ARRAYSIZE(name), "a dialog's choice %s is not shown selected", kPasses[pass].name);
            Check(name, selected == 0);
        }
        CanvasClose(&shown);
        CanvasClose(&drawn);
    }
    ShowWindow(host, SW_HIDE);
    DestroyWindow(combo);
    DeleteObject(font);
}

typedef struct ChoiceProbe {
    HWND combo;
    int confirmed, cancelled, changes, opened, closed;
    UINT keys[8], messages[8];
    int keyCount, nextKey, idle;
    BOOL menuWindow, values, checked, focus, expanded, pressed, face, endedByProbe;
    Canvas popup;
} ChoiceProbe;

static LRESULT CALLBACK ProbeChoiceNotifications(HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    ChoiceProbe *probe = (ChoiceProbe *)reference;
    (void)id;
    if (message == WM_COMMAND && (HWND)lp == probe->combo) {
        if (HIWORD(wp) == CBN_SELENDOK) probe->confirmed++;
        if (HIWORD(wp) == CBN_SELENDCANCEL) probe->cancelled++;
        if (HIWORD(wp) == CBN_SELCHANGE) probe->changes++;
        if (HIWORD(wp) == CBN_DROPDOWN) probe->opened++;
        if (HIWORD(wp) == CBN_CLOSEUP) probe->closed++;
    }
    if (message == WM_INITMENUPOPUP) {
        HMENU menu = (HMENU)wp;
        probe->values = GetMenuItemCount(menu) == 2;
        probe->checked = (GetMenuState(menu, 1, MF_BYPOSITION) & MF_CHECKED) != 0;
    }
    if (message == WM_ENTERIDLE && wp == MSGF_MENU) {
        WCHAR windowClass[64];
        HWND popup = (HWND)lp;
        RECT frame;
        probe->idle++;
        GetClassNameW(popup, windowClass, ARRAYSIZE(windowClass));
        probe->menuWindow = wcscmp(windowClass, L"#32768") == 0;
        if (!probe->popup.dc && GetWindowRect(popup, &frame) && CanvasOpen(&probe->popup, frame.right - frame.left, frame.bottom - frame.top, RGB(1, 2, 3)))
            SendMessageW(popup, WM_PRINT, (WPARAM)probe->popup.dc, PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        if (probe->combo) {
            COMBOBOXINFO info = { 0 };
            Canvas shown = { 0 }, button = { 0 };
            WCHAR value[64] = { 0 };
            RECT client;
            int x, y, differences = 0, selected = (int)SendMessageW(probe->combo, CB_GETCURSEL, 0, 0);
            info.cbSize = sizeof info;
            probe->focus = GetFocus() == probe->combo;
            probe->expanded = SendMessageW(probe->combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
            SendMessageW(probe->combo, CB_GETCOMBOBOXINFO, 0, (LPARAM)&info);
            probe->pressed = (info.stateButton & STATE_SYSTEM_PRESSED) != 0;
            if (selected >= 0 && SendMessageW(probe->combo, CB_GETLBTEXTLEN, selected, 0) < (LRESULT)ARRAYSIZE(value))
                SendMessageW(probe->combo, CB_GETLBTEXT, selected, (LPARAM)value);
            GetClientRect(probe->combo, &client);
            if (CanvasOpen(&shown, client.right, client.bottom, RGB(1, 2, 3)) && CanvasOpen(&button, client.right, client.bottom, RGB(1, 2, 3))) {
                SendMessageW(probe->combo, WM_PRINTCLIENT, (WPARAM)shown.dc, PRF_CLIENT);
                Theme_DrawDropDown(probe->combo, button.dc, &client, value, (HFONT)SendMessageW(probe->combo, WM_GETFONT, 0, 0), THEME_BUTTON_PRESSED);
                for (y = 0; y < client.bottom; y++) for (x = 0; x < client.right; x++)
                    if (PixelAt(&shown, x, y) != PixelAt(&button, x, y)) differences++;
                probe->face = differences == 0;
            }
            CanvasClose(&shown);
            CanvasClose(&button);
        }
        /* One input event per native idle notification lets the test observe
         * a menu's actual keyboard selection without a timer or input hook;
         * a menu still open once the keys are out is ended here. */
        if (probe->nextKey < probe->keyCount) {
            int key = probe->nextKey++;
            PostMessageW(GetFocus() ? GetFocus() : host, probe->messages[key] ? probe->messages[key] : WM_KEYDOWN, probe->keys[key], 0);
        } else {
            probe->endedByProbe = TRUE;
            EndMenu();
        }
        return 0;
    }
    return DefSubclassProc(host, message, wp, lp);
}

static void ResetChoiceProbe(ChoiceProbe *probe, HWND combo, const UINT *keys, const UINT *messages, int count)
{
    int i;
    CanvasClose(&probe->popup);
    ZeroMemory(probe, sizeof *probe);
    probe->combo = combo;
    probe->keyCount = count;
    for (i = 0; i < count && i < (int)ARRAYSIZE(probe->keys); i++) {
        probe->keys[i] = keys[i];
        probe->messages[i] = messages ? messages[i] : WM_KEYDOWN;
    }
}

static BOOL ChoiceClosed(const void *probe)
{
    const ChoiceProbe *choice = (const ChoiceProbe *)probe;
    return !SendMessageW(choice->combo, CB_GETDROPPEDSTATE, 0, 0) && choice->closed > 0;
}

/* Opened and answered: its menu runs inside a queued message. */
static BOOL AwaitChoice(ChoiceProbe *probe)
{
    return PumpUntil(ChoiceClosed, probe, 5000);
}

static void TestComboInput(void)
{
    static const UINT kEscape[] = { VK_ESCAPE };
    static const UINT kEnter[] = { VK_RETURN };
    static const UINT kSelectFirst[] = { VK_DOWN, VK_RETURN };
    static const UINT kSelectSecond[] = { VK_DOWN, VK_DOWN, VK_RETURN };
    static const UINT kTypeGreen[] = { L'G' };
    static const UINT kCharacter[] = { WM_CHAR };
    static const UINT kF4ThenEscape[] = { VK_F4, VK_ESCAPE };
    static const UINT kTab[] = { VK_TAB };
    HWND host = ThemedHost(), combo;
    HFONT font = DialogFont();
    COMBOBOXINFO info = { 0 };
    BOOL animationBefore = FALSE, animationAfter = FALSE, fadeBefore = FALSE, fadeAfter = FALSE, focused;
    WCHAR text[64], windowClass[64];
    ChoiceProbe probe = { 0 };
    Canvas reference = { 0 };
    RECT box;
    UINT command;
    HMENU menu;
    int x, y, differ = 0;
    SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &animationBefore, 0);
    SystemParametersInfoW(SPI_GETMENUFADE, 0, &fadeBefore, 0);
    combo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                            10, 10, 220, 180, host, NULL, GetModuleHandleW(NULL), NULL);
    Check("combo input: private native control created", combo != NULL);
    if (!combo) {
        DeleteObject(font);
        return;
    }
    SendMessageW(combo, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Blue");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Green");
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    Theme_Apply(host);
    SetWindowSubclass(host, ProbeChoiceNotifications, CHOICE_PROBE_SUBCLASS, (DWORD_PTR)&probe);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    focused = TakeFocus(combo, "combo input's keyboard focus during expansion");
    SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
    Check("combo input: closed keyboard arrows update the native model", SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1);
    GetWindowTextW(combo, text, ARRAYSIZE(text));
    Check("combo input: native value is available to accessibility clients", wcscmp(text, L"Green") == 0);
    GetClassNameW(combo, windowClass, ARRAYSIZE(windowClass));
    Check("combo input: the native ComboBox class is retained", wcscmp(windowClass, WC_COMBOBOXW) == 0);

    /* An Actions menu and a choice menu with the same contents and states
     * must have the same native popup, including its full painted pixels. */
    ResetChoiceProbe(&probe, NULL, kEscape, NULL, ARRAYSIZE(kEscape));
    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Blue");
    AppendMenuW(menu, MF_STRING, 2, L"Green");
    CheckMenuRadioItem(menu, 1, 2, 2, MF_BYCOMMAND);
    GetWindowRect(combo, &box);
    command = Theme_TrackDropDown(host, menu, &box);
    Check("drop-down menu: the Actions reference uses the native menu window", probe.menuWindow && probe.idle > 0 && command == 0);
    reference = probe.popup;
    ZeroMemory(&probe.popup, sizeof probe.popup);
    DestroyMenu(menu);

    ResetChoiceProbe(&probe, combo, kEscape, NULL, ARRAYSIZE(kEscape));
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    Check("combo input: a box asked to drop down is dropped at once, its menu still to open from the queue",
          SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0 && probe.idle == 0);
    Check("combo input: the queued native menu completes from its own events", AwaitChoice(&probe));
    Check("combo input: choices use the same native menu window as Actions", probe.menuWindow && probe.idle > 0);
    Check("combo input: the native menu contains every value and marks the current choice", probe.values && probe.checked);
    Check("combo input: expansion exposes the pressed state", probe.expanded && probe.pressed);
    if (focused) Check("combo input: expansion preserves keyboard focus", probe.focus);
    Check("drop-down menu: native popup painting contains actual menu pixels", reference.dc && probe.popup.dc &&
          PixelAt(&reference, reference.width / 2, reference.height / 2) != RGB(1, 2, 3) &&
          PixelAt(&probe.popup, probe.popup.width / 2, probe.popup.height / 2) != RGB(1, 2, 3));
    Check("combo input: an expanded choice has the full Actions pressed pixels", probe.face);
    if (reference.dc && probe.popup.dc && reference.width == probe.popup.width && reference.height == probe.popup.height) {
        for (y = 0; y < reference.height; y++)
            for (x = 0; x < reference.width; x++)
                if (PixelAt(&reference, x, y) != PixelAt(&probe.popup, x, y)) differ++;
        Check("drop-down menu: Actions and choices have identical native popup pixels", differ == 0);
        if (differ) printf("        %d of %d popup pixels differ\n", differ, reference.width * reference.height);
    } else Check("drop-down menu: Actions and choices have identical popup dimensions", FALSE);
    CanvasClose(&reference);
    Check("combo input: Escape cancels once and leaves the model unchanged", probe.cancelled == 1 && probe.closed == 1 && probe.confirmed == 0 &&
          !probe.endedByProbe && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1 && !SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0));

    ResetChoiceProbe(&probe, combo, kEnter, NULL, ARRAYSIZE(kEnter));
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    AwaitChoice(&probe);
    Check("combo input: Return without an active menu item follows native cancellation", probe.cancelled == 1 && probe.confirmed == 0 &&
          probe.changes == 0 && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1);

    ResetChoiceProbe(&probe, combo, kSelectFirst, NULL, ARRAYSIZE(kSelectFirst));
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    AwaitChoice(&probe);
    Check("combo input: arrow then Return commits the actual native menu item once", SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0 &&
          probe.confirmed == 1 && probe.changes == 1 && probe.cancelled == 0 && probe.opened == 1 && probe.closed == 1);

    ResetChoiceProbe(&probe, combo, kSelectSecond, NULL, ARRAYSIZE(kSelectSecond));
    SendMessageW(combo, WM_KEYDOWN, VK_F4, 0);
    AwaitChoice(&probe);
    Check("combo input: F4 opens the same menu and arrow navigation selects the next value", SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1 &&
          probe.confirmed == 1 && probe.changes == 1);

    ResetChoiceProbe(&probe, combo, kTypeGreen, kCharacter, ARRAYSIZE(kTypeGreen));
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    AwaitChoice(&probe);
    Check("combo input: native type-ahead activates the matching choice", SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1 &&
          probe.confirmed == 1 && probe.changes == 0);

    ResetChoiceProbe(&probe, combo, kF4ThenEscape, NULL, ARRAYSIZE(kF4ThenEscape));
    SendMessageW(combo, WM_KEYDOWN, VK_F4, 0);
    AwaitChoice(&probe);
    Check("combo input: an expanded menu ignores F4 and closes on Escape", probe.nextKey == 2 && probe.cancelled == 1 && !probe.endedByProbe);

    ResetChoiceProbe(&probe, combo, kTab, NULL, ARRAYSIZE(kTab));
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    {
        MSG key = { 0 };
        key.hwnd = combo;
        key.message = WM_KEYDOWN;
        key.wParam = VK_TAB;
        Check("combo input: Tab remains available to dialog focus navigation", (SendMessageW(combo, WM_GETDLGCODE, VK_TAB, (LPARAM)&key) & DLGC_WANTMESSAGE) == 0);
    }
    SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
    PumpMessages();
    Check("combo input: a pending expansion can be cancelled before its menu shows", probe.idle == 0 && probe.cancelled == 1 && probe.closed == 1);

    ResetChoiceProbe(&probe, combo, kEscape, NULL, ARRAYSIZE(kEscape));
    SendMessageW(combo, WM_SYSKEYDOWN, VK_DOWN, 1L << 29);
    AwaitChoice(&probe);
    Check("combo input: Alt+Down opens the shared native menu", probe.menuWindow && probe.cancelled == 1 && probe.opened == 1);
    ResetChoiceProbe(&probe, combo, kEscape, NULL, ARRAYSIZE(kEscape));
    SendMessageW(combo, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(combo, WM_CHAR, L' ', 0);
    AwaitChoice(&probe);
    Check("combo input: Space opens the shared native menu", probe.menuWindow && probe.opened == 1 && probe.cancelled == 1);
    ResetChoiceProbe(&probe, combo, kEscape, NULL, ARRAYSIZE(kEscape));
    SendMessageW(combo, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(5, 5));
    AwaitChoice(&probe);
    Check("combo input: a double click uses the shared native menu", probe.menuWindow && probe.opened == 1 && probe.cancelled == 1);
    info.cbSize = sizeof info;
    Check("combo input: native combo information remains available", GetComboBoxInfo(combo, &info));
    Check("combo input: the unused native combo list stays hidden", !IsWindowVisible(info.hwndList));
    SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &animationAfter, 0);
    SystemParametersInfoW(SPI_GETMENUFADE, 0, &fadeAfter, 0);
    Check("drop-down menu: the user's animation and fade preferences stay unchanged", animationBefore == animationAfter && fadeBefore == fadeAfter);
    CanvasClose(&probe.popup);
    ShowWindow(host, SW_HIDE);
    RemoveWindowSubclass(host, ProbeChoiceNotifications, CHOICE_PROBE_SUBCLASS);
    DestroyWindow(combo);
    DeleteObject(font);
}

/* ---------------------------------------------------------- scroll bars */

static HWND FilledTree(HWND host, int x, int rows, BOOL framed)
{
    HWND tree = CreateWindowExW(framed ? WS_EX_CLIENTEDGE : 0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_NOHSCROLL, x, 10, 200, 120,
                                host, NULL, GetModuleHandleW(NULL), NULL);
    TVINSERTSTRUCTW insert;
    int i;
    ZeroMemory(&insert, sizeof insert);
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = (LPWSTR)L"Row";
    for (i = 0; tree && i < rows; i++) TreeView_InsertItem(tree, &insert);
    return tree;
}

/* The control's whole window, as drawn. */
static BOOL CaptureWholeWindow(HWND window, Canvas *canvas)
{
    RECT frame, area;
    GetWindowRect(window, &frame);
    SetRect(&area, 0, 0, frame.right - frame.left, frame.bottom - frame.top);
    return CanvasOpen(canvas, area.right, area.bottom, RGB(1, 2, 3)) && CopyFromWindow(window, TRUE, &area, canvas, 0, 0);
}

/* A dark tree (as the lists and list boxes) has no frame, so its scroll bar
 * reaches the edges of its window: made with a frame, it shows it as a tree
 * made without one does, to the pixel. A light one keeps the frame it was
 * made with. */
static void TestScrollBarEdges(void)
{
    HWND host = ThemedHost(), framed = FilledTree(host, 10, 40, TRUE), bare = FilledTree(host, 250, 40, FALSE);
    SCROLLBARINFO bar;
    Canvas framedImage = { 0 }, bareImage = { 0 };
    RECT framedWindow, bareWindow;
    int x, y, width, height, differ = 0;
    if (!framed || !bare) {
        Check("scroll bar: trees created", FALSE);
        if (framed) DestroyWindow(framed);
        if (bare) DestroyWindow(bare);
        return;
    }
    Theme_Apply(host);
    if (!Theme_IsDark()) {
        Check("a light tree keeps its frame", (GetWindowLongW(framed, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0);
        SkipInLight("a dark tree's scroll bar at its edges");
        DestroyWindow(framed);
        DestroyWindow(bare);
        return;
    }
    ZeroMemory(&bar, sizeof bar);
    bar.cbSize = sizeof bar;
    GetWindowRect(framed, &framedWindow);
    if (!GetScrollBarInfo(framed, OBJID_VSCROLL, &bar) || (bar.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
        Check("scroll bar: the tree shows one", FALSE);
    } else {
        Check("a dark tree's scroll bar reaches the right, top and bottom edges",
              bar.rcScrollBar.right == framedWindow.right && bar.rcScrollBar.top == framedWindow.top && bar.rcScrollBar.bottom == framedWindow.bottom);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
        GetWindowRect(bare, &bareWindow);
        width = bar.rcScrollBar.right - bar.rcScrollBar.left;
        height = bar.rcScrollBar.bottom - bar.rcScrollBar.top;
        if (CaptureWholeWindow(framed, &framedImage) && CaptureWholeWindow(bare, &bareImage)) {
            if (HasImage(&framedImage, "scroll bar edges")) {
                /* The ring around the scroll bar, where a frame would show. */
                for (y = 0; y < height; y++)
                    for (x = 0; x < width; x++) {
                        if (y > 1 && y < height - 2 && x < width - 2) continue;
                        if (PixelAt(&framedImage, bar.rcScrollBar.left - framedWindow.left + x, bar.rcScrollBar.top - framedWindow.top + y) !=
                            PixelAt(&bareImage, bareWindow.right - bareWindow.left - width + x, y))
                            differ++;
                    }
                Check("a dark tree made with a frame shows none around its scroll bar, as a tree made without one", differ == 0);
                if (differ) printf("        %d pixels differ around the scroll bar\n", differ);
            }
        } else Check("scroll bar: trees captured", FALSE);
        CanvasClose(&framedImage);
        CanvasClose(&bareImage);
        ShowWindow(host, SW_HIDE);
    }
    DestroyWindow(framed);
    DestroyWindow(bare);
}

/* A scroll bar's place in its window's canvas. */
static RECT BarInWindow(HWND window, const SCROLLBARINFO *bar)
{
    RECT frame, place = bar->rcScrollBar;
    GetWindowRect(window, &frame);
    OffsetRect(&place, -frame.left, -frame.top);
    return place;
}

/* Both axes, including an axis made after Theme_Apply, are measured on the
 * native non-client surface rather than inferred from a theme name. */
static void TestScrollBarColors(void)
{
    static const char *const kKinds[] = { "list", "tree", "list box", "edit", "view" };
    HWND host = ThemedHost();
    HFONT font = DialogFont();
    int kind;
    if (SkipInLight("scroll bar colors")) {
        DeleteObject(font);
        return;
    }
    SetWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS, 0);
    for (kind = 0; kind < (int)ARRAYSIZE(kKinds); kind++) {
        HWND control = NULL;
        int stage, axis, i;
        if (kind == 0 || kind == 4) {
            LVCOLUMNW column = { 0 };
            LVITEMW item = { 0 };
            control = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 180, 100, host, NULL, GetModuleHandleW(NULL), NULL);
            column.mask = LVCF_WIDTH | LVCF_TEXT;
            column.cx = 100;
            column.pszText = (LPWSTR)L"";
            ListView_InsertColumn(control, 0, &column);
            item.mask = LVIF_TEXT;
            item.pszText = (LPWSTR)L"";
            for (i = 0; i < 20; i++) {
                item.iItem = i;
                ListView_InsertItem(control, &item);
            }
            if (kind == 4) control = Theme_SmoothView(control);
        } else if (kind == 1) {
            TVINSERTSTRUCTW item = { 0 };
            control = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_LINESATROOT, 10, 10, 180, 100,
                                      host, NULL, GetModuleHandleW(NULL), NULL);
            item.hParent = TVI_ROOT;
            item.hInsertAfter = TVI_LAST;
            item.item.mask = TVIF_TEXT;
            item.item.pszText = (LPWSTR)L"Short";
            for (i = 0; i < 20; i++) TreeView_InsertItem(control, &item);
        } else if (kind == 2) {
            control = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 180, 100,
                                      host, NULL, GetModuleHandleW(NULL), NULL);
            for (i = 0; i < 20; i++) SendMessageW(control, LB_ADDSTRING, 0, (LPARAM)L"Short");
        } else {
            control = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 10, 10, 180, 100,
                                      host, NULL, GetModuleHandleW(NULL), NULL);
            SetWindowTextW(control, L"Line\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine\r\nLine");
        }
        if (!control) {
            Check("scroll bar color: private control created", FALSE);
            continue;
        }
        SendMessageW(control, WM_SETFONT, (WPARAM)font, FALSE);
        Theme_Apply(host);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        /* A view never scrolls sideways (its table does): it has one stage. */
        for (stage = 0; stage < (kind == 4 ? 1 : 2); stage++) {
            if (stage) {
                if (kind == 0) ListView_SetColumnWidth(control, 0, 600);
                else if (kind == 1) {
                    TVITEMW item = { 0 };
                    item.mask = TVIF_HANDLE | TVIF_TEXT;
                    item.hItem = TreeView_GetRoot(control);
                    item.pszText = (LPWSTR)L"A very long tree label that exceeds the viewport and creates a horizontal scroll bar";
                    TreeView_SetItem(control, &item);
                } else {
                    SCROLLINFO range = { sizeof range, SIF_RANGE | SIF_PAGE | SIF_POS, 0, 600, 180, 0, 0 };
                    SetWindowLongW(control, GWL_STYLE, GetWindowLongW(control, GWL_STYLE) | WS_HSCROLL);
                    SetWindowPos(control, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                    if (kind == 2) SendMessageW(control, LB_SETHORIZONTALEXTENT, 600, 0);
                    else SetScrollInfo(control, SB_HORZ, &range, TRUE);
                }
            }
            PumpMessages();
            RedrawWindow(control, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
            for (axis = 0; axis < 2; axis++) {
                SCROLLBARINFO bar = { 0 };
                Canvas canvas = { 0 };
                RECT place;
                int bright, samples;
                char label[160];
                bar.cbSize = sizeof bar;
                if (!GetScrollBarInfo(control, axis ? OBJID_HSCROLL : OBJID_VSCROLL, &bar) || (bar.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
                    /* Every kind is taller than its window; only the horizontal axis comes later. */
                    if (!axis || stage) {
                        StringCchPrintfA(label, ARRAYSIZE(label), "scroll bar color: a %s %s the %s axis", kKinds[kind],
                                         axis ? "adds" : "shows", axis ? "horizontal" : "vertical");
                        Check(label, FALSE);
                    }
                    continue;
                }
                if (!CaptureWholeWindow(control, &canvas)) {
                    Check("scroll bar color: control captured", FALSE);
                    CanvasClose(&canvas);
                    continue;
                }
                place = BarInWindow(control, &bar);
                StringCchPrintfA(label, ARRAYSIZE(label), "scroll bar color: a %s's %s scroll bar%s has a dark native surface", kKinds[kind],
                                 axis ? "horizontal" : "vertical", !stage ? "" : axis ? " made after theming" : " beside the horizontal one");
                Check(label, MostlyDark(&canvas, &place, &bright, &samples));
                if (bright * 4 >= samples) printf("        bright=%d/%d\n", bright, samples);
                CanvasClose(&canvas);
            }
        }
        {
            SCROLLBARINFO vertical = { 0 }, horizontal = { 0 };
            vertical.cbSize = sizeof vertical;
            horizontal.cbSize = sizeof horizontal;
            if (GetScrollBarInfo(control, OBJID_VSCROLL, &vertical) && GetScrollBarInfo(control, OBJID_HSCROLL, &horizontal) &&
                !(vertical.rgstate[0] & STATE_SYSTEM_INVISIBLE) && !(horizontal.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
                Canvas canvas = { 0 };
                RECT across = BarInWindow(control, &vertical), along = BarInWindow(control, &horizontal), corner;
                int bright, samples;
                char label[160];
                SetRect(&corner, across.left, along.top, across.right, along.bottom);
                StringCchPrintfA(label, ARRAYSIZE(label), "scroll bar color: a %s has a dark corner between its scroll bars", kKinds[kind]);
                Check(label, CaptureWholeWindow(control, &canvas) && MostlyDark(&canvas, &corner, &bright, &samples));
                CanvasClose(&canvas);
            }
        }
        ShowWindow(host, SW_HIDE);
        DestroyWindow(control);
    }
    {
        HWND list, view;
        LVCOLUMNW column = { 0 };
        LVITEMW item = { 0 };
        SCROLLBARINFO vertical = { 0 }, horizontal = { 0 };
        int i;
        list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 180, 100, host, NULL, GetModuleHandleW(NULL), NULL);
        SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = (LPWSTR)L"";
        column.cx = 100;
        ListView_InsertColumn(list, 0, &column);
        item.mask = LVIF_TEXT;
        item.pszText = (LPWSTR)L"";
        ListView_InsertItem(list, &item);
        view = Theme_SmoothView(list);
        Theme_Apply(host);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        for (i = 1; i < 40; i++) {
            item.iItem = i;
            ListView_InsertItem(list, &item);
        }
        ReportDividerDrag(list, 0, 600);
        PumpMessages();
        RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
        /* Grown wider than its view, the list scrolls itself: both its
         * scroll bars, made after theming, at the view's edges. */
        vertical.cbSize = sizeof vertical;
        horizontal.cbSize = sizeof horizontal;
        Check("scroll bar color: growth gives a list wider than its view both scroll bars after theming",
              GetScrollBarInfo(list, OBJID_VSCROLL, &vertical) && !(vertical.rgstate[0] & STATE_SYSTEM_INVISIBLE) &&
              GetScrollBarInfo(list, OBJID_HSCROLL, &horizontal) && !(horizontal.rgstate[0] & STATE_SYSTEM_INVISIBLE));
        {
            RECT viewport, place;
            Canvas canvas = { 0 };
            int bright, samples;
            GetWindowRect(view, &viewport);
            Check("scroll bar color: the horizontal axis shows at the viewport's bottom without scrolling",
                  horizontal.rcScrollBar.bottom == viewport.bottom && horizontal.rcScrollBar.top >= viewport.top &&
                  !(horizontal.rgstate[0] & STATE_SYSTEM_OFFSCREEN));
            place = BarInWindow(list, &horizontal);
            Check("scroll bar color: the horizontal axis of a list wider than its view is dark",
                  CaptureWholeWindow(list, &canvas) && MostlyDark(&canvas, &place, &bright, &samples));
            CanvasClose(&canvas);
            place = BarInWindow(list, &vertical);
            Check("scroll bar color: the vertical axis of a list wider than its view is dark",
                  CaptureWholeWindow(list, &canvas) && MostlyDark(&canvas, &place, &bright, &samples));
            CanvasClose(&canvas);
        }
        ShowWindow(host, SW_HIDE);
        DestroyWindow(view);
    }
    RemoveWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS);
    DeleteObject(font);
}

/* ------------------------------------------------------------- the wheel */

static void SendWheelNotches(HWND window, int notches)
{
    RECT frame;
    GetWindowRect(window, &frame);
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA * notches), MAKELPARAM((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2));
}

static BOOL WheelScrollsLines(UINT *lines, const char *test)
{
    char line[160];
    *lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, lines, 0);
    if (*lines != WHEEL_PAGESCROLL && *lines != 0) return TRUE;
    StringCchPrintfA(line, ARRAYSIZE(line), "%s: the wheel scrolls by pages here", test);
    Skip(line);
    return FALSE;
}

/* Where a list box's and a tree's wheel animations are to end (their top rows). */
typedef struct WheelTargets { HWND box, tree; int boxTop, treeTop; } WheelTargets;

static BOOL WheelTargetsReached(const void *context)
{
    const WheelTargets *targets = (const WheelTargets *)context;
    return (int)SendMessageW(targets->box, LB_GETTOPINDEX, 0, 0) == targets->boxTop && GetScrollPos(targets->tree, SB_VERT) == targets->treeTop;
}

/* A wheel notch scrolls a list box and a tree their lines, their scroll bars
 * with them, and the way back ends at the top. */
static void TestWheel(void)
{
    HWND host = ThemedHost(), box, tree;
    HTREEITEM top;
    WheelTargets targets;
    UINT lines;
    int i;
    box = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 200, 120, host, NULL,
                          GetModuleHandleW(NULL), NULL);
    tree = FilledTree(host, 250, 40, FALSE);
    if (!box || !tree) {
        Check("wheel: controls created", FALSE);
        if (box) DestroyWindow(box);
        if (tree) DestroyWindow(tree);
        return;
    }
    for (i = 0; i < 40; i++) SendMessageW(box, LB_ADDSTRING, 0, (LPARAM)L"Row");
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    if (WheelScrollsLines(&lines, "wheel")) {
        top = TreeView_GetFirstVisible(tree);
        targets.box = box;
        targets.tree = tree;
        targets.boxTop = targets.treeTop = (int)lines;
        SendWheelNotches(box, 1);
        SendWheelNotches(tree, 1);
        Check("a wheel notch scrolls a list box and a tree their lines", PumpUntil(WheelTargetsReached, &targets, ANIMATION_TIMEOUT_MS));
        if (!WheelTargetsReached(&targets))
            printf("        list box at row %d, tree at row %d, expected %u\n", (int)SendMessageW(box, LB_GETTOPINDEX, 0, 0), GetScrollPos(tree, SB_VERT), lines);
        Check("a wheel notch moves a list box's scroll bar with its rows", GetScrollPos(box, SB_VERT) == (int)SendMessageW(box, LB_GETTOPINDEX, 0, 0));
        Check("a wheel notch moves a tree's first row with its scroll bar", TreeView_GetFirstVisible(tree) != top);
        targets.boxTop = targets.treeTop = 0;
        SendWheelNotches(box, -5);
        SendWheelNotches(tree, -5);
        Check("the wheel takes a list box and a tree back up, to the top",
              PumpUntil(WheelTargetsReached, &targets, ANIMATION_TIMEOUT_MS) && GetScrollPos(box, SB_VERT) == 0 && TreeView_GetFirstVisible(tree) == top);
    }
    ShowWindow(host, SW_HIDE);
    DestroyWindow(box);
    DestroyWindow(tree);
}

/* ---------------------------------------------------------------- views */

typedef struct ViewedControls { HWND controls[3], views[3]; } ViewedControls;

static const char *const kViewedNames[] = { "a list box", "a list view", "a tree" };

/* A list box, a list view and a tree with `rows` rows each, in smooth views. */
static BOOL CreateViewedControls(HWND host, int rows, ViewedControls *viewed)
{
    LVCOLUMNW column;
    LVITEMW item;
    int i, kind;
    ZeroMemory(viewed, sizeof *viewed);
    viewed->controls[0] = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                          10, 10, 140, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    viewed->controls[1] = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 160, 10, 140, 120,
                                          host, NULL, GetModuleHandleW(NULL), NULL);
    viewed->controls[2] = FilledTree(host, 310, rows, FALSE);
    if (!viewed->controls[0] || !viewed->controls[1] || !viewed->controls[2]) {
        for (kind = 0; kind < 3; kind++) if (viewed->controls[kind]) DestroyWindow(viewed->controls[kind]);
        return FALSE;
    }
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 120;
    column.pszText = (LPWSTR)L"Name";
    ListView_InsertColumn(viewed->controls[1], 0, &column);
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"Row";
    for (i = 0; i < rows; i++) {
        SendMessageW(viewed->controls[0], LB_ADDSTRING, 0, (LPARAM)L"Row");
        item.iItem = i;
        ListView_InsertItem(viewed->controls[1], &item);
    }
    for (kind = 0; kind < 3; kind++) viewed->views[kind] = Theme_SmoothView(viewed->controls[kind]);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    PumpMessages();   /* the views measure what they hold */
    return TRUE;
}

static void DestroyViewedControls(ViewedControls *viewed)
{
    int kind;
    for (kind = 0; kind < 3; kind++) DestroyWindow(viewed->views[kind] ? viewed->views[kind] : viewed->controls[kind]);
}

static int RowHeight(HWND control, int kind)
{
    RECT row;
    if (kind == 0) return (int)SendMessageW(control, LB_GETITEMHEIGHT, 0, 0);
    if (kind == 1) return ListView_GetItemRect(control, 0, &row, LVIR_BOUNDS) ? row.bottom - row.top : 0;
    return TreeView_GetItemHeight(control);
}

static int ViewPosition(HWND view)
{
    SCROLLINFO scroll;
    ZeroMemory(&scroll, sizeof scroll);
    scroll.cbSize = sizeof scroll;
    scroll.fMask = SIF_POS;
    return GetScrollInfo(view, SB_VERT, &scroll) ? scroll.nPos : -1;
}

/* A view's wheel animation, frame by frame: each frame a place the view
 * scrolled to (the first at once, from the wheel itself). */
typedef struct FrameSample { HWND view; int row, target, frames, last; BOOL betweenRows; } FrameSample;

static void SampleFrame(FrameSample *sample)
{
    int position = ViewPosition(sample->view);
    sample->frames++;
    sample->last = position;
    if (sample->row > 0 && position % sample->row != 0) sample->betweenRows = TRUE;
}

/* Installed after the view's own subclass: it sees each frame once drawn,
 * whatever message brought it. */
static LRESULT CALLBACK SampleViewFrames(HWND view, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    LRESULT result = DefSubclassProc(view, message, wp, lp);
    FrameSample *sample = (FrameSample *)reference;
    (void)id;
    (void)message;
    if (ViewPosition(view) != sample->last) SampleFrame(sample);
    return result;
}

static BOOL ViewReached(const void *context)
{
    const FrameSample *sample = (const FrameSample *)context;
    return ViewPosition(sample->view) == sample->target;
}

/* A list box, a list view and a tree in smooth views scroll alike, by the
 * pixel: a wheel notch covers its lines exactly, through frames between
 * rows; the control moves in its view; a list view's header stays on top;
 * the keyboard's row is scrolled into sight. */
static void TestSmoothView(void)
{
    HWND host = ThemedHost();
    ViewedControls viewed;
    char name[160];
    UINT lines;
    int kind;
    if (!CreateViewedControls(host, 40, &viewed)) {
        Check("smooth view: controls created", FALSE);
        return;
    }
    for (kind = 0; kind < 3; kind++) {
        StringCchPrintfA(name, ARRAYSIZE(name), "%s in a view is as tall as its rows, with no scroll bar of its own", kViewedNames[kind]);
        Check(name, !(GetWindowLongW(viewed.controls[kind], GWL_STYLE) & WS_VSCROLL));
    }
    if (WheelScrollsLines(&lines, "smooth view")) {
        for (kind = 0; kind < 3; kind++) {
            RECT view, control, header;
            FrameSample sample = { 0 };
            int row = RowHeight(viewed.controls[kind], kind), position;
            BOOL reached;
            sample.view = viewed.views[kind];
            sample.row = row;
            sample.target = (int)lines * row;
            SetWindowSubclass(sample.view, SampleViewFrames, FRAME_SAMPLE_SUBCLASS, (DWORD_PTR)&sample);
            SendMessageW(viewed.controls[kind], WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
            SampleFrame(&sample);
            reached = PumpUntil(ViewReached, &sample, ANIMATION_TIMEOUT_MS);
            RemoveWindowSubclass(sample.view, SampleViewFrames, FRAME_SAMPLE_SUBCLASS);
            position = ViewPosition(viewed.views[kind]);
            StringCchPrintfA(name, ARRAYSIZE(name), "a wheel notch scrolls %s in a view its lines exactly", kViewedNames[kind]);
            Check(name, row > 0 && reached && position == sample.target);
            if (row > 0 && position != sample.target) printf("        at %d px, rows of %d px\n", position, row);
            StringCchPrintfA(name, ARRAYSIZE(name), "a wheel notch scrolls %s in a view by the pixel, through frames between rows", kViewedNames[kind]);
            Check(name, sample.betweenRows);
            if (!sample.betweenRows) printf("        %d frames, each on a row's edge\n", sample.frames);
            GetClientRect(viewed.views[kind], &view);
            MapWindowPoints(viewed.views[kind], NULL, (POINT *)&view, 2);
            GetWindowRect(viewed.controls[kind], &control);
            StringCchPrintfA(name, ARRAYSIZE(name), "a wheel notch moves %s in its view", kViewedNames[kind]);
            Check(name, control.top == view.top - position);
            if (kind == 1 && GetWindowRect(ListView_GetHeader(viewed.controls[1]), &header))
                Check("a list view's header stays on top of its scrolled view", header.top == view.top);
        }
        SendMessageW(viewed.controls[0], WM_KEYDOWN, VK_END, 0);
        PumpMessages();
        {
            SCROLLINFO scroll;
            ZeroMemory(&scroll, sizeof scroll);
            scroll.cbSize = sizeof scroll;
            scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            GetScrollInfo(viewed.views[0], SB_VERT, &scroll);
            Check("the keyboard's row is scrolled into sight", scroll.nPos == scroll.nMax - (int)scroll.nPage + 1);
        }
    }
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
}

/* A list view wider than its view (a column wider than it) scrolls itself:
 * both its scroll bars show at the view's edges without scrolling, and the
 * rows on top stay on top as it starts and stops. */
static void TestWideListInView(void)
{
    HWND host = ThemedHost(), list, view;
    LVCOLUMNW column;
    LVITEMW item;
    SCROLLBARINFO horizontal, vertical;
    RECT first, client;
    int i, row;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 200, 140, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("wide list: private list created", FALSE);
        return;
    }
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 80;
    column.pszText = (LPWSTR)L"Name";
    ListView_InsertColumn(list, 0, &column);
    ListView_InsertColumn(list, 1, &column);
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"Row";
    for (i = 0; i < 40; i++) {
        item.iItem = i;
        ListView_InsertItem(list, &item);
    }
    view = Theme_SmoothView(list);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    PumpMessages();
    if (!ListView_GetItemRect(list, 0, &first, LVIR_BOUNDS) || (row = first.bottom - first.top) <= 0) {
        Check("wide list: rows measured", FALSE);
        ShowWindow(host, SW_HIDE);
        DestroyWindow(view);
        return;
    }
    SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 5 * row), 0);
    Theme_SetColumnWidth(list, 0, 400);
    PumpMessages();
    GetClientRect(view, &client);
    MapWindowPoints(view, NULL, (POINT *)&client, 2);
    ZeroMemory(&horizontal, sizeof horizontal);
    horizontal.cbSize = sizeof horizontal;
    vertical = horizontal;
    Check("a list wider than its view shows its horizontal scroll bar at the view's bottom edge",
          GetScrollBarInfo(list, OBJID_HSCROLL, &horizontal) && !(horizontal.rgstate[0] & STATE_SYSTEM_INVISIBLE) &&
          horizontal.rcScrollBar.bottom == client.bottom);
    Check("a list wider than its view shows its vertical scroll bar at the view's right edge",
          GetScrollBarInfo(list, OBJID_VSCROLL, &vertical) && !(vertical.rgstate[0] & STATE_SYSTEM_INVISIBLE) &&
          vertical.rcScrollBar.right == client.right);
    Check("a list keeps its rows on top as it starts scrolling itself", ListView_GetTopIndex(list) == 5);
    Theme_SetColumnWidth(list, 0, 80);
    PumpMessages();
    Check("a list that fits its view again is as tall as its rows, scrolled by the view",
          !(GetWindowLongW(list, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)) && ListView_GetTopIndex(list) == 0);
    Check("a list keeps its rows on top as the view scrolls it again", GetScrollPos(view, SB_VERT) == 5 * row);
    ShowWindow(host, SW_HIDE);
    DestroyWindow(view);
}

typedef struct SelectionCount { HWND control; int changes; } SelectionCount;

/* Selection notifications of the control, as the dialog gets them (the
 * view passes them on). */
static LRESULT CALLBACK CountSelections(HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    SelectionCount *count = (SelectionCount *)reference;
    (void)id;
    if (message == WM_COMMAND && (HWND)lp == count->control && HIWORD(wp) == LBN_SELCHANGE) count->changes++;
    if (message == WM_NOTIFY && lp && ((const NMHDR *)lp)->hwndFrom == count->control) {
        const NMHDR *header = (const NMHDR *)lp;
        if (header->code == TVN_SELCHANGEDW) count->changes++;
        if (header->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)) count->changes++;
        }
    }
    return DefSubclassProc(host, message, wp, lp);
}

/* The index of the row the keyboard is on, and its rectangle in the control. */
static int KeyboardRow(HWND control, int kind, RECT *row)
{
    int index = -1;
    SetRectEmpty(row);
    if (kind == 0) {
        index = (int)SendMessageW(control, LB_GETCURSEL, 0, 0);
        if (index >= 0) SendMessageW(control, LB_GETITEMRECT, (WPARAM)index, (LPARAM)row);
    } else if (kind == 1) {
        index = ListView_GetNextItem(control, -1, LVNI_FOCUSED | LVNI_SELECTED);
        if (index >= 0) ListView_GetItemRect(control, index, row, LVIR_BOUNDS);
    } else {
        HTREEITEM item, selected = TreeView_GetSelection(control);
        for (item = TreeView_GetRoot(control); item; item = TreeView_GetNextVisible(control, item)) {
            index++;
            if (item == selected) break;
        }
        if (!item) index = -1;
        else TreeView_GetItemRect(control, item, row, FALSE);
    }
    return index;
}

static void SelectRow(HWND control, int kind, int index)
{
    if (kind == 0) SendMessageW(control, LB_SETCURSEL, (WPARAM)index, 0);
    else if (kind == 1) {
        ListView_SetItemState(control, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(control, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else {
        HTREEITEM item = TreeView_GetRoot(control);
        while (item && index-- > 0) item = TreeView_GetNextVisible(control, item);
        TreeView_SelectItem(control, item);
    }
}

/* Page Down and Page Up in a list box, a list view and a tree in views move
 * the selection by one view's height (as many rows as show, less one, as
 * a list that scrolls itself does), with one selection notification, and
 * scroll the new row into sight. */
static void TestViewPaging(void)
{
    static const struct { UINT key; int pages; const char *name; } kPresses[] = {
        { VK_NEXT, 1, "Page Down" }, { VK_NEXT, 2, "a second Page Down" }, { VK_PRIOR, 1, "Page Up" }
    };
    HWND host = ThemedHost();
    ViewedControls viewed;
    int kind;
    size_t press;
    if (!CreateViewedControls(host, 100, &viewed)) {
        Check("view paging: controls created", FALSE);
        return;
    }
    for (kind = 0; kind < 3; kind++) {
        SelectionCount count;
        RECT client, row;
        int rowHeight = RowHeight(viewed.controls[kind], kind), page, header = 0;
        if (kind == 1 && GetWindowRect(ListView_GetHeader(viewed.controls[1]), &row)) header = row.bottom - row.top;
        GetClientRect(viewed.views[kind], &client);
        page = rowHeight > 0 ? max(1, (client.bottom - header) / rowHeight - 1) : 1;
        SelectRow(viewed.controls[kind], kind, 0);
        PumpMessages();
        count.control = viewed.controls[kind];
        SetWindowSubclass(host, CountSelections, SELECTION_COUNT_SUBCLASS, (DWORD_PTR)&count);
        for (press = 0; press < ARRAYSIZE(kPresses); press++) {
            char name[200];
            int index, position;
            count.changes = 0;
            SendMessageW(viewed.controls[kind], WM_KEYDOWN, kPresses[press].key, 0);
            SendMessageW(viewed.controls[kind], WM_KEYUP, kPresses[press].key, 0);
            PumpMessages();
            index = KeyboardRow(viewed.controls[kind], kind, &row);
            position = ViewPosition(viewed.views[kind]);
            StringCchPrintfA(name, ARRAYSIZE(name), "view paging: %s in %s's view moves the selection one view height", kPresses[press].name, kViewedNames[kind]);
            Check(name, index == kPresses[press].pages * page);
            if (index != kPresses[press].pages * page) printf("        row %d, expected %d (view %ld px, rows %d px)\n", index, kPresses[press].pages * page,
                                                              client.bottom - header, rowHeight);
            StringCchPrintfA(name, ARRAYSIZE(name), "view paging: %s in %s's view notifies one selection change", kPresses[press].name, kViewedNames[kind]);
            Check(name, count.changes == 1);
            StringCchPrintfA(name, ARRAYSIZE(name), "view paging: %s in %s's view shows the selected row whole", kPresses[press].name, kViewedNames[kind]);
            Check(name, row.top >= position + header && row.bottom <= position + client.bottom);
        }
        RemoveWindowSubclass(host, CountSelections, SELECTION_COUNT_SUBCLASS);
        {
            /* The view's own scroll bar pages by what shows under a list view's header. */
            char name[200];
            SendMessageW(viewed.views[kind], WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 0), 0);
            SendMessageW(viewed.views[kind], WM_VSCROLL, SB_PAGEDOWN, 0);
            StringCchPrintfA(name, ARRAYSIZE(name), "view paging: a page down from %s's scroll bar is its view's height under any header", kViewedNames[kind]);
            Check(name, ViewPosition(viewed.views[kind]) == client.bottom - header);
            SendMessageW(viewed.views[kind], WM_VSCROLL, SB_PAGEUP, 0);
            StringCchPrintfA(name, ARRAYSIZE(name), "view paging: a page up from %s's scroll bar goes back as far", kViewedNames[kind]);
            Check(name, ViewPosition(viewed.views[kind]) == 0);
        }
    }
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
}

/* A view keeps its place when its control is refilled, when a tree's folder
 * opens by its arrow, and when the view is resized; the program setting the
 * selection brings that row into sight. */
static void TestViewKeepsPlace(void)
{
    HWND host = ThemedHost(), box, view, tree, treeView;
    TVINSERTSTRUCTW insert;
    HTREEITEM folder = NULL, opened = NULL;
    RECT button;
    int i, folders;
    box = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 10, 10, 160, 120,
                          host, NULL, GetModuleHandleW(NULL), NULL);
    tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_LINESATROOT, 200, 10, 200, 120,
                           host, NULL, GetModuleHandleW(NULL), NULL);
    if (!box || !tree) {
        Check("view place: controls created", FALSE);
        if (box) DestroyWindow(box);
        if (tree) DestroyWindow(tree);
        return;
    }
    for (i = 0; i < 100; i++) SendMessageW(box, LB_ADDSTRING, 0, (LPARAM)L"Row");
    ZeroMemory(&insert, sizeof insert);
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    for (folders = 0; folders < 30; folders++) {
        insert.hParent = TVI_ROOT;
        insert.item.pszText = (LPWSTR)L"Folder";
        folder = TreeView_InsertItem(tree, &insert);
        if (folders == 12) opened = folder;
        insert.hParent = folder;
        insert.item.pszText = (LPWSTR)L"Session";
        for (i = 0; i < 3; i++) TreeView_InsertItem(tree, &insert);
    }
    view = Theme_SmoothView(box);
    treeView = Theme_SmoothView(tree);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    SendMessageW(box, LB_SETCURSEL, 0, 0);
    TreeView_SelectItem(tree, TreeView_GetRoot(tree));
    PumpMessages();
    SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 400), 0);
    PumpMessages();
    Check("view place: the list box's view starts scrolled away from its selection", ViewPosition(view) == 400);
    SendMessageW(box, WM_SETREDRAW, FALSE, 0);
    SendMessageW(box, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < 100; i++) SendMessageW(box, LB_ADDSTRING, 0, (LPARAM)L"Refilled row");
    SendMessageW(box, WM_SETREDRAW, TRUE, 0);
    PumpMessages();
    Check("view place: a refill keeps the view where it was", ViewPosition(view) == 400);
    SetWindowPos(view, NULL, 0, 0, 160, 150, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    Check("view place: a resize keeps the view where it was", ViewPosition(view) == 400);
    SendMessageW(box, LB_SETCURSEL, 0, 0);
    PumpMessages();
    Check("view place: a selection the program sets is brought into sight", ViewPosition(view) == 0);

    if (TreeView_GetItemRect(tree, opened, &button, TRUE)) {
        int position;
        SendMessageW(treeView, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, max(0, button.top - 40)), 0);
        PumpMessages();
        position = ViewPosition(treeView);
        Check("view place: the tree's view starts scrolled away from its selection", position > 0);
        TreeView_GetItemRect(tree, opened, &button, TRUE);
        /* The folder's arrow, left of its label; the click's own button-up comes first if the tree waits for it. */
        PostMessageW(tree, WM_LBUTTONUP, 0, MAKELPARAM(button.left - TreeView_GetIndent(tree) / 2, (button.top + button.bottom) / 2));
        SendMessageW(tree, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(button.left - TreeView_GetIndent(tree) / 2, (button.top + button.bottom) / 2));
        PumpMessages();
        Check("view place: a folder opened by its arrow opens", (TreeView_GetItemState(tree, opened, TVIS_EXPANDED) & TVIS_EXPANDED) != 0);
        Check("view place: opening a folder keeps the view where it was", ViewPosition(treeView) == position);
        Check("view place: opening a folder by its arrow leaves the selection", TreeView_GetSelection(tree) == TreeView_GetRoot(tree));
    } else Check("view place: the folder to open found", FALSE);
    ShowWindow(host, SW_HIDE);
    DestroyWindow(view);
    DestroyWindow(treeView);
}

typedef struct TreeTop { HWND tree; HTREEITEM item; } TreeTop;

static BOOL TreeTopReached(const void *context)
{
    const TreeTop *top = (const TreeTop *)context;
    return TreeView_GetFirstVisible(top->tree) == top->item;
}

/* A tree too tall for a view (THEME_VIEW_MAX_PX) scrolls itself, by rows, with
 * a smooth wheel of its own and the view's wheel line; back under that
 * height, its view scrolls it again. */
static void TestTallViewWheel(void)
{
    HWND host = ThemedHost(), tree, view;
    TVINSERTSTRUCTW insert;
    UINT lines;
    int i, rows, rowHeight;
    if (!WheelScrollsLines(&lines, "tall view wheel")) return;
    tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_NOHSCROLL, 10, 10, 200, 150, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!tree) {
        Check("tall view: tree created", FALSE);
        return;
    }
    TreeView_SetItemHeight(tree, 20);
    rowHeight = TreeView_GetItemHeight(tree);
    rows = THEME_VIEW_MAX_PX / max(1, rowHeight) + 100;
    /* In its view first, then filled, as the sessions view fills its tree. */
    view = Theme_SmoothView(tree);
    Theme_SetScrollRow(tree, 2 * rowHeight);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    ZeroMemory(&insert, sizeof insert);
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = (LPWSTR)L"Row";
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    for (i = 0; i < rows; i++) TreeView_InsertItem(tree, &insert);
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    PumpMessages();
    {
        SCROLLINFO scroll = { sizeof scroll, SIF_RANGE | SIF_PAGE };
        BOOL ranged = GetScrollInfo(view, SB_VERT, &scroll);
        Check("tall view: a tree taller than a view scrolls itself", (GetWindowLongW(tree, GWL_STYLE) & WS_VSCROLL) != 0 && (!ranged || scroll.nMax <= (int)scroll.nPage));
        if (!(GetWindowLongW(tree, GWL_STYLE) & WS_VSCROLL) || (ranged && scroll.nMax > (int)scroll.nPage))
            printf("        tree scroll bar=%d view range=%d..%d page=%u\n", (GetWindowLongW(tree, GWL_STYLE) & WS_VSCROLL) != 0,
                   scroll.nMin, scroll.nMax, scroll.nPage);
    }
    {
        HTREEITEM top = TreeView_GetFirstVisible(tree), first;
        TreeTop expected;
        int moved;
        expected.tree = tree;
        expected.item = top;
        for (moved = 0; expected.item && moved < (int)lines * 2; moved++) expected.item = TreeView_GetNextVisible(tree, expected.item);
        SendWheelNotches(tree, 1);
        first = TreeView_GetFirstVisible(tree);
        Check("tall view: a wheel notch scrolls the tree the view's wheel line, its lines of two rows",
              expected.item && PumpUntil(TreeTopReached, &expected, ANIMATION_TIMEOUT_MS));
        for (moved = 0; top && top != TreeView_GetFirstVisible(tree); top = TreeView_GetNextVisible(tree, top)) moved++;
        if (moved != (int)lines * 2) printf("        moved %d rows, expected %u\n", moved, lines * 2);
        Check("tall view: the tree's own wheel is smooth: its first frame goes part of the way", first != expected.item);
    }
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    for (i = rows / 2; i < rows; i++) TreeView_DeleteItem(tree, TreeView_GetRoot(tree));
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    SendMessageW(tree, WM_VSCROLL, SB_TOP, 0);
    PumpMessages();
    {
        FrameSample sample = { 0 };
        int before = ViewPosition(view), after;
        Check("tall view: a tree back under a view's height is scrolled by its view", !(GetWindowLongW(tree, GWL_STYLE) & WS_VSCROLL));
        sample.view = view;
        sample.target = before + (int)lines * 2 * rowHeight;
        SendWheelNotches(tree, 1);
        PumpUntil(ViewReached, &sample, ANIMATION_TIMEOUT_MS);
        after = ViewPosition(view);
        Check("tall view: the view takes the wheel back, with the same wheel line", after - before == (int)lines * 2 * rowHeight);
        if (after - before != (int)lines * 2 * rowHeight) printf("        view moved %d px, expected %d\n", after - before, (int)lines * 2 * rowHeight);
    }
    ShowWindow(host, SW_HIDE);
    DestroyWindow(view);
}

/* A control already too tall for a view when it gets one (its size in the
 * view unchanged) keeps the scroll bar it showed: it scrolls itself. */
static void TestTallControlGivenView(void)
{
    HWND host = ThemedHost(), controls[2], views[2];
    TVINSERTSTRUCTW insert;
    int i, kind, rows;
    controls[0] = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | TVS_NOHSCROLL, 10, 10, 200, 150, host, NULL,
                                  GetModuleHandleW(NULL), NULL);
    controls[1] = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 220, 10, 200, 150, host, NULL,
                                  GetModuleHandleW(NULL), NULL);
    if (!controls[0] || !controls[1]) {
        Check("tall control given a view: tree and list box created", FALSE);
        for (kind = 0; kind < 2; kind++) if (controls[kind]) DestroyWindow(controls[kind]);
        return;
    }
    TreeView_SetItemHeight(controls[0], 20);
    SendMessageW(controls[1], LB_SETITEMHEIGHT, 0, 20);
    rows = THEME_VIEW_MAX_PX / 20 + 100;
    ZeroMemory(&insert, sizeof insert);
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = (LPWSTR)L"Row";
    for (i = 0; i < rows; i++) {
        TreeView_InsertItem(controls[0], &insert);
        SendMessageW(controls[1], LB_ADDSTRING, 0, (LPARAM)L"Row");
    }
    for (kind = 0; kind < 2; kind++) views[kind] = Theme_SmoothView(controls[kind]);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    PumpMessages();
    Check("tall control given a view: a tree keeps its own scroll bar", (GetWindowLongW(controls[0], GWL_STYLE) & WS_VSCROLL) != 0);
    Check("tall control given a view: a list box keeps its own scroll bar", (GetWindowLongW(controls[1], GWL_STYLE) & WS_VSCROLL) != 0);
    ShowWindow(host, SW_HIDE);
    for (kind = 0; kind < 2; kind++) DestroyWindow(views[kind]);
}

/* A view follows a selection the program sets while its control draws (on
 * the measure it posts), not one set during a refill (drawing off); a row
 * asked to be shown is shown even during a refill; a view whose content
 * fits stays at its top. */
static void TestViewFollowsSelection(void)
{
    static const char *const kAsked[] = { "LB_SETCARETINDEX", "ListView_EnsureVisible", "TreeView_EnsureVisible" };
    HWND host = ThemedHost(), shortBox, shortView;
    ViewedControls viewed;
    char name[200];
    int kind, i;
    if (!CreateViewedControls(host, 100, &viewed)) {
        Check("view selection: controls created", FALSE);
        return;
    }
    for (kind = 0; kind < 3; kind++) {
        HWND control = viewed.controls[kind], view = viewed.views[kind];
        RECT client, row;
        int asked = 80, position;
        GetClientRect(view, &client);
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 400), 0);
        PumpMessages();
        /* A refill that selects its first row: the view stays where it is. */
        SendMessageW(control, WM_SETREDRAW, FALSE, 0);
        if (kind == 0) {
            SendMessageW(control, LB_RESETCONTENT, 0, 0);
            for (i = 0; i < 100; i++) SendMessageW(control, LB_ADDSTRING, 0, (LPARAM)L"Refilled row");
        } else if (kind == 1) {
            LVITEMW item = { 0 };
            ListView_DeleteAllItems(control);
            item.mask = LVIF_TEXT;
            item.pszText = (LPWSTR)L"Refilled row";
            for (i = 0; i < 100; i++) {
                item.iItem = i;
                ListView_InsertItem(control, &item);
            }
        } else {
            TVINSERTSTRUCTW insert = { 0 };
            TreeView_DeleteAllItems(control);
            insert.hParent = TVI_ROOT;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT;
            insert.item.pszText = (LPWSTR)L"Refilled row";
            for (i = 0; i < 100; i++) TreeView_InsertItem(control, &insert);
        }
        SelectRow(control, kind, 0);
        SendMessageW(control, WM_SETREDRAW, TRUE, 0);
        PumpMessages();
        StringCchPrintfA(name, ARRAYSIZE(name), "view selection: %s's view stays put when a refill selects its first row", kViewedNames[kind]);
        Check(name, ViewPosition(view) == 400);
        /* The same selection set while it draws: followed, on the measure the view posts. */
        if (kind != 1) {
            SelectRow(control, kind, 1);
            SelectRow(control, kind, 0);
            position = ViewPosition(view);
            PumpMessages();
            StringCchPrintfA(name, ARRAYSIZE(name), "view selection: %s's view follows a selection set while it draws, once its measure runs",
                             kViewedNames[kind]);
            Check(name, position == 400 && ViewPosition(view) == 0);
        }
        /* A row asked to be shown during a refill is shown once it draws again. */
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 0), 0);
        SendMessageW(control, WM_SETREDRAW, FALSE, 0);
        if (kind == 0) SendMessageW(control, LB_SETCARETINDEX, (WPARAM)asked, FALSE);
        else if (kind == 1) ListView_EnsureVisible(control, asked, FALSE);
        else {
            HTREEITEM item = TreeView_GetRoot(control);
            for (i = 0; item && i < asked; i++) item = TreeView_GetNextSibling(control, item);
            TreeView_EnsureVisible(control, item);
        }
        SendMessageW(control, WM_SETREDRAW, TRUE, 0);
        PumpMessages();
        if (kind == 0) SendMessageW(control, LB_GETITEMRECT, (WPARAM)asked, (LPARAM)&row);
        else if (kind == 1) ListView_GetItemRect(control, asked, &row, LVIR_BOUNDS);
        else {
            HTREEITEM item = TreeView_GetRoot(control);
            for (i = 0; item && i < asked; i++) item = TreeView_GetNextSibling(control, item);
            TreeView_GetItemRect(control, item, &row, FALSE);
        }
        position = ViewPosition(view);
        StringCchPrintfA(name, ARRAYSIZE(name), "view selection: %s asked during a refill shows that row", kAsked[kind]);
        Check(name, row.top >= position && row.bottom <= position + client.bottom);
    }
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);

    shortBox = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 160, 120,
                               host, NULL, GetModuleHandleW(NULL), NULL);
    if (!shortBox) {
        Check("view selection: short list created", FALSE);
        return;
    }
    for (i = 0; i < 3; i++) SendMessageW(shortBox, LB_ADDSTRING, 0, (LPARAM)L"Row");
    shortView = Theme_SmoothView(shortBox);
    Theme_Apply(host);
    PumpMessages();
    SendMessageW(shortView, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 50), 0);
    {
        RECT placed;
        GetWindowRect(shortBox, &placed);
        MapWindowPoints(NULL, shortView, (POINT *)&placed, 2);
        Check("view selection: a thumb position in a view whose content fits leaves it at its top", ViewPosition(shortView) == 0 && placed.top == 0);
    }
    DestroyWindow(shortView);
}

/* The keyboard's row whole in sight in its view (under a list view's header). */
static BOOL RowInSight(HWND control, HWND view, int kind, int expectedIndex)
{
    RECT client, row, header;
    int position = ViewPosition(view), top = 0;
    if (kind == 1 && GetWindowRect(ListView_GetHeader(control), &header)) top = header.bottom - header.top;
    GetClientRect(view, &client);
    return KeyboardRow(control, kind, &row) == expectedIndex && row.top >= position + top && row.bottom <= position + client.bottom;
}

/* Ctrl with an arrow, Page Up, Page Down, Home or End scrolls a tree's view
 * without moving its selection, as a tree that scrolls itself does; a list
 * keeps those keys (they move its focus, and its view follows). */
static void TestViewKeys(void)
{
    static const struct { UINT key; const char *name; } kKeys[] = {
        { VK_DOWN, "Ctrl+Down" }, { VK_NEXT, "Ctrl+Page Down" }, { VK_UP, "Ctrl+Up" }, { VK_PRIOR, "Ctrl+Page Up" },
        { VK_END, "Ctrl+End" }, { VK_HOME, "Ctrl+Home" }
    };
    HWND host = ThemedHost(), tree, view;
    ViewedControls viewed;
    BYTE saved[256], keys[256];
    SCROLLINFO scroll;
    RECT client;
    HTREEITEM selected;
    int row, bottom, expected = 0;
    size_t i;
    if (!CreateViewedControls(host, 100, &viewed)) {
        Check("view keys: controls created", FALSE);
        return;
    }
    if (!GetKeyboardState(saved)) {
        Check("view keys: keyboard state read", FALSE);
        ShowWindow(host, SW_HIDE);
        DestroyViewedControls(&viewed);
        return;
    }
    tree = viewed.controls[2];
    view = viewed.views[2];
    SelectRow(tree, 2, 0);
    PumpMessages();
    selected = TreeView_GetSelection(tree);
    row = RowHeight(tree, 2);
    GetClientRect(view, &client);
    ZeroMemory(&scroll, sizeof scroll);
    scroll.cbSize = sizeof scroll;
    scroll.fMask = SIF_RANGE | SIF_PAGE;
    GetScrollInfo(view, SB_VERT, &scroll);
    bottom = scroll.nMax - (int)scroll.nPage + 1;
    /* Ctrl held in this thread's keyboard state only. */
    CopyMemory(keys, saved, sizeof keys);
    keys[VK_CONTROL] = 0x80;
    SetKeyboardState(keys);
    for (i = 0; i < ARRAYSIZE(kKeys); i++) {
        char name[160];
        switch (kKeys[i].key) {
        case VK_DOWN:  expected += row; break;
        case VK_UP:    expected -= row; break;
        case VK_NEXT:  expected += client.bottom; break;
        case VK_PRIOR: expected -= client.bottom; break;
        case VK_END:   expected = bottom; break;
        default:       expected = 0; break;
        }
        expected = max(0, min(expected, bottom));
        SendMessageW(tree, WM_KEYDOWN, kKeys[i].key, 0);
        SendMessageW(tree, WM_KEYUP, kKeys[i].key, 0);
        PumpMessages();
        StringCchPrintfA(name, ARRAYSIZE(name), "view keys: %s scrolls a tree's view and keeps its selection", kKeys[i].name);
        Check(name, ViewPosition(view) == expected && TreeView_GetSelection(tree) == selected);
        if (ViewPosition(view) != expected) printf("        view at %d px, expected %d\n", ViewPosition(view), expected);
    }
    SelectRow(viewed.controls[1], 1, 0);
    PumpMessages();
    SendMessageW(viewed.controls[1], WM_KEYDOWN, VK_END, 0);
    SendMessageW(viewed.controls[1], WM_KEYUP, VK_END, 0);
    PumpMessages();
    {
        /* The focus moves alone: the selection stays on the first row. */
        HWND list = viewed.controls[1], header = ListView_GetHeader(list);
        RECT last, top;
        int position = ViewPosition(viewed.views[1]);
        BOOL shown = ListView_GetItemRect(list, 99, &last, LVIR_BOUNDS) && GetWindowRect(header, &top) && GetClientRect(viewed.views[1], &client) &&
                     last.top >= position + (top.bottom - top.top) && last.bottom <= position + client.bottom;
        Check("view keys: Ctrl+End in a list view moves its focus to its last row and its view shows it, as Windows does",
              ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 99 && ListView_GetItemState(list, 0, LVIS_SELECTED) && shown);
    }
    SetKeyboardState(saved);
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
}

/* An arrow, type-ahead and a click on another row bring the keyboard's row
 * into sight in a list box's, a list view's and a tree's views. */
static void TestViewRevealsKeyboardRow(void)
{
    HWND host = ThemedHost();
    ViewedControls viewed;
    int kind;
    if (!CreateViewedControls(host, 100, &viewed)) {
        Check("view reveal: controls created", FALSE);
        return;
    }
    for (kind = 0; kind < 3; kind++) {
        HWND control = viewed.controls[kind], view = viewed.views[kind];
        RECT client, row, label;
        char name[200];
        int index, rowHeight = RowHeight(control, kind);
        GetClientRect(view, &client);
        SelectRow(control, kind, 10);
        PumpMessages();
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 60 * rowHeight), 0);
        SendMessageW(control, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(control, WM_KEYUP, VK_DOWN, 0);
        PumpMessages();
        StringCchPrintfA(name, ARRAYSIZE(name), "view reveal: an arrow in %s's view brings the keyboard's row into sight", kViewedNames[kind]);
        Check(name, RowInSight(control, view, kind, 11));
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 60 * rowHeight), 0);
        SendMessageW(control, WM_CHAR, L'R', 0);
        PumpMessages();
        index = KeyboardRow(control, kind, &row);
        StringCchPrintfA(name, ARRAYSIZE(name), "view reveal: type-ahead in %s's view brings the keyboard's row into sight", kViewedNames[kind]);
        Check(name, index != 11 && RowInSight(control, view, kind, index));
        if (kind == 1) continue;   /* a list view's click waits for the mouse in a loop of its own */
        /* Row 40 half shown at the bottom of the view, then clicked. */
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 40 * rowHeight + rowHeight / 2 - client.bottom), 0);
        SelectRow(control, kind, 10);
        PumpMessages();
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 40 * rowHeight + rowHeight / 2 - client.bottom), 0);
        if (kind == 0) {
            SendMessageW(control, LB_GETITEMRECT, 40, (LPARAM)&row);
            SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(row.left + 5, row.top + 2));
            SendMessageW(control, WM_LBUTTONUP, 0, MAKELPARAM(row.left + 5, row.top + 2));
        } else {
            HTREEITEM item = TreeView_GetRoot(control);
            for (index = 0; item && index < 40; index++) item = TreeView_GetNextVisible(control, item);
            TreeView_GetItemRect(control, item, &label, TRUE);
            /* The click's own button-up comes first if the tree waits for it. */
            PostMessageW(control, WM_LBUTTONUP, 0, MAKELPARAM(label.left + 2, label.top + 2));
            SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(label.left + 2, label.top + 2));
        }
        PumpMessages();
        StringCchPrintfA(name, ARRAYSIZE(name), "view reveal: a click on a row half shown in %s's view brings it whole into sight", kViewedNames[kind]);
        Check(name, RowInSight(control, view, kind, 40));
    }
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
}

typedef struct PositionWatch { HWND view; int from; } PositionWatch;

static BOOL PositionMoved(const void *context)
{
    const PositionWatch *watch = (const PositionWatch *)context;
    return ViewPosition(watch->view) != watch->from;
}

/* A key, a click and a context menu stop a smooth wheel that is still moving. */
static void TestWheelStops(void)
{
    static const struct { UINT message; WPARAM wp; const char *name; } kStops[] = {
        { WM_KEYDOWN, VK_SHIFT, "a key" }, { WM_LBUTTONDOWN, MK_LBUTTON, "a click" }, { WM_CONTEXTMENU, 0, "a context menu" }
    };
    HWND host = ThemedHost(), box, view;
    ViewedControls viewed;
    UINT lines;
    size_t i;
    if (!WheelScrollsLines(&lines, "wheel stops")) return;
    if (!CreateViewedControls(host, 100, &viewed)) {
        Check("wheel stops: controls created", FALSE);
        return;
    }
    box = viewed.controls[0];
    view = viewed.views[0];
    SelectRow(box, 0, 0);
    for (i = 0; i < ARRAYSIZE(kStops); i++) {
        PositionWatch watch;
        RECT first;
        int target = (int)lines * RowHeight(box, 0);
        BOOL moved;
        char name[160];
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 0), 0);
        PumpMessages();
        SendMessageW(box, LB_GETITEMRECT, 0, (LPARAM)&first);
        SendMessageW(box, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        watch.view = view;
        watch.from = ViewPosition(view);   /* its first frame, at once */
        /* The keyboard's row, clicked: nothing to bring into sight. */
        SendMessageW(box, kStops[i].message, kStops[i].wp, kStops[i].message == WM_LBUTTONDOWN ? MAKELPARAM(first.left + 5, first.top + 2) :
                                                           kStops[i].message == WM_CONTEXTMENU ? (LPARAM)-1 : 0);
        if (kStops[i].message == WM_LBUTTONDOWN) SendMessageW(box, WM_LBUTTONUP, 0, MAKELPARAM(first.left + 5, first.top + 2));
        moved = PumpUntil(PositionMoved, &watch, STOPPED_ANIMATION_MS);
        StringCchPrintfA(name, ARRAYSIZE(name), "wheel stops: %s stops a wheel animation still moving", kStops[i].name);
        Check(name, watch.from > 0 && watch.from < target && !moved);
        if (moved || watch.from <= 0 || watch.from >= target) printf("        first frame at %d px, then %d px, the notch's end %d px\n", watch.from, ViewPosition(view), target);
    }
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
}

/* Theme_SmoothView gives a control one view, which takes the control's id. */
static void TestSmoothViewIdentity(void)
{
    HWND host = ThemedHost(), box, view;
    box = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 140, 120, host,
                          (HMENU)(INT_PTR)4321, GetModuleHandleW(NULL), NULL);
    if (!box) {
        Check("smooth view identity: list box created", FALSE);
        return;
    }
    view = Theme_SmoothView(box);
    Check("smooth view identity: the view takes its control's id", view && view != box && GetDlgCtrlID(view) == 4321 && GetDlgItem(host, 4321) == view);
    Check("smooth view identity: asked again for the control, it returns its view", Theme_SmoothView(box) == view);
    Check("smooth view identity: asked for the view itself, it returns it", Theme_SmoothView(view) == view);
    DestroyWindow(view && view != box ? view : box);
}

/* A table's last column in display order takes the room the others leave,
 * never less than its title: past it, the table scrolls sideways. */
static void TestLastColumn(void)
{
    static const WCHAR kTitle[] = L"A long title";
    int order[] = { 1, 2, 0 }, i;
    HWND host = ThemedHost(), list, view, header;
    HFONT font = DialogFont();
    LVCOLUMNW column = { 0 };
    LVITEMW item = { 0 };
    SCROLLBARINFO bar = { sizeof bar };
    RECT client, title = { 0, 0, 0, 0 };
    HDC dc;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 300, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("last column: private list created", FALSE);
        DeleteObject(font);
        return;
    }
    SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 60;
    for (i = 0; i < 3; i++) {
        column.pszText = (LPWSTR)(i ? L"" : kTitle);
        ListView_InsertColumn(list, i, &column);
    }
    ListView_SetColumnOrderArray(list, 3, order);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"Row";
    ListView_InsertItem(list, &item);
    view = Theme_SmoothView(list);
    Theme_Apply(host);
    PumpMessages();
    GetClientRect(list, &client);
    Check("last column: the last column in display order takes the room the others leave",
          ListView_GetColumnWidth(list, 0) == client.right - ListView_GetColumnWidth(list, 1) - ListView_GetColumnWidth(list, 2));
    Check("last column: a column last by its index only keeps its width", ListView_GetColumnWidth(list, 2) == 60);
    header = ListView_GetHeader(list);
    if ((dc = GetDC(header)) != NULL) {
        HGDIOBJ previous = SelectObject(dc, (HFONT)SendMessageW(header, WM_GETFONT, 0, 0));
        DrawTextW(dc, kTitle, -1, &title, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
        ReleaseDC(header, dc);
    }
    SetWindowPos(view, NULL, 0, 0, 160, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    GetClientRect(list, &client);
    Check("last column: in a narrow view the last column keeps its whole title, more than the room left",
          title.right > 0 && ListView_GetColumnWidth(list, 0) >= title.right && ListView_GetColumnWidth(list, 0) > client.right - 120);
    Check("last column: past it, the table scrolls sideways",
          GetScrollBarInfo(list, OBJID_HSCROLL, &bar) && !(bar.rgstate[0] & STATE_SYSTEM_INVISIBLE));
    DestroyWindow(view);
    DeleteObject(font);
}

/* A key that moves nothing in a list view leaves its view's layout alone
 * (its last column keeps a width the program gave it); one that moves the
 * keyboard's row lays the view out again (the last column takes the room). */
static void TestKeysAndLayout(void)
{
    HWND host = ThemedHost(), list;
    ViewedControls viewed;
    RECT client;
    if (!CreateViewedControls(host, 100, &viewed)) {
        Check("keys and layout: controls created", FALSE);
        return;
    }
    list = viewed.controls[1];
    SelectRow(list, 1, 0);
    PumpMessages();
    Theme_SetColumnWidth(list, 0, 50);
    SendMessageW(list, WM_KEYDOWN, VK_UP, 0);
    SendMessageW(list, WM_KEYUP, VK_UP, 0);
    PumpMessages();
    Check("keys and layout: an arrow that moves nothing in a list view lays nothing out again", ListView_GetColumnWidth(list, 0) == 50);
    SendMessageW(list, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(list, WM_KEYUP, VK_DOWN, 0);
    PumpMessages();
    GetClientRect(list, &client);
    Check("keys and layout: an arrow that moves the keyboard's row lays the view out again", ListView_GetColumnWidth(list, 0) == client.right);
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
}

/* A new font in a list box, and a new row height in a tree, keep the row on
 * top of their view on top. */
static void TestViewKeepsTopRow(void)
{
    HWND host = ThemedHost();
    ViewedControls viewed;
    LOGFONTW logical;
    HFONT bigger;
    int kind;
    ZeroMemory(&logical, sizeof logical);
    logical.lfHeight = -24;
    StringCchCopyW(logical.lfFaceName, ARRAYSIZE(logical.lfFaceName), L"Segoe UI");
    bigger = CreateFontIndirectW(&logical);
    if (!bigger || !CreateViewedControls(host, 100, &viewed)) {
        Check("top row: private font and controls created", FALSE);
        if (bigger) DeleteObject(bigger);
        return;
    }
    for (kind = 0; kind < 3; kind += 2) {
        HWND control = viewed.controls[kind], view = viewed.views[kind];
        RECT top;
        int row = RowHeight(control, kind);
        char name[160];
        SendMessageW(view, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 20 * row), 0);
        if (kind == 0) SendMessageW(control, WM_SETFONT, (WPARAM)bigger, TRUE);
        else TreeView_SetItemHeight(control, 2 * row);
        PumpMessages();
        if (kind == 0) SendMessageW(control, LB_GETITEMRECT, 20, (LPARAM)&top);
        else {
            HTREEITEM item = TreeView_GetRoot(control);
            int i;
            for (i = 0; item && i < 20; i++) item = TreeView_GetNextVisible(control, item);
            TreeView_GetItemRect(control, item, &top, FALSE);
        }
        StringCchPrintfA(name, ARRAYSIZE(name), "top row: %s's taller rows keep the row on top of its view on top", kViewedNames[kind]);
        Check(name, RowHeight(control, kind) > row && top.top == ViewPosition(view));
        if (top.top != ViewPosition(view)) printf("        row 20 at %ld px, view at %d px\n", top.top, ViewPosition(view));
    }
    ShowWindow(host, SW_HIDE);
    DestroyViewedControls(&viewed);
    DeleteObject(bigger);
}

/* Theme_FitLastColumn gives a list's last column the width the others
 * leave, never less than the minimum asked; a column the user sized keeps
 * its width. */
static void TestFitLastColumn(void)
{
    HWND host = ThemedHost(), list;
    LVCOLUMNW column = { 0 };
    RECT client;
    int i;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 300, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("fit last column: private list created", FALSE);
        return;
    }
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.pszText = (LPWSTR)L"";
    column.cx = 60;
    for (i = 0; i < 3; i++) ListView_InsertColumn(list, i, &column);
    Theme_Apply(host);
    GetClientRect(list, &client);
    Theme_FitLastColumn(list, 0);
    Check("fit last column: the last column takes the width the others leave", ListView_GetColumnWidth(list, 2) == client.right - 120);
    Theme_FitLastColumn(list, client.right);
    Check("fit last column: never less than the minimum asked", ListView_GetColumnWidth(list, 2) == client.right);
    ReportDividerDrag(list, 2, 70);
    Theme_FitLastColumn(list, 0);
    Check("fit last column: a last column the user sized keeps its width", ListView_GetColumnWidth(list, 2) == 70);
    DestroyWindow(list);
}

/* A view made for a framed control after its dialog was themed has the
 * frame of one made before (the control's own, which a dark theme takes off). */
static void TestViewFrameAfterTheming(void)
{
    HWND host = ThemedHost(), early, late, earlyView, lateView;
    early = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 140, 120, host,
                            NULL, GetModuleHandleW(NULL), NULL);
    late = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 160, 10, 140, 120, host,
                           NULL, GetModuleHandleW(NULL), NULL);
    if (!early || !late) {
        Check("view frame: list boxes created", FALSE);
        if (early) DestroyWindow(early);
        if (late) DestroyWindow(late);
        return;
    }
    earlyView = Theme_SmoothView(early);
    Theme_Apply(host);
    lateView = Theme_SmoothView(late);
    Theme_Apply(host);
    Check("view frame: a view made after theming has the frame of one made before",
          (GetWindowLongW(earlyView, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == (GetWindowLongW(lateView, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) &&
          (GetWindowLongW(earlyView, GWL_STYLE) & WS_BORDER) == (GetWindowLongW(lateView, GWL_STYLE) & WS_BORDER));
    if (!Theme_IsDark()) Check("view frame: a light view made after theming keeps its control's frame", (GetWindowLongW(lateView, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0);
    DestroyWindow(earlyView);
    DestroyWindow(lateView);
}

/* A dark header draws each title aligned as its column is. */
static void TestDarkHeaderAlignment(void)
{
    static const struct { const WCHAR *title; int format; const char *name; } kColumns[] = {
        { L"Left", LVCFMT_LEFT, "left" }, { L"Right", LVCFMT_RIGHT, "right" }, { L"Mid", LVCFMT_CENTER, "centered" }
    };
    HWND host = ThemedHost(), list, header;
    HFONT font;
    LVCOLUMNW column = { 0 };
    Canvas canvas = { 0 };
    RECT client;
    size_t i;
    if (SkipInLight("dark header alignment")) return;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 10, 330, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("dark header alignment: private list created", FALSE);
        return;
    }
    font = DialogFont();
    SendMessageW(list, WM_SETFONT, (WPARAM)font, FALSE);
    column.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_FMT;
    column.cx = 100;
    for (i = 0; i < ARRAYSIZE(kColumns); i++) {
        column.pszText = (LPWSTR)kColumns[i].title;
        column.fmt = kColumns[i].format;
        ListView_InsertColumn(list, (int)i, &column);
    }
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    header = ListView_GetHeader(list);
    RedrawWindow(header, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    GetClientRect(header, &client);
    if (CanvasOpen(&canvas, client.right, client.bottom, RGB(1, 2, 3)) && CopyClient(header, &canvas) && HasImage(&canvas, "dark header alignment")) {
        for (i = 0; i < ARRAYSIZE(kColumns); i++) {
            RECT item, ink;
            int before, after;
            BOOL found, aligned;
            char name[160];
            Header_GetItemRect(header, (int)i, &item);
            /* Its title's ink, away from the divider at its right end. */
            found = InkBox(&canvas, item.left + 1, item.top + 1, item.right - 3, item.bottom - 1, PixelAt(&canvas, item.left + 1, item.top + 1), &ink);
            before = ink.left - item.left;
            after = item.right - ink.right;
            aligned = kColumns[i].format == LVCFMT_RIGHT ? after < before : kColumns[i].format == LVCFMT_CENTER ? abs(before - after) <= 3 : before < after;
            StringCchPrintfA(name, ARRAYSIZE(name), "dark header alignment: a %s column's title is drawn %s", kColumns[i].name, kColumns[i].name);
            Check(name, found && aligned);
            if (!found || !aligned) printf("        title ink %ld..%ld in %ld..%ld\n", ink.left, ink.right, item.left, item.right);
        }
    }
    CanvasClose(&canvas);
    ShowWindow(host, SW_HIDE);
    DestroyWindow(list);
    DeleteObject(font);
}

/* With the pointer resting on a tree in its view (the view placed under it,
 * the pointer never moved), a wheel scroll's frames tell the tree where the
 * pointer now is: an open tip closes, and none opens for the row that came
 * under the pointer; a real move brings the tip back. */
static void TestTipsUnderWheel(void)
{
    HWND host, tree, view, tips;
    TVINSERTSTRUCTW insert = { 0 };
    TipProbe probe = { 0 };
    FrameSample sample = { 0 };
    POINT cursor, after, inTree = { 60, 50 }, under;
    UINT lines;
    int i, shown;
    BOOL reached, opened, closed, back;
    if (!WheelScrollsLines(&lines, "tips under a wheel scroll")) return;
    if (!GetCursorPos(&cursor)) {
        Skip("tips under a wheel scroll: the pointer's position is unknown here");
        return;
    }
    /* A window of its own, transparent to the pointer, whose tree point is under it. */
    host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, WC_STATICW, L"", WS_POPUP | WS_CLIPCHILDREN,
                           cursor.x - 10 - inTree.x, cursor.y - 10 - inTree.y, 260, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    tree = host ? CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_NOHSCROLL, 10, 10, 200, 120, host, NULL, GetModuleHandleW(NULL), NULL)
                : NULL;
    if (!tree) {
        Check("tips under a wheel scroll: private window and tree created", FALSE);
        if (host) DestroyWindow(host);
        return;
    }
    SetLayeredWindowAttributes(host, 0, 1, LWA_ALPHA);
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = (LPWSTR)L"Session title with enough words to exceed the visible tree viewport by a comfortable margin";
    view = Theme_SmoothView(tree);
    for (i = 0; i < 60; i++) TreeView_InsertItem(tree, &insert);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    PumpMessages();
    tips = TreeView_GetToolTips(tree);
    SetWindowSubclass(tips, ProbeTipWindow, TIP_WINDOW_SUBCLASS, (DWORD_PTR)&probe);
    under = cursor;
    ScreenToClient(tree, &under);
    SendMessageW(tree, WM_MOUSEMOVE, 0, MAKELPARAM(under.x, under.y));
    opened = IsWindowVisible(tips);
    shown = probe.shown;
    sample.view = view;
    sample.target = (int)lines * TreeView_GetItemHeight(tree);
    SendMessageW(tree, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    reached = PumpUntil(ViewReached, &sample, ANIMATION_TIMEOUT_MS);
    closed = !IsWindowVisible(tips) && probe.shown == shown;
    under = cursor;
    ScreenToClient(tree, &under);
    SendMessageW(tree, WM_MOUSEMOVE, 0, MAKELPARAM(under.x, under.y));
    back = IsWindowVisible(tips);
    if (!GetCursorPos(&after) || after.x != cursor.x || after.y != cursor.y) {
        Skip("tips under a wheel scroll: the pointer moved meanwhile");
    } else {
        Check("tips under a wheel scroll: a clipped title under the pointer shows its tip", opened);
        Check("tips under a wheel scroll: the scroll's frames close the tip and open none", reached && closed);
        Check("tips under a wheel scroll: a real move brings the tip back", back);
    }
    RemoveWindowSubclass(tips, ProbeTipWindow, TIP_WINDOW_SUBCLASS);
    DestroyWindow(host);
}

/* Theme_ScrollTarget: where a WM_VSCROLL request takes a window scrolled by
 * the pixel, inside its range. */
static void TestScrollTarget(void)
{
    static const struct { WORD request; WORD thumb; int from, expected; const char *name; } kRequests[] = {
        { SB_LINEUP, 0, 300, 280, "a line up moves one line" },
        { SB_LINEDOWN, 0, 300, 320, "a line down moves one line" },
        { SB_PAGEUP, 0, 300, 200, "a page up moves one page" },
        { SB_PAGEDOWN, 0, 300, 400, "a page down moves one page" },
        { SB_THUMBPOSITION, 555, 300, 555, "the thumb goes where it is put" },
        { SB_TOP, 0, 300, 0, "the top is 0" },
        { SB_BOTTOM, 0, 300, 900, "the bottom is the range less the page" },
        { SB_ENDSCROLL, 0, 300, 300, "another request keeps the position" },
        { SB_LINEUP, 0, 10, 0, "a line up stops at 0" },
        { SB_PAGEDOWN, 0, 890, 900, "a page down stops at the end" },
        { SB_THUMBPOSITION, 2000, 300, 900, "a thumb past the end stops at the end" },
    };
    HWND scrolled = CreateWindowExW(0, WC_STATICW, L"", WS_POPUP | WS_VSCROLL, 0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HWND plain = CreateWindowExW(0, WC_STATICW, L"", WS_POPUP, 0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    size_t i;
    if (!scrolled || !plain) {
        Check("scroll target: windows created", FALSE);
        if (scrolled) DestroyWindow(scrolled);
        if (plain) DestroyWindow(plain);
        return;
    }
    for (i = 0; i < ARRAYSIZE(kRequests); i++) {
        SCROLLINFO range = { sizeof range, SIF_RANGE | SIF_PAGE | SIF_POS, 0, 999, 100, 0, 0 };
        char name[160];
        range.nPos = kRequests[i].from;
        SetScrollInfo(scrolled, SB_VERT, &range, FALSE);
        StringCchPrintfA(name, ARRAYSIZE(name), "scroll target: %s", kRequests[i].name);
        Check(name, Theme_ScrollTarget(scrolled, MAKEWPARAM(kRequests[i].request, kRequests[i].thumb), 20) == kRequests[i].expected);
    }
    Check("scroll target: a window with no scroll bar goes nowhere", Theme_ScrollTarget(plain, MAKEWPARAM(SB_LINEDOWN, 0), 20) == 0);
    DestroyWindow(scrolled);
    DestroyWindow(plain);
}

/* The arrow of a folder a window draws itself is the tree's own glyph, open
 * or closed, hot under the mouse. */
static void TestTreeGlyph(void)
{
    static const struct { UINT state; BOOL open; int part, glyphState; const char *name; } kGlyphs[] = {
        { 0, FALSE, TVP_GLYPH, GLPS_CLOSED, "a closed folder's arrow" },
        { 0, TRUE, TVP_GLYPH, GLPS_OPENED, "an open folder's arrow" },
        { THEME_ROW_HOT, FALSE, TVP_HOTGLYPH, GLPS_CLOSED, "a closed folder's arrow under the mouse" },
        { THEME_ROW_HOT, TRUE, TVP_HOTGLYPH, GLPS_OPENED, "an open folder's arrow under the mouse" },
    };
    HWND host = ThemedHost(), tree = FilledTree(host, 10, 2, FALSE);
    HTHEME theme;
    RECT cell = { 0, 0, 20, 30 };
    size_t i;
    if (!tree) {
        Check("tree glyph: tree created", FALSE);
        return;
    }
    Theme_Apply(host);
    if ((theme = OpenThemeData(tree, L"TreeView")) == NULL) {
        Skip("tree glyph: the tree has no visual style here");
        DestroyWindow(tree);
        return;
    }
    for (i = 0; i < ARRAYSIZE(kGlyphs); i++) {
        Canvas ours = { 0 }, native = { 0 };
        SIZE size;
        RECT box;
        char name[160];
        if (!CanvasOpen(&ours, 20, 30, Theme_Color(THEME_FIELD)) || !CanvasOpen(&native, 20, 30, Theme_Color(THEME_FIELD))) {
            Check("tree glyph: canvases created", FALSE);
            CanvasClose(&ours);
            continue;
        }
        Theme_DrawTreeGlyph(tree, ours.dc, &cell, kGlyphs[i].state, kGlyphs[i].open);
        size.cx = size.cy = 10;
        GetThemePartSize(theme, native.dc, kGlyphs[i].part, kGlyphs[i].glyphState, NULL, TS_DRAW, &size);
        SetRect(&box, (20 - size.cx) / 2, (30 - size.cy) / 2, (20 - size.cx) / 2 + size.cx, (30 - size.cy) / 2 + size.cy);
        DrawThemeBackground(theme, native.dc, kGlyphs[i].part, kGlyphs[i].glyphState, &box, NULL);
        if (HasImage(&native, "tree glyph")) {
            StringCchPrintfA(name, ARRAYSIZE(name), "tree glyph: %s is the tree theme's own, centered", kGlyphs[i].name);
            Check(name, SameCanvas(&ours, &native));
        }
        CanvasClose(&ours);
        CanvasClose(&native);
    }
    CloseThemeData(theme);
    DestroyWindow(tree);
}

/* Theme_DrawFocusCue marks the keyboard's row after keyboard use only, with
 * a dotted outline inside the row; Theme_RowMuted, a row's secondary text,
 * reads on each row Theme_DrawRow draws. */
static void TestFocusCueAndMutedText(void)
{
    static const struct { UINT state; const char *name; } kRows[] = {
        { 0, "a row" }, { THEME_ROW_HOT, "a row under the mouse" }, { THEME_ROW_SELECTED, "a selected row" }
    };
    HWND host = ThemedHost(), owner;
    COLORREF field = Theme_Color(THEME_FIELD);
    RECT row = { 4, 4, 84, 28 }, ink;
    Canvas canvas = { 0 };
    int x, y, changed, inside;
    size_t i;
    owner = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD, 0, 0, 10, 10, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!owner || !CanvasOpen(&canvas, 90, 32, field)) {
        Check("focus cue: private owner and canvas created", FALSE);
        if (owner) DestroyWindow(owner);
        CanvasClose(&canvas);
        return;
    }
    SendMessageW(owner, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    Theme_DrawFocusCue(owner, canvas.dc, &row);
    for (changed = 0, y = 0; y < canvas.height; y++) for (x = 0; x < canvas.width; x++) if (PixelAt(&canvas, x, y) != field) changed++;
    Check("focus cue: hidden until the keyboard is used", (SendMessageW(owner, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) && changed == 0);
    SendMessageW(owner, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    Theme_DrawFocusCue(owner, canvas.dc, &row);
    SetRect(&ink, canvas.width, canvas.height, 0, 0);
    for (changed = 0, y = 0; y < canvas.height; y++) for (x = 0; x < canvas.width; x++) if (PixelAt(&canvas, x, y) != field) {
        changed++;
        ink.left = min(ink.left, x);
        ink.top = min(ink.top, y);
        ink.right = max(ink.right, x + 1);
        ink.bottom = max(ink.bottom, y + 1);
    }
    for (inside = 0, y = ink.top + 2; y < ink.bottom - 2; y++) for (x = ink.left + 2; x < ink.right - 2; x++) if (PixelAt(&canvas, x, y) != field) inside++;
    Check("focus cue: after keyboard use, an outline inside the row and nothing within it",
          changed > 0 && ink.left >= row.left && ink.top >= row.top && ink.right <= row.right && ink.bottom <= row.bottom && inside == 0);
    CanvasClose(&canvas);
    CheckColor("muted text: a row's secondary text is the muted color", Theme_Color(THEME_MUTED), Theme_RowMuted(0));
    for (i = 0; i < ARRAYSIZE(kRows); i++) {
        char name[160];
        if (!CanvasOpen(&canvas, 90, 32, RGB(1, 2, 3))) {
            Check("muted text: canvas created", FALSE);
            continue;
        }
        Theme_DrawRow(owner, canvas.dc, &row, kRows[i].state, field);
        StringCchPrintfA(name, ARRAYSIZE(name), "muted text: secondary text reads on %s", kRows[i].name);
        Check(name, StandsOut(Theme_RowMuted(kRows[i].state), PixelAt(&canvas, (row.left + row.right) / 2, (row.top + row.bottom) / 2)));
        CanvasClose(&canvas);
    }
    DestroyWindow(owner);
}

/* A push button and a default one, at rest, under the mouse, pressed and
 * disabled, show the pixels Theme_DrawButton gives that state (the default
 * one its thicker frame). Light buttons are Windows' own. */
static void TestPushButtons(void)
{
    static const struct { UINT state; const char *name; } kStates[] = {
        { 0, "at rest" }, { THEME_BUTTON_HOT, "under the mouse" }, { THEME_BUTTON_PRESSED, "pressed" }, { THEME_BUTTON_DISABLED, "disabled" }
    };
    HWND host = ThemedHost(), buttons[2];
    HFONT font;
    Canvas rest[2] = { { 0 }, { 0 } };
    int kind, x, y, differ;
    size_t state;
    if (SkipInLight("push buttons drawn by the theme")) return;
    font = DialogFont();
    for (kind = 0; kind < 2; kind++) {
        buttons[kind] = CreateWindowExW(0, WC_BUTTONW, L"OK", WS_CHILD | WS_VISIBLE | (kind ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                        10 + 120 * kind, 10, 100, 28, host, NULL, GetModuleHandleW(NULL), NULL);
        if (buttons[kind]) SendMessageW(buttons[kind], WM_SETFONT, (WPARAM)font, FALSE);
    }
    if (!buttons[0] || !buttons[1]) {
        Check("push buttons: created", FALSE);
        for (kind = 0; kind < 2; kind++) if (buttons[kind]) DestroyWindow(buttons[kind]);
        DeleteObject(font);
        return;
    }
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    for (kind = 0; kind < 2; kind++) for (state = 0; state < ARRAYSIZE(kStates); state++) {
        HWND button = buttons[kind];
        RECT client;
        Canvas shown = { 0 }, drawn = { 0 };
        UINT format = DT_SINGLELINE | DT_CENTER | DT_VCENTER;
        char name[160];
        if (kStates[state].state == THEME_BUTTON_HOT) SendMessageW(button, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
        if (kStates[state].state == THEME_BUTTON_PRESSED) SendMessageW(button, BM_SETSTATE, TRUE, 0);
        if (kStates[state].state == THEME_BUTTON_DISABLED) EnableWindow(button, FALSE);
        /* Windows fades a button from one state to the next: its last frame is the state's. */
        BufferedPaintStopAllAnimations(button);
        RedrawWindow(button, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        GetClientRect(button, &client);
        if (SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) format |= DT_HIDEPREFIX;
        if (CanvasOpen(&shown, client.right, client.bottom, RGB(1, 2, 3)) && CopyClient(button, &shown) &&
            CanvasOpen(&drawn, client.right, client.bottom, RGB(1, 2, 3)) && HasImage(&shown, "push buttons")) {
            Theme_DrawButton(button, drawn.dc, &client, L"OK", font, kStates[state].state | (kind ? THEME_BUTTON_DEFAULT : 0), format);
            for (differ = 0, y = 0; y < client.bottom; y++) for (x = 0; x < client.right; x++)
                if (PixelAt(&shown, x, y) != PixelAt(&drawn, x, y)) differ++;
            StringCchPrintfA(name, ARRAYSIZE(name), "push buttons: a %s button %s is drawn as Theme_DrawButton draws it",
                             kind ? "default" : "push", kStates[state].name);
            Check(name, differ == 0);
            if (differ) printf("        %d of %ld pixels differ\n", differ, client.right * client.bottom);
            if (state == 0) {
                rest[kind] = shown;
                ZeroMemory(&shown, sizeof shown);
            }
        }
        CanvasClose(&shown);
        CanvasClose(&drawn);
        if (kStates[state].state == THEME_BUTTON_HOT) SendMessageW(button, WM_MOUSELEAVE, 0, 0);
        if (kStates[state].state == THEME_BUTTON_PRESSED) SendMessageW(button, BM_SETSTATE, FALSE, 0);
        if (kStates[state].state == THEME_BUTTON_DISABLED) EnableWindow(button, TRUE);
    }
    Check("push buttons: the default button's frame differs from a push button's", rest[0].dc && rest[1].dc && !SameCanvas(&rest[0], &rest[1]));
    CanvasClose(&rest[0]);
    CanvasClose(&rest[1]);
    ShowWindow(host, SW_HIDE);
    for (kind = 0; kind < 2; kind++) DestroyWindow(buttons[kind]);
    DeleteObject(font);
}

/* A window of every kind the theme serves, themed, drawn, then destroyed with
 * its controls. */
static void ThemedRound(void)
{
    static const struct { const WCHAR *windowClass, *text; DWORD style; int height; } kControls[] = {
        { WC_BUTTONW, L"Push", BS_PUSHBUTTON, 28 }, { WC_BUTTONW, L"Default", BS_DEFPUSHBUTTON, 28 },
        { WC_BUTTONW, L"Check", BS_AUTOCHECKBOX, 20 }, { WC_EDITW, L"One line", ES_AUTOHSCROLL | WS_BORDER, 28 },
        { WC_EDITW, L"Several\r\nlines", ES_MULTILINE | WS_VSCROLL | WS_BORDER, 60 },
        { WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 120 },
        { WC_LISTVIEWW, L"", LVS_REPORT, 80 }, { WC_TREEVIEWW, L"", TVS_INFOTIP, 80 }, { WC_LISTBOXW, L"", LBS_NOINTEGRALHEIGHT, 80 },
        { WC_STATICW, L"A label too long for its width, cut at its end", SS_LEFT | SS_ENDELLIPSIS, 20 },
        { WC_LINK, L"A <a href=\"https://example.com\">link</a>", 0, 20 },
    };
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, WC_STATICW, L"",
                                  WS_POPUP | WS_CLIPCHILDREN, 0, 0, 300, 900, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HFONT font = DialogFont();
    size_t i;
    int top = 4;
    if (!dialog) {
        DeleteObject(font);
        return;
    }
    SetLayeredWindowAttributes(dialog, 0, 1, LWA_ALPHA);
    for (i = 0; i < ARRAYSIZE(kControls); i++) {
        HWND control = CreateWindowExW(0, kControls[i].windowClass, kControls[i].text, WS_CHILD | WS_VISIBLE | kControls[i].style,
                                       4, top, 200, kControls[i].height, dialog, NULL, GetModuleHandleW(NULL), NULL);
        top += kControls[i].height + 4;
        if (!control) continue;
        SendMessageW(control, WM_SETFONT, (WPARAM)font, FALSE);
        if (wcscmp(kControls[i].windowClass, WC_COMBOBOXW) == 0) SendMessageW(control, CB_ADDSTRING, 0, (LPARAM)L"Blue");
        if (wcscmp(kControls[i].windowClass, WC_LISTVIEWW) == 0) {
            LVCOLUMNW column = { 0 };
            LVITEMW item = { 0 };
            column.mask = LVCF_WIDTH;
            column.cx = 100;
            ListView_InsertColumn(control, 0, &column);
            item.mask = LVIF_TEXT;
            item.pszText = (LPWSTR)L"Row";
            ListView_InsertItem(control, &item);
            Theme_SmoothView(control);
        }
        if (wcscmp(kControls[i].windowClass, WC_TREEVIEWW) == 0) {
            TVINSERTSTRUCTW insert = { 0 };
            insert.hParent = TVI_ROOT;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT;
            insert.item.pszText = (LPWSTR)L"Row";
            TreeView_InsertItem(control, &insert);
            Theme_SmoothView(control);
        }
        if (wcscmp(kControls[i].windowClass, WC_LISTBOXW) == 0) {
            SendMessageW(control, LB_ADDSTRING, 0, (LPARAM)L"Row");
            Theme_SmoothView(control);
        }
    }
    Theme_Apply(dialog);
    ShowWindow(dialog, SW_SHOWNOACTIVATE);
    RedrawWindow(dialog, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    PumpMessages();
    DestroyWindow(dialog);
    PumpMessages();
    DeleteObject(font);
}

/* Destroying a themed window and its controls frees what the theme made for
 * them: rounds of making, theming, drawing and destroying keep the process's
 * GDI and USER objects (the first rounds warm Windows' own caches). */
static void TestResourceLifetime(void)
{
    DWORD gdi, user, gdiAfter, userAfter;
    int round;
    for (round = 0; round < 2; round++) ThemedRound();
    gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (round = 0; round < 4; round++) ThemedRound();
    gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    Check("resources: themed windows destroyed leave no GDI object behind", gdi > 0 && gdiAfter == gdi);
    Check("resources: themed windows destroyed leave no USER object behind", user > 0 && userAfter == user);
    if (gdiAfter != gdi || userAfter != user) printf("        GDI objects %lu then %lu, USER objects %lu then %lu\n", gdi, gdiAfter, user, userAfter);
}

/* The line between parts of a window: the field's color (its gray in dark
 * mode, the window color in light mode), the text color in a contrast theme. */
static void TestSeparatorColor(void)
{
    COLORREF expected = HighContrastOn() ? GetSysColor(COLOR_BTNTEXT) : Theme_Color(THEME_FIELD);
    Canvas line = { 0 };
    RECT all = { 0, 0, 4, 4 };
    CheckColor("the separator color follows the mode", expected, Theme_Color(THEME_SEPARATOR));
    if (CanvasOpen(&line, 4, 4, RGB(1, 2, 3))) {
        FillRect(line.dc, &all, Theme_Brush(THEME_SEPARATOR));
        CheckColor("the separator brush paints the separator color", Theme_Color(THEME_SEPARATOR), PixelAt(&line, 2, 2));
        CanvasClose(&line);
    } else Check("separator: canvas created", FALSE);
}

/* A list box without a frame (a side bar, alone or in a view) takes the
 * window's face; a framed one keeps the field's colors (dark) or Windows'
 * own (light). */
static void TestSideBarColors(void)
{
    HWND host = ThemedHost(), bare, inView, framed;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        Check("side bar colors: device context created", FALSE);
        return;
    }
    bare = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | LBS_NOINTEGRALHEIGHT, 10, 10, 100, 60, host, NULL, GetModuleHandleW(NULL), NULL);
    inView = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | LBS_NOINTEGRALHEIGHT, 120, 10, 100, 60, host, NULL, GetModuleHandleW(NULL), NULL);
    framed = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_BORDER | LBS_NOINTEGRALHEIGHT, 230, 10, 100, 60, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!bare || !inView || !framed) {
        Check("side bar colors: list boxes created", FALSE);
        if (inView) DestroyWindow(inView);
    } else {
        HWND view = Theme_SmoothView(inView);
        HWND sideBars[2];
        const char *names[2] = { "a list box without a frame", "a list box without a frame in a view" };
        INT_PTR brush;
        int i;
        sideBars[0] = bare;
        sideBars[1] = inView;
        Theme_Apply(host);
        for (i = 0; i < 2; i++) {
            char name[160];
            SetTextColor(dc, RGB(1, 2, 3));
            SetBkColor(dc, RGB(1, 2, 3));
            brush = Theme_CtlColor(WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)sideBars[i], 0);
            StringCchPrintfA(name, ARRAYSIZE(name), "side bar colors: %s takes the face, with the text color", names[i]);
            Check(name, brush == (INT_PTR)Theme_Brush(THEME_FACE) && GetTextColor(dc) == Theme_Color(THEME_TEXT) && GetBkColor(dc) == Theme_Color(THEME_FACE));
        }
        SetTextColor(dc, RGB(1, 2, 3));
        SetBkColor(dc, RGB(1, 2, 3));
        brush = Theme_CtlColor(WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)framed, 0);
        if (Theme_IsDark())
            Check("side bar colors: a framed list box keeps the field's colors", brush == (INT_PTR)Theme_Brush(THEME_FIELD) &&
                  GetTextColor(dc) == Theme_Color(THEME_TEXT) && GetBkColor(dc) == Theme_Color(THEME_FIELD));
        else Check("side bar colors: a framed list box keeps Windows' own colors", brush == 0 && GetTextColor(dc) == RGB(1, 2, 3));
        DestroyWindow(view);
    }
    if (bare) DestroyWindow(bare);
    if (framed) DestroyWindow(framed);
    DeleteDC(dc);
}

/* --------------------------------------------------------------- links */

/* How much bluer than red the ink of x0..x1 is, on average: ClearType's
 * colored fringes even out, a colored text does not. */
static int InkBlueness(const Canvas *canvas, int x0, int x1, int y0, int y1, COLORREF back, int *green)
{
    long red = 0, blue = 0, greens = 0, inked = 0;
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            COLORREF pixel = PixelAt(canvas, x, y);
            if (!StandsOut(pixel, back)) continue;
            red += GetRValue(pixel);
            greens += GetGValue(pixel);
            blue += GetBValue(pixel);
            inked++;
        }
    if (green) *green = inked ? (int)(greens / inked) : 0;
    return inked ? (int)((blue - red) / inked) : 0;
}

/* A link control's link is drawn in a blue that reads on the dark
 * background, its other text not. */
static void TestLink(void)
{
    HWND host = ThemedHost(), link;
    HFONT font;
    RECT window;
    Canvas canvas;
    int plain, linked, green;
    if (SkipInLight("link")) return;
    link = CreateWindowExW(0, WC_LINK, L"Plain text here <a href=\"https://example.com\">a link</a>",
                           WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT | LWS_USECUSTOMTEXT, 10, 10, 300, 20, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!link) {
        Check("link: created", FALSE);
        return;
    }
    font = DialogFont();
    SendMessageW(link, WM_SETFONT, (WPARAM)font, FALSE);
    SetWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS, 0);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    GetClientRect(link, &window);
    if (!CanvasOpen(&canvas, window.right, window.bottom, RGB(0, 0, 0)) || !CopyClient(link, &canvas)) {
        Check("link: captured", FALSE);
    } else {
        if (HasImage(&canvas, "link")) {
            COLORREF back = PixelAt(&canvas, window.right - 2, 1);
            plain = InkBlueness(&canvas, 0, 60, 0, window.bottom, back, NULL);   /* "Plain text" */
            linked = InkBlueness(&canvas, 100, window.right, 0, window.bottom, back, &green);
            Check("a link is drawn in the link blue", linked > 60);
            Check("a link's blue is light enough for the dark background (not Windows' own dark blue)", green > 120);
            Check("a link control's text before its link is not blue", plain < 30 && plain > -30);
            if (linked <= 60 || green <= 120 || plain >= 30 || plain <= -30) printf("        blueness: text %d, link %d (green %d)\n", plain, linked, green);
        }
    }
    CanvasClose(&canvas);
    ShowWindow(host, SW_HIDE);
    RemoveWindowSubclass(host, ThemedColors, THEMED_COLORS_SUBCLASS);
    DestroyWindow(link);
    DeleteObject(font);
}

/* ------------------------------------------------------- the profile note */

/* The note shortens a long name only between characters: a cut never leaves
 * half of an emoji's surrogate pair before its ellipsis. */
static void TestSidebarNoteCuts(void)
{
    static const WCHAR kFormat[] = L"Opens \x201C%s\x201D.";
    WCHAR names[2][LABEL_CCH];
    HWND host = ThemedHost(), note;
    HFONT font = DialogFont();
    TEXTMETRICW metrics;
    HDC dc;
    size_t i, kind;
    int width, cuts = 0;
    BOOL whole = TRUE;
    /* 24 emoji, then 16 letter-and-emoji pairs: MAX_LABEL characters each. */
    for (i = 0; i < 24; i++) {
        names[0][2 * i] = 0xD83D;
        names[0][2 * i + 1] = 0xDE00;
    }
    names[0][48] = 0;
    for (i = 0; i < 16; i++) {
        names[1][3 * i] = L'A';
        names[1][3 * i + 1] = 0xD83D;
        names[1][3 * i + 2] = 0xDE80;
    }
    names[1][48] = 0;
    note = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 10, 10, 300, 40, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!note) {
        Check("note cuts: private note created", FALSE);
        DeleteObject(font);
        return;
    }
    SendMessageW(note, WM_SETFONT, (WPARAM)font, FALSE);
    Theme_Apply(host);
    dc = GetDC(note);
    if (dc) {
        HGDIOBJ previous = SelectObject(dc, font);
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        ReleaseDC(note, dc);
    } else metrics.tmHeight = metrics.tmExternalLeading = 16;
    for (kind = 0; kind < ARRAYSIZE(names); kind++) for (width = 60; width <= 360; width += 7) {
        WCHAR text[512];
        const WCHAR *open, *close;
        size_t shown, length = wcslen(names[kind]);
        SetWindowPos(note, NULL, 0, 0, width, metrics.tmHeight + metrics.tmExternalLeading + 2, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Theme_LayoutSidebarNote(note, kFormat, names[kind]);
        GetWindowTextW(note, text, ARRAYSIZE(text));
        open = wcschr(text, 0x201C);
        close = open ? wcschr(open + 1, 0x201D) : NULL;
        if (!open || !close) {
            whole = FALSE;
            continue;
        }
        shown = (size_t)(close - open - 1);
        if (shown == length && wcsncmp(open + 1, names[kind], length) == 0) continue;
        cuts++;
        /* A cut name: a prefix of whole characters, then the ellipsis. */
        if (shown < 1 || open[shown] != 0x2026 || wcsncmp(open + 1, names[kind], shown - 1) != 0 || (shown >= 2 && IS_HIGH_SURROGATE(open[shown - 1])))
            whole = FALSE;
    }
    Check("note cuts: some widths cut the name", cuts > 0);
    Check("note cuts: a cut name ends between characters, never inside a surrogate pair", whole);
    DestroyWindow(note);
    DeleteObject(font);
}

/* -------------------------------------------------------------- buffers */

static void TestBuffer(void)
{
    Canvas target;
    ThemeBuffer buffer;
    RECT part = { 10, 10, 20, 20 };
    HDC dc;
    if (!CanvasOpen(&target, 30, 30, RGB(0, 0, 0))) {
        Check("buffer: canvas created", FALSE);
        return;
    }
    dc = Theme_BufferBegin(&buffer, target.dc, &part);
    Check("drawing off screen gets its own DC", dc != NULL && dc != target.dc);
    if (dc) {
        SetDCBrushColor(dc, RGB(255, 255, 255));
        FillRect(dc, &part, (HBRUSH)GetStockObject(DC_BRUSH));
        CheckColor("nothing drawn off screen shows before the buffer ends", RGB(0, 0, 0), PixelAt(&target, 15, 15));
        Theme_BufferEnd(&buffer);
        CheckColor("all of an off-screen drawing shows when it ends, where it was drawn", RGB(255, 255, 255), PixelAt(&target, 10, 10));
        CheckColor("an off-screen drawing shows nothing around its rectangle", RGB(0, 0, 0), PixelAt(&target, 9, 9));
    }
    CanvasClose(&target);
}

int wmain(void)
{
    INITCOMMONCONTROLSEX controls = { sizeof controls, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    setvbuf(stdout, NULL, _IONBF, 0);
    /* As the program runs (app.res): common controls 6 (the link class needs
     * them) and per-monitor DPI. */
    if (!InitCommonControlsEx(&controls) ||
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        printf("test_theme must be linked with the program's manifest (app.res, /MANIFEST:NO).\n");
        return 1;
    }
    g_hInst = GetModuleHandleW(NULL);
    Localize_SetLanguage(Localize_LanguageForCode(L"en"), FALSE);
    Theme_Init();
    printf("Windows apps: %s\n", Theme_IsDark() ? "dark" : "light");
    TestRows();
    TestRoundedRowBounds();
    TestRoundedControlRegions();
    TestReportGlyphs();
    TestTableEdges();
    TestHorizontalScrollFrames();
    TestColumnOwnership();
    TestTableComponentStyles();
    TestTableReentrantDestroy();
    TestCheckBoxMetrics();
    TestCellTips();
    TestStatusTip();
    TestEdit();
    TestEditInput();
    TestEditReentrantPause();
    TestEditPrinting();
    TestPushButtons();
    TestDropDownButton();
    TestDropDownWidth();
    TestDropDownList();
    TestComboInput();
    TestScrollBarEdges();
    TestScrollBarColors();
    TestWheel();
    TestSmoothView();
    TestWideListInView();
    TestViewPaging();
    TestViewKeepsPlace();
    TestTallViewWheel();
    TestTallControlGivenView();
    TestViewFollowsSelection();
    TestViewKeys();
    TestViewRevealsKeyboardRow();
    TestWheelStops();
    TestSmoothViewIdentity();
    TestLastColumn();
    TestKeysAndLayout();
    TestViewKeepsTopRow();
    TestFitLastColumn();
    TestViewFrameAfterTheming();
    TestDarkHeaderAlignment();
    TestTipsUnderWheel();
    TestScrollTarget();
    TestTreeGlyph();
    TestFocusCueAndMutedText();
    TestSeparatorColor();
    TestSideBarColors();
    TestLink();
    TestSidebarNoteCuts();
    TestBuffer();
    TestResourceLifetime();
    if (g_themedHost) DestroyWindow(g_themedHost);
    printf("Theme tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
