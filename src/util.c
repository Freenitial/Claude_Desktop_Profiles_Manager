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

BOOL Util_StateDir(WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    return Util_LocalAppData(base, ARRAYSIZE(base)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_NAME, base));
}

BOOL Util_FileExists(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL Util_DirExists(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The nearest existing directory, including the path itself, for watching
 * the creation of a missing child without creating any directories. */
BOOL Util_ExistingDir(const WCHAR *path, WCHAR *out, size_t cch)
{
    if (!cch) return FALSE;
    if (path != out && FAILED(StringCchCopyW(out, cch, path))) return FALSE;
    while (!Util_DirExists(out)) {
        WCHAR *slash = wcsrchr(out, L'\\');
        if (!slash || slash == out || (slash == out + 2 && out[1] == L':' && !slash[1])) {
            out[0] = 0;
            return FALSE;
        }
        if (slash == out + 2 && out[1] == L':') slash[1] = 0;
        else *slash = 0;
    }
    return TRUE;
}

BOOL Util_EnsureDir(const WCHAR *path)
{
    int rc = SHCreateDirectoryExW(NULL, path, NULL);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

/* --------------------------------------------------------------- registry */

BOOL Util_RegGetString(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, DWORD cch)
{
    DWORD bytes = cch * sizeof(WCHAR);
    if (cch == 0) return FALSE;
    out[0] = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, NULL, out, &bytes) != ERROR_SUCCESS) {
        out[0] = 0;
        return FALSE;
    }
    return TRUE;
}

BOOL Util_RegSetString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data)
{
    HKEY k;
    LSTATUS rc;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return FALSE;
    rc = RegSetValueExW(k, value, 0, REG_SZ, (const BYTE *)data, (DWORD)((wcslen(data) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

BOOL Util_RegGetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out)
{
    DWORD bytes = sizeof *out;
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, NULL, out, &bytes) == ERROR_SUCCESS;
}

BOOL Util_RegSetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data)
{
    HKEY k;
    LSTATUS rc;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return FALSE;
    rc = RegSetValueExW(k, value, 0, REG_DWORD, (const BYTE *)&data, sizeof data);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

BOOL Util_RegKeyExists(HKEY root, const WCHAR *key)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
    RegCloseKey(k);
    return TRUE;
}

BOOL Util_RegValueExists(HKEY root, const WCHAR *key, const WCHAR *value)
{
    return RegGetValueW(root, key, value, RRF_RT_ANY, NULL, NULL, NULL) == ERROR_SUCCESS;
}

void Util_RegDeleteValue(HKEY root, const WCHAR *key, const WCHAR *value)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, value);
    RegCloseKey(k);
}

void Util_RegDeleteTree(HKEY root, const WCHAR *key)
{
    RegDeleteTreeW(root, key);
    RegDeleteKeyW(root, key);
}

/* ------------------------------------------------------------------- misc */

ULONGLONG Util_LocalNowTicks(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Core_SystemTimeTicks(&st);
}

/* Leaves the caller's last-error value as it was, so a failure can be logged
 * before it is reported. */
void Util_Log(const WCHAR *fmt, ...)
{
    WCHAR msg[1024], line[1200], dir[MAX_PATH], path[MAX_PATH], old[MAX_PATH];
    char utf8[4096];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    SYSTEMTIME st;
    va_list ap;
    HANDLE h;
    DWORD saved = GetLastError();
    int n;

    va_start(ap, fmt);
    StringCchVPrintfW(msg, ARRAYSIZE(msg), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    StringCchPrintfW(line, ARRAYSIZE(line), L"%04u-%02u-%02u %02u:%02u:%02u [%lu] %s\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     GetCurrentProcessId(), msg);
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, NULL, NULL);
    if (n <= 1 || !Util_StateDir(dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\claude-desktop-profiles-manager.log", dir))) {
        SetLastError(saved);
        return;
    }
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024 &&
        SUCCEEDED(StringCchPrintfW(old, ARRAYSIZE(old), L"%s.1", path)))
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, utf8, (DWORD)(n - 1), &written, NULL);
        CloseHandle(h);
    }
    SetLastError(saved);
}

/* Starts `exe args` detached from this process. */
BOOL Util_Spawn(const WCHAR *exe, const WCHAR *args)
{
    WCHAR cmd[MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (FAILED(StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\"%s%s", exe, args[0] ? L" " : L"", args))) return FALSE;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    /* Leave the caller's job object when allowed, so the new process outlives
     * the installer, shortcut or terminal that started it. */
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) &&
        !CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* A file's content, zero-terminated (free it with HeapFree): all of it when
 * it holds at most `maxBytes`, else NULL, or with `tail` its last `maxBytes`
 * (the end of a log). Other processes may keep writing it. */
char *Util_ReadFile(const WCHAR *path, DWORD maxBytes, BOOL tail, DWORD *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size, from;
    DWORD want, got = 0, n;
    char *buf = NULL;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && (tail || size.QuadPart <= (LONGLONG)maxBytes)) {
        want = size.QuadPart > (LONGLONG)maxBytes ? maxBytes : (DWORD)size.QuadPart;
        from.QuadPart = size.QuadPart - want;
        if ((buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)want + 1)) != NULL && SetFilePointerEx(h, from, NULL, FILE_BEGIN)) {
            while (got < want && ReadFile(h, buf + got, want - got, &n, NULL) && n > 0) got += n;
            buf[got] = 0;
            *len = got;
        } else if (buf) {
            HeapFree(GetProcessHeap(), 0, buf);
            buf = NULL;
        }
    }
    CloseHandle(h);
    return buf;
}

/* Files and folders to the Recycle Bin, in one operation (Windows asks
 * before deleting what the bin cannot hold). One already gone counts as
 * done. A junction or directory symlink is removed as a link: what it leads
 * to may be anything, so it is never followed. */
RemoveResult Util_Recycle(HWND owner, const WCHAR *const *paths, int count)
{
    SHFILEOPSTRUCTW op;
    WCHAR *from;
    size_t used = 0, cap = 1;
    BOOL anyLeft = FALSE;
    int i, rc;
    for (i = 0; i < count; i++) cap += wcslen(paths[i]) + 1;
    if ((from = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(WCHAR))) == NULL) return REMOVE_FAILED;
    for (i = 0; i < count; i++) {
        DWORD attr = GetFileAttributesW(paths[i]), err = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        if (attr == INVALID_FILE_ATTRIBUTES) {
            /* Gone only when Windows says so: an unreachable share is not. */
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) continue;
            Util_Log(L"cannot reach %s (error %lu)", paths[i], err);
            HeapFree(GetProcessHeap(), 0, from);
            return REMOVE_FAILED;
        }
        if ((attr & FILE_ATTRIBUTE_DIRECTORY) && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (!RemoveDirectoryW(paths[i])) {
                Util_Log(L"could not remove the link %s (error %lu)", paths[i], GetLastError());
                HeapFree(GetProcessHeap(), 0, from);
                return REMOVE_FAILED;
            }
            Util_Log(L"removed the link %s (its target is left in place)", paths[i]);
            continue;
        }
        StringCchCopyW(from + used, cap - used, paths[i]);
        used += wcslen(paths[i]) + 1;
        anyLeft = TRUE;
    }
    if (!anyLeft) {
        HeapFree(GetProcessHeap(), 0, from);
        return REMOVE_DONE;
    }
    ZeroMemory(&op, sizeof op);
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = from;   /* double zero-terminated */
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | (owner ? 0 : FOF_SILENT);
    rc = SHFileOperationW(&op);
    HeapFree(GetProcessHeap(), 0, from);
    if (rc != 0) Util_Log(L"recycling %d item(s): code %d", count, rc);
    if (op.fAnyOperationsAborted || rc == ERROR_CANCELLED) return REMOVE_CANCELLED;
    for (i = 0; i < count; i++)
        if (GetFileAttributesW(paths[i]) != INVALID_FILE_ATTRIBUTES) return REMOVE_FAILED;
    return REMOVE_DONE;
}

void Util_OpenUrl(const WCHAR *url)
{
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}
