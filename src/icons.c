/*
 * Profile icons: the installed Claude icon with a colored badge carrying the
 * profile's initial, so shortcuts and taskbar pins can be told apart; the
 * badge alone; and Claude's notification-area glyph in the profile's color.
 *
 * Plain GDI and straight-alpha BGRA buffers (0xAARRGGBB): the Claude icon is
 * extracted at each size by Windows, the badge is drawn with analytic
 * anti-aliasing, and the .ico is written by hand (32-bit DIB entries). The
 * initial is rasterized once, large, by GDI; each icon size samples it with
 * exact area coverage, centered on the middle of its ink rather than on its
 * text box, so it sits in the middle of the badge at every size. The Claude
 * artwork is never shipped: it is read from the user's own installation.
 */
#include "app.h"
#include "resource.h"
#include <math.h>
#include <string.h>

#define MAX_ICON_SIZE        256
#define MIN_ICON_SIZE        8
#define MIN_BADGE_SIZE       4
#define MIN_LETTER_SIZE      24   /* a smaller badge has no room for a legible initial */
#define TRAY_LIGHTEN_PERCENT 35   /* on a dark taskbar the darker colors of the palette fade into it */
#define WHITE                RGB(255, 255, 255)

/* A profile icon's badge: a disc in its bottom-right corner, in a white
 * ring. On a small icon it takes a larger share, so that it stays visible,
 * and its ring is one pixel. */
#define LARGE_ICON_SIZE        32
#define BADGE_SHARE            0.54
#define SMALL_ICON_BADGE_SHARE 0.62
#define BADGE_RING_DIVISOR     22.0   /* a large icon's ring: its size / this */
/* The badge alone (Icons_CreateBadge) fills its square, with a thicker ring
 * from LONE_BADGE_THICK_RING_SIZE. */
#define LONE_BADGE_RING_DIVISOR     11.0
#define LONE_BADGE_THICK_RING_SIZE  22
/* The initial's ink: as high as this share of the disc inside the ring, and
 * at most as wide as the other one (W, M). */
#define LETTER_HEIGHT_SHARE    0.56
#define LETTER_MAX_WIDTH_SHARE 0.72
#define MIN_LETTER_COVERAGE    0.002   /* less would not change the pixel once rounded */

/* The .ico layout: an ICONDIR, one ICONDIRENTRY per size, then the images. */
#define ICONDIR_BYTES       6
#define ICONDIRENTRY_BYTES  16
#define ICON_BITS_PER_PIXEL 32
#define ICON_EXTENSION      L".ico"
#define TEMPORARY_EXTENSION L".tmp"
#define DWORD_DIGITS        10   /* 4294967295: a process ID in a file name */

const WCHAR *const g_ColorNames[PALETTE_SIZE] = {
    L"Orange", L"Blue", L"Green", L"Purple", L"Red", L"Teal", L"Pink", L"Slate",
};

static const COLORREF kPalette[PALETTE_SIZE] = {
    RGB(0xE0, 0x7A, 0x3F), RGB(0x25, 0x63, 0xEB), RGB(0x16, 0xA3, 0x4A), RGB(0x7C, 0x3A, 0xED),
    RGB(0xDC, 0x26, 0x26), RGB(0x0D, 0x94, 0x88), RGB(0xDB, 0x27, 0x77), RGB(0x47, 0x55, 0x69),
};

/* The sizes Windows asks for at every scale from 100 % to 300 % (100, 125,
 * 150, 175, 200, 225, 250 and 300 %) of small icons (16 at 100 %), taskbar
 * buttons (24) and large icons (32), and 256 for Explorer's largest views.
 * Without its own size, an icon is scaled from another one and the initial
 * comes out blurred. */
static const int kIcoSizes[] = { 16, 20, 24, 28, 30, 32, 36, 40, 42, 48, 54, 56, 60, 64, 72, 80, 96, 256 };

/* -------------------------------------------------------------- pixels */

static void *Alloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); }
static void Free(void *memory) { if (memory) HeapFree(GetProcessHeap(), 0, memory); }

static COLORREF PaletteColor(int index)
{
    return kPalette[index >= 0 && index < PALETTE_SIZE ? index : 0];
}

/* A size x size, 32-bit DIB whose rows run top-down, as in the pixel buffers. */
static void InitTopDownBitmapInfo(BITMAPINFO *info, int size)
{
    ZeroMemory(info, sizeof *info);
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = size;
    info->bmiHeader.biHeight = -size;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
}

static BOOL IconToPixels(HICON icon, int size, DWORD *pixels)
{
    ICONINFO parts;
    BITMAP color;
    BITMAPINFO info;
    HDC screen;
    BOOL ok = FALSE, hasAlpha = FALSE;
    int i, count = size * size;

    if (!GetIconInfo(icon, &parts)) return FALSE;
    if (parts.hbmColor && GetObjectW(parts.hbmColor, sizeof color, &color) == sizeof color &&
        color.bmWidth == size && color.bmHeight == size) {
        InitTopDownBitmapInfo(&info, size);
        screen = GetDC(NULL);
        if (GetDIBits(screen, parts.hbmColor, 0, (UINT)size, pixels, &info, DIB_RGB_COLORS) == size) {
            ok = TRUE;
            for (i = 0; i < count && !hasAlpha; i++) hasAlpha = (pixels[i] >> 24) != 0;
            if (!hasAlpha) {
                /* Old-style icon: opacity lives in the AND mask. */
                DWORD *mask = (DWORD *)Alloc((size_t)count * sizeof(DWORD));
                if (mask && parts.hbmMask && GetDIBits(screen, parts.hbmMask, 0, (UINT)size, mask, &info, DIB_RGB_COLORS) == size) {
                    for (i = 0; i < count; i++) pixels[i] = (mask[i] & 0xFFFFFF) ? 0 : (pixels[i] | 0xFF000000);
                } else {
                    for (i = 0; i < count; i++) pixels[i] |= 0xFF000000;
                }
                Free(mask);
            }
        }
        ReleaseDC(NULL, screen);
    }
    if (parts.hbmColor) DeleteObject(parts.hbmColor);
    if (parts.hbmMask) DeleteObject(parts.hbmMask);
    return ok;
}

/* Takes `icon` (NULL: none): its pixels at size x size, then it is destroyed. */
static BOOL TakeIconPixels(HICON icon, int size, DWORD *pixels)
{
    BOOL ok;
    if (!icon) return FALSE;
    ok = IconToPixels(icon, size, pixels);
    DestroyIcon(icon);
    return ok;
}

static HICON ExtractSizedIcon(const WCHAR *file, int size)
{
    HICON icon = NULL;
    UINT id = 0;
    return PrivateExtractIconsW(file, 0, size, size, &icon, &id, 1, 0) == 1 ? icon : NULL;
}

static HICON ManagerIcon(int size)
{
    return (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
}

/* An icon's AND mask (1: transparent) from the pixels' alpha, in the rows of
 * the bitmap it goes in. `mask` starts zeroed. */
static void BuildAndMask(const DWORD *pixels, int size, BYTE *mask, size_t rowBytes, BOOL bottomUp)
{
    int x, y;
    for (y = 0; y < size; y++) {
        const DWORD *row = pixels + (size_t)(bottomUp ? size - 1 - y : y) * size;
        BYTE *bits = mask + (size_t)y * rowBytes;
        for (x = 0; x < size; x++)
            if ((row[x] >> 24) == 0) bits[x / 8] |= (BYTE)(0x80 >> (x % 8));
    }
}

/* `color` at opacity `alpha` over a straight-alpha pixel. */
static void BlendOver(DWORD *pixel, COLORREF color, double alpha)
{
    double below, out, red, green, blue;
    if (alpha <= 0.0) return;
    if (alpha > 1.0) alpha = 1.0;
    below = ((*pixel >> 24) & 0xFF) / 255.0;
    out = alpha + below * (1.0 - alpha);
    red = (GetRValue(color) * alpha + ((*pixel >> 16) & 0xFF) * below * (1.0 - alpha)) / out;
    green = (GetGValue(color) * alpha + ((*pixel >> 8) & 0xFF) * below * (1.0 - alpha)) / out;
    blue = (GetBValue(color) * alpha + (*pixel & 0xFF) * below * (1.0 - alpha)) / out;
    *pixel = ((DWORD)(out * 255.0 + 0.5) << 24) | ((DWORD)(red + 0.5) << 16) | ((DWORD)(green + 0.5) << 8) |
             (DWORD)(blue + 0.5);
}

static double Clamp01(double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

/* ------------------------------------------------------------- initial */

/* The initial, drawn once at GLYPH_EM pixels on a GLYPH_CANVAS square: a
 * summed-area table of its coverage (0..255) over the box around its ink. */
#define GLYPH_CANVAS 512
#define GLYPH_EM     256
#define GLYPH_CACHE  8   /* initials kept: every list refresh draws the profiles' again */

typedef struct Glyph {
    WCHAR  letter;
    int    width, height;   /* the ink box, in canvas pixels */
    DWORD *sum;             /* (width + 1) x (height + 1) */
} Glyph;

static SRWLOCK g_glyphLock = SRWLOCK_INIT;
static Glyph g_glyphCache[GLYPH_CACHE];
static int g_glyphCacheNext;   /* the entry replaced next */

static BOOL RasterizeLetter(WCHAR letter, Glyph *glyph)
{
    const int canvas = GLYPH_CANVAS;
    BITMAPINFO info;
    void *bits = NULL;
    const DWORD *pixels;
    HDC dc;
    HBITMAP bitmap, oldBitmap;
    HFONT font, oldFont;
    TEXTMETRICW metrics;
    SIZE extent;
    int x, y, stride, left = canvas, top = canvas, right = -1, bottom = -1;
    BOOL ok = FALSE;

    ZeroMemory(glyph, sizeof *glyph);
    InitTopDownBitmapInfo(&info, canvas);
    dc = CreateCompatibleDC(NULL);
    bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    font = bitmap ? CreateFontW(-GLYPH_EM, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI")
                  : NULL;
    if (!font || !bits) {
        if (font) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return FALSE;
    }
    oldBitmap = (HBITMAP)SelectObject(dc, bitmap);
    ZeroMemory(bits, (size_t)canvas * canvas * sizeof(DWORD));
    oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, WHITE);
    /* Unmeasured, the letter is not drawn: no ink, no glyph cached. */
    if (GetTextMetricsW(dc, &metrics) && GetTextExtentPoint32W(dc, &letter, 1, &extent))
        TextOutW(dc, (canvas - extent.cx) / 2, (canvas - metrics.tmHeight) / 2, &letter, 1);
    GdiFlush();

    pixels = (const DWORD *)bits;
    for (y = 0; y < canvas; y++)
        for (x = 0; x < canvas; x++)
            if ((pixels[y * canvas + x] >> 8) & 0xFF) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
    if (right >= left) {
        glyph->width = right - left + 1;
        glyph->height = bottom - top + 1;
        stride = glyph->width + 1;
        glyph->sum = (DWORD *)Alloc((size_t)stride * (glyph->height + 1) * sizeof(DWORD));
        if (glyph->sum) {
            for (y = 0; y < glyph->height; y++) {
                DWORD row = 0;
                for (x = 0; x < glyph->width; x++) {
                    row += (pixels[(top + y) * canvas + left + x] >> 8) & 0xFF;
                    glyph->sum[(y + 1) * stride + x + 1] = glyph->sum[y * stride + x + 1] + row;
                }
            }
            glyph->letter = letter;
            ok = TRUE;
        }
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return ok;
}

/* A copy of the initial's glyph (free its `sum`), from the cache, else drawn
 * and kept there. Its `sum` stays NULL when it cannot be drawn: the badge
 * then has no initial. */
static void LoadGlyph(WCHAR letter, Glyph *glyph)
{
    const Glyph *cached = NULL;
    Glyph drawn;
    size_t bytes;
    int i;
    ZeroMemory(glyph, sizeof *glyph);
    AcquireSRWLockExclusive(&g_glyphLock);
    for (i = 0; i < GLYPH_CACHE && !cached; i++)
        if (g_glyphCache[i].sum && g_glyphCache[i].letter == letter) cached = &g_glyphCache[i];
    if (!cached && RasterizeLetter(letter, &drawn)) {
        Free(g_glyphCache[g_glyphCacheNext].sum);
        g_glyphCache[g_glyphCacheNext] = drawn;
        cached = &g_glyphCache[g_glyphCacheNext];
        g_glyphCacheNext = (g_glyphCacheNext + 1) % GLYPH_CACHE;
    }
    if (cached) {
        bytes = (size_t)(cached->width + 1) * (cached->height + 1) * sizeof(DWORD);
        *glyph = *cached;
        glyph->sum = (DWORD *)HeapAlloc(GetProcessHeap(), 0, bytes);
        if (glyph->sum) memcpy(glyph->sum, cached->sum, bytes);
    }
    ReleaseSRWLockExclusive(&g_glyphLock);
}

/* Coverage summed over [0,x) x [0,y) of the ink box, at any real point: the
 * table is bilinear inside each canvas pixel, so this is exact. */
static double GlyphSum(const Glyph *glyph, double x, double y)
{
    const int stride = glyph->width + 1;
    int column, row;
    double fx, fy, a, b, c, d;
    if (x <= 0.0 || y <= 0.0) return 0.0;
    if (x > glyph->width) x = glyph->width;
    if (y > glyph->height) y = glyph->height;
    column = (int)x;
    row = (int)y;
    if (column >= glyph->width) column = glyph->width - 1;
    if (row >= glyph->height) row = glyph->height - 1;
    fx = x - column;
    fy = y - row;
    a = glyph->sum[row * stride + column];
    b = glyph->sum[row * stride + column + 1];
    c = glyph->sum[(row + 1) * stride + column];
    d = glyph->sum[(row + 1) * stride + column + 1];
    return a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy;
}

/* The white initial, its ink centered on (centerX, centerY) and fitted inside
 * a disc of radius `radius`. */
static void DrawLetter(DWORD *pixels, int size, double centerX, double centerY, double radius, const Glyph *glyph)
{
    double scale, inverse, half;
    int x, y, left, right, top, bottom;

    if (!glyph->sum) return;
    scale = (radius * 2.0 * LETTER_HEIGHT_SHARE) / glyph->height;   /* output pixels per canvas pixel */
    if (glyph->width * scale > radius * 2.0 * LETTER_MAX_WIDTH_SHARE) scale = (radius * 2.0 * LETTER_MAX_WIDTH_SHARE) / glyph->width;
    inverse = 1.0 / scale;
    half = (glyph->width > glyph->height ? glyph->width : glyph->height) * scale / 2.0 + 1.0;
    left = (int)floor(centerX - half);
    right = (int)ceil(centerX + half);
    top = (int)floor(centerY - half);
    bottom = (int)ceil(centerY + half);
    for (y = top < 0 ? 0 : top; y < bottom && y < size; y++) {
        for (x = left < 0 ? 0 : left; x < right && x < size; x++) {
            double gx0 = glyph->width / 2.0 + (x - centerX) * inverse, gx1 = glyph->width / 2.0 + (x + 1 - centerX) * inverse;
            double gy0 = glyph->height / 2.0 + (y - centerY) * inverse, gy1 = glyph->height / 2.0 + (y + 1 - centerY) * inverse;
            double cover = GlyphSum(glyph, gx1, gy1) - GlyphSum(glyph, gx0, gy1) - GlyphSum(glyph, gx1, gy0) +
                           GlyphSum(glyph, gx0, gy0);
            cover /= 255.0 * (gx1 - gx0) * (gy1 - gy0);
            if (cover > MIN_LETTER_COVERAGE) BlendOver(&pixels[y * size + x], WHITE, cover);
        }
    }
}

/* The letter a profile's badge shows: the first letter or digit of its name,
 * upper case; 0 for none. */
WCHAR Icons_ProfileInitial(const WCHAR *name)
{
    WORD type;
    for (; name && *name; name++) {
        WCHAR c = *name;
        if (GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & (C1_ALPHA | C1_DIGIT))) {
            WCHAR upper = c;
            LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, &c, 1, &upper, 1, NULL, NULL, 0);
            return upper;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- badge */

/* A disc of radius `radius` in `color` inside a white ring of width `ring`. */
static void DrawDisc(DWORD *pixels, int size, double centerX, double centerY, double radius, double ring, COLORREF color)
{
    double inner = radius - ring;
    int x, y, left = (int)floor(centerX - radius) - 1, top = (int)floor(centerY - radius) - 1;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    for (y = top; y < size && y <= centerY + radius + 1; y++) {
        for (x = left; x < size && x <= centerX + radius + 1; x++) {
            double distance = sqrt((x + 0.5 - centerX) * (x + 0.5 - centerX) + (y + 0.5 - centerY) * (y + 0.5 - centerY));
            BlendOver(&pixels[y * size + x], WHITE, Clamp01(radius + 0.5 - distance));
            BlendOver(&pixels[y * size + x], color, Clamp01(inner + 0.5 - distance));
        }
    }
}

static void DrawBadge(DWORD *pixels, int size, COLORREF color, const Glyph *glyph)
{
    double diameter = size >= LARGE_ICON_SIZE ? size * BADGE_SHARE : size * SMALL_ICON_BADGE_SHARE;
    double radius = diameter / 2.0, centerX = size - radius, centerY = size - radius;
    double ring = size >= LARGE_ICON_SIZE ? size / BADGE_RING_DIVISOR : 1.0;

    DrawDisc(pixels, size, centerX, centerY, radius, ring, color);
    if (size >= MIN_LETTER_SIZE) DrawLetter(pixels, size, centerX, centerY, radius - ring, glyph);
}

/* The exe the Claude artwork is read from: the package's, or the current
 * package's when Claude was updated since `pkg` was looked up. FALSE: there
 * is none, the manager's own artwork is used. */
static BOOL ClaudeArtExe(const ClaudePackage *pkg, WCHAR *exe, size_t cch)
{
    ClaudePackage current;
    if (!pkg || !pkg->found) return FALSE;
    if (Util_FileExists(pkg->exe)) return SUCCEEDED(StringCchCopyW(exe, cch, pkg->exe));
    return Claude_FindPackage(&current) && SUCCEEDED(StringCchCopyW(exe, cch, current.exe));
}

/* The profile's icon at size x size: the artwork of `artExe` (NULL: the
 * manager's own) with the badge. FALSE when that artwork cannot be read. */
static BOOL RenderProfileIcon(const WCHAR *artExe, const Profile *profile, int size, DWORD *pixels, const Glyph *glyph)
{
    if (!TakeIconPixels(artExe ? ExtractSizedIcon(artExe, size) : ManagerIcon(size), size, pixels)) return FALSE;
    DrawBadge(pixels, size, PaletteColor(profile->color), glyph);
    return TRUE;
}

/* ------------------------------------------------------------ .ico file */

static void AppendWord(BYTE **cursor, WORD value)
{
    (*cursor)[0] = (BYTE)value;
    (*cursor)[1] = (BYTE)(value >> 8);
    *cursor += 2;
}

static void AppendDword(BYTE **cursor, DWORD value)
{
    AppendWord(cursor, (WORD)value);
    AppendWord(cursor, (WORD)(value >> 16));
}

static size_t IcoMaskRowBytes(int size) { return (size_t)((size + 31) / 32) * 4; }

/* One size in the .ico: its header, its pixels and its AND mask. */
static size_t IcoImageBytes(int size)
{
    return sizeof(BITMAPINFOHEADER) + (size_t)size * size * sizeof(DWORD) + IcoMaskRowBytes(size) * size;
}

static volatile LONG g_iconFailureLogged;

/* TRUE the first time in this process: an icon that cannot be made is asked
 * for again at every refresh, and logged once. */
static BOOL FirstIconFailure(void)
{
    return InterlockedExchange(&g_iconFailureLogged, TRUE) == FALSE;
}

/* `data` in `path`, through a temporary file of this process (the manager and
 * the watchers can make the same icon at once) flushed before it takes the
 * name: a file a power cut left damaged would never be made again, its name
 * being the one of its image. FALSE: `*error` says why. */
static BOOL SaveIconFile(const WCHAR *path, const BYTE *data, DWORD bytes, DWORD *error)
{
    WCHAR temporary[MAX_PATH];
    HANDLE output;
    DWORD written = 0;
    *error = ERROR_SUCCESS;
    if (FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.%lu" TEMPORARY_EXTENSION, path, GetCurrentProcessId()))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    output = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return FALSE;
    }
    if (!WriteFile(output, data, bytes, &written, NULL)) *error = GetLastError();
    else if (written != bytes) *error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(output)) *error = GetLastError();
    CloseHandle(output);
    if (*error == ERROR_SUCCESS && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING)) *error = GetLastError();
    if (*error == ERROR_SUCCESS) return TRUE;
    DeleteFileW(temporary);
    return FALSE;
}

/* Every size of the profile's icon, drawn on `artExe`'s artwork (NULL: the
 * manager's), in one .ico. FALSE when the artwork cannot be read at a size:
 * the file is then not written. */
static BOOL WriteIconFile(const WCHAR *path, const WCHAR *artExe, const Profile *profile)
{
    const int count = ARRAYSIZE(kIcoSizes);
    const size_t headerBytes = ICONDIR_BYTES + ICONDIRENTRY_BYTES * (size_t)count;
    size_t total = headerBytes, offset;
    BYTE *file, *directory, *image;
    DWORD *pixels, error;
    BOOL ok = FALSE;
    int i, x, y;
    Glyph glyph;
    WCHAR letter = Icons_ProfileInitial(profile->name);

    ZeroMemory(&glyph, sizeof glyph);
    for (i = 0; i < count; i++) total += IcoImageBytes(kIcoSizes[i]);
    file = (BYTE *)Alloc(total);
    pixels = (DWORD *)Alloc((size_t)MAX_ICON_SIZE * MAX_ICON_SIZE * sizeof(DWORD));
    if (!file || !pixels) {
        if (FirstIconFailure()) Util_Log(L"could not make icon %s: not enough memory", path);
        goto done;
    }
    if (letter) LoadGlyph(letter, &glyph);

    directory = file;
    AppendWord(&directory, 0);
    AppendWord(&directory, 1);   /* an icon, not a cursor */
    AppendWord(&directory, (WORD)count);
    offset = headerBytes;
    for (i = 0; i < count; i++) {
        int size = kIcoSizes[i];
        DWORD bytes = (DWORD)IcoImageBytes(size);
        if (!RenderProfileIcon(artExe, profile, size, pixels, &glyph)) {
            if (FirstIconFailure())
                Util_Log(L"could not read the artwork of %s at %d pixels for %s", artExe ? artExe : L"the manager", size, path);
            goto done;
        }
        *directory++ = (BYTE)(size >= MAX_ICON_SIZE ? 0 : size);   /* 0 stands for 256 */
        *directory++ = (BYTE)(size >= MAX_ICON_SIZE ? 0 : size);
        *directory++ = 0;
        *directory++ = 0;
        AppendWord(&directory, 1);
        AppendWord(&directory, ICON_BITS_PER_PIXEL);
        AppendDword(&directory, bytes);
        AppendDword(&directory, (DWORD)offset);

        image = file + offset;
        AppendDword(&image, (DWORD)sizeof(BITMAPINFOHEADER));
        AppendDword(&image, (DWORD)size);
        AppendDword(&image, (DWORD)(size * 2));   /* the pixels, then the AND mask */
        AppendWord(&image, 1);
        AppendWord(&image, ICON_BITS_PER_PIXEL);
        AppendDword(&image, BI_RGB);
        AppendDword(&image, bytes - (DWORD)sizeof(BITMAPINFOHEADER));
        AppendDword(&image, 0);
        AppendDword(&image, 0);
        AppendDword(&image, 0);
        AppendDword(&image, 0);
        for (y = size - 1; y >= 0; y--)                 /* bottom-up rows */
            for (x = 0; x < size; x++) AppendDword(&image, pixels[y * size + x]);
        BuildAndMask(pixels, size, image, IcoMaskRowBytes(size), TRUE);
        offset += bytes;
    }

    /* Made by another process meanwhile: the same name is the same image. */
    ok = SaveIconFile(path, file, (DWORD)total, &error) || Util_FileExists(path);
    if (!ok && FirstIconFailure()) Util_Log(L"could not write icon %s (error %lu)", path, error);
done:
    Free(glyph.sum);
    Free(file);
    Free(pixels);
    return ok;
}

/* ------------------------------------------------------------------ API */

static BOOL IconsDir(WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\icons", state));
}

/* The file name encodes what the icon shows (name, color, whether it is drawn
 * on the Claude artwork, and the drawing's revision), so a change produces a
 * new file and Explorer's icon cache cannot serve a stale image. */
#define ICON_REVISION 4
static BOOL IconFilePath(const WCHAR *directory, const Profile *profile, BOOL claudeArt, WCHAR *out, size_t cch)
{
    WCHAR style[LABEL_CCH + 16];
    StringCchPrintfW(style, ARRAYSIZE(style), L"%s|%d|%c|%d", profile->name, profile->color, claudeArt ? L'c' : L'x', ICON_REVISION);
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%08lX-%08lX" ICON_EXTENSION, directory,
                                      (unsigned long)Core_HashIgnoringCase(profile->folder),
                                      (unsigned long)Core_HashIgnoringCase(style)));
}

/* Every icon file of the profile, whatever it shows; with `temporaries`, the
 * temporary files they are written through (IsTemporaryIcon). */
static BOOL ProfileIconPattern(const WCHAR *directory, const Profile *profile, BOOL temporaries, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%08lX-*" ICON_EXTENSION L"%s", directory,
                                      (unsigned long)Core_HashIgnoringCase(profile->folder),
                                      temporaries ? L"*" TEMPORARY_EXTENSION : L""));
}

/* Each file matching `pattern` in `directory`, by its path, until `visit`
 * returns FALSE. */
typedef BOOL (*IconFileVisitor)(const WCHAR *path, void *context);

static void VisitIconFiles(const WCHAR *directory, const WCHAR *pattern, IconFileVisitor visit, void *context)
{
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", directory, found.cFileName)) && !visit(path, context))
            break;
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

/* Whether `path` is a temporary icon file: "<icon>.<process ID>.tmp", that
 * process in `*writer`, or "<icon>.tmp" as release 1.0 named them, with no
 * writer (0): its processes are gone once this release is installed. */
static BOOL IsTemporaryIcon(const WCHAR *path, DWORD *writer)
{
    const WCHAR *name = wcsrchr(path, L'\\'), *character;
    int digits = 0;
    *writer = 0;
    if (!name || (character = wcsstr(name, ICON_EXTENSION)) == NULL) return FALSE;
    character += ARRAYSIZE(ICON_EXTENSION) - 1;
    if (Core_EqualsI(character, TEMPORARY_EXTENSION)) return TRUE;
    if (*character++ != L'.') return FALSE;
    for (; *character >= L'0' && *character <= L'9' && digits < DWORD_DIGITS; character++, digits++)
        *writer = *writer * 10 + (DWORD)(*character - L'0');
    return digits && Core_EqualsI(character, TEMPORARY_EXTENSION);
}

/* The process has exited. Its ID may name another one since: a file of the
 * ended one then waits for a later sweep. */
static BOOL ProcessEnded(DWORD processId)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    BOOL ended;
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return ended;
}

BOOL Icons_Ensure(const ClaudePackage *pkg, const Profile *profile, WCHAR *out, size_t cch)
{
    WCHAR directory[MAX_PATH], artExe[MAX_PATH];
    BOOL claudeArt = pkg && pkg->found;
    if (!IconsDir(directory, ARRAYSIZE(directory)) || !IconFilePath(directory, profile, claudeArt, out, cch)) return FALSE;
    if (Util_FileExists(out)) return TRUE;
    if (!Util_EnsureDir(directory)) return FALSE;
    if (!claudeArt) return WriteIconFile(out, NULL, profile);
    if (ClaudeArtExe(pkg, artExe, ARRAYSIZE(artExe)) && WriteIconFile(out, artExe, profile)) return TRUE;
    /* Claude's artwork could not be read: the manager's, under its own name,
     * so that Icons_IsStale offers Claude's again later. */
    return IconFilePath(directory, profile, FALSE, out, cch) && (Util_FileExists(out) || WriteIconFile(out, NULL, profile));
}

static HICON PixelsToIcon(const DWORD *pixels, int size)
{
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP color = NULL, mask = NULL;
    BYTE *maskBits = NULL;
    size_t maskRowBytes = (size_t)((size + 15) / 16) * 2;   /* a monochrome bitmap's rows are WORD-aligned */
    ICONINFO parts;
    HICON icon = NULL;
    HDC screen;

    InitTopDownBitmapInfo(&info, size);
    screen = GetDC(NULL);
    color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color || !bits) goto done;
    memcpy(bits, pixels, (size_t)size * size * sizeof(DWORD));
    maskBits = (BYTE *)Alloc(maskRowBytes * size);
    if (!maskBits) goto done;
    BuildAndMask(pixels, size, maskBits, maskRowBytes, FALSE);
    mask = CreateBitmap(size, size, 1, 1, maskBits);
    if (!mask) goto done;
    ZeroMemory(&parts, sizeof parts);
    parts.fIcon = TRUE;
    parts.hbmColor = color;
    parts.hbmMask = mask;
    icon = CreateIconIndirect(&parts);
done:
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    Free(maskBits);
    return icon;
}

HICON Icons_Create(const ClaudePackage *pkg, const Profile *profile, int size)
{
    WCHAR artExe[MAX_PATH];
    DWORD *pixels;
    HICON icon = NULL;
    Glyph glyph;
    WCHAR letter = Icons_ProfileInitial(profile->name);
    if (size < MIN_ICON_SIZE || size > MAX_ICON_SIZE) return NULL;
    pixels = (DWORD *)Alloc((size_t)size * size * sizeof(DWORD));
    if (!pixels) return NULL;
    ZeroMemory(&glyph, sizeof glyph);
    if (letter && size >= MIN_LETTER_SIZE) LoadGlyph(letter, &glyph);
    if ((ClaudeArtExe(pkg, artExe, ARRAYSIZE(artExe)) && RenderProfileIcon(artExe, profile, size, pixels, &glyph)) ||
        RenderProfileIcon(NULL, profile, size, pixels, &glyph))
        icon = PixelsToIcon(pixels, size);
    Free(glyph.sum);
    Free(pixels);
    return icon;
}

/* The badge alone, filling a size x size square, without the initial. */
HICON Icons_CreateBadge(int color, int size)
{
    DWORD *pixels;
    HICON icon;
    if (size < MIN_BADGE_SIZE || size > MAX_ICON_SIZE) return NULL;
    pixels = (DWORD *)Alloc((size_t)size * size * sizeof(DWORD));
    if (!pixels) return NULL;
    DrawDisc(pixels, size, size / 2.0, size / 2.0, size / 2.0,
             size >= LONE_BADGE_THICK_RING_SIZE ? size / LONE_BADGE_RING_DIVISOR : 1.0, PaletteColor(color));
    icon = PixelsToIcon(pixels, size);
    Free(pixels);
    return icon;
}

static COLORREF Lighten(COLORREF color, int percent)
{
    return RGB(GetRValue(color) + (255 - GetRValue(color)) * percent / 100,
               GetGValue(color) + (255 - GetGValue(color)) * percent / 100,
               GetBValue(color) + (255 - GetBValue(color)) * percent / 100);
}

/* Claude's notification-area glyph (its own file, read from the installed
 * package) in the profile's color, lightened on a dark taskbar. The badged
 * icon when that glyph cannot be read. */
HICON Icons_CreateTray(const ClaudePackage *pkg, const Profile *profile, int size, BOOL darkTaskbar)
{
    WCHAR path[MAX_PATH];
    HICON icon = NULL;
    DWORD *pixels, tint;
    COLORREF color = PaletteColor(profile->color);
    int i;

    if (size < MIN_ICON_SIZE || size > MAX_ICON_SIZE) return NULL;
    if (darkTaskbar) color = Lighten(color, TRAY_LIGHTEN_PERCENT);
    tint = ((DWORD)GetRValue(color) << 16) | ((DWORD)GetGValue(color) << 8) | GetBValue(color);
    pixels = (DWORD *)Alloc((size_t)size * size * sizeof(DWORD));
    if (pixels && Tray_ClaudeImagePath(pkg, FALSE, path, ARRAYSIZE(path)) &&
        TakeIconPixels((HICON)LoadImageW(NULL, path, IMAGE_ICON, size, size, LR_LOADFROMFILE), size, pixels)) {
        for (i = 0; i < size * size; i++) pixels[i] = (pixels[i] & 0xFF000000) | tint;
        icon = PixelsToIcon(pixels, size);
    }
    Free(pixels);
    return icon ? icon : Icons_Create(pkg, profile, size);
}

typedef struct StaleSearch {
    const WCHAR *current;
    BOOL         stale;
} StaleSearch;

static BOOL StaleVisitor(const WCHAR *path, void *context)
{
    StaleSearch *search = (StaleSearch *)context;
    search->stale = !Core_PathEquals(path, search->current);
    return !search->stale;
}

/* TRUE when this profile has an icon file other than its current one (its
 * name, color, artwork or drawing changed): its shortcuts, pins and open
 * windows should get the current icon. The current file may exist already (a
 * watcher makes it for the windows it tags). */
BOOL Icons_IsStale(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR current[MAX_PATH], directory[MAX_PATH], pattern[MAX_PATH];
    StaleSearch search;
    if (!pkg || !pkg->found || !IconsDir(directory, ARRAYSIZE(directory)) ||
        !IconFilePath(directory, profile, TRUE, current, ARRAYSIZE(current)) ||
        !ProfileIconPattern(directory, profile, FALSE, pattern, ARRAYSIZE(pattern)))
        return FALSE;
    search.current = current;
    search.stale = FALSE;
    VisitIconFiles(directory, pattern, StaleVisitor, &search);
    return search.stale;
}

static BOOL DeleteStaleVisitor(const WCHAR *path, void *context)
{
    const WCHAR *keep = (const WCHAR *)context;
    if ((!keep || !Core_PathEquals(path, keep)) && !TaskbarPin_UsesIcon(path)) DeleteFileW(path);
    return TRUE;
}

/* A temporary file a process left when it ended before renaming it. */
static BOOL DeleteOrphanVisitor(const WCHAR *path, void *context)
{
    DWORD writer;
    (void)context;
    if (IsTemporaryIcon(path, &writer) && (!writer || ProcessEnded(writer))) DeleteFileW(path);
    return TRUE;
}

/* Remove this profile's icon files except `keep` (NULL: all of them) and
 * the ones a taskbar pin still shows, and the temporary files left by
 * processes that ended while writing one. */
void Icons_DeleteStale(const Profile *profile, const WCHAR *keep)
{
    WCHAR directory[MAX_PATH], pattern[MAX_PATH];
    if (!IconsDir(directory, ARRAYSIZE(directory))) return;
    if (ProfileIconPattern(directory, profile, FALSE, pattern, ARRAYSIZE(pattern)))
        VisitIconFiles(directory, pattern, DeleteStaleVisitor, (void *)keep);
    if (ProfileIconPattern(directory, profile, TRUE, pattern, ARRAYSIZE(pattern)))
        VisitIconFiles(directory, pattern, DeleteOrphanVisitor, NULL);
}
