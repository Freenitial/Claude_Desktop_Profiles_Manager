/*
 * What the sessions view changes in Claude Code sessions (sessionstore.c
 * reads them).
 *
 * A running Claude keeps its session list in memory and writes it back: an
 * entry changed under it is overwritten, and one added under it is not seen
 * until it starts again. So:
 * - opening, sharing and copying go through Claude itself, with its own link
 *   claude://resume?session=<id>: the profile's Claude opens the session and
 *   adds it to its list when it is not there (started first when closed);
 * - a new title, a star or a removal is made at once in a closed
 *   profile, else kept in a file of ours (pending-sessions-<folder>.txt) and
 *   made when the profile has closed: after its watcher sees Claude exit
 *   (main.c), before it opens through us (Launcher_Open), or when the
 *   manager finds it closed.
 * A shared session is one conversation (one transcript) that each profile
 * lists; a copy is a new conversation, the transcript copied under a new id,
 * that goes on separately. A session listed twice in one profile is changed
 * in both entries.
 */
#include "app.h"
#include <objbase.h>
#include <shellapi.h>
#include <wchar.h>
#include <limits.h>

#define COPY_CHUNK            (1024u * 1024u)
#define WRITE_CHUNK_MAX       0x40000000u   /* bytes given to one WriteFile */
#define TEMPORARY_SUFFIX      L".cdm-new"   /* a file written next to the one it replaces */
#define EDIT_LOCK_WAIT_MS     5000
#define LISTED_FILES_FIRST    64            /* paths a deletion list has room for at first */
#define TOMBSTONE_PREFIX      L"deleted_"   /* Claude's mark of a session deleted from a profile */
#define STAGING_DIR           L"imported-staging"   /* beside the entries: a transcript Claude takes in, before it is filed */
#define CLAUDE_CODE_TEMP_DIR  L"claude"     /* in the temporary folder: Claude Code's, a folder per project, one per session in it */
#define UNIX_EPOCH_TICKS      116444736000000000ULL   /* 1970-01-01 as a FILETIME */
#define TICKS_PER_MILLISECOND (TICKS_PER_SECOND / 1000)
#define JSON_MEMBER_SYNTAX    4             /* an added member besides its key and value: the comma, two quotes, the colon */
#define GUID_TEXT_CCH         39            /* {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} */
#define SCRATCH_NAME_CCH      32            /* scratch-yyyy-mm-dd-xxxxxx */
#define RESUME_LINK_CCH       (ARRAYSIZE(L"claude://resume?session=") + SESSION_ID_CCH)
/* A path shorter than MAX_PATH as a JSON string in UTF-8: at most 3 bytes a
 * character (an escaped backslash takes 2), the quotes and the end. */
#define QUOTED_PATH_BYTES     (MAX_PATH * 3)
#define ID_MEMBER_BYTES       (sizeof "\"sessionId\":\"\"" + SESSION_ID_CCH)
#define CWD_MEMBER_BYTES      (sizeof "\"cwd\":\\\\" + QUOTED_PATH_BYTES)

/* -------------------------------------------------------------- open */

HRESULT SessionEdit_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *sessionId)
{
    WCHAR link[RESUME_LINK_CCH];
    HRESULT hr;
    if (!Core_ResumeLink(sessionId, link, ARRAYSIZE(link))) return E_INVALIDARG;
    hr = Launcher_Open(pkg, p, link, NULL, NULL);
    Util_Log(L"session %s -> %s%s", sessionId, p->folder, SUCCEEDED(hr) ? L"" : L" FAILED");
    return hr;
}

/* ----------------------------------------------------------- files */

static BOOL WriteWhole(HANDLE file, const char *data, size_t length)
{
    while (length > 0) {
        DWORD written = 0, chunk = length > WRITE_CHUNK_MAX ? WRITE_CHUNK_MAX : (DWORD)length;
        if (!WriteFile(file, data, chunk, &written, NULL) || written == 0) return FALSE;
        data += written;
        length -= written;
    }
    return TRUE;
}

/* `path` replaced by `data` in one step: written next to it, flushed, then
 * put in its place. An entry file (`profile` given) is replaced only while
 * its profile is closed, and only when it is there (ReplaceFileW fails on a
 * missing file): a new entry is never written (rule 13). */
static BOOL SaveFile(const WCHAR *path, const char *data, size_t length, const Profile *profile)
{
    WCHAR target[LONG_PATH_CCH], temporary[LONG_PATH_CCH];
    HANDLE file;
    DWORD error = ERROR_SUCCESS;
    BOOL ok;
    if (profile && Claude_IsRunning(profile)) return FALSE;
    if (!Util_ExtendedPath(path, target, ARRAYSIZE(target)) ||
        FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" TEMPORARY_SUFFIX, target))) {
        Util_Log(L"could not write %s: its path is too long", path);
        return FALSE;
    }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        Util_Log(L"could not write %s (error %lu)", path, GetLastError());
        return FALSE;
    }
    ok = WriteWhole(file, data, length) && FlushFileBuffers(file);
    if (!ok) error = GetLastError();
    CloseHandle(file);
    if (ok && profile && Claude_IsRunning(profile)) {
        DeleteFileW(temporary);
        Util_Log(L"%s left as it was: its profile started", path);
        return FALSE;
    }
    if (ok) {
        ok = profile ? ReplaceFileW(target, temporary, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)
                     : MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) error = GetLastError();
        /* ReplaceFileW took the entry away but could not put the new one in its place. */
        if (!ok && error == ERROR_UNABLE_TO_MOVE_REPLACEMENT && MoveFileExW(temporary, target, MOVEFILE_WRITE_THROUGH)) ok = TRUE;
    }
    if (!ok) {
        DeleteFileW(temporary);
        Util_Log(L"could not write %s (error %lu)", path, error);
    }
    return ok;
}

/* Sets members of an entry file (`raws` are JSON values). */
static BOOL SetMembers(const Profile *profile, const WCHAR *file, const char *const *keys, const char *const *raws, int count)
{
    DWORD length = 0;
    char *json = Util_ReadFile(file, SESSION_ENTRY_MAX_BYTES, FALSE, &length), *current = NULL, *next = NULL;
    size_t capacity = length, used = length;
    BOOL ok = json != NULL;
    int i;
    if (!ok) Util_Log(L"could not read %s whole", file);   /* missing, empty, too big or still written */
    for (i = 0; i < count; i++) capacity += strlen(keys[i]) + strlen(raws[i]) + JSON_MEMBER_SYNTAX;
    if (ok) {
        current = (char *)HeapAlloc(GetProcessHeap(), 0, capacity);
        next = (char *)HeapAlloc(GetProcessHeap(), 0, capacity);
        ok = current && next;
    }
    if (ok) memcpy(current, json, length);
    for (i = 0; ok && i < count; i++) {
        char *swap;
        ok = Core_JsonSetMember(current, used, keys[i], raws[i], next, capacity, &used);
        swap = current;
        current = next;
        next = swap;
    }
    if (ok) ok = SaveFile(file, current, used, profile);
    if (json) HeapFree(GetProcessHeap(), 0, json);
    if (current) HeapFree(GetProcessHeap(), 0, current);
    if (next) HeapFree(GetProcessHeap(), 0, next);
    return ok;
}

/* A title or a star set in one entry file of closed profile `profile`. */
static BOOL SetInEntry(const Profile *profile, const WCHAR *file, const PendingEdit *edit)
{
    char title[SESSION_TITLE_CCH * 6 + 3];   /* every character escaped as \u00XX, and the quotes */
    const char *keys[2], *raws[2];
    if (edit->op == PENDING_TITLE) {
        if (!edit->value[0] || !Core_JsonQuote(edit->value, title, sizeof title)) return FALSE;
        keys[0] = "title";
        raws[0] = title;
        keys[1] = "titleSource";   /* a title someone chose: Claude does not replace it */
        raws[1] = "\"user\"";
        return SetMembers(profile, file, keys, raws, 2);
    }
    keys[0] = "isStarred";
    raws[0] = edit->value[0] == L'1' ? "true" : "false";
    return SetMembers(profile, file, keys, raws, 1);
}

/* A title or a star set in every entry of the session at row `row` of
 * `entries` (one closed profile's, SessionStore_LoadEntries). */
static BOOL SetInEntries(const Profile *profile, const SessionSet *entries, int row, const PendingEdit *edit)
{
    int entry;
    for (entry = entries->rows[row].entry[0]; entry >= 0; entry = entries->entries[entry].duplicate)
        if (!SetInEntry(profile, entries->entries[entry].file, edit)) return FALSE;
    return TRUE;
}

static BOOL PathMissing(const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH];
    return Util_ExtendedPath(path, extended, ARRAYSIZE(extended)) && Util_QueryPath(extended, NULL) == PATH_MISSING;
}

static void WriteTombstone(const WCHAR *dir, const WCHAR *id, const char *now)
{
    WCHAR path[LONG_PATH_CCH];
    if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" TOMBSTONE_PREFIX L"%s", dir, id))) SaveFile(path, now, strlen(now), NULL);
}

/* Claude's marks that a session was deleted from profile `profile`, as its
 * own delete writes them: deleted_<id> beside the entries, holding the time
 * in ms, once for each id of the session (its entries' own ids, its
 * transcripts) that no other session there claims. Claude then leaves that
 * conversation out of the ones of Claude Code it offers to take in. */
static void MarkDeleted(const SessionSet *set, int row, int profile)
{
    const SessionRow *session = &set->rows[row];
    const WCHAR **transcripts, **ids;
    WCHAR dir[LONG_PATH_CCH], *slash;
    char now[sizeof "18446744073709551615"];
    FILETIME time;
    int entry, transcriptCount, entryCount = 0, count = 0, i, seen;
    if (session->entry[profile] < 0 || FAILED(StringCchCopyW(dir, ARRAYSIZE(dir), set->entries[session->entry[profile]].file)) ||
        (slash = wcsrchr(dir, L'\\')) == NULL || (transcripts = SessionStore_TranscriptIds(set, row, &transcriptCount)) == NULL)
        return;
    *slash = 0;
    for (entry = session->entry[profile]; entry >= 0; entry = set->entries[entry].duplicate) entryCount++;
    if ((ids = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)(entryCount + transcriptCount) * sizeof *ids)) == NULL) {
        HeapFree(GetProcessHeap(), 0, (void *)transcripts);
        return;
    }
    for (entry = session->entry[profile]; entry >= 0; entry = set->entries[entry].duplicate) ids[count++] = SessionStore_OwnId(&set->entries[entry]);
    for (i = 0; i < transcriptCount; i++) ids[count++] = transcripts[i];
    GetSystemTimeAsFileTime(&time);
    StringCchPrintfA(now, sizeof now, "%I64u",
                     ((((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime) - UNIX_EPOCH_TICKS) / TICKS_PER_MILLISECOND);
    for (i = 0; i < count; i++) {
        for (seen = 0; seen < i && !Core_EqualsI(ids[seen], ids[i]); seen++) {}
        if (seen == i && Core_IsUuid(ids[i]) && !SessionStore_ClaimedByOther(set, row, profile, ids[i])) WriteTombstone(dir, ids[i], now);
    }
    HeapFree(GetProcessHeap(), 0, (void *)ids);
    HeapFree(GetProcessHeap(), 0, (void *)transcripts);
}

/* Whether Windows' Recycle Bin takes `path` (Util_FitsRecycleBin). When it
 * does not, `error` names it. */
static BOOL FitsRecycleBin(const WCHAR *path, WCHAR *error, size_t errorCch)
{
    if (Util_FitsRecycleBin(path)) return TRUE;
    if (errorCch) StringCchPrintfW(error, errorCch, TR(L"This file's path is too long for the Recycle Bin: %s"), path);
    Util_Log(L"%s not moved to the Recycle Bin: its path is too long", path);
    return FALSE;
}

/* The entries of the session at row `row` of `entries` (one closed
 * profile's) to the Recycle Bin. Never while the lock is held: Windows may
 * ask, in `owner`, before deleting what the Recycle Bin cannot hold. */
static RemoveResult RemoveSessionEntries(HWND owner, const SessionSet *entries, int row, WCHAR *error, size_t errorCch)
{
    const WCHAR **files;
    int entry, count = 0;
    RemoveResult result = REMOVE_FAILED;
    for (entry = entries->rows[row].entry[0]; entry >= 0; entry = entries->entries[entry].duplicate) count++;
    files = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)max(count, 1) * sizeof *files);
    if (!files) return REMOVE_FAILED;
    count = 0;
    for (entry = entries->rows[row].entry[0];
         entry >= 0 && FitsRecycleBin(entries->entries[entry].file, error, errorCch); entry = entries->entries[entry].duplicate)
        files[count++] = entries->entries[entry].file;
    if (entry < 0) result = Util_Recycle(owner, files, count);
    HeapFree(GetProcessHeap(), 0, (void *)files);
    return result;
}

/* ---------------------------------------------------------- pending */

/* Managers and exit watchers serialize a profile's entry changes and queue.
 * An abandoned owner leaves complete files: replacement is atomic. */
static HANDLE LockEdits(const Profile *p)
{
    WCHAR path[MAX_PATH], name[ARRAYSIZE(SESSION_MUTEX_PREFIX) + 2 * sizeof(ULONGLONG)];   /* and the hash's hex digits */
    ULONGLONG hash;
    HANDLE mutex;
    DWORD wait;
    if (!SessionStore_PendingPath(p, path, ARRAYSIZE(path))) return NULL;
    hash = Core_HashBytes(CORE_HASH_START, path, wcslen(path) * sizeof(WCHAR));
    if (FAILED(StringCchPrintfW(name, ARRAYSIZE(name), SESSION_MUTEX_PREFIX L"%08lx%08lx", (DWORD)(hash >> 32), (DWORD)hash)))
        return NULL;
    mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex) {
        Util_Log(L"session changes of %s: no lock (error %lu)", p->folder, GetLastError());
        return NULL;
    }
    wait = WaitForSingleObject(mutex, EDIT_LOCK_WAIT_MS);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return mutex;
    Util_Log(L"session changes of %s: another process kept them locked", p->folder);
    CloseHandle(mutex);
    return NULL;
}

static void UnlockEdits(HANDLE mutex)
{
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

/* The file of changes for `p`, rewritten: `add` (when not NULL) replaces the
 * queued changes it supersedes (Core_PendingReplaces); each of `drops`
 * cancels the change of its kind to its session, or with `everyKind` every
 * change to its session. With neither, the file is written again as it is,
 * which tells whether it can be; drops that cancel nothing leave it as it
 * is. Fails, writing nothing, when the file cannot be read or a change
 * cannot be written back. */
static BOOL RewritePending(const Profile *p, const PendingEdit *add, const PendingEdit *drops, int dropCount, BOOL everyKind)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH], line[SESSION_ID_CCH + SESSION_TITLE_CCH + ARRAYSIZE(L"remove\t\t\n")], *slash;
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    char *out = NULL;
    size_t used = 0, capacity;
    int n, i, drop, kept = 0;
    BOOL ok = FALSE;
    if (!edits || !SessionStore_PendingPath(p, path, ARRAYSIZE(path))) goto done;
    if ((n = SessionStore_LoadPending(p, edits, SESSION_PENDING_MAX)) < 0) {
        Util_Log(L"session changes waiting for %s: their file cannot be read", p->folder);
        goto done;
    }
    for (i = 0; i < n; i++) {
        const PendingEdit *queued = &edits[i];
        BOOL superseded = add && Core_PendingReplaces(queued, add);
        for (drop = 0; drop < dropCount && !superseded; drop++)
            superseded = Core_EqualsI(queued->key, drops[drop].key) && (everyKind || queued->op == drops[drop].op);
        if (!superseded) edits[kept++] = *queued;
    }
    if (!add && dropCount > 0 && kept == n) {
        ok = TRUE;   /* nothing to cancel: the file, and its time, stay */
        goto done;
    }
    if (add) {
        if (kept >= SESSION_PENDING_MAX) {
            Util_Log(L"session changes waiting for %s: %d already, no more is taken", p->folder, kept);
            goto done;
        }
        edits[kept++] = *add;
    }
    if (kept == 0) {
        ok = DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
        if (!ok) Util_Log(L"could not delete %s (error %lu)", path, GetLastError());
        goto done;
    }
    capacity = (size_t)kept * ARRAYSIZE(line) * 3;   /* at most 3 UTF-8 bytes per character */
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, capacity)) == NULL) goto done;
    for (i = 0; i < kept; i++) {
        int bytes;
        if (!Core_PendingFormat(&edits[i], line, ARRAYSIZE(line)) || FAILED(StringCchCatW(line, ARRAYSIZE(line), L"\n")) ||
            (bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, out + used, (int)(capacity - used), NULL, NULL)) <= 0)
            goto done;
        used += (size_t)bytes - 1;
    }
    if (FAILED(StringCchCopyW(dir, ARRAYSIZE(dir), path)) || (slash = wcsrchr(dir, L'\\')) == NULL) goto done;
    *slash = 0;
    ok = Util_EnsureDir(dir) && SaveFile(path, out, used, NULL);
done:
    if (out) HeapFree(GetProcessHeap(), 0, out);
    if (edits) HeapFree(GetProcessHeap(), 0, edits);
    return ok;
}

/* RewritePending under the lock. */
static BOOL RewritePendingLocked(const Profile *p, const PendingEdit *add, const PendingEdit *drops, int dropCount, BOOL everyKind)
{
    HANDLE mutex = LockEdits(p);
    BOOL ok;
    if (!mutex) return FALSE;
    ok = RewritePending(p, add, drops, dropCount, everyKind);
    UnlockEdits(mutex);
    return ok;
}

/* The changes `edits` cancel the waiting change of their kind to their session. */
BOOL SessionEdit_Cancel(const Profile *p, const PendingEdit *edits, int count)
{
    return RewritePendingLocked(p, NULL, edits, count, FALSE);
}

/* Every change waiting for `p` to a session that is gone, under any of `keys`. */
void SessionEdit_Forget(const Profile *p, const WCHAR *const *keys, int count)
{
    PendingEdit *forgotten = (PendingEdit *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)max(count, 1) * sizeof *forgotten);
    int i;
    if (!forgotten) return;
    for (i = 0; i < count; i++) StringCchCopyW(forgotten[i].key, ARRAYSIZE(forgotten[i].key), keys[i]);
    RewritePendingLocked(p, NULL, forgotten, count, TRUE);
    HeapFree(GetProcessHeap(), 0, forgotten);
}

/* The keys a change of kind `op` to session `row` of `set` can wait under in
 * profile `profile`: the session's id, and the own id of each of its
 * entries there (a change made before the session's first message keeps
 * it). A heap array (HeapFree it); NULL without memory. */
static PendingEdit *SessionKeys(const SessionSet *set, int row, int profile, PendingOp op, int *count)
{
    PendingEdit *keys;
    int entry, capacity = 1;
    *count = 0;
    for (entry = set->rows[row].entry[profile]; entry >= 0; entry = set->entries[entry].duplicate) capacity++;
    if ((keys = (PendingEdit *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof *keys)) == NULL) return NULL;
    StringCchCopyW(keys[(*count)++].key, ARRAYSIZE(keys->key), set->rows[row].key);
    for (entry = set->rows[row].entry[profile]; entry >= 0; entry = set->entries[entry].duplicate)
        if (!Core_EqualsI(set->entries[entry].localId, set->rows[row].key))
            StringCchCopyW(keys[(*count)++].key, ARRAYSIZE(keys->key), set->entries[entry].localId);
    for (entry = 0; entry < *count; entry++) keys[entry].op = op;
    return keys;
}

static const WCHAR *ChangeName(PendingOp operation)
{
    return operation == PENDING_TITLE ? L"title changed" : operation == PENDING_STAR ? L"star changed" : L"entry removed";
}

/* Changes one profile's entries for a session: at once when the profile is
 * closed, else when it closes (*waiting then). FALSE when it could not be
 * made, `error` (when not empty) saying why; a change impossible while the
 * profile is closed waits no more. A removal declined at Windows' question
 * is no failure. */
BOOL SessionEdit_Change(HWND owner, const Profile *p, const SessionEntry *entry, const PendingEdit *edit, BOOL *waiting,
                        WCHAR *error, size_t errorCch)
{
    SessionSet entries;
    PendingEdit *keys = NULL;
    RemoveResult removed = REMOVE_FAILED;
    HANDLE mutex;
    int row = -1, keyCount = 0;
    BOOL made = FALSE;
    *waiting = FALSE;
    if (errorCch) error[0] = 0;
    if ((mutex = LockEdits(p)) == NULL) return FALSE;
    /* Queued before the entry changes: if taking it off the queue fails, a
     * replay makes this same change, never an older one. */
    if (!RewritePending(p, edit, NULL, 0, FALSE)) {
        UnlockEdits(mutex);
        return FALSE;
    }
    if (!entry || !entry->file[0] || Claude_IsRunning(p)) {
        UnlockEdits(mutex);
        *waiting = TRUE;
        return TRUE;
    }
    /* Read again: every entry of the session as it is on disk now. */
    SessionStore_LoadEntries(&entries, p);
    SessionStore_FindEntry(&entries, 0, edit->key, &row);
    if (row >= 0) keys = SessionKeys(&entries, row, 0, edit->op, &keyCount);
    if (edit->op == PENDING_REMOVE) {
        UnlockEdits(mutex);   /* Windows may ask before deleting what the Recycle Bin cannot hold */
        if (row >= 0) removed = Claude_IsRunning(p) ? REMOVE_FAILED : RemoveSessionEntries(owner, &entries, row, error, errorCch);
        else if (PathMissing(entry->file)) removed = REMOVE_DONE;   /* gone already */
        /* A Claude started while Windows asked keeps the session: it goes once that Claude closes. */
        made = removed == REMOVE_DONE && !Claude_IsRunning(p);
        if (made && row >= 0) MarkDeleted(&entries, row, 0);
        mutex = LockEdits(p);
    } else if (row >= 0) {
        made = SetInEntries(p, &entries, row, edit);
    }
    Util_Log(L"session %s in %s: %s%s", edit->key, p->folder, ChangeName(edit->op),
             made ? L"" : removed == REMOVE_CANCELLED ? L" CANCELLED" : row < 0 ? L" FAILED (not listed)" : L" FAILED");
    if (made || !Claude_IsRunning(p)) {
        /* Made, or impossible while the profile is closed: it waits no more, under any key of its session. */
        if (!mutex || !RewritePending(p, NULL, keys ? keys : edit, keys ? keyCount : 1, edit->op == PENDING_REMOVE))
            Util_Log(L"session %s in %s: the change could not be taken off the queue", edit->key, p->folder);
    } else {
        *waiting = TRUE;   /* its Claude started meanwhile: made when it closes */
    }
    if (mutex) UnlockEdits(mutex);
    if (keys) HeapFree(GetProcessHeap(), 0, keys);
    SessionStore_Free(&entries);
    return made || *waiting || removed == REMOVE_CANCELLED;
}

/* Makes the changes waiting for `p` while its Claude is closed. The entries
 * are read again: the one to change may have been made since (a session
 * shared or copied there). A change made, or impossible while the profile is
 * closed (logged), leaves the queue, as in SessionEdit_Change. One to a
 * session the profile does not list waits on, unless `afterRun`: Claude has
 * just closed, having written every session it had, so it never will be.
 * Removals come last, made without the lock (RemoveSessionEntries); one a
 * Claude started meanwhile overtakes waits again. Returns how many changes
 * left the queue. */
static int ApplyWaitingChanges(HWND owner, const Profile *p, BOOL afterRun)
{
    PendingEdit *queued, *finished, *removals;
    SessionSet entries;
    HANDLE mutex;
    int n = 0, i, row, finishedCount = 0, removalCount = 0, queuedAgain = 0;
    BOOL complete, queueWritable = FALSE;
    if ((mutex = LockEdits(p)) == NULL) return 0;
    queued = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, 3 * SESSION_PENDING_MAX * sizeof *queued);
    if (queued && !Claude_IsRunning(p)) n = SessionStore_LoadPending(p, queued, SESSION_PENDING_MAX);
    if (n <= 0) {
        if (n < 0) Util_Log(L"session changes waiting for %s: their file cannot be read", p->folder);
        if (queued) HeapFree(GetProcessHeap(), 0, queued);
        UnlockEdits(mutex);
        return 0;
    }
    finished = queued + SESSION_PENDING_MAX;
    removals = finished + SESSION_PENDING_MAX;
    complete = SessionStore_LoadEntries(&entries, p);
    for (i = 0; i < n && !Claude_IsRunning(p); i++) {
        const PendingEdit *edit = &queued[i];
        BOOL made;
        if (SessionStore_FindEntry(&entries, 0, edit->key, &row) < 0) {
            if (afterRun && complete) {
                Util_Log(L"session %s in %s: %s dropped, the profile does not list the session", edit->key, p->folder,
                         ChangeName(edit->op));
                finished[finishedCount++] = *edit;
            }
            continue;
        }
        /* A change made but left queued would be made again on every load:
         * the queue must be writable before any entry changes. */
        if (!queueWritable && !(queueWritable = RewritePending(p, NULL, NULL, 0, FALSE))) break;
        if (edit->op == PENDING_REMOVE) {
            removals[removalCount++] = *edit;
            continue;
        }
        made = SetInEntries(p, &entries, row, edit);
        Util_Log(L"session %s in %s: %s%s", edit->key, p->folder, ChangeName(edit->op), made ? L"" : L" FAILED");
        if (!made && Claude_IsRunning(p)) break;   /* its Claude started: the rest waits */
        finished[finishedCount++] = *edit;
    }
    /* Removals leave the queue before they are made: whatever Windows asks
     * about one, it is not asked again. */
    for (i = 0; i < removalCount; i++) finished[finishedCount++] = removals[i];
    if (finishedCount && !RewritePending(p, NULL, finished, finishedCount, FALSE)) {
        finishedCount = 0;
        removalCount = 0;   /* still queued: made once they can leave the queue */
    }
    UnlockEdits(mutex);
    for (i = 0; i < removalCount; i++) {
        RemoveResult result = REMOVE_FAILED;
        if (SessionStore_FindEntry(&entries, 0, removals[i].key, &row) < 0) continue;
        if (!Claude_IsRunning(p)) result = RemoveSessionEntries(owner, &entries, row, NULL, 0);
        if (Claude_IsRunning(p) && result != REMOVE_CANCELLED) {
            /* Its Claude started meanwhile and keeps the session: removed when it closes. */
            if (RewritePendingLocked(p, &removals[i], NULL, 0, FALSE)) queuedAgain++;
            Util_Log(L"session %s in %s: removal waits again, its Claude started", removals[i].key, p->folder);
            continue;
        }
        if (result == REMOVE_DONE) MarkDeleted(&entries, row, 0);
        Util_Log(L"session %s in %s: %s%s", removals[i].key, p->folder, ChangeName(PENDING_REMOVE),
                 result == REMOVE_DONE ? L"" : result == REMOVE_CANCELLED ? L" CANCELLED" : L" FAILED");
    }
    SessionStore_Free(&entries);
    HeapFree(GetProcessHeap(), 0, queued);
    if (finishedCount > queuedAgain)
        Util_Log(L"%d waiting session change(s) taken off the queue of %s", finishedCount - queuedAgain, p->folder);
    return finishedCount - queuedAgain;
}

int SessionEdit_ApplyPending(HWND owner, const Profile *p)
{
    return ApplyWaitingChanges(owner, p, FALSE);
}

int SessionEdit_ApplyPendingAfterRun(const Profile *p)
{
    return ApplyWaitingChanges(NULL, p, TRUE);
}

/* The watcher's call each time the profile's Claude has closed. */
void SessionEdit_ApplyPendingFor(const WCHAR *folder)
{
    ProfileList list;
    int i;
    Profiles_Load(&list, NULL);
    if ((i = Profiles_Find(&list, folder)) >= 0) SessionEdit_ApplyPendingAfterRun(&list.items[i]);
}

/* ------------------------------------------------------------ copy */

/* What the latest SessionEdit_CopyConversation made, for
 * SessionEdit_RemoveCopy: only files this program has just made are ever
 * deleted for good. Both run on the manager's window thread. */
static struct {
    WCHAR copyId[SESSION_ID_CCH], sourceKey[SESSION_ID_CCH], targetFolder[FOLDER_CCH];
    WCHAR transcript[LONG_PATH_CCH], projectFolder[LONG_PATH_CCH], workingFolder[MAX_PATH];
} g_lastCopy;

static BOOL NewSessionId(WCHAR *out, size_t cch, DWORD *random)
{
    GUID guid;
    WCHAR text[GUID_TEXT_CCH];
    size_t i;
    if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(&guid, text, ARRAYSIZE(text)) != ARRAYSIZE(text)) return FALSE;
    text[ARRAYSIZE(text) - 2] = 0;   /* the closing brace */
    for (i = 1; text[i]; i++) text[i] = (WCHAR)towlower(text[i]);
    *random = guid.Data1;
    return SUCCEEDED(StringCchCopyW(out, cch, text + 1));
}

/* The transcript `from` copied to the new file `to` with `swaps` made,
 * through a file next to it moved in place at the end. A transcript in use
 * can end with a line still being written: the copy ends after the last
 * whole line. */
static BOOL CopyTranscript(const WCHAR *from, const WCHAR *to, const CoreSwap *swaps, int count)
{
    WCHAR source[LONG_PATH_CCH], target[LONG_PATH_CCH], temporary[LONG_PATH_CCH];
    HANDLE in = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
    char *buffer = NULL, *replaced = NULL;
    size_t held = 0, shortest = (size_t)-1, longest = 0, input, extra = 0;
    ULONGLONG total = 0, wholeLines = 0;
    LARGE_INTEGER end;
    DWORD error = ERROR_INVALID_PARAMETER;
    BOOL ok = FALSE, last = FALSE, madeTemporary = FALSE;
    int i;
    for (i = 0; i < count; i++) {
        shortest = min(shortest, strlen(swaps[i].from));
        longest = max(longest, strlen(swaps[i].from));
    }
    if (count < 1 || shortest == 0 || !Util_ExtendedPath(from, source, ARRAYSIZE(source)) ||
        !Util_ExtendedPath(to, target, ARRAYSIZE(target)) ||
        FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" TEMPORARY_SUFFIX, target)))
        goto done;
    /* A chunk, with what may start a swap kept from the one before. A swap
     * takes at least its `from` and adds at most its growth: the output
     * outgrows the input by at most the steepest growth's share of it. */
    input = COPY_CHUNK + longest;
    for (i = 0; i < count; i++) {
        size_t fromLength = strlen(swaps[i].from), toLength = strlen(swaps[i].to);
        if (toLength > fromLength) extra = max(extra, (input / fromLength + 1) * (toLength - fromLength));
    }
    in = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                     FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        goto done;
    }
    out = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        goto done;
    }
    madeTemporary = TRUE;
    buffer = (char *)HeapAlloc(GetProcessHeap(), 0, input);
    replaced = (char *)HeapAlloc(GetProcessHeap(), 0, input + extra);
    if (!buffer || !replaced) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    for (ok = TRUE; ok && !last;) {
        DWORD got = 0;
        size_t used = 0, written, lineEnd;
        if (!ReadFile(in, buffer + held, COPY_CHUNK, &got, NULL)) {
            ok = FALSE;
            break;
        }
        last = got == 0;
        written = Core_ReplaceChunk(buffer, held + got, swaps, count, last, replaced, &used);
        ok = WriteWhole(out, replaced, written);
        for (lineEnd = written; lineEnd > 0 && replaced[lineEnd - 1] != '\n'; lineEnd--) {}
        if (lineEnd > 0) wholeLines = total + lineEnd;
        total += written;
        held = held + got - used;
        memmove(buffer, buffer + used, held);   /* what may start a swap goes with the next chunk */
    }
    if (ok && wholeLines < total) {
        end.QuadPart = (LONGLONG)wholeLines;
        ok = SetFilePointerEx(out, end, NULL, FILE_BEGIN) && SetEndOfFile(out);
    }
    if (ok) ok = FlushFileBuffers(out);
    if (!ok) error = GetLastError();
    CloseHandle(out);
    out = INVALID_HANDLE_VALUE;
    if (ok && !MoveFileExW(temporary, target, MOVEFILE_WRITE_THROUGH)) {
        ok = FALSE;
        error = GetLastError();
    }
done:
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    if (replaced) HeapFree(GetProcessHeap(), 0, replaced);
    if (!ok) {
        if (madeTemporary) DeleteFileW(temporary);
        Util_Log(L"transcript %s not copied to %s (error %lu)", from, to, error);
    }
    return ok;
}

/* A folder this program has just made by copying, deleted for good: it only
 * holds copies. */
static BOOL RemoveCopiedFolder(const WCHAR *path)
{
    WCHAR list[MAX_PATH + 1];   /* double zero-terminated */
    SHFILEOPSTRUCTW operation;
    int code;
    ZeroMemory(list, sizeof list);
    if (FAILED(StringCchCopyW(list, ARRAYSIZE(list) - 1, path))) return FALSE;
    ZeroMemory(&operation, sizeof operation);
    operation.wFunc = FO_DELETE;
    operation.pFrom = list;
    operation.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    code = SHFileOperationW(&operation);
    if (code == 0 && !operation.fAnyOperationsAborted) return TRUE;
    Util_Log(L"copied folder %s not removed (code %d)", path, code);
    return FALSE;
}

/* A folder removed when it is empty, at any path length. */
static void RemoveEmptyFolder(const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH];
    if (Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) RemoveDirectoryW(extended);
}

/* A folder's content copied into a new folder (created, even when there is
 * nothing to copy; removed again when the copy fails or is cancelled). With
 * an owner window, Windows shows the progress of a long copy, which can be
 * cancelled. */
static CopyResult CopyFolder(HWND owner, const WCHAR *from, const WCHAR *to)
{
    WCHAR source[MAX_PATH + 3], target[MAX_PATH + 1];   /* double zero-terminated lists */
    SHFILEOPSTRUCTW operation;
    WIN32_FIND_DATAW found;
    HANDLE find;
    BOOL contents = FALSE;
    int code;
    if (!Util_DirExists(from)) {
        Util_Log(L"working folder %s not found (error %lu)", from, GetLastError());
        return COPY_FAILED;
    }
    ZeroMemory(source, sizeof source);
    ZeroMemory(target, sizeof target);
    if (FAILED(StringCchPrintfW(source, ARRAYSIZE(source) - 1, L"%s\\*", from)) ||
        FAILED(StringCchCopyW(target, ARRAYSIZE(target) - 1, to)))
        return COPY_FAILED;
    find = FindFirstFileW(source, &found);
    if (find == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) {
        Util_Log(L"working folder %s not read (error %lu)", from, GetLastError());
        return COPY_FAILED;
    }
    if (find != INVALID_HANDLE_VALUE) {
        DWORD error;
        do {
            if (wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) { contents = TRUE; break; }
        } while (FindNextFileW(find, &found));
        error = GetLastError();
        FindClose(find);
        if (!contents && error != ERROR_NO_MORE_FILES) {
            Util_Log(L"working folder %s not read (error %lu)", from, error);
            return COPY_FAILED;
        }
    }
    if (!CreateDirectoryW(to, NULL)) {
        Util_Log(L"could not create %s (error %lu)", to, GetLastError());
        return COPY_FAILED;
    }
    if (!contents) return COPY_MADE;
    ZeroMemory(&operation, sizeof operation);
    operation.hwnd = owner;
    operation.wFunc = FO_COPY;
    operation.pFrom = source;
    operation.pTo = target;
    operation.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | (owner ? 0 : FOF_SILENT);
    code = SHFileOperationW(&operation);
    if (code == 0 && !operation.fAnyOperationsAborted) return COPY_MADE;
    Util_Log(L"working folder %s not copied to %s (code %d%s)", from, to, code, operation.fAnyOperationsAborted ? L", cancelled" : L"");
    RemoveCopiedFolder(to);
    return operation.fAnyOperationsAborted || code == ERROR_CANCELLED ? COPY_CANCELLED : COPY_FAILED;
}

/* A new conversation for profile `target`, copied from row `row`: a new id,
 * the transcript copied under it. A session without a folder gets its own
 * working folder in the target's "no folder" area (its files copied), so it
 * is there that it shows. `newId` gets the copy's id; a copy cancelled in
 * Windows' progress leaves nothing behind and no error. */
CopyResult SessionEdit_CopyConversation(HWND owner, const SessionSet *set, int row, int target, WCHAR *newId, size_t idCch,
                                        WCHAR *error, size_t errorCch)
{
    const SessionRow *source = &set->rows[row];
    const SessionSource *destination = &set->source[target];
    const Profile *targetProfile = &set->profiles.items[target];
    WCHAR cwd[MAX_PATH], scratchName[SCRATCH_NAME_CCH], sourceFiles[MAX_PATH], scratchFiles[MAX_PATH], copiedFiles[MAX_PATH];
    WCHAR projects[LONG_PATH_CCH], projectName[CORE_PROJECT_NAME_MAX + 1], projectDir[LONG_PATH_CCH];
    WCHAR transcriptDir[LONG_PATH_CCH], copy[LONG_PATH_CCH], extended[LONG_PATH_CCH], *slash;
    char fromId[ID_MEMBER_BYTES], toId[ID_MEMBER_BYTES], quotedSource[QUOTED_PATH_BYTES], quotedCopy[QUOTED_PATH_BYTES];
    char fromCwd[CWD_MEMBER_BYTES], toCwd[CWD_MEMBER_BYTES], fromSubfolder[CWD_MEMBER_BYTES], toSubfolder[CWD_MEMBER_BYTES];
    CoreSwap swaps[3];
    SYSTEMTIME today;
    DWORD random = 0;
    CopyResult folderCopied;
    int count = 1;
    BOOL scratch = set->groups[source->group].scratchOf >= 0, madeFolder = FALSE, madeProjectDir = FALSE;

    error[0] = 0;
    copiedFiles[0] = 0;
    ZeroMemory(&g_lastCopy, sizeof g_lastCopy);
    if (!source->transcript || !Core_IsUuid(source->key)) {
        StringCchCopyW(error, errorCch, TR(L"This session has no conversation on disk to copy."));
        return COPY_FAILED;
    }
    if (!NewSessionId(newId, idCch, &random)) {
        StringCchCopyW(error, errorCch, TR(L"A new session ID could not be made."));
        return COPY_FAILED;
    }
    StringCchCopyW(transcriptDir, ARRAYSIZE(transcriptDir), source->transcriptPath);
    if ((slash = wcsrchr(transcriptDir, L'\\')) != NULL) *slash = 0;
    if (scratch) {
        if (!destination->scratchDir[0]) {
            StringCchPrintfW(error, errorCch, TR(L"\x201C%s\x201D has no Claude Code sessions yet: open its Code tab once, then copy again."),
                             targetProfile->name);
            return COPY_FAILED;
        }
        GetSystemTime(&today);   /* Claude dates them in UTC */
        Core_ScratchName(&today, random, scratchName, ARRAYSIZE(scratchName));
        folderCopied = COPY_FAILED;
        if (SUCCEEDED(StringCchPrintfW(cwd, ARRAYSIZE(cwd), L"%s\\%s", destination->scratchDir, scratchName)) &&
            SessionStore_WorkingDir(set, source->cwd, sourceFiles, ARRAYSIZE(sourceFiles)) &&
            Core_ProfileFilePath(targetProfile, destination->scratchDir, scratchFiles, ARRAYSIZE(scratchFiles)) &&
            Core_ProfileFilePath(targetProfile, cwd, copiedFiles, ARRAYSIZE(copiedFiles)) && Util_EnsureDir(scratchFiles))
            folderCopied = CopyFolder(owner, sourceFiles, copiedFiles);
        if (folderCopied != COPY_MADE) {
            Util_Log(L"session %s: its working folder %s was not copied for %s", source->key, source->cwd, targetProfile->folder);
            if (folderCopied == COPY_FAILED) StringCchCopyW(error, errorCch, TR(L"Its working folder could not be copied."));
            return folderCopied;
        }
        madeFolder = TRUE;
        /* Filed where Claude Code keeps that folder's sessions; a working
         * folder too long for a project folder name keeps the copy beside the
         * original (Claude Code finds it by id). */
        if (Core_ProjectDirName(cwd, projectName, ARRAYSIZE(projectName)) && SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) &&
            SUCCEEDED(StringCchPrintfW(projectDir, ARRAYSIZE(projectDir), L"%s\\%s", projects, projectName)) &&
            Util_ExtendedPath(projectDir, extended, ARRAYSIZE(extended))) {
            if (CreateDirectoryW(extended, NULL)) madeProjectDir = TRUE;
            if (madeProjectDir || GetLastError() == ERROR_ALREADY_EXISTS)
                StringCchCopyW(transcriptDir, ARRAYSIZE(transcriptDir), projectDir);
        }
    }
    if (FAILED(StringCchPrintfW(copy, ARRAYSIZE(copy), L"%s\\%s.jsonl", transcriptDir, newId)) ||
        FAILED(StringCchPrintfA(fromId, sizeof fromId, "\"sessionId\":\"%ls\"", source->key)) ||
        FAILED(StringCchPrintfA(toId, sizeof toId, "\"sessionId\":\"%ls\"", newId))) {
        StringCchCopyW(error, errorCch, TR(L"The path is too long."));
        goto failed;
    }
    swaps[0].from = fromId;
    swaps[0].to = toId;
    if (scratch) {
        /* Every line's working folder, or a subfolder of it (after a cd),
         * points at the copy's: the copy would otherwise go on in the
         * original's. The subfolder form is the quoted path without its
         * closing quote, then an escaped backslash. */
        if (!Core_JsonQuote(source->cwd, quotedSource, sizeof quotedSource) || !Core_JsonQuote(cwd, quotedCopy, sizeof quotedCopy) ||
            FAILED(StringCchPrintfA(fromCwd, sizeof fromCwd, "\"cwd\":%s", quotedSource)) ||
            FAILED(StringCchPrintfA(toCwd, sizeof toCwd, "\"cwd\":%s", quotedCopy)) ||
            FAILED(StringCchPrintfA(fromSubfolder, sizeof fromSubfolder, "\"cwd\":%.*s\\\\", (int)strlen(quotedSource) - 1, quotedSource)) ||
            FAILED(StringCchPrintfA(toSubfolder, sizeof toSubfolder, "\"cwd\":%.*s\\\\", (int)strlen(quotedCopy) - 1, quotedCopy))) {
            StringCchCopyW(error, errorCch, TR(L"The path is too long."));
            goto failed;
        }
        swaps[1].from = fromCwd;
        swaps[1].to = toCwd;
        swaps[2].from = fromSubfolder;
        swaps[2].to = toSubfolder;
        count = 3;
    }
    if (!CopyTranscript(source->transcriptPath, copy, swaps, count)) {
        StringCchCopyW(error, errorCch, TR(L"The conversation could not be copied."));
        goto failed;
    }
    StringCchCopyW(g_lastCopy.copyId, ARRAYSIZE(g_lastCopy.copyId), newId);
    StringCchCopyW(g_lastCopy.sourceKey, ARRAYSIZE(g_lastCopy.sourceKey), source->key);
    StringCchCopyW(g_lastCopy.targetFolder, ARRAYSIZE(g_lastCopy.targetFolder), targetProfile->folder);
    StringCchCopyW(g_lastCopy.transcript, ARRAYSIZE(g_lastCopy.transcript), copy);
    if (madeProjectDir) StringCchCopyW(g_lastCopy.projectFolder, ARRAYSIZE(g_lastCopy.projectFolder), projectDir);
    if (madeFolder) StringCchCopyW(g_lastCopy.workingFolder, ARRAYSIZE(g_lastCopy.workingFolder), copiedFiles);
    Util_Log(L"session %s copied as %s for %s", source->key, newId, targetProfile->folder);
    return COPY_MADE;
failed:
    if (madeProjectDir) RemoveEmptyFolder(projectDir);
    if (madeFolder) RemoveCopiedFolder(copiedFiles);
    return COPY_FAILED;
}

/* What SessionEdit_CopyConversation made for `copyId` (row `row` copied for
 * profile `target`), deleted for good when the copy could not be opened and
 * so no profile lists it: its transcript, the project folder made for it,
 * and for a session without a folder the copy of its working folder. Only
 * for the latest copy, which this program has just made. FALSE, with `left`
 * naming what stays, when it could not all be deleted. */
BOOL SessionEdit_RemoveCopy(const SessionSet *set, int row, int target, const WCHAR *copyId, WCHAR *left, size_t leftCch)
{
    WCHAR extended[LONG_PATH_CCH];
    BOOL removed = TRUE;
    if (leftCch) left[0] = 0;
    if (!g_lastCopy.copyId[0] || !Core_EqualsI(copyId, g_lastCopy.copyId) || !Core_EqualsI(set->rows[row].key, g_lastCopy.sourceKey) ||
        !Core_EqualsI(set->profiles.items[target].folder, g_lastCopy.targetFolder)) {
        Util_Log(L"session copy %s is not the latest copy made: nothing removed", copyId);
        return FALSE;
    }
    if (!Util_ExtendedPath(g_lastCopy.transcript, extended, ARRAYSIZE(extended)) ||
        (!DeleteFileW(extended) && GetLastError() != ERROR_FILE_NOT_FOUND)) {
        Util_Log(L"could not delete %s (error %lu)", g_lastCopy.transcript, GetLastError());
        if (leftCch) StringCchCopyW(left, leftCch, g_lastCopy.transcript);
        removed = FALSE;
    }
    if (g_lastCopy.workingFolder[0] && !RemoveCopiedFolder(g_lastCopy.workingFolder)) {
        if (removed && leftCch) StringCchCopyW(left, leftCch, g_lastCopy.workingFolder);
        removed = FALSE;
    }
    if (g_lastCopy.projectFolder[0]) RemoveEmptyFolder(g_lastCopy.projectFolder);   /* kept if Claude Code filed more there */
    Util_Log(L"session copy %s %s: it could not be opened", copyId, removed ? L"removed" : L"partly removed");
    ZeroMemory(&g_lastCopy, sizeof g_lastCopy);
    return removed;
}

/* ----------------------------------------------------------- delete */

/* What Claude Code keeps for a session, named after the id of each of its
 * transcripts, as Claude's own delete removes it. */
typedef struct SessionItem {
    const WCHAR *store;    /* a folder in Claude Code's; NULL: each project folder */
    const WCHAR *suffix;   /* after the id */
    BOOL         folder;
} SessionItem;

static const SessionItem kSessionItems[] = {
    { NULL, L".jsonl", FALSE },                     /* the transcript */
    { NULL, L"", TRUE },                            /* its subagents, tool results and title */
    { NULL, L".jsonl.pre-import", FALSE },          /* the transcript as it was before Claude took it in */
    { NULL, L".desktop-released.json", FALSE },     /* Claude's mark of a transcript it let go */
    { NULL, L".ccr-tip.json", FALSE },
    { NULL, L".precompact.json", FALSE },
    { L"file-history", L"", TRUE },                 /* the files before each edit, for rewinding */
    { L"session-env", L"", TRUE },
    { L"uploads", L"", TRUE },
    { L"tasks", L"", TRUE },
    { L"image-cache", L"", TRUE },
    { L"debug", L".txt", FALSE },
    { L"debug", L".1.txt", FALSE },
    { L"usage-data\\facets", L".json", FALSE },
    { L"usage-data\\session-meta", L".json", FALSE },
    { L"startup-perf", L".txt", FALSE },
    { L"startup-perf", L".json", FALSE },
};

static BOOL AddSessionFile(WCHAR (**paths)[LONG_PATH_CCH], int *count, int *capacity, const WCHAR *file)
{
    if (*count == *capacity) {
        WCHAR (*grown)[LONG_PATH_CCH];
        int larger;
        if (*capacity > INT_MAX / 2) return FALSE;
        larger = *capacity ? *capacity * 2 : LISTED_FILES_FIRST;
        grown = *paths ? (WCHAR (*)[LONG_PATH_CCH])HeapReAlloc(GetProcessHeap(), 0, *paths, (size_t)larger * sizeof **paths)
                       : (WCHAR (*)[LONG_PATH_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)larger * sizeof **paths);
        if (!grown) return FALSE;
        *paths = grown;
        *capacity = larger;
    }
    if (FAILED(StringCchCopyW((*paths)[*count], ARRAYSIZE(**paths), file))) return FALSE;
    (*count)++;
    return TRUE;
}

/* Adds `path` when it is there as a folder (`folder`) or a file. FALSE when
 * that cannot be told or it cannot be added: `failed` and `code` then say
 * where and why. */
static BOOL AddIfPresent(WCHAR (**paths)[LONG_PATH_CCH], int *count, int *capacity, const WCHAR *path, BOOL folder,
                         WCHAR *failed, DWORD *code)
{
    WCHAR extended[LONG_PATH_CCH];
    DWORD attributes;
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        StringCchCopyW(failed, LONG_PATH_CCH, path);
        return FALSE;
    }
    switch (Util_QueryPath(extended, &attributes)) {
    case PATH_MISSING:
        return TRUE;
    case PATH_PRESENT:
        if (((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != folder || AddSessionFile(paths, count, capacity, path)) return TRUE;
        *code = ERROR_NOT_ENOUGH_MEMORY;
        break;
    default:
        *code = GetLastError();
        break;
    }
    StringCchCopyW(failed, LONG_PATH_CCH, path);
    return FALSE;
}

/* Adds <folder>\<id><suffix> of `item` when it is there. */
static BOOL AddPresentItem(WCHAR (**paths)[LONG_PATH_CCH], int *count, int *capacity, const WCHAR *folder, const WCHAR *id,
                           const SessionItem *item, WCHAR *failed, DWORD *code)
{
    WCHAR path[LONG_PATH_CCH];
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s%s", folder, id, item->suffix))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        StringCchCopyW(failed, LONG_PATH_CCH, folder);
        return FALSE;
    }
    return AddIfPresent(paths, count, capacity, path, item->folder, failed, code);
}

/* Claude Code's own temporary folder: CLAUDE_CODE_TMPDIR, else Windows'
 * temporary folder, then claude. */
static BOOL ClaudeCodeTempDir(WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH], full[MAX_PATH], whole[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", base, ARRAYSIZE(base));
    if (length >= ARRAYSIZE(base)) return FALSE;
    if (length == 0 && ((length = GetTempPathW(ARRAYSIZE(base), base)) == 0 || length >= ARRAYSIZE(base))) return FALSE;
    length = GetFullPathNameW(base, ARRAYSIZE(full), full, NULL);
    if (length == 0 || length >= ARRAYSIZE(full)) return FALSE;
    /* The folder's own names, as the Recycle Bin then shows them (Windows'
     * temporary folder can come in 8.3 names). */
    length = GetLongPathNameW(full, whole, ARRAYSIZE(whole));
    if (length > 0 && length < ARRAYSIZE(whole)) StringCchCopyW(full, ARRAYSIZE(full), whole);
    for (length = (DWORD)wcslen(full); length > 0 && full[length - 1] == L'\\';) full[--length] = 0;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" CLAUDE_CODE_TEMP_DIR, full));
}

/* Claude Code's temporary folder of each of `ids` (<temp>\claude\<project>\<id>),
 * as Claude's delete removes it. One too long for the Recycle Bin is left
 * to Claude Code, which clears its temporary folder itself. */
static BOOL AddTemporaryFolders(WCHAR (**paths)[LONG_PATH_CCH], int *count, int *capacity, const WCHAR *const *ids, int idCount,
                                WCHAR *failed, DWORD *code)
{
    WCHAR root[MAX_PATH], project[LONG_PATH_CCH], folder[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    int i;
    if (!ClaudeCodeTempDir(root, ARRAYSIZE(root)) || (find = Util_FindFiles(root, L"*", &found, TRUE)) == INVALID_HANDLE_VALUE)
        return TRUE;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.' ||
            FAILED(StringCchPrintfW(project, ARRAYSIZE(project), L"%s\\%s", root, found.cFileName)))
            continue;
        for (i = 0; i < idCount; i++) {
            if (FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", project, ids[i]))) continue;
            if (!Util_FitsRecycleBin(folder)) {
                if (!PathMissing(folder)) Util_Log(L"%s left to Claude Code: its path is too long for the Recycle Bin", folder);
                continue;
            }
            if (!AddIfPresent(paths, count, capacity, folder, TRUE, failed, code)) {
                FindClose(find);
                return FALSE;
            }
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return TRUE;
}

/* The transcript Claude staged when it took the session in (stagedTranscriptPath),
 * when it is in the staging folder beside entry `file`, as Claude's delete checks. */
static BOOL AddStagedTranscript(WCHAR (**paths)[LONG_PATH_CCH], int *count, int *capacity, const WCHAR *file,
                                WCHAR *failed, DWORD *code)
{
    WCHAR staging[LONG_PATH_CCH], staged[LONG_PATH_CCH], *slash;
    const char *value;
    size_t valueLength;
    DWORD length = 0;
    char *json = Util_ReadFile(file, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
    BOOL named;
    if (!json) return TRUE;
    named = Core_JsonMember(json, length, "stagedTranscriptPath", &value, &valueLength) &&
            Core_JsonString(value, valueLength, staged, ARRAYSIZE(staged));
    HeapFree(GetProcessHeap(), 0, json);
    if (!named || FAILED(StringCchCopyW(staging, ARRAYSIZE(staging), file)) || (slash = wcsrchr(staging, L'\\')) == NULL) return TRUE;
    *slash = 0;
    if (FAILED(StringCchCatW(staging, ARRAYSIZE(staging), L"\\" STAGING_DIR)) || !Core_PathUnder(staged, staging) ||
        Core_PathEquals(staged, staging) || wcsstr(staged, L"\\..") || wcschr(staged, L'/'))
        return TRUE;
    return AddIfPresent(paths, count, capacity, staged, FALSE, failed, code);
}

/* A session without a folder works in a folder of its own, right in its
 * profile's "no folder" area: Claude's delete removes it with the session,
 * unless another session works there. TRUE with its physical path when it
 * would go. */
BOOL SessionEdit_RemovesWorkingFolder(const SessionSet *set, int row, WCHAR *physical, size_t cch)
{
    const SessionRow *session = &set->rows[row];
    WCHAR area[MAX_PATH], *slash;
    int owner, r;
    if (!set->groups || session->group < 0 || session->group >= set->groupCount ||
        (owner = set->groups[session->group].scratchOf) < 0 || !set->source[owner].scratchDir[0] ||
        FAILED(StringCchCopyW(area, ARRAYSIZE(area), session->cwd)) || (slash = wcsrchr(area, L'\\')) == NULL)
        return FALSE;
    *slash = 0;
    if (!Core_PathEquals(area, set->source[owner].scratchDir)) return FALSE;
    for (r = 0; r < set->rowCount; r++)
        if (r != row && Core_PathUnder(set->rows[r].cwd, session->cwd)) return FALSE;
    return SessionStore_WorkingDir(set, session->cwd, physical, cch) && Util_DirExists(physical);
}

/* The complete removal, listed before any file moves: the session's entries
 * in every profile with the transcript Claude staged for each; for each of
 * its transcripts that no other session claims, what Claude Code keeps for
 * it (kSessionItems) and its temporary folder; for a session without a
 * folder, its working folder (SessionEdit_RemovesWorkingFolder). The caller
 * frees the heap array. FALSE with the reason in `error` (and logged) when
 * a part of it could not be told. */
BOOL SessionEdit_ListFiles(const SessionSet *set, int row, WCHAR (**paths)[LONG_PATH_CCH], int *count, WCHAR *error, size_t errorCch)
{
    const SessionRow *session;
    const WCHAR **ids = NULL;
    WCHAR projects[LONG_PATH_CCH], folder[LONG_PATH_CCH], failed[LONG_PATH_CCH], working[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find = INVALID_HANDLE_VALUE;
    DWORD code = ERROR_INVALID_PARAMETER;
    BOOL ok = FALSE;
    int p, entry, capacity = 0, idCount = 0, i, item;
    *paths = NULL;
    *count = 0;
    error[0] = 0;
    failed[0] = 0;
    if (row < 0 || row >= set->rowCount) goto done;
    session = &set->rows[row];
    StringCchCopyW(failed, ARRAYSIZE(failed), session->key);
    for (p = 0; p < set->profiles.count; p++)
        for (entry = session->entry[p]; entry >= 0; entry = set->entries[entry].duplicate) {
            if (!AddSessionFile(paths, count, &capacity, set->entries[entry].file)) {
                code = ERROR_NOT_ENOUGH_MEMORY;
                goto done;
            }
            if (!AddStagedTranscript(paths, count, &capacity, set->entries[entry].file, failed, &code)) goto done;
        }
    if (SessionEdit_RemovesWorkingFolder(set, row, working, ARRAYSIZE(working)) &&
        !AddIfPresent(paths, count, &capacity, working, TRUE, failed, &code))
        goto done;
    if ((ids = SessionStore_TranscriptIds(set, row, &idCount)) == NULL) {
        code = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    /* A transcript another session claims stays, with what Claude Code keeps for it. */
    for (i = 0; i < idCount; i++)
        if (SessionStore_ClaimedByOther(set, row, -1, ids[i])) ids[i--] = ids[--idCount];
    if (idCount == 0) {
        ok = TRUE;
        goto done;
    }
    for (item = 0; item < (int)ARRAYSIZE(kSessionItems); item++) {
        if (!kSessionItems[item].store) continue;
        if (!SessionStore_ClaudeCodePath(kSessionItems[item].store, folder, ARRAYSIZE(folder))) {
            code = ERROR_FILENAME_EXCED_RANGE;
            StringCchCopyW(failed, ARRAYSIZE(failed), L"CLAUDE_CONFIG_DIR");
            goto done;
        }
        for (i = 0; i < idCount; i++)
            if (!AddPresentItem(paths, count, &capacity, folder, ids[i], &kSessionItems[item], failed, &code)) goto done;
    }
    if (!AddTemporaryFolders(paths, count, &capacity, ids, idCount, failed, &code)) goto done;
    if (!SessionStore_ProjectsDir(projects, ARRAYSIZE(projects))) {
        code = ERROR_FILENAME_EXCED_RANGE;
        StringCchCopyW(failed, ARRAYSIZE(failed), L"CLAUDE_CONFIG_DIR");
        goto done;
    }
    find = Util_FindFiles(projects, L"*", &found, TRUE);
    if (find == INVALID_HANDLE_VALUE) {
        code = GetLastError();
        ok = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
        StringCchCopyW(failed, ARRAYSIZE(failed), projects);
        goto done;
    }
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        if (FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", projects, found.cFileName))) {
            code = ERROR_FILENAME_EXCED_RANGE;
            StringCchCopyW(failed, ARRAYSIZE(failed), found.cFileName);
            goto done;
        }
        for (item = 0; item < (int)ARRAYSIZE(kSessionItems); item++)
            for (i = 0; !kSessionItems[item].store && i < idCount; i++)
                if (!AddPresentItem(paths, count, &capacity, folder, ids[i], &kSessionItems[item], failed, &code)) goto done;
    } while (FindNextFileW(find, &found));
    code = GetLastError();
    ok = code == ERROR_NO_MORE_FILES;
    StringCchCopyW(failed, ARRAYSIZE(failed), projects);
done:
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    if (ids) HeapFree(GetProcessHeap(), 0, (void *)ids);
    if (!ok) {
        if (*paths) HeapFree(GetProcessHeap(), 0, *paths);
        *paths = NULL;
        *count = 0;
        StringCchPrintfW(error, errorCch, TR(L"Its files could not all be listed (error %lu): %s"), code, failed);
        Util_Log(L"session files could not all be listed (error %lu): %s", code, failed);
    }
    return ok;
}

/* No Claude would write the session back once it is deleted: no profile
 * that lists it runs, and no Claude Code runs it (one a profile started
 * without listing it yet, or one in a terminal). FALSE with the reason in
 * `error`. */
BOOL SessionEdit_CanDelete(const SessionSet *set, int row, WCHAR *error, size_t errorCch)
{
    const SessionRow *session = &set->rows[row];
    ProfileList now;
    DWORD running = 0;
    BOOL outside = FALSE;
    int p;
    if (errorCch) error[0] = 0;
    for (p = 0; p < set->profiles.count; p++)
        if (session->entry[p] >= 0 && Claude_IsRunning(&set->profiles.items[p])) running |= 1u << p;
    if (!running) {
        now = set->profiles;
        Claude_UpdateRunning(&now);
        running = SessionStore_RunningNow(&now, session->key, &outside);
    }
    for (p = 0; p < set->profiles.count && !(running & (1u << p)); p++) {}
    if (p < set->profiles.count)
        StringCchPrintfW(error, errorCch, TR(L"Close \x201C%s\x201D first: while it runs, Claude keeps this session and would write it back."),
                         set->profiles.items[p].name);
    else if (outside)
        StringCchCopyW(error, errorCch, TR(L"Close the Claude Code that runs it first (in a terminal, for example): it would write it back."));
    else
        return TRUE;
    Util_Log(L"session %s not deleted: it runs in %s", session->key,
             p < set->profiles.count ? set->profiles.items[p].folder : L"a Claude Code of no profile");
    return FALSE;
}

/* Every entry of the session (in each profile's current account and
 * organization) and its conversation (SessionEdit_ListFiles) to the Recycle
 * Bin, then Claude's marks that it was deleted and the changes waiting for it
 * dropped. Only when nothing runs it; a profile started while Windows asked
 * keeps the session in its Claude: its entry goes when that Claude closes. */
RemoveResult SessionEdit_DeleteEverywhere(HWND owner, const SessionSet *set, int row, WCHAR *error, size_t errorCch)
{
    const SessionRow *session = &set->rows[row];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    const WCHAR **list = NULL;
    int pathCount = 0, i, p;
    RemoveResult result = REMOVE_FAILED;

    error[0] = 0;
    if (!SessionEdit_CanDelete(set, row, error, errorCch) || !SessionEdit_ListFiles(set, row, &paths, &pathCount, error, errorCch)) goto done;
    /* Rather nothing deleted than a part. */
    for (i = 0; i < pathCount; i++)
        if (!FitsRecycleBin(paths[i], error, errorCch)) goto done;
    list = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)max(pathCount, 1) * sizeof *list);
    if (!list) {
        StringCchPrintfW(error, errorCch, TR(L"Its files could not all be listed (error %lu): %s"), (DWORD)ERROR_NOT_ENOUGH_MEMORY, session->key);
        goto done;
    }
    /* Listing can take time; a Claude may have started meanwhile. */
    if (!SessionEdit_CanDelete(set, row, error, errorCch)) goto done;
    for (i = 0; i < pathCount; i++) list[i] = paths[i];
    result = Util_Recycle(owner, list, pathCount);
    Util_Log(L"session %s deleted everywhere (%d item(s)): %s", session->key, pathCount,
             result == REMOVE_DONE ? L"done" : result == REMOVE_CANCELLED ? L"cancelled" : L"FAILED");
    if (result == REMOVE_FAILED) StringCchCopyW(error, errorCch, TR(L"Some of its files could not be moved to the Recycle Bin."));
    if (result != REMOVE_DONE) goto done;
    for (p = 0; p < set->profiles.count; p++) {
        int keyCount = 0;
        PendingEdit *keys = SessionKeys(set, row, p, PENDING_REMOVE, &keyCount);
        if (!keys) continue;
        if (session->entry[p] >= 0 && Claude_IsRunning(&set->profiles.items[p])) {
            if (RewritePendingLocked(&set->profiles.items[p], &keys[0], NULL, 0, FALSE))
                Util_Log(L"session %s in %s: removal waits, its Claude started", session->key, set->profiles.items[p].folder);
        } else {
            if (session->entry[p] >= 0) MarkDeleted(set, row, p);
            /* What waited for the session there has nothing to change now. */
            RewritePendingLocked(&set->profiles.items[p], NULL, keys, keyCount, TRUE);
        }
        HeapFree(GetProcessHeap(), 0, keys);
    }
done:
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (list) HeapFree(GetProcessHeap(), 0, (void *)list);
    return result;
}
