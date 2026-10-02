/*
 * Checks that the installed Claude Desktop, and the Claude Code it runs, still
 * work the way docs/HOW-IT-WORKS.md describes: each fact Claude Desktop
 * Profiles Manager relies on is looked up in Claude's own files (its app.asar,
 * its Claude.exe and resources, the Claude Code binary it installs) and, while
 * a profile's Claude runs, in that profile's data folder. Only reads. Built
 * and run by build.cmd, linked with the program's objects. Skipped when Claude
 * is not installed; exits non-zero when Claude changed one of these facts, so
 * a Claude update that breaks an assumption shows at the next build.
 */
#include "../src/app.h"
#include <appmodel.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATTERN_BYTES  512                     /* a fact's text as looked for, UTF-16 included */
#define NEARBY_BYTES       300                     /* how far minified code keeps the two texts of one fact apart */
#define NAME_MARK          '\x01'                  /* in a fact's text: a name the minifier chose, which a new build may change */
#define NAME_MAX_BYTES     16                      /* the longest such name */
#define MAX_MATCH_BYTES    (MAX_PATTERN_BYTES * NAME_MAX_BYTES)   /* a fact's text as found, each mark a name */
#define SCAN_CHUNK_BYTES   (16u * 1024u * 1024u)
/* What a chunk keeps of the one before: a fact cut by the chunk's end, its nearby text on either side. */
#define SCAN_OVERLAP_BYTES (2u * NEARBY_BYTES + MAX_MATCH_BYTES)
#define LINE_CCH           1024

/* The text of a macro's value: "200" for CORE_PROJECT_NAME_MAX. */
#define TEXT_OF(value)     #value
#define VALUE_TEXT(macro)  TEXT_OF(macro)

typedef struct Fact {
    const char *what;      /* the fact, as the documentation states it */
    const char *text;      /* what shows it in Claude's file (NAME_MARK: a minified name) */
    const char *nearby;    /* NULL, or a text within NEARBY_BYTES of `text` (minified names between them) */
    BOOL        wide;      /* looked for as UTF-16 (strings of the native exe) */
    BOOL        found;
} Fact;

static int g_failures, g_checks;

/* Counts one check and prints it; when it failed, `detailFormat` (NULL for none) says why. */
static void ReportCheck(BOOL passed, const WCHAR *what, const WCHAR *detailFormat, ...)
{
    WCHAR detail[LINE_CCH];
    va_list arguments;
    g_checks++;
    if (passed) {
        wprintf(L"  ok    %s\n", what);
        return;
    }
    g_failures++;
    wprintf(L"  FAIL  %s\n", what);
    if (!detailFormat) return;
    va_start(arguments, detailFormat);
    StringCchVPrintfW(detail, ARRAYSIZE(detail), detailFormat, arguments);
    va_end(arguments);
    wprintf(L"        (%s)\n", detail);
}

static void ReportUnreadable(const WCHAR *path)
{
    WCHAR what[LINE_CCH];
    StringCchPrintfW(what, ARRAYSIZE(what), L"cannot read %s", path);
    ReportCheck(FALSE, what, NULL);
}

/* The first place `needle` is in `haystack`, or NULL. */
static const BYTE *FindBytes(const BYTE *haystack, size_t size, const BYTE *needle, size_t length)
{
    const BYTE *p = haystack, *end = haystack + size;
    if (length == 0 || size < length) return NULL;
    while ((p = (const BYTE *)memchr(p, needle[0], (size_t)(end - p) - length + 1)) != NULL) {
        if (memcmp(p, needle, length) == 0) return p;
        if (++p > end - length) break;
    }
    return NULL;
}

/* A fact looked for as written: no longer than MAX_PATTERN_BYTES, starting with text (not a name mark). */
static BOOL FactFitsPattern(const Fact *fact)
{
    size_t unit = fact->wide ? 2 : 1;
    return strlen(fact->text) * unit <= MAX_PATTERN_BYTES && fact->text[0] != NAME_MARK &&
           (!fact->nearby || (strlen(fact->nearby) * unit <= MAX_PATTERN_BYTES && fact->nearby[0] != NAME_MARK));
}

/* The bytes an ASCII text has in the file, as is or as UTF-16. `out` holds
 * MAX_PATTERN_BYTES, which FactFitsPattern checked. */
static size_t PatternBytes(const char *text, BOOL wide, BYTE *out)
{
    size_t length = strlen(text), i;
    if (!wide) {
        memcpy(out, text, length);
        return length;
    }
    for (i = 0; i < length; i++) {
        out[2 * i] = (BYTE)text[i];
        out[2 * i + 1] = 0;
    }
    return length * 2;
}

static BOOL IsNameByte(BYTE c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
}

/* Where `pattern` ends when it matches from `at` on, or NULL; each
 * NAME_MARK matches a name of 1 to NAME_MAX_BYTES bytes. */
static const BYTE *MatchFrom(const BYTE *at, const BYTE *end, const BYTE *pattern, size_t length)
{
    size_t i, n;
    for (i = 0; i < length; i++) {
        if (pattern[i] == NAME_MARK) {
            for (n = 1; n <= NAME_MAX_BYTES && at + n <= end && IsNameByte(at[n - 1]); n++) {
                const BYTE *matched = MatchFrom(at + n, end, pattern + i + 1, length - i - 1);
                if (matched) return matched;
            }
            return NULL;
        }
        if (at >= end || *at != pattern[i]) return NULL;
        at++;
    }
    return at;
}

/* The first place `pattern` matches in `haystack` (it starts with text, not
 * a mark), with where that match ends. */
static const BYTE *FindPattern(const BYTE *haystack, size_t size, const BYTE *pattern, size_t length, const BYTE **matchEnd)
{
    const BYTE *at = haystack, *end = haystack + size, *matched;
    size_t prefix;
    for (prefix = 0; prefix < length && pattern[prefix] != NAME_MARK; prefix++) {}
    while (prefix && at < end && (at = FindBytes(at, (size_t)(end - at), pattern, prefix)) != NULL) {
        if ((matched = MatchFrom(at, end, pattern, length)) != NULL) {
            *matchEnd = matched;
            return at;
        }
        at++;
    }
    return NULL;
}

/* Whether `data` shows the fact: its text, with its nearby text close to it
 * when it has one. */
static BOOL ShowsFact(const BYTE *data, size_t size, const Fact *fact)
{
    BYTE text[MAX_PATTERN_BYTES], nearby[MAX_PATTERN_BYTES];
    const BYTE *at = data, *end = data + size, *matchEnd, *nearbyEnd;
    size_t textLength, nearbyLength = 0;
    if (!FactFitsPattern(fact)) return FALSE;
    textLength = PatternBytes(fact->text, fact->wide, text);
    if (fact->nearby) nearbyLength = PatternBytes(fact->nearby, fact->wide, nearby);
    while ((at = FindPattern(at, (size_t)(end - at), text, textLength, &matchEnd)) != NULL) {
        const BYTE *from = (size_t)(at - data) > NEARBY_BYTES ? at - NEARBY_BYTES : data;
        const BYTE *to = (size_t)(end - matchEnd) > NEARBY_BYTES ? matchEnd + NEARBY_BYTES : end;
        if (!fact->nearby || FindPattern(from, (size_t)(to - from), nearby, nearbyLength, &nearbyEnd)) return TRUE;
        at++;
    }
    return FALSE;
}

static void MarkFactsShownIn(const BYTE *data, size_t size, Fact *facts, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (!facts[i].found && ShowsFact(data, size, &facts[i])) facts[i].found = TRUE;
}

/* Looks for every fact in the file, in chunks that overlap by more than a
 * fact spans. FALSE when the file cannot be read to its end. */
static BOOL ScanFileForFacts(const WCHAR *path, Fact *facts, size_t count)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    BYTE *buffer;
    DWORD kept = 0, got = 0;
    BOOL readOk = FALSE;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, SCAN_CHUNK_BYTES + SCAN_OVERLAP_BYTES);
    if (!buffer) {
        CloseHandle(file);
        return FALSE;
    }
    while ((readOk = ReadFile(file, buffer + kept, SCAN_CHUNK_BYTES, &got, NULL)) != FALSE && got > 0) {
        size_t n = kept + got;
        MarkFactsShownIn(buffer, n, facts, count);
        kept = n > SCAN_OVERLAP_BYTES ? SCAN_OVERLAP_BYTES : (DWORD)n;
        memmove(buffer, buffer + n - kept, kept);
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    CloseHandle(file);
    return readOk;
}

static void ReportFacts(const WCHAR *file, const Fact *facts, size_t count)
{
    WCHAR what[LINE_CCH];
    size_t i;
    for (i = 0; i < count; i++) {
        const Fact *fact = &facts[i];
        StringCchPrintfW(what, ARRAYSIZE(what), L"%hs", fact->what);
        if (!FactFitsPattern(fact))
            ReportCheck(FALSE, what, L"its text is longer than the %d bytes looked for, or starts with a name", MAX_PATTERN_BYTES);
        else if (fact->nearby)
            ReportCheck(fact->found, what, L"\"%hs\" with \"%hs\" within %d bytes not found in %s", fact->text, fact->nearby,
                        NEARBY_BYTES, file);
        else
            ReportCheck(fact->found, what, L"\"%hs\" not found in %s", fact->text, file);
    }
}

/* The notification-area icons tray.c and icons.c draw on. */
static void CheckTrayIcons(const ClaudePackage *pkg)
{
    static const WCHAR *const names[] = { L"Tray-Win32.ico", L"Tray-Win32-Dark.ico" };
    WCHAR path[MAX_PATH], what[LINE_CCH];
    size_t i;
    for (i = 0; i < ARRAYSIZE(names); i++) {
        BOOL found = SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\app\\resources\\%s", pkg->installDir, names[i])) &&
                     Util_FileExists(path);
        StringCchPrintfW(what, ARRAYSIZE(what), L"Claude's notification-area icon is app\\resources\\%s", names[i]);
        ReportCheck(found, what, L"%s not found", path);
    }
}

/* A running profile's Claude holds "lockfile" in its data folder, created
 * right before its Chrome_MessageWindow: without the process, the watcher sees
 * the profile start that way. Only reads. */
static void CheckLockFiles(const ClaudePackage *pkg)
{
    WCHAR title[MAX_PATH], family[ARRAYSIZE(pkg->family)], localAppData[MAX_PATH], path[MAX_PATH], what[LINE_CCH];
    HWND window = NULL;
    int running = 0;
    if (!Util_LocalAppData(localAppData, ARRAYSIZE(localAppData))) localAppData[0] = 0;
    while ((window = FindWindowExW(HWND_MESSAGE, window, L"Chrome_MessageWindow", NULL)) != NULL) {
        const WCHAR *name;
        Profile profile;
        UINT32 cch = ARRAYSIZE(family);
        DWORD pid = 0;
        HANDLE process;
        BOOL ours;
        if (GetWindowTextW(window, title, ARRAYSIZE(title)) <= 0 || !GetWindowThreadProcessId(window, &pid)) continue;
        if ((process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) == NULL) continue;
        ours = GetPackageFamilyName(process, &cch, family) == ERROR_SUCCESS && Core_EqualsI(family, pkg->family);
        CloseHandle(process);
        if (!ours) continue;
        ZeroMemory(&profile, sizeof profile);
        StringCchCopyW(profile.dataDir, ARRAYSIZE(profile.dataDir), title);
        name = wcsrchr(title, L'\\');
        profile.isStock = name && Core_EqualsI(name + 1, STOCK_FOLDER);
        if (!Profiles_ResolveStorage(&profile, localAppData, pkg->family) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lockfile", profile.storageDir)))
            continue;
        running++;
        StringCchPrintfW(what, ARRAYSIZE(what), L"the running Claude of %s holds lockfile in its data folder", title);
        ReportCheck(Util_FileExists(path), what, L"%s not found", path);
    }
    if (!running) wprintf(L"  skip  lockfile checks: no profile's Claude is running\n");
}

/* Claude Code binaries under the profile folders of one Roaming root. */
static BOOL FindClaudeCodeIn(const WCHAR *root, FILETIME *best, WCHAR *out, size_t cch)
{
    WCHAR search[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAW profileFolder, versionFolder;
    HANDLE profiles, versions;
    BOOL found = FALSE;
    if (FAILED(StringCchPrintfW(search, ARRAYSIZE(search), L"%s\\Claude*", root))) return FALSE;
    profiles = FindFirstFileExW(search, FindExInfoBasic, &profileFolder, FindExSearchLimitToDirectories, NULL, 0);
    if (profiles == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(profileFolder.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s\\claude-code\\*", root, profileFolder.cFileName))) continue;
        versions = FindFirstFileExW(dir, FindExInfoBasic, &versionFolder, FindExSearchLimitToDirectories, NULL, 0);
        if (versions == INVALID_HANDLE_VALUE) continue;
        do {
            WCHAR exe[MAX_PATH];
            if (!(versionFolder.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || versionFolder.cFileName[0] == L'.') continue;
            if (FAILED(StringCchPrintfW(exe, ARRAYSIZE(exe), L"%s\\%s\\claude-code\\%s\\claude.exe", root, profileFolder.cFileName,
                                        versionFolder.cFileName)) ||
                !Util_FileExists(exe) || CompareFileTime(&versionFolder.ftLastWriteTime, best) <= 0)
                continue;
            *best = versionFolder.ftLastWriteTime;
            StringCchCopyW(out, cch, exe);
            found = TRUE;
        } while (FindNextFileW(versions, &versionFolder));
        FindClose(versions);
    } while (FindNextFileW(profiles, &profileFolder));
    FindClose(profiles);
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
        { "claude://resume?session=<id> imports that transcript into the running profile",
          "Resume deep link: importing CLI session", "searchParams.get(\"session\")" },
        { "the link host is \"resume\"", ".Resume=\"resume\"" },
        { "the import creates the profile's entry itself", "Imported CLI session " },
        { "Claude logs when its data folder is virtualized by MSIX", "Filesystem virtualization active" },
        { "Claude's updater logs its quit for an update", "beforeQuitForUpdate handler fired" },
        /* The reasons are Electron's: "close-app" (exe[]) when Windows closes Claude to update it. */
        { "a close by Windows logs \"Windows session ending (<reasons>)\"", "Windows session ending (%s) - quitting the app",
          "reasons.join(\", \")" },
        { "a quit from the window logs \"Quitting app\"", "Quitting app" },
        /* "beforeQuit: handler ..." and "willQuit: handler ...": one template, a name for each of Electron's events. */
        { "a quit logs \"<name>: handler ... quitting\"", ": handler is ready for quit, so quitting" },
        { "... named beforeQuit for Electron's before-quit", "\"before-quit\",\x01(\"beforeQuit\")" },
        { "... and willQuit for its will-quit", "\"will-quit\",\x01(\"willQuit\")" },
        /* "[Auth] Using system browser for: /login/...": the tag and the text apart. */
        { "the window that opens the browser logs \"Using system browser for:\"", " Using system browser for: %s" },
        { "... tagged [Auth]", "logPrefix??\"[Auth]\"" },
        { "other windows ignore a sign-in they did not start", "does not answer a sign-in this app started" },
        { "session entries live in claude-code-sessions, in files that start with local_", "=\"claude-code-sessions\",\x01=\"local_\"" },
        { "config.json names the account signed in (lastKnownAccountUuid)", "lastKnownAccountUuid" },
        { "a star is isStarred in the entry", "isStarred" },
        { "an archived session is isArchived in the entry", "isArchived" },
        { "entries keep lastActivityAt", "lastActivityAt" },
        { "entries name their transcript (cliSessionId)", "cliSessionId" },
        { "entries keep originCwd", "originCwd" },
        { "entries keep titleSource", "titleSource" },
        { "an SSH session's entry has sshConfig (not listed)", "sshConfig" },
        { "a WSL session's entry has wslConfig (not listed)", "wslConfig" },
        { "a cloud session's entry has cloudSessionId or movedToCloud (not listed)", "movedToCloud" },
        { "... cloudSessionId", "cloudSessionId" },
        /* Delete session everywhere removes what Claude's own delete removes (sessionedit.c). */
        { "entries name the session's earlier transcripts: priorCliSessionIds", "priorCliSessionIds", "preClearCliSessionId" },
        { "... preClearCliSessionId and unarchivedCliSessionId", "unarchivedCliSessionId", "preClearCliSessionId" },
        { "Claude's delete leaves a deleted_<id> mark holding the time", "\"deleted_\";", "[SessionTombstones]" },
        { "... for the entry's own id and the transcripts no other session claims",
          "),\x01=[...new Set([\x01.startsWith(\"local_\")?", "!\x01.has(\x01))))];await \x01((0,\x01.dirname)(\x01),\x01,Date.now())" },
        { "... lets the session's transcripts go (reason \"delete\")", ",\"delete\",{mayRelease:" },
        { "... marking each in its project folder with <id>.desktop-released.json (gone here with the transcript)",
          "releasedAt:(new Date).toISOString(),reason:", "desktop-released marker: write failed" },
        { "... the marks being named <id>.desktop-released.json", "=\".desktop-released.json\"" },
        { "... removes beside the transcript <id>.ccr-tip.json, <id>.precompact.json and <id>.jsonl.pre-import, nothing else",
          "=[\".ccr-tip.json\",\".precompact.json\",`.jsonl${\x01}`],",
          "[\"file-history\",\"session-env\",\"uploads\",\"tasks\",\"image-cache\"]" },
        { "... .pre-import being the transcript as it was before Claude took it in", "=\".pre-import\",",
          "Transcript changed while it was being adopted" },
        { "... removes Claude Code's temporary folder of the session, <temp>\\<project>\\<id>", "\"temp session dir\"",
          "\"temp project dir\"" },
        { "... <temp> being CLAUDE_CODE_TMPDIR, else the system's temporary folder",
          "process.env.CLAUDE_CODE_TMPDIR??(process.platform===\"darwin\"?\"/tmp\":(0,", ".tmpdir)()" },
        { "... with claude in it on Windows", "process.platform===\"win32\"?\"claude\":" },
        { "... removes the entry's staged import transcript (stagedTranscriptPath, in imported-staging)",
          "\"imported-staging\"))&&await", "staged import transcript could not be removed on delete" },
        { "... and a session's own scratch workspace when no other session works there", "removeScratchWorkspace(",
          "scratch workspace removal on delete failed" },
        { "... the session's file-history, session-env, uploads, tasks and image-cache folders",
          "[\"file-history\",\"session-env\",\"uploads\",\"tasks\",\"image-cache\"]", "[\"startup-perf\",\".json\"]]" },
        { "... and its debug, usage-data and startup-perf files",
          "[[\"debug\",\".txt\"],[\"debug\",\".1.txt\"],[\"usage-data/facets\",\".json\"],[\"usage-data/session-meta\",\".json\"],"
          "[\"startup-perf\",\".txt\"],[\"startup-perf\",\".json\"]]" },
        { "a session without a folder lives in <profile data>\\scratch-workspaces (userData + \"scratch-workspaces\")",
          "\"scratch-workspaces\"", "getPath(\"userData\")" },
        /* `scratch-${day.toISOString().slice(0,10)}-${randomBytes(3).toString("hex")}`: Core_ScratchName. */
        { "a session without a folder works in scratch-<UTC date>-<6 hex digits>", "`scratch-${", ".toISOString().slice(0,10)}-${" },
        { "... the 6 hex digits being 3 random bytes", "`scratch-${", "randomBytes)(3).toString(\"hex\")}`" },
    };
    Fact exe[] = {
        { "a running Claude owns a Chrome_MessageWindow (running profiles, links)", "Chrome_MessageWindow", NULL, TRUE },
        { "Claude's notification-area icon sits on Electron_NotifyIconHostWindow", "Electron_NotifyIconHostWindow", NULL, TRUE },
        /* Electron's names for the reasons of a Windows session end, as narrow text. */
        { "Windows closing Claude to update it gives the reason \"close-app\"", "close-app", "logoff" },
    };
    Fact cli[] = {
        { "Claude Code finds a transcript by id in any project when resuming", "tengu_transcript_id_scan_fallback" },
        /* Core_ProjectDirName: the projects folder, then the working folder's name, as written in the projects module. */
        { "a folder's transcripts go in projects\\<a name made from its path>",
          "(),\"projects\")}function \x01(e){return \x01()??\x01(e)}" },
        { "... every character but a-z A-Z 0-9 as '-', past " VALUE_TEXT(CORE_PROJECT_NAME_MAX) " characters cut with a hash of its own",
          "=" VALUE_TEXT(CORE_PROJECT_NAME_MAX) ";function \x01(e){return Math.abs(\x01(e)).toString(36)}function \x01(e){return "
          "e.replace(/[^a-zA-Z0-9]/g,\"-\")}" },
        { "each running Claude Code records its session and its process start time (procStart) in ~/.claude/sessions",
          "pid:process.pid,sessionId:", "procStart:await " },
        { "transcript lines and session records carry their session id as \"sessionId\"", "\"sessionId\"" },
    };
    ClaudePackage pkg;
    WCHAR path[MAX_PATH], code[MAX_PATH];

    /* Paths keep their non-ASCII letters, on the console as in a redirected file (UTF-8). */
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (!Claude_FindPackage(&pkg)) {
        wprintf(L"Claude tests: skipped, Claude Desktop is not installed.\n");
        return 0;
    }
    wprintf(L"Claude Desktop %s\n", pkg.version);
    if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\app\\resources\\app.asar", pkg.installDir)) &&
        ScanFileForFacts(path, app, ARRAYSIZE(app)))
        ReportFacts(path, app, ARRAYSIZE(app));
    else
        ReportUnreadable(path);
    if (ScanFileForFacts(pkg.exe, exe, ARRAYSIZE(exe))) ReportFacts(pkg.exe, exe, ARRAYSIZE(exe));
    else ReportUnreadable(pkg.exe);
    CheckTrayIcons(&pkg);
    CheckLockFiles(&pkg);
    if (FindClaudeCode(&pkg, code, ARRAYSIZE(code))) {
        wprintf(L"Claude Code %s\n", code);
        if (ScanFileForFacts(code, cli, ARRAYSIZE(cli))) ReportFacts(code, cli, ARRAYSIZE(cli));
        else ReportUnreadable(code);
    } else {
        wprintf(L"  skip  Claude Code checks: Claude Desktop has not installed it yet\n");
    }
    wprintf(L"Claude tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
