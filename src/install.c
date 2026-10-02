/*
 * Installation is a copy: the exe lives in %LOCALAPPDATA%\Programs\Claude
 * Desktop Profiles Manager so the claude:// handler and the shortcuts point at
 * a path that does not move. No admin rights, everything under HKCU.
 */
#include "app.h"
#include "resource.h"
#include <shellapi.h>

#define MANAGER_CLOSE_WAIT_MS 5000   /* an open manager closes its windows, then exits */
#define INSTALL_ATTEMPTS      40     /* INSTALL_RETRY_MS apart: a scanner or an exiting copy lets go of a file */
#define INSTALL_RETRY_MS      250
#define STAGED_SUFFIX         L".new"
#define ASIDE_SUFFIX          L".old"
/* What is removed after this process exits: ping waits about one second per
 * count less one, before the first attempt, then before the second. */
#define EXIT_WAIT_PINGS       3
#define SLOW_EXIT_WAIT_PINGS  4
#define REMOVALS_CCH          (4 * MAX_PATH)

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

/* ------------------------------------------------------- after this exits */

/* `command "path"` added to the cmd commands in `removals`. Nothing is added,
 * and that is logged, when it does not fit whole (a cut path could name
 * another folder) or holds a %, which cmd expands even inside quotes. */
static BOOL AppendRemoval(WCHAR *removals, size_t cch, const WCHAR *command, const WCHAR *path)
{
    size_t used = wcslen(removals);
    if (!wcschr(path, L'%') &&
        SUCCEEDED(StringCchPrintfW(removals + used, cch - used, L"%s%s \"%s\"", used ? L" & " : L"", command, path)))
        return TRUE;
    removals[used] = 0;
    Util_Log(L"could not prepare the removal of %s after exit", path);
    return FALSE;
}

/* Runs the cmd commands `removals` in a hidden cmd once this process has
 * exited, and once more a little later for a slow exit: a running exe cannot
 * delete its own file or folder. */
static void RemoveAfterExit(const WCHAR *removals)
{
    WCHAR systemDir[MAX_PATH], commandLine[3 * MAX_PATH + 2 * REMOVALS_CCH + 128];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    UINT length = GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir));
    /* /d /v:off: no AutoRun command, and a ! in a path stays one whatever the
     * registry turns on. /s /c "...": cmd drops only the outer quotes and
     * keeps the inner ones. */
    if (length == 0 || length >= ARRAYSIZE(systemDir) ||
        FAILED(StringCchPrintfW(commandLine, ARRAYSIZE(commandLine),
                                L"\"%s\\cmd.exe\" /d /v:off /s /c \"\"%s\\ping.exe\" -n %d 127.0.0.1 >nul & %s & "
                                L"\"%s\\ping.exe\" -n %d 127.0.0.1 >nul & %s\"",
                                systemDir, systemDir, EXIT_WAIT_PINGS, removals, systemDir, SLOW_EXIT_WAIT_PINGS, removals))) {
        Util_Log(L"could not prepare the removal of %s after exit", removals);
        return;
    }
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, systemDir,
                       &startup, &process) ||
        CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, systemDir, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return;
    }
    Util_Log(L"could not start the removal of %s after exit (error %lu)", removals, GetLastError());
}

/* -------------------------------------------------------------- install */

/* What an install leaves when it fails half way or finds the installed copy
 * running: the new copy before it took its place, the old one moved aside. */
static void RemoveLeftovers(const WCHAR *dir)
{
    const WCHAR *const patterns[] = { L"*" STAGED_SUFFIX, L"*" ASIDE_SUFFIX };
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    size_t i;
    for (i = 0; i < ARRAYSIZE(patterns); i++) {
        if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\%s", dir, patterns[i]))) continue;
        search = FindFirstFileW(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName))) DeleteFileW(path);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
}

/* A running exe cannot be replaced, but it can be renamed: the running
 * process keeps its file and the new copy takes the name. */
static BOOL MoveAside(const WCHAR *exe, WCHAR *aside, size_t cch)
{
    if (FAILED(StringCchPrintfW(aside, cch, L"%s.%lu" ASIDE_SUFFIX, exe, GetTickCount())) ||
        !MoveFileExW(exe, aside, MOVEFILE_WRITE_THROUGH)) {
        aside[0] = 0;
        return FALSE;
    }
    Util_Log(L"the installed copy was in use: moved it to %s", aside);
    return TRUE;
}

/* Gives the file open as `file` (with DELETE access) the path `name`. */
static BOOL RenameOpenFile(HANDLE file, const WCHAR *name, BOOL replace)
{
    size_t nameBytes = wcslen(name) * sizeof(WCHAR), bytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    FILE_RENAME_INFO *renameRequest = (FILE_RENAME_INFO *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    BOOL renamed;
    DWORD error;
    if (!renameRequest) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    renameRequest->ReplaceIfExists = (BOOLEAN)(replace != FALSE);
    renameRequest->FileNameLength = (DWORD)nameBytes;
    memcpy(renameRequest->FileName, name, nameBytes);
    renamed = SetFileInformationByHandle(file, FileRenameInfo, renameRequest, (DWORD)bytes);
    error = GetLastError();
    HeapFree(GetProcessHeap(), 0, renameRequest);
    SetLastError(error);
    return renamed;
}

/* The staged copy becomes the installed exe in one rename, so that the
 * installed exe is never half written; its data reaches the disk first. It
 * is held open, unshared, until it has the installed name: a running
 * installed copy moved aside for it is missing only between two renames, and
 * is put back at once if the new copy still cannot take its name. */
static BOOL TakeInstalledName(const WCHAR *staged, const WCHAR *exe, WCHAR *aside, size_t asideCch)
{
    HANDLE copy = CreateFileW(staged, GENERIC_WRITE | DELETE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL renamed;
    DWORD error;
    if (copy == INVALID_HANDLE_VALUE) return FALSE;
    renamed = FlushFileBuffers(copy) && RenameOpenFile(copy, exe, TRUE);
    error = GetLastError();
    if (!renamed && !aside[0] && (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) &&
        MoveAside(exe, aside, asideCch)) {
        renamed = RenameOpenFile(copy, exe, FALSE);
        error = GetLastError();
        if (!renamed && MoveFileExW(aside, exe, MOVEFILE_WRITE_THROUGH)) aside[0] = 0;
    }
    CloseHandle(copy);
    SetLastError(error);
    return renamed;
}

/* This exe over the installed one: copied next to it first, then renamed
 * over it, so that a failure at any point leaves the installed copy as it
 * was. `*error` says why it failed. */
static BOOL ReplaceInstalledCopy(const WCHAR *self, const WCHAR *exe, const WCHAR *dir, DWORD *error)
{
    WCHAR staged[MAX_PATH], aside[MAX_PATH] = L"", zone[MAX_PATH + 32];
    BOOL copied = FALSE, replaced = FALSE;
    int attempt;
    *error = ERROR_SUCCESS;
    if (FAILED(StringCchPrintfW(staged, ARRAYSIZE(staged), L"%s" STAGED_SUFFIX, exe))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    RemoveLeftovers(dir);
    for (attempt = 0; attempt < INSTALL_ATTEMPTS && !replaced; attempt++) {
        if (attempt) Sleep(INSTALL_RETRY_MS);
        /* A copy keeps the read-only mark of a file on a read-only medium:
         * the staged copy is opened for writing to be flushed. */
        if (!copied) copied = CopyFileW(self, staged, FALSE) && SetFileAttributesW(staged, FILE_ATTRIBUTE_NORMAL);
        if (copied) replaced = TakeInstalledName(staged, exe, aside, ARRAYSIZE(aside));
        if (!replaced) {
            *error = GetLastError();
            /* A manager started meanwhile removes the staged copy it finds
             * (RemoveLeftovers): it is made again. */
            if (*error == ERROR_FILE_NOT_FOUND) copied = FALSE;
        }
    }
    if (!replaced) {
        DeleteFileW(staged);
        if (aside[0] && !MoveFileExW(aside, exe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            Util_Log(L"could not restore the installed copy from %s (error %lu)", aside, GetLastError());
        Util_Log(L"could not install %s (error %lu)", exe, *error);
        return FALSE;
    }
    /* Still running, the old copy stays until the next start (RemoveLeftovers). */
    if (aside[0]) DeleteFileW(aside);
    /* The installed copy is ours: it does not carry the download's zone mark. */
    if (SUCCEEDED(StringCchPrintfW(zone, ARRAYSIZE(zone), L"%s:Zone.Identifier", exe))) DeleteFileW(zone);
    return TRUE;
}

/* The profiles running with a watcher, which get one again after a stop
 * (RestartWatchers). */
static void FindWatchedProfiles(const ProfileList *list, BOOL *watched)
{
    int i;
    for (i = 0; i < list->count; i++) watched[i] = list->items[i].running && Taskbar_IsWatched(&list->items[i]);
}

static void RestartWatchers(const ProfileList *list, const BOOL *watched)
{
    int i;
    for (i = 0; i < list->count; i++)
        if (watched[i] && Claude_IsRunning(&list->items[i])) Taskbar_Watch(&list->items[i]);
}

/* An open manager runs the installed exe: it is asked to close, and given
 * MANAGER_CLOSE_WAIT_MS to exit. TRUE when one was open. */
static BOOL CloseManagers(void)
{
    HWND window = FindWindowW(APP_WINDOW_CLASS, NULL);
    DWORD processId = 0;
    HANDLE process;
    if (!window) return FALSE;
    GetWindowThreadProcessId(window, &processId);
    process = processId && processId != GetCurrentProcessId() ? OpenProcess(SYNCHRONIZE, FALSE, processId) : NULL;
    PostMessageW(window, WM_CLOSE, 0, 0);
    if (process) {
        WaitForSingleObject(process, MANAGER_CLOSE_WAIT_MS);
        CloseHandle(process);
    }
    return TRUE;
}

static DWORD FileSizeKb(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    ULARGE_INTEGER size;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) return 0;
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    return (DWORD)((size.QuadPart + 1023) / 1024);
}

static BOOL SetUninstallString(const WCHAR *value, const WCHAR *data)
{
    return Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_UNINSTALL, value, data, NULL);
}

static BOOL SetUninstallDword(const WCHAR *value, DWORD data)
{
    DWORD stored;
    return (Util_RegGetDword(HKEY_CURRENT_USER, REG_UNINSTALL, value, &stored) && stored == data) ||
           Util_RegSetDword(HKEY_CURRENT_USER, REG_UNINSTALL, value, data);
}

/* The Apps & features entry, the claude:// handler and the install path.
 * Values already right are not written again: the manager watches REG_ROOT. */
static BOOL RegisterAll(const WCHAR *exe, const WCHAR *dir)
{
    WCHAR icon[MAX_PATH + 8], uninstall[MAX_PATH + 32];
    return SUCCEEDED(StringCchPrintfW(icon, ARRAYSIZE(icon), L"%s,0", exe)) &&
           SUCCEEDED(StringCchPrintfW(uninstall, ARRAYSIZE(uninstall), L"\"%s\" --uninstall", exe)) &&
           SetUninstallString(L"DisplayName", APP_NAME) &&
           SetUninstallString(L"DisplayVersion", APP_VERSION_WSTR) &&
           SetUninstallString(L"Publisher", L"Freenitial") &&
           SetUninstallString(L"InstallLocation", dir) &&
           SetUninstallString(L"DisplayIcon", icon) &&
           SetUninstallString(L"UninstallString", uninstall) &&
           SetUninstallDword(L"NoModify", 1) &&
           SetUninstallDword(L"NoRepair", 1) &&
           SetUninstallDword(L"EstimatedSize", FileSizeKb(exe)) &&
           Handler_Register(exe) &&
           Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_ROOT, L"InstallPath", exe, NULL);
}

/* Windows 10 1809 (build 17763) or later. Builds before 1607 cannot even load
 * this exe (it imports GetDpiForWindow): this explains it to 1607 to 1803. */
static BOOL WindowsSupported(void)
{
    OSVERSIONINFOEXW version;
    DWORDLONG mask = 0;
    ZeroMemory(&version, sizeof version);
    version.dwOSVersionInfoSize = sizeof version;
    version.dwMajorVersion = 10;
    version.dwBuildNumber = 17763;
    mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&version, VER_MAJORVERSION | VER_BUILDNUMBER, mask);
}

/* The user started this copy, so the manager may come to the front. After a
 * first install it offers to make Claude Desktop Profiles Manager the app for
 * claude:// links, which only the user can do (in Windows Settings); an
 * update does not ask again. */
static BOOL StartInstalledManager(const WCHAR *exe, BOOL update)
{
    DWORD error;
    AllowSetForegroundWindow(ASFW_ANY);
    if (Util_Spawn(exe, update ? L"" : L"--set-up-links", NULL)) return TRUE;
    error = GetLastError();
    Util_Log(L"could not start %s (error %lu)", exe, error);
    Ui_Message(NULL, MB_ICONERROR,
               TR(APP_NAME L" was installed but could not be started (error %lu):\n%s\n\nSecurity software may be blocking it."),
               error, exe);
    return FALSE;
}

/* An update runs from its download (Update_Run), which goes once it has
 * exited. */
static void RemoveDownloadAfterExit(const WCHAR *self)
{
    WCHAR download[MAX_PATH], removals[REMOVALS_CCH] = L"";
    if (Update_DownloadPath(download, ARRAYSIZE(download)) && Core_PathEquals(self, download) &&
        AppendRemoval(removals, ARRAYSIZE(removals), L"del /f /q", self))
        RemoveAfterExit(removals);
}

/* An install that failed, once the user has read why: with `openManager`, the
 * installed manager opens (the one the installer closed, or the one asked
 * for once the copy is in place). An update's download goes as after a
 * success: the manager downloads it again. FALSE. */
static BOOL EndFailedInstall(const WCHAR *self, const WCHAR *exe, BOOL openManager)
{
    if (openManager) {
        AllowSetForegroundWindow(ASFW_ANY);
        if (!Util_Spawn(exe, L"", NULL)) Util_Log(L"could not open the manager %s (error %lu)", exe, GetLastError());
    }
    RemoveDownloadAfterExit(self);
    return FALSE;
}

BOOL Install_Run(BOOL openManager)
{
    WCHAR self[MAX_PATH], exe[MAX_PATH], dir[MAX_PATH];
    ProfileList list;
    BOOL watched[MAX_PROFILES] = { 0 };
    BOOL copying, update, registered, managerClosed = FALSE, started;
    DWORD error;
    HRESULT link;

    if (!WindowsSupported()) {
        Ui_Message(NULL, MB_ICONERROR, TR(APP_NAME L" needs Windows 10 version 1809 or later."));
        return FALSE;
    }
    if (!Util_SelfExe(self, ARRAYSIZE(self)) || !Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_InstallDir(dir, ARRAYSIZE(dir))) {
        Ui_Message(NULL, MB_ICONERROR, TR(L"The install folder %%LOCALAPPDATA%%\\Programs\\" APP_NAME L" is not available."));
        return FALSE;
    }
    if (!Util_EnsureDir(dir)) {
        error = GetLastError();
        Util_Log(L"could not create the install folder %s (error %lu)", dir, error);
        Ui_Message(NULL, MB_ICONERROR, TR(L"Could not create %s (error %lu)."), dir, error);
        return FALSE;
    }
    copying = !Core_PathEquals(self, exe);
    update = Util_FileExists(exe);
    if (copying) {
        managerClosed = CloseManagers();
        /* The profiles open with a watcher get one from the new copy (below),
         * the others stay as they are. */
        Profiles_Load(&list, NULL);
        FindWatchedProfiles(&list, watched);
        if (!Taskbar_StopWatchers(FALSE)) {
            RestartWatchers(&list, watched);   /* those that did stop */
            Ui_Message(NULL, MB_ICONERROR, TR(APP_NAME L" could not be installed: it is still running for an open profile. Try again in a moment."));
            return EndFailedInstall(self, exe, managerClosed);
        }
        if (!ReplaceInstalledCopy(self, exe, dir, &error)) {
            RestartWatchers(&list, watched);
            Ui_Message(NULL, MB_ICONERROR, TR(L"Could not copy " APP_NAME L" to\n%s\n\n(error %lu). Close " APP_NAME L" and try again."),
                       exe, error);
            return EndFailedInstall(self, exe, managerClosed);
        }
    }

    registered = RegisterAll(exe, dir);
    link = registered ? Shortcut_CreateManagerLink(exe) : E_FAIL;
    if (copying) RestartWatchers(&list, watched);
    if (!registered || FAILED(link)) {
        Util_Log(L"installation registration failed at %s (registered %d, shortcut 0x%08lX)", exe, registered, (unsigned long)link);
        Ui_Message(NULL, MB_ICONERROR,
                   TR(APP_NAME L" was installed but could not be fully registered for this user (claude:// links, Start menu).\n\n"
                      L"It registers itself again each time it opens. Security software may be blocking it."));
        return EndFailedInstall(self, exe, managerClosed || openManager);
    }
    Util_Log(L"installed version %s at %s", APP_VERSION_WSTR, exe);
    started = !openManager || StartInstalledManager(exe, update);
    RemoveDownloadAfterExit(self);
    return started;
}

/* What Windows shows of the program and keeps for it, as the install made
 * it. Values already right are not written again. */
static void RepairRegistration(const WCHAR *exe, const WCHAR *dir)
{
    HRESULT link;
    if (!RegisterAll(exe, dir)) Util_Log(L"could not repair installation registration at %s", exe);
    link = Shortcut_RestoreManagerLink(exe);
    if (FAILED(link)) Util_Log(L"could not restore the manager's shortcut (0x%08lX)", (unsigned long)link);
    else if (link == S_OK) Util_Log(L"wrote the manager's shortcut again");
}

/* Every time the manager opens from the installed copy: re-assert everything
 * Windows or Claude might have changed since. Idempotent. */
void Install_Repair(void)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH];
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_InstallDir(dir, ARRAYSIZE(dir))) return;
    RepairRegistration(exe, dir);
    RemoveLeftovers(dir);
    Update_RemoveDownload();
}

/* After a change of the interface language, what Windows shows of the
 * program in it: the claude:// handler's name and description, and the
 * tooltips of the manager's shortcut and of the profiles' shortcuts and pins.
 * Only what differs is written, in one pass for all profiles; most of the
 * time goes to Explorer taking each changed pin in. */
void Install_ApplyLanguage(const ClaudePackage *pkg, const ProfileList *list)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], icons[MAX_PROFILES][MAX_PATH];
    const WCHAR *iconOf[MAX_PROFILES];
    ULONGLONG start = GetTickCount64();
    int i;
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Util_InstallDir(dir, ARRAYSIZE(dir))) return;
    RepairRegistration(exe, dir);
    for (i = 0; i < list->count && i < MAX_PROFILES; i++)
        iconOf[i] = Icons_Ensure(pkg, &list->items[i], icons[i], ARRAYSIZE(icons[i])) ? icons[i] : NULL;
    Shortcut_RefreshProfiles(list, iconOf);
    TaskbarPin_RefreshProfiles(list, iconOf);
    Util_Log(L"shortcuts and pins follow the new language (%llu ms)", GetTickCount64() - start);
}

/* -------------------------------------------------------------- uninstall */

/* FALSE when something is left, with SHFileOperation's answer in `*error`. */
static BOOL DeleteTreeNow(const WCHAR *dir, int *error)
{
    WCHAR from[MAX_PATH + 2];
    SHFILEOPSTRUCTW operation;
    *error = 0;
    if (!Util_DirExists(dir)) return TRUE;
    ZeroMemory(from, sizeof from);
    if (FAILED(StringCchCopyW(from, MAX_PATH, dir))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    ZeroMemory(&operation, sizeof operation);
    operation.wFunc = FO_DELETE;
    operation.pFrom = from;
    operation.fFlags = FOF_NO_UI;
    *error = SHFileOperationW(&operation);
    return *error == 0 && !operation.fAnyOperationsAborted;
}

/* Right before exiting, once no window of ours is left: what the uninstall
 * could not remove while they showed. The manager's shortcut gives them their
 * taskbar icon (Shortcut_RemoveManagerLink). The state folder goes after the
 * last line logged, which would make it again: only a failure logged after
 * that makes it again, to say what was left. What is still in use (this exe
 * and its folder, an icon Explorer holds, a download that ended meanwhile)
 * goes once this process has exited. */
void Install_FinishUninstall(void)
{
    WCHAR installDir[MAX_PATH], stateDir[MAX_PATH], download[MAX_PATH], removals[REMOVALS_CCH] = L"";
    BOOL hasState;
    int error;
    Shortcut_RemoveManagerLink();
    hasState = Util_StateDir(stateDir, ARRAYSIZE(stateDir));
    if (hasState && !DeleteTreeNow(stateDir, &error)) Util_Log(L"could not remove %s (error %d)", stateDir, error);
    if (!Install_IsInstalledCopy() || !Util_InstallDir(installDir, ARRAYSIZE(installDir)) ||
        !AppendRemoval(removals, ARRAYSIZE(removals), L"rd /s /q", installDir))
        return;
    if (hasState) AppendRemoval(removals, ARRAYSIZE(removals), L"rd /s /q", stateDir);
    if (Update_DownloadPath(download, ARRAYSIZE(download))) AppendRemoval(removals, ARRAYSIZE(removals), L"del /f /q", download);
    RemoveAfterExit(removals);
}

/* A running kept profile may have session changes waiting for it to close
 * (SessionEdit_Change), which its watcher makes, and the uninstall stops it:
 * for each, the user decides whether to go on without them. */
static BOOL ConfirmDiscardingWaitingChanges(HWND owner, const ProfileList *list)
{
    WCHAR text[1024];
    PendingEdit first;
    int i;
    for (i = 0; i < list->count; i++) {
        const Profile *p = &list->items[i];
        if (!p->running || SessionStore_LoadPending(p, &first, 1) == 0) continue;
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Session changes for \x201C%s\x201D are waiting for it to close, and uninstalling now discards them.\n\n"
                            L"To keep them, choose Cancel, quit Claude for this profile (right-click its icon in the notification area and choose Quit), then uninstall."),
                         p->name);
        if (!Ui_Ask(owner, IDI_WARNING, text, TR(L"Uninstall"), TR(L"Cancel"), TRUE)) return FALSE;
    }
    return TRUE;
}

/* The files a profile gets before its first start (Profiles_CopySettings). */
static const WCHAR *const kCopiedSettings[] = { CLAUDE_DESKTOP_SETTINGS, CLAUDE_APP_SETTINGS };

/* Whether `dir` holds anything but the settings copied to a profile (one that
 * cannot be listed does). A missing folder holds nothing. */
static BOOL HoldsMoreThanCopiedSettings(const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    BOOL more = FALSE, copied;
    DWORD error;
    size_t i;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", dir))) return TRUE;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    }
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        copied = FALSE;
        for (i = 0; i < ARRAYSIZE(kCopiedSettings) && !copied; i++)
            copied = !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                     CompareStringOrdinal(found.cFileName, -1, kCopiedSettings[i], -1, TRUE) == CSTR_EQUAL;
        more = !copied;
    } while (!more && FindNextFileW(search, &found));
    FindClose(search);
    return more;
}

/* A kept profile never opened holds at most the settings copied to it: with
 * our registry entry gone nothing would find it again, and it would block its
 * own name. Its folder is listed first, so that one holding anything else
 * keeps everything. A link is skipped, since it would go whatever it leads to
 * (its target may just be offline). */
static void RemoveNeverOpened(const Profile *p)
{
    WCHAR path[MAX_PATH];
    DWORD error;
    size_t i;
    if (Claude_IsRunning(p) || Profiles_IsLinked(p) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Local State", p->dataDir)) || Util_FileExists(path))
        return;
    if (HoldsMoreThanCopiedSettings(p->dataDir)) {
        Util_Log(L"kept %s, which holds more than a profile never opened", p->dataDir);
        return;
    }
    for (i = 0; i < ARRAYSIZE(kCopiedSettings); i++)
        if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", p->dataDir, kCopiedSettings[i]))) DeleteFileW(path);
    if (RemoveDirectoryW(p->dataDir)) return;
    error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        Util_Log(L"could not remove %s (error %lu)", p->dataDir, error);
}

BOOL Install_Uninstall(HWND owner, const ProfileList *list, const BOOL *removeData)
{
    BOOL watched[MAX_PROFILES] = { 0 };
    LSTATUS status;
    int i;

    for (i = 0; i < list->count; i++) {
        if (removeData[i] && list->items[i].running) {
            Ui_Message(owner, MB_ICONWARNING,
                       TR(L"Quit Claude for the \x201C%s\x201D profile first (right-click its icon in the notification area and choose Quit), then try again."),
                       list->items[i].name);
            return FALSE;
        }
    }
    if (!ConfirmDiscardingWaitingChanges(owner, list)) return FALSE;
    FindWatchedProfiles(list, watched);
    if (!Taskbar_StopWatchers(TRUE)) {
        RestartWatchers(list, watched);   /* those that did stop */
        Ui_Message(owner, MB_ICONERROR, TR(APP_NAME L" could not be removed: it is still running for an open profile. Try again in a moment."));
        return FALSE;
    }
    Handler_Unregister();
    TaskbarPin_RemoveOurs();
    Shortcut_RemoveOurs(NULL);
    for (i = 0; i < list->count; i++) {
        const Profile *p = &list->items[i];
        if (p->isStock) continue;
        if (removeData[i]) {
            RemoveResult removal = Profiles_RecycleData(owner, p);
            if (removal == REMOVE_FAILED)
                Ui_Message(owner, MB_ICONWARNING, TR(L"The data of \x201C%s\x201D could not be removed:\n%s"), p->name, p->dataDir);
            else if (removal == REMOVE_CANCELLED)
                Util_Log(L"kept %s: removal cancelled", p->dataDir);
        } else {
            RemoveNeverOpened(p);
        }
    }
    if ((status = Util_RegDeleteTree(HKEY_CURRENT_USER, REG_UNINSTALL)) != ERROR_SUCCESS)
        Util_Log(L"could not remove the Apps & features entry (error %ld)", status);
    if ((status = Util_RegDeleteTree(HKEY_CURRENT_USER, REG_ROOT)) != ERROR_SUCCESS)
        Util_Log(L"could not remove the settings key %s (error %ld)", REG_ROOT, status);
    /* After REG_ROOT: a download that ends meanwhile sees it gone and deletes
     * itself (update.c). */
    Update_RemoveDownload();
    return TRUE;
}
