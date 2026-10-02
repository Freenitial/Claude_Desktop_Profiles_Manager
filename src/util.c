#include "app.h"
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>

HINSTANCE g_hInst;

/* ------------------------------------------------------------------ paths */

BOOL Util_KnownFolder(const GUID *id, WCHAR *out, size_t cch)
{
    PWSTR p = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, NULL, &p)) && p)
        ok = SUCCEEDED(StringCchCopyW(out, cch, p));
    CoTaskMemFree(p);
    return ok;
}

BOOL Util_AppData(WCHAR *out, size_t cch)      { return Util_KnownFolder(&FOLDERID_RoamingAppData, out, cch); }
BOOL Util_LocalAppData(WCHAR *out, size_t cch) { return Util_KnownFolder(&FOLDERID_LocalAppData, out, cch); }

BOOL Util_SelfExe(WCHAR *out, size_t cch)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cch);
    return n > 0 && n < cch;
}

BOOL Util_InstallDir(WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    return Util_KnownFolder(&FOLDERID_UserProgramFiles, base, ARRAYSIZE(base)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME, base));
}

BOOL Util_InstallExe(WCHAR *out, size_t cch)
{
    WCHAR dir[MAX_PATH];
    return Util_InstallDir(dir, ARRAYSIZE(dir)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_EXE, dir));
}

static WCHAR g_stateDirOverride[MAX_PATH];

/* Tests keep the log and the queued session changes in a private folder:
 * called before anything is logged. */
void Util_SetStateDir(const WCHAR *dir)
{
    StringCchCopyW(g_stateDirOverride, ARRAYSIZE(g_stateDirOverride), dir ? dir : L"");
}

BOOL Util_StateDir(WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    if (g_stateDirOverride[0]) return SUCCEEDED(StringCchCopyW(out, cch, g_stateDirOverride));
    return Util_LocalAppData(base, ARRAYSIZE(base)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME, base));
}

BOOL Util_FileExists(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL Util_DirExists(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* Missing only when Windows says so: an unreachable share is not. */
PathState Util_QueryPath(const WCHAR *path, DWORD *attributes)
{
    DWORD found = GetFileAttributesW(path), error;
    if (attributes) *attributes = found;
    if (found != INVALID_FILE_ATTRIBUTES) return PATH_PRESENT;
    error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PATH_MISSING : PATH_UNREACHABLE;
}

/* The nearest existing directory, including the path itself, for watching
 * the creation of a missing child without creating any directories. A drive
 * or share root that is not there gives none. */
BOOL Util_ExistingDir(const WCHAR *path, WCHAR *out, size_t cch)
{
    size_t rootLength;
    if (!cch) return FALSE;
    if (path != out && FAILED(StringCchCopyW(out, cch, path))) return FALSE;
    if (out[0] == L'\\' && out[1] == L'\\') {
        const WCHAR *share = wcschr(out + 2, L'\\'), *end = share ? wcschr(share + 1, L'\\') : NULL;
        rootLength = share ? (end ? (size_t)(end - out) : wcslen(out)) : wcslen(out);
    } else {
        rootLength = out[0] && out[1] == L':' ? 2 : 0;
    }
    while (!Util_DirExists(out)) {
        WCHAR *slash = wcsrchr(out, L'\\');
        if (!slash || (size_t)(slash - out) < rootLength || ((size_t)(slash - out) == rootLength && !slash[1])) {
            out[0] = 0;
            return FALSE;
        }
        if ((size_t)(slash - out) == rootLength && rootLength == 2) slash[1] = 0;   /* "C:\" */
        else *slash = 0;
    }
    return TRUE;
}

BOOL Util_EnsureDir(const WCHAR *path)
{
    int rc = SHCreateDirectoryExW(NULL, path, NULL);
    if (rc == ERROR_SUCCESS || ((rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS) && Util_DirExists(path)))
        return TRUE;
    SetLastError((DWORD)rc);   /* it returns its error rather than setting it */
    return FALSE;
}

/* --------------------------------------------------------------- registry */

BOOL Util_RegGetString(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, size_t cch)
{
    DWORD bytes = (DWORD)min(cch * sizeof(WCHAR), MAXDWORD - 1);
    if (cch == 0) return FALSE;
    out[0] = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, NULL, out, &bytes) != ERROR_SUCCESS) {
        out[0] = 0;
        return FALSE;
    }
    return TRUE;
}

/* The registry calls return their error rather than setting it. */
static BOOL RegResult(LSTATUS status)
{
    if (status == ERROR_SUCCESS) return TRUE;
    SetLastError((DWORD)status);
    return FALSE;
}

BOOL Util_RegSetString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data)
{
    HKEY openedKey;
    LSTATUS status = RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &openedKey, NULL);
    if (status != ERROR_SUCCESS) return RegResult(status);
    status = RegSetValueExW(openedKey, value, 0, REG_SZ, (const BYTE *)data, (DWORD)((wcslen(data) + 1) * sizeof(WCHAR)));
    RegCloseKey(openedKey);
    return RegResult(status);
}

/* Writes the value only when it holds something else: an unchanged value is
 * not written again. `*changed` (when not NULL) becomes TRUE when it is
 * written and is left alone otherwise, so one flag can cover several values. */
BOOL Util_RegSetStringIfDifferent(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data, BOOL *changed)
{
    WCHAR stored[MAX_PATH + 64];
    if (Util_RegGetString(root, key, value, stored, ARRAYSIZE(stored)) && wcscmp(stored, data) == 0) return TRUE;
    if (changed) *changed = TRUE;
    return Util_RegSetString(root, key, value, data);
}

BOOL Util_RegGetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out)
{
    DWORD bytes = sizeof *out;
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, NULL, out, &bytes) == ERROR_SUCCESS;
}

BOOL Util_RegSetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data)
{
    HKEY openedKey;
    LSTATUS status = RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &openedKey, NULL);
    if (status != ERROR_SUCCESS) return RegResult(status);
    status = RegSetValueExW(openedKey, value, 0, REG_DWORD, (const BYTE *)&data, sizeof data);
    RegCloseKey(openedKey);
    return RegResult(status);
}

BOOL Util_RegKeyExists(HKEY root, const WCHAR *key)
{
    HKEY openedKey;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &openedKey) != ERROR_SUCCESS) return FALSE;
    RegCloseKey(openedKey);
    return TRUE;
}

BOOL Util_RegValueExists(HKEY root, const WCHAR *key, const WCHAR *value)
{
    return RegGetValueW(root, key, value, RRF_RT_ANY, NULL, NULL, NULL) == ERROR_SUCCESS;
}

/* ERROR_SUCCESS when the value is gone, also when it or its key never was;
 * any other status is the last error too. */
LSTATUS Util_RegDeleteValue(HKEY root, const WCHAR *key, const WCHAR *value)
{
    HKEY openedKey;
    LSTATUS status = RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &openedKey);
    if (status == ERROR_SUCCESS) {
        status = RegDeleteValueW(openedKey, value);
        RegCloseKey(openedKey);
    }
    if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    RegResult(status);
    return status;
}

/* The key with everything under it; ERROR_SUCCESS when it is gone. A key
 * whose values cannot be removed one by one (Windows denies changing
 * UserChoice) can still be deleted whole. */
LSTATUS Util_RegDeleteTree(HKEY root, const WCHAR *key)
{
    LSTATUS status = RegDeleteTreeW(root, key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) status = RegDeleteKeyW(root, key);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

/* ------------------------------------------------------------------- misc */

ULONGLONG Util_LocalNowTicks(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Core_SystemTimeTicks(&st);
}

#define LOG_ROTATE_BYTES (512 * 1024)
#define LOG_LOCK_WAIT_MS 1000   /* another process holds the lock only to rotate or open the log */
#define LOG_MESSAGE_CCH  1024
#define LOG_LINE_CCH     (LOG_MESSAGE_CCH + 64)   /* the message after its time and process id */

static WCHAR g_logDir[MAX_PATH], g_logPath[MAX_PATH], g_logOldPath[MAX_PATH];

static BOOL CALLBACK FindLogPaths(PINIT_ONCE once, void *parameter, void **context)
{
    (void)once;
    (void)parameter;
    (void)context;
    if (!Util_StateDir(g_logDir, ARRAYSIZE(g_logDir)) ||
        FAILED(StringCchPrintfW(g_logPath, ARRAYSIZE(g_logPath), L"%s\\claude-desktop-profiles-manager.log", g_logDir)) ||
        FAILED(StringCchPrintfW(g_logOldPath, ARRAYSIZE(g_logOldPath), L"%s.1", g_logPath)))
        g_logPath[0] = 0;
    return TRUE;
}

/* Every process of the program appends to one log; past LOG_ROTATE_BYTES it
 * becomes the ".1" copy, one process at a time so no rotation is lost. */
static HANDLE OpenLog(void)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    HANDLE mutex = CreateMutexW(NULL, FALSE, LOG_MUTEX), log;
    DWORD wait = mutex ? WaitForSingleObject(mutex, LOG_LOCK_WAIT_MS) : WAIT_FAILED;
    if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &attributes) &&
        (attributes.nFileSizeHigh || attributes.nFileSizeLow > LOG_ROTATE_BYTES) &&
        (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED))
        MoveFileExW(g_logPath, g_logOldPath, MOVEFILE_REPLACE_EXISTING);
    log = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND && Util_EnsureDir(g_logDir))
        log = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    return log;
}

/* Leaves the caller's last-error value as it was, so a failure can be logged
 * before it is reported. */
void Util_Log(const WCHAR *fmt, ...)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    WCHAR message[LOG_MESSAGE_CCH], line[LOG_LINE_CCH];
    char utf8[LOG_LINE_CCH * 3];   /* at most 3 UTF-8 bytes per UTF-16 unit */
    SYSTEMTIME st;
    va_list ap;
    HANDLE log;
    DWORD saved = GetLastError();
    int bytes;

    va_start(ap, fmt);
    StringCchVPrintfW(message, ARRAYSIZE(message), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    StringCchPrintfW(line, ARRAYSIZE(line), L"%04u-%02u-%02u %02u:%02u:%02u [%lu] %s\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     GetCurrentProcessId(), message);
    bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, NULL, NULL);
    InitOnceExecuteOnce(&once, FindLogPaths, NULL, NULL);
    if (bytes > 1 && g_logPath[0] && (log = OpenLog()) != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(log, utf8, (DWORD)(bytes - 1), &written, NULL);
        CloseHandle(log);
    }
    SetLastError(saved);
}

/* Starts `exe args` detached from this process; `pid` (when not NULL) gets
 * its id. On failure the last error says why. */
BOOL Util_Spawn(const WCHAR *exe, const WCHAR *args, DWORD *pid)
{
    size_t cch = wcslen(exe) + wcslen(args) + 4;
    WCHAR *command = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cch * sizeof(WCHAR));
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL started;
    DWORD error;
    if (pid) *pid = 0;
    if (!command) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    StringCchPrintfW(command, cch, L"\"%s\"%s%s", exe, args[0] ? L" " : L"", args);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    /* Leave the caller's job object when allowed, so the new process outlives
     * the installer, shortcut or terminal that started it. */
    started = CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) ||
              CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    error = GetLastError();
    HeapFree(GetProcessHeap(), 0, command);
    if (!started) {
        SetLastError(error);
        return FALSE;
    }
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* `path` as the file functions take it past MAX_PATH: \\?\C:\... or
 * \\?\UNC\server\share\.... Windows then leaves the path as it is, so it
 * must be a full path with backslashes only; any other is copied as it is. */
BOOL Util_ExtendedPath(const WCHAR *path, WCHAR *out, size_t cch)
{
    if (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.')
        return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\?\\UNC\\%s", path + 2));
    if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\')
        return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\?\\%s", path));
    return SUCCEEDED(StringCchCopyW(out, cch, path));
}

/* FindFirstFileExW on `pattern` inside `dir`, at any path length. */
HANDLE Util_FindFiles(const WCHAR *dir, const WCHAR *pattern, WIN32_FIND_DATAW *found, BOOL foldersOnly)
{
    WCHAR path[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, pattern)) ||
        !Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileExW(extended, FindExInfoBasic, found, foldersOnly ? FindExSearchLimitToDirectories : FindExSearchNameMatch,
                            NULL, FIND_FIRST_EX_LARGE_FETCH);
}

/* A file's content, zero-terminated (free it with HeapFree): all of it when
 * it holds at most `maxBytes`, or with `tail` its last `maxBytes` (the end of
 * a log). NULL when it is missing, empty, too big or cannot be read whole.
 * Other processes may keep writing it. Any path length. */
char *Util_ReadFile(const WCHAR *path, DWORD maxBytes, BOOL tail, DWORD *len)
{
    WCHAR extended[LONG_PATH_CCH];
    HANDLE file;
    LARGE_INTEGER size, from;
    DWORD want, got = 0, n;
    char *buffer = NULL;
    *len = 0;
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return NULL;
    file = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && (tail || size.QuadPart <= (LONGLONG)maxBytes)) {
        want = size.QuadPart > (LONGLONG)maxBytes ? maxBytes : (DWORD)size.QuadPart;
        from.QuadPart = size.QuadPart - want;
        if ((buffer = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)want + 1)) != NULL && SetFilePointerEx(file, from, NULL, FILE_BEGIN)) {
            char extra;
            BOOL grew;
            while (got < want && ReadFile(file, buffer + got, want - got, &n, NULL) && n > 0) got += n;
            buffer[got] = 0;
            *len = got;
            /* A file read whole must end there: one that grew meanwhile was
             * read only in part. */
            grew = !tail && got == want && ReadFile(file, &extra, 1, &n, NULL) && n > 0;
            if (grew) *len = 0;
        }
        /* A log that shrank meanwhile still has its end; anything else read
         * short would be taken for the whole file. */
        if (buffer && (!*len || (!tail && got < want))) {
            HeapFree(GetProcessHeap(), 0, buffer);
            buffer = NULL;
            *len = 0;
        }
    }
    CloseHandle(file);
    return buffer;
}

/* A junction or directory symlink (other reparse points, cloud placeholders
 * say, are ordinary folders). */
BOOL Util_IsDirectoryLink(const WCHAR *path)
{
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileExW(path, FindExInfoBasic, &entry, FindExSearchNameMatch, NULL, 0);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(find);
    return (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (entry.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || entry.dwReserved0 == IO_REPARSE_TAG_SYMLINK);
}

/* Windows' Recycle Bin takes no path of MAX_PATH characters or more, nor
 * the \\?\ form that reaches such paths. */
BOOL Util_FitsRecycleBin(const WCHAR *path)
{
    return wcslen(path) < MAX_PATH && wcsncmp(path, L"\\\\?\\", 4) != 0;
}

/* Files and folders to the Recycle Bin, in one operation (Windows asks
 * before deleting what the bin cannot hold). One already gone counts as
 * done. A junction or directory symlink is removed as a link: what it leads
 * to may be anything, so it is never followed. Every path is checked before
 * anything is removed, and one the bin cannot take (Util_FitsRecycleBin)
 * fails it all. The links go once everything else is in the bin, so a
 * removal cancelled or failed there leaves them in place. */
RemoveResult Util_Recycle(HWND owner, const WCHAR *const *paths, int count)
{
    SHFILEOPSTRUCTW operation;
    WCHAR *from;
    BOOL *isLink, anyRecycled = FALSE;
    size_t used = 0, cap = 1;
    RemoveResult result = REMOVE_FAILED;
    int i, code;
    for (i = 0; i < count; i++) cap += wcslen(paths[i]) + 1;
    from = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(WCHAR));
    isLink = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)max(count, 1) * sizeof *isLink);
    if (!from || !isLink) goto done;
    for (i = 0; i < count; i++) {
        DWORD attributes;
        if (!Util_FitsRecycleBin(paths[i])) {
            Util_Log(L"%s not removed: its path is too long for the Recycle Bin", paths[i]);
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            goto done;
        }
        switch (Util_QueryPath(paths[i], &attributes)) {
        case PATH_UNREACHABLE:
            Util_Log(L"cannot reach %s (error %lu)", paths[i], GetLastError());
            goto done;
        case PATH_MISSING:
            continue;
        case PATH_PRESENT:
            break;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) && Util_IsDirectoryLink(paths[i])) {
            isLink[i] = TRUE;
            continue;
        }
        StringCchCopyW(from + used, cap - used, paths[i]);
        used += wcslen(paths[i]) + 1;
        anyRecycled = TRUE;
    }
    result = REMOVE_DONE;
    if (anyRecycled) {
        ZeroMemory(&operation, sizeof operation);
        operation.hwnd = owner;
        operation.wFunc = FO_DELETE;
        operation.pFrom = from;   /* double zero-terminated */
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | (owner ? 0 : FOF_SILENT);
        code = SHFileOperationW(&operation);
        if (code != 0) Util_Log(L"recycling %d item(s): code %d", count, code);
        if (operation.fAnyOperationsAborted || code == ERROR_CANCELLED) {
            result = REMOVE_CANCELLED;
            goto done;
        }
        if (code != 0) {
            result = REMOVE_FAILED;
            goto done;
        }
        for (i = 0; i < count; i++) {
            if (isLink[i]) continue;
            switch (Util_QueryPath(paths[i], NULL)) {
            case PATH_MISSING:
                continue;
            case PATH_PRESENT:
                Util_Log(L"%s is still there after recycling", paths[i]);
                break;
            case PATH_UNREACHABLE:
                Util_Log(L"could not verify removal of %s (error %lu)", paths[i], GetLastError());
                break;
            }
            result = REMOVE_FAILED;
            goto done;
        }
    }
    for (i = 0; i < count; i++) {
        DWORD error;
        if (!isLink[i]) continue;
        if (RemoveDirectoryW(paths[i])) {
            Util_Log(L"removed the link %s (its target is left in place)", paths[i]);
            continue;
        }
        error = GetLastError();
        if (Util_QueryPath(paths[i], NULL) == PATH_MISSING) continue;   /* it went with a folder recycled above */
        Util_Log(L"could not remove the link %s (error %lu)", paths[i], error);
        result = REMOVE_FAILED;
        goto done;
    }
done:
    if (from) HeapFree(GetProcessHeap(), 0, from);
    if (isLink) HeapFree(GetProcessHeap(), 0, isLink);
    return result;
}

BOOL Util_OpenUrl(const WCHAR *url)
{
    INT_PTR result = (INT_PTR)ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if (result > 32) return TRUE;
    Util_Log(L"could not open %s (code %ld)", url, (long)result);
    return FALSE;
}
