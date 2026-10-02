/*
 * The Claude Code sessions of every profile, as the sessions view shows
 * them. Read only (sessionedit.c changes them). Only the sessions of this PC:
 * one run over SSH, in WSL or in the cloud has its conversation elsewhere and
 * is only counted.
 *
 * A Code session is two things on disk: its transcript, in the Claude Code
 * folder every profile shares (~\.claude\projects\<project>\<id>.jsonl, or
 * under CLAUDE_CONFIG_DIR), and one entry per profile that lists it, in that
 * profile's claude-code-sessions\<account>\<organization>\local_<...>.json
 * (title, star, folder, last activity, and the session's other
 * transcripts: the ones it went on from). A row here is one transcript, with
 * the entries of each profile that lists it. Claude only shows the entries of
 * the account signed in (config.json, lastKnownAccountUuid) and of its
 * current organization, taken as the one with the latest entry.
 *
 * Paths are kept as Claude writes them. They can be longer than MAX_PATH (a
 * long project folder name, a long user name): the file functions get them
 * with the \\?\ prefix.
 *
 * Also read: the changes waiting for a profile to close (sessionedit.c), and
 * which profiles run a session right now: every running Claude Code writes
 * ~\.claude\sessions\<pid>.json (its session, its start time), and its parent
 * process is the Claude of the profile that opened it.
 */
#include "app.h"
#include <stdlib.h>
#include <tlhelp32.h>

#define PENDING_MAX_BYTES           (1024u * 1024u)
#define ENTRIES_DIR                 L"claude-code-sessions"
#define SCRATCH_DIR                 L"scratch-workspaces"
#define CLAUDE_CODE_ENVIRONMENT     L"ccd-environment-config.json"   /* Claude's settings for the Claude Code it starts */
#define TRANSCRIPT_EXTENSION        L".jsonl"
#define TRANSCRIPT_EXTENSION_LENGTH (ARRAYSIZE(TRANSCRIPT_EXTENSION) - 1)
#define LOCAL_ID_PREFIX             L"local_"   /* an entry's own id: local_<uuid> */
#define FIRST_CAPACITY              64    /* items a growing array starts with */
#define INDEX_FIRST_SLOTS           128   /* the row index, kept at most half full, starts with these */
#define TITLE_SOURCE_CCH            16    /* "user" or "auto" */
#define START_TIME_CCH              32    /* a FILETIME in decimal */

typedef struct Transcript {
    WCHAR     id[SESSION_ID_CCH];
    WCHAR     path[LONG_PATH_CCH];
    ULONGLONG bytes;
} Transcript;

typedef struct Transcripts {
    Transcript *items;
    int         count, capacity;
} Transcripts;

typedef struct ProcessParent {
    DWORD process, parent;
} ProcessParent;

/* What reading one entry file gave. */
typedef enum EntryOutcome {
    ENTRY_LISTED,                     /* a session of this PC, added */
    ENTRY_COUNTED,                    /* a session run elsewhere: counted only */
    ENTRY_BROKEN,                     /* no session entry (cut short, say): counted as unreadable */
    ENTRY_NO_MEMORY
} EntryOutcome;

/* What reading one profile's entries gave. */
typedef enum EntriesRead {
    ENTRIES_COMPLETE,                 /* an entries folder listed to its end, every entry read */
    ENTRIES_PARTIAL,                  /* no entries folder, or one of them could not be read */
    ENTRIES_NO_MEMORY
} EntriesRead;

/* Rows by key: open addressing, the ids case-folded as Core_EqualsI compares
 * them (Core_HashIgnoringCase). */
struct SessionRowIndex {
    int *slots;                       /* row index plus one; zero is empty */
    int capacity;
    BOOL fallback;                    /* no memory: every row is searched */
};

static BOOL Cancelled(HANDLE cancel)
{
    return cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0;
}

static void *Grow(void *items, int *capacity, int need, size_t size)
{
    void *bigger;
    int n;
    if (need <= *capacity) return items;
    n = *capacity ? *capacity * 2 : FIRST_CAPACITY;
    while (n < need) n *= 2;
    bigger = items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, items, (size_t)n * size)
                   : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)n * size);
    if (!bigger) return NULL;
    *capacity = n;
    return bigger;
}

static BOOL MemberString(const char *json, size_t length, const char *key, WCHAR *out, size_t cch)
{
    const char *value;
    size_t valueLength;
    out[0] = 0;
    return Core_JsonMember(json, length, key, &value, &valueLength) && Core_JsonString(value, valueLength, out, cch);
}

/* A text to show: a value longer than `out` keeps its beginning. */
static void MemberText(const char *json, size_t length, const char *key, WCHAR *out, size_t cch)
{
    const char *value;
    size_t valueLength, keep;
    WCHAR *whole;
    if (MemberString(json, length, key, out, cch) || !Core_JsonMember(json, length, key, &value, &valueLength)) return;
    /* A JSON string decodes to at most one character per byte. */
    whole = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (valueLength + 1) * sizeof(WCHAR));
    if (whole && Core_JsonString(value, valueLength, whole, valueLength + 1)) {
        keep = min(wcslen(whole), cch - 1);
        if (keep > 0 && IS_HIGH_SURROGATE(whole[keep - 1])) keep--;
        StringCchCopyNW(out, cch, whole, keep);
    }
    if (whole) HeapFree(GetProcessHeap(), 0, whole);
}

static BOOL MemberTrue(const char *json, size_t length, const char *key)
{
    const char *value;
    size_t valueLength;
    return Core_JsonMember(json, length, key, &value, &valueLength) && Core_JsonTrue(value, valueLength);
}

static ULONGLONG MemberNumber(const char *json, size_t length, const char *key)
{
    const char *value;
    size_t valueLength;
    ULONGLONG number = 0;
    if (Core_JsonMember(json, length, key, &value, &valueLength)) Core_JsonNumber(value, valueLength, &number);
    return number;
}

/* The next string of the JSON array `raw` (as Core_JsonMember gives it),
 * from `*at` (0 at its start), decoded into `out` ("" when it does not fit).
 * FALSE at the end of the array, or at a value that is not a string. */
static BOOL NextArrayString(const char *raw, size_t length, size_t *at, WCHAR *out, size_t cch)
{
    size_t i = *at, end;
    if (i == 0) {
        if (length == 0 || raw[0] != '[') return FALSE;
        i = 1;
    }
    while (i < length && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n' || raw[i] == ',')) i++;
    if (i >= length || raw[i] != '"') return FALSE;
    for (end = i + 1; end < length && raw[end] != '"'; end++)
        if (raw[end] == '\\') end++;
    if (end >= length) return FALSE;
    *at = end + 1;
    if (!Core_JsonString(raw + i, end + 1 - i, out, cch)) out[0] = 0;
    return TRUE;
}

/* A path in Claude Code's folder: CLAUDE_CONFIG_DIR when it is set, else
 * ~\.claude, as Claude Code finds it. FALSE when the variable is too long
 * (logged once): ~\.claude instead would read, and delete, another folder. */
BOOL SessionStore_ClaudeCodePath(const WCHAR *sub, WCHAR *out, size_t cch)
{
    static LONG tooLongLogged;
    WCHAR value[MAX_PATH], full[MAX_PATH];
    const WCHAR *format = L"%s\\%s";
    DWORD length = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", value, ARRAYSIZE(value));
    if (length >= ARRAYSIZE(value)) {
        if (!InterlockedExchange(&tooLongLogged, 1))
            Util_Log(L"CLAUDE_CONFIG_DIR has %lu characters: Claude Code's sessions are not read", length - 1);
        return FALSE;
    }
    if (length == 0) {
        length = GetEnvironmentVariableW(L"USERPROFILE", value, ARRAYSIZE(value));
        if (length == 0 || length >= ARRAYSIZE(value)) return FALSE;
        format = L"%s\\.claude\\%s";
    }
    /* The \\?\ form needs a full path with backslashes only, and no separator at its end. */
    length = GetFullPathNameW(value, ARRAYSIZE(full), full, NULL);
    if (length == 0 || length >= ARRAYSIZE(full)) return FALSE;
    while (length > 0 && full[length - 1] == L'\\') full[--length] = 0;
    return SUCCEEDED(StringCchPrintfW(out, cch, format, full, sub));
}

/* ------------------------------------------------------------ transcripts */

BOOL SessionStore_ProjectsDir(WCHAR *out, size_t cch)
{
    return SessionStore_ClaudeCodePath(L"projects", out, cch);
}

static int __cdecl CompareTranscripts(const void *a, const void *b)
{
    return CompareStringOrdinal(((const Transcript *)a)->id, -1, ((const Transcript *)b)->id, -1, TRUE) - CSTR_EQUAL;
}

/* Every <id>.jsonl directly inside a project folder. A transcript Claude Code
 * continued from another folder can exist twice: the larger one counts.
 * FALSE when memory ran out: the list would be partial. */
static BOOL LoadTranscripts(Transcripts *transcripts, HANDLE cancel)
{
    WCHAR root[LONG_PATH_CCH], folder[LONG_PATH_CCH];
    WIN32_FIND_DATAW project, file;
    HANDLE projects, files;
    BOOL enough = TRUE;
    if (!SessionStore_ProjectsDir(root, ARRAYSIZE(root))) return TRUE;
    projects = Util_FindFiles(root, L"*", &project, TRUE);
    if (projects == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if (Cancelled(cancel)) break;
        if (!(project.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || project.cFileName[0] == L'.' ||
            FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", root, project.cFileName)))
            continue;
        files = Util_FindFiles(folder, L"*" TRANSCRIPT_EXTENSION, &file, FALSE);
        if (files == INVALID_HANDLE_VALUE) continue;
        do {
            Transcript *grown, *transcript;
            size_t idLength;
            if (Cancelled(cancel)) break;
            if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Core_EndsWithI(file.cFileName, TRANSCRIPT_EXTENSION)) continue;
            idLength = wcslen(file.cFileName) - TRANSCRIPT_EXTENSION_LENGTH;
            if (idLength == 0 || idLength >= SESSION_ID_CCH) continue;
            grown = (Transcript *)Grow(transcripts->items, &transcripts->capacity, transcripts->count + 1, sizeof *transcripts->items);
            if (!grown) {
                enough = FALSE;
                break;
            }
            transcripts->items = grown;
            transcript = &transcripts->items[transcripts->count];
            if (FAILED(StringCchPrintfW(transcript->path, ARRAYSIZE(transcript->path), L"%s\\%s", folder, file.cFileName))) continue;
            StringCchCopyNW(transcript->id, ARRAYSIZE(transcript->id), file.cFileName, idLength);
            transcript->bytes = ((ULONGLONG)file.nFileSizeHigh << 32) | file.nFileSizeLow;
            transcripts->count++;
        } while (FindNextFileW(files, &file));
        FindClose(files);
    } while (enough && FindNextFileW(projects, &project));
    FindClose(projects);
    if (transcripts->count > 1) qsort(transcripts->items, (size_t)transcripts->count, sizeof *transcripts->items, CompareTranscripts);
    return enough;
}

static const Transcript *FindTranscript(const Transcripts *transcripts, const WCHAR *id)
{
    Transcript key;
    const Transcript *hit;
    if (!transcripts->count || FAILED(StringCchCopyW(key.id, ARRAYSIZE(key.id), id))) return NULL;
    hit = (const Transcript *)bsearch(&key, transcripts->items, (size_t)transcripts->count, sizeof *transcripts->items, CompareTranscripts);
    /* Duplicates sit side by side: take the largest. */
    if (hit) {
        const Transcript *best = hit, *p;
        for (p = hit; p > transcripts->items && CompareTranscripts(p - 1, &key) == 0; p--)
            if (p[-1].bytes > best->bytes) best = p - 1;
        for (p = hit; p + 1 < transcripts->items + transcripts->count && CompareTranscripts(p + 1, &key) == 0; p++)
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
    return p->storageDir[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" ENTRIES_DIR, p->storageDir));
}

/* Until the entries exist, a notification on their closest existing parent
 * catches the creation of every missing directory without creating it. */
BOOL SessionStore_WatchDir(const Profile *p, WCHAR *out, size_t cch)
{
    return SessionStore_SessionsDir(p, out, cch) && Util_ExistingDir(out, out, cch);
}

/* Entries and transcripts retain the paths seen inside Claude's package.
 * Explorer and file operations need the corresponding path outside it. */
BOOL SessionStore_WorkingDir(const SessionSet *set, const WCHAR *cwd, WCHAR *out, size_t cch)
{
    int p;
    for (p = 0; p < set->profiles.count; p++) {
        const Profile *profile = &set->profiles.items[p];
        if (Core_PathUnder(cwd, profile->dataDir)) return Core_ProfileFilePath(profile, cwd, out, cch);
    }
    return SUCCEEDED(StringCchCopyW(out, cch, cwd));
}

/* The subfolder of `dir` whose local_*.json is the most recent: an
 * organization folder, or with `holdsAccounts` the most recent organization
 * folder of any account folder. */
static BOOL NewestSubfolder(const WCHAR *dir, WCHAR *out, size_t cch, BOOL holdsAccounts, ULONGLONG *modified, HANDLE cancel)
{
    WCHAR child[LONG_PATH_CCH], organization[LONG_PATH_CCH], newestFolder[LONG_PATH_CCH];
    WIN32_FIND_DATAW sub, entry;
    ULONGLONG best = 0;
    HANDLE subfolders, entries;
    BOOL found = FALSE;
    subfolders = Util_FindFiles(dir, L"*", &sub, TRUE);
    if (subfolders == INVALID_HANDLE_VALUE) return FALSE;
    do {
        ULONGLONG newest = 1;   /* an empty folder still beats none */
        const WCHAR *candidate = child;
        if (Cancelled(cancel)) break;
        if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sub.cFileName[0] == L'.') continue;
        if (FAILED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", dir, sub.cFileName))) continue;
        if (holdsAccounts) {
            if (!NewestSubfolder(child, organization, ARRAYSIZE(organization), FALSE, &newest, cancel)) continue;
            candidate = organization;
        } else if ((entries = Util_FindFiles(child, L"local_*.json", &entry, FALSE)) != INVALID_HANDLE_VALUE) {
            do {
                ULONGLONG written = ((ULONGLONG)entry.ftLastWriteTime.dwHighDateTime << 32) | entry.ftLastWriteTime.dwLowDateTime;
                if (Cancelled(cancel)) break;
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && written > newest) newest = written;
            } while (FindNextFileW(entries, &entry));
            FindClose(entries);
        }
        if (newest > best && SUCCEEDED(StringCchCopyW(newestFolder, ARRAYSIZE(newestFolder), candidate))) {
            best = newest;
            found = TRUE;
        }
    } while (FindNextFileW(subfolders, &sub));
    FindClose(subfolders);
    found = found && SUCCEEDED(StringCchCopyW(out, cch, newestFolder));
    if (modified && found) *modified = best;
    return found;
}

/* A name that stays one folder down: no separator, no "." or "..". */
static BOOL IsPlainFolderName(const WCHAR *name)
{
    const WCHAR *at;
    if (!name[0] || wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) return FALSE;
    for (at = name; *at; at++)
        if (*at < 0x20 || wcschr(L"\\/:*?\"<>|", *at)) return FALSE;
    return TRUE;
}

/* The folder holding the entries Claude shows for this profile: those of the
 * account config.json names (none when that account has no entries folder:
 * Claude does not show another account's entries), else, when no account is
 * named, those of the account with the latest entry. */
static BOOL EntriesDir(const Profile *p, WCHAR *out, size_t cch, BOOL *signedIn, HANDLE cancel)
{
    WCHAR root[LONG_PATH_CCH], config[LONG_PATH_CCH], account[SESSION_ID_CCH], dir[LONG_PATH_CCH];
    DWORD length = 0;
    char *json;
    account[0] = 0;
    *signedIn = FALSE;
    if (!p->storageDir[0]) return FALSE;
    if (SUCCEEDED(StringCchPrintfW(config, ARRAYSIZE(config), L"%s\\" CLAUDE_APP_SETTINGS, p->storageDir)) &&
        (json = Util_ReadFile(config, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) != NULL) {
        MemberString(json, length, "lastKnownAccountUuid", account, ARRAYSIZE(account));
        HeapFree(GetProcessHeap(), 0, json);
    }
    *signedIn = account[0] != 0;
    if (!SessionStore_SessionsDir(p, root, ARRAYSIZE(root))) return FALSE;
    if (account[0])
        return IsPlainFolderName(account) && SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", root, account)) &&
               NewestSubfolder(dir, out, cch, FALSE, NULL, cancel);
    return NewestSubfolder(root, out, cch, TRUE, NULL, cancel);
}

int SessionStore_FindRow(const SessionSet *set, const WCHAR *key)
{
    const SessionRowIndex *index = set->index;
    int i;
    if (index && !index->fallback && index->capacity) {
        int slot = (int)(Core_HashIgnoringCase(key) & (DWORD)(index->capacity - 1));
        while (index->slots[slot]) {
            i = index->slots[slot] - 1;
            if (Core_EqualsI(set->rows[i].key, key)) return i;
            slot = (slot + 1) & (index->capacity - 1);
        }
        return -1;
    }
    for (i = 0; i < set->rowCount; i++)
        if (Core_EqualsI(set->rows[i].key, key)) return i;
    return -1;
}

/* Adds row `row` to the index, made or rebuilt as the rows grow. Without
 * memory the index falls back to searching every row. */
static void IndexRow(SessionSet *set, int row)
{
    SessionRowIndex *index = set->index;
    int first = row, i;
    if (!index) {
        if ((index = (SessionRowIndex *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *index)) == NULL) return;
        set->index = index;
    }
    if (index->fallback) return;
    if (set->rowCount > index->capacity / 2) {
        int capacity = index->capacity ? index->capacity * 2 : INDEX_FIRST_SLOTS;
        int *slots;
        while (capacity / 2 < set->rowCount) capacity *= 2;
        slots = (int *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof *slots);
        if (!slots) { index->fallback = TRUE; return; }
        if (index->slots) HeapFree(GetProcessHeap(), 0, index->slots);
        index->slots = slots;
        index->capacity = capacity;
        first = 0;
    }
    for (i = first; i <= row; i++) {
        int slot = (int)(Core_HashIgnoringCase(set->rows[i].key) & (DWORD)(index->capacity - 1));
        while (index->slots[slot]) slot = (slot + 1) & (index->capacity - 1);
        index->slots[slot] = i + 1;
    }
}

static void FreeIndex(SessionSet *set)
{
    if (!set->index) return;
    if (set->index->slots) HeapFree(GetProcessHeap(), 0, set->index->slots);
    HeapFree(GetProcessHeap(), 0, set->index);
    set->index = NULL;
}

/* The entry of profile `profile` that a queued change names: the entry of
 * its session, or of the session one of whose entries has the change's key
 * as its own id (a change made before the session's first message keeps
 * that id). It is the entry the row names, the one Claude goes on with; -1
 * when the profile lists none. `row` (when not NULL) gets the session's row. */
int SessionStore_FindEntry(const SessionSet *set, int profile, const WCHAR *key, int *row)
{
    int r = SessionStore_FindRow(set, key), entry;
    if (r < 0 || set->rows[r].entry[profile] < 0) {
        for (r = 0; r < set->rowCount; r++) {
            for (entry = set->rows[r].entry[profile]; entry >= 0; entry = set->entries[entry].duplicate)
                if (Core_EqualsI(set->entries[entry].localId, key)) break;
            if (entry >= 0) break;
        }
        if (r == set->rowCount) r = -1;
    }
    if (row) *row = r;
    return r >= 0 ? set->rows[r].entry[profile] : -1;
}

/* An entry's own id as Claude names its marks: without local_. */
const WCHAR *SessionStore_OwnId(const SessionEntry *entry)
{
    size_t prefix = ARRAYSIZE(LOCAL_ID_PREFIX) - 1;
    return wcsncmp(entry->localId, LOCAL_ID_PREFIX, prefix) == 0 ? entry->localId + prefix : entry->localId;
}

static BOOL AddOtherTranscript(SessionSet *set, SessionEntry *entry, const WCHAR *id, const WCHAR *own)
{
    WCHAR (*grown)[SESSION_ID_CCH];
    int i;
    if (!Core_IsUuid(id) || Core_EqualsI(id, own)) return TRUE;
    for (i = 0; i < entry->otherTranscriptCount; i++)
        if (Core_EqualsI(set->otherTranscriptIds[entry->firstOtherTranscript + i], id)) return TRUE;
    grown = (WCHAR (*)[SESSION_ID_CCH])Grow(set->otherTranscriptIds, &set->otherTranscriptCapacity, set->otherTranscriptCount + 1,
                                            sizeof *set->otherTranscriptIds);
    if (!grown) return FALSE;
    set->otherTranscriptIds = grown;
    StringCchCopyW(set->otherTranscriptIds[set->otherTranscriptCount++], ARRAYSIZE(*grown), id);
    entry->otherTranscriptCount++;
    return TRUE;
}

/* The session's other transcripts, as Claude counts them: those it went on
 * from (priorCliSessionIds), the one before a /clear and one it took up
 * again when the session left the archive. Only UUIDs other than `own`
 * (the session's transcript): they name files. */
static BOOL AddOtherTranscripts(SessionSet *set, SessionEntry *entry, const char *json, size_t length, const WCHAR *own)
{
    static const char *const singleIds[] = { "preClearCliSessionId", "unarchivedCliSessionId" };
    WCHAR id[SESSION_ID_CCH];
    const char *value;
    size_t valueLength, at = 0;
    int i;
    entry->firstOtherTranscript = set->otherTranscriptCount;
    entry->otherTranscriptCount = 0;
    for (i = 0; i < (int)ARRAYSIZE(singleIds); i++)
        if (MemberString(json, length, singleIds[i], id, ARRAYSIZE(id)) && !AddOtherTranscript(set, entry, id, own)) return FALSE;
    if (Core_JsonMember(json, length, "priorCliSessionIds", &value, &valueLength))
        while (NextArrayString(value, valueLength, &at, id, ARRAYSIZE(id)))
            if (!AddOtherTranscript(set, entry, id, own)) return FALSE;
    return TRUE;
}

/* Adds an entry of profile `profile` to the row of its session, made when it
 * is the session's first. */
static EntryOutcome AddEntry(SessionSet *set, int profile, const WCHAR *file, const char *json, size_t length,
                             const Transcripts *transcripts)
{
    SessionEntry entry;
    WCHAR cliSessionId[SESSION_ID_CCH], titleSource[TITLE_SOURCE_CCH], cwd[MAX_PATH], originCwd[MAX_PATH];
    const WCHAR *key;
    SessionEntry *entries;
    SessionRow *rows;
    int row, added, listed;

    ZeroMemory(&entry, sizeof entry);
    /* A session run over SSH, in WSL or moved to the cloud keeps its
     * conversation elsewhere: only this PC's sessions are listed. */
    switch (Core_SessionEntryKind(json, length)) {
    case ENTRY_ELSEWHERE:
        set->source[profile].elsewhere++;
        return ENTRY_COUNTED;
    case ENTRY_NOT_SESSION:
        set->source[profile].unreadable++;
        return ENTRY_BROKEN;
    case ENTRY_LOCAL:
        break;
    }
    if (!MemberString(json, length, "sessionId", entry.localId, ARRAYSIZE(entry.localId)) || !entry.localId[0]) {
        set->source[profile].unreadable++;
        return ENTRY_BROKEN;
    }
    StringCchCopyW(entry.file, ARRAYSIZE(entry.file), file);
    MemberText(json, length, "title", entry.title, ARRAYSIZE(entry.title));
    MemberString(json, length, "titleSource", titleSource, ARRAYSIZE(titleSource));
    entry.userTitle = entry.title[0] && Core_EqualsI(titleSource, L"user");
    entry.starred = MemberTrue(json, length, "isStarred");
    entry.archived = MemberTrue(json, length, "isArchived");
    entry.lastActivity = MemberNumber(json, length, "lastActivityAt");
    entry.pendingStar = -1;
    entry.duplicate = -1;
    MemberString(json, length, "cliSessionId", cliSessionId, ARRAYSIZE(cliSessionId));
    MemberString(json, length, "cwd", cwd, ARRAYSIZE(cwd));
    MemberString(json, length, "originCwd", originCwd, ARRAYSIZE(originCwd));
    key = cliSessionId[0] ? cliSessionId : entry.localId;
    if (!AddOtherTranscripts(set, &entry, json, length, key)) return ENTRY_NO_MEMORY;

    entries = (SessionEntry *)Grow(set->entries, &set->entryCap, set->entryCount + 1, sizeof *set->entries);
    if (!entries) return ENTRY_NO_MEMORY;
    set->entries = entries;
    row = SessionStore_FindRow(set, key);
    if (row < 0) {
        const Transcript *transcript = cliSessionId[0] ? FindTranscript(transcripts, cliSessionId) : NULL;
        rows = (SessionRow *)Grow(set->rows, &set->rowCap, set->rowCount + 1, sizeof *set->rows);
        if (!rows) return ENTRY_NO_MEMORY;
        set->rows = rows;
        row = set->rowCount++;
        ZeroMemory(&set->rows[row], sizeof set->rows[row]);
        StringCchCopyW(set->rows[row].key, ARRAYSIZE(set->rows[row].key), key);
        FillMemory(set->rows[row].entry, sizeof set->rows[row].entry, 0xFF);   /* -1: not listed */
        set->rows[row].transcript = transcript != NULL;
        set->rows[row].transcriptBytes = transcript ? transcript->bytes : 0;
        if (transcript) StringCchCopyW(set->rows[row].transcriptPath, ARRAYSIZE(set->rows[row].transcriptPath), transcript->path);
        IndexRow(set, row);
    }
    /* The same session twice in one profile: the row names the latest entry,
     * the one Claude goes on with; the others follow it, changed with it. */
    added = set->entryCount++;
    listed = set->rows[row].entry[profile];
    if (listed < 0) {
        set->rows[row].entry[profile] = added;
    } else if (entry.lastActivity > set->entries[listed].lastActivity) {
        entry.duplicate = listed;
        set->rows[row].entry[profile] = added;
    } else {
        entry.duplicate = set->entries[listed].duplicate;
        set->entries[listed].duplicate = added;
    }
    set->entries[added] = entry;
    /* The folder of the latest entry: after Claude Code moved a session to
     * another folder, that is where it goes on. */
    if (entry.lastActivity >= set->rows[row].lastActivity) {
        set->rows[row].lastActivity = entry.lastActivity;
        StringCchCopyW(set->rows[row].cwd, ARRAYSIZE(set->rows[row].cwd), originCwd[0] ? originCwd : cwd);
    }
    return ENTRY_LISTED;
}

/* Claude's settings give the Claude Code it starts environment variables of
 * their own (encrypted): a CLAUDE_CONFIG_DIR there is not followed here. */
static BOOL HasClaudeCodeEnvironment(const Profile *p)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    char *json;
    BOOL set = FALSE;
    if (!p->storageDir[0] || FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_CODE_ENVIRONMENT, p->storageDir))) return FALSE;
    if ((json = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) != NULL) {
        const char *value;
        size_t valueLength;
        set = Core_JsonMember(json, length, "envVars", &value, &valueLength) && valueLength > 2 && value[0] == '"';
        HeapFree(GetProcessHeap(), 0, json);
    }
    return set;
}

/* Logs a profile's resolved source once per state, not on every reload. */
static void LogSource(const Profile *p, const SessionSource *source)
{
    static struct { ULONGLONG folder, state; } logged[MAX_PROFILES * 2];
    static int next;
    static SRWLOCK lock = SRWLOCK_INIT;
    ULONGLONG folder = Core_HashText(CORE_HASH_START, p->folder), state;
    BOOL environment = HasClaudeCodeEnvironment(p);
    const WCHAR *reason;
    int i;
    state = Core_HashText(folder, p->storageDir);
    state = Core_HashText(state, source->entriesDir);
    state = Core_HashBytes(state, &source->signedIn, sizeof source->signedIn);
    state = Core_HashBytes(state, &environment, sizeof environment);
    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < (int)ARRAYSIZE(logged) && logged[i].folder != folder; i++) {}
    if (i < (int)ARRAYSIZE(logged) && logged[i].state == state) {
        ReleaseSRWLockExclusive(&lock);
        return;
    }
    if (i == (int)ARRAYSIZE(logged)) {
        i = next;
        next = (next + 1) % (int)ARRAYSIZE(logged);
    }
    logged[i].folder = folder;
    logged[i].state = state;
    ReleaseSRWLockExclusive(&lock);
    if (!p->storageDir[0]) reason = L"profile storage could not be resolved";
    else if (source->entriesDir[0]) reason = source->signedIn ? L"account entries resolved" : L"stored entries found without an account in config.json";
    else if (source->signedIn) reason = L"account known; no session entries folder";
    else reason = L"no account or entries found; config.json may be missing, unreadable or have no account";
    Util_Log(L"sessions in %s: data=%s; entries=%s; %s%s", p->folder, p->storageDir, source->entriesDir, reason,
             environment ? L"; Claude Code variables set in Claude's settings (a CLAUDE_CONFIG_DIR there is not followed)" : L"");
}

/* One profile's entries. Complete when an entries folder was found and
 * listed to its end, and every entry in it read as one: an entry Claude cut
 * short leaves it partial, as one that cannot be read. */
static EntriesRead LoadProfileEntries(SessionSet *set, int profile, const Transcripts *transcripts, HANDLE cancel)
{
    WCHAR path[LONG_PATH_CCH];
    const Profile *p = &set->profiles.items[profile];
    SessionSource *source = &set->source[profile];
    WIN32_FIND_DATAW found;
    HANDLE entries;
    EntriesRead read = ENTRIES_COMPLETE;
    BOOL located = TRUE;
    if (!EntriesDir(p, source->entriesDir, ARRAYSIZE(source->entriesDir), &source->signedIn, cancel)) {
        source->entriesDir[0] = 0;
        located = FALSE;
    }
    /* A read cancelled while looking for the folder found nothing to tell. */
    if (Cancelled(cancel)) return ENTRIES_PARTIAL;
    LogSource(p, source);
    if (!located) return ENTRIES_PARTIAL;
    if (!Core_ScratchDirFor(p->dataDir, source->entriesDir, source->scratchDir, ARRAYSIZE(source->scratchDir))) source->scratchDir[0] = 0;
    entries = Util_FindFiles(source->entriesDir, L"local_*.json", &found, FALSE);
    if (entries == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? ENTRIES_COMPLETE : ENTRIES_PARTIAL;
    do {
        DWORD length;
        char *json;
        EntryOutcome outcome;
        if (Cancelled(cancel)) {
            read = ENTRIES_PARTIAL;
            break;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", source->entriesDir, found.cFileName)) ||
            (json = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) == NULL) {
            source->unreadable++;
            read = ENTRIES_PARTIAL;
            continue;
        }
        outcome = AddEntry(set, profile, path, json, length, transcripts);
        HeapFree(GetProcessHeap(), 0, json);
        if (outcome == ENTRY_NO_MEMORY) {
            read = ENTRIES_NO_MEMORY;
            break;
        }
        if (outcome == ENTRY_BROKEN) read = ENTRIES_PARTIAL;
    } while (FindNextFileW(entries, &found));
    if (read == ENTRIES_COMPLETE && GetLastError() != ERROR_NO_MORE_FILES) read = ENTRIES_PARTIAL;
    FindClose(entries);
    return read;
}

/* ------------------------------------------------- the other transcripts */

static BOOL HasOtherTranscripts(const SessionSet *set, int row)
{
    int p, entry;
    for (p = 0; p < set->profiles.count; p++)
        for (entry = set->rows[row].entry[p]; entry >= 0; entry = set->entries[entry].duplicate)
            if (set->entries[entry].otherTranscriptCount) return TRUE;
    return FALSE;
}

/* The ids of row `row`'s transcripts, each once: its own (its key, when it
 * is a UUID), then the other ones its entries name. A heap array of
 * pointers into `set` (HeapFree it); NULL without memory. */
const WCHAR **SessionStore_TranscriptIds(const SessionSet *set, int row, int *count)
{
    const SessionRow *session = &set->rows[row];
    const WCHAR **ids;
    int bound = 1, p, entry, i, seen;
    *count = 0;
    for (p = 0; p < set->profiles.count; p++)
        for (entry = session->entry[p]; entry >= 0; entry = set->entries[entry].duplicate)
            bound += set->entries[entry].otherTranscriptCount;
    ids = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)bound * sizeof *ids);
    if (!ids) return NULL;
    if (Core_IsUuid(session->key)) ids[(*count)++] = session->key;
    for (p = 0; p < set->profiles.count; p++)
        for (entry = session->entry[p]; entry >= 0; entry = set->entries[entry].duplicate)
            for (i = 0; i < set->entries[entry].otherTranscriptCount; i++) {
                const WCHAR *id = set->otherTranscriptIds[set->entries[entry].firstOtherTranscript + i];
                for (seen = 0; seen < *count && !Core_EqualsI(ids[seen], id); seen++) {}
                if (seen == *count) ids[(*count)++] = id;
            }
    return ids;
}

/* `id` belongs to a session other than row `row`'s that profile `profile`
 * lists (any profile when -1): its transcript, one of its other transcripts,
 * or the own id of one of its entries. Claude keeps what such an id names
 * for that session. */
BOOL SessionStore_ClaimedByOther(const SessionSet *set, int row, int profile, const WCHAR *id)
{
    int r, p, entry, i;
    for (r = 0; r < set->rowCount; r++) {
        if (r == row || (profile >= 0 && set->rows[r].entry[profile] < 0)) continue;
        if (Core_EqualsI(set->rows[r].key, id)) return TRUE;
        for (p = 0; p < set->profiles.count; p++)
            for (entry = set->rows[r].entry[p]; entry >= 0; entry = set->entries[entry].duplicate) {
                if (Core_EqualsI(SessionStore_OwnId(&set->entries[entry]), id)) return TRUE;
                for (i = 0; i < set->entries[entry].otherTranscriptCount; i++)
                    if (Core_EqualsI(set->otherTranscriptIds[set->entries[entry].firstOtherTranscript + i], id)) return TRUE;
            }
    }
    return FALSE;
}

/* A conversation's size counts its other transcripts too, except one that
 * another session goes on with. */
static void AddOtherTranscriptBytes(SessionSet *set, const Transcripts *transcripts, HANDLE cancel)
{
    int r, i, count;
    for (r = 0; r < set->rowCount && !Cancelled(cancel); r++) {
        const WCHAR **ids;
        if (!HasOtherTranscripts(set, r) || (ids = SessionStore_TranscriptIds(set, r, &count)) == NULL) continue;
        for (i = 0; i < count; i++) {
            const Transcript *other;
            if (Core_EqualsI(ids[i], set->rows[r].key) || SessionStore_ClaimedByOther(set, r, -1, ids[i])) continue;
            if ((other = FindTranscript(transcripts, ids[i])) != NULL) set->rows[r].transcriptBytes += other->bytes;
        }
        HeapFree(GetProcessHeap(), 0, (void *)ids);
    }
}

/* ---------------------------------------------------------------- pending */

BOOL SessionStore_PendingPath(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\pending-sessions-%s.txt", state, p->folder));
}

/* The changes waiting for the profile to close, oldest first (UTF-8, one per
 * line, see Core_PendingParse). -1 when the file is there but cannot be read:
 * rewriting it then would lose them. */
int SessionStore_LoadPending(const Profile *p, PendingEdit *edits, int capacity)
{
    WCHAR path[MAX_PATH], *text, *line, *next;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    DWORD length = 0;
    char *raw;
    int n = 0, wide;
    if (!SessionStore_PendingPath(p, path, ARRAYSIZE(path))) return -1;
    if ((raw = Util_ReadFile(path, PENDING_MAX_BYTES, FALSE, &length)) == NULL) {
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            return attributes.nFileSizeHigh || attributes.nFileSizeLow ? -1 : 0;
        return Util_QueryPath(path, NULL) == PATH_MISSING ? 0 : -1;
    }
    wide = MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, NULL, 0);
    text = wide > 0 ? (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)wide + 1) * sizeof(WCHAR)) : NULL;
    if (!text) {
        HeapFree(GetProcessHeap(), 0, raw);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, text, wide);
    text[wide] = 0;
    for (line = text; line && n < capacity; line = next) {
        size_t lineLength;
        next = wcschr(line, L'\n');
        if (next) *next++ = 0;
        lineLength = wcslen(line);
        if (lineLength && line[lineLength - 1] == L'\r') line[--lineLength] = 0;
        if (lineLength && Core_PendingParse(line, &edits[n])) n++;
    }
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, raw);
    return n;
}

/* What waits for each profile, shown on its entries: the latest change of
 * each kind wins. A profile's count is what its entries can take now. */
static void LoadAllPending(SessionSet *set, HANDLE cancel)
{
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    int p, i, n, found;
    if (!edits) return;
    for (p = 0; p < set->profiles.count; p++) {
        if (Cancelled(cancel)) break;
        n = SessionStore_LoadPending(&set->profiles.items[p], edits, SESSION_PENDING_MAX);
        for (i = 0; i < n; i++) {
            SessionEntry *entry;
            if ((found = SessionStore_FindEntry(set, p, edits[i].key, NULL)) < 0) continue;
            entry = &set->entries[found];
            entry->pending = TRUE;
            set->source[p].pending++;
            if (edits[i].op == PENDING_TITLE) StringCchCopyW(entry->pendingTitle, ARRAYSIZE(entry->pendingTitle), edits[i].value);
            else if (edits[i].op == PENDING_STAR) entry->pendingStar = edits[i].value[0] == L'1';
            else entry->pendingRemove = TRUE;
        }
    }
    HeapFree(GetProcessHeap(), 0, edits);
}

/* ------------------------------------------------------------------- live */

typedef void (*LiveSessionVisitor)(void *context, const WCHAR *sessionId, DWORD parent);

/* Each Claude Code running now, with its session and its parent process (0
 * when unknown): the record it keeps in ~\.claude\sessions, whose process
 * still runs with the start time written there (a pid used again does not
 * count). */
static void VisitLiveSessions(LiveSessionVisitor visit, void *context, HANDLE cancel)
{
    WCHAR dir[LONG_PATH_CCH], path[LONG_PATH_CCH], id[SESSION_ID_CCH], start[START_TIME_CCH];
    WIN32_FIND_DATAW found;
    PROCESSENTRY32W process;
    ProcessParent *parents = NULL;
    HANDLE records, snapshot;
    int count = 0, capacity = 0, i;
    if (!SessionStore_ClaudeCodePath(L"sessions", dir, ARRAYSIZE(dir)) ||
        (records = Util_FindFiles(dir, L"*.json", &found, FALSE)) == INVALID_HANDLE_VALUE)
        return;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        process.dwSize = sizeof process;
        if (Process32FirstW(snapshot, &process)) {
            do {
                ProcessParent *grown = (ProcessParent *)Grow(parents, &capacity, count + 1, sizeof *parents);
                if (!grown) break;
                parents = grown;
                parents[count].process = process.th32ProcessID;
                parents[count].parent = process.th32ParentProcessID;
                count++;
            } while (Process32NextW(snapshot, &process));
        }
        CloseHandle(snapshot);
    }
    do {
        DWORD length, pid = (DWORD)wcstoul(found.cFileName, NULL, 10), parent = 0;
        ULONGLONG started;
        FILETIME created, exited, kernel, user;
        HANDLE running;
        char *json;
        BOOL alive = FALSE;
        if (Cancelled(cancel)) break;
        if (!pid || (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) ||
            (json = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) == NULL)
            continue;
        MemberString(json, length, "sessionId", id, ARRAYSIZE(id));
        /* The process's creation time (a FILETIME), written as a string. */
        started = MemberString(json, length, "procStart", start, ARRAYSIZE(start)) ? _wcstoui64(start, NULL, 10)
                                                                                 : MemberNumber(json, length, "procStart");
        HeapFree(GetProcessHeap(), 0, json);
        if ((running = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) != NULL) {
            alive = GetProcessTimes(running, &created, &exited, &kernel, &user) &&
                    (((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime) == started;
            CloseHandle(running);
        }
        if (!alive || !id[0]) continue;
        for (i = 0; i < count && !parent; i++)
            if (parents[i].process == pid) parent = parents[i].parent;
        visit(context, id, parent);
    } while (FindNextFileW(records, &found));
    FindClose(records);
    if (parents) HeapFree(GetProcessHeap(), 0, parents);
}

/* The profiles of `profiles` whose Claude is process `parent`. */
static DWORD ProfilesRunning(const ProfileList *profiles, DWORD parent)
{
    DWORD running = 0;
    int p;
    for (p = 0; parent && p < profiles->count; p++)
        if (profiles->items[p].running && profiles->items[p].pid == parent) running |= 1u << p;
    return running;
}

static void MarkLive(void *context, const WCHAR *sessionId, DWORD parent)
{
    SessionSet *set = (SessionSet *)context;
    DWORD running = ProfilesRunning(&set->profiles, parent);
    int row;
    if (running && (row = SessionStore_FindRow(set, sessionId)) >= 0) set->rows[row].live |= running;
}

/* Which profiles run each session now: a live Claude Code whose parent is a
 * profile's Claude. */
static void LoadLive(SessionSet *set, HANDLE cancel)
{
    int p;
    for (p = 0; p < set->profiles.count && !set->profiles.items[p].running; p++) {}
    if (p < set->profiles.count) VisitLiveSessions(MarkLive, set, cancel);
}

typedef struct RunningSearch {
    const ProfileList *profiles;
    const WCHAR       *sessionId;
    DWORD              running;   /* the profiles whose Claude runs it */
    BOOL               outside;   /* a Claude Code no profile started runs it */
} RunningSearch;

static void FindRunning(void *context, const WCHAR *sessionId, DWORD parent)
{
    RunningSearch *search = (RunningSearch *)context;
    DWORD running;
    if (!Core_EqualsI(sessionId, search->sessionId)) return;
    running = ProfilesRunning(search->profiles, parent);
    search->running |= running;
    if (!running) search->outside = TRUE;
}

/* The profiles of `profiles` (with their running state as it is now) whose
 * Claude runs session `sessionId` now; `outside` tells whether a Claude Code
 * that none of them started runs it (in a terminal, for example). */
DWORD SessionStore_RunningNow(const ProfileList *profiles, const WCHAR *sessionId, BOOL *outside)
{
    RunningSearch search;
    ZeroMemory(&search, sizeof search);
    search.profiles = profiles;
    search.sessionId = sessionId;
    VisitLiveSessions(FindRunning, &search, NULL);
    *outside = search.outside;
    return search.running;
}

/* ----------------------------------------------------------------- groups */

static int CreateGroup(SessionSet *set, const SessionRow *row, int owner)
{
    SessionGroup *groups, *group;
    const WCHAR *leaf;
    groups = (SessionGroup *)Grow(set->groups, &set->groupCap, set->groupCount + 1, sizeof *set->groups);
    if (!groups) return -1;
    set->groups = groups;
    group = &set->groups[set->groupCount];
    ZeroMemory(group, sizeof *group);
    group->scratchOf = owner;
    if (owner < 0 && row->cwd[0]) {
        StringCchCopyW(group->path, ARRAYSIZE(group->path), row->cwd);
        leaf = wcsrchr(row->cwd, L'\\');
        StringCchCopyW(group->name, ARRAYSIZE(group->name), leaf && leaf[1] ? leaf + 1 : row->cwd);
    }
    return set->groupCount++;
}

/* The name a group is shown with, in the current language. */
void SessionStore_GroupName(const SessionSet *set, int group, WCHAR *out, size_t cch)
{
    const SessionGroup *shown = &set->groups[group];
    if (shown->scratchOf >= 0)
        StringCchPrintfW(out, cch, TR(L"No folder \x00B7 %s"), set->profiles.items[shown->scratchOf].name);
    else
        StringCchCopyW(out, cch, shown->name[0] ? shown->name : TR(L"Unknown folder"));
}

static int __cdecl CompareRows(void *context, const void *a, const void *b)
{
    const SessionSet *set = (const SessionSet *)context;
    const SessionRow *x = (const SessionRow *)a, *y = (const SessionRow *)b;
    const SessionGroup *gx = &set->groups[x->group], *gy = &set->groups[y->group];
    if (x->group != y->group) {
        if (gx->lastActivity != gy->lastActivity) return gx->lastActivity > gy->lastActivity ? -1 : 1;
        return x->group < y->group ? -1 : 1;
    }
    if (x->lastActivity != y->lastActivity) return x->lastActivity > y->lastActivity ? -1 : 1;
    /* Same time: by id, so that two reloads order them alike. */
    return CompareStringOrdinal(x->key, -1, y->key, -1, TRUE) - CSTR_EQUAL;
}

typedef struct GroupOrder {
    int row, owner;
} GroupOrder;

/* Rows of one profile's scratch area group together, others by folder. */
static int CompareGroupKeys(const SessionSet *set, const GroupOrder *a, const GroupOrder *b)
{
    if (a->owner != b->owner) return a->owner < b->owner ? -1 : 1;
    if (a->owner >= 0) return 0;
    return Core_PathCompare(set->rows[a->row].cwd, set->rows[b->row].cwd);
}

static int __cdecl CompareGroupOrder(void *context, const void *a, const void *b)
{
    const GroupOrder *x = (const GroupOrder *)a, *y = (const GroupOrder *)b;
    int key = CompareGroupKeys((const SessionSet *)context, x, y);
    if (key) return key;
    return x->row < y->row ? -1 : x->row != y->row;
}

/* Equal project keys are adjacent in the temporary order. Representatives
 * are their first input row, retaining the groups' original discovery order. */
static BOOL GroupRows(SessionSet *set, HANDLE cancel)
{
    GroupOrder *order;
    WCHAR (*scratch)[MAX_PATH];
    int r, p, representative = 0;
    BOOL ok = FALSE;
    if (!set->rowCount) return TRUE;
    order = (GroupOrder *)HeapAlloc(GetProcessHeap(), 0, (size_t)set->rowCount * sizeof *order);
    scratch = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, (size_t)max(set->profiles.count, 1) * sizeof *scratch);
    if (!order || !scratch) goto done;
    for (p = 0; p < set->profiles.count; p++)
        if (FAILED(StringCchPrintfW(scratch[p], ARRAYSIZE(scratch[p]), L"%s\\" SCRATCH_DIR, set->profiles.items[p].dataDir))) scratch[p][0] = 0;
    for (r = 0; r < set->rowCount; r++) {
        if (Cancelled(cancel)) goto done;
        order[r].row = r;
        order[r].owner = -1;
        for (p = 0; p < set->profiles.count && order[r].owner < 0; p++)
            if (scratch[p][0] && Core_PathUnder(set->rows[r].cwd, scratch[p])) order[r].owner = p;
    }
    qsort_s(order, (size_t)set->rowCount, sizeof *order, CompareGroupOrder, set);
    for (r = 0; r < set->rowCount; r++) {
        if (Cancelled(cancel)) goto done;
        if (!r || CompareGroupKeys(set, &order[r - 1], &order[r]) != 0) {
            representative = order[r].row;
            set->rows[representative].group = -2 - order[r].owner;   /* a group's first row: its owner, until the group exists */
        } else {
            set->rows[order[r].row].group = representative;
        }
    }
    /* A group's first row has the smallest index of its group: its group
     * exists before any other row of it is reached. */
    for (r = 0; r < set->rowCount; r++) {
        int first = set->rows[r].group, group;
        group = first < 0 ? CreateGroup(set, &set->rows[r], -2 - first) : set->rows[first].group;
        if (Cancelled(cancel) || group < 0) goto done;
        set->rows[r].group = group;
        if (set->rows[r].lastActivity > set->groups[group].lastActivity) set->groups[group].lastActivity = set->rows[r].lastActivity;
    }
    if (set->rowCount > 1) qsort_s(set->rows, (size_t)set->rowCount, sizeof *set->rows, CompareRows, set);
    ok = TRUE;
done:
    if (order) HeapFree(GetProcessHeap(), 0, order);
    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    return ok;
}

/* ------------------------------------------------------------------- API */

BOOL SessionStore_LoadProfiles(SessionSet *set, const ProfileList *profiles)
{
    return SessionStore_LoadProfilesCancel(set, profiles, NULL);
}

/* FALSE, with nothing kept, when the read was cancelled or memory ran out:
 * a partial snapshot would show sessions as gone. */
BOOL SessionStore_LoadProfilesCancel(SessionSet *set, const ProfileList *profiles, HANDLE cancel)
{
    WCHAR projects[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    Transcripts transcripts;
    BOOL enough;
    int p, r;
    ZeroMemory(set, sizeof *set);
    ZeroMemory(&transcripts, sizeof transcripts);
    if (Cancelled(cancel)) return FALSE;
    set->profiles = *profiles;
    set->noTranscripts = !SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) ||
                         !Util_ExtendedPath(projects, extended, ARRAYSIZE(extended)) || !Util_DirExists(extended);
    enough = LoadTranscripts(&transcripts, cancel);
    for (p = 0; enough && p < set->profiles.count && !Cancelled(cancel); p++)
        enough = LoadProfileEntries(set, p, &transcripts, cancel) != ENTRIES_NO_MEMORY;
    if (enough) AddOtherTranscriptBytes(set, &transcripts, cancel);
    if (transcripts.items) HeapFree(GetProcessHeap(), 0, transcripts.items);
    if (!enough) Util_Log(L"sessions could not be read whole: not enough memory");
    if (!enough || !GroupRows(set, cancel)) {
        SessionStore_Free(set);
        return FALSE;
    }
    /* Sorting moved the rows: index them again before resolving pending and live ids. */
    FreeIndex(set);
    for (r = 0; r < set->rowCount && !Cancelled(cancel); r++) IndexRow(set, r);
    LoadAllPending(set, cancel);
    if (!Cancelled(cancel)) LoadLive(set, cancel);
    if (Cancelled(cancel)) {
        SessionStore_Free(set);
        return FALSE;
    }
    return TRUE;
}

/* One profile's entries only (no transcripts, groups or live sessions), for
 * the changes made to them. TRUE when every entry was read: a session it
 * does not list then is not there. */
BOOL SessionStore_LoadEntries(SessionSet *set, const Profile *profile)
{
    Transcripts none;
    ZeroMemory(set, sizeof *set);
    ZeroMemory(&none, sizeof none);
    set->profiles.count = 1;
    set->profiles.items[0] = *profile;
    return LoadProfileEntries(set, 0, &none, NULL) == ENTRIES_COMPLETE;
}

void SessionStore_Free(SessionSet *set)
{
    if (set->entries) HeapFree(GetProcessHeap(), 0, set->entries);
    if (set->rows) HeapFree(GetProcessHeap(), 0, set->rows);
    if (set->groups) HeapFree(GetProcessHeap(), 0, set->groups);
    if (set->otherTranscriptIds) HeapFree(GetProcessHeap(), 0, set->otherTranscriptIds);
    FreeIndex(set);
    ZeroMemory(set, sizeof *set);
}

/* The title a row goes by: a title someone gave it, the latest first, else
 * any title Claude gave it; "" when it has none. */
const WCHAR *SessionStore_RowTitle(const SessionSet *set, const SessionRow *row)
{
    const SessionEntry *best = NULL;
    int p;
    for (p = 0; p < set->profiles.count; p++) {
        const SessionEntry *entry = row->entry[p] >= 0 ? &set->entries[row->entry[p]] : NULL;
        if (!entry || !entry->title[0]) continue;
        if (!best || (entry->userTitle && !best->userTitle) ||
            (entry->userTitle == best->userTitle && entry->lastActivity > best->lastActivity))
            best = entry;
    }
    return best ? best->title : L"";
}
