/*
 * Sessions on private profiles, transcripts and Claude Code folders in a
 * temporary fixture: where a profile's sessions are stored and how they are
 * read (sessionstore.c); copies, changes waiting for a running profile,
 * removals and Delete session everywhere, with a stand-in for the Recycle
 * Bin (sessionedit.c); paths past MAX_PATH; reads cancelled or made at once;
 * the sessions view's background snapshots (sessions.c). Built and run by
 * build.cmd; exits non-zero when a check fails.
 */
#include "../src/app.h"
#include "../src/resource.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define TRANSCRIPT_READ_MAX (1024u * 1024u)

static int g_checks, g_failures;
static WCHAR g_root[MAX_PATH];
static DWORD g_fixtureTag;
static const WCHAR *const g_family = L"Fixture_package";
static const WCHAR *const g_scratchId = L"11111111-1111-4111-8111-111111111111";
static const WCHAR *const g_projectId = L"22222222-2222-4222-8222-222222222222";
static const WCHAR *const g_oldId = L"33333333-3333-4333-8333-333333333333";
static const WCHAR *const g_lineageId = L"aaaaaaaa-0000-4000-8000-000000000001";
static const WCHAR *const g_priorId = L"aaaaaaaa-0000-4000-8000-000000000002";
static const WCHAR *const g_preClearId = L"aaaaaaaa-0000-4000-8000-000000000003";
static const WCHAR *const g_claimedId = L"aaaaaaaa-0000-4000-8000-000000000004";
static const WCHAR *const g_duplicateId = L"aaaaaaaa-0000-4000-8000-000000000005";
static const WCHAR *const g_longId = L"bbbbbbbb-0000-4000-8000-000000000001";
static const WCHAR *const g_editIds[3] = {
    L"cccccccc-0000-4000-8000-000000000001", L"cccccccc-0000-4000-8000-000000000002", L"cccccccc-0000-4000-8000-000000000003"
};
static const WCHAR *const g_unlistedId = L"dddddddd-0000-4000-8000-000000000001";
static const WCHAR *const g_accountId = L"eeeeeeee-0000-4000-8000-000000000001";
static const WCHAR *const g_organizationId = L"eeeeeeee-0000-4000-8000-000000000002";
static const WCHAR *const g_deletedId = L"ffffffff-0000-4000-8000-000000000001";
static const WCHAR *const g_startedId = L"ffffffff-0000-4000-8000-000000000002";
static const WCHAR *const g_neighborId = L"ffffffff-0000-4000-8000-000000000003";

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

static BOOL Join(const WCHAR *dir, const WCHAR *leaf, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", dir, leaf));
}

static BOOL FixturePath(const WCHAR *leaf, WCHAR *out, size_t cch)
{
    return Join(g_root, leaf, out, cch);
}

/* Inside the fixture root, with no way out of it. */
static BOOL InFixture(const WCHAR *path)
{
    return Core_PathEquals(path, g_root) || (Core_PathUnder(path, g_root) && !wcsstr(path, L"\\..") && !wcschr(path, L'/'));
}

/* There as a folder (or, without `folder`, as a file), at any path length. */
static BOOL PathThere(const WCHAR *path, BOOL folder)
{
    WCHAR extended[LONG_PATH_CCH];
    DWORD attributes;
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    attributes = GetFileAttributesW(extended);
    return attributes != INVALID_FILE_ATTRIBUTES && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == folder;
}

static BOOL FileThere(const WCHAR *path)
{
    return PathThere(path, FALSE);
}

static BOOL DirThere(const WCHAR *path)
{
    return PathThere(path, TRUE);
}

/* Every missing folder of `path`, inside the fixture root, at any length. */
static BOOL MakeDir(const WCHAR *path)
{
    WCHAR partial[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    size_t length, i;
    if (!InFixture(path) || FAILED(StringCchCopyW(partial, ARRAYSIZE(partial), path))) return FALSE;
    length = wcslen(partial);
    for (i = wcslen(g_root); i <= length; i++) {
        if (i < length && partial[i] != L'\\') continue;
        partial[i] = 0;
        if (!Util_ExtendedPath(partial, extended, ARRAYSIZE(extended)) ||
            (!CreateDirectoryW(extended, NULL) && GetLastError() != ERROR_ALREADY_EXISTS))
            return FALSE;
        if (i < length) partial[i] = L'\\';
    }
    return DirThere(path);
}

static BOOL Save(const WCHAR *path, const char *text)
{
    WCHAR parent[LONG_PATH_CCH], extended[LONG_PATH_CCH], *slash;
    HANDLE file;
    DWORD bytes = (DWORD)strlen(text), written = 0;
    BOOL ok;
    if (!InFixture(path) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path))) return FALSE;
    slash = wcsrchr(parent, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    if (!MakeDir(parent) || !Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    file = CreateFileW(extended, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, text, bytes, &written, NULL) && written == bytes;
    CloseHandle(file);
    return ok;
}

static BOOL SaveIn(const WCHAR *dir, const WCHAR *name, const char *text)
{
    WCHAR path[LONG_PATH_CCH];
    return Join(dir, name, path, ARRAYSIZE(path)) && Save(path, text);
}

/* Member `key` of the JSON file `path`, as Core_JsonMember gives it, to `check`. */
static BOOL CheckMember(const WCHAR *path, const char *key, BOOL (*check)(const char *value, size_t length, void *context), void *context)
{
    DWORD length;
    size_t valueLength;
    const char *value;
    char *json = Util_ReadFile(path, 65536, FALSE, &length);
    BOOL ok = FALSE;
    if (json) {
        ok = Core_JsonMember(json, length, key, &value, &valueLength) && check(value, valueLength, context);
        HeapFree(GetProcessHeap(), 0, json);
    }
    return ok;
}

typedef struct StringOut {
    WCHAR *text;
    size_t cch;
} StringOut;

static BOOL DecodeString(const char *value, size_t length, void *context)
{
    StringOut *out = (StringOut *)context;
    return Core_JsonString(value, length, out->text, out->cch);
}

static BOOL IsTrue(const char *value, size_t length, void *context)
{
    (void)context;
    return Core_JsonTrue(value, length);
}

static BOOL IsAny(const char *value, size_t length, void *context)
{
    (void)value;
    (void)length;
    (void)context;
    return TRUE;
}

static BOOL ReadString(const WCHAR *path, const char *key, WCHAR *out, size_t cch)
{
    StringOut string;
    string.text = out;
    string.cch = cch;
    out[0] = 0;
    return CheckMember(path, key, DecodeString, &string);
}

static BOOL ReadTrue(const WCHAR *path, const char *key)
{
    return CheckMember(path, key, IsTrue, NULL);
}

static BOOL HasMember(const WCHAR *path, const char *key)
{
    return CheckMember(path, key, IsAny, NULL);
}

/* Refuse reparse points and paths outside the unique fixture root. */
static BOOL RemoveFixture(const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH], child[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    DWORD attributes;
    HANDLE find;
    BOOL ok = TRUE;
    if (!InFixture(path) || !Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    attributes = GetFileAttributesW(extended);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(extended, attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(extended);
    find = Util_FindFiles(path, L"*", &found, FALSE);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if (!Join(path, found.cFileName, child, ARRAYSIZE(child)) || !RemoveFixture(child)) ok = FALSE;
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return ok && RemoveDirectoryW(extended);
}

/* In the temporary folder, by its long names: Claude Code's temporary
 * folder is found by them (the temporary folder can come in 8.3 names). */
static BOOL CreateRoot(void)
{
    WCHAR temp[MAX_PATH], path[MAX_PATH], full[MAX_PATH];
    GUID id;
    DWORD n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp) || FAILED(CoCreateGuid(&id)) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%scdm-session-%08lx-%08lx", temp,
                                GetCurrentProcessId(), id.Data1))) return FALSE;
    n = GetFullPathNameW(path, ARRAYSIZE(full), full, NULL);
    g_fixtureTag = id.Data1;
    if (n == 0 || n >= ARRAYSIZE(full) || !CreateDirectoryW(full, NULL)) return FALSE;
    n = GetLongPathNameW(full, g_root, ARRAYSIZE(g_root));
    return n > 0 && n < ARRAYSIZE(g_root);
}

/* A fake Claude of `profile`: the window its running Claude has. */
static HWND StartFakeClaude(const Profile *profile)
{
    WNDCLASSW cls;
    ZeroMemory(&cls, sizeof cls);
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"Chrome_MessageWindow";
    RegisterClassW(&cls);
    return CreateWindowExW(0, cls.lpszClassName, profile->dataDir, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, cls.hInstance, NULL);
}

static void StopFakeClaude(HWND window)
{
    if (!window) return;
    DestroyWindow(window);
    UnregisterClassW(L"Chrome_MessageWindow", GetModuleHandleW(NULL));
}

/* -------------------------------------------- sessionedit.c, in the fixture */

#define RECYCLED_MAX 16

/* What the Recycle Bin is asked to take: never the real one. Every path must
 * be in the fixture; those taken (`result` REMOVE_DONE) are deleted, as the
 * bin would move them away. With `startsClaude`, that profile's Claude
 * starts while Windows asks. */
static struct {
    RemoveResult  result;
    const Profile *startsClaude;
    HWND          started;
    BOOL          escaped;
    int           calls, count;
    WCHAR         paths[RECYCLED_MAX][LONG_PATH_CCH];
} g_recycle;

static void StopStartedClaude(void)
{
    StopFakeClaude(g_recycle.started);
    g_recycle.started = NULL;
    g_recycle.startsClaude = NULL;
}

/* What the next calls do; whether a path ever escaped the fixture is kept. */
static void RecycleWill(RemoveResult result, const Profile *startsClaude)
{
    BOOL escaped = g_recycle.escaped;
    StopStartedClaude();
    ZeroMemory(&g_recycle, sizeof g_recycle);
    g_recycle.escaped = escaped;
    g_recycle.result = result;
    g_recycle.startsClaude = startsClaude;
}

static RemoveResult FixtureRecycle(HWND owner, const WCHAR *const *paths, int count)
{
    BOOL escaped = FALSE;
    int i;
    (void)owner;
    g_recycle.calls++;
    for (i = 0; i < count; i++) {
        if (!InFixture(paths[i])) escaped = TRUE;
        if (g_recycle.count < RECYCLED_MAX) StringCchCopyW(g_recycle.paths[g_recycle.count++], LONG_PATH_CCH, paths[i]);
    }
    if (escaped) {
        g_recycle.escaped = TRUE;
        return REMOVE_FAILED;
    }
    for (i = 0; i < count; i++)
        if (!Util_FitsRecycleBin(paths[i])) return REMOVE_FAILED;
    if (g_recycle.startsClaude && !g_recycle.started) g_recycle.started = StartFakeClaude(g_recycle.startsClaude);
    if (g_recycle.result == REMOVE_DONE)
        for (i = 0; i < count; i++)
            if (!RemoveFixture(paths[i])) return REMOVE_FAILED;
    return g_recycle.result;
}

static BOOL Recycled(const WCHAR *path)
{
    int i;
    for (i = 0; i < g_recycle.count; i++)
        if (Core_PathEquals(g_recycle.paths[i], path)) return TRUE;
    return FALSE;
}

/* With `g_cancelCopies`, a folder copy is cancelled in Windows' progress
 * once it has copied everything. */
static BOOL g_cancelCopies;

static int FixtureFileOperation(LPSHFILEOPSTRUCTW operation)
{
    int code = SHFileOperationW(operation);
    if (!g_cancelCopies || operation->wFunc != FO_COPY || code != 0) return code;
    operation->fAnyOperationsAborted = TRUE;
    return ERROR_CANCELLED;
}

#define SessionEdit_Open                 TestedSessionEdit_Open
#define SessionEdit_Change               TestedSessionEdit_Change
#define SessionEdit_Cancel               TestedSessionEdit_Cancel
#define SessionEdit_Forget               TestedSessionEdit_Forget
#define SessionEdit_ApplyPending         TestedSessionEdit_ApplyPending
#define SessionEdit_ApplyPendingAfterRun TestedSessionEdit_ApplyPendingAfterRun
#define SessionEdit_ApplyPendingFor      TestedSessionEdit_ApplyPendingFor
#define SessionEdit_CopyConversation     TestedSessionEdit_CopyConversation
#define SessionEdit_RemoveCopy           TestedSessionEdit_RemoveCopy
#define SessionEdit_CanDelete            TestedSessionEdit_CanDelete
#define SessionEdit_RemovesWorkingFolder TestedSessionEdit_RemovesWorkingFolder
#define SessionEdit_ListFiles            TestedSessionEdit_ListFiles
#define SessionEdit_DeleteEverywhere     TestedSessionEdit_DeleteEverywhere
#define Util_Recycle                     FixtureRecycle
#define SHFileOperationW                 FixtureFileOperation
#include "../src/sessionedit.c"
#undef Util_Recycle
#undef SHFileOperationW

/* ------------------------------------------------------------------ fixtures */

static BOOL SetProfile(Profile *p, const WCHAR *label, const WCHAR *leaf, BOOL stock)
{
    ZeroMemory(p, sizeof *p);
    p->isStock = stock;
    return SUCCEEDED(StringCchPrintfW(p->folder, ARRAYSIZE(p->folder), L"Fixture-%08lx-%08lx-%s", GetCurrentProcessId(), g_fixtureTag, label)) &&
           SUCCEEDED(StringCchCopyW(p->name, ARRAYSIZE(p->name), label)) &&
           FixturePath(leaf, p->dataDir, ARRAYSIZE(p->dataDir));
}

/* A profile of its own signed in to `account`, whose entries go in `entries`. */
static BOOL PrepareProfile(Profile *p, const WCHAR *label, const WCHAR *leaf, const WCHAR *account, const WCHAR *organization,
                           WCHAR *entries, size_t cch)
{
    char config[256];
    WCHAR sub[MAX_PATH];
    return SetProfile(p, label, leaf, FALSE) && Profiles_ResolveStorage(p, NULL, NULL) &&
           SUCCEEDED(StringCchPrintfA(config, sizeof config, "{\"lastKnownAccountUuid\":\"%ls\"}", account)) &&
           SaveIn(p->storageDir, L"config.json", config) &&
           SUCCEEDED(StringCchPrintfW(sub, ARRAYSIZE(sub), L"claude-code-sessions\\%s\\%s", account, organization)) &&
           Join(p->storageDir, sub, entries, cch) && MakeDir(entries);
}

/* An entry of session `id` (its own id local_<id>) working in `cwd`, with
 * `extra` JSON members. */
static BOOL WriteEntryIn(const WCHAR *entries, const WCHAR *id, const WCHAR *cwd, const char *extra)
{
    WCHAR file[LONG_PATH_CCH];
    char quoted[MAX_PATH * 6 + 4], json[8192];
    return Core_JsonQuote(cwd, quoted, sizeof quoted) &&
           SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,"
                                                         "\"title\":\"Fixture session\",\"lastActivityAt\":123%s}", id, id, quoted, extra)) &&
           SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) && Save(file, json);
}

static BOOL WriteEntry(const WCHAR *entries, const WCHAR *id, const char *extra)
{
    return WriteEntryIn(entries, id, L"C:\\Fixture", extra);
}

static BOOL EntryPath(const WCHAR *entries, const WCHAR *id, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\local_%s.json", entries, id));
}

/* Claude Code's private folder of the fixture: the one holding `projects`. */
static BOOL CodeDir(const WCHAR *projects, WCHAR *out, size_t cch)
{
    WCHAR *slash;
    if (FAILED(StringCchCopyW(out, cch, projects)) || (slash = wcsrchr(out, L'\\')) == NULL) return FALSE;
    *slash = 0;
    return TRUE;
}

/* A session's entry, and its transcript as Claude Code writes it: one line
 * per message (some without a session or a folder), the working folder
 * becoming a subfolder after a cd, and a last line still being written. */
static BOOL WriteSession(const WCHAR *entries, const WCHAR *projects, const WCHAR *id, const WCHAR *cwd)
{
    WCHAR file[LONG_PATH_CCH], subfolder[MAX_PATH];
    char quoted[MAX_PATH * 6 + 4], quotedSubfolder[MAX_PATH * 6 + 4], json[8192];
    if (!Core_JsonQuote(cwd, quoted, sizeof quoted) || !Join(cwd, L"sub", subfolder, ARRAYSIZE(subfolder)) ||
        !Core_JsonQuote(subfolder, quotedSubfolder, sizeof quotedSubfolder) ||
        FAILED(StringCchPrintfA(json, sizeof json,
            "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,"
            "\"title\":\"Fixture session\",\"lastActivityAt\":123}\n", id, id, quoted)) ||
        FAILED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) || !Save(file, json)) return FALSE;
    return SUCCEEDED(StringCchPrintfA(json, sizeof json,
               "{\"sessionId\":\"%ls\",\"cwd\":%s,\"type\":\"user\"}\n"
               "{\"type\":\"summary\",\"summary\":\"Fixture\"}\n"
               "{\"parentUuid\":null,\"cwd\":%s,\"sessionId\":\"%ls\",\"type\":\"assistant\"}\n"
               "{\"cwd\":%s,\"sessionId\":\"%ls\",\"type\":\"user\"}\n"
               "{\"sessionId\":\"%ls\",\"cwd\":%s,\"partial\":",
               id, quoted, quoted, id, quotedSubfolder, id, id, quoted)) &&
           SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\fixture\\%s.jsonl", projects, id)) && Save(file, json);
}

static int CountText(const char *text, const char *part)
{
    int count = 0;
    size_t length = strlen(part);
    for (; (text = strstr(text, part)) != NULL; text += length) count++;
    return count;
}

/* The copy at `path` of a WriteSession transcript: each of its whole lines
 * with the new id and the working folder `copyCwd` (its subfolder too), and
 * not the line that was still being written. */
static void CheckCopiedLines(const char *what, const WCHAR *path, const WCHAR *oldId, const WCHAR *newId,
                             const WCHAR *oldCwd, const WCHAR *copyCwd)
{
    char label[256], member[128], quoted[MAX_PATH * 6 + 4], cwdMember[MAX_PATH * 6 + 16], *text;
    DWORD length = 0;
    BOOL moved = !Core_PathEquals(oldCwd, copyCwd);
    text = Util_ReadFile(path, TRANSCRIPT_READ_MAX, FALSE, &length);
    StringCchPrintfA(label, sizeof label, "%s: the copied transcript can be read", what);
    Check(label, text != NULL);
    if (!text) return;
    StringCchPrintfA(member, sizeof member, "\"sessionId\":\"%ls\"", oldId);
    StringCchPrintfA(label, sizeof label, "%s: no line keeps the original's id", what);
    Check(label, CountText(text, member) == 0);
    StringCchPrintfA(member, sizeof member, "\"sessionId\":\"%ls\"", newId);
    StringCchPrintfA(label, sizeof label, "%s: every whole line has the copy's id", what);
    Check(label, CountText(text, member) == 3);
    StringCchPrintfA(label, sizeof label, "%s: the copy ends after the last whole line", what);
    Check(label, length > 0 && text[length - 1] == '\n' && !strstr(text, "\"partial\""));
    Core_JsonQuote(copyCwd, quoted, sizeof quoted);
    StringCchPrintfA(cwdMember, sizeof cwdMember, "\"cwd\":%s", quoted);
    StringCchPrintfA(label, sizeof label, "%s: every line in the working folder names the copy's", what);
    Check(label, CountText(text, cwdMember) == 2);
    StringCchPrintfA(cwdMember, sizeof cwdMember, "\"cwd\":%.*s\\\\sub\"", (int)strlen(quoted) - 1, quoted);
    StringCchPrintfA(label, sizeof label, "%s: a line in a subfolder names the copy's subfolder", what);
    Check(label, CountText(text, cwdMember) == 1);
    if (moved) {
        Core_JsonQuote(oldCwd, quoted, sizeof quoted);
        StringCchPrintfA(cwdMember, sizeof cwdMember, "\"cwd\":%.*s", (int)strlen(quoted) - 1, quoted);
        StringCchPrintfA(label, sizeof label, "%s: no line keeps the original's working folder", what);
        Check(label, CountText(text, cwdMember) == 0);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

static BOOL PrepareProfiles(ProfileList *profiles, WCHAR *local, WCHAR *projects)
{
    WCHAR physical[MAX_PATH], stockEntries[MAX_PATH], oldEntries[MAX_PATH], secondaryEntries[MAX_PATH];
    WCHAR scratch[MAX_PATH], physicalScratch[MAX_PATH], project[MAX_PATH], code[MAX_PATH];
    Profile *stock = &profiles->items[0], *secondary = &profiles->items[1];
    ZeroMemory(profiles, sizeof *profiles);
    profiles->count = 2;
    if (!SetProfile(stock, L"Stock", L"R\\Claude", TRUE) || !SetProfile(secondary, L"Secondary", L"R\\Claude-Secondary", FALSE) ||
        !FixturePath(L"L", local, MAX_PATH) || !FixturePath(L"code", code, ARRAYSIZE(code)) ||
        !Join(code, L"projects", projects, MAX_PATH) || !MakeDir(secondary->dataDir) ||
        FAILED(StringCchPrintfW(physical, ARRAYSIZE(physical), L"%s\\Packages\\%s\\LocalCache\\Roaming\\Claude", local, g_family)) ||
        !MakeDir(physical)) return FALSE;
    Check("stock absent: virtual storage resolves", Profiles_ResolveStorage(stock, local, g_family));
    Check("stock absent: storage is LocalCache", Core_PathEquals(stock->storageDir, physical));
    Check("stock absent: logical path is preserved", Core_EndsWithI(stock->dataDir, L"\\R\\Claude"));
    Check("stock absent: resolution does not create logical directory", !Util_DirExists(stock->dataDir));
    Check("secondary: storage resolves", Profiles_ResolveStorage(secondary, local, g_family));
    Check("secondary: storage is its real profile directory", Core_PathEquals(secondary->storageDir, secondary->dataDir));
    if (!Core_PathEquals(stock->storageDir, physical) || !Core_PathEquals(secondary->storageDir, secondary->dataDir)) return FALSE;
    if (!Join(physical, L"claude-code-sessions\\account-a\\org-a", stockEntries, ARRAYSIZE(stockEntries)) ||
        !Join(physical, L"claude-code-sessions\\account-old\\org-old", oldEntries, ARRAYSIZE(oldEntries)) ||
        !Join(secondary->storageDir, L"claude-code-sessions\\account-b\\org-b", secondaryEntries, ARRAYSIZE(secondaryEntries)) ||
        !MakeDir(secondaryEntries) ||
        !Join(stock->dataDir, L"scratch-workspaces\\account-a\\org-a\\scratch-original", scratch, ARRAYSIZE(scratch)) ||
        !Join(physical, L"scratch-workspaces\\account-a\\org-a\\scratch-original", physicalScratch, ARRAYSIZE(physicalScratch)) ||
        !FixturePath(L"workspace", project, ARRAYSIZE(project)) || !MakeDir(project) ||
        !SaveIn(physicalScratch, L"sentinel.txt", "scratch fixture\n") ||
        !SaveIn(physical, L"config.json", "{\"lastKnownAccountUuid\":\"account-a\",\"windowSizeWasSignedIn\":true,"
            "\"locale\":\"en-US\",\"userThemeMode\":\"dark\",\"accessToken\":\"must-not-copy\"}") ||
        !SaveIn(physical, L"claude_desktop_config.json", "{\"mcpServers\":{\"fixture\":{\"command\":\"fixture-command\"}},"
            "\"isHardwareAccelerationDisabled\":true,\"preferences\":{\"menuBarEnabled\":false,\"privateValue\":\"must-not-copy\"},"
            "\"credentials\":\"must-not-copy\"}") ||
        !SaveIn(secondary->storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"account-b\"}") ||
        !WriteSession(stockEntries, projects, g_scratchId, scratch) || !WriteSession(stockEntries, projects, g_projectId, project) ||
        !WriteSession(oldEntries, projects, g_oldId, project)) return FALSE;
    return SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", code);
}

static void TestResolution(const WCHAR *local)
{
    Profile p;
    WCHAR expected[MAX_PATH], mapped[MAX_PATH], ancestor[MAX_PATH], watched[MAX_PATH];
    Check("real stock fixture", SetProfile(&p, L"Real", L"real\\Claude", TRUE) && MakeDir(p.dataDir));
    Check("real stock: storage resolves", Profiles_ResolveStorage(&p, local, g_family));
    Check("real stock: real directory wins over LocalCache", Core_PathEquals(p.storageDir, p.dataDir));
    Check("missing secondary fixture", SetProfile(&p, L"Missing", L"R\\Claude-Missing", FALSE));
    Check("missing secondary: storage resolves", Profiles_ResolveStorage(&p, local, g_family));
    Check("missing secondary: no stock fallback", Core_PathEquals(p.storageDir, p.dataDir));
    Check("missing secondary: no directory created", !Util_DirExists(p.dataDir));
    Check("path mapping: boundary fixture", Join(p.dataDir, L"config.json", expected, ARRAYSIZE(expected)));
    Check("secondary: file path unchanged", Core_ProfileFilePath(&p, expected, mapped, ARRAYSIZE(mapped)) &&
          Core_PathEquals(mapped, expected));
    Check("watch fixture", SetProfile(&p, L"Watch", L"R\\Claude-Watch", FALSE) &&
          Profiles_ResolveStorage(&p, local, g_family) && FixturePath(L"R", ancestor, ARRAYSIZE(ancestor)));
    Check("watch: missing storage uses existing ancestor", SessionStore_WatchDir(&p, watched, ARRAYSIZE(watched)) &&
          Core_PathEquals(watched, ancestor));
    Check("watch: lookup creates no profile directory", !Util_DirExists(p.dataDir));
    Check("watch: storage fixture created", MakeDir(p.storageDir));
    Check("watch: existing storage is watched", SessionStore_WatchDir(&p, watched, ARRAYSIZE(watched)) &&
          Core_PathEquals(watched, p.storageDir));
    Check("watch: sessions fixture created", Join(p.storageDir, L"claude-code-sessions", expected, ARRAYSIZE(expected)) && MakeDir(expected));
    Check("watch: existing sessions directory is watched", SessionStore_WatchDir(&p, watched, ARRAYSIZE(watched)) &&
          Core_PathEquals(watched, expected));
}

static int FindRow(const SessionSet *s, const WCHAR *id)
{
    int r;
    for (r = 0; r < s->rowCount; r++)
        if (wcscmp(s->rows[r].key, id) == 0) return r;
    return -1;
}

static int ProfileRows(const SessionSet *s, int profile)
{
    int r, n = 0;
    for (r = 0; r < s->rowCount; r++)
        if (s->rows[r].entry[profile] >= 0) n++;
    return n;
}

static BOOL FindCopiedTranscript(const WCHAR *projects, const WCHAR *id, WCHAR *out, size_t cch)
{
    WIN32_FIND_DATAW project;
    HANDLE find;
    BOOL found = FALSE;
    find = Util_FindFiles(projects, L"*", &project, TRUE);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(project.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || project.cFileName[0] == L'.') continue;
        if (SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s\\%s.jsonl", projects, project.cFileName, id)) && FileThere(out)) {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(find, &project));
    FindClose(find);
    return found;
}

/* Whether the paths of a deletion list hold `path`. */
static BOOL Listed(WCHAR (*paths)[LONG_PATH_CCH], int count, const WCHAR *path)
{
    int i;
    for (i = 0; i < count; i++)
        if (Core_PathEquals(paths[i], path)) return TRUE;
    return FALSE;
}

static BOOL CheckCopy(SessionSet *s, int row, int target, const WCHAR *projects, WCHAR *id, WCHAR *path, WCHAR *cwd)
{
    WCHAR error[512], physical[MAX_PATH], marker[MAX_PATH];
    WCHAR projectName[CORE_PROJECT_NAME_MAX + 1], expected[LONG_PATH_CCH];
    BOOL ok = SessionEdit_CopyConversation(NULL, s, row, target, id, SESSION_ID_CCH, error, ARRAYSIZE(error)) == COPY_MADE;
    Check("scratch copy: completes", ok);
    if (!ok) {
        wprintf(L"        %s\n", error);
        return FALSE;
    }
    Check("scratch copy: distinct conversation id", Core_IsUuid(id) && wcscmp(id, s->rows[row].key) != 0);
    ok = FindCopiedTranscript(projects, id, path, LONG_PATH_CCH);
    Check("scratch copy: transcript exists", ok);
    if (!ok) return FALSE;
    ok = ReadString(path, "cwd", cwd, MAX_PATH);
    Check("scratch copy: transcript has working directory", ok);
    if (!ok) return FALSE;
    Check("scratch copy: transcript uses target logical path", Core_PathUnder(cwd, s->source[target].scratchDir));
    CheckCopiedLines("scratch copy", path, s->rows[row].key, id, s->rows[row].cwd, cwd);
    if (Core_ProjectDirName(cwd, projectName, ARRAYSIZE(projectName)) &&
        SUCCEEDED(StringCchPrintfW(expected, ARRAYSIZE(expected), L"%s\\%s\\%s.jsonl", projects, projectName, id))) {
        Check("scratch copy: transcript filed in its working folder's project folder", Core_PathEquals(expected, path));
    } else {
        WCHAR *slash;
        StringCchCopyW(expected, ARRAYSIZE(expected), s->rows[row].transcriptPath);
        if ((slash = wcsrchr(expected, L'\\')) != NULL) *slash = 0;
        Check("scratch copy: transcript too long for its project folder stays beside the original",
              Core_PathUnder(path, expected) && wcschr(path + wcslen(expected) + 1, L'\\') == NULL);
    }
    Check("scratch copy: physical working directory resolves", SessionStore_WorkingDir(s, cwd, physical, ARRAYSIZE(physical)));
    Check("scratch copy: scratch content exists in target storage", Join(physical, L"sentinel.txt", marker, ARRAYSIZE(marker)) &&
          Util_FileExists(marker));
    Check("scratch copy: absent stock logical directory stays absent", !Util_DirExists(s->profiles.items[0].dataDir));
    return TRUE;
}

static void TestSessions(const ProfileList *profiles, const WCHAR *projects)
{
    SessionSet s;
    WCHAR logical[MAX_PATH], physical[MAX_PATH], expected[MAX_PATH], outside[MAX_PATH];
    WCHAR id[SESSION_ID_CCH], path[LONG_PATH_CCH], cwd[MAX_PATH], error[512];
    int r, i;
    BOOL loaded = SessionStore_LoadProfiles(&s, profiles);
    Check("session load succeeds", loaded);
    if (!loaded) return;
    Check("virtual stock: signed in", s.source[0].signedIn);
    Check("virtual stock: entries directory found", s.source[0].entriesDir[0] != 0);
    Check("virtual stock: exactly two active-account sessions", ProfileRows(&s, 0) == 2 && s.entryCount == 2);
    Check("virtual stock: old account ignored", FindRow(&s, g_oldId) < 0);
    Check("empty secondary: signed in", s.source[1].signedIn);
    Check("empty secondary: entries directory found", s.source[1].entriesDir[0] != 0);
    Check("empty secondary: zero sessions", ProfileRows(&s, 1) == 0);
    Check("virtual stock: entries directory uses physical storage", Core_PathUnder(s.source[0].entriesDir, profiles->items[0].storageDir));
    Check("virtual stock: scratch directory uses logical storage", Core_PathUnder(s.source[0].scratchDir, profiles->items[0].dataDir));
    for (i = 0; i < s.entryCount; i++)
        Check("virtual stock: entry file uses physical storage", Core_PathUnder(s.entries[i].file, profiles->items[0].storageDir));
    r = FindRow(&s, g_scratchId);
    Check("virtual stock: scratch session loaded", r >= 0);
    if (r >= 0) {
        SessionRow original = s.rows[r];
        int originalOwner = s.groups[original.group].scratchOf;
        Check("virtual stock: scratch session belongs to logical profile", originalOwner == 0);
        Check("virtual stock: scratch transcript found", original.transcript);
        Check("working directory: virtual storage", SessionStore_WorkingDir(&s, original.cwd, physical, ARRAYSIZE(physical)) &&
              Core_PathUnder(physical, profiles->items[0].storageDir) && Util_DirExists(physical));
        if (CheckCopy(&s, r, 1, projects, id, path, cwd)) {
            WCHAR storage[MAX_PATH];
            StringCchCopyW(s.rows[r].key, ARRAYSIZE(s.rows[r].key), id);
            StringCchCopyW(s.rows[r].transcriptPath, ARRAYSIZE(s.rows[r].transcriptPath), path);
            StringCchCopyW(s.rows[r].cwd, ARRAYSIZE(s.rows[r].cwd), cwd);
            s.groups[original.group].scratchOf = 1;
            CheckCopy(&s, r, 0, projects, id, path, cwd);
            StringCchCopyW(storage, ARRAYSIZE(storage), s.profiles.items[0].storageDir);
            s.profiles.items[0].storageDir[0] = 0;
            Check("unresolved target: scratch copy fails safely",
                  SessionEdit_CopyConversation(NULL, &s, r, 0, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_FAILED && error[0]);
            Check("unresolved target: logical stock stays absent", !Util_DirExists(s.profiles.items[0].dataDir));
            StringCchCopyW(s.profiles.items[0].storageDir, ARRAYSIZE(s.profiles.items[0].storageDir), storage);
            s.rows[r] = original;
            s.groups[original.group].scratchOf = originalOwner;
        }
    }
    r = FindRow(&s, g_projectId);
    if (r >= 0 && SessionEdit_CopyConversation(NULL, &s, r, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE) {
        WCHAR beside[LONG_PATH_CCH], *slash;
        StringCchCopyW(beside, ARRAYSIZE(beside), s.rows[r].transcriptPath);
        if ((slash = wcsrchr(beside, L'\\')) != NULL) slash[1] = 0;
        StringCchCatW(beside, ARRAYSIZE(beside), id);
        StringCchCatW(beside, ARRAYSIZE(beside), L".jsonl");
        Check("project copy: filed beside the original", FileThere(beside));
        CheckCopiedLines("project copy", beside, g_projectId, id, s.rows[r].cwd, s.rows[r].cwd);
    } else Check("project copy: completes", FALSE);
    Check("path mapping: fixture", Join(profiles->items[0].dataDir, L"config.json", logical, ARRAYSIZE(logical)) &&
          Join(profiles->items[0].storageDir, L"config.json", expected, ARRAYSIZE(expected)));
    Check("path mapping: profile file uses physical storage", Core_ProfileFilePath(&profiles->items[0], logical, physical, ARRAYSIZE(physical)) &&
          Core_PathEquals(physical, expected));
    Check("path mapping: external fixture", FixturePath(L"workspace", outside, ARRAYSIZE(outside)));
    Check("working directory: external path unchanged", SessionStore_WorkingDir(&s, outside, physical, ARRAYSIZE(physical)) &&
          Core_PathEquals(physical, outside));
    Check("path mapping: neighboring profile fixture", Join(profiles->items[1].dataDir, L"config.json", logical, ARRAYSIZE(logical)));
    Check("path mapping: prefix boundary preserved", Core_ProfileFilePath(&profiles->items[0], logical, physical, ARRAYSIZE(physical)) &&
          Core_PathEquals(physical, logical));
    Check("virtual stock: reading and copies never create logical stock", !Util_DirExists(profiles->items[0].dataDir));
    SessionStore_Free(&s);
}

/* A copy that could not be opened goes again, and only what that copy made;
 * what could not go is named. */
static void TestCopyRemoval(const ProfileList *profiles, const WCHAR *projects)
{
    SessionSet set;
    WCHAR id[SESSION_ID_CCH], error[512], path[LONG_PATH_CCH], folder[LONG_PATH_CCH], original[LONG_PATH_CCH];
    WCHAR cwd[MAX_PATH], physical[MAX_PATH], originalFiles[MAX_PATH], left[LONG_PATH_CCH], *slash;
    HANDLE held;
    int row;
    BOOL ready;
    if (!SessionStore_LoadProfiles(&set, profiles)) {
        Check("copy removal snapshot loads", FALSE);
        return;
    }
    row = FindRow(&set, g_scratchId);
    ready = row >= 0 && SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE &&
            FindCopiedTranscript(projects, id, path, ARRAYSIZE(path)) && ReadString(path, "cwd", cwd, ARRAYSIZE(cwd)) &&
            SessionStore_WorkingDir(&set, cwd, physical, ARRAYSIZE(physical)) && Util_DirExists(physical) &&
            SessionStore_WorkingDir(&set, set.rows[row].cwd, originalFiles, ARRAYSIZE(originalFiles));
    Check("copy removal: a scratch copy is made", ready);
    if (ready) {
        StringCchCopyW(folder, ARRAYSIZE(folder), path);
        if ((slash = wcsrchr(folder, L'\\')) != NULL) *slash = 0;
        StringCchCopyW(original, ARRAYSIZE(original), set.rows[row].transcriptPath);
        if ((slash = wcsrchr(original, L'\\')) != NULL) *slash = 0;
        Check("copy removal: another id removes nothing",
              !SessionEdit_RemoveCopy(&set, row, 1, g_oldId, left, ARRAYSIZE(left)) && !left[0] && FileThere(path));
        Check("copy removal: another target removes nothing",
              !SessionEdit_RemoveCopy(&set, row, 0, id, left, ARRAYSIZE(left)) && !left[0] && FileThere(path));
        Check("copy removal: the copy is removed", SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)) && !left[0]);
        Check("copy removal: its transcript is gone", !FileThere(path));
        Check("copy removal: its copied working folder is gone", !Util_DirExists(physical));
        Check("copy removal: a project folder made for it is gone, the original's stays",
              Core_PathEquals(folder, original) ? DirThere(folder) : !DirThere(folder));
        Check("copy removal: the original's working folder stays", Util_DirExists(originalFiles));
        Check("copy removal: removes once only", !SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)));
    }
    row = FindRow(&set, g_projectId);
    ready = row >= 0 && SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE &&
            FindCopiedTranscript(projects, id, path, ARRAYSIZE(path));
    Check("copy removal: a project copy is made", ready);
    if (ready) {
        Check("copy removal: the project copy is removed", SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)) && !FileThere(path));
        Check("copy removal: the original transcript and its folder stay", FileThere(set.rows[row].transcriptPath));
    }
    ready = row >= 0 && SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE &&
            FindCopiedTranscript(projects, id, path, ARRAYSIZE(path));
    held = ready ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    Check("copy removal: a copy held open by another program", held != INVALID_HANDLE_VALUE);
    if (held != INVALID_HANDLE_VALUE) {
        Check("copy removal: what could not be deleted is named",
              !SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)) && Core_PathEquals(left, path) && FileThere(path));
        CloseHandle(held);
        DeleteFileW(path);
    }
    SessionStore_Free(&set);
}

static void TestSettings(const Profile *stock, const WCHAR *local)
{
    Profile target;
    WCHAR config[MAX_PATH], desktop[MAX_PATH], value[64];
    char *json;
    DWORD len;
    BOOL ready = SetProfile(&target, L"Settings", L"R\\Claude-Settings", FALSE) && MakeDir(target.dataDir) &&
                 Profiles_ResolveStorage(&target, local, g_family) && Join(target.storageDir, L"config.json", config, ARRAYSIZE(config)) &&
                 Join(target.storageDir, L"claude_desktop_config.json", desktop, ARRAYSIZE(desktop));
    Check("settings: fixture ready", ready);
    if (!ready) return;
    Profiles_CopySettings(stock, &target);
    Check("settings: app settings copied from virtual storage", ReadString(config, "locale", value, ARRAYSIZE(value)) && wcscmp(value, L"en-US") == 0);
    Check("settings: theme copied", ReadString(config, "userThemeMode", value, ARRAYSIZE(value)) && wcscmp(value, L"dark") == 0);
    Check("settings: account omitted", !HasMember(config, "lastKnownAccountUuid"));
    Check("settings: sign-in state omitted", !HasMember(config, "windowSizeWasSignedIn"));
    Check("settings: token omitted", !HasMember(config, "accessToken"));
    Check("settings: MCP servers copied", HasMember(desktop, "mcpServers"));
    Check("settings: acceleration setting copied", HasMember(desktop, "isHardwareAccelerationDisabled"));
    Check("settings: credentials omitted", !HasMember(desktop, "credentials"));
    json = Util_ReadFile(desktop, 65536, FALSE, &len);
    Check("settings: only allowed preference copied", json && strstr(json, "menuBarEnabled") && !strstr(json, "privateValue"));
    if (json) HeapFree(GetProcessHeap(), 0, json);
    Check("settings: source logical directory stays absent", !Util_DirExists(stock->dataDir));
}

static BOOL SetModified(const WCHAR *path, ULONGLONG ticks)
{
    FILETIME modified;
    HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    modified.dwLowDateTime = (DWORD)ticks;
    modified.dwHighDateTime = (DWORD)(ticks >> 32);
    ok = SetFileTime(file, NULL, NULL, &modified);
    CloseHandle(file);
    return ok;
}

static void TestNewestAccount(const WCHAR *projects)
{
    ProfileList profiles;
    SessionSet set;
    WCHAR oldEntries[MAX_PATH], newEntries[MAX_PATH], oldFile[MAX_PATH], newFile[MAX_PATH], cwd[MAX_PATH];
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = SetProfile(&profiles.items[0], L"AccountFallback", L"accounts\\profile", FALSE) &&
            Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\account-a-old\\org-old", oldEntries, ARRAYSIZE(oldEntries)) &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\account-z-new\\org-new", newEntries, ARRAYSIZE(newEntries)) &&
            FixturePath(L"workspace", cwd, ARRAYSIZE(cwd)) &&
            WriteSession(oldEntries, projects, g_oldId, cwd) && WriteSession(newEntries, projects, g_projectId, cwd) &&
            SUCCEEDED(StringCchPrintfW(oldFile, ARRAYSIZE(oldFile), L"%s\\local_%s.json", oldEntries, g_oldId)) &&
            SUCCEEDED(StringCchPrintfW(newFile, ARRAYSIZE(newFile), L"%s\\local_%s.json", newEntries, g_projectId)) &&
            SetModified(oldFile, 132000000000000000ULL) && SetModified(newFile, 133000000000000000ULL);
    Check("account fallback fixture created", ready);
    if (!ready) return;
    Check("account fallback snapshot loads", SessionStore_LoadProfiles(&set, &profiles));
    Check("missing config selects the account whose nested entry is newest", Core_PathEquals(set.source[0].entriesDir, newEntries));
    Check("missing config excludes older account sessions", FindRow(&set, g_projectId) >= 0 && FindRow(&set, g_oldId) < 0);
    SessionStore_Free(&set);
}

static int CountFolders(const WCHAR *dir)
{
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(dir, L"*", &found, TRUE);
    int count = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0)
            count++;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}

/* A copy without the working folder it needs fails with a reason; one
 * cancelled in Windows' progress leaves nothing and no error. */
static void TestScratchCopyFailure(const ProfileList *profiles, const WCHAR *projects)
{
    SessionSet set;
    WCHAR id[SESSION_ID_CCH], error[512], missing[MAX_PATH], original[MAX_PATH], physical[MAX_PATH], marker[MAX_PATH];
    WCHAR area[MAX_PATH], copied[LONG_PATH_CCH];
    CopyResult result;
    int row, before;
    Check("scratch error snapshot loads", SessionStore_LoadProfiles(&set, profiles));
    row = FindRow(&set, g_scratchId);
    if (row >= 0 && FixturePath(L"missing-scratch", missing, ARRAYSIZE(missing))) {
        StringCchCopyW(original, ARRAYSIZE(original), set.rows[row].cwd);
        StringCchCopyW(set.rows[row].cwd, ARRAYSIZE(set.rows[row].cwd), missing);
        Check("missing scratch files do not produce a successful empty copy", SessionEdit_CopyConversation(NULL, &set, row, 1,
              id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_FAILED);
        Check("missing scratch copy has a reason", error[0] != 0);
        StringCchCopyW(set.rows[row].cwd, ARRAYSIZE(set.rows[row].cwd), original);
        if (Core_ProfileFilePath(&set.profiles.items[1], set.source[1].scratchDir, area, ARRAYSIZE(area))) {
            before = CountFolders(area);
            g_cancelCopies = TRUE;
            result = SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error));
            g_cancelCopies = FALSE;
            Check("a copy cancelled in Windows' progress is told apart from a failure", result == COPY_CANCELLED && !error[0]);
            Check("a cancelled copy leaves no working folder, not even in part", CountFolders(area) == before);
            Check("a cancelled copy leaves no transcript", !FindCopiedTranscript(projects, id, copied, ARRAYSIZE(copied)));
        } else Check("scratch area of the copy's profile", FALSE);
        if (SessionStore_WorkingDir(&set, original, physical, ARRAYSIZE(physical)) &&
            Join(physical, L"sentinel.txt", marker, ARRAYSIZE(marker)) && Core_PathUnder(marker, g_root) && DeleteFileW(marker)) {
            Check("empty existing scratch directory can be copied", SessionEdit_CopyConversation(NULL, &set, row, 1,
                  id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE);
        } else Check("empty scratch fixture ready", FALSE);
    } else Check("scratch error row found", FALSE);
    SessionStore_Free(&set);
}

static int QueueWorker(const WCHAR *root, const WCHAR *folder, const WCHAR *eventName, int worker)
{
    Profile profile;
    PendingEdit edit;
    HANDLE start;
    WCHAR state[MAX_PATH];
    BOOL waiting;
    int i;
    if (!Join(root, L"state", state, ARRAYSIZE(state))) return 1;
    Util_SetStateDir(state);
    ZeroMemory(&profile, sizeof profile);
    if (FAILED(StringCchCopyW(profile.folder, ARRAYSIZE(profile.folder), folder)) ||
        !Join(root, L"pending-profile", profile.dataDir, ARRAYSIZE(profile.dataDir))) return 1;
    start = OpenEventW(SYNCHRONIZE, FALSE, eventName);
    if (!start) return 1;
    if (WaitForSingleObject(start, 5000) != WAIT_OBJECT_0) { CloseHandle(start); return 1; }
    CloseHandle(start);
    for (i = 0; i < 64; i++) {
        ZeroMemory(&edit, sizeof edit);
        edit.op = PENDING_STAR;
        StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"%08lx-7777-4777-8777-%012lx", (DWORD)worker, (DWORD)i);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
        if (!SessionEdit_Change(NULL, &profile, NULL, &edit, &waiting, NULL, 0) || !waiting) return 1;
    }
    return 0;
}

static void TestPendingProcesses(void)
{
    Profile profile;
    PendingEdit *edits;
    WCHAR path[MAX_PATH], exe[MAX_PATH], command[MAX_PATH * 3], eventName[128];
    HANDLE start, process[2] = { NULL, NULL };
    BOOL ready;
    int worker, count, stars = 0;
    ready = SetProfile(&profile, L"Pending", L"pending-profile", FALSE) &&
            SessionStore_PendingPath(&profile, path, ARRAYSIZE(path)) && Util_SelfExe(exe, ARRAYSIZE(exe));
    Check("pending process fixture prepared", ready);
    if (!ready) return;
    StringCchPrintfW(eventName, ARRAYSIZE(eventName), L"Local\\SessionFixture.%08lx.%08lx", GetCurrentProcessId(), g_fixtureTag);
    start = CreateEventW(NULL, TRUE, FALSE, eventName);
    if (!start) { Check("pending process start event created", FALSE); return; }
    for (worker = 0; worker < 2; worker++) {
        STARTUPINFOW startup;
        PROCESS_INFORMATION child;
        ZeroMemory(&startup, sizeof startup);
        startup.cb = sizeof startup;
        ZeroMemory(&child, sizeof child);
        ready = SUCCEEDED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" --queue-worker \"%s\" \"%s\" \"%s\" %d",
                                           exe, g_root, profile.folder, eventName, worker)) &&
                CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child);
        Check("parallel queue worker started", ready);
        if (ready) { process[worker] = child.hProcess; CloseHandle(child.hThread); }
    }
    SetEvent(start);
    for (worker = 0; worker < 2; worker++) {
        if (process[worker]) {
            DWORD code = 1;
            ready = WaitForSingleObject(process[worker], 60000) == WAIT_OBJECT_0;
            Check("queue worker completes without write collisions", ready && GetExitCodeProcess(process[worker], &code) && code == 0);
            if (!ready) {
                TerminateProcess(process[worker], 1);
                WaitForSingleObject(process[worker], 5000);
            }
            CloseHandle(process[worker]);
        }
    }
    CloseHandle(start);
    edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    if (edits) {
        count = SessionStore_LoadPending(&profile, edits, SESSION_PENDING_MAX);
        for (worker = 0; worker < count; worker++)
            if (edits[worker].op == PENDING_STAR && edits[worker].value[0] == L'1') stars++;
        Check("parallel processes retain every queued change", count == 128 && stars == 128);
        HeapFree(GetProcessHeap(), 0, edits);
    } else Check("pending process results can be read", FALSE);
    Check("unique pending fixture file removed", DeleteFileW(path));
}

/* Whether a removal waits for `profile` (or its queue cannot be read). */
static BOOL RemovalQueued(const Profile *profile)
{
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    int n = edits ? SessionStore_LoadPending(profile, edits, SESSION_PENDING_MAX) : -1, i;
    BOOL queued = n < 0;
    for (i = 0; i < n && !queued; i++) queued = edits[i].op == PENDING_REMOVE;
    if (edits) HeapFree(GetProcessHeap(), 0, edits);
    return queued;
}

static void TestPendingApplication(const ProfileList *profiles)
{
    const Profile *profile = &profiles->items[0];
    SessionSet set;
    PendingEdit edit, queued[3];
    HWND window;
    WCHAR path[MAX_PATH], title[SESSION_TITLE_CCH];
    BOOL waiting;
    int row;
    Check("pending application snapshot loads", SessionStore_LoadProfiles(&set, profiles));
    row = FindRow(&set, g_projectId);
    if (row < 0 || !SessionStore_PendingPath(profile, path, ARRAYSIZE(path))) {
        Check("pending application entry found", FALSE);
        SessionStore_Free(&set);
        return;
    }
    window = StartFakeClaude(profile);
    Check("running profile message-window fixture created", window != NULL);
    if (!window) { SessionStore_Free(&set); return; }
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_projectId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Queued fixture title");
    Check("running profile change queues successfully",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) && waiting);
    Check("running profile entry stays unchanged", ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) &&
          wcscmp(title, L"Fixture session") == 0);
    Check("running profile does not apply its queue", SessionEdit_ApplyPending(NULL, profile) == 0 && SessionStore_LoadPending(profile, queued, 3) == 1);
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    Check("second pending kind retains queued title", SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) &&
          waiting && SessionStore_LoadPending(profile, queued, 3) == 2);
    Check("cancelling one pending kind preserves another", SessionEdit_Cancel(profile, &edit, 1) && SessionStore_LoadPending(profile, queued, 3) == 1 &&
          queued[0].op == PENDING_TITLE);
    StopFakeClaude(window);
    Check("closed private profile applies the queued title", SessionEdit_ApplyPending(NULL, profile) == 1 &&
          ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, L"Queued fixture title") == 0);
    Check("applied title records user ownership", ReadString(set.entries[set.rows[row].entry[0]].file, "titleSource", title, ARRAYSIZE(title)) &&
          wcscmp(title, L"user") == 0);
    Check("applied queue file removed", !Util_FileExists(path));
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"local_%s", g_projectId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Title queued under the entry's own id");
    Check("a change named by an entry's own id is queued", SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting);
    Check("a change queued under an entry's own id reaches the entry of its session",
          SessionEdit_ApplyPending(NULL, profile) == 1 &&
          ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, edit.value) == 0);
    if (Util_FileExists(path)) DeleteFileW(path);
    SessionStore_Free(&set);
}

static BOOL WrittenAt(const WCHAR *path, FILETIME *written)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return FALSE;
    *written = data.ftLastWriteTime;
    return TRUE;
}

/* The queue file: blank lines are skipped, a file that cannot be read is
 * never rewritten (its changes would be lost), and cancelling what is not
 * there writes nothing. */
static void TestPendingQueueFile(void)
{
    Profile profile, empty;
    PendingEdit edits[4], edit;
    WCHAR path[MAX_PATH], emptyPath[MAX_PATH];
    const WCHAR *unqueued[1];
    FILETIME before, after;
    HANDLE locked;
    BOOL waiting;
    char text[512];
    BOOL ready = SetProfile(&profile, L"Queue", L"queue-profile", FALSE) && SessionStore_PendingPath(&profile, path, ARRAYSIZE(path)) &&
                 SUCCEEDED(StringCchPrintfA(text, sizeof text, "title\t%ls\tFirst\r\n\r\n\nstar\t%ls\t1\n", g_projectId, g_projectId)) &&
                 Save(path, text);
    Check("queue file fixture written", ready);
    if (!ready) return;
    Check("blank lines in the queue are skipped", SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == 2 &&
          edits[0].op == PENDING_TITLE && edits[1].op == PENDING_STAR);
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_oldId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    ready = SetModified(path, 132000000000000000ULL) && WrittenAt(path, &before);
    Check("cancelling a change that is not queued leaves the queue file as it is",
          ready && SessionEdit_Cancel(&profile, &edit, 1) && WrittenAt(path, &after) && CompareFileTime(&before, &after) == 0);
    unqueued[0] = g_oldId;
    SessionEdit_Forget(&profile, unqueued, ARRAYSIZE(unqueued));
    Check("forgetting a session with nothing queued leaves the queue file as it is",
          ready && WrittenAt(path, &after) && CompareFileTime(&before, &after) == 0 &&
          SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == 2);
    Check("cancelling with no queue makes none",
          SetProfile(&empty, L"Unqueued", L"unqueued-profile", FALSE) && SessionStore_PendingPath(&empty, emptyPath, ARRAYSIZE(emptyPath)) &&
          SessionEdit_Cancel(&empty, &edit, 1) && !Util_FileExists(emptyPath));
    locked = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    Check("queue file locked by another process fixture", locked != INVALID_HANDLE_VALUE);
    if (locked != INVALID_HANDLE_VALUE) {
        Check("an unreadable queue reads as unreadable, not empty", SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == -1);
        Check("a change is refused rather than written over an unreadable queue",
              !SessionEdit_Change(NULL, &profile, NULL, &edit, &waiting, NULL, 0) && !waiting);
        CloseHandle(locked);
        Check("the unreadable queue kept its changes", SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == 2);
    }
    DeleteFileW(path);
}

/* Both entries of a session listed twice titled `title`. */
static BOOL BothTitled(const WCHAR *older, const WCHAR *newer, const WCHAR *title)
{
    WCHAR read[SESSION_TITLE_CCH];
    return ReadString(older, "title", read, ARRAYSIZE(read)) && wcscmp(read, title) == 0 &&
           ReadString(newer, "title", read, ARRAYSIZE(read)) && wcscmp(read, title) == 0;
}

/* Entry details: every entry kept when a session is listed twice in one
 * profile, and changed under any of its keys; long titles, group names and
 * lookups by key. */
static void TestEntryDetails(void)
{
    ProfileList profiles;
    SessionSet set, shown;
    PendingEdit edit, keep[3];
    WCHAR entries[MAX_PATH], name[MAX_PATH], older[MAX_PATH], newer[MAX_PATH], pending[MAX_PATH], error[512];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    HWND window;
    char json[2048], titleText[301];
    int row, count = 0, head, shownRow;
    BOOL ready, waiting;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    memset(titleText, 'T', 300);
    titleText[300] = 0;
    ready = SetProfile(&profiles.items[0], L"Details", L"details\\profile", FALSE) &&
            Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            SaveIn(profiles.items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"details\"}") &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\details\\organization", entries, ARRAYSIZE(entries)) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_older\",\"cliSessionId\":\"%ls\",\"title\":\"Older\","
                                                          "\"cwd\":\"C:\\\\Details\",\"lastActivityAt\":10}", g_projectId)) &&
            SaveIn(entries, L"local_older.json", json) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_newer\",\"cliSessionId\":\"%ls\",\"title\":\"Newer\","
                                                          "\"cwd\":\"C:\\\\Details\",\"lastActivityAt\":20}", g_projectId)) &&
            SaveIn(entries, L"local_newer.json", json) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_long\",\"cliSessionId\":\"%ls\",\"title\":\"%s\","
                                                          "\"lastActivityAt\":5}", g_oldId, titleText)) &&
            SaveIn(entries, L"local_long.json", json) &&
            Join(entries, L"local_older.json", older, ARRAYSIZE(older)) && Join(entries, L"local_newer.json", newer, ARRAYSIZE(newer));
    Check("entry detail fixtures created", ready);
    if (!ready) return;
    Check("entry detail snapshot loads", SessionStore_LoadProfiles(&set, &profiles));
    row = SessionStore_FindRow(&set, g_projectId);
    Check("rows are found by key through the index", row >= 0 && row == FindRow(&set, g_projectId));
    head = row >= 0 ? set.rows[row].entry[0] : -1;
    Check("a session listed twice in one profile names its latest entry",
          head >= 0 && set.entryCount == 3 && wcscmp(set.entries[head].title, L"Newer") == 0);
    Check("the other entry of that session follows it",
          head >= 0 && set.entries[head].duplicate >= 0 && wcscmp(set.entries[set.entries[head].duplicate].title, L"Older") == 0 &&
          set.entries[set.entries[head].duplicate].duplicate < 0);
    Check("a session listed twice lists both entries for deletion",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) &&
          Listed(paths, count, older) && Listed(paths, count, newer));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    paths = NULL;
    Check("a row that is not there lists nothing, with a reason",
          !SessionEdit_ListFiles(&set, -1, &paths, &count, error, ARRAYSIZE(error)) && !paths && count == 0 && error[0]);
    if (head >= 0 && SessionStore_PendingPath(&profiles.items[0], pending, ARRAYSIZE(pending))) {
        Profile *profile = &profiles.items[0];
        ZeroMemory(&edit, sizeof edit);
        edit.op = PENDING_TITLE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_projectId);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Renamed twice");
        Check("a rename of a session listed twice is made at once",
              SessionEdit_Change(NULL, profile, &set.entries[head], &edit, &waiting, NULL, 0) && !waiting);
        Check("the rename reaches both of its entries", BothTitled(older, newer, L"Renamed twice"));

        window = StartFakeClaude(profile);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Renamed while running");
        ready = window && SessionEdit_Change(NULL, profile, &set.entries[head], &edit, &waiting, NULL, 0) && waiting;
        StopFakeClaude(window);
        Check("a rename of a session listed twice waits while its profile runs", ready);
        Check("the waiting rename reaches both of its entries once the profile is closed",
              ready && SessionEdit_ApplyPending(NULL, profile) == 1 && BothTitled(older, newer, L"Renamed while running") &&
              !Util_FileExists(pending));

        /* A rename queued under the own id of the older entry, made before the session's first message. */
        window = StartFakeClaude(profile);
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), L"local_older");
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Queued under an own id");
        ready = window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
        StopFakeClaude(window);
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_projectId);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Renamed at once");
        Check("a rename made at once also takes off what waited under another key of the session",
              ready && SessionEdit_Change(NULL, profile, &set.entries[head], &edit, &waiting, NULL, 0) && !waiting &&
              BothTitled(older, newer, L"Renamed at once") && !Util_FileExists(pending));

        /* Keep: a removal queued under the own id of the older entry, shown on the session and cancelled. */
        window = StartFakeClaude(profile);
        edit.op = PENDING_REMOVE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), L"local_older");
        edit.value[0] = 0;
        ready = window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting && RemovalQueued(profile);
        StopFakeClaude(window);
        Check("a removal queued under the own id of a session's other entry", ready);
        if (ready && SessionStore_LoadProfiles(&shown, &profiles)) {
            shownRow = FindRow(&shown, g_projectId);
            Check("a removal queued under another entry's own id shows on the session",
                  shownRow >= 0 && shown.entries[shown.rows[shownRow].entry[0]].pendingRemove);
            SessionStore_Free(&shown);
        } else Check("a removal queued under another entry's own id is read", FALSE);
        ZeroMemory(keep, sizeof keep);
        keep[0].op = keep[1].op = keep[2].op = PENDING_REMOVE;
        StringCchCopyW(keep[0].key, ARRAYSIZE(keep[0].key), g_projectId);
        StringCchCopyW(keep[1].key, ARRAYSIZE(keep[1].key), L"local_newer");
        StringCchCopyW(keep[2].key, ARRAYSIZE(keep[2].key), L"local_older");
        Check("Keep, under every key of the session, cancels it",
              SessionEdit_Cancel(profile, keep, ARRAYSIZE(keep)) && !RemovalQueued(profile) && !Util_FileExists(pending) &&
              FileThere(older) && FileThere(newer));
    } else Check("a session listed twice and its queue", FALSE);
    row = SessionStore_FindRow(&set, g_oldId);
    Check("a title too long to keep whole keeps its beginning",
          row >= 0 && wcslen(set.entries[set.rows[row].entry[0]].title) == SESSION_TITLE_CCH - 1 &&
          wcsspn(set.entries[set.rows[row].entry[0]].title, L"T") == SESSION_TITLE_CCH - 1);
    if (row >= 0) SessionStore_GroupName(&set, set.rows[row].group, name, ARRAYSIZE(name));
    Check("a session without a working folder is shown in the unknown folder", row >= 0 && wcscmp(name, L"Unknown folder") == 0);
    SessionStore_Free(&set);
}

/* Stars made at once and later, what a snapshot shows of waiting
 * changes, changes that cannot be made, the queue's limit and Forget. */
static void TestPendingChanges(void)
{
    ProfileList profiles;
    Profile *profile = &profiles.items[0], full;
    SessionSet set;
    static PendingEdit edits[SESSION_PENDING_MAX];
    PendingEdit edit;
    WCHAR entries[MAX_PATH], files[3][MAX_PATH], pending[MAX_PATH], title[SESSION_TITLE_CCH], temporary[MAX_PATH];
    const WCHAR *forgotten[1];
    HWND window;
    HANDLE locked;
    BOOL ready, waiting, accepted;
    int i, rows[3], row;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(profile, L"Edits", L"edits\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SessionStore_PendingPath(profile, pending, ARRAYSIZE(pending));
    for (i = 0; ready && i < 3; i++) ready = WriteEntry(entries, g_editIds[i], "") && EntryPath(entries, g_editIds[i], files[i], MAX_PATH);
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("change fixtures ready", ready);
    if (!ready) return;
    for (i = 0; i < 3; i++) rows[i] = FindRow(&set, g_editIds[i]);
    if (rows[0] < 0 || rows[1] < 0 || rows[2] < 0) {
        Check("change fixture sessions loaded", FALSE);
        SessionStore_Free(&set);
        return;
    }

    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[1]);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    Check("a star in a closed profile is made at once",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[1]].entry[0]], &edit, &waiting, NULL, 0) &&
          !waiting && ReadTrue(files[1], "isStarred") && !Util_FileExists(pending));

    window = StartFakeClaude(profile);
    Check("change fixture: running profile", window != NULL);
    if (window) {
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"0");
        Check("a star in a running profile waits",
              SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[1]].entry[0]], &edit, &waiting, NULL, 0) &&
              waiting && ReadTrue(files[1], "isStarred"));
        edit.op = PENDING_TITLE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[0]);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Waiting title");
        accepted = SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[0]].entry[0]], &edit, &waiting, NULL, 0) && waiting;
        edit.op = PENDING_REMOVE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[2]);
        edit.value[0] = 0;
        accepted = accepted && SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[2]].entry[0]], &edit, &waiting, NULL, 0) && waiting;
        Check("changes to a running profile are queued", accepted);
        SessionStore_Free(&set);
        ready = SessionStore_LoadProfiles(&set, &profiles);
        Check("a snapshot shows what waits", ready && set.source[0].pending == 3);
        if (ready && (row = FindRow(&set, g_editIds[0])) >= 0) {
            const SessionEntry *entry = &set.entries[set.rows[row].entry[0]];
            Check("a waiting title shows on its entry", entry->pending && wcscmp(entry->pendingTitle, L"Waiting title") == 0 &&
                  entry->pendingStar == -1 && !entry->pendingRemove);
        } else Check("a waiting title's session is listed", FALSE);
        if (ready && (row = FindRow(&set, g_editIds[1])) >= 0)
            Check("a waiting star shows on its entry", set.entries[set.rows[row].entry[0]].pendingStar == 0 &&
                  set.entries[set.rows[row].entry[0]].starred);
        else Check("a waiting star's session is listed", FALSE);
        if (ready && (row = FindRow(&set, g_editIds[2])) >= 0)
            Check("a waiting removal shows on its entry", set.entries[set.rows[row].entry[0]].pendingRemove);
        else Check("a waiting removal's session is listed", FALSE);
        Check("Keep cancels the waiting removal", SessionEdit_Cancel(profile, &edit, 1) && !RemovalQueued(profile));
        forgotten[0] = g_editIds[0];
        SessionEdit_Forget(profile, forgotten, ARRAYSIZE(forgotten));
        Check("Forget drops every change to that session only", SessionStore_LoadPending(profile, edits, ARRAYSIZE(edits)) == 1 &&
              wcscmp(edits[0].key, g_editIds[1]) == 0);
        StopFakeClaude(window);
        Check("the waiting star is made once the profile is closed",
              SessionEdit_ApplyPending(NULL, profile) == 1 && !ReadTrue(files[1], "isStarred") && !Util_FileExists(pending));
    }

    /* A change that fails while the profile is closed leaves the queue, logged, and the folder holds no temporary file. */
    StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.cdm-new", files[2]);
    window = StartFakeClaude(profile);
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[2]);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Never written");
    row = FindRow(&set, g_editIds[2]);
    ready = window && row >= 0 && SetFileAttributesW(files[2], FILE_ATTRIBUTE_READONLY) &&
            SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    Check("a read-only entry fixture with a waiting title", ready);
    if (ready) {
        Check("a change that cannot be made is dropped", SessionEdit_ApplyPending(NULL, profile) == 1 &&
              !Util_FileExists(pending) && ReadString(files[2], "title", title, ARRAYSIZE(title)) && wcscmp(title, L"Fixture session") == 0);
        Check("a dropped change leaves no temporary file", !Util_FileExists(temporary));
        Check("a dropped change is not tried again", SessionEdit_ApplyPending(NULL, profile) == 0);
    }
    SetFileAttributesW(files[2], FILE_ATTRIBUTE_NORMAL);

    /* Rule 13: an entry gone is never written again. */
    row = FindRow(&set, g_editIds[2]);
    ready = row >= 0 && DeleteFileW(files[2]);
    Check("a deleted entry fixture", ready);
    if (ready) {
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Rule 13");
        Check("a change to an entry gone fails at once",
              !SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) &&
              !waiting && !Util_FileExists(files[2]) && !Util_FileExists(temporary) && !Util_FileExists(pending));
        window = StartFakeClaude(profile);
        ready = window && SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) && waiting;
        StopFakeClaude(window);
        Check("a change waits for an entry that is gone", ready);
        Check("a waiting change never makes a missing entry", SessionEdit_ApplyPending(NULL, profile) == 0 &&
              !Util_FileExists(files[2]) && SessionStore_LoadPending(profile, edits, ARRAYSIZE(edits)) == 1);
        Check("once the profile has run, a change to a session it does not list is dropped",
              SessionEdit_ApplyPendingAfterRun(profile) == 1 && !Util_FileExists(files[2]) && !Util_FileExists(pending));
    }

    /* After a run, an unreadable entry keeps every change waiting: its session may be the one. */
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_unlistedId);
    ready = SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    locked = CreateFileW(files[0], GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    Check("an unlisted session's change with an unreadable entry", ready && locked != INVALID_HANDLE_VALUE);
    if (locked != INVALID_HANDLE_VALUE) {
        Check("an incomplete read drops nothing", SessionEdit_ApplyPendingAfterRun(profile) == 0 &&
              SessionStore_LoadPending(profile, edits, ARRAYSIZE(edits)) == 1);
        CloseHandle(locked);
        Check("a complete read drops it", SessionEdit_ApplyPendingAfterRun(profile) == 1 && !Util_FileExists(pending));
    }
    if (Util_FileExists(pending)) DeleteFileW(pending);
    SessionStore_Free(&set);

    /* The queue takes SESSION_PENDING_MAX changes, and refuses the next one. */
    ready = SetProfile(&full, L"Full", L"full-profile", FALSE) && SessionStore_PendingPath(&full, pending, ARRAYSIZE(pending));
    accepted = TRUE;
    for (i = 0; ready && accepted && i < SESSION_PENDING_MAX; i++) {
        ZeroMemory(&edit, sizeof edit);
        edit.op = PENDING_STAR;
        StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"%08lx-9999-4999-8999-999999999999", (DWORD)i);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
        accepted = SessionEdit_Change(NULL, &full, NULL, &edit, &waiting, NULL, 0) && waiting;
    }
    Check("the queue takes its limit of changes", ready && accepted && SessionStore_LoadPending(&full, edits, ARRAYSIZE(edits)) == SESSION_PENDING_MAX);
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_unlistedId);
    Check("a change past the limit is refused, the queue kept", !SessionEdit_Change(NULL, &full, NULL, &edit, &waiting, NULL, 0) && !waiting &&
          SessionStore_LoadPending(&full, edits, ARRAYSIZE(edits)) == SESSION_PENDING_MAX);
    DeleteFileW(pending);
}

static void TestReadonlyPending(const ProfileList *profiles)
{
    const Profile *profile = &profiles->items[0];
    SessionSet set;
    PendingEdit oldEdit = { 0 }, newer, queued[3];
    WCHAR path[MAX_PATH], original[SESSION_TITLE_CCH], title[SESSION_TITLE_CCH];
    BOOL waiting, ready;
    int row;
    ready = SessionStore_LoadProfiles(&set, profiles);
    Check("readonly pending snapshot loads", ready);
    if (!ready) return;
    row = FindRow(&set, g_projectId);
    if (row < 0 || !SessionStore_PendingPath(profile, path, ARRAYSIZE(path)) ||
        !ReadString(set.entries[set.rows[row].entry[0]].file, "title", original, ARRAYSIZE(original))) {
        Check("readonly pending entry is available", FALSE);
        SessionStore_Free(&set);
        return;
    }
    oldEdit.op = PENDING_TITLE;
    StringCchCopyW(oldEdit.key, ARRAYSIZE(oldEdit.key), g_projectId);
    StringCchCopyW(oldEdit.value, ARRAYSIZE(oldEdit.value), L"Old queued intent");
    ready = SessionEdit_Change(NULL, profile, NULL, &oldEdit, &waiting, NULL, 0) && waiting &&
            SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY);
    Check("readonly pending intent fixture is durable", ready);
    if (ready) {
        newer = oldEdit;
        StringCchCopyW(newer.value, ARRAYSIZE(newer.value), L"Newest chosen intent");
        Check("a blocked intent commit returns failure before changing the entry",
              !SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &newer, &waiting, NULL, 0) && !waiting &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, original) == 0);
        Check("a failed intent replacement retains the older durable queue",
              SessionStore_LoadPending(profile, queued, ARRAYSIZE(queued)) == 1 && wcscmp(queued[0].value, oldEdit.value) == 0);
        Check("a blocked queue cannot replay entries or sustain a reload loop",
              SessionEdit_ApplyPending(NULL, profile) == 0 && SessionEdit_ApplyPending(NULL, profile) == 0 &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, original) == 0);
        Check("pending intent becomes writable", SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
        Check("a writable intent commits the latest entry and clears its queue",
              SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &newer, &waiting, NULL, 0) && !waiting &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, newer.value) == 0 &&
              !Util_FileExists(path));
    }
    if (Util_FileExists(path)) {
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path);
    }
    SessionStore_Free(&set);
}

static void TestDeletionList(const ProfileList *profiles, const WCHAR *projects)
{
    enum { COPIES = 56 };
    SessionSet set;
    WCHAR file[LONG_PATH_CCH], child[LONG_PATH_CCH], error[1024], (*paths)[LONG_PATH_CCH] = NULL;
    BOOL ready = TRUE, intact = TRUE;
    int copy, count = 0, row;
    for (copy = 0; copy < COPIES && ready; copy++) {
        ready = SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\deletion-%02d\\%s.jsonl", projects, copy, g_projectId)) &&
                Save(file, "{}\n") &&
                SUCCEEDED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\deletion-%02d\\%s\\sentinel.txt", projects, copy, g_projectId)) &&
                Save(child, "subagent fixture\n");
    }
    Check("many-project deletion fixtures prepared", ready);
    if (!ready) return;
    Check("many-project deletion snapshot loads", SessionStore_LoadProfiles(&set, profiles));
    row = FindRow(&set, g_projectId);
    Check("deletion collection finds conversation", row >= 0);
    if (row >= 0) {
        Check("deletion collection completes", SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)));
        Check("deletion collection retains every path beyond its initial capacity", count == COPIES * 2 + 2);
        for (copy = 0; copy < count; copy++)
            if (!Core_PathUnder(paths[copy], g_root) || (!FileThere(paths[copy]) && !DirThere(paths[copy]))) intact = FALSE;
        Check("deletion preparation leaves all private files intact", intact);
        if (paths) HeapFree(GetProcessHeap(), 0, paths);
        paths = NULL;
        {
            /* A project folder whose transcript paths are longer than MAX_PATH. */
            WCHAR leaf[MAX_PATH], longDir[LONG_PATH_CCH], transcript[LONG_PATH_CCH];
            size_t length = wcslen(projects) + 24 < MAX_PATH ? MAX_PATH - wcslen(projects) - 24 : 0;
            wmemset(leaf, L'x', length);
            leaf[length] = 0;
            ready = length > 0 && Join(projects, leaf, longDir, ARRAYSIZE(longDir)) && MakeDir(longDir);
            Check("long project directory fixture created", ready);
            if (ready) {
                Check("an unrelated long project folder does not stop deletion",
                      SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == COPIES * 2 + 2 && !error[0]);
                if (paths) HeapFree(GetProcessHeap(), 0, paths);
                paths = NULL;
                ready = SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\%s.jsonl", longDir, g_projectId)) &&
                        wcslen(transcript) >= MAX_PATH && Save(transcript, "{}\n");
                Check("a transcript past MAX_PATH fixture", ready);
                Check("a transcript past MAX_PATH is listed for deletion",
                      ready && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == COPIES * 2 + 3 &&
                      Listed(paths, count, transcript));
                if (paths) HeapFree(GetProcessHeap(), 0, paths);
                paths = NULL;
                Check("long project directory fixture removed", RemoveFixture(longDir));
            }
        }
        Check("failed or partial preparation leaves the original entry intact", Util_FileExists(set.entries[set.rows[row].entry[0]].file));
    }
    SessionStore_Free(&set);
}

/* A session's other transcripts (earlier ones, the one before a /clear) and
 * what Claude Code keeps for each, a transcript another session goes on
 * with, and a transcript filed in two project folders. */
static void TestOtherTranscripts(const WCHAR *projects)
{
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], code[MAX_PATH], path[LONG_PATH_CCH], error[1024], entry[MAX_PATH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    char extra[512];
    int row, count = 0;
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(&profiles.items[0], L"Lineage", L"lineage\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SUCCEEDED(StringCchPrintfA(extra, sizeof extra, ",\"priorCliSessionIds\":[\"%ls\", \"%ls\",\"not-a-uuid\"],\"preClearCliSessionId\":\"%ls\"",
                                       g_priorId, g_claimedId, g_preClearId)) &&
            WriteEntry(entries, g_lineageId, extra) && WriteEntry(entries, g_claimedId, "") && WriteEntry(entries, g_duplicateId, "") &&
            EntryPath(entries, g_lineageId, entry, ARRAYSIZE(entry)) && CodeDir(projects, code, ARRAYSIZE(code)) &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_lineageId)) && Save(path, "0123456789") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_priorId)) && Save(path, "01234567890123456789") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_preClearId)) &&
            Save(path, "0123456789012345678901234567890123456789") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_claimedId)) && Save(path, "claimed") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\file-history\\%s\\snapshot", code, g_lineageId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\session-env\\%s\\environment", code, g_priorId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\debug\\%s.txt", code, g_lineageId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\file-history\\%s\\snapshot", code, g_claimedId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.desktop-released.json", projects, g_lineageId)) && Save(path, "{}") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\dup-small\\%s.jsonl", projects, g_duplicateId)) && Save(path, "12345") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\dup-large\\%s.jsonl", projects, g_duplicateId)) &&
            Save(path, "12345678901234567890123456789012345678901234567890");
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("other transcript fixtures ready", ready);
    if (!ready) return;
    row = FindRow(&set, g_lineageId);
    Check("a conversation's size counts its other transcripts, not another session's", row >= 0 && set.rows[row].transcriptBytes == 70);
    Check("a session's other transcripts list for deletion with what Claude Code keeps for them",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 8 && Listed(paths, count, entry));
    if (paths) {
        const WCHAR *const expected[] = { L"lineage\\aaaaaaaa-0000-4000-8000-000000000001.jsonl",
                                          L"lineage\\aaaaaaaa-0000-4000-8000-000000000002.jsonl",
                                          L"lineage\\aaaaaaaa-0000-4000-8000-000000000003.jsonl",
                                          L"lineage\\aaaaaaaa-0000-4000-8000-000000000001.desktop-released.json" };
        const WCHAR *const stores[] = { L"file-history\\aaaaaaaa-0000-4000-8000-000000000001",
                                        L"session-env\\aaaaaaaa-0000-4000-8000-000000000002",
                                        L"debug\\aaaaaaaa-0000-4000-8000-000000000001.txt" };
        BOOL all = TRUE;
        int i;
        for (i = 0; i < (int)ARRAYSIZE(expected); i++)
            all = all && Join(projects, expected[i], path, ARRAYSIZE(path)) && Listed(paths, count, path);
        for (i = 0; i < (int)ARRAYSIZE(stores); i++)
            all = all && Join(code, stores[i], path, ARRAYSIZE(path)) && Listed(paths, count, path);
        Check("the transcripts, Claude's marks and Claude Code's folders of the session are listed", all);
        Check("a transcript another session goes on with stays, with its folders",
              SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_claimedId)) && !Listed(paths, count, path) &&
              SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\file-history\\%s", code, g_claimedId)) && !Listed(paths, count, path));
        HeapFree(GetProcessHeap(), 0, paths);
        paths = NULL;
    }
    row = FindRow(&set, g_duplicateId);
    Check("a transcript filed twice is the larger one",
          row >= 0 && set.rows[row].transcriptBytes == 50 && Core_EndsWithI(set.rows[row].transcriptPath, L"\\dup-large\\aaaaaaaa-0000-4000-8000-000000000005.jsonl"));
    Check("both copies of a transcript filed twice are listed for deletion",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 3);
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    SessionStore_Free(&set);
}

/* Entries counted, not listed: remote sessions (moved to the cloud: set,
 * whatever its form), files that are no entry and entries that cannot be
 * read; with either of the last two, the read is not complete. */
static void TestEntryCounts(void)
{
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], locked[MAX_PATH], broken[MAX_PATH];
    HANDLE file;
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(&profiles.items[0], L"Counts", L"counts\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            WriteEntry(entries, g_editIds[0], ",\"movedToCloud\":false") &&
            WriteEntry(entries, g_editIds[1], ",\"sshConfig\":{\"host\":\"fixture\"}") &&
            WriteEntry(entries, g_editIds[2], ",\"cloudSessionId\":\"fixture\"") &&
            WriteEntry(entries, g_priorId, ",\"movedToCloud\":{\"sessionId\":\"fixture\"}") &&
            WriteEntry(entries, g_preClearId, ",\"movedToCloud\":null") &&
            SaveIn(entries, L"local_not-an-entry.json", "{\"title\":\"no session\"}") && Join(entries, L"local_not-an-entry.json", broken, ARRAYSIZE(broken)) &&
            WriteEntry(entries, g_unlistedId, "") && EntryPath(entries, g_unlistedId, locked, ARRAYSIZE(locked));
    Check("entry count fixtures ready", ready);
    if (!ready) return;
    file = CreateFileW(locked, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ready = file != INVALID_HANDLE_VALUE && SessionStore_LoadProfiles(&set, &profiles);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Check("entry count snapshot loads", ready);
    if (!ready) return;
    Check("sessions run elsewhere are counted, not listed", set.source[0].elsewhere == 3);
    Check("a file that is no entry and an entry that cannot be read are counted", set.source[0].unreadable == 2);
    Check("only this PC's readable sessions are listed",
          set.rowCount == 2 && FindRow(&set, g_editIds[0]) >= 0 && FindRow(&set, g_preClearId) >= 0);
    SessionStore_Free(&set);
    Check("a file that is no entry leaves the read incomplete", !SessionStore_LoadEntries(&set, &profiles.items[0]));
    SessionStore_Free(&set);
    Check("a profile's entries all read count as complete",
          DeleteFileW(broken) && SessionStore_LoadEntries(&set, &profiles.items[0]) && set.rowCount == 3);
    SessionStore_Free(&set);
}

/* A record of a running Claude Code for `sessionId`, of process `pid`
 * started at `started` (a FILETIME). */
static BOOL WriteLiveRecord(const WCHAR *code, DWORD pid, const WCHAR *sessionId, ULONGLONG started)
{
    WCHAR path[MAX_PATH];
    char json[256];
    return SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\sessions\\%lu.json", code, pid)) &&
           SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"pid\":%lu,\"sessionId\":\"%ls\",\"procStart\":\"%I64u\"}", pid, sessionId, started)) &&
           Save(path, json);
}

/* Paths past MAX_PATH, an entry's and a transcript's: read, changed, copied,
 * listed; a deletion that would need the Recycle Bin for them is refused, as
 * it is while the session runs. Every file of this session is longer than
 * MAX_PATH, so not even a broken check could move one to the Recycle Bin. */
static void TestLongPaths(const WCHAR *projects)
{
    ProfileList profiles, running;
    SessionSet set;
    PendingEdit edit;
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    FILETIME created, exited, kernel, user;
    WCHAR leaf[MAX_PATH], storage[MAX_PATH], entries[LONG_PATH_CCH], entry[LONG_PATH_CCH], folder[LONG_PATH_CCH];
    WCHAR transcript[LONG_PATH_CCH], code[MAX_PATH], exe[MAX_PATH], id[SESSION_ID_CCH], error[2048], title[SESSION_TITLE_CCH];
    WCHAR left[LONG_PATH_CCH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    HWND window;
    RemoveResult result;
    /* A profile folder of 145 characters: its entries folder (95 more) is
     * shorter than MAX_PATH, an entry in it (48 more) is not. */
    size_t storageLength = MAX_PATH - 115, padding = storageLength > wcslen(g_root) + 1 ? storageLength - wcslen(g_root) - 1 : 0;
    int row, count = 0, i, calls;
    BOOL ready, waiting, outside, loaded;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    wmemset(leaf, L'p', padding);
    leaf[padding] = 0;
    ready = padding > 0 && FixturePath(leaf, storage, ARRAYSIZE(storage)) &&
            SetProfile(&profiles.items[0], L"Long", leaf, FALSE) && Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            SaveIn(storage, L"config.json", "{\"lastKnownAccountUuid\":\"eeeeeeee-0000-4000-8000-000000000001\"}") &&
            SUCCEEDED(StringCchPrintfW(entries, ARRAYSIZE(entries), L"%s\\claude-code-sessions\\%s\\%s", storage, g_accountId, g_organizationId)) &&
            WriteEntry(entries, g_longId, "") && EntryPath(entries, g_longId, entry, ARRAYSIZE(entry)) &&
            wcslen(entries) < MAX_PATH && wcslen(entry) >= MAX_PATH;
    wmemset(leaf, L'q', CORE_PROJECT_NAME_MAX);
    leaf[CORE_PROJECT_NAME_MAX] = 0;
    ready = ready && Join(projects, leaf, folder, ARRAYSIZE(folder)) &&
            SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\%s.jsonl", folder, g_longId)) &&
            Save(transcript, "{\"sessionId\":\"bbbbbbbb-0000-4000-8000-000000000001\",\"cwd\":\"C:\\\\Fixture\"}\n") &&
            wcslen(transcript) >= MAX_PATH && CodeDir(projects, code, ARRAYSIZE(code)) && SessionStore_LoadProfiles(&set, &profiles);
    Check("long path fixtures ready", ready);
    if (!ready) return;
    row = FindRow(&set, g_longId);
    Check("an entry past MAX_PATH is read", row >= 0 && Core_PathEquals(set.entries[set.rows[row].entry[0]].file, entry));
    Check("a transcript past MAX_PATH is found", row >= 0 && set.rows[row].transcript && Core_PathEquals(set.rows[row].transcriptPath, transcript) &&
          set.rows[row].transcriptBytes > 0);
    if (row < 0) {
        SessionStore_Free(&set);
        return;
    }
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_longId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Long title");
    Check("an entry past MAX_PATH is renamed",
          SessionEdit_Change(NULL, &profiles.items[0], &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) &&
          !waiting && ReadString(entry, "title", title, ARRAYSIZE(title)) && wcscmp(title, L"Long title") == 0);
    edit.op = PENDING_REMOVE;
    edit.value[0] = 0;
    calls = g_recycle.calls;
    Check("an entry too long for the Recycle Bin is not removed, its path named",
          !SessionEdit_Change(NULL, &profiles.items[0], &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) &&
          !waiting && wcsstr(error, entry) != NULL && FileThere(entry) && g_recycle.calls == calls);
    if (SessionEdit_CopyConversation(NULL, &set, row, 0, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE) {
        WCHAR copied[LONG_PATH_CCH];
        Check("a transcript past MAX_PATH is copied beside it with its own id",
              SUCCEEDED(StringCchPrintfW(copied, ARRAYSIZE(copied), L"%s\\%s.jsonl", folder, id)) && FileThere(copied) &&
              ReadString(copied, "sessionId", title, ARRAYSIZE(title)) && wcscmp(title, id) == 0);
        Check("a copy past MAX_PATH that could not be opened is removed",
              SessionEdit_RemoveCopy(&set, row, 0, id, left, ARRAYSIZE(left)) && !FileThere(copied));
    } else Check("a transcript past MAX_PATH is copied", FALSE);
    Check("every file of the session is listed for deletion",
          SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 2 &&
          Listed(paths, count, entry) && Listed(paths, count, transcript));
    for (i = 0; paths && i < count; i++) ready = ready && wcslen(paths[i]) >= MAX_PATH;
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (ready) {
        Check("nothing runs the session: it can be deleted", SessionEdit_CanDelete(&set, row, error, ARRAYSIZE(error)) && !error[0]);
        result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
        Check("a deletion needing the Recycle Bin for paths past MAX_PATH is refused with the path",
              result == REMOVE_FAILED && wcsstr(error, entry) != NULL);
        Check("a refused deletion moves nothing", FileThere(entry) && FileThere(transcript) && g_recycle.calls == calls);
    }

    window = StartFakeClaude(&profiles.items[0]);
    if (ready && window && Claude_IsRunning(&profiles.items[0])) {
        Check("a session whose profile runs cannot be deleted, the profile named",
              !SessionEdit_CanDelete(&set, row, error, ARRAYSIZE(error)) && wcsstr(error, profiles.items[0].name) != NULL);
        result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
        Check("deletion is refused while a profile listing the session runs", result == REMOVE_FAILED && error[0] &&
              wcsstr(error, profiles.items[0].name) != NULL && FileThere(entry) && FileThere(transcript));
    } else Check("running profile fixture for deletion", FALSE);
    StopFakeClaude(window);

    /* A Claude Code running the session: a suspended child of this process, recorded as Claude Code records itself. */
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    ZeroMemory(&child, sizeof child);
    ready = Util_SelfExe(exe, ARRAYSIZE(exe)) &&
            CreateProcessW(exe, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &startup, &child);
    if (ready) {
        ready = GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user) &&
                WriteLiveRecord(code, child.dwProcessId, g_longId, ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime) &&
                WriteLiveRecord(code, GetCurrentProcessId(), g_editIds[0], 1);
        Check("running Claude Code fixture", ready);
        running = profiles;
        running.items[0].running = TRUE;
        running.items[0].pid = GetCurrentProcessId();
        SessionStore_Free(&set);
        loaded = ready && SessionStore_LoadProfiles(&set, &running);
        row = loaded ? FindRow(&set, g_longId) : -1;
        if (row >= 0) {
            Check("a session run by a profile's Claude Code is in use there", set.rows[row].live == 1);
            Check("a profile's Claude Code is found running the session", SessionStore_RunningNow(&running, g_longId, &outside) == 1 && !outside);
            Check("a record whose process started at another time does not count", SessionStore_RunningNow(&running, g_editIds[0], &outside) == 0 &&
                  !outside);
        } else Check("in-use snapshot loads", FALSE);
        if (loaded) SessionStore_Free(&set);
        loaded = ready && SessionStore_LoadProfiles(&set, &profiles);
        row = loaded ? FindRow(&set, g_longId) : -1;
        if (row >= 0 && SessionStore_RunningNow(&profiles, g_longId, &outside) == 0 && outside) {
            Check("a session a Claude Code of no profile runs cannot be deleted", !SessionEdit_CanDelete(&set, row, error, ARRAYSIZE(error)) && error[0]);
            result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
            Check("deletion is refused while a Claude Code of no profile runs the session",
                  result == REMOVE_FAILED && error[0] && FileThere(entry) && FileThere(transcript));
        } else Check("Claude Code of no profile fixture", FALSE);
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    } else Check("suspended Claude Code fixture started", FALSE);
    SessionStore_Free(&set);
    Check("long path fixtures removed", RemoveFixture(folder) && RemoveFixture(storage));
}

static ULONGLONG NowMs(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - 116444736000000000ULL) / 10000;
}

/* The time in ms that Claude's mark of session `id` deleted, beside the
 * entries `entries`, holds; 0 when there is no such mark. */
static ULONGLONG DeletedMark(const WCHAR *entries, const WCHAR *id)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    ULONGLONG ms = 0;
    char *text;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\deleted_%s", entries, id)) ||
        (text = Util_ReadFile(path, 64, FALSE, &length)) == NULL) return 0;
    if (strspn(text, "0123456789") == length) ms = _strtoui64(text, NULL, 10);
    HeapFree(GetProcessHeap(), 0, text);
    return ms;
}

static int CountMarks(const WCHAR *entries)
{
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(entries, L"deleted_*", &found, FALSE);
    int count = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do count++; while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}

static BOOL RemoveMarks(const WCHAR *entries)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(entries, L"deleted_*", &found, FALSE);
    BOOL ok = TRUE;
    if (find == INVALID_HANDLE_VALUE) return TRUE;
    do ok = Join(entries, found.cFileName, path, ARRAYSIZE(path)) && RemoveFixture(path) && ok; while (FindNextFileW(find, &found));
    FindClose(find);
    return ok;
}

static void MakeRemoval(PendingEdit *edit, const WCHAR *key)
{
    ZeroMemory(edit, sizeof *edit);
    edit->op = PENDING_REMOVE;
    StringCchCopyW(edit->key, ARRAYSIZE(edit->key), key);
}

/* Removing a session from one profile: its entries to the Recycle Bin, and
 * Claude's marks that it was deleted there, for each id of the session no
 * other session claims; declined at Windows' question, nothing; with Claude
 * started meanwhile, it waits for that Claude to close. */
static void TestRemovals(void)
{
    ProfileList profiles;
    Profile *profile = &profiles.items[0];
    SessionSet set;
    PendingEdit edit;
    WCHAR entries[MAX_PATH], head[MAX_PATH], other[MAX_PATH], third[MAX_PATH], pending[MAX_PATH], error[512];
    HWND window;
    char extra[512], json[1024];
    ULONGLONG before, after, mark;
    int row;
    BOOL ready, waiting, made;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    /* Session A, listed twice (its other entry under its own id), naming an
     * earlier transcript and one session B goes on with; and session C. */
    ready = PrepareProfile(profile, L"Removals", L"removals\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SessionStore_PendingPath(profile, pending, ARRAYSIZE(pending)) &&
            SUCCEEDED(StringCchPrintfA(extra, sizeof extra, ",\"priorCliSessionIds\":[\"%ls\",\"%ls\"]", g_priorId, g_claimedId)) &&
            WriteEntry(entries, g_lineageId, extra) && EntryPath(entries, g_lineageId, head, ARRAYSIZE(head)) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                          "\"lastActivityAt\":100}", g_editIds[0], g_lineageId)) &&
            EntryPath(entries, g_editIds[0], other, ARRAYSIZE(other)) && Save(other, json) &&
            WriteEntry(entries, g_claimedId, "") && WriteEntry(entries, g_duplicateId, "") && EntryPath(entries, g_duplicateId, third, ARRAYSIZE(third)) &&
            SessionStore_LoadProfiles(&set, &profiles);
    Check("removal fixtures ready", ready);
    if (!ready) return;
    row = FindRow(&set, g_lineageId);
    if (row < 0 || FindRow(&set, g_duplicateId) < 0) {
        Check("removal fixture sessions listed", FALSE);
        SessionStore_Free(&set);
        return;
    }

    /* What waited for A under any of its keys goes with it. */
    window = StartFakeClaude(profile);
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"local_%s", g_editIds[0]);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Queued under the other entry's own id");
    ready = window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_lineageId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    ready = ready && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    Check("changes wait for a session under two of its keys", ready);
    RecycleWill(REMOVE_DONE, NULL);
    MakeRemoval(&edit, g_lineageId);
    before = NowMs();
    made = SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) && !waiting;
    after = NowMs();
    Check("a session is removed from a closed profile", made && !g_recycle.escaped && !FileThere(head) && !FileThere(other));
    Check("both of its entries go to the Recycle Bin together", g_recycle.calls == 1 && g_recycle.count == 2 && Recycled(head) && Recycled(other));
    mark = DeletedMark(entries, g_lineageId);
    Check("Claude's mark that it was deleted holds the time in ms", mark >= before && mark <= after);
    Check("each id of the session is marked once: its entries' own ids and its earlier transcript",
          DeletedMark(entries, g_editIds[0]) == mark && DeletedMark(entries, g_priorId) == mark);
    Check("a transcript another session goes on with is not marked", DeletedMark(entries, g_claimedId) == 0 && CountMarks(entries) == 3);
    Check("nothing waits any more for the removed session, under any of its keys", !Util_FileExists(pending));

    /* C: declined at Windows' question. */
    SessionStore_Free(&set);
    ready = SessionStore_LoadProfiles(&set, &profiles) && (row = FindRow(&set, g_duplicateId)) >= 0 && RemoveMarks(entries);
    Check("removal fixture read again", ready);
    if (!ready) {
        SessionStore_Free(&set);
        return;
    }
    RecycleWill(REMOVE_CANCELLED, NULL);
    MakeRemoval(&edit, g_duplicateId);
    Check("a removal declined at Windows' question is no failure, and does not wait",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) && !waiting &&
          !error[0] && g_recycle.calls == 1);
    Check("a declined removal leaves the entry, marks nothing and queues nothing",
          FileThere(third) && CountMarks(entries) == 0 && !Util_FileExists(pending));

    /* C again: its Claude starts while Windows asks; it then keeps the session in memory. */
    RecycleWill(REMOVE_DONE, profile);
    Check("a removal overtaken by its Claude starting waits for that Claude",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) && waiting &&
          RemovalQueued(profile));
    Check("a removal overtaken marks nothing", CountMarks(entries) == 0);
    StopStartedClaude();
    Check("its Claude wrote the session back as it closed", WriteEntry(entries, g_duplicateId, ""));
    RecycleWill(REMOVE_DONE, NULL);
    before = NowMs();
    Check("the waiting removal is made once the profile is closed",
          SessionEdit_ApplyPending(NULL, profile) == 1 && !FileThere(third) && Recycled(third) && !Util_FileExists(pending));
    mark = DeletedMark(entries, g_duplicateId);
    Check("a waiting removal made writes Claude's mark", mark >= before && mark <= NowMs() && CountMarks(entries) == 1);

    /* C once more, queued while its profile runs; its Claude starts again while Windows asks. */
    ready = RemoveMarks(entries) && WriteEntry(entries, g_duplicateId, "");
    window = StartFakeClaude(profile);
    ready = ready && window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    Check("a removal waits while its profile runs", ready);
    RecycleWill(REMOVE_DONE, profile);
    Check("a waiting removal overtaken by its Claude starting waits again, unmarked",
          SessionEdit_ApplyPending(NULL, profile) == 0 && RemovalQueued(profile) && CountMarks(entries) == 0);
    StopStartedClaude();
    Check("once that Claude has run without the session, its removal waits no more",
          SessionEdit_ApplyPendingAfterRun(profile) == 1 && !RemovalQueued(profile) && CountMarks(entries) == 0);
    RecycleWill(REMOVE_DONE, NULL);
    SessionStore_Free(&set);
}

/* Delete everywhere: every entry, the conversation and everything Claude's
 * own delete removes with it, in one go; nothing when Windows' question is
 * declined; with a Claude started meanwhile, its entry there goes when it
 * closes. */
static void TestDeleteEverywhere(const WCHAR *projects)
{
    ProfileList profiles;
    SessionSet set;
    PendingEdit edit;
    WCHAR entries[2][MAX_PATH], files[2][MAX_PATH], staged[MAX_PATH], scratch[MAX_PATH], working[MAX_PATH], subfolder[MAX_PATH];
    WCHAR temporary[MAX_PATH], temporaryFolder[MAX_PATH], longTemporary[LONG_PATH_CCH], leaf[MAX_PATH], neighbor[MAX_PATH];
    WCHAR transcript[MAX_PATH], preImport[MAX_PATH], sessionFolder[MAX_PATH], physical[MAX_PATH], pending[MAX_PATH], error[1024];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    const WCHAR *listed[8];
    char quoted[MAX_PATH * 6 + 4], extra[MAX_PATH * 6 + 64];
    HWND window;
    RemoveResult result;
    size_t padding;
    int p, row, count = 0, i;
    BOOL ready = TRUE, all, waiting;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (p = 0; p < 2 && ready; p++)
        ready = PrepareProfile(&profiles.items[p], p ? L"DeleteB" : L"DeleteA", p ? L"delete\\b" : L"delete\\a", g_accountId, g_organizationId,
                               entries[p], MAX_PATH);
    /* D: listed in both profiles, without a folder in A's "no folder" area;
     * the transcript A staged for it; Claude Code's transcript, the one it
     * kept before taking it in, its folder and its temporary folder. */
    ready = ready && SUCCEEDED(StringCchPrintfW(scratch, ARRAYSIZE(scratch), L"%s\\scratch-workspaces\\%s\\%s", profiles.items[0].dataDir,
                                                g_accountId, g_organizationId)) &&
            Join(scratch, L"scratch-delete", working, ARRAYSIZE(working)) && SaveIn(working, L"notes.txt", "scratch\n") &&
            Join(working, L"sub", subfolder, ARRAYSIZE(subfolder)) &&
            SUCCEEDED(StringCchPrintfW(staged, ARRAYSIZE(staged), L"%s\\imported-staging\\%s.jsonl", entries[0], g_deletedId)) && Save(staged, "{}\n") &&
            Core_JsonQuote(staged, quoted, sizeof quoted) &&
            SUCCEEDED(StringCchPrintfA(extra, sizeof extra, ",\"stagedTranscriptPath\":%s", quoted)) &&
            WriteEntryIn(entries[0], g_deletedId, working, extra) && WriteEntryIn(entries[1], g_deletedId, working, "") &&
            EntryPath(entries[0], g_deletedId, files[0], MAX_PATH) && EntryPath(entries[1], g_deletedId, files[1], MAX_PATH) &&
            WriteEntryIn(entries[0], g_neighborId, subfolder, "") && EntryPath(entries[0], g_neighborId, neighbor, ARRAYSIZE(neighbor)) &&
            WriteEntry(entries[0], g_startedId, "") && WriteEntry(entries[1], g_startedId, "") &&
            SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\delete\\%s.jsonl", projects, g_deletedId)) && Save(transcript, "{}\n") &&
            SUCCEEDED(StringCchPrintfW(preImport, ARRAYSIZE(preImport), L"%s.pre-import", transcript)) && Save(preImport, "{}\n") &&
            SUCCEEDED(StringCchPrintfW(sessionFolder, ARRAYSIZE(sessionFolder), L"%s\\delete\\%s", projects, g_deletedId)) &&
            SaveIn(sessionFolder, L"subagent.jsonl", "{}\n") &&
            GetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", temporary, ARRAYSIZE(temporary)) > 0 &&
            SUCCEEDED(StringCchPrintfW(temporaryFolder, ARRAYSIZE(temporaryFolder), L"%s\\claude\\C--Fixture\\%s", temporary, g_deletedId)) &&
            SaveIn(temporaryFolder, L"output.txt", "task\n") && SessionStore_PendingPath(&profiles.items[1], pending, ARRAYSIZE(pending));
    /* A temporary folder too long for the Recycle Bin, left to Claude Code. */
    padding = wcslen(temporary) + 40 < MAX_PATH ? MAX_PATH - wcslen(temporary) - 40 : 1;
    wmemset(leaf, L't', padding);
    leaf[padding] = 0;
    ready = ready && SUCCEEDED(StringCchPrintfW(longTemporary, ARRAYSIZE(longTemporary), L"%s\\claude\\%s\\%s", temporary, leaf, g_deletedId)) &&
            wcslen(longTemporary) >= MAX_PATH && MakeDir(longTemporary);
    /* A title waiting for D in B. */
    window = ready ? StartFakeClaude(&profiles.items[1]) : NULL;
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_deletedId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Waiting title");
    ready = ready && window && SessionEdit_Change(NULL, &profiles.items[1], NULL, &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("delete everywhere fixtures ready", ready);
    listed[0] = files[0];
    listed[1] = files[1];
    listed[2] = staged;
    listed[3] = working;            /* folders: 3, 4 and 7 */
    listed[4] = temporaryFolder;
    listed[5] = transcript;
    listed[6] = preImport;
    listed[7] = sessionFolder;
    if (!ready) return;
    row = FindRow(&set, g_deletedId);
    Check("a session another one works under keeps its working folder",
          row >= 0 && !SessionEdit_RemovesWorkingFolder(&set, row, physical, ARRAYSIZE(physical)));
    Check("so its working folder is not listed for deletion",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 7 && !Listed(paths, count, working));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    paths = NULL;
    SessionStore_Free(&set);
    ready = RemoveFixture(neighbor) && SessionStore_LoadProfiles(&set, &profiles) && (row = FindRow(&set, g_deletedId)) >= 0;
    Check("delete everywhere fixture without the other session", ready);
    if (!ready) {
        SessionStore_Free(&set);
        return;
    }
    Check("a session without a folder takes its working folder with it",
          SessionEdit_RemovesWorkingFolder(&set, row, physical, ARRAYSIZE(physical)) && Core_PathEquals(physical, working));
    all = SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == (int)ARRAYSIZE(listed);
    for (i = 0; all && i < (int)ARRAYSIZE(listed); i++) all = Listed(paths, count, listed[i]);
    Check("the deletion lists the entries, the staged transcript, the working folder, the transcript kept before, the temporary folder", all);
    Check("a temporary folder too long for the Recycle Bin is not listed", paths && !Listed(paths, count, longTemporary));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    paths = NULL;

    RecycleWill(REMOVE_CANCELLED, NULL);
    result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
    all = result == REMOVE_CANCELLED && g_recycle.calls == 1 && g_recycle.count == (int)ARRAYSIZE(listed);
    for (i = 0; all && i < (int)ARRAYSIZE(listed); i++) all = Recycled(listed[i]) && PathThere(listed[i], i == 3 || i == 4 || i == 7);
    Check("a deletion declined at Windows' question leaves everything", all);
    Check("a declined deletion marks nothing and keeps what waits",
          CountMarks(entries[0]) == 0 && CountMarks(entries[1]) == 0 && Util_FileExists(pending));

    RecycleWill(REMOVE_DONE, NULL);
    result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
    all = result == REMOVE_DONE && !g_recycle.escaped && g_recycle.calls == 1 && g_recycle.count == (int)ARRAYSIZE(listed);
    for (i = 0; all && i < (int)ARRAYSIZE(listed); i++) all = !PathThere(listed[i], i == 3 || i == 4 || i == 7);
    Check("a session is deleted everywhere in one go", all && !error[0]);
    Check("each profile that listed it gets Claude's mark",
          DeletedMark(entries[0], g_deletedId) > 0 && DeletedMark(entries[1], g_deletedId) > 0 && CountMarks(entries[0]) == 1 && CountMarks(entries[1]) == 1);
    Check("what waited for it is dropped", !Util_FileExists(pending));
    Check("a temporary folder too long for the Recycle Bin is left", DirThere(longTemporary));
    SessionStore_Free(&set);

    /* B's Claude starts while Windows asks. */
    ready = SessionStore_LoadProfiles(&set, &profiles) && (row = FindRow(&set, g_startedId)) >= 0;
    Check("started-during-deletion fixture read", ready);
    if (ready) {
        RecycleWill(REMOVE_DONE, &profiles.items[1]);
        result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
        Check("a deletion overtaken by a profile starting is made", result == REMOVE_DONE && g_recycle.count == 2);
        Check("the profile that did not start gets Claude's mark", DeletedMark(entries[0], g_startedId) > 0);
        Check("the profile that started waits to remove the entry its Claude keeps, unmarked",
              RemovalQueued(&profiles.items[1]) && DeletedMark(entries[1], g_startedId) == 0);
        StopStartedClaude();
        Check("once that Claude has run without the session, its removal waits no more",
              SessionEdit_ApplyPendingAfterRun(&profiles.items[1]) == 1 && !RemovalQueued(&profiles.items[1]));
    }
    RecycleWill(REMOVE_DONE, NULL);
    SessionStore_Free(&set);
}

typedef struct ConcurrentRead {
    const ProfileList *profiles;
    HANDLE start;
    BOOL valid;
} ConcurrentRead;

static DWORD WINAPI ReadFixtures(void *param)
{
    ConcurrentRead *read = (ConcurrentRead *)param;
    int pass;
    read->valid = TRUE;
    WaitForSingleObject(read->start, INFINITE);
    for (pass = 0; pass < 12; pass++) {
        SessionSet set;
        int r;
        if (!SessionStore_LoadProfiles(&set, read->profiles)) { read->valid = FALSE; break; }
        if (set.rowCount != 2 || set.groupCount != 2 || set.entryCount != 2) read->valid = FALSE;
        for (r = 0; r < set.rowCount; r++)
            if (set.rows[r].group < 0 || set.rows[r].group >= set.groupCount || set.rows[r].entry[0] < 0)
                read->valid = FALSE;
        SessionStore_Free(&set);
    }
    return 0;
}

/* A read cancelled while it goes: the cancel handle is a change
 * notification on the state folder, which the read signals itself when it
 * logs where the first profile's entries are. */
static void TestCancelledRead(void)
{
    enum { SESSIONS = 32 };
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], state[MAX_PATH], id[SESSION_ID_CCH];
    HANDLE change;
    BOOL ready, cancelled;
    int i;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(&profiles.items[0], L"Cancel", L"cancel\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            Util_StateDir(state, ARRAYSIZE(state));
    for (i = 0; ready && i < SESSIONS; i++)
        ready = SUCCEEDED(StringCchPrintfW(id, ARRAYSIZE(id), L"%08x-5555-4555-8555-555555555555", i)) && WriteEntry(entries, id, "");
    Check("cancelled read fixtures ready", ready);
    if (!ready) return;
    change = FindFirstChangeNotificationW(state, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    Check("cancel notification created", change != INVALID_HANDLE_VALUE);
    if (change == INVALID_HANDLE_VALUE) return;
    ready = WaitForSingleObject(change, 0) == WAIT_TIMEOUT;
    cancelled = !SessionStore_LoadProfilesCancel(&set, &profiles, change);
    Check("a read cancelled while it goes publishes nothing",
          ready && cancelled && WaitForSingleObject(change, 0) == WAIT_OBJECT_0 && !set.entries && !set.rows && !set.groups && set.rowCount == 0);
    if (!cancelled) SessionStore_Free(&set);
    FindCloseChangeNotification(change);
    Check("the same read not cancelled is complete", SessionStore_LoadProfiles(&set, &profiles) && set.rowCount == SESSIONS);
    SessionStore_Free(&set);
}

static void TestConcurrentReads(const ProfileList *profiles)
{
    HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL), threads[2] = { NULL, NULL };
    ConcurrentRead read[2];
    SessionSet set;
    int i;
    Check("cancellation event created", start != NULL);
    if (!start) return;
    SetEvent(start);
    Check("a read cancelled before it starts returns no partial data", !SessionStore_LoadProfilesCancel(&set, profiles, start) &&
          !set.entries && !set.rows && !set.groups && set.rowCount == 0);
    ResetEvent(start);
    for (i = 0; i < 2; i++) {
        read[i].profiles = profiles;
        read[i].start = start;
        read[i].valid = FALSE;
        threads[i] = CreateThread(NULL, 0, ReadFixtures, &read[i], 0, NULL);
        Check("concurrent fixture reader created", threads[i] != NULL);
    }
    SetEvent(start);
    for (i = 0; i < 2; i++) {
        if (threads[i]) {
            WaitForSingleObject(threads[i], INFINITE);
            Check("concurrent reader keeps its own rows and groups", read[i].valid);
            CloseHandle(threads[i]);
        }
    }
    CloseHandle(start);
}

/* Wait on the window queue, with a bounded test deadline. */
static BOOL WaitForSnapshot(HWND window, BOOL apply)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    for (;;) {
        ULONGLONG now;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.hwnd == window && message.message == WM_APP_SESSIONS) {
                SessionsView_Reload();
                continue;
            }
            if (message.hwnd == window && message.message == WM_APP_SESSIONS_READY) {
                SessionsView_Ready(apply);
                return TRUE;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        now = GetTickCount64();
        if (now >= deadline) return FALSE;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static BOOL WaitForViewChange(HWND window, HWND tree, int expected, BOOL notificationOnly)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    for (;;) {
        ULONGLONG now;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.hwnd == window && message.message == WM_APP_SESSIONS) {
                SessionsView_Reload();
                if (notificationOnly) return TRUE;
                continue;
            }
            if (message.hwnd == window && message.message == WM_APP_SESSIONS_READY) {
                SessionsView_Ready(TRUE);
                if (!notificationOnly && TreeView_GetCount(tree) == (UINT)expected) return TRUE;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        now = GetTickCount64();
        if (now >= deadline) return FALSE;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static void TestBackgroundView(const ProfileList *profiles)
{
    static const int ids[] = { IDC_S_PROFILES, IDC_S_TREE, IDC_S_SEARCH, IDC_S_ARCHIVED, IDC_S_DETAILS };
    static const WCHAR *const classes[] = { L"LISTBOX", WC_TREEVIEWW, L"EDIT", L"BUTTON", L"STATIC" };
    HWND window, tree;
    ClaudePackage package;
    ProfileList newer;
    INITCOMMONCONTROLSEX controls = { sizeof controls, ICC_TREEVIEW_CLASSES };
    DWORD handlesBefore = 0, handlesAfter = 0, handlesAgain = 0;
    int i;
    /* The view makes what waits with sessionedit.c itself: never a removal here (the real Recycle Bin). */
    for (i = 0; i < profiles->count; i++)
        if (RemovalQueued(&profiles->items[i])) {
            Check("no removal waits for the view's profiles", FALSE);
            return;
        }
    ZeroMemory(&package, sizeof package);
    g_hInst = GetModuleHandleW(NULL);
    InitCommonControlsEx(&controls);
    window = CreateWindowExW(0, L"STATIC", L"Session test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, g_hInst, NULL);
    Check("background view fixture window created", window != NULL);
    if (!window) return;
    for (i = 0; i < (int)ARRAYSIZE(ids); i++) {
        DWORD style = WS_CHILD;
        if (ids[i] == IDC_S_PROFILES) style |= LBS_OWNERDRAWFIXED | LBS_NOTIFY;
        CreateWindowExW(0, classes[i], L"", style, i * 100, 0, 100, 300, window, (HMENU)(INT_PTR)ids[i], g_hInst, NULL);
    }
    tree = GetDlgItem(window, IDC_S_TREE);
    SessionsView_Init(window);
    SessionsView_SetProfiles(profiles);
    SessionsView_Warm(profiles);
    Check("background snapshot completes through a window event", WaitForSnapshot(window, TRUE));
    Check("background loading does not fill hidden controls", TreeView_GetCount(tree) == 0);
    SessionsView_Enter(&package, profiles->items[0].folder);
    Check("cached snapshot renders both fixture conversations", TreeView_GetCount(tree) == 4);
    Check("account switch fixture config written", SaveIn(profiles->items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"account-old\"}"));
    Check("config change reloads the active account through a window event", WaitForViewChange(window, tree, 2, FALSE));
    Check("account fixture config switched back", SaveIn(profiles->items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"account-a\"}"));
    Check("restored account reloads without a focus change", WaitForViewChange(window, tree, 4, FALSE));
    {
        WCHAR projects[MAX_PATH], transcript[MAX_PATH];
        HANDLE file;
        DWORD written;
        BOOL ready = SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) &&
                     SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\fixture\\%s.jsonl", projects, g_scratchId));
        file = ready ? CreateFileW(transcript, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        ready = file != INVALID_HANDLE_VALUE && WriteFile(file, "{}\n", 3, &written, NULL) && written == 3;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Check("existing transcript fixture appended", ready);
        Check("transcript append produces a sessions window event", ready && WaitForViewChange(window, tree, 0, TRUE));
        Check("transcript write snapshot completes", WaitForSnapshot(window, TRUE));
    }
    SessionsView_Leave();
    newer = *profiles;
    newer.count = 0;
    SessionsView_SetProfiles(&newer);
    Check("ready can defer a snapshot during a modal", WaitForSnapshot(window, FALSE));
    Check("deferred snapshot leaves existing control contents", TreeView_GetCount(tree) == 4);
    SessionsView_Ready(TRUE);
    SessionsView_Enter(&package, NULL);
    Check("deferred empty snapshot replaces the cache on return", TreeView_GetCount(tree) == 0);
    SessionsView_SetProfiles(profiles);
    SessionsView_SetProfiles(&newer);
    Check("rapid profile changes publish the latest snapshot", WaitForSnapshot(window, TRUE));
    Check("obsolete background data does not return to the tree", TreeView_GetCount(tree) == 0);
    SessionsView_SetProfiles(profiles);
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    SessionsView_Destroy();            /* cancel a read while the window is still alive */
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    /* Its reader and watcher threads, their events and folder notifications. */
    Check("background shutdown closes its thread and event handles", handlesAfter < handlesBefore);
    /* A whole view lifetime again, what it opens first made once already: every handle it opened is closed. */
    SessionsView_Init(window);
    SessionsView_SetProfiles(profiles);
    SessionsView_Warm(profiles);
    Check("second background snapshot completes", WaitForSnapshot(window, TRUE));
    SessionsView_Destroy();
    GetProcessHandleCount(GetCurrentProcess(), &handlesAgain);
    Check("a view's lifetime leaves no handle open", handlesAgain == handlesAfter);
    DestroyWindow(window);
    Check("background fixture does not create logical stock directory", !Util_DirExists(profiles->items[0].dataDir));
}

static void TestIndexedSessions(void)
{
    enum { ROWS = 384 };
    ProfileList profiles;
    SessionSet first, second;
    WCHAR entries[MAX_PATH], id[SESSION_ID_CCH], file[MAX_PATH];
    char json[512];
    int p, r;
    BOOL ready = TRUE, valid = TRUE;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (p = 0; p < profiles.count && ready; p++) {
        Profile *profile = &profiles.items[p];
        ready = SetProfile(profile, p ? L"Index-B" : L"Index-A", p ? L"index\\B" : L"index\\A", FALSE) &&
                Profiles_ResolveStorage(profile, NULL, NULL) &&
                SaveIn(profile->storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"indexed\"}") &&
                Join(profile->storageDir, L"claude-code-sessions\\indexed\\organization", entries, ARRAYSIZE(entries));
        for (r = 0; r < ROWS && ready; r++) {
            StringCchPrintfW(id, ARRAYSIZE(id), p ? L"%08X-4444-4444-8444-444444444444" : L"%08x-4444-4444-8444-444444444444", r);
            ready = SUCCEEDED(StringCchPrintfA(json, sizeof json,
                "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"title\":\"Row %d\","
                "\"cwd\":\"C:\\\\Fixture\\\\Project%d\",\"lastActivityAt\":%d}", id, id, r, r, r)) &&
                SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) && Save(file, json);
        }
        if (ready) ready = SaveIn(entries, L"local_unicode.json",
            p ? "{\"sessionId\":\"local_unicode\",\"cliSessionId\":\"\\u00c4session\",\"cwd\":\"C:\\\\Unicode\"}"
              : "{\"sessionId\":\"local_unicode\",\"cliSessionId\":\"\\u00e4session\",\"cwd\":\"C:\\\\Unicode\"}");
    }
    Check("indexed session fixtures created", ready);
    if (!ready) return;
    Check("indexed session load completes", SessionStore_LoadProfiles(&first, &profiles));
    Check("case variants and Unicode ids merge across profiles", first.rowCount == ROWS + 1 && first.entryCount == (ROWS + 1) * 2);
    Check("indexed sessions retain hundreds of independent project groups", first.groupCount == ROWS + 1);
    for (r = 0; r < first.rowCount; r++)
        if (first.rows[r].entry[0] < 0 || first.rows[r].entry[1] < 0 || first.rows[r].group < 0 ||
            first.rows[r].group >= first.groupCount) valid = FALSE;
    Check("each indexed conversation keeps both profile entries after sorting", valid);
    Check("indexed reload completes", SessionStore_LoadProfiles(&second, &profiles));
    valid = first.rowCount == second.rowCount;
    for (r = 0; valid && r < first.rowCount; r++)
        if (wcscmp(first.rows[r].key, second.rows[r].key) != 0 || first.rows[r].group != second.rows[r].group) valid = FALSE;
    Check("indexed reload preserves deterministic row and group order", valid);
    SessionStore_Free(&first);
    SessionStore_Free(&second);
}

static void TestGroupAliases(void)
{
    static const WCHAR *const directories[] = {
        L"C:\\Workspace\\\x00c9quipe", L"c:\\workspace\\\x00e9quipe\\", L"C:\\Build", L"c:\\build///", L"", L""
    };
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], file[MAX_PATH], ids[6][SESSION_ID_CCH];
    char quoted[MAX_PATH * 6 + 4], json[2048];
    BOOL ready, valid = TRUE;
    int item, rows[6];
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = SetProfile(&profiles.items[0], L"GroupAliases", L"groups\\aliases", FALSE) &&
            Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            SaveIn(profiles.items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"groups\"}") &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\groups\\organization", entries, ARRAYSIZE(entries));
    for (item = 0; ready && item < (int)ARRAYSIZE(directories); item++) {
        StringCchPrintfW(ids[item], ARRAYSIZE(ids[item]), L"%08x-8888-4888-8888-888888888888", item);
        ready = Core_JsonQuote(directories[item], quoted, sizeof quoted) &&
                SUCCEEDED(StringCchPrintfA(json, sizeof json,
                    "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,\"lastActivityAt\":%d}",
                    ids[item], ids[item], quoted, item)) &&
                SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, ids[item])) && Save(file, json);
    }
    Check("Unicode and separator group fixtures created", ready);
    if (!ready) return;
    Check("group alias snapshot loads", SessionStore_LoadProfiles(&set, &profiles));
    Check("Unicode case, trailing separators and unknown folders retain three distinct groups", set.groupCount == 3 && set.rowCount == 6);
    for (item = 0; item < (int)ARRAYSIZE(rows); item++) {
        rows[item] = FindRow(&set, ids[item]);
        if (rows[item] < 0) valid = FALSE;
    }
    Check("group alias rows retain all session ids", valid);
    if (valid) {
        Check("Unicode folder case variants share their group", set.rows[rows[0]].group == set.rows[rows[1]].group);
        Check("forward and back trailing separators share their group", set.rows[rows[2]].group == set.rows[rows[3]].group);
        Check("empty working folders share the unknown group", set.rows[rows[4]].group == set.rows[rows[5]].group);
        Check("groups retain first-discovery order before activity sorting",
              set.rows[rows[0]].group == 0 && set.rows[rows[2]].group == 1 && set.rows[rows[4]].group == 2);
    }
    SessionStore_Free(&set);
}

/* Environment variable `name` kept to be put back: `*kept` NULL when it is
 * not set. FALSE when it could not be kept. */
static BOOL KeepVariable(const WCHAR *name, WCHAR **kept)
{
    DWORD cch = GetEnvironmentVariableW(name, NULL, 0);
    *kept = NULL;
    if (!cch) return TRUE;
    *kept = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)cch * sizeof(WCHAR));
    return *kept && GetEnvironmentVariableW(name, *kept, cch) < cch;
}

int wmain(int argc, WCHAR **argv)
{
    ProfileList profiles;
    WCHAR local[MAX_PATH], projects[MAX_PATH], temporary[MAX_PATH];
    WCHAR *previousConfig = NULL, *previousTemporary = NULL;
    BOOL ready = FALSE, configChanged = FALSE, temporaryChanged = FALSE, kept;
    /* TestPendingProcesses starts copies of this exe as queue workers. */
    if (argc == 6 && wcscmp(argv[1], L"--queue-worker") == 0) return QueueWorker(argv[2], argv[3], argv[4], _wtoi(argv[5]));
    kept = KeepVariable(L"CLAUDE_CONFIG_DIR", &previousConfig);
    kept = KeepVariable(L"CLAUDE_CODE_TMPDIR", &previousTemporary) && kept;
    if (!kept) {
        if (previousConfig) HeapFree(GetProcessHeap(), 0, previousConfig);
        if (previousTemporary) HeapFree(GetProcessHeap(), 0, previousTemporary);
        printf("Session tests: could not preserve CLAUDE_CONFIG_DIR and CLAUDE_CODE_TMPDIR.\n");
        return 1;
    }
    RecycleWill(REMOVE_DONE, NULL);
    Check("fixture root created", CreateRoot());
    if (!g_failures) {
        WCHAR state[MAX_PATH];
        /* The log, the queued changes and Claude Code's temporary folder stay
         * in the fixture, not in the user's folders. */
        Check("private state folder created", FixturePath(L"state", state, ARRAYSIZE(state)) && MakeDir(state));
        Util_SetStateDir(state);
        temporaryChanged = FixturePath(L"tmp", temporary, ARRAYSIZE(temporary)) && MakeDir(temporary) &&
                           SetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", temporary);
        Check("private Claude Code temporary folder set", temporaryChanged);
        ready = temporaryChanged && PrepareProfiles(&profiles, local, projects);
        configChanged = ready;
        Check("session fixtures ready", ready);
        if (ready) {
            TestResolution(local);
            TestSessions(&profiles, projects);
            TestCopyRemoval(&profiles, projects);
            TestSettings(&profiles.items[0], local);
            TestNewestAccount(projects);
            TestScratchCopyFailure(&profiles, projects);
            TestPendingProcesses();
            TestPendingApplication(&profiles);
            TestPendingQueueFile();
            TestEntryDetails();
            TestPendingChanges();
            TestReadonlyPending(&profiles);
            TestRemovals();
            TestDeletionList(&profiles, projects);
            TestOtherTranscripts(projects);
            TestDeleteEverywhere(projects);
            TestEntryCounts();
            TestLongPaths(projects);
            TestCancelledRead();
            TestConcurrentReads(&profiles);
            TestBackgroundView(&profiles);
            TestIndexedSessions();
            TestGroupAliases();
        }
        Check("nothing outside the fixture was given to the Recycle Bin", !g_recycle.escaped);
        StopStartedClaude();
        if (configChanged) Check("process environment restored", SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", previousConfig));
        if (temporaryChanged) Check("Claude Code's temporary folder restored", SetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", previousTemporary));
        Check("fixture tree removed", RemoveFixture(g_root));
    }
    if (previousConfig) HeapFree(GetProcessHeap(), 0, previousConfig);
    if (previousTemporary) HeapFree(GetProcessHeap(), 0, previousTemporary);
    printf("Session tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
