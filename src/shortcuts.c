/*
 * .lnk files. A profile shortcut runs
 * `ClaudeDesktopProfilesManager.exe --launch "<folder>"` and carries its own
 * AppUserModelID, so each profile can be pinned to the taskbar separately.
 */
#include "app.h"
#include <knownfolders.h>
#include <shlobj.h>
#include <propsys.h>

/* PKEY_AppUserModel_ID */
#define MAX_LINK_COPIES 50    /* "Claude (<name>) (n).lnk": n up to this */
#define MAX_RECORDED    256   /* shortcuts recorded in the registry, read at once */

static const PROPERTYKEY kAppUserModelId = {
    { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5
};

BOOL Shortcut_Read(const WCHAR *lnk, LinkInfo *info)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    BOOL ok = FALSE;

    ZeroMemory(info, sizeof *info);
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl)))
        return FALSE;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) &&
        SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ))) {
        ok = TRUE;
        if (IShellLinkW_GetPath(sl, info->target, ARRAYSIZE(info->target), NULL, 0) != S_OK) info->target[0] = 0;
        if (FAILED(IShellLinkW_GetArguments(sl, info->args, ARRAYSIZE(info->args)))) info->args[0] = 0;
        if (!info->target[0]) {
            PIDLIST_ABSOLUTE pidl = NULL;
            if (SUCCEEDED(IShellLinkW_GetIDList(sl, &pidl)) && pidl) {
                PWSTR name = NULL;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
                    StringCchCopyW(info->parsing, ARRAYSIZE(info->parsing), name);
                    CoTaskMemFree(name);
                }
                CoTaskMemFree(pidl);
            }
        }
    }
    if (pf) IPersistFile_Release(pf);
    IShellLinkW_Release(sl);
    return ok;
}

/* The shortcut carries this AppUserModelID. */
BOOL Shortcut_HasAppId(const WCHAR *lnk, const WCHAR *aumid)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    IPropertyStore *ps = NULL;
    PROPVARIANT pv;
    BOOL same = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return FALSE;
    PropVariantInit(&pv);
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) &&
        SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ)) &&
        SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPropertyStore, (void **)&ps)) &&
        SUCCEEDED(IPropertyStore_GetValue(ps, &kAppUserModelId, &pv)))
        same = pv.vt == VT_LPWSTR && pv.pwszVal && CompareStringOrdinal(pv.pwszVal, -1, aumid, -1, TRUE) == CSTR_EQUAL;
    PropVariantClear(&pv);
    if (ps) IPropertyStore_Release(ps);
    if (pf) IPersistFile_Release(pf);
    IShellLinkW_Release(sl);
    return same;
}

static HRESULT WriteLink(const WCHAR *lnk, const WCHAR *exe, const WCHAR *args, const WCHAR *icon,
                         const WCHAR *description, const WCHAR *workDir, const WCHAR *aumid)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    IPropertyStore *ps = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl);
    if (FAILED(hr)) return hr;
    IShellLinkW_SetPath(sl, exe);
    IShellLinkW_SetArguments(sl, args ? args : L"");
    IShellLinkW_SetIconLocation(sl, icon, 0);
    IShellLinkW_SetDescription(sl, description);
    IShellLinkW_SetWorkingDirectory(sl, workDir);
    if (aumid && SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPropertyStore, (void **)&ps))) {
        PROPVARIANT pv;
        size_t bytes = (wcslen(aumid) + 1) * sizeof(WCHAR);
        PropVariantInit(&pv);
        pv.vt = VT_LPWSTR;
        pv.pwszVal = (LPWSTR)CoTaskMemAlloc(bytes);
        if (pv.pwszVal) {
            memcpy(pv.pwszVal, aumid, bytes);
            IPropertyStore_SetValue(ps, &kAppUserModelId, &pv);
            IPropertyStore_Commit(ps);
        }
        PropVariantClear(&pv);
        IPropertyStore_Release(ps);
    }
    hr = IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) {
        hr = IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return hr;
}

/* A profile shortcut, not recorded: taskbar pins are written this way. */
HRESULT Shortcut_WriteForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], icon[MAX_PATH], args[FOLDER_CCH + 16], desc[128], aumid[64];

    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_InstallDir(dir, ARRAYSIZE(dir))) return E_FAIL;
    if (!Icons_Ensure(pkg, profile, icon, ARRAYSIZE(icon))) StringCchCopyW(icon, ARRAYSIZE(icon), exe);
    StringCchPrintfW(args, ARRAYSIZE(args), L"--launch \"%s\"", profile->folder);
    StringCchPrintfW(desc, ARRAYSIZE(desc), L"Open Claude with the %s profile", profile->name);
    Core_ProfileAumid(profile->folder, aumid, ARRAYSIZE(aumid));
    return WriteLink(lnk, exe, args, icon, desc, dir, aumid);
}

HRESULT Shortcut_CreateForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk)
{
    HRESULT hr = Shortcut_WriteForProfile(pkg, profile, lnk);
    if (SUCCEEDED(hr)) {
        Util_RegSetString(HKEY_CURRENT_USER, REG_SHORTCUTS, lnk, profile->folder);
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
        Util_Log(L"created shortcut %s", lnk);
    }
    return hr;
}

/* The Start menu folder of Claude Desktop Profiles Manager: its own shortcut and
 * the profiles added to the Start menu. */
BOOL Shortcut_StartMenuDir(WCHAR *out, size_t cch)
{
    WCHAR programs[MAX_PATH];
    return Util_KnownFolder(&FOLDERID_Programs, programs, ARRAYSIZE(programs)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME, programs));
}

/* The manager's own shortcut, in its Start menu folder. */
static BOOL ManagerLinkPath(WCHAR *out, size_t cch)
{
    WCHAR folder[MAX_PATH];
    return Shortcut_StartMenuDir(folder, ARRAYSIZE(folder)) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME L".lnk", folder));
}

HRESULT Shortcut_CreateManagerLink(const WCHAR *exe)
{
    WCHAR programs[MAX_PATH], folder[MAX_PATH], lnk[MAX_PATH], old[MAX_PATH], dir[MAX_PATH];
    LinkInfo info;
    HRESULT hr;
    if (!Util_KnownFolder(&FOLDERID_Programs, programs, ARRAYSIZE(programs)) || !Shortcut_StartMenuDir(folder, ARRAYSIZE(folder)) ||
        !Util_InstallDir(dir, ARRAYSIZE(dir)) || !Util_EnsureDir(folder) || !ManagerLinkPath(lnk, ARRAYSIZE(lnk)))
        return E_FAIL;
    hr = WriteLink(lnk, exe, L"", exe, L"Manage your Claude Desktop profiles", dir, APP_AUMID_PREFIX L"Manager");
    if (FAILED(hr)) return hr;
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
    /* Before the folder, this shortcut was straight in Programs. */
    if (SUCCEEDED(StringCchPrintfW(old, ARRAYSIZE(old), L"%s\\" APP_NAME L".lnk", programs)) &&
        Shortcut_Read(old, &info) && Core_IsOurExe(info.target) && !info.args[0] && DeleteFileW(old))
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, old, NULL);
    return hr;
}

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

typedef BOOL (*LinkVisitor)(const WCHAR *path, const LinkInfo *info, void *ctx);

/* Calls visit() for every .lnk directly inside dir; stops when it returns FALSE. */
static BOOL VisitLinks(const WCHAR *dir, LinkVisitor visit, void *ctx)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    LinkInfo info;
    BOOL go = TRUE;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.lnk", dir))) return TRUE;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) continue;
        if (!Shortcut_Read(path, &info)) continue;
        go = visit(path, &info, ctx);
    } while (go && FindNextFileW(h, &fd));
    FindClose(h);
    return go;
}

typedef struct FindCtx {
    const Profile *profile;
    WCHAR *found;
    size_t cch;
    BOOL hit;
} FindCtx;

static BOOL FindVisitor(const WCHAR *path, const LinkInfo *info, void *ctx)
{
    FindCtx *f = (FindCtx *)ctx;
    if (!Core_LinkOpensProfile(info, f->profile->folder, f->profile->dataDir, f->profile->isStock)) return TRUE;
    if (f->found) StringCchCopyW(f->found, f->cch, path);
    f->hit = TRUE;
    return FALSE;
}

/* A shortcut on the desktop (the user's or the public one) that already opens
 * this profile: ours, or any launcher passing its data folder, or, for the
 * stock profile, a plain Claude shortcut. */
BOOL Shortcut_FindOnDesktop(const Profile *profile, WCHAR *found, size_t cch)
{
    const GUID *folders[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop };
    WCHAR dir[MAX_PATH];
    FindCtx f;
    size_t i;
    f.profile = profile;
    f.found = found;
    f.cch = cch;
    f.hit = FALSE;
    if (found && cch) found[0] = 0;
    for (i = 0; i < ARRAYSIZE(folders) && !f.hit; i++)
        if (Util_KnownFolder(folders[i], dir, ARRAYSIZE(dir))) VisitLinks(dir, FindVisitor, &f);
    return f.hit;
}

typedef struct OursCtx {
    const Profile *profile; /* NULL: every shortcut of ours */
    WCHAR          kept[MAX_PATH];   /* one left for later, or empty */
} OursCtx;

static BOOL IsOursFor(const LinkInfo *info, const WCHAR *folder)
{
    if (!Core_IsOurExe(info->target)) return FALSE;
    return !folder || Core_ArgsSelectProfile(info->args, folder);
}

static BOOL IsOurs(const LinkInfo *info, const Profile *profile)
{
    return IsOursFor(info, profile ? profile->folder : NULL);
}

/* Shortcuts recorded for `folder` (NULL: all of them), up to `max`. */
static DWORD Recorded(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD max)
{
    WCHAR data[FOLDER_CCH];
    HKEY key;
    DWORD i, n = 0, cch, dataBytes, type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SHORTCUTS, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    for (i = 0; n < max; i++) {
        LSTATUS rc;
        cch = MAX_PATH;
        dataBytes = sizeof data;
        rc = RegEnumValueW(key, i, paths[n], &cch, NULL, &type, (BYTE *)data, &dataBytes);
        if (rc == ERROR_MORE_DATA) continue;
        if (rc != ERROR_SUCCESS) break;    /* the end, or the key was deleted meanwhile */
        if (type != REG_SZ) continue;
        data[ARRAYSIZE(data) - 1] = 0;
        if (folder && CompareStringOrdinal(data, -1, folder, -1, TRUE) != CSTR_EQUAL) continue;
        n++;
    }
    RegCloseKey(key);
    return n;
}

/* Gives a profile shortcut the profile's icon and description when they
 * differ, saved in place with a new last write time. Sends no notification.
 * S_OK: saved; S_FALSE: already current. */
HRESULT Shortcut_Update(const WCHAR *lnk, const Profile *after, const WCHAR *icon)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    WCHAR desc[128], current[MAX_PATH], currentDesc[128];
    WIN32_FILE_ATTRIBUTE_DATA before, now;
    int index = 0;
    HRESULT hr;

    StringCchPrintfW(desc, ARRAYSIZE(desc), L"Open Claude with the %s profile", after->name);
    if (!GetFileAttributesExW(lnk, GetFileExInfoStandard, &before)) return HRESULT_FROM_WIN32(GetLastError());
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl);
    if (FAILED(hr)) return hr;
    hr = IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) hr = IPersistFile_Load(pf, lnk, STGM_READWRITE);
    if (SUCCEEDED(hr) &&
        SUCCEEDED(IShellLinkW_GetIconLocation(sl, current, ARRAYSIZE(current), &index)) && index == 0 &&
        Core_PathEquals(current, icon) &&
        SUCCEEDED(IShellLinkW_GetDescription(sl, currentDesc, ARRAYSIZE(currentDesc))) &&
        wcscmp(currentDesc, desc) == 0)
        hr = S_FALSE;
    if (hr == S_OK) hr = IShellLinkW_SetIconLocation(sl, icon, 0);
    if (hr == S_OK) hr = IShellLinkW_SetDescription(sl, desc);
    if (hr == S_OK) hr = IPersistFile_Save(pf, NULL, TRUE);
    if (pf) IPersistFile_Release(pf);
    IShellLinkW_Release(sl);
    if (hr == S_OK && GetFileAttributesExW(lnk, GetFileExInfoStandard, &now) &&
        Core_SameFatTime(&before.ftLastWriteTime, &now.ftLastWriteTime)) {
        HANDLE f = CreateFileW(lnk, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            ULARGE_INTEGER t;
            FILETIME later;
            t.LowPart = before.ftLastWriteTime.dwLowDateTime;
            t.HighPart = before.ftLastWriteTime.dwHighDateTime;
            t.QuadPart += 20000000ULL;   /* 2 s */
            later.dwLowDateTime = t.LowPart;
            later.dwHighDateTime = t.HighPart;
            SetFileTime(f, NULL, NULL, &later);
            CloseHandle(f);
        }
    }
    return hr;
}

typedef struct ListCtx {
    const WCHAR *folder;
    WCHAR (*paths)[MAX_PATH];
    DWORD n, max;
} ListCtx;

static void AddPath(ListCtx *l, const WCHAR *path)
{
    DWORD i;
    for (i = 0; i < l->n; i++)
        if (Core_PathEquals(l->paths[i], path)) return;
    if (l->n < l->max && SUCCEEDED(StringCchCopyW(l->paths[l->n], MAX_PATH, path))) l->n++;
}

static BOOL ListVisitor(const WCHAR *path, const LinkInfo *info, void *ctx)
{
    ListCtx *l = (ListCtx *)ctx;
    if (IsOursFor(info, l->folder)) AddPath(l, path);
    return l->n < l->max;
}

/* The profile's shortcuts that Claude Desktop Profiles Manager keeps up to
 * date (recorded ones, and ours on the desktop and in the Start menu), taskbar
 * pins excepted. */
DWORD Shortcut_ListOurs(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD max)
{
    const GUID *folders[] = { &FOLDERID_Desktop, &FOLDERID_Programs, &FOLDERID_Startup };
    WCHAR dir[MAX_PATH], (*recorded)[MAX_PATH];
    LinkInfo info;
    ListCtx l;
    DWORD i, n;
    size_t f;
    l.folder = folder;
    l.paths = paths;
    l.n = 0;
    l.max = max;
    recorded = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, MAX_RECORDED * sizeof *recorded);
    if (recorded) {
        n = Recorded(folder, recorded, MAX_RECORDED);
        for (i = 0; i < n; i++)
            if (Shortcut_Read(recorded[i], &info) && IsOursFor(&info, folder)) AddPath(&l, recorded[i]);
        HeapFree(GetProcessHeap(), 0, recorded);
    }
    for (f = 0; f < ARRAYSIZE(folders); f++)
        if (Util_KnownFolder(folders[f], dir, ARRAYSIZE(dir))) VisitLinks(dir, ListVisitor, &l);
    if (Shortcut_StartMenuDir(dir, ARRAYSIZE(dir))) VisitLinks(dir, ListVisitor, &l);
    return l.n;
}

/* Our shortcuts that put the profile in the Start menu: in our Start menu
 * folder or straight in Programs, or recorded below Programs, the Startup
 * folder aside (that one is Open at Windows sign-in). */
#define START_LINKS 64
static DWORD StartMenuLinks(const Profile *profile, WCHAR (*paths)[MAX_PATH], DWORD max)
{
    WCHAR programs[MAX_PATH], startup[MAX_PATH], ours[MAX_PATH], (*recorded)[MAX_PATH];
    LinkInfo info;
    ListCtx l;
    DWORD i, n;
    if (!Util_KnownFolder(&FOLDERID_Programs, programs, ARRAYSIZE(programs))) return 0;
    if (!Util_KnownFolder(&FOLDERID_Startup, startup, ARRAYSIZE(startup))) startup[0] = 0;
    l.folder = profile->folder;
    l.paths = paths;
    l.n = 0;
    l.max = max;
    recorded = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, MAX_RECORDED * sizeof *recorded);
    if (recorded) {
        n = Recorded(profile->folder, recorded, MAX_RECORDED);
        for (i = 0; i < n; i++)
            if (Core_PathUnder(recorded[i], programs) && !(startup[0] && Core_PathUnder(recorded[i], startup)) &&
                Shortcut_Read(recorded[i], &info) && IsOursFor(&info, profile->folder))
                AddPath(&l, recorded[i]);
        HeapFree(GetProcessHeap(), 0, recorded);
    }
    if (Shortcut_StartMenuDir(ours, ARRAYSIZE(ours))) VisitLinks(ours, ListVisitor, &l);
    VisitLinks(programs, ListVisitor, &l);
    return l.n;
}

BOOL Shortcut_IsInStartMenu(const Profile *profile)
{
    WCHAR (*paths)[MAX_PATH] = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, START_LINKS * sizeof *paths);
    DWORD n = 0;
    if (paths) {
        n = StartMenuLinks(profile, paths, START_LINKS);
        HeapFree(GetProcessHeap(), 0, paths);
    }
    return n > 0;
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

void Shortcut_RemoveFromStartMenu(const Profile *profile)
{
    WCHAR (*paths)[MAX_PATH] = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, START_LINKS * sizeof *paths);
    DWORD i, n;
    if (!paths) return;
    n = StartMenuLinks(profile, paths, START_LINKS);
    for (i = 0; i < n; i++) {
        if (DeleteFileW(paths[i])) {
            SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, paths[i], NULL);
            Util_Log(L"removed shortcut %s", paths[i]);
        }
        Util_RegDeleteValue(HKEY_CURRENT_USER, REG_SHORTCUTS, paths[i]);
    }
    HeapFree(GetProcessHeap(), 0, paths);
}

static BOOL RemoveVisitor(const WCHAR *path, const LinkInfo *info, void *ctx)
{
    const OursCtx *o = (const OursCtx *)ctx;
    if (IsOurs(info, o->profile) && !Core_PathEquals(path, o->kept) && DeleteFileW(path)) {
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, NULL);
        Util_Log(L"removed shortcut %s", path);
    }
    return TRUE;
}

/* Shortcuts recorded in the registry (they can be anywhere) plus a sweep of
 * the desktop and the Start menu. Only links that run
 * ClaudeDesktopProfilesManager.exe are touched. Taskbar pins are left to
 * taskbar-pin.c: a pin to a deleted profile still explains itself when
 * clicked. With NULL, every shortcut but the manager's own, which goes last
 * (Shortcut_RemoveManagerLink). */
void Shortcut_RemoveOurs(const Profile *profile)
{
    const GUID *folders[] = { &FOLDERID_Desktop, &FOLDERID_Programs, &FOLDERID_Startup };
    WCHAR dir[MAX_PATH], (*paths)[MAX_PATH];
    OursCtx ctx;
    DWORD i, n;
    size_t f;
    LinkInfo info;

    ctx.profile = profile;
    if (profile || !ManagerLinkPath(ctx.kept, ARRAYSIZE(ctx.kept))) ctx.kept[0] = 0;
    paths = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, MAX_RECORDED * sizeof *paths);
    if (paths) {
        n = Recorded(profile ? profile->folder : NULL, paths, MAX_RECORDED);
        for (i = 0; i < n; i++) {
            if (Shortcut_Read(paths[i], &info) && IsOurs(&info, profile) && DeleteFileW(paths[i])) {
                SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, paths[i], NULL);
                Util_Log(L"removed shortcut %s", paths[i]);
            }
            Util_RegDeleteValue(HKEY_CURRENT_USER, REG_SHORTCUTS, paths[i]);
        }
    }
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    for (f = 0; f < ARRAYSIZE(folders); f++)
        if (Util_KnownFolder(folders[f], dir, ARRAYSIZE(dir))) VisitLinks(dir, RemoveVisitor, &ctx);
    if (Shortcut_StartMenuDir(dir, ARRAYSIZE(dir))) VisitLinks(dir, RemoveVisitor, &ctx);
}

/* The taskbar takes the icon of the manager's windows from the manager's
 * shortcut (their AppUserModelID's), and still looks for it once it is
 * deleted: a window shown after that gets a blank icon. So at uninstall it
 * goes last, when no window is left, with its Start menu folder (unless
 * something else was put in it). */
void Shortcut_RemoveManagerLink(void)
{
    WCHAR dir[MAX_PATH], lnk[MAX_PATH];
    LinkInfo info;
    if (!Shortcut_StartMenuDir(dir, ARRAYSIZE(dir)) || !ManagerLinkPath(lnk, ARRAYSIZE(lnk))) return;
    if (Shortcut_Read(lnk, &info) && Core_IsOurExe(info.target) && DeleteFileW(lnk))
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk, NULL);
    if (RemoveDirectoryW(dir)) SHChangeNotify(SHCNE_RMDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, dir, NULL);
}

/* Windows opens the shortcuts of its Startup folder at sign-in. */
typedef struct StartupCtx {
    const WCHAR *folder;
    BOOL         remove;
    BOOL         hit;
} StartupCtx;

static BOOL StartupVisitor(const WCHAR *path, const LinkInfo *info, void *ctx)
{
    StartupCtx *s = (StartupCtx *)ctx;
    if (!IsOursFor(info, s->folder)) return TRUE;
    s->hit = TRUE;
    if (!s->remove) return FALSE;
    if (DeleteFileW(path)) {
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, NULL);
        Util_Log(L"removed shortcut %s", path);
    }
    Util_RegDeleteValue(HKEY_CURRENT_USER, REG_SHORTCUTS, path);
    return TRUE;
}

BOOL Shortcut_IsAtStartup(const Profile *profile)
{
    WCHAR dir[MAX_PATH];
    StartupCtx s;
    s.folder = profile->folder;
    s.remove = FALSE;
    s.hit = FALSE;
    if (Util_KnownFolder(&FOLDERID_Startup, dir, ARRAYSIZE(dir))) VisitLinks(dir, StartupVisitor, &s);
    return s.hit;
}

/* The profile opens at Windows sign-in, or no longer does. The shortcut is
 * recorded, so it follows renames and goes with the profile. */
HRESULT Shortcut_SetStartup(const ClaudePackage *pkg, const Profile *profile, BOOL on)
{
    WCHAR dir[MAX_PATH], lnk[MAX_PATH];
    StartupCtx s;
    if (!Util_KnownFolder(&FOLDERID_Startup, dir, ARRAYSIZE(dir))) return E_FAIL;
    s.folder = profile->folder;
    s.remove = !on;
    s.hit = FALSE;
    VisitLinks(dir, StartupVisitor, &s);
    if (!on || s.hit) return S_OK;
    if (!FreeLinkPath(dir, profile->name, lnk, ARRAYSIZE(lnk))) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    return Shortcut_CreateForProfile(pkg, profile, lnk);
}

/* "Claude (<label>).lnk" or "Claude (<label>) (n).lnk": a name we gave it. */
static BOOL HasOurName(const WCHAR *path, const WCHAR *label)
{
    const WCHAR *file = wcsrchr(path, L'\\');
    WCHAR expect[MAX_PATH];
    int copy;
    file = file ? file + 1 : path;
    for (copy = 1; copy <= MAX_LINK_COPIES; copy++) {
        Core_ShortcutFileName(label, copy, expect, ARRAYSIZE(expect));
        if (CompareStringOrdinal(file, -1, expect, -1, TRUE) == CSTR_EQUAL) return TRUE;
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

/* A shortcut of ours after a rename or a color change: the new icon and
 * description, and the new name when it is named after the profile. `out`
 * gets the path in use. TRUE when something changed. */
static BOOL RefreshLink(const WCHAR *path, const Profile *before, const Profile *after, const WCHAR *icon,
                        WCHAR *out, size_t cch)
{
    WCHAR target[MAX_PATH];
    BOOL changed = FALSE;
    StringCchCopyW(out, cch, path);
    if (Shortcut_Update(path, after, icon) == S_OK) {
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, NULL);
        changed = TRUE;
    }
    if (Shortcut_RenamedPath(path, before, after, target, ARRAYSIZE(target)) && MoveFileW(path, target)) {
        SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, target);
        StringCchCopyW(out, cch, target);
        changed = TRUE;
    }
    return changed;
}

typedef struct RefreshCtx {
    const Profile *before;
    const Profile *after;
    const WCHAR   *icon;
    BOOL           changed;
} RefreshCtx;

static BOOL RefreshVisitor(const WCHAR *path, const LinkInfo *info, void *ctx)
{
    RefreshCtx *c = (RefreshCtx *)ctx;
    WCHAR now[MAX_PATH];
    if (IsOurs(info, c->before) && RefreshLink(path, c->before, c->after, c->icon, now, ARRAYSIZE(now)))
        c->changed = TRUE;
    return TRUE;
}

/* After a rename or a color change: our shortcuts get the new icon and
 * description, and the ones named after the profile get its new name. Taskbar
 * pins are brought up to date by taskbar-pin.c. TRUE when a shortcut changed. */
BOOL Shortcut_Refresh(const Profile *before, const Profile *after, const WCHAR *icon)
{
    const GUID *folders[] = { &FOLDERID_Desktop, &FOLDERID_Programs };
    WCHAR dir[MAX_PATH], (*paths)[MAX_PATH], now[MAX_PATH];
    RefreshCtx ctx;
    DWORD i, n;
    size_t f;
    LinkInfo info;

    ctx.before = before;
    ctx.after = after;
    ctx.icon = icon;
    ctx.changed = FALSE;

    /* Recorded shortcuts first (they can be anywhere), then the usual folders. */
    paths = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, MAX_RECORDED * sizeof *paths);
    if (paths) {
        n = Recorded(before->folder, paths, MAX_RECORDED);
        for (i = 0; i < n; i++) {
            if (!Shortcut_Read(paths[i], &info) || !IsOurs(&info, before)) continue;
            if (!RefreshLink(paths[i], before, after, icon, now, ARRAYSIZE(now))) continue;
            ctx.changed = TRUE;
            if (!Core_PathEquals(now, paths[i])) {
                Util_RegDeleteValue(HKEY_CURRENT_USER, REG_SHORTCUTS, paths[i]);
                Util_RegSetString(HKEY_CURRENT_USER, REG_SHORTCUTS, now, after->folder);
            }
        }
        HeapFree(GetProcessHeap(), 0, paths);
    }
    for (f = 0; f < ARRAYSIZE(folders); f++)
        if (Util_KnownFolder(folders[f], dir, ARRAYSIZE(dir))) VisitLinks(dir, RefreshVisitor, &ctx);
    if (Shortcut_StartMenuDir(dir, ARRAYSIZE(dir))) VisitLinks(dir, RefreshVisitor, &ctx);
    return ctx.changed;
}
