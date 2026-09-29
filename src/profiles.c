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

static void ProfileKey(const WCHAR *folder, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, REG_PROFILES L"\\%s", folder);
}

static void Fill(Profile *p, const WCHAR *appData, const WCHAR *folder, BOOL isStock)
{
    WCHAR key[MAX_PATH];
    DWORD color;
    ZeroMemory(p, sizeof *p);
    StringCchCopyW(p->folder, ARRAYSIZE(p->folder), folder);
    StringCchPrintfW(p->dataDir, ARRAYSIZE(p->dataDir), L"%s\\%s", appData, folder);
    p->isStock = isStock;
    ProfileKey(folder, key, ARRAYSIZE(key));
    if (!Util_RegGetString(HKEY_CURRENT_USER, key, L"Name", p->name, ARRAYSIZE(p->name)) || !p->name[0])
        StringCchCopyW(p->name, ARRAYSIZE(p->name), isStock ? STOCK_DEFAULT_NAME : folder + wcslen(PROFILE_PREFIX));
    p->color = (Util_RegGetDword(HKEY_CURRENT_USER, key, L"Color", &color) && color < PALETTE_SIZE) ? (int)color : -1;
}

/* Registry entries whose folder was deleted outside the manager. Only a
 * folder that is positively not there counts: a %APPDATA% on an unreachable
 * network share must not erase every name and color. */
static void PruneMissing(const WCHAR *appData)
{
    WCHAR names[MAX_PROFILES * 2][FOLDER_CCH], dir[MAX_PATH], key[MAX_PATH];
    HKEY root;
    DWORD i, n = 0, cch;
    if (!Util_DirExists(appData)) return;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PROFILES, 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (i = 0; n < ARRAYSIZE(names); i++) {
        LSTATUS rc;
        cch = FOLDER_CCH;
        rc = RegEnumKeyExW(root, i, names[n], &cch, NULL, NULL, NULL, NULL);
        if (rc == ERROR_MORE_DATA) continue;   /* not a name of ours */
        if (rc != ERROR_SUCCESS) break;        /* the end, or the key was deleted meanwhile */
        n++;
    }
    RegCloseKey(root);
    for (i = 0; i < n; i++) {
        if (CompareStringOrdinal(names[i], -1, STOCK_FOLDER, -1, TRUE) == CSTR_EQUAL) continue;
        if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, names[i]))) continue;
        if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND) continue;
        ProfileKey(names[i], key, ARRAYSIZE(key));
        Util_RegDeleteTree(HKEY_CURRENT_USER, key);
    }
}

/* Profiles without a color get the first free one; it is saved so it stays
 * put, but only while Claude Desktop Profiles Manager is installed (loading
 * never recreates its registry key after an uninstall). */
static void AssignColors(ProfileList *list)
{
    BOOL used[PALETTE_SIZE] = { 0 };
    BOOL persist = Install_IsRegistered();
    WCHAR key[MAX_PATH];
    int i, c;
    for (i = 0; i < list->count; i++)
        if (list->items[i].color >= 0) used[list->items[i].color] = TRUE;
    for (i = 0; i < list->count; i++) {
        if (list->items[i].color >= 0) continue;
        for (c = 0; c < PALETTE_SIZE && used[c]; c++) {}
        if (c == PALETTE_SIZE) c = i % PALETTE_SIZE;
        used[c] = TRUE;
        list->items[i].color = c;
        if (!persist) continue;
        ProfileKey(list->items[i].folder, key, ARRAYSIZE(key));
        Util_RegSetDword(HKEY_CURRENT_USER, key, L"Color", (DWORD)c);
    }
}

static int __cdecl CompareProfiles(const void *a, const void *b)
{
    const Profile *x = (const Profile *)a, *y = (const Profile *)b;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                           x->name, -1, y->name, -1, NULL, NULL, 0) - CSTR_EQUAL;
}

void Profiles_Load(ProfileList *list)
{
    WCHAR appData[MAX_PATH], pattern[MAX_PATH], key[MAX_PATH], marker[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    static BOOL reportedCap;

    ZeroMemory(list, sizeof *list);
    StringCchCopyW(list->defaultFolder, ARRAYSIZE(list->defaultFolder), STOCK_FOLDER);
    if (!Util_AppData(appData, ARRAYSIZE(appData))) return;

    Fill(&list->items[list->count++], appData, STOCK_FOLDER, TRUE);

    if (SUCCEEDED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\" PROFILE_PREFIX L"*", appData))) {
        h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchLimitToDirectories, NULL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (!Core_IsProfileFolder(fd.cFileName)) continue;
                ProfileKey(fd.cFileName, key, ARRAYSIZE(key));
                if (FAILED(StringCchPrintfW(marker, ARRAYSIZE(marker), L"%s\\%s\\Local State", appData, fd.cFileName)))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    /* A profile moved elsewhere and linked back (junction or
                     * directory symlink) counts once it reaches a real one. */
                    if ((fd.dwReserved0 != IO_REPARSE_TAG_MOUNT_POINT && fd.dwReserved0 != IO_REPARSE_TAG_SYMLINK) ||
                        !Util_FileExists(marker))
                        continue;
                } else if (!Util_RegKeyExists(HKEY_CURRENT_USER, key) && !Util_FileExists(marker)) {
                    continue;   /* neither a Chromium profile nor one created here */
                }
                if (list->count >= MAX_PROFILES) {
                    if (!reportedCap)
                        Util_Log(L"more than %d profiles: %s and later ones are not listed", MAX_PROFILES, fd.cFileName);
                    reportedCap = TRUE;
                    break;
                }
                Fill(&list->items[list->count++], appData, fd.cFileName, FALSE);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    PruneMissing(appData);
    AssignColors(list);
    if (list->count > 2)
        qsort(list->items + 1, (size_t)(list->count - 1), sizeof(Profile), CompareProfiles);

    if (Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, L"DefaultProfile", key, FOLDER_CCH) &&
        Profiles_Find(list, key) >= 0)
        StringCchCopyW(list->defaultFolder, ARRAYSIZE(list->defaultFolder), list->items[Profiles_Find(list, key)].folder);

    Claude_UpdateRunning(list);
}

int Profiles_Find(const ProfileList *list, const WCHAR *folder)
{
    int i;
    if (!folder || !*folder) return -1;
    for (i = 0; i < list->count; i++)
        if (CompareStringOrdinal(list->items[i].folder, -1, folder, -1, TRUE) == CSTR_EQUAL) return i;
    return -1;
}

int Profiles_DefaultIndex(const ProfileList *list)
{
    int i = Profiles_Find(list, list->defaultFolder);
    return i >= 0 ? i : 0;
}

BOOL Profiles_Create(const WCHAR *name, int color, WCHAR *folder, size_t folderCch, WCHAR *err, size_t errCch)
{
    WCHAR clean[LABEL_CCH], appData[MAX_PATH], dir[MAX_PATH], key[MAX_PATH];
    const WCHAR *why = NULL;

    if (!Core_ValidateNewName(name, clean, ARRAYSIZE(clean), folder, folderCch, &why)) {
        StringCchCopyW(err, errCch, why ? why : L"Invalid name.");
        return FALSE;
    }
    if (!Util_AppData(appData, ARRAYSIZE(appData)) ||
        FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, folder))) {
        StringCchCopyW(err, errCch, L"The Roaming AppData folder is not available.");
        return FALSE;
    }
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
        StringCchPrintfW(err, errCch, L"A folder named \x201C%s\x201D already exists in %%APPDATA%%.", folder);
        return FALSE;
    }
    if (!CreateDirectoryW(dir, NULL)) {
        StringCchPrintfW(err, errCch, L"Could not create %s (error %lu).", dir, GetLastError());
        return FALSE;
    }
    ProfileKey(folder, key, ARRAYSIZE(key));
    Util_RegSetString(HKEY_CURRENT_USER, key, L"Name", clean);
    Util_RegSetDword(HKEY_CURRENT_USER, key, L"Color", (DWORD)((color >= 0 && color < PALETTE_SIZE) ? color : 0));
    Util_Log(L"created profile %s", folder);
    return TRUE;
}

BOOL Profiles_Update(const WCHAR *folder, const WCHAR *label, int color)
{
    WCHAR clean[LABEL_CCH], key[MAX_PATH];
    if (!Core_ValidateLabel(label, clean, ARRAYSIZE(clean), NULL)) return FALSE;
    ProfileKey(folder, key, ARRAYSIZE(key));
    return Util_RegSetString(HKEY_CURRENT_USER, key, L"Name", clean) &&
           Util_RegSetDword(HKEY_CURRENT_USER, key, L"Color", (DWORD)((color >= 0 && color < PALETTE_SIZE) ? color : 0));
}

void Profiles_SetDefault(const WCHAR *folder)
{
    Util_RegSetString(HKEY_CURRENT_USER, REG_ROOT, L"DefaultProfile", folder);
}

/* ------------------------------------------------------- settings copy */

#define SETTINGS_MAX (1024 * 1024)

/* `json` (a buffer of `cap` bytes holding *len) with `key` set to the value
 * `raw` of `rawLen` bytes. */
static BOOL SetValue(char *json, size_t cap, size_t *len, const char *key, const char *raw, size_t rawLen)
{
    char *value = (char *)HeapAlloc(GetProcessHeap(), 0, rawLen + 1), *out = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
    BOOL ok = value && out;
    if (ok) {
        memcpy(value, raw, rawLen);
        value[rawLen] = 0;
        ok = Core_JsonSetMember(json, *len, key, value, out, cap, len);
        if (ok) memcpy(json, out, *len);
    }
    if (value) HeapFree(GetProcessHeap(), 0, value);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    return ok;
}

/* Writes the members `keys` of the file `name` of one data folder to the
 * same file of another one, which must not exist yet. `nestedIn` puts
 * `nestedKey` under that object (for preferences). */
static BOOL CopyMembers(const WCHAR *fromDir, const WCHAR *toDir, const WCHAR *name,
                        const char *const *keys, size_t count, const char *nestedIn, const char *nestedKey)
{
    WCHAR from[MAX_PATH], to[MAX_PATH];
    char *src, *out, wrapped[256];
    const char *v, *inner;
    size_t vl, il, i, cap, o = 2, wl = 2;
    DWORD len = 0, written = 0;
    HANDLE h;
    BOOL ok = FALSE, any = FALSE;
    if (FAILED(StringCchPrintfW(from, ARRAYSIZE(from), L"%s\\%s", fromDir, name)) ||
        FAILED(StringCchPrintfW(to, ARRAYSIZE(to), L"%s\\%s", toDir, name)) || Util_FileExists(to))
        return FALSE;
    if ((src = Util_ReadFile(from, SETTINGS_MAX, FALSE, &len)) == NULL) return FALSE;
    cap = (size_t)len + 256;
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, cap)) != NULL) {
        memcpy(out, "{}", 2);
        for (i = 0; i < count; i++)
            if (Core_JsonMember(src, len, keys[i], &v, &vl) && SetValue(out, cap, &o, keys[i], v, vl)) any = TRUE;
        if (nestedIn && Core_JsonMember(src, len, nestedIn, &inner, &il) && Core_JsonMember(inner, il, nestedKey, &v, &vl)) {
            memcpy(wrapped, "{}", 2);
            if (SetValue(wrapped, sizeof wrapped, &wl, nestedKey, v, vl) && SetValue(out, cap, &o, nestedIn, wrapped, wl)) any = TRUE;
        }
        if (any && o + 1 < cap) {
            out[o++] = '\n';
            h = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                ok = WriteFile(h, out, (DWORD)o, &written, NULL) && written == (DWORD)o;
                CloseHandle(h);
                if (!ok) DeleteFileW(to);
            }
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
    HeapFree(GetProcessHeap(), 0, src);
    return ok;
}

/* A new profile starts with the settings of another one that are not tied to
 * its account: MCP servers, notification-area icon, hardware acceleration,
 * language and theme. Never its sign-in. */
void Profiles_CopySettings(const Profile *from, const Profile *to)
{
    static const char *const desktop[] = { "mcpServers", "isHardwareAccelerationDisabled" };
    static const char *const app[] = { "locale", "userThemeMode" };
    BOOL a = CopyMembers(from->dataDir, to->dataDir, L"claude_desktop_config.json", desktop, ARRAYSIZE(desktop),
                         "preferences", "menuBarEnabled");
    BOOL b = CopyMembers(from->dataDir, to->dataDir, L"config.json", app, ARRAYSIZE(app), NULL, NULL);
    Util_Log(L"copied settings of %s to %s (%d, %d)", from->folder, to->folder, a, b);
}

BOOL Profiles_IsLinked(const Profile *profile)
{
    DWORD attr = GetFileAttributesW(profile->dataDir);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* One profile folder to the Recycle Bin (a file of that name is not one). */
static RemoveResult Recycle(HWND owner, const WCHAR *path)
{
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return REMOVE_DONE;
    return Util_Recycle(owner, &path, 1);
}

/* Where a profile folder that is a junction or directory symlink leads.
 * FALSE when it is a plain folder or the target cannot be resolved. */
BOOL Profiles_LinkTarget(const Profile *profile, WCHAR *out, size_t cch)
{
    WCHAR buf[MAX_PATH + 8];
    const WCHAR *p = buf;
    DWORD attr = GetFileAttributesW(profile->dataDir), n;
    HANDLE h;
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) return FALSE;
    h = CreateFileW(profile->dataDir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    n = GetFinalPathNameByHandleW(h, buf, ARRAYSIZE(buf), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= ARRAYSIZE(buf)) return FALSE;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\%s", p + 8));
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    return SUCCEEDED(StringCchCopyW(out, cch, p));
}

/* The data folder, then what Claude keeps for the profile in %LOCALAPPDATA%
 * (logs, "<folder>-Data") and in its package's LocalCache\\Local and \\Roaming,
 * where Claude's own writes land when the folder did not exist yet (see
 * claude.c). Each location is handled on its own and skipped once gone. */
RemoveResult Profiles_RecycleData(HWND owner, const Profile *profile)
{
    static const WCHAR *const suffixes[] = { L"", L"-Data" };
    WCHAR local[MAX_PATH], p[MAX_PATH];
    ClaudePackage pkg;
    BOOL havePkg;
    RemoveResult r;
    size_t i;

    if (profile->isStock) return REMOVE_FAILED;
    /* A %APPDATA% on a disconnected drive can also answer "path not found". */
    if (!Util_AppData(local, ARRAYSIZE(local)) || !Util_DirExists(local)) return REMOVE_FAILED;
    r = Recycle(owner, profile->dataDir);
    if (r != REMOVE_DONE) return r;
    if (Util_LocalAppData(local, ARRAYSIZE(local))) {
        havePkg = Claude_FindPackage(&pkg);
        for (i = 0; i < ARRAYSIZE(suffixes); i++) {
            if (SUCCEEDED(StringCchPrintfW(p, ARRAYSIZE(p), L"%s\\%s%s", local, profile->folder, suffixes[i])))
                Recycle(owner, p);
            if (havePkg && SUCCEEDED(StringCchPrintfW(p, ARRAYSIZE(p), L"%s\\Packages\\%s\\LocalCache\\Local\\%s%s",
                                                      local, pkg.family, profile->folder, suffixes[i])))
                Recycle(owner, p);
        }
        if (havePkg &&
            SUCCEEDED(StringCchPrintfW(p, ARRAYSIZE(p), L"%s\\Packages\\%s\\LocalCache\\Roaming\\%s", local, pkg.family, profile->folder)))
            Recycle(owner, p);
    }
    Util_Log(L"recycled profile data %s", profile->dataDir);
    return REMOVE_DONE;
}

RemoveResult Profiles_Delete(HWND owner, const Profile *profile)
{
    WCHAR key[MAX_PATH], def[FOLDER_CCH], pending[MAX_PATH];
    RemoveResult r;
    if (profile->isStock) return REMOVE_FAILED;
    r = Profiles_RecycleData(owner, profile);
    if (r != REMOVE_DONE) return r;
    Shortcut_RemoveOurs(profile);
    Icons_DeleteStale(profile, NULL);
    if (SessionStore_PendingPath(profile, pending, ARRAYSIZE(pending))) DeleteFileW(pending);
    ProfileKey(profile->folder, key, ARRAYSIZE(key));
    Util_RegDeleteTree(HKEY_CURRENT_USER, key);
    if (Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, L"DefaultProfile", def, ARRAYSIZE(def)) &&
        CompareStringOrdinal(def, -1, profile->folder, -1, TRUE) == CSTR_EQUAL)
        Util_RegDeleteValue(HKEY_CURRENT_USER, REG_ROOT, L"DefaultProfile");
    Util_Log(L"deleted profile %s", profile->folder);
    return REMOVE_DONE;
}
