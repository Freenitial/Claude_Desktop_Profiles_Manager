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
 * - a new title, a favorite or a removal is made at once in a closed
 *   profile, else kept in a file of ours (pending-sessions-<folder>.txt) and
 *   made when the profile has closed: after its watcher sees Claude exit
 *   (main.c), before it opens through us (Launcher_Open), or when the
 *   manager finds it closed.
 * A shared session is one conversation (one transcript) that each profile
 * lists; a copy is a new conversation, the transcript copied under a new id,
 * that goes on separately.
 */
#include "app.h"
#include <objbase.h>
#include <shellapi.h>
#include <wchar.h>

#define ENTRY_MAX_BYTES (4u * 1024u * 1024u)
#define COPY_CHUNK      (1024u * 1024u)

/* -------------------------------------------------------------- open */

HRESULT SessionEdit_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *sessionId)
{
    WCHAR link[128];
    DWORD pid = 0;
    BOOL identity = FALSE;
    HRESULT hr;
    if (!Core_ResumeLink(sessionId, link, ARRAYSIZE(link))) return E_INVALIDARG;
    hr = Launcher_Open(pkg, p, link, &pid, &identity);
    Util_Log(L"session %s -> %s%s", sessionId, p->folder, SUCCEEDED(hr) ? L"" : L" FAILED");
    return hr;
}

/* ----------------------------------------------------------- files */

static BOOL WriteWhole(HANDLE h, const char *data, size_t len)
{
    while (len > 0) {
        DWORD n = 0, chunk = len > 0x40000000u ? 0x40000000u : (DWORD)len;
        if (!WriteFile(h, data, chunk, &n, NULL) || n == 0) return FALSE;
        data += n;
        len -= n;
    }
    return TRUE;
}

/* `path` replaced by `data` in one step: written next to it, then moved over it. */
static BOOL SaveFile(const WCHAR *path, const char *data, size_t len)
{
    WCHAR temp[MAX_PATH];
    HANDLE h;
    BOOL ok;
    if (FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s.cdm-new", path))) return FALSE;
    h = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteWhole(h, data, len) && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp);
    return ok;
}

/* Sets members of an entry file (`raw` are JSON values). */
static BOOL SetMembers(const WCHAR *file, const char *const *keys, const char *const *raws, int count)
{
    DWORD len = 0;
    char *json = Util_ReadFile(file, ENTRY_MAX_BYTES, FALSE, &len), *a = NULL, *b = NULL;
    size_t cap = (size_t)len + 4096 + 2 * SESSION_TITLE_CCH * 4, n = len;
    BOOL ok = json != NULL;
    int i;
    if (ok) {
        a = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        b = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        ok = a && b && len < cap;
    }
    if (ok) memcpy(a, json, len);
    for (i = 0; ok && i < count; i++) {
        char *t;
        ok = Core_JsonSetMember(a, n, keys[i], raws[i], b, cap, &n);
        t = a;
        a = b;
        b = t;
    }
    if (ok) ok = SaveFile(file, a, n);
    if (json) HeapFree(GetProcessHeap(), 0, json);
    if (a) HeapFree(GetProcessHeap(), 0, a);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    return ok;
}

/* One change made to an entry file, its profile being closed. */
static BOOL ApplyToEntry(const WCHAR *file, const PendingEdit *edit)
{
    char title[SESSION_TITLE_CCH * 4 + 8];
    const char *keys[2], *raws[2];
    switch (edit->op) {
    case PENDING_TITLE:
        if (!edit->value[0] || !Core_JsonQuote(edit->value, title, sizeof title)) return FALSE;
        keys[0] = "title";
        raws[0] = title;
        keys[1] = "titleSource";   /* a title someone chose: Claude does not replace it */
        raws[1] = "\"user\"";
        return SetMembers(file, keys, raws, 2);
    case PENDING_STAR:
        keys[0] = "isStarred";
        raws[0] = edit->value[0] == L'1' ? "true" : "false";
        return SetMembers(file, keys, raws, 1);
    case PENDING_REMOVE:
        return Util_Recycle(NULL, &file, 1) == REMOVE_DONE;
    }
    return FALSE;
}

/* ---------------------------------------------------------- pending */

/* The file of changes for `p`, rewritten: `add` (when not NULL) replaces the
 * change of the same kind to the same session (a removal replaces them all),
 * `drop` (when not NULL) cancels the one it names. */
static BOOL RewritePending(const Profile *p, const PendingEdit *add, const PendingEdit *drop)
{
    WCHAR path[MAX_PATH], line[SESSION_ID_CCH + SESSION_TITLE_CCH + 16];
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, (SESSION_PENDING_MAX + 1) * sizeof *edits);
    char *out = NULL;
    size_t used = 0, cap = 0;
    int n, i, kept = 0;
    BOOL ok = FALSE;
    if (!edits || !SessionStore_PendingPath(p, path, ARRAYSIZE(path))) goto done;
    n = SessionStore_LoadPending(p, edits, SESSION_PENDING_MAX);
    for (i = 0; i < n; i++) {
        const PendingEdit *e = &edits[i];
        BOOL same = FALSE;
        if (add && CompareStringOrdinal(e->key, -1, add->key, -1, TRUE) == CSTR_EQUAL)
            same = e->op == add->op || add->op == PENDING_REMOVE || e->op == PENDING_REMOVE;
        if (drop && e->op == drop->op && CompareStringOrdinal(e->key, -1, drop->key, -1, TRUE) == CSTR_EQUAL) same = TRUE;
        if (!same) edits[kept++] = *e;
    }
    if (add) {
        if (kept >= SESSION_PENDING_MAX) goto done;
        edits[kept++] = *add;
    }
    if (kept == 0) {
        ok = DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
        goto done;
    }
    cap = (size_t)kept * sizeof line * 3 + 16;
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, cap)) == NULL) goto done;
    for (i = 0; i < kept; i++) {
        int bytes;
        if (!Core_PendingFormat(&edits[i], line, ARRAYSIZE(line)) || FAILED(StringCchCatW(line, ARRAYSIZE(line), L"\n"))) continue;
        bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, out + used, (int)(cap - used), NULL, NULL);
        if (bytes > 0) used += (size_t)bytes - 1;
    }
    {
        WCHAR dir[MAX_PATH];
        if (Util_StateDir(dir, ARRAYSIZE(dir))) Util_EnsureDir(dir);
    }
    ok = SaveFile(path, out, used);
done:
    if (out) HeapFree(GetProcessHeap(), 0, out);
    if (edits) HeapFree(GetProcessHeap(), 0, edits);
    return ok;
}

BOOL SessionEdit_Cancel(const Profile *p, const PendingEdit *edit)
{
    return RewritePending(p, NULL, edit);
}

/* Changes one profile's entry for a session: at once when the profile is
 * closed, else when it closes (*waiting then). */
BOOL SessionEdit_Change(const Profile *p, const SessionEntry *e, const PendingEdit *edit, BOOL *waiting)
{
    *waiting = FALSE;
    if (e && e->file[0] && !Claude_IsRunning(p)) {
        BOOL ok = ApplyToEntry(e->file, edit);
        Util_Log(L"session %s in %s: %s changed%s", edit->key, p->folder, edit->op == PENDING_TITLE ? L"title"
                 : edit->op == PENDING_STAR ? L"favorite" : L"entry removed", ok ? L"" : L" FAILED");
        if (ok) RewritePending(p, NULL, edit);   /* an older wait for the same change is over */
        return ok;
    }
    *waiting = TRUE;
    return RewritePending(p, edit, NULL);
}

/* Makes the changes waiting for `p`, when its Claude is not running. The
 * entries are looked up again: the one to change may have been created
 * since (a session shared or copied there). Changes to sessions the profile
 * does not list (yet) wait on. Returns how many were made. */
int SessionEdit_ApplyPending(const Profile *p)
{
    PendingEdit *edits;
    SessionSet set;
    int n, i, made = 0, at, r;
    if (Claude_IsRunning(p)) return 0;
    edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    if (!edits) return 0;
    n = SessionStore_LoadPending(p, edits, SESSION_PENDING_MAX);
    if (n > 0) {
        SessionStore_Load(&set);
        at = Profiles_Find(&set.profiles, p->folder);
        if (at >= 0 && !set.profiles.items[at].running) {
            for (i = 0; i < n; i++) {
                for (r = 0; r < set.rowCount; r++)
                    if (CompareStringOrdinal(set.rows[r].key, -1, edits[i].key, -1, TRUE) == CSTR_EQUAL) break;
                if (r == set.rowCount || set.rows[r].entry[at] < 0) continue;
                if (ApplyToEntry(set.entries[set.rows[r].entry[at]].file, &edits[i])) {
                    RewritePending(p, NULL, &edits[i]);
                    made++;
                }
            }
            if (made) Util_Log(L"%d waiting session change(s) made in %s", made, p->folder);
        }
        SessionStore_Free(&set);
    }
    HeapFree(GetProcessHeap(), 0, edits);
    return made;
}

void SessionEdit_ApplyPendingFor(const WCHAR *folder)
{
    ProfileList list;
    int i;
    Profiles_Load(&list);
    if ((i = Profiles_Find(&list, folder)) >= 0) SessionEdit_ApplyPending(&list.items[i]);
}

/* ------------------------------------------------------------ copy */

static BOOL NewSessionId(WCHAR *out, size_t cch, DWORD *random)
{
    GUID g;
    WCHAR text[40];
    size_t i;
    if (FAILED(CoCreateGuid(&g)) || StringFromGUID2(&g, text, ARRAYSIZE(text)) != 39) return FALSE;
    text[37] = 0;   /* {xxxxxxxx-...} */
    for (i = 1; i < 37; i++) text[i] = (WCHAR)towlower(text[i]);
    *random = g.Data1;
    return SUCCEEDED(StringCchCopyW(out, cch, text + 1));
}

/* The transcript `from` copied to `to` with `swaps` made, through a file
 * next to it moved in place at the end. */
static BOOL CopyTranscript(const WCHAR *from, const WCHAR *to, const CoreSwap *swaps, int count)
{
    WCHAR temp[MAX_PATH];
    HANDLE in, out;
    char *buf, *res;
    size_t held = 0, shortest = (size_t)-1, longest = 0, cap;
    BOOL ok = TRUE, last = FALSE;
    int i;
    for (i = 0; i < count; i++) {
        shortest = min(shortest, strlen(swaps[i].from));
        longest = max(longest, strlen(swaps[i].to));
    }
    if (shortest == 0 || FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s.cdm-new", to))) return FALSE;
    cap = 2 * COPY_CHUNK + (2 * COPY_CHUNK / shortest + 1) * longest;
    in = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) return FALSE;
    out = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, 2 * COPY_CHUNK);
    res = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
    if (out == INVALID_HANDLE_VALUE || !buf || !res) ok = FALSE;
    while (ok && !last) {
        DWORD got = 0;
        size_t used = 0, written;
        ok = ReadFile(in, buf + held, COPY_CHUNK, &got, NULL);
        last = got == 0;
        if (!ok) break;
        written = Core_ReplaceChunk(buf, held + got, swaps, count, last, res, &used);
        ok = WriteWhole(out, res, written);
        held = held + got - used;
        memmove(buf, buf + used, held);   /* what may start a swap goes with the next chunk */
    }
    CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (res) HeapFree(GetProcessHeap(), 0, res);
    if (ok) ok = MoveFileExW(temp, to, MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp);
    return ok;
}

/* A folder's content copied into a new folder (created, even when there is
 * nothing to copy). */
static BOOL CopyFolder(const WCHAR *from, const WCHAR *to)
{
    WCHAR src[MAX_PATH + 4], dst[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    if (!CreateDirectoryW(to, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    if (!Util_DirExists(from)) return TRUE;
    ZeroMemory(src, sizeof src);
    ZeroMemory(dst, sizeof dst);
    if (FAILED(StringCchPrintfW(src, MAX_PATH + 2, L"%s\\*", from)) || FAILED(StringCchCopyW(dst, MAX_PATH, to))) return FALSE;
    ZeroMemory(&op, sizeof op);
    op.wFunc = FO_COPY;
    op.pFrom = src;
    op.pTo = dst;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);   /* an empty folder "fails" with nothing to copy */
    return !op.fAnyOperationsAborted;
}

/* A new conversation for profile `target`, copied from row `r`: a new id,
 * the transcript copied under it. A session without a folder gets its own
 * working folder in the target's "no folder" area (its files copied), so it
 * is there that it shows. `newId` gets the copy's id. */
BOOL SessionEdit_CopyConversation(const SessionSet *s, int r, int target, WCHAR *newId, size_t idCch, WCHAR *error, size_t errorCch)
{
    const SessionRow *row = &s->rows[r];
    const SessionSource *dest = &s->source[target];
    WCHAR cwd[MAX_PATH], dir[MAX_PATH], name[64], to[MAX_PATH], projects[MAX_PATH], *slash;
    char fromId[80], toId[80], fromCwd[MAX_PATH * 4 + 16], toCwd[MAX_PATH * 4 + 16];
    char q1[MAX_PATH * 3 + 4], q2[MAX_PATH * 3 + 4];
    CoreSwap swaps[2];
    SYSTEMTIME today;
    DWORD random = 0;
    int count = 1;
    BOOL scratch = s->groups[row->group].scratchOf >= 0;

    error[0] = 0;
    if (!row->transcript || !Core_IsSessionId(row->key)) {
        StringCchCopyW(error, errorCch, L"This session has no conversation on disk to copy.");
        return FALSE;
    }
    if (!NewSessionId(newId, idCch, &random)) {
        StringCchCopyW(error, errorCch, L"A new session id could not be made.");
        return FALSE;
    }
    StringCchCopyW(cwd, ARRAYSIZE(cwd), row->cwd);
    StringCchCopyW(dir, ARRAYSIZE(dir), row->transcriptPath);
    if ((slash = wcsrchr(dir, L'\\')) != NULL) *slash = 0;
    if (scratch) {
        if (!dest->scratchDir[0]) {
            StringCchPrintfW(error, errorCch, L"\x201C%s\x201D has no Claude Code sessions yet: open its Code tab once, then copy again.",
                             s->profiles.items[target].name);
            return FALSE;
        }
        GetSystemTime(&today);   /* Claude dates them in UTC */
        Core_ScratchName(&today, random, name, ARRAYSIZE(name));
        if (FAILED(StringCchPrintfW(cwd, ARRAYSIZE(cwd), L"%s\\%s", dest->scratchDir, name)) ||
            !Util_EnsureDir(dest->scratchDir) || !CopyFolder(row->cwd, cwd)) {
            StringCchCopyW(error, errorCch, L"Its working folder could not be copied.");
            return FALSE;
        }
        /* Filed where Claude Code keeps that folder's sessions (a name too
         * long for that stays with the original: Claude Code finds it by id). */
        if (Core_ProjectDirName(cwd, name, ARRAYSIZE(name)) && SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) &&
            SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", projects, name)))
            CreateDirectoryW(dir, NULL);
    }
    if (FAILED(StringCchPrintfW(to, ARRAYSIZE(to), L"%s\\%s.jsonl", dir, newId)) ||
        FAILED(StringCchPrintfA(fromId, sizeof fromId, "\"sessionId\":\"%ls\"", row->key)) ||
        FAILED(StringCchPrintfA(toId, sizeof toId, "\"sessionId\":\"%ls\"", newId))) {
        StringCchCopyW(error, errorCch, L"The path is too long.");
        return FALSE;
    }
    swaps[0].from = fromId;
    swaps[0].to = toId;
    if (scratch && Core_JsonQuote(row->cwd, q1, sizeof q1) && Core_JsonQuote(cwd, q2, sizeof q2) &&
        SUCCEEDED(StringCchPrintfA(fromCwd, sizeof fromCwd, "\"cwd\":%s", q1)) &&
        SUCCEEDED(StringCchPrintfA(toCwd, sizeof toCwd, "\"cwd\":%s", q2))) {
        swaps[1].from = fromCwd;
        swaps[1].to = toCwd;
        count = 2;
    }
    if (!CopyTranscript(row->transcriptPath, to, swaps, count)) {
        StringCchCopyW(error, errorCch, L"The conversation could not be copied.");
        return FALSE;
    }
    Util_Log(L"session %s copied as %s for %s", row->key, newId, s->profiles.items[target].folder);
    return TRUE;
}

/* ----------------------------------------------------------- delete */

/* Every entry of the session and its conversation (each transcript of that
 * id, with the folder of its subagents) to the Recycle Bin. Only when no
 * profile that lists it runs: a running Claude would write it back. */
RemoveResult SessionEdit_DeleteEverywhere(HWND owner, const SessionSet *s, int r, WCHAR *error, size_t errorCch)
{
    const SessionRow *row = &s->rows[r];
    WCHAR projects[MAX_PATH], pattern[MAX_PATH], (*paths)[MAX_PATH];
    const WCHAR **list;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    RemoveResult result = REMOVE_FAILED;
    int p, n = 0, max = MAX_PROFILES + 64;

    error[0] = 0;
    for (p = 0; p < s->profiles.count; p++) {
        if (row->entry[p] >= 0 && Claude_IsRunning(&s->profiles.items[p])) {
            StringCchPrintfW(error, errorCch, L"Close \x201C%s\x201D first: while it runs, Claude keeps this session and would write it back.",
                             s->profiles.items[p].name);
            return REMOVE_FAILED;
        }
    }
    paths = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, (size_t)max * sizeof *paths);
    list = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)max * sizeof *list);
    if (!paths || !list) goto done;
    for (p = 0; p < s->profiles.count; p++)
        if (row->entry[p] >= 0) StringCchCopyW(paths[n++], MAX_PATH, s->entries[row->entry[p]].file);
    if (Core_IsSessionId(row->key) && SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) &&
        SUCCEEDED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", projects)) &&
        (h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchLimitToDirectories, NULL, 0)) != INVALID_HANDLE_VALUE) {
        do {
            WCHAR file[MAX_PATH], sub[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || n + 2 > max) continue;
            if (SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\%s\\%s.jsonl", projects, fd.cFileName, row->key)) &&
                Util_FileExists(file))
                StringCchCopyW(paths[n++], MAX_PATH, file);
            if (SUCCEEDED(StringCchPrintfW(sub, ARRAYSIZE(sub), L"%s\\%s\\%s", projects, fd.cFileName, row->key)) && Util_DirExists(sub))
                StringCchCopyW(paths[n++], MAX_PATH, sub);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (p = 0; p < n; p++) list[p] = paths[p];
    result = Util_Recycle(owner, list, n);
    Util_Log(L"session %s deleted everywhere (%d item(s)): %s", row->key, n,
             result == REMOVE_DONE ? L"done" : result == REMOVE_CANCELLED ? L"cancelled" : L"FAILED");
    if (result == REMOVE_FAILED) StringCchCopyW(error, errorCch, L"Some of its files could not be moved to the Recycle Bin.");
done:
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (list) HeapFree(GetProcessHeap(), 0, (void *)list);
    return result;
}
