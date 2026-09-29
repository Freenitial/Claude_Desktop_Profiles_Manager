/*
 * Profile icons: the installed Claude icon with a colored badge carrying the
 * profile's initial, so shortcuts and taskbar pins can be told apart; the
 * badge alone; and Claude's notification-area glyph in the profile's color.
 *
 * Plain GDI and straight-alpha BGRA buffers (0xAARRGGBB): the Claude icon is
 * extracted at each size by Windows, the badge is drawn with analytic
 * anti-aliasing, and the .ico is written by hand (32-bit DIB entries). The
 * initial is rasterised once, large, by GDI; each icon size samples it with
 * exact area coverage, centred on the middle of its ink rather than on its
 * text box, so it sits in the middle of the badge at every size. The Claude
 * artwork is never shipped: it is read from the user's own installation.
 */
#include "app.h"
#include "resource.h"
#include <math.h>
#include <string.h>

const WCHAR *const g_ColorNames[PALETTE_SIZE] = {
    L"Orange", L"Blue", L"Green", L"Purple", L"Red", L"Teal", L"Pink", L"Slate",
};

static const COLORREF kPalette[PALETTE_SIZE] = {
    RGB(0xE0, 0x7A, 0x3F), RGB(0x25, 0x63, 0xEB), RGB(0x16, 0xA3, 0x4A), RGB(0x7C, 0x3A, 0xED),
    RGB(0xDC, 0x26, 0x26), RGB(0x0D, 0x94, 0x88), RGB(0xDB, 0x27, 0x77), RGB(0x47, 0x55, 0x69),
};

static const int kSizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };

/* -------------------------------------------------------------- pixels */

static void *Alloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void Free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static BOOL IconToPixels(HICON icon, int size, DWORD *px)
{
    ICONINFO ii;
    BITMAP bm;
    BITMAPINFO bi;
    HDC dc;
    BOOL ok = FALSE, alpha = FALSE;
    int i, n = size * size;

    if (!GetIconInfo(icon, &ii)) return FALSE;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) == sizeof bm &&
        bm.bmWidth == size && bm.bmHeight == size) {
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = GetDC(NULL);
        if (GetDIBits(dc, ii.hbmColor, 0, (UINT)size, px, &bi, DIB_RGB_COLORS) == size) {
            ok = TRUE;
            for (i = 0; i < n && !alpha; i++) alpha = (px[i] >> 24) != 0;
            if (!alpha) {
                /* Old-style icon: opacity lives in the AND mask. */
                DWORD *mask = (DWORD *)Alloc((size_t)n * 4);
                if (mask && ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, (UINT)size, mask, &bi, DIB_RGB_COLORS) == size) {
                    for (i = 0; i < n; i++) px[i] = (mask[i] & 0xFFFFFF) ? 0 : (px[i] | 0xFF000000);
                } else {
                    for (i = 0; i < n; i++) px[i] |= 0xFF000000;
                }
                Free(mask);
            }
        }
        ReleaseDC(NULL, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

static void Over(DWORD *d, COLORREF c, double a)
{
    double da, oa, r, g, b;
    if (a <= 0.0) return;
    if (a > 1.0) a = 1.0;
    da = ((*d >> 24) & 0xFF) / 255.0;
    oa = a + da * (1.0 - a);
    if (oa <= 0.0) { *d = 0; return; }
    r = (GetRValue(c) * a + ((*d >> 16) & 0xFF) * da * (1.0 - a)) / oa;
    g = (GetGValue(c) * a + ((*d >> 8) & 0xFF) * da * (1.0 - a)) / oa;
    b = (GetBValue(c) * a + (*d & 0xFF) * da * (1.0 - a)) / oa;
    *d = ((DWORD)(oa * 255.0 + 0.5) << 24) | ((DWORD)(r + 0.5) << 16) | ((DWORD)(g + 0.5) << 8) | (DWORD)(b + 0.5);
}

static double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* The initial, drawn once at GLYPH_EM pixels: a summed-area table of its
 * coverage (0..255) and the box around its ink. */
#define GLYPH_CANVAS 512
#define GLYPH_EM     256

typedef struct Glyph {
    DWORD *sum;           /* (GLYPH_CANVAS + 1)^2 */
    double cx, cy, w, h;  /* ink box, canvas pixels */
} Glyph;

static BOOL GlyphLoad(WCHAR letter, Glyph *g)
{
    const int n = GLYPH_CANVAS, stride = GLYPH_CANVAS + 1;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP bmp, oldBmp;
    HFONT font, oldFont;
    TEXTMETRICW tm;
    SIZE ext;
    int x, y, x0 = n, y0 = n, x1 = -1, y1 = -1;

    ZeroMemory(g, sizeof *g);
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = n;
    bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bmp || !bits) {
        if (dc) DeleteDC(dc);
        return FALSE;
    }
    oldBmp = (HBITMAP)SelectObject(dc, bmp);
    ZeroMemory(bits, (size_t)n * n * 4);
    font = CreateFontW(-GLYPH_EM, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    GetTextMetricsW(dc, &tm);
    GetTextExtentPoint32W(dc, &letter, 1, &ext);
    TextOutW(dc, (n - ext.cx) / 2, (n - tm.tmHeight) / 2, &letter, 1);
    GdiFlush();

    g->sum = (DWORD *)Alloc((size_t)stride * stride * sizeof(DWORD));
    if (g->sum) {
        const DWORD *px = (const DWORD *)bits;
        for (y = 0; y < n; y++) {
            DWORD row = 0;
            for (x = 0; x < n; x++) {
                DWORD c = (px[y * n + x] >> 8) & 0xFF;
                if (c) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
                row += c;
                g->sum[(y + 1) * stride + x + 1] = g->sum[y * stride + x + 1] + row;
            }
        }
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (!g->sum || x1 < x0) {
        Free(g->sum);
        g->sum = NULL;
        return FALSE;
    }
    g->cx = (x0 + x1 + 1) / 2.0;
    g->cy = (y0 + y1 + 1) / 2.0;
    g->w = x1 - x0 + 1.0;
    g->h = y1 - y0 + 1.0;
    return TRUE;
}

/* Coverage summed over [0,x) x [0,y), at any real point: the table is
 * bilinear inside each canvas pixel, so this is exact. */
static double GlyphSum(const Glyph *g, double x, double y)
{
    const int n = GLYPH_CANVAS, stride = GLYPH_CANVAS + 1;
    int ix, iy;
    double fx, fy, a, b, c, d;
    if (x <= 0.0 || y <= 0.0) return 0.0;
    if (x > n) x = n;
    if (y > n) y = n;
    ix = (int)x;
    iy = (int)y;
    if (ix >= n) ix = n - 1;
    if (iy >= n) iy = n - 1;
    fx = x - ix;
    fy = y - iy;
    a = g->sum[iy * stride + ix];
    b = g->sum[iy * stride + ix + 1];
    c = g->sum[(iy + 1) * stride + ix];
    d = g->sum[(iy + 1) * stride + ix + 1];
    return a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy;
}

/* The white initial, its ink centred on (cx, cy) and fitted inside a disc of
 * radius r. */
static void DrawLetter(DWORD *px, int size, double cx, double cy, double r, WCHAR letter)
{
    Glyph g;
    double scale, inv, half;
    int x, y, xa, xb, ya, yb;

    if (!GlyphLoad(letter, &g)) return;
    scale = (r * 2.0 * 0.56) / g.h;                          /* output pixels per canvas pixel */
    if (g.w * scale > r * 2.0 * 0.72) scale = (r * 2.0 * 0.72) / g.w;
    inv = 1.0 / scale;
    half = (g.w > g.h ? g.w : g.h) * scale / 2.0 + 1.0;
    xa = (int)floor(cx - half);
    xb = (int)ceil(cx + half);
    ya = (int)floor(cy - half);
    yb = (int)ceil(cy + half);
    for (y = ya < 0 ? 0 : ya; y < yb && y < size; y++) {
        for (x = xa < 0 ? 0 : xa; x < xb && x < size; x++) {
            double gx0 = g.cx + (x - cx) * inv, gx1 = g.cx + (x + 1 - cx) * inv;
            double gy0 = g.cy + (y - cy) * inv, gy1 = g.cy + (y + 1 - cy) * inv;
            double cover = GlyphSum(&g, gx1, gy1) - GlyphSum(&g, gx0, gy1) - GlyphSum(&g, gx1, gy0) + GlyphSum(&g, gx0, gy0);
            cover /= 255.0 * (gx1 - gx0) * (gy1 - gy0);
            if (cover > 0.002) Over(&px[y * size + x], RGB(255, 255, 255), cover);
        }
    }
    Free(g.sum);
}

static WCHAR Initial(const WCHAR *name)
{
    WORD type;
    for (; name && *name; name++) {
        WCHAR c = *name;
        if (GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & (C1_ALPHA | C1_DIGIT))) {
            WCHAR up = c;
            LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, &c, 1, &up, 1, NULL, NULL, 0);
            return up;
        }
    }
    return 0;
}

/* A disc of radius r in `color` inside a white ring of width `ring`. */
static void DrawDisc(DWORD *px, int size, double cx, double cy, double r, double ring, COLORREF color)
{
    double inner = r - ring;
    int x, y, x0 = (int)floor(cx - r) - 1, y0 = (int)floor(cy - r) - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    for (y = y0; y < size && y <= cy + r + 1; y++) {
        for (x = x0; x < size && x <= cx + r + 1; x++) {
            double dist = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
            Over(&px[y * size + x], RGB(255, 255, 255), Clamp01(r + 0.5 - dist));
            Over(&px[y * size + x], color, Clamp01(inner + 0.5 - dist));
        }
    }
}

static void DrawBadge(DWORD *px, int size, COLORREF color, WCHAR letter)
{
    double d = size >= 32 ? size * 0.54 : size * 0.62;
    double r = d / 2.0, cx = size - r, cy = size - r;
    double ring = size >= 32 ? size / 22.0 : 1.0;

    DrawDisc(px, size, cx, cy, r, ring, color);
    if (letter && size >= 24) DrawLetter(px, size, cx, cy, r - ring, letter);
}

static BOOL Render(const ClaudePackage *pkg, const Profile *profile, int size, DWORD *px)
{
    HICON icon = NULL;
    UINT id = 0;
    BOOL ok = FALSE;
    int color = (profile->color >= 0 && profile->color < PALETTE_SIZE) ? profile->color : 0;

    ZeroMemory(px, (size_t)size * size * 4);
    if (pkg && pkg->found && PrivateExtractIconsW(pkg->exe, 0, size, size, &icon, &id, 1, 0) == 1 && icon) {
        ok = IconToPixels(icon, size, px);
        DestroyIcon(icon);
    }
    if (!ok && pkg && pkg->found) {
        /* The exe moved (Claude updated since the package was looked up). */
        ClaudePackage fresh;
        if (Claude_FindPackage(&fresh) &&
            PrivateExtractIconsW(fresh.exe, 0, size, size, &icon, &id, 1, 0) == 1 && icon) {
            ok = IconToPixels(icon, size, px);
            DestroyIcon(icon);
        }
    }
    if (!ok) {
        icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
        if (icon) {
            ok = IconToPixels(icon, size, px);
            DestroyIcon(icon);
        }
    }
    if (!ok) return FALSE;
    DrawBadge(px, size, kPalette[color], Initial(profile->name));
    return TRUE;
}

/* ------------------------------------------------------------ .ico file */

static void Put16(BYTE **p, WORD v) { (*p)[0] = (BYTE)v; (*p)[1] = (BYTE)(v >> 8); *p += 2; }
static void Put32(BYTE **p, DWORD v) { Put16(p, (WORD)v); Put16(p, (WORD)(v >> 16)); }

static BOOL WriteIco(const WCHAR *path, const ClaudePackage *pkg, const Profile *profile)
{
    const int count = ARRAYSIZE(kSizes);
    size_t total = 6 + 16 * (size_t)count, offset;
    BYTE *file, *dir, *img;
    DWORD *px;
    WCHAR tmp[MAX_PATH];
    HANDLE h;
    DWORD written = 0;
    BOOL ok = FALSE;
    int i, x, y;

    for (i = 0; i < count; i++) {
        int s = kSizes[i], maskRow = ((s + 31) / 32) * 4;
        total += 40 + (size_t)s * s * 4 + (size_t)maskRow * s;
    }
    file = (BYTE *)Alloc(total);
    px = (DWORD *)Alloc((size_t)256 * 256 * 4);
    if (!file || !px) goto done;

    dir = file;
    Put16(&dir, 0);
    Put16(&dir, 1);
    Put16(&dir, (WORD)count);
    offset = 6 + 16 * (size_t)count;
    for (i = 0; i < count; i++) {
        int s = kSizes[i], maskRow = ((s + 31) / 32) * 4;
        DWORD bytes = (DWORD)(40 + (size_t)s * s * 4 + (size_t)maskRow * s);
        if (!Render(pkg, profile, s, px)) goto done;
        *dir++ = (BYTE)(s >= 256 ? 0 : s);
        *dir++ = (BYTE)(s >= 256 ? 0 : s);
        *dir++ = 0;
        *dir++ = 0;
        Put16(&dir, 1);
        Put16(&dir, 32);
        Put32(&dir, bytes);
        Put32(&dir, (DWORD)offset);

        img = file + offset;
        Put32(&img, 40);
        Put32(&img, (DWORD)s);
        Put32(&img, (DWORD)(s * 2));
        Put16(&img, 1);
        Put16(&img, 32);
        Put32(&img, BI_RGB);
        Put32(&img, bytes - 40);
        Put32(&img, 0); Put32(&img, 0); Put32(&img, 0); Put32(&img, 0);
        for (y = s - 1; y >= 0; y--)                    /* bottom-up rows */
            for (x = 0; x < s; x++) Put32(&img, px[y * s + x]);
        for (y = s - 1; y >= 0; y--) {                  /* AND mask: 1 = transparent */
            BYTE *row = img;
            for (x = 0; x < s; x++)
                if ((px[y * s + x] >> 24) == 0) row[x / 8] |= (BYTE)(0x80 >> (x % 8));
            img += maskRow;
        }
        offset += bytes;
    }

    if (FAILED(StringCchPrintfW(tmp, ARRAYSIZE(tmp), L"%s.tmp", path))) goto done;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) goto done;
    ok = WriteFile(h, file, (DWORD)total, &written, NULL) && written == total;
    CloseHandle(h);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
done:
    Free(file);
    Free(px);
    return ok;
}

/* ------------------------------------------------------------------ API */

static BOOL IconsDir(WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\icons", state));
}

/* The file name encodes what the icon shows (name, color, whether the Claude
 * art was available, and the drawing's revision), so a change produces a new
 * file and Explorer's icon cache cannot serve a stale image. */
#define ICON_REVISION 2
static BOOL PathFor(const Profile *profile, BOOL claudeArt, WCHAR *out, size_t cch)
{
    WCHAR dir[MAX_PATH], style[LABEL_CCH + 16];
    if (!IconsDir(dir, ARRAYSIZE(dir))) return FALSE;
    StringCchPrintfW(style, ARRAYSIZE(style), L"%s|%d|%c|%d", profile->name, profile->color, claudeArt ? L'c' : L'x', ICON_REVISION);
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%08lX-%08lX.ico", dir,
                                      (unsigned long)Core_Hash(profile->folder), (unsigned long)Core_Hash(style)));
}

BOOL Icons_Ensure(const ClaudePackage *pkg, const Profile *profile, WCHAR *out, size_t cch)
{
    WCHAR dir[MAX_PATH];
    if (!PathFor(profile, pkg && pkg->found, out, cch)) return FALSE;
    if (Util_FileExists(out)) return TRUE;
    if (!IconsDir(dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir)) return FALSE;
    return WriteIco(out, pkg, profile);
}

static HICON PixelsToIcon(const DWORD *px, int size)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP color = NULL, mask = NULL;
    BYTE *maskBits = NULL;
    ICONINFO ii;
    HICON icon = NULL;
    HDC dc;

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) goto done;
    memcpy(bits, px, (size_t)size * size * 4);
    maskBits = (BYTE *)Alloc((size_t)((size + 15) / 16) * 2 * size);
    mask = maskBits ? CreateBitmap(size, size, 1, 1, maskBits) : NULL;
    if (!mask) goto done;
    ZeroMemory(&ii, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
done:
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    Free(maskBits);
    return icon;
}

HICON Icons_Create(const ClaudePackage *pkg, const Profile *profile, int size)
{
    DWORD *px;
    HICON icon = NULL;
    if (size < 8 || size > 256) return NULL;
    px = (DWORD *)Alloc((size_t)size * size * 4);
    if (px && Render(pkg, profile, size, px)) icon = PixelsToIcon(px, size);
    Free(px);
    return icon;
}

/* The badge alone, filling a size x size square, without the initial. */
HICON Icons_CreateBadge(int color, int size)
{
    DWORD *px;
    HICON icon = NULL;
    if (size < 4 || size > 256) return NULL;
    if (color < 0 || color >= PALETTE_SIZE) color = 0;
    px = (DWORD *)Alloc((size_t)size * size * 4);
    if (!px) return NULL;
    DrawDisc(px, size, size / 2.0, size / 2.0, size / 2.0, size >= 22 ? size / 11.0 : 1.0, kPalette[color]);
    icon = PixelsToIcon(px, size);
    Free(px);
    return icon;
}

/* Claude's notification-area glyph (its own file, read from the installed
 * package) in the profile's color. On a dark taskbar the color is lightened:
 * the darker colors of the palette fade into it at that size. Falls back to
 * the badged icon when the file is missing. */
HICON Icons_CreateTray(const ClaudePackage *pkg, const Profile *profile, int size, BOOL darkTaskbar)
{
    WCHAR path[MAX_PATH];
    HICON glyph = NULL, icon = NULL;
    DWORD *px;
    COLORREF c;
    int i, color = (profile->color >= 0 && profile->color < PALETTE_SIZE) ? profile->color : 0;

    if (size < 8 || size > 256) return NULL;
    if (pkg && pkg->found &&
        SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\app\\resources\\Tray-Win32-Dark.ico", pkg->installDir)))
        glyph = (HICON)LoadImageW(NULL, path, IMAGE_ICON, size, size, LR_LOADFROMFILE);
    if (!glyph) return Icons_Create(pkg, profile, size);
    px = (DWORD *)Alloc((size_t)size * size * 4);
    if (px && IconToPixels(glyph, size, px)) {
        c = kPalette[color];
        if (darkTaskbar)
            c = RGB(GetRValue(c) + (255 - GetRValue(c)) * 35 / 100, GetGValue(c) + (255 - GetGValue(c)) * 35 / 100,
                    GetBValue(c) + (255 - GetBValue(c)) * 35 / 100);
        for (i = 0; i < size * size; i++)
            px[i] = (px[i] & 0xFF000000) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
        icon = PixelsToIcon(px, size);
    }
    Free(px);
    DestroyIcon(glyph);
    return icon;
}

/* TRUE when this profile has an icon file other than the current one (made
 * while Claude was missing, or by an older drawing): its shortcuts, pins and
 * open windows should get the current icon. The current file may exist already
 * (a watcher makes it for the windows it tags). */
BOOL Icons_IsStale(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR current[MAX_PATH], dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL stale = FALSE;
    if (!pkg || !pkg->found || !PathFor(profile, TRUE, current, ARRAYSIZE(current))) return FALSE;
    if (!IconsDir(dir, ARRAYSIZE(dir)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\%08lX-*.ico", dir, (unsigned long)Core_Hash(profile->folder))))
        return FALSE;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) continue;
        stale = !Core_PathEquals(path, current);
    } while (!stale && FindNextFileW(h, &fd));
    FindClose(h);
    return stale;
}

/* Remove this profile's icon files except `keep` (NULL: all of them) and
 * the ones a taskbar pin still shows. */
void Icons_DeleteStale(const Profile *profile, const WCHAR *keep)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!IconsDir(dir, ARRAYSIZE(dir))) return;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\%08lX-*.ico", dir,
                                (unsigned long)Core_Hash(profile->folder))))
        return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) continue;
        if (keep && Core_PathEquals(path, keep)) continue;
        if (TaskbarPin_UsesIcon(path)) continue;
        DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
