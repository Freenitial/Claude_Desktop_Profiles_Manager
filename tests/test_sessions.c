/* Session storage checks using private temporary profiles and transcripts. */
#include "../src/app.h"
#include <objbase.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;
static WCHAR g_root[MAX_PATH];
static DWORD g_fixtureTag;
static const WCHAR *const g_family = L"Fixture_package";
static const WCHAR *const g_scratchId = L"11111111-1111-4111-8111-111111111111";
static const WCHAR *const g_projectId = L"22222222-2222-4222-8222-222222222222";
static const WCHAR *const g_oldId = L"33333333-3333-4333-8333-333333333333";

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

static BOOL MakeDir(const WCHAR *path)
{
    return Core_PathUnder(path, g_root) && Util_EnsureDir(path);
}

static BOOL Save(const WCHAR *path, const char *text)
{
    WCHAR parent[MAX_PATH], *slash;
    HANDLE h;
    DWORD bytes = (DWORD)strlen(text), written = 0;
    BOOL ok;
    if (!Core_PathUnder(path, g_root) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path))) return FALSE;
    slash = wcsrchr(parent, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    if (!MakeDir(parent)) return FALSE;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(h, text, bytes, &written, NULL) && written == bytes;
    CloseHandle(h);
    return ok;
}

static BOOL SaveIn(const WCHAR *dir, const WCHAR *name, const char *text)
{
    WCHAR path[MAX_PATH];
    return Join(dir, name, path, ARRAYSIZE(path)) && Save(path, text);
}

static BOOL ReadString(const WCHAR *path, const char *key, WCHAR *out, size_t cch)
{
    DWORD len;
    size_t n;
    const char *v;
    char *json = Util_ReadFile(path, 65536, FALSE, &len);
    BOOL ok = FALSE;
    out[0] = 0;
    if (json) {
        ok = Core_JsonMember(json, len, key, &v, &n) && Core_JsonString(v, n, out, cch);
        HeapFree(GetProcessHeap(), 0, json);
    }
    return ok;
}

static BOOL HasMember(const WCHAR *path, const char *key)
{
    DWORD len;
    size_t n;
    const char *v;
    char *json = Util_ReadFile(path, 65536, FALSE, &len);
    BOOL found = FALSE;
    if (json) {
        found = Core_JsonMember(json, len, key, &v, &n);
        HeapFree(GetProcessHeap(), 0, json);
    }
    return found;
}

/* Refuse reparse points and paths outside the unique fixture root. */
static BOOL RemoveFixture(const WCHAR *path)
{
    WCHAR full[MAX_PATH], pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW fd;
    DWORD n = GetFullPathNameW(path, ARRAYSIZE(full), full, NULL), attr;
    HANDLE h;
    BOOL ok = TRUE;
    if (!n || n >= ARRAYSIZE(full) ||
        (!Core_PathEquals(full, g_root) && !Core_PathUnder(full, g_root))) return FALSE;
    attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(full);
    if (!Join(full, L"*", pattern, ARRAYSIZE(pattern))) return FALSE;
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (!Join(full, fd.cFileName, child, ARRAYSIZE(child)) || !RemoveFixture(child)) ok = FALSE;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return ok && RemoveDirectoryW(full);
}

static BOOL CreateRoot(void)
{
    WCHAR temp[MAX_PATH], path[MAX_PATH];
    GUID id;
    DWORD n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp) || FAILED(CoCreateGuid(&id)) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%scdm-session-%08lx-%08lx", temp,
                               GetCurrentProcessId(), id.Data1))) return FALSE;
    n = GetFullPathNameW(path, ARRAYSIZE(g_root), g_root, NULL);
    g_fixtureTag = id.Data1;
    return n > 0 && n < ARRAYSIZE(g_root) && CreateDirectoryW(g_root, NULL);
}

static BOOL SetProfile(Profile *p, const WCHAR *label, const WCHAR *leaf, BOOL stock)
{
    ZeroMemory(p, sizeof *p);
    p->isStock = stock;
    return SUCCEEDED(StringCchPrintfW(p->folder, ARRAYSIZE(p->folder), L"Fixture-%08lx-%08lx-%s", GetCurrentProcessId(), g_fixtureTag, label)) &&
           SUCCEEDED(StringCchCopyW(p->name, ARRAYSIZE(p->name), label)) &&
           FixturePath(leaf, p->dataDir, ARRAYSIZE(p->dataDir));
}

static BOOL WriteSession(const WCHAR *entries, const WCHAR *projects, const WCHAR *id, const WCHAR *cwd)
{
    WCHAR file[MAX_PATH];
    char quoted[MAX_PATH * 6 + 4], json[4096];
    if (!Core_JsonQuote(cwd, quoted, sizeof quoted) ||
        FAILED(StringCchPrintfA(json, sizeof json,
            "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,"
            "\"title\":\"Fixture session\",\"lastActivityAt\":123}\n", id, id, quoted)) ||
        FAILED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) || !Save(file, json)) return FALSE;
    return SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"%ls\",\"cwd\":%s,\"type\":\"user\"}\n", id, quoted)) &&
           SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\fixture\\%s.jsonl", projects, id)) && Save(file, json);
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
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL found = FALSE;
    if (!Join(projects, L"*", pattern, ARRAYSIZE(pattern))) return FALSE;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s\\%s.jsonl", projects, fd.cFileName, id)) && Util_FileExists(out)) {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static BOOL CheckCopy(SessionSet *s, int row, int target, const WCHAR *projects, WCHAR *id, WCHAR *path, WCHAR *cwd)
{
    WCHAR error[512], readId[SESSION_ID_CCH], physical[MAX_PATH], marker[MAX_PATH];
    BOOL ok = SessionEdit_CopyConversation(s, row, target, id, SESSION_ID_CCH, error, ARRAYSIZE(error));
    Check("scratch copy: completes", ok);
    if (!ok) {
        wprintf(L"        %s\n", error);
        return FALSE;
    }
    Check("scratch copy: distinct conversation id", Core_IsSessionId(id) && wcscmp(id, s->rows[row].key) != 0);
    ok = FindCopiedTranscript(projects, id, path, MAX_PATH);
    Check("scratch copy: transcript exists", ok);
    if (!ok) return FALSE;
    Check("scratch copy: transcript uses new id", ReadString(path, "sessionId", readId, ARRAYSIZE(readId)) && wcscmp(readId, id) == 0);
    ok = ReadString(path, "cwd", cwd, MAX_PATH);
    Check("scratch copy: transcript has working directory", ok);
    if (!ok) return FALSE;
    Check("scratch copy: transcript uses target logical path", Core_PathUnder(cwd, s->source[target].scratchDir));
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
    WCHAR id[SESSION_ID_CCH], path[MAX_PATH], cwd[MAX_PATH];
    int r, i;
    BOOL loaded = SessionStore_LoadProfiles(&s, profiles);
    Check("session load succeeds", loaded);
    if (!loaded) return;
    Check("virtual stock: signed in", s.source[0].signedIn);
    Check("virtual stock: entries directory found", s.source[0].found);
    Check("virtual stock: exactly two active-account sessions", ProfileRows(&s, 0) == 2 && s.entryCount == 2);
    Check("virtual stock: old account ignored", FindRow(&s, g_oldId) < 0);
    Check("empty secondary: signed in", s.source[1].signedIn);
    Check("empty secondary: entries directory found", s.source[1].found);
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
            WCHAR storage[MAX_PATH], error[512];
            StringCchCopyW(s.rows[r].key, ARRAYSIZE(s.rows[r].key), id);
            StringCchCopyW(s.rows[r].transcriptPath, ARRAYSIZE(s.rows[r].transcriptPath), path);
            StringCchCopyW(s.rows[r].cwd, ARRAYSIZE(s.rows[r].cwd), cwd);
            s.groups[original.group].scratchOf = 1;
            CheckCopy(&s, r, 0, projects, id, path, cwd);
            StringCchCopyW(storage, ARRAYSIZE(storage), s.profiles.items[0].storageDir);
            s.profiles.items[0].storageDir[0] = 0;
            Check("unresolved target: scratch copy fails safely", !SessionEdit_CopyConversation(&s, r, 0, id, ARRAYSIZE(id), error, ARRAYSIZE(error)));
            Check("unresolved target: logical stock stays absent", !Util_DirExists(s.profiles.items[0].dataDir));
            StringCchCopyW(s.profiles.items[0].storageDir, ARRAYSIZE(s.profiles.items[0].storageDir), storage);
            s.rows[r] = original;
            s.groups[original.group].scratchOf = originalOwner;
        }
    }
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

int main(void)
{
    ProfileList profiles;
    WCHAR local[MAX_PATH], projects[MAX_PATH];
    WCHAR *previous = NULL;
    DWORD previousCch = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", NULL, 0);
    BOOL ready = FALSE, changed = FALSE;
    if (previousCch) {
        previous = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)previousCch * sizeof(WCHAR));
        if (!previous || GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", previous, previousCch) >= previousCch) {
            if (previous) HeapFree(GetProcessHeap(), 0, previous);
            printf("Session tests: could not preserve CLAUDE_CONFIG_DIR.\n");
            return 1;
        }
    }
    Check("fixture root created", CreateRoot());
    if (!g_failures) {
        ready = PrepareProfiles(&profiles, local, projects);
        changed = ready;
        Check("session fixtures ready", ready);
        if (ready) {
            TestResolution(local);
            TestSessions(&profiles, projects);
            TestSettings(&profiles.items[0], local);
        }
        if (changed) Check("process environment restored", SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", previous));
        Check("fixture tree removed", RemoveFixture(g_root));
    }
    if (previous) HeapFree(GetProcessHeap(), 0, previous);
    printf("Session tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
