/*
 * Installation is a copy: the exe lives in %LOCALAPPDATA%\Programs\Claude
 * Desktop Profiles Manager so the claude:// handler and the shortcuts point at
 * a path that does not move. No admin rights, everything under HKCU.
 */
#include "app.h"
#include <shellapi.h>

BOOL Install_IsInstalledCopy(void)
{
    WCHAR self[MAX_PATH], exe[MAX_PATH];
    return Util_SelfExe(self, ARRAYSIZE(self)) && Util_InstallExe(exe, ARRAYSIZE(exe)) && Core_PathEquals(self, exe);
}

BOOL Install_IsRegistered(void)
{
    WCHAR path[MAX_PATH];
    return Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, L"InstallPath", path, ARRAYSIZE(path)) && path[0];
}

/* -------------------------------------------------------------- install */

/* Copies left by an update that found the installed exe running. */
static void RemoveLeftovers(const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.old", dir))) return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, fd.cFileName))) DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* A running exe cannot be overwritten, but it can be renamed: the running
 * process keeps its file and the new copy takes the name. */
static BOOL MoveAside(const WCHAR *exe)
{
    WCHAR aside[MAX_PATH];
    if (FAILED(StringCchPrintfW(aside, ARRAYSIZE(aside), L"%s.%lu.old", exe, GetTickCount()))) return FALSE;
    if (!MoveFileExW(exe, aside, 0)) return FALSE;
    Util_Log(L"the installed copy was in use: moved it to %s", aside);
    return TRUE;
}

static void CloseManagers(DWORD waitMs)
{
    DWORD waited = 0;
    HWND w;
    HANDLE m;
    while ((w = FindWindowW(APP_WINDOW_CLASS, NULL)) != NULL && waited < waitMs) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        Sleep(100);
        waited += 100;
    }
    while (waited < waitMs && (m = OpenMutexW(SYNCHRONIZE, FALSE, MANAGER_MUTEX)) != NULL) {
        CloseHandle(m);
        Sleep(100);
        waited += 100;
    }
}

static DWORD FileSizeKb(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    return fa.nFileSizeLow / 1024 + 1;
}

static void RegisterAll(const WCHAR *exe, const WCHAR *dir)
{
    WCHAR value[MAX_PATH + 32];
    Util_RegSetString(HKEY_CURRENT_USER, REG_ROOT, L"InstallPath", exe);

    Util_RegSetString(HKEY_CURRENT_USER, REG_UNINSTALL, L"DisplayName", APP_NAME);
    Util_RegSetString(HKEY_CURRENT_USER, REG_UNINSTALL, L"DisplayVersion", APP_VERSION_WSTR);
    Util_RegSetString(HKEY_CURRENT_USER, REG_UNINSTALL, L"Publisher", L"Freenitial");
    Util_RegSetString(HKEY_CURRENT_USER, REG_UNINSTALL, L"InstallLocation", dir);
    StringCchPrintfW(value, ARRAYSIZE(value), L"%s,0", exe);
    Util_RegSetString(HKEY_CURRENT_USER, REG_UNINSTALL, L"DisplayIcon", value);
    StringCchPrintfW(value, ARRAYSIZE(value), L"\"%s\" --uninstall", exe);
    Util_RegSetString(HKEY_CURRENT_USER, REG_UNINSTALL, L"UninstallString", value);
    Util_RegSetDword(HKEY_CURRENT_USER, REG_UNINSTALL, L"NoModify", 1);
    Util_RegSetDword(HKEY_CURRENT_USER, REG_UNINSTALL, L"NoRepair", 1);
    Util_RegSetDword(HKEY_CURRENT_USER, REG_UNINSTALL, L"EstimatedSize", FileSizeKb(exe));

    Handler_Register(exe);
}

/* Windows 10 1809 (build 17763) or later. */
static BOOL WindowsSupported(void)
{
    OSVERSIONINFOEXW v;
    DWORDLONG mask = 0;
    ZeroMemory(&v, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    v.dwMajorVersion = 10;
    v.dwBuildNumber = 17763;
    mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&v, VER_MAJORVERSION | VER_BUILDNUMBER, mask);
}

BOOL Install_Run(BOOL openManager)
{
    WCHAR self[MAX_PATH], exe[MAX_PATH], dir[MAX_PATH], zone[MAX_PATH + 32];
    ProfileList list;
    BOOL watched[MAX_PROFILES] = { 0 };
    int tries, i;
    BOOL copied = FALSE;

    if (!WindowsSupported()) {
        Util_Message(NULL, MB_ICONERROR, APP_NAME L" needs Windows 10 version 1809 or later.");
        return FALSE;
    }
    if (!Util_SelfExe(self, ARRAYSIZE(self)) || !Util_InstallExe(exe, ARRAYSIZE(exe)) ||
        !Util_InstallDir(dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir)) {
        Util_Message(NULL, MB_ICONERROR, L"The install folder %%LOCALAPPDATA%%\\Programs\\" APP_NAME L" is not available.");
        return FALSE;
    }
    if (!Core_PathEquals(self, exe)) {
        CloseManagers(5000);
        /* An update: the profiles open with a watcher get one from the new
         * copy (below), the others stay as they are. */
        Profiles_Load(&list);
        for (i = 0; i < list.count; i++) watched[i] = list.items[i].running && Taskbar_IsWatched(&list.items[i]);
        Taskbar_StopWatchers(FALSE);
        RemoveLeftovers(dir);
        for (tries = 0; tries < 40 && !copied; tries++) {
            copied = CopyFileW(self, exe, FALSE);
            if (!copied && GetLastError() == ERROR_SHARING_VIOLATION && MoveAside(exe)) continue;
            if (!copied) Sleep(250);
        }
        if (!copied) {
            DWORD err = GetLastError();
            Util_Message(NULL, MB_ICONERROR, L"Could not copy " APP_NAME L" to\n%s\n\n(error %lu). Close " APP_NAME L" and try again.",
                         exe, err);
            return FALSE;
        }
        /* The installed copy is ours: do not carry the download's zone mark. */
        if (SUCCEEDED(StringCchPrintfW(zone, ARRAYSIZE(zone), L"%s:Zone.Identifier", exe))) DeleteFileW(zone);
    }

    RegisterAll(exe, dir);
    Shortcut_CreateManagerLink(exe);
    Util_Log(L"installed version %s at %s", APP_VERSION_WSTR, exe);
    for (i = 0; i < MAX_PROFILES; i++)
        if (watched[i]) Taskbar_Watch(&list.items[i]);
    /* The manager then offers to make Claude Desktop Profiles Manager the app
     * for claude:// links, which only the user can do (in Windows Settings).
     * The user started this copy, so the manager may come to the front. */
    if (openManager) AllowSetForegroundWindow(ASFW_ANY);
    if (openManager && !Util_Spawn(exe, L"--set-up-links")) {
        DWORD err = GetLastError();
        Util_Log(L"could not start %s (error %lu)", exe, err);
        Util_Message(NULL, MB_ICONERROR,
                     APP_NAME L" was installed but could not be started (error %lu):\n%s\n\nSecurity software may be blocking it.",
                     err, exe);
        return FALSE;
    }
    return TRUE;
}

/* Every time the manager opens from the installed copy: re-assert everything
 * Windows or Claude might have changed since. Idempotent. */
void Install_Repair(void)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH];
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_InstallDir(dir, ARRAYSIZE(dir))) return;
    RegisterAll(exe, dir);
    RemoveLeftovers(dir);
    Update_RemoveDownload();
}

/* -------------------------------------------------------------- uninstall */

static void DeleteTreeNow(const WCHAR *dir)
{
    WCHAR from[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    if (!Util_DirExists(dir)) return;
    ZeroMemory(from, sizeof from);
    if (FAILED(StringCchCopyW(from, MAX_PATH, dir))) return;
    ZeroMemory(&op, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

/* Right before exiting, once no window of ours is left: what the uninstall
 * could not remove while they showed. The manager's shortcut gives them their
 * taskbar icon (Shortcut_RemoveManagerLink). The running exe cannot delete its
 * own folder: a hidden cmd removes it once this process has exited (a second
 * attempt covers a slow exit). */
void Install_FinishUninstall(void)
{
    WCHAR dir[MAX_PATH], sys[MAX_PATH], cmd[4 * MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    Shortcut_RemoveManagerLink();
    if (!Install_IsInstalledCopy() || !Util_InstallDir(dir, ARRAYSIZE(dir))) return;
    if (!GetSystemDirectoryW(sys, ARRAYSIZE(sys))) return;
    /* /s /c "...": cmd drops only the outer quotes and keeps the inner ones. */
    if (FAILED(StringCchPrintfW(cmd, ARRAYSIZE(cmd),
                                L"\"%s\\cmd.exe\" /d /s /c \"\"%s\\ping.exe\" -n 3 127.0.0.1 >nul & rd /s /q \"%s\" & "
                                L"\"%s\\ping.exe\" -n 4 127.0.0.1 >nul & rd /s /q \"%s\"\"", sys, sys, dir, sys, dir)))
        return;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, sys, &si, &pi) ||
        CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

BOOL Install_Uninstall(HWND owner, const ProfileList *list, const BOOL *removeData)
{
    WCHAR state[MAX_PATH];
    int i;

    for (i = 0; i < list->count; i++) {
        if (removeData[i] && list->items[i].running) {
            Util_Message(owner, MB_ICONWARNING,
                         L"Quit Claude for the \x201C%s\x201D profile first (right-click its icon in the notification area, then Quit), then try again.",
                         list->items[i].name);
            return FALSE;
        }
    }

    Taskbar_StopWatchers(TRUE);
    Handler_Unregister();
    TaskbarPin_RemoveOurs();
    Shortcut_RemoveOurs(NULL);
    for (i = 0; i < list->count; i++) {
        WCHAR marker[MAX_PATH];
        const Profile *p = &list->items[i];
        if (p->isStock) continue;
        if (removeData[i]) {
            RemoveResult r = Profiles_RecycleData(owner, p);
            if (r == REMOVE_FAILED)
                Util_Message(owner, MB_ICONWARNING, L"The data of \x201C%s\x201D could not be removed:\n%s", p->name, p->dataDir);
            else if (r == REMOVE_CANCELLED)
                Util_Log(L"kept %s: removal cancelled", p->dataDir);
        } else if (!Profiles_IsLinked(p) &&
                   SUCCEEDED(StringCchPrintfW(marker, ARRAYSIZE(marker), L"%s\\Local State", p->dataDir)) &&
                   !Util_FileExists(marker)) {
            /* A kept profile never opened is an empty folder: without our
             * registry entry nothing would find it again, and it would block
             * its own name. On a plain folder RemoveDirectory only removes an
             * empty one; a link is skipped, since it would go whatever it
             * leads to (its target may just be offline). */
            RemoveDirectoryW(p->dataDir);
        }
    }
    Util_RegDeleteTree(HKEY_CURRENT_USER, REG_UNINSTALL);
    Util_RegDeleteTree(HKEY_CURRENT_USER, REG_ROOT);
    if (Util_StateDir(state, ARRAYSIZE(state))) DeleteTreeNow(state);
    return TRUE;
}
