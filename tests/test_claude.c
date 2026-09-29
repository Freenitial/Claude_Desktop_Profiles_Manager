/*
 * Checks that the installed Claude Desktop, and the Claude Code it runs, still
 * work the way docs/HOW-IT-WORKS.md describes: each fact Claude Desktop
 * Profiles Manager relies on is looked up in Claude's own files (its app.asar,
 * its Claude.exe, the Claude Code binary it installs). Built and run by
 * build.cmd, linked with the program's objects. Skipped when Claude is not
 * installed; exits non-zero when Claude changed one of these facts, so a
 * Claude update that breaks an assumption shows at the next build.
 */
#include "../src/app.h"
#include <stdio.h>
#include <string.h>

typedef struct Fact {
    const char *what;      /* the fact, as the documentation states it */
    const char *text;      /* what shows it in Claude's file */
    BOOL        wide;      /* looked for as UTF-16 (strings of the native exe) */
    BOOL        found;
} Fact;

static int g_failures = 0, g_checks = 0;

static const BYTE *Find(const BYTE *hay, size_t n, const BYTE *needle, size_t len)
{
    const BYTE *p = hay, *end = hay + n;
    if (len == 0 || n < len) return NULL;
    while ((p = (const BYTE *)memchr(p, needle[0], (size_t)(end - p) - len + 1)) != NULL) {
        if (memcmp(p, needle, len) == 0) return p;
        if (++p > end - len) break;
    }
    return NULL;
}

static size_t Pattern(const Fact *f, BYTE *out, size_t cap)
{
    size_t n = strlen(f->text), i;
    if (!f->wide) {
        if (n > cap) return 0;
        memcpy(out, f->text, n);
        return n;
    }
    if (n * 2 > cap) return 0;
    for (i = 0; i < n; i++) {
        out[2 * i] = (BYTE)f->text[i];
        out[2 * i + 1] = 0;
    }
    return n * 2;
}

/* Looks for every fact in the file, in chunks that overlap by more than the
 * longest pattern. FALSE when the file cannot be read. */
static BOOL Scan(const WCHAR *path, Fact *facts, int count)
{
    const DWORD chunk = 16u * 1024u * 1024u, overlap = 1024;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    BYTE *buf, pat[512];
    DWORD keep = 0, got;
    int i;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, chunk + overlap);
    if (!buf) {
        CloseHandle(h);
        return FALSE;
    }
    while (ReadFile(h, buf + keep, chunk, &got, NULL) && got > 0) {
        size_t n = keep + got;
        for (i = 0; i < count; i++) {
            size_t len;
            if (facts[i].found || (len = Pattern(&facts[i], pat, sizeof pat)) == 0) continue;
            if (Find(buf, n, pat, len)) facts[i].found = TRUE;
        }
        keep = n > overlap ? overlap : (DWORD)n;
        memmove(buf, buf + n - keep, keep);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
    return TRUE;
}

static void Report(const WCHAR *file, Fact *facts, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        g_checks++;
        if (facts[i].found) {
            printf("  ok    %s\n", facts[i].what);
        } else {
            g_failures++;
            wprintf(L"  FAIL  %hs\n        (\"%hs\" not found in %s)\n", facts[i].what, facts[i].text, file);
        }
    }
}

/* Claude computes the "no folder" area from its own data folder:
 * app.getPath("userData") joined with "scratch-workspaces". A session counts
 * as having no folder only when its folder is under that root. */
static void CheckScratchRoot(const WCHAR *asar)
{
    static const char root[] = "\"scratch-workspaces\"", user[] = "getPath(\"userData\")";
    HANDLE h = CreateFileW(asar, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER size;
    BYTE *buf = NULL;
    DWORD got = 0, n;
    BOOL ok = FALSE;
    g_checks++;
    if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size.QuadPart < 512LL * 1024 * 1024 &&
        (buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)size.QuadPart)) != NULL) {
        while (got < (DWORD)size.QuadPart && ReadFile(h, buf + got, (DWORD)size.QuadPart - got, &n, NULL) && n > 0) got += n;
        {
            const BYTE *p = buf, *end = buf + got;
            while (!ok && (p = Find(p, (size_t)(end - p), (const BYTE *)root, sizeof root - 1)) != NULL) {
                size_t window = (size_t)(end - p) < 300 ? (size_t)(end - p) : 300;
                ok = Find(p, window, (const BYTE *)user, sizeof user - 1) != NULL;
                p += sizeof root - 1;
            }
        }
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (ok) {
        printf("  ok    a session without a folder lives in <profile data>\\scratch-workspaces (userData + \"scratch-workspaces\")\n");
    } else {
        g_failures++;
        wprintf(L"  FAIL  a session without a folder lives in <profile data>\\scratch-workspaces\n"
                L"        (\"scratch-workspaces\" is no longer joined to getPath(\"userData\") in %s)\n", asar);
    }
}

/* Claude Code binaries under the profile folders of one Roaming root. */
static BOOL FindClaudeCodeIn(const WCHAR *root, FILETIME *best, WCHAR *out, size_t cch)
{
    WCHAR pattern[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAW pf, vf;
    HANDLE hp, hv;
    BOOL found = FALSE;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\Claude*", root))) return FALSE;
    hp = FindFirstFileExW(pattern, FindExInfoBasic, &pf, FindExSearchLimitToDirectories, NULL, 0);
    if (hp == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(pf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s\\claude-code\\*", root, pf.cFileName))) continue;
        hv = FindFirstFileExW(dir, FindExInfoBasic, &vf, FindExSearchLimitToDirectories, NULL, 0);
        if (hv == INVALID_HANDLE_VALUE) continue;
        do {
            WCHAR exe[MAX_PATH];
            if (!(vf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || vf.cFileName[0] == L'.') continue;
            if (FAILED(StringCchPrintfW(exe, ARRAYSIZE(exe), L"%s\\%s\\claude-code\\%s\\claude.exe", root, pf.cFileName, vf.cFileName)) ||
                !Util_FileExists(exe) || CompareFileTime(&vf.ftLastWriteTime, best) <= 0)
                continue;
            *best = vf.ftLastWriteTime;
            StringCchCopyW(out, cch, exe);
            found = TRUE;
        } while (FindNextFileW(hv, &vf));
        FindClose(hv);
    } while (FindNextFileW(hp, &pf));
    FindClose(hp);
    return found;
}

/* The Claude Code that Claude Desktop installed last, including a profile
 * stored in its package's LocalCache. Discovery only reads these folders. */
static BOOL FindClaudeCode(const ClaudePackage *pkg, WCHAR *out, size_t cch)
{
    WCHAR roaming[MAX_PATH], local[MAX_PATH];
    FILETIME best = { 0, 0 };
    BOOL found = FALSE;
    if (Util_AppData(roaming, ARRAYSIZE(roaming)))
        found = FindClaudeCodeIn(roaming, &best, out, cch);
    if (pkg->found && Util_LocalAppData(local, ARRAYSIZE(local)) &&
        SUCCEEDED(StringCchPrintfW(roaming, ARRAYSIZE(roaming), L"%s\\Packages\\%s\\LocalCache\\Roaming", local, pkg->family)) &&
        FindClaudeCodeIn(roaming, &best, out, cch))
        found = TRUE;
    return found;
}

int wmain(void)
{
    Fact app[] = {
        { "claude://resume?session=<id> imports a transcript into the running profile", "Resume deep link: importing CLI session", FALSE, FALSE },
        { "the link host is \"resume\"", ".Resume=\"resume\"", FALSE, FALSE },
        { "the import creates the profile's entry itself", "Imported CLI session ", FALSE, FALSE },
        { "Claude logs when its data folder is virtualized by MSIX", "Filesystem virtualization active", FALSE, FALSE },
        { "Claude's updater logs its quit for an update", "beforeQuitForUpdate handler fired", FALSE, FALSE },
        { "a close by Windows logs \"Windows session ending (...)\"", "Windows session ending (", FALSE, FALSE },
        { "a quit from the window logs \"Quitting app\"", "Quitting app", FALSE, FALSE },
        /* "beforeQuit: handler ..." and "willQuit: handler ...": one template, two names. */
        { "a quit logs \"<name>: handler ... quitting\"", ": handler is ready for quit, so quitting", FALSE, FALSE },
        { "... named beforeQuit", "\"beforeQuit\"", FALSE, FALSE },
        { "... and willQuit", "\"willQuit\"", FALSE, FALSE },
        /* "[Auth] Using system browser for: /login/...": the tag and the text apart. */
        { "the window that opens the browser logs \"Using system browser for:\"", " Using system browser for: %s", FALSE, FALSE },
        { "... tagged [Auth]", "\"[Auth]\"", FALSE, FALSE },
        { "other windows ignore a sign-in they did not start", "does not answer a sign-in this app started", FALSE, FALSE },
        { "session entries live in claude-code-sessions", "\"claude-code-sessions\"", FALSE, FALSE },
        { "entry files start with local_", "\"local_\"", FALSE, FALSE },
        { "config.json names the account signed in (lastKnownAccountUuid)", "lastKnownAccountUuid", FALSE, FALSE },
        { "a favorite is isStarred in the entry", "isStarred", FALSE, FALSE },
        { "entries keep lastActivityAt", "lastActivityAt", FALSE, FALSE },
        { "entries name their transcript (cliSessionId)", "cliSessionId", FALSE, FALSE },
        { "entries keep originCwd", "originCwd", FALSE, FALSE },
        { "entries keep titleSource", "titleSource", FALSE, FALSE },
        { "an SSH session's entry has sshConfig (not listed)", "sshConfig", FALSE, FALSE },
        { "a WSL session's entry has wslConfig (not listed)", "wslConfig", FALSE, FALSE },
        { "a cloud session's entry has cloudSessionId or movedToCloud (not listed)", "movedToCloud", FALSE, FALSE },
        { "... cloudSessionId", "cloudSessionId", FALSE, FALSE },
        { "deleting a session in Claude can remove its shared transcript", "desktop-released marker: transcript removed", FALSE, FALSE },
        /* scratch-<UTC date>-<3 random bytes in hex>: Core_ScratchName. */
        { "a session without a folder works in scratch-<UTC date>-<6 hex digits>", ".toISOString().slice(0,10)}-${", FALSE, FALSE },
        { "... the 6 hex digits being 3 random bytes", "randomBytes)(3).toString(\"hex\")", FALSE, FALSE },
    };
    Fact exe[] = {
        { "a running Claude owns a Chrome_MessageWindow (running profiles, links)", "Chrome_MessageWindow", TRUE, FALSE },
        { "Claude's notification-area icon sits on Electron_NotifyIconHostWindow", "Electron_NotifyIconHostWindow", TRUE, FALSE },
    };
    Fact cli[] = {
        { "Claude Code finds a transcript by id in any project when resuming", "tengu_transcript_id_scan_fallback", FALSE, FALSE },
        { "each running Claude Code writes its session and host session in ~/.claude/sessions", "hostSessionId", FALSE, FALSE },
        { "... with its messaging pipe", "messagingSocketPath", FALSE, FALSE },
        { "transcripts keep custom titles", "custom-title", FALSE, FALSE },
        { "workspace trust is hasTrustDialogAccepted in ~/.claude.json", "hasTrustDialogAccepted", FALSE, FALSE },
        /* Core_ProjectDirName: every other character becomes '-', up to 200. */
        { "a folder's transcripts go in projects\\<its path, other than a-z A-Z 0-9 as '-'>", "replace(/[^a-zA-Z0-9]/g,\"-\")", FALSE, FALSE },
        { "... longer than 200 characters, with a hash of its own", "MAX_SANITIZED_LENGTH", FALSE, FALSE },
        { "each running Claude Code writes its process start time (procStart)", "procStart", FALSE, FALSE },
        { "transcript lines carry their session id as \"sessionId\"", "\"sessionId\"", FALSE, FALSE },
    };
    ClaudePackage pkg;
    WCHAR path[MAX_PATH], code[MAX_PATH];

    if (!Claude_FindPackage(&pkg)) {
        printf("skipped: Claude Desktop is not installed\n");
        return 0;
    }
    wprintf(L"Claude Desktop %s\n", pkg.version);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\app\\resources\\app.asar", pkg.installDir);
    CheckScratchRoot(path);
    if (Scan(path, app, ARRAYSIZE(app))) Report(path, app, ARRAYSIZE(app));
    else wprintf(L"  FAIL  cannot read %s\n", path), g_failures++, g_checks++;
    if (Scan(pkg.exe, exe, ARRAYSIZE(exe))) Report(pkg.exe, exe, ARRAYSIZE(exe));
    else wprintf(L"  FAIL  cannot read %s\n", pkg.exe), g_failures++, g_checks++;
    if (FindClaudeCode(&pkg, code, ARRAYSIZE(code))) {
        wprintf(L"Claude Code %s\n", code);
        if (Scan(code, cli, ARRAYSIZE(cli))) Report(code, cli, ARRAYSIZE(cli));
        else wprintf(L"  FAIL  cannot read %s\n", code), g_failures++, g_checks++;
    } else {
        printf("  (Claude Code not installed by Claude Desktop yet: its checks are skipped)\n");
    }
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
