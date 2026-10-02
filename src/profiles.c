/*
 * A profile is a folder directly under %APPDATA%: "Claude" is the one the
 * regular Claude icon opens, every other one is "Claude-<name>". Folders must
 * stay directly under %APPDATA%: Claude's Cowork VM service looks for its disk
 * image at %APPDATA%\<folder>\vm_bundles.
 *
 * The folder name never changes once created (Claude stores absolute paths
 * inside a profile), so renaming only changes the display name kept in
 * HKCU\Software\Claude Desktop Profiles Manager\Profiles\<folder>.
 */
#include "app.h"
#include <stdlib.h>

#define VALUE_NAME            L"Name"             /* under the profile's key */
#define VALUE_COLOR           L"Color"
#define VALUE_DEFAULT_PROFILE L"DefaultProfile"   /* under REG_ROOT */

static void ProfileKey(const WCHAR *folder, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, REG_PROFILES L"\\%s", folder);
}

/* A missing stock folder is virtualized by the package. An inaccessible
 * Roaming folder is not evidence of that absence, so it cannot select a
 * different data store. Resolving a path never creates a profile folder. */
BOOL Profiles_ResolveStorage(Profile *p, const WCHAR *localAppData, const WCHAR *family)
{
    WCHAR parent[MAX_PATH], *slash;
    DWORD attributes;
    if (!p) return FALSE;
    p->storageDir[0] = 0;
    if (!p->dataDir[0]) return FALSE;
    if (!p->isStock)
        return SUCCEEDED(StringCchCopyW(p->storageDir, ARRAYSIZE(p->storageDir), p->dataDir));
    switch (Util_QueryPath(p->dataDir, &attributes)) {
    case PATH_PRESENT:
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
               SUCCEEDED(StringCchCopyW(p->storageDir, ARRAYSIZE(p->storageDir), p->dataDir));
    case PATH_UNREACHABLE:
        return FALSE;
    case PATH_MISSING:
        break;
    }
    if (!localAppData || !*localAppData || !family || !*family) return FALSE;
    if (FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), p->dataDir))) return FALSE;
    parent[Core_TrimmedPathLength(parent)] = 0;
    slash = wcsrchr(parent, L'\\');
    if (!slash || slash == parent) return FALSE;
    *slash = 0;
    if (!Util_DirExists(parent) ||
        !Core_PackageCachePath(localAppData, family, L"Roaming", STOCK_FOLDER, p->storageDir, ARRAYSIZE(p->storageDir))) {
        p->storageDir[0] = 0;
        return FALSE;
    }
    return TRUE;
}

static void Fill(Profile *p, const WCHAR *appData, const WCHAR *folder, BOOL isStock, const ClaudePackage *pkg)
{
    WCHAR key[MAX_PATH], localAppData[MAX_PATH];
    DWORD color;
    ZeroMemory(p, sizeof *p);
    StringCchCopyW(p->folder, ARRAYSIZE(p->folder), folder);
    if (FAILED(StringCchPrintfW(p->dataDir, ARRAYSIZE(p->dataDir), L"%s\\%s", appData, folder))) p->dataDir[0] = 0;
    p->isStock = isStock;
    if (isStock && pkg->found && Util_LocalAppData(localAppData, ARRAYSIZE(localAppData)))
        Profiles_ResolveStorage(p, localAppData, pkg->family);
    else
        Profiles_ResolveStorage(p, NULL, NULL);
    ProfileKey(folder, key, ARRAYSIZE(key));
    if (!Util_RegGetString(HKEY_CURRENT_USER, key, VALUE_NAME, p->name, ARRAYSIZE(p->name)) || !p->name[0])
        StringCchCopyW(p->name, ARRAYSIZE(p->name), isStock ? STOCK_DEFAULT_NAME : folder + wcslen(PROFILE_PREFIX));
    p->color = (Util_RegGetDword(HKEY_CURRENT_USER, key, VALUE_COLOR, &color) && color < PALETTE_SIZE) ? (int)color : -1;
}

/* The profile's name and color. */
static void DeleteProfileKey(const WCHAR *folder)
{
    WCHAR key[MAX_PATH];
    LSTATUS status;
    ProfileKey(folder, key, ARRAYSIZE(key));
    if ((status = Util_RegDeleteTree(HKEY_CURRENT_USER, key)) != ERROR_SUCCESS)
        Util_Log(L"could not remove the name and color of %s (error %ld)", folder, status);
}

/* What the manager keeps for a profile that is gone: its name and color, the
 * session changes queued for it and the default setting when it names it. A
 * new profile of that name must not inherit them. */
static void ForgetProfile(const WCHAR *folder)
{
    WCHAR pending[MAX_PATH], defaultFolder[FOLDER_CCH];
    Profile gone;
    LSTATUS status;
    DeleteProfileKey(folder);
    ZeroMemory(&gone, sizeof gone);
    StringCchCopyW(gone.folder, ARRAYSIZE(gone.folder), folder);
    if (SessionStore_PendingPath(&gone, pending, ARRAYSIZE(pending)) && !DeleteFileW(pending) &&
        GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
        Util_Log(L"could not delete %s (error %lu)", pending, GetLastError());
    if (Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, VALUE_DEFAULT_PROFILE, defaultFolder, ARRAYSIZE(defaultFolder)) &&
        Core_EqualsI(defaultFolder, folder) &&
        (status = Util_RegDeleteValue(HKEY_CURRENT_USER, REG_ROOT, VALUE_DEFAULT_PROFILE)) != ERROR_SUCCESS)
        Util_Log(L"could not stop %s being the default profile (error %ld)", folder, status);
}

/* Registry entries whose folder was deleted outside the manager, and what
 * goes with them (ForgetProfile). Only a folder that is positively not there
 * counts: a %APPDATA% on an unreachable network share must not erase every
 * name and color. */
static void PruneMissing(const WCHAR *appData)
{
    WCHAR names[MAX_PROFILES * 2][FOLDER_CCH], dir[MAX_PATH];
    HKEY root;
    DWORD i, n = 0, cch;
    if (!Util_DirExists(appData)) return;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PROFILES, 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (i = 0; n < ARRAYSIZE(names); i++) {
        LSTATUS rc;
        cch = ARRAYSIZE(names[n]);
        rc = RegEnumKeyExW(root, i, names[n], &cch, NULL, NULL, NULL, NULL);
        if (rc == ERROR_MORE_DATA) continue;   /* not a name of ours */
        if (rc != ERROR_SUCCESS) break;        /* the end, or the key was deleted meanwhile */
        n++;
    }
    RegCloseKey(root);
    for (i = 0; i < n; i++) {
        if (Core_EqualsI(names[i], STOCK_FOLDER)) continue;
        if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, names[i])) ||
            Util_QueryPath(dir, NULL) != PATH_MISSING)
            continue;
        ForgetProfile(names[i]);
    }
}

/* Profiles without a color get the first free one; it is saved so it stays
 * put, but only while Claude Desktop Profiles Manager is installed (loading
 * never recreates its registry key after an uninstall). */
static void AssignColors(ProfileList *list)
{
    BOOL used[PALETTE_SIZE] = { 0 }, persist = FALSE, persistKnown = FALSE;
    WCHAR key[MAX_PATH];
    int i, color;
    for (i = 0; i < list->count; i++)
        if (list->items[i].color >= 0) used[list->items[i].color] = TRUE;
    for (i = 0; i < list->count; i++) {
        if (list->items[i].color >= 0) continue;
        for (color = 0; color < PALETTE_SIZE && used[color]; color++) {}
        if (color == PALETTE_SIZE) color = i % PALETTE_SIZE;
        used[color] = TRUE;
        list->items[i].color = color;
        if (!persistKnown) {
            persist = Install_IsRegistered();
            persistKnown = TRUE;
        }
        if (!persist) continue;
        ProfileKey(list->items[i].folder, key, ARRAYSIZE(key));
        if (!Util_RegSetDword(HKEY_CURRENT_USER, key, VALUE_COLOR, (DWORD)color))
            Util_Log(L"could not save the color of %s (error %lu)", list->items[i].folder, GetLastError());
    }
}

static int __cdecl CompareProfiles(const void *a, const void *b)
{
    const Profile *first = (const Profile *)a, *second = (const Profile *)b;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                           first->name, -1, second->name, -1, NULL, NULL, 0) - CSTR_EQUAL;
}

/* `pkg` locates Main's data when Claude keeps it in its package (NULL: looked up here). */
void Profiles_Load(ProfileList *list, const ClaudePackage *pkg)
{
    WCHAR appData[MAX_PATH], pattern[MAX_PATH], dir[MAX_PATH], key[MAX_PATH], marker[MAX_PATH], defaultFolder[FOLDER_CCH];
    WIN32_FIND_DATAW entry;
    ClaudePackage found;
    HANDLE search;
    int defaultIndex;
    static BOOL reportedCap;

    ZeroMemory(list, sizeof *list);
    StringCchCopyW(list->defaultFolder, ARRAYSIZE(list->defaultFolder), STOCK_FOLDER);
    if (!Util_AppData(appData, ARRAYSIZE(appData))) return;
    if (!pkg) {
        Claude_FindPackage(&found);
        pkg = &found;
    }

    Fill(&list->items[list->count++], appData, STOCK_FOLDER, TRUE, pkg);

    if (SUCCEEDED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\" PROFILE_PREFIX L"*", appData))) {
        search = FindFirstFileExW(pattern, FindExInfoBasic, &entry, FindExSearchLimitToDirectories, NULL, 0);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (!Core_IsProfileFolder(entry.cFileName)) continue;
                if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, entry.cFileName)) ||
                    FAILED(StringCchPrintfW(marker, ARRAYSIZE(marker), L"%s\\Local State", dir)))
                    continue;
                if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && Util_IsDirectoryLink(dir)) {
                    /* A profile moved elsewhere and linked back counts once it reaches a real one. */
                    if (!Util_FileExists(marker)) continue;
                } else if (!Util_FileExists(marker)) {
                    ProfileKey(entry.cFileName, key, ARRAYSIZE(key));
                    if (!Util_RegKeyExists(HKEY_CURRENT_USER, key)) continue;   /* neither a Chromium profile nor one created here */
                }
                if (list->count >= MAX_PROFILES) {
                    if (!reportedCap)
                        Util_Log(L"more than %d profiles: %s and later ones are not listed", MAX_PROFILES, entry.cFileName);
                    reportedCap = TRUE;
                    break;
                }
                Fill(&list->items[list->count++], appData, entry.cFileName, FALSE, pkg);
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }
    }

    PruneMissing(appData);
    AssignColors(list);
    if (list->count > 2)
        qsort(list->items + 1, (size_t)(list->count - 1), sizeof(Profile), CompareProfiles);

    if (Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, VALUE_DEFAULT_PROFILE, defaultFolder, ARRAYSIZE(defaultFolder)) &&
        (defaultIndex = Profiles_Find(list, defaultFolder)) >= 0)
        StringCchCopyW(list->defaultFolder, ARRAYSIZE(list->defaultFolder), list->items[defaultIndex].folder);

    Claude_UpdateRunning(list);
}

int Profiles_Find(const ProfileList *list, const WCHAR *folder)
{
    int i;
    if (!folder || !*folder) return -1;
    for (i = 0; i < list->count; i++)
        if (Core_EqualsI(list->items[i].folder, folder)) return i;
    return -1;
}

/* The profile claude:// links open when no Claude runs; -1 for an empty list. */
int Profiles_DefaultIndex(const ProfileList *list)
{
    int i = Profiles_Find(list, list->defaultFolder);
    return i >= 0 ? i : list->count > 0 ? 0 : -1;
}

static BOOL WriteNameAndColor(const WCHAR *folder, const WCHAR *name, int color)
{
    WCHAR key[MAX_PATH];
    ProfileKey(folder, key, ARRAYSIZE(key));
    return Util_RegSetString(HKEY_CURRENT_USER, key, VALUE_NAME, name) &&
           Util_RegSetDword(HKEY_CURRENT_USER, key, VALUE_COLOR, (DWORD)((color >= 0 && color < PALETTE_SIZE) ? color : 0));
}

BOOL Profiles_Create(const WCHAR *name, int color, WCHAR *folder, size_t folderCch, WCHAR *error, size_t errorCch)
{
    WCHAR clean[LABEL_CCH], appData[MAX_PATH], dir[MAX_PATH];
    const WCHAR *invalid = NULL;
    DWORD lastError;

    if (!Core_ValidateNewName(name, clean, ARRAYSIZE(clean), folder, folderCch, &invalid)) {
        StringCchCopyW(error, errorCch, TR(invalid));
        return FALSE;
    }
    if (!Util_AppData(appData, ARRAYSIZE(appData)) ||
        FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, folder))) {
        StringCchCopyW(error, errorCch, TR(L"The Roaming AppData folder is not available."));
        return FALSE;
    }
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
        StringCchPrintfW(error, errorCch, TR(L"A folder named \x201C%s\x201D already exists in %%APPDATA%%."), folder);
        return FALSE;
    }
    if (!CreateDirectoryW(dir, NULL)) {
        StringCchPrintfW(error, errorCch, TR(L"Could not create %s (error %lu)."), dir, GetLastError());
        return FALSE;
    }
    /* Without its name in the registry, an empty folder would not be listed
     * and would still block the name: it goes again. */
    if (!WriteNameAndColor(folder, clean, color)) {
        lastError = GetLastError();
        DeleteProfileKey(folder);
        if (!RemoveDirectoryW(dir)) Util_Log(L"could not remove %s, which keeps its name taken (error %lu)", dir, GetLastError());
        StringCchPrintfW(error, errorCch, TR(L"Could not create %s (error %lu)."), dir, lastError);
        return FALSE;
    }
    Util_Log(L"created profile %s", folder);
    return TRUE;
}

BOOL Profiles_Update(const WCHAR *folder, const WCHAR *label, int color)
{
    WCHAR clean[LABEL_CCH];
    return Core_ValidateLabel(label, clean, ARRAYSIZE(clean), NULL) && WriteNameAndColor(folder, clean, color);
}

BOOL Profiles_SetDefault(const WCHAR *folder)
{
    if (Util_RegSetString(HKEY_CURRENT_USER, REG_ROOT, VALUE_DEFAULT_PROFILE, folder)) return TRUE;
    Util_Log(L"could not make %s the default profile (error %lu)", folder, GetLastError());
    return FALSE;
}

/* ------------------------------------------------------- settings copy */

#define SETTINGS_MAX        (1024 * 1024)
#define SETTINGS_COPY_SLACK 256   /* what a copy adds to the members it takes: braces, commas, a line end */
#define NESTED_SETTING_MAX  256   /* the one nested member copied, in an object of its own */

typedef enum SettingsCopy { SETTINGS_COPIED, SETTINGS_NOTHING_TO_COPY, SETTINGS_COPY_FAILED } SettingsCopy;
static const WCHAR *const kSettingsCopyNames[] = { L"copied", L"nothing to copy", L"FAILED" };

/* `json` (a buffer of `cap` bytes holding *len) with `key` set to the value
 * `raw` of `rawLen` bytes; unchanged when it does not fit. */
static BOOL SetValue(char *json, size_t cap, size_t *len, const char *key, const char *raw, size_t rawLen)
{
    char *value = (char *)HeapAlloc(GetProcessHeap(), 0, rawLen + 1), *out = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
    size_t newLen = 0;
    BOOL ok = value && out;
    if (ok) {
        memcpy(value, raw, rawLen);
        value[rawLen] = 0;
        ok = Core_JsonSetMember(json, *len, key, value, out, cap, &newLen);
        if (ok) {
            memcpy(json, out, newLen);
            *len = newLen;
        }
    }
    if (value) HeapFree(GetProcessHeap(), 0, value);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    return ok;
}

/* Writes the members `keys` of the file `name` of one data folder to the
 * same file of another one, which must not exist yet. `nestedIn` puts
 * `nestedKey` under that object (for preferences). SETTINGS_NOTHING_TO_COPY
 * when the file or every member is missing; a failure is logged. */
static SettingsCopy CopyMembers(const WCHAR *fromDir, const WCHAR *toDir, const WCHAR *name,
                                const char *const *keys, size_t count, const char *nestedIn, const char *nestedKey)
{
    WCHAR from[MAX_PATH], to[MAX_PATH];
    char *source, *copy, wrapped[NESTED_SETTING_MAX];
    const char *value, *nested;
    size_t valueLength, nestedLength, i, cap, copyLength = 2, wrappedLength = 2;
    DWORD sourceLength = 0, written = 0;
    HANDLE file;
    SettingsCopy outcome = SETTINGS_COPY_FAILED;
    BOOL any = FALSE;
    if (FAILED(StringCchPrintfW(from, ARRAYSIZE(from), L"%s\\%s", fromDir, name)) ||
        FAILED(StringCchPrintfW(to, ARRAYSIZE(to), L"%s\\%s", toDir, name))) {
        Util_Log(L"settings not copied: a path to %s is too long", name);
        return SETTINGS_COPY_FAILED;
    }
    if (Util_QueryPath(from, NULL) == PATH_MISSING) return SETTINGS_NOTHING_TO_COPY;
    if (Util_FileExists(to)) {
        Util_Log(L"settings not copied: %s is there already", to);
        return SETTINGS_COPY_FAILED;
    }
    if ((source = Util_ReadFile(from, SETTINGS_MAX, FALSE, &sourceLength)) == NULL) {
        Util_Log(L"settings not copied: %s cannot be read whole", from);
        return SETTINGS_COPY_FAILED;
    }
    cap = (size_t)sourceLength + SETTINGS_COPY_SLACK;
    if ((copy = (char *)HeapAlloc(GetProcessHeap(), 0, cap)) == NULL) {
        Util_Log(L"settings not copied to %s: out of memory", to);
    } else {
        /* Room is kept for the line end. */
        memcpy(copy, "{}", 2);
        for (i = 0; i < count; i++)
            if (Core_JsonMember(source, sourceLength, keys[i], &value, &valueLength) &&
                SetValue(copy, cap - 1, &copyLength, keys[i], value, valueLength))
                any = TRUE;
        if (nestedIn && Core_JsonMember(source, sourceLength, nestedIn, &nested, &nestedLength) &&
            Core_JsonMember(nested, nestedLength, nestedKey, &value, &valueLength)) {
            memcpy(wrapped, "{}", 2);
            if (SetValue(wrapped, sizeof wrapped, &wrappedLength, nestedKey, value, valueLength) &&
                SetValue(copy, cap - 1, &copyLength, nestedIn, wrapped, wrappedLength))
                any = TRUE;
        }
        if (!any) {
            outcome = SETTINGS_NOTHING_TO_COPY;
        } else {
            copy[copyLength++] = '\n';
            file = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE) {
                Util_Log(L"settings not copied: %s cannot be created (error %lu)", to, GetLastError());
            } else {
                BOOL whole = WriteFile(file, copy, (DWORD)copyLength, &written, NULL) && written == (DWORD)copyLength;
                DWORD error = GetLastError();
                CloseHandle(file);
                if (whole) {
                    outcome = SETTINGS_COPIED;
                } else {
                    Util_Log(L"settings not copied: %s cannot be written (error %lu)", to, error);
                    DeleteFileW(to);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, copy);
    }
    HeapFree(GetProcessHeap(), 0, source);
    return outcome;
}

/* A new profile starts with the settings of another one that are not tied to
 * its account: MCP servers, notification-area icon, hardware acceleration,
 * language and theme. Never its sign-in. */
void Profiles_CopySettings(const Profile *from, const Profile *to)
{
    static const char *const desktop[] = { "mcpServers", "isHardwareAccelerationDisabled" };
    static const char *const app[] = { "locale", "userThemeMode" };
    SettingsCopy desktopOutcome, appOutcome;
    if (!from->storageDir[0] || !to->storageDir[0]) {
        Util_Log(L"could not copy settings of %s to %s: profile storage is unavailable", from->folder, to->folder);
        return;
    }
    desktopOutcome = CopyMembers(from->storageDir, to->storageDir, CLAUDE_DESKTOP_SETTINGS, desktop, ARRAYSIZE(desktop),
                                 "preferences", "menuBarEnabled");
    appOutcome = CopyMembers(from->storageDir, to->storageDir, CLAUDE_APP_SETTINGS, app, ARRAYSIZE(app), NULL, NULL);
    Util_Log(L"settings of %s for %s: desktop settings %s, app settings %s", from->folder, to->folder,
             kSettingsCopyNames[desktopOutcome], kSettingsCopyNames[appOutcome]);
}

/* The profile's folder is a junction or directory symlink to one elsewhere. */
BOOL Profiles_IsLinked(const Profile *profile)
{
    return Util_IsDirectoryLink(profile->dataDir);
}

/* One profile folder to the Recycle Bin (a file of that name is not one). */
static RemoveResult Recycle(HWND owner, const Profile *profile, const WCHAR *path)
{
    DWORD attributes;
    if (Claude_IsRunning(profile)) {
        Util_Log(L"kept %s: its Claude is running", path);
        return REMOVE_FAILED;
    }
    if (Util_QueryPath(path, &attributes) == PATH_PRESENT && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return REMOVE_DONE;
    return Util_Recycle(owner, &path, 1);
}

/* Where a profile folder that is a junction or directory symlink leads.
 * FALSE when it is a plain folder or the target cannot be resolved. */
BOOL Profiles_LinkTarget(const Profile *profile, WCHAR *out, size_t cch)
{
    WCHAR finalPath[MAX_PATH + 8];
    const WCHAR *p = finalPath;
    DWORD n;
    HANDLE folder;
    if (!Util_IsDirectoryLink(profile->dataDir)) return FALSE;
    folder = CreateFileW(profile->dataDir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (folder == INVALID_HANDLE_VALUE) return FALSE;
    n = GetFinalPathNameByHandleW(folder, finalPath, ARRAYSIZE(finalPath), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(folder);
    if (n == 0 || n >= ARRAYSIZE(finalPath)) return FALSE;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\%s", p + 8));
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    return SUCCEEDED(StringCchCopyW(out, cch, p));
}

/* What Claude keeps for the profile in %LOCALAPPDATA% (logs, "<folder>-Data")
 * and in its package's LocalCache\Local and \Roaming, where Claude's own
 * writes land when the folder did not exist yet (see claude.c), then the data
 * folder. Each location is handled on its own and skipped once gone; the data
 * folder goes last, so a profile left half removed is still listed and its
 * removal can be tried again. */
RemoveResult Profiles_RecycleData(HWND owner, const Profile *profile)
{
    static const WCHAR *const suffixes[] = { L"", L"-Data" };
    WCHAR appData[MAX_PATH], localAppData[MAX_PATH], path[MAX_PATH], name[FOLDER_CCH + 8];
    ClaudePackage pkg;
    BOOL havePackage;
    RemoveResult result;
    size_t i;

    if (profile->isStock) return REMOVE_FAILED;
    /* A %APPDATA% on a disconnected drive can also answer "path not found". */
    if (!Util_AppData(appData, ARRAYSIZE(appData)) || !Util_DirExists(appData)) return REMOVE_FAILED;
    if (!Util_LocalAppData(localAppData, ARRAYSIZE(localAppData))) return REMOVE_FAILED;
    havePackage = Claude_FindPackage(&pkg);
    for (i = 0; i < ARRAYSIZE(suffixes); i++) {
        if (FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s%s", profile->folder, suffixes[i])) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", localAppData, name))) return REMOVE_FAILED;
        if ((result = Recycle(owner, profile, path)) != REMOVE_DONE) return result;
        if (havePackage) {
            if (!Core_PackageCachePath(localAppData, pkg.family, L"Local", name, path, ARRAYSIZE(path))) return REMOVE_FAILED;
            if ((result = Recycle(owner, profile, path)) != REMOVE_DONE) return result;
        }
    }
    if (havePackage) {
        if (!Core_PackageCachePath(localAppData, pkg.family, L"Roaming", profile->folder, path, ARRAYSIZE(path))) return REMOVE_FAILED;
        if ((result = Recycle(owner, profile, path)) != REMOVE_DONE) return result;
    }
    if ((result = Recycle(owner, profile, profile->dataDir)) != REMOVE_DONE) return result;
    Util_Log(L"recycled profile data %s", profile->dataDir);
    return REMOVE_DONE;
}

RemoveResult Profiles_Delete(HWND owner, const Profile *profile)
{
    RemoveResult result;
    if (profile->isStock) return REMOVE_FAILED;
    result = Profiles_RecycleData(owner, profile);
    if (result != REMOVE_DONE) return result;
    Shortcut_RemoveOurs(profile);
    Icons_DeleteStale(profile, NULL);
    ForgetProfile(profile->folder);
    Util_Log(L"deleted profile %s", profile->folder);
    return REMOVE_DONE;
}
