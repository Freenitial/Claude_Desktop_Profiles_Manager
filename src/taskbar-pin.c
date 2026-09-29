/*
 * Taskbar pins (part of the Taskbar Module: see LICENSE).
 *
 * A pin is a .lnk in the User Pinned\TaskBar folder plus an entry in the
 * Taskband Favorites value, written under the taskbar's TaskbarPinListMutex.
 * A pin's icon and name follow the profile: its .lnk is saved in place and the
 * taskbar is notified, so the pin keeps its place. A profile counts as pinned
 * while that folder holds a .lnk that opens it or Favorites names its
 * AppUserModelID; pinning it again brings its pin up to date. At uninstall
 * our pins leave the taskbar.
 */
#include "app.h"
#include <knownfolders.h>
#include <shlobj.h>

#define REG_TASKBAND     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband"
#define PIN_MUTEX        L"TaskbarPinListMutex"
#define PIN_MUTEX_WAIT   5000
#define WM_PINS_CHANGED  (WM_USER + 0x46)   /* 0x446, to the taskbar's MSTaskSwWClass window */
#define SIG_APPID        0xBEEF001Du
#define MAX_PINS         64
#define MAX_BLOCKS       256

static void *Alloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void Free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static WORD Get16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD Get32(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }
static void Put16(BYTE *p, WORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void Put32(BYTE *p, DWORD v) { Put16(p, (WORD)v); Put16(p + 2, (WORD)(v >> 16)); }

BOOL TaskbarPin_Dir(WCHAR *out, size_t cch)
{
    WCHAR pinned[MAX_PATH];
    return Util_KnownFolder(&FOLDERID_UserPinned, pinned, ARRAYSIZE(pinned)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\TaskBar", pinned));
}

static BOOL OpensProfile(const WCHAR *lnk, const Profile *profile)
{
    LinkInfo info;
    return Shortcut_Read(lnk, &info) &&
           Core_LinkOpensProfile(&info, profile->folder, profile->dataDir, profile->isStock);
}

/* A pin shows this icon file: it must stay on disk. */
BOOL TaskbarPin_UsesIcon(const WCHAR *icon)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH], location[MAX_PATH];
    WIN32_FIND_DATAW fd;
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    HANDLE h;
    BOOL hit = FALSE;
    int index;
    if (!TaskbarPin_Dir(dir, ARRAYSIZE(dir)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.lnk", dir)))
        return FALSE;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return FALSE;
    if (FAILED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        IShellLinkW_Release(sl);
        return FALSE;
    }
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) continue;
            if (FAILED(IPersistFile_Load(pf, path, STGM_READ))) continue;
            if (SUCCEEDED(IShellLinkW_GetIconLocation(sl, location, ARRAYSIZE(location), &index)))
                hit = Core_PathEquals(location, icon);
        } while (!hit && FindNextFileW(h, &fd));
        FindClose(h);
    }
    IPersistFile_Release(pf);
    IShellLinkW_Release(sl);
    return hit;
}

/* ------------------------------------------------------ extension blocks */

static BOOL IsBlock(const BYTE *item, size_t pos, size_t cb)
{
    WORD size;
    if (pos + 8 > cb) return FALSE;
    size = Get16(item + pos);
    return size >= 8 && pos + size <= cb && (Get32(item + pos + 4) >> 16) == 0xBEEFu;
}

/* Offset of the first extension block of a well-formed item (the chain from
 * it ends exactly at the item's end), else 0. */
static size_t FirstBlock(const BYTE *item, size_t cb)
{
    size_t first, pos;
    if (cb < 12) return 0;
    first = Get16(item + cb - 2);
    if (first < 4 || first >= cb) return 0;
    for (pos = first; pos < cb && IsBlock(item, pos, cb); pos += Get16(item + pos)) {}
    return pos == cb ? first : 0;
}

/* The last item of a shell ID list with a BEEF001D block holding `appId`.
 * Returns its size, 0 on failure. */
size_t TaskbarPin_InjectAppId(const BYTE *item, size_t cb, const WCHAR *appId, BYTE *out, size_t cap)
{
    size_t nameBytes, blockCb, first, newCb;
    if (!item || !appId || cb < 4) return 0;
    nameBytes = (wcslen(appId) + 1) * sizeof(WCHAR);
    blockCb = 12 + nameBytes;
    first = FirstBlock(item, cb);
    if (!first) first = cb;                     /* no block yet: this one is the first */
    newCb = cb + blockCb;
    if (newCb > 0xFFFF || newCb > cap || first > 0xFFFF) return 0;
    memcpy(out, item, cb);
    Put16(out + cb, (WORD)blockCb);
    Put16(out + cb + 2, 0);
    Put32(out + cb + 4, SIG_APPID);
    Put16(out + cb + 8, 2);
    memcpy(out + cb + 10, appId, nameBytes);
    Put16(out + newCb - 2, (WORD)first);
    Put16(out, (WORD)newCb);
    return newCb;
}

/* An item whose block chain is broken by an offset WORD outside its block:
 * the WORD goes back into its block, and blocks repeated byte for byte are
 * kept once. Returns the repaired size; 0 when the chain is whole or its
 * layout is not recognized. */
size_t TaskbarPin_RepairItem(const BYTE *item, size_t cb, BYTE *out, size_t cap)
{
    size_t start[MAX_BLOCKS], size[MAX_BLOCKS];
    size_t first, pos, n = 0, i, j, o;
    BOOL changed = FALSE, keep[MAX_BLOCKS];
    if (!item || cb < 12) return 0;
    first = Get16(item + cb - 2);
    if (first < 4 || first >= cb) return 0;
    for (pos = first; pos < cb;) {
        if (IsBlock(item, pos, cb)) {
            if (n == MAX_BLOCKS) return 0;
            start[n] = pos;
            size[n++] = Get16(item + pos);
            pos += Get16(item + pos);
        } else if (n && pos + 2 <= cb && Get16(item + pos) == first && (pos + 2 == cb || IsBlock(item, pos + 2, cb))) {
            size[n - 1] += 2;                  /* the stray offset WORD ends the block before it */
            pos += 2;
            changed = TRUE;
        } else {
            return 0;
        }
    }
    if (!changed) return 0;
    for (i = 0; i < n; i++) {
        keep[i] = TRUE;
        for (j = 0; j < i && keep[i]; j++)
            if (keep[j] && size[j] == size[i] && memcmp(item + start[j] + 2, item + start[i] + 2, size[i] - 4) == 0)
                keep[i] = FALSE;
    }
    o = first;
    for (i = 0; i < n; i++) if (keep[i]) o += size[i];
    if (o > 0xFFFF || o > cap) return 0;
    memcpy(out, item, first);
    for (o = first, i = 0; i < n; i++) {
        if (!keep[i]) continue;
        memcpy(out + o, item + start[i], size[i]);
        Put16(out + o, (WORD)size[i]);
        o += size[i];
    }
    Put16(out, (WORD)o);
    return o;
}

/* ---------------------------------------------------------------- entries */

/* One Favorites entry for `lnk`: 0x00, the ID list size (4), the ID list. */
static BYTE *BuildEntry(const WCHAR *lnk, const WCHAR *aumid, DWORD *size)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    PCUITEMID_CHILD last;
    SFGAOF attrs = 0;
    BYTE *entry = NULL, *patched = NULL;
    size_t prefix, lastCb, patchedCb, pidlCb;

    *size = 0;
    if (FAILED(SHParseDisplayName(lnk, NULL, &pidl, 0, &attrs)) || !pidl) pidl = ILCreateFromPathW(lnk);
    if (!pidl) return NULL;
    last = ILFindLastID(pidl);
    if (!last) goto done;
    prefix = (size_t)((const BYTE *)last - (const BYTE *)pidl);
    lastCb = last->mkid.cb;
    if (lastCb < 4) goto done;
    patched = (BYTE *)Alloc(0x10000);
    if (!patched) goto done;
    patchedCb = TaskbarPin_InjectAppId((const BYTE *)last, lastCb, aumid, patched, 0x10000);
    if (!patchedCb) goto done;
    pidlCb = prefix + patchedCb + 2;
    entry = (BYTE *)Alloc(5 + pidlCb);
    if (!entry) goto done;
    entry[0] = 0;
    Put32(entry + 1, (DWORD)pidlCb);
    memcpy(entry + 5, pidl, prefix);
    memcpy(entry + 5 + prefix, patched, patchedCb);
    *size = (DWORD)(5 + pidlCb);                       /* the last two bytes stay 0: end of list */
done:
    Free(patched);
    ILFree(pidl);
    return entry;
}

/* The entry of `blob` starting at `pos`: its total size, 0 when malformed. */
static size_t EntrySize(const BYTE *blob, size_t len, size_t pos)
{
    size_t end;
    if (pos + 5 > len || blob[pos] == 0xFF) return 0;
    end = pos + 5 + Get32(blob + pos + 1);
    return end > len || end < pos + 5 ? 0 : end - pos;
}

static BOOL Contains(const BYTE *p, size_t n, const WCHAR *text)
{
    size_t needle = wcslen(text) * sizeof(WCHAR), b;
    for (b = 0; needle && b + needle <= n; b++)
        if (memcmp(p + b, text, needle) == 0) return TRUE;
    return FALSE;
}

static size_t CountEntries(const BYTE *blob, size_t len)
{
    size_t pos = 0, n, count = 0;
    while ((n = EntrySize(blob, len, pos)) != 0) {
        pos += n;
        count++;
    }
    return count;
}

/* FavoritesResolve keeps one record ([size][data]) per Favorites entry, in
 * the same order: the records of the `dropped` entries go, then the list is
 * cut or padded with empty records to `count`. FALSE when `cap` is too small. */
BOOL TaskbarPin_FitResolve(const BYTE *res, size_t len, const BOOL *dropped, size_t oldCount, size_t count,
                           BYTE *out, size_t cap, size_t *outLen)
{
    size_t pos = 0, o = 0, i, kept = 0;
    DWORD rec;
    for (i = 0; pos + 4 <= len && kept < count; i++) {
        rec = Get32(res + pos);
        if (rec > len - pos - 4) break;
        if (!(dropped && i < oldCount && dropped[i])) {
            if (o + 4 + rec > cap) return FALSE;
            memcpy(out + o, res + pos, 4 + rec);
            o += 4 + rec;
            kept++;
        }
        pos += 4 + rec;
    }
    for (; kept < count; kept++) {
        if (o + 4 > cap) return FALSE;
        Put32(out + o, 0);
        o += 4;
    }
    *outLen = o;
    return TRUE;
}

/* Index of the Favorites entry whose ID list contains `text` (UTF-16), or -1. */
static int FindEntry(const BYTE *blob, size_t len, const WCHAR *text)
{
    size_t pos = 0, n;
    int index = 0;
    while ((n = EntrySize(blob, len, pos)) != 0) {
        if (Contains(blob + pos + 5, n - 5, text)) return index;
        pos += n;
        index++;
    }
    return -1;
}

/* The entry repaired (TaskbarPin_RepairItem on its last item), or NULL. */
static BYTE *RepairEntry(const BYTE *entry, size_t size, size_t *newSize)
{
    size_t pos = 5, last = 0, lastCb = 0, fixedCb, pidlCb;
    BYTE *fixed, *out = NULL;
    while (pos + 2 <= size && Get16(entry + pos) != 0) {
        last = pos;
        lastCb = Get16(entry + pos);
        if (pos + lastCb > size) return NULL;
        pos += lastCb;
    }
    if (!last) return NULL;
    fixed = (BYTE *)Alloc(0x10000);
    if (!fixed) return NULL;
    fixedCb = TaskbarPin_RepairItem(entry + last, lastCb, fixed, 0x10000);
    if (fixedCb) {
        pidlCb = (last - 5) + fixedCb + 2;
        out = (BYTE *)Alloc(5 + pidlCb);
        if (out) {
            memcpy(out, entry, last);
            Put32(out + 1, (DWORD)pidlCb);
            memcpy(out + last, fixed, fixedCb);
            *newSize = 5 + pidlCb;
        }
    }
    Free(fixed);
    return out;
}

/* Favorites as the taskbar keeps it, or NULL when there is none. */
static BYTE *ReadFavorites(HKEY key, DWORD *len)
{
    BYTE *blob;
    DWORD type = 0;
    *len = 0;
    if (RegQueryValueExW(key, L"Favorites", NULL, &type, NULL, len) != ERROR_SUCCESS || type != REG_BINARY || *len < 2)
        return NULL;
    blob = (BYTE *)Alloc(*len);
    if (blob && RegQueryValueExW(key, L"Favorites", NULL, &type, blob, len) == ERROR_SUCCESS && *len >= 2) return blob;
    Free(blob);
    *len = 0;
    return NULL;
}

/* An entry of the taskbar's list names this AppUserModelID: the taskbar
 * groups the windows carrying it with that pin. */
BOOL TaskbarPin_HasAppId(const WCHAR *aumid)
{
    HKEY key;
    BYTE *blob;
    DWORD len;
    BOOL hit = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_TASKBAND, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return FALSE;
    blob = ReadFavorites(key, &len);
    if (blob) hit = FindEntry(blob, len, aumid) >= 0;
    Free(blob);
    RegCloseKey(key);
    return hit;
}

/* A pin that already opens this profile (ours, one made from its taskbar
 * button, or, for the stock profile, Claude's own), or an entry of the
 * taskbar's list naming the profile's AppUserModelID. */
BOOL TaskbarPin_IsPinned(const Profile *profile)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH], aumid[64];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL hit = FALSE;
    Core_ProfileAumid(profile->folder, aumid, ARRAYSIZE(aumid));
    if (TaskbarPin_HasAppId(aumid)) return TRUE;
    if (!TaskbarPin_Dir(dir, ARRAYSIZE(dir)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.lnk", dir)))
        return FALSE;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) continue;
        hit = OpensProfile(path, profile);
    } while (!hit && FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

/* ------------------------------------------------------------- Favorites */

/* Changes Favorites the way the taskbar does: under its mutex, then
 * FavoritesVersion and FavoritesChanges + 1. `edit` gets the list and returns
 * the new one (allocated with Alloc) or NULL to leave it as it is; `hr` says
 * why. It flags in `dropped` (one per entry) the entries it removed. */
typedef BYTE *(*FavoritesEdit)(const BYTE *blob, size_t len, size_t *newLen, BOOL *dropped, void *ctx, HRESULT *hr);

static void FitResolve(HKEY key, const BOOL *dropped, size_t oldCount, size_t count)
{
    BYTE *res, *out;
    DWORD len = 0, type = 0;
    size_t outLen = 0;
    if (RegQueryValueExW(key, L"FavoritesResolve", NULL, &type, NULL, &len) != ERROR_SUCCESS || type != REG_BINARY) return;
    res = (BYTE *)Alloc((size_t)len + 1);
    out = (BYTE *)Alloc((size_t)len + 4 * count + 1);
    if (res && out && RegQueryValueExW(key, L"FavoritesResolve", NULL, &type, res, &len) == ERROR_SUCCESS &&
        TaskbarPin_FitResolve(res, len, dropped, oldCount, count, out, (size_t)len + 4 * count, &outLen) &&
        (outLen != len || memcmp(out, res, outLen) != 0))
        RegSetValueExW(key, L"FavoritesResolve", 0, REG_BINARY, out, (DWORD)outLen);
    Free(res);
    Free(out);
}

static HRESULT EditFavorites(FavoritesEdit edit, void *ctx)
{
    HANDLE mutex;
    HKEY key = NULL;
    BYTE *blob = NULL, *changed = NULL;
    BOOL *dropped = NULL;
    DWORD len = 0, changes = 0, version = 3, wait;
    size_t newLen = 0, oldCount;
    LSTATUS rc;
    HRESULT hr = S_FALSE;

    mutex = CreateMutexExW(NULL, PIN_MUTEX, 0, MUTEX_ALL_ACCESS);
    if (!mutex) return HRESULT_FROM_WIN32(GetLastError());
    wait = WaitForSingleObject(mutex, PIN_MUTEX_WAIT);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, REG_TASKBAND, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        hr = HRESULT_FROM_WIN32(rc);
        goto done;
    }
    blob = ReadFavorites(key, &len);
    if (!blob) {
        blob = (BYTE *)Alloc(1);
        if (!blob) {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        blob[0] = 0xFF;
        len = 1;
    }
    oldCount = CountEntries(blob, len);
    dropped = (BOOL *)Alloc((oldCount + 1) * sizeof *dropped);
    if (!dropped) {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    changed = edit(blob, len, &newLen, dropped, ctx, &hr);
    if (!changed) goto done;
    if (!Util_RegGetDword(HKEY_CURRENT_USER, REG_TASKBAND, L"FavoritesChanges", &changes)) changes = 0;
    changes++;
    rc = RegSetValueExW(key, L"Favorites", 0, REG_BINARY, changed, (DWORD)newLen);
    if (rc == ERROR_SUCCESS) FitResolve(key, dropped, oldCount, CountEntries(changed, newLen));
    if (rc == ERROR_SUCCESS) rc = RegSetValueExW(key, L"FavoritesVersion", 0, REG_DWORD, (const BYTE *)&version, sizeof version);
    if (rc == ERROR_SUCCESS) rc = RegSetValueExW(key, L"FavoritesChanges", 0, REG_DWORD, (const BYTE *)&changes, sizeof changes);
    hr = rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
done:
    if (key) RegCloseKey(key);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    Free(blob);
    Free(changed);
    Free(dropped);
    return hr;
}

/* The end of the list: where a new entry goes (before the 0xFF). */
static size_t ListEnd(const BYTE *blob, size_t len)
{
    size_t pos = 0, n;
    while ((n = EntrySize(blob, len, pos)) != 0) pos += n;
    return pos;
}

typedef struct AddCtx {
    const BYTE  *entry;
    DWORD        entrySize;
    const WCHAR *file;
    const WCHAR *aumid;
} AddCtx;

/* Appends the entry unless one for the file or the AppUserModelID is there. */
static BYTE *AddEdit(const BYTE *blob, size_t len, size_t *newLen, BOOL *dropped, void *ctx, HRESULT *hr)
{
    const AddCtx *a = (const AddCtx *)ctx;
    size_t end;
    BYTE *merged;
    (void)dropped;
    if (FindEntry(blob, len, a->file) >= 0 || FindEntry(blob, len, a->aumid) >= 0) {
        *hr = S_FALSE;
        return NULL;
    }
    end = ListEnd(blob, len);
    merged = (BYTE *)Alloc(end + a->entrySize + 1);
    if (!merged) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    memcpy(merged, blob, end);
    memcpy(merged + end, a->entry, a->entrySize);
    merged[end + a->entrySize] = 0xFF;
    *newLen = end + a->entrySize + 1;
    return merged;
}

/* Our entries (their ID list names an AppUserModelID of ours) repaired. */
static BYTE *RepairEdit(const BYTE *blob, size_t len, size_t *newLen, BOOL *dropped, void *ctx, HRESULT *hr)
{
    BYTE *out, *fixed;
    size_t pos = 0, o = 0, n, fixedSize = 0;
    BOOL changed = FALSE;
    (void)dropped;
    (void)ctx;
    out = (BYTE *)Alloc(len + 1);
    if (!out) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    while ((n = EntrySize(blob, len, pos)) != 0) {
        fixed = Contains(blob + pos + 5, n - 5, APP_AUMID_PREFIX) ? RepairEntry(blob + pos, n, &fixedSize) : NULL;
        if (fixed && fixedSize <= n) {         /* a repair only ever removes bytes */
            memcpy(out + o, fixed, fixedSize);
            o += fixedSize;
            changed = TRUE;
        } else {
            memcpy(out + o, blob + pos, n);
            o += n;
        }
        Free(fixed);
        pos += n;
    }
    out[o++] = 0xFF;
    if (!changed) {
        Free(out);
        *hr = S_FALSE;
        return NULL;
    }
    *newLen = o;
    return out;
}

/* Our entries removed: their ID list names an AppUserModelID of ours, which
 * pins made from a profile's taskbar button carry too. */
static BYTE *RemoveEdit(const BYTE *blob, size_t len, size_t *newLen, BOOL *dropped, void *ctx, HRESULT *hr)
{
    BYTE *out;
    size_t pos = 0, o = 0, n, index = 0;
    BOOL changed = FALSE;
    (void)ctx;
    out = (BYTE *)Alloc(len + 1);
    if (!out) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    while ((n = EntrySize(blob, len, pos)) != 0) {
        if (Contains(blob + pos + 5, n - 5, APP_AUMID_PREFIX)) {
            dropped[index] = TRUE;
            changed = TRUE;
        } else {
            memcpy(out + o, blob + pos, n);
            o += n;
        }
        pos += n;
        index++;
    }
    out[o++] = 0xFF;
    if (!changed) {
        Free(out);
        *hr = S_FALSE;
        return NULL;
    }
    *newLen = o;
    return out;
}

static void TellTaskbar(void)
{
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND rebar = tray ? FindWindowExW(tray, NULL, L"ReBarWindow32", NULL) : NULL;
    HWND band = rebar ? FindWindowExW(rebar, NULL, L"MSTaskSwWClass", NULL) : NULL;
    if (band) PostMessageW(band, WM_PINS_CHANGED, 0, 0);
}

/* Our malformed entries repaired. */
void TaskbarPin_RepairOurs(void)
{
    if (EditFavorites(RepairEdit, NULL) == S_OK) {
        TellTaskbar();
        Util_Log(L"repaired taskbar pin entries");
    }
}

/* ------------------------------------------------------------------ pins */

/* Our pins that open `folder` (target
 * ClaudeDesktopProfilesManager.exe --launch "<folder>"), or with NULL all the
 * pins that run ClaudeDesktopProfilesManager.exe. */
static DWORD OurPins(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD max)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    LinkInfo info;
    HANDLE h;
    DWORD n = 0;
    if (!TaskbarPin_Dir(dir, ARRAYSIZE(dir)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.lnk", dir)))
        return 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (FAILED(StringCchPrintfW(paths[n], MAX_PATH, L"%s\\%s", dir, fd.cFileName))) continue;
        if (Shortcut_Read(paths[n], &info) && Core_IsOurExe(info.target) &&
            (!folder || Core_ArgsSelectProfile(info.args, folder)))
            n++;
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* At uninstall: our entries leave Favorites, then our .lnk files go. */
void TaskbarPin_RemoveOurs(void)
{
    WCHAR (*paths)[MAX_PATH];
    DWORD i, n = 0;
    HRESULT hr = EditFavorites(RemoveEdit, NULL);
    if (hr == S_OK) {
        TellTaskbar();
        Util_Log(L"removed our taskbar pins");
    } else if (FAILED(hr)) {
        Util_Log(L"could not remove our taskbar pins (0x%08lX)", (unsigned long)hr);
    }
    paths = (WCHAR (*)[MAX_PATH])Alloc(MAX_PINS * sizeof *paths);
    if (paths) n = OurPins(NULL, paths, MAX_PINS);
    for (i = 0; i < n; i++) {
        if (DeleteFileW(paths[i])) {
            SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, paths[i], NULL);
            Util_Log(L"removed taskbar pin %s", paths[i]);
        }
    }
    Free(paths);
}

/* After a rename or a color change, and when a pinned profile is pinned
 * again: its pins get the new icon, description and name. TRUE when a pin
 * changed. */
BOOL TaskbarPin_Refresh(const Profile *before, const Profile *after, const WCHAR *icon)
{
    WCHAR (*paths)[MAX_PATH], target[MAX_PATH];
    DWORD i, n;
    BOOL changed = FALSE;

    TaskbarPin_RepairOurs();
    paths = (WCHAR (*)[MAX_PATH])Alloc(MAX_PINS * sizeof *paths);
    if (!paths) return FALSE;
    n = OurPins(before->folder, paths, MAX_PINS);
    for (i = 0; i < n; i++) {
        const WCHAR *path = paths[i];
        BOOL renamed = FALSE;
        HRESULT hr;
        if (Shortcut_RenamedPath(path, before, after, target, ARRAYSIZE(target)) && MoveFileW(path, target)) {
            SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSH, path, target);
            Util_Log(L"renamed taskbar pin %s to %s", path, target);
            path = target;
            renamed = TRUE;
        }
        hr = Shortcut_Update(path, after, icon);
        if (hr == S_OK && !renamed) SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSH, path, path);
        if (hr == S_OK || renamed) {
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, path, NULL);
            Util_Log(L"updated taskbar pin %s", path);
            changed = TRUE;
        } else if (FAILED(hr)) {
            Util_Log(L"could not update taskbar pin %s (0x%08lX)", path, (unsigned long)hr);
        }
    }
    Free(paths);
    return changed;
}

/* The file the pin uses: a free "Claude (<name>).lnk" in the pins folder, or
 * an existing one that already opens this profile (never overwritten). */
static BOOL PinPath(const WCHAR *dir, const Profile *profile, WCHAR *out, size_t cch, BOOL *exists)
{
    WCHAR name[MAX_PATH];
    int copy;
    for (copy = 1; copy <= 50; copy++) {
        Core_ShortcutFileName(profile->name, copy, name, ARRAYSIZE(name));
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\%s", dir, name))) return FALSE;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
            *exists = FALSE;
            return TRUE;
        }
        if (OpensProfile(out, profile)) {
            *exists = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

/* Pins the profile. Already pinned: its pin is brought up to date instead
 * (S_FALSE). */
HRESULT TaskbarPin_Pin(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR dir[MAX_PATH], lnk[MAX_PATH], aumid[64], icon[MAX_PATH];
    AddCtx add;
    BYTE *entry = NULL;
    DWORD entrySize = 0;
    BOOL exists = FALSE;
    HRESULT hr;

    if (TaskbarPin_IsPinned(profile)) {
        if (Icons_Ensure(pkg, profile, icon, ARRAYSIZE(icon))) TaskbarPin_Refresh(profile, profile, icon);
        else TaskbarPin_RepairOurs();
        return S_FALSE;
    }
    if (!TaskbarPin_Dir(dir, ARRAYSIZE(dir)) || !Util_DirExists(dir) || !Util_RegKeyExists(HKEY_CURRENT_USER, REG_TASKBAND))
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if (!PinPath(dir, profile, lnk, ARRAYSIZE(lnk), &exists)) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    if (!exists) {
        hr = Shortcut_WriteForProfile(pkg, profile, lnk);
        if (FAILED(hr)) return hr;
    }
    Core_ProfileAumid(profile->folder, aumid, ARRAYSIZE(aumid));
    entry = BuildEntry(lnk, aumid, &entrySize);
    add.entry = entry;
    add.entrySize = entrySize;
    add.file = wcsrchr(lnk, L'\\') + 1;
    add.aumid = aumid;
    hr = entry ? EditFavorites(AddEdit, &add) : E_FAIL;
    Free(entry);
    if (hr != S_OK) {
        if (!exists) DeleteFileW(lnk);   /* only a file made here: never a live pin */
        if (FAILED(hr)) Util_Log(L"could not pin %s to the taskbar (0x%08lX)", profile->folder, (unsigned long)hr);
        return hr;
    }
    TellTaskbar();
    Util_Log(L"pinned %s to the taskbar (%s)", profile->folder, lnk);
    return S_OK;
}
