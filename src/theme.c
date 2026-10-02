/*
 * The look of every window, in one place, following the Windows "app mode"
 * setting (Settings > Personalization > Colors), and the message box every
 * part of the program uses (the system MessageBox and TaskDialog have no dark
 * mode).
 *
 * Every dialog opens through Ui_Dialog, which centers it on its owner and
 * themes it. Theme_Apply on a dialog themes it and its controls, and again
 * after a theme change: push buttons, check boxes, list views and their
 * headers, trees, edits, drop-down lists, their frames, focus rectangles and
 * wheel scrolling (every list by the pixel, in a view: Theme_SmoothView).
 * What a window draws itself (the sessions view) takes its colors, fonts,
 * rows, folder arrows and buttons from here (Theme_Color, Theme_CreateFonts,
 * Theme_DrawRow, Theme_DrawTreeGlyph, Theme_DrawButton, Theme_DrawDropDown),
 * so that every list, selection, button and drop-down looks the same.
 *
 * Dark mode for Win32 controls: the title bar uses the documented
 * DWMWA_USE_IMMERSIVE_DARK_MODE; the controls use the DarkMode_* visual-style
 * classes, enabled through three uxtheme exports that Windows only exposes by
 * ordinal (the approach used by Windows' own inbox Win32 apps and by common
 * editors). When they are missing everything stays light. Where a dark class
 * falls short (light frames, black captions, a header whose dividers miss the
 * rows' by a pixel, the system blue on selected text), it is drawn here.
 *
 * Painting without flicker: a control that repaints often (an edit while a
 * selection is dragged, a drop-down list under the mouse) and what a window draws
 * itself are painted off screen first (Theme_BufferBegin), then shown at once;
 * anything painted over a control's own drawing is painted right after it,
 * never on a timer.
 *
 * Layout: every dialog is fitted to its captions in all the interface
 * languages, the rows it hides closed (Theme_FitDialog), and the manager
 * window's controls are measured and placed by the main-window policy
 * (Theme_MainMinimum, Theme_LayoutMain).
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>

/* What the theme keeps on the windows it serves. */
#define THEME_PROP        L"ClaudeDesktopProfilesManager.Theme"           /* THEMED_LIGHT or THEMED_DARK */
#define DIALOG_FONT_PROP  L"ClaudeDesktopProfilesManager.DialogFont"
#define DIALOG_BASE_PROP  L"ClaudeDesktopProfilesManager.DialogBase"
#define MAIN_BUDGET_PROP  L"ClaudeDesktopProfilesManager.MainBudget"
#define SHELL_PROP        L"ClaudeDesktopProfilesManager.Dialog"
#define STRONG_PROP       L"ClaudeDesktopProfilesManager.Strong"          /* the semibold font a control shows its text in */
#define TIP_PROP          L"ClaudeDesktopProfilesManager.CellTip"
#define TABLE_STATE_PROP  L"ClaudeDesktopProfilesManager.TableState"
#define HOT_PROP          L"ClaudeDesktopProfilesManager.Hot"             /* an edit or drop-down list under the mouse */
#define CHOICE_PROP       L"ClaudeDesktopProfilesManager.Choice"          /* CHOICE_QUEUED or CHOICE_TRACKING */
#define BORDER_PROP       L"ClaudeDesktopProfilesManager.Border"          /* the frame and scroll bar a control was made with */
#define EDIT_PAUSED_PROP  L"ClaudeDesktopProfilesManager.EditPaused"
#define EDIT_UPDATE_PROP  L"ClaudeDesktopProfilesManager.EditUpdate"
#define CORNER_PROP       L"ClaudeDesktopProfilesManager.ControlCorners"

#define THEMED_LIGHT    1
#define THEMED_DARK     2
#define CHOICE_QUEUED   1
#define CHOICE_TRACKING 2   /* its native menu shows */

#define LIST_SUBCLASS    1
#define CHILD_SUBCLASS   2
#define DIALOG_SUBCLASS  3
#define SCROLL_SUBCLASS  4
#define VIEW_SUBCLASS    5
#define TIP_SUBCLASS     6

#define WM_THEME_CHOICE        (WM_APP + 0x52)
#define WM_THEME_DIALOG_LAYOUT (WM_APP + 0x53)
#define WM_THEME_MESSAGE_DPI   (WM_APP + 0x54)
#define WM_THEME_FRAME         (WM_APP + 0x55)
#define VIEW_MEASURE           (WM_APP + 0x3E0)

typedef struct DialogControlBase {
    HWND window;
    RECT rectangle;
} DialogControlBase;

typedef struct DialogBase {
    LOGFONTW font;
    UINT fontDpi, layoutDpi;
    BOOL hasLayout;
    SIZE client;
    int count;
    DialogControlBase controls[128];
} DialogBase;

typedef int (WINAPI *SetPreferredAppModeFn)(int mode);      /* uxtheme #135 */
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL); /* uxtheme #133 */
typedef void (WINAPI *FlushMenuThemesFn)(void);              /* uxtheme #136 */

static AllowDarkModeForWindowFn g_allowDarkModeForWindow;
static FlushMenuThemesFn g_flushMenuThemes;
static BOOL g_dark, g_highContrast;

/* `dips` at the window's scale. */
static int ScaleForWindow(HWND window, int dips)
{
    UINT dpi = window ? GetDpiForWindow(window) : 0;
    return MulDiv(dips, dpi ? (int)dpi : 96, 96);
}

/* A one-DIP line: never thinner than a pixel. */
static int LineWidth(HWND window)
{
    return max(1, ScaleForWindow(window, 1));
}

static UINT ReadingFlagsAt(int language)
{
    return Localize_IsRTLAt(language) ? DT_RTLREADING : 0;
}

/* ---------------------------------------------------------------- palette */

typedef struct Palette {
    COLORREF color[THEME_COLORS];
    COLORREF button, buttonOff;           /* a dark push button's fill; a disabled one's frame */
    COLORREF header, divider;             /* a dark list header's background and dividers */
} Palette;

/* Dark: the colors of Explorer's dark theme. The three blues are those of a
 * list view row there (DarkMode_Explorer::ListView) over the field: selected,
 * its frame, under the mouse. They are read from Windows' theme at start
 * (ReadRowColors), so the rows drawn here match the list views on every
 * Windows version; these are Windows 11's, kept when the theme cannot be
 * read. tests/test_theme.c compares the rows with Windows'. */
static const Palette kDark = {
    { RGB(0xF2, 0xF2, 0xF2),    /* text */
      RGB(0x9A, 0x9A, 0x9A),    /* muted */
      RGB(0x20, 0x20, 0x20),    /* face */
      RGB(0x2B, 0x2B, 0x2B),    /* field */
      RGB(0x22, 0x3E, 0x55),    /* main blue */
      RGB(0x00, 0x78, 0xD4),    /* bright blue */
      RGB(0x27, 0x35, 0x41),    /* pale blue */
      RGB(0x2B, 0x2B, 0x2B) },  /* separator: the field's gray */
    RGB(0x33, 0x33, 0x33), RGB(0x40, 0x40, 0x40), RGB(0x19, 0x19, 0x19), RGB(0x63, 0x63, 0x63)
};

/* Light: the same from Explorer's light list view (Explorer::ListView). */
#define LIGHT_MUTED           RGB(0x6E, 0x6E, 0x6E)
#define LIGHT_MAIN_BLUE       RGB(0xCC, 0xE8, 0xFF)
#define LIGHT_PALE_BLUE       RGB(0xE5, 0xF3, 0xFF)

static Palette g_palette;
static HBRUSH  g_brush[THEME_COLORS];

static BOOL ReadHighContrast(void)
{
    HIGHCONTRASTW highContrast;
    ZeroMemory(&highContrast, sizeof highContrast);
    highContrast.cbSize = sizeof highContrast;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof highContrast, &highContrast, 0) &&
           (highContrast.dwFlags & HCF_HIGHCONTRASTON);
}

/* Dark only when the user picked dark apps and no contrast theme is on: a
 * contrast theme's system colors always win. Painting reads the result, not
 * the setting: it changes only through Theme_Follow. */
static void ReadSystemTheme(void)
{
    DWORD light = 1;
    g_highContrast = ReadHighContrast();
    if (!g_highContrast) Util_RegGetDword(HKEY_CURRENT_USER, REG_PERSONALIZE, L"AppsUseLightTheme", &light);
    g_dark = !g_highContrast && light == 0;
}

static void FillSolid(HDC dc, const RECT *rc, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

/* A 32-bit DIB pixel's color. */
static COLORREF PixelColor(DWORD pixel)
{
    return RGB((pixel >> 16) & 0xFF, (pixel >> 8) & 0xFF, pixel & 0xFF);
}

static COLORREF SamplePixel(const DWORD *pixels, int width, int x, int y)
{
    return PixelColor(pixels[y * width + x]);
}

/* The row colors as this Windows draws a list view row over the field:
 * selected (fill, frame) and under the mouse (fill). The theme's row images
 * are translucent, so they are drawn over the field and read back. Keeps the
 * measured values when the theme is not there. */
static void ReadRowColors(Palette *palette, const WCHAR *themeClass)
{
    enum { SampleWidth = 24, SampleHeight = 12 };
    BITMAPINFO bitmapInfo;
    HTHEME theme = OpenThemeData(NULL, themeClass);
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = NULL;
    HGDIOBJ old;
    void *bits = NULL;
    RECT rc = { 0, 0, SampleWidth, SampleHeight };
    int pass;
    if (theme && dc) {
        ZeroMemory(&bitmapInfo, sizeof bitmapInfo);
        bitmapInfo.bmiHeader.biSize = sizeof bitmapInfo.bmiHeader;
        bitmapInfo.bmiHeader.biWidth = SampleWidth;
        bitmapInfo.bmiHeader.biHeight = -SampleHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &bits, NULL, 0);
    }
    if (bitmap) {
        const DWORD *pixels = (const DWORD *)bits;
        old = SelectObject(dc, bitmap);
        for (pass = 0; pass < 2; pass++) {
            int state = pass == 0 ? LISS_SELECTED : LISS_HOT;
            FillSolid(dc, &rc, palette->color[THEME_FIELD]);
            if (FAILED(DrawThemeBackground(theme, dc, LVP_LISTITEM, state, &rc, NULL))) break;
            GdiFlush();
            if (pass == 0) {
                palette->color[THEME_MAIN_BLUE] = SamplePixel(pixels, SampleWidth, SampleWidth / 2, SampleHeight / 2);
                palette->color[THEME_BRIGHT_BLUE] = SamplePixel(pixels, SampleWidth, SampleWidth / 2, 0);
            } else {
                palette->color[THEME_PALE_BLUE] = SamplePixel(pixels, SampleWidth, SampleWidth / 2, SampleHeight / 2);
            }
        }
        SelectObject(dc, old);
        DeleteObject(bitmap);
    }
    if (dc) DeleteDC(dc);
    if (theme) CloseThemeData(theme);
}

/* Every open dialog follows the same broadcast: the brushes are made again
 * only when a color changed. */
static void UpdatePalette(void)
{
    Palette previous = g_palette;
    int i;
    if (g_dark) {
        g_palette = kDark;
        ReadRowColors(&g_palette, L"DarkMode_Explorer::ListView");
    } else {
        ZeroMemory(&g_palette, sizeof g_palette);
        g_palette.color[THEME_TEXT] = GetSysColor(COLOR_WINDOWTEXT);
        g_palette.color[THEME_MUTED] = g_highContrast ? GetSysColor(COLOR_GRAYTEXT) : LIGHT_MUTED;
        g_palette.color[THEME_FACE] = GetSysColor(COLOR_3DFACE);
        g_palette.color[THEME_FIELD] = GetSysColor(COLOR_WINDOW);
        g_palette.color[THEME_MAIN_BLUE] = g_highContrast ? GetSysColor(COLOR_HIGHLIGHT) : LIGHT_MAIN_BLUE;
        g_palette.color[THEME_BRIGHT_BLUE] = g_highContrast ? GetSysColor(COLOR_HIGHLIGHT) : kDark.color[THEME_BRIGHT_BLUE];
        g_palette.color[THEME_PALE_BLUE] = g_highContrast ? GetSysColor(COLOR_WINDOW) : LIGHT_PALE_BLUE;
        /* A contrast theme's face and field can be the same color: its text color stays visible on the face. */
        g_palette.color[THEME_SEPARATOR] = GetSysColor(g_highContrast ? COLOR_BTNTEXT : COLOR_WINDOW);
        if (!g_highContrast) ReadRowColors(&g_palette, L"Explorer::ListView");
    }
    if (g_brush[0] && memcmp(&previous, &g_palette, sizeof g_palette) == 0) return;
    for (i = 0; i < THEME_COLORS; i++) {
        if (g_brush[i]) DeleteObject(g_brush[i]);
        g_brush[i] = CreateSolidBrush(g_palette.color[i]);
    }
}

void Theme_Init(void)
{
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme) {
        SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
        g_allowDarkModeForWindow = (AllowDarkModeForWindowFn)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
        g_flushMenuThemes = (FlushMenuThemesFn)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
        if (setMode) setMode(1); /* "allow dark": follow the system setting */
        if (g_flushMenuThemes) g_flushMenuThemes();
    }
    ReadSystemTheme();
    UpdatePalette();
}

BOOL Theme_IsDark(void) { return g_dark; }

COLORREF Theme_Color(ThemeColor color)
{
    return color >= 0 && color < THEME_COLORS ? g_palette.color[color] : g_palette.color[THEME_TEXT];
}

HBRUSH Theme_Brush(ThemeColor color)
{
    return color >= 0 && color < THEME_COLORS ? g_brush[color] : g_brush[THEME_FIELD];
}

/* --------------------------------------------------------- rounded boxes */

/* GDI+ (a system DLL, loaded on first use) draws anti-aliased rounded
 * corners; without it GDI's RoundRect does, jagged. */
typedef struct GdipStartupInput { UINT32 version; void *debugCallback; BOOL noBackgroundThread; BOOL noCodecs; } GdipStartupInput;
typedef int (WINAPI *GdiplusStartupFn)(ULONG_PTR *, const GdipStartupInput *, void *);
typedef int (WINAPI *GdipCreateFromHDCFn)(HDC, void **);
typedef int (WINAPI *GdipSetSmoothingModeFn)(void *, int);
typedef int (WINAPI *GdipCreatePathFn)(int, void **);
typedef int (WINAPI *GdipAddPathArcFn)(void *, float, float, float, float, float, float);
typedef int (WINAPI *GdipClosePathFigureFn)(void *);
typedef int (WINAPI *GdipCreateSolidFillFn)(DWORD, void **);
typedef int (WINAPI *GdipFillPathFn)(void *, void *, void *);
typedef int (WINAPI *GdipCreatePen1Fn)(DWORD, float, int, void **);
typedef int (WINAPI *GdipDrawPathFn)(void *, void *, void *);
typedef int (WINAPI *GdipDeleteFn)(void *);

#define GDIP_SMOOTHING_ANTIALIAS 4
#define GDIP_UNIT_PIXEL          2

static struct {
    BOOL                   tried, ready;
    GdipCreateFromHDCFn    createFromHdc;
    GdipSetSmoothingModeFn setSmoothing;
    GdipCreatePathFn       createPath;
    GdipAddPathArcFn       addArc;
    GdipClosePathFigureFn  closeFigure;
    GdipCreateSolidFillFn  createFill;
    GdipFillPathFn         fillPath;
    GdipCreatePen1Fn       createPen;
    GdipDrawPathFn         drawPath;
    GdipDeleteFn           deleteBrush, deletePen, deletePath, deleteGraphics;
} g_gdip;

static BOOL GdiplusReady(void)
{
    GdipStartupInput input = { 1, NULL, FALSE, FALSE };
    GdiplusStartupFn startup;
    ULONG_PTR token;
    HMODULE dll;
    if (g_gdip.tried) return g_gdip.ready;
    g_gdip.tried = TRUE;
    if ((dll = LoadLibraryExW(L"gdiplus.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) == NULL) return FALSE;
    startup = (GdiplusStartupFn)(void *)GetProcAddress(dll, "GdiplusStartup");
    g_gdip.createFromHdc = (GdipCreateFromHDCFn)(void *)GetProcAddress(dll, "GdipCreateFromHDC");
    g_gdip.setSmoothing = (GdipSetSmoothingModeFn)(void *)GetProcAddress(dll, "GdipSetSmoothingMode");
    g_gdip.createPath = (GdipCreatePathFn)(void *)GetProcAddress(dll, "GdipCreatePath");
    g_gdip.addArc = (GdipAddPathArcFn)(void *)GetProcAddress(dll, "GdipAddPathArc");
    g_gdip.closeFigure = (GdipClosePathFigureFn)(void *)GetProcAddress(dll, "GdipClosePathFigure");
    g_gdip.createFill = (GdipCreateSolidFillFn)(void *)GetProcAddress(dll, "GdipCreateSolidFill");
    g_gdip.fillPath = (GdipFillPathFn)(void *)GetProcAddress(dll, "GdipFillPath");
    g_gdip.createPen = (GdipCreatePen1Fn)(void *)GetProcAddress(dll, "GdipCreatePen1");
    g_gdip.drawPath = (GdipDrawPathFn)(void *)GetProcAddress(dll, "GdipDrawPath");
    g_gdip.deleteBrush = (GdipDeleteFn)(void *)GetProcAddress(dll, "GdipDeleteBrush");
    g_gdip.deletePen = (GdipDeleteFn)(void *)GetProcAddress(dll, "GdipDeletePen");
    g_gdip.deletePath = (GdipDeleteFn)(void *)GetProcAddress(dll, "GdipDeletePath");
    g_gdip.deleteGraphics = (GdipDeleteFn)(void *)GetProcAddress(dll, "GdipDeleteGraphics");
    if (!startup || !g_gdip.createFromHdc || !g_gdip.setSmoothing || !g_gdip.createPath || !g_gdip.addArc ||
        !g_gdip.closeFigure || !g_gdip.createFill || !g_gdip.fillPath || !g_gdip.createPen || !g_gdip.drawPath ||
        !g_gdip.deleteBrush || !g_gdip.deletePen || !g_gdip.deletePath || !g_gdip.deleteGraphics || startup(&token, &input, NULL) != 0)
        return FALSE;
    g_gdip.ready = TRUE;
    return TRUE;
}

static DWORD Argb(COLORREF color)
{
    return 0xFF000000u | ((DWORD)GetRValue(color) << 16) | ((DWORD)GetGValue(color) << 8) | GetBValue(color);
}

/* `fill` inside a `width` px `frame`, corners of `radius` px, within `rc`. */
static void RoundedBox(HDC dc, const RECT *rc, int radius, COLORREF fill, COLORREF frame, int width)
{
    void *graphics = NULL, *path = NULL, *brush = NULL, *pen = NULL;
    float inset = width > 0 ? (width - 1) / 2.0f : 0, diameter, left, top, right, bottom;
    radius = max(0, min(radius, min(rc->right - rc->left, rc->bottom - rc->top) / 2));
    diameter = 2.0f * radius;
    left = rc->left + inset;
    top = rc->top + inset;
    right = rc->right - 1 - inset;
    bottom = rc->bottom - 1 - inset;
    if (GdiplusReady() && g_gdip.createFromHdc(dc, &graphics) == 0) {
        g_gdip.setSmoothing(graphics, GDIP_SMOOTHING_ANTIALIAS);
        if (g_gdip.createPath(0, &path) == 0) {
            g_gdip.addArc(path, left, top, diameter, diameter, 180.0f, 90.0f);
            g_gdip.addArc(path, right - diameter, top, diameter, diameter, 270.0f, 90.0f);
            g_gdip.addArc(path, right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
            g_gdip.addArc(path, left, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
            g_gdip.closeFigure(path);
            if (g_gdip.createFill(Argb(fill), &brush) == 0) g_gdip.fillPath(graphics, brush, path);
            if (width > 0 && g_gdip.createPen(Argb(frame), (float)width, GDIP_UNIT_PIXEL, &pen) == 0) g_gdip.drawPath(graphics, pen, path);
            if (brush) g_gdip.deleteBrush(brush);
            if (pen) g_gdip.deletePen(pen);
            g_gdip.deletePath(path);
        }
        g_gdip.deleteGraphics(graphics);
    } else {
        HBRUSH solid = CreateSolidBrush(fill);
        HPEN outline = width > 0 ? CreatePen(PS_INSIDEFRAME, width, frame) : (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ oldBrush = SelectObject(dc, solid), oldPen = SelectObject(dc, outline);
        RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, 2 * radius, 2 * radius);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(solid);
        if (width > 0) DeleteObject(outline);
    }
}

/* ------------------------------------------------------------------- rows */

static int CornerRadius(HWND owner)
{
    return max(1, ScaleForWindow(owner, THEME_CORNER_RADIUS_DIPS));
}

/* A row of a list a window draws itself, as the list views draw theirs:
 * selected, a main blue fill in a bright blue frame; under the mouse, pale
 * blue; corners softened; else `around`, the list's background. Returns the
 * color for its text. */
COLORREF Theme_DrawRow(HWND owner, HDC dc, const RECT *rc, UINT state, COLORREF around)
{
    int radius = min(CornerRadius(owner), min(rc->right - rc->left, rc->bottom - rc->top) / 2);
    FillSolid(dc, rc, around);
    if (state & THEME_ROW_SELECTED) {
        if (g_highContrast) FillSolid(dc, rc, GetSysColor(COLOR_HIGHLIGHT));
        else RoundedBox(dc, rc, radius, g_palette.color[THEME_MAIN_BLUE], g_palette.color[THEME_BRIGHT_BLUE], LineWidth(owner));
        return g_highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : g_palette.color[THEME_TEXT];
    }
    if ((state & THEME_ROW_HOT) && !g_highContrast) {
        RoundedBox(dc, rc, radius, g_palette.color[THEME_PALE_BLUE], g_palette.color[THEME_PALE_BLUE], 0);
    }
    return g_palette.color[THEME_TEXT];
}

/* Secondary text on such a row: gray, but on a dark or contrast selection
 * the row's own text color, which reads better there. */
COLORREF Theme_RowMuted(UINT state)
{
    if ((state & THEME_ROW_SELECTED) && g_highContrast) return GetSysColor(COLOR_HIGHLIGHTTEXT);
    if ((state & THEME_ROW_SELECTED) && g_dark) return g_palette.color[THEME_TEXT];
    return g_palette.color[THEME_MUTED];
}

/* The arrow that opens or closes a folder of a tree a window draws itself,
 * centered in `cell`: from the tree's own theme (the one ThemeChild gives
 * it), hot under the mouse (`state` THEME_ROW_*). Without a theme, a text
 * arrow in the row's secondary color. */
void Theme_DrawTreeGlyph(HWND tree, HDC dc, const RECT *cell, UINT state, BOOL open)
{
    HTHEME theme = OpenThemeData(tree, L"TreeView");
    RECT box = *cell;
    if (theme) {
        int part = (state & THEME_ROW_HOT) ? TVP_HOTGLYPH : TVP_GLYPH, glyphState = open ? GLPS_OPENED : GLPS_CLOSED;
        SIZE size;
        size.cx = size.cy = (cell->right - cell->left) / 2;
        GetThemePartSize(theme, dc, part, glyphState, NULL, TS_DRAW, &size);
        box.left = cell->left + (cell->right - cell->left - size.cx) / 2;
        box.top = cell->top + (cell->bottom - cell->top - size.cy) / 2;
        box.right = box.left + size.cx;
        box.bottom = box.top + size.cy;
        DrawThemeBackground(theme, dc, part, glyphState, &box, NULL);
        CloseThemeData(theme);
        return;
    }
    SetTextColor(dc, Theme_RowMuted(state));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, open ? L"\x25BE" : L"\x25B8", -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

/* Keyboard cues: a focus rectangle and accelerator underlines show after
 * keyboard use only (a click hides them: HideFocusCues). The cues `control`
 * hides now (UISF_*). */
static UINT HiddenCues(HWND control)
{
    return (UINT)SendMessageW(control, WM_QUERYUISTATE, 0, 0);
}

static BOOL ShowsFocusCues(HWND control)
{
    return !(HiddenCues(control) & UISF_HIDEFOCUS);
}

static UINT TextFormatForCues(HWND control, UINT format, BOOL *showFocus)
{
    *showFocus = ShowsFocusCues(control);
    return format | ((HiddenCues(control) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
}

/* The keyboard's row in a list (`row`, as Theme_DrawRow drew it): a dotted
 * rectangle inside its frame, inverting what it covers, after keyboard use
 * only. */
void Theme_DrawFocusCue(HWND owner, HDC dc, const RECT *row)
{
    RECT focus = *row;
    COLORREF text, back;
    if (!ShowsFocusCues(owner)) return;
    InflateRect(&focus, -LineWidth(owner), -LineWidth(owner));
    text = SetTextColor(dc, RGB(0, 0, 0));   /* DrawFocusRect's pattern: these colors invert what is under it */
    back = SetBkColor(dc, RGB(255, 255, 255));
    DrawFocusRect(dc, &focus);
    SetTextColor(dc, text);
    SetBkColor(dc, back);
}

/* ------------------------------------------------------------------ fonts */

static int CALLBACK FontFound(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found)
{
    (void)lf;
    (void)tm;
    (void)type;
    *(BOOL *)found = TRUE;
    return 0;
}

/* Windows 11's Segoe UI Variable (its "Text" optical size) reads better
 * than Segoe UI in lists; Windows 10 does not have it. */
static BOOL HasVariableFont(void)
{
    static int known = -1;
    if (known < 0) {
        LOGFONTW lf;
        BOOL found = FALSE;
        HDC dc = GetDC(NULL);
        ZeroMemory(&lf, sizeof lf);
        lf.lfCharSet = DEFAULT_CHARSET;
        StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI Variable Text");
        if (dc) {
            EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
            ReleaseDC(NULL, dc);
        }
        known = found;
    }
    return known == 1;
}

/* A language set in Segoe UI uses the variable font where Windows has it. */
static BOOL UsesVariableFont(const WCHAR *face)
{
    return HasVariableFont() && CompareStringOrdinal(face, -1, L"Segoe UI", -1, TRUE) == CSTR_EQUAL;
}

/* The face of semibold text in a language whose font is `face`: Segoe UI's
 * own semibold reads heavy and blurred next to the regular buttons. */
static const WCHAR *StrongFace(const WCHAR *face)
{
    return UsesVariableFont(face) ? L"Segoe UI Variable Text Semibold" : face;
}

#define DIALOG_FONT_POINTS 9

/* The 9 pt Segoe UI of the dialog resources, for a window without a font. */
static void DefaultDialogFont(LOGFONTW *font, UINT dpi)
{
    ZeroMemory(font, sizeof *font);
    font->lfHeight = -MulDiv(DIALOG_FONT_POINTS, dpi ? (int)dpi : 96, 72);
    font->lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(font->lfFaceName, ARRAYSIZE(font->lfFaceName), L"Segoe UI");
}

/* The fonts of what a window draws itself, sized from its dialog font (so at
 * its scale): text 105% of it, headings 120%. Free them with Theme_FreeFonts. */
void Theme_CreateFonts(HWND dialog, ThemeFonts *fonts)
{
    static const struct { int weight, percent; BOOL underline, strike, italic; } kRoles[THEME_FONTS] = {
        { FW_NORMAL, 105, FALSE, FALSE, FALSE },     /* text */
        { FW_SEMIBOLD, 105, FALSE, FALSE, FALSE },   /* strong */
        { FW_SEMIBOLD, 120, FALSE, FALSE, FALSE },   /* heading */
        { FW_SEMIBOLD, 105, TRUE, FALSE, FALSE },    /* current */
        { FW_NORMAL, 105, FALSE, TRUE, FALSE },      /* absent */
        { FW_NORMAL, 105, FALSE, FALSE, TRUE },      /* italic */
    };
    HFONT dialogFont = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    BOOL variable = UsesVariableFont(Localize_FontFace());
    LOGFONTW base, lf;
    int i;
    Theme_FreeFonts(fonts);
    if (!dialogFont || !GetObjectW(dialogFont, sizeof base, &base)) DefaultDialogFont(&base, GetDpiForWindow(dialog));
    StringCchCopyW(base.lfFaceName, ARRAYSIZE(base.lfFaceName), Localize_FontFace());
    for (i = 0; i < THEME_FONTS; i++) {
        lf = base;
        lf.lfHeight = MulDiv(base.lfHeight, kRoles[i].percent, 100);
        lf.lfWeight = kRoles[i].weight;
        lf.lfUnderline = (BYTE)kRoles[i].underline;
        lf.lfStrikeOut = (BYTE)kRoles[i].strike;
        lf.lfItalic = (BYTE)kRoles[i].italic;
        lf.lfQuality = CLEARTYPE_QUALITY;
        if (variable && !kRoles[i].italic)   /* the variable font has no italic: Segoe UI's own */
            StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName),
                           kRoles[i].weight >= FW_SEMIBOLD ? StrongFace(Localize_FontFace()) : L"Segoe UI Variable Text");
        fonts->font[i] = CreateFontIndirectW(&lf);
    }
}

/* `font`, semibold, as the strong texts the program draws. */
static HFONT StrongOf(HFONT font)
{
    LOGFONTW lf;
    if (!font || !GetObjectW(font, sizeof lf, &lf)) return NULL;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), StrongFace(Localize_FontFace()));
    return CreateFontIndirectW(&lf);
}

/* A control's text semibold (a button that leads to another view): its font
 * keeps that weight when the dialog supplies a font at another scale
 * (see ChildSubclass). */
void Theme_SetStrong(HWND control)
{
    HFONT strong = StrongOf((HFONT)SendMessageW(control, WM_GETFONT, 0, 0)), old = (HFONT)GetPropW(control, STRONG_PROP);
    if (!strong) return;
    SetPropW(control, STRONG_PROP, strong);
    SendMessageW(control, WM_SETFONT, (WPARAM)strong, TRUE);
    if (old) DeleteObject(old);
}

void Theme_FreeFonts(ThemeFonts *fonts)
{
    int i;
    for (i = 0; i < THEME_FONTS; i++) {
        if (fonts->font[i]) DeleteObject(fonts->font[i]);
        fonts->font[i] = NULL;
    }
}

/* ---------------------------------------------------------------- buffers */

/* Drawing meant for `rc` of `target` goes to the DC returned, off screen,
 * with the same coordinates; Theme_BufferEnd shows it at once. Without memory
 * for it, the target itself is returned and drawn on directly. */
HDC Theme_BufferBegin(ThemeBuffer *buffer, HDC target, const RECT *rc)
{
    BITMAPINFO info;
    void *pixels = NULL;
    ZeroMemory(buffer, sizeof *buffer);
    buffer->target = target;
    buffer->rc = *rc;
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = max(1, rc->right - rc->left);
    info.bmiHeader.biHeight = -max(1, rc->bottom - rc->top);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if ((buffer->dc = CreateCompatibleDC(target)) != NULL)
        buffer->bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!buffer->bitmap) {
        if (buffer->dc) DeleteDC(buffer->dc);
        buffer->dc = NULL;
        return target;
    }
    buffer->old = SelectObject(buffer->dc, buffer->bitmap);
    SetViewportOrgEx(buffer->dc, -rc->left, -rc->top, NULL);
    return buffer->dc;
}

void Theme_BufferEnd(ThemeBuffer *buffer)
{
    if (!buffer->dc) return;
    SetViewportOrgEx(buffer->dc, 0, 0, NULL);
    BitBlt(buffer->target, buffer->rc.left, buffer->rc.top, buffer->rc.right - buffer->rc.left, buffer->rc.bottom - buffer->rc.top,
           buffer->dc, 0, 0, SRCCOPY);
    SelectObject(buffer->dc, buffer->old);
    DeleteObject(buffer->bitmap);
    DeleteDC(buffer->dc);
    buffer->dc = NULL;
}

/* ---------------------------------------------------------- theme changes */

/* WM_SETTINGCHANGE / WM_SYSCOLORCHANGE in any of our dialogs: re-read the
 * setting (app mode, contrast theme) and re-theme that window if it was
 * themed with the other mode. Every open dialog gets the broadcast. */
void Theme_Follow(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR mode;
    BOOL reread = msg == WM_SYSCOLORCHANGE || wp == SPI_SETHIGHCONTRAST ||
                  (lp && CompareStringOrdinal((const WCHAR *)lp, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL);
    if (!reread) return;
    ReadSystemTheme();
    UpdatePalette();
    if (g_flushMenuThemes) g_flushMenuThemes();
    mode = (INT_PTR)GetPropW(dialog, THEME_PROP);
    if (mode != (g_dark ? THEMED_DARK : THEMED_LIGHT) || msg == WM_SYSCOLORCHANGE || wp == SPI_SETHIGHCONTRAST) Theme_Apply(dialog);
}

static void ApplyTitleBarMode(HWND window)
{
    BOOL on = g_dark;
    if (FAILED(DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on)))
        DwmSetWindowAttribute(window, 19 /* same attribute before Windows 10 2004 */, &on, sizeof on);
}

static BOOL IsClass(HWND window, const WCHAR *className)
{
    WCHAR name[256];   /* the longest a class name can be */
    return GetClassNameW(window, name, ARRAYSIZE(name)) && CompareStringOrdinal(name, -1, className, -1, TRUE) == CSTR_EQUAL;
}

/* ---------------------------------------------------- buttons, check boxes */

static LONG ButtonType(HWND control)
{
    return IsClass(control, WC_BUTTONW) ? (GetWindowLongW(control, GWL_STYLE) & BS_TYPEMASK) : -1;
}

static BOOL IsPushButton(HWND control)
{
    LONG type = ButtonType(control);
    return (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) && !(GetWindowLongW(control, GWL_STYLE) & (BS_ICON | BS_BITMAP));
}

static BOOL IsCheckBox(HWND control)
{
    LONG type = ButtonType(control);
    return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX;
}

#define CHECKBOX_CAPTION_GAP_DIPS 4   /* between a check box's glyph and its caption */
#define CHECKBOX_FOCUS_OUTSET_PX  1   /* its focus rectangle past its caption, left and right */

#define FOCUS_INSET_DIPS 3   /* a button's focus rectangle inside its frame, clear of its corners */

/* A focus rectangle inside a button's frame, clear of its rounded corners. */
static int FocusRectangleInset(HWND control)
{
    return ScaleForWindow(control, FOCUS_INSET_DIPS) + 2 * LineWidth(control);
}

/* A push button's face, for real buttons and for the ones a window draws
 * itself (the sessions view's details), so that they all look alike. Dark:
 * Explorer's dark button (its fill, 4 DIP corners) framed in main blue instead
 * of gray and white, the frame twice as thick on the default button (the one
 * Enter presses); under the mouse its fill is pale blue in a bright blue
 * frame, pressed it is all bright blue. Light: the theme's button. Leaves
 * the text color set for the label. */
static void ButtonFace(HWND owner, HDC dc, const RECT *rc, UINT state)
{
    BOOL enabled = !(state & THEME_BUTTON_DISABLED);
    BOOL pressed = enabled && (state & THEME_BUTTON_PRESSED), hot = enabled && (state & THEME_BUTTON_HOT);
    if (g_dark) {
        int line = LineWidth(owner);
        COLORREF fill = pressed ? g_palette.color[THEME_BRIGHT_BLUE] : hot ? g_palette.color[THEME_PALE_BLUE] : g_palette.button;
        COLORREF frame = !enabled ? g_palette.buttonOff : (pressed || hot) ? g_palette.color[THEME_BRIGHT_BLUE] : g_palette.color[THEME_MAIN_BLUE];
        FillRect(dc, rc, g_brush[THEME_FACE]);
        RoundedBox(dc, rc, CornerRadius(owner), fill, frame, enabled && (state & THEME_BUTTON_DEFAULT) ? 2 * line : line);
        SetTextColor(dc, enabled ? g_palette.color[THEME_TEXT] : g_palette.color[THEME_MUTED]);
    } else {
        HTHEME theme = OpenThemeData(owner, L"Button");
        int buttonState = !enabled ? PBS_DISABLED : pressed ? PBS_PRESSED : hot ? PBS_HOT :
                          (state & THEME_BUTTON_DEFAULT) ? PBS_DEFAULTED : PBS_NORMAL;
        FillRect(dc, rc, GetSysColorBrush(COLOR_3DFACE));
        if (theme) {
            DrawThemeBackground(theme, dc, BP_PUSHBUTTON, buttonState, rc, NULL);
            CloseThemeData(theme);
        } else {
            RECT edge = *rc;
            DrawFrameControl(dc, &edge, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0) | (!enabled ? DFCS_INACTIVE : 0));
        }
        SetTextColor(dc, GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
    }
}

static void DrawLabel(HDC dc, const WCHAR *text, HFONT font, RECT *rc, UINT format)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, rc, format | Localize_ReadingFlags());
    SelectObject(dc, old);
}

/* A push button (see ButtonFace). `owner` gives the scale and the theme;
 * `format` is DrawText's. */
void Theme_DrawButton(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state, UINT format)
{
    RECT label = *rc;
    ButtonFace(owner, dc, rc, state);
    DrawLabel(dc, text, font, &label, format);
}

#define DROPDOWN_LABEL_INSET_DIPS 8   /* before a drop-down button's label */
#define DROPDOWN_LABEL_GAP_DIPS   2   /* the least room between its label and its arrow */
#define DROPDOWN_ARROW_INSET_DIPS 2   /* after its arrow */

/* Where a drop-down box's arrow goes: at its right end, as wide as a
 * drop-down list's button. */
static RECT DropDownArrow(HWND owner, const RECT *rc)
{
    RECT arrow = *rc;
    arrow.right -= ScaleForWindow(owner, DROPDOWN_ARROW_INSET_DIPS);
    arrow.left = arrow.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(owner));
    return arrow;
}

/* What a drop-down button adds to its label's width (Theme_DrawDropDown). */
static int DropDownFrameWidth(HWND owner)
{
    return ScaleForWindow(owner, DROPDOWN_LABEL_INSET_DIPS + DROPDOWN_LABEL_GAP_DIPS + DROPDOWN_ARROW_INSET_DIPS) +
           GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(owner));
}

/* The width of a drop-down button that shows `text` in `font` whole, in the
 * current language. */
int Theme_DropDownWidth(HWND owner, HFONT font, const WCHAR *text)
{
    RECT measured = { 0 };
    HDC dc = GetDC(owner);
    HGDIOBJ old;
    if (dc) {
        old = SelectObject(dc, font);
        DrawTextW(dc, text, -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | Localize_ReadingFlags());
        SelectObject(dc, old);
        ReleaseDC(owner, dc);
    }
    return measured.right + DropDownFrameWidth(owner);
}

/* Where Theme_DrawDropDown draws the label of a drop-down box `box`: after
 * its inset, up to its arrow. */
void Theme_DropDownLabel(HWND owner, const RECT *box, RECT *label)
{
    *label = *box;
    label->left += ScaleForWindow(owner, DROPDOWN_LABEL_INSET_DIPS);
    label->right = DropDownArrow(owner, box).left;
}

/* A button that opens a menu (the sessions view's Actions): a push button
 * (see ButtonFace), `text` on its left and on its right the arrow the
 * drop-down lists of the same theme show. */
void Theme_DrawDropDown(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state)
{
    /* Not through `owner`: a window with a theme name of its own (a dialog's
     * drop-down list) would not find the class. */
    HTHEME theme = OpenThemeDataForDpi(NULL, g_dark ? L"DarkMode_CFD::Combobox" : L"Combobox", GetDpiForWindow(owner));
    RECT arrow = DropDownArrow(owner, rc), label;
    BOOL enabled = !(state & THEME_BUTTON_DISABLED);
    ButtonFace(owner, dc, rc, state);
    if (theme) {
        DrawThemeBackground(theme, dc, CP_DROPDOWNBUTTONRIGHT, enabled ? CBXSR_NORMAL : CBXSR_DISABLED, &arrow, NULL);
        CloseThemeData(theme);
    } else {
        /* Marlett's down arrow, in the label's color ButtonFace set. */
        HFONT marlett = CreateFontW(-(arrow.bottom - arrow.top) / 2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SYMBOL_CHARSET, 0, 0, 0, 0, L"Marlett");
        HGDIOBJ old = SelectObject(dc, marlett);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"u", 1, &arrow, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
        if (marlett) DeleteObject(marlett);
    }
    Theme_DropDownLabel(owner, rc, &label);
    DrawLabel(dc, text, font, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

/* The menu below a drop-down button uses Windows' menu colors, sizing,
 * animation, accessibility and input tracking. */
UINT Theme_TrackDropDown(HWND owner, HMENU menu, const RECT *screenBox)
{
    static const UINT kPresses[] = { WM_LBUTTONDOWN, WM_LBUTTONDBLCLK };
    TPMPARAMS around;
    UINT command, flags = TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_VERTICAL;
    size_t i;
    ZeroMemory(&around, sizeof around);
    around.cbSize = sizeof around;
    around.rcExclude = *screenBox;
    if (Localize_IsRTL()) flags |= TPM_LAYOUTRTL;
    command = (UINT)TrackPopupMenuEx(menu, flags, screenBox->right, screenBox->bottom, owner, &around);
    /* A press on the same button closes the menu and must not reopen it. */
    for (i = 0; i < ARRAYSIZE(kPresses); i++) {
        MSG message;
        POINT point;
        if (!PeekMessageW(&message, owner, kPresses[i], kPresses[i], PM_NOREMOVE)) continue;
        point.x = (short)LOWORD(message.lParam);
        point.y = (short)HIWORD(message.lParam);
        MapWindowPoints(message.hwnd, NULL, &point, 1);
        if (PtInRect(screenBox, point)) PeekMessageW(&message, message.hwnd, kPresses[i], kPresses[i], PM_REMOVE);
    }
    return command;
}

/* A dark push button (see Theme_DrawButton), with its focus rectangle. */
static LRESULT ButtonCustomDraw(const NMCUSTOMDRAW *customDraw)
{
    WCHAR text[512];
    HWND button = customDraw->hdr.hwndFrom;
    BOOL showFocus;
    UINT format, state = 0;
    RECT focus = customDraw->rc;
    if (customDraw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    if (!IsWindowEnabled(button)) state |= THEME_BUTTON_DISABLED;
    if (customDraw->uItemState & CDIS_SELECTED) state |= THEME_BUTTON_PRESSED;
    if (customDraw->uItemState & CDIS_HOT) state |= THEME_BUTTON_HOT;
    if ((customDraw->uItemState & CDIS_DEFAULT) || (GetWindowLongW(button, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON)
        state |= THEME_BUTTON_DEFAULT;
    GetWindowTextW(button, text, ARRAYSIZE(text));
    format = TextFormatForCues(button, DT_SINGLELINE | DT_CENTER | DT_VCENTER, &showFocus);
    Theme_DrawButton(button, customDraw->hdc, &customDraw->rc, text, (HFONT)SendMessageW(button, WM_GETFONT, 0, 0), state, format);
    if ((customDraw->uItemState & CDIS_FOCUS) && showFocus) {
        InflateRect(&focus, -FocusRectangleInset(button), -FocusRectangleInset(button));
        SetTextColor(customDraw->hdc, g_palette.color[THEME_TEXT]);
        SetBkColor(customDraw->hdc, g_palette.button);
        DrawFocusRect(customDraw->hdc, &focus);
    }
    return CDRF_SKIPDEFAULT;
}

static int CheckBoxThemeState(BOOL enabled, BOOL checked, BOOL pressed, BOOL hot)
{
    if (!enabled) return checked ? CBS_CHECKEDDISABLED : CBS_UNCHECKEDDISABLED;
    if (pressed)  return checked ? CBS_CHECKEDPRESSED : CBS_UNCHECKEDPRESSED;
    if (hot)      return checked ? CBS_CHECKEDHOT : CBS_UNCHECKEDHOT;
    return checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
}

#define CHECKBOX_GLYPH_DIPS 13   /* a check box's glyph where no theme gives its size */

/* The check box glyph's size in `state`; the theme returned draws it. */
static HTHEME CheckBoxGlyphMetrics(HWND control, HDC dc, int state, SIZE *glyph)
{
    HTHEME theme = OpenThemeData(control, L"Button");
    glyph->cx = glyph->cy = ScaleForWindow(control, CHECKBOX_GLYPH_DIPS);
    if (theme) GetThemePartSize(theme, dc, BP_CHECKBOX, state, NULL, TS_DRAW, glyph);
    return theme;
}

/* A control's current caption on one line, in its own font. */
static SIZE CaptionSize(HWND control)
{
    WCHAR text[512];
    RECT caption = { 0 };
    SIZE size = { 0, 0 };
    HDC dc = GetDC(control);
    HGDIOBJ old;
    if (!dc) return size;
    old = SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    GetWindowTextW(control, text, ARRAYSIZE(text));
    DrawTextW(dc, text, -1, &caption, DT_CALCRECT | DT_SINGLELINE | Localize_ReadingFlags());
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    size.cx = caption.right;
    size.cy = caption.bottom;
    return size;
}

BOOL Theme_CheckBoxSize(HWND control, SIZE *size)
{
    HDC dc;
    SIZE glyph, caption;
    HTHEME theme;
    if (!control || !size || !IsCheckBox(control)) return FALSE;
    size->cx = size->cy = 0;   /* BCM_GETIDEALSIZE reads a width to wrap at */
    if (!g_dark && SendMessageW(control, BCM_GETIDEALSIZE, 0, (LPARAM)size)) {
        if (GetWindowTextLengthW(control)) size->cx++;
        return TRUE;
    }
    if ((dc = GetDC(control)) == NULL) return FALSE;
    theme = CheckBoxGlyphMetrics(control, dc, CheckBoxThemeState(IsWindowEnabled(control),
                                 SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED, FALSE, FALSE), &glyph);
    if (theme) CloseThemeData(theme);
    ReleaseDC(control, dc);
    caption = CaptionSize(control);
    /* Rasterized text and the focus cue can occupy the pixel after its advance. */
    size->cx = glyph.cx + (GetWindowTextLengthW(control) ? ScaleForWindow(control, CHECKBOX_CAPTION_GAP_DIPS) + caption.cx + 1 : 0);
    size->cy = max(glyph.cy, caption.cy);
    return TRUE;
}

/* What a check box adds to its caption's width: its glyph and the gap after
 * it, as Theme_CheckBoxSize measures them. */

/* A dark check box: a themed one draws its caption in black, so the glyph
 * comes from the dark theme and the caption and focus rectangle from here. */
static LRESULT CheckBoxCustomDraw(const NMCUSTOMDRAW *customDraw)
{
    HWND button = customDraw->hdr.hwndFrom;
    WCHAR text[512];
    RECT box, focus;
    SIZE glyph;
    HTHEME theme;
    HGDIOBJ old;
    BOOL enabled, showFocus;
    UINT format;
    int state;

    if (customDraw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    enabled = IsWindowEnabled(button);
    state = CheckBoxThemeState(enabled, SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED,
                               (customDraw->uItemState & CDIS_SELECTED) != 0, (customDraw->uItemState & CDIS_HOT) != 0);
    theme = CheckBoxGlyphMetrics(button, customDraw->hdc, state, &glyph);
    if (!theme) return CDRF_DODEFAULT;

    FillRect(customDraw->hdc, &customDraw->rc, g_brush[THEME_FACE]);
    box.left = customDraw->rc.left;
    box.top = customDraw->rc.top + (customDraw->rc.bottom - customDraw->rc.top - glyph.cy) / 2;
    box.right = box.left + glyph.cx;
    box.bottom = box.top + glyph.cy;
    DrawThemeBackground(theme, customDraw->hdc, BP_CHECKBOX, state, &box, NULL);
    CloseThemeData(theme);

    GetWindowTextW(button, text, ARRAYSIZE(text));
    format = TextFormatForCues(button, DT_SINGLELINE | DT_LEFT | DT_NOCLIP, &showFocus) | Localize_ReadingFlags();
    old = SelectObject(customDraw->hdc, (HFONT)SendMessageW(button, WM_GETFONT, 0, 0));
    focus = customDraw->rc;
    focus.left = box.right + ScaleForWindow(button, CHECKBOX_CAPTION_GAP_DIPS);
    DrawTextW(customDraw->hdc, text, -1, &focus, format | DT_CALCRECT);
    OffsetRect(&focus, 0, ((customDraw->rc.bottom - customDraw->rc.top) - (focus.bottom - focus.top)) / 2);
    SetBkMode(customDraw->hdc, TRANSPARENT);
    SetTextColor(customDraw->hdc, enabled ? g_palette.color[THEME_TEXT] : g_palette.color[THEME_MUTED]);
    DrawTextW(customDraw->hdc, text, -1, &focus, format);
    if ((customDraw->uItemState & CDIS_FOCUS) && showFocus) {
        InflateRect(&focus, CHECKBOX_FOCUS_OUTSET_PX, 0);
        SetTextColor(customDraw->hdc, g_palette.color[THEME_TEXT]);
        DrawFocusRect(customDraw->hdc, &focus);
    }
    SelectObject(customDraw->hdc, old);
    return CDRF_SKIPDEFAULT;
}

/* ------------------------------------------------------------ list views */

#define TIP_MAX_WIDTH_DIPS 600
#define TIP_GAP_DIPS       2   /* between a cell and its tip below it */

typedef enum CellTipKind { TIP_TABLE_CELL, TIP_TREE_ROW, TIP_LABEL } CellTipKind;

/* The control HoverUnderMouse tells where the still mouse now is: its rows
 * show the one under it hot, and no tip opens for it until the mouse moves,
 * as a native tip does after a scroll. */
static HWND g_replayedMouseMove;

/* What the pointer is on: no cell, a cell whose text shows whole, or one
 * whose tip shows (its text cut, or a tree's info tip). */
typedef enum CellTipFound { TIP_NO_CELL, TIP_CELL_FITS, TIP_CELL_SHOWS } CellTipFound;

typedef struct CellTip {
    HWND tooltipWindow;
    CellTipKind kind;   /* found once: every mouse move reads it */
    int row, column;
    HTREEITEM treeItem;
    RECT cell;
    WCHAR text[2048];
    BOOL active;
    BOOL ownsWindow;
    BOOL byHover;       /* a tree's info tip shown for a row that fits, once the pointer rested on it */
} CellTip;

static TOOLINFOW TipTool(HWND control, CellTip *tip)
{
    TOOLINFOW tool;
    ZeroMemory(&tool, sizeof tool);
    tool.cbSize = sizeof tool;
    tool.hwnd = control;
    tool.uId = (UINT_PTR)tip;
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT | (Localize_IsRTL() ? TTF_RTLREADING : 0);
    tool.lpszText = tip->text;
    return tool;
}

static void HideCellTip(HWND control)
{
    CellTip *tip = (CellTip *)GetPropW(control, TIP_PROP);
    TOOLINFOW tool;
    if (!tip) return;
    tip->byHover = FALSE;
    if (tip->active) {
        tip->active = FALSE;
        tool = TipTool(control, tip);
        SendMessageW(tip->tooltipWindow, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
    }
    /* A list's or tree's own tooltip window can be activated again by the
     * control itself: its native hover tips stay off. */
    SendMessageW(tip->tooltipWindow, TTM_ACTIVATE, FALSE, 0);
    SendMessageW(tip->tooltipWindow, TTM_POP, 0, 0);
}

/* A control in a view extends beyond the view that clips it: only the part
 * of a cell inside the view counts. */
static BOOL VisibleTipCell(HWND control, RECT *cell, POINT point)
{
    RECT client, parent;
    HWND owner = GetParent(control);
    GetClientRect(control, &client);
    if (owner && GetClientRect(owner, &parent)) {
        MapWindowPoints(owner, control, (POINT *)&parent, 2);
        IntersectRect(&client, &client, &parent);
    }
    return IntersectRect(cell, cell, &client) && PtInRect(cell, point);
}

#define CELL_TEXT_INSET_DIPS       6   /* between a cell's edges and its text */
#define FIRST_CELL_TEXT_INSET_DIPS 2   /* the first column's text, after the room its icon keeps */
#define CELL_IMAGE_GAP_DIPS        2   /* between a cell's image and its text */
#define HEADER_TEXT_INSET_DIPS     7   /* between a header item's edges and its title */
#define TABLE_EDGE_PX              2   /* a column's last pixels: its divider, or the table's edge after the last one */

typedef struct TableCellGeometry {
    RECT label, icon;
    HIMAGELIST images;
    int image;
} TableCellGeometry;

/* Painting and clipping checks use the same native label geometry, padding
 * and optional subitem image. Column zero already reserves its state/icon. */
static BOOL TableCellBounds(HWND list, int row, int column, TableCellGeometry *cell)
{
    ZeroMemory(cell, sizeof *cell);
    cell->image = I_IMAGENONE;
    if (!ListView_GetSubItemRect(list, row, column, LVIR_LABEL, &cell->label) || cell->label.right <= cell->label.left) return FALSE;
    cell->label.left += ScaleForWindow(list, column ? CELL_TEXT_INSET_DIPS : FIRST_CELL_TEXT_INSET_DIPS);
    cell->label.right -= ScaleForWindow(list, CELL_TEXT_INSET_DIPS);
    cell->images = ListView_GetImageList(list, LVSIL_SMALL);
    if (column && cell->images && (ListView_GetExtendedListViewStyle(list) & LVS_EX_SUBITEMIMAGES)) {
        LVITEMW item = { 0 };
        int width, height;
        item.mask = LVIF_IMAGE;
        item.iItem = row;
        item.iSubItem = column;
        item.iImage = I_IMAGENONE;
        if (ListView_GetItem(list, &item) && item.iImage >= 0 && ImageList_GetIconSize(cell->images, &width, &height)) {
            cell->image = item.iImage;
            cell->icon.left = cell->label.left;
            cell->icon.top = cell->label.top + (cell->label.bottom - cell->label.top - height) / 2;
            cell->icon.right = cell->icon.left + width;
            cell->icon.bottom = cell->icon.top + height;
            cell->label.left += width + ScaleForWindow(list, CELL_IMAGE_GAP_DIPS);
        }
    }
    return TRUE;
}

/* Where a table cell's text is drawn; a text wider than that shows a tip. */
BOOL Theme_TableCellText(HWND list, int row, int column, RECT *text)
{
    TableCellGeometry cell;
    if (!TableCellBounds(list, row, column, &cell)) return FALSE;
    *text = cell.label;
    return TRUE;
}

/* `text` wider than `label`, in the control's font. For a tree row, the
 * parent's custom draw is asked for the row's font and its title's bounds:
 * an item prepaint with an empty rectangle measures and draws nothing (the
 * parent answers CDRF_NEWFONT with the bounds in the rectangle). Measured on
 * a memory DC, so a parent unaware of that draws nowhere. */
static BOOL ClippedTipText(HWND control, const WCHAR *text, const RECT *label, const TVITEMW *item)
{
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old;
    SIZE size = { 0 };
    RECT available = *label;
    if (!dc) return FALSE;
    old = SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    if (item) {
        NMTVCUSTOMDRAW draw;
        ZeroMemory(&draw, sizeof draw);
        draw.nmcd.hdr.hwndFrom = control;
        draw.nmcd.hdr.idFrom = (UINT_PTR)GetDlgCtrlID(control);
        draw.nmcd.hdr.code = NM_CUSTOMDRAW;
        draw.nmcd.dwDrawStage = CDDS_ITEMPREPAINT;
        draw.nmcd.hdc = dc;
        draw.nmcd.dwItemSpec = (DWORD_PTR)item->hItem;
        draw.nmcd.lItemlParam = item->lParam;
        SendMessageW(GetParent(control), WM_NOTIFY, draw.nmcd.hdr.idFrom, (LPARAM)&draw);
        if (draw.nmcd.rc.bottom > draw.nmcd.rc.top) {
            available.left = max(available.left, draw.nmcd.rc.left);
            available.right = min(available.right, draw.nmcd.rc.right);
        }
    }
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
    SelectObject(dc, old);
    DeleteDC(dc);
    return size.cx > max(0, available.right - available.left);
}

static CellTipFound ListTipCell(HWND list, POINT point, CellTip *next)
{
    LVHITTESTINFO hit;
    TableCellGeometry cell;
    DWORD style = ListView_GetExtendedListViewStyle(list);
    ZeroMemory(&hit, sizeof hit);
    hit.pt = point;
    if (ListView_SubItemHitTest(list, &hit) < 0 || hit.iSubItem < 0 ||
        (!hit.iSubItem && !(hit.flags & LVHT_ONITEM)) ||
        !ListView_GetSubItemRect(list, hit.iItem, hit.iSubItem, LVIR_BOUNDS, &next->cell)) return TIP_NO_CELL;
    if (!hit.iSubItem) next->cell.right = next->cell.left + ListView_GetColumnWidth(list, 0);
    if (!VisibleTipCell(list, &next->cell, point)) return TIP_NO_CELL;
    next->row = hit.iItem;
    next->column = hit.iSubItem;
    ListView_GetItemText(list, hit.iItem, hit.iSubItem, next->text, ARRAYSIZE(next->text));
    if (!TableCellBounds(list, hit.iItem, hit.iSubItem, &cell)) return TIP_CELL_FITS;
    cell.label.left = max(cell.label.left, next->cell.left);
    cell.label.right = min(cell.label.right, next->cell.right);
    return next->text[0] && (!hit.iSubItem || (style & LVS_EX_LABELTIP)) && ClippedTipText(list, next->text, &cell.label, NULL)
           ? TIP_CELL_SHOWS : TIP_CELL_FITS;
}

/* A tree row's tip: its title when it is cut; a TVS_INFOTIP tree's info tip
 * then, and also for a row that fits once the pointer rested on it
 * (`everyRow`), as the tree's own info tips do. A row without an info tip
 * whose title shows whole has none. */
static CellTipFound TreeTipCell(HWND tree, POINT point, BOOL everyRow, CellTip *next)
{
    TVHITTESTINFO hit;
    TVITEMW item;
    WCHAR labelText[2048];
    RECT label;
    BOOL infoTips = (GetWindowLongW(tree, GWL_STYLE) & TVS_INFOTIP) != 0, clipped;
    ZeroMemory(&hit, sizeof hit);
    hit.pt = point;
    if (!TreeView_HitTest(tree, &hit) || !(hit.flags & (TVHT_ONITEMLABEL | TVHT_ONITEMRIGHT)) ||
        !TreeView_GetItemRect(tree, hit.hItem, &next->cell, FALSE) || !VisibleTipCell(tree, &next->cell, point)) return TIP_NO_CELL;
    ZeroMemory(&item, sizeof item);
    item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_PARAM;
    item.hItem = hit.hItem;
    item.pszText = labelText;
    item.cchTextMax = ARRAYSIZE(labelText);
    labelText[0] = 0;
    next->treeItem = hit.hItem;
    if (!TreeView_GetItem(tree, &item) || !TreeView_GetItemRect(tree, hit.hItem, &label, TRUE) || !labelText[0]) return TIP_CELL_FITS;
    label.right = next->cell.right;
    clipped = ClippedTipText(tree, labelText, &label, &item);
    if (!clipped && !(everyRow && infoTips)) return TIP_CELL_FITS;
    if (infoTips) {
        NMTVGETINFOTIPW info;
        ZeroMemory(&info, sizeof info);
        info.hdr.hwndFrom = tree;
        info.hdr.idFrom = (UINT_PTR)GetDlgCtrlID(tree);
        info.hdr.code = TVN_GETINFOTIPW;
        info.pszText = next->text;
        info.cchTextMax = ARRAYSIZE(next->text);
        info.hItem = hit.hItem;
        info.lParam = item.lParam;
        SendMessageW(GetParent(tree), WM_NOTIFY, info.hdr.idFrom, (LPARAM)&info);
        if (next->text[0]) return TIP_CELL_SHOWS;
    }
    if (!clipped) return TIP_CELL_FITS;   /* no info tip, and the title shows whole */
    StringCchCopyW(next->text, ARRAYSIZE(next->text), labelText);
    return TIP_CELL_SHOWS;
}

/* A text label cut at its end: its whole text shows in a tooltip. Path and
 * word ellipses shorten text on purpose. */
static BOOL IsEndEllipsisLabel(HWND control)
{
    LONG style = GetWindowLongW(control, GWL_STYLE);
    return IsClass(control, WC_STATICW) && (style & SS_ELLIPSISMASK) == SS_ENDELLIPSIS && (style & SS_TYPEMASK) <= SS_RIGHT;
}

static CellTipFound StaticTipCell(HWND control, POINT point, CellTip *next)
{
    if (!IsEndEllipsisLabel(control) || !GetClientRect(control, &next->cell) || !VisibleTipCell(control, &next->cell, point)) return TIP_NO_CELL;
    GetWindowTextW(control, next->text, ARRAYSIZE(next->text));
    return next->text[0] && ClippedTipText(control, next->text, &next->cell, NULL) ? TIP_CELL_SHOWS : TIP_CELL_FITS;
}

/* The tip of the cell at `point`, shown, moved or hidden; `rested`: the
 * pointer rested there (WM_MOUSEHOVER). A move inside the cell whose tip
 * shows changes nothing: the tip shows what its cell held when it opened, as
 * a native tip does, and a change of the control's content hides it
 * (TipSubclass). A cell that fits is looked at again on every move: what its
 * parent draws beside a tree's title can change without the tree knowing. */
static void UpdateCellTip(HWND control, POINT point, BOOL rested)
{
    CellTip *tip = (CellTip *)GetPropW(control, TIP_PROP), next;
    TOOLINFOW tool;
    POINT place;
    CellTipFound found;
    BOOL everyRow;
    TRACKMOUSEEVENT track = { sizeof track, TME_LEAVE, control, HOVER_DEFAULT };
    if (!tip) return;
    if (!IsWindowVisible(control) || !IsWindowEnabled(control) || !IsWindowEnabled(GetAncestor(control, GA_ROOT))) {
        HideCellTip(control);
        return;
    }
    if (tip->active && IsWindowVisible(tip->tooltipWindow) && PtInRect(&tip->cell, point)) return;
    everyRow = rested || tip->byHover;
    ZeroMemory(&next, sizeof next);
    if (tip->kind == TIP_TABLE_CELL) found = ListTipCell(control, point, &next);
    else if (tip->kind == TIP_TREE_ROW) found = TreeTipCell(control, point, everyRow, &next);
    else found = StaticTipCell(control, point, &next);
    if (GetPropW(control, TIP_PROP) != (HANDLE)tip) return;
    if (found != TIP_CELL_SHOWS) {
        HideCellTip(control);
        if (found == TIP_NO_CELL) return;
        /* A tree's info tip waits for the pointer to rest on the row. */
        if (tip->kind == TIP_TREE_ROW && (GetWindowLongW(control, GWL_STYLE) & TVS_INFOTIP)) {
            track.dwFlags = TME_HOVER | TME_LEAVE;
            TrackMouseEvent(&track);
        }
        return;
    }
    if (tip->active && IsWindowVisible(tip->tooltipWindow) && tip->row == next.row && tip->column == next.column &&
        tip->treeItem == next.treeItem && EqualRect(&tip->cell, &next.cell) && wcscmp(tip->text, next.text) == 0) return;
    HideCellTip(control);
    tip->row = next.row;
    tip->column = next.column;
    tip->treeItem = next.treeItem;
    tip->cell = next.cell;
    tip->byHover = everyRow && tip->kind == TIP_TREE_ROW;
    StringCchCopyW(tip->text, ARRAYSIZE(tip->text), next.text);
    tool = TipTool(control, tip);
    SendMessageW(tip->tooltipWindow, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
    place.x = next.cell.left;
    place.y = next.cell.bottom + ScaleForWindow(control, TIP_GAP_DIPS);
    ClientToScreen(control, &place);
    SendMessageW(tip->tooltipWindow, TTM_TRACKPOSITION, 0, MAKELPARAM(place.x, place.y));
    tip->active = TRUE;
    TrackMouseEvent(&track);
    SendMessageW(tip->tooltipWindow, TTM_ACTIVATE, TRUE, 0);
    SendMessageW(tip->tooltipWindow, TTM_TRACKACTIVATE, TRUE, (LPARAM)&tool);
}

static LRESULT CALLBACK TipSubclass(HWND control, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    CellTip *tip = (CellTip *)ref;
    if (msg == WM_NCHITTEST && tip->kind == TIP_LABEL) return HTCLIENT;
    if (msg == WM_MOUSEMOVE && control == g_replayedMouseMove) {
        if (tip->active) HideCellTip(control);
        return DefSubclassProc(control, msg, wp, lp);
    } else if (msg == WM_MOUSEMOVE || msg == WM_MOUSEHOVER) {
        /* The point is read before ListSubclass moves it off a selected row
         * (no hot look there); the tip follows once the control has handled
         * the move. */
        POINT point = { (short)LOWORD(lp), (short)HIWORD(lp) };
        LRESULT result = DefSubclassProc(control, msg, wp, lp);
        if (GetPropW(control, TIP_PROP) == (HANDLE)tip) UpdateCellTip(control, point, msg == WM_MOUSEHOVER);
        return result;
    } else if (msg == WM_NOTIFY && lp && tip->kind == TIP_TABLE_CELL && ((const NMHDR *)lp)->hwndFrom == ListView_GetHeader(control) &&
               (((const NMHDR *)lp)->code == HDN_ITEMCHANGINGW || ((const NMHDR *)lp)->code == HDN_ITEMCHANGEDW ||
                ((const NMHDR *)lp)->code == HDN_BEGINTRACKW)) {
        HideCellTip(control);
    } else if (msg == WM_NOTIFY && lp && ((const NMHDR *)lp)->hwndFrom == tip->tooltipWindow &&
               (((const NMHDR *)lp)->code == TTN_SHOW || ((const NMHDR *)lp)->code == TTN_POP)) {
        return 0;   /* tracking geometry belongs to the cell, not native label hover */
    } else {
        switch (msg) {
        case WM_MOUSELEAVE: case WM_NCMOUSEMOVE: case WM_CANCELMODE: case WM_KILLFOCUS:
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_KEYDOWN:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_VSCROLL: case WM_HSCROLL: case LVM_SCROLL: case WM_SIZE: case WM_WINDOWPOSCHANGING:
        case WM_SETFONT: case WM_SETTEXT: case WM_SHOWWINDOW: case LVM_SETITEMTEXTW: case LVM_DELETEITEM: case LVM_DELETEALLITEMS:
        case LVM_SETCOLUMNWIDTH: case LVM_SETCOLUMNW: case LVM_SETCOLUMNORDERARRAY: case LVM_INSERTCOLUMNW: case LVM_DELETECOLUMN:
        case TVM_SETITEMW: case TVM_DELETEITEM: case TVM_EXPAND:
            HideCellTip(control);
            break;
        case WM_NCDESTROY: {
            TOOLINFOW tool = TipTool(control, tip);
            HideCellTip(control);
            RemovePropW(control, TIP_PROP);
            RemoveWindowSubclass(control, TipSubclass, id);
            SendMessageW(tip->tooltipWindow, TTM_DELTOOLW, 0, (LPARAM)&tool);
            if (tip->ownsWindow && IsWindow(tip->tooltipWindow)) DestroyWindow(tip->tooltipWindow);
            HeapFree(GetProcessHeap(), 0, tip);
            return DefSubclassProc(control, msg, wp, lp);
        }
        }
    }
    return DefSubclassProc(control, msg, wp, lp);
}

/* The tip is a tracking tool that UpdateCellTip shows, moves and hides for
 * the cell under the pointer; the window's hover tips stay off, so none
 * reopens once the pointer has left. `tooltipWindow` is a list's or tree's
 * own; a label gets one of its own. */
static void ApplyCellTip(HWND control, HWND tooltipWindow)
{
    CellTip *tip = (CellTip *)GetPropW(control, TIP_PROP);
    TOOLINFOW tool;
    BOOL ownsWindow = FALSE;
    if (!tip) {
        if (!tooltipWindow && IsClass(control, WC_STATICW)) {
            tooltipWindow = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                            WS_POPUP | TTS_NOANIMATE | TTS_NOFADE | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                            0, 0, 0, 0, control, NULL, g_hInst, NULL);
            ownsWindow = tooltipWindow != NULL;
        }
        if (!tooltipWindow) return;
        tip = (CellTip *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *tip);
        if (!tip) {
            if (ownsWindow) DestroyWindow(tooltipWindow);
            return;
        }
        tip->tooltipWindow = tooltipWindow;
        tip->ownsWindow = ownsWindow;
        tip->kind = IsClass(control, WC_LISTVIEWW) ? TIP_TABLE_CELL : IsClass(control, WC_TREEVIEWW) ? TIP_TREE_ROW : TIP_LABEL;
        tool = TipTool(control, tip);
        if (!SetPropW(control, TIP_PROP, tip) || !SendMessageW(tooltipWindow, TTM_ADDTOOLW, 0, (LPARAM)&tool)) {
            RemovePropW(control, TIP_PROP);
            if (ownsWindow) DestroyWindow(tooltipWindow);
            HeapFree(GetProcessHeap(), 0, tip);
            return;
        }
        if (!SetWindowSubclass(control, TipSubclass, TIP_SUBCLASS, (DWORD_PTR)tip)) {
            RemovePropW(control, TIP_PROP);
            SendMessageW(tooltipWindow, TTM_DELTOOLW, 0, (LPARAM)&tool);
            if (ownsWindow) DestroyWindow(tooltipWindow);
            HeapFree(GetProcessHeap(), 0, tip);
            return;
        }
    }
    /* The settings go to the window the tool lives in. */
    tooltipWindow = tip->tooltipWindow;
    HideCellTip(control);
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(tooltipWindow, g_dark);
    SetWindowTheme(tooltipWindow, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    SendMessageW(tooltipWindow, WM_THEMECHANGED, 0, 0);
    tool = TipTool(control, tip);
    SendMessageW(tooltipWindow, TTM_SETTOOLINFOW, 0, (LPARAM)&tool);
    SendMessageW(tooltipWindow, TTM_ACTIVATE, FALSE, 0);
    SendMessageW(tooltipWindow, TTM_SETDELAYTIME, TTDT_INITIAL, 0);
    SendMessageW(tooltipWindow, TTM_SETDELAYTIME, TTDT_RESHOW, 0);
    SendMessageW(tooltipWindow, TTM_SETMAXTIPWIDTH, 0, ScaleForWindow(control, TIP_MAX_WIDTH_DIPS));
    SendMessageW(tooltipWindow, WM_SETFONT, SendMessageW(control, WM_GETFONT, 0, 0), FALSE);
    /* A list's or tree's tooltip window was made without these. */
    SetWindowLongW(tooltipWindow, GWL_STYLE, GetWindowLongW(tooltipWindow, GWL_STYLE) | TTS_NOANIMATE | TTS_NOFADE | TTS_NOPREFIX | TTS_ALWAYSTIP);
}

static BOOL CALLBACK HideDialogTip(HWND child, LPARAM data)
{
    (void)data;
    HideCellTip(child);
    return TRUE;
}

static int LastHeaderColumn(HWND header)
{
    int order;
    for (order = Header_GetItemCount(header) - 1; order >= 0; order--) {
        RECT rc;
        int column = Header_OrderToIndex(header, order);
        if (column >= 0 && Header_GetItemRect(header, column, &rc) && rc.right > rc.left) return column;
    }
    return -1;
}

typedef struct TableGeometry {
    RECT client, header;   /* the list's client and its header, in the list's coordinates */
    int origin;            /* where the rows' content starts in the list: Windows' horizontal scroll */
    int right, lastColumn; /* where the table ends: its last column's right edge */
} TableGeometry;

/* A column in its header's coordinates, which are the content's. */
static BOOL TableColumnBounds(HWND header, int column, RECT *bounds)
{
    return Header_GetItemRect(header, column, bounds) && bounds->right > bounds->left;
}

/* Rows and the empty area end at the last column's right edge (the layout
 * keeps it at the list's). Everything the table draws is placed in its
 * content, so the pixels Windows copies as it scrolls or shifts columns stay
 * right. Each window draws from its own geometry: the header's items give
 * the columns' places in the content, the list's rows where the content
 * starts; the header window, which Windows moves on its own, can lag behind
 * them while the scroll bar's thumb is dragged. */
static BOOL TableBounds(HWND list, TableGeometry *table)
{
    HWND header = ListView_GetHeader(list);
    RECT column, first;
    if ((GetWindowLongW(list, GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT || !header || !GetClientRect(list, &table->client)) return FALSE;
    table->lastColumn = LastHeaderColumn(header);
    if (table->lastColumn < 0 || !TableColumnBounds(header, table->lastColumn, &column)) return FALSE;
    SetRectEmpty(&table->header);
    if (GetWindowLongW(header, GWL_STYLE) & WS_VISIBLE) {
        GetWindowRect(header, &table->header);
        MapWindowPoints(NULL, list, (POINT *)&table->header, 2);
    }
    table->origin = ListView_GetItemCount(list) > 0 && ListView_GetItemRect(list, 0, &first, LVIR_BOUNDS) ? first.left : table->header.left;
    table->right = table->origin + column.right;
    return table->right >= table->client.left + TABLE_EDGE_PX;
}

/* The table's edge, in its region's background: no separator after the last
 * column, whose native divider stays draggable. */
static void PaintTableSide(HDC dc, int right, int top, int bottom, COLORREF fill)
{
    RECT line = { right - TABLE_EDGE_PX, top, right, bottom };
    if (bottom <= top) return;
    FillSolid(dc, &line, fill);
}

/* A light header's last item over the table's edge: its background drawn
 * wider than the item, so that the theme's divider falls past the edge, in
 * the item's state and colors. */
static void PaintHeaderSide(HWND header, HDC dc, const RECT *bounds, int right, int state)
{
    RECT line = { right - TABLE_EDGE_PX, bounds->top, right, bounds->bottom }, background = *bounds;
    HTHEME theme;
    int saved;
    if (IsRectEmpty(&line)) return;
    FillSolid(dc, &line, GetSysColor(COLOR_BTNFACE));
    if ((theme = OpenThemeData(header, L"Header")) == NULL) return;
    saved = SaveDC(dc);
    if (saved) {
        background.right = max(background.right, right) + TABLE_EDGE_PX;
        IntersectClipRect(dc, line.left, line.top, line.right, line.bottom);
        DrawThemeBackground(theme, dc, HP_HEADERITEM, state, &background, NULL);
        RestoreDC(dc, saved);
    }
    CloseThemeData(theme);
}

/* The interior dividers' columns of pixels, found once for every row:
 * `local` when they fit, else a heap block the caller frees. */
static int *TableDividers(HWND list, const TableGeometry *table, int *local, int localCount, int *count)
{
    HWND header = ListView_GetHeader(list);
    int columns = Header_GetItemCount(header), column, *dividers = local;
    *count = 0;
    if (columns > localCount && (dividers = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)columns * sizeof *dividers)) == NULL) return local;
    for (column = 0; column < columns; column++) {
        RECT bounds;
        if (column != table->lastColumn && TableColumnBounds(header, column, &bounds))
            dividers[(*count)++] = table->origin + bounds.right - TABLE_EDGE_PX;
    }
    return dividers;
}

static void PaintTableBody(HWND list, HDC dc)
{
    TableGeometry table;
    RECT first, row, paint;
    BOOL focused = GetFocus() == list;
    BOOL contrastSelectionShown = (ListView_GetExtendedListViewStyle(list) & LVS_EX_FULLROWSELECT) &&
                                  (focused || (GetWindowLongW(list, GWL_STYLE) & LVS_SHOWSELALWAYS));
    int count, i, start, end, height, local[32], *dividers, dividerCount = 0, d;
    if (!TableBounds(list, &table)) return;
    if (GetClipBox(dc, &paint) == ERROR) paint = table.client;   /* the rows this paint covers */
    PaintTableSide(dc, table.right, max(paint.top, table.header.bottom), paint.bottom, g_palette.color[THEME_FIELD]);
    count = ListView_GetItemCount(list);
    if (!count || !ListView_GetItemRect(list, 0, &first, LVIR_BOUNDS) || (height = first.bottom - first.top) <= 0) return;
    start = max(0, (paint.top - first.top) / height);
    end = min(count, (paint.bottom - first.top) / height + 1);
    dividers = TableDividers(list, &table, local, ARRAYSIZE(local), &dividerCount);
    for (i = start; i < end; i++) {
        BOOL selected;
        if (!ListView_GetItemRect(list, i, &row, LVIR_BOUNDS)) continue;
        selected = ListView_GetItemState(list, i, LVIS_SELECTED) != 0;
        if (g_highContrast) {
            PaintTableSide(dc, table.right, row.top, row.bottom, selected && contrastSelectionShown ?
                           GetSysColor(focused ? COLOR_HIGHLIGHT : COLOR_BTNFACE) : g_palette.color[THEME_FIELD]);
            continue;
        }
        /* A row ends before the table's edge, as a column before its divider. */
        PaintTableSide(dc, table.right, row.top, row.bottom, g_palette.color[THEME_FIELD]);
        if (!selected && !g_dark) continue;
        for (d = 0; d < dividerCount; d++) {
            RECT divider = { dividers[d], row.top, dividers[d] + 1, row.bottom };
            FillSolid(dc, &divider, selected ? g_palette.color[THEME_BRIGHT_BLUE] : g_palette.divider);
        }
    }
    if (dividers != local) HeapFree(GetProcessHeap(), 0, dividers);
}

static WCHAR *TableCellText(HWND list, int row, int column, WCHAR *local, int localCount)
{
    WCHAR *text = local;
    int capacity = localCount;
    for (;;) {
        LVITEMW item = { 0 };
        int copied;
        WCHAR *larger;
        item.iSubItem = column;
        item.pszText = text;
        item.cchTextMax = capacity;
        text[0] = 0;
        copied = (int)SendMessageW(list, LVM_GETITEMTEXTW, row, (LPARAM)&item);
        if (copied < capacity - 1 || capacity > INT_MAX / 2) return text;
        larger = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)(2 * capacity) * sizeof *larger);
        if (!larger) return text;
        if (text != local) HeapFree(GetProcessHeap(), 0, text);
        text = larger;
        capacity *= 2;
    }
}

/* A row's check box (the theme's glyph, where Windows draws its state
 * image) or other state image, and its icon, at the native item geometry. */
static void PaintTableRowImages(HWND list, HDC dc, const RECT *bounds, const LVITEMW *item)
{
    HIMAGELIST images = ListView_GetImageList(list, LVSIL_SMALL), states = ListView_GetImageList(list, LVSIL_STATE);
    RECT icon;
    int width, height, stateImage = (int)((item->state & LVIS_STATEIMAGEMASK) >> 12) - 1;
    if (!ListView_GetItemRect(list, item->iItem, &icon, LVIR_ICON)) return;
    if (states && stateImage >= 0 && ImageList_GetIconSize(states, &width, &height)) {
        HTHEME theme = NULL;
        SIZE glyph = { 0 };
        int checkState = stateImage == 0 ? CBS_UNCHECKEDNORMAL : stateImage == 1 ? CBS_CHECKEDNORMAL : CBS_MIXEDNORMAL;
        if (stateImage < 3 && (ListView_GetExtendedListViewStyle(list) & LVS_EX_CHECKBOXES))
            theme = CheckBoxGlyphMetrics(list, dc, checkState, &glyph);
        if (theme) {
            RECT check;
            TEXTMETRICW metrics;
            int contentHeight, imageWidth, imageHeight;
            GetTextMetricsW(dc, &metrics);
            contentHeight = metrics.tmHeight;
            if (images && ImageList_GetIconSize(images, &imageWidth, &imageHeight)) contentHeight = max(contentHeight, imageHeight);
            check.left = icon.left - max(width, GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(list)));
            check.top = bounds->top + max(0, (contentHeight - glyph.cy + 1) / 2);
            check.right = check.left + glyph.cx;
            check.bottom = check.top + glyph.cy;
            DrawThemeBackground(theme, dc, BP_CHECKBOX, checkState, &check, NULL);
            CloseThemeData(theme);
        } else {
            ImageList_Draw(states, stateImage, dc, icon.left - width, icon.top + (icon.bottom - icon.top - height) / 2, ILD_TRANSPARENT);
        }
    }
    if (images && item->iImage >= 0 && ImageList_GetIconSize(images, &width, &height))
        ImageList_Draw(images, item->iImage, dc, icon.left, bounds->top + (bounds->bottom - bounds->top - height) / 2,
                       ILD_TRANSPARENT | (item->state & LVIS_OVERLAYMASK) | ((item->state & LVIS_CUT) ? ILD_BLEND50 : 0));
}

/* DrawText's alignment of a column whose header format is `format` (HDF_*). */
static UINT ColumnAlignment(int format)
{
    if ((format & HDF_JUSTIFYMASK) == HDF_RIGHT) return DT_RIGHT;
    if ((format & HDF_JUSTIFYMASK) == HDF_CENTER) return DT_CENTER;
    return DT_LEFT;
}

static BOOL PaintTableRow(HWND list, HDC dc, int index)
{
    TableGeometry table;
    RECT bounds, shape;
    LVITEMW item = { 0 };
    HWND header = ListView_GetHeader(list);
    LONG windowExtendedStyle = GetWindowLongW(list, GWL_EXSTYLE);
    HGDIOBJ old;
    COLORREF textColor;
    UINT state = 0;
    int column, columns = Header_GetItemCount(header);
    if (!TableBounds(list, &table) || !ListView_GetItemRect(list, index, &bounds, LVIR_BOUNDS)) return FALSE;
    shape = bounds;
    shape.right = table.right - TABLE_EDGE_PX;
    if (ListView_GetItemState(list, index, LVIS_SELECTED)) state = THEME_ROW_SELECTED;
    else if (ListView_GetHotItem(list) == index) state = THEME_ROW_HOT;
    textColor = Theme_DrawRow(list, dc, &shape, state, g_palette.color[THEME_FIELD]);
    old = SelectObject(dc, (HFONT)SendMessageW(list, WM_GETFONT, 0, 0));
    SetTextColor(dc, textColor);
    SetBkMode(dc, TRANSPARENT);
    item.mask = LVIF_IMAGE | LVIF_STATE;
    item.stateMask = LVIS_STATEIMAGEMASK | LVIS_OVERLAYMASK | LVIS_CUT | LVIS_FOCUSED;
    item.iItem = index;
    item.iImage = I_IMAGENONE;
    ListView_GetItem(list, &item);
    PaintTableRowImages(list, dc, &bounds, &item);
    for (column = 0; column < columns; column++) {
        WCHAR local[512], *text;
        HDITEMW heading = { 0 };
        TableCellGeometry cell;
        UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (!TableCellBounds(list, index, column, &cell)) continue;
        heading.mask = HDI_FORMAT;
        Header_GetItem(header, column, &heading);
        format |= ColumnAlignment(heading.fmt);
        if ((heading.fmt & HDF_RTLREADING) || (windowExtendedStyle & WS_EX_RTLREADING)) format |= DT_RTLREADING;
        if (cell.image >= 0) ImageList_Draw(cell.images, cell.image, dc, cell.icon.left, cell.icon.top, ILD_TRANSPARENT);
        text = TableCellText(list, index, column, local, ARRAYSIZE(local));
        DrawTextW(dc, text, -1, &cell.label, format);
        if (text != local) HeapFree(GetProcessHeap(), 0, text);
    }
    SelectObject(dc, old);
    if ((item.state & LVIS_FOCUSED) && GetFocus() == list) Theme_DrawFocusCue(list, dc, &shape);
    return TRUE;
}

/* The component owns each report row's background and content in one pass;
 * the native control retains its model and all interaction notifications. */
static LRESULT ListCustomDraw(NMLVCUSTOMDRAW *customDraw)
{
    HWND list = customDraw->nmcd.hdr.hwndFrom;
    if ((GetWindowLongW(list, GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT) return CDRF_DODEFAULT;
    if (customDraw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    if (customDraw->nmcd.dwDrawStage == CDDS_POSTPAINT) {
        PaintTableBody(list, customDraw->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    if (customDraw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT || g_highContrast || customDraw->dwItemType != LVCDI_ITEM) return CDRF_DODEFAULT;
    return PaintTableRow(list, customDraw->nmcd.hdc, (int)customDraw->nmcd.dwItemSpec) ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
}

/* The table's edge in the header: its last item's right edge, in the header's own coordinates. */
static BOOL HeaderTableEdge(HWND header, int *x)
{
    RECT column;
    int last = LastHeaderColumn(header);
    if (last < 0 || !TableColumnBounds(header, last, &column)) return FALSE;
    *x = column.right;
    return TRUE;
}

/* Light mode: each native header item, then its share of the table's edge. */
static LRESULT LightHeaderCustomDraw(const NMCUSTOMDRAW *customDraw)
{
    HWND header = customDraw->hdr.hwndFrom;
    RECT rc;
    int edge, state;
    if (customDraw->dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYPOSTPAINT;
    if (customDraw->dwDrawStage != CDDS_ITEMPOSTPAINT || !HeaderTableEdge(header, &edge) ||
        !TableColumnBounds(header, (int)customDraw->dwItemSpec, &rc) || edge <= rc.left || edge > rc.right)
        return CDRF_DODEFAULT;
    state = (customDraw->uItemState & CDIS_SELECTED) ? HIS_PRESSED : (customDraw->uItemState & CDIS_HOT) ? HIS_HOT : HIS_NORMAL;
    if (g_highContrast) PaintTableSide(customDraw->hdc, edge, rc.top, rc.bottom, GetSysColor(COLOR_BTNFACE));
    else PaintHeaderSide(header, customDraw->hdc, &rc, edge, state);
    return CDRF_DODEFAULT;
}

/* Dark mode: an item's fill, its divider (where the rows draw theirs; none
 * after the last column) and its title, aligned as its column is. */
static LRESULT PaintDarkHeaderItem(const NMCUSTOMDRAW *customDraw)
{
    WCHAR text[128];
    HDITEMW item;
    HWND header = customDraw->hdr.hwndFrom;
    RECT rc, divider;
    HGDIOBJ old;
    int pad = ScaleForWindow(header, HEADER_TEXT_INSET_DIPS);
    if (!TableColumnBounds(header, (int)customDraw->dwItemSpec, &rc)) return CDRF_DODEFAULT;
    ZeroMemory(&item, sizeof item);
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text;
    item.cchTextMax = ARRAYSIZE(text);
    text[0] = 0;
    Header_GetItem(header, (int)customDraw->dwItemSpec, &item);
    FillSolid(customDraw->hdc, &rc, g_palette.header);
    if ((int)customDraw->dwItemSpec != LastHeaderColumn(header)) {
        divider = rc;
        divider.left = rc.right - TABLE_EDGE_PX;
        divider.right = divider.left + 1;
        FillSolid(customDraw->hdc, &divider, g_palette.divider);
    }
    rc.left += pad;
    rc.right -= pad;
    old = SelectObject(customDraw->hdc, (HFONT)SendMessageW(header, WM_GETFONT, 0, 0));
    SetBkMode(customDraw->hdc, TRANSPARENT);
    SetTextColor(customDraw->hdc, g_palette.color[THEME_TEXT]);
    DrawTextW(customDraw->hdc, text, -1, &rc,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | ColumnAlignment(item.fmt) | Localize_ReadingFlags());
    SelectObject(customDraw->hdc, old);
    return CDRF_SKIPDEFAULT;
}

/* Header labels and interior dividers follow their native item rectangles.
 * The table's outer edge is painted after each item in light mode, and once
 * after all of them in dark mode, where the items are painted here. */
static LRESULT HeaderCustomDraw(const NMCUSTOMDRAW *customDraw)
{
    HWND header = customDraw->hdr.hwndFrom;
    RECT client;
    int edge;
    if (customDraw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | (g_dark ? CDRF_NOTIFYPOSTPAINT : 0);
    if (!g_dark) return LightHeaderCustomDraw(customDraw);
    if (customDraw->dwDrawStage == CDDS_ITEMPREPAINT) return PaintDarkHeaderItem(customDraw);
    if (customDraw->dwDrawStage == CDDS_POSTPAINT && HeaderTableEdge(header, &edge) && GetClientRect(header, &client))
        PaintTableSide(customDraw->hdc, edge, client.top, client.bottom, g_palette.header);
    return CDRF_DODEFAULT;
}

/* What the table component keeps of a list: the columns the user sizes. */
typedef struct TableState {
    int trackedColumn, trackedWidth;   /* the column whose divider was pressed, and its width then */
    BOOL dragging;                     /* the user drags that divider */
    ULONGLONG userSizedColumns;        /* bit n: the user sized column n (columns past 63 never count) */
} TableState;

static ULONGLONG ColumnBit(int column)
{
    return column >= 0 && column < 64 ? (ULONGLONG)1 << column : 0;
}

BOOL Theme_ColumnResizeIsManual(HWND list, int column)
{
    const TableState *table = (const TableState *)GetPropW(list, TABLE_STATE_PROP);
    return table && (table->userSizedColumns & ColumnBit(column)) != 0;
}

/* The column whose divider the user drags: its width is the drag's. */
static BOOL ColumnDragged(HWND list, int column)
{
    const TableState *table = (const TableState *)GetPropW(list, TABLE_STATE_PROP);
    return table && table->dragging && table->trackedColumn == column;
}

static BOOL LastColumnFit(HWND list, int minimum, int column, int width, int *last, int *fit);

void Theme_SetColumnWidth(HWND list, int column, int width)
{
    width = max(0, width);
    if (ListView_GetColumnWidth(list, column) != width) ListView_SetColumnWidth(list, column, width);
}

/* The report table: a selected row keeps its look under the mouse; its
 * header's custom draw comes here (HeaderCustomDraw: a dark header's items,
 * and the table's outer edge in both modes); and the columns the user sizes
 * (a divider dragged or double-clicked, where the list fits the column to its
 * content) are recorded, so that layout keeps their width. */
static LRESULT CALLBACK ListSubclass(HWND list, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    TableState *table = (TableState *)ref;
    /* Over a selected row, the list is told the mouse is off its rows, so the
     * theme does not paint it hot. */
    if (msg == WM_MOUSEMOVE) {
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = (short)LOWORD(lp);
        hit.pt.y = (short)HIWORD(lp);
        if (ListView_HitTest(list, &hit) >= 0 && ListView_GetItemState(list, hit.iItem, LVIS_SELECTED))
            lp = MAKELPARAM(-1, -1);
    } else if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        /* The row clicked becomes selected under the mouse. */
        LRESULT clicked = DefSubclassProc(list, msg, wp, lp);
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = (short)LOWORD(lp);
        hit.pt.y = (short)HIWORD(lp);
        if (ListView_HitTest(list, &hit) >= 0 && ListView_GetItemState(list, hit.iItem, LVIS_SELECTED))
            DefSubclassProc(list, WM_MOUSEMOVE, 0, MAKELPARAM(-1, -1));
        return clicked;
    } else if (msg == WM_NOTIFY && ((const NMHDR *)lp)->hwndFrom == ListView_GetHeader(list)) {
        const NMHDR *notification = (const NMHDR *)lp;
        const NMHEADERW *change = (const NMHEADERW *)lp;
        BOOL resized = (notification->code == HDN_ITEMCHANGINGW || notification->code == HDN_ITEMCHANGEDW) && change->pitem &&
                       (change->pitem->mask & HDI_WIDTH);
        int last, fit;
        if (notification->code == HDN_BEGINTRACKW) {
            LRESULT refused = DefSubclassProc(list, msg, wp, lp);
            if (!refused) {
                table->trackedColumn = change->iItem;
                table->trackedWidth = ListView_GetColumnWidth(list, change->iItem);
                table->dragging = TRUE;
            }
            return refused;
        }
        /* A divider pressed and released where it was sizes nothing. */
        if (notification->code == HDN_ENDTRACKW) {
            int width = change->pitem && (change->pitem->mask & HDI_WIDTH) ? change->pitem->cxy : ListView_GetColumnWidth(list, change->iItem);
            table->dragging = FALSE;
            if (change->iItem != table->trackedColumn || width != table->trackedWidth) table->userSizedColumns |= ColumnBit(change->iItem);
        }
        if (notification->code == NM_RELEASEDCAPTURE) table->dragging = FALSE;   /* a drag cancelled ends there */
        if (notification->code == HDN_DIVIDERDBLCLICKW) table->userSizedColumns |= ColumnBit(change->iItem);
        if (notification->code == NM_CUSTOMDRAW) return HeaderCustomDraw((const NMCUSTOMDRAW *)lp);
        /* Another column's width changes: the last one gives or takes the
         * difference in the same step, narrower before the change and wider
         * after it, so that the columns never outgrow the list for a moment
         * (its scroll bar would come and go). */
        if (resized && notification->code == HDN_ITEMCHANGINGW && LastColumnFit(list, 0, change->iItem, change->pitem->cxy, &last, &fit) &&
            change->iItem != last && fit < ListView_GetColumnWidth(list, last))
            Theme_SetColumnWidth(list, last, fit);
        if (resized && notification->code == HDN_ITEMCHANGEDW) {
            LRESULT result = DefSubclassProc(list, msg, wp, lp);
            if (LastColumnFit(list, 0, -1, 0, &last, &fit) && change->iItem != last && fit > ListView_GetColumnWidth(list, last))
                Theme_SetColumnWidth(list, last, fit);
            return result;
        }
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(list, TABLE_STATE_PROP);
        RemoveWindowSubclass(list, ListSubclass, id);
        HeapFree(GetProcessHeap(), 0, table);
    }
    return DefSubclassProc(list, msg, wp, lp);
}

/* The table component: the list's subclass and its state, made once. */
static void ApplyTableComponent(HWND list)
{
    TableState *table;
    if (GetPropW(list, TABLE_STATE_PROP)) return;
    table = (TableState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *table);
    if (!table) return;
    if (!SetPropW(list, TABLE_STATE_PROP, table) || !SetWindowSubclass(list, ListSubclass, LIST_SUBCLASS, (DWORD_PTR)table)) {
        RemovePropW(list, TABLE_STATE_PROP);
        HeapFree(GetProcessHeap(), 0, table);
    }
}

/* ------------------------------------------------------- smooth scrolling */

/* The mouse wheel scrolls lists and trees smoothly, as a browser does: each
 * notch adds to the distance left, and a short animation covers it pixel by
 * pixel, easing out with time (Core_ScrollStep): a frame at once, then one
 * at each refresh of the screen (the frame clock, below) while there is
 * distance left. A control that moves by whole rows (a tree, a list) takes
 * the next row once the animation is half way across it, so its rows come at
 * the animation's pace. Each frame moves the control at once (ScrollBy),
 * never through the animation a list box adds to its own steps. */
#define SCROLL_TIMER            0x5C01
#define SCROLL_FRAME_MS         USER_TIMER_MINIMUM   /* without the frame clock, a timer's frames */
#define SCROLL_FRAME_LATEST_MS  1000   /* a later frame (a busy window) moves as far as this one would */
#define SCROLL_FRAMES_AT_ONCE   64     /* without a clock or a timer, a notch's frames run at once, up to this many */

typedef struct SmoothScroll {
    int      pending;   /* px the animation has still to cover, down > 0 */
    int      owed;      /* px it covered that the control has not moved yet */
    int      rowPx;     /* a wheel line; 0: one scroll unit */
    int      direction; /* 1 down, -1 up */
    BOOL     running, scrollingItself;
    BOOL     clocked;   /* its frames come from the frame clock, else from its timer */
    LONGLONG last;      /* when the last frame ran (performance counter) */
} SmoothScroll;

static BOOL FramesStart(HWND window);
static void FramesStop(HWND window);

/* The height of one scroll position of the control. */
static int ScrollUnit(HWND window)
{
    RECT r;
    if (IsClass(window, WC_TREEVIEWW)) return TreeView_GetItemHeight(window);
    if (IsClass(window, WC_LISTBOXW)) return (int)SendMessageW(window, LB_GETITEMHEIGHT, 0, 0);
    if (IsClass(window, WC_LISTVIEWW) && ListView_GetItemCount(window) > 0 && ListView_GetItemRect(window, 0, &r, LVIR_BOUNDS))
        return r.bottom - r.top;
    return (GetWindowLongW(window, GWL_STYLE) & WS_VSCROLL) ? 1 : 0;   /* a window of ours, scrolled by the pixel */
}

static void ScrollStop(HWND window, SmoothScroll *scroll)
{
    if (scroll->running && scroll->clocked) FramesStop(window);
    else if (scroll->running) KillTimer(window, SCROLL_TIMER);
    scroll->running = scroll->clocked = FALSE;
    scroll->pending = scroll->owed = 0;
}

static int ScrollPos(HWND window)
{
    SCROLLINFO scrollInfo;
    ZeroMemory(&scrollInfo, sizeof scrollInfo);
    scrollInfo.cbSize = sizeof scrollInfo;
    scrollInfo.fMask = SIF_POS;
    return GetScrollInfo(window, SB_VERT, &scrollInfo) ? scrollInfo.nPos : 0;
}

/* Whole rows closest to `px` (of `row` px each): none under half a row. */
static int NearestWholeRows(int px, int row)
{
    return row > 0 ? (px + (px >= 0 ? row / 2 : -(row / 2))) / row : 0;
}

/* The height in px of the tree's row that would come into view next, down
 * (`down`) or up; 0 at an end. */
static int NextTreeRow(HWND tree, BOOL down)
{
    HTREEITEM first = TreeView_GetFirstVisible(tree), next;
    RECT rc;
    if (!first) return 0;
    next = down ? first : TreeView_GetPrevVisible(tree, first);   /* going down, the top row leaves */
    return next && TreeView_GetItemRect(tree, next, &rc, FALSE) ? rc.bottom - rc.top : 0;
}

/* A control that moved with its drawing off, drawn again whole, its scroll
 * bar with it. */
static void RedrawMoved(HWND window)
{
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

/* After the rows moved under a still mouse, the one under it is the one
 * shown under the mouse (not the one that was there); no tip opens for it
 * (g_replayedMouseMove). No button state goes with it: a move with a button
 * down would start the control's own drag handling. */
static void HoverUnderMouse(HWND window)
{
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !ScreenToClient(window, &pt) || !GetClientRect(window, &rc) || !PtInRect(&rc, pt)) return;
    g_replayedMouseMove = window;
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
    g_replayedMouseMove = NULL;
}

/* The control moved by about `px`, each its own way; returns the px it
 * moved (0: none, `*end` set when it could not). A list box animates its own
 * steps (some 400 ms each), and a tree scrolled on screen shifts its pixels
 * and repaints strips, its text glitching: both move with their drawing off,
 * then are drawn once. A tree takes each next row once `px` is half way
 * across it. A tree and a list view ignore a thumb position they did not
 * track. */
static int ScrollBy(HWND window, int px, BOOL *end)
{
    int unit, before, moved = 0, rows, row;
    *end = FALSE;
    if (IsClass(window, WC_LISTBOXW)) {
        /* Its top row kept where its scroll bar can show it: a list box takes
         * a top row past its last page, and its scroll bar is then stuck. */
        SCROLLINFO scrollInfo;
        int top = (int)SendMessageW(window, LB_GETTOPINDEX, 0, 0), target;
        unit = ScrollUnit(window);
        if ((rows = NearestWholeRows(px, unit)) == 0) return 0;
        ZeroMemory(&scrollInfo, sizeof scrollInfo);
        scrollInfo.cbSize = sizeof scrollInfo;
        scrollInfo.fMask = SIF_RANGE | SIF_PAGE;
        GetScrollInfo(window, SB_VERT, &scrollInfo);
        target = max(0, min(top + rows, scrollInfo.nMax - max(0, (int)scrollInfo.nPage - 1)));
        if (target == top) {
            *end = TRUE;
            return 0;
        }
        SendMessageW(window, WM_SETREDRAW, FALSE, 0);
        SendMessageW(window, LB_SETTOPINDEX, (WPARAM)target, 0);
        RedrawMoved(window);
        HoverUnderMouse(window);
        return ((int)SendMessageW(window, LB_GETTOPINDEX, 0, 0) - top) * unit;
    }
    if (IsClass(window, WC_TREEVIEWW)) {
        BOOL off = FALSE;
        while ((row = NextTreeRow(window, px > 0)) > 0 && 2 * (px > 0 ? px - moved : moved - px) >= row) {   /* half a row still to go */
            HTREEITEM first = TreeView_GetFirstVisible(window);
            if (!off) SendMessageW(window, WM_SETREDRAW, FALSE, 0);
            off = TRUE;
            SendMessageW(window, WM_VSCROLL, px > 0 ? SB_LINEDOWN : SB_LINEUP, 0);
            if (TreeView_GetFirstVisible(window) == first) {
                row = 0;   /* at an end */
                break;
            }
            moved += px > 0 ? row : -row;
        }
        if (off) RedrawMoved(window);
        if (moved) HoverUnderMouse(window);
        if (moved == 0 && row == 0) *end = TRUE;
        return moved;
    }
    unit = ScrollUnit(window);
    before = ScrollPos(window);
    if (IsClass(window, WC_LISTVIEWW)) {
        if ((rows = NearestWholeRows(px, unit)) == 0) return 0;
        ListView_Scroll(window, 0, rows * unit);
    } else {
        /* A window of ours, by the pixel; the position is 16 bits: never
         * below 0, where it would wrap to the end. */
        SendMessageW(window, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, max(0, before + px)), 0);
    }
    moved = (ScrollPos(window) - before) * unit;
    if (moved == 0) *end = TRUE;
    else HoverUnderMouse(window);
    return moved;
}

/* A key, a click of any button or a context menu ends a wheel animation:
 * rows must not slide under the pointer or an open menu. */
static BOOL StopsWheel(UINT msg)
{
    return msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_CONTEXTMENU;
}

static void ScrollFrame(HWND window, SmoothScroll *scroll)
{
    LARGE_INTEGER now, rate;
    int elapsed = SCROLL_FRAME_MS, step;
    BOOL end;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&rate);
    if (scroll->last) elapsed = (int)min(SCROLL_FRAME_LATEST_MS, (now.QuadPart - scroll->last) * 1000 / rate.QuadPart);
    scroll->last = now.QuadPart;
    step = Core_ScrollStep(scroll->pending, elapsed);
    scroll->pending -= step;
    scroll->owed += step;
    /* A row taken half way leaves the control a little ahead: it waits for
     * the animation there, it never comes back. */
    end = FALSE;
    scroll->scrollingItself = TRUE;
    if (scroll->owed * scroll->direction > 0) scroll->owed -= ScrollBy(window, scroll->owed, &end);
    scroll->scrollingItself = FALSE;
    if (end || (scroll->pending == 0 && !step)) ScrollStop(window, scroll);   /* at an end, or all covered */
}

static HWND ViewAround(HWND control);
static BOOL ViewScrollsControl(HWND control);
static int WheelRow(HWND window, int unit);

static LRESULT CALLBACK ScrollSubclass(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    SmoothScroll *scroll = (SmoothScroll *)ref;
    if (StopsWheel(msg)) ScrollStop(window, scroll);
    switch (msg) {
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        int unit = ScrollUnit(window), notch, frames;
        /* A control in a view that scrolls it passes the wheel on
         * (ViewedSubclass): its own serves while it is too tall for one. */
        if (unit <= 0 || ViewScrollsControl(window)) break;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL) {
            RECT rc;
            GetClientRect(window, &rc);
            notch = rc.bottom;
        } else {
            notch = (int)lines * (scroll->rowPx > 0 ? scroll->rowPx : WheelRow(window, unit));
        }
        /* The other way: what was left of the last move is dropped. */
        if ((GET_WHEEL_DELTA_WPARAM(wp) > 0) == (scroll->pending + scroll->owed > 0)) scroll->pending = scroll->owed = 0;
        scroll->pending -= MulDiv(GET_WHEEL_DELTA_WPARAM(wp), notch, WHEEL_DELTA);
        scroll->direction = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
        if (!scroll->running) {
            scroll->clocked = FramesStart(window);
            scroll->running = scroll->clocked || SetTimer(window, SCROLL_TIMER, SCROLL_FRAME_MS, NULL) != 0;
            scroll->last = 0;   /* the first frame of a scroll: at once */
        }
        ScrollFrame(window, scroll);
        /* No clock nor timer: the rest at once. */
        for (frames = 0; !scroll->running && scroll->pending && frames < SCROLL_FRAMES_AT_ONCE; frames++) ScrollFrame(window, scroll);
        return 0;
    }
    case WM_TIMER:
        if (wp == SCROLL_TIMER) {
            ScrollFrame(window, scroll);
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (!scroll->scrollingItself) ScrollStop(window, scroll);   /* the scroll bar or the keyboard takes over */
        break;
    case WM_NCDESTROY:
        ScrollStop(window, scroll);
        RemoveWindowSubclass(window, ScrollSubclass, id);
        HeapFree(GetProcessHeap(), 0, scroll);
        return DefSubclassProc(window, msg, wp, lp);
    }
    return DefSubclassProc(window, msg, wp, lp);
}

static SmoothScroll *SmoothScrollOf(HWND window)
{
    DWORD_PTR ref = 0;
    return GetWindowSubclass(window, ScrollSubclass, SCROLL_SUBCLASS, &ref) ? (SmoothScroll *)ref : NULL;
}

/* The frame clock: an animation's frames come with the screen's refresh, as
 * a timer's cannot (WM_TIMER comes some 15.6 ms apart at best, and only once
 * the queue is empty). While an animation runs, a thread waits for each
 * composition (DwmFlush) and posts a frame to a window of the theme's, which
 * moves every running animation. One frame is posted at a time, once the
 * last one was handled, so that input never waits behind frames. */
#define FRAMES_CLASS     L"ClaudeDesktopProfilesManager.Frames"
#define FRAMES_ANIMATED  16   /* animations running at once, at most */
#define FRAME_NO_WAIT_MS 1    /* a composition that did not wait: nothing changed on screen */

static struct {
    HWND window;                       /* message-only, on the window thread: frames are handled there */
    HANDLE thread, wanted;             /* the clock, and its event, set while an animation runs */
    volatile LONG posted;              /* a frame waits to be handled */
    BOOL failed;                       /* no clock: animations use their timer */
    HWND animated[FRAMES_ANIMATED];    /* window thread only */
    int count;
} g_frames;

static DWORD WINAPI FrameClock(void *unused)
{
    LARGE_INTEGER rate, before, after;
    (void)unused;
    QueryPerformanceFrequency(&rate);
    while (WaitForSingleObject(g_frames.wanted, INFINITE) == WAIT_OBJECT_0) {
        QueryPerformanceCounter(&before);
        /* The next composition. One that did not wait had nothing new to
         * show: the clock then waits a timer's period, not spinning. */
        if (FAILED(DwmFlush())) Sleep(SCROLL_FRAME_MS);
        else if (QueryPerformanceCounter(&after) && (after.QuadPart - before.QuadPart) * 1000 < FRAME_NO_WAIT_MS * rate.QuadPart) Sleep(SCROLL_FRAME_MS);
        if (InterlockedCompareExchange(&g_frames.posted, 1, 0) == 0 && !PostMessageW(g_frames.window, WM_THEME_FRAME, 0, 0))
            InterlockedExchange(&g_frames.posted, 0);
    }
    return 0;
}

static LRESULT CALLBACK FramesProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_THEME_FRAME) {
        HWND animated[FRAMES_ANIMATED];
        int count = g_frames.count, i;
        /* A frame can end its animation (FramesStop): the ones that ran are kept here. */
        CopyMemory(animated, g_frames.animated, (size_t)count * sizeof *animated);
        for (i = 0; i < count; i++) {
            SmoothScroll *scroll = SmoothScrollOf(animated[i]);
            if (scroll && scroll->running && scroll->clocked) ScrollFrame(animated[i], scroll);
        }
        InterlockedExchange(&g_frames.posted, 0);   /* handled: the next may come */
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

/* The clock, made on the first animation; FALSE when it cannot run. */
static BOOL FramesReady(void)
{
    WNDCLASSEXW frames;
    if (g_frames.thread) return TRUE;
    if (g_frames.failed) return FALSE;
    ZeroMemory(&frames, sizeof frames);
    frames.cbSize = sizeof frames;
    frames.lpfnWndProc = FramesProc;
    frames.hInstance = GetModuleHandleW(NULL);
    frames.lpszClassName = FRAMES_CLASS;
    RegisterClassExW(&frames);
    g_frames.window = CreateWindowExW(0, FRAMES_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, frames.hInstance, NULL);
    g_frames.wanted = g_frames.window ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
    g_frames.thread = g_frames.wanted ? CreateThread(NULL, 0, FrameClock, NULL, 0, NULL) : NULL;
    if (g_frames.thread) return TRUE;
    if (g_frames.wanted) CloseHandle(g_frames.wanted);
    if (g_frames.window) DestroyWindow(g_frames.window);
    g_frames.wanted = NULL;
    g_frames.window = NULL;
    g_frames.failed = TRUE;
    return FALSE;
}

/* `window`'s animation takes the clock's frames; FALSE when it cannot. */
static BOOL FramesStart(HWND window)
{
    int i;
    if (!FramesReady()) return FALSE;
    for (i = 0; i < g_frames.count; i++)
        if (g_frames.animated[i] == window) return TRUE;
    if (g_frames.count == FRAMES_ANIMATED) return FALSE;
    g_frames.animated[g_frames.count++] = window;
    SetEvent(g_frames.wanted);
    return TRUE;
}

static void FramesStop(HWND window)
{
    int i;
    for (i = 0; i < g_frames.count; i++) {
        if (g_frames.animated[i] != window) continue;
        g_frames.animated[i] = g_frames.animated[--g_frames.count];
        break;
    }
    if (!g_frames.count && g_frames.wanted) ResetEvent(g_frames.wanted);   /* the clock sleeps */
}

/* The wheel of `target` scrolls smoothly, `rowPx` per wheel line. */
static void SetSmoothScroll(HWND target, int rowPx)
{
    SmoothScroll *scroll = SmoothScrollOf(target);
    if (scroll) {
        scroll->rowPx = rowPx;
        return;
    }
    if ((scroll = (SmoothScroll *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *scroll)) == NULL) return;
    scroll->rowPx = rowPx;
    if (!SetWindowSubclass(target, ScrollSubclass, SCROLL_SUBCLASS, (DWORD_PTR)scroll)) HeapFree(GetProcessHeap(), 0, scroll);
}

static void SmoothScrolling(HWND window)
{
    if (!SmoothScrollOf(window)) SetSmoothScroll(window, 0);
}

/* A control in a view: the view scrolls it, and the control itself once it
 * is too tall for a view (ViewLayout); both take the same wheel line. */
void Theme_SetScrollRow(HWND control, int rowPx)
{
    HWND view = ViewAround(control);
    SmoothScroll *controlScroll = view ? SmoothScrollOf(control) : NULL;
    SetSmoothScroll(view ? view : control, rowPx);
    if (controlScroll) controlScroll->rowPx = rowPx;
}

/* What ends a control's wheel animation (StopsWheel) ends its view's too. */
static void StopSmoothScroll(HWND window)
{
    SmoothScroll *scroll = SmoothScrollOf(window);
    if (scroll) ScrollStop(window, scroll);
}

static BOOL ScrollingItself(HWND window)
{
    SmoothScroll *scroll = SmoothScrollOf(window);
    return scroll && scroll->scrollingItself;
}

/* ------------------------------------------------------ fields and frames */

#define FRAME_COMBO  1   /* a drop-down list: drawn here whole (PaintDropDownList) */
#define FRAME_CENTER 2   /* a one-line edit: its text centered in its height */
#define FRAME_BARE   4   /* a scrolling control whose frame is omitted in dark mode */
#define FRAME_EDIT   8   /* native edit layout and glyphs, one buffered paint */

typedef struct ControlCorners {
    HRGN original;
    UINT references, dpi;
    SIZE size;
    BOOL requested, rounded, applying;
} ControlCorners;

static void ReleaseControlCorners(ControlCorners *corners)
{
    if (--corners->references) return;
    if (corners->original) DeleteObject(corners->original);
    HeapFree(GetProcessHeap(), 0, corners);
}

/* The window region clips descendants and native non-client drawing alike.
 * The component keeps the caller's original region and transfers each new
 * region to Windows only after SetWindowRgn succeeds. */
static void RoundControl(HWND control, BOOL requested)
{
    ControlCorners *corners = (ControlCorners *)GetPropW(control, CORNER_PROP);
    RECT window;
    HRGN region = NULL;
    BOOL rounded = requested && !g_highContrast;
    UINT dpi = GetDpiForWindow(control);
    int width, height;
    if (!corners && !requested) return;
    if (!corners) {
        corners = (ControlCorners *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *corners);
        if (!corners) return;
        corners->references = 1;
        corners->original = CreateRectRgn(0, 0, 0, 0);
        if (!corners->original) { ReleaseControlCorners(corners); return; }
        if (GetWindowRgn(control, corners->original) == ERROR) {
            DeleteObject(corners->original);
            corners->original = NULL;
        }
        if (!SetPropW(control, CORNER_PROP, corners)) { ReleaseControlCorners(corners); return; }
    }
    corners->requested = requested;
    if (corners->applying || !GetWindowRect(control, &window)) return;
    width = window.right - window.left;
    height = window.bottom - window.top;
    if (corners->rounded == rounded && corners->size.cx == width && corners->size.cy == height && corners->dpi == dpi) return;
    if (rounded) {
        int radius = min(CornerRadius(control), min(width, height) / 2);
        region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 2 * radius, 2 * radius);
        if (!region) return;
        if (corners->original) CombineRgn(region, region, corners->original, RGN_AND);
    } else if (corners->original) {
        region = CreateRectRgn(0, 0, 0, 0);
        if (!region) return;
        CombineRgn(region, corners->original, NULL, RGN_COPY);
    }
    corners->references++;
    corners->applying = TRUE;
    if (SetWindowRgn(control, region, (GetWindowLongW(control, GWL_STYLE) & WS_VISIBLE) != 0)) {
        corners->rounded = rounded;
        corners->size.cx = width;
        corners->size.cy = height;
        corners->dpi = dpi;
    } else if (region) DeleteObject(region);
    corners->applying = FALSE;
    ReleaseControlCorners(corners);
}

/* What an edit is filled with: what its parent answers to WM_CTLCOLOR*
 * (a dialog procedure returns the brush itself). */
static HBRUSH EditBrush(HWND edit, HDC dc)
{
    BOOL still = !IsWindowEnabled(edit) || (GetWindowLongW(edit, GWL_STYLE) & ES_READONLY);
    HBRUSH brush = (HBRUSH)SendMessageW(GetParent(edit), still ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)edit);
    return brush ? brush : GetSysColorBrush(still ? COLOR_3DFACE : COLOR_WINDOW);
}

/* An edit's non-client area, but its scroll bars. Outside a contrast theme a
 * rounded frame follows the window: in dark mode the field reaches the edge
 * (no line), in light mode a line in the theme's border color for the
 * edit's state. A contrast theme keeps the native frame; the margins inside
 * it are filled in the edit's fill, as they are under the rounded frame,
 * whose anti-aliased corners blend into them. `given` is a window DC
 * (WM_PRINT), or NULL. */
static void PaintEditFrame(HWND edit, HDC given)
{
    static const LONG kBars[] = { OBJID_VSCROLL, OBJID_HSCROLL };
    RECT window, client;
    HDC dc = given ? given : GetWindowDC(edit);
    HBRUSH brush;
    UINT dpi = GetDpiForWindow(edit);
    int saved, i, nativeFrame = 0;
    if (!dc) return;
    if (!g_dark && (GetWindowLongW(edit, GWL_EXSTYLE) & WS_EX_CLIENTEDGE)) nativeFrame = GetSystemMetricsForDpi(SM_CXEDGE, dpi);
    else if (!g_dark && (GetWindowLongW(edit, GWL_STYLE) & WS_BORDER)) nativeFrame = GetSystemMetricsForDpi(SM_CXBORDER, dpi);
    saved = SaveDC(dc);
    GetWindowRect(edit, &window);
    GetClientRect(edit, &client);
    MapWindowPoints(edit, NULL, (POINT *)&client, 2);
    OffsetRect(&client, -window.left, -window.top);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    for (i = 0; i < (int)ARRAYSIZE(kBars); i++) {
        SCROLLBARINFO bar;
        ZeroMemory(&bar, sizeof bar);
        bar.cbSize = sizeof bar;
        if (GetScrollBarInfo(edit, kBars[i], &bar) && !(bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) {
            OffsetRect(&bar.rcScrollBar, -window.left, -window.top);
            ExcludeClipRect(dc, bar.rcScrollBar.left, bar.rcScrollBar.top, bar.rcScrollBar.right, bar.rcScrollBar.bottom);
        }
    }
    OffsetRect(&window, -window.left, -window.top);
    InflateRect(&window, -nativeFrame, -nativeFrame);
    brush = EditBrush(edit, dc);
    FillRect(dc, &window, brush);
    if (!g_highContrast) {
        LOGBRUSH brushInfo = { 0 };
        COLORREF fill = g_palette.color[THEME_FIELD], frame;
        HTHEME theme = NULL;
        if (GetObjectW(brush, sizeof brushInfo, &brushInfo) && brushInfo.lbStyle == BS_SOLID) fill = brushInfo.lbColor;
        frame = fill;
        if (!g_dark) {
            int state = !IsWindowEnabled(edit) ? EPSN_DISABLED : GetFocus() == edit ? EPSN_FOCUSED : EPSN_NORMAL;
            frame = GetSysColor(COLOR_WINDOWFRAME);
            theme = OpenThemeData(edit, L"Edit");
            if (theme) GetThemeColor(theme, EP_EDITBORDER_NOSCROLL, state, TMT_BORDERCOLOR, &frame);
        }
        /* Native client margins and scroll-bar rectangles keep their own layout. */
        InflateRect(&window, nativeFrame, nativeFrame);
        RoundedBox(dc, &window, CornerRadius(edit), fill, frame, g_dark ? 0 : LineWidth(edit));
        if (theme) CloseThemeData(theme);
    }
    RestoreDC(dc, saved);
    if (!given) ReleaseDC(edit, dc);
}

#define MADE_RECORDED 1
#define MADE_BORDER   2   /* WS_BORDER */
#define MADE_EDGE     4   /* WS_EX_CLIENTEDGE */
#define MADE_SCROLLS  8   /* scroll bars, including an axis hidden while its content fits */

/* The frame and scroll bar a control was made with (MADE_*), recorded the
 * first time. */
static INT_PTR RecordedFrameFlags(HWND control)
{
    INT_PTR made = (INT_PTR)GetPropW(control, BORDER_PROP);
    if (!made) {
        LONG style = GetWindowLongW(control, GWL_STYLE);
        made = MADE_RECORDED | ((style & WS_BORDER) ? MADE_BORDER : 0) | ((style & (WS_VSCROLL | WS_HSCROLL)) ? MADE_SCROLLS : 0) |
               ((GetWindowLongW(control, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) ? MADE_EDGE : 0);
        SetPropW(control, BORDER_PROP, (HANDLE)made);
    }
    return made;
}

/* A list, tree, list box or multi-line edit scrolls. In dark mode it has no
 * frame: its field and its scroll bars reach its edges (a frame painted in
 * the field's color would leave a line around the scroll bar). In light mode
 * it has the frame it was made with (`made`). */
static void FitFrame(HWND control, INT_PTR made)
{
    LONG style = GetWindowLongW(control, GWL_STYLE), extendedStyle = GetWindowLongW(control, GWL_EXSTYLE), wantStyle, wantExtendedStyle;
    wantStyle = g_dark || !(made & MADE_BORDER) ? style & ~WS_BORDER : style | WS_BORDER;
    wantExtendedStyle = g_dark || !(made & MADE_EDGE) ? extendedStyle & ~WS_EX_CLIENTEDGE : extendedStyle | WS_EX_CLIENTEDGE;
    if (wantStyle == style && wantExtendedStyle == extendedStyle) return;
    SetWindowLongW(control, GWL_STYLE, wantStyle);
    SetWindowLongW(control, GWL_EXSTYLE, wantExtendedStyle);
    SetWindowPos(control, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* A one-line edit draws its text at the top of its client area: the client
 * area starts lower by half the height the text leaves free (it keeps the
 * rest below the text), so the text, its selection and the caret sit in the
 * middle. */
static void CenterEditText(HWND edit, RECT *client)
{
    TEXTMETRICW tm;
    HDC dc = GetDC(edit);
    HFONT font = (HFONT)SendMessageW(edit, WM_GETFONT, 0, 0);
    HGDIOBJ old;
    int spare;
    if (!dc) return;
    old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(edit, dc);
    spare = (client->bottom - client->top) - tm.tmHeight;
    if (spare > 1) client->top += spare / 2;
}

static BOOL IsDropDownList(HWND control)
{
    LONG style = GetWindowLongW(control, GWL_STYLE);
    return IsClass(control, WC_COMBOBOXW) && (style & (CBS_SIMPLE | CBS_DROPDOWN | CBS_DROPDOWNLIST)) == CBS_DROPDOWNLIST;
}

#define COMBO_ROW_PADDING_DIPS 8   /* above and below a choice's text in the list */
#define COMBO_BOX_PADDING_DIPS 4   /* above and below the closed box's text */

static SIZE MeasureEveryLanguage(HWND control, const WCHAR *const *keys, int keyCount, const WCHAR *value, UINT format, int width,
                                 int *tallestFont);

/* The closed box's height and its rows' follow its font. Its native list
 * never opens (QueueChoice shows a menu): the space it would take is not
 * kept. A height set again resizes the combo: only a change is set. Its
 * rows take the tallest script's height, so that a new language changes
 * nothing there: a combo on screen given another row height also gives its
 * list the height of all its rows. */
static void FitComboRows(HWND combo)
{
    HDC dc = GetDC(combo);
    HGDIOBJ old;
    TEXTMETRICW metrics;
    int rowHeight, boxHeight, tallestFont = 0;
    if (!dc) return;
    old = SelectObject(dc, (HFONT)SendMessageW(combo, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(combo, dc);
    MeasureEveryLanguage(combo, NULL, 0, NULL, DT_SINGLELINE, 0, &tallestFont);
    rowHeight = max(metrics.tmHeight, tallestFont) + ScaleForWindow(combo, COMBO_ROW_PADDING_DIPS);
    boxHeight = metrics.tmHeight + ScaleForWindow(combo, COMBO_BOX_PADDING_DIPS);
    if (SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0) != rowHeight) SendMessageW(combo, CB_SETITEMHEIGHT, 0, rowHeight);
    if (SendMessageW(combo, CB_GETITEMHEIGHT, (WPARAM)-1, 0) != boxHeight) SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, boxHeight);
}

/* An item's text: in `local` when it fits, else in a heap block the caller
 * frees. */
static WCHAR *ComboItemText(HWND combo, LRESULT item, WCHAR *local, size_t localCount)
{
    LRESULT length = item >= 0 ? SendMessageW(combo, CB_GETLBTEXTLEN, (WPARAM)item, 0) : CB_ERR;
    WCHAR *text = local;
    local[0] = 0;
    if (length < 0) return local;
    if ((size_t)length >= localCount) {
        text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)length + 1) * sizeof(WCHAR));
        if (!text) return local;
    }
    if (SendMessageW(combo, CB_GETLBTEXT, (WPARAM)item, (LPARAM)text) == CB_ERR) text[0] = 0;
    return text;
}

/* A drop-down list, drawn whole like the drop-down buttons
 * (Theme_DrawDropDown): its choice on the left, a button under the mouse,
 * pressed while its menu shows. Its choice is never shown selected; focus
 * shows as a focus rectangle, after keyboard use only. While the menu shows,
 * the box keeps its value: the menu changes it only once an item is chosen. */
static void PaintDropDownList(HWND combo, HDC dc)
{
    WCHAR local[256], *text;
    RECT rc;
    UINT state = 0;
    BOOL dropped = SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
    text = ComboItemText(combo, SendMessageW(combo, CB_GETCURSEL, 0, 0), local, ARRAYSIZE(local));
    if (!IsWindowEnabled(combo)) state = THEME_BUTTON_DISABLED;
    else if (dropped) state = THEME_BUTTON_PRESSED;
    else if (GetPropW(combo, HOT_PROP)) state = THEME_BUTTON_HOT;
    GetClientRect(combo, &rc);
    Theme_DrawDropDown(combo, dc, &rc, text, (HFONT)SendMessageW(combo, WM_GETFONT, 0, 0), state);
    if (text != local) HeapFree(GetProcessHeap(), 0, text);
    if (ShowsFocusCues(combo) && GetFocus() == combo && !(state & THEME_BUTTON_PRESSED)) {
        RECT focus = rc;
        InflateRect(&focus, -FocusRectangleInset(combo), -FocusRectangleInset(combo));
        SetTextColor(dc, g_dark ? g_palette.color[THEME_TEXT] : GetSysColor(COLOR_BTNTEXT));
        SetBkColor(dc, g_dark ? g_palette.button : GetSysColor(COLOR_3DFACE));
        DrawFocusRect(dc, &focus);
    }
}

static void ComboNotify(HWND combo, UINT notification)
{
    SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), notification), (LPARAM)combo);
}

/* A queued choice can be cancelled before Windows starts menu tracking. */
static void CancelChoice(HWND combo)
{
    INT_PTR choice = (INT_PTR)GetPropW(combo, CHOICE_PROP);
    if (!choice) return;
    if (choice == CHOICE_TRACKING) {
        EndMenu();
        return;
    }
    RemovePropW(combo, CHOICE_PROP);
    ComboNotify(combo, CBN_SELENDCANCEL);
    if (IsWindow(combo)) ComboNotify(combo, CBN_CLOSEUP);
    if (IsWindow(combo)) {
        RedrawWindow(combo, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, combo, OBJID_CLIENT, CHILDID_SELF);
    }
}

/* The native combo holds the values and supplies its closed keyboard and
 * accessibility behavior; the shared drop-down menu presents the choices. */
static void QueueChoice(HWND combo)
{
    if (!IsWindowEnabled(combo) || GetPropW(combo, CHOICE_PROP) || SendMessageW(combo, CB_GETCOUNT, 0, 0) <= 0) return;
    SetPropW(combo, CHOICE_PROP, (HANDLE)CHOICE_QUEUED);
    ComboNotify(combo, CBN_DROPDOWN);
    if (IsWindow(combo) && GetPropW(combo, CHOICE_PROP)) {
        PostMessageW(combo, WM_THEME_CHOICE, 0, 0);
        RedrawWindow(combo, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, combo, OBJID_CLIENT, CHILDID_SELF);
    }
}

/* The combo's choices as a menu, the current one checked; NULL when one
 * could not be read or added. Choice values are literal labels: an
 * ampersand is no mnemonic there. */
static HMENU ChoiceMenu(HWND combo, int count, int current)
{
    HMENU menu = CreatePopupMenu();
    int i;
    if (!menu) return NULL;
    for (i = 0; i < count; i++) {
        LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, i, 0);
        WCHAR *text, *label;
        size_t input, output = 0;
        MENUITEMINFOW item;
        BOOL added;
        if (length < 0 || (size_t)length > (((size_t)-1 / sizeof(WCHAR)) - 2) / 3) break;
        text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (3 * (size_t)length + 2) * sizeof(WCHAR));
        if (!text) break;
        if (SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)text) == CB_ERR) {
            HeapFree(GetProcessHeap(), 0, text);
            break;
        }
        label = text + length + 1;
        for (input = 0; input < (size_t)length; input++) {
            if (text[input] == L'&') label[output++] = L'&';
            label[output++] = text[input];
        }
        label[output] = 0;
        ZeroMemory(&item, sizeof item);
        item.cbSize = sizeof item;
        item.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
        item.wID = (UINT)i + 1;
        item.dwTypeData = label;
        item.fType = MFT_STRING | MFT_RADIOCHECK;
        item.fState = i == current ? MFS_CHECKED : MFS_UNCHECKED;
        added = InsertMenuItemW(menu, (UINT)i, TRUE, &item);
        HeapFree(GetProcessHeap(), 0, text);
        if (!added) break;
    }
    if (i < count) {
        DestroyMenu(menu);
        return NULL;
    }
    return menu;
}

static void TrackChoice(HWND combo)
{
    HMENU menu;
    RECT box;
    int original, count;
    UINT command = 0;
    if ((INT_PTR)GetPropW(combo, CHOICE_PROP) != CHOICE_QUEUED) return;
    original = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    menu = ChoiceMenu(combo, count, original);
    if (!menu) {
        CancelChoice(combo);
        return;
    }
    if (IsWindow(combo) && (INT_PTR)GetPropW(combo, CHOICE_PROP) == CHOICE_QUEUED && GetWindowRect(combo, &box)) {
        SetPropW(combo, CHOICE_PROP, (HANDLE)CHOICE_TRACKING);
        RedrawWindow(combo, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        command = Theme_TrackDropDown(GetParent(combo), menu, &box);
    }
    DestroyMenu(menu);
    if (!IsWindow(combo) || !GetPropW(combo, CHOICE_PROP)) return;
    RemovePropW(combo, CHOICE_PROP);
    if (command && command <= (UINT)count) {
        int selected = (int)command - 1;
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        if (selected != original) ComboNotify(combo, CBN_SELCHANGE);
        if (IsWindow(combo)) ComboNotify(combo, CBN_SELENDOK);
    } else ComboNotify(combo, CBN_SELENDCANCEL);
    if (IsWindow(combo)) ComboNotify(combo, CBN_CLOSEUP);
    if (IsWindow(combo)) {
        RedrawWindow(combo, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, combo, OBJID_CLIENT, CHILDID_SELF);
    }
}

/* One channel of a pixel drawn between `sourceBack` and `sourceText`, as
 * far between `targetBack` and `targetText`. */
static int RemapChannel(int value, int sourceBack, int sourceText, int targetBack, int targetText)
{
    int span = sourceText - sourceBack, part = value - sourceBack;
    if (!span) return targetBack;
    part = span > 0 ? max(0, min(part, span)) : max(span, min(part, 0));
    return targetBack + MulDiv(targetText - targetBack, part, span);
}

static DWORD RemapPixel(DWORD pixel, COLORREF sourceBack, COLORREF sourceText, COLORREF targetBack, COLORREF targetText)
{
    int red = RemapChannel((pixel >> 16) & 255, GetRValue(sourceBack), GetRValue(sourceText), GetRValue(targetBack), GetRValue(targetText));
    int green = RemapChannel((pixel >> 8) & 255, GetGValue(sourceBack), GetGValue(sourceText), GetGValue(targetBack), GetGValue(targetText));
    int blue = RemapChannel(pixel & 255, GetBValue(sourceBack), GetBValue(sourceText), GetBValue(targetBack), GetBValue(targetText));
    return (pixel & 0xFF000000u) | ((DWORD)red << 16) | ((DWORD)green << 8) | (DWORD)blue;
}

/* Recolor the native selection in the bitmap without drawing its glyphs
 * again: character positions, shaping and line metrics remain the edit's.
 * On each line of pixels, the selection runs between pixels of the system
 * highlight with none of the edit's own background (`field`) between them;
 * its text's smoothed edges are blends of the system's two colors, each
 * channel on its own, and are blended again between the theme's. */
static void PaintEditSelection(HWND edit, HDC dc, COLORREF field)
{
    DWORD start = 0, end = 0;
    DIBSECTION dib;
    RECT rc;
    COLORREF sourceBack = GetSysColor(COLOR_HIGHLIGHT), sourceText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    COLORREF back = g_palette.color[THEME_MAIN_BLUE], text = g_palette.color[THEME_TEXT];
    int x, y, width, height;
    if (!g_dark) return;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (start == end) return;
    if (GetFocus() != edit && !(GetWindowLongW(edit, GWL_STYLE) & ES_NOHIDESEL)) return;
    ZeroMemory(&dib, sizeof dib);
    if (GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof dib, &dib) != (int)sizeof dib || !dib.dsBm.bmBits || dib.dsBm.bmBitsPixel != 32) return;
    GetClientRect(edit, &rc);
    LPtoDP(dc, (POINT *)&rc, 2);
    width = dib.dsBm.bmWidth;
    height = abs(dib.dsBmih.biHeight);
    GdiFlush();
    for (y = max(0, rc.top); y < min(height, rc.bottom); y++) {
        DWORD *pixels = (DWORD *)((BYTE *)dib.dsBm.bmBits + (dib.dsBmih.biHeight < 0 ? y : height - y - 1) * dib.dsBm.bmWidthBytes);
        int lastHighlight = -1, between;
        BOOL fieldSince = FALSE;
        for (x = max(0, rc.left); x < min(width, rc.right); x++) {
            COLORREF native = PixelColor(pixels[x]);
            if (native == field) {
                fieldSince = TRUE;
            } else if (native == sourceBack) {
                if (lastHighlight >= 0 && !fieldSince)
                    for (between = lastHighlight + 1; between < x; between++)
                        pixels[between] = RemapPixel(pixels[between], sourceBack, sourceText, back, text);
                pixels[x] = RemapPixel(pixels[x], sourceBack, sourceText, back, text);
                lastHighlight = x;
                fieldSince = FALSE;
            }
        }
    }
}

/* A control painted off screen by `paint`, then shown at once. */
static void PaintBuffered(HWND control, void (*paint)(HWND, HDC))
{
    PAINTSTRUCT ps;
    ThemeBuffer buffer;
    RECT rc;
    HDC dc = BeginPaint(control, &ps);
    if (!dc) return;
    GetClientRect(control, &rc);
    paint(control, Theme_BufferBegin(&buffer, dc, &rc));
    Theme_BufferEnd(&buffer);
    EndPaint(control, &ps);
}

/* The edit's native drawing is retained in the buffer, including complex
 * scripts and password glyphs. Only selection colors are replaced. */
static void PaintEdit(HWND edit, HDC dc)
{
    RECT rc;
    ThemeBuffer buffer;
    DIBSECTION dib;
    HDC target = dc;
    HBRUSH brush;
    LOGBRUSH brushInfo = { 0 };
    COLORREF field = g_palette.color[THEME_FIELD];
    BOOL needsBuffer;
    GetClientRect(edit, &rc);
    ZeroMemory(&dib, sizeof dib);
    needsBuffer = GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof dib, &dib) != (int)sizeof dib || !dib.dsBm.bmBits;
    if (needsBuffer) dc = Theme_BufferBegin(&buffer, target, &rc);
    brush = EditBrush(edit, dc);
    if (GetObjectW(brush, sizeof brushInfo, &brushInfo) && brushInfo.lbStyle == BS_SOLID) field = brushInfo.lbColor;
    FillRect(dc, &rc, brush);
    DefSubclassProc(edit, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
    PaintEditSelection(edit, dc, field);
    if (needsBuffer) Theme_BufferEnd(&buffer);
}

/* WM_PRINT uses window coordinates; native text uses client coordinates.
 * Both paths share the same buffered text and selection renderer. The
 * buffer starts as a copy of the target, so what is not printed stays. */
static void PrintEdit(HWND edit, HDC target, LPARAM flags)
{
    RECT window, client;
    ThemeBuffer buffer;
    HDC dc;
    POINT origin;
    int saved;
    if ((flags & PRF_CHECKVISIBLE) && !IsWindowVisible(edit)) return;
    GetWindowRect(edit, &window);
    GetClientRect(edit, &client);
    MapWindowPoints(edit, NULL, (POINT *)&client, 2);
    OffsetRect(&client, -window.left, -window.top);
    OffsetRect(&window, -window.left, -window.top);
    dc = Theme_BufferBegin(&buffer, target, &window);
    if (dc != target) BitBlt(dc, 0, 0, window.right, window.bottom, target, 0, 0, SRCCOPY);
    /* The client is painted once, below, not also through the native print. */
    DefSubclassProc(edit, WM_PRINT, (WPARAM)dc, flags & ~(LPARAM)PRF_CLIENT);
    if (flags & PRF_NONCLIENT) PaintEditFrame(edit, dc);
    if (flags & PRF_CLIENT) {
        saved = SaveDC(dc);
        GetViewportOrgEx(dc, &origin);
        SetViewportOrgEx(dc, origin.x + client.left, origin.y + client.top, NULL);
        PaintEdit(edit, dc);
        RestoreDC(dc, saved);
    }
    Theme_BufferEnd(&buffer);
}

static BOOL EditChangesState(HWND edit, UINT msg, WPARAM wp)
{
    switch (msg) {
    case WM_MOUSEMOVE: return (wp & MK_LBUTTON) != 0;
    case WM_TIMER: return GetCapture() == edit;   /* a selection dragged past an end scrolls on */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_KEYDOWN: case WM_CHAR: case WM_SYSCHAR: case WM_SETFOCUS: case WM_KILLFOCUS:
    case WM_CUT: case WM_PASTE: case WM_CLEAR: case WM_UNDO: case EM_UNDO: case WM_SETTEXT:
    case EM_SETSEL: case EM_REPLACESEL: case EM_SETREADONLY: case EM_SETPASSWORDCHAR:
    case EM_SCROLLCARET: case EM_SCROLL: case WM_HSCROLL: case WM_VSCROLL:
    case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_ENABLE:
        return TRUE;
    }
    return FALSE;
}

/* Native input updates state while drawing is paused, then commits one
 * themed paint. A caller's own WM_SETREDRAW batch is left in control. */
static LRESULT UpdateEdit(HWND edit, UINT msg, WPARAM wp, LPARAM lp)
{
    BOOL opensUpdate = !GetPropW(edit, EDIT_UPDATE_PROP) && !GetPropW(edit, EDIT_PAUSED_PROP);
    BOOL visible = (GetWindowLongW(edit, GWL_STYLE) & WS_VISIBLE) != 0;
    LRESULT result;
    if (opensUpdate) {
        SetPropW(edit, EDIT_UPDATE_PROP, (HANDLE)1);
        DefSubclassProc(edit, WM_SETREDRAW, FALSE, 0);
    }
    result = DefSubclassProc(edit, msg, wp, lp);
    if (opensUpdate && IsWindow(edit)) {
        BOOL paused = GetPropW(edit, EDIT_PAUSED_PROP) != NULL;
        if (!paused) {
            DefSubclassProc(edit, WM_SETREDRAW, TRUE, 0);
            if (!visible) SetWindowLongW(edit, GWL_STYLE, GetWindowLongW(edit, GWL_STYLE) & ~WS_VISIBLE);
        }
        RemovePropW(edit, EDIT_UPDATE_PROP);
        /* Scroll bars changed while drawing was paused are part of the frame. */
        if (visible && !paused)
            RedrawWindow(edit, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW |
                         ((GetWindowLongW(edit, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)) ? RDW_FRAME : 0));
    }
    return result;
}

/* Focus rectangles are for the keyboard: Tab or an arrow shows them (the
 * dialog manager does), a click hides them again. */
static void HideFocusCues(HWND control)
{
    HWND root = GetAncestor(control, GA_ROOT);
    if (root && ShowsFocusCues(root))
        SendMessageW(root, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
}

static LRESULT CALLBACK ChildSubclass(HWND control, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR frame);

/* A drop-down list's input and state before the native combo sees them:
 * its choice goes through the shared menu (QueueChoice), never its native
 * list. TRUE when handled, with `*result`. */
static BOOL ComboInput(HWND combo, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    BOOL choosing = GetPropW(combo, CHOICE_PROP) != NULL;
    *result = 0;
    switch (msg) {
    case WM_THEME_CHOICE:
        TrackChoice(combo);
        return TRUE;
    case CB_GETDROPPEDSTATE:
        *result = choosing;
        return TRUE;
    case WM_GETDLGCODE:
        *result = DefSubclassProc(combo, msg, wp, lp);
        if (choosing && (!lp || ((const MSG *)lp)->wParam != VK_TAB)) *result |= DLGC_WANTMESSAGE;
        return TRUE;
    case CB_GETCOMBOBOXINFO:
        *result = DefSubclassProc(combo, msg, wp, lp);
        if (*result && lp && choosing) ((COMBOBOXINFO *)lp)->stateButton |= STATE_SYSTEM_PRESSED;
        return TRUE;
    case CB_SHOWDROPDOWN:
        if (wp) QueueChoice(combo);
        else CancelChoice(combo);
        return TRUE;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        CancelChoice(combo);
        return FALSE;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        HideFocusCues(combo);
        SetFocus(combo);
        SendMessageW(combo, CB_SHOWDROPDOWN, !choosing, 0);
        return TRUE;
    case WM_LBUTTONUP:
        return TRUE;
    case WM_KEYDOWN:
        if (wp == VK_F4 || wp == VK_SPACE || (wp == VK_ESCAPE && choosing)) {
            SendMessageW(combo, CB_SHOWDROPDOWN, wp != VK_ESCAPE && !choosing, 0);
            return TRUE;
        }
        return FALSE;
    case WM_CHAR:
        if (wp != L' ') return FALSE;
        if (!choosing) QueueChoice(combo);
        return TRUE;
    case WM_SYSKEYDOWN:   /* Alt with Down or Up */
        if ((wp != VK_DOWN && wp != VK_UP) || !(HIWORD(lp) & KF_ALTDOWN)) return FALSE;
        SendMessageW(combo, CB_SHOWDROPDOWN, !choosing, 0);
        return TRUE;
    case WM_PAINT:   /* with a DC: ChildSubclass paints the window DC case */
    case WM_PRINTCLIENT:
        PaintDropDownList(combo, (HDC)wp);
        return TRUE;
    case WM_MOUSEMOVE:
        if (!GetPropW(combo, HOT_PROP)) {
            TRACKMOUSEEVENT track = { sizeof track, TME_LEAVE, combo, 0 };
            SetPropW(combo, HOT_PROP, (HANDLE)1);
            TrackMouseEvent(&track);
            InvalidateRect(combo, NULL, FALSE);
        }
        return FALSE;
    case WM_MOUSELEAVE:
        RemovePropW(combo, HOT_PROP);
        InvalidateRect(combo, NULL, FALSE);
        return FALSE;
    }
    return FALSE;
}

/* A drop-down list also draws itself outside WM_PAINT (its choice selected
 * when it has the focus): drawn again at once, off screen. */
static void RedrawCombo(HWND combo, UINT msg)
{
    switch (msg) {
    case WM_SETFONT:
        FitComboRows(combo);
        RedrawWindow(combo, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_MOUSEWHEEL:
    case WM_ENABLE:
    case WM_COMMAND:
    case WM_CAPTURECHANGED:
    case WM_UPDATEUISTATE:
    case CB_SETCURSEL:
        RedrawWindow(combo, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        break;
    }
}

/* An edit's painting: its client in one buffered pass, its print, and its
 * caller's redraw pauses. TRUE when handled, with `*result`. */
static BOOL EditDrawing(HWND edit, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    *result = 0;
    switch (msg) {
    case WM_PRINT:
        PrintEdit(edit, (HDC)wp, lp);
        return TRUE;
    case WM_PRINTCLIENT:
    case WM_PAINT:
        PaintEdit(edit, (HDC)wp);
        return TRUE;
    case WM_SETREDRAW:
        if (wp) RemovePropW(edit, EDIT_PAUSED_PROP);
        else SetPropW(edit, EDIT_PAUSED_PROP, (HANDLE)1);
        return wp && GetPropW(edit, EDIT_UPDATE_PROP);   /* the update in progress shows it */
    }
    return FALSE;
}

/* An edit after the native one handled `msg`: its text centered in a
 * one-line edit (FRAME_CENTER), its frame drawn again. */
static void FollowEdit(HWND edit, UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR frame, LRESULT *result)
{
    if (frame & FRAME_CENTER) {
        switch (msg) {
        case WM_NCCALCSIZE:
            CenterEditText(edit, wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp);
            break;
        case WM_NCHITTEST:
            /* The margin above the text is still the edit. */
            if (*result == HTBORDER || *result == HTNOWHERE) *result = HTCLIENT;
            break;
        case WM_SETFONT:
            SetWindowPos(edit, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
    }
    switch (msg) {
    case WM_NCPAINT:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        PaintEditFrame(edit, NULL);
        break;
    /* A themed edit redraws its native frame when the mouse comes or goes;
     * the frame drawn here follows it then, not on every move. */
    case WM_MOUSEMOVE:
        if (!GetPropW(edit, HOT_PROP)) {
            SetPropW(edit, HOT_PROP, (HANDLE)1);
            PaintEditFrame(edit, NULL);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(edit, HOT_PROP);
        PaintEditFrame(edit, NULL);
        break;
    }
}

/* What the theme keeps on a control goes with it. */
static LRESULT ForgetChild(HWND control, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id)
{
    HFONT strong = (HFONT)RemovePropW(control, STRONG_PROP);
    ControlCorners *corners = (ControlCorners *)RemovePropW(control, CORNER_PROP);
    LRESULT result;
    RemovePropW(control, HOT_PROP);
    if ((INT_PTR)GetPropW(control, CHOICE_PROP) == CHOICE_TRACKING) EndMenu();
    RemovePropW(control, CHOICE_PROP);
    RemovePropW(control, BORDER_PROP);
    RemovePropW(control, EDIT_PAUSED_PROP);
    RemovePropW(control, EDIT_UPDATE_PROP);
    RemoveWindowSubclass(control, ChildSubclass, id);
    result = DefSubclassProc(control, msg, wp, lp);
    if (strong) DeleteObject(strong);
    if (corners) ReleaseControlCorners(corners);
    return result;
}

static LRESULT CALLBACK ChildSubclass(HWND control, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR frame)
{
    LRESULT result;
    /* Edits and drop-down lists are painted here, off screen; the background
     * is part of it. */
    if (frame & (FRAME_EDIT | FRAME_COMBO)) {
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT && !wp) {
            PaintBuffered(control, (frame & FRAME_COMBO) ? PaintDropDownList : PaintEdit);
            return 0;
        }
    }
    if ((frame & FRAME_EDIT) && EditDrawing(control, msg, wp, lp, &result)) return result;
    /* A scrolling control without its frame (FRAME_BARE: dark mode) leaves
     * its non-client area, its scroll bars only, to Windows: the control's
     * own painting would draw the frame it was made with. An edit's rounded
     * frame is drawn over it. */
    if ((frame & FRAME_BARE) && msg == WM_NCPAINT) {
        result = DefWindowProcW(control, msg, wp, lp);
        if (frame & FRAME_EDIT) PaintEditFrame(control, NULL);
        return result;
    }
    if ((frame & FRAME_COMBO) && ComboInput(control, msg, wp, lp, &result)) return result;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        HideFocusCues(control);
        break;
    case WM_SETFONT: {
        /* A control made semibold (Theme_SetStrong) stays so in a new font. */
        HFONT strong = (HFONT)GetPropW(control, STRONG_PROP), made;
        if (strong && (HFONT)wp != strong && (made = StrongOf((HFONT)wp)) != NULL) {
            SetPropW(control, STRONG_PROP, made);
            result = DefSubclassProc(control, msg, (WPARAM)made, lp);
            DeleteObject(strong);
            return result;
        }
        break;
    }
    case WM_NCDESTROY:
        return ForgetChild(control, msg, wp, lp, id);
    }
    result = (frame & FRAME_EDIT) && EditChangesState(control, msg, wp) ? UpdateEdit(control, msg, wp, lp) : DefSubclassProc(control, msg, wp, lp);
    if (msg == WM_SIZE) {
        ControlCorners *corners = (ControlCorners *)GetPropW(control, CORNER_PROP);
        if (corners) RoundControl(control, corners->requested);
    }
    if (frame & FRAME_EDIT) FollowEdit(control, msg, wp, lp, frame, &result);
    if (frame & FRAME_COMBO) RedrawCombo(control, msg);
    return result;
}

/* ----------------------------------------------------------- smooth views */

/* A list, list box or tree scrolls by whole rows, what the program draws
 * itself by the pixel. For every list to scroll alike, Theme_SmoothView puts
 * the control, as tall as all its rows, in a view of ours that scrolls it by
 * the pixel: the control is moved and never scrolls itself; the view has the
 * scroll bar and the smooth wheel (above). A list view's header stays on
 * top; the row the keyboard moves to is scrolled into sight, and Page Up and
 * Page Down move by the view's height. The view takes the control's place,
 * its id (so the dialog shows and hides it) and its frame, and passes on
 * whatever the control tells its parent. A control too tall for a window
 * (THEME_VIEW_MAX_PX), or showing its own horizontal scroll bar (columns
 * wider than the view), scrolls itself, by rows, with a smooth wheel of its
 * own: its scroll bars are then at the view's edges, as a native list's. */
#define VIEW_CLASS    L"ClaudeDesktopProfilesManager.View"
#define REVEAL_KEYBOARD_ROW ((LRESULT)-2)   /* neither an index nor a tree item */

typedef struct View {
    HWND control;       /* NULL once destroyed */
    int  pos;           /* px scrolled */
    int  scaleFrom;     /* the content's height before its font or rows' height changed: the next layout keeps the rows on top (0: none) */
    BOOL posted;        /* a VIEW_MEASURE is on its way */
    BOOL native;        /* too tall, or wider than the view: the control scrolls itself */
    BOOL reveal;        /* the next VIEW_MEASURE scrolls revealRow into sight (RevealLater) */
    LRESULT revealRow;
    BOOL refilling;     /* the control's drawing is off (WM_SETREDRAW): a selection set meanwhile is not followed */
} View;

static View *ViewOf(HWND window)
{
    return window && IsClass(window, VIEW_CLASS) ? (View *)GetWindowLongPtrW(window, GWLP_USERDATA) : NULL;
}

/* The view a control is in, or NULL. */
static HWND ViewAround(HWND control)
{
    HWND parent = GetParent(control);
    return ViewOf(parent) ? parent : NULL;
}

/* A control in a view that scrolls it (not too tall for a view). */
static BOOL ViewScrollsControl(HWND control)
{
    View *viewState = ViewOf(ViewAround(control));
    return viewState && !viewState->native;
}

/* A list view's header, when it shows one (whether or not the window is on
 * screen yet), else NULL. */
static HWND ShownHeader(HWND list)
{
    HWND header = IsClass(list, WC_LISTVIEWW) ? ListView_GetHeader(list) : NULL;
    return header && (GetWindowLongW(header, GWL_STYLE) & WS_VISIBLE) ? header : NULL;
}

/* The height of a list view's header, 0 without one. */
static int HeaderHeight(HWND list)
{
    HWND header = ShownHeader(list);
    RECT r;
    return header && GetWindowRect(header, &r) ? r.bottom - r.top : 0;
}

/* The height of the control's own horizontal scroll bar, 0 when none shows. */
static int HorizontalBarHeight(HWND control)
{
    SCROLLBARINFO horizontal;
    ZeroMemory(&horizontal, sizeof horizontal);
    horizontal.cbSize = sizeof horizontal;
    if (!GetScrollBarInfo(control, OBJID_HSCROLL, &horizontal) || (horizontal.rgstate[0] & STATE_SYSTEM_INVISIBLE)) return 0;
    return horizontal.rcScrollBar.bottom - horizontal.rcScrollBar.top;
}

/* The height of all the control's rows, with a list view's header and the
 * horizontal scroll bar when either shows. */
static int ContentHeight(HWND control)
{
    int barHeight = HorizontalBarHeight(control);
    if (IsClass(control, WC_LISTBOXW))
        return barHeight + (int)SendMessageW(control, LB_GETCOUNT, 0, 0) * (int)SendMessageW(control, LB_GETITEMHEIGHT, 0, 0);
    if (IsClass(control, WC_LISTVIEWW)) {
        RECT first;
        int rowCount = ListView_GetItemCount(control);
        return barHeight + HeaderHeight(control) +
               (rowCount > 0 && ListView_GetItemRect(control, 0, &first, LVIR_BOUNDS) ? rowCount * (first.bottom - first.top) : 0);
    }
    if (IsClass(control, WC_TREEVIEWW)) {
        int unit = TreeView_GetItemHeight(control), total = 0;
        HTREEITEM item;
        for (item = TreeView_GetRoot(control); item; item = TreeView_GetNextVisible(control, item)) {
            TVITEMEXW itemHeight;
            ZeroMemory(&itemHeight, sizeof itemHeight);
            itemHeight.mask = TVIF_HANDLE | TVIF_INTEGRAL;
            itemHeight.hItem = item;
            total += (TreeView_GetItem(control, (TVITEMW *)&itemHeight) ? max(1, itemHeight.iIntegral) : 1) * unit;
        }
        return barHeight + total;
    }
    return 0;
}

/* A list view's header at the top of the view, whatever is scrolled under it;
 * quiet (SWP_NOREDRAW): moved without drawing, the caller draws. */
static void PlaceHeader(HWND view, UINT quiet)
{
    View *viewState = ViewOf(view);
    HWND header = viewState ? ShownHeader(viewState->control) : NULL;
    RECT r;
    if (!header || !GetWindowRect(header, &r)) return;
    MapWindowPoints(NULL, viewState->control, (POINT *)&r, 2);
    SetWindowPos(header, HWND_TOP, r.left, viewState->native ? 0 : viewState->pos, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | quiet);
}

static BOOL CachedProfileWidths(HWND list, int *profile, int *role, int *dataMinimum);

/* The least a list view's last column keeps: its title, and in the
 * profile list the data folder in every language (Theme_ProfileColumnWidths). */
static int LastColumnMinimum(HWND list, HWND header, int column)
{
    WCHAR title[128];
    HDITEMW item;
    RECT measured = { 0 };
    HDC dc;
    int minimum = 2 * ScaleForWindow(header, HEADER_TEXT_INSET_DIPS), profile, role, dataMinimum;
    ZeroMemory(&item, sizeof item);
    item.mask = HDI_TEXT;
    item.pszText = title;
    item.cchTextMax = ARRAYSIZE(title);
    title[0] = 0;
    if (Header_GetItem(header, column, &item) && title[0] && (dc = GetDC(header)) != NULL) {
        HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(header, WM_GETFONT, 0, 0));
        DrawTextW(dc, title, -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | Localize_ReadingFlags());
        SelectObject(dc, old);
        ReleaseDC(header, dc);
        minimum += measured.right;
    }
    if (CachedProfileWidths(list, &profile, &role, &dataMinimum)) minimum = max(minimum, dataMinimum);
    return minimum;
}

/* A list view's last column (in display order, `*last`) and the width it
 * takes (`*fit`): the width the others leave, `column` at `width` (-1: each
 * at its own), never less than its minimum nor `minimum`: past it, the list
 * scrolls sideways. FALSE when it keeps its width: the user sized it, or
 * drags its divider. */
static BOOL LastColumnFit(HWND list, int minimum, int column, int width, int *last, int *fit)
{
    HWND header = IsClass(list, WC_LISTVIEWW) ? ListView_GetHeader(list) : NULL;
    RECT client;
    int columnCount = header ? Header_GetItemCount(header) : 0, other, used = 0;
    if (columnCount < 1) return FALSE;
    *last = Header_OrderToIndex(header, columnCount - 1);
    if (*last < 0 || Theme_ColumnResizeIsManual(list, *last) || ColumnDragged(list, *last)) return FALSE;
    GetClientRect(list, &client);
    for (other = 0; other < columnCount; other++)
        if (other != *last) used += other == column ? width : ListView_GetColumnWidth(list, other);
    *fit = max(max(minimum, LastColumnMinimum(list, header, *last)), client.right - used);
    return TRUE;
}

/* The last column takes the width the others leave, as the view's scroll
 * bar comes or goes too (LastColumnFit). */
void Theme_FitLastColumn(HWND list, int minimum)
{
    int last, fit;
    if (LastColumnFit(list, minimum, -1, 0, &last, &fit)) Theme_SetColumnWidth(list, last, fit);
}

/* A list view's rows scrolled out above it, in px (0 for other controls). */
static int ScrolledRowsPx(HWND control)
{
    RECT first;
    if (!IsClass(control, WC_LISTVIEWW) || !ListView_GetItemRect(control, 0, &first, LVIR_BOUNDS)) return 0;
    return ListView_GetTopIndex(control) * (first.bottom - first.top);
}

/* The control as tall as its rows (at least the view) and as wide as the
 * view, and the scroll bar for the difference; or, too tall or wider than
 * the view, as large as the view, scrolling itself. The rows on top stay on
 * top when it changes from one to the other. */
static void ViewLayout(HWND view)
{
    View *viewState = ViewOf(view);
    SCROLLINFO scrollInfo;
    RECT rc;
    int content, carried = 0;
    BOOL native;
    if (!viewState || !viewState->control) return;
    GetClientRect(view, &rc);
    content = ContentHeight(viewState->control);
    if (viewState->scaleFrom > 0) viewState->pos = MulDiv(viewState->pos, content, viewState->scaleFrom);
    viewState->scaleFrom = 0;
    native = content > THEME_VIEW_MAX_PX || HorizontalBarHeight(viewState->control) > 0;
    if (viewState->native != native) {
        viewState->native = native;
        if (native) {
            /* Its own smooth wheel, a line as long as the view's; it gives
             * the wheel back to the view once the control fits one again. */
            SmoothScroll *viewScroll = SmoothScrollOf(view);
            SetSmoothScroll(viewState->control, viewScroll ? viewScroll->rowPx : 0);
            carried = viewState->pos;
        } else {
            viewState->pos = ScrolledRowsPx(viewState->control);
            if (viewState->pos && IsClass(viewState->control, WC_LISTVIEWW)) ListView_Scroll(viewState->control, 0, -viewState->pos);
        }
    }
    ZeroMemory(&scrollInfo, sizeof scrollInfo);
    scrollInfo.cbSize = sizeof scrollInfo;
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (viewState->native || content <= rc.bottom) {
        viewState->pos = 0;
    } else {
        scrollInfo.nMax = content - 1;
        scrollInfo.nPage = (UINT)rc.bottom;
        viewState->pos = min(viewState->pos, content - rc.bottom);
    }
    scrollInfo.nPos = viewState->pos;
    SetScrollInfo(view, SB_VERT, &scrollInfo, TRUE);   /* the scroll bar may come or go: the width changes */
    GetClientRect(view, &rc);
    SetWindowPos(viewState->control, NULL, 0, -viewState->pos, rc.right, viewState->native ? rc.bottom : max(content, rc.bottom),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (carried && IsClass(viewState->control, WC_LISTVIEWW)) ListView_Scroll(viewState->control, 0, carried);
    Theme_FitLastColumn(viewState->control, 0);
    PlaceHeader(view, 0);
}

static void ViewRemeasure(HWND view)
{
    View *viewState = ViewOf(view);
    if (!viewState || viewState->posted) return;
    viewState->posted = TRUE;
    PostMessageW(view, VIEW_MEASURE, 0, 0);
}

/* A tree or list view drawn off screen (its double-buffer style, which its
 * view gives it); a list box draws on screen. */
static BOOL PaintsOffScreen(HWND control)
{
    if (IsClass(control, WC_TREEVIEWW)) return (TreeView_GetExtendedStyle(control) & TVS_EX_DOUBLEBUFFER) != 0;
    if (IsClass(control, WC_LISTVIEWW)) return (ListView_GetExtendedListViewStyle(control) & LVS_EX_DOUBLEBUFFER) != 0;
    return FALSE;
}

static void ViewScrollTo(HWND view, int pos)
{
    View *viewState = ViewOf(view);
    SCROLLINFO scrollInfo;
    if (!viewState || viewState->native) return;
    ZeroMemory(&scrollInfo, sizeof scrollInfo);
    scrollInfo.cbSize = sizeof scrollInfo;
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE;
    if (!GetScrollInfo(view, SB_VERT, &scrollInfo)) return;
    pos = max(0, min(pos, scrollInfo.nMax - max(0, (int)scrollInfo.nPage - 1)));   /* no page: everything fits, nothing to scroll */
    if (pos == viewState->pos) return;
    viewState->pos = pos;
    scrollInfo.fMask = SIF_POS;
    scrollInfo.nPos = pos;
    SetScrollInfo(view, SB_VERT, &scrollInfo, TRUE);
    if (PaintsOffScreen(viewState->control)) {
        /* Moved without copying its pixels, its row under the mouse updated,
         * then the part that shows drawn whole in one pass off screen: the
         * screen shows the frame before or the next one, never a copy whose
         * uncovered strip waits for its rows. */
        RECT shown;
        SetWindowPos(viewState->control, NULL, 0, -pos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        PlaceHeader(view, SWP_NOREDRAW);
        HoverUnderMouse(viewState->control);
        GetClientRect(view, &shown);
        MapWindowPoints(view, viewState->control, (POINT *)&shown, 2);
        RedrawWindow(viewState->control, &shown, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        return;
    }
    SetWindowPos(viewState->control, NULL, 0, -pos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PlaceHeader(view, 0);
    UpdateWindow(viewState->control);
    HoverUnderMouse(viewState->control);
}

/* Where a WM_VSCROLL request (its wParam: a line of `linePx`, a page, the
 * thumb, an end) takes the vertical scroll bar of `window`, a window of ours
 * scrolled by the pixel: within its range, its position for any other request. */
int Theme_ScrollTarget(HWND window, WPARAM request, int linePx)
{
    SCROLLINFO scrollInfo;
    int target;
    ZeroMemory(&scrollInfo, sizeof scrollInfo);
    scrollInfo.cbSize = sizeof scrollInfo;
    scrollInfo.fMask = SIF_ALL;
    if (!GetScrollInfo(window, SB_VERT, &scrollInfo)) return 0;
    switch (LOWORD(request)) {
    case SB_LINEUP:        target = scrollInfo.nPos - linePx; break;
    case SB_LINEDOWN:      target = scrollInfo.nPos + linePx; break;
    case SB_PAGEUP:        target = scrollInfo.nPos - (int)scrollInfo.nPage; break;
    case SB_PAGEDOWN:      target = scrollInfo.nPos + (int)scrollInfo.nPage; break;
    case SB_THUMBTRACK:    target = scrollInfo.nTrackPos; break;
    case SB_THUMBPOSITION: target = HIWORD(request); break;
    case SB_TOP:           target = 0; break;
    case SB_BOTTOM:        target = scrollInfo.nMax; break;
    default:               return scrollInfo.nPos;
    }
    return max(0, min(target, scrollInfo.nMax - max(0, (int)scrollInfo.nPage - 1)));
}

/* WM_VSCROLL from the view's scroll bar, Ctrl and a navigation key, or the
 * smooth wheel's SB_THUMBPOSITION; a line is a wheel line (Theme_SetScrollRow),
 * else a row of the control. */
static void ViewScroll(HWND view, WPARAM wp)
{
    View *viewState = ViewOf(view);
    SmoothScroll *scroll = SmoothScrollOf(view);
    RECT client;
    if (!viewState) return;
    if ((LOWORD(wp) == SB_PAGEUP || LOWORD(wp) == SB_PAGEDOWN) && viewState->control && GetClientRect(view, &client)) {
        /* A page is what shows under a list view's header. */
        int page = max(1, client.bottom - HeaderHeight(viewState->control));
        ViewScrollTo(view, viewState->pos + (LOWORD(wp) == SB_PAGEDOWN ? page : -page));
        return;
    }
    ViewScrollTo(view, Theme_ScrollTarget(view, wp, scroll && scroll->rowPx > 0 ? scroll->rowPx : WheelRow(view, 1)));
}

/* Ctrl with a navigation key scrolls a tree without moving its selection,
 * as a tree that scrolls itself does; a list keeps those keys (they move its
 * focus). */
static BOOL ViewScrollKey(HWND view, HWND control, WPARAM key)
{
    View *viewState = ViewOf(view);
    WORD request;
    if (!viewState || viewState->native || GetKeyState(VK_CONTROL) >= 0 || !IsClass(control, WC_TREEVIEWW)) return FALSE;
    switch (key) {
    case VK_UP:    request = SB_LINEUP; break;
    case VK_DOWN:  request = SB_LINEDOWN; break;
    case VK_PRIOR: request = SB_PAGEUP; break;
    case VK_NEXT:  request = SB_PAGEDOWN; break;
    case VK_HOME:  request = SB_TOP; break;
    case VK_END:   request = SB_BOTTOM; break;
    default:       return FALSE;
    }
    ViewScroll(view, MAKEWPARAM(request, 0));
    return TRUE;
}

/* What the keyboard is on in the control: a row's index or a tree item. */
static LRESULT KeyboardRow(HWND control)
{
    if (IsClass(control, WC_LISTBOXW)) return SendMessageW(control, LB_GETCARETINDEX, 0, 0);
    if (IsClass(control, WC_LISTVIEWW)) return ListView_GetNextItem(control, -1, LVNI_FOCUSED);
    if (IsClass(control, WC_TREEVIEWW)) return (LRESULT)TreeView_GetSelection(control);
    return -1;
}

/* `row` of the control (an index, or a tree item), scrolled into sight
 * (under a list view's header): the control, as tall as its rows, does not
 * do it itself. */
static void ViewShowRow(HWND view, LRESULT row)
{
    View *viewState = ViewOf(view);
    RECT item = { 0 }, rc;
    HWND control;
    int top = 0;
    BOOL found = FALSE;
    if (!viewState || viewState->native) return;
    control = viewState->control;
    if (IsClass(control, WC_LISTBOXW)) {
        found = row >= 0 && SendMessageW(control, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&item) != LB_ERR;
    } else if (IsClass(control, WC_LISTVIEWW)) {
        found = row >= 0 && ListView_GetItemRect(control, (int)row, &item, LVIR_BOUNDS);
        top = HeaderHeight(control);
    } else if (IsClass(control, WC_TREEVIEWW)) {
        found = row && TreeView_GetItemRect(control, (HTREEITEM)row, &item, FALSE);
    }
    if (!found) return;
    GetClientRect(view, &rc);
    if (item.top < viewState->pos + top) ViewScrollTo(view, item.top - top);
    else if (item.bottom > viewState->pos + rc.bottom) ViewScrollTo(view, item.bottom - rc.bottom);
}

/* The next VIEW_MEASURE scrolls `row` into sight: the row asked for, or
 * REVEAL_KEYBOARD_ROW, the keyboard's row once the control is measured. */
static void RevealLater(HWND view, LRESULT row)
{
    View *viewState = ViewOf(view);
    if (!viewState) return;
    viewState->reveal = TRUE;
    viewState->revealRow = row;
    ViewRemeasure(view);
}

/* Page Down and Page Up select the farthest row that shows on one page (the
 * view's height) with the current one, as a control that scrolls itself
 * does: the control, as tall as all its rows, would take them all for a
 * page. One selection change, notified as the control notifies its own.
 * FALSE: a control whose selection works otherwise keeps the native keys. */
static BOOL ViewPage(HWND view, BOOL down)
{
    View *viewState = ViewOf(view);
    HWND control;
    RECT client, row;
    int page, direction = down ? 1 : -1;
    if (!viewState || viewState->native) return FALSE;
    control = viewState->control;
    GetClientRect(view, &client);
    page = max(1, client.bottom - HeaderHeight(control));
    if (IsClass(control, WC_LISTBOXW)) {
        LONG style = GetWindowLongW(control, GWL_STYLE);
        int count = (int)SendMessageW(control, LB_GETCOUNT, 0, 0), rowHeight = (int)SendMessageW(control, LB_GETITEMHEIGHT, 0, 0);
        int target = (int)max(0, SendMessageW(control, LB_GETCARETINDEX, 0, 0));
        if ((style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL | LBS_OWNERDRAWVARIABLE | LBS_WANTKEYBOARDINPUT)) || count <= 0 || rowHeight <= 0)
            return FALSE;
        target = max(0, min(count - 1, target + direction * max(1, page / rowHeight - 1)));
        if (target != (int)SendMessageW(control, LB_GETCURSEL, 0, 0)) {
            SendMessageW(control, LB_SETCURSEL, (WPARAM)target, 0);
            if (style & LBS_NOTIFY)
                SendMessageW(GetParent(control), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(control), LBN_SELCHANGE), (LPARAM)control);
        }
        return TRUE;
    }
    if (IsClass(control, WC_LISTVIEWW)) {
        int count = ListView_GetItemCount(control), focused = ListView_GetNextItem(control, -1, LVNI_FOCUSED), rowHeight, target;
        if ((GetWindowLongW(control, GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT || count <= 0 ||
            !ListView_GetItemRect(control, 0, &row, LVIR_BOUNDS) || (rowHeight = row.bottom - row.top) <= 0) return FALSE;
        target = max(0, min(count - 1, max(0, focused) + direction * max(1, page / rowHeight - 1)));
        if (target != focused || !ListView_GetItemState(control, target, LVIS_SELECTED)) {
            ListView_SetItemState(control, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(control, target, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_SetSelectionMark(control, target);
        }
        return TRUE;
    }
    if (IsClass(control, WC_TREEVIEWW)) {
        HTREEITEM start = TreeView_GetSelection(control), target, next;
        RECT first;
        if (!start) start = TreeView_GetRoot(control);
        if (!start || !TreeView_GetItemRect(control, start, &first, FALSE)) return FALSE;
        for (target = start; ; target = next) {
            next = down ? TreeView_GetNextVisible(control, target) : TreeView_GetPrevVisible(control, target);
            if (!next || !TreeView_GetItemRect(control, next, &row, FALSE)) break;
            if (target != start && (down ? row.bottom - first.top : first.bottom - row.top) > page) break;
        }
        if (target != TreeView_GetSelection(control)) TreeView_SelectItem(control, target);
        return TRUE;
    }
    return FALSE;
}

/* What the view does with `msg` before its control sees it: the wheel is
 * the view's (not a control too tall for it), keys that scroll or page
 * through it, and the scroll position a new font or row height keeps.
 * TRUE when the view handled `msg` (`*result`). */
static BOOL ViewHandlesFirst(HWND view, HWND control, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    View *viewState = ViewOf(view);
    if (msg == WM_MOUSEWHEEL && viewState && !viewState->native) {
        *result = SendMessageW(view, msg, wp, lp);
        return TRUE;
    }
    if (StopsWheel(msg)) StopSmoothScroll(view);
    /* A new font or row height: the rows on top stay there (ViewLayout). */
    if ((msg == WM_SETFONT || msg == LB_SETITEMHEIGHT || msg == TVM_SETITEMHEIGHT) && viewState && !viewState->scaleFrom)
        viewState->scaleFrom = ContentHeight(control);
    *result = 0;
    if (msg == WM_KEYDOWN && ViewScrollKey(view, control, wp)) return TRUE;
    if (msg == WM_KEYDOWN && (wp == VK_NEXT || wp == VK_PRIOR) && GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_CONTROL) >= 0 &&
        ViewPage(view, wp == VK_NEXT)) {
        ViewShowRow(view, KeyboardRow(control));
        return TRUE;
    }
    return FALSE;
}

/* What the view follows once its control has handled `msg` (the keyboard's
 * row before it: `rowBefore`). */
static void ViewFollows(HWND view, HWND control, UINT msg, WPARAM wp, LPARAM lp, LRESULT rowBefore)
{
    View *viewState = ViewOf(view);   /* what the parent was told may have destroyed the view */
    switch (msg) {
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_DELETESTRING:
    case LB_RESETCONTENT:
    case LB_SETITEMHEIGHT:
    case LVM_INSERTITEMW:
    case LVM_DELETEITEM:
    case LVM_DELETEALLITEMS:
    case TVM_DELETEITEM:
        /* A tree item asked to be shown may be gone. */
        if (viewState && viewState->reveal && viewState->revealRow != REVEAL_KEYBOARD_ROW) viewState->reveal = FALSE;
        ViewRemeasure(view);
        break;
    case TVM_INSERTITEMW:
    case TVM_EXPAND:
    case TVM_SETITEMW:
    case TVM_SETITEMHEIGHT:
    case WM_SETFONT:
        ViewRemeasure(view);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        /* A click on a row the keyboard was not on brings that row whole
         * into sight; a folder's arrow changes the rows. */
        if (msg == WM_LBUTTONDOWN && KeyboardRow(control) != rowBefore) RevealLater(view, REVEAL_KEYBOARD_ROW);
        else if (IsClass(control, WC_TREEVIEWW)) ViewRemeasure(view);
        break;
    case WM_KEYDOWN:
    case WM_CHAR:   /* type-ahead */
        /* Only a key that moved the keyboard's row brings it into sight (a
         * modifier pressed before a click does not); in a tree, the arrows
         * also open and close folders. */
        if (KeyboardRow(control) != rowBefore) {
            RevealLater(view, REVEAL_KEYBOARD_ROW);
            if (msg == WM_KEYDOWN) ViewShowRow(view, KeyboardRow(control));
        } else if (IsClass(control, WC_TREEVIEWW)) {
            ViewRemeasure(view);
        }
        break;
    case LB_SETCARETINDEX:
    case LVM_ENSUREVISIBLE:
        RevealLater(view, (LRESULT)wp);
        break;
    case TVM_ENSUREVISIBLE:
        RevealLater(view, lp);
        break;
    case LB_SETCURSEL:
    case TVM_SELECTITEM:
        if (viewState && !viewState->refilling && (msg == LB_SETCURSEL || wp == TVGN_CARET)) RevealLater(view, REVEAL_KEYBOARD_ROW);
        break;
    case WM_SETREDRAW:
        if (viewState) viewState->refilling = !wp;
        /* Refilled: its full height at once, before it shows a scroll bar
         * of its own for its current rows. A control too tall for a view
         * redraws once per wheel frame, its rows unchanged. */
        if (wp && !ScrollingItself(control)) ViewLayout(view);
        break;
    case WM_SIZE:
        /* Its own scroll bar gone: the last column takes the room, and
         * nothing of the bar stays drawn. */
        PlaceHeader(view, 0);
        InvalidateRect(control, NULL, FALSE);
        /* Native layout must finish before a column width is written. */
        if (viewState) ViewRemeasure(view);
        break;
    case WM_STYLECHANGED:
        if (wp == GWL_STYLE && (((const STYLESTRUCT *)lp)->styleOld ^ ((const STYLESTRUCT *)lp)->styleNew) & WS_HSCROLL)
            ViewRemeasure(view);
        break;
    }
}

/* The control in a view: its wheel is the view's; what changes its rows
 * (items, heights, a folder opened or closed by the mouse or the keyboard)
 * has the view measure it again, once. The view follows the keyboard's row
 * when the keyboard or the program moves it (an "ensure visible" request,
 * a selection set while the control draws), not when rows change around it:
 * a refill (drawing off, its selection included), a folder opened by its
 * arrow or a new size keep the scroll position. */
static LRESULT CALLBACK ViewedSubclass(HWND control, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    HWND view = (HWND)ref;
    LRESULT result, rowBefore = 0;
    if (msg == WM_NCDESTROY) {
        View *viewState = ViewOf(view);
        if (viewState) viewState->control = NULL;
        RemoveWindowSubclass(control, ViewedSubclass, id);
        return DefSubclassProc(control, msg, wp, lp);
    }
    if (ViewHandlesFirst(view, control, msg, wp, lp, &result)) return result;
    if (msg == WM_LBUTTONDOWN || msg == WM_KEYDOWN || msg == WM_CHAR) rowBefore = KeyboardRow(control);
    result = DefSubclassProc(control, msg, wp, lp);
    ViewFollows(view, control, msg, wp, lp, rowBefore);
    return result;
}

static LRESULT CALLBACK ViewProc(HWND view, UINT msg, WPARAM wp, LPARAM lp)
{
    View *viewState = (View *)GetWindowLongPtrW(view, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(view, GWLP_USERDATA, (LONG_PTR)((const CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_SIZE:
        ViewLayout(view);
        return 0;
    case VIEW_MEASURE:
        if (!viewState) return 0;
        viewState->posted = FALSE;
        ViewLayout(view);
        if (viewState->reveal) {
            viewState->reveal = FALSE;
            ViewShowRow(view, viewState->revealRow == REVEAL_KEYBOARD_ROW ? KeyboardRow(viewState->control) : viewState->revealRow);
        }
        return 0;
    case WM_VSCROLL:
        ViewScroll(view, wp);
        return 0;
    case WM_SETFOCUS:
        if (viewState && viewState->control) SetFocus(viewState->control);   /* the dialog gives it the focus by its id */
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* the control covers it */
    case WM_NOTIFY:
    case WM_COMMAND:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_COMPAREITEM:
    case WM_DELETEITEM:
    case WM_VKEYTOITEM:
    case WM_CHARTOITEM:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSCROLLBAR:
        return SendMessageW(GetParent(view), msg, wp, lp);   /* what the control tells its parent is for the dialog */
    case WM_NCDESTROY:
        if (viewState) HeapFree(GetProcessHeap(), 0, viewState);
        SetWindowLongPtrW(view, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(view, msg, wp, lp);
}

/* A wheel line in `window`: a row of the control in a view, else `unit`. */
static int WheelRow(HWND window, int unit)
{
    View *viewState = ViewOf(window);
    return viewState && viewState->control ? max(1, ScrollUnit(viewState->control)) : unit;
}

/* The scroll bars the control showed (`style`) that its ranges still need
 * once it is in its view: one too tall for a view scrolls itself, a list
 * view can be wider than it. Taking the styles off hid the bars and kept
 * their ranges, and a control shows a bar only when its range starts to
 * need one. */
static void ShowNeededScrollBars(HWND control, LONG style)
{
    static const int kScrollBars[] = { SB_VERT, SB_HORZ };
    static const LONG kScrollStyles[] = { WS_VSCROLL, WS_HSCROLL };
    SCROLLINFO scrollInfo;
    int i;
    for (i = 0; i < (int)ARRAYSIZE(kScrollBars); i++) {
        ZeroMemory(&scrollInfo, sizeof scrollInfo);
        scrollInfo.cbSize = sizeof scrollInfo;
        scrollInfo.fMask = SIF_RANGE | SIF_PAGE;
        if ((style & kScrollStyles[i]) && GetScrollInfo(control, kScrollBars[i], &scrollInfo) &&
            (scrollInfo.nPage > 0 ? (int)scrollInfo.nPage <= scrollInfo.nMax - scrollInfo.nMin : scrollInfo.nMax > scrollInfo.nMin))
            ShowScrollBar(control, kScrollBars[i], TRUE);
    }
}

HWND Theme_SmoothView(HWND control)
{
    static BOOL registered;
    HWND parent = GetParent(control), view, around;
    LONG style = GetWindowLongW(control, GWL_STYLE), extendedStyle = GetWindowLongW(control, GWL_EXSTYLE);
    INT_PTR made;
    View *viewState;
    RECT rc;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = VIEW_CLASS;
        registered = RegisterClassW(&wc) != 0;
        if (!registered) Util_Log(L"theme: the scrolling view's class cannot be registered (error %lu)", GetLastError());
    }
    if (ViewOf(control)) return control;   /* a view already */
    around = ViewAround(control);
    if (!registered || !parent || around) return around ? around : control;
    if ((viewState = (View *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *viewState)) == NULL) return control;
    /* The frame the control was made with, which a dark theme may have taken off already. */
    made = RecordedFrameFlags(control);
    GetWindowRect(control, &rc);
    MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
    view = CreateWindowExW(WS_EX_CONTROLPARENT | ((made & MADE_EDGE) ? WS_EX_CLIENTEDGE : 0), VIEW_CLASS, L"",
                           WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN | (style & WS_VISIBLE) | ((made & MADE_BORDER) ? WS_BORDER : 0),
                           rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, (HMENU)(INT_PTR)GetDlgCtrlID(control), g_hInst,
                           viewState);
    if (!view) {
        Util_Log(L"theme: a scrolling view cannot be made (error %lu)", GetLastError());
        HeapFree(GetProcessHeap(), 0, viewState);
        return control;
    }
    viewState->control = control;
    SetWindowPos(view, control, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* its place in the tab order */
    /* The view owns the frame; the control, as tall as its rows, needs no scroll bar. */
    SetWindowLongW(control, GWL_STYLE, (style & ~(WS_BORDER | WS_VSCROLL | WS_HSCROLL)) | WS_VISIBLE |
                                           (IsClass(control, WC_LISTVIEWW) ? WS_CLIPCHILDREN : 0));   /* rows scroll under the header */
    SetWindowLongW(control, GWL_EXSTYLE, extendedStyle & ~WS_EX_CLIENTEDGE);
    /* A tree or list view draws off screen: each frame of its scrolling shows whole (ViewScrollTo). */
    if (IsClass(control, WC_TREEVIEWW)) TreeView_SetExtendedStyle(control, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    else if (IsClass(control, WC_LISTVIEWW)) ListView_SetExtendedListViewStyleEx(control, LVS_EX_DOUBLEBUFFER, LVS_EX_DOUBLEBUFFER);
    SetPropW(control, BORDER_PROP, (HANDLE)(INT_PTR)MADE_RECORDED);
    /* The view scrolls, with the control's frame, whether or not its scroll
     * bar shows when it is themed. */
    SetPropW(view, BORDER_PROP, (HANDLE)(MADE_RECORDED | MADE_SCROLLS | (made & (MADE_BORDER | MADE_EDGE))));
    FitFrame(view, made);   /* none in dark mode */
    SmoothScrolling(view);
    SetParent(control, view);
    SetWindowPos(control, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowSubclass(control, ViewedSubclass, VIEW_SUBCLASS, (DWORD_PTR)view);
    ViewLayout(view);
    ShowNeededScrollBars(control, style);
    return view;
}

/* --------------------------------------------------------------- dialogs */

/* Windows 11's link blue on a dark background (the system's is for light ones). */
#define DARK_LINK RGB(0x60, 0xCD, 0xFF)

/* A link control's text (LWS_USECUSTOMTEXT) in the colors the dialog gives
 * its statics, its links in the link blue. */
static void LinkColors(HWND dialog, const NMCUSTOMTEXT *customText)
{
    SetTextColor(customText->hDC, g_palette.color[THEME_TEXT]);   /* a parent that gives none */
    SendMessageW(dialog, WM_CTLCOLORSTATIC, (WPARAM)customText->hDC, (LPARAM)customText->hdr.hwndFrom);
    if (customText->fLink) SetTextColor(customText->hDC, g_dark ? DARK_LINK : GetSysColor(COLOR_HOTLIGHT));
}

/* What the theme keeps on a dialog goes with it. */
static void ForgetDialog(HWND dialog)
{
    DialogBase *base = (DialogBase *)RemovePropW(dialog, DIALOG_BASE_PROP);
    void *mainBudget = RemovePropW(dialog, MAIN_BUDGET_PROP);
    HFONT font = (HFONT)RemovePropW(dialog, DIALOG_FONT_PROP);
    RemovePropW(dialog, THEME_PROP);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    if (mainBudget) HeapFree(GetProcessHeap(), 0, mainBudget);
    if (font) DeleteObject(font);
}

/* Every themed dialog, whatever dialog it is: in dark mode its push buttons
 * and check boxes are drawn here, and in both modes its list view rows and
 * its drop-down lists; it answers WM_GETFONT with the font ApplyDialogFont
 * made, and frees what the theme keeps on it. */
static LRESULT CALLBACK DialogSubclass(HWND dialog, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if ((msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || (msg == WM_ENABLE && !wp) ||
        (msg == WM_SHOWWINDOW && !wp) || msg == WM_CANCELMODE || msg == WM_ENTERMENULOOP || msg == WM_ENTERSIZEMOVE)
        EnumChildWindows(dialog, HideDialogTip, 0);
    if (msg == WM_GETFONT) {
        HFONT font = (HFONT)GetPropW(dialog, DIALOG_FONT_PROP);
        if (font) return (LRESULT)font;
    }
    if (msg == WM_DRAWITEM && lp) {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lp;
        /* The closed box only: its native list never opens (QueueChoice). */
        if (item->CtlType == ODT_COMBOBOX && IsDropDownList(item->hwndItem)) {
            if (item->itemState & ODS_COMBOBOXEDIT) PaintDropDownList(item->hwndItem, item->hDC);
            return TRUE;
        }
    }
    if (msg == WM_NOTIFY && ((const NMHDR *)lp)->code == NM_CUSTOMTEXT && IsClass(((const NMHDR *)lp)->hwndFrom, WC_LINK)) {
        LinkColors(dialog, (const NMCUSTOMTEXT *)lp);
        return 0;
    }
    if (msg == WM_NOTIFY && ((const NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        HWND from = ((const NMHDR *)lp)->hwndFrom;
        if (g_dark && IsPushButton(from)) return ButtonCustomDraw((const NMCUSTOMDRAW *)lp);
        if (g_dark && IsCheckBox(from)) return CheckBoxCustomDraw((const NMCUSTOMDRAW *)lp);
        if (IsClass(from, WC_LISTVIEWW)) return ListCustomDraw((NMLVCUSTOMDRAW *)lp);
    } else if (msg == WM_NCDESTROY) {
        ForgetDialog(dialog);
        RemoveWindowSubclass(dialog, DialogSubclass, id);
    }
    return DefSubclassProc(dialog, msg, wp, lp);
}

static BOOL CALLBACK SetDialogFont(HWND child, LPARAM font)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)font, FALSE);
    if (GetPropW(child, STRONG_PROP) && (HFONT)SendMessageW(child, WM_GETFONT, 0, 0) == (HFONT)font)
        Theme_SetStrong(child);
    return TRUE;
}

/* The dialog's resource font and layout, read once. DialogSubclass, which
 * frees the theme's state with the dialog, comes with the first of it. */
static DialogBase *DialogBaseline(HWND dialog)
{
    DialogBase *base = (DialogBase *)GetPropW(dialog, DIALOG_BASE_PROP);
    HFONT font;
    if (base) return base;
    if (!SetWindowSubclass(dialog, DialogSubclass, DIALOG_SUBCLASS, 0)) return NULL;
    base = (DialogBase *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *base);
    if (!base) return NULL;
    base->fontDpi = GetDpiForWindow(dialog);
    if (!base->fontDpi) base->fontDpi = 96;
    font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    if (!font || !GetObjectW(font, sizeof base->font, &base->font)) DefaultDialogFont(&base->font, base->fontDpi);
    if (!SetPropW(dialog, DIALOG_BASE_PROP, base)) {
        HeapFree(GetProcessHeap(), 0, base);
        return NULL;
    }
    return base;
}

/* Every locale uses the same resource size and weight at the current DPI,
 * in its script's face. DialogSubclass answers WM_GETFONT with it. */
static void ApplyDialogFont(HWND dialog)
{
    DialogBase *base = DialogBaseline(dialog);
    HFONT owned = (HFONT)GetPropW(dialog, DIALOG_FONT_PROP), made;
    LOGFONTW lf, current;
    UINT dpi = GetDpiForWindow(dialog);
    if (!base) return;
    if (!dpi) dpi = base->fontDpi;
    lf = base->font;
    lf.lfHeight = MulDiv(lf.lfHeight, (int)dpi, (int)base->fontDpi);
    lf.lfWidth = MulDiv(lf.lfWidth, (int)dpi, (int)base->fontDpi);
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), Localize_FontFace());
    lf.lfQuality = CLEARTYPE_QUALITY;
    ZeroMemory(&current, sizeof current);
    if (owned && GetObjectW(owned, sizeof current, &current) && memcmp(&lf, &current, sizeof lf) == 0) return;
    made = CreateFontIndirectW(&lf);
    if (!made) return;
    if (!SetPropW(dialog, DIALOG_FONT_PROP, made)) {
        DeleteObject(made);
        return;
    }
    SendMessageW(dialog, WM_SETFONT, (WPARAM)made, FALSE);
    EnumChildWindows(dialog, SetDialogFont, (LPARAM)made);
    if (owned) DeleteObject(owned);
}

/* Capture direct viewports, rather than their scrolling contents, before
 * translated text changes the resource geometry. */
void Theme_RememberLayout(HWND dialog)
{
    DialogBase *base = DialogBaseline(dialog);
    RECT client;
    HWND child;
    if (!base || base->hasLayout || !GetClientRect(dialog, &client)) return;
    base->layoutDpi = GetDpiForWindow(dialog);
    if (!base->layoutDpi) base->layoutDpi = 96;
    base->client.cx = client.right;
    base->client.cy = client.bottom;
    for (child = GetWindow(dialog, GW_CHILD); child && base->count < (int)ARRAYSIZE(base->controls);
         child = GetWindow(child, GW_HWNDNEXT)) {
        DialogControlBase *control = &base->controls[base->count];
        if (!GetWindowRect(child, &control->rectangle)) continue;
        MapWindowPoints(NULL, dialog, (POINT *)&control->rectangle, 2);
        control->window = child;
        base->count++;
    }
    base->hasLayout = TRUE;
}

/* A control's resource rectangle at `dpi` (its current one when the layout
 * was not captured); an empty one when it has neither. */
static BOOL LayoutSourceRect(HWND dialog, const DialogBase *base, HWND child, UINT dpi, RECT *out)
{
    int i;
    SetRectEmpty(out);
    if (base && base->hasLayout) for (i = 0; i < base->count; i++) {
        const DialogControlBase *control = &base->controls[i];
        if (control->window != child) continue;
        out->left = MulDiv(control->rectangle.left, (int)dpi, (int)base->layoutDpi);
        out->top = MulDiv(control->rectangle.top, (int)dpi, (int)base->layoutDpi);
        out->right = MulDiv(control->rectangle.right, (int)dpi, (int)base->layoutDpi);
        out->bottom = MulDiv(control->rectangle.bottom, (int)dpi, (int)base->layoutDpi);
        return TRUE;
    }
    if (!child || !GetWindowRect(child, out)) return FALSE;
    MapWindowPoints(NULL, dialog, (POINT *)out, 2);
    return TRUE;
}

/* A caption as the catalogs know it: its English key, with the user value
 * that fills the key's one %s, if any. User text has no key. */
typedef struct LayoutCaption {
    const WCHAR *key;
    WCHAR text[2048], value[2048];
    BOOL hasValue;
} LayoutCaption;

static BOOL MatchLayoutValue(const WCHAR *text, const WCHAR *format, WCHAR *value, size_t cch)
{
    const WCHAR *slot = wcsstr(format, L"%s");
    size_t prefix, suffix, length = wcslen(text);
    if (!slot || wcschr(format, L'%') != slot || wcschr(slot + 2, L'%')) return FALSE;
    prefix = (size_t)(slot - format);
    suffix = wcslen(slot + 2);
    if (length < prefix + suffix || wcsncmp(text, format, prefix) != 0 || wcscmp(text + length - suffix, slot + 2) != 0) return FALSE;
    return SUCCEEDED(StringCchCopyNW(value, cch, text + prefix, length - prefix - suffix));
}

/* The control's caption, found in the catalog of the current language (as
 * it is or with its one value), so that every language's text can be
 * measured; the value the user gave stays. */
static void ReadLayoutCaption(HWND child, LayoutCaption *caption)
{
    size_t i;
    int language = Localize_EffectiveLanguage();
    ZeroMemory(caption, sizeof *caption);
    GetWindowTextW(child, caption->text, ARRAYSIZE(caption->text));
    if (!caption->text[0]) return;   /* a list, an edit: no caption to find */
    for (i = 0; i < Localize_CatalogCount(); i++) {
        const WCHAR *key = Localize_CatalogKey(i), *translated = Localize_TranslateAt(language, key);
        if (wcscmp(caption->text, translated) == 0) { caption->key = key; return; }
        if (MatchLayoutValue(caption->text, translated, caption->value, ARRAYSIZE(caption->value))) {
            caption->key = key;
            caption->hasValue = TRUE;
            return;
        }
    }
}

static void LayoutCaptionAt(const LayoutCaption *caption, int language, WCHAR *out, size_t cch)
{
    const WCHAR *format = caption->key ? Localize_TranslateAt(language, caption->key) : caption->text;
    if (caption->hasValue) StringCchPrintfW(out, cch, format, caption->value);
    else StringCchCopyW(out, cch, format);
}

/* The English key whose translation in the current language is `shown`,
 * else NULL. */
static const WCHAR *CatalogKeyFor(const WCHAR *shown)
{
    size_t i;
    for (i = 0; i < Localize_CatalogCount(); i++) {
        const WCHAR *english = Localize_CatalogKey(i);
        if (wcscmp(shown, Localize_Text(english)) == 0) return english;
    }
    return NULL;
}

/* The control's font in `language`'s face; a semibold control's as
 * StrongOf makes it. */
static HFONT LayoutFontAt(HWND control, int language)
{
    LOGFONTW font;
    if (!GetObjectW((HFONT)SendMessageW(control, WM_GETFONT, 0, 0), sizeof font, &font)) return NULL;
    StringCchCopyW(font.lfFaceName, ARRAYSIZE(font.lfFaceName),
                   GetPropW(control, STRONG_PROP) ? StrongFace(Localize_FontFaceAt(language)) : Localize_FontFaceAt(language));
    return CreateFontIndirectW(&font);
}

/* What a check box needs beyond its caption as measured here (`*space`: its
 * glyph and the gap after it) and its height, the largest of every interface
 * language, each caption in its script's font: the same whatever language
 * shows. A dark check box is drawn here, its glyph and gap the same in every
 * font. A light one is the native control's: a hidden copy of it gives its
 * ideal size (BCM_GETIDEALSIZE) in each language, as its text measure can be
 * a pixel wider than this one's in some fonts. */
static void CheckBoxNeeds(HWND control, int *space, int *height)
{
    SIZE ideal;
    LayoutCaption caption;
    HWND copy;
    HDC dc;
    int language;
    *space = *height = 0;
    if (!Theme_CheckBoxSize(control, &ideal)) return;
    *space = max(0, ideal.cx - CaptionSize(control).cx);
    *height = ideal.cy;
    if (g_dark) return;
    ReadLayoutCaption(control, &caption);
    copy = CreateWindowExW(WS_EX_NOPARENTNOTIFY, WC_BUTTONW, L"", (GetWindowLongW(control, GWL_STYLE) & ~WS_VISIBLE) | WS_CHILD,
                           0, 0, 0, 0, GetParent(control), NULL, g_hInst, NULL);
    if (!copy) return;
    if ((dc = GetDC(control)) != NULL) {
        for (language = 0; language < Localize_LanguageCount(); language++) {
            WCHAR text[512];
            HFONT font = LayoutFontAt(control, language);
            RECT measured = { 0, 0, 0, 0 };
            SIZE need = { 0, 0 };
            HGDIOBJ old;
            if (!font) continue;
            LayoutCaptionAt(&caption, language, text, ARRAYSIZE(text));
            SendMessageW(copy, WM_SETFONT, (WPARAM)font, FALSE);
            SetWindowTextW(copy, text);
            old = SelectObject(dc, font);
            DrawTextW(dc, text, -1, &measured, DT_CALCRECT | DT_SINGLELINE | ReadingFlagsAt(language));
            SelectObject(dc, old);
            if (SendMessageW(copy, BCM_GETIDEALSIZE, 0, (LPARAM)&need)) {
                if (text[0]) need.cx++;   /* as Theme_CheckBoxSize counts it */
                *space = max(*space, need.cx - measured.right);
                *height = max(*height, need.cy);
            }
            SendMessageW(copy, WM_SETFONT, 0, FALSE);
            DeleteObject(font);
        }
        ReleaseDC(control, dc);
    }
    DestroyWindow(copy);
}

/* The widest and tallest of `keys` in every interface language, each in
 * `control`'s font for its script (a text that is no key is measured as it
 * is), with DrawText's `format`: `value` fills a translation's one %s when
 * given; `width` is the width wrapped text wraps at. `tallestFont`, when
 * given, grows to the tallest of those fonts. */
static SIZE MeasureEveryLanguage(HWND control, const WCHAR *const *keys, int keyCount, const WCHAR *value, UINT format, int width,
                                 int *tallestFont)
{
    SIZE largest = { 0, 0 };
    HDC dc = GetDC(control);
    int language, key;
    if (!dc) return largest;
    for (language = 0; language < Localize_LanguageCount(); language++) {
        HFONT font = LayoutFontAt(control, language);
        HGDIOBJ old;
        if (!font) continue;
        old = SelectObject(dc, font);
        for (key = 0; key < keyCount; key++) {
            WCHAR text[2048];
            RECT measured = { 0, 0, (format & DT_WORDBREAK) ? max(1, width) : 0, 0 };
            const WCHAR *translated = Localize_TranslateAt(language, keys[key]);
            if (value) StringCchPrintfW(text, ARRAYSIZE(text), translated, value);
            else StringCchCopyW(text, ARRAYSIZE(text), translated);
            DrawTextW(dc, text, -1, &measured, DT_CALCRECT | format | ReadingFlagsAt(language));
            largest.cx = max(largest.cx, measured.right);
            largest.cy = max(largest.cy, measured.bottom);
        }
        if (tallestFont) {
            TEXTMETRICW metrics;
            if (GetTextMetricsW(dc, &metrics)) *tallestFont = max(*tallestFont, metrics.tmHeight);
        }
        SelectObject(dc, old);
        DeleteObject(font);
    }
    ReleaseDC(control, dc);
    return largest;
}

static const WCHAR *const kProfileColumnTitles[] = { L"Profile", L"Role", L"Data folder" };
/* The role column's values: the profile the regular Claude icon opens, the default one, or both. */
static const WCHAR *const kProfileRoles[] = { L"Claude icon, default", L"Claude icon", L"Default" };

const WCHAR *Theme_ProfileColumnTitle(int column)
{
    return column >= 0 && column < (int)ARRAYSIZE(kProfileColumnTitles) ? kProfileColumnTitles[column] : NULL;
}

const WCHAR *Theme_ProfileRole(BOOL stock, BOOL isDefault)
{
    if (stock) return isDefault ? kProfileRoles[0] : kProfileRoles[1];
    return isDefault ? kProfileRoles[2] : L"";
}

#define PROFILE_COLUMN_PADDING_DIPS 16   /* around a profile column's widest text */

void Theme_ProfileColumnWidths(HWND list, int *profile, int *role, int *dataMinimum)
{
    static const WCHAR *const kStockFolder[] = { L"%APPDATA%\\" STOCK_FOLDER };
    HWND header = ListView_GetHeader(list);
    HWND titles = header && SendMessageW(header, WM_GETFONT, 0, 0) ? header : list;
    UINT format = DT_SINGLELINE | DT_NOPREFIX;
    int padding = ScaleForWindow(list, PROFILE_COLUMN_PADDING_DIPS);
    int dataColumnMinimum;
    if (CachedProfileWidths(list, profile, role, &dataColumnMinimum)) {
        if (dataMinimum) *dataMinimum = dataColumnMinimum;
        return;
    }
    *profile = ScaleForWindow(list, THEME_PROFILE_COLUMN_DIPS);
    *role = max(MeasureEveryLanguage(titles, &kProfileColumnTitles[1], 1, NULL, format, 0, NULL).cx,
                MeasureEveryLanguage(list, kProfileRoles, ARRAYSIZE(kProfileRoles), NULL, format, 0, NULL).cx) + padding;
    if (dataMinimum)
        *dataMinimum = max(MeasureEveryLanguage(titles, &kProfileColumnTitles[2], 1, NULL, format, 0, NULL).cx,
                           MeasureEveryLanguage(list, kStockFolder, ARRAYSIZE(kStockFolder), NULL, format, 0, NULL).cx) + padding;
}

/* SysLink's native line layout includes its link runs and their wrapping. */
static int LayoutLinkHeight(HWND link, HFONT font, const WCHAR *text, int width)
{
    HWND measure = CreateWindowExW(WS_EX_NOPARENTNOTIFY, WC_LINK, text, WS_CHILD,
                                   0, 0, width, 1, GetParent(link), NULL, g_hInst, NULL);
    SIZE ideal = { 0, 0 };
    if (!measure) return 0;
    SendMessageW(measure, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(measure, LM_GETIDEALSIZE, max(1, width), (LPARAM)&ideal);
    DestroyWindow(measure);
    return ideal.cy;
}

#define BUTTON_PADDING_DIPS     20   /* around a button's caption */
#define BUTTON_TEXT_MARGIN_DIPS 6    /* above and below a control's text */
#define LABEL_SLACK_PX          2    /* rasterized text can reach a pixel past its font's cell, above and below */

/* Each of a drop-down list's `count` choices by its catalog key (NULL for
 * one that is none), found once for every language; NULL without memory
 * for them (the choices are then measured as they show). The caller frees
 * the array. */
static const WCHAR **ChoiceKeys(HWND combo, int count)
{
    const WCHAR **keys;
    int choice;
    if (count <= 0) return NULL;
    keys = (const WCHAR **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *keys);
    if (keys) for (choice = 0; choice < count; choice++) {
        WCHAR local[256], *text = ComboItemText(combo, choice, local, ARRAYSIZE(local));
        keys[choice] = CatalogKeyFor(text);
        if (text != local) HeapFree(GetProcessHeap(), 0, text);
    }
    return keys;
}

/* The widest of a drop-down list's `count` choices in `language`, on `dc`
 * (that language's font selected). */
static int WidestChoice(HDC dc, HWND combo, int count, const WCHAR **keys, int language)
{
    int choice, widest = 0;
    for (choice = 0; choice < count; choice++) {
        WCHAR local[256], *shown = NULL;
        const WCHAR *text = keys && keys[choice] ? Localize_TranslateAt(language, keys[choice])
                                                 : (shown = ComboItemText(combo, choice, local, ARRAYSIZE(local)));
        RECT extent = { 0, 0, 0, 0 };
        DrawTextW(dc, text, -1, &extent, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | ReadingFlagsAt(language));
        widest = max(widest, extent.right);
        if (shown && shown != local) HeapFree(GetProcessHeap(), 0, shown);
    }
    return widest;
}

/* One common text budget is the maximum over every interface language's
 * text in its script's font. Measuring never changes the selected language
 * or a visible control. A button's ampersand marks its access key; other
 * controls show it. */
static void LayoutTextBudget(HWND child, int width, BOOL wrapped, int *textWidth, int *height, int *fontHeight)
{
    LayoutCaption caption;
    HDC dc = GetDC(child);
    LONG style = GetWindowLongW(child, GWL_STYLE);
    BOOL button = IsClass(child, WC_BUTTONW), label = IsClass(child, WC_STATICW), combo = IsDropDownList(child);
    UINT prefix = button || (label && !(style & SS_NOPREFIX)) ? 0 : DT_NOPREFIX;
    UINT editControl = label && (style & SS_EDITCONTROL) ? DT_EDITCONTROL : 0, tabs = label ? DT_EXPANDTABS : 0;
    const WCHAR **choiceKeys;
    int language, choices = combo ? (int)SendMessageW(child, CB_GETCOUNT, 0, 0) : 0;
    *textWidth = 0;
    *height = 0;
    *fontHeight = 0;
    if (!dc) return;
    ReadLayoutCaption(child, &caption);
    choiceKeys = ChoiceKeys(child, choices);
    for (language = 0; language < Localize_LanguageCount(); language++) {
        WCHAR text[2048];
        HFONT font = LayoutFontAt(child, language);
        HGDIOBJ old;
        TEXTMETRICW metrics;
        RECT measured = { 0, 0, max(1, width), 0 };
        int minimum = 0;
        if (!font) continue;
        old = SelectObject(dc, font);
        GetTextMetricsW(dc, &metrics);
        *fontHeight = max(*fontHeight, metrics.tmHeight);
        LayoutCaptionAt(&caption, language, text, ARRAYSIZE(text));
        if ((button && ButtonType(child) != BS_GROUPBOX) || IsClass(child, WC_EDITW) || combo)
            minimum = metrics.tmHeight + ScaleForWindow(child, BUTTON_TEXT_MARGIN_DIPS);
        else if (label && (style & SS_TYPEMASK) <= SS_RIGHT) minimum = metrics.tmHeight + LABEL_SLACK_PX;
        if (IsClass(child, WC_LINK)) {
            minimum = LayoutLinkHeight(child, font, text, width) + LABEL_SLACK_PX;
        } else if (wrapped) {
            DrawTextW(dc, text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | prefix | editControl | tabs | ReadingFlagsAt(language));
            minimum = max(minimum, measured.bottom + LABEL_SLACK_PX);
        } else {
            ZeroMemory(&measured, sizeof measured);
            DrawTextW(dc, text, -1, &measured, DT_CALCRECT | DT_SINGLELINE | prefix | tabs | ReadingFlagsAt(language));
            *textWidth = max(*textWidth, max(measured.right, WidestChoice(dc, child, choices, choiceKeys, language)));
        }
        *height = max(*height, minimum);
        SelectObject(dc, old);
        DeleteObject(font);
    }
    if (choiceKeys) HeapFree(GetProcessHeap(), 0, (void *)choiceKeys);
    ReleaseDC(child, dc);
}

/* The note's text, `format` with the first `keep` characters of `name`
 * (an ellipsis after a cut one), wrapped at `width` on `dc` (the note's font
 * selected): its height, and the width of its longest line. */
static int NoteHeight(HDC dc, const WCHAR *format, const WCHAR *name, size_t keep, int width, WCHAR *text, size_t cch,
                      int *measuredWidth)
{
    RECT measured = { 0, 0, width, 0 };
    Localize_FormatCutName(format, name, keep, text, cch);
    DrawTextW(dc, text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | Localize_ReadingFlags());
    *measuredWidth = measured.right;
    return measured.bottom;
}

/* The note's tallest text in any language (`key`), its name cut to one character. */
static int NoteHeightMaximum(HWND note, const WCHAR *key, const WCHAR *name, int width)
{
    WCHAR shown[LABEL_CCH + 2];
    Localize_FormatCutName(L"%s", name, min(wcslen(name), (size_t)1), shown, ARRAYSIZE(shown));
    return MeasureEveryLanguage(note, &key, 1, shown, DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS, width, NULL).cy;
}

/* The note names a profile in `format`'s one %s: while the note shows, the
 * name alone shortens (with an ellipsis) until the whole text fits the
 * rectangle the layout gave the note; when nothing fits, the whole text
 * stays. */
void Theme_LayoutSidebarNote(HWND note, const WCHAR *format, const WCHAR *name)
{
    WCHAR text[2048], shortened[2048];
    RECT area;
    HDC dc;
    HGDIOBJ old;
    size_t keep;
    int width, available, fullHeight, cutHeight, measuredWidth;
    if (!note || !format || !name || !GetWindowRect(note, &area) || (dc = GetDC(note)) == NULL) return;
    old = SelectObject(dc, (HFONT)SendMessageW(note, WM_GETFONT, 0, 0));
    width = max(1, area.right - area.left);
    available = area.bottom - area.top;
    keep = wcslen(name);
    fullHeight = NoteHeight(dc, format, name, keep, width, text, ARRAYSIZE(text), &measuredWidth);
    if (fullHeight > 0 && (GetWindowLongW(note, GWL_STYLE) & WS_VISIBLE) && (fullHeight > available || measuredWidth > width)) {
        while (keep > 1) {
            keep = Localize_ShorterCut(name, keep);
            cutHeight = NoteHeight(dc, format, name, keep, width, shortened, ARRAYSIZE(shortened), &measuredWidth);
            if (cutHeight > 0 && cutHeight <= available && measuredWidth <= width) {
                StringCchCopyW(text, ARRAYSIZE(text), shortened);
                break;
            }
        }
    }
    SelectObject(dc, old);
    ReleaseDC(note, dc);
    if (fullHeight > 0) SetWindowTextW(note, text);
}

/* ----------------------------------------------------- main window layout */

/* The manager window: what every control needs in every language and
 * script font is measured once per DPI and font (MeasureMain, a cached
 * MainBudget); a new size only places the controls (Theme_LayoutMain). */
#define MAIN_SIDE_GAP_DIPS            12
#define MAIN_RESOURCE_WIDTH_DIPS      821    /* IDD_MAIN's 420 x 270 dialog units in its 9 pt font */
#define MAIN_RESOURCE_HEIGHT_DIPS     499
#define MAIN_STATUS_MINIMUM_DIPS      60     /* the status text between the header buttons */
#define SIDEBAR_DEFAULT_GAP_DIPS      15     /* "Set as default" stays apart from the other actions */
#define SIDEBAR_NOTE_GAP_DIPS         10     /* between "Set as default" and the note below it */
#define DETAILS_PADDING_DIPS          12     /* around the caption of the details' button */
#define ARCHIVED_INSET_DIPS           5      /* "Show archived" ends before the tree: room before the details */
#define SHORTCUT_GROWTH_ONE_ROW_DIPS  280    /* the most a shortcut button grows, on one row */
#define SHORTCUT_GROWTH_TWO_ROWS_DIPS 420    /* ... and on two rows */

/* A button, with every caption it shows (catalog keys). */
typedef struct MainButton {
    int id;
    const WCHAR *captions[2];
} MainButton;

static const MainButton kMainActions[] = {
    { IDC_OPEN, { L"&Open", NULL } }, { IDC_NEW, { L"&New profile\x2026", NULL } }, { IDC_EDIT, { L"&Edit\x2026", NULL } },
    { IDC_DELETE, { L"&Delete\x2026", NULL } }, { IDC_DEFAULT, { L"Set as de&fault", NULL } }
};
#define MAIN_STACKED_ACTIONS (ARRAYSIZE(kMainActions) - 1)   /* all but "Set as default", which sits above the note */
static const MainButton kMainShortcuts[] = {
    { IDC_SC_DESKTOP, { L"Create shortcut on des&ktop", L"Shortcut on desktop" } },
    { IDC_SC_SAVEAS, { L"Create s&hortcut\x2026", NULL } },
    { IDC_SC_PIN, { L"Pin to &taskbar", L"Pinned" } },
    { IDC_SC_START, { L"Add to Start &menu", L"Remove from Start &menu" } }
};
static const MainButton kMainSessions = { IDC_SESSIONS, { L"&Sessions  >", L"<  &Back" } };
static const MainButton kMainLanguage = { IDC_LANGUAGE, { L"&Language\x2026", NULL } };
static const MainButton kMainStatusAction = { IDC_STATUS_ACTION, { L"&Get Claude", L"Set up l&inks" } };
static const MainButton kMainUninstall = { IDC_UNINSTALL, { L"&Uninstall\x2026", NULL } };
static const MainButton kMainUpdate = { IDC_UPDATE, { L"U&pdate", NULL } };
static const MainButton kMainClose = { IDCANCEL, { L"Close", NULL } };

const WCHAR *Theme_MainCaption(int id, int state)
{
    static const MainButton *const single[] = { &kMainSessions, &kMainLanguage, &kMainStatusAction, &kMainUninstall, &kMainUpdate, &kMainClose };
    const MainButton *button = NULL;
    size_t i;
    for (i = 0; i < ARRAYSIZE(kMainActions); i++)
        if (kMainActions[i].id == id) button = &kMainActions[i];
    for (i = 0; i < ARRAYSIZE(kMainShortcuts); i++)
        if (kMainShortcuts[i].id == id) button = &kMainShortcuts[i];
    for (i = 0; i < ARRAYSIZE(single); i++)
        if (single[i]->id == id) button = single[i];
    if (!button) return NULL;
    return state && button->captions[1] ? button->captions[1] : button->captions[0];
}

/* The footer link's texts in each state (MainFooter); their arguments: this
 * version and the author's page; this version and the new one; the new one. */
static const WCHAR *const kMainFooters[MAIN_FOOTERS] = {
    L"Version %s \x00B7 by <a href=\"%s\">Freenitial</a>, not affiliated with Anthropic",
    L"Version %s \x00B7 version %s is available",
    L"Downloading version %s\x2026",
    L"Installing the new version\x2026"
};
/* The note under Set as default; its argument: the profile the regular Claude icon opens. */
static const WCHAR kMainNote[] = L"The default profile opens claude:// links while Claude is closed.\n\nThe regular Claude icon opens \x201C%s\x201D.";
/* The sessions details' captions the main window's minimum keeps room for (SessionsCaption). */
static const WCHAR *const kSessionsCaptions[SESSIONS_CAPTIONS] = { L"Actions", L"Delete session everywhere\x2026" };

const WCHAR *Theme_MainFooter(MainFooter state)
{
    return state >= 0 && state < MAIN_FOOTERS ? kMainFooters[state] : kMainFooters[MAIN_FOOTER_CREDITS];
}

const WCHAR *Theme_MainNote(void)
{
    return kMainNote;
}

const WCHAR *Theme_SessionsCaption(SessionsCaption caption)
{
    return caption >= 0 && caption < SESSIONS_CAPTIONS ? kSessionsCaptions[caption] : kSessionsCaptions[SESSIONS_ACTIONS];
}

/* Main placement reads a cached union of every catalog and script font.
 * The cache owns scalar geometry, never a font or a monitor work area;
 * ForgetDialog frees it. */
typedef struct MainBudget {
    UINT dpi;
    ULONGLONG fontKey;
    BOOL initialized, measuring;
    SIZE minimum;
    int margin, gap, sideGap, sidebar, headerHeight, buttonHeight, headerRow;
    int sessionsWidth, languageWidth, statusActionWidth, uninstallWidth, updateWidth, closeWidth;
    int profileWidth, roleWidth, dataMinimum, profilesPane, detailsPane, archivedWidth;
    int shortcutWidth[ARRAYSIZE(kMainShortcuts)], shortcutTotal, noteHeight, groupHeight, minimumBody, footerHeight;
} MainBudget;

static BOOL IsMainDialog(HWND dialog)
{
    return GetDlgItem(dialog, IDC_STATUS) && GetDlgItem(dialog, IDC_S_TREE);
}

#define MAIN_PLACED_CONTROLS 32   /* the controls Theme_LayoutMain places; its batch grows past it if need be */

/* The main window's controls move together, in one batch (DeferWindowPos):
 * Windows repaints what they leave and what they cover once, for all. */
typedef struct MainMoves {
    HWND dialog;
    HDWP batch;     /* NULL: each control moves at once */
    BOOL lost;      /* the batch failed: Theme_LayoutMain places them again, at once */
} MainMoves;

static void PlaceMainControl(MainMoves *moves, HWND control, int left, int top, int width, int height)
{
    RECT current, target;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!control || moves->lost) return;
    SetRect(&target, left, top, left + max(1, width), top + max(1, height));
    if (GetWindowRect(control, &current)) {
        MapWindowPoints(NULL, moves->dialog, (POINT *)&current, 2);
        if (EqualRect(&current, &target)) return;
        /* What a control shows depends on its size (aligned, wrapped or cut
         * text, rounded corners): resized, it is drawn again whole, not
         * from the pixels it had. */
        if (current.right - current.left != target.right - target.left || current.bottom - current.top != target.bottom - target.top)
            flags |= SWP_NOCOPYBITS;
    }
    if (!moves->batch) {
        SetWindowPos(control, NULL, left, top, target.right - left, target.bottom - top, flags);
        return;
    }
    moves->batch = DeferWindowPos(moves->batch, control, NULL, left, top, target.right - left, target.bottom - top, flags);
    if (!moves->batch) moves->lost = TRUE;
}

/* The widest `key` in any language, in the control's font for its script (a
 * button's ampersand marks its access key); `tallestFont`, when given, grows
 * to the tallest of those fonts. */
static int MainKeyWidth(HWND control, const WCHAR *key, int *tallestFont)
{
    return MeasureEveryLanguage(control, &key, 1, NULL, DT_SINGLELINE | (IsClass(control, WC_BUTTONW) ? 0 : DT_NOPREFIX), 0, tallestFont).cx;
}

/* A button as wide as its widest caption in any language, as tall as the
 * tallest text, and never smaller than in the resource. */
static void MainButtonBudget(HWND dialog, const DialogBase *base, UINT dpi, const MainButton *button, int *width, int *height)
{
    HWND control = GetDlgItem(dialog, button->id);
    RECT source;
    size_t i;
    int tallestFont = 0;
    LayoutSourceRect(dialog, base, control, dpi, &source);
    *width = source.right - source.left;
    for (i = 0; i < ARRAYSIZE(button->captions) && button->captions[i]; i++)
        *width = max(*width, MainKeyWidth(control, button->captions[i], &tallestFont) + MulDiv(BUTTON_PADDING_DIPS, (int)dpi, 96));
    *height = max(source.bottom - source.top, tallestFont ? tallestFont + MulDiv(BUTTON_TEXT_MARGIN_DIPS, (int)dpi, 96) : 0);
}

static HWND MainViewContent(HWND dialog, int id)
{
    HWND control = GetDlgItem(dialog, id);
    View *viewState = ViewOf(control);
    return viewState ? viewState->control : control;
}

/* The face follows the language and every language is measured: only the
 * rest of each font, and the mode (a dark check box is measured here), make
 * a new budget. */
static ULONGLONG HashControlFont(ULONGLONG key, HWND control)
{
    LOGFONTW font = { 0 };
    GetObjectW((HFONT)SendMessageW(control, WM_GETFONT, 0, 0), sizeof font, &font);
    ZeroMemory(font.lfFaceName, sizeof font.lfFaceName);
    return Core_HashBytes(key, &font, sizeof font);
}

static ULONGLONG MainFontKey(HWND dialog)
{
    static const int kControls[] = { IDC_OPEN, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT, IDC_SC_DESKTOP,
        IDC_SC_SAVEAS, IDC_SC_PIN, IDC_SC_START, IDC_SESSIONS, IDC_LANGUAGE, IDC_STATUS_ACTION, IDC_UNINSTALL,
        IDC_UPDATE, IDCANCEL, IDC_S_ARCHIVED, IDC_S_SEARCH, IDC_S_DETAILS, IDC_NOTE, IDC_ABOUT };
    HWND list = MainViewContent(dialog, IDC_LIST);
    ULONGLONG key = Core_HashBytes(CORE_HASH_START, &g_dark, sizeof g_dark);
    size_t i;
    for (i = 0; i < ARRAYSIZE(kControls); i++) key = HashControlFont(key, GetDlgItem(dialog, kControls[i]));
    key = HashControlFont(key, list);
    return HashControlFont(key, ListView_GetHeader(list));
}

static BOOL CachedProfileWidths(HWND list, int *profile, int *role, int *dataMinimum)
{
    HWND dialog = GetAncestor(list, GA_ROOT);
    MainBudget *budget = (MainBudget *)GetPropW(dialog, MAIN_BUDGET_PROP);
    if (!budget || budget->measuring || budget->dpi != GetDpiForWindow(list) || budget->fontKey != MainFontKey(dialog)) return FALSE;
    *profile = budget->profileWidth;
    *role = budget->roleWidth;
    *dataMinimum = budget->dataMinimum;
    return TRUE;
}

/* How far below the top of a row `rowHeight` px tall a line of text in `font`
 * (NULL: the control's own) starts, so that it sits where the captions of the
 * row's buttons sit: a label draws its text at its top, a button centers it. */
static int TextLineOffset(HWND control, HFONT font, int rowHeight)
{
    TEXTMETRICW metrics;
    HDC dc = GetDC(control);
    HGDIOBJ old;
    int offset = 0;
    if (!dc) return 0;
    old = SelectObject(dc, font ? font : (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    if (GetTextMetricsW(dc, &metrics)) offset = max(0, (rowHeight - metrics.tmHeight) / 2);
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    return offset;
}

/* The footer link in every state it shows, its first line beside the
 * buttons' captions (`buttonHeight`): credits, a new version, its download
 * and install. */
static int MainFooterHeight(HWND about, int width, int buttonHeight)
{
    int language, state, height = 0;
    for (language = 0; language < Localize_LanguageCount(); language++) {
        HFONT font = LayoutFontAt(about, language);
        WCHAR text[1024];
        int offset;
        if (!font) continue;
        offset = TextLineOffset(about, font, buttonHeight);
        for (state = 0; state < MAIN_FOOTERS; state++) {
            /* A format takes the arguments of its state; one that takes fewer ignores the rest. */
            StringCchPrintfW(text, ARRAYSIZE(text), Localize_TranslateAt(language, kMainFooters[state]), APP_VERSION_WSTR,
                             state == MAIN_FOOTER_CREDITS ? APP_AUTHOR_URL : APP_VERSION_WSTR);
            height = max(height, offset + LayoutLinkHeight(about, font, text, width) + LABEL_SLACK_PX);
        }
        DeleteObject(font);
    }
    return height;
}

/* A check box's widest caption in any language, with its glyph. */
static void MainCheckBoxBudget(HWND control, int *width, int *height)
{
    int tallestFont, space, needHeight;
    LayoutTextBudget(control, 0, FALSE, width, height, &tallestFont);
    CheckBoxNeeds(control, &space, &needHeight);
    *width += space;
    *height = max(*height, needHeight);
}

/* One row of shortcut buttons when they fit the row with their gaps, else two. */
static int ShortcutRows(const MainBudget *budget, int rowWidth)
{
    return budget->shortcutTotal + ((int)ARRAYSIZE(kMainShortcuts) - 1) * budget->gap <= rowWidth ? 1 : 2;
}

#define LIST_FRAME_PX    2       /* the profile list's frame, a pixel on each side */
#define NOTE_SAMPLE_NAME L"WW"   /* a name the note shows cut to one character: its shortest text */

/* Every button of the window: the side bar's width, each shortcut's, the
 * header's and the footer's, and the tallest of them. */
static void MeasureMainButtons(HWND dialog, const DialogBase *base, MainBudget *budget)
{
    UINT dpi = budget->dpi;
    int i, width, height;
    for (i = 0; i < (int)ARRAYSIZE(kMainActions); i++) {
        MainButtonBudget(dialog, base, dpi, &kMainActions[i], &width, &height);
        budget->sidebar = max(budget->sidebar, width);
        budget->buttonHeight = max(budget->buttonHeight, height);
    }
    for (i = 0; i < (int)ARRAYSIZE(kMainShortcuts); i++) {
        MainButtonBudget(dialog, base, dpi, &kMainShortcuts[i], &budget->shortcutWidth[i], &height);
        budget->shortcutTotal += budget->shortcutWidth[i];
        budget->buttonHeight = max(budget->buttonHeight, height);
    }
    MainButtonBudget(dialog, base, dpi, &kMainSessions, &budget->sessionsWidth, &height);
    budget->buttonHeight = max(budget->buttonHeight, height);
    MainButtonBudget(dialog, base, dpi, &kMainLanguage, &budget->languageWidth, &height);
    budget->buttonHeight = max(budget->buttonHeight, height);
    /* The status action and the footer's buttons take the others' height. */
    MainButtonBudget(dialog, base, dpi, &kMainStatusAction, &budget->statusActionWidth, &height);
    MainButtonBudget(dialog, base, dpi, &kMainUninstall, &budget->uninstallWidth, &height);
    MainButtonBudget(dialog, base, dpi, &kMainUpdate, &budget->updateWidth, &height);
    MainButtonBudget(dialog, base, dpi, &kMainClose, &budget->closeWidth, &height);
}

/* The sessions view's panes: the profiles' side bar and the details as in
 * the resource (the details at least as wide as their widest button and
 * their Actions box in its widest language: sessions.c sizes the box to the
 * current one, Theme_DropDownWidth), and the tree's, under the search box and
 * "Show archived". Returns the tree pane's least width; the search box's and
 * the check box's heights go to `searchHeight` and `archivedHeight`. */
static int MeasureSessionsPanes(HWND dialog, const DialogBase *base, MainBudget *budget, int *searchHeight, int *archivedHeight)
{
    HWND search = GetDlgItem(dialog, IDC_S_SEARCH), details = GetDlgItem(dialog, IDC_S_DETAILS);
    RECT source;
    UINT dpi = budget->dpi;
    int searchWidth, textWidth, tallestFont;
    MainCheckBoxBudget(GetDlgItem(dialog, IDC_S_ARCHIVED), &budget->archivedWidth, archivedHeight);
    LayoutSourceRect(dialog, base, GetDlgItem(dialog, IDC_S_PROFILES), dpi, &source);
    budget->profilesPane = source.right - source.left;
    LayoutSourceRect(dialog, base, details, dpi, &source);
    budget->detailsPane = max(source.right - source.left,
        max(MainKeyWidth(details, kSessionsCaptions[SESSIONS_DELETE_EVERYWHERE], NULL) + MulDiv(DETAILS_PADDING_DIPS, (int)dpi, 96),
            MainKeyWidth(details, kSessionsCaptions[SESSIONS_ACTIONS], NULL) + DropDownFrameWidth(details)));
    LayoutSourceRect(dialog, base, search, dpi, &source);
    searchWidth = source.right - source.left;
    LayoutTextBudget(search, searchWidth, FALSE, &textWidth, searchHeight, &tallestFont);
    return searchWidth + budget->gap + budget->archivedWidth + MulDiv(ARCHIVED_INSET_DIPS, (int)dpi, 96);
}

static void MeasureMain(HWND dialog, const DialogBase *base, MainBudget *budget)
{
    HWND list = MainViewContent(dialog, IDC_LIST);
    RECT source;
    int textWidth, textHeight, tallestFont, searchHeight, archivedHeight, treePane, rows;
    UINT dpi = budget->dpi;
    budget->margin = MulDiv(THEME_MAIN_MARGIN_DIPS, (int)dpi, 96);
    budget->gap = MulDiv(THEME_MAIN_GAP_DIPS, (int)dpi, 96);
    budget->sideGap = MulDiv(MAIN_SIDE_GAP_DIPS, (int)dpi, 96);
    budget->measuring = TRUE;
    Theme_ProfileColumnWidths(list, &budget->profileWidth, &budget->roleWidth, &budget->dataMinimum);
    MeasureMainButtons(dialog, base, budget);
    treePane = MeasureSessionsPanes(dialog, base, budget, &searchHeight, &archivedHeight);
    LayoutTextBudget(ListView_GetHeader(list), 0, FALSE, &textWidth, &textHeight, &tallestFont);
    budget->headerHeight = tallestFont + MulDiv(BUTTON_TEXT_MARGIN_DIPS, (int)dpi, 96);
    LayoutSourceRect(dialog, base, GetDlgItem(dialog, IDC_LIST), dpi, &source);
    budget->minimumBody = source.bottom - source.top;
    LayoutSourceRect(dialog, base, GetDlgItem(dialog, IDC_SC_GROUP), dpi, &source);
    budget->groupHeight = source.bottom - source.top;
    /* As wide as the list's columns and the side bar, the sessions' panes,
     * and the header's buttons with room for the status; never narrower than
     * the resource. */
    budget->minimum.cx = max(budget->profileWidth + budget->roleWidth + budget->dataMinimum + LIST_FRAME_PX + budget->sidebar + budget->sideGap,
                             budget->profilesPane + treePane + budget->detailsPane + 2 * budget->gap) + 2 * budget->margin;
    budget->minimum.cx = max(budget->minimum.cx, budget->sessionsWidth + budget->languageWidth + budget->statusActionWidth + 3 * budget->gap +
                             2 * budget->margin + MulDiv(MAIN_STATUS_MINIMUM_DIPS, (int)dpi, 96));
    budget->minimum.cx = max(budget->minimum.cx, MulDiv(MAIN_RESOURCE_WIDTH_DIPS, (int)dpi, 96));
    /* As tall as the side bar's stacked actions, Set as default and its note. */
    budget->noteHeight = NoteHeightMaximum(GetDlgItem(dialog, IDC_NOTE), kMainNote, NOTE_SAMPLE_NAME, budget->sidebar);
    budget->minimumBody = max(budget->minimumBody, budget->headerHeight + (int)MAIN_STACKED_ACTIONS * budget->buttonHeight +
                              ((int)MAIN_STACKED_ACTIONS - 1) * budget->gap + MulDiv(SIDEBAR_DEFAULT_GAP_DIPS + SIDEBAR_NOTE_GAP_DIPS, (int)dpi, 96) +
                              budget->buttonHeight + budget->noteHeight);
    budget->headerRow = max(budget->buttonHeight, max(archivedHeight, searchHeight));
    budget->footerHeight = max(budget->buttonHeight, MainFooterHeight(GetDlgItem(dialog, IDC_ABOUT),
        max(1, budget->minimum.cx - 2 * budget->margin - budget->uninstallWidth - budget->closeWidth - budget->updateWidth - 3 * budget->gap),
        budget->buttonHeight));
    rows = ShortcutRows(budget, budget->minimum.cx - 2 * budget->margin);
    budget->minimum.cy = budget->margin + budget->headerRow + budget->sideGap + budget->minimumBody + budget->sideGap +
                         budget->groupHeight + budget->gap + rows * budget->buttonHeight + (rows - 1) * budget->gap + 2 * budget->gap +
                         budget->footerHeight + budget->margin;
    budget->minimum.cy = max(budget->minimum.cy, MulDiv(MAIN_RESOURCE_HEIGHT_DIPS, (int)dpi, 96));
    budget->measuring = FALSE;
}

static MainBudget *MainBudgetFor(HWND dialog, BOOL create)
{
    DialogBase *base = (DialogBase *)GetPropW(dialog, DIALOG_BASE_PROP);
    MainBudget *budget = (MainBudget *)GetPropW(dialog, MAIN_BUDGET_PROP);
    ULONGLONG key;
    UINT dpi = GetDpiForWindow(dialog);
    if (!base || !base->hasLayout || !IsMainDialog(dialog)) return NULL;
    if (!budget && !create) return NULL;
    key = MainFontKey(dialog);
    if (budget && budget->dpi == dpi && budget->fontKey == key) return budget;
    if (!budget) {
        budget = (MainBudget *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *budget);
        if (!budget) return NULL;
        if (!SetPropW(dialog, MAIN_BUDGET_PROP, budget)) {
            HeapFree(GetProcessHeap(), 0, budget);
            return NULL;
        }
    } else {
        BOOL initialized = budget->initialized;
        ZeroMemory(budget, sizeof *budget);
        budget->initialized = initialized;
    }
    budget->dpi = dpi;
    budget->fontKey = key;
    MeasureMain(dialog, base, budget);
    return budget;
}

BOOL Theme_MainMinimum(HWND dialog, SIZE *client)
{
    MainBudget *budget = MainBudgetFor(dialog, FALSE);
    if (!budget || !client) return FALSE;
    *client = budget->minimum;
    return TRUE;
}

/* Where the main window's bands go in its current client area. */
typedef struct MainArea {
    int left, right;              /* the content's edges inside the margins */
    int rowWidth;                 /* right - left */
    int bodyTop, bodyHeight;      /* the list (or the sessions' panes) */
    int shortcutsTop, shortcutRows, footerTop;
} MainArea;

/* The header row: Sessions and Language on the left, the status action on
 * the right while it shows, the status between them, its text level with
 * the buttons' captions. */
static void PlaceMainHeader(MainMoves *moves, const MainBudget *budget, const MainArea *area)
{
    HWND dialog = moves->dialog, status = GetDlgItem(dialog, IDC_STATUS);
    int gap = budget->gap, statusLeft = area->left + budget->sessionsWidth + gap + budget->languageWidth + gap, statusRight = area->right;
    int statusTop = budget->margin + TextLineOffset(status, NULL, budget->headerRow);
    if (GetWindowLongW(GetDlgItem(dialog, IDC_STATUS_ACTION), GWL_STYLE) & WS_VISIBLE)
        statusRight -= budget->statusActionWidth + gap;
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_SESSIONS), area->left, budget->margin, budget->sessionsWidth, budget->headerRow);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_LANGUAGE), area->left + budget->sessionsWidth + gap, budget->margin, budget->languageWidth,
                     budget->headerRow);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_STATUS_ACTION), area->right - budget->statusActionWidth, budget->margin,
                     budget->statusActionWidth, budget->headerRow);
    PlaceMainControl(moves,status, statusLeft, statusTop, statusRight - statusLeft, budget->margin + budget->headerRow - statusTop);
}

/* The profile list, its side bar of actions (Set as default and its note
 * at the bottom of the minimum body) and the shortcuts' title below. */
static void PlaceProfilesView(MainMoves *moves, const MainBudget *budget, const MainArea *area)
{
    HWND dialog = moves->dialog;
    int sidebarLeft = area->right - budget->sidebar, noteTop = area->bodyTop + budget->minimumBody - budget->noteHeight, i;
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_LIST), area->left, area->bodyTop, area->rowWidth - budget->sidebar - budget->sideGap,
                     area->bodyHeight);
    for (i = 0; i < (int)MAIN_STACKED_ACTIONS; i++)
        PlaceMainControl(moves,GetDlgItem(dialog, kMainActions[i].id), sidebarLeft,
                         area->bodyTop + budget->headerHeight + i * (budget->buttonHeight + budget->gap), budget->sidebar, budget->buttonHeight);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_NOTE), sidebarLeft, noteTop, budget->sidebar, budget->noteHeight);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_DEFAULT), sidebarLeft,
                     noteTop - MulDiv(SIDEBAR_NOTE_GAP_DIPS, (int)budget->dpi, 96) - budget->buttonHeight, budget->sidebar, budget->buttonHeight);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_SC_GROUP), area->left, area->bodyTop + area->bodyHeight + budget->sideGap, area->rowWidth,
                     budget->groupHeight);
}

/* The shortcut buttons on one row or two. The free space goes to the two
 * middle buttons, up to a limit; on one row, what is left is shared between
 * the gaps; on two, each row's second button ends at the right edge. */
static void PlaceShortcuts(MainMoves *moves, const MainBudget *budget, const MainArea *area)
{
    HWND dialog = moves->dialog;
    int width[ARRAYSIZE(kMainShortcuts)], gaps = (int)ARRAYSIZE(kMainShortcuts) - 1, gap = budget->gap, i, before;
    CopyMemory(width, budget->shortcutWidth, sizeof width);
    if (area->shortcutRows == 1) {
        int extra = max(0, area->rowWidth - budget->shortcutTotal - gaps * gap);
        int limit = MulDiv(SHORTCUT_GROWTH_ONE_ROW_DIPS, (int)budget->dpi, 96);
        width[1] += min(extra / 2, max(0, limit - width[1]));
        width[2] += min(extra - extra / 2, max(0, limit - width[2]));
    } else {
        int limit = MulDiv(SHORTCUT_GROWTH_TWO_ROWS_DIPS, (int)budget->dpi, 96);
        width[1] = max(width[1], min(limit, area->rowWidth - width[0] - gap));
        width[2] = max(width[2], min(limit, area->rowWidth - width[3] - gap));
    }
    for (i = 0; i < (int)ARRAYSIZE(kMainShortcuts); i++) {
        int x = area->left, row = area->shortcutRows == 1 ? 0 : i / 2, column = area->shortcutRows == 1 ? i : i % 2;
        if (area->shortcutRows == 1) {
            int spaces = area->rowWidth;
            for (before = 0; before < (int)ARRAYSIZE(kMainShortcuts); before++) spaces -= width[before];
            for (before = 0; before < column; before++) x += width[before];
            x += column * (spaces / gaps) + min(column, spaces % gaps);
        } else if (column) {
            x += area->rowWidth - width[i];
        }
        PlaceMainControl(moves,GetDlgItem(dialog, kMainShortcuts[i].id), x, area->shortcutsTop + row * (budget->buttonHeight + gap),
                         width[i], budget->buttonHeight);
    }
}

/* The footer: Uninstall, the version link (its first line level with the
 * buttons' captions), Update while it shows, Close. */
static void PlaceMainFooter(MainMoves *moves, const MainBudget *budget, const MainArea *area)
{
    HWND dialog = moves->dialog, about = GetDlgItem(dialog, IDC_ABOUT);
    int gap = budget->gap, updateRoom = (GetWindowLongW(GetDlgItem(dialog, IDC_UPDATE), GWL_STYLE) & WS_VISIBLE) ? budget->updateWidth + gap : 0;
    int aboutTop = area->footerTop + TextLineOffset(about, NULL, budget->buttonHeight);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_UNINSTALL), area->left, area->footerTop, budget->uninstallWidth, budget->buttonHeight);
    PlaceMainControl(moves,GetDlgItem(dialog, IDCANCEL), area->right - budget->closeWidth, area->footerTop, budget->closeWidth,
                     budget->buttonHeight);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_UPDATE), area->right - budget->closeWidth - gap - budget->updateWidth, area->footerTop,
                     budget->updateWidth, budget->buttonHeight);
    PlaceMainControl(moves,about, area->left + budget->uninstallWidth + gap, aboutTop,
                     area->rowWidth - budget->uninstallWidth - budget->closeWidth - updateRoom - 2 * gap,
                     area->footerTop + budget->footerHeight - aboutTop);
}

/* The sessions: profiles, tree and details down to the footer; above the
 * tree, the search box and "Show archived", as wide as its caption in the
 * current language. */
static void PlaceSessionsView(MainMoves *moves, const MainBudget *budget, const MainArea *area)
{
    HWND dialog = moves->dialog, archived = GetDlgItem(dialog, IDC_S_ARCHIVED);
    SIZE ideal;
    int gap = budget->gap, bottom = area->footerTop - budget->sideGap, detailsLeft = area->right - budget->detailsPane;
    int treeLeft = area->left + budget->profilesPane + gap, treeRight = detailsLeft - gap;
    int archivedRight = treeRight - MulDiv(ARCHIVED_INSET_DIPS, (int)budget->dpi, 96);
    int archivedWidth = Theme_CheckBoxSize(archived, &ideal) && ideal.cx > 0 ? ideal.cx : budget->archivedWidth;
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_S_PROFILES), area->left, area->bodyTop, budget->profilesPane, bottom - area->bodyTop);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_S_DETAILS), detailsLeft, area->bodyTop, budget->detailsPane, bottom - area->bodyTop);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_S_SEARCH), treeLeft, area->bodyTop, archivedRight - archivedWidth - gap - treeLeft,
                     budget->headerRow);
    PlaceMainControl(moves,archived, archivedRight - archivedWidth, area->bodyTop, archivedWidth, budget->headerRow);
    PlaceMainControl(moves,GetDlgItem(dialog, IDC_S_TREE), treeLeft, area->bodyTop + budget->headerRow + gap, treeRight - treeLeft,
                     bottom - area->bodyTop - budget->headerRow - gap);
}

static void PlaceMain(MainMoves *moves, const MainBudget *budget, const MainArea *area)
{
    PlaceMainHeader(moves, budget, area);
    PlaceProfilesView(moves, budget, area);
    PlaceShortcuts(moves, budget, area);
    PlaceMainFooter(moves, budget, area);
    PlaceSessionsView(moves, budget, area);
}

void Theme_LayoutMain(HWND dialog)
{
    MainBudget *budget = MainBudgetFor(dialog, FALSE);
    MainArea area;
    MainMoves moves;
    RECT client;
    int width, gap;
    if (!budget || !GetClientRect(dialog, &client) || client.right <= 0 || client.bottom <= 0) return;
    gap = budget->gap;
    width = min(client.right, max(budget->minimum.cx, MulDiv(THEME_MAIN_READING_WIDTH_DIPS, (int)budget->dpi, 96)));
    area.left = (client.right - width) / 2 + budget->margin;
    area.right = (client.right - width) / 2 + width - budget->margin;
    area.rowWidth = area.right - area.left;
    area.shortcutRows = ShortcutRows(budget, area.rowWidth);
    area.bodyTop = budget->margin + budget->headerRow + budget->sideGap;
    area.footerTop = client.bottom - budget->margin - budget->footerHeight;
    area.shortcutsTop = area.footerTop - 2 * gap - area.shortcutRows * budget->buttonHeight - (area.shortcutRows - 1) * gap;
    area.bodyHeight = area.shortcutsTop - gap - budget->groupHeight - budget->sideGap - area.bodyTop;
    moves.dialog = dialog;
    moves.lost = FALSE;
    moves.batch = BeginDeferWindowPos(MAIN_PLACED_CONTROLS);
    PlaceMain(&moves, budget, &area);
    if (!moves.batch || !EndDeferWindowPos(moves.batch)) {
        /* Without a batch, each control moves at once; one placed already stays. */
        moves.batch = NULL;
        moves.lost = FALSE;
        PlaceMain(&moves, budget, &area);
    }
}

static void FitCompactMain(HWND dialog)
{
    MainBudget *budget = MainBudgetFor(dialog, TRUE);
    if (!budget) return;
    if (!budget->initialized) {
        RECT frame, size = { 0, 0, budget->minimum.cx, budget->minimum.cy };
        MONITORINFO monitor = { sizeof monitor };
        budget->initialized = TRUE;
        SetDialogDpiChangeBehavior(dialog, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
        if (GetWindowRect(dialog, &frame) && GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor) &&
            AdjustWindowRectExForDpi(&size, (DWORD)GetWindowLongW(dialog, GWL_STYLE), GetMenu(dialog) != NULL,
                                     (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), budget->dpi)) {
            int width = min(size.right - size.left, monitor.rcWork.right - monitor.rcWork.left);
            int height = min(size.bottom - size.top, monitor.rcWork.bottom - monitor.rcWork.top);
            int x = min(max(frame.left, monitor.rcWork.left), monitor.rcWork.right - width);
            int y = min(max(frame.top, monitor.rcWork.top), monitor.rcWork.bottom - height);
            SetWindowPos(dialog, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    Theme_LayoutMain(dialog);
}

/* ---------------------------------------------------------- dialog fitting */

#define FIT_MAX_CONTROLS 128

typedef struct FitControl {
    HWND hwnd;
    RECT rc;          /* its resource rectangle at the dialog's scale, then its place */
    int  grow;        /* the height its text needs beyond that rectangle's */
    int  minimum;     /* the height its text needs */
    BOOL visible, flexible, wrapped;
} FitControl;

/* A band of the dialog that changes height: its text grows (grow > 0), or
 * it only holds hidden controls and closes (grow < 0). */
typedef struct FitRow {
    int top, bottom, grow;
} FitRow;

/* The rows a dialog hides close: a hidden control beside no shown one takes
 * out its band, from the bottom of the shown control above it (else its own
 * top) to its own bottom, so what is below moves up by the row and the gap
 * above it. Each control adds at most one row; returns the row count. */
static int CloseHiddenRows(const FitControl *controls, int count, FitRow *rows, int rowCount)
{
    int i, j, first = rowCount;
    for (i = 0; i < count; i++) {
        const RECT *hidden = &controls[i].rc;
        int top = hidden->top;
        BOOL beside = FALSE, shownAbove = FALSE;
        if (controls[i].visible || hidden->bottom <= hidden->top) continue;
        for (j = 0; j < count && !beside; j++) {
            const RECT *shown = &controls[j].rc;
            if (!controls[j].visible) continue;
            if (shown->top < hidden->bottom && shown->bottom > hidden->top) {
                beside = TRUE;
            } else if (shown->bottom <= hidden->top) {
                top = shownAbove ? max(top, shown->bottom) : shown->bottom;
                shownAbove = TRUE;
            }
        }
        if (beside) continue;
        rows[rowCount].top = top;
        rows[rowCount].bottom = hidden->bottom;
        rowCount++;
    }
    /* Bands that overlap or touch are one row. */
    for (i = first; i < rowCount; i++)
        for (j = i + 1; j < rowCount; j++)
            if (rows[j].top <= rows[i].bottom && rows[j].bottom >= rows[i].top) {
                rows[i].top = min(rows[i].top, rows[j].top);
                rows[i].bottom = max(rows[i].bottom, rows[j].bottom);
                rows[j] = rows[--rowCount];
                j = i;   /* the larger row is compared again with all the others */
            }
    for (i = first; i < rowCount; i++) rows[i].grow = rows[i].top - rows[i].bottom;
    return rowCount;
}

/* A dialog too tall for its monitor: its tallest flexible list `flexible`
 * gives up `excess` px (down to three lines), what is below it moves up, and
 * the fixed columns beside it compact their padding and gaps (their text
 * keeps its measured height). */
static void GiveUpHeight(FitControl *controls, int count, int flexible, int excess, int fontHeight)
{
    RECT body = controls[flexible].rc;
    BOOL grouped[FIT_MAX_CONTROLS] = { FALSE }, sideColumn[FIT_MAX_CONTROLS] = { FALSE };
    int minimumBody = 3 * fontHeight, gap = max(1, fontHeight / 4), shrink, i, j, k;
    shrink = min(excess, max(0, body.bottom - body.top - minimumBody));
    for (i = 0; i < count; i++) {
        RECT *rc = &controls[i].rc;
        sideColumn[i] = controls[i].visible && !controls[i].flexible && controls[i].minimum > 0 &&
                        rc->top >= body.top && rc->bottom <= body.bottom && (rc->right <= body.left || rc->left >= body.right);
        if (rc->top >= body.bottom) OffsetRect(rc, 0, -shrink);
        else if (controls[i].visible && controls[i].flexible && rc->top < body.bottom && rc->bottom > body.top)
            rc->bottom = max(rc->top + minimumBody, rc->bottom - shrink);
    }
    for (i = 0; i < count; i++) {
        int members[FIT_MAX_CONTROLS], memberCount = 0, top;
        RECT column;
        if (grouped[i] || !sideColumn[i]) continue;
        column = controls[i].rc;
        for (j = i; j < count; j++) {
            RECT rc = controls[j].rc;
            if (!grouped[j] && sideColumn[j] && rc.left < column.right && rc.right > column.left) {
                members[memberCount++] = j;
                grouped[j] = TRUE;
                column.left = min(column.left, rc.left);
                column.right = max(column.right, rc.right);
            }
        }
        for (j = 0; j < memberCount; j++) for (k = j + 1; k < memberCount; k++)
            if (controls[members[k]].rc.top < controls[members[j]].rc.top) {
                int earlier = members[k];
                members[k] = members[j];
                members[j] = earlier;
            }
        /* No higher than a line below the list's top, where its header ends. */
        top = max(body.top + fontHeight + gap, controls[members[0]].rc.top);
        for (j = 0; j < memberCount; j++) {
            FitControl *control = &controls[members[j]];
            control->rc.top = top;
            control->rc.bottom = top + control->minimum;
            top = control->rc.bottom + gap;
        }
    }
}

#define FIT_FONT_DIPS      16   /* a 9 pt font's height: the least a dialog's text takes */
#define LABEL_PADDING_DIPS 6    /* around a one-line label's caption */

/* The dialog's controls with their resource rectangles at `dpi`, and what
 * their captions need in every language: the width percent the widest
 * one-line caption asks of the dialog, and the tallest font. Returns the
 * control count. */
static int MeasureFitControls(HWND dialog, const DialogBase *base, UINT dpi, FitControl *controls, int *widthPercent, int *fontHeight)
{
    HWND child;
    int count = 0;
    for (child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        BOOL button = IsClass(child, WC_BUTTONW), label = IsClass(child, WC_STATICW);
        BOOL link = IsClass(child, WC_LINK), combo = IsDropDownList(child);
        FitControl *control = &controls[count];
        RECT rc;
        WCHAR text[2048];
        int need, room, pad, textWidth, tallestFont, minimum = 0;
        LONG style = GetWindowLongW(child, GWL_STYLE), labelType = style & SS_TYPEMASK;
        if (combo) FitComboRows(child);
        if (!LayoutSourceRect(dialog, base, child, dpi, &rc)) continue;
        if (count == FIT_MAX_CONTROLS) break;
        ZeroMemory(control, sizeof *control);
        control->hwnd = child;
        control->rc = rc;
        control->visible = (style & WS_VISIBLE) != 0;
        control->flexible = IsClass(child, VIEW_CLASS) || IsClass(child, WC_LISTVIEWW) ||
                            IsClass(child, WC_TREEVIEWW) || IsClass(child, WC_LISTBOXW);
        count++;
        if (!control->visible) continue;
        room = rc.right - rc.left;
        text[0] = 0;
        GetWindowTextW(child, text, ARRAYSIZE(text));
        LayoutTextBudget(child, room, FALSE, &textWidth, &minimum, &tallestFont);
        *fontHeight = max(*fontHeight, tallestFont);
        /* A wrapped caption (a link, or a label made several lines tall,
         * with a line break, or wrapping as an edit does) is measured at its
         * final width: GrowRows. */
        control->wrapped = link || (label && labelType == SS_LEFT &&
                           (rc.bottom - rc.top >= 2 * tallestFont || wcschr(text, L'\n') || (style & SS_EDITCONTROL)));
        control->grow = max(0, minimum - (rc.bottom - rc.top));
        control->minimum = minimum;
        if ((!button && !label && !combo) || control->wrapped) continue;
        if (label && labelType != SS_LEFT && labelType != SS_LEFTNOWORDWRAP && labelType != SS_CENTER && labelType != SS_RIGHT) continue;
        if (button && (style & (BS_ICON | BS_BITMAP))) continue;
        /* A drop-down list is painted as a drop-down button (PaintDropDownList). */
        pad = combo ? DropDownFrameWidth(child) : ScaleForWindow(child, button ? BUTTON_PADDING_DIPS : LABEL_PADDING_DIPS);
        if (IsCheckBox(child)) {
            int space, needHeight;
            CheckBoxNeeds(child, &space, &needHeight);
            pad += space;
        }
        else if (ButtonType(child) == BS_RADIOBUTTON || ButtonType(child) == BS_AUTORADIOBUTTON)
            pad += GetSystemMetricsForDpi(SM_CXMENUCHECK, GetDpiForWindow(child));
        need = textWidth + pad;
        if (room > 0 && need > room) *widthPercent = max(*widthPercent, (int)(((LONGLONG)need * 100 + room - 1) / room));
    }
    return count;
}

/* The rows whose text grows: wrapped captions measured at their width in a
 * dialog `width` px wide (`sourceWidth` in the resource), merged with the
 * rows they share. Returns the row count. */
static int GrowRows(FitControl *controls, int count, int width, int sourceWidth, FitRow *rows)
{
    int i, j, rowCount = 0;
    for (i = 0; i < count; i++) {
        RECT rc = controls[i].rc;
        if (!controls[i].visible) continue;
        if (controls[i].wrapped) {
            int textWidth, minimum, tallestFont;
            LayoutTextBudget(controls[i].hwnd, max(1, MulDiv(rc.right - rc.left, width, sourceWidth)), TRUE,
                             &textWidth, &minimum, &tallestFont);
            controls[i].minimum = max(tallestFont, minimum);
            controls[i].grow = max(0, controls[i].minimum - (rc.bottom - rc.top));
        }
        if (!controls[i].grow) continue;
        for (j = 0; j < rowCount; j++) if (rc.top < rows[j].bottom && rc.bottom > rows[j].top) break;
        if (j == rowCount) {
            rows[j].top = rc.top;
            rows[j].bottom = rc.bottom;
            rows[j].grow = controls[i].grow;
            rowCount++;
        } else {
            rows[j].top = min(rows[j].top, rc.top);
            rows[j].bottom = max(rows[j].bottom, rc.bottom);
            rows[j].grow = max(rows[j].grow, controls[i].grow);
        }
    }
    return rowCount;
}

/* Each control widened with the dialog and moved by the rows that grew or
 * closed above it, its own row's growth taken in. Returns the tallest
 * visible flexible control, -1 for none. */
static int MoveWithRows(FitControl *controls, int count, const FitRow *rows, int rowCount, int width, int sourceWidth)
{
    int i, j, flexible = -1;
    for (i = 0; i < count; i++) {
        RECT rc = controls[i].rc;
        int shift = 0, span = 0;
        for (j = 0; j < rowCount; j++) {
            if (rows[j].bottom <= rc.top) shift += rows[j].grow;
            else if (rows[j].bottom <= rc.bottom) span += rows[j].grow;
        }
        SetRect(&controls[i].rc, MulDiv(rc.left, width, sourceWidth), rc.top + shift, MulDiv(rc.right, width, sourceWidth),
                rc.bottom + shift + max(span, controls[i].grow));
        if (controls[i].visible && controls[i].flexible && (flexible < 0 ||
            controls[i].rc.bottom - controls[i].rc.top > controls[flexible].rc.bottom - controls[flexible].rc.top)) flexible = i;
    }
    return flexible;
}

/* The controls where the fit puts them; one already there is left alone. */
static void PlaceFitControls(HWND dialog, const FitControl *controls, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        RECT rc = controls[i].rc, actual;
        if (GetWindowRect(controls[i].hwnd, &actual)) {
            MapWindowPoints(NULL, dialog, (POINT *)&actual, 2);
            if (EqualRect(&actual, &rc)) continue;
        }
        SetWindowPos(controls[i].hwnd, NULL, rc.left, rc.top, max(1, rc.right - rc.left), max(1, rc.bottom - rc.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* Localized captions are measured in every language, not clipped to their
 * source language's widths: a dialog widens for the widest and its rows
 * grow for wrapped text; the rows it hides close. Scaling keeps each row's
 * spacing and the dialog inside its monitor; a flexible list gives up height
 * first. Every fit starts again from the resource layout: a row shown again
 * opens at the next fit. */
void Theme_FitDialog(HWND dialog)
{
    FitControl controls[FIT_MAX_CONTROLS];
    FitRow rows[FIT_MAX_CONTROLS];
    RECT client, actualClient, window, work;
    DialogBase *base;
    UINT dpi = GetDpiForWindow(dialog);
    MONITORINFO monitor;
    int widthPercent = 100, width, height, sourceWidth, count, rowCount, i, growHeight = 0;
    int nonclientWidth, nonclientHeight, fontHeight, flexible, left, top;
    monitor.cbSize = sizeof monitor;
    ApplyDialogFont(dialog);
    Theme_RememberLayout(dialog);
    if (IsMainDialog(dialog)) { FitCompactMain(dialog); return; }
    if (!GetClientRect(dialog, &client) || !GetWindowRect(dialog, &window) || client.right <= 0) return;
    actualClient = client;
    base = (DialogBase *)GetPropW(dialog, DIALOG_BASE_PROP);
    if (base && base->hasLayout) {
        if (!dpi) dpi = base->layoutDpi;
        client.right = MulDiv(base->client.cx, (int)dpi, (int)base->layoutDpi);
        client.bottom = MulDiv(base->client.cy, (int)dpi, (int)base->layoutDpi);
    }
    fontHeight = MulDiv(FIT_FONT_DIPS, dpi ? (int)dpi : 96, 96);
    count = MeasureFitControls(dialog, base, dpi, controls, &widthPercent, &fontHeight);
    if (!GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    work = monitor.rcWork;
    sourceWidth = client.right;
    nonclientWidth = window.right - window.left - actualClient.right;
    nonclientHeight = window.bottom - window.top - actualClient.bottom;
    width = max(1, min(MulDiv(sourceWidth, widthPercent, 100), work.right - work.left - nonclientWidth));
    rowCount = GrowRows(controls, count, width, sourceWidth, rows);
    rowCount = CloseHiddenRows(controls, count, rows, rowCount);
    for (i = 0; i < rowCount; i++) growHeight += rows[i].grow;
    height = min(client.bottom + growHeight, max(1, work.bottom - work.top - nonclientHeight));
    flexible = MoveWithRows(controls, count, rows, rowCount, width, sourceWidth);
    if (flexible >= 0 && client.bottom + growHeight > height)
        GiveUpHeight(controls, count, flexible, client.bottom + growHeight - height, fontHeight);
    left = min(max(window.left, work.left), work.right - width - nonclientWidth);
    top = min(max(window.top, work.top), work.bottom - height - nonclientHeight);
    if (width != actualClient.right || height != actualClient.bottom || left != window.left || top != window.top)
        SetWindowPos(dialog, NULL, left, top, width + nonclientWidth, height + nonclientHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    PlaceFitControls(dialog, controls, count);
}

/* ------------------------------------------------------------ theming */

/* The frame the theme draws for `child` (FRAME_*): a drop-down list whole,
 * a one-line edit with its text centered, and in dark mode no frame around
 * a scrolling control made with one (`made`). */
static DWORD_PTR ChildFrame(HWND child, INT_PTR made, BOOL scrolls)
{
    DWORD_PTR frame = 0;
    if (IsDropDownList(child)) return FRAME_COMBO;
    if (IsClass(child, WC_EDITW)) {
        frame = FRAME_EDIT;
        if (!(GetWindowLongW(child, GWL_STYLE) & ES_MULTILINE) && !IsClass(GetParent(child), WC_COMBOBOXW)) frame |= FRAME_CENTER;
    }
    if (scrolls && g_dark && (made & (MADE_BORDER | MADE_EDGE))) frame |= FRAME_BARE;
    return frame;
}

/* A report table: its component, Windows' theme for it and its header, its
 * cell tips and the palette's colors. */
static void ThemeListView(HWND list)
{
    HWND header = ListView_GetHeader(list), tips = ListView_GetToolTips(list);
    ApplyTableComponent(list);
    SetWindowTheme(list, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    if (header) {
        if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(header, g_dark);
        SetWindowTheme(header, g_dark ? L"DarkMode_ItemsView" : NULL, NULL);
        SendMessageW(header, WM_THEMECHANGED, 0, 0);
    }
    if (tips) ApplyCellTip(list, tips);
    ListView_SetBkColor(list, g_palette.color[THEME_FIELD]);
    /* Native labels are transparent over the common row background. */
    ListView_SetTextBkColor(list, g_highContrast ? g_palette.color[THEME_FIELD] : CLR_NONE);
    ListView_SetTextColor(list, g_palette.color[THEME_TEXT]);
}

/* A tree: Windows' theme for it, its row tips and the palette's colors. */
static void ThemeTree(HWND tree)
{
    HWND tips = TreeView_GetToolTips(tree);
    SetWindowTheme(tree, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    if (tips) ApplyCellTip(tree, tips);
    TreeView_SetBkColor(tree, g_dark ? g_palette.color[THEME_FIELD] : (COLORREF)-1);
    TreeView_SetTextColor(tree, g_dark ? g_palette.color[THEME_TEXT] : (COLORREF)-1);
}

static BOOL CALLBACK ThemeChild(HWND child, LPARAM lp)
{
    BOOL edit = IsClass(child, WC_EDITW), list = IsClass(child, WC_LISTVIEWW), tree = IsClass(child, WC_TREEVIEWW);
    BOOL listbox = IsClass(child, WC_LISTBOXW), combo = IsClass(child, WC_COMBOBOXW);
    BOOL view = IsClass(child, VIEW_CLASS), viewed = ViewAround(child) != NULL;
    BOOL multiline = edit && (GetWindowLongW(child, GWL_STYLE) & ES_MULTILINE), scrolls = list || tree || listbox || multiline || view;
    INT_PTR made = RecordedFrameFlags(child);
    DWORD_PTR frame = ChildFrame(child, made, scrolls);
    (void)lp;
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(child, g_dark);
    if (SetWindowSubclass(child, ChildSubclass, CHILD_SUBCLASS, frame))
        RoundControl(child, edit || view || ((list || tree || listbox) && !viewed));
    if (frame & FRAME_COMBO) FitComboRows(child);
    if (scrolls) FitFrame(child, made);
    if (frame & FRAME_CENTER)   /* the edit's font determines its client area */
        SetWindowPos(child, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if ((list || tree || listbox) && !viewed) SmoothScrolling(child);   /* in a view, the view scrolls */
    if (list) {
        ThemeListView(child);
    } else if (tree) {
        ThemeTree(child);
    } else if (IsEndEllipsisLabel(child)) {
        ApplyCellTip(child, NULL);
    } else if (listbox || edit) {
        /* A list box's scroll bar; an edit's native scroll bars (its client and frame are painted here). */
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    } else if (IsClass(child, WC_BUTTONW)) {
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    } else if (combo) {
        SetWindowTheme(child, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (view || (made & MADE_SCROLLS)) {
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);   /* a window of ours that scrolls */
        SmoothScrolling(child);
    }
    /* AllowDarkModeForWindow takes effect then, also where the theme name
     * did not change. */
    SendMessageW(child, WM_THEMECHANGED, 0, 0);
    return TRUE;
}

void Theme_Apply(HWND dialog)
{
    ApplyDialogFont(dialog);
    SetPropW(dialog, THEME_PROP, (HANDLE)(INT_PTR)(g_dark ? THEMED_DARK : THEMED_LIGHT));
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(dialog, g_dark);
    ApplyTitleBarMode(dialog);
    EnumChildWindows(dialog, ThemeChild, 0);
    RedrawWindow(dialog, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

/* A list box made without a frame (the sessions' profiles) is a side bar on
 * the window's background, in both modes. In a view, the view has the frame
 * it was made with. */
static BOOL IsSideBar(HWND control)
{
    HWND view;
    INT_PTR made;
    if (!IsClass(control, WC_LISTBOXW)) return FALSE;
    view = ViewAround(control);
    made = (INT_PTR)GetPropW(view ? view : control, BORDER_PROP);
    if (made) return !(made & (MADE_BORDER | MADE_EDGE));
    return !(GetWindowLongW(control, GWL_STYLE) & WS_BORDER) && !(GetWindowLongW(control, GWL_EXSTYLE) & WS_EX_CLIENTEDGE);
}

/* WM_CTLCOLOR* for dialogs. The control `mutedId` draws its text in gray. */
INT_PTR Theme_CtlColor(UINT msg, WPARAM wp, LPARAM lp, int mutedId)
{
    HDC dc = (HDC)wp;
    BOOL muted = mutedId && GetDlgCtrlID((HWND)lp) == mutedId;
    if (msg == WM_CTLCOLORLISTBOX && IsSideBar((HWND)lp)) {
        SetTextColor(dc, g_palette.color[THEME_TEXT]);
        SetBkColor(dc, g_palette.color[THEME_FACE]);
        return (INT_PTR)g_brush[THEME_FACE];
    }
    if (!g_dark) {
        if (!muted || msg != WM_CTLCOLORSTATIC) return FALSE;
        SetTextColor(dc, g_palette.color[THEME_MUTED]);
        SetBkMode(dc, TRANSPARENT);
        return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
    }
    switch (msg) {
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_brush[THEME_FACE];
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc, g_palette.color[muted ? THEME_MUTED : THEME_TEXT]);
        SetBkColor(dc, g_palette.color[THEME_FACE]);
        return (INT_PTR)g_brush[THEME_FACE];
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, g_palette.color[THEME_TEXT]);
        SetBkColor(dc, g_palette.color[THEME_FIELD]);
        return (INT_PTR)g_brush[THEME_FIELD];
    }
    return FALSE;
}

/* Centered on its owner when the owner is on screen, else on its monitor;
 * kept inside the monitor's work area. */
static void CenterOnOwner(HWND dialog)
{
    HWND owner = GetWindow(dialog, GW_OWNER);
    BOOL shown = owner && IsWindowVisible(owner) && !IsIconic(owner);
    RECT on, self, placed;
    MONITORINFO monitor;
    monitor.cbSize = sizeof monitor;
    if (!GetWindowRect(dialog, &self) || !GetMonitorInfoW(MonitorFromWindow(shown ? owner : dialog, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    if (!shown || !GetWindowRect(owner, &on)) on = monitor.rcWork;
    Core_CenterRect(&on, self.right - self.left, self.bottom - self.top, &monitor.rcWork, &placed);
    SetWindowPos(dialog, NULL, placed.left, placed.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

typedef struct DialogShell {
    DLGPROC proc;
    LPARAM  param;
    HICON   bigIcon, smallIcon;   /* its own, with a taskbar button of its own */
} DialogShell;

/* The program's icon at the dialog's scale, for its title bar and taskbar
 * button; the shell frees them once the dialog is gone. */
static void SetDialogIcons(HWND dialog, DialogShell *shell)
{
    UINT dpi = GetDpiForWindow(dialog);
    HICON bigIcon = NULL, smallIcon = NULL;
    LoadIconWithScaleDown(g_hInst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi),
                          &bigIcon);
    LoadIconWithScaleDown(g_hInst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi),
                          &smallIcon);
    SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    if (shell->bigIcon) DestroyIcon(shell->bigIcon);
    if (shell->smallIcon) DestroyIcon(shell->smallIcon);
    shell->bigIcon = bigIcon;
    shell->smallIcon = smallIcon;
}

/* A dialog whose owner is hidden (the manager started only to uninstall) gets
 * its own taskbar button and icon, so it cannot get lost behind other windows. */
static void TaskbarButtonIfOwnerHidden(HWND dialog, DialogShell *shell)
{
    HWND owner = GetWindow(dialog, GW_OWNER);
    if (owner && IsWindowVisible(owner)) return;
    SetWindowLongPtrW(dialog, GWL_EXSTYLE, GetWindowLongPtrW(dialog, GWL_EXSTYLE) | WS_EX_APPWINDOW);
    SetDialogIcons(dialog, shell);
}

/* What every dialog shares, around its own procedure (see Ui_Dialog). */
static INT_PTR CALLBACK ShellProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    DialogShell *shell = (DialogShell *)GetPropW(dialog, SHELL_PROP);
    INT_PTR result;
    if (msg == WM_INITDIALOG) {
        shell = (DialogShell *)lp;
        SetPropW(dialog, SHELL_PROP, shell);
        Localize_Window(dialog);
        ApplyDialogFont(dialog);
        result = shell->proc(dialog, msg, wp, shell->param);   /* laid out and filled first: then its size is known */
        Theme_FitDialog(dialog);
        CenterOnOwner(dialog);
        TaskbarButtonIfOwnerHidden(dialog, shell);
        Theme_Apply(dialog);
        return result;
    }
    if (!shell) return FALSE;
    if (msg == WM_DPICHANGED) PostMessageW(dialog, WM_THEME_DIALOG_LAYOUT, 0, 0);
    if (msg == WM_THEME_DIALOG_LAYOUT) {
        Theme_FitDialog(dialog);
        Theme_Apply(dialog);
        if (shell->bigIcon || shell->smallIcon) SetDialogIcons(dialog, shell);
        return TRUE;
    }
    if (msg == WM_SETTINGCHANGE || msg == WM_SYSCOLORCHANGE) Theme_Follow(dialog, msg, wp, lp);
    result = shell->proc(dialog, msg, wp, lp);
    if (!result && msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) result = Theme_CtlColor(msg, wp, lp, 0);
    if (msg == WM_DESTROY) Localize_ForgetWindow(dialog);   /* its controls are still there */
    /* DialogSubclass has already freed the theme's state. */
    if (msg == WM_NCDESTROY) RemovePropW(dialog, SHELL_PROP);
    return result;
}

/* Every dialog of the program opens here, modal to `owner`, and so gets what
 * they all share: once `proc` has set it up (WM_INITDIALOG brings `param`),
 * it is centered on its owner (on its monitor, with a taskbar button of its
 * own, when the owner is hidden) and themed; its colors are the theme's
 * unless `proc` answers WM_CTLCOLOR* itself; it follows theme changes. */
INT_PTR Ui_Dialog(HWND owner, int id, DLGPROC proc, LPARAM param)
{
    DialogShell shell;
    INT_PTR result;
    ZeroMemory(&shell, sizeof shell);
    shell.proc = proc;
    shell.param = param;
    result = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(id), owner, ShellProc, (LPARAM)&shell);
    if (result == -1) Util_Log(L"theme: dialog %d cannot be shown (error %lu)", id, GetLastError());
    if (shell.bigIcon) DestroyIcon(shell.bigIcon);
    if (shell.smallIcon) DestroyIcon(shell.smallIcon);
    return result;
}

/* ------------------------------------------------------------ message box */

typedef struct MessageBoxState {
    const WCHAR *text;
    const WCHAR *ok;
    const WCHAR *cancel;   /* NULL: single button */
    LPCWSTR      icon;     /* IDI_* or NULL */
    BOOL         defaultCancel;
    HICON        hicon;
} MessageBoxState;

#define MESSAGE_ICON_DIPS 32

/* A single button takes the second one's place. Theme_FitDialog grows the
 * text's row for the message (its label wraps as an edit does) and moves the
 * buttons below it. */
static void LayoutMessage(HWND dialog, const MessageBoxState *state)
{
    HWND ok = GetDlgItem(dialog, IDOK), cancel = GetDlgItem(dialog, IDCANCEL);
    RECT slot;
    if (state->cancel || !GetWindowRect(cancel, &slot)) return;
    MapWindowPoints(NULL, dialog, (POINT *)&slot, 2);
    ShowWindow(cancel, SW_HIDE);
    SetWindowPos(ok, NULL, slot.left, slot.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void SetMessageIcon(HWND dialog, MessageBoxState *state)
{
    int pixels = ScaleForWindow(dialog, MESSAGE_ICON_DIPS);
    HICON icon = NULL;
    if (!state->icon || FAILED(LoadIconWithScaleDown(NULL, state->icon, pixels, pixels, &icon))) return;
    SendDlgItemMessageW(dialog, IDC_M_ICON, STM_SETICON, (WPARAM)icon, 0);
    if (state->hicon) DestroyIcon(state->hicon);
    state->hicon = icon;
}

static INT_PTR CALLBACK MessageProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    MessageBoxState *state = (MessageBoxState *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        state = (MessageBoxState *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, APP_NAME);
        SetDlgItemTextW(dialog, IDC_M_TEXT, state->text);
        SetDlgItemTextW(dialog, IDOK, state->ok);
        if (state->cancel) SetDlgItemTextW(dialog, IDCANCEL, state->cancel);
        SetMessageIcon(dialog, state);
        LayoutMessage(dialog, state);
        SetForegroundWindow(dialog);   /* a question is always in front, even when its owner is not */
        if (state->cancel && state->defaultCancel) {
            SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
            SetFocus(GetDlgItem(dialog, IDCANCEL));
            return FALSE;
        }
        return TRUE;
    case WM_DPICHANGED:
        PostMessageW(dialog, WM_THEME_MESSAGE_DPI, 0, 0);
        break;
    case WM_THEME_MESSAGE_DPI:
        if (state) SetMessageIcon(dialog, state);
        return TRUE;
    case WM_COMMAND:
        if (state && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
            EndDialog(dialog, LOWORD(wp) == IDCANCEL && !state->cancel ? IDOK : LOWORD(wp));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (state && state->hicon) DestroyIcon(state->hicon);
        break;
    }
    return FALSE;
}

/* A themed message box with translated button labels (`cancel` NULL for none).
 * Returns TRUE when the user picked the first button. */
BOOL Ui_Ask(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel)
{
    MessageBoxState state;
    ZeroMemory(&state, sizeof state);
    state.text = text;
    state.ok = ok;
    state.cancel = cancel;
    state.icon = icon;
    state.defaultCancel = defaultCancel;
    return Ui_Dialog(owner, IDD_MESSAGE, MessageProc, (LPARAM)&state) == IDOK;
}

/* MessageBox-compatible wrapper: MB_OK, MB_OKCANCEL or MB_YESNO, an MB_ICON*
 * and optionally MB_DEFBUTTON2. `format` is translated already. */
int Ui_Message(HWND owner, UINT flags, const WCHAR *format, ...)
{
    WCHAR text[2048];
    LPCWSTR icon = NULL;
    va_list arguments;
    UINT buttons = flags & MB_TYPEMASK;
    BOOL first;

    va_start(arguments, format);
    StringCchVPrintfW(text, ARRAYSIZE(text), format, arguments);
    va_end(arguments);
    switch (flags & MB_ICONMASK) {
    case MB_ICONERROR:       icon = IDI_ERROR; break;
    case MB_ICONWARNING:     icon = IDI_WARNING; break;
    case MB_ICONINFORMATION: icon = IDI_INFORMATION; break;
    case MB_ICONQUESTION:    icon = IDI_QUESTION; break;
    }
    if (buttons == MB_YESNO) {
        first = Ui_Ask(owner, icon, text, TR(L"Yes"), TR(L"No"), (flags & MB_DEFMASK) == MB_DEFBUTTON2);
        return first ? IDYES : IDNO;
    }
    if (buttons == MB_OKCANCEL) {
        first = Ui_Ask(owner, icon, text, TR(L"OK"), TR(L"Cancel"), (flags & MB_DEFMASK) == MB_DEFBUTTON2);
        return first ? IDOK : IDCANCEL;
    }
    Ui_Ask(owner, icon, text, TR(L"OK"), NULL, FALSE);
    return IDOK;
}
