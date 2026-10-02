/*
 * Unit tests for src/taskbar-pin.c: the pin list's entries, their records,
 * the edits of the list and their commit, on private values and files only:
 * the user's pin list, pins folder and Explorer are never touched. Built and
 * run by build.cmd (linked with the program's objects); exits non-zero when a
 * check fails.
 */
#include "../src/app.h"
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>

/* A record in the taskbar's own form: a link holding an ID list, which may point to a link. */
#define WINDOWS_RECORD_FLAGS ((DWORD)(SLDF_HAS_ID_LIST | SLDF_ALLOW_LINK_TO_LINK))
#define LIST_VERSION          3
#define FIXTURE_CAPACITY      16384
#define FIXTURE_FOLDER        L"Claude-PinFixture"
#define OTHER_FIXTURE_FOLDER  L"Claude-OtherFixture"

static int g_failures, g_checks;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

/* A step the checks after it depend on: its failure counts and is reported. */
static BOOL Prepared(const char *step, BOOL ok)
{
    if (!ok) {
        g_failures++;
        printf("  FAIL  setup: %s\n", step);
    }
    return ok;
}

static WORD Get16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD Get32(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }
static void Put16(BYTE *p, WORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void Put32(BYTE *p, DWORD v) { Put16(p, (WORD)v); Put16(p + 2, (WORD)(v >> 16)); }

static void FreeBytes(BYTE **bytes)
{
    if (*bytes) HeapFree(GetProcessHeap(), 0, *bytes);
    *bytes = NULL;
}

/* Bytes laid one after the other in a fixed buffer. */
typedef struct Bytes {
    BYTE  *data;
    size_t cap, len;
    BOOL   overflow;
} Bytes;

static void StartBytes(Bytes *bytes, BYTE *storage, size_t cap)
{
    bytes->data = storage;
    bytes->cap = cap;
    bytes->len = 0;
    bytes->overflow = FALSE;
}

static void Append(Bytes *bytes, const void *data, size_t size)
{
    if (bytes->overflow || size > bytes->cap - bytes->len) {
        bytes->overflow = TRUE;
        return;
    }
    if (size) memcpy(bytes->data + bytes->len, data, size);
    bytes->len += size;
}

/* A record, its size first. */
static void AppendRecord(Bytes *bytes, const BYTE *record, size_t size)
{
    BYTE header[4];
    Put32(header, (DWORD)size);
    Append(bytes, header, sizeof header);
    Append(bytes, record, size);
}

static void AppendListEnd(Bytes *bytes)
{
    static const BYTE end = 0xFF;
    Append(bytes, &end, 1);
}

/* ------------------------------------------------------- items and blocks */

/* A file item with one block. */
static size_t FileItem(BYTE *out)
{
    ZeroMemory(out, 86);
    Put16(out, 86);
    out[2] = 0x32;
    Put16(out + 22, 64);
    Put16(out + 24, 9);
    Put32(out + 26, 0xBEEF0004u);
    Put16(out + 84, 22);
    return 86;
}

/* A block of `tag` holding `text`, closed when `withOffset`. */
static size_t Block(BYTE *out, DWORD tag, const WCHAR *text, BOOL withOffset)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR), size = 10 + bytes + (withOffset ? 2 : 0);
    Put16(out, (WORD)size);
    Put16(out + 2, 0);
    Put32(out + 4, tag);
    Put16(out + 8, 2);
    memcpy(out + 10, text, bytes);
    if (withOffset) Put16(out + size - 2, 22);
    return size;
}

/* The blocks' tags, low words, in order; -1 when they do not end exactly at
 * the item's end. */
static int Chain(const BYTE *item, WORD *tags, int max)
{
    size_t cb = Get16(item), pos = Get16(item + cb - 2);
    int n = 0;
    while (pos + 8 <= cb && Get16(item + pos) >= 8 && pos + Get16(item + pos) <= cb &&
           Get16(item + pos + 6) == 0xBEEF && n < max) {
        tags[n++] = Get16(item + pos + 4);
        pos += Get16(item + pos);
    }
    return pos == cb ? n : -1;
}

static void TestInject(void)
{
    BYTE item[512], out[512];
    WORD tags[8];
    size_t cb = FileItem(item), n;
    const WCHAR *id = L"ClaudeDesktopProfilesManager.Profile.1234ABCD";

    n = TaskbarPin_InjectAppId(item, cb, id, out, sizeof out);
    Check("inject: size", n == cb + 12 + (wcslen(id) + 1) * 2 && Get16(out) == n);
    Check("inject: blocks 0004, 001D", Chain(out, tags, 8) == 2 && tags[0] == 0x0004 && tags[1] == 0x001D);
    Check("inject: the item still closes on its first block", Get16(out + n - 2) == 22);
    Check("inject: the app ID", memcmp(out + cb + 10, id, (wcslen(id) + 1) * 2) == 0);
    Check("inject: a well-formed result needs no repair", TaskbarPin_RepairItem(out, n, item, sizeof item) == 0);
    Check("inject: too small a buffer fails", TaskbarPin_InjectAppId(item, cb, id, out, 100) == 0);
}

static void TestRepair(void)
{
    BYTE item[1024], out[1024];
    WORD tags[16];
    size_t cb, n, i;
    const WCHAR *id = L"C:\\Windows\\System32\\winver.exe";

    /* The AppID block written short, its last WORD after it. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, FALSE);
    Put16(item + cb, 22);
    cb += 2;
    Put16(item, (WORD)cb);
    n = TaskbarPin_RepairItem(item, cb, out, sizeof out);
    Check("stray WORD at the end: repaired, same size", n == cb);
    Check("stray WORD at the end: blocks 0004, 001D", n && Chain(out, tags, 16) == 2 && tags[1] == 0x001D);

    /* ...then three blocks Windows appended after the WORD. */
    for (i = 0; i < 3; i++) cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    Put16(item, (WORD)cb);
    Check("broken blocks as Windows reads them", Chain(item, tags, 16) == -1);
    n = TaskbarPin_RepairItem(item, cb, out, sizeof out);
    Check("duplicates dropped", n == cb - 2 * 34);
    Check("repaired blocks 0004, 001D, 001E", n && Chain(out, tags, 16) == 3 && tags[0] == 0x0004 && tags[1] == 0x001D && tags[2] == 0x001E);
    Check("app ID kept", n && memcmp(out + 86 + 10, id, (wcslen(id) + 1) * 2) == 0);
    Check("repaired item size field", n && Get16(out) == n);
    Check("a repaired item needs no more repair", n && TaskbarPin_RepairItem(out, n, item, sizeof item) == 0);

    /* Whole blocks are left as they are, even with a repeated one. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    Put16(item, (WORD)cb);
    Check("whole blocks with a repeated one untouched", TaskbarPin_RepairItem(item, cb, out, sizeof out) == 0);

    /* Broken blocks: only those repeated byte for byte are dropped. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, FALSE);
    Put16(item + cb, 22);
    cb += 2;
    cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    cb += Block(item + cb, 0xBEEF001Eu, L"OEMPinned", TRUE);
    cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    Put16(item, (WORD)cb);
    n = TaskbarPin_RepairItem(item, cb, out, sizeof out);
    Check("broken blocks: identical ones kept once, different ones kept", n == cb - 34 && Chain(out, tags, 16) == 4);

    /* Well-formed: nothing to do. Unknown form: left alone. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    Put16(item, (WORD)cb);
    Check("well-formed item untouched", TaskbarPin_RepairItem(item, cb, out, sizeof out) == 0);
    Put16(item + 86 + 6, 0x1234);
    Check("unknown form untouched", TaskbarPin_RepairItem(item, cb, out, sizeof out) == 0);
    Check("tiny item untouched", TaskbarPin_RepairItem(item, 6, out, sizeof out) == 0);
}

/* ---------------------------------------------------------------- entries */

/* Appends a record of `n` bytes filled with `fill`. */
static size_t Record(BYTE *out, DWORD n, BYTE fill)
{
    Put32(out, n);
    memset(out + 4, fill, n);
    return 4 + n;
}

/* An entry holding `item` as its ID list. */
static size_t Entry(BYTE *out, const BYTE *item, size_t cb)
{
    out[0] = 0;
    Put32(out + 1, (DWORD)(cb + 2));
    memcpy(out + 5, item, cb);
    Put16(out + 5 + cb, 0);
    return cb + 7;
}

/* The item retired, as Windows marks an entry it unpinned. */
static size_t MarkRemoved(BYTE *item, size_t cb)
{
    WORD first = Get16(item + cb - 2);
    ZeroMemory(item + cb, 20);
    Put16(item + cb, 20);
    Put32(item + cb + 4, 0xBEEF002Cu);
    Put16(item + cb + 18, first);
    Put16(item, (WORD)(cb + 20));
    return cb + 20;
}

/* An entry of another application holding `appId`, retired when `removed`. */
static size_t OtherEntry(BYTE *out, const WCHAR *appId, BOOL removed)
{
    BYTE item[512];
    size_t cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, appId, TRUE);
    Put16(item, (WORD)cb);
    if (removed) cb = MarkRemoved(item, cb);
    return Entry(out, item, cb);
}

/* The record at `index` of `records` (its size in *size), NULL past the end. */
static const BYTE *NthRecord(const BYTE *records, size_t len, size_t index, size_t *size)
{
    size_t pos = 0, i;
    *size = 0;
    for (i = 0; records && len - pos >= 4 && Get32(records + pos) <= len - pos - 4; i++) {
        if (i == index) {
            *size = Get32(records + pos);
            return records + pos + 4;
        }
        pos += 4 + Get32(records + pos);
    }
    return NULL;
}

/* `len` holds exactly `count` records. */
static BOOL RecordCount(const BYTE *records, size_t len, size_t count)
{
    size_t pos = 0, n = 0;
    while (records && len - pos >= 4 && Get32(records + pos) <= len - pos - 4) {
        pos += 4 + Get32(records + pos);
        n++;
    }
    return pos == len && n == count;
}

/* Records follow their entries; none is built here (no entry of ours, no
 * empty record of an active entry). */
static void TestRecordAlignment(void)
{
    BYTE list[2048], shorter[2048], longer[2048], records[64], expect[64], *out = NULL;
    BOOL dropped[3] = { FALSE, TRUE, FALSE };
    size_t a, r, b, len, shorterLen, longerLen, recordsLen = 0, e, outLen = 0, extra;
    HRESULT hr;

    a = OtherEntry(list, L"Other.A", FALSE);
    r = OtherEntry(list + a, L"Other.Retired", TRUE);
    b = OtherEntry(list + a + r, L"Other.B", FALSE);
    len = a + r + b + 1;
    list[len - 1] = 0xFF;
    recordsLen += Record(records + recordsLen, 3, 0xAA);
    recordsLen += Record(records + recordsLen, 0, 0);
    recordsLen += Record(records + recordsLen, 2, 0xBB);

    memcpy(shorter, list, a);
    memcpy(shorter + a, list + a + r, b);
    shorterLen = a + b + 1;
    shorter[shorterLen - 1] = 0xFF;
    e = Record(expect, 3, 0xAA);
    e += Record(expect + e, 2, 0xBB);
    hr = TaskbarPin_PrepareRecords(list, len, shorter, shorterLen, dropped, TASKBAR_PIN_NONE, TRUE, records, recordsLen, &out, &outLen);
    Check("records: a dropped entry's record goes with it", hr == S_OK && outLen == e && memcmp(out, expect, e) == 0);
    FreeBytes(&out);

    hr = TaskbarPin_PrepareRecords(list, len, list, len, NULL, TASKBAR_PIN_NONE, FALSE, records, recordsLen, &out, &outLen);
    Check("records: nothing to write without an edit or a pin of ours", hr == S_FALSE && !out && !outLen);

    extra = recordsLen + Record(records + recordsLen, 1, 0xCC);
    hr = TaskbarPin_PrepareRecords(list, len, list, len, NULL, TASKBAR_PIN_NONE, TRUE, records, extra, &out, &outLen);
    Check("records: records past the last entry are dropped", hr == S_OK && outLen == recordsLen && memcmp(out, records, recordsLen) == 0);
    FreeBytes(&out);

    memcpy(longer, list, len - 1);
    longerLen = len - 1 + OtherEntry(longer + len - 1, L"Other.Retired2", TRUE) + 1;
    longer[longerLen - 1] = 0xFF;
    hr = TaskbarPin_PrepareRecords(list, len, longer, longerLen, NULL, TASKBAR_PIN_NONE, TRUE, records, recordsLen, &out, &outLen);
    Check("records: an added retired entry gets an empty record", hr == S_OK && outLen == recordsLen + 4 &&
          memcmp(out, records, recordsLen) == 0 && Get32(out + recordsLen) == 0);
    FreeBytes(&out);

    hr = TaskbarPin_PrepareRecords(list, len, list, len, NULL, 3, TRUE, records, recordsLen, &out, &outLen);
    Check("records: a new entry past the list is refused", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !out);
    FreeBytes(&out);
    hr = TaskbarPin_PrepareRecords(list, len, shorter, shorterLen, NULL, TASKBAR_PIN_NONE, TRUE, records, recordsLen, &out, &outLen);
    Check("records: an entry removed without its flag is refused", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !out);
    FreeBytes(&out);
}

static void TestList(void)
{
    BYTE item[2048], active[2048], retired[2048], list[8192], out[8192];
    BOOL dropped[4];
    TaskbarPin_Merge merge;
    const WCHAR *id = L"ClaudeDesktopProfilesManager.Profile.1234ABCD";
    const WCHAR *longId = L"ClaudeDesktopProfilesManager.Profile.1234ABCD.extra";
    size_t cb = FileItem(item), activeSize, retiredSize, len, n, other;
    HRESULT hr;

    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    Put16(item, (WORD)cb);
    activeSize = Entry(active, item, cb);
    memcpy(list, active, activeSize);
    list[activeSize] = 0xFF;
    len = activeSize + 1;
    Check("list: a complete list is valid", TaskbarPin_ValidateList(list, len));
    Check("list: active AppID matches", TaskbarPin_ListHasAppId(list, len, id, FALSE));
    Check("list: AppID matching ignores case", TaskbarPin_ListHasAppId(list, len, L"claudedesktopprofilesmanager.profile.1234abcd", FALSE));
    Check("list: AppID prefix is not a match", !TaskbarPin_ListHasAppId(list, len, L"ClaudeDesktopProfilesManager.Profile.1234", FALSE));
    ZeroMemory(dropped, sizeof dropped);
    hr = TaskbarPin_MergeEntry(list, len, active, activeSize, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: an active AppID is replaced at its rank, without a duplicate", hr == S_OK && n == len &&
          merge.rank == 0 && merge.replacedPin && !dropped[0] && memcmp(out, list, len) == 0);

    cb = MarkRemoved(item, cb);
    retiredSize = Entry(retired, item, cb);
    memcpy(list, retired, retiredSize);
    list[retiredSize] = 0xFF;
    len = retiredSize + 1;
    Check("list: removal history is valid", TaskbarPin_ValidateList(list, len));
    Check("list: removal history does not count as pinned", !TaskbarPin_ListHasAppId(list, len, id, FALSE));
    hr = TaskbarPin_MergeEntry(list, len, active, activeSize, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: a retired AppID is pinned again at its rank", hr == S_OK && n == activeSize + 1 &&
          merge.rank == 0 && !merge.replacedPin && memcmp(out, active, activeSize) == 0);
    Check("list: the list pinned again is valid and has the active AppID", hr == S_OK && TaskbarPin_ValidateList(out, n) &&
          TaskbarPin_ListHasAppId(out, n, id, FALSE));

    memcpy(list + retiredSize, active, activeSize);
    other = OtherEntry(list + retiredSize + activeSize, L"Other.App", FALSE);
    len = retiredSize + activeSize + other + 1;
    list[len - 1] = 0xFF;
    ZeroMemory(dropped, sizeof dropped);
    hr = TaskbarPin_MergeEntry(list, len, active, activeSize, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: the active entry is replaced before a retired one", hr == S_OK && n == len && merge.rank == 1 &&
          merge.replacedPin && memcmp(out, list, len) == 0);

    memcpy(list + activeSize, active, activeSize);
    memcpy(list, active, activeSize);
    other = OtherEntry(list + 2 * activeSize, L"Other.App", FALSE);
    len = 2 * activeSize + other + 1;
    list[len - 1] = 0xFF;
    ZeroMemory(dropped, sizeof dropped);
    hr = TaskbarPin_MergeEntry(list, len, active, activeSize, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: a second active entry of the profile is dropped", hr == S_OK && n == len - activeSize &&
          merge.rank == 0 && !dropped[0] && dropped[1] && !dropped[2] && memcmp(out + activeSize, list + 2 * activeSize, other) == 0);

    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, longId, TRUE);
    Put16(item, (WORD)cb);
    len = Entry(list, item, cb);
    list[len++] = 0xFF;
    Check("list: AppID substring in a longer ID is not pinned", !TaskbarPin_ListHasAppId(list, len, id, FALSE));
    hr = TaskbarPin_MergeEntry(list, len, active, activeSize, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: distinct complete AppIDs can coexist", hr == S_OK && n == len + activeSize && merge.rank == 1 && !merge.replacedPin);

    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Eu, id, TRUE);
    Put16(item, (WORD)cb);
    len = Entry(list, item, cb);
    list[len++] = 0xFF;
    Check("list: an AppID string in another block is not a match", !TaskbarPin_ListHasAppId(list, len, id, FALSE));

    memcpy(list, active, activeSize);
    list[activeSize] = 0xFF;
    len = activeSize + 1;
    Check("list: missing end rejected", !TaskbarPin_ValidateList(list, len - 1));
    list[len] = 0;
    Check("list: bytes after the end rejected", !TaskbarPin_ValidateList(list, len + 1));
    Put32(list + 1, (DWORD)len);
    Check("list: oversized entry rejected", !TaskbarPin_ValidateList(list, len));

    memcpy(list, active, activeSize);
    list[activeSize] = 0;
    Put32(list + activeSize + 1, 1000);
    list[activeSize + 5] = 0xFF;
    len = activeSize + 6;
    FillMemory(out, sizeof out, 0xA5);
    hr = TaskbarPin_MergeEntry(list, len, active, activeSize, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: a malformed end blocks an add instead of truncating pins", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && n == 0 && out[0] == 0xA5);
    Check("list: a malformed end also blocks AppID detection", !TaskbarPin_ListHasAppId(list, len, id, FALSE));

    memcpy(list, active, activeSize);
    list[activeSize] = 0xFF;
    Put16(list + 5, 1);
    Check("list: a one-byte ID list item is invalid", !TaskbarPin_ValidateList(list, activeSize + 1));
    memcpy(list, active, activeSize);
    Put16(list + activeSize - 2, 2);
    Check("list: an ID list without its end is invalid", !TaskbarPin_ValidateList(list, activeSize + 1));
    list[0] = 0xFF;
    Check("list: the empty list is valid", TaskbarPin_ValidateList(list, 1));
    hr = TaskbarPin_MergeEntry(list, 1, NULL, 0, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: a missing zero-size entry is rejected without writing output",
          hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && n == 0 && out[0] == 0xA5);
    hr = TaskbarPin_MergeEntry(list, 1, active, 0, NULL, id, dropped, out, sizeof out, &n, &merge);
    Check("list: a present zero-size entry is rejected without writing output",
          hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && n == 0 && out[0] == 0xA5);
    hr = TaskbarPin_MergeEntry(list, 1, active, activeSize, NULL, id, dropped, out, activeSize, &n, &merge);
    Check("list: insufficient output capacity leaves output untouched", hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && n == 0 && out[0] == 0xA5);
}

/* A private volatile key, never the taskbar's own. */
static void TestListRead(void)
{
    WCHAR path[128];
    HKEY key = NULL, writeOnly = NULL;
    DWORD disposition, len = 0;
    BYTE *list = NULL, empty = 0xFF, bad[6] = { 0, 100, 0, 0, 0, 0xFF };
    HRESULT hr;
    LSTATUS rc;
    if (!Prepared("read: private key path", SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"Software\\ClaudeProfiles.PinFixture.%lu.%llu",
                                                                       GetCurrentProcessId(), GetTickCount64()))))
        return;
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &disposition);
    if (!Prepared("read: private volatile key created", rc == ERROR_SUCCESS)) return;
    if (!Prepared("read: the private key is new", disposition == REG_CREATED_NEW_KEY)) {
        RegCloseKey(key);
        return;
    }
    hr = TaskbarPin_ReadList(key, &list, &len);
    Check("read: a missing list is distinguished", hr == S_FALSE && list == NULL);
    if (Prepared("read: wrong-type value written", RegSetValueExW(key, L"Favorites", 0, REG_SZ, (const BYTE *)L"bad", 8) == ERROR_SUCCESS)) {
        hr = TaskbarPin_ReadList(key, &list, &len);
        Check("read: wrong type is an error, not an empty list", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && list == NULL);
    }
    if (Prepared("read: malformed value written", RegSetValueExW(key, L"Favorites", 0, REG_BINARY, bad, sizeof bad) == ERROR_SUCCESS)) {
        hr = TaskbarPin_ReadList(key, &list, &len);
        Check("read: a malformed list is an error", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && list == NULL);
    }
    if (Prepared("read: valid empty value written", RegSetValueExW(key, L"Favorites", 0, REG_BINARY, &empty, 1) == ERROR_SUCCESS)) {
        hr = TaskbarPin_ReadList(key, &list, &len);
        Check("read: a one-byte empty list is readable", hr == S_OK && list && len == 1 && list[0] == 0xFF);
        FreeBytes(&list);
    }
    if (Prepared("read: zero-byte value written", RegSetValueExW(key, L"Favorites", 0, REG_BINARY, &empty, 0) == ERROR_SUCCESS)) {
        hr = TaskbarPin_ReadList(key, &list, &len);
        Check("read: a zero-byte value is an empty list", hr == S_FALSE && list == NULL);
    }
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_SET_VALUE, &writeOnly);
    if (Prepared("read: write-only handle opened", rc == ERROR_SUCCESS)) {
        hr = TaskbarPin_ReadList(writeOnly, &list, &len);
        Check("read: access denial is an error, not absence", FAILED(hr) && list == NULL);
        RegCloseKey(writeOnly);
    }
    RegCloseKey(key);
    Prepared("read: private volatile key removed", RegDeleteKeyW(HKEY_CURRENT_USER, path) == ERROR_SUCCESS);
}

/* ------------------------------------------------- values held in memory */

enum { LIST_VALUE, RECORDS_VALUE, VERSION_VALUE, COUNTER_VALUE, VALUE_COUNT };
static const WCHAR *const kValueNames[VALUE_COUNT] = { L"Favorites", L"FavoritesResolve", L"FavoritesVersion", L"FavoritesChanges" };
static const BYTE kListBefore[] = { 1, 2, 3 }, kListAfter[] = { 4, 5, 6, 7 };
static const BYTE kRecordsBefore[] = { 8, 9 }, kRecordsAfter[] = { 10, 11, 12, 13, 14 };

typedef struct MemoryValue {
    BOOL exists;
    DWORD type, bytes;
    BYTE data[FIXTURE_CAPACITY];
} MemoryValue;

typedef struct MemoryRegistry MemoryRegistry;
typedef void (*MemoryChange)(MemoryRegistry *registry);

/* The pin list's values in memory. A write may fail on purpose, and a read
 * may first change the values, as Explorer saving its own list would. */
struct MemoryRegistry {
    MemoryValue values[VALUE_COUNT];
    int writes, failures[3];
    BOOL failAfterStore;
    int written[16], writtenCount;      /* the values written or erased, in order */
    int reads[VALUE_COUNT];             /* each value's reads so far */
    int changedValue, changeAtReads[3]; /* before these reads of that value (from 1)... */
    MemoryChange change;                /* ...this happens to the values */
    MemoryValue saved[2];               /* a list and its records Explorer saves */
};

static int MemoryIndex(const WCHAR *name)
{
    int i;
    for (i = 0; i < VALUE_COUNT; i++) if (wcscmp(name, kValueNames[i]) == 0) return i;
    return -1;
}

static LSTATUS MemoryRead(void *context, const WCHAR *name, DWORD *type, BYTE *data, DWORD *bytes)
{
    MemoryRegistry *registry = (MemoryRegistry *)context;
    int i = MemoryIndex(name), r;
    MemoryValue *value;
    DWORD capacity = *bytes;
    if (i < 0) return ERROR_FILE_NOT_FOUND;
    registry->reads[i]++;
    if (registry->change && i == registry->changedValue)
        for (r = 0; r < (int)ARRAYSIZE(registry->changeAtReads); r++)
            if (registry->changeAtReads[r] == registry->reads[i]) registry->change(registry);
    value = &registry->values[i];
    if (!value->exists) return ERROR_FILE_NOT_FOUND;
    *type = value->type;
    *bytes = value->bytes;
    if (!data) return ERROR_SUCCESS;
    if (capacity < value->bytes) return ERROR_MORE_DATA;
    if (value->bytes) memcpy(data, value->data, value->bytes);
    return ERROR_SUCCESS;
}

static BOOL MemoryFails(MemoryRegistry *registry, int index)
{
    int i;
    registry->writes++;
    if (registry->writtenCount < (int)ARRAYSIZE(registry->written)) registry->written[registry->writtenCount++] = index;
    for (i = 0; i < (int)ARRAYSIZE(registry->failures); i++)
        if (registry->writes == registry->failures[i]) return TRUE;
    return FALSE;
}

static LSTATUS MemoryWrite(void *context, const WCHAR *name, DWORD type, const BYTE *data, DWORD bytes)
{
    MemoryRegistry *registry = (MemoryRegistry *)context;
    int i = MemoryIndex(name);
    BOOL fail = MemoryFails(registry, i);
    if (i < 0 || bytes > sizeof registry->values[0].data) return ERROR_INVALID_PARAMETER;
    if (fail && !registry->failAfterStore) return ERROR_WRITE_FAULT;
    ZeroMemory(&registry->values[i], sizeof registry->values[i]);
    registry->values[i].exists = TRUE;
    registry->values[i].type = type;
    registry->values[i].bytes = bytes;
    if (bytes) memcpy(registry->values[i].data, data, bytes);
    return fail ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
}

static LSTATUS MemoryErase(void *context, const WCHAR *name)
{
    MemoryRegistry *registry = (MemoryRegistry *)context;
    int i = MemoryIndex(name);
    BOOL fail = MemoryFails(registry, i);
    if (i < 0) return ERROR_INVALID_PARAMETER;
    if (fail && !registry->failAfterStore) return ERROR_WRITE_FAULT;
    if (!registry->values[i].exists) return ERROR_FILE_NOT_FOUND;   /* as the registry answers */
    ZeroMemory(&registry->values[i], sizeof registry->values[i]);
    return fail ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
}

static TaskbarPin_ValueIO MemoryAccess(MemoryRegistry *registry)
{
    TaskbarPin_ValueIO io;
    io.context = registry;
    io.read = MemoryRead;
    io.write = MemoryWrite;
    io.erase = MemoryErase;
    return io;
}

static void StoreValue(MemoryValue *value, DWORD type, const void *data, DWORD bytes)
{
    ZeroMemory(value, sizeof *value);
    value->exists = TRUE;
    value->type = type;
    value->bytes = bytes;
    if (bytes) memcpy(value->data, data, bytes);
}

/* `list` and `records` (missing when NULL), version 3 and a counter of 40;
 * no reads or writes counted yet. */
static void SeedPinValues(MemoryRegistry *registry, const BYTE *list, size_t listLen, const BYTE *records, size_t recordsLen)
{
    DWORD version = LIST_VERSION, changes = 40;
    ZeroMemory(registry, sizeof *registry);
    if (list) StoreValue(&registry->values[LIST_VALUE], REG_BINARY, list, (DWORD)listLen);
    if (records) StoreValue(&registry->values[RECORDS_VALUE], REG_BINARY, records, (DWORD)recordsLen);
    StoreValue(&registry->values[VERSION_VALUE], REG_DWORD, &version, sizeof version);
    StoreValue(&registry->values[COUNTER_VALUE], REG_DWORD, &changes, sizeof changes);
}

static void SeedMemoryRegistry(MemoryRegistry *registry, BOOL absent)
{
    if (absent) ZeroMemory(registry, sizeof *registry);
    else SeedPinValues(registry, kListBefore, sizeof kListBefore, kRecordsBefore, sizeof kRecordsBefore);
}

static BOOL MemoryValueIs(const MemoryRegistry *registry, int index, DWORD type, const void *data, size_t bytes)
{
    const MemoryValue *value = &registry->values[index];
    return value->exists && value->type == type && value->bytes == bytes && (!bytes || memcmp(value->data, data, bytes) == 0);
}

static BOOL CounterIs(const MemoryRegistry *registry, DWORD expected)
{
    return MemoryValueIs(registry, COUNTER_VALUE, REG_DWORD, &expected, sizeof expected);
}

/* The list and its records of `registry`, as a commit's base. */
static void MemoryBase(const MemoryRegistry *registry, TaskbarPin_Value base[2])
{
    int i;
    for (i = 0; i < 2; i++) {
        base[i].exists = registry->values[i].exists;
        base[i].type = registry->values[i].type;
        base[i].bytes = registry->values[i].bytes;
        base[i].data = (BYTE *)registry->values[i].data;
    }
}

/* The prepared pair committed over the values `registry` holds now. */
static HRESULT CommitOverCurrent(MemoryRegistry *registry, TaskbarPin_CommitResult *result)
{
    static MemoryRegistry read;
    TaskbarPin_ValueIO io = MemoryAccess(registry);
    TaskbarPin_Value base[2];
    read = *registry;
    MemoryBase(&read, base);
    return TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, result);
}

static void TestCommitFailures(void)
{
    static MemoryRegistry registry, before, read;
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    TaskbarPin_Value base[2];
    TaskbarPin_CommitResult result;
    DWORD version = LIST_VERSION + 1;
    HRESULT hr;
    int fail, after, absent, failuresBefore;
    for (absent = 0; absent < 2; absent++) for (after = 0; after < 2; after++) for (fail = 1; fail <= 4; fail++) {
        SeedMemoryRegistry(&registry, absent);
        before = registry;
        registry.failures[0] = fail;
        registry.failAfterStore = after;
        failuresBefore = g_failures;
        hr = CommitOverCurrent(&registry, &result);
        Check("write: failure at each value is reported", hr == HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        Check("write: complete values and absence restored after each failure", memcmp(registry.values, before.values, sizeof registry.values) == 0);
        Check("write: a rolled back list lets a new shortcut be cleaned up", !result.listCommitted && !result.uncertain && !result.rollbackError);
        if (g_failures != failuresBefore) printf("        with values absent %d, failing after storing %d, at write %d\n", absent, after, fail);
    }

    SeedMemoryRegistry(&registry, FALSE);
    registry.failures[0] = 4;
    registry.failures[1] = 7;
    hr = CommitOverCurrent(&registry, &result);
    Check("write: a failed list rollback keeps the referenced shortcut", FAILED(hr) && result.listCommitted && result.uncertain);
    Check("write: a failed list rollback keeps the records matching it", MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, kListAfter, sizeof kListAfter) &&
          MemoryValueIs(&registry, RECORDS_VALUE, REG_BINARY, kRecordsAfter, sizeof kRecordsAfter));

    SeedMemoryRegistry(&registry, FALSE);
    registry.failures[0] = 4;
    registry.failures[1] = 8;
    hr = CommitOverCurrent(&registry, &result);
    Check("write: a records rollback failure reports the pin kept", FAILED(hr) && result.listCommitted && result.uncertain);
    Check("write: the prepared pair stays aligned after a records rollback failure",
          MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, kListAfter, sizeof kListAfter) &&
          MemoryValueIs(&registry, RECORDS_VALUE, REG_BINARY, kRecordsAfter, sizeof kRecordsAfter));

    SeedMemoryRegistry(&registry, FALSE);
    before = registry;
    registry.failures[0] = 4;
    registry.failures[1] = 8;
    registry.failAfterStore = TRUE;
    hr = CommitOverCurrent(&registry, &result);
    Check("write: a reported rollback error can be verified as restored bytes", FAILED(hr) && !result.listCommitted && result.uncertain &&
          memcmp(registry.values, before.values, sizeof registry.values) == 0);

    SeedMemoryRegistry(&registry, FALSE);
    registry.failures[0] = 4;
    registry.failures[1] = 8;
    registry.failures[2] = 9;
    hr = CommitOverCurrent(&registry, &result);
    Check("write: an unconfirmed pair keeps its shortcut", FAILED(hr) && result.uncertain);

    SeedMemoryRegistry(&registry, FALSE);
    registry.values[COUNTER_VALUE].type = REG_BINARY;
    before = registry;
    hr = CommitOverCurrent(&registry, &result);
    Check("write: a malformed counter refuses changes", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && registry.writes == 0 &&
          memcmp(registry.values, before.values, sizeof registry.values) == 0);

    SeedMemoryRegistry(&registry, FALSE);
    StoreValue(&registry.values[VERSION_VALUE], REG_DWORD, &version, sizeof version);
    before = registry;
    hr = CommitOverCurrent(&registry, &result);
    Check("write: a list of another form is never written over", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && registry.writes == 0 &&
          memcmp(registry.values, before.values, sizeof registry.values) == 0);

    /* Explorer saved the list after it was read: nothing is written. */
    SeedMemoryRegistry(&registry, FALSE);
    read = registry;
    StoreValue(&registry.values[RECORDS_VALUE], REG_BINARY, kRecordsAfter, sizeof kRecordsAfter);
    before = registry;
    MemoryBase(&read, base);
    hr = TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, &result);
    Check("write: a list changed since it was read is not written", hr == E_CHANGED_STATE && registry.writes == 0 &&
          memcmp(registry.values, before.values, sizeof registry.values) == 0 && !result.listCommitted && !result.uncertain);
    SeedMemoryRegistry(&read, TRUE);
    MemoryBase(&read, base);
    hr = TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, &result);
    Check("write: values created since they were read as missing are not overwritten", hr == E_CHANGED_STATE && registry.writes == 0);
    Check("write: a commit needs its base", TaskbarPin_CommitPrepared(&io, NULL, NULL, 0, NULL, 0, FALSE, &result) == E_INVALIDARG);
}

/* The records go first and the change counter last, as the taskbar writes them. */
static void TestWriteOrder(void)
{
    static MemoryRegistry registry;
    TaskbarPin_CommitResult result;
    DWORD version = LIST_VERSION;
    HRESULT hr;
    SeedMemoryRegistry(&registry, FALSE);
    hr = CommitOverCurrent(&registry, &result);
    Check("order: the pair is committed", hr == S_OK && result.listCommitted && !result.uncertain);
    Check("order: records, list, version, then the counter", registry.writtenCount == 4 && registry.written[0] == RECORDS_VALUE &&
          registry.written[1] == LIST_VALUE && registry.written[2] == VERSION_VALUE && registry.written[3] == COUNTER_VALUE);
    Check("order: the counter goes on from its value", CounterIs(&registry, 41));

    SeedMemoryRegistry(&registry, TRUE);
    hr = CommitOverCurrent(&registry, &result);
    Check("order: missing values are created", hr == S_OK && MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, kListAfter, sizeof kListAfter) &&
          MemoryValueIs(&registry, VERSION_VALUE, REG_DWORD, &version, sizeof version) && CounterIs(&registry, 1));
}

static void ListGrows(MemoryRegistry *registry)
{
    registry->values[LIST_VALUE] = registry->saved[0];
}

static void ValueGrowsByOne(MemoryRegistry *registry)
{
    registry->values[registry->changedValue].bytes++;
}

static void ValueDisappears(MemoryRegistry *registry)
{
    registry->values[registry->changedValue].exists = FALSE;
}

/* Explorer may change a value between the query of its size and its read. */
static void TestSnapshotRereads(void)
{
    static MemoryRegistry registry;
    static const BYTE grown[] = { 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 };
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    TaskbarPin_Value base[2];
    TaskbarPin_CommitResult result;
    HRESULT hr;

    SeedMemoryRegistry(&registry, FALSE);
    StoreValue(&registry.saved[0], REG_BINARY, grown, sizeof grown);
    MemoryBase(&registry, base);
    base[LIST_VALUE].bytes = sizeof grown;
    base[LIST_VALUE].data = (BYTE *)grown;
    registry.changedValue = LIST_VALUE;
    registry.changeAtReads[0] = 2;
    registry.change = ListGrows;
    hr = TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, &result);
    Check("snapshot: a value that grew before its read is read again", hr == S_OK && registry.reads[LIST_VALUE] == 4 &&
          MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, kListAfter, sizeof kListAfter));

    SeedMemoryRegistry(&registry, FALSE);
    MemoryBase(&registry, base);
    base[RECORDS_VALUE].exists = FALSE;
    registry.changedValue = RECORDS_VALUE;
    registry.changeAtReads[0] = 2;
    registry.change = ValueDisappears;
    hr = TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, &result);
    Check("snapshot: a value deleted before its read counts as missing", hr == S_OK &&
          MemoryValueIs(&registry, RECORDS_VALUE, REG_BINARY, kRecordsAfter, sizeof kRecordsAfter));

    SeedMemoryRegistry(&registry, FALSE);
    MemoryBase(&registry, base);
    registry.changedValue = LIST_VALUE;
    registry.changeAtReads[0] = 2;
    registry.changeAtReads[1] = 4;
    registry.changeAtReads[2] = 6;
    registry.change = ValueGrowsByOne;
    hr = TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, &result);
    Check("snapshot: a value that keeps growing fails the commit, writing nothing",
          hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) && registry.writes == 0);
}

static BOOL RegistryValueIs(HKEY key, const WCHAR *name, DWORD expectedType, const void *expected, DWORD expectedBytes)
{
    BYTE bytes[128];
    DWORD type = 0, count = sizeof bytes;
    return RegQueryValueExW(key, name, NULL, &type, bytes, &count) == ERROR_SUCCESS &&
           type == expectedType && count == expectedBytes && (!count || memcmp(bytes, expected, count) == 0);
}

/* The list and its records of `key`, as a commit's base. */
static void KeyBase(HKEY key, TaskbarPin_Value base[2], BYTE (*storage)[128])
{
    int i;
    for (i = 0; i < 2; i++) {
        base[i].bytes = sizeof storage[i];
        base[i].data = storage[i];
        base[i].exists = RegQueryValueExW(key, kValueNames[i], NULL, &base[i].type, storage[i], &base[i].bytes) == ERROR_SUCCESS;
        if (!base[i].exists) base[i].type = base[i].bytes = 0;
    }
}

/* The registry access, only under a fresh private key. */
static void TestRegistryWrite(void)
{
    GUID guid;
    WCHAR id[40], path[128];
    HKEY key = NULL;
    DWORD disposition = 0, changes, version = LIST_VERSION;
    TaskbarPin_ValueIO io;
    TaskbarPin_Value base[2];
    BYTE storage[2][128];
    TaskbarPin_CommitResult result;
    HRESULT hr;
    LSTATUS rc;
    if (!Prepared("write: private key name", SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, id, ARRAYSIZE(id)) &&
                                             SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"Software\\ClaudeProfiles.PinWrite.%s", id))))
        return;
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &disposition);
    if (!Prepared("write: private volatile key created", rc == ERROR_SUCCESS)) return;
    if (!Prepared("write: the private key is new", disposition == REG_CREATED_NEW_KEY)) {
        RegCloseKey(key);
        return;
    }
    Check("write: a whole hive is refused", !TaskbarPin_RegistryValues(HKEY_CURRENT_USER, &io) && !io.write);
    Check("write: the local-settings hive is refused", !TaskbarPin_RegistryValues(HKEY_CURRENT_USER_LOCAL_SETTINGS, &io) && !io.write);
    Check("write: no key is refused", !TaskbarPin_RegistryValues(NULL, &io) && !io.write);
    if (Prepared("write: registry access to the private key", TaskbarPin_RegistryValues(key, &io))) {
        changes = 40;
        if (Prepared("write: private counter written",
                     RegSetValueExW(key, L"FavoritesChanges", 0, REG_DWORD, (const BYTE *)&changes, sizeof changes) == ERROR_SUCCESS)) {
            changes = 41;
            KeyBase(key, base, storage);
            hr = TaskbarPin_CommitPrepared(&io, base, kListAfter, sizeof kListAfter, kRecordsAfter, sizeof kRecordsAfter, TRUE, &result);
            Check("write: the registry takes a prepared pair", hr == S_OK && result.listCommitted && !result.uncertain);
            Check("write: list bytes", RegistryValueIs(key, L"Favorites", REG_BINARY, kListAfter, sizeof kListAfter));
            Check("write: records bytes", RegistryValueIs(key, L"FavoritesResolve", REG_BINARY, kRecordsAfter, sizeof kRecordsAfter));
            Check("write: version three", RegistryValueIs(key, L"FavoritesVersion", REG_DWORD, &version, sizeof version));
            Check("write: the counter goes on from its value", RegistryValueIs(key, L"FavoritesChanges", REG_DWORD, &changes, sizeof changes));
        }
        KeyBase(key, base, storage);
        Check("write: a missing prepared buffer is refused", TaskbarPin_CommitPrepared(&io, base, NULL, 1, NULL, 0, FALSE, &result) == E_INVALIDARG);
        if (Prepared("write: private counter deleted", RegDeleteValueW(key, L"FavoritesChanges") == ERROR_SUCCESS)) {
            hr = TaskbarPin_CommitPrepared(&io, base, NULL, 0, kRecordsBefore, sizeof kRecordsBefore, TRUE, &result);
            changes = 1;
            Check("write: a records-only repair keeps the list", hr == S_OK && !result.listCommitted &&
                  RegistryValueIs(key, L"Favorites", REG_BINARY, kListAfter, sizeof kListAfter));
            Check("write: a missing counter starts at one", RegistryValueIs(key, L"FavoritesChanges", REG_DWORD, &changes, sizeof changes));
        }
    }
    RegCloseKey(key);
    Prepared("write: private volatile key removed", RegDeleteKeyW(HKEY_CURRENT_USER, path) == ERROR_SUCCESS);
}

/* ---------------------------------------------------------- private files */

/* The private files of the tests below, in a private temporary folder. */
typedef struct PinFiles {
    WCHAR root[MAX_PATH];
    WCHAR pins[MAX_PATH];           /* stands for the pins folder */
    WCHAR away[MAX_PATH];           /* another folder */
    WCHAR state[MAX_PATH];          /* the log and the icons */
    WCHAR target[MAX_PATH];         /* a copy of this exe, named like the manager's */
    WCHAR shortcut[MAX_PATH];       /* the profile's shortcut in the pins folder */
    WCHAR older[MAX_PATH];          /* another one there */
    WCHAR elsewhere[MAX_PATH];      /* another one outside it */
    WCHAR otherFile[MAX_PATH];      /* files pinned by other applications */
    WCHAR secondOtherFile[MAX_PATH];
    WCHAR id[AUMID_CCH];
    Profile profile, other;
} PinFiles;

static PinFiles g_files;

static BOOL JoinPath(const WCHAR *dir, const WCHAR *name, WCHAR *out)
{
    return SUCCEEDED(StringCchPrintfW(out, MAX_PATH, L"%s\\%s", dir, name));
}

static BOOL WriteEmptyFile(const WCHAR *path)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(file);
    return TRUE;
}

static BOOL DeleteTree(const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    BOOL ok = TRUE;
    if (!JoinPath(dir, L"*", pattern)) return FALSE;
    search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if (!JoinPath(dir, found.cFileName, path)) ok = FALSE;
            else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = DeleteTree(path) && ok;
            else ok = DeleteFileW(path) && ok;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return RemoveDirectoryW(dir) && ok;
}

/* A shortcut that opens `folder` through `target`, carrying `appId` and
 * `showCommand` when given; or, with `native`, one to that item. */
static BOOL WriteFixtureLink(const WCHAR *path, const WCHAR *target, const WCHAR *folder, const WCHAR *appId, int showCommand,
                             PCIDLIST_ABSOLUTE native)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    IPropertyStore *store = NULL;
    PROPVARIANT value;
    WCHAR args[128];
    HRESULT hr;
    PropVariantInit(&value);
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr)) {
        if (native) {
            hr = IShellLinkW_SetIDList(link, native);
        } else {
            hr = StringCchPrintfW(args, ARRAYSIZE(args), L"--launch \"%s\"", folder);
            if (SUCCEEDED(hr)) hr = IShellLinkW_SetPath(link, target);
            if (SUCCEEDED(hr)) hr = IShellLinkW_SetArguments(link, args);
        }
    }
    if (SUCCEEDED(hr) && showCommand) hr = IShellLinkW_SetShowCmd(link, showCommand);
    if (SUCCEEDED(hr) && appId) {
        size_t bytes = (wcslen(appId) + 1) * sizeof(WCHAR);
        hr = IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&store);
        if (SUCCEEDED(hr)) {
            value.vt = VT_LPWSTR;
            value.pwszVal = (LPWSTR)CoTaskMemAlloc(bytes);
            hr = value.pwszVal ? S_OK : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) memcpy(value.pwszVal, appId, bytes);
        if (SUCCEEDED(hr)) hr = IPropertyStore_SetValue(store, &PKEY_AppUserModel_ID, &value);
        if (SUCCEEDED(hr)) hr = IPropertyStore_Commit(store);
    }
    if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr)) hr = IPersistFile_Save(file, path, TRUE);
    PropVariantClear(&value);
    if (store) IPropertyStore_Release(store);
    if (file) IPersistFile_Release(file);
    if (link) IShellLinkW_Release(link);
    return SUCCEEDED(hr);
}

/* What a shortcut holds: its show command, icon file and description. */
static BOOL ReadLinkState(const WCHAR *path, int *showCommand, WCHAR *icon, WCHAR *description)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    int index = 0;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr)) hr = IPersistFile_Load(file, path, STGM_READ);
    if (SUCCEEDED(hr)) hr = IShellLinkW_GetShowCmd(link, showCommand);
    if (SUCCEEDED(hr)) hr = IShellLinkW_GetIconLocation(link, icon, MAX_PATH, &index);
    if (SUCCEEDED(hr)) hr = IShellLinkW_GetDescription(link, description, MAX_PATH);
    if (file) IPersistFile_Release(file);
    if (link) IShellLinkW_Release(link);
    return SUCCEEDED(hr);
}

/* The entry pinning `path` with `appId`, retired when `removed`; with
 * `damaged`, its AppID block written short, its last WORD after it, as some
 * Windows versions leave it. */
static size_t PathEntry(const WCHAR *path, const WCHAR *appId, BOOL removed, BOOL damaged, BYTE *out, size_t cap)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    PCUITEMID_CHILD last;
    BYTE item[2048];
    size_t prefix, cb, n = 0, appIdBlock = 12 + (wcslen(appId) + 1) * sizeof(WCHAR);
    if (FAILED(SHParseDisplayName(path, NULL, &pidl, 0, NULL)) || !pidl) return 0;
    last = ILFindLastID(pidl);
    prefix = (size_t)((const BYTE *)last - (const BYTE *)pidl);
    cb = TaskbarPin_InjectAppId((const BYTE *)last, last->mkid.cb, appId, item, sizeof item);
    if (cb && damaged) Put16(item + cb - appIdBlock, (WORD)(appIdBlock - 2));
    if (cb && removed && cb + 20 <= sizeof item) cb = MarkRemoved(item, cb);
    if (cb && prefix + cb + 7 <= cap) {
        out[0] = 0;
        Put32(out + 1, (DWORD)(prefix + cb + 2));
        memcpy(out + 5, pidl, prefix);
        memcpy(out + 5 + prefix, item, cb);
        Put16(out + 5 + prefix + cb, 0);
        n = prefix + cb + 7;
    }
    ILFree(pidl);
    return n;
}

static BOOL CacheResolves(const BYTE *data, size_t len, const WCHAR *target)
{
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, len);
    void *bytes;
    IStream *stream = NULL;
    IShellLinkW *link = NULL;
    IPersistStream *persist = NULL;
    WCHAR resolved[MAX_PATH];
    BOOL ok = FALSE;
    HRESULT hr;
    const WCHAR *stage = L"CreateStreamOnHGlobal";
    if (!memory) return FALSE;
    bytes = GlobalLock(memory);
    if (!bytes) {
        GlobalFree(memory);
        return FALSE;
    }
    memcpy(bytes, data, len);
    GlobalUnlock(memory);
    hr = CreateStreamOnHGlobal(memory, TRUE, &stream);
    if (FAILED(hr)) {
        fwprintf(stderr, L"  record %s: 0x%08lX\n", stage, (unsigned long)hr);
        GlobalFree(memory);
        return FALSE;
    }
    stage = L"CoCreateInstance";
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr)) {
        stage = L"QueryInterface(IPersistStream)";
        hr = IShellLinkW_QueryInterface(link, &IID_IPersistStream, (void **)&persist);
    }
    if (SUCCEEDED(hr)) {
        stage = L"IPersistStream.Load";
        hr = IPersistStream_Load(persist, stream);
    }
    if (SUCCEEDED(hr)) {
        stage = L"IShellLink.Resolve";
        hr = IShellLinkW_Resolve(link, NULL, SLR_NO_UI | SLR_NOSEARCH);
    }
    if (SUCCEEDED(hr)) {
        stage = L"IShellLink.GetPath";
        hr = IShellLinkW_GetPath(link, resolved, ARRAYSIZE(resolved), NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        ok = Core_PathEquals(resolved, target);
        if (!ok) fwprintf(stderr, L"  record path mismatch: expected %s, got %s\n", target, resolved);
    } else fwprintf(stderr, L"  record %s: 0x%08lX\n", stage, (unsigned long)hr);
    if (persist) IPersistStream_Release(persist);
    if (link) IShellLinkW_Release(link);
    IStream_Release(stream);
    return ok;
}

/* A record of the form Windows writes, resolving to the pinned file itself. */
static BOOL WindowsRecord(const BYTE *data, size_t len, const WCHAR *lnk)
{
    return data && len >= 0x4C && Get32(data) == 0x4C && (Get32(data + 0x14) & WINDOWS_RECORD_FLAGS) == WINDOWS_RECORD_FLAGS &&
           CacheResolves(data, len, lnk);
}

/* The record at `index` is of the form Windows writes, for `lnk`. */
static BOOL WindowsRecordAt(const BYTE *records, size_t len, size_t index, const WCHAR *lnk)
{
    size_t size;
    const BYTE *record = NthRecord(records, len, index, &size);
    return record && WindowsRecord(record, size, lnk);
}

/* The record at `index` is `expected` byte for byte. */
static BOOL RecordAtIs(const BYTE *records, size_t len, size_t index, const BYTE *expected, size_t expectedSize)
{
    size_t size;
    const BYTE *record = NthRecord(records, len, index, &size);
    return record && size == expectedSize && (!size || memcmp(record, expected, size) == 0);
}

/* The record earlier versions wrote: the shortcut file saved again, size first. */
static size_t LegacyRecord(const WCHAR *lnk, BYTE *out, size_t cap)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    IPersistStream *persist = NULL;
    IStream *stream = NULL;
    STATSTG stat;
    LARGE_INTEGER start;
    ULONG read = 0;
    size_t n = 0;
    start.QuadPart = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)) &&
        SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)) &&
        SUCCEEDED(IPersistFile_Load(file, lnk, STGM_READ)) &&
        SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistStream, (void **)&persist)) &&
        SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) && SUCCEEDED(IPersistStream_Save(persist, stream, TRUE)) &&
        SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) && stat.cbSize.LowPart + 4 <= cap &&
        SUCCEEDED(IStream_Seek(stream, start, STREAM_SEEK_SET, NULL)) &&
        SUCCEEDED(IStream_Read(stream, out + 4, stat.cbSize.LowPart, &read)) && read == stat.cbSize.LowPart) {
        Put32(out, read);
        n = 4 + read;
    }
    if (stream) IStream_Release(stream);
    if (persist) IPersistStream_Release(persist);
    if (file) IPersistFile_Release(file);
    if (link) IShellLinkW_Release(link);
    return n;
}

static void TestRecords(const BYTE *active, size_t activeSize)
{
    const WCHAR *path = g_files.shortcut;
    BYTE empty[1] = { 0xFF }, zero[4] = { 0 }, older[4096], oldList[16384], newList[16384], records[64], legacy[4096], olderRecords[8192];
    BYTE *cache = NULL, *again = NULL, *built = NULL;
    size_t cacheLen = 0, againLen = 0, builtLen = 0, olderSize, retiredSize, otherSize, oldLen, newLen, recordsLen, legacyLen;
    Bytes bytes;
    HRESULT hr;

    hr = TaskbarPin_BuildRecord(active, activeSize, &cache, &cacheLen);
    Check("record: built in Windows' form, resolving to the pinned shortcut", hr == S_OK && WindowsRecord(cache, cacheLen, path));
    FreeBytes(&cache);

    memcpy(newList, active, activeSize);
    newList[activeSize] = 0xFF;
    newLen = activeSize + 1;
    hr = TaskbarPin_PrepareRecords(empty, sizeof empty, newList, newLen, NULL, 0, TRUE, NULL, 0, &cache, &cacheLen);
    Check("records: a first pin creates the missing value with its record", hr == S_OK && RecordCount(cache, cacheLen, 1) &&
          WindowsRecordAt(cache, cacheLen, 0, path));
    FreeBytes(&cache);

    hr = TaskbarPin_PrepareRecords(newList, newLen, newList, newLen, NULL, TASKBAR_PIN_NONE, FALSE, zero, sizeof zero, &cache, &cacheLen);
    Check("records: an empty record of our pin is rebuilt", hr == S_OK && RecordCount(cache, cacheLen, 1) && WindowsRecordAt(cache, cacheLen, 0, path));
    if (hr == S_OK) {
        hr = TaskbarPin_PrepareRecords(newList, newLen, newList, newLen, NULL, TASKBAR_PIN_NONE, FALSE, cache, cacheLen, &again, &againLen);
        Check("records: a record in Windows' form needs nothing", hr == S_FALSE && !again && !againLen);
        FreeBytes(&again);
    }
    FreeBytes(&cache);

    legacyLen = LegacyRecord(path, legacy, sizeof legacy);
    if (Prepared("records: an earlier version's record built", legacyLen > 4 && !WindowsRecord(legacy + 4, legacyLen - 4, path))) {
        hr = TaskbarPin_PrepareRecords(newList, newLen, newList, newLen, NULL, TASKBAR_PIN_NONE, FALSE, legacy, legacyLen, &cache, &cacheLen);
        Check("records: a record saved by an earlier version is replaced", hr == S_OK && RecordCount(cache, cacheLen, 1) &&
              WindowsRecordAt(cache, cacheLen, 0, path));
        FreeBytes(&cache);
    }

    /* A record in Windows' form, made for the ID list of another shortcut the
     * entry pinned before. */
    olderSize = PathEntry(g_files.older, g_files.id, FALSE, FALSE, older, sizeof older);
    if (Prepared("records: an older entry built", olderSize != 0) &&
        Prepared("records: its record built", SUCCEEDED(TaskbarPin_BuildRecord(older, olderSize, &built, &builtLen)))) {
        StartBytes(&bytes, olderRecords, sizeof olderRecords);
        AppendRecord(&bytes, built, builtLen);
        hr = TaskbarPin_PrepareRecords(newList, newLen, newList, newLen, NULL, TASKBAR_PIN_NONE, FALSE, olderRecords, bytes.len, &cache, &cacheLen);
        Check("records: a record made for an older entry's ID list is replaced", !bytes.overflow && hr == S_OK &&
              RecordCount(cache, cacheLen, 1) && WindowsRecordAt(cache, cacheLen, 0, path) && !RecordAtIs(cache, cacheLen, 0, built, builtLen));
        FreeBytes(&cache);
    }
    FreeBytes(&built);

    /* Our retired entry, another application's pin, then a new pin. */
    retiredSize = PathEntry(path, g_files.id, TRUE, FALSE, oldList, sizeof oldList - 1);
    otherSize = retiredSize ? PathEntry(g_files.otherFile, L"Unrelated.App", FALSE, FALSE, oldList + retiredSize, sizeof oldList - retiredSize - 1) : 0;
    if (!Prepared("records: retired and other entries built", retiredSize != 0 && otherSize != 0)) return;
    oldLen = retiredSize + otherSize + 1;
    oldList[oldLen - 1] = 0xFF;
    memcpy(newList, oldList, oldLen - 1);
    memcpy(newList + oldLen - 1, active, activeSize);
    newLen = oldLen + activeSize;
    newList[newLen - 1] = 0xFF;
    recordsLen = Record(records, 0, 0);
    recordsLen += Record(records + recordsLen, 3, 0xA7);
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, newList, newLen, NULL, 2, TRUE, records, recordsLen, &cache, &cacheLen);
    Check("records: history and another pin's record stay byte for byte beside a new pin", hr == S_OK &&
          RecordCount(cache, cacheLen, 3) && memcmp(cache, records, recordsLen) == 0 && WindowsRecordAt(cache, cacheLen, 2, path));
    FreeBytes(&cache);
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, oldList, oldLen, NULL, TASKBAR_PIN_NONE, FALSE, records, recordsLen, &cache, &cacheLen);
    Check("records: history is never given a record", hr == S_FALSE && !cache);
    FreeBytes(&cache);

    hr = TaskbarPin_PrepareRecords(oldList, oldLen, newList, newLen, NULL, 2, TRUE, records, 4, &cache, &cacheLen);
    Check("records: records that do not cover the list are read as empty, an active entry's rebuilt", hr == S_OK &&
          RecordCount(cache, cacheLen, 3) && Get32(cache) == 0 && WindowsRecordAt(cache, cacheLen, 1, g_files.otherFile) &&
          WindowsRecordAt(cache, cacheLen, 2, path));
    FreeBytes(&cache);
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, newList, newLen, NULL, 2, TRUE, NULL, 0, &cache, &cacheLen);
    Check("records: a missing value with existing pins is rebuilt aligned", hr == S_OK && RecordCount(cache, cacheLen, 3) &&
          Get32(cache) == 0 && WindowsRecordAt(cache, cacheLen, 1, g_files.otherFile));
    FreeBytes(&cache);
    Put32(records, 1000);
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, newList, newLen, NULL, 2, TRUE, records, recordsLen, &cache, &cacheLen);
    Check("records: a record past the end makes every record empty, as Windows reads it", hr == S_OK &&
          RecordCount(cache, cacheLen, 3) && Get32(cache) == 0 && WindowsRecordAt(cache, cacheLen, 1, g_files.otherFile));
    FreeBytes(&cache);
}

static BOOL TestMissingPin(const BYTE *oldEntry, size_t oldSize)
{
    const WCHAR *path = g_files.shortcut;
    BYTE neighbor[2048], oldList[16384], newEntry[8192], merged[16384], records[64];
    BYTE *cache = NULL;
    BOOL dropped[3] = { FALSE, FALSE, FALSE };
    TaskbarPin_Merge merge;
    size_t neighborSize, oldLen, newSize, mergedLen = 0, recordsLen, cacheLen = 0, ownSize;
    HRESULT hr;
    neighborSize = OtherEntry(neighbor, L"Unrelated.Pin", FALSE);
    memcpy(oldList, neighbor, neighborSize);
    memcpy(oldList + neighborSize, oldEntry, oldSize);
    memcpy(oldList + neighborSize + oldSize, neighbor, neighborSize);
    oldLen = neighborSize * 2 + oldSize + 1;
    oldList[oldLen - 1] = 0xFF;
    if (!Prepared("missing pin: private shortcut removed", DeleteFileW(path))) return FALSE;
    Check("missing pin: an active AppID without its file is not pinned", !TaskbarPin_ListHasProfile(oldList, oldLen, &g_files.profile));
    if (!Prepared("missing pin: shortcut written again at its path",
                  WriteFixtureLink(path, g_files.target, g_files.profile.folder, NULL, 0, NULL)))
        return FALSE;
    newSize = PathEntry(path, g_files.id, FALSE, FALSE, newEntry, sizeof newEntry);
    if (!Prepared("missing pin: replacement entry built", newSize != 0)) return TRUE;
    hr = TaskbarPin_MergeEntry(oldList, oldLen, newEntry, newSize, path, g_files.id, dropped, merged, sizeof merged, &mergedLen, &merge);
    Check("missing pin: the shortcut written again replaces the profile's entry", hr == S_OK && merge.rank == 1 && merge.replacedPin);
    Check("missing pin: entry rank and neighboring entries are preserved", hr == S_OK && mergedLen == neighborSize * 2 + newSize + 1 &&
          memcmp(merged, neighbor, neighborSize) == 0 && memcmp(merged + neighborSize, newEntry, newSize) == 0 &&
          memcmp(merged + neighborSize + newSize, neighbor, neighborSize) == 0 && merged[mergedLen - 1] == 0xFF);
    Check("missing pin: the replacement is detected and its file exists", hr == S_OK && TaskbarPin_ListHasProfile(merged, mergedLen, &g_files.profile));
    recordsLen = Record(records, 3, 0xA9);
    recordsLen += Record(records + recordsLen, 0, 0);
    recordsLen += Record(records + recordsLen, 2, 0xB8);
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, merged, mergedLen, dropped, merge.rank, TRUE, records, recordsLen, &cache, &cacheLen);
    Check("missing pin: replacement records prepared", hr == S_OK && RecordCount(cache, cacheLen, 3));
    if (hr == S_OK && cache && cacheLen > 11) {
        ownSize = Get32(cache + 7);
        Check("missing pin: neighboring records stay exact and aligned", ownSize > 0 && cacheLen == 7 + 4 + ownSize + 6 &&
              memcmp(cache, records, 7) == 0 && memcmp(cache + 11 + ownSize, records + 11, 6) == 0);
        Check("missing pin: the replaced entry's record resolves to its shortcut", WindowsRecord(cache + 11, ownSize, path));
    }
    FreeBytes(&cache);
    return TRUE;
}

static void TestRetiredPin(void)
{
    const WCHAR *path = g_files.shortcut;
    BYTE neighbor[2048], history[2048], oldList[16384], newEntry[8192], merged[16384], records[64];
    BYTE *cache = NULL;
    BOOL dropped[3] = { FALSE, FALSE, FALSE };
    TaskbarPin_Merge merge;
    size_t neighborSize, historySize, retiredSize, oldLen, newSize, mergedLen = 0, recordsLen, cacheLen = 0;
    HRESULT hr;
    neighborSize = OtherEntry(neighbor, L"Unrelated.ActivePin", FALSE);
    historySize = OtherEntry(history, L"Unrelated.RetiredPin", TRUE);
    memcpy(oldList, neighbor, neighborSize);
    retiredSize = PathEntry(path, g_files.id, TRUE, FALSE, oldList + neighborSize, sizeof oldList - neighborSize - historySize - 1);
    newSize = PathEntry(path, g_files.id, FALSE, FALSE, newEntry, sizeof newEntry);
    if (!Prepared("retired pin: private entries built", retiredSize != 0 && newSize != 0)) return;
    memcpy(oldList + neighborSize + retiredSize, history, historySize);
    oldLen = neighborSize + retiredSize + historySize + 1;
    oldList[oldLen - 1] = 0xFF;
    Check("retired pin: own removal history is not an active pin", !TaskbarPin_ListHasProfile(oldList, oldLen, &g_files.profile));
    hr = TaskbarPin_MergeEntry(oldList, oldLen, newEntry, newSize, path, g_files.id, dropped, merged, sizeof merged, &mergedLen, &merge);
    Check("retired pin: an explicit pin replaces its own retired AppID at its rank", hr == S_OK && merge.rank == 1 && !merge.replacedPin &&
          mergedLen == neighborSize + newSize + historySize + 1 && memcmp(merged + neighborSize, newEntry, newSize) == 0);
    Check("retired pin: the active neighbor and unrelated history stay byte for byte", hr == S_OK &&
          memcmp(merged, neighbor, neighborSize) == 0 && memcmp(merged + neighborSize + newSize, history, historySize) == 0);
    Check("retired pin: three entries remain three with one own AppID", hr == S_OK && merged[mergedLen - 1] == 0xFF &&
          TaskbarPin_ValidateList(merged, mergedLen) && TaskbarPin_ListHasProfile(merged, mergedLen, &g_files.profile));
    recordsLen = Record(records, 3, 0xAD);
    recordsLen += Record(records + recordsLen, 0, 0);
    recordsLen += Record(records + recordsLen, 2, 0xBC);
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, merged, mergedLen, dropped, merge.rank, TRUE, records, recordsLen, &cache, &cacheLen);
    Check("retired pin: records are prepared for the same three entries", hr == S_OK && RecordCount(cache, cacheLen, 3));
    if (hr == S_OK && cache && cacheLen > 11) {
        size_t ownSize = Get32(cache + 7);
        Check("retired pin: the replaced slot gets a record and neighbors stay exact", ownSize > 0 &&
              cacheLen == 7 + 4 + ownSize + 6 && memcmp(cache, records, 7) == 0 && memcmp(cache + 11 + ownSize, records + 11, 6) == 0);
        Check("retired pin: the replaced slot's record resolves to its shortcut", WindowsRecord(cache + 11, ownSize, path));
    }
    FreeBytes(&cache);
}

/* A pin of ours whose shortcut is gone: pinning another profile still works,
 * and the gone pin gets the record Windows would build, from its entry. */
static void TestGonePin(const BYTE *gone, size_t goneSize)
{
    WCHAR other[MAX_PATH], otherId[AUMID_CCH];
    BYTE oldList[8192], newList[16384], zero[4] = { 0 };
    BYTE *cache = NULL;
    size_t oldLen, newLen, otherSize, cacheLen = 0, goneRecord;
    HRESULT hr;
    Core_ProfileAumid(OTHER_FIXTURE_FOLDER, otherId, ARRAYSIZE(otherId));
    if (!Prepared("gone pin: another profile's shortcut written", JoinPath(g_files.pins, L"Claude other fixture.lnk", other) &&
                                                               WriteFixtureLink(other, g_files.target, OTHER_FIXTURE_FOLDER, NULL, 0, NULL)))
        return;
    memcpy(oldList, gone, goneSize);
    oldLen = goneSize + 1;
    oldList[oldLen - 1] = 0xFF;
    memcpy(newList, gone, goneSize);
    otherSize = PathEntry(other, otherId, FALSE, FALSE, newList + goneSize, sizeof newList - goneSize - 1);
    newLen = goneSize + otherSize + 1;
    newList[newLen - 1] = 0xFF;
    hr = TaskbarPin_PrepareRecords(oldList, oldLen, newList, newLen, NULL, 1, TRUE, zero, sizeof zero, &cache, &cacheLen);
    Check("gone pin: another profile can still be pinned", hr == S_OK && otherSize && RecordCount(cache, cacheLen, 2));
    if (hr == S_OK && cache) {
        goneRecord = Get32(cache);
        Check("gone pin: the new pin's record resolves to its shortcut", WindowsRecordAt(cache, cacheLen, 1, other));
        Check("gone pin: the gone pin gets a record in Windows' form", goneRecord >= 0x4C && Get32(cache + 4) == 0x4C &&
              (Get32(cache + 4 + 0x14) & WINDOWS_RECORD_FLAGS) == WINDOWS_RECORD_FLAGS);
    }
    FreeBytes(&cache);
    Prepared("gone pin: another profile's shortcut removed", DeleteFileW(other));
}

/* An Apps folder item carries no file: an active entry of one counts as live. */
static void TestAppsFolderEntry(void)
{
    ClaudePackage pkg;
    WCHAR parsing[512];
    PIDLIST_ABSOLUTE native = NULL;
    PCUITEMID_CHILD last;
    BYTE item[2048], list[4096];
    size_t prefix, cb, len;
    if (FAILED(StringCchPrintfW(parsing, ARRAYSIZE(parsing), L"shell:AppsFolder\\%s",
                                Claude_FindPackage(&pkg) ? pkg.aumid : L"Microsoft.Windows.Explorer")) ||
        FAILED(SHParseDisplayName(parsing, NULL, &native, 0, NULL)) || !native) {
        printf("  skip  Apps folder entry: no Apps folder item to pin\n");
        return;
    }
    last = ILFindLastID(native);
    prefix = (size_t)((const BYTE *)last - (const BYTE *)native);
    cb = TaskbarPin_InjectAppId((const BYTE *)last, last->mkid.cb, g_files.id, item, sizeof item);
    if (Prepared("apps folder: entry built", cb && prefix + cb + 8 <= sizeof list)) {
        list[0] = 0;
        Put32(list + 1, (DWORD)(prefix + cb + 2));
        memcpy(list + 5, native, prefix);
        memcpy(list + 5 + prefix, item, cb);
        Put16(list + 5 + prefix + cb, 0);
        len = prefix + cb + 7;
        list[len++] = 0xFF;
        Check("apps folder: an active Apps folder entry counts as live", TaskbarPin_ListHasAppId(list, len, g_files.id, TRUE));
    }
    ILFree(native);
}

/* Explorer saves its own list while an edit is under way: the values and
 * `saved` trade places. */
static void ExplorerSaves(MemoryRegistry *registry)
{
    static MemoryValue swap[2];
    memcpy(swap, registry->values, sizeof swap);
    memcpy(registry->values, registry->saved, sizeof swap);
    memcpy(registry->saved, swap, sizeof swap);
}

/* Fixture entries of the list edits below. */
typedef struct FixtureEntry {
    BYTE   bytes[4096];
    size_t size;
} FixtureEntry;

static BOOL MakeFixtureEntry(FixtureEntry *entry, const WCHAR *path, const WCHAR *appId, BOOL removed, BOOL damaged)
{
    entry->size = PathEntry(path, appId, removed, damaged, entry->bytes, sizeof entry->bytes);
    return entry->size != 0;
}

/* An edit that finds the list changed when it writes starts over from the
 * new list, three times at most. */
static void TestEditRetries(void)
{
    static MemoryRegistry registry;
    static FixtureEntry ours, otherA, otherB;
    static BYTE readList[FIXTURE_CAPACITY], readRecords[FIXTURE_CAPACITY], savedList[FIXTURE_CAPACITY], savedRecords[FIXTURE_CAPACITY];
    static BYTE expectedList[FIXTURE_CAPACITY], expectedRecords[FIXTURE_CAPACITY];
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    BYTE *ourRecord = NULL, recordA[5], recordB[6];
    size_t ourRecordLen = 0;
    Bytes readListBytes, readRecordBytes, savedListBytes, savedRecordBytes, expectedListBytes, expectedRecordBytes;
    BOOL savesBeforeEveryWrite;
    HRESULT hr;
    memset(recordA, 0xA1, sizeof recordA);
    memset(recordB, 0xB2, sizeof recordB);
    if (!Prepared("retries: entries built", MakeFixtureEntry(&ours, g_files.shortcut, g_files.id, FALSE, FALSE) &&
                                            MakeFixtureEntry(&otherA, g_files.otherFile, L"Unrelated.A", FALSE, FALSE) &&
                                            MakeFixtureEntry(&otherB, g_files.secondOtherFile, L"Unrelated.B", FALSE, FALSE) &&
                                            SUCCEEDED(TaskbarPin_BuildRecord(ours.bytes, ours.size, &ourRecord, &ourRecordLen))))
        goto done;
    /* The list read holds ours, then A; Explorer saves B, ours, A meanwhile. */
    StartBytes(&readListBytes, readList, sizeof readList);
    Append(&readListBytes, ours.bytes, ours.size);
    Append(&readListBytes, otherA.bytes, otherA.size);
    AppendListEnd(&readListBytes);
    StartBytes(&readRecordBytes, readRecords, sizeof readRecords);
    AppendRecord(&readRecordBytes, ourRecord, ourRecordLen);
    AppendRecord(&readRecordBytes, recordA, sizeof recordA);
    StartBytes(&savedListBytes, savedList, sizeof savedList);
    Append(&savedListBytes, otherB.bytes, otherB.size);
    Append(&savedListBytes, ours.bytes, ours.size);
    Append(&savedListBytes, otherA.bytes, otherA.size);
    AppendListEnd(&savedListBytes);
    StartBytes(&savedRecordBytes, savedRecords, sizeof savedRecords);
    AppendRecord(&savedRecordBytes, recordB, sizeof recordB);
    AppendRecord(&savedRecordBytes, ourRecord, ourRecordLen);
    AppendRecord(&savedRecordBytes, recordA, sizeof recordA);
    StartBytes(&expectedListBytes, expectedList, sizeof expectedList);
    Append(&expectedListBytes, otherB.bytes, otherB.size);
    Append(&expectedListBytes, otherA.bytes, otherA.size);
    AppendListEnd(&expectedListBytes);
    StartBytes(&expectedRecordBytes, expectedRecords, sizeof expectedRecords);
    AppendRecord(&expectedRecordBytes, recordB, sizeof recordB);
    AppendRecord(&expectedRecordBytes, recordA, sizeof recordA);
    if (!Prepared("retries: lists fit", !readListBytes.overflow && !readRecordBytes.overflow && !savedListBytes.overflow &&
                                        !savedRecordBytes.overflow && !expectedListBytes.overflow && !expectedRecordBytes.overflow))
        goto done;

    /* Each attempt reads the list twice, and its write twice more: Explorer
     * saves right before the first attempt's write... */
    for (savesBeforeEveryWrite = FALSE; savesBeforeEveryWrite <= TRUE; savesBeforeEveryWrite++) {
        SeedPinValues(&registry, readList, readListBytes.len, readRecords, readRecordBytes.len);
        StoreValue(&registry.saved[0], REG_BINARY, savedList, (DWORD)savedListBytes.len);
        StoreValue(&registry.saved[1], REG_BINARY, savedRecords, (DWORD)savedRecordBytes.len);
        registry.changedValue = LIST_VALUE;
        registry.changeAtReads[0] = 3;
        if (savesBeforeEveryWrite) {
            /* ...or before every attempt's write. */
            registry.changeAtReads[1] = 7;
            registry.changeAtReads[2] = 11;
        }
        registry.change = ExplorerSaves;
        hr = TaskbarPin_RemoveFromList(&io, NULL, 0);
        if (!savesBeforeEveryWrite) {
            Check("retries: a list changed before the write is edited again from the new one", hr == S_OK &&
                  MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, expectedList, expectedListBytes.len) &&
                  MemoryValueIs(&registry, RECORDS_VALUE, REG_BINARY, expectedRecords, expectedRecordBytes.len));
            Check("retries: the second attempt writes once", registry.reads[LIST_VALUE] == 8 && registry.writes == 4 && CounterIs(&registry, 41));
        } else {
            Check("retries: three attempts at most, writing nothing", hr == E_CHANGED_STATE && registry.reads[LIST_VALUE] == 12 &&
                  registry.writes == 0 && CounterIs(&registry, 40));
        }
    }
done:
    FreeBytes(&ourRecord);
}

/* At uninstall our active entries and the entries of our shortcuts leave the
 * list; retired entries and other applications' stay, records aligned. */
static void TestRemoveAtUninstall(void)
{
    static MemoryRegistry registry;
    static FixtureEntry ours, otherActive, oursRetired, ourShortcut, otherRetired;
    static BYTE list[FIXTURE_CAPACITY], records[FIXTURE_CAPACITY], expectedList[FIXTURE_CAPACITY], expectedRecords[FIXTURE_CAPACITY];
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    WCHAR shortcuts[1][MAX_PATH];
    BYTE recordOurs[4], recordOther[5], recordRetired[3], recordShortcut[4], recordOtherRetired[2];
    Bytes listBytes, recordBytes, expectedListBytes, expectedRecordBytes;
    HRESULT hr;
    memset(recordOurs, 0x10, sizeof recordOurs);
    memset(recordOther, 0xA1, sizeof recordOther);
    memset(recordRetired, 0xC3, sizeof recordRetired);
    memset(recordShortcut, 0xD4, sizeof recordShortcut);
    memset(recordOtherRetired, 0xE5, sizeof recordOtherRetired);
    if (!Prepared("uninstall: entries built", MakeFixtureEntry(&ours, g_files.shortcut, g_files.id, FALSE, FALSE) &&
                                              MakeFixtureEntry(&otherActive, g_files.otherFile, L"Unrelated.A", FALSE, FALSE) &&
                                              MakeFixtureEntry(&oursRetired, g_files.older, g_files.id, TRUE, FALSE) &&
                                              MakeFixtureEntry(&ourShortcut, g_files.elsewhere, L"Unrelated.Pinned.Shortcut", FALSE, FALSE) &&
                                              MakeFixtureEntry(&otherRetired, g_files.secondOtherFile, L"Unrelated.B", TRUE, FALSE)))
        return;
    StartBytes(&listBytes, list, sizeof list);
    Append(&listBytes, ours.bytes, ours.size);
    Append(&listBytes, otherActive.bytes, otherActive.size);
    Append(&listBytes, oursRetired.bytes, oursRetired.size);
    Append(&listBytes, ourShortcut.bytes, ourShortcut.size);
    Append(&listBytes, otherRetired.bytes, otherRetired.size);
    AppendListEnd(&listBytes);
    StartBytes(&recordBytes, records, sizeof records);
    AppendRecord(&recordBytes, recordOurs, sizeof recordOurs);
    AppendRecord(&recordBytes, recordOther, sizeof recordOther);
    AppendRecord(&recordBytes, recordRetired, sizeof recordRetired);
    AppendRecord(&recordBytes, recordShortcut, sizeof recordShortcut);
    AppendRecord(&recordBytes, recordOtherRetired, sizeof recordOtherRetired);
    StartBytes(&expectedListBytes, expectedList, sizeof expectedList);
    Append(&expectedListBytes, otherActive.bytes, otherActive.size);
    Append(&expectedListBytes, oursRetired.bytes, oursRetired.size);
    Append(&expectedListBytes, otherRetired.bytes, otherRetired.size);
    AppendListEnd(&expectedListBytes);
    StartBytes(&expectedRecordBytes, expectedRecords, sizeof expectedRecords);
    AppendRecord(&expectedRecordBytes, recordOther, sizeof recordOther);
    AppendRecord(&expectedRecordBytes, recordRetired, sizeof recordRetired);
    AppendRecord(&expectedRecordBytes, recordOtherRetired, sizeof recordOtherRetired);
    if (!Prepared("uninstall: lists fit", !listBytes.overflow && !recordBytes.overflow && !expectedListBytes.overflow && !expectedRecordBytes.overflow) ||
        FAILED(StringCchCopyW(shortcuts[0], MAX_PATH, g_files.elsewhere)))
        return;
    SeedPinValues(&registry, list, listBytes.len, records, recordBytes.len);
    hr = TaskbarPin_RemoveFromList(&io, shortcuts, 1);
    Check("uninstall: our active entries and our shortcuts' leave, the others stay in order", hr == S_OK &&
          MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, expectedList, expectedListBytes.len));
    Check("uninstall: the remaining records stay byte for byte and aligned",
          MemoryValueIs(&registry, RECORDS_VALUE, REG_BINARY, expectedRecords, expectedRecordBytes.len));
    Check("uninstall: the counter moves on", CounterIs(&registry, 41));
    hr = TaskbarPin_RemoveFromList(&io, shortcuts, 1);
    Check("uninstall: nothing more to remove the second time", hr == S_FALSE && registry.writes == 4);
}

/* Our entries are repaired inside the list, their records rebuilt for the
 * repaired entry; other applications' stay as they are. */
static void TestRepairInList(void)
{
    static MemoryRegistry registry;
    static FixtureEntry otherDamaged, oursDamaged, oursWhole;
    static BYTE list[FIXTURE_CAPACITY], records[FIXTURE_CAPACITY];
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    BYTE recordOther[5], *damagedRecord = NULL;
    size_t damagedRecordLen = 0;
    const MemoryValue *repaired;
    Bytes listBytes, recordBytes;
    HRESULT hr;
    memset(recordOther, 0xA1, sizeof recordOther);
    if (!Prepared("repair: entries built", MakeFixtureEntry(&otherDamaged, g_files.otherFile, L"Unrelated.A", FALSE, TRUE) &&
                                           MakeFixtureEntry(&oursDamaged, g_files.shortcut, g_files.id, FALSE, TRUE) &&
                                           MakeFixtureEntry(&oursWhole, g_files.shortcut, g_files.id, FALSE, FALSE) &&
                                           SUCCEEDED(TaskbarPin_BuildRecord(oursDamaged.bytes, oursDamaged.size, &damagedRecord, &damagedRecordLen))))
        goto done;
    StartBytes(&listBytes, list, sizeof list);
    Append(&listBytes, otherDamaged.bytes, otherDamaged.size);
    Append(&listBytes, oursDamaged.bytes, oursDamaged.size);
    AppendListEnd(&listBytes);
    StartBytes(&recordBytes, records, sizeof records);
    AppendRecord(&recordBytes, recordOther, sizeof recordOther);
    AppendRecord(&recordBytes, damagedRecord, damagedRecordLen);
    if (!Prepared("repair: lists fit", !listBytes.overflow && !recordBytes.overflow)) goto done;
    Check("repair: the damaged entry still names our AppID", TaskbarPin_ListHasAppId(list, listBytes.len, g_files.id, FALSE));
    SeedPinValues(&registry, list, listBytes.len, records, recordBytes.len);
    hr = TaskbarPin_RepairList(&io);
    repaired = &registry.values[LIST_VALUE];
    Check("repair: our entry is repaired in place, another application's left as it is", hr == S_OK &&
          repaired->bytes == otherDamaged.size + oursWhole.size + 1 && memcmp(repaired->data, otherDamaged.bytes, otherDamaged.size) == 0 &&
          memcmp(repaired->data + otherDamaged.size, oursWhole.bytes, oursWhole.size) == 0);
    Check("repair: another application's record stays byte for byte",
          RecordAtIs(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 0, recordOther, sizeof recordOther));
    Check("repair: our record is rebuilt for the repaired entry",
          WindowsRecordAt(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 1, g_files.shortcut) &&
          !RecordAtIs(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 1, damagedRecord, damagedRecordLen));
    hr = TaskbarPin_RepairList(&io);
    Check("repair: a repaired list needs nothing more", hr == S_FALSE && registry.writes == 4);
done:
    FreeBytes(&damagedRecord);
}

/* Pinning drops the profile's other active entries; only their shortcuts in
 * the pins folder, never the one pinned, are to be deleted. */
static void TestAddDropsShortcuts(void)
{
    static MemoryRegistry registry;
    static FixtureEntry replaced, droppedInPins, droppedElsewhere, sameShortcut, other, fresh;
    static TaskbarPin_Addition addition;
    static BYTE list[FIXTURE_CAPACITY], records[FIXTURE_CAPACITY];
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    TaskbarPin_CommitResult result;
    WCHAR oldest[MAX_PATH];
    BYTE filled[5];
    Bytes listBytes, recordBytes;
    const MemoryValue *added;
    int i;
    HRESULT hr;
    memset(filled, 0x5A, sizeof filled);
    if (!Prepared("add: the profile's oldest shortcut written", JoinPath(g_files.pins, L"Claude oldest.lnk", oldest) &&
                                                             WriteFixtureLink(oldest, g_files.target, g_files.profile.folder, NULL, 0, NULL)) ||
        !Prepared("add: entries built", MakeFixtureEntry(&replaced, oldest, g_files.id, FALSE, FALSE) &&
                                        MakeFixtureEntry(&droppedInPins, g_files.older, g_files.id, FALSE, FALSE) &&
                                        MakeFixtureEntry(&droppedElsewhere, g_files.elsewhere, g_files.id, FALSE, FALSE) &&
                                        MakeFixtureEntry(&sameShortcut, g_files.shortcut, L"Unrelated.Same.Shortcut", FALSE, FALSE) &&
                                        MakeFixtureEntry(&other, g_files.otherFile, L"Unrelated.A", FALSE, FALSE) &&
                                        MakeFixtureEntry(&fresh, g_files.shortcut, g_files.id, FALSE, FALSE)))
        return;
    StartBytes(&listBytes, list, sizeof list);
    Append(&listBytes, replaced.bytes, replaced.size);
    Append(&listBytes, droppedInPins.bytes, droppedInPins.size);
    Append(&listBytes, droppedElsewhere.bytes, droppedElsewhere.size);
    Append(&listBytes, sameShortcut.bytes, sameShortcut.size);
    Append(&listBytes, other.bytes, other.size);
    AppendListEnd(&listBytes);
    StartBytes(&recordBytes, records, sizeof records);
    for (i = 0; i < 5; i++) AppendRecord(&recordBytes, filled, sizeof filled);
    if (!Prepared("add: lists fit", !listBytes.overflow && !recordBytes.overflow)) return;
    SeedPinValues(&registry, list, listBytes.len, records, recordBytes.len);
    ZeroMemory(&addition, sizeof addition);
    addition.entry = fresh.bytes;
    addition.entrySize = (DWORD)fresh.size;
    addition.shortcut = g_files.shortcut;
    addition.aumid = g_files.id;
    addition.pinsDir = g_files.pins;
    hr = TaskbarPin_AddToList(&io, &addition, &result);
    added = &registry.values[LIST_VALUE];
    Check("add: the new entry takes the first active entry's place, the others of the profile leave", hr == S_OK &&
          result.listCommitted && addition.replacedPin && added->bytes == fresh.size + other.size + 1 &&
          memcmp(added->data, fresh.bytes, fresh.size) == 0 && memcmp(added->data + fresh.size, other.bytes, other.size) == 0);
    Check("add: only a dropped shortcut in the pins folder is to be deleted, never the one pinned",
          addition.obsoleteShortcutCount == 1 && Core_PathEquals(addition.obsoleteShortcuts[0], g_files.older));
    Check("add: the new entry gets its record, the other application's stays",
          RecordCount(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 2) &&
          WindowsRecordAt(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 0, g_files.shortcut) &&
          RecordAtIs(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 1, filled, sizeof filled));
    Prepared("add: the profile's oldest shortcut removed", DeleteFileW(oldest));
}

/* The first pin of all, over a missing or an empty list; records of another
 * type refuse the edit; a failed write is reported back. */
static void TestAddEdgeCases(void)
{
    static MemoryRegistry registry;
    static FixtureEntry fresh;
    static TaskbarPin_Addition addition;
    static BYTE expectedList[FIXTURE_CAPACITY];
    TaskbarPin_ValueIO io = MemoryAccess(&registry);
    TaskbarPin_CommitResult result;
    Bytes expectedListBytes;
    BOOL emptyList;
    HRESULT hr;
    if (!Prepared("first pin: entry built", MakeFixtureEntry(&fresh, g_files.shortcut, g_files.id, FALSE, FALSE))) return;
    StartBytes(&expectedListBytes, expectedList, sizeof expectedList);
    Append(&expectedListBytes, fresh.bytes, fresh.size);
    AppendListEnd(&expectedListBytes);
    if (!Prepared("first pin: list fits", !expectedListBytes.overflow)) return;
    ZeroMemory(&addition, sizeof addition);
    addition.entry = fresh.bytes;
    addition.entrySize = (DWORD)fresh.size;
    addition.shortcut = g_files.shortcut;
    addition.aumid = g_files.id;
    addition.pinsDir = g_files.pins;
    for (emptyList = FALSE; emptyList <= TRUE; emptyList++) {
        SeedPinValues(&registry, emptyList ? expectedList : NULL, 0, NULL, 0);
        hr = TaskbarPin_AddToList(&io, &addition, &result);
        Check(emptyList ? "first pin: over an empty list, the list and its record are written"
                        : "first pin: over a missing list, the list and its record are written",
              hr == S_OK && result.listCommitted && !addition.replacedPin &&
              MemoryValueIs(&registry, LIST_VALUE, REG_BINARY, expectedList, expectedListBytes.len) &&
              RecordCount(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 1) &&
              WindowsRecordAt(registry.values[RECORDS_VALUE].data, registry.values[RECORDS_VALUE].bytes, 0, g_files.shortcut));
    }

    SeedPinValues(&registry, NULL, 0, NULL, 0);
    StoreValue(&registry.values[RECORDS_VALUE], REG_SZ, L"x", sizeof L"x");
    hr = TaskbarPin_AddToList(&io, &addition, &result);
    Check("first pin: records of another type refuse the edit, writing nothing", hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) &&
          registry.writes == 0);

    SeedPinValues(&registry, NULL, 0, NULL, 0);
    registry.failures[0] = 2;
    hr = TaskbarPin_AddToList(&io, &addition, &result);
    Check("first pin: a failed write rolled back is reported as not committed", hr == HRESULT_FROM_WIN32(ERROR_WRITE_FAULT) &&
          !result.listCommitted && !result.uncertain && !registry.values[LIST_VALUE].exists && !registry.values[RECORDS_VALUE].exists);
    SeedPinValues(&registry, NULL, 0, NULL, 0);
    registry.failures[0] = 4;
    registry.failures[1] = 7;
    hr = TaskbarPin_AddToList(&io, &addition, &result);
    Check("first pin: a list that could not be taken back is reported as committed and uncertain", FAILED(hr) &&
          result.listCommitted && result.uncertain);
}

/* A shortcut left in the pins folder: saved in place when it starts the
 * installed copy with the profile's ID, written again otherwise. The profile's
 * icon is drawn on Claude's artwork, read from its installed files; this test
 * has no artwork of the manager's own. */
static void TestLeftoverShortcut(void)
{
    ClaudePackage pkg;
    WCHAR installed[MAX_PATH], leftover[MAX_PATH], icon[MAX_PATH], linkIcon[MAX_PATH], description[MAX_PATH];
    LinkInfo info;
    int showCommand = 0;
    HRESULT hr;
    Claude_FindPackage(&pkg);
    if (!Prepared("leftover: the installed copy's path", Util_InstallExe(installed, ARRAYSIZE(installed))) ||
        !Prepared("leftover: its path in the pins folder", JoinPath(g_files.pins, L"Claude leftover.lnk", leftover)))
        return;
    if (!Icons_Ensure(&pkg, &g_files.profile, icon, ARRAYSIZE(icon))) {
        printf("  skip  leftover saved in place: no profile icon without Claude Desktop installed\n");
    } else if (Prepared("leftover: a current leftover written",
                        WriteFixtureLink(leftover, installed, g_files.profile.folder, g_files.id, SW_SHOWMINNOACTIVE, NULL))) {
        hr = TaskbarPin_UpdateLeftover(&pkg, &g_files.profile, leftover);
        Check("leftover: a current one is saved in place, keeping what it holds", hr == S_OK &&
              ReadLinkState(leftover, &showCommand, linkIcon, description) && showCommand == SW_SHOWMINNOACTIVE &&
              Core_PathEquals(linkIcon, icon) && description[0]);
    }
    if (!Prepared("leftover: a leftover of another copy written",
                  WriteFixtureLink(leftover, g_files.target, g_files.profile.folder, g_files.id, SW_SHOWMINNOACTIVE, NULL)))
        return;
    hr = TaskbarPin_UpdateLeftover(&pkg, &g_files.profile, leftover);
    Check("leftover: one of another copy is written again for the installed copy", hr == S_OK &&
          Shortcut_Read(leftover, &info) && Core_PathEquals(info.target, installed) && Shortcut_HasAppId(leftover, g_files.id) &&
          ReadLinkState(leftover, &showCommand, linkIcon, description) && showCommand == SW_SHOWNORMAL);
    Prepared("leftover: removed", DeleteFileW(leftover));
}

/* The stock profile's pin may be Claude's own Apps folder item. */
static void TestStockPins(void)
{
    ClaudePackage pkg;
    WCHAR parsing[512];
    PIDLIST_ABSOLUTE native = NULL;
    Profile stock = g_files.profile;
    BYTE list[8192];
    size_t n;
    HRESULT hr;
    if (!Claude_FindPackage(&pkg)) {
        printf("  skip  stock pins: Claude Desktop is not installed\n");
        return;
    }
    StringCchCopyW(stock.folder, ARRAYSIZE(stock.folder), STOCK_FOLDER);
    stock.isStock = TRUE;
    StringCchPrintfW(parsing, ARRAYSIZE(parsing), L"shell:AppsFolder\\%s", pkg.aumid);
    hr = SHParseDisplayName(parsing, NULL, &native, 0, NULL);
    if (!Prepared("stock: the installed Apps folder item resolves without a launch", SUCCEEDED(hr) && native != NULL)) return;
    n = ILGetSize(native);
    if (Prepared("stock: the Apps folder entry fits", n + 6 <= sizeof list)) {
        list[0] = 0;
        Put32(list + 1, (DWORD)n);
        memcpy(list + 5, native, n);
        list[5 + n] = 0xFF;
        Check("stock: a direct Apps folder pin is detected", TaskbarPin_ListHasProfile(list, n + 6, &stock));
        Check("stock: an Apps folder pin does not select another profile", !TaskbarPin_ListHasProfile(list, n + 6, &g_files.profile));
    }
    if (Prepared("stock: a shortcut to Claude's item written", WriteFixtureLink(g_files.shortcut, NULL, NULL, NULL, 0, native))) {
        n = PathEntry(g_files.shortcut, pkg.aumid, FALSE, FALSE, list, sizeof list - 1);
        if (Prepared("stock: its entry built", n != 0)) {
            list[n++] = 0xFF;
            Check("stock: an existing shortcut to Claude's item is detected", TaskbarPin_ListHasProfile(list, n, &stock));
        }
    }
    ILFree(native);
}

static void TestProfilePins(void)
{
    BYTE list[8192], active[8192], out[16384];
    TaskbarPin_Merge merge;
    size_t n, activeSize, outLen;
    HRESULT hr;
    n = PathEntry(g_files.shortcut, g_files.id, FALSE, FALSE, list, sizeof list - 1);
    if (!Prepared("profile: the pin's entry built", n != 0)) return;
    activeSize = n;
    memcpy(active, list, n);
    TestRecords(active, activeSize);
    if (!TestMissingPin(active, activeSize)) return;
    TestRetiredPin();
    list[n++] = 0xFF;
    Check("profile: an active existing shortcut is pinned", TaskbarPin_ListHasProfile(list, n, &g_files.profile));
    Check("profile: an active pin whose shortcut exists is live", TaskbarPin_ListHasAppId(list, n, g_files.id, TRUE));
    Check("profile: a shortcut for another profile is not pinned", !TaskbarPin_ListHasProfile(list, n, &g_files.other));
    hr = TaskbarPin_MergeEntry(list, n, active, activeSize, g_files.shortcut, L"Different.AppID", NULL, out, sizeof out, &outLen, &merge);
    Check("profile: an active entry of the same shortcut is replaced, without a duplicate", hr == S_OK && outLen == n &&
          merge.rank == 0 && merge.replacedPin);
    list[0] = 0xFF;
    Check("profile: an orphan shortcut is not pinned", !TaskbarPin_ListHasProfile(list, 1, &g_files.profile));
    n = PathEntry(g_files.shortcut, g_files.id, TRUE, FALSE, list, sizeof list - 1);
    if (Prepared("profile: a retired entry built", n != 0)) {
        list[n++] = 0xFF;
        Check("profile: an existing retired shortcut is not pinned", !TaskbarPin_ListHasProfile(list, n, &g_files.profile));
        hr = TaskbarPin_MergeEntry(list, n, active, activeSize, g_files.shortcut, g_files.id, NULL, out, sizeof out, &outLen, &merge);
        Check("profile: removal history does not block a file name or an AppID", hr == S_OK && TaskbarPin_ListHasProfile(out, outLen, &g_files.profile));
    }
    TestAppsFolderEntry();
    TestEditRetries();
    TestRemoveAtUninstall();
    TestRepairInList();
    TestAddDropsShortcuts();
    TestAddEdgeCases();
    TestLeftoverShortcut();
    TestStockPins();
    memcpy(list, active, activeSize);
    list[activeSize] = 0xFF;
    if (Prepared("profile: private shortcut deleted", DeleteFileW(g_files.shortcut))) {
        Check("profile: a missing shortcut is not pinned", !TaskbarPin_ListHasProfile(list, activeSize + 1, &g_files.profile));
        Check("profile: a pin whose shortcut is gone keeps its AppID but is not live", TaskbarPin_ListHasAppId(list, activeSize + 1, g_files.id, FALSE) &&
              !TaskbarPin_ListHasAppId(list, activeSize + 1, g_files.id, TRUE));
    }
    TestGonePin(active, activeSize);
}

/* The private folder and files: the log and the icons go there too. */
static BOOL CreatePinFiles(void)
{
    WCHAR temp[MAX_PATH], made[MAX_PATH], exe[MAX_PATH];
    DWORD cch = GetTempPathW(ARRAYSIZE(temp), temp);
    ZeroMemory(&g_files, sizeof g_files);
    if (!Prepared("files: temporary folder", cch != 0 && cch < ARRAYSIZE(temp)) ||
        !Prepared("files: private folder made", SUCCEEDED(StringCchPrintfW(made, ARRAYSIZE(made), L"%sClaudeProfilesPinFixture-%lu-%llu", temp,
                                                                           GetCurrentProcessId(), GetTickCount64())) &&
                                                 CreateDirectoryW(made, NULL)))
        return FALSE;
    cch = GetLongPathNameW(made, g_files.root, ARRAYSIZE(g_files.root));
    if (!Prepared("files: private folder's full path", cch != 0 && cch < ARRAYSIZE(g_files.root))) {
        RemoveDirectoryW(made);
        g_files.root[0] = 0;
        return FALSE;
    }
    if (!Prepared("files: private state folder made", JoinPath(g_files.root, L"state", g_files.state) && CreateDirectoryW(g_files.state, NULL)))
        return FALSE;
    Util_SetStateDir(g_files.state);
    StringCchCopyW(g_files.profile.folder, ARRAYSIZE(g_files.profile.folder), FIXTURE_FOLDER);
    StringCchCopyW(g_files.profile.name, ARRAYSIZE(g_files.profile.name), L"Pin fixture");
    StringCchCopyW(g_files.profile.dataDir, ARRAYSIZE(g_files.profile.dataDir), L"C:\\Fixture\\" FIXTURE_FOLDER);
    g_files.other = g_files.profile;
    StringCchCopyW(g_files.other.folder, ARRAYSIZE(g_files.other.folder), OTHER_FIXTURE_FOLDER);
    StringCchCopyW(g_files.other.dataDir, ARRAYSIZE(g_files.other.dataDir), L"C:\\Fixture\\" OTHER_FIXTURE_FOLDER);
    Core_ProfileAumid(g_files.profile.folder, g_files.id, ARRAYSIZE(g_files.id));
    return Prepared("files: private folders made", JoinPath(g_files.root, L"pins", g_files.pins) && CreateDirectoryW(g_files.pins, NULL) &&
                                                   JoinPath(g_files.root, L"away", g_files.away) && CreateDirectoryW(g_files.away, NULL)) &&
           Prepared("files: a private copy of this exe, never started", GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe)) != 0 &&
                                                                        JoinPath(g_files.root, L"ClaudeDesktopProfilesManager.exe", g_files.target) &&
                                                                        CopyFileW(exe, g_files.target, TRUE)) &&
           Prepared("files: the profile's shortcuts written", JoinPath(g_files.pins, L"Claude fixture.lnk", g_files.shortcut) &&
                                                              WriteFixtureLink(g_files.shortcut, g_files.target, FIXTURE_FOLDER, NULL, 0, NULL) &&
                                                              JoinPath(g_files.pins, L"Claude older.lnk", g_files.older) &&
                                                              WriteFixtureLink(g_files.older, g_files.target, FIXTURE_FOLDER, NULL, 0, NULL) &&
                                                              JoinPath(g_files.away, L"Claude elsewhere.lnk", g_files.elsewhere) &&
                                                              WriteFixtureLink(g_files.elsewhere, g_files.target, FIXTURE_FOLDER, NULL, 0, NULL)) &&
           Prepared("files: other applications' files written", JoinPath(g_files.root, L"other-a.txt", g_files.otherFile) &&
                                                                 WriteEmptyFile(g_files.otherFile) &&
                                                                 JoinPath(g_files.root, L"other-b.txt", g_files.secondOtherFile) &&
                                                                 WriteEmptyFile(g_files.secondOtherFile));
}

int wmain(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL files;
    Prepared("COM initialized", SUCCEEDED(hr));
    files = CreatePinFiles();
    TestInject();
    TestRepair();
    TestRecordAlignment();
    TestList();
    TestListRead();
    TestCommitFailures();
    TestWriteOrder();
    TestSnapshotRereads();
    TestRegistryWrite();
    if (SUCCEEDED(hr) && files) TestProfilePins();
    if (g_files.root[0]) Prepared("files: private folder removed", DeleteTree(g_files.root));
    if (SUCCEEDED(hr)) CoUninitialize();
    printf("Pin tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
