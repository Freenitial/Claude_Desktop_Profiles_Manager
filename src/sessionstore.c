/*
 * The Claude Code sessions of every profile, as the sessions view shows
 * them. Read only (sessionedit.c changes them). Only the sessions of this PC:
 * one run over SSH, in WSL or in the cloud has its conversation elsewhere and
 * is only counted.
 *
 * A Code session is two things on disk: its transcript, in the Claude Code
 * folder every profile shares (~\.claude\projects\<project>\<id>.jsonl), and
 * one entry per profile that lists it, in that profile's
 * claude-code-sessions\<account>\<organization>\local_<...>.json (title,
 * favorite, folder, last activity). A row here is one transcript, with the
 * entry of each profile that lists it. Claude only shows the entries of the
 * account signed in (config.json, lastKnownAccountUuid) and of its current
 * organization, taken as the one with the latest entry.
 *
 * Also read: the changes waiting for a profile to close (sessionedit.c), and
 * which profiles run a session right now: every running Claude Code writes
 * ~\.claude\sessions\<pid>.json (its session, its start time), and its parent
 * process is the Claude of the profile that opened it.
 */
#include "app.h"
#include <stdlib.h>
#include <tlhelp32.h>

#define RECORD_MAX_BYTES  (4u * 1024u * 1024u)
#define PENDING_MAX_BYTES (1024u * 1024u)
#define SESSIONS_DIR      L"claude-code-sessions"
#define SCRATCH_DIR       L"scratch-workspaces"

typedef struct Transcript {
    WCHAR     id[SESSION_ID_CCH];
    WCHAR     path[MAX_PATH];
    ULONGLONG bytes;
} Transcript;

typedef struct Transcripts {
    Transcript *items;
    int         count, cap;
} Transcripts;

static void *Grow(void *items, int *cap, int need, size_t size)
{
    void *bigger;
    int n;
    if (need <= *cap) return items;
    n = *cap ? *cap * 2 : 64;
    while (n < need) n *= 2;
    bigger = items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, items, (size_t)n * size)
                   : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)n * size);
    if (!bigger) return NULL;
    *cap = n;
    return bigger;
}

static BOOL MemberString(const char *json, size_t len, const char *key, WCHAR *out, size_t cch)
{
    const char *v;
    size_t n;
    out[0] = 0;
    return Core_JsonMember(json, len, key, &v, &n) && Core_JsonString(v, n, out, cch);
}

static BOOL MemberTrue(const char *json, size_t len, const char *key)
{
    const char *v;
    size_t n;
    return Core_JsonMember(json, len, key, &v, &n) && Core_JsonTrue(v, n);
}

static ULONGLONG MemberNumber(const char *json, size_t len, const char *key)
{
    const char *v;
    size_t n;
    ULONGLONG t = 0;
    if (Core_JsonMember(json, len, key, &v, &n)) Core_JsonNumber(v, n, &t);
    return t;
}

static BOOL SameId(const WCHAR *a, const WCHAR *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

/* ------------------------------------------------------------ transcripts */

/* Claude Code's folder: CLAUDE_CONFIG_DIR, else ~\.claude. */
static BOOL ClaudeCodeDir(const WCHAR *sub, WCHAR *out, size_t cch)
{
    WCHAR env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", env, ARRAYSIZE(env));
    if (n > 0 && n < ARRAYSIZE(env)) return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", env, sub));
    n = GetEnvironmentVariableW(L"USERPROFILE", env, ARRAYSIZE(env));
    return n > 0 && n < ARRAYSIZE(env) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\.claude\\%s", env, sub));
}

BOOL SessionStore_ProjectsDir(WCHAR *out, size_t cch)
{
    return ClaudeCodeDir(L"projects", out, cch);
}

static int __cdecl CompareTranscripts(const void *a, const void *b)
{
    return CompareStringOrdinal(((const Transcript *)a)->id, -1, ((const Transcript *)b)->id, -1, TRUE) - CSTR_EQUAL;
}

/* Every <id>.jsonl directly inside a project folder. A transcript Claude Code
 * continued from another folder can exist twice: the larger one counts. */
static void LoadTranscripts(Transcripts *t)
{
    WCHAR root[MAX_PATH], pattern[MAX_PATH], files[MAX_PATH];
    WIN32_FIND_DATAW dir, fd;
    HANDLE hd, hf;
    if (!SessionStore_ProjectsDir(root, ARRAYSIZE(root)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", root)))
        return;
    hd = FindFirstFileExW(pattern, FindExInfoBasic, &dir, FindExSearchLimitToDirectories, NULL, 0);
    if (hd == INVALID_HANDLE_VALUE) return;
    do {
        if (!(dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || dir.cFileName[0] == L'.') continue;
        if (FAILED(StringCchPrintfW(files, ARRAYSIZE(files), L"%s\\%s\\*.jsonl", root, dir.cFileName))) continue;
        hf = FindFirstFileExW(files, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            size_t n = wcslen(fd.cFileName);
            Transcript *grown, *tr;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n <= 6 || n - 6 >= ARRAYSIZE(grown->id)) continue;
            grown = (Transcript *)Grow(t->items, &t->cap, t->count + 1, sizeof *t->items);
            if (!grown) break;
            t->items = grown;
            tr = &t->items[t->count];
            if (FAILED(StringCchPrintfW(tr->path, ARRAYSIZE(tr->path), L"%s\\%s\\%s", root, dir.cFileName, fd.cFileName))) continue;
            StringCchCopyNW(tr->id, ARRAYSIZE(tr->id), fd.cFileName, n - 6);
            tr->bytes = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            t->count++;
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    } while (FindNextFileW(hd, &dir));
    FindClose(hd);
    if (t->count > 1) qsort(t->items, (size_t)t->count, sizeof *t->items, CompareTranscripts);
}

static const Transcript *FindTranscript(const Transcripts *t, const WCHAR *id)
{
    Transcript key;
    const Transcript *hit;
    if (!t->count || FAILED(StringCchCopyW(key.id, ARRAYSIZE(key.id), id))) return NULL;
    hit = (const Transcript *)bsearch(&key, t->items, (size_t)t->count, sizeof *t->items, CompareTranscripts);
    /* Duplicates sit side by side: take the largest. */
    if (hit) {
        const Transcript *best = hit, *p;
        for (p = hit; p > t->items && CompareTranscripts(p - 1, &key) == 0; p--)
            if (p[-1].bytes > best->bytes) best = p - 1;
        for (p = hit; p + 1 < t->items + t->count && CompareTranscripts(p + 1, &key) == 0; p++)
            if (p[1].bytes > best->bytes) best = p + 1;
        hit = best;
    }
    return hit;
}

/* ---------------------------------------------------------------- entries */

BOOL SessionStore_SessionsDir(const Profile *p, WCHAR *out, size_t cch)
{
    if (!cch) return FALSE;
    out[0] = 0;
    return p->storageDir[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" SESSIONS_DIR, p->storageDir));
}

/* Until the entries exist, a notification on their closest existing parent
 * catches the creation of every missing directory without creating it. */
BOOL SessionStore_WatchDir(const Profile *p, WCHAR *out, size_t cch)
{
    return SessionStore_SessionsDir(p, out, cch) && Util_ExistingDir(out, out, cch);
}

/* Entries and transcripts retain the paths seen inside Claude's package.
 * Explorer and file operations need the corresponding path outside it. */
BOOL SessionStore_WorkingDir(const SessionSet *s, const WCHAR *cwd, WCHAR *out, size_t cch)
{
    int p;
    for (p = 0; p < s->profiles.count; p++) {
        const Profile *profile = &s->profiles.items[p];
        if (Core_PathEquals(cwd, profile->dataDir) || Core_PathUnder(cwd, profile->dataDir))
            return Core_ProfileFilePath(profile, cwd, out, cch);
    }
    return SUCCEEDED(StringCchCopyW(out, cch, cwd));
}

/* The subfolder of `dir` whose local_*.json is the most recent. */
static BOOL NewestSubfolder(const WCHAR *dir, WCHAR *out, size_t cch)
{
    WCHAR pattern[MAX_PATH], records[MAX_PATH];
    WIN32_FIND_DATAW sub, fd;
    ULONGLONG best = 0;
    HANDLE hs, hf;
    BOOL found = FALSE;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", dir))) return FALSE;
    hs = FindFirstFileExW(pattern, FindExInfoBasic, &sub, FindExSearchLimitToDirectories, NULL, 0);
    if (hs == INVALID_HANDLE_VALUE) return FALSE;
    do {
        ULONGLONG newest = 1;   /* an empty folder still beats none */
        if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sub.cFileName[0] == L'.') continue;
        if (SUCCEEDED(StringCchPrintfW(records, ARRAYSIZE(records), L"%s\\%s\\local_*.json", dir, sub.cFileName)) &&
            (hf = FindFirstFileExW(records, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0)) != INVALID_HANDLE_VALUE) {
            do {
                ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (t > newest) newest = t;
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        if (newest > best && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", dir, sub.cFileName))) {
            best = newest;
            found = TRUE;
        }
    } while (FindNextFileW(hs, &sub));
    FindClose(hs);
    return found;
}

/* The folder holding the entries Claude shows for this profile. The account
 * signed in has none yet: the entries of an account signed in before are not
 * shown by Claude either. Only without config.json is the latest account taken. */
static BOOL EntriesDir(const Profile *p, WCHAR *out, size_t cch, BOOL *signedIn)
{
    WCHAR root[MAX_PATH], config[MAX_PATH], account[64], dir[MAX_PATH];
    DWORD len = 0;
    char *json;
    account[0] = 0;
    *signedIn = FALSE;
    if (!p->storageDir[0]) return FALSE;
    if (SUCCEEDED(StringCchPrintfW(config, ARRAYSIZE(config), L"%s\\config.json", p->storageDir)) &&
        (json = Util_ReadFile(config, RECORD_MAX_BYTES, FALSE, &len)) != NULL) {
        MemberString(json, len, "lastKnownAccountUuid", account, ARRAYSIZE(account));
        HeapFree(GetProcessHeap(), 0, json);
    }
    *signedIn = account[0] != 0;
    if (!SessionStore_SessionsDir(p, root, ARRAYSIZE(root))) return FALSE;
    if (account[0]) {
        return !wcschr(account, L'\\') && !wcschr(account, L'/') &&
               SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", root, account)) && Util_DirExists(dir) &&
               NewestSubfolder(dir, out, cch);
    }
    return NewestSubfolder(root, dir, ARRAYSIZE(dir)) && NewestSubfolder(dir, out, cch);
}

/* The "no folder" area that goes with an entries folder: the same account
 * and organization under scratch-workspaces. */
static BOOL ScratchDirFor(const Profile *p, const WCHAR *entriesDir, WCHAR *out, size_t cch)
{
    const WCHAR *org = wcsrchr(entriesDir, L'\\'), *account;
    if (!org || org == entriesDir) return FALSE;
    for (account = org - 1; account > entriesDir && *account != L'\\'; account--) {}
    if (*account != L'\\') return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" SCRATCH_DIR L"%s", p->dataDir, account));
}

static int FindRow(const SessionSet *s, const WCHAR *key)
{
    int i;
    for (i = 0; i < s->rowCount; i++)
        if (SameId(s->rows[i].key, key)) return i;
    return -1;
}

static void AddEntry(SessionSet *s, int profile, const WCHAR *file, const char *json, size_t len, const Transcripts *t)
{
    SessionEntry e;
    WCHAR cli[SESSION_ID_CCH], titleSource[16], cwd[MAX_PATH], origin[MAX_PATH];
    const WCHAR *key;
    SessionEntry *entries;
    SessionRow *rows;
    int r;

    ZeroMemory(&e, sizeof e);
    /* A session run over SSH, in WSL or moved to the cloud keeps its
     * conversation elsewhere: only this PC's sessions are listed. */
    switch (Core_SessionEntryKind(json, len)) {
    case ENTRY_ELSEWHERE:
        s->source[profile].elsewhere++;
        return;
    case ENTRY_NOT_ONE:
        s->source[profile].unreadable++;
        return;
    case ENTRY_LOCAL:
        break;
    }
    if (!MemberString(json, len, "sessionId", e.localId, ARRAYSIZE(e.localId)) || !e.localId[0]) {
        s->source[profile].unreadable++;
        return;
    }
    StringCchCopyW(e.file, ARRAYSIZE(e.file), file);
    MemberString(json, len, "title", e.title, ARRAYSIZE(e.title));
    MemberString(json, len, "titleSource", titleSource, ARRAYSIZE(titleSource));
    e.userTitle = e.title[0] && SameId(titleSource, L"user");
    e.starred = MemberTrue(json, len, "isStarred");
    e.archived = MemberTrue(json, len, "isArchived");
    e.lastActivity = MemberNumber(json, len, "lastActivityAt");
    e.pendingStar = -1;
    MemberString(json, len, "cliSessionId", cli, ARRAYSIZE(cli));
    MemberString(json, len, "cwd", cwd, ARRAYSIZE(cwd));
    MemberString(json, len, "originCwd", origin, ARRAYSIZE(origin));
    key = cli[0] ? cli : e.localId;

    entries = (SessionEntry *)Grow(s->entries, &s->entryCap, s->entryCount + 1, sizeof *s->entries);
    if (!entries) return;
    s->entries = entries;
    r = FindRow(s, key);
    if (r < 0) {
        const Transcript *tr = cli[0] ? FindTranscript(t, cli) : NULL;
        rows = (SessionRow *)Grow(s->rows, &s->rowCap, s->rowCount + 1, sizeof *s->rows);
        if (!rows) return;
        s->rows = rows;
        r = s->rowCount++;
        ZeroMemory(&s->rows[r], sizeof s->rows[r]);
        StringCchCopyW(s->rows[r].key, ARRAYSIZE(s->rows[r].key), key);
        FillMemory(s->rows[r].entry, sizeof s->rows[r].entry, 0xFF);   /* -1: not listed */
        s->rows[r].transcript = tr != NULL;
        s->rows[r].transcriptBytes = tr ? tr->bytes : 0;
        if (tr) StringCchCopyW(s->rows[r].transcriptPath, ARRAYSIZE(s->rows[r].transcriptPath), tr->path);
    }
    if (s->rows[r].entry[profile] >= 0) return;   /* the same transcript twice in one profile */
    s->entries[s->entryCount] = e;
    s->rows[r].entry[profile] = s->entryCount++;
    /* The folder of the latest entry: after Claude Code moved a session to
     * another folder, that is where it goes on. */
    if (e.lastActivity >= s->rows[r].lastActivity) {
        s->rows[r].lastActivity = e.lastActivity;
        StringCchCopyW(s->rows[r].cwd, ARRAYSIZE(s->rows[r].cwd), origin[0] ? origin : cwd);
    }
}

/* Log a resolved source once per state, not on every transcript notification. */
static void LogSource(int profile, const Profile *p, const SessionSource *src)
{
    static ULONGLONG previous[MAX_PROFILES];
    ULONGLONG hash = Core_HashBytes(CORE_HASH_START, p->folder, wcslen(p->folder) * sizeof(WCHAR));
    const WCHAR *reason;
    hash = Core_HashBytes(hash, p->storageDir, wcslen(p->storageDir) * sizeof(WCHAR));
    hash = Core_HashBytes(hash, src->entriesDir, wcslen(src->entriesDir) * sizeof(WCHAR));
    hash = Core_HashBytes(hash, &src->signedIn, sizeof src->signedIn);
    if (hash == previous[profile]) return;
    previous[profile] = hash;
    if (!p->storageDir[0]) reason = L"profile storage could not be resolved";
    else if (src->found) reason = src->signedIn ? L"account entries resolved" : L"stored entries found without an account in config.json";
    else if (src->signedIn) reason = L"account known; no session entries folder";
    else reason = L"no account or entries found; config.json may be missing, unreadable or have no account";
    Util_Log(L"sessions in %s: data=%s; entries=%s; %s", p->folder, p->storageDir, src->entriesDir, reason);
}

static void LoadProfileEntries(SessionSet *s, int profile, const Transcripts *t)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    const Profile *p = &s->profiles.items[profile];
    SessionSource *src = &s->source[profile];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!EntriesDir(p, src->entriesDir, ARRAYSIZE(src->entriesDir), &src->signedIn) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\local_*.json", src->entriesDir))) {
        src->entriesDir[0] = 0;
        LogSource(profile, p, src);
        return;
    }
    src->found = TRUE;
    LogSource(profile, p, src);
    ScratchDirFor(p, src->entriesDir, src->scratchDir, ARRAYSIZE(src->scratchDir));
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DWORD len;
        char *json;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", src->entriesDir, fd.cFileName))) continue;
        if ((json = Util_ReadFile(path, RECORD_MAX_BYTES, FALSE, &len)) == NULL) {
            src->unreadable++;
            continue;
        }
        AddEntry(s, profile, path, json, len, t);
        HeapFree(GetProcessHeap(), 0, json);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------------------------------------------------------------- pending */

BOOL SessionStore_PendingPath(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\pending-sessions-%s.txt", state, p->folder));
}

/* The changes waiting for the profile to close, oldest first (UTF-8, one per
 * line, see Core_PendingParse). */
int SessionStore_LoadPending(const Profile *p, PendingEdit *edits, int max)
{
    WCHAR path[MAX_PATH], *text, *line, *next;
    DWORD len = 0;
    char *raw;
    int n = 0, wide;
    if (!SessionStore_PendingPath(p, path, ARRAYSIZE(path)) || (raw = Util_ReadFile(path, PENDING_MAX_BYTES, FALSE, &len)) == NULL)
        return 0;
    wide = MultiByteToWideChar(CP_UTF8, 0, raw, (int)len, NULL, 0);
    text = wide > 0 ? (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)wide + 1) * sizeof(WCHAR)) : NULL;
    if (text) {
        MultiByteToWideChar(CP_UTF8, 0, raw, (int)len, text, wide);
        text[wide] = 0;
        for (line = text; line && *line && n < max; line = next) {
            next = wcschr(line, L'\n');
            if (next) *next++ = 0;
            if (*line && line[wcslen(line) - 1] == L'\r') line[wcslen(line) - 1] = 0;
            if (Core_PendingParse(line, &edits[n])) n++;
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    HeapFree(GetProcessHeap(), 0, raw);
    return n;
}

/* What waits for each profile, shown on its entries: the latest change of
 * each kind wins. */
static void LoadAllPending(SessionSet *s)
{
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    int p, i, n, r;
    if (!edits) return;
    for (p = 0; p < s->profiles.count; p++) {
        n = SessionStore_LoadPending(&s->profiles.items[p], edits, SESSION_PENDING_MAX);
        s->source[p].pending = n;
        for (i = 0; i < n; i++) {
            SessionEntry *e;
            if ((r = FindRow(s, edits[i].key)) < 0 || s->rows[r].entry[p] < 0) continue;
            e = &s->entries[s->rows[r].entry[p]];
            e->pending = TRUE;
            if (edits[i].op == PENDING_TITLE) StringCchCopyW(e->pendingTitle, ARRAYSIZE(e->pendingTitle), edits[i].value);
            else if (edits[i].op == PENDING_STAR) e->pendingStar = edits[i].value[0] == L'1';
            else e->pendingRemove = TRUE;
        }
    }
    HeapFree(GetProcessHeap(), 0, edits);
}

/* ------------------------------------------------------------------- live */

/* Which profiles run each session now: a live Claude Code process (its
 * start time matches what it wrote, so a recycled pid does not count) whose
 * parent is a profile's Claude. */
static void LoadLive(SessionSet *s)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH], id[SESSION_ID_CCH], start[32];
    WIN32_FIND_DATAW fd;
    PROCESSENTRY32W pe;
    HANDLE find, snap;
    DWORD parents[4096][2];
    int count = 0, i, p, r;
    if (!ClaudeCodeDir(L"sessions", dir, ARRAYSIZE(dir)) || FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.json", dir)))
        return;
    find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (find == INVALID_HANDLE_VALUE) return;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        pe.dwSize = sizeof pe;
        if (Process32FirstW(snap, &pe)) {
            do {
                parents[count][0] = pe.th32ProcessID;
                parents[count][1] = pe.th32ParentProcessID;
            } while (++count < (int)ARRAYSIZE(parents) && Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    do {
        DWORD len, pid = (DWORD)wcstoul(fd.cFileName, NULL, 10), parent = 0;
        ULONGLONG started = 0;
        FILETIME created, exited, kernel, user;
        HANDLE process;
        char *json;
        BOOL alive = FALSE;
        if (!pid || FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) continue;
        if ((json = Util_ReadFile(path, RECORD_MAX_BYTES, FALSE, &len)) == NULL) continue;
        MemberString(json, len, "sessionId", id, ARRAYSIZE(id));
        /* The process's creation time (a FILETIME), written as a string. */
        started = MemberString(json, len, "procStart", start, ARRAYSIZE(start)) ? _wcstoui64(start, NULL, 10)
                                                                              : MemberNumber(json, len, "procStart");
        HeapFree(GetProcessHeap(), 0, json);
        if ((process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) != NULL) {
            alive = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                    (((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime) == started;
            CloseHandle(process);
        }
        if (!alive || !id[0] || (r = FindRow(s, id)) < 0) continue;
        for (i = 0; i < count; i++)
            if (parents[i][0] == pid) parent = parents[i][1];
        for (p = 0; parent && p < s->profiles.count; p++)
            if (s->profiles.items[p].running && s->profiles.items[p].pid == parent) s->rows[r].live |= 1u << p;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

/* ----------------------------------------------------------------- groups */

/* The profile whose scratch area (a session without a folder) holds `cwd`. */
static int ScratchOwner(const SessionSet *s, const WCHAR *cwd)
{
    WCHAR scratch[MAX_PATH];
    int i;
    for (i = 0; i < s->profiles.count; i++)
        if (SUCCEEDED(StringCchPrintfW(scratch, ARRAYSIZE(scratch), L"%s\\" SCRATCH_DIR, s->profiles.items[i].dataDir)) &&
            Core_PathUnder(cwd, scratch))
            return i;
    return -1;
}

static int GroupFor(SessionSet *s, const SessionRow *row)
{
    SessionGroup *groups;
    const WCHAR *leaf;
    int owner = ScratchOwner(s, row->cwd), g;
    for (g = 0; g < s->groupCount; g++) {
        if (owner >= 0 ? s->groups[g].scratchOf == owner : (s->groups[g].scratchOf < 0 && Core_PathEquals(s->groups[g].path, row->cwd)))
            return g;
        if (owner < 0 && !row->cwd[0] && s->groups[g].scratchOf < 0 && !s->groups[g].path[0]) return g;
    }
    groups = (SessionGroup *)Grow(s->groups, &s->groupCap, s->groupCount + 1, sizeof *s->groups);
    if (!groups) return -1;
    s->groups = groups;
    g = s->groupCount++;
    ZeroMemory(&s->groups[g], sizeof s->groups[g]);
    s->groups[g].scratchOf = owner;
    if (owner >= 0) {
        StringCchPrintfW(s->groups[g].name, ARRAYSIZE(s->groups[g].name), L"No folder \x00B7 %s", s->profiles.items[owner].name);
    } else if (row->cwd[0]) {
        StringCchCopyW(s->groups[g].path, ARRAYSIZE(s->groups[g].path), row->cwd);
        leaf = wcsrchr(row->cwd, L'\\');
        StringCchCopyW(s->groups[g].name, ARRAYSIZE(s->groups[g].name), leaf && leaf[1] ? leaf + 1 : row->cwd);
    } else {
        StringCchCopyW(s->groups[g].name, ARRAYSIZE(s->groups[g].name), L"Unknown folder");
    }
    return g;
}

static const SessionSet *g_sortSet;

static int __cdecl CompareRows(const void *a, const void *b)
{
    const SessionRow *x = (const SessionRow *)a, *y = (const SessionRow *)b;
    const SessionGroup *gx = &g_sortSet->groups[x->group], *gy = &g_sortSet->groups[y->group];
    if (x->group != y->group) {
        if (gx->lastActivity != gy->lastActivity) return gx->lastActivity > gy->lastActivity ? -1 : 1;
        return x->group < y->group ? -1 : 1;
    }
    if (x->lastActivity != y->lastActivity) return x->lastActivity > y->lastActivity ? -1 : 1;
    /* Same time: by id, so that two reloads order them alike. */
    return CompareStringOrdinal(x->key, -1, y->key, -1, TRUE) - CSTR_EQUAL;
}

/* Rows by project, the project used last first, then by last activity. */
static void GroupRows(SessionSet *s)
{
    int r;
    for (r = 0; r < s->rowCount; r++) {
        int g = GroupFor(s, &s->rows[r]);
        s->rows[r].group = g < 0 ? 0 : g;
        if (g >= 0) {
            s->groups[g].count++;
            if (s->rows[r].lastActivity > s->groups[g].lastActivity) s->groups[g].lastActivity = s->rows[r].lastActivity;
        }
    }
    if (s->rowCount > 1 && s->groupCount > 0) {
        g_sortSet = s;
        qsort(s->rows, (size_t)s->rowCount, sizeof *s->rows, CompareRows);
        g_sortSet = NULL;
    }
}

/* ------------------------------------------------------------------- API */

BOOL SessionStore_Load(SessionSet *s)
{
    ProfileList profiles;
    Profiles_Load(&profiles);
    return SessionStore_LoadProfiles(s, &profiles);
}

BOOL SessionStore_LoadProfiles(SessionSet *s, const ProfileList *profiles)
{
    WCHAR projects[MAX_PATH];
    Transcripts t;
    int p;
    ZeroMemory(s, sizeof *s);
    ZeroMemory(&t, sizeof t);
    s->profiles = *profiles;
    s->noTranscripts = !SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) || !Util_DirExists(projects);
    LoadTranscripts(&t);
    for (p = 0; p < s->profiles.count; p++) LoadProfileEntries(s, p, &t);
    if (t.items) HeapFree(GetProcessHeap(), 0, t.items);
    GroupRows(s);
    LoadAllPending(s);
    LoadLive(s);
    return TRUE;
}

void SessionStore_Free(SessionSet *s)
{
    if (s->entries) HeapFree(GetProcessHeap(), 0, s->entries);
    if (s->rows) HeapFree(GetProcessHeap(), 0, s->rows);
    if (s->groups) HeapFree(GetProcessHeap(), 0, s->groups);
    ZeroMemory(s, sizeof *s);
}

/* The title a row goes by: a title someone gave it, the latest first, else
 * any title Claude gave it. */
const WCHAR *SessionStore_RowTitle(const SessionSet *s, const SessionRow *row)
{
    const SessionEntry *best = NULL;
    int p;
    for (p = 0; p < s->profiles.count; p++) {
        const SessionEntry *e = row->entry[p] >= 0 ? &s->entries[row->entry[p]] : NULL;
        if (!e || !e->title[0]) continue;
        if (!best || (e->userTitle && !best->userTitle) ||
            (e->userTitle == best->userTitle && e->lastActivity > best->lastActivity))
            best = e;
    }
    return best ? best->title : L"Untitled session";
}
