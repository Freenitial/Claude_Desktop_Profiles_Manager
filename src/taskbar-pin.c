/*
 * Taskbar pins (part of the Taskbar Module: see LICENSE).
 *
 * A pin is a shortcut in the taskbar's pins folder, an entry of the taskbar's
 * pin list and the record that goes with that entry, written under the
 * taskbar's lock in the form the taskbar writes them. A pin's icon and name
 * follow the profile: its shortcut is saved in place and the taskbar is told,
 * so the pin keeps its place. A profile counts as pinned while the list has an
 * active entry whose shortcut opens it; entries Windows retired stay as its
 * history. At uninstall our pins leave the taskbar.
 */
#include "app.h"
#include <knownfolders.h>
#include <shlobj.h>

#define PIN_LIST_LOCK         L"TaskbarPinListMutex"
#define PIN_LIST_LOCK_WAIT_MS 5000
#define EDIT_ATTEMPTS         3      /* Explorer saved the list between our read and our write */
#define SNAPSHOT_ATTEMPTS     3      /* a value grew between the query of its size and its read */
#define WM_PINS_CHANGED       (WM_USER + 0x46)   /* the taskbar reloads its pins */
#define PIN_LIST_VERSION      3      /* the only form of the list read and written here */
#define BLOCK_FAMILY          0xBEEFu
#define APPID_TAG             0xBEEF001Du
#define RETIRED_TAG           0xBEEF002Cu
#define BLOCK_HEADER_SIZE     8
#define BLOCKS_MIN_ITEM_SIZE  12     /* the smallest item that can hold a block */
#define FIRST_BLOCK_MIN_OFFSET 4
#define APPID_TEXT_OFFSET     10
#define APPID_BLOCK_EXTRA     12     /* an AppUserModelID's block beyond its text */
#define ENTRY_HEADER_SIZE     5
#define LIST_END              0xFF
#define LINK_HEADER_SIZE      0x4Cu
#define LINK_FLAGS_OFFSET     0x14u
#define PIN_APPID_CCH         256
#define MAX_BLOCKS            256
#define MAX_LISTED_PATHS      0x10000   /* far more shortcuts than a pins folder holds */

enum { VALUE_PIN_LIST, VALUE_RECORDS, VALUE_LIST_VERSION, VALUE_CHANGE_COUNT, PIN_VALUE_COUNT };
static const WCHAR *const kPinValues[PIN_VALUE_COUNT] = { L"Favorites", L"FavoritesResolve", L"FavoritesVersion", L"FavoritesChanges" };

static void *AllocateZeroed(size_t size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size); }
static void FreeMemory(void *block) { if (block) HeapFree(GetProcessHeap(), 0, block); }

static WORD Get16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD Get32(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }
static void Put16(BYTE *p, WORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void Put32(BYTE *p, DWORD v) { Put16(p, (WORD)v); Put16(p + 2, (WORD)(v >> 16)); }

/* Paths in a list that grows as needed (free `items` with FreeMemory). */
typedef struct PathList {
    WCHAR (*items)[MAX_PATH];
    DWORD count, capacity;
} PathList;

static BOOL AppendPath(PathList *list, const WCHAR *path)
{
    if (list->count == list->capacity) {
        DWORD capacity = list->capacity ? list->capacity * 2 : 16;
        void *grown;
        if (capacity > MAX_LISTED_PATHS) return FALSE;
        grown = list->items ? HeapReAlloc(GetProcessHeap(), 0, list->items, capacity * sizeof *list->items)
                            : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof *list->items);
        if (!grown) return FALSE;
        list->items = (WCHAR (*)[MAX_PATH])grown;
        list->capacity = capacity;
    }
    if (FAILED(StringCchCopyW(list->items[list->count], MAX_PATH, path))) return FALSE;
    list->count++;
    return TRUE;
}

BOOL TaskbarPin_Dir(WCHAR *out, size_t cch)
{
    WCHAR pinned[MAX_PATH];
    return Util_KnownFolder(&FOLDERID_UserPinned, pinned, ARRAYSIZE(pinned)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\TaskBar", pinned));
}

/* AppsFolder may return either a parsing path or the bare package AppID. */
static void NativeParsingPath(LinkInfo *info)
{
    WCHAR parsing[ARRAYSIZE(info->parsing)];
    if (info->target[0] || !info->parsing[0] || wcschr(info->parsing, L'\\') || wcschr(info->parsing, L'/')) return;
    if (SUCCEEDED(StringCchPrintfW(parsing, ARRAYSIZE(parsing), L"shell:AppsFolder\\%s", info->parsing)))
        StringCchCopyW(info->parsing, ARRAYSIZE(info->parsing), parsing);
}

static BOOL OpensProfile(const WCHAR *lnk, const Profile *profile)
{
    LinkInfo info;
    if (!Shortcut_Read(lnk, &info)) return FALSE;
    NativeParsingPath(&info);
    return Core_LinkOpensProfile(&info, profile->folder, profile->dataDir, profile->isStock);
}

/* Every shortcut of the pins folder (free `pins->items`). FALSE when they
 * could not all be listed. */
static BOOL PinnedShortcuts(PathList *pins)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    BOOL complete = TRUE;
    DWORD error;
    ZeroMemory(pins, sizeof *pins);
    if (!TaskbarPin_Dir(dir, ARRAYSIZE(dir)) || FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.lnk", dir)))
        return FALSE;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            complete = SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) && AppendPath(pins, path);
    } while (complete && FindNextFileW(search, &found));
    if (complete && GetLastError() != ERROR_NO_MORE_FILES) complete = FALSE;
    FindClose(search);
    return complete;
}

/* A pin may show this icon file, which must then stay on disk; so must it
 * when the pins, or one of them, cannot be read. */
BOOL TaskbarPin_UsesIcon(const WCHAR *icon)
{
    PathList pins;
    WCHAR location[MAX_PATH];
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    BOOL used = FALSE;
    DWORD i;
    int index;
    if (!PinnedShortcuts(&pins)) {
        used = TRUE;
    } else if (pins.count) {
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)) &&
            SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            for (i = 0; i < pins.count && !used; i++)
                used = FAILED(IPersistFile_Load(file, pins.items[i], STGM_READ)) ||
                       FAILED(IShellLinkW_GetIconLocation(link, location, ARRAYSIZE(location), &index)) ||
                       Core_PathEquals(location, icon);
        } else {
            used = TRUE;
        }
    }
    if (file) IPersistFile_Release(file);
    if (link) IShellLinkW_Release(link);
    FreeMemory(pins.items);
    return used;
}

/* -------------------------------------------------------- an item's blocks */

static BOOL IsBlock(const BYTE *item, size_t pos, size_t cb)
{
    WORD size;
    if (pos + BLOCK_HEADER_SIZE > cb) return FALSE;
    size = Get16(item + pos);
    return size >= BLOCK_HEADER_SIZE && pos + size <= cb && (Get32(item + pos + 4) >> 16) == BLOCK_FAMILY;
}

/* One step along an item's blocks from `pos`: the bytes to go over (`*stray`
 * when they belong to the block before), 0 when the blocks cannot be followed. */
static size_t BlockStep(const BYTE *item, size_t cb, size_t first, size_t pos, BOOL afterBlock, BOOL *stray)
{
    *stray = FALSE;
    if (IsBlock(item, pos, cb)) return Get16(item + pos);
    if (afterBlock && pos + 2 <= cb && Get16(item + pos) == first && (pos + 2 == cb || IsBlock(item, pos + 2, cb))) {
        *stray = TRUE;
        return 2;
    }
    return 0;
}

/* Where an item says its blocks start, 0 when they cannot start there. */
static size_t StatedFirstBlock(const BYTE *item, size_t cb)
{
    size_t first;
    if (cb < BLOCKS_MIN_ITEM_SIZE) return 0;
    first = Get16(item + cb - 2);
    return first >= FIRST_BLOCK_MIN_OFFSET && first < cb ? first : 0;
}

/* Where the blocks of a well-formed item start, else 0. */
static size_t FirstBlock(const BYTE *item, size_t cb)
{
    size_t first = StatedFirstBlock(item, cb), pos;
    if (!first) return 0;
    for (pos = first; pos < cb && IsBlock(item, pos, cb); pos += Get16(item + pos)) {}
    return pos == cb ? first : 0;
}

/* The last item of an ID list with `appId` added, in the form the taskbar
 * reads. Returns its size, 0 on failure. */
size_t TaskbarPin_InjectAppId(const BYTE *item, size_t cb, const WCHAR *appId, BYTE *out, size_t cap)
{
    size_t textBytes, blockCb, first, newCb;
    if (!item || !appId || cb < 4) return 0;
    textBytes = (wcslen(appId) + 1) * sizeof(WCHAR);
    blockCb = APPID_BLOCK_EXTRA + textBytes;
    first = FirstBlock(item, cb);
    if (!first) first = cb;                     /* no block yet: this one is the first */
    newCb = cb + blockCb;
    if (newCb > 0xFFFF || newCb > cap) return 0;
    memcpy(out, item, cb);
    Put16(out + cb, (WORD)blockCb);
    Put16(out + cb + 2, 0);
    Put32(out + cb + 4, APPID_TAG);
    Put16(out + cb + 8, 2);
    memcpy(out + cb + APPID_TEXT_OFFSET, appId, textBytes);
    Put16(out + newCb - 2, (WORD)first);
    Put16(out, (WORD)newCb);
    return newCb;
}

/* An item whose blocks Windows saved damaged, repaired; blocks repeated byte
 * for byte are kept once. Returns the repaired size; 0 when the item is whole
 * or its form is not recognized. */
size_t TaskbarPin_RepairItem(const BYTE *item, size_t cb, BYTE *out, size_t cap)
{
    size_t start[MAX_BLOCKS], size[MAX_BLOCKS];
    size_t first, pos, step, n = 0, i, j, outPos;
    BOOL changed = FALSE, keep[MAX_BLOCKS], stray;
    if (!item || (first = StatedFirstBlock(item, cb)) == 0) return 0;
    for (pos = first; pos < cb; pos += step) {
        if ((step = BlockStep(item, cb, first, pos, n > 0, &stray)) == 0) return 0;
        if (stray) {
            size[n - 1] += 2;
            changed = TRUE;
        } else {
            if (n == MAX_BLOCKS) return 0;
            start[n] = pos;
            size[n++] = step;
        }
    }
    if (!changed) return 0;
    for (i = 0; i < n; i++) {
        keep[i] = TRUE;
        for (j = 0; j < i && keep[i]; j++)
            if (keep[j] && size[j] == size[i] && memcmp(item + start[j] + 2, item + start[i] + 2, size[i] - 4) == 0)
                keep[i] = FALSE;
    }
    outPos = first;
    for (i = 0; i < n; i++) if (keep[i]) outPos += size[i];
    if (outPos > 0xFFFF || outPos > cap) return 0;
    memcpy(out, item, first);
    for (outPos = first, i = 0; i < n; i++) {
        if (!keep[i]) continue;
        memcpy(out + outPos, item + start[i], size[i]);
        Put16(out + outPos, (WORD)size[i]);
        outPos += size[i];
    }
    Put16(out, (WORD)outPos);
    return outPos;
}

/* ---------------------------------------------------------------- entries */

/* The pin list's entry for shortcut `lnk` with the AppUserModelID `aumid`
 * (free it with FreeMemory). */
static HRESULT BuildEntry(const WCHAR *lnk, const WCHAR *aumid, BYTE **entry, DWORD *size)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    PCUITEMID_CHILD last;
    BYTE *patched = NULL;
    size_t prefix, lastCb, patchedCap, patchedCb, pidlCb;
    HRESULT hr = SHParseDisplayName(lnk, NULL, &pidl, 0, NULL);
    *entry = NULL;
    *size = 0;
    if (FAILED(hr) || !pidl) {
        pidl = ILCreateFromPathW(lnk);
        if (!pidl) return FAILED(hr) ? hr : E_FAIL;
    }
    hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    last = ILFindLastID(pidl);
    if (last && (lastCb = last->mkid.cb) >= 4) {
        prefix = (size_t)((const BYTE *)last - (const BYTE *)pidl);
        patchedCap = lastCb + APPID_BLOCK_EXTRA + (wcslen(aumid) + 1) * sizeof(WCHAR);
        patched = (BYTE *)AllocateZeroed(patchedCap);
        if (!patched) {
            hr = E_OUTOFMEMORY;
        } else if ((patchedCb = TaskbarPin_InjectAppId((const BYTE *)last, lastCb, aumid, patched, patchedCap)) != 0) {
            pidlCb = prefix + patchedCb + 2;
            *entry = (BYTE *)AllocateZeroed(ENTRY_HEADER_SIZE + pidlCb);
            if (!*entry) {
                hr = E_OUTOFMEMORY;
            } else {
                Put32(*entry + 1, (DWORD)pidlCb);
                memcpy(*entry + ENTRY_HEADER_SIZE, pidl, prefix);
                memcpy(*entry + ENTRY_HEADER_SIZE + prefix, patched, patchedCb);
                *size = (DWORD)(ENTRY_HEADER_SIZE + pidlCb);   /* the ID list's terminator stays 0 */
                hr = S_OK;
            }
        }
    }
    FreeMemory(patched);
    ILFree(pidl);
    return hr;
}

/* The entry of `list` starting at `pos`: its total size, 0 when malformed. */
static size_t EntrySize(const BYTE *list, size_t len, size_t pos)
{
    size_t cb;
    if (!list || pos >= len || len - pos < ENTRY_HEADER_SIZE || list[pos] == LIST_END) return 0;
    cb = Get32(list + pos + 1);
    return cb < 2 || cb > len - pos - ENTRY_HEADER_SIZE ? 0 : ENTRY_HEADER_SIZE + cb;
}

/* The last item of the entry's ID list (its size in *lastCb); NULL when the
 * list does not end exactly where the entry says it does. */
static const BYTE *LastIdListItem(const BYTE *entry, size_t size, size_t *lastCb)
{
    size_t pos = ENTRY_HEADER_SIZE, cb;
    const BYTE *last = NULL;
    if (!entry || size < ENTRY_HEADER_SIZE + 2 || EntrySize(entry, size, 0) != size) return NULL;
    while (size - pos >= 2) {
        cb = Get16(entry + pos);
        if (!cb) return pos + 2 == size ? last : NULL;
        if (cb < 2 || cb > size - pos) return NULL;
        last = entry + pos;
        *lastCb = cb;
        pos += cb;
    }
    return NULL;
}

BOOL TaskbarPin_ValidateList(const BYTE *list, size_t len)
{
    size_t pos = 0, n, cb;
    if (!list || !len) return FALSE;
    while (pos < len && list[pos] != LIST_END) {
        n = EntrySize(list, len, pos);
        if (!n || !LastIdListItem(list + pos, n, &cb)) return FALSE;
        pos += n;
    }
    return pos + 1 == len && list[pos] == LIST_END;
}

/* What the entry's blocks say: whether Windows retired it, and its
 * AppUserModelID (a retired entry keeps it). FALSE when they cannot be read. */
static BOOL ParseEntryState(const BYTE *entry, size_t size, BOOL *removed, WCHAR *appId, size_t cch)
{
    size_t cb = 0, first, pos, step, chars;
    const BYTE *item = LastIdListItem(entry, size, &cb);
    BOOL stray;
    *removed = FALSE;
    appId[0] = 0;
    if (!item) return FALSE;
    first = StatedFirstBlock(item, cb);
    if (!first || first + BLOCK_HEADER_SIZE > cb || (Get32(item + first + 4) >> 16) != BLOCK_FAMILY) return TRUE;   /* no blocks */
    for (pos = first; pos < cb; pos += step) {
        if ((step = BlockStep(item, cb, first, pos, pos > first, &stray)) == 0) return FALSE;
        if (stray) continue;
        if (Get32(item + pos + 4) == RETIRED_TAG) *removed = TRUE;
        if (Get32(item + pos + 4) == APPID_TAG) {
            if (step < APPID_BLOCK_EXTRA || Get16(item + pos + 8) != 2) return FALSE;
            for (chars = 0; APPID_TEXT_OFFSET + (chars + 1) * sizeof(WCHAR) <= step; chars++)
                if (!Get16(item + pos + APPID_TEXT_OFFSET + chars * sizeof(WCHAR))) break;
            if (APPID_TEXT_OFFSET + (chars + 1) * sizeof(WCHAR) > step ||
                FAILED(StringCchCopyNW(appId, cch, (const WCHAR *)(item + pos + APPID_TEXT_OFFSET), chars))) return FALSE;
        }
    }
    return TRUE;
}

/* Like ParseEntryState, an entry whose blocks cannot be read counting as
 * active, without an AppUserModelID. */
static void ReadEntryState(const BYTE *entry, size_t size, BOOL *removed, WCHAR *appId, size_t cch)
{
    if (!ParseEntryState(entry, size, removed, appId, cch)) {
        *removed = FALSE;
        appId[0] = 0;
    }
}

static BOOL SameAppId(const WCHAR *a, const WCHAR *b)
{
    return a[0] && b && b[0] && Core_EqualsI(a, b);
}

static BOOL IsOurAppId(const WCHAR *appId)
{
    size_t n = wcslen(APP_AUMID_PREFIX);
    return wcslen(appId) >= n && CompareStringOrdinal(appId, (int)n, APP_AUMID_PREFIX, (int)n, TRUE) == CSTR_EQUAL;
}

static PIDLIST_ABSOLUTE EntryPidl(const BYTE *entry, size_t size)
{
    PIDLIST_RELATIVE child;
    PIDLIST_ABSOLUTE parent = NULL, full = NULL;
    size_t cb = 0;
    if (!LastIdListItem(entry, size, &cb)) return NULL;
    child = (PIDLIST_RELATIVE)CoTaskMemAlloc(size - ENTRY_HEADER_SIZE);
    if (!child) return NULL;
    memcpy(child, entry + ENTRY_HEADER_SIZE, size - ENTRY_HEADER_SIZE);
    if (!entry[0]) return child;
    if (SUCCEEDED(SHGetSpecialFolderLocation(NULL, entry[0], &parent)) && parent) full = ILCombine(parent, child);
    ILFree(parent);
    CoTaskMemFree(child);
    return full;
}

static BOOL EntryPath(const BYTE *entry, size_t size, WCHAR *path)
{
    PIDLIST_ABSOLUTE pidl = EntryPidl(entry, size);
    BOOL ok = pidl && SHGetPathFromIDListW(pidl, path);
    ILFree(pidl);
    return ok;
}

/* An active entry holds `appId`; with `live`, its shortcut also still exists
 * (an entry without a file, such as an Apps folder item, counts). */
BOOL TaskbarPin_ListHasAppId(const BYTE *list, size_t len, const WCHAR *appId, BOOL live)
{
    size_t pos = 0, n;
    WCHAR found[PIN_APPID_CCH], path[MAX_PATH];
    BOOL removed;
    if (!TaskbarPin_ValidateList(list, len)) return FALSE;
    while ((n = EntrySize(list, len, pos)) != 0) {
        if (ParseEntryState(list + pos, n, &removed, found, ARRAYSIZE(found)) && !removed && SameAppId(found, appId) &&
            (!live || !EntryPath(list + pos, n, path) || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES))
            return TRUE;
        pos += n;
    }
    return FALSE;
}

BOOL TaskbarPin_ListHasProfile(const BYTE *list, size_t len, const Profile *profile)
{
    size_t pos = 0, n;
    WCHAR appId[PIN_APPID_CCH], path[MAX_PATH];
    BOOL removed;
    if (!profile || !TaskbarPin_ValidateList(list, len)) return FALSE;
    while ((n = EntrySize(list, len, pos)) != 0) {
        if (ParseEntryState(list + pos, n, &removed, appId, ARRAYSIZE(appId)) && !removed) {
            PIDLIST_ABSOLUTE pidl = EntryPidl(list + pos, n);
            BOOL hit = FALSE;
            if (pidl && SHGetPathFromIDListW(pidl, path)) {
                DWORD attributes = GetFileAttributesW(path);
                hit = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) && OpensProfile(path, profile);
            } else if (pidl && profile->isStock) {
                PWSTR name = NULL;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
                    LinkInfo info;
                    ZeroMemory(&info, sizeof info);
                    if (SUCCEEDED(StringCchCopyW(info.parsing, ARRAYSIZE(info.parsing), name))) {
                        NativeParsingPath(&info);
                        hit = Core_LinkOpensProfile(&info, profile->folder, profile->dataDir, TRUE);
                    }
                    CoTaskMemFree(name);
                }
            }
            ILFree(pidl);
            if (hit) return TRUE;
        }
        pos += n;
    }
    return FALSE;
}

static size_t CountEntries(const BYTE *list, size_t len)
{
    size_t pos = 0, n, count = 0;
    while ((n = EntrySize(list, len, pos)) != 0) {
        pos += n;
        count++;
    }
    return count;
}

/* The entry repaired (TaskbarPin_RepairItem on its last item), or NULL. */
static BYTE *RepairEntry(const BYTE *entry, size_t size, size_t *newSize)
{
    size_t lastCb = 0, last, repairedCb, pidlCb;
    const BYTE *item = LastIdListItem(entry, size, &lastCb);
    BYTE *repaired, *out = NULL;
    if (!item) return NULL;
    last = (size_t)(item - entry);
    repaired = (BYTE *)AllocateZeroed(lastCb);   /* a repair only removes bytes */
    if (!repaired) return NULL;
    repairedCb = TaskbarPin_RepairItem(item, lastCb, repaired, lastCb);
    if (repairedCb) {
        pidlCb = (last - ENTRY_HEADER_SIZE) + repairedCb + 2;
        out = (BYTE *)AllocateZeroed(ENTRY_HEADER_SIZE + pidlCb);
        if (out) {
            memcpy(out, entry, last);
            Put32(out + 1, (DWORD)pidlCb);
            memcpy(out + last, repaired, repairedCb);
            *newSize = ENTRY_HEADER_SIZE + pidlCb;
        }
    }
    FreeMemory(repaired);
    return out;
}

/* ---------------------------------------------------------------- values */

/* A value as it is now (free `data`). Explorer may change it between the
 * query of its size and its read: a larger value is read again, a deleted one
 * counts as missing. */
static LSTATUS SnapshotValue(const TaskbarPin_ValueIO *io, const WCHAR *name, TaskbarPin_Value *value)
{
    LSTATUS rc = ERROR_MORE_DATA;
    int attempt;
    ZeroMemory(value, sizeof *value);
    for (attempt = 0; attempt < SNAPSHOT_ATTEMPTS && rc == ERROR_MORE_DATA; attempt++) {
        FreeMemory(value->data);
        ZeroMemory(value, sizeof *value);
        rc = io->read(io->context, name, &value->type, NULL, &value->bytes);
        if (rc != ERROR_SUCCESS) break;
        value->data = (BYTE *)AllocateZeroed((size_t)value->bytes + 1);
        rc = value->data ? io->read(io->context, name, &value->type, value->data, &value->bytes) : ERROR_OUTOFMEMORY;
    }
    if (rc == ERROR_SUCCESS) {
        value->exists = TRUE;
        return ERROR_SUCCESS;
    }
    FreeMemory(value->data);
    ZeroMemory(value, sizeof *value);
    return rc == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : rc;
}

static BOOL SameValue(const TaskbarPin_Value *a, const TaskbarPin_Value *b)
{
    return a->exists == b->exists &&
           (!a->exists || (a->type == b->type && a->bytes == b->bytes && (!a->bytes || memcmp(a->data, b->data, a->bytes) == 0)));
}

/* A DWORD value's number; FALSE when it is not one. */
static BOOL ValueNumber(const TaskbarPin_Value *value, DWORD *number)
{
    if (value->type != REG_DWORD || value->bytes != sizeof *number) return FALSE;
    memcpy(number, value->data, sizeof *number);
    return TRUE;
}

static LSTATUS ReadPinValue(void *context, const WCHAR *name, DWORD *type, BYTE *data, DWORD *bytes)
{
    return RegQueryValueExW((HKEY)context, name, NULL, type, data, bytes);
}

static LSTATUS WritePinValue(void *context, const WCHAR *name, DWORD type, const BYTE *data, DWORD bytes)
{
    return RegSetValueExW((HKEY)context, name, 0, type, data, bytes);
}

static LSTATUS ErasePinValue(void *context, const WCHAR *name)
{
    return RegDeleteValueW((HKEY)context, name);
}

static BOOL IsPredefinedKey(HKEY key)
{
    return (ULONG_PTR)key >= (ULONG_PTR)HKEY_CLASSES_ROOT && (ULONG_PTR)key <= (ULONG_PTR)HKEY_PERFORMANCE_NLSTEXT;
}

/* The pin list's values in an opened key, never in a whole hive: FALSE (and
 * no access) for one. */
BOOL TaskbarPin_RegistryValues(HKEY key, TaskbarPin_ValueIO *io)
{
    ZeroMemory(io, sizeof *io);
    if (!key || IsPredefinedKey(key)) return FALSE;
    io->context = key;
    io->read = ReadPinValue;
    io->write = WritePinValue;
    io->erase = ErasePinValue;
    return TRUE;
}

/* The pin list a value holds: S_FALSE (and no list) when the value is missing
 * or empty, an error when it is not a well-formed list. */
static HRESULT ListFromValue(const TaskbarPin_Value *value, BYTE **list, DWORD *len)
{
    *list = NULL;
    *len = 0;
    if (!value->exists) return S_FALSE;
    if (value->type != REG_BINARY) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (!value->bytes) return S_FALSE;
    if (!TaskbarPin_ValidateList(value->data, value->bytes)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    *list = (BYTE *)AllocateZeroed(value->bytes);
    if (!*list) return E_OUTOFMEMORY;
    memcpy(*list, value->data, value->bytes);
    *len = value->bytes;
    return S_OK;
}

HRESULT TaskbarPin_ReadList(HKEY key, BYTE **list, DWORD *len)
{
    const TaskbarPin_ValueIO io = { key, ReadPinValue, NULL, NULL };
    TaskbarPin_Value value;
    LSTATUS rc = SnapshotValue(&io, kPinValues[VALUE_PIN_LIST], &value);
    HRESULT hr = rc == ERROR_SUCCESS ? ListFromValue(&value, list, len) : HRESULT_FROM_WIN32(rc);
    if (rc != ERROR_SUCCESS) {
        *list = NULL;
        *len = 0;
    }
    FreeMemory(value.data);
    return hr;
}

/* The user's pin list (free it with FreeMemory); FALSE when there is none. */
static BOOL ReadPinList(BYTE **list, DWORD *len)
{
    HKEY key;
    BOOL found;
    *list = NULL;
    *len = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_TASKBAND, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return FALSE;
    found = TaskbarPin_ReadList(key, list, len) == S_OK;
    RegCloseKey(key);
    return found;
}

/* An entry of the taskbar's list names this AppUserModelID and its shortcut
 * still exists: the taskbar groups the windows carrying it with that pin. */
BOOL TaskbarPin_HasAppId(const WCHAR *aumid)
{
    BYTE *list;
    DWORD len;
    BOOL hit = ReadPinList(&list, &len) && TaskbarPin_ListHasAppId(list, len, aumid, TRUE);
    FreeMemory(list);
    return hit;
}

/* Only active entries count; a shortcut left in the folder is not a pin. */
BOOL TaskbarPin_IsPinned(const Profile *profile)
{
    BYTE *list;
    DWORD len;
    BOOL hit = ReadPinList(&list, &len) && TaskbarPin_ListHasProfile(list, len, profile);
    FreeMemory(list);
    return hit;
}

/* --------------------------------------------------------------- records */

/* The record the taskbar writes for an entry: a link to the pinned item
 * itself (its shortcut, or its Apps folder item), built from the entry alone. */
HRESULT TaskbarPin_BuildRecord(const BYTE *entry, size_t size, BYTE **data, size_t *len)
{
    PIDLIST_ABSOLUTE pidl = EntryPidl(entry, size);
    IShellLinkW *link = NULL;
    IShellLinkDataList *flags = NULL;
    IPersistStream *persist = NULL;
    IStream *stream = NULL;
    STATSTG stat;
    HGLOBAL memory = NULL;
    DWORD linkFlags = 0;
    void *bytes;
    HRESULT hr;
    *data = NULL;
    *len = 0;
    ZeroMemory(&stat, sizeof stat);
    if (!pidl) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IShellLinkDataList, (void **)&flags);
    if (SUCCEEDED(hr)) hr = IShellLinkDataList_GetFlags(flags, &linkFlags);
    if (SUCCEEDED(hr)) hr = IShellLinkDataList_SetFlags(flags, linkFlags | SLDF_ALLOW_LINK_TO_LINK);
    if (SUCCEEDED(hr)) hr = IShellLinkW_SetIDList(link, pidl);
    if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IPersistStream, (void **)&persist);
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = IPersistStream_Save(persist, stream, TRUE);
    if (SUCCEEDED(hr)) hr = IStream_Stat(stream, &stat, STATFLAG_NONAME);
    if (SUCCEEDED(hr) && (stat.cbSize.HighPart || !stat.cbSize.LowPart)) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream, &memory);
    if (SUCCEEDED(hr)) {
        bytes = GlobalLock(memory);
        if (!bytes) hr = E_FAIL;
        else {
            *data = (BYTE *)AllocateZeroed(stat.cbSize.LowPart);
            if (!*data) hr = E_OUTOFMEMORY;
            else {
                memcpy(*data, bytes, stat.cbSize.LowPart);
                *len = stat.cbSize.LowPart;
            }
            GlobalUnlock(memory);
        }
    }
    if (stream) IStream_Release(stream);
    if (persist) IPersistStream_Release(persist);
    if (flags) IShellLinkDataList_Release(flags);
    if (link) IShellLinkW_Release(link);
    ILFree(pidl);
    return hr;
}

/* A record in the taskbar's own form, made for this entry's ID list. */
static BOOL RecordFitsEntry(const BYTE *record, size_t recordSize, const BYTE *entry, size_t entrySize)
{
    const DWORD form = (DWORD)(SLDF_HAS_ID_LIST | SLDF_ALLOW_LINK_TO_LINK);
    PIDLIST_ABSOLUTE pidl;
    size_t listSize;
    BOOL fits;
    if (recordSize < LINK_HEADER_SIZE + 2 || Get32(record) != LINK_HEADER_SIZE || (Get32(record + LINK_FLAGS_OFFSET) & form) != form)
        return FALSE;
    listSize = Get16(record + LINK_HEADER_SIZE);
    pidl = EntryPidl(entry, entrySize);
    fits = pidl && LINK_HEADER_SIZE + 2 + listSize <= recordSize && ILGetSize(pidl) == listSize &&
           memcmp(record + LINK_HEADER_SIZE + 2, pidl, listSize) == 0;
    ILFree(pidl);
    return fits;
}

typedef struct RecordSlot {
    size_t      pos, size;      /* the entry in the new list */
    const BYTE *record;
    size_t      recordSize;
    BYTE       *built;
    BOOL        active, ours, buildFailed;
} RecordSlot;

static BOOL BuildSlotRecord(const BYTE *list, RecordSlot *slot, HRESULT *hr)
{
    BYTE *data = NULL;
    size_t size = 0;
    *hr = TaskbarPin_BuildRecord(list + slot->pos, slot->size, &data, &size);
    slot->buildFailed = FAILED(*hr);
    if (slot->buildFailed) return FALSE;
    FreeMemory(slot->built);
    slot->built = data;
    slot->record = data;
    slot->recordSize = size;
    return TRUE;
}

/* The records for `newList`: one per entry, in the same order. Each entry
 * keeps the record of the `oldList` entry it comes from (`dropped` flags the
 * old entries removed); records that do not cover every old entry count as
 * empty, as the taskbar reads them. The entry at `fresh` gets a new record,
 * and so does an active entry of ours whose record is empty or not made for
 * it; whenever the records are written, every active entry with an empty
 * record gets one. S_FALSE: the list is not `edited` and no pin of ours needs
 * a record. */
HRESULT TaskbarPin_PrepareRecords(const BYTE *oldList, size_t oldLen, const BYTE *newList, size_t newLen,
                                  const BOOL *dropped, size_t fresh, BOOL edited,
                                  const BYTE *records, size_t recordsLen, BYTE **out, size_t *outLen)
{
    size_t oldCount, count, kept, i, n, pos, recordPos = 0, oldIndex = 0, total = 0;
    const BYTE **oldRecord = NULL;
    size_t *oldSize = NULL;
    RecordSlot *slots = NULL;
    BOOL covered = TRUE, needed = edited;
    WCHAR appId[PIN_APPID_CCH];
    HRESULT hr = S_OK, buildResult;
    *out = NULL;
    *outLen = 0;
    if (!TaskbarPin_ValidateList(oldList, oldLen) || !TaskbarPin_ValidateList(newList, newLen))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    oldCount = CountEntries(oldList, oldLen);
    count = CountEntries(newList, newLen);
    kept = oldCount;
    for (i = 0; dropped && i < oldCount; i++) if (dropped[i]) kept--;
    if (count < kept || (fresh != TASKBAR_PIN_NONE && fresh >= count)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    oldRecord = (const BYTE **)AllocateZeroed((oldCount + 1) * sizeof *oldRecord);
    oldSize = (size_t *)AllocateZeroed((oldCount + 1) * sizeof *oldSize);
    slots = (RecordSlot *)AllocateZeroed((count + 1) * sizeof *slots);
    if (!oldRecord || !oldSize || !slots) {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < oldCount && covered; i++) {
        covered = records && recordsLen - recordPos >= 4 && Get32(records + recordPos) <= recordsLen - recordPos - 4;
        if (!covered) break;
        oldSize[i] = Get32(records + recordPos);
        oldRecord[i] = records + recordPos + 4;
        recordPos += 4 + oldSize[i];
    }
    if (!covered) ZeroMemory(oldSize, oldCount * sizeof *oldSize);

    for (i = 0, pos = 0; (n = EntrySize(newList, newLen, pos)) != 0; i++, pos += n) {
        RecordSlot *slot = &slots[i];
        BOOL removed;
        while (dropped && oldIndex < oldCount && dropped[oldIndex]) oldIndex++;
        if (oldIndex < oldCount) {
            slot->record = oldRecord[oldIndex];
            slot->recordSize = oldSize[oldIndex];
            oldIndex++;
        }
        slot->pos = pos;
        slot->size = n;
        ReadEntryState(newList + pos, n, &removed, appId, ARRAYSIZE(appId));
        slot->active = !removed;
        slot->ours = slot->active && IsOurAppId(appId);
        if (slot->ours && !covered) needed = TRUE;
        if (i == fresh || (slot->ours && (!slot->recordSize || !RecordFitsEntry(slot->record, slot->recordSize, newList + pos, n)))) {
            if (BuildSlotRecord(newList, slot, &buildResult)) needed = TRUE;
            else if (i == fresh) {
                hr = buildResult;
                goto done;
            }
        }
    }
    if (!needed) {
        hr = S_FALSE;
        goto done;
    }
    for (i = 0; i < count; i++) {
        if (slots[i].active && !slots[i].recordSize && !slots[i].buildFailed) BuildSlotRecord(newList, &slots[i], &buildResult);
        total += 4 + slots[i].recordSize;
    }
    if (total > MAXDWORD) {
        hr = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        goto done;
    }
    *out = (BYTE *)AllocateZeroed(total + 1);
    if (!*out) {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0, pos = 0; i < count; i++) {
        Put32(*out + pos, (DWORD)slots[i].recordSize);
        if (slots[i].recordSize) memcpy(*out + pos + 4, slots[i].record, slots[i].recordSize);
        pos += 4 + slots[i].recordSize;
    }
    *outLen = total;
done:
    for (i = 0; slots && i < count; i++) FreeMemory(slots[i].built);
    FreeMemory(slots);
    FreeMemory(oldSize);
    FreeMemory((void *)oldRecord);
    return hr;
}

/* ---------------------------------------------------------------- commit */

static void NoteRollback(TaskbarPin_CommitResult *result, LSTATUS rc)
{
    if (rc != ERROR_SUCCESS && result->rollbackError == ERROR_SUCCESS) result->rollbackError = rc;
}

static LSTATUS RestoreValue(const TaskbarPin_ValueIO *io, int index, const TaskbarPin_Value *value)
{
    LSTATUS rc = value->exists ? io->write(io->context, kPinValues[index], value->type, value->data, value->bytes)
                              : io->erase(io->context, kPinValues[index]);
    return rc == ERROR_FILE_NOT_FOUND && !value->exists ? ERROR_SUCCESS : rc;
}

static BOOL ValueMatches(const TaskbarPin_ValueIO *io, int index, const TaskbarPin_Value *expected)
{
    TaskbarPin_Value actual;
    BOOL same = SnapshotValue(io, kPinValues[index], &actual) == ERROR_SUCCESS && SameValue(&actual, expected);
    FreeMemory(actual.data);
    return same;
}

/* The caller holds the taskbar's lock; `base` is the list and its records as
 * the new values were prepared from: when either changed since, nothing is
 * written (E_CHANGED_STATE), nor over a list of another form. The records go
 * first and the change counter last, as the taskbar writes them; a failed
 * write restores the pair in dependency order. */
HRESULT TaskbarPin_CommitPrepared(const TaskbarPin_ValueIO *io, const TaskbarPin_Value *base,
                                  const BYTE *list, size_t listLen,
                                  const BYTE *records, size_t recordsLen, BOOL writeRecords, TaskbarPin_CommitResult *result)
{
    TaskbarPin_Value old[PIN_VALUE_COUNT];
    BOOL attempted[PIN_VALUE_COUNT] = { FALSE, FALSE, FALSE, FALSE }, recordsWritten = FALSE, listRestored = TRUE;
    DWORD version = PIN_LIST_VERSION, changes = 0;
    LSTATUS rc = ERROR_SUCCESS, restore;
    HRESULT hr = S_OK;
    int i;
    TaskbarPin_CommitResult local;
    if (!result) result = &local;
    ZeroMemory(result, sizeof *result);
    ZeroMemory(old, sizeof old);
    if (!io || !io->read || !io->write || !io->erase || !base || (!list && listLen) ||
        (!records && recordsLen) || listLen > MAXDWORD || recordsLen > MAXDWORD) return E_INVALIDARG;
    for (i = 0; i < PIN_VALUE_COUNT; i++) {
        rc = SnapshotValue(io, kPinValues[i], &old[i]);
        if (rc != ERROR_SUCCESS) goto done;
    }
    if (!SameValue(&old[VALUE_PIN_LIST], &base[VALUE_PIN_LIST]) || !SameValue(&old[VALUE_RECORDS], &base[VALUE_RECORDS])) {
        hr = E_CHANGED_STATE;
        goto done;
    }
    if ((old[VALUE_LIST_VERSION].exists && (!ValueNumber(&old[VALUE_LIST_VERSION], &version) || version != PIN_LIST_VERSION)) ||
        (old[VALUE_CHANGE_COUNT].exists && !ValueNumber(&old[VALUE_CHANGE_COUNT], &changes))) {
        rc = ERROR_INVALID_DATA;
        goto done;
    }
    changes++;
    if (writeRecords) {
        attempted[VALUE_RECORDS] = TRUE;
        rc = io->write(io->context, kPinValues[VALUE_RECORDS], REG_BINARY, records, (DWORD)recordsLen);
        if (rc != ERROR_SUCCESS) goto rollback;
        recordsWritten = TRUE;
    }
    if (list) {
        attempted[VALUE_PIN_LIST] = TRUE;
        rc = io->write(io->context, kPinValues[VALUE_PIN_LIST], REG_BINARY, list, (DWORD)listLen);
        if (rc != ERROR_SUCCESS) goto rollback;
        result->listCommitted = TRUE;
    }
    attempted[VALUE_LIST_VERSION] = TRUE;
    rc = io->write(io->context, kPinValues[VALUE_LIST_VERSION], REG_DWORD, (const BYTE *)&version, sizeof version);
    if (rc != ERROR_SUCCESS) goto rollback;
    attempted[VALUE_CHANGE_COUNT] = TRUE;
    rc = io->write(io->context, kPinValues[VALUE_CHANGE_COUNT], REG_DWORD, (const BYTE *)&changes, sizeof changes);
    if (rc == ERROR_SUCCESS) goto done;
rollback:
    for (i = VALUE_CHANGE_COUNT; i >= VALUE_LIST_VERSION; i--) if (attempted[i]) NoteRollback(result, RestoreValue(io, i, &old[i]));
    if (attempted[VALUE_PIN_LIST]) {
        restore = RestoreValue(io, VALUE_PIN_LIST, &old[VALUE_PIN_LIST]);
        NoteRollback(result, restore);
        listRestored = restore == ERROR_SUCCESS || ValueMatches(io, VALUE_PIN_LIST, &old[VALUE_PIN_LIST]);
        result->listCommitted = !listRestored;
    }
    if (attempted[VALUE_RECORDS] && listRestored) {
        restore = RestoreValue(io, VALUE_RECORDS, &old[VALUE_RECORDS]);
        NoteRollback(result, restore);
        if (restore != ERROR_SUCCESS && recordsWritten && list) {
            TaskbarPin_Value prepared;
            prepared.exists = TRUE;
            prepared.type = REG_BINARY;
            prepared.bytes = (DWORD)recordsLen;
            prepared.data = (BYTE *)records;
            /* The new records stayed: the new list goes back with them, and a
             * newly referenced shortcut must remain on disk. */
            if (ValueMatches(io, VALUE_RECORDS, &prepared)) {
                restore = io->write(io->context, kPinValues[VALUE_PIN_LIST], REG_BINARY, list, (DWORD)listLen);
                NoteRollback(result, restore);
                result->listCommitted = restore == ERROR_SUCCESS;
            }
        }
    }
    result->uncertain = result->rollbackError != ERROR_SUCCESS;
done:
    for (i = 0; i < PIN_VALUE_COUNT; i++) FreeMemory(old[i].data);
    return hr == S_OK ? HRESULT_FROM_WIN32(rc) : hr;
}

/* ---------------------------------------------------------------- edits */

/* Changes the pin list the way the taskbar does. An edit gets the list and
 * returns the new one (allocated with AllocateZeroed) or NULL to leave it as
 * it is, `hr` saying why; it flags in `dropped` (one per entry) the entries it
 * removed, and in `fresh` the entry it added or replaced. */
typedef BYTE *(*PinListEdit)(const BYTE *list, size_t len, size_t *newLen, BOOL *dropped, size_t *fresh,
                             void *context, HRESULT *hr);

/* One read, edit and write of the pin list. */
static HRESULT EditPinListOnce(const TaskbarPin_ValueIO *io, PinListEdit edit, void *context, TaskbarPin_CommitResult *result)
{
    TaskbarPin_Value base[2];
    BYTE *list = NULL, *changed = NULL, *records = NULL;
    BOOL *dropped = NULL, recordsChanged;
    DWORD len = 0;
    size_t newLen = 0, recordsLen = 0, fresh = TASKBAR_PIN_NONE;
    LSTATUS rc;
    HRESULT hr;

    ZeroMemory(base, sizeof base);
    rc = SnapshotValue(io, kPinValues[VALUE_PIN_LIST], &base[VALUE_PIN_LIST]);
    if (rc == ERROR_SUCCESS) rc = SnapshotValue(io, kPinValues[VALUE_RECORDS], &base[VALUE_RECORDS]);
    hr = HRESULT_FROM_WIN32(rc);
    if (SUCCEEDED(hr) && base[VALUE_RECORDS].exists && base[VALUE_RECORDS].type != REG_BINARY) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (SUCCEEDED(hr)) hr = ListFromValue(&base[VALUE_PIN_LIST], &list, &len);
    if (FAILED(hr)) goto done;
    if (!list) {
        list = (BYTE *)AllocateZeroed(1);
        if (!list) {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        list[0] = LIST_END;
        len = 1;
    }
    dropped = (BOOL *)AllocateZeroed((CountEntries(list, len) + 1) * sizeof *dropped);
    if (!dropped) {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    hr = S_FALSE;
    changed = edit(list, len, &newLen, dropped, &fresh, context, &hr);
    if (!changed && FAILED(hr)) goto done;
    if (changed && newLen == len && memcmp(changed, list, len) == 0) {
        FreeMemory(changed);
        changed = NULL;
    }
    hr = TaskbarPin_PrepareRecords(list, len, changed ? changed : list, changed ? newLen : len, dropped, fresh,
                                   changed != NULL, base[VALUE_RECORDS].exists ? base[VALUE_RECORDS].data : NULL,
                                   base[VALUE_RECORDS].bytes, &records, &recordsLen);
    if (hr != S_OK) goto done;
    recordsChanged = !base[VALUE_RECORDS].exists || recordsLen != base[VALUE_RECORDS].bytes ||
                     (recordsLen && memcmp(records, base[VALUE_RECORDS].data, recordsLen) != 0);
    if (!changed && !recordsChanged) {
        hr = S_FALSE;
        goto done;
    }
    hr = TaskbarPin_CommitPrepared(io, base, changed, changed ? newLen : 0, records, recordsLen, recordsChanged, result);
done:
    FreeMemory(base[VALUE_PIN_LIST].data);
    FreeMemory(base[VALUE_RECORDS].data);
    FreeMemory(list);
    FreeMemory(changed);
    FreeMemory(records);
    FreeMemory(dropped);
    return hr;
}

/* The pin list edited through `io`. An edit that finds the list changed when
 * it writes starts over from the new list. */
static HRESULT EditPinList(const TaskbarPin_ValueIO *io, PinListEdit edit, void *context, TaskbarPin_CommitResult *result)
{
    TaskbarPin_CommitResult local;
    HRESULT hr = E_CHANGED_STATE;
    int attempt;
    if (!result) result = &local;
    ZeroMemory(result, sizeof *result);
    if (!io || !io->read || !io->write || !io->erase) return E_INVALIDARG;
    for (attempt = 0; attempt < EDIT_ATTEMPTS && hr == E_CHANGED_STATE; attempt++) hr = EditPinListOnce(io, edit, context, result);
    if (result->rollbackError != ERROR_SUCCESS)
        Util_Log(L"could not restore all taskbar pin values (error %ld)", result->rollbackError);
    return hr;
}

/* The user's pin list edited under the taskbar's lock. Explorer holds it only
 * while it changes the list itself, so the wait is short. */
static HRESULT EditTaskbarPinList(PinListEdit edit, void *context, TaskbarPin_CommitResult *result)
{
    TaskbarPin_ValueIO io;
    HANDLE lock;
    HKEY key = NULL;
    DWORD wait;
    LSTATUS rc;
    HRESULT hr;

    if (result) ZeroMemory(result, sizeof *result);
    lock = CreateMutexExW(NULL, PIN_LIST_LOCK, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    if (!lock) return HRESULT_FROM_WIN32(GetLastError());
    wait = WaitForSingleObject(lock, PIN_LIST_LOCK_WAIT_MS);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        hr = HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
        CloseHandle(lock);
        return hr;
    }
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, REG_TASKBAND, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        hr = HRESULT_FROM_WIN32(rc);
    } else {
        hr = TaskbarPin_RegistryValues(key, &io) ? EditPinList(&io, edit, context, result) : E_INVALIDARG;
        RegCloseKey(key);
    }
    ReleaseMutex(lock);
    CloseHandle(lock);
    return hr;
}

/* `entry`, the pin of shortcut `lnk` with AppID `aumid`, merged into `list`:
 * it takes the place of the active entry with that AppID or that shortcut,
 * else of a retired entry with that AppID, else goes last. Other active
 * entries with that AppID or shortcut are dropped: Windows would unpin them,
 * deleting the shortcut they share. */
HRESULT TaskbarPin_MergeEntry(const BYTE *list, size_t len, const BYTE *entry, size_t entrySize,
                              const WCHAR *lnk, const WCHAR *aumid, BOOL *dropped,
                              BYTE *out, size_t cap, size_t *outLen, TaskbarPin_Merge *merge)
{
    enum { OTHER, PIN, HISTORY };
    size_t pos, n, index, count, lastCb = 0, target = TASKBAR_PIN_NONE, retired = TASKBAR_PIN_NONE, size, outPos;
    size_t written = 0, rank = 0;
    WCHAR found[PIN_APPID_CCH], path[MAX_PATH];
    BYTE *kind;
    BOOL removed, mine;
    *outLen = 0;
    if (!TaskbarPin_ValidateList(list, len) || !aumid || !LastIdListItem(entry, entrySize, &lastCb) ||
        !ParseEntryState(entry, entrySize, &removed, found, ARRAYSIZE(found)) || removed)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    count = CountEntries(list, len);
    kind = (BYTE *)AllocateZeroed(count + 1);
    if (!kind) return E_OUTOFMEMORY;
    for (pos = 0, index = 0; (n = EntrySize(list, len, pos)) != 0; pos += n, index++) {
        ReadEntryState(list + pos, n, &removed, found, ARRAYSIZE(found));
        mine = SameAppId(found, aumid) ||
               (!removed && lnk && EntryPath(list + pos, n, path) && Core_PathEquals(path, lnk));
        kind[index] = !mine ? OTHER : removed ? HISTORY : PIN;
        if (kind[index] == PIN && target == TASKBAR_PIN_NONE) target = index;
        if (kind[index] == HISTORY && retired == TASKBAR_PIN_NONE) retired = index;
    }
    if (merge) merge->replacedPin = target != TASKBAR_PIN_NONE;
    if (target == TASKBAR_PIN_NONE) target = retired;
    for (size = entrySize + 1, pos = 0, index = 0; (n = EntrySize(list, len, pos)) != 0; pos += n, index++)
        if (kind[index] == OTHER || (kind[index] == HISTORY && index != target)) size += n;
    if (size > cap) {
        FreeMemory(kind);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    for (outPos = 0, pos = 0, index = 0; (n = EntrySize(list, len, pos)) != 0; pos += n, index++) {
        if (index == target) {
            rank = written++;
            memcpy(out + outPos, entry, entrySize);
            outPos += entrySize;
        } else if (kind[index] == PIN) {
            if (dropped) dropped[index] = TRUE;
        } else {
            written++;
            memcpy(out + outPos, list + pos, n);
            outPos += n;
        }
    }
    if (target == TASKBAR_PIN_NONE) {
        rank = written;
        memcpy(out + outPos, entry, entrySize);
        outPos += entrySize;
    }
    out[outPos++] = LIST_END;
    *outLen = outPos;
    if (merge) merge->rank = rank;
    FreeMemory(kind);
    return S_OK;
}

static BYTE *AddPinEdit(const BYTE *list, size_t len, size_t *newLen, BOOL *dropped, size_t *fresh, void *context, HRESULT *hr)
{
    TaskbarPin_Addition *addition = (TaskbarPin_Addition *)context;
    TaskbarPin_Merge merge;
    WCHAR path[MAX_PATH];
    size_t pos, n, index;
    BYTE *merged = (BYTE *)AllocateZeroed(len + addition->entrySize);
    if (!merged) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    *hr = TaskbarPin_MergeEntry(list, len, addition->entry, addition->entrySize, addition->shortcut, addition->aumid, dropped,
                                merged, len + addition->entrySize, newLen, &merge);
    if (FAILED(*hr)) {
        FreeMemory(merged);
        return NULL;
    }
    *fresh = merge.rank;
    addition->replacedPin = merge.replacedPin;
    addition->obsoleteShortcutCount = 0;
    /* The profile's other entries left the list: their shortcuts in the pins
     * folder would only linger. */
    for (pos = 0, index = 0; (n = EntrySize(list, len, pos)) != 0; pos += n, index++)
        if (dropped[index] && addition->obsoleteShortcutCount < ARRAYSIZE(addition->obsoleteShortcuts) &&
            addition->pinsDir && EntryPath(list + pos, n, path) && !Core_PathEquals(path, addition->shortcut) &&
            Core_PathUnder(path, addition->pinsDir) &&
            SUCCEEDED(StringCchCopyW(addition->obsoleteShortcuts[addition->obsoleteShortcutCount], MAX_PATH, path)))
            addition->obsoleteShortcutCount++;
    return merged;
}

/* Our entries (their ID list names an AppUserModelID of ours) repaired. */
static BYTE *RepairPinsEdit(const BYTE *list, size_t len, size_t *newLen, BOOL *dropped, size_t *fresh, void *context, HRESULT *hr)
{
    BYTE *out, *repaired;
    size_t pos = 0, outPos = 0, n, repairedSize = 0;
    BOOL changed = FALSE, removed;
    WCHAR appId[PIN_APPID_CCH];
    (void)dropped;
    (void)fresh;
    (void)context;
    out = (BYTE *)AllocateZeroed(len + 1);
    if (!out) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    while ((n = EntrySize(list, len, pos)) != 0) {
        repaired = ParseEntryState(list + pos, n, &removed, appId, ARRAYSIZE(appId)) && !removed && IsOurAppId(appId) ?
                   RepairEntry(list + pos, n, &repairedSize) : NULL;
        if (repaired && repairedSize <= n) {         /* a repair only ever removes bytes */
            memcpy(out + outPos, repaired, repairedSize);
            outPos += repairedSize;
            changed = TRUE;
        } else {
            memcpy(out + outPos, list + pos, n);
            outPos += n;
        }
        FreeMemory(repaired);
        pos += n;
    }
    out[outPos++] = LIST_END;
    if (!changed) {
        FreeMemory(out);
        *hr = S_FALSE;
        return NULL;
    }
    *newLen = outPos;
    return out;
}

/* Our active entries removed: their ID list names an AppUserModelID of ours
 * (pins made from a profile's taskbar button carry it too), or their file is
 * one of our pins' shortcuts (`context`, a PathList). Retired entries stay,
 * as Windows keeps them. */
static BYTE *RemovePinsEdit(const BYTE *list, size_t len, size_t *newLen, BOOL *dropped, size_t *fresh, void *context, HRESULT *hr)
{
    const PathList *ours = (const PathList *)context;
    BYTE *out;
    size_t pos = 0, outPos = 0, n, index = 0;
    BOOL changed = FALSE, removed, ourEntry;
    WCHAR appId[PIN_APPID_CCH], path[MAX_PATH];
    DWORD i;
    (void)fresh;
    out = (BYTE *)AllocateZeroed(len + 1);
    if (!out) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    while ((n = EntrySize(list, len, pos)) != 0) {
        ReadEntryState(list + pos, n, &removed, appId, ARRAYSIZE(appId));
        ourEntry = !removed && IsOurAppId(appId);
        if (!removed && !ourEntry && EntryPath(list + pos, n, path))
            for (i = 0; i < ours->count && !ourEntry; i++) ourEntry = Core_PathEquals(path, ours->items[i]);
        if (ourEntry) {
            dropped[index] = TRUE;
            changed = TRUE;
        } else {
            memcpy(out + outPos, list + pos, n);
            outPos += n;
        }
        pos += n;
        index++;
    }
    out[outPos++] = LIST_END;
    if (!changed) {
        FreeMemory(out);
        *hr = S_FALSE;
        return NULL;
    }
    *newLen = outPos;
    return out;
}

HRESULT TaskbarPin_AddToList(const TaskbarPin_ValueIO *io, TaskbarPin_Addition *addition, TaskbarPin_CommitResult *result)
{
    return EditPinList(io, AddPinEdit, addition, result);
}

HRESULT TaskbarPin_RepairList(const TaskbarPin_ValueIO *io)
{
    return EditPinList(io, RepairPinsEdit, NULL, NULL);
}

HRESULT TaskbarPin_RemoveFromList(const TaskbarPin_ValueIO *io, WCHAR (*shortcuts)[MAX_PATH], DWORD shortcutCount)
{
    PathList ours;
    ours.items = shortcuts;
    ours.count = ours.capacity = shortcutCount;
    return EditPinList(io, RemovePinsEdit, &ours, NULL);
}

static void TellTaskbar(void)
{
    HWND tray = FindWindowW(TASKBAR_WINDOW_CLASS, NULL);
    HWND rebar = tray ? FindWindowExW(tray, NULL, L"ReBarWindow32", NULL) : NULL;
    HWND band = rebar ? FindWindowExW(rebar, NULL, L"MSTaskSwWClass", NULL) : NULL;
    if (band) PostMessageW(band, WM_PINS_CHANGED, 0, 0);
}

/* The taskbar takes a .lnk's new content: a fresh ID list, then its icon. */
void TaskbarPin_TellShortcutChanged(const WCHAR *lnk)
{
    SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSH, lnk, lnk);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, lnk, NULL);
}

/* Our entries and their records repaired. */
void TaskbarPin_RepairOurs(void)
{
    HRESULT hr = EditTaskbarPinList(RepairPinsEdit, NULL, NULL);
    if (hr == S_OK) {
        TellTaskbar();
        Util_Log(L"repaired taskbar pin entries");
    } else if (FAILED(hr)) {
        Util_Log(L"could not repair taskbar pin entries (0x%08lX)", (unsigned long)hr);
    }
}

/* ------------------------------------------------------------------ pins */

/* Our pins' shortcuts that open `folder` (target
 * ClaudeDesktopProfilesManager.exe --launch "<folder>"), or with NULL all
 * those that run ClaudeDesktopProfilesManager.exe (free `ours->items`). FALSE
 * when the pins could not all be listed. */
static BOOL OurPinShortcuts(const WCHAR *folder, PathList *ours)
{
    LinkInfo info;
    DWORD i, kept = 0;
    BOOL complete = PinnedShortcuts(ours);
    for (i = 0; i < ours->count; i++) {
        if (!Shortcut_Read(ours->items[i], &info) || !Core_IsOurExe(info.target) || (folder && !Core_ArgsSelectProfile(info.args, folder)))
            continue;
        if (kept != i) StringCchCopyW(ours->items[kept], MAX_PATH, ours->items[i]);
        kept++;
    }
    ours->count = kept;
    return complete;
}

static void DeletePinShortcut(const WCHAR *path)
{
    if (!DeleteFileW(path)) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) Util_Log(L"could not remove taskbar pin %s (error %lu)", path, error);
        return;
    }
    SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, NULL);
    Util_Log(L"removed taskbar pin %s", path);
}

/* At uninstall: our entries leave the list, then our shortcuts go. */
void TaskbarPin_RemoveOurs(void)
{
    PathList ours;
    DWORD i;
    HRESULT hr;
    if (!OurPinShortcuts(NULL, &ours)) Util_Log(L"could not list every taskbar pin");
    hr = EditTaskbarPinList(RemovePinsEdit, &ours, NULL);
    if (hr == S_OK) {
        TellTaskbar();
        Util_Log(L"removed our taskbar pins");
    } else if (FAILED(hr)) {
        Util_Log(L"could not remove our taskbar pins (0x%08lX)", (unsigned long)hr);
        FreeMemory(ours.items);
        return;
    }
    for (i = 0; i < ours.count; i++) DeletePinShortcut(ours.items[i]);
    FreeMemory(ours.items);
}

/* A pin's shortcut gets the profile's new icon, description and name. TRUE
 * when it changed. */
static BOOL RefreshPin(const WCHAR *pin, const Profile *before, const Profile *after, const WCHAR *icon)
{
    WCHAR target[MAX_PATH];
    const WCHAR *path = pin;
    BOOL renamed = FALSE;
    HRESULT hr;
    if (Shortcut_RenamedPath(path, before, after, target, ARRAYSIZE(target))) {
        if (MoveFileW(path, target)) {
            SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSH, path, target);
            Util_Log(L"renamed taskbar pin %s to %s", path, target);
            path = target;
            renamed = TRUE;
        } else {
            Util_Log(L"could not rename taskbar pin %s to %s (error %lu)", path, target, GetLastError());
        }
    }
    hr = Shortcut_Update(path, after, icon);
    if (renamed) SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, path, NULL);
    else if (hr == S_OK) TaskbarPin_TellShortcutChanged(path);
    if (hr == S_OK) Util_Log(L"updated taskbar pin %s", path);
    else if (FAILED(hr)) Util_Log(L"could not update taskbar pin %s (0x%08lX)", path, (unsigned long)hr);
    return renamed || hr == S_OK;
}

/* The pins of `before` get the profile's new icon, description and name.
 * TRUE when one of them changed. */
static BOOL RefreshPinShortcuts(const Profile *before, const Profile *after, const WCHAR *icon)
{
    PathList pins;
    DWORD i;
    BOOL anyChanged = FALSE;
    if (!OurPinShortcuts(before->folder, &pins)) Util_Log(L"could not list every taskbar pin to update those of %s", before->folder);
    for (i = 0; i < pins.count; i++) anyChanged = RefreshPin(pins.items[i], before, after, icon) || anyChanged;
    FreeMemory(pins.items);
    return anyChanged;
}

/* After a rename or a color change: the profile's pins get the new icon,
 * description and name. */
void TaskbarPin_Refresh(const Profile *before, const Profile *after, const WCHAR *icon)
{
    /* Explorer saves a changed pin's entry again: its record must follow it. */
    if (RefreshPinShortcuts(before, after, icon)) TaskbarPin_RepairOurs();
}

/* After a change of language: every profile's pins get their description in
 * it, the pins listed and their entries repaired once for all profiles
 * (`icons[i]`: profile i's icon, NULL to leave its pins as they are). */
void TaskbarPin_RefreshProfiles(const ProfileList *list, const WCHAR *const *icons)
{
    PathList pins;
    LinkInfo info;
    DWORD i;
    int p;
    BOOL anyChanged = FALSE;
    if (!OurPinShortcuts(NULL, &pins)) Util_Log(L"could not list every taskbar pin");
    for (i = 0; i < pins.count; i++) {
        if (!Shortcut_Read(pins.items[i], &info)) continue;
        for (p = 0; p < list->count; p++) {
            const Profile *profile = &list->items[p];
            if (!icons[p] || !Core_ArgsSelectProfile(info.args, profile->folder)) continue;
            anyChanged = RefreshPin(pins.items[i], profile, profile, icons[p]) || anyChanged;
            break;
        }
    }
    FreeMemory(pins.items);
    if (anyChanged) TaskbarPin_RepairOurs();
}

/* The file the pin uses: a free "Claude (<name>).lnk" in the pins folder, or
 * an existing one that already opens this profile (`*exists` then). */
static BOOL ChoosePinShortcut(const WCHAR *dir, const Profile *profile, WCHAR *out, size_t cch, BOOL *exists)
{
    WCHAR name[MAX_PATH];
    int copyNumber;
    for (copyNumber = 1; copyNumber <= MAX_LINK_COPIES; copyNumber++) {
        Core_ShortcutFileName(profile->name, copyNumber, name, ARRAYSIZE(name));
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

/* A shortcut left in the pins folder for the profile, brought up to date
 * before it is pinned again: saved in place when it already starts the
 * installed copy with the profile's ID, written again otherwise. */
HRESULT TaskbarPin_UpdateLeftover(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk)
{
    WCHAR icon[MAX_PATH], installed[MAX_PATH], aumid[AUMID_CCH];
    LinkInfo info;
    Core_ProfileAumid(profile->folder, aumid, ARRAYSIZE(aumid));
    if (Shortcut_Read(lnk, &info) && Util_InstallExe(installed, ARRAYSIZE(installed)) && Core_PathEquals(info.target, installed) &&
        Shortcut_HasAppId(lnk, aumid) && Icons_Ensure(pkg, profile, icon, ARRAYSIZE(icon)))
        return Shortcut_Update(lnk, profile, icon);
    return Shortcut_WriteForProfile(pkg, profile, lnk);
}

/* Pins the profile. Already pinned: its pin is brought up to date instead
 * (S_FALSE). */
HRESULT TaskbarPin_Pin(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR dir[MAX_PATH], lnk[MAX_PATH], aumid[AUMID_CCH], icon[MAX_PATH];
    TaskbarPin_Addition *addition;
    TaskbarPin_CommitResult committed;
    BYTE *entry = NULL;
    DWORD i;
    BOOL exists = FALSE;
    HRESULT hr;

    if (TaskbarPin_IsPinned(profile)) {
        if (Icons_Ensure(pkg, profile, icon, ARRAYSIZE(icon))) RefreshPinShortcuts(profile, profile, icon);
        TaskbarPin_RepairOurs();
        return S_FALSE;
    }
    if (!TaskbarPin_Dir(dir, ARRAYSIZE(dir)) || !Util_DirExists(dir) || !Util_RegKeyExists(HKEY_CURRENT_USER, REG_TASKBAND)) {
        Util_Log(L"could not pin %s to the taskbar: no pins folder or pin list", profile->folder);
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    }
    if (!ChoosePinShortcut(dir, profile, lnk, ARRAYSIZE(lnk), &exists)) {
        Util_Log(L"could not pin %s to the taskbar: every shortcut name for it is taken", profile->folder);
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    }
    addition = (TaskbarPin_Addition *)AllocateZeroed(sizeof *addition);
    if (!addition) {
        Util_Log(L"could not pin %s to the taskbar: not enough memory", profile->folder);
        return E_OUTOFMEMORY;
    }
    Core_ProfileAumid(profile->folder, aumid, ARRAYSIZE(aumid));
    ZeroMemory(&committed, sizeof committed);
    hr = exists ? TaskbarPin_UpdateLeftover(pkg, profile, lnk) : Shortcut_WriteForProfile(pkg, profile, lnk);
    if (SUCCEEDED(hr)) hr = BuildEntry(lnk, aumid, &entry, &addition->entrySize);
    if (SUCCEEDED(hr)) {
        addition->entry = entry;
        addition->shortcut = lnk;
        addition->aumid = aumid;
        addition->pinsDir = dir;
        hr = EditTaskbarPinList(AddPinEdit, addition, &committed);
    }
    FreeMemory(entry);
    if (FAILED(hr)) {
        if (!exists && !committed.listCommitted && !committed.uncertain) DeleteFileW(lnk);
        if (committed.listCommitted || committed.uncertain) TellTaskbar();
        Util_Log(L"could not pin %s to the taskbar (0x%08lX)", profile->folder, (unsigned long)hr);
        FreeMemory(addition);
        return hr;
    }
    if (hr == S_OK) {
        TellTaskbar();
        for (i = 0; i < addition->obsoleteShortcutCount; i++) DeletePinShortcut(addition->obsoleteShortcuts[i]);
    }
    if (addition->replacedPin) TaskbarPin_TellShortcutChanged(lnk);
    Util_Log(L"pinned %s to the taskbar (%s)", profile->folder, lnk);
    FreeMemory(addition);
    return S_OK;
}
