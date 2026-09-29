/*
 * Unit tests for the Favorites entry helpers of src/taskbar-pin.c. Built and
 * run by build.cmd (linked with the program's objects); exits non-zero when a
 * check fails.
 */
#include "../src/app.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0, g_checks = 0;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

static WORD Get16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static void Put16(BYTE *p, WORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void Put32(BYTE *p, DWORD v) { Put16(p, (WORD)v); Put16(p + 2, (WORD)(v >> 16)); }

/* A file item: a 22-byte head, then a 64-byte BEEF0004 block ending with the
 * first block's offset (22). */
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

/* A block of `sig` holding `text`, ending with the first block's offset when
 * `withOffset`. */
static size_t Block(BYTE *out, DWORD sig, const WCHAR *text, BOOL withOffset)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR), size = 10 + bytes + (withOffset ? 2 : 0);
    Put16(out, (WORD)size);
    Put16(out + 2, 0);
    Put32(out + 4, sig);
    Put16(out + 8, 2);
    memcpy(out + 10, text, bytes);
    if (withOffset) Put16(out + size - 2, 22);
    return size;
}

/* The blocks' signatures, low words, in chain order; -1 when the chain from
 * the item's last WORD does not end exactly at the item's end. */
static int Chain(const BYTE *item, WORD *sigs, int max)
{
    size_t cb = Get16(item), pos = Get16(item + cb - 2);
    int n = 0;
    while (pos + 8 <= cb && Get16(item + pos) >= 8 && pos + Get16(item + pos) <= cb &&
           Get16(item + pos + 6) == 0xBEEF && n < max) {
        sigs[n++] = Get16(item + pos + 4);
        pos += Get16(item + pos);
    }
    return pos == cb ? n : -1;
}

static void TestInject(void)
{
    BYTE item[512], out[512];
    WORD sigs[8];
    size_t cb = FileItem(item), n;
    const WCHAR *id = L"ClaudeDesktopProfilesManager.Profile.1234ABCD";

    n = TaskbarPin_InjectAppId(item, cb, id, out, sizeof out);
    Check("inject: size", n == cb + 12 + (wcslen(id) + 1) * 2 && Get16(out) == n);
    Check("inject: chain 0004, 001D", Chain(out, sigs, 8) == 2 && sigs[0] == 0x0004 && sigs[1] == 0x001D);
    Check("inject: the block ends with the first block's offset", Get16(out + n - 2) == 22);
    Check("inject: the app ID", memcmp(out + cb + 10, id, (wcslen(id) + 1) * 2) == 0);
    Check("inject: a well-formed result needs no repair", TaskbarPin_RepairItem(out, n, item, sizeof item) == 0);
    Check("inject: too small a buffer fails", TaskbarPin_InjectAppId(item, cb, id, out, 100) == 0);
}

static void TestRepair(void)
{
    BYTE item[1024], out[1024];
    WORD sigs[16];
    size_t cb, n, i;
    const WCHAR *id = L"C:\\Windows\\System32\\winver.exe";

    /* BEEF001D written without its final WORD, the WORD after it. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, FALSE);
    Put16(item + cb, 22);
    cb += 2;
    Put16(item, (WORD)cb);
    n = TaskbarPin_RepairItem(item, cb, out, sizeof out);
    Check("stray WORD at the end: repaired, same size", n == cb);
    Check("stray WORD at the end: chain 0004, 001D", n && Chain(out, sigs, 16) == 2 && sigs[1] == 0x001D);

    /* ...then three BEEF001E blocks Windows appended after the WORD. */
    for (i = 0; i < 3; i++) cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    Put16(item, (WORD)cb);
    Check("broken chain as Windows reads it", Chain(item, sigs, 16) == -1);
    n = TaskbarPin_RepairItem(item, cb, out, sizeof out);
    Check("duplicates dropped", n == cb - 2 * 34);
    Check("repaired chain 0004, 001D, 001E", n && Chain(out, sigs, 16) == 3 && sigs[0] == 0x0004 && sigs[1] == 0x001D && sigs[2] == 0x001E);
    Check("app ID kept", n && memcmp(out + 86 + 10, id, (wcslen(id) + 1) * 2) == 0);
    Check("repaired item size field", n && Get16(out) == n);
    Check("a repaired item needs no more repair", n && TaskbarPin_RepairItem(out, n, item, sizeof item) == 0);

    /* A whole chain is left as it is, even with a repeated block. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    Put16(item, (WORD)cb);
    Check("whole chain with a repeated block untouched", TaskbarPin_RepairItem(item, cb, out, sizeof out) == 0);

    /* Broken chain: only blocks repeated byte for byte are dropped. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, FALSE);
    Put16(item + cb, 22);
    cb += 2;
    cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    cb += Block(item + cb, 0xBEEF001Eu, L"OEMPinned", TRUE);
    cb += Block(item + cb, 0xBEEF001Eu, L"UserPinned", TRUE);
    Put16(item, (WORD)cb);
    n = TaskbarPin_RepairItem(item, cb, out, sizeof out);
    Check("broken chain: identical blocks kept once, different ones kept", n == cb - 34 && Chain(out, sigs, 16) == 4);

    /* Well-formed: nothing to do. Unknown layout: left alone. */
    cb = FileItem(item);
    cb += Block(item + cb, 0xBEEF001Du, id, TRUE);
    Put16(item, (WORD)cb);
    Check("well-formed item untouched", TaskbarPin_RepairItem(item, cb, out, sizeof out) == 0);
    Put16(item + 86 + 6, 0x1234);
    Check("unknown layout untouched", TaskbarPin_RepairItem(item, cb, out, sizeof out) == 0);
    Check("tiny item untouched", TaskbarPin_RepairItem(item, 6, out, sizeof out) == 0);
}

/* Appends a FavoritesResolve record of `n` bytes filled with `fill`. */
static size_t Record(BYTE *out, DWORD n, BYTE fill)
{
    Put32(out, n);
    memset(out + 4, fill, n);
    return 4 + n;
}

static void TestFitResolve(void)
{
    BYTE res[64], out[64], expect[64];
    BOOL dropped[3] = { FALSE, TRUE, FALSE };
    size_t len = 0, n = 0, e;

    len += Record(res + len, 3, 0xAA);
    len += Record(res + len, 0, 0);
    len += Record(res + len, 2, 0xBB);

    e = Record(expect, 3, 0xAA);
    e += Record(expect + e, 2, 0xBB);
    Check("resolve: dropped entry's record removed",
          TaskbarPin_FitResolve(res, len, dropped, 3, 2, out, sizeof out, &n) && n == e && memcmp(out, expect, e) == 0);

    Check("resolve: added entry gets an empty record",
          TaskbarPin_FitResolve(res, len, NULL, 3, 4, out, sizeof out, &n) && n == len + 4 &&
          memcmp(out, res, len) == 0 && Get16(out + len) == 0 && Get16(out + len + 2) == 0);

    Check("resolve: cut to the entry count",
          TaskbarPin_FitResolve(res, len, NULL, 3, 1, out, sizeof out, &n) && n == 7 && memcmp(out, res, 7) == 0);

    Check("resolve: unchanged list kept as it is",
          TaskbarPin_FitResolve(res, len, NULL, 3, 3, out, sizeof out, &n) && n == len && memcmp(out, res, len) == 0);

    Put32(res + 7, 100);   /* the second record runs past the end */
    Check("resolve: a record past the end ends the list",
          TaskbarPin_FitResolve(res, len, NULL, 3, 2, out, sizeof out, &n) && n == 11 && memcmp(out, res, 7) == 0 &&
          Get16(out + 7) == 0 && Get16(out + 9) == 0);

    Check("resolve: no entries, no records", TaskbarPin_FitResolve(res, len, NULL, 3, 0, out, sizeof out, &n) && n == 0);
    Check("resolve: too small a buffer fails", !TaskbarPin_FitResolve(res, 7, NULL, 1, 1, out, 6, &n));
}

int wmain(void)
{
    TestInject();
    TestRepair();
    TestFitResolve();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
