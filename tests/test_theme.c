/*
 * Checks theme.c against Windows itself, in the mode Windows is set to (dark
 * or light apps): the rows the program draws look like Explorer's list view
 * rows (the main, bright and pale blues), a dark list view's header dividers
 * fall on its rows' column lines and none closes a last column at its edge,
 * a one-line edit's text sits in the middle
 * of the box, a drop-down button has the buttons' face and the drop-down
 * lists' arrow, a dialog's drop-down list looks like one and never shows its
 * choice selected, the list it drops has the rows of every list, a dark
 * list's scroll bar reaches its edges, the wheel scrolls lists and trees
 * with their scroll bars, and by the pixel in a smooth view, a link reads
 * in its blue, and what is
 * drawn off screen shows up whole. Built and run by
 * build.cmd, linked with the program's objects. Checks that need a visual
 * style Windows lacks are skipped; a Windows update that changes what they
 * rely on makes the build fail with the check's name.
 */
#include "../src/app.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int g_failures = 0, g_checks = 0;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) g_failures++;
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
    HDC     dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    DWORD  *px;
    int     w, h;
} Canvas;

static BOOL CanvasOpen(Canvas *c, int w, int h, COLORREF fill)
{
    BITMAPINFO bi;
    RECT all = { 0, 0, w, h };
    void *bits = NULL;
    ZeroMemory(c, sizeof *c);
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    if ((c->dc = CreateCompatibleDC(NULL)) == NULL) return FALSE;
    if ((c->bitmap = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0)) == NULL) {
        DeleteDC(c->dc);
        return FALSE;
    }
    c->old = SelectObject(c->dc, c->bitmap);
    c->px = (DWORD *)bits;
    c->w = w;
    c->h = h;
    SetDCBrushColor(c->dc, fill);
    FillRect(c->dc, &all, (HBRUSH)GetStockObject(DC_BRUSH));
    GdiFlush();
    return TRUE;
}

static COLORREF At(const Canvas *c, int x, int y)
{
    DWORD p;
    GdiFlush();
    p = c->px[y * c->w + x];
    return RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
}

static void CanvasClose(Canvas *c)
{
    if (!c->dc) return;
    SelectObject(c->dc, c->old);
    DeleteObject(c->bitmap);
    DeleteDC(c->dc);
    ZeroMemory(c, sizeof *c);
}

/* ------------------------------------------------------------------ rows */

static BOOL HighContrastOn(void)
{
    HIGHCONTRASTW hc;
    ZeroMemory(&hc, sizeof hc);
    hc.cbSize = sizeof hc;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

/* Theme_DrawRow against the list view item of Windows' theme, drawn over
 * the same field: fill, frame and corners. */
static void TestRows(void)
{
    static const struct { int state; UINT flags; const char *name; } kStates[] = {
        { LISS_SELECTED, THEME_ROW_SELECTED, "a selected row" },
        { LISS_HOT, THEME_ROW_HOT, "a row under the mouse" },
    };
    const WCHAR *cls = Theme_IsDark() ? L"DarkMode_Explorer::ListView" : L"Explorer::ListView";
    HTHEME theme = OpenThemeData(NULL, cls);
    COLORREF field = Theme_Color(THEME_FIELD);
    RECT rc = { 0, 0, 60, 24 };
    char name[128];
    size_t i;
    if (!theme || HighContrastOn()) {
        printf("  skip  rows: no %ls visual style\n", cls);
        if (theme) CloseThemeData(theme);
        return;
    }
    for (i = 0; i < ARRAYSIZE(kStates); i++) {
        static const struct { int x, y; const char *where; } kPoints[] = {
            { 30, 12, "fill" }, { 30, 0, "top edge" }, { 0, 12, "left edge" }, { 59, 23, "corner" }, { 0, 0, "other corner" },
        };
        Canvas native, ours;
        size_t k;
        if (!CanvasOpen(&native, 60, 24, field) || !CanvasOpen(&ours, 60, 24, field)) {
            CanvasClose(&native);
            Check("rows: canvas", FALSE);
            continue;
        }
        DrawThemeBackground(theme, native.dc, LVP_LISTITEM, kStates[i].state, &rc, NULL);
        Theme_DrawRow(ours.dc, &rc, kStates[i].flags, field);
        for (k = 0; k < ARRAYSIZE(kPoints); k++) {
            StringCchPrintfA(name, ARRAYSIZE(name), "%s is drawn as the list views draw it: %s", kStates[i].name, kPoints[k].where);
            CheckColor(name, At(&native, kPoints[k].x, kPoints[k].y), At(&ours, kPoints[k].x, kPoints[k].y));
        }
        CanvasClose(&native);
        CanvasClose(&ours);
    }
    {
        Canvas plain;
        if (CanvasOpen(&plain, 10, 10, RGB(1, 2, 3))) {
            RECT r = { 0, 0, 10, 10 };
            Theme_DrawRow(plain.dc, &r, 0, field);
            CheckColor("a row neither selected nor under the mouse is the list's background", field, At(&plain, 5, 5));
            CanvasClose(&plain);
        }
    }
    CloseThemeData(theme);
}

/* --------------------------------------------------------------- windows */

static HWND g_host;

/* The controls' window: on screen (Windows only composes what is), but
 * transparent to the eye and the mouse, and never activated. */
static HWND Host(void)
{
    if (!g_host) {
        g_host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, WC_STATICW, L"",
                                 WS_POPUP | WS_CLIPCHILDREN, 0, 0, 500, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (g_host) SetLayeredWindowAttributes(g_host, 0, 1, LWA_ALPHA);
    }
    return g_host;
}

static HFONT DialogFont(void)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof lf);
    lf.lfHeight = -12;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

static BOOL Stands(COLORREF c, COLORREF back)
{
    return abs((int)GetRValue(c) - (int)GetRValue(back)) > 16 || abs((int)GetGValue(c) - (int)GetGValue(back)) > 16 ||
           abs((int)GetBValue(c) - (int)GetBValue(back)) > 16;
}

/* A dark list view's header dividers sit on the column lines its rows draw
 * (Windows draws them a pixel apart; theme.c draws the header). */
static void TestHeader(void)
{
    static const int kWidths[] = { 100, 120 };
    HWND host = Host(), list;
    LVCOLUMNW col;
    LVITEMW item;
    Canvas canvas;
    RECT header;
    COLORREF headerBack, field = Theme_Color(THEME_FIELD);
    int i, x, yHeader, yRow, found = 0, matched = 0;
    if (!Theme_IsDark()) {
        printf("  skip  header: Windows is set to light apps\n");
        return;
    }
    list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER, 0, 0, 300, 150,
                           host, NULL, GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("header: list view created", FALSE);
        return;
    }
    SendMessageW(list, WM_SETFONT, (WPARAM)DialogFont(), FALSE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ZeroMemory(&col, sizeof col);
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.pszText = (LPWSTR)L"";
    for (i = 0; i < (int)ARRAYSIZE(kWidths); i++) {
        col.cx = kWidths[i];
        ListView_InsertColumn(list, i, &col);
    }
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"";
    for (i = 0; i < 3; i++) {
        item.iItem = i;
        ListView_InsertItem(list, &item);
    }
    Theme_Apply(host);
    /* Captured as the screen has it (WM_PRINT draws list views otherwise). */
    ShowWindow(host, SW_SHOWNOACTIVATE);
    UpdateWindow(host);
    DwmFlush();
    GetWindowRect(ListView_GetHeader(list), &header);
    MapWindowPoints(NULL, host, (POINT *)&header, 2);
    if (!CanvasOpen(&canvas, 500, 300, RGB(0, 0, 0))) {
        Check("header: canvas", FALSE);
        DestroyWindow(list);
        return;
    }
    PrintWindow(host, canvas.dc, PW_RENDERFULLCONTENT);
    yHeader = header.top + (header.bottom - header.top) / 2;
    yRow = header.bottom + 30;
    headerBack = At(&canvas, header.left + 3, yHeader);
    if (headerBack == RGB(0, 0, 0) && At(&canvas, header.left + 3, yRow) == RGB(0, 0, 0)) {
        printf("  skip  header: Windows gave no image of the window\n");
        CanvasClose(&canvas);
        ShowWindow(host, SW_HIDE);
        DestroyWindow(list);
        return;
    }
    for (x = header.left + 2; x < header.right - 2; x++) {
        /* A line is what stands out (a faint shade next to it does not). */
        BOOL divider = Stands(At(&canvas, x, yHeader), headerBack), line = Stands(At(&canvas, x, yRow), field);
        if (line) found++;
        if (line && divider) matched++;
        else if (divider != line) {
            printf("        x=%d: header %s, rows %s\n", x, divider ? "divider" : "none", line ? "line" : "none");
        }
    }
    if (!found) {
        printf("  skip  header: the rows draw no column lines here\n");
    } else {
        Check("a dark header's dividers fall on the rows' column lines", matched == found);
    }
    CanvasClose(&canvas);
    ShowWindow(host, SW_HIDE);
    DestroyWindow(list);
}

/* A dark list view whose last column reaches its right edge: no line closes
 * it there, in the header or the rows, and a selected row's frame is its
 * last pixel, with its fill up to it. */
static void TestListEdge(void)
{
    HWND host = Host(), list;
    LVCOLUMNW col;
    LVITEMW item;
    RECT client, row, other, header, win;
    Canvas canvas;
    int i, x;
    if (!Theme_IsDark()) {
        printf("  skip  list edge: Windows is set to light apps\n");
        return;
    }
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 10, 10, 260, 160, host, NULL,
                           GetModuleHandleW(NULL), NULL);
    if (!list) {
        Check("list edge: list view created", FALSE);
        return;
    }
    SendMessageW(list, WM_SETFONT, (WPARAM)DialogFont(), FALSE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    GetClientRect(list, &client);
    ZeroMemory(&col, sizeof col);
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.pszText = (LPWSTR)L"";
    col.cx = 100;
    ListView_InsertColumn(list, 0, &col);
    col.cx = client.right - 100;   /* the last one to the edge */
    ListView_InsertColumn(list, 1, &col);
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"";
    for (i = 0; i < 3; i++) {
        item.iItem = i;
        ListView_InsertItem(list, &item);
    }
    ListView_SetItemState(list, 0, LVIS_SELECTED, LVIS_SELECTED);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    UpdateWindow(host);
    DwmFlush();
    if (!CanvasOpen(&canvas, 500, 300, RGB(0, 0, 0))) {
        Check("list edge: canvas", FALSE);
    } else {
        PrintWindow(host, canvas.dc, PW_RENDERFULLCONTENT);
        GetClientRect(list, &client);
        GetWindowRect(ListView_GetHeader(list), &header);
        MapWindowPoints(NULL, list, (POINT *)&header, 2);
        ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS);
        ListView_GetItemRect(list, 1, &other, LVIR_BOUNDS);
        GetWindowRect(list, &win);
        MapWindowPoints(NULL, host, (POINT *)&win, 2);
        x = win.left + client.right;   /* the list's right edge, in the canvas */
        if (At(&canvas, x - 2, win.top + (row.top + row.bottom) / 2) == RGB(0, 0, 0)) {
            printf("  skip  list edge: Windows gave no image of the window\n");
        } else {
            CheckColor("a selected row's fill reaches its frame at the edge", Theme_Color(THEME_MAIN_BLUE),
                       At(&canvas, x - 2, win.top + (row.top + row.bottom) / 2));
            CheckColor("... its frame on the edge pixel", Theme_Color(THEME_BRIGHT_BLUE), At(&canvas, x - 1, win.top + (row.top + row.bottom) / 2));
            CheckColor("... no line closing the last column in the other rows", Theme_Color(THEME_FIELD),
                       At(&canvas, x - 2, win.top + (other.top + other.bottom) / 2));
            Check("... nor in the header", !Stands(At(&canvas, x - 2, win.top + (header.top + header.bottom) / 2),
                                                   At(&canvas, x - 20, win.top + (header.top + header.bottom) / 2)));
        }
        CanvasClose(&canvas);
    }
    ShowWindow(host, SW_HIDE);
    DestroyWindow(list);
}

/* A one-line edit's text (and its selection) sits in the middle of the box,
 * at the font it has and after a new one. */
static void TestEdit(void)
{
    static const int kHeights[] = { 24, 40 };
    HWND host = Host(), edit;
    HFONT font = DialogFont(), big;
    LOGFONTW lf;
    size_t i;
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"Personal", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 200, kHeights[0], host,
                           NULL, GetModuleHandleW(NULL), NULL);
    if (!edit) {
        Check("edit: created", FALSE);
        return;
    }
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, FALSE);
    Theme_Apply(host);
    GetObjectW(font, sizeof lf, &lf);
    lf.lfHeight = -20;
    big = CreateFontIndirectW(&lf);
    for (i = 0; i < ARRAYSIZE(kHeights); i++) {
        RECT win, client;
        TEXTMETRICW tm;
        HDC dc = GetDC(edit);
        HGDIOBJ old;
        int above, below;
        if (i > 0) {
            SetWindowPos(edit, NULL, 0, 0, 200, kHeights[i], SWP_NOMOVE | SWP_NOZORDER);
            SendMessageW(edit, WM_SETFONT, (WPARAM)big, FALSE);
        }
        old = SelectObject(dc, (HFONT)SendMessageW(edit, WM_GETFONT, 0, 0));
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(edit, dc);
        GetWindowRect(edit, &win);
        GetClientRect(edit, &client);
        MapWindowPoints(edit, NULL, (POINT *)&client, 2);
        above = client.top - win.top;
        below = win.bottom - (client.top + tm.tmHeight);
        Check(i == 0 ? "an edit's text is centered in its box" : "... and again after a new font", above - below >= -1 && above - below <= 1);
        if (above - below < -1 || above - below > 1) printf("        above %d, below %d\n", above, below);
    }
    DestroyWindow(edit);
    DeleteObject(font);
    DeleteObject(big);
}

/* ---------------------------------------------------------- drop-downs */

/* The box around what stands out from `back` in x0..x1, y0..y1 of `c`;
 * FALSE when nothing does. */
static BOOL InkBox(const Canvas *c, int x0, int y0, int x1, int y1, COLORREF back, RECT *box)
{
    int x, y;
    SetRect(box, x1, y1, x0, y0);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (Stands(At(c, x, y), back)) {
                box->left = min(box->left, x);
                box->top = min(box->top, y);
                box->right = max(box->right, x + 1);
                box->bottom = max(box->bottom, y + 1);
            }
    return box->left < box->right;
}

/* A drop-down button (Theme_DrawDropDown, the sessions view's Actions) has
 * the face of the buttons next to it in every state, and the arrow Windows
 * draws on a drop-down list, in the middle of its height. */
static void TestDropDownButton(void)
{
    static const struct { UINT state; const char *name; } kStates[] = {
        { 0, "at rest" }, { THEME_BUTTON_HOT, "under the mouse" }, { THEME_BUTTON_PRESSED, "pressed" },
    };
    static const struct { int x, y; const char *where; } kPoints[] = {
        { 12, 12, "fill" }, { 60, 0, "top edge" }, { 0, 12, "left edge" }, { 0, 0, "corner" },
    };
    const WCHAR *cls = Theme_IsDark() ? L"DarkMode_CFD::Combobox" : L"Combobox";
    HWND host = Host();
    HTHEME theme;
    HFONT font = DialogFont();
    COLORREF face = Theme_Color(THEME_FACE);
    RECT rc = { 0, 0, 120, 24 }, ours = { 0 }, theirs = { 0 };
    Canvas button, box, glyph;
    char name[128];
    size_t i, k;
    for (i = 0; i < ARRAYSIZE(kStates); i++) {
        if (!CanvasOpen(&button, 120, 24, face) || !CanvasOpen(&box, 120, 24, face)) {
            CanvasClose(&button);
            Check("drop-down: canvas", FALSE);
            continue;
        }
        Theme_DrawButton(host, button.dc, &rc, L"", font, kStates[i].state, DT_SINGLELINE);
        Theme_DrawDropDown(host, box.dc, &rc, L"", font, kStates[i].state);
        for (k = 0; k < ARRAYSIZE(kPoints); k++) {
            StringCchPrintfA(name, ARRAYSIZE(name), "a drop-down button %s is a button: %s", kStates[i].name, kPoints[k].where);
            CheckColor(name, At(&button, kPoints[k].x, kPoints[k].y), At(&box, kPoints[k].x, kPoints[k].y));
        }
        CanvasClose(&button);
        CanvasClose(&box);
    }

    theme = OpenThemeDataForDpi(NULL, cls, GetDpiForWindow(host));
    if (!theme || HighContrastOn()) {
        printf("  skip  drop-down arrow: no %ls visual style\n", cls);
        if (theme) CloseThemeData(theme);
        DeleteObject(font);
        return;
    }
    if (CanvasOpen(&box, 120, 24, face) && CanvasOpen(&glyph, 40, 24, RGB(0, 0, 0))) {
        int arrow = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(host));
        RECT cell = { 0, 0, arrow, 24 };
        COLORREF fill;
        BOOL found, same, middle;
        Theme_DrawDropDown(host, box.dc, &rc, L"", font, 0);
        fill = At(&box, 60, 12);
        SetDCBrushColor(glyph.dc, fill);
        FillRect(glyph.dc, &cell, (HBRUSH)GetStockObject(DC_BRUSH));
        DrawThemeBackground(theme, glyph.dc, CP_DROPDOWNBUTTONRIGHT, CBXSR_NORMAL, &cell, NULL);
        found = InkBox(&box, 120 - arrow - 4, 3, 120 - 3, 21, fill, &ours) && InkBox(&glyph, 0, 0, arrow, 24, fill, &theirs);
        same = found && ours.right - ours.left == theirs.right - theirs.left && ours.bottom - ours.top == theirs.bottom - theirs.top;
        middle = found && abs((ours.top + ours.bottom) - 24) <= 2;
        Check("a drop-down button shows the arrow Windows draws on a drop-down list", same);
        Check("... in the middle of its height", middle);
        if (found && (!same || !middle))
            printf("        arrow %ldx%ld at y %ld (Windows': %ldx%ld at y %ld)\n", ours.right - ours.left, ours.bottom - ours.top, ours.top,
                   theirs.right - theirs.left, theirs.bottom - theirs.top, theirs.top);
        CanvasClose(&glyph);
    } else {
        Check("drop-down arrow: canvas", FALSE);
    }
    CanvasClose(&box);
    CloseThemeData(theme);
    DeleteObject(font);
}

/* A dialog's drop-down list looks like a drop-down button showing its
 * choice, and never shows its choice in the selection color, with the focus
 * or without. */
static void TestDropDownList(void)
{
    HWND host = Host(), combo;
    HFONT font = DialogFont();
    COLORREF face = Theme_Color(THEME_FACE), highlight = GetSysColor(COLOR_HIGHLIGHT);
    RECT win, rc;
    Canvas screen, box;
    int pass;
    combo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 10, 120, 200, host, NULL,
                            GetModuleHandleW(NULL), NULL);
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
    for (pass = 0; pass < 2; pass++) {
        int w, h, x, y, differ = 0, selected = 0;
        if (pass == 1) {
            SetFocus(combo);
            if (GetFocus() != combo) {
                printf("  skip  drop-down list: it could not take the focus here\n");
                break;
            }
        }
        RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        DwmFlush();
        GetWindowRect(combo, &win);
        MapWindowPoints(NULL, host, (POINT *)&win, 2);
        w = win.right - win.left;
        h = win.bottom - win.top;
        if (!CanvasOpen(&screen, 500, 300, RGB(0, 0, 0)) || !CanvasOpen(&box, w, h, face)) {
            CanvasClose(&screen);
            Check("drop-down list: canvas", FALSE);
            break;
        }
        PrintWindow(host, screen.dc, PW_RENDERFULLCONTENT);
        if (At(&screen, win.left + w / 2, win.top + h / 2) == RGB(0, 0, 0) && At(&screen, win.left + 1, win.top + 1) == RGB(0, 0, 0)) {
            printf("  skip  drop-down list: Windows gave no image of the window\n");
            CanvasClose(&screen);
            CanvasClose(&box);
            break;
        }
        SetRect(&rc, 0, 0, w, h);
        Theme_DrawDropDown(host, box.dc, &rc, L"Blue", font, 0);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                COLORREF c = At(&screen, win.left + x, win.top + y);
                if (c == highlight) selected++;
                if (pass == 0 && c != At(&box, x, y)) differ++;
            }
        if (pass == 0) {
            Check("a dialog's drop-down list is drawn as a drop-down button showing its choice", differ == 0);
            if (differ) printf("        %d of %d pixels differ\n", differ, w * h);
        }
        Check(pass == 0 ? "... its choice is not shown selected" : "... not even with the focus", selected == 0);
        CanvasClose(&screen);
        CanvasClose(&box);
    }
    {
        /* The list it drops: its rows as every list's, the choice in the main blue. */
        COMBOBOXINFO info;
        RECT client, row, next;
        Canvas list;
        ZeroMemory(&info, sizeof info);
        info.cbSize = sizeof info;
        if (!GetComboBoxInfo(combo, &info) || !info.hwndList) {
            Check("drop-down list: its list found", FALSE);
        } else {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Green");
            GetClientRect(info.hwndList, &client);
            SendMessageW(info.hwndList, LB_GETITEMRECT, 0, (LPARAM)&row);
            SendMessageW(info.hwndList, LB_GETITEMRECT, 1, (LPARAM)&next);
            if (CanvasOpen(&list, max(1, (int)client.right), max(1, (int)next.bottom), RGB(0, 0, 0))) {
                SendMessageW(info.hwndList, WM_PRINTCLIENT, (WPARAM)list.dc, PRF_CLIENT);
                CheckColor("the list it drops shows its choice as every list's selected row", Theme_Color(THEME_MAIN_BLUE),
                           At(&list, (row.left + row.right) / 2 - 20, (row.top + row.bottom) / 2 - 3));
                CheckColor("... and its other rows on the list's field",
                           Theme_IsDark() ? Theme_Color(THEME_FIELD) : GetSysColor(COLOR_WINDOW),
                           At(&list, (next.left + next.right) / 2 - 20, (next.top + next.bottom) / 2 - 3));
                CanvasClose(&list);
            } else {
                Check("drop-down list: canvas", FALSE);
            }
            /* Its list open, the list's selection following the mouse: the box
             * keeps the choice it had. */
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
            if (!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
                printf("  skip  drop-down list: its list did not open here\n");
            } else {
                RECT area;
                Canvas shown, chosen;
                int x, y, differ = 0;
                SendMessageW(info.hwndList, LB_SETCURSEL, 1, 0);   /* the mouse over "Green" */
                GetClientRect(combo, &area);
                if (CanvasOpen(&shown, area.right, area.bottom, RGB(0, 0, 0)) && CanvasOpen(&chosen, area.right, area.bottom, RGB(0, 0, 0))) {
                    SendMessageW(combo, WM_PRINTCLIENT, (WPARAM)shown.dc, PRF_CLIENT);
                    Theme_DrawDropDown(combo, chosen.dc, &area, L"Blue", font, THEME_BUTTON_PRESSED);
                    for (y = 0; y < area.bottom; y++)
                        for (x = 0; x < area.right; x++)
                            if (At(&shown, x, y) != At(&chosen, x, y)) differ++;
                    Check("a drop-down list keeps its choice while its list follows the mouse", differ == 0);
                } else {
                    Check("drop-down list: canvas", FALSE);
                }
                CanvasClose(&shown);
                CanvasClose(&chosen);
                SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
            }
        }
    }
    ShowWindow(host, SW_HIDE);
    DestroyWindow(combo);
    DeleteObject(font);
}

/* ---------------------------------------------------------- scroll bars */

static HWND FilledTree(HWND host, int x, BOOL framed)
{
    HWND tree = CreateWindowExW(framed ? WS_EX_CLIENTEDGE : 0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_NOHSCROLL, x, 10, 200, 120,
                                host, NULL, GetModuleHandleW(NULL), NULL);
    TVINSERTSTRUCTW ins;
    int i;
    ZeroMemory(&ins, sizeof ins);
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT;
    ins.item.pszText = (LPWSTR)L"Row";
    for (i = 0; tree && i < 40; i++) TreeView_InsertItem(tree, &ins);
    return tree;
}

/* A dark tree (as the lists and list boxes) has no frame, so its scroll bar
 * reaches the edges of its window: made with a frame, it shows it as a tree
 * made without one does, to the pixel. A light one keeps the frame it was
 * made with. */
static void TestScrollBarEdges(void)
{
    HWND host = Host(), framed = FilledTree(host, 10, TRUE), bare = FilledTree(host, 250, FALSE);
    SCROLLBARINFO bar;
    Canvas screen;
    RECT a, b, h;
    int x, y, w, ht, differ = 0;
    if (!framed || !bare) {
        Check("scroll bar: trees created", FALSE);
        if (framed) DestroyWindow(framed);
        if (bare) DestroyWindow(bare);
        return;
    }
    Theme_Apply(host);
    if (!Theme_IsDark()) {
        Check("a light tree keeps its frame", (GetWindowLongW(framed, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0);
        DestroyWindow(framed);
        DestroyWindow(bare);
        return;
    }
    ZeroMemory(&bar, sizeof bar);
    bar.cbSize = sizeof bar;
    GetWindowRect(framed, &a);
    if (!GetScrollBarInfo(framed, OBJID_VSCROLL, &bar) || (bar.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
        Check("scroll bar: the tree shows one", FALSE);
    } else {
        Check("a dark tree's scroll bar reaches the right, top and bottom edges",
              bar.rcScrollBar.right == a.right && bar.rcScrollBar.top == a.top && bar.rcScrollBar.bottom == a.bottom);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        RedrawWindow(host, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
        DwmFlush();
        GetWindowRect(bare, &b);
        GetWindowRect(host, &h);
        w = bar.rcScrollBar.right - bar.rcScrollBar.left;
        ht = bar.rcScrollBar.bottom - bar.rcScrollBar.top;
        if (CanvasOpen(&screen, 500, 300, RGB(0, 0, 0))) {
            PrintWindow(host, screen.dc, PW_RENDERFULLCONTENT);
            /* The ring around the scroll bar, where a frame would show. */
            for (y = 0; y < ht; y++)
                for (x = 0; x < w; x++) {
                    if (y > 1 && y < ht - 2 && x < w - 2) continue;
                    if (At(&screen, bar.rcScrollBar.left - h.left + x, bar.rcScrollBar.top - h.top + y) !=
                        At(&screen, b.right - w - h.left + x, b.top - h.top + y))
                        differ++;
                }
            Check("... and shows no frame around it, as a tree made without one", differ == 0);
            if (differ) printf("        %d pixels differ around the scroll bar\n", differ);
            CanvasClose(&screen);
        } else {
            Check("scroll bar: canvas", FALSE);
        }
        ShowWindow(host, SW_HIDE);
    }
    DestroyWindow(framed);
    DestroyWindow(bare);
}

/* ------------------------------------------------------------- the wheel */

/* Messages handled for `ms` (the smooth scroll's frames). */
static void Pump(int ms)
{
    ULONGLONG until = GetTickCount64() + (ULONGLONG)ms;
    MSG m;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
}

static void Wheel(HWND h, int notches)
{
    RECT rc;
    GetWindowRect(h, &rc);
    SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA * notches), MAKELPARAM((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2));
    Pump(400);
}

/* A wheel notch scrolls a list box and a tree (smoothly, frame by frame),
 * within a moment, their scroll bars with them, and the way back ends at the
 * top. */
static void TestWheel(void)
{
    HWND host = Host(), box, tree;
    HTREEITEM top;
    UINT lines = 3;
    int i;
    box = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 200, 120, host, NULL,
                          GetModuleHandleW(NULL), NULL);
    tree = FilledTree(host, 250, FALSE);
    if (!box || !tree) {
        Check("wheel: controls created", FALSE);
        if (box) DestroyWindow(box);
        if (tree) DestroyWindow(tree);
        return;
    }
    for (i = 0; i < 40; i++) SendMessageW(box, LB_ADDSTRING, 0, (LPARAM)L"Row");
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL || lines == 0) {
        printf("  skip  wheel: the wheel scrolls by pages here\n");
    } else {
        top = TreeView_GetFirstVisible(tree);
        Wheel(box, 1);
        Wheel(tree, 1);
        Check("a wheel notch scrolls a list box its lines", (UINT)SendMessageW(box, LB_GETTOPINDEX, 0, 0) == lines);
        Check("... its scroll bar with it", GetScrollPos(box, SB_VERT) == (int)SendMessageW(box, LB_GETTOPINDEX, 0, 0));
        Check("a wheel notch scrolls a tree", TreeView_GetFirstVisible(tree) != top && GetScrollPos(tree, SB_VERT) > 0);
        Wheel(box, -5);
        Wheel(tree, -5);
        Check("... and back up, to the top", SendMessageW(box, LB_GETTOPINDEX, 0, 0) == 0 && GetScrollPos(box, SB_VERT) == 0 &&
                                                TreeView_GetFirstVisible(tree) == top && GetScrollPos(tree, SB_VERT) == 0);
    }
    ShowWindow(host, SW_HIDE);
    DestroyWindow(box);
    DestroyWindow(tree);
}

/* A list box, a list view and a tree in smooth views scroll alike, by the
 * pixel: a wheel notch covers its lines exactly, in steps finer than a row;
 * the control moves in its view; a list view's header stays on top; the
 * keyboard's row is scrolled into sight. */
static void TestSmoothView(void)
{
    static const WCHAR *kNames[] = { L"a list box", L"a list view", L"a tree" };
    HWND host = Host(), controls[3], views[3];
    LVCOLUMNW col;
    LVITEMW item;
    UINT lines = 3;
    char name[128];
    int i, k;
    controls[0] = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 10, 140, 120, host, NULL,
                                  GetModuleHandleW(NULL), NULL);
    controls[1] = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 160, 10, 140, 120, host, NULL, GetModuleHandleW(NULL), NULL);
    controls[2] = FilledTree(host, 310, FALSE);
    if (!controls[0] || !controls[1] || !controls[2]) {
        Check("smooth view: controls created", FALSE);
        for (k = 0; k < 3; k++)
            if (controls[k]) DestroyWindow(controls[k]);
        return;
    }
    ZeroMemory(&col, sizeof col);
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.cx = 120;
    col.pszText = (LPWSTR)L"Name";
    ListView_InsertColumn(controls[1], 0, &col);
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (LPWSTR)L"Row";
    for (i = 0; i < 40; i++) {
        SendMessageW(controls[0], LB_ADDSTRING, 0, (LPARAM)L"Row");
        item.iItem = i;
        ListView_InsertItem(controls[1], &item);
    }
    for (k = 0; k < 3; k++) views[k] = Theme_SmoothView(controls[k]);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    Pump(50);   /* the views measure what they hold */
    for (k = 0; k < 3; k++) {
        /* Made while the window was hidden: all its rows still count (a
         * list view's header too), so no scroll bar of its own. */
        StringCchPrintfA(name, ARRAYSIZE(name), "%ls in a view is as tall as its rows, with no scroll bar of its own", kNames[k]);
        Check(name, !(GetWindowLongW(controls[k], GWL_STYLE) & WS_VSCROLL));
    }
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL || lines == 0) {
        printf("  skip  smooth view: the wheel scrolls by pages here\n");
    } else {
        for (k = 0; k < 3; k++) {
            RECT view, control, header;
            int row = k == 0 ? (int)SendMessageW(controls[0], LB_GETITEMHEIGHT, 0, 0)
                    : k == 1 ? (ListView_GetItemRect(controls[1], 0, &control, LVIR_BOUNDS) ? control.bottom - control.top : 0)
                             : TreeView_GetItemHeight(controls[2]);
            int pos, first;
            RECT rc;
            SendMessageW(controls[k], WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
            first = GetScrollPos(views[k], SB_VERT);   /* the first frame, at once */
            Pump(400);
            pos = GetScrollPos(views[k], SB_VERT);
            StringCchPrintfA(name, ARRAYSIZE(name), "a wheel notch scrolls %ls in a view its lines exactly", kNames[k]);
            Check(name, row > 0 && pos == (int)lines * row);
            if (row > 0 && pos != (int)lines * row) printf("        at %d px, rows of %d px\n", pos, row);
            Check("... by the pixel, not by rows", row > 0 && first > 0 && first % row != 0);
            GetClientRect(views[k], &rc);
            MapWindowPoints(views[k], NULL, (POINT *)&rc, 2);
            GetWindowRect(controls[k], &control);
            Check("... the control moving in it", control.top == rc.top - pos);
            if (k == 1 && GetWindowRect(ListView_GetHeader(controls[1]), &header))
                Check("... a list view's header staying on top", header.top == rc.top);
            (void)view;
        }
        SendMessageW(controls[0], WM_KEYDOWN, VK_END, 0);
        Pump(50);
        {
            SCROLLINFO si;
            ZeroMemory(&si, sizeof si);
            si.cbSize = sizeof si;
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            GetScrollInfo(views[0], SB_VERT, &si);
            Check("the keyboard's row is scrolled into sight", si.nPos == si.nMax - (int)si.nPage + 1);
        }
    }
    ShowWindow(host, SW_HIDE);
    for (k = 0; k < 3; k++) DestroyWindow(views[k] ? views[k] : controls[k]);
}

/* --------------------------------------------------------------- links */

/* How much bluer than red the ink of x0..x1 is, on average: ClearType's
 * colored fringes even out, a colored text does not. */
static int InkBlueness(const Canvas *c, int x0, int x1, int y0, int y1, COLORREF back, int *green)
{
    long red = 0, blue = 0, grn = 0, n = 0;
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            COLORREF px = At(c, x, y);
            if (!Stands(px, back)) continue;
            red += GetRValue(px);
            grn += GetGValue(px);
            blue += GetBValue(px);
            n++;
        }
    if (green) *green = n ? (int)(grn / n) : 0;
    return n ? (int)((blue - red) / n) : 0;
}

/* The host answers its controls' colors as a dialog does. */
static LRESULT CALLBACK DialogColors(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    (void)id;
    if (msg == WM_CTLCOLORSTATIC) return Theme_CtlColor(msg, wp, lp, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

/* A link control's link is drawn in a blue that reads on the dark
 * background, its other text not. */
static void TestLink(void)
{
    HWND host = Host(), link;
    RECT win;
    Canvas canvas;
    int plain, linked, green;
    if (!Theme_IsDark()) {
        printf("  skip  link: Windows is set to light apps\n");
        return;
    }
    link = CreateWindowExW(0, WC_LINK, L"Plain text here <a href=\"https://example.com\">a link</a>",
                           WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT | LWS_USECUSTOMTEXT, 10, 10, 300, 20, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!link) {
        Check("link: created", FALSE);
        return;
    }
    SendMessageW(link, WM_SETFONT, (WPARAM)DialogFont(), FALSE);
    SetWindowSubclass(host, DialogColors, 99, 0);
    Theme_Apply(host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    UpdateWindow(host);
    DwmFlush();
    if (!CanvasOpen(&canvas, 500, 300, RGB(0, 0, 0))) {
        Check("link: canvas", FALSE);
    } else {
        PrintWindow(host, canvas.dc, PW_RENDERFULLCONTENT);
        GetWindowRect(link, &win);
        MapWindowPoints(NULL, host, (POINT *)&win, 2);
        {
            COLORREF back = At(&canvas, win.right - 2, win.top + 1);
            plain = InkBlueness(&canvas, win.left, win.left + 60, win.top, win.bottom, back, NULL);   /* "Plain text" */
            linked = InkBlueness(&canvas, win.left + 100, win.right, win.top, win.bottom, back, &green);
        }
        Check("a link is drawn in the link blue", linked > 60);
        Check("... light enough for the dark background (not Windows' own dark blue)", green > 120);
        Check("... and the text before it is not", plain < 30 && plain > -30);
        if (linked <= 60 || green <= 120 || plain >= 30 || plain <= -30) printf("        blueness: text %d, link %d (green %d)\n", plain, linked, green);
        CanvasClose(&canvas);
    }
    ShowWindow(host, SW_HIDE);
    RemoveWindowSubclass(host, DialogColors, 99);
    DestroyWindow(link);
}

/* -------------------------------------------------------------- buffers */

static void TestBuffer(void)
{
    Canvas target;
    ThemeBuffer buffer;
    RECT part = { 10, 10, 20, 20 };
    HDC dc;
    if (!CanvasOpen(&target, 30, 30, RGB(0, 0, 0))) {
        Check("buffer: canvas", FALSE);
        return;
    }
    dc = Theme_BufferBegin(&buffer, target.dc, &part);
    Check("drawing off screen gets its own DC", dc != target.dc);
    SetDCBrushColor(dc, RGB(255, 255, 255));
    FillRect(dc, &part, (HBRUSH)GetStockObject(DC_BRUSH));
    CheckColor("... nothing shows before it ends", RGB(0, 0, 0), At(&target, 15, 15));
    Theme_BufferEnd(&buffer);
    CheckColor("... then all of it, where it was drawn", RGB(255, 255, 255), At(&target, 10, 10));
    CheckColor("... and nothing around it", RGB(0, 0, 0), At(&target, 9, 9));
    CanvasClose(&target);
}

int wmain(void)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    Theme_Init();
    printf("Windows apps: %s\n", Theme_IsDark() ? "dark" : "light");
    TestRows();
    TestHeader();
    TestListEdge();
    TestEdit();
    TestDropDownButton();
    TestDropDownList();
    TestScrollBarEdges();
    TestWheel();
    TestSmoothView();
    TestLink();
    TestBuffer();
    if (g_host) DestroyWindow(g_host);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
