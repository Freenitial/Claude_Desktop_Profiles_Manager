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
 * What a window draws itself (the sessions view) takes its
 * colors, fonts, rows and buttons from here (Theme_Color, Theme_CreateFonts,
 * Theme_DrawRow, Theme_DrawButton, Theme_DrawDropDown), so that every list,
 * selection, button and drop-down looks the same.
 *
 * Dark mode for Win32 controls: the title bar uses the documented
 * DWMWA_USE_IMMERSIVE_DARK_MODE; the controls use the DarkMode_* visual-style
 * classes, enabled through two uxtheme exports that Windows only exposes by
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

#define THEME_PROP L"ClaudeDesktopProfilesManager.Theme"   /* 1 light, 2 dark: how a window was themed */

typedef int (WINAPI *SetPreferredAppModeFn)(int mode);      /* uxtheme #135 */
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL); /* uxtheme #133 */
typedef void (WINAPI *FlushMenuThemesFn)(void);              /* uxtheme #136 */

static AllowDarkModeForWindowFn g_allowDark;
static FlushMenuThemesFn g_flushMenus;
static BOOL g_dark;

/* ---------------------------------------------------------------- palette */

typedef struct Palette {
    COLORREF color[THEME_COLORS];
    COLORREF selectedCorner, hotCorner;   /* a row's softened corner pixels */
    COLORREF button, buttonOff;           /* a dark push button's fill; a disabled one's frame */
    COLORREF header, divider;             /* a dark list header's background and dividers */
} Palette;

/* Dark: the colors of Explorer's dark theme. The three blues and the
 * corners are those of a list view row there (DarkMode_Explorer::ListView)
 * over the field: selected, its frame, under the mouse. They are read from
 * Windows' theme at start (ReadRowColors), so the rows drawn here match the
 * list views on every Windows version; these are Windows 11's, kept when the
 * theme cannot be read. tests/test_theme.c compares the rows with Windows'. */
static const Palette kDark = {
    { RGB(0xF2, 0xF2, 0xF2),    /* text */
      RGB(0x9A, 0x9A, 0x9A),    /* muted */
      RGB(0x20, 0x20, 0x20),    /* face */
      RGB(0x2B, 0x2B, 0x2B),    /* field */
      RGB(0x22, 0x3E, 0x55),    /* main blue */
      RGB(0x00, 0x78, 0xD4),    /* bright blue */
      RGB(0x27, 0x35, 0x41) },  /* pale blue */
    RGB(0x22, 0x3A, 0x4C), RGB(0x29, 0x2E, 0x32),
    RGB(0x33, 0x33, 0x33), RGB(0x40, 0x40, 0x40), RGB(0x19, 0x19, 0x19), RGB(0x63, 0x63, 0x63)
};

/* Light: the same from Explorer's light list view (Explorer::ListView). */
#define LIGHT_MUTED           RGB(0x6E, 0x6E, 0x6E)
#define LIGHT_MAIN_BLUE       RGB(0xCC, 0xE8, 0xFF)
#define LIGHT_PALE_BLUE       RGB(0xE5, 0xF3, 0xFF)
#define LIGHT_SELECTED_CORNER RGB(0xCC, 0xE4, 0xF6)
#define LIGHT_HOT_CORNER      RGB(0xF6, 0xFB, 0xFF)

static Palette g_palette;
static HBRUSH  g_brush[THEME_COLORS];

static BOOL HighContrast(void)
{
    HIGHCONTRASTW hc;
    ZeroMemory(&hc, sizeof hc);
    hc.cbSize = sizeof hc;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

/* Dark only when the user picked dark apps and no contrast theme is on: a
 * contrast theme's system colors always win. */
static BOOL SystemPrefersDark(void)
{
    DWORD light = 1;
    if (HighContrast()) return FALSE;
    Util_RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", &light);
    return light == 0;
}

static void Fill(HDC dc, const RECT *rc, COLORREF color);

/* The row colors as this Windows draws a list view row over the field:
 * selected (fill, frame, corner) and under the mouse (fill, corner). The
 * theme's row images are translucent, so they are drawn over the field and
 * read back. Keeps the measured values when the theme is not there. */
static void ReadRowColors(Palette *p, const WCHAR *themeClass)
{
    enum { W = 24, H = 12 };
    BITMAPINFO bi;
    HTHEME theme = OpenThemeData(NULL, themeClass);
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = NULL;
    HGDIOBJ old;
    void *bits = NULL;
    const DWORD *px;
    RECT rc = { 0, 0, W, H };
    int pass;
    if (theme && dc) {
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    }
    if (bitmap) {
        old = SelectObject(dc, bitmap);
        px = (const DWORD *)bits;
        for (pass = 0; pass < 2; pass++) {
            int state = pass == 0 ? LISS_SELECTED : LISS_HOT;
            Fill(dc, &rc, p->color[THEME_FIELD]);
            if (FAILED(DrawThemeBackground(theme, dc, LVP_LISTITEM, state, &rc, NULL))) break;
            GdiFlush();
#define PX(x, y) RGB((px[(y) * W + (x)] >> 16) & 0xFF, (px[(y) * W + (x)] >> 8) & 0xFF, px[(y) * W + (x)] & 0xFF)
            if (pass == 0) {
                p->color[THEME_MAIN_BLUE] = PX(W / 2, H / 2);
                p->color[THEME_BRIGHT_BLUE] = PX(W / 2, 0);
                p->selectedCorner = PX(0, 0);
            } else {
                p->color[THEME_PALE_BLUE] = PX(W / 2, H / 2);
                p->hotCorner = PX(0, 0);
            }
#undef PX
        }
        SelectObject(dc, old);
        DeleteObject(bitmap);
    }
    if (dc) DeleteDC(dc);
    if (theme) CloseThemeData(theme);
}

static void UpdatePalette(void)
{
    int i;
    if (g_dark) {
        g_palette = kDark;
        ReadRowColors(&g_palette, L"DarkMode_Explorer::ListView");
    } else {
        BOOL hc = HighContrast();
        ZeroMemory(&g_palette, sizeof g_palette);
        g_palette.color[THEME_TEXT] = GetSysColor(COLOR_WINDOWTEXT);
        g_palette.color[THEME_MUTED] = hc ? GetSysColor(COLOR_GRAYTEXT) : LIGHT_MUTED;
        g_palette.color[THEME_FACE] = GetSysColor(COLOR_3DFACE);
        g_palette.color[THEME_FIELD] = GetSysColor(COLOR_WINDOW);
        g_palette.color[THEME_MAIN_BLUE] = hc ? GetSysColor(COLOR_HIGHLIGHT) : LIGHT_MAIN_BLUE;
        g_palette.color[THEME_BRIGHT_BLUE] = hc ? GetSysColor(COLOR_HIGHLIGHT) : kDark.color[THEME_BRIGHT_BLUE];
        g_palette.color[THEME_PALE_BLUE] = hc ? GetSysColor(COLOR_WINDOW) : LIGHT_PALE_BLUE;
        g_palette.selectedCorner = LIGHT_SELECTED_CORNER;
        g_palette.hotCorner = LIGHT_HOT_CORNER;
        if (!hc) ReadRowColors(&g_palette, L"Explorer::ListView");
    }
    for (i = 0; i < THEME_COLORS; i++) {
        if (g_brush[i]) DeleteObject(g_brush[i]);
        g_brush[i] = CreateSolidBrush(g_palette.color[i]);
    }
}

void Theme_Init(void)
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux) {
        SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        g_allowDark = (AllowDarkModeForWindowFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        g_flushMenus = (FlushMenuThemesFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (setMode) setMode(1); /* "allow dark": follow the system setting */
        if (g_flushMenus) g_flushMenus();
    }
    g_dark = SystemPrefersDark();
    UpdatePalette();
}

BOOL Theme_IsDark(void) { return g_dark; }

COLORREF Theme_Color(ThemeColor c)
{
    return c >= 0 && c < THEME_COLORS ? g_palette.color[c] : g_palette.color[THEME_TEXT];
}

HBRUSH Theme_Brush(ThemeColor c)
{
    return c >= 0 && c < THEME_COLORS ? g_brush[c] : g_brush[THEME_FIELD];
}

static void Fill(HDC dc, const RECT *rc, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

/* ------------------------------------------------------------------- rows */

static void Corners(HDC dc, const RECT *rc, COLORREF corner)
{
    if (rc->right - rc->left < 3 || rc->bottom - rc->top < 3) return;
    SetPixelV(dc, rc->left, rc->top, corner);
    SetPixelV(dc, rc->right - 1, rc->top, corner);
    SetPixelV(dc, rc->left, rc->bottom - 1, corner);
    SetPixelV(dc, rc->right - 1, rc->bottom - 1, corner);
}

/* A row of a list a window draws itself, as the list views draw theirs:
 * selected, a main blue fill in a bright blue frame; under the mouse, pale
 * blue; corners softened; else `around`, the list's background. Returns the
 * color for its text. */
COLORREF Theme_DrawRow(HDC dc, const RECT *rc, UINT state, COLORREF around)
{
    BOOL hc = HighContrast();
    if (state & THEME_ROW_SELECTED) {
        Fill(dc, rc, g_palette.color[THEME_MAIN_BLUE]);
        if (!hc) {
            FrameRect(dc, rc, g_brush[THEME_BRIGHT_BLUE]);
            Corners(dc, rc, g_palette.selectedCorner);
        }
        return hc ? GetSysColor(COLOR_HIGHLIGHTTEXT) : g_palette.color[THEME_TEXT];
    }
    if ((state & THEME_ROW_HOT) && !hc) {
        Fill(dc, rc, g_palette.color[THEME_PALE_BLUE]);
        Corners(dc, rc, g_palette.hotCorner);
    } else {
        Fill(dc, rc, around);
    }
    return g_palette.color[THEME_TEXT];
}

/* Secondary text on such a row: grey, but on a dark or contrast selection
 * the row's own text color, which reads better there. */
COLORREF Theme_RowMuted(UINT state)
{
    if ((state & THEME_ROW_SELECTED) && HighContrast()) return GetSysColor(COLOR_HIGHLIGHTTEXT);
    if ((state & THEME_ROW_SELECTED) && g_dark) return g_palette.color[THEME_TEXT];
    return g_palette.color[THEME_MUTED];
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

/* The fonts of what a window draws itself, at the size of its dialog font
 * (so at its scale). Free them with Theme_FreeFonts. */
void Theme_CreateFonts(HWND dlg, ThemeFonts *fonts)
{
    static const struct { int weight, percent; BOOL underline, strike, italic; } kRoles[THEME_FONTS] = {
        { FW_NORMAL, 105, FALSE, FALSE, FALSE },     /* text */
        { FW_SEMIBOLD, 105, FALSE, FALSE, FALSE },   /* strong */
        { FW_SEMIBOLD, 120, FALSE, FALSE, FALSE },   /* heading */
        { FW_SEMIBOLD, 105, TRUE, FALSE, FALSE },    /* current */
        { FW_NORMAL, 105, FALSE, TRUE, FALSE },      /* absent */
        { FW_NORMAL, 105, FALSE, FALSE, TRUE },      /* italic */
    };
    HFONT dialog = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
    BOOL variable = HasVariableFont();
    LOGFONTW base, lf;
    int i;
    Theme_FreeFonts(fonts);
    if (!dialog || !GetObjectW(dialog, sizeof base, &base)) {
        ZeroMemory(&base, sizeof base);
        base.lfHeight = -MulDiv(9, (int)GetDpiForWindow(dlg), 72);
        StringCchCopyW(base.lfFaceName, ARRAYSIZE(base.lfFaceName), L"Segoe UI");
    }
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
                           kRoles[i].weight >= FW_SEMIBOLD ? L"Segoe UI Variable Text Semibold" : L"Segoe UI Variable Text");
        fonts->font[i] = CreateFontIndirectW(&lf);
    }
}

#define STRONG_PROP L"ClaudeDesktopProfilesManager.Strong"   /* the semibold font a control shows its text in */

/* `font`, semibold: in the semibold of the variable font when Windows has
 * it (as the strong texts the program draws), Segoe UI's own semibold
 * reads heavy and blurred next to the regular buttons. */
static HFONT StrongOf(HFONT font)
{
    LOGFONTW lf;
    if (!font || !GetObjectW(font, sizeof lf, &lf)) return NULL;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (HasVariableFont()) StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI Variable Text Semibold");
    return CreateFontIndirectW(&lf);
}

/* A control's text semibold (a button that leads to another view): its font
 * made semibold now, and again whenever the dialog gives it a new one (at a
 * new scale, see ChildSubclass). */
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
HDC Theme_BufferBegin(ThemeBuffer *b, HDC target, const RECT *rc)
{
    ZeroMemory(b, sizeof *b);
    b->target = target;
    b->rc = *rc;
    if ((b->dc = CreateCompatibleDC(target)) != NULL)
        b->bitmap = CreateCompatibleBitmap(target, max(1, rc->right - rc->left), max(1, rc->bottom - rc->top));
    if (!b->bitmap) {
        if (b->dc) DeleteDC(b->dc);
        b->dc = NULL;
        return target;
    }
    b->old = SelectObject(b->dc, b->bitmap);
    SetViewportOrgEx(b->dc, -rc->left, -rc->top, NULL);
    return b->dc;
}

void Theme_BufferEnd(ThemeBuffer *b)
{
    if (!b->dc) return;
    SetViewportOrgEx(b->dc, 0, 0, NULL);
    BitBlt(b->target, b->rc.left, b->rc.top, b->rc.right - b->rc.left, b->rc.bottom - b->rc.top, b->dc, 0, 0, SRCCOPY);
    SelectObject(b->dc, b->old);
    DeleteObject(b->bitmap);
    DeleteDC(b->dc);
    b->dc = NULL;
}

/* ---------------------------------------------------------- theme changes */

/* WM_SETTINGCHANGE / WM_SYSCOLORCHANGE in any of our dialogs: re-read the
 * setting (app mode, contrast theme) and re-theme that window if it was
 * themed with the other mode. Every open dialog gets the broadcast. */
void Theme_Follow(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR mode;
    BOOL reread = msg == WM_SYSCOLORCHANGE || wp == SPI_SETHIGHCONTRAST ||
                  (lp && CompareStringOrdinal((const WCHAR *)lp, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL);
    if (!reread) return;
    g_dark = SystemPrefersDark();
    UpdatePalette();
    if (g_flushMenus) g_flushMenus();
    mode = (INT_PTR)GetPropW(dlg, THEME_PROP);
    if (mode != (g_dark ? 2 : 1) || msg == WM_SYSCOLORCHANGE) Theme_Apply(dlg);
}

void Theme_Forget(HWND dlg)
{
    RemovePropW(dlg, THEME_PROP);
}

static void TitleBar(HWND hwnd)
{
    BOOL on = g_dark;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on)))
        DwmSetWindowAttribute(hwnd, 19 /* same attribute before Windows 10 2004 */, &on, sizeof on);
}

static BOOL IsClass(HWND h, const WCHAR *cls)
{
    WCHAR name[256];   /* the longest a class name can be */
    return GetClassNameW(h, name, ARRAYSIZE(name)) && CompareStringOrdinal(name, -1, cls, -1, TRUE) == CSTR_EQUAL;
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

static BOOL Gdip(void)
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

static DWORD Argb(COLORREF c)
{
    return 0xFF000000u | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
}

/* `fill` inside a `width` px `frame`, corners of `radius` px, within `rc`. */
static void RoundedBox(HDC dc, const RECT *rc, int radius, COLORREF fill, COLORREF frame, int width)
{
    void *graphics = NULL, *path = NULL, *brush = NULL, *pen = NULL;
    float inset = (width - 1) / 2.0f, d = 2.0f * radius;
    float x0 = rc->left + inset, y0 = rc->top + inset, x1 = rc->right - 1 - inset, y1 = rc->bottom - 1 - inset;
    if (Gdip() && g_gdip.createFromHdc(dc, &graphics) == 0) {
        g_gdip.setSmoothing(graphics, GDIP_SMOOTHING_ANTIALIAS);
        if (g_gdip.createPath(0, &path) == 0) {
            g_gdip.addArc(path, x0, y0, d, d, 180.0f, 90.0f);
            g_gdip.addArc(path, x1 - d, y0, d, d, 270.0f, 90.0f);
            g_gdip.addArc(path, x1 - d, y1 - d, d, d, 0.0f, 90.0f);
            g_gdip.addArc(path, x0, y1 - d, d, d, 90.0f, 90.0f);
            g_gdip.closeFigure(path);
            if (g_gdip.createFill(Argb(fill), &brush) == 0) g_gdip.fillPath(graphics, brush, path);
            if (g_gdip.createPen(Argb(frame), (float)width, GDIP_UNIT_PIXEL, &pen) == 0) g_gdip.drawPath(graphics, pen, path);
            if (brush) g_gdip.deleteBrush(brush);
            if (pen) g_gdip.deletePen(pen);
            g_gdip.deletePath(path);
        }
        g_gdip.deleteGraphics(graphics);
    } else {
        HBRUSH b = CreateSolidBrush(fill);
        HPEN p = CreatePen(PS_INSIDEFRAME, width, frame);
        HGDIOBJ oldBrush = SelectObject(dc, b), oldPen = SelectObject(dc, p);
        RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, 2 * radius, 2 * radius);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(b);
        DeleteObject(p);
    }
}

/* ---------------------------------------------------- buttons, check boxes */

static LONG ButtonType(HWND h)
{
    return IsClass(h, WC_BUTTONW) ? (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) : -1;
}

static BOOL IsPushButton(HWND h)
{
    LONG type = ButtonType(h);
    return (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) && !(GetWindowLongW(h, GWL_STYLE) & (BS_ICON | BS_BITMAP));
}

static BOOL IsCheckBox(HWND h)
{
    LONG type = ButtonType(h);
    return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX;
}

/* Keyboard cues of a control: its accelerator underlines and focus rectangle. */
static UINT TextFormatForCues(HWND h, UINT format, BOOL *showFocus)
{
    UINT cues = (UINT)SendMessageW(h, WM_QUERYUISTATE, 0, 0);
    *showFocus = !(cues & UISF_HIDEFOCUS);
    return format | ((cues & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
}

/* A push button's face, for real buttons and for the ones a window draws
 * itself (the sessions view's details), so that they all look alike. Dark:
 * Explorer's dark button (its fill, 4 px corners) framed in main blue instead
 * of grey and white, the frame twice as thick on the default button (the one
 * Enter presses); under the mouse its fill is pale blue in a bright blue
 * frame, pressed it is all bright blue. Light: the theme's button. Leaves
 * the text color set for the label. */
static void ButtonFace(HWND owner, HDC dc, const RECT *rc, UINT state)
{
    BOOL enabled = !(state & THEME_BUTTON_DISABLED);
    BOOL pressed = enabled && (state & THEME_BUTTON_PRESSED), hot = enabled && (state & THEME_BUTTON_HOT);
    if (g_dark) {
        int dpi = (int)GetDpiForWindow(owner), line = max(1, MulDiv(1, dpi, 96));
        COLORREF fill = pressed ? g_palette.color[THEME_BRIGHT_BLUE] : hot ? g_palette.color[THEME_PALE_BLUE] : g_palette.button;
        COLORREF frame = !enabled ? g_palette.buttonOff : (pressed || hot) ? g_palette.color[THEME_BRIGHT_BLUE] : g_palette.color[THEME_MAIN_BLUE];
        FillRect(dc, rc, g_brush[THEME_FACE]);
        RoundedBox(dc, rc, MulDiv(4, dpi, 96), fill, frame, enabled && (state & THEME_BUTTON_DEFAULT) ? 2 * line : line);
        SetTextColor(dc, enabled ? g_palette.color[THEME_TEXT] : g_palette.color[THEME_MUTED]);
    } else {
        HTHEME theme = OpenThemeData(owner, L"Button");
        int part = !enabled ? PBS_DISABLED : pressed ? PBS_PRESSED : hot ? PBS_HOT : (state & THEME_BUTTON_DEFAULT) ? PBS_DEFAULTED : PBS_NORMAL;
        FillRect(dc, rc, GetSysColorBrush(COLOR_3DFACE));
        if (theme) {
            DrawThemeBackground(theme, dc, BP_PUSHBUTTON, part, rc, NULL);
            CloseThemeData(theme);
        } else {
            RECT edge = *rc;
            DrawFrameControl(dc, &edge, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0) | (!enabled ? DFCS_INACTIVE : 0));
        }
        SetTextColor(dc, GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
    }
}

static void Label(HDC dc, const WCHAR *text, HFONT font, RECT *rc, UINT format)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, rc, format);
    SelectObject(dc, old);
}

/* A push button (see ButtonFace). `owner` gives the scale and the theme;
 * `format` is DrawText's. */
void Theme_DrawButton(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state, UINT format)
{
    RECT label = *rc;
    ButtonFace(owner, dc, rc, state);
    Label(dc, text, font, &label, format);
}

/* Where a drop-down box's arrow goes: at its right end, as wide as a
 * drop-down list's button. */
static RECT DropDownArrow(HWND owner, const RECT *rc)
{
    RECT arrow = *rc;
    UINT dpi = GetDpiForWindow(owner);
    arrow.left = rc->right - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - MulDiv(2, (int)dpi, 96);
    arrow.right -= MulDiv(2, (int)dpi, 96);
    return arrow;
}

/* A button that opens a menu (the sessions view's Actions): a push button
 * (see ButtonFace), `text` on its left and on its right the arrow the
 * drop-down lists of the same theme show. */
void Theme_DrawDropDown(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state)
{
    /* Not through `owner`: a window with a theme name of its own (a dialog's
     * drop-down list) would not find the class. */
    HTHEME theme = OpenThemeDataForDpi(NULL, g_dark ? L"DarkMode_CFD::Combobox" : L"Combobox", GetDpiForWindow(owner));
    RECT arrow = DropDownArrow(owner, rc), label = *rc;
    BOOL enabled = !(state & THEME_BUTTON_DISABLED);
    ButtonFace(owner, dc, rc, state);
    if (theme) {
        DrawThemeBackground(theme, dc, CP_DROPDOWNBUTTONRIGHT, enabled ? CBXSR_NORMAL : CBXSR_DISABLED, &arrow, NULL);
        CloseThemeData(theme);
    } else {
        COLORREF ink = GetTextColor(dc);   /* the arrow in the label's color */
        HGDIOBJ old;
        HFONT marlett = CreateFontW(-(arrow.bottom - arrow.top) / 2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SYMBOL_CHARSET, 0, 0, 0, 0, L"Marlett");
        old = SelectObject(dc, marlett);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        DrawTextW(dc, L"u", 1, &arrow, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);   /* Marlett's down arrow */
        SelectObject(dc, old);
        if (marlett) DeleteObject(marlett);
    }
    label.left += MulDiv(8, (int)GetDpiForWindow(owner), 96);
    label.right = arrow.left;
    Label(dc, text, font, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

/* A dark push button (see Theme_DrawButton), with its focus rectangle. */
static LRESULT ButtonCustomDraw(const NMCUSTOMDRAW *cd)
{
    WCHAR text[128];
    HWND b = cd->hdr.hwndFrom;
    int dpi = (int)GetDpiForWindow(b), inset;
    BOOL showFocus;
    UINT format, state = 0;
    RECT label = cd->rc;
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    if (!IsWindowEnabled(b)) state |= THEME_BUTTON_DISABLED;
    if (cd->uItemState & CDIS_SELECTED) state |= THEME_BUTTON_PRESSED;
    if (cd->uItemState & CDIS_HOT) state |= THEME_BUTTON_HOT;
    if ((cd->uItemState & CDIS_DEFAULT) || (GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON) state |= THEME_BUTTON_DEFAULT;
    GetWindowTextW(b, text, ARRAYSIZE(text));
    format = TextFormatForCues(b, DT_SINGLELINE | DT_CENTER | DT_VCENTER, &showFocus);
    Theme_DrawButton(b, cd->hdc, &cd->rc, text, (HFONT)SendMessageW(b, WM_GETFONT, 0, 0), state, format);
    if ((cd->uItemState & CDIS_FOCUS) && showFocus) {
        inset = MulDiv(3, dpi, 96) + 2 * max(1, MulDiv(1, dpi, 96));
        InflateRect(&label, -inset, -inset);
        SetTextColor(cd->hdc, g_palette.color[THEME_TEXT]);
        SetBkColor(cd->hdc, g_palette.button);
        DrawFocusRect(cd->hdc, &label);
    }
    return CDRF_SKIPDEFAULT;
}

/* A dark check box: a themed one draws its caption in black, so the glyph
 * comes from the dark theme and the caption and focus rectangle from here. */
static LRESULT CheckBoxCustomDraw(const NMCUSTOMDRAW *cd)
{
    HWND button = cd->hdr.hwndFrom;
    WCHAR text[128];
    RECT box, label, focus;
    SIZE glyph;
    HTHEME theme;
    HGDIOBJ old;
    BOOL checked, enabled, showFocus;
    UINT format;
    int state;

    if (cd->dwDrawStage != CDDS_PREPAINT || (theme = OpenThemeData(button, L"Button")) == NULL) return CDRF_DODEFAULT;
    checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
    enabled = IsWindowEnabled(button);
    if (!enabled)                             state = checked ? CBS_CHECKEDDISABLED : CBS_UNCHECKEDDISABLED;
    else if (cd->uItemState & CDIS_SELECTED)  state = checked ? CBS_CHECKEDPRESSED : CBS_UNCHECKEDPRESSED;
    else if (cd->uItemState & CDIS_HOT)       state = checked ? CBS_CHECKEDHOT : CBS_UNCHECKEDHOT;
    else                                      state = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
    glyph.cx = glyph.cy = MulDiv(13, (int)GetDpiForWindow(button), 96);
    GetThemePartSize(theme, cd->hdc, BP_CHECKBOX, state, NULL, TS_DRAW, &glyph);

    FillRect(cd->hdc, &cd->rc, g_brush[THEME_FACE]);
    box.left = cd->rc.left;
    box.top = cd->rc.top + (cd->rc.bottom - cd->rc.top - glyph.cy) / 2;
    box.right = box.left + glyph.cx;
    box.bottom = box.top + glyph.cy;
    DrawThemeBackground(theme, cd->hdc, BP_CHECKBOX, state, &box, NULL);
    CloseThemeData(theme);

    GetWindowTextW(button, text, ARRAYSIZE(text));
    format = TextFormatForCues(button, DT_SINGLELINE | DT_LEFT | DT_NOCLIP, &showFocus);
    old = SelectObject(cd->hdc, (HFONT)SendMessageW(button, WM_GETFONT, 0, 0));
    label = cd->rc;
    label.left = box.right + MulDiv(4, (int)GetDpiForWindow(button), 96);
    focus = label;
    DrawTextW(cd->hdc, text, -1, &focus, format | DT_CALCRECT);
    OffsetRect(&focus, 0, ((cd->rc.bottom - cd->rc.top) - (focus.bottom - focus.top)) / 2);
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, enabled ? g_palette.color[THEME_TEXT] : g_palette.color[THEME_MUTED]);
    DrawTextW(cd->hdc, text, -1, &focus, format);
    if ((cd->uItemState & CDIS_FOCUS) && showFocus) {
        InflateRect(&focus, 1, 0);
        SetTextColor(cd->hdc, g_palette.color[THEME_TEXT]);
        DrawFocusRect(cd->hdc, &focus);
    }
    SelectObject(cd->hdc, old);
    return CDRF_SKIPDEFAULT;
}

/* ------------------------------------------------------------ list views */

/* Rows of a dark list view: text colors, and the selection's text
 * background (the theme paints the row itself). */
/* A last column that reaches the list's right edge (where the scroll bar
 * starts) is not closed by a line: a dark row draws one two pixels short of
 * the edge, and a selected row's frame then stands a pixel past it. The line
 * gives way to the row's own fill; the header has no divider there either
 * (HeaderCustomDraw). */
static void OpenLastColumn(HWND list, int i, HDC dc)
{
    RECT row, client, line;
    COLORREF fill = g_palette.color[THEME_FIELD];
    BOOL plain = TRUE;
    if (!ListView_GetItemRect(list, i, &row, LVIR_BOUNDS) || !GetClientRect(list, &client) || row.right < client.right) return;
    if (ListView_GetItemState(list, i, LVIS_SELECTED)) {
        fill = g_palette.color[THEME_MAIN_BLUE];
        plain = FALSE;
    } else if (ListView_GetHotItem(list) == i) {
        fill = g_palette.color[THEME_PALE_BLUE];
        plain = FALSE;
    }
    SetRect(&line, client.right - 2, row.top + (plain ? 0 : 1), client.right - (plain ? 0 : 1), row.bottom - (plain ? 0 : 1));
    Fill(dc, &line, fill);   /* a framed row keeps its frame on the edge pixel */
}

static LRESULT ListCustomDraw(NMLVCUSTOMDRAW *cd)
{
    HWND list = cd->nmcd.hdr.hwndFrom;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
        OpenLastColumn(list, (int)cd->nmcd.dwItemSpec, cd->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    if ((cd->nmcd.dwDrawStage & ~(DWORD)CDDS_SUBITEM) != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    if (ListView_GetItemState(list, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED)) {
        cd->nmcd.uItemState &= ~(UINT)(CDIS_SELECTED | CDIS_FOCUS);
        cd->clrTextBk = g_palette.color[THEME_MAIN_BLUE];
    } else {
        cd->clrTextBk = g_palette.color[THEME_FIELD];
    }
    cd->clrText = g_palette.color[THEME_TEXT];
    return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
}

/* A dark list view's header: its labels come out black and its dividers a
 * pixel right of the column lines the rows draw (on each column's last pixel
 * but one), so every item is drawn here, its divider on the rows' line. */
static LRESULT HeaderCustomDraw(const NMCUSTOMDRAW *cd)
{
    WCHAR text[128];
    HDITEMW item;
    HWND header = cd->hdr.hwndFrom;
    RECT rc = cd->rc, divider, client;
    HGDIOBJ old;
    int pad = MulDiv(7, (int)GetDpiForWindow(header), 96);
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    ZeroMemory(&item, sizeof item);
    item.mask = HDI_TEXT;
    item.pszText = text;
    item.cchTextMax = ARRAYSIZE(text);
    text[0] = 0;
    Header_GetItem(header, (int)cd->dwItemSpec, &item);
    Fill(cd->hdc, &rc, g_palette.header);
    GetClientRect(header, &client);
    if (rc.right < client.right) {   /* none on the edge (see OpenLastColumn) */
        divider = rc;
        divider.left = rc.right - 2;
        divider.right = rc.right - 1;
        Fill(cd->hdc, &divider, g_palette.divider);
    }
    rc.left += pad;
    rc.right -= pad;
    old = SelectObject(cd->hdc, (HFONT)SendMessageW(header, WM_GETFONT, 0, 0));
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, g_palette.color[THEME_TEXT]);
    DrawTextW(cd->hdc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(cd->hdc, old);
    return CDRF_SKIPDEFAULT;
}

/* A selected row keeps its look under the mouse; the header of a dark list
 * is drawn here. */
static LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    /* Over a selected row, the list is told the mouse is off its rows, so the
     * theme does not paint it hot. */
    if (msg == WM_MOUSEMOVE) {
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = (short)LOWORD(lp);
        hit.pt.y = (short)HIWORD(lp);
        if (ListView_HitTest(h, &hit) >= 0 && ListView_GetItemState(h, hit.iItem, LVIS_SELECTED))
            lp = MAKELPARAM(-1, -1);
    } else if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        /* The row clicked becomes selected under the mouse. */
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = (short)LOWORD(lp);
        hit.pt.y = (short)HIWORD(lp);
        if (ListView_HitTest(h, &hit) >= 0 && ListView_GetItemState(h, hit.iItem, LVIS_SELECTED))
            DefSubclassProc(h, WM_MOUSEMOVE, 0, MAKELPARAM(-1, -1));
        return r;
    } else if (msg == WM_NOTIFY && g_dark) {
        const NMHDR *n = (const NMHDR *)lp;
        if (n->hwndFrom == ListView_GetHeader(h) && n->code == NM_CUSTOMDRAW) return HeaderCustomDraw((const NMCUSTOMDRAW *)lp);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ListSubclass, id);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* ------------------------------------------------------- smooth scrolling */

/* The mouse wheel scrolls lists and trees smoothly, as a browser does: each
 * notch adds to the distance left, and a short animation covers it pixel by
 * pixel, easing out with time (Core_ScrollStep): a frame at once, then every
 * SCROLL_FRAME_MS while there is distance left. A control that moves by
 * whole rows (a tree, a list) takes the next row once the animation is half
 * way across it, so its rows come at the animation's pace. Each frame moves
 * the control at once (ScrollBy), never through the animation a list box
 * adds to its own steps. */
#define SCROLL_SUBCLASS  4
#define SCROLL_TIMER     0x5C01
#define SCROLL_FRAME_MS  USER_TIMER_MINIMUM

typedef struct SmoothScroll {
    int      pending;   /* px the animation has still to cover, down > 0 */
    int      owed;      /* px it covered that the control has not moved yet */
    int      rowPx;     /* a wheel line; 0: one scroll unit */
    int      dir;       /* 1 down, -1 up */
    BOOL     running, own;
    LONGLONG last;      /* when the last frame ran (performance counter) */
} SmoothScroll;

/* The height of one scroll position of the control. */
static int ScrollUnit(HWND h)
{
    RECT r;
    if (IsClass(h, WC_TREEVIEWW)) return TreeView_GetItemHeight(h);
    if (IsClass(h, WC_LISTBOXW)) return (int)SendMessageW(h, LB_GETITEMHEIGHT, 0, 0);
    if (IsClass(h, WC_LISTVIEWW) && ListView_GetItemCount(h) > 0 && ListView_GetItemRect(h, 0, &r, LVIR_BOUNDS))
        return r.bottom - r.top;
    return (GetWindowLongW(h, GWL_STYLE) & WS_VSCROLL) ? 1 : 0;   /* a window of ours, scrolled by the pixel */
}

static void ScrollStop(HWND h, SmoothScroll *s)
{
    if (s->running) KillTimer(h, SCROLL_TIMER);
    s->running = FALSE;
    s->pending = s->owed = 0;
}

static int ScrollPos(HWND h)
{
    SCROLLINFO si;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_POS;
    return GetScrollInfo(h, SB_VERT, &si) ? si.nPos : 0;
}

/* Whole rows closest to `px` (of `row` px each): none under half a row. */
static int Rows(int px, int row)
{
    return row > 0 ? (px + (px >= 0 ? row / 2 : -(row / 2))) / row : 0;
}

/* The height in px of the tree's row that would come into view next, down
 * (`down`) or up; 0 at an end. */
static int NextTreeRow(HWND h, BOOL down)
{
    HTREEITEM first = TreeView_GetFirstVisible(h), next;
    RECT rc;
    if (!first) return 0;
    next = down ? first : TreeView_GetPrevVisible(h, first);   /* going down, the top row leaves */
    return next && TreeView_GetItemRect(h, next, &rc, FALSE) ? rc.bottom - rc.top : 0;
}

/* A control that moved with its drawing off, drawn again whole, its scroll
 * bar with it. */
static void RedrawMoved(HWND h)
{
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

/* After the rows moved under a still mouse, the one under it is the one
 * shown under the mouse (not the one that was there). */
static void HoverUnderMouse(HWND h)
{
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !ScreenToClient(h, &pt) || !GetClientRect(h, &rc) || !PtInRect(&rc, pt)) return;
    SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
}

/* The control moved by about `px`, each its own way; returns the px it
 * moved (0: none, `*end` set when it could not). A list box animates its own
 * steps (some 400 ms each), and a tree scrolled on screen shifts its pixels
 * and repaints strips, its text glitching: both move with their drawing off,
 * then are drawn once. A tree takes each next row once `px` is half way
 * across it. A tree and a list view ignore a thumb position they did not
 * track. */
static int ScrollBy(HWND h, int px, BOOL *end)
{
    int unit = ScrollUnit(h), before = ScrollPos(h), moved = 0, rows, row;
    *end = FALSE;
    if (IsClass(h, WC_LISTBOXW)) {
        /* Its top row kept where its scroll bar can show it: a list box takes
         * a top row past its last page, and its scroll bar is then stuck. */
        SCROLLINFO si;
        int top = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0), target;
        if ((rows = Rows(px, unit)) == 0) return 0;
        ZeroMemory(&si, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_RANGE | SIF_PAGE;
        GetScrollInfo(h, SB_VERT, &si);
        target = max(0, min(top + rows, si.nMax - max(0, (int)si.nPage - 1)));
        if (target == top) {
            *end = TRUE;
            return 0;
        }
        SendMessageW(h, WM_SETREDRAW, FALSE, 0);
        SendMessageW(h, LB_SETTOPINDEX, (WPARAM)target, 0);
        RedrawMoved(h);
        HoverUnderMouse(h);
        return ((int)SendMessageW(h, LB_GETTOPINDEX, 0, 0) - top) * unit;
    } else if (IsClass(h, WC_LISTVIEWW)) {
        if ((rows = Rows(px, unit)) == 0) return 0;
        ListView_Scroll(h, 0, rows * unit);
    } else if (IsClass(h, WC_TREEVIEWW)) {
        BOOL off = FALSE;
        while ((row = NextTreeRow(h, px > 0)) > 0 && 2 * (px > 0 ? px - moved : moved - px) >= row) {   /* half a row still to go */
            HTREEITEM first = TreeView_GetFirstVisible(h);
            if (!off) SendMessageW(h, WM_SETREDRAW, FALSE, 0);
            off = TRUE;
            SendMessageW(h, WM_VSCROLL, px > 0 ? SB_LINEDOWN : SB_LINEUP, 0);
            if (TreeView_GetFirstVisible(h) == first) {
                row = 0;   /* at an end */
                break;
            }
            moved += px > 0 ? row : -row;
        }
        if (off) RedrawMoved(h);
        if (moved) HoverUnderMouse(h);
        if (moved == 0 && row == 0) *end = TRUE;
        return moved;
    } else {
        /* A window of ours, by the pixel; the position is 16 bits: never
         * below 0, where it would wrap to the end. */
        SendMessageW(h, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, max(0, before + px)), 0);
    }
    moved = (ScrollPos(h) - before) * unit;
    if (moved == 0) *end = TRUE;
    else HoverUnderMouse(h);
    return moved;
}

static void ScrollFrame(HWND h, SmoothScroll *s)
{
    LARGE_INTEGER now, rate;
    int elapsed = SCROLL_FRAME_MS, step;
    BOOL end;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&rate);
    if (s->last) elapsed = (int)min(1000, (now.QuadPart - s->last) * 1000 / rate.QuadPart);
    s->last = now.QuadPart;
    step = Core_ScrollStep(s->pending, elapsed);
    s->pending -= step;
    s->owed += step;
    /* A row taken half way leaves the control a little ahead: it waits for
     * the animation there, it never comes back. */
    end = FALSE;
    s->own = TRUE;
    if (s->owed * s->dir > 0) s->owed -= ScrollBy(h, s->owed, &end);
    s->own = FALSE;
    if (end || (s->pending == 0 && !step)) ScrollStop(h, s);   /* at an end, or all covered */
}

static HWND ViewAround(HWND control);
static int WheelRow(HWND h, int unit);

static LRESULT CALLBACK ScrollSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    SmoothScroll *s = (SmoothScroll *)ref;
    switch (msg) {
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        int unit = ScrollUnit(h), notch, frames;
        if (unit <= 0) break;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL) {
            RECT rc;
            GetClientRect(h, &rc);
            notch = rc.bottom;
        } else {
            notch = (int)lines * (s->rowPx > 0 ? s->rowPx : WheelRow(h, unit));
        }
        /* The other way: what was left of the last move is dropped. */
        if ((GET_WHEEL_DELTA_WPARAM(wp) > 0) == (s->pending + s->owed > 0)) s->pending = s->owed = 0;
        s->pending -= MulDiv(GET_WHEEL_DELTA_WPARAM(wp), notch, WHEEL_DELTA);
        s->dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
        if (!s->running) {
            s->running = SetTimer(h, SCROLL_TIMER, SCROLL_FRAME_MS, NULL) != 0;
            s->last = 0;   /* the first frame of a scroll: at once */
        }
        ScrollFrame(h, s);
        /* No timer: the rest at once. */
        for (frames = 0; !s->running && s->pending && frames < 64; frames++) ScrollFrame(h, s);
        return 0;
    }
    case WM_TIMER:
        if (wp == SCROLL_TIMER) {
            ScrollFrame(h, s);
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (!s->own) ScrollStop(h, s);   /* the scroll bar or the keyboard takes over */
        break;
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
        ScrollStop(h, s);
        break;
    case WM_NCDESTROY:
        ScrollStop(h, s);
        RemoveWindowSubclass(h, ScrollSubclass, id);
        HeapFree(GetProcessHeap(), 0, s);
        return DefSubclassProc(h, msg, wp, lp);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void Theme_SetScrollRow(HWND list, int rowPx)
{
    DWORD_PTR ref = 0;
    SmoothScroll *s;
    if (ViewAround(list)) list = ViewAround(list);   /* a control in a view: the view scrolls */
    if (GetWindowSubclass(list, ScrollSubclass, SCROLL_SUBCLASS, &ref) && ref) {
        ((SmoothScroll *)ref)->rowPx = rowPx;
        return;
    }
    if ((s = (SmoothScroll *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s)) == NULL) return;
    s->rowPx = rowPx;
    if (!SetWindowSubclass(list, ScrollSubclass, SCROLL_SUBCLASS, (DWORD_PTR)s)) HeapFree(GetProcessHeap(), 0, s);
}

static void SmoothScrolling(HWND list)
{
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(list, ScrollSubclass, SCROLL_SUBCLASS, &ref)) Theme_SetScrollRow(list, 0);
}

/* ------------------------------------------------------ fields and frames */

#define FRAME_COMBO  2   /* a drop-down list: drawn here whole (PaintDropDownList) */
#define FRAME_CENTER 4   /* a one-line edit: its text centered in its height */
#define FRAME_BARE   8   /* a scrolling control made with a frame, without it now (FitFrame) */

#define HOT_PROP    L"ClaudeDesktopProfilesManager.Hot"      /* a drop-down list under the mouse */
#define CHOSEN_PROP L"ClaudeDesktopProfilesManager.Chosen"   /* a drop-down list's choice while its list shows */
#define BORDER_PROP L"ClaudeDesktopProfilesManager.Border"   /* the frame and scroll bar a control was made with */

/* What an edit is filled with: what its parent answers to WM_CTLCOLOR*
 * (a dialog procedure returns the brush itself). */
static HBRUSH EditBrush(HWND h, HDC dc)
{
    BOOL still = !IsWindowEnabled(h) || (GetWindowLongW(h, GWL_STYLE) & ES_READONLY);
    HBRUSH brush = (HBRUSH)SendMessageW(GetParent(h), still ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)h);
    return brush ? brush : GetSysColorBrush(still ? COLOR_3DFACE : COLOR_WINDOW);
}

/* The non-client area inside `inset` pixels from the edge, but the scroll
 * bars, in the edit's fill: in dark mode (inset 0) its light frame is
 * painted over, so the field reaches its edge; its margins are filled in
 * both modes. `dc` is a window DC (WM_PRINT) or NULL. */
static void PaintBorder(HWND h, HDC given, int inset)
{
    static const LONG kBars[] = { OBJID_VSCROLL, OBJID_HSCROLL };
    RECT win, client;
    HDC dc = given ? given : GetWindowDC(h);
    int saved, i;
    if (!dc) return;
    saved = SaveDC(dc);
    GetWindowRect(h, &win);
    GetClientRect(h, &client);
    MapWindowPoints(h, NULL, (POINT *)&client, 2);
    OffsetRect(&client, -win.left, -win.top);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    for (i = 0; i < (int)ARRAYSIZE(kBars); i++) {
        SCROLLBARINFO bar;
        ZeroMemory(&bar, sizeof bar);
        bar.cbSize = sizeof bar;
        if (GetScrollBarInfo(h, kBars[i], &bar) && !(bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) {
            OffsetRect(&bar.rcScrollBar, -win.left, -win.top);
            ExcludeClipRect(dc, bar.rcScrollBar.left, bar.rcScrollBar.top, bar.rcScrollBar.right, bar.rcScrollBar.bottom);
        }
    }
    OffsetRect(&win, -win.left, -win.top);
    InflateRect(&win, -inset, -inset);
    FillRect(dc, &win, EditBrush(h, dc));
    RestoreDC(dc, saved);
    if (!given) ReleaseDC(h, dc);
}

/* A centered edit's margins; in dark mode over its frame too, in light mode
 * inside it. */
static void PaintFrame(HWND h, HDC given)
{
    UINT dpi = GetDpiForWindow(h);
    int edge = 0;
    if (!g_dark && (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE)) edge = GetSystemMetricsForDpi(SM_CXEDGE, dpi);
    else if (!g_dark && (GetWindowLongW(h, GWL_STYLE) & WS_BORDER)) edge = GetSystemMetricsForDpi(SM_CXBORDER, dpi);
    PaintBorder(h, given, edge);
}

#define MADE_RECORDED 1
#define MADE_BORDER   2   /* WS_BORDER */
#define MADE_EDGE     4   /* WS_EX_CLIENTEDGE */
#define MADE_SCROLLS  8   /* WS_VSCROLL (which goes while there is nothing to scroll) */

/* The frame and scroll bar a control was made with (MADE_*), recorded the
 * first time. */
static INT_PTR FrameMade(HWND h)
{
    INT_PTR made = (INT_PTR)GetPropW(h, BORDER_PROP);
    if (!made) {
        LONG style = GetWindowLongW(h, GWL_STYLE);
        made = MADE_RECORDED | ((style & WS_BORDER) ? MADE_BORDER : 0) | ((style & WS_VSCROLL) ? MADE_SCROLLS : 0) |
               ((GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) ? MADE_EDGE : 0);
        SetPropW(h, BORDER_PROP, (HANDLE)made);
    }
    return made;
}

/* A list, tree, list box or multi-line edit scrolls. In dark mode it has no
 * frame: its field and its scroll bars reach its edges (a frame painted in
 * the field's color would leave a line around the scroll bar). In light mode
 * it has the frame it was made with (`made`). */
static void FitFrame(HWND h, INT_PTR made)
{
    LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE), wantStyle, wantEx;
    wantStyle = g_dark || !(made & MADE_BORDER) ? style & ~WS_BORDER : style | WS_BORDER;
    wantEx = g_dark || !(made & MADE_EDGE) ? ex & ~WS_EX_CLIENTEDGE : ex | WS_EX_CLIENTEDGE;
    if (wantStyle == style && wantEx == ex) return;
    SetWindowLongW(h, GWL_STYLE, wantStyle);
    SetWindowLongW(h, GWL_EXSTYLE, wantEx);
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* A one-line edit draws its text at the top of its client area: the client
 * area starts lower by half the height the text leaves free, so the text,
 * its selection and the caret sit in the middle. */
static void CenterEditText(HWND h, RECT *client)
{
    TEXTMETRICW tm;
    HDC dc = GetDC(h);
    HFONT font = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HGDIOBJ old;
    int spare;
    if (!dc) return;
    old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    spare = (client->bottom - client->top) - tm.tmHeight;
    if (spare > 1) client->top += spare / 2;
}

static BOOL IsDropDownList(HWND h)
{
    LONG style = GetWindowLongW(h, GWL_STYLE);
    return IsClass(h, WC_COMBOBOXW) && (style & 3) == CBS_DROPDOWNLIST && !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE));
}

/* A drop-down list, drawn whole like the drop-down buttons
 * (Theme_DrawDropDown): its choice on the left, a button under the mouse,
 * pressed while its list shows. Its choice is never shown selected; focus
 * shows as a focus rectangle, after keyboard use only. */
static void PaintDropDownList(HWND h, HDC dc)
{
    WCHAR text[256];
    RECT rc;
    UINT state = 0;
    BOOL showFocus, dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
    LRESULT sel = SendMessageW(h, CB_GETCURSEL, 0, 0);
    /* While its list shows, the list's selection follows the mouse: the box
     * keeps the choice it had when the list opened, until a click or Enter
     * makes another. */
    if (dropped) {
        INT_PTR chosen = (INT_PTR)GetPropW(h, CHOSEN_PROP);   /* the choice + 2: never 0 */
        if (!chosen) SetPropW(h, CHOSEN_PROP, (HANDLE)(chosen = (INT_PTR)sel + 2));
        sel = (LRESULT)chosen - 2;
    } else {
        RemovePropW(h, CHOSEN_PROP);
    }
    text[0] = 0;
    if (sel >= 0 && SendMessageW(h, CB_GETLBTEXTLEN, (WPARAM)sel, 0) < (LRESULT)ARRAYSIZE(text))
        SendMessageW(h, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)text);
    if (!IsWindowEnabled(h)) state = THEME_BUTTON_DISABLED;
    else if (dropped) state = THEME_BUTTON_PRESSED;
    else if (GetPropW(h, HOT_PROP)) state = THEME_BUTTON_HOT;
    GetClientRect(h, &rc);
    Theme_DrawDropDown(h, dc, &rc, text, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0), state);
    TextFormatForCues(h, 0, &showFocus);
    if (showFocus && GetFocus() == h && !(state & THEME_BUTTON_PRESSED)) {
        int dpi = (int)GetDpiForWindow(h);
        RECT focus = rc;
        InflateRect(&focus, -(MulDiv(3, dpi, 96) + 2 * max(1, MulDiv(1, dpi, 96))), -(MulDiv(3, dpi, 96) + 2 * max(1, MulDiv(1, dpi, 96))));
        SetTextColor(dc, g_dark ? g_palette.color[THEME_TEXT] : GetSysColor(COLOR_BTNTEXT));
        SetBkColor(dc, g_dark ? g_palette.button : GetSysColor(COLOR_3DFACE));
        DrawFocusRect(dc, &focus);
    }
}

/* An edit's selection uses the system highlight color: in dark mode it is
 * drawn again in the main blue, over what the edit painted in `dc`. */
static void PaintEditSelection(HWND h, HDC dc)
{
    DWORD start = 0, end = 0;
    int len, x0, x1, y;
    WCHAR *text;
    TEXTMETRICW tm;
    RECT sel, format;
    HGDIOBJ old;
    if (!g_dark || (GetWindowLongW(h, GWL_STYLE) & ES_PASSWORD)) return;
    SendMessageW(h, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    len = GetWindowTextLengthW(h);
    if (start >= end || len <= 0 || end > (DWORD)len) return;
    if (GetFocus() != h && !(GetWindowLongW(h, GWL_STYLE) & ES_NOHIDESEL)) return;
    if ((text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)len + 1) * sizeof(WCHAR))) == NULL) return;
    GetWindowTextW(h, text, len + 1);
    old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &tm);
    x0 = (short)LOWORD(SendMessageW(h, EM_POSFROMCHAR, start, 0));
    y = (short)HIWORD(SendMessageW(h, EM_POSFROMCHAR, start, 0));
    if (end < (DWORD)len) {
        x1 = (short)LOWORD(SendMessageW(h, EM_POSFROMCHAR, end, 0));
    } else {
        SIZE last;
        GetTextExtentPoint32W(dc, text + len - 1, 1, &last);
        x1 = (short)LOWORD(SendMessageW(h, EM_POSFROMCHAR, (WPARAM)len - 1, 0)) + last.cx;
    }
    SendMessageW(h, EM_GETRECT, 0, (LPARAM)&format);
    SetRect(&sel, x0, y, x1, y + tm.tmHeight);
    {
        /* The edit's own highlight reaches a pixel or so past the text: what
         * is left of it around the selection turns main blue too. */
        COLORREF system = GetSysColor(COLOR_HIGHLIGHT);
        RECT client, around = sel;
        int px, py;
        GetClientRect(h, &client);
        InflateRect(&around, 2, 2);
        if (IntersectRect(&around, &around, &client))
            for (py = around.top; py < around.bottom; py++)
                for (px = around.left; px < around.right; px++)
                    if (GetPixel(dc, px, py) == system) SetPixelV(dc, px, py, g_palette.color[THEME_MAIN_BLUE]);
    }
    if (IntersectRect(&sel, &sel, &format)) {
        SetBkColor(dc, g_palette.color[THEME_MAIN_BLUE]);
        SetTextColor(dc, g_palette.color[THEME_TEXT]);
        ExtTextOutW(dc, x0, y, ETO_OPAQUE | ETO_CLIPPED, &sel, text + start, end - start, NULL);
    }
    SelectObject(dc, old);
    HeapFree(GetProcessHeap(), 0, text);
}

/* A control painted off screen by `paint`, then shown at once. */
static void PaintBuffered(HWND h, void (*paint)(HWND, HDC))
{
    PAINTSTRUCT ps;
    ThemeBuffer buffer;
    RECT rc;
    HDC dc = BeginPaint(h, &ps);
    if (!dc) return;
    GetClientRect(h, &rc);
    paint(h, Theme_BufferBegin(&buffer, dc, &rc));
    Theme_BufferEnd(&buffer);
    EndPaint(h, &ps);
}

/* A one-line edit: its fill, what it paints itself (controls paint in the
 * DC that WM_PAINT's wParam brings), then its selection in the main blue. */
static void PaintEdit(HWND h, HDC dc)
{
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, EditBrush(h, dc));
    DefSubclassProc(h, WM_PAINT, (WPARAM)dc, 0);
    PaintEditSelection(h, dc);
}

/* The list a drop-down list drops: its rows drawn as every list's
 * (Theme_DrawRow: the choice under the mouse in the main blue, in its bright
 * frame), its text where the box shows it. */
static void PaintDroppedList(HWND h, HDC dc)
{
    WCHAR text[256];
    RECT rc, row;
    COLORREF field = g_dark ? g_palette.color[THEME_FIELD] : GetSysColor(COLOR_WINDOW);
    int i, count = (int)SendMessageW(h, LB_GETCOUNT, 0, 0), sel = (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
    int inset = MulDiv(8, (int)GetDpiForWindow(h), 96);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    GetClientRect(h, &rc);
    Fill(dc, &rc, field);
    SetBkMode(dc, TRANSPARENT);
    for (i = max(0, (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0)); i < count; i++) {
        if (SendMessageW(h, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&row) == LB_ERR || row.top >= rc.bottom) break;
        SetTextColor(dc, Theme_DrawRow(dc, &row, i == sel ? THEME_ROW_SELECTED : 0, field));
        text[0] = 0;
        if (SendMessageW(h, LB_GETTEXTLEN, (WPARAM)i, 0) < (LRESULT)ARRAYSIZE(text)) SendMessageW(h, LB_GETTEXT, (WPARAM)i, (LPARAM)text);
        row.left += inset;
        DrawTextW(dc, text, -1, &row, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
}

/* A dark dropped list: framed in the main blue, as the buttons are, not in
 * light grey. */
static void PaintDroppedFrame(HWND h)
{
    RECT win;
    HDC dc = GetWindowDC(h);
    if (!dc) return;
    GetWindowRect(h, &win);
    OffsetRect(&win, -win.left, -win.top);
    SetDCBrushColor(dc, g_palette.color[THEME_MAIN_BLUE]);
    FrameRect(dc, &win, (HBRUSH)GetStockObject(DC_BRUSH));
    ReleaseDC(h, dc);
}

static LRESULT CALLBACK DroppedListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    LRESULT r;
    (void)ref;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (wp) PaintDroppedList(h, (HDC)wp);
        else PaintBuffered(h, PaintDroppedList);
        return 0;
    case WM_PRINTCLIENT:
        PaintDroppedList(h, (HDC)wp);
        return 0;
    case WM_NCPAINT:
        r = DefSubclassProc(h, msg, wp, lp);
        if (g_dark) PaintDroppedFrame(h);
        return r;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, DroppedListSubclass, id);
        break;
    }
    r = DefSubclassProc(h, msg, wp, lp);
    /* The list moves its choice under the mouse by drawing it itself: drawn
     * again at once, off screen. */
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_KEYDOWN:
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
    case WM_CAPTURECHANGED:
    case LB_SETCURSEL:
        RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        break;
    }
    return r;
}

/* Focus rectangles are for the keyboard: Tab or an arrow shows them (the
 * dialog manager does), a click hides them again. */
static void HideFocusCues(HWND h)
{
    HWND root = GetAncestor(h, GA_ROOT);
    if (root && !(SendMessageW(root, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        SendMessageW(root, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
}

static LRESULT CALLBACK ChildSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR frame)
{
    LRESULT r;
    /* Edits and drop-down lists are painted here, off screen; the background
     * is part of it. */
    if (frame & (FRAME_CENTER | FRAME_COMBO)) {
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT && !wp) {
            PaintBuffered(h, (frame & FRAME_COMBO) ? PaintDropDownList : PaintEdit);
            return 0;
        }
    }
    /* A tree paints the frame it was made with around its scroll bars, even
     * once it has none: its non-client area, scroll bars only, is Windows'. */
    if ((frame & FRAME_BARE) && msg == WM_NCPAINT) return DefWindowProcW(h, msg, wp, lp);
    if (frame & FRAME_COMBO) {
        switch (msg) {
        case WM_PAINT:        /* into the DC it brings */
        case WM_PRINTCLIENT:
            PaintDropDownList(h, (HDC)wp);
            return 0;
        case WM_MOUSEMOVE:
            if (!GetPropW(h, HOT_PROP)) {
                TRACKMOUSEEVENT track = { sizeof track, TME_LEAVE, h, 0 };
                SetPropW(h, HOT_PROP, (HANDLE)1);
                TrackMouseEvent(&track);
                InvalidateRect(h, NULL, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(h, HOT_PROP);
            InvalidateRect(h, NULL, FALSE);
            break;
        }
    }
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        HideFocusCues(h);
        break;
    case WM_SETFONT: {
        /* A control made semibold (Theme_SetStrong) stays so in a new font. */
        HFONT strong = (HFONT)GetPropW(h, STRONG_PROP), made;
        if (strong && (HFONT)wp != strong && (made = StrongOf((HFONT)wp)) != NULL) {
            SetPropW(h, STRONG_PROP, made);
            r = DefSubclassProc(h, msg, (WPARAM)made, lp);
            DeleteObject(strong);
            return r;
        }
        break;
    }
    case WM_NCDESTROY: {
        HFONT strong = (HFONT)RemovePropW(h, STRONG_PROP);
        RemovePropW(h, HOT_PROP);
        RemovePropW(h, CHOSEN_PROP);
        RemovePropW(h, BORDER_PROP);
        RemoveWindowSubclass(h, ChildSubclass, id);
        r = DefSubclassProc(h, msg, wp, lp);
        if (strong) DeleteObject(strong);
        return r;
    }
    }
    r = DefSubclassProc(h, msg, wp, lp);
    if (frame & FRAME_CENTER) {
        switch (msg) {
        case WM_NCCALCSIZE:
            CenterEditText(h, wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp);
            break;
        case WM_NCHITTEST:
            /* The margin above and below the text is still the edit. */
            if (r == HTBORDER || r == HTNOWHERE) r = HTCLIENT;
            break;
        case WM_SETFONT:
            SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            break;
        }
    }
    if (frame & FRAME_CENTER) {
        switch (msg) {
        case WM_NCPAINT:
            PaintFrame(h, NULL);
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_ENABLE:
            if (g_dark) PaintFrame(h, NULL);   /* a themed frame changes with these */
            break;
        case WM_PRINT:
            if (lp & PRF_NONCLIENT) PaintFrame(h, (HDC)wp);
            break;
        }
    }
    /* A drop-down list also draws itself outside WM_PAINT (its choice
     * selected when it has the focus): drawn again at once, off screen. */
    if (frame & FRAME_COMBO) {
        switch (msg) {
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_KEYDOWN:
        case WM_CHAR:
        case WM_MOUSEWHEEL:
        case WM_ENABLE:
        case WM_SETFONT:
        case WM_COMMAND:
        case WM_CAPTURECHANGED:
        case WM_UPDATEUISTATE:
        case CB_SETCURSEL:
        case CB_SHOWDROPDOWN:
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            break;
        }
    }
    /* An edit draws its selection and typed text itself, outside WM_PAINT:
     * repainted at once, off screen, with the main blue. */
    if ((frame & FRAME_CENTER) && g_dark) {
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_KEYDOWN:
        case WM_CHAR:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case EM_SETSEL:
        case EM_REPLACESEL:
        case WM_SETTEXT:
        case WM_CUT:
        case WM_PASTE:
        case WM_CLEAR:
        case WM_UNDO:
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            break;
        case WM_MOUSEMOVE:
            if (wp & MK_LBUTTON) RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            break;
        }
    }
    return r;
}

/* ----------------------------------------------------------- smooth views */

/* A list, list box or tree scrolls by whole rows, what the program draws
 * itself by the pixel. For every list to scroll alike, Theme_SmoothView puts
 * the control, as tall as all its rows, in a view of ours that scrolls it by
 * the pixel: the control is moved and never scrolls itself; the view has the
 * scroll bar and the smooth wheel (above). A list view's header stays on
 * top; the row the keyboard moves to is scrolled into sight. The view takes
 * the control's place, its id (so the dialog shows and hides it) and its
 * frame, and passes on whatever the control tells its parent. A control too
 * tall for a window (VIEW_MAX_PX) scrolls itself, by rows. */
#define VIEW_CLASS    L"ClaudeDesktopProfilesManager.View"
#define VIEW_MEASURE  (WM_APP + 0x3E0)
#define VIEW_MAX_PX   30000
#define VIEW_SUBCLASS 6

typedef struct View {
    HWND control;
    int  pos;      /* px scrolled */
    BOOL posted;   /* a VIEW_MEASURE is on its way */
    BOOL native;   /* too tall: the control scrolls itself */
} View;

static View *ViewOf(HWND h)
{
    return h && IsClass(h, VIEW_CLASS) ? (View *)GetWindowLongPtrW(h, GWLP_USERDATA) : NULL;
}

/* The view a control is in, or NULL. */
static HWND ViewAround(HWND control)
{
    HWND parent = GetParent(control);
    return ViewOf(parent) ? parent : NULL;
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

/* The height of all the control's rows (a list view's header with them). */
static int ContentHeight(HWND c)
{
    if (IsClass(c, WC_LISTBOXW)) return (int)SendMessageW(c, LB_GETCOUNT, 0, 0) * (int)SendMessageW(c, LB_GETITEMHEIGHT, 0, 0);
    if (IsClass(c, WC_LISTVIEWW)) {
        RECT r;
        int n = ListView_GetItemCount(c);
        return HeaderHeight(c) + (n > 0 && ListView_GetItemRect(c, 0, &r, LVIR_BOUNDS) ? n * (r.bottom - r.top) : 0);
    }
    if (IsClass(c, WC_TREEVIEWW)) {
        int unit = TreeView_GetItemHeight(c), total = 0;
        HTREEITEM item;
        for (item = TreeView_GetRoot(c); item; item = TreeView_GetNextVisible(c, item)) {
            TVITEMEXW it;
            ZeroMemory(&it, sizeof it);
            it.mask = TVIF_HANDLE | TVIF_INTEGRAL;
            it.hItem = item;
            total += (TreeView_GetItem(c, (TVITEMW *)&it) ? max(1, it.iIntegral) : 1) * unit;
        }
        return total;
    }
    return 0;
}

/* A list view's header at the top of the view, whatever is scrolled under it. */
static void PlaceHeader(HWND view)
{
    View *v = ViewOf(view);
    HWND header = v ? ShownHeader(v->control) : NULL;
    RECT r, client;
    if (!header || !GetWindowRect(header, &r)) return;
    GetClientRect(v->control, &client);
    SetWindowPos(header, HWND_TOP, 0, v->native ? 0 : v->pos, client.right, r.bottom - r.top, SWP_NOACTIVATE);
}

/* A list view's last column takes the width the others leave, as the view's
 * scroll bar comes or goes. */
static void FitLastColumn(HWND list)
{
    HWND header = IsClass(list, WC_LISTVIEWW) ? ListView_GetHeader(list) : NULL;
    RECT client;
    int n = header ? Header_GetItemCount(header) : 0, i, used = 0;
    if (n < 1) return;
    GetClientRect(list, &client);
    for (i = 0; i < n - 1; i++) used += ListView_GetColumnWidth(list, i);
    if (client.right - used > 0 && ListView_GetColumnWidth(list, n - 1) != client.right - used)
        ListView_SetColumnWidth(list, n - 1, client.right - used);
}

/* The control as tall as its rows (at least the view) and as wide as the
 * view, and the scroll bar for the difference. */
static void ViewLayout(HWND view)
{
    View *v = ViewOf(view);
    SCROLLINFO si;
    RECT rc;
    int content;
    if (!v || !v->control) return;
    GetClientRect(view, &rc);
    content = ContentHeight(v->control);
    if (v->native != (content > VIEW_MAX_PX)) {
        v->native = content > VIEW_MAX_PX;
        if (v->native) SmoothScrolling(v->control);   /* by rows, its own way */
    }
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (v->native || content <= rc.bottom) {
        v->pos = 0;
    } else {
        si.nMax = content - 1;
        si.nPage = (UINT)rc.bottom;
        v->pos = min(v->pos, content - rc.bottom);
    }
    si.nPos = v->pos;
    SetScrollInfo(view, SB_VERT, &si, TRUE);   /* the scroll bar may come or go: the width changes */
    GetClientRect(view, &rc);
    SetWindowPos(v->control, NULL, 0, -v->pos, rc.right, v->native ? rc.bottom : max(content, rc.bottom), SWP_NOZORDER | SWP_NOACTIVATE);
    FitLastColumn(v->control);
    PlaceHeader(view);
}

static void ViewRemeasure(HWND view)
{
    View *v = ViewOf(view);
    if (!v || v->posted) return;
    v->posted = TRUE;
    PostMessageW(view, VIEW_MEASURE, 0, 0);
}

static void ViewScrollTo(HWND view, int pos)
{
    View *v = ViewOf(view);
    SCROLLINFO si;
    if (!v || v->native) return;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE;
    if (!GetScrollInfo(view, SB_VERT, &si)) return;
    pos = max(0, min(pos, si.nMax - (int)si.nPage + 1));
    if (pos == v->pos) return;
    v->pos = pos;
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(view, SB_VERT, &si, TRUE);
    SetWindowPos(v->control, NULL, 0, -pos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PlaceHeader(view);
    UpdateWindow(v->control);
    HoverUnderMouse(v->control);
}

/* WM_VSCROLL from the view's scroll bar, or the smooth wheel's
 * SB_THUMBPOSITION; a line is a row of the control. */
static void ViewScroll(HWND view, WPARAM wp)
{
    View *v = ViewOf(view);
    SCROLLINFO si;
    int line;
    if (!v) return;
    line = max(1, ScrollUnit(v->control));
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(view, SB_VERT, &si)) return;
    switch (LOWORD(wp)) {
    case SB_LINEUP:        ViewScrollTo(view, si.nPos - line); break;
    case SB_LINEDOWN:      ViewScrollTo(view, si.nPos + line); break;
    case SB_PAGEUP:        ViewScrollTo(view, si.nPos - (int)si.nPage); break;
    case SB_PAGEDOWN:      ViewScrollTo(view, si.nPos + (int)si.nPage); break;
    case SB_THUMBTRACK:    ViewScrollTo(view, si.nTrackPos); break;
    case SB_THUMBPOSITION: ViewScrollTo(view, HIWORD(wp)); break;
    case SB_TOP:           ViewScrollTo(view, 0); break;
    case SB_BOTTOM:        ViewScrollTo(view, si.nMax); break;
    }
}

/* The row the keyboard is on, scrolled into sight (under a list view's
 * header): the control, as tall as its rows, does not do it itself. */
static void ViewShowFocus(HWND view)
{
    View *v = ViewOf(view);
    RECT item = { 0 }, rc;
    HWND c;
    int top = 0;
    BOOL found = FALSE;
    if (!v || v->native) return;
    c = v->control;
    if (IsClass(c, WC_LISTBOXW)) {
        int i = (int)SendMessageW(c, LB_GETCARETINDEX, 0, 0);
        found = i >= 0 && SendMessageW(c, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&item) != LB_ERR;
    } else if (IsClass(c, WC_LISTVIEWW)) {
        int i = ListView_GetNextItem(c, -1, LVNI_FOCUSED);
        found = i >= 0 && ListView_GetItemRect(c, i, &item, LVIR_BOUNDS);
        top = HeaderHeight(c);
    } else if (IsClass(c, WC_TREEVIEWW)) {
        HTREEITEM sel = TreeView_GetSelection(c);
        found = sel && TreeView_GetItemRect(c, sel, &item, FALSE);
    }
    if (!found) return;
    GetClientRect(view, &rc);
    if (item.top < v->pos + top) ViewScrollTo(view, item.top - top);
    else if (item.bottom > v->pos + rc.bottom) ViewScrollTo(view, item.bottom - rc.bottom);
}

/* The control in a view: its wheel is the view's; what changes its rows
 * (items, heights, a folder opened or closed by the mouse or the keyboard)
 * has the view measure it again, once. */
static LRESULT CALLBACK ViewedSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    HWND view = (HWND)ref;
    View *v = ViewOf(view);
    LRESULT r;
    if (msg == WM_MOUSEWHEEL && v && !v->native) return SendMessageW(view, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ViewedSubclass, id);
        return DefSubclassProc(h, msg, wp, lp);
    }
    r = DefSubclassProc(h, msg, wp, lp);
    switch (msg) {
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_DELETESTRING:
    case LB_RESETCONTENT:
    case LB_SETITEMHEIGHT:
    case LVM_INSERTITEMW:
    case LVM_DELETEITEM:
    case LVM_DELETEALLITEMS:
    case TVM_INSERTITEMW:
    case TVM_DELETEITEM:
    case TVM_EXPAND:
    case TVM_SETITEMW:
    case TVM_SETITEMHEIGHT:
    case WM_SETFONT:
    case WM_LBUTTONDOWN:     /* a folder's arrow */
    case WM_LBUTTONDBLCLK:
        ViewRemeasure(view);
        break;
    case WM_KEYDOWN:
        ViewRemeasure(view);   /* the arrows open and close folders too */
        ViewShowFocus(view);
        break;
    case WM_SETREDRAW:
        /* Refilled: its full height at once, before it shows a scroll bar
         * of its own for the rows it now has. */
        if (wp) ViewLayout(view);
        break;
    case WM_SIZE:
        /* Its own scroll bar gone: the last column takes the room, and
         * nothing of the bar stays drawn. */
        FitLastColumn(h);
        PlaceHeader(view);
        InvalidateRect(h, NULL, FALSE);
        if (v && !v->native && (GetWindowLongW(h, GWL_STYLE) & WS_VSCROLL)) ViewRemeasure(view);   /* short of its rows */
        break;
    }
    return r;
}

static LRESULT CALLBACK ViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    View *v = (View *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((const CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_SIZE:
        ViewLayout(h);
        return 0;
    case VIEW_MEASURE:
        if (v) v->posted = FALSE;
        ViewLayout(h);
        ViewShowFocus(h);
        return 0;
    case WM_VSCROLL:
        ViewScroll(h, wp);
        return 0;
    case WM_SETFOCUS:
        if (v && v->control) SetFocus(v->control);   /* the dialog gives it the focus by its id */
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
        return SendMessageW(GetParent(h), msg, wp, lp);   /* what the control tells its parent is for the dialog */
    case WM_NCDESTROY:
        if (v) HeapFree(GetProcessHeap(), 0, v);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* A wheel line in `h`: a row of the control in a view, else `unit`. */
static int WheelRow(HWND h, int unit)
{
    View *v = ViewOf(h);
    return v && v->control ? max(1, ScrollUnit(v->control)) : unit;
}

HWND Theme_SmoothView(HWND control)
{
    static BOOL registered;
    HWND parent = GetParent(control), view;
    LONG style = GetWindowLongW(control, GWL_STYLE), ex = GetWindowLongW(control, GWL_EXSTYLE);
    View *v;
    RECT rc;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = VIEW_CLASS;
        registered = RegisterClassW(&wc) != 0;
    }
    if (!registered || !parent || ViewAround(control)) return ViewAround(control) ? ViewAround(control) : control;
    if ((v = (View *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *v)) == NULL) return control;
    GetWindowRect(control, &rc);
    MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
    view = CreateWindowExW(WS_EX_CONTROLPARENT | (ex & WS_EX_CLIENTEDGE), VIEW_CLASS, L"",
                           WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN | (style & (WS_VISIBLE | WS_BORDER)), rc.left, rc.top, rc.right - rc.left,
                           rc.bottom - rc.top, parent, (HMENU)(INT_PTR)GetDlgCtrlID(control), g_hInst, v);
    if (!view) {
        HeapFree(GetProcessHeap(), 0, v);
        return control;
    }
    v->control = control;
    SetWindowPos(view, control, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* its place in the tab order */
    /* The frame is the view's now; the control, as tall as its rows, needs no scroll bar. */
    SetWindowLongW(control, GWL_STYLE, (style & ~(WS_BORDER | WS_VSCROLL | WS_HSCROLL)) | WS_VISIBLE |
                                           (IsClass(control, WC_LISTVIEWW) ? WS_CLIPCHILDREN : 0));   /* rows scroll under the header */
    SetWindowLongW(control, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE);
    SetPropW(control, BORDER_PROP, (HANDLE)(INT_PTR)MADE_RECORDED);
    /* The view scrolls, with the control's frame, whether or not its scroll
     * bar shows when it is themed. */
    SetPropW(view, BORDER_PROP, (HANDLE)(INT_PTR)(MADE_RECORDED | MADE_SCROLLS | ((style & WS_BORDER) ? MADE_BORDER : 0) |
                                                  ((ex & WS_EX_CLIENTEDGE) ? MADE_EDGE : 0)));
    SmoothScrolling(view);
    SetParent(control, view);
    SetWindowPos(control, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowSubclass(control, ViewedSubclass, VIEW_SUBCLASS, (DWORD_PTR)view);
    ViewLayout(view);
    return view;
}

/* --------------------------------------------------------------- dialogs */

/* Every themed dialog: in dark mode its push buttons, check boxes and list
 * view rows are drawn here, whatever dialog it is. */
/* Windows 11's link blue on a dark background (the system's is for light ones). */
#define DARK_LINK RGB(0x60, 0xCD, 0xFF)

/* A link control's text (LWS_USECUSTOMTEXT) in the colors the dialog gives
 * its statics, its links in the link blue. */
static void LinkColors(HWND dlg, const NMCUSTOMTEXT *t)
{
    SetTextColor(t->hDC, g_dark ? g_palette.color[THEME_TEXT] : GetSysColor(COLOR_WINDOWTEXT));   /* a parent that gives none */
    SendMessageW(dlg, WM_CTLCOLORSTATIC, (WPARAM)t->hDC, (LPARAM)t->hdr.hwndFrom);
    if (t->fLink) SetTextColor(t->hDC, g_dark ? DARK_LINK : GetSysColor(COLOR_HOTLIGHT));
}

static LRESULT CALLBACK DialogSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_NOTIFY && ((const NMHDR *)lp)->code == NM_CUSTOMTEXT && IsClass(((const NMHDR *)lp)->hwndFrom, WC_LINK)) {
        LinkColors(h, (const NMCUSTOMTEXT *)lp);
        return 0;
    }
    if (msg == WM_NOTIFY && g_dark && ((const NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        HWND from = ((const NMHDR *)lp)->hwndFrom;
        if (IsPushButton(from)) return ButtonCustomDraw((const NMCUSTOMDRAW *)lp);
        if (IsCheckBox(from)) return CheckBoxCustomDraw((const NMCUSTOMDRAW *)lp);
        if (IsClass(from, WC_LISTVIEWW)) return ListCustomDraw((NMLVCUSTOMDRAW *)lp);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, DialogSubclass, id);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static BOOL CALLBACK ThemeChild(HWND child, LPARAM lp)
{
    DWORD_PTR frame = 0;
    BOOL edit = IsClass(child, WC_EDITW), list = IsClass(child, WC_LISTVIEWW), tree = IsClass(child, WC_TREEVIEWW);
    BOOL listbox = IsClass(child, WC_LISTBOXW), combo = IsClass(child, WC_COMBOBOXW);
    BOOL view = IsClass(child, VIEW_CLASS), viewed = ViewAround(child) != NULL;
    BOOL multiline = edit && (GetWindowLongW(child, GWL_STYLE) & ES_MULTILINE), scrolls = list || tree || listbox || multiline || view;
    INT_PTR made = FrameMade(child);
    (void)lp;
    if (g_allowDark) g_allowDark(child, g_dark);
    if (combo && IsDropDownList(child))
        frame = FRAME_COMBO;
    else if (edit && !multiline && !IsClass(GetParent(child), WC_COMBOBOXW))
        frame = FRAME_CENTER;   /* a combo box's own edit keeps its place */
    else if (scrolls && g_dark && (made & (MADE_BORDER | MADE_EDGE)))
        frame = FRAME_BARE;
    SetWindowSubclass(child, ChildSubclass, 2, frame);
    if (frame & FRAME_COMBO) {
        COMBOBOXINFO info;
        ZeroMemory(&info, sizeof info);
        info.cbSize = sizeof info;
        if (GetComboBoxInfo(child, &info) && info.hwndList) {
            if (g_allowDark) g_allowDark(info.hwndList, g_dark);
            SetWindowTheme(info.hwndList, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);   /* its scroll bar */
            SetWindowSubclass(info.hwndList, DroppedListSubclass, 5, 0);
        }
    }
    if (scrolls) FitFrame(child, made);
    if (frame & FRAME_CENTER)   /* the client area moves now, with the font the edit has */
        SetWindowPos(child, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if ((list || tree || listbox) && !viewed) SmoothScrolling(child);   /* in a view, the view scrolls */
    if (list) {
        HWND header = ListView_GetHeader(child), tips = ListView_GetToolTips(child);
        SetWindowSubclass(child, ListSubclass, 1, 0);
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        if (header) {
            if (g_allowDark) g_allowDark(header, g_dark);
            SetWindowTheme(header, g_dark ? L"DarkMode_ItemsView" : NULL, NULL);
            SendMessageW(header, WM_THEMECHANGED, 0, 0);
        }
        if (tips) {
            if (g_allowDark) g_allowDark(tips, g_dark);
            SetWindowTheme(tips, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
            SendMessageW(tips, WM_THEMECHANGED, 0, 0);
        }
        ListView_SetBkColor(child, g_palette.color[THEME_FIELD]);
        ListView_SetTextBkColor(child, g_palette.color[THEME_FIELD]);
        ListView_SetTextColor(child, g_palette.color[THEME_TEXT]);
    } else if (tree) {
        HWND tips = TreeView_GetToolTips(child);
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        if (tips) {
            if (g_allowDark) g_allowDark(tips, g_dark);
            SetWindowTheme(tips, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
        }
        TreeView_SetBkColor(child, g_dark ? g_palette.color[THEME_FIELD] : (COLORREF)-1);
        TreeView_SetTextColor(child, g_dark ? g_palette.color[THEME_TEXT] : (COLORREF)-1);
    } else if (listbox) {
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);   /* its scroll bar */
    } else if (IsClass(child, WC_BUTTONW)) {
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    } else if (edit || combo) {
        SetWindowTheme(child, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (made & MADE_SCROLLS) {
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);   /* a window of ours that scrolls */
        SmoothScrolling(child);
    }
    SendMessageW(child, WM_THEMECHANGED, 0, 0);
    return TRUE;
}

void Theme_Apply(HWND dlg)
{
    SetPropW(dlg, THEME_PROP, (HANDLE)(INT_PTR)(g_dark ? 2 : 1));
    if (g_allowDark) g_allowDark(dlg, g_dark);
    SetWindowSubclass(dlg, DialogSubclass, 3, 0);
    TitleBar(dlg);
    EnumChildWindows(dlg, ThemeChild, 0);
    RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

/* WM_CTLCOLOR* for dialogs. `muted` draws that control's text in grey. */
INT_PTR Theme_CtlColor(UINT msg, WPARAM wp, LPARAM lp, int mutedId)
{
    HDC dc = (HDC)wp;
    BOOL muted = mutedId && GetDlgCtrlID((HWND)lp) == mutedId;
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
static void CenterOnOwner(HWND d)
{
    HWND owner = GetWindow(d, GW_OWNER);
    BOOL shown = owner && IsWindowVisible(owner) && !IsIconic(owner);
    RECT on, self, placed;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetWindowRect(d, &self) || !GetMonitorInfoW(MonitorFromWindow(shown ? owner : d, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (!shown || !GetWindowRect(owner, &on)) on = mi.rcWork;
    Core_CenterRect(&on, self.right - self.left, self.bottom - self.top, &mi.rcWork, &placed);
    SetWindowPos(d, NULL, placed.left, placed.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* A dialog whose owner is hidden (the manager started only to uninstall) gets
 * its own taskbar button and icon, so it cannot get lost behind other windows. */
static void TaskbarButtonIfOwnerHidden(HWND d)
{
    HWND owner = GetWindow(d, GW_OWNER);
    if (owner && IsWindowVisible(owner)) return;
    SetWindowLongPtrW(d, GWL_EXSTYLE, GetWindowLongPtrW(d, GWL_EXSTYLE) | WS_EX_APPWINDOW);
    SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP)));
    SendMessageW(d, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_SHARED));
}

typedef struct DialogShell {
    DLGPROC proc;
    LPARAM  param;
} DialogShell;

#define SHELL_PROP L"ClaudeDesktopProfilesManager.Dialog"

/* What every dialog shares, around its own procedure (see Ui_Dialog). */
static INT_PTR CALLBACK ShellProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    DialogShell *shell = (DialogShell *)GetPropW(d, SHELL_PROP);
    INT_PTR r;
    if (msg == WM_INITDIALOG) {
        shell = (DialogShell *)lp;
        SetPropW(d, SHELL_PROP, shell);
        r = shell->proc(d, msg, wp, shell->param);   /* laid out and filled first: then its size is known */
        CenterOnOwner(d);
        TaskbarButtonIfOwnerHidden(d);
        Theme_Apply(d);
        return r;
    }
    if (!shell) return FALSE;
    if (msg == WM_SETTINGCHANGE || msg == WM_SYSCOLORCHANGE) Theme_Follow(d, msg, wp, lp);
    r = shell->proc(d, msg, wp, lp);
    if (!r && msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) r = Theme_CtlColor(msg, wp, lp, 0);
    if (msg == WM_NCDESTROY) {
        Theme_Forget(d);
        RemovePropW(d, SHELL_PROP);
    }
    return r;
}

/* Every dialog of the program opens here, modal to `owner`, and so gets what
 * they all share: once `proc` has set it up (WM_INITDIALOG brings `param`),
 * it is centered on its owner (on its monitor, with a taskbar button of its
 * own, when the owner is hidden) and themed; its colors are the theme's
 * unless `proc` answers WM_CTLCOLOR* itself; it follows theme changes. */
INT_PTR Ui_Dialog(HWND owner, int id, DLGPROC proc, LPARAM param)
{
    DialogShell shell;
    shell.proc = proc;
    shell.param = param;
    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(id), owner, ShellProc, (LPARAM)&shell);
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

static void LayoutMessage(HWND d, MessageBoxState *s)
{
    HWND text = GetDlgItem(d, IDC_M_TEXT), ok = GetDlgItem(d, IDOK), cancel = GetDlgItem(d, IDCANCEL);
    RECT rt, rok, rwin, calc;
    HDC dc;
    HFONT old;
    int grow;

    GetWindowRect(text, &rt);
    MapWindowPoints(NULL, d, (POINT *)&rt, 2);
    calc = rt;
    dc = GetDC(text);
    old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(text, WM_GETFONT, 0, 0));
    DrawTextW(dc, s->text, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_EXPANDTABS);
    SelectObject(dc, old);
    ReleaseDC(text, dc);
    grow = (calc.bottom - calc.top) - (rt.bottom - rt.top);
    if (grow < 0) grow = 0;
    SetWindowPos(text, NULL, 0, 0, rt.right - rt.left, (rt.bottom - rt.top) + grow, SWP_NOMOVE | SWP_NOZORDER);

    GetWindowRect(ok, &rok);
    MapWindowPoints(NULL, d, (POINT *)&rok, 2);
    if (!s->cancel) {
        /* Single button: take the right-hand slot. */
        RECT rc;
        GetWindowRect(cancel, &rc);
        MapWindowPoints(NULL, d, (POINT *)&rc, 2);
        ShowWindow(cancel, SW_HIDE);
        SetWindowPos(ok, NULL, rc.left, rc.top + grow, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    } else {
        SetWindowPos(ok, NULL, rok.left, rok.top + grow, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        GetWindowRect(cancel, &rok);
        MapWindowPoints(NULL, d, (POINT *)&rok, 2);
        SetWindowPos(cancel, NULL, rok.left, rok.top + grow, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    GetWindowRect(d, &rwin);
    SetWindowPos(d, NULL, 0, 0, rwin.right - rwin.left, (rwin.bottom - rwin.top) + grow, SWP_NOMOVE | SWP_NOZORDER);
}

static void SetMessageIcon(HWND d, MessageBoxState *s)
{
    int px = MulDiv(32, (int)GetDpiForWindow(d), 96);
    HICON icon = NULL;
    if (!s->icon || FAILED(LoadIconWithScaleDown(NULL, s->icon, px, px, &icon))) return;
    SendDlgItemMessageW(d, IDC_M_ICON, STM_SETICON, (WPARAM)icon, 0);
    if (s->hicon) DestroyIcon(s->hicon);
    s->hicon = icon;
}

#define WM_APP_MESSAGE_DPI (WM_APP + 1)

static INT_PTR CALLBACK MessageProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    MessageBoxState *s = (MessageBoxState *)GetWindowLongPtrW(d, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        s = (MessageBoxState *)lp;
        SetWindowLongPtrW(d, DWLP_USER, lp);
        SetWindowTextW(d, APP_NAME);
        SetDlgItemTextW(d, IDC_M_TEXT, s->text);
        SetDlgItemTextW(d, IDOK, s->ok);
        if (s->cancel) SetDlgItemTextW(d, IDCANCEL, s->cancel);
        SetMessageIcon(d, s);
        LayoutMessage(d, s);
        SetForegroundWindow(d);   /* a question is always in front, even when its owner is not */
        if (s->cancel && s->defaultCancel) {
            SendMessageW(d, DM_SETDEFID, IDCANCEL, 0);
            SetFocus(GetDlgItem(d, IDCANCEL));
            return FALSE;
        }
        return TRUE;
    case WM_DPICHANGED:
        PostMessageW(d, WM_APP_MESSAGE_DPI, 0, 0);
        break;
    case WM_APP_MESSAGE_DPI:
        if (s) SetMessageIcon(d, s);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(d, LOWORD(wp) == IDCANCEL && !s->cancel ? IDOK : LOWORD(wp));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (s && s->hicon) DestroyIcon(s->hicon);
        break;
    }
    return FALSE;
}

/* A themed message box. Returns TRUE when the user picked the first button. */
BOOL Ui_Ask(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel)
{
    MessageBoxState s;
    ZeroMemory(&s, sizeof s);
    s.text = text;
    s.ok = ok;
    s.cancel = cancel;
    s.icon = icon;
    s.defaultCancel = defaultCancel;
    return Ui_Dialog(owner, IDD_MESSAGE, MessageProc, (LPARAM)&s) == IDOK;
}

/* MessageBox-compatible wrapper: MB_OK, MB_OKCANCEL or MB_YESNO, an MB_ICON*
 * and optionally MB_DEFBUTTON2. */
int Util_Message(HWND owner, UINT flags, const WCHAR *fmt, ...)
{
    WCHAR text[2048];
    LPCWSTR icon = NULL;
    va_list ap;
    UINT buttons = flags & MB_TYPEMASK;
    BOOL first;

    va_start(ap, fmt);
    StringCchVPrintfW(text, ARRAYSIZE(text), fmt, ap);
    va_end(ap);
    switch (flags & MB_ICONMASK) {
    case MB_ICONERROR:       icon = IDI_ERROR; break;
    case MB_ICONWARNING:     icon = IDI_WARNING; break;
    case MB_ICONINFORMATION: icon = IDI_INFORMATION; break;
    case MB_ICONQUESTION:    icon = IDI_QUESTION; break;
    }
    if (buttons == MB_YESNO) {
        first = Ui_Ask(owner, icon, text, L"Yes", L"No", (flags & MB_DEFMASK) == MB_DEFBUTTON2);
        return first ? IDYES : IDNO;
    }
    if (buttons == MB_OKCANCEL) {
        first = Ui_Ask(owner, icon, text, L"OK", L"Cancel", (flags & MB_DEFMASK) == MB_DEFBUTTON2);
        return first ? IDOK : IDCANCEL;
    }
    Ui_Ask(owner, icon, text, L"OK", NULL, FALSE);
    return IDOK;
}
