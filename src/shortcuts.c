/*
 * .lnk files. A profile shortcut runs
 * `ClaudeDesktopProfilesManager.exe --launch "<folder>"` and carries its own
 * AppUserModelID, so each profile can be pinned to the taskbar separately.
 * The shortcuts made here are recorded in REG_SHORTCUTS (path: folder), so
 * they follow renames and go with the profile wherever they were put; ours
 * are also looked for where shortcuts usually are.
 */
#include "app.h"
#include <knownfolders.h>
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>

#define DESCRIPTION_CCH     256
#define FAT_TIME_STEP_TICKS 20000000ULL   /* 2 s, in FILETIME's 100 ns units */

/* ------------------------------------------------------------- reading */

/* One shell link object reads every shortcut of a search. */
typedef struct LinkReader {
    IShellLinkW  *link;
    IPersistFile *file;
} LinkReader;

static BOOL OpenLinkReader(LinkReader *reader)
{
    reader->link = NULL;
    reader->file = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&reader->link)))
        return FALSE;
    if (SUCCEEDED(IShellLinkW_QueryInterface(reader->link, &IID_IPersistFile, (void **)&reader->file))) return TRUE;
    IShellLinkW_Release(reader->link);
    reader->link = NULL;
    return FALSE;
}

static void CloseLinkReader(LinkReader *reader)
{
    if (reader->file) IPersistFile_Release(reader->file);
    if (reader->link) IShellLinkW_Release(reader->link);
}

static BOOL ReadLink(const LinkReader *reader, const WCHAR *lnk, LinkInfo *info)
{
    ZeroMemory(info, sizeof *info);
    if (FAILED(IPersistFile_Load(reader->file, lnk, STGM_READ))) return FALSE;
    if (IShellLinkW_GetPath(reader->link, info->target, ARRAYSIZE(info->target), NULL, 0) != S_OK) info->target[0] = 0;
    if (FAILED(IShellLinkW_GetArguments(reader->link, info->args, ARRAYSIZE(info->args)))) info->args[0] = 0;
    if (!info->target[0]) {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(IShellLinkW_GetIDList(reader->link, &pidl)) && pidl) {
            PWSTR name = NULL;
            if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
                StringCchCopyW(info->parsing, ARRAYSIZE(info->parsing), name);
                CoTaskMemFree(name);
            }
            CoTaskMemFree(pidl);
        }
    }
    return TRUE;
}

BOOL Shortcut_Read(const WCHAR *lnk, LinkInfo *info)
{
    LinkReader reader;
    BOOL ok;
    ZeroMemory(info, sizeof *info);
    if (!OpenLinkReader(&reader)) return FALSE;
    ok = ReadLink(&reader, lnk, info);
    CloseLinkReader(&reader);
    return ok;
}

/* The shortcut carries this AppUserModelID. */
BOOL Shortcut_HasAppId(const WCHAR *lnk, const WCHAR *aumid)
{
    LinkReader reader;
    IPropertyStore *store = NULL;
    PROPVARIANT value;
    BOOL same = FALSE;
    if (!OpenLinkReader(&reader)) return FALSE;
    PropVariantInit(&value);
    if (SUCCEEDED(IPersistFile_Load(reader.file, lnk, STGM_READ)) &&
        SUCCEEDED(IShellLinkW_QueryInterface(reader.link, &IID_IPropertyStore, (void **)&store)) &&
        SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_AppUserModel_ID, &value)))
        same = value.vt == VT_LPWSTR && Core_EqualsI(value.pwszVal, aumid);
    PropVariantClear(&value);
    if (store) IPropertyStore_Release(store);
    CloseLinkReader(&reader);
    return same;
}

/* A shortcut of ours, for the profile in `folder` (NULL: for any profile). */
static BOOL IsOurs(const LinkInfo *info, const WCHAR *folder)
{
    return Core_IsOurExe(info->target) && (!folder || Core_ArgsSelectProfile(info->args, folder));
}

/* ------------------------------------------------------------- writing */

static HRESULT WriteLink(const WCHAR *lnk, const WCHAR *exe, const WCHAR *args, const WCHAR *icon,
                         const WCHAR *description, const WCHAR *workDir, const WCHAR *aumid)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    IPropertyStore *store = NULL;
    PROPVARIANT value;
    size_t aumidCch = wcslen(aumid) + 1;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr)) return hr;
    PropVariantInit(&value);
    hr = IShellLinkW_SetPath(link, exe);
    if (SUCCEEDED(hr)) hr = IShellLinkW_SetArguments(link, args);
    if (SUCCEEDED(hr)) hr = IShellLinkW_SetIconLocation(link, icon, 0);
    if (SUCCEEDED(hr)) hr = IShellLinkW_SetDescription(link, description);
    if (SUCCEEDED(hr)) hr = IShellLinkW_SetWorkingDirectory(link, workDir);
    if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&store);
    if (SUCCEEDED(hr)) {
        value.vt = VT_LPWSTR;
        value.pwszVal = (LPWSTR)CoTaskMemAlloc(aumidCch * sizeof(WCHAR));
        hr = value.pwszVal ? StringCchCopyW(value.pwszVal, aumidCch, aumid) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = IPropertyStore_SetValue(store, &PKEY_AppUserModel_ID, &value);
    if (SUCCEEDED(hr)) hr = IPropertyStore_Commit(store);
    PropVariantClear(&value);
    if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr)) hr = IPersistFile_Save(file, lnk, TRUE);
    if (store) IPropertyStore_Release(store);
    if (file) IPersistFile_Release(file);
    IShellLinkW_Release(link);
    return hr;
}

/* The taskbar keeps a pin's icon while the shortcut's write time stays the
 * same at FAT resolution (2 s): a save within the same two seconds as the
 * last write before it, `before`, gets a later time. */
static void SetLaterWriteTime(const WCHAR *lnk, const FILETIME *before)
{
    WIN32_FILE_ATTRIBUTE_DATA saved;
    ULARGE_INTEGER ticks;
    FILETIME later;
    HANDLE times;
    if (!GetFileAttributesExW(lnk, GetFileExInfoStandard, &saved) || !Core_SameFatTime(before, &saved.ftLastWriteTime)) return;
    ticks.LowPart = before->dwLowDateTime;
    ticks.HighPart = before->dwHighDateTime;
    ticks.QuadPart += FAT_TIME_STEP_TICKS;
    later.dwLowDateTime = ticks.LowPart;
    later.dwHighDateTime = ticks.HighPart;
    times = CreateFileW(lnk, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (times == INVALID_HANDLE_VALUE || !SetFileTime(times, NULL, NULL, &later))
        Util_Log(L"could not give %s a later write time: a pin of it may keep its icon (error %lu)", lnk, GetLastError());
    if (times != INVALID_HANDLE_VALUE) CloseHandle(times);
}

/* Gives a shortcut `icon` and `description` when they differ, saved in place
 * with a new last write time. Sends no notification. S_OK: saved; S_FALSE:
 * already current. */
static HRESULT UpdateLink(const WCHAR *lnk, const WCHAR *icon, const WCHAR *description)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    WCHAR currentIcon[MAX_PATH], currentDescription[DESCRIPTION_CCH];
    WIN32_FILE_ATTRIBUTE_DATA beforeSave;
    int index = 0;
    HRESULT hr;

    if (!GetFileAttributesExW(lnk, GetFileExInfoStandard, &beforeSave)) return HRESULT_FROM_WIN32(GetLastError());
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr)) return hr;
    hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr)) hr = IPersistFile_Load(file, lnk, STGM_READWRITE);
    if (SUCCEEDED(hr) &&
        SUCCEEDED(IShellLinkW_GetIconLocation(link, currentIcon, ARRAYSIZE(currentIcon), &index)) && index == 0 &&
        Core_PathEquals(currentIcon, icon) &&
        SUCCEEDED(IShellLinkW_GetDescription(link, currentDescription, ARRAYSIZE(currentDescription))) &&
        wcscmp(currentDescription, description) == 0)
        hr = S_FALSE;
    if (hr == S_OK) hr = IShellLinkW_SetIconLocation(link, icon, 0);
    if (hr == S_OK) hr = IShellLinkW_SetDescription(link, description);
    if (hr == S_OK) hr = IPersistFile_Save(file, NULL, TRUE);
    if (file) IPersistFile_Release(file);
    IShellLinkW_Release(link);
    if (hr == S_OK) SetLaterWriteTime(lnk, &beforeSave.ftLastWriteTime);
    return hr;
}

/* What a profile shortcut says it does (its tooltip). */
static void ProfileDescription(const Profile *profile, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, TR(L"Open Claude with the %s profile"), profile->name);
}

/* What the manager's shortcut says it does. */
static const WCHAR *ManagerDescription(void)
{
    return TR(L"Manage your Claude Desktop profiles");
}

/* FALSE, logged: unless it is where ours are looked for, the shortcut does
 * not follow the profile's renames nor go with it. */
static BOOL RecordLink(const WCHAR *lnk, const WCHAR *folder)
{
    if (Util_RegSetString(HKEY_CURRENT_USER, REG_SHORTCUTS, lnk, folder)) return TRUE;
    Util_Log(L"could not record the shortcut %s (error %lu)", lnk, GetLastError());
    return FALSE;
}

static void ForgetLink(const WCHAR *lnk)
{
    LSTATUS status = Util_RegDeleteValue(HKEY_CURRENT_USER, REG_SHORTCUTS, lnk);
    if (status != ERROR_SUCCESS) Util_Log(L"could not forget the shortcut %s (error %ld)", lnk, status);
}

/* Deletes a shortcut and tells the shell. FALSE (with the last error): it is
 * still there. */
static BOOL DeleteLink(const WCHAR *lnk)
{
    if (!DeleteFileW(lnk)) {
        Util_Log(L"could not remove shortcut %s (error %lu)", lnk, GetLastError());
        return FALSE;
    }
    SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
    Util_Log(L"removed shortcut %s", lnk);
    return TRUE;
}

/* Deletes one of our shortcuts and forgets its record. One that cannot be
 * deleted stays recorded, and the first such failure goes in `*result`
 * (when not NULL). */
static void RemoveLink(const WCHAR *lnk, BOOL recorded, HRESULT *result)
{
    if (DeleteLink(lnk)) {
        if (recorded) ForgetLink(lnk);
    } else if (result && SUCCEEDED(*result)) {
        *result = HRESULT_FROM_WIN32(GetLastError());
    }
}

/* A profile shortcut, not recorded: taskbar pins are written this way. */
HRESULT Shortcut_WriteForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], icon[MAX_PATH], args[FOLDER_CCH + 16], description[DESCRIPTION_CCH], aumid[AUMID_CCH];

    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_InstallDir(dir, ARRAYSIZE(dir)) ||
        FAILED(StringCchPrintfW(args, ARRAYSIZE(args), L"--launch \"%s\"", profile->folder)))
        return E_FAIL;
    if (!Icons_Ensure(pkg, profile, icon, ARRAYSIZE(icon))) StringCchCopyW(icon, ARRAYSIZE(icon), exe);
    ProfileDescription(profile, description, ARRAYSIZE(description));
    Core_ProfileAumid(profile->folder, aumid, ARRAYSIZE(aumid));
    return WriteLink(lnk, exe, args, icon, description, dir, aumid);
}

HRESULT Shortcut_CreateForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk)
{
    HRESULT hr = Shortcut_WriteForProfile(pkg, profile, lnk);
    if (SUCCEEDED(hr)) {
        RecordLink(lnk, profile->folder);
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
        Util_Log(L"created shortcut %s", lnk);
    }
    return hr;
}

/* ---------------------------------------------- the manager's shortcut */

static BOOL ManagerLinkIn(const WCHAR *dir, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME L".lnk", dir));
}

/* The Start menu folder of Claude Desktop Profiles Manager: its own shortcut and
 * the profiles added to the Start menu. */
BOOL Shortcut_StartMenuDir(WCHAR *out, size_t cch)
{
    WCHAR programs[MAX_PATH];
    return Util_KnownFolder(&FOLDERID_Programs, programs, ARRAYSIZE(programs)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME, programs));
}

/* The manager's own shortcut, and the folder it is in. */
static BOOL ManagerLinkPath(WCHAR *out, size_t cch, WCHAR *dir, size_t dirCch)
{
    return Shortcut_StartMenuDir(dir, dirCch) && ManagerLinkIn(dir, out, cch);
}

HRESULT Shortcut_CreateManagerLink(const WCHAR *exe)
{
    WCHAR folder[MAX_PATH], lnk[MAX_PATH], programs[MAX_PATH], duplicate[MAX_PATH], installDir[MAX_PATH];
    LinkInfo info;
    HRESULT hr;
    if (!ManagerLinkPath(lnk, ARRAYSIZE(lnk), folder, ARRAYSIZE(folder)) || !Util_InstallDir(installDir, ARRAYSIZE(installDir)) ||
        !Util_EnsureDir(folder))
        return E_FAIL;
    hr = WriteLink(lnk, exe, L"", exe, ManagerDescription(), installDir, APP_MANAGER_AUMID);
    if (FAILED(hr)) return hr;
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
    /* A manager shortcut straight in Programs would show it twice. */
    if (Util_KnownFolder(&FOLDERID_Programs, programs, ARRAYSIZE(programs)) && ManagerLinkIn(programs, duplicate, ARRAYSIZE(duplicate)) &&
        Shortcut_Read(duplicate, &info) && Core_IsOurExe(info.target) && !info.args[0])
        DeleteLink(duplicate);
    return hr;
}

/* The manager's shortcut again when it is missing (the taskbar takes the icon
 * of the manager's windows from it), or with the icon and tooltip of the
 * interface language when it shows others. S_FALSE: it is as it should be,
 * or it cannot be reached. */
HRESULT Shortcut_RestoreManagerLink(const WCHAR *exe)
{
    WCHAR folder[MAX_PATH], lnk[MAX_PATH];
    LinkInfo info;
    PathState state;
    HRESULT hr;
    if (!ManagerLinkPath(lnk, ARRAYSIZE(lnk), folder, ARRAYSIZE(folder))) return E_FAIL;
    state = Util_QueryPath(lnk, NULL);
    if (state == PATH_MISSING) return Shortcut_CreateManagerLink(exe);
    if (state == PATH_UNREACHABLE || !Shortcut_Read(lnk, &info) || !Core_IsOurExe(info.target)) return S_FALSE;
    hr = UpdateLink(lnk, exe, ManagerDescription());
    if (hr == S_OK) SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
    return hr;
}

/* The taskbar takes the icon of the manager's windows from the manager's
 * shortcut (their AppUserModelID's), and still looks for it once it is
 * deleted: a window shown after that gets a blank icon. So at uninstall it
 * goes last, when no window is left, with its Start menu folder (unless
 * something else was put in it). */
void Shortcut_RemoveManagerLink(void)
{
    WCHAR folder[MAX_PATH], lnk[MAX_PATH];
    LinkInfo info;
    if (!ManagerLinkPath(lnk, ARRAYSIZE(lnk), folder, ARRAYSIZE(folder))) return;
    if (Shortcut_Read(lnk, &info) && Core_IsOurExe(info.target)) DeleteLink(lnk);
    if (RemoveDirectoryW(folder)) SHChangeNotify(SHCNE_RMDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder, NULL);
}

/* -------------------------------------------------------------- names */

/* A free "Claude (<label>).lnk", else "Claude (<label>) (n).lnk", in `dir`. */
static BOOL FreeLinkPath(const WCHAR *dir, const WCHAR *label, WCHAR *out, size_t cch)
{
    WCHAR name[MAX_PATH];
    int copy;
    for (copy = 1; copy <= MAX_LINK_COPIES; copy++) {
        Core_ShortcutFileName(label, copy, name, ARRAYSIZE(name));
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\%s", dir, name))) return FALSE;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

BOOL Shortcut_DesktopPathFor(const Profile *profile, WCHAR *out, size_t cch)
{
    WCHAR desktop[MAX_PATH];
    return Util_KnownFolder(&FOLDERID_Desktop, desktop, ARRAYSIZE(desktop)) && FreeLinkPath(desktop, profile->name, out, cch);
}

/* "Claude (<label>).lnk" or "Claude (<label>) (n).lnk": a name we gave it. */
static BOOL HasOurName(const WCHAR *path, const WCHAR *label)
{
    const WCHAR *file = wcsrchr(path, L'\\');
    WCHAR expected[MAX_PATH];
    int copy;
    file = file ? file + 1 : path;
    for (copy = 1; copy <= MAX_LINK_COPIES; copy++) {
        Core_ShortcutFileName(label, copy, expected, ARRAYSIZE(expected));
        if (CompareStringOrdinal(file, -1, expected, -1, TRUE) == CSTR_EQUAL) return TRUE;
    }
    return FALSE;
}

/* The path a shortcut named after the profile takes after a rename: a free
 * "Claude (<new name>).lnk" in the same folder. FALSE: it keeps its path. */
BOOL Shortcut_RenamedPath(const WCHAR *path, const Profile *before, const Profile *after, WCHAR *out, size_t cch)
{
    WCHAR dir[MAX_PATH], name[MAX_PATH];
    WCHAR *slash;
    int copy;
    if (wcscmp(before->name, after->name) == 0 || !HasOurName(path, before->name) ||
        FAILED(StringCchCopyW(dir, ARRAYSIZE(dir), path)))
        return FALSE;
    slash = wcsrchr(dir, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    for (copy = 1; copy <= MAX_LINK_COPIES; copy++) {
        Core_ShortcutFileName(after->name, copy, name, ARRAYSIZE(name));
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\%s", dir, name))) return FALSE;
        if (wcscmp(out, path) == 0) return FALSE;
        /* The same file when only the case differs: renamed in place. */
        if (Core_PathEquals(out, path) || GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------ searches */

typedef struct PathSet {
    WCHAR (*paths)[MAX_PATH];
    DWORD count;
} PathSet;

static BOOL PathSetHas(const PathSet *set, const WCHAR *path)
{
    DWORD i;
    for (i = 0; i < set->count; i++)
        if (Core_PathEquals(set->paths[i], path)) return TRUE;
    return FALSE;
}

/* The shortcuts recorded for `folder` (NULL: for every profile), as many as
 * the key holds. Free `recorded->paths`. */
static void LoadRecordedLinks(const WCHAR *folder, PathSet *recorded)
{
    WCHAR data[FOLDER_CCH];
    HKEY key;
    DWORD i, values = 0, nameCch, dataBytes, dataChars, type;
    LSTATUS status;
    recorded->count = 0;
    recorded->paths = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SHORTCUTS, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    if (RegQueryInfoKeyW(key, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL) != ERROR_SUCCESS ||
        values == 0 ||
        (recorded->paths = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, (size_t)values * sizeof *recorded->paths)) == NULL) {
        RegCloseKey(key);
        return;
    }
    /* A record added meanwhile is a shortcut just made: the next search reads it. */
    for (i = 0; recorded->count < values; i++) {
        nameCch = MAX_PATH;
        dataBytes = sizeof data;
        status = RegEnumValueW(key, i, recorded->paths[recorded->count], &nameCch, NULL, &type, (BYTE *)data, &dataBytes);
        if (status == ERROR_MORE_DATA) continue;   /* not a path or a folder of ours */
        if (status != ERROR_SUCCESS) break;    /* the end, or the key was deleted meanwhile */
        if (type != REG_SZ) continue;
        /* REG_SZ data need not end with its NUL. */
        dataChars = dataBytes / sizeof(WCHAR);
        data[dataChars < ARRAYSIZE(data) ? dataChars : ARRAYSIZE(data) - 1] = 0;
        if (folder && CompareStringOrdinal(data, -1, folder, -1, TRUE) != CSTR_EQUAL) continue;
        recorded->count++;
    }
    RegCloseKey(key);
}

typedef BOOL (*LinkVisitor)(const WCHAR *path, const LinkInfo *info, void *context);   /* FALSE stops */

/* The shortcuts of the folders searched, read once: the desktops hold dozens,
 * and the buttons look at them at every refresh. One is read again when its
 * times or size changed; one that could not be read is not kept. Searches
 * run on the window's thread and on the language's (gui.c), hence the lock. */
#define LINK_CACHE_MAX 1024

typedef struct CachedLink {
    WCHAR    path[MAX_PATH];
    FILETIME created, written;
    DWORD    sizeHigh, sizeLow;
    LinkInfo info;
} CachedLink;

static struct {
    SRWLOCK     lock;
    CachedLink *links;
    size_t      count, capacity;
} g_linkCache = { SRWLOCK_INIT, NULL, 0, 0 };

static BOOL SameFileState(const CachedLink *link, const WIN32_FIND_DATAW *found)
{
    return CompareFileTime(&link->written, &found->ftLastWriteTime) == 0 &&
           CompareFileTime(&link->created, &found->ftCreationTime) == 0 &&
           link->sizeHigh == found->nFileSizeHigh && link->sizeLow == found->nFileSizeLow;
}

static void RememberLink(const WCHAR *path, const WIN32_FIND_DATAW *found, const LinkInfo *info)
{
    CachedLink *link = NULL;
    size_t i;
    AcquireSRWLockExclusive(&g_linkCache.lock);
    for (i = 0; i < g_linkCache.count && !link; i++)
        if (CompareStringOrdinal(g_linkCache.links[i].path, -1, path, -1, TRUE) == CSTR_EQUAL) link = &g_linkCache.links[i];
    if (!link) {
        if (g_linkCache.count == g_linkCache.capacity) {
            size_t capacity = g_linkCache.capacity ? g_linkCache.capacity * 2 : 64;
            void *grown = capacity > LINK_CACHE_MAX ? NULL
                        : g_linkCache.links ? HeapReAlloc(GetProcessHeap(), 0, g_linkCache.links, capacity * sizeof *g_linkCache.links)
                                            : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof *g_linkCache.links);
            if (grown) {
                g_linkCache.links = (CachedLink *)grown;
                g_linkCache.capacity = capacity;
            } else {
                g_linkCache.count = 0;   /* full: what is read from now on is kept instead */
            }
        }
        if (g_linkCache.count < g_linkCache.capacity) link = &g_linkCache.links[g_linkCache.count++];
    }
    if (link && SUCCEEDED(StringCchCopyW(link->path, ARRAYSIZE(link->path), path))) {
        link->created = found->ftCreationTime;
        link->written = found->ftLastWriteTime;
        link->sizeHigh = found->nFileSizeHigh;
        link->sizeLow = found->nFileSizeLow;
        link->info = *info;
    }
    ReleaseSRWLockExclusive(&g_linkCache.lock);
}

/* The shortcut `path` that `found` describes, from the cache when it did not
 * change since it was read. */
static BOOL ReadFolderLink(const LinkReader *reader, const WCHAR *path, const WIN32_FIND_DATAW *found, LinkInfo *info)
{
    BOOL cached = FALSE;
    size_t i;
    AcquireSRWLockShared(&g_linkCache.lock);
    for (i = 0; i < g_linkCache.count && !cached; i++) {
        const CachedLink *link = &g_linkCache.links[i];
        if (SameFileState(link, found) && CompareStringOrdinal(link->path, -1, path, -1, TRUE) == CSTR_EQUAL) {
            *info = link->info;
            cached = TRUE;
        }
    }
    ReleaseSRWLockShared(&g_linkCache.lock);
    if (cached) return TRUE;
    if (!ReadLink(reader, path, info)) return FALSE;
    RememberLink(path, found, info);
    return TRUE;
}

/* Every .lnk directly in `dir` but the `skipped` ones, read with `reader`.
 * FALSE: the visitor stopped. */
static BOOL VisitFolderLinks(const LinkReader *reader, const WCHAR *dir, const PathSet *skipped, LinkVisitor visit, void *context)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    LinkInfo info;
    BOOL going = TRUE;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.lnk", dir))) return TRUE;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) ||
            (skipped && PathSetHas(skipped, path)) || !ReadFolderLink(reader, path, &found, &info))
            continue;
        going = visit(path, &info, context);
    } while (going && FindNextFileW(search, &found));
    FindClose(search);
    return going;
}

/* A recorded shortcut deleted by hand: its folder is there, it is not. One on
 * a drive or share that cannot be reached keeps its record. */
static BOOL IsDeletedLink(const WCHAR *path)
{
    WCHAR folder[MAX_PATH];
    WCHAR *slash;
    if (Util_QueryPath(path, NULL) != PATH_MISSING || FAILED(StringCchCopyW(folder, ARRAYSIZE(folder), path)) ||
        (slash = wcsrchr(folder, L'\\')) == NULL)
        return FALSE;
    *slash = 0;
    return Util_DirExists(folder);
}

/* One of our shortcuts, from VisitOurLinks. `recorded`: its path is in
 * REG_SHORTCUTS; `info` is NULL for a recorded path that does not lead to one
 * of our shortcuts (unreachable, unreadable or changed). FALSE stops the
 * search. */
typedef BOOL (*OurLinkVisitor)(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context);

typedef struct SweepContext {
    const WCHAR   *folder;
    OurLinkVisitor visit;
    void          *context;
} SweepContext;

static BOOL SweptLinkVisitor(const WCHAR *path, const LinkInfo *info, void *context)
{
    const SweepContext *sweep = (const SweepContext *)context;
    return !IsOurs(info, sweep->folder) || sweep->visit(path, info, FALSE, sweep->context);
}

/* The folders where our shortcuts are looked for besides the recorded ones. */
#define SWEEP_DESKTOP    0x1
#define SWEEP_PROGRAMS   0x2
#define SWEEP_STARTUP    0x4
#define SWEEP_START_MENU 0x8   /* our folder in Programs */
#define SWEEP_ALL        (SWEEP_DESKTOP | SWEEP_PROGRAMS | SWEEP_STARTUP | SWEEP_START_MENU)

/* Our shortcuts for the profile in `folder` (NULL: for every profile): the
 * recorded ones, then the others in the `sweep` folders. Each is read once,
 * with one shell link object. The record of a shortcut deleted by hand is
 * forgotten instead, so that no search reads it again. */
static void VisitOurLinks(const WCHAR *folder, DWORD sweep, OurLinkVisitor visit, void *context)
{
    const GUID *const known[] = { &FOLDERID_Desktop, &FOLDERID_Programs, &FOLDERID_Startup };
    const DWORD knownSweeps[] = { SWEEP_DESKTOP, SWEEP_PROGRAMS, SWEEP_STARTUP };
    WCHAR dir[MAX_PATH];
    LinkReader reader;
    LinkInfo info;
    PathSet recorded;
    SweepContext swept;
    BOOL going = TRUE, ours;
    DWORD i;
    size_t folderIndex;

    if (!OpenLinkReader(&reader)) return;
    LoadRecordedLinks(folder, &recorded);
    for (i = 0; i < recorded.count && going; i++) {
        ours = ReadLink(&reader, recorded.paths[i], &info) && IsOurs(&info, folder);
        if (!ours && IsDeletedLink(recorded.paths[i])) ForgetLink(recorded.paths[i]);
        else going = visit(recorded.paths[i], ours ? &info : NULL, TRUE, context);
    }
    swept.folder = folder;
    swept.visit = visit;
    swept.context = context;
    for (folderIndex = 0; folderIndex < ARRAYSIZE(known) && going; folderIndex++)
        if ((sweep & knownSweeps[folderIndex]) && Util_KnownFolder(known[folderIndex], dir, ARRAYSIZE(dir)))
            going = VisitFolderLinks(&reader, dir, &recorded, SweptLinkVisitor, &swept);
    if (going && (sweep & SWEEP_START_MENU) && Shortcut_StartMenuDir(dir, ARRAYSIZE(dir)))
        VisitFolderLinks(&reader, dir, &recorded, SweptLinkVisitor, &swept);
    if (recorded.paths) HeapFree(GetProcessHeap(), 0, recorded.paths);
    CloseLinkReader(&reader);
}

/* ---------------------------------------------------- the profile's own */

typedef struct LinkList {
    WCHAR (*paths)[MAX_PATH];
    DWORD count, capacity;
} LinkList;

static BOOL AddToList(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context)
{
    LinkList *list = (LinkList *)context;
    (void)recorded;
    if (info && SUCCEEDED(StringCchCopyW(list->paths[list->count], MAX_PATH, path))) list->count++;
    return list->count < list->capacity;
}

/* The profile's shortcuts that Claude Desktop Profiles Manager keeps up to
 * date (recorded ones, and ours on the desktop and in the Start menu), taskbar
 * pins excepted. */
DWORD Shortcut_ListOurs(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD capacity)
{
    LinkList list;
    list.paths = paths;
    list.count = 0;
    list.capacity = capacity;
    if (capacity) VisitOurLinks(folder, SWEEP_ALL, AddToList, &list);
    return list.count;
}

/* A shortcut on the desktop (the user's or the public one) that already opens
 * this profile: ours, or any launcher passing its data folder, or, for the
 * stock profile, a plain Claude shortcut. */
typedef struct DesktopSearch {
    const Profile *profile;
    BOOL           found;
} DesktopSearch;

static BOOL DesktopVisitor(const WCHAR *path, const LinkInfo *info, void *context)
{
    DesktopSearch *search = (DesktopSearch *)context;
    (void)path;
    search->found = Core_LinkOpensProfile(info, search->profile->folder, search->profile->dataDir, search->profile->isStock);
    return !search->found;
}

BOOL Shortcut_IsOnDesktop(const Profile *profile)
{
    const GUID *const desktops[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop };
    WCHAR dir[MAX_PATH];
    LinkReader reader;
    DesktopSearch search;
    size_t i;
    search.profile = profile;
    search.found = FALSE;
    if (!OpenLinkReader(&reader)) return FALSE;
    for (i = 0; i < ARRAYSIZE(desktops) && !search.found; i++)
        if (Util_KnownFolder(desktops[i], dir, ARRAYSIZE(dir))) VisitFolderLinks(&reader, dir, NULL, DesktopVisitor, &search);
    CloseLinkReader(&reader);
    return search.found;
}

/* Our shortcuts that put the profile in the Start menu: anywhere below
 * Programs but in the Startup folder (that one is Open at Windows sign-in). */
typedef struct StartMenuSearch {
    WCHAR   programs[MAX_PATH];
    WCHAR   startup[MAX_PATH];
    BOOL    remove;
    BOOL    found;
    HRESULT result;   /* of the removals: the first failure */
} StartMenuSearch;

static BOOL BeginStartMenuSearch(StartMenuSearch *search, BOOL remove)
{
    search->remove = remove;
    search->found = FALSE;
    search->result = S_OK;
    if (!Util_KnownFolder(&FOLDERID_Startup, search->startup, ARRAYSIZE(search->startup))) search->startup[0] = 0;
    return Util_KnownFolder(&FOLDERID_Programs, search->programs, ARRAYSIZE(search->programs));
}

static BOOL StartMenuVisitor(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context)
{
    StartMenuSearch *search = (StartMenuSearch *)context;
    if (!info || !Core_PathUnder(path, search->programs) || (search->startup[0] && Core_PathUnder(path, search->startup)))
        return TRUE;
    search->found = TRUE;
    if (!search->remove) return FALSE;
    RemoveLink(path, recorded, &search->result);
    return TRUE;
}

BOOL Shortcut_IsInStartMenu(const Profile *profile)
{
    StartMenuSearch search;
    if (!BeginStartMenuSearch(&search, FALSE)) return FALSE;
    VisitOurLinks(profile->folder, SWEEP_PROGRAMS | SWEEP_START_MENU, StartMenuVisitor, &search);
    return search.found;
}

/* "Claude (<name>).lnk" in our Start menu folder, recorded like the other
 * shortcuts so it follows renames and goes with the profile. */
HRESULT Shortcut_AddToStartMenu(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR folder[MAX_PATH], lnk[MAX_PATH];
    if (!Shortcut_StartMenuDir(folder, ARRAYSIZE(folder)) || !Util_EnsureDir(folder)) return E_FAIL;
    if (!FreeLinkPath(folder, profile->name, lnk, ARRAYSIZE(lnk))) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    return Shortcut_CreateForProfile(pkg, profile, lnk);
}

/* A shortcut that cannot be deleted stays recorded, and the failure is
 * returned. */
HRESULT Shortcut_RemoveFromStartMenu(const Profile *profile)
{
    StartMenuSearch search;
    if (!BeginStartMenuSearch(&search, TRUE)) return E_FAIL;
    VisitOurLinks(profile->folder, SWEEP_PROGRAMS | SWEEP_START_MENU, StartMenuVisitor, &search);
    return search.result;
}

static BOOL RemoveVisitor(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context)
{
    const WCHAR *kept = (const WCHAR *)context;
    if (info && kept[0] && Core_PathEquals(path, kept)) return TRUE;
    /* A record that does not lead to one of our shortcuts is only forgotten. */
    if (info) RemoveLink(path, recorded, NULL);
    else if (recorded) ForgetLink(path);
    return TRUE;
}

/* Only links that run ClaudeDesktopProfilesManager.exe are touched. Taskbar
 * pins are left to taskbar-pin.c: a pin to a deleted profile still explains
 * itself when clicked. With NULL, every shortcut but the manager's own, which
 * goes last (Shortcut_RemoveManagerLink). */
void Shortcut_RemoveOurs(const Profile *profile)
{
    WCHAR kept[MAX_PATH] = L"", keptFolder[MAX_PATH];
    if (!profile && !ManagerLinkPath(kept, ARRAYSIZE(kept), keptFolder, ARRAYSIZE(keptFolder))) kept[0] = 0;
    VisitOurLinks(profile ? profile->folder : NULL, SWEEP_ALL, RemoveVisitor, kept);
}

/* ---------------------------------------------------- Windows sign-in */

/* Windows opens the shortcuts of its Startup folder at sign-in. */
typedef struct StartupSearch {
    const WCHAR *folder;
    WCHAR        dir[MAX_PATH];
    BOOL         remove;
    BOOL         found;
    HRESULT      result;   /* of the removals: the first failure */
} StartupSearch;

static BOOL StartupVisitor(const WCHAR *path, const LinkInfo *info, void *context)
{
    StartupSearch *search = (StartupSearch *)context;
    if (!IsOurs(info, search->folder)) return TRUE;
    search->found = TRUE;
    if (!search->remove) return FALSE;
    /* Shortcut_SetStartup records the ones it makes. */
    RemoveLink(path, TRUE, &search->result);
    return TRUE;
}

static BOOL SearchStartup(const Profile *profile, BOOL remove, StartupSearch *search)
{
    LinkReader reader;
    search->folder = profile->folder;
    search->remove = remove;
    search->found = FALSE;
    search->result = S_OK;
    if (!Util_KnownFolder(&FOLDERID_Startup, search->dir, ARRAYSIZE(search->dir)) || !OpenLinkReader(&reader)) return FALSE;
    VisitFolderLinks(&reader, search->dir, NULL, StartupVisitor, search);
    CloseLinkReader(&reader);
    return TRUE;
}

BOOL Shortcut_IsAtStartup(const Profile *profile)
{
    StartupSearch search;
    return SearchStartup(profile, FALSE, &search) && search.found;
}

/* The profile opens at Windows sign-in, or stops opening there. The shortcut is
 * recorded, so it follows renames and goes with the profile; one that cannot
 * be deleted stays recorded, and the failure is returned. */
HRESULT Shortcut_SetStartup(const ClaudePackage *pkg, const Profile *profile, BOOL on)
{
    WCHAR lnk[MAX_PATH];
    StartupSearch search;
    if (!SearchStartup(profile, !on, &search)) return E_FAIL;
    if (!on) return search.result;
    if (search.found) return S_OK;
    if (!FreeLinkPath(search.dir, profile->name, lnk, ARRAYSIZE(lnk))) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    return Shortcut_CreateForProfile(pkg, profile, lnk);
}

/* ------------------------------------------------- renames and colors */

/* Gives a profile shortcut the profile's icon and description when they
 * differ (UpdateLink). */
HRESULT Shortcut_Update(const WCHAR *lnk, const Profile *after, const WCHAR *icon)
{
    WCHAR description[DESCRIPTION_CCH];
    ProfileDescription(after, description, ARRAYSIZE(description));
    return UpdateLink(lnk, icon, description);
}

/* A shortcut of ours after a rename or a color change: the new icon and
 * description, and the new name when it is named after the profile. `out`
 * gets the path in use. TRUE when something changed. */
static BOOL RefreshLink(const WCHAR *path, const Profile *before, const Profile *after, const WCHAR *icon,
                        WCHAR *out, size_t cch)
{
    WCHAR renamed[MAX_PATH];
    BOOL changed = FALSE;
    HRESULT updated;
    StringCchCopyW(out, cch, path);
    updated = Shortcut_Update(path, after, icon);
    if (updated == S_OK) {
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, NULL);
        changed = TRUE;
    } else if (FAILED(updated)) {
        Util_Log(L"could not update shortcut %s (0x%08lX)", path, (unsigned long)updated);
    }
    if (Shortcut_RenamedPath(path, before, after, renamed, ARRAYSIZE(renamed))) {
        if (MoveFileW(path, renamed)) {
            SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, renamed);
            StringCchCopyW(out, cch, renamed);
            changed = TRUE;
        } else {
            Util_Log(L"could not rename shortcut %s to %s (error %lu)", path, renamed, GetLastError());
        }
    }
    return changed;
}

typedef struct RefreshSearch {
    const Profile *before;
    const Profile *after;
    const WCHAR   *icon;
    BOOL           changed;
} RefreshSearch;

static BOOL RefreshVisitor(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context)
{
    RefreshSearch *search = (RefreshSearch *)context;
    WCHAR current[MAX_PATH];
    if (!info || !RefreshLink(path, search->before, search->after, search->icon, current, ARRAYSIZE(current))) return TRUE;
    search->changed = TRUE;
    /* The record follows a renamed shortcut: its new path is recorded before
     * the old one goes. */
    if (recorded && !Core_PathEquals(current, path) && RecordLink(current, search->after->folder)) ForgetLink(path);
    return TRUE;
}

/* After a rename or a color change: our shortcuts get the new icon and
 * description, and the ones named after the profile get its new name. Taskbar
 * pins are brought up to date by taskbar-pin.c. TRUE when a shortcut changed. */
BOOL Shortcut_Refresh(const Profile *before, const Profile *after, const WCHAR *icon)
{
    RefreshSearch search;
    search.before = before;
    search.after = after;
    search.icon = icon;
    search.changed = FALSE;
    VisitOurLinks(before->folder, SWEEP_ALL, RefreshVisitor, &search);
    return search.changed;
}

typedef struct LanguageSearch {
    const ProfileList  *list;
    const WCHAR *const *icons;
} LanguageSearch;

static BOOL LanguageVisitor(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context)
{
    const LanguageSearch *search = (const LanguageSearch *)context;
    WCHAR current[MAX_PATH];
    int i;
    (void)recorded;
    if (!info) return TRUE;
    for (i = 0; i < search->list->count; i++) {
        const Profile *profile = &search->list->items[i];
        if (!search->icons[i] || !Core_ArgsSelectProfile(info->args, profile->folder)) continue;
        RefreshLink(path, profile, profile, search->icons[i], current, ARRAYSIZE(current));
        break;
    }
    return TRUE;
}

/* After a change of language: every profile's shortcuts get their
 * description in it, in one pass over the folders for all profiles
 * (`icons[i]`: profile i's icon, NULL to leave its shortcuts as they are). */
void Shortcut_RefreshProfiles(const ProfileList *list, const WCHAR *const *icons)
{
    LanguageSearch search;
    search.list = list;
    search.icons = icons;
    VisitOurLinks(NULL, SWEEP_ALL, LanguageVisitor, &search);
}
