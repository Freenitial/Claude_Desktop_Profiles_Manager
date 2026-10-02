/*
 * Platform tests: the code that talks to Windows, its failure paths above
 * all. The release check, the download, its verification and start
 * (update.c); the atomic install, its registration, the uninstall and its
 * cleanup after exit (install.c); stopping watchers, waiting for a profile's
 * Claude, reopening it after a Claude update, a start handed to a running
 * watcher and a watcher's end (taskbar.c); late downloads and the sign-in
 * setting in the manager (gui.c); where a link goes, its delivery and
 * watchers (router.c); Recycle Bin results (util.c) and profile data removal
 * (profiles.c).
 *
 * Each source is included with fixtures in place of its system calls and
 * with its exported functions renamed Tested<Name>, so that it runs on
 * private files, unnamed events and controlled callbacks: no real registry
 * value, shortcut, watcher, Claude or installer is touched.
 */
#include "../src/app.h"
#include "../src/resource.h"
#include <shellapi.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_WAIT_MS    5000    /* the longest wait for a fixture thread or event that should answer at once */
#define CHILD_EXIT_WAIT_MS 60000   /* a child process: a scanner may hold the first run of a new exe for seconds */

static int g_checks, g_failures;
static WCHAR g_root[MAX_PATH], g_stateDir[MAX_PATH], g_downloadFile[MAX_PATH];

static void Check(const char *what, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* A check made for each of several cases: a failure names the case. */
static void CheckFor(const char *what, const char *which, BOOL ok)
{
    char text[320];
    StringCchPrintfA(text, ARRAYSIZE(text), "%s (%s)", what, which);
    Check(text, ok);
}

/* A step that sets up a fixture: when it fails, the checks that need it are
 * skipped and the failure says which step and why. */
static BOOL Prepared(const char *step, BOOL done)
{
    DWORD error = GetLastError();
    g_checks++;
    if (!done) {
        g_failures++;
        printf("  FAIL  fixture setup: %s (error %lu)\n", step, error);
    }
    return done;
}

static void FixtureLog(const WCHAR *format, ...)
{
    (void)format;
}

static BOOL JoinPath(const WCHAR *dir, const WCHAR *name, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", dir, name));
}

static BOOL WriteFixtureFile(const WCHAR *path, const char *content)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD length = (DWORD)strlen(content), written = 0;
    BOOL ok;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, content, length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

static BOOL FileHolds(const WCHAR *path, const char *content)
{
    DWORD length = 0;
    char *data = Util_ReadFile(path, 1024, FALSE, &length);
    BOOL same = data && length == strlen(content) && memcmp(data, content, length) == 0;
    if (data) HeapFree(GetProcessHeap(), 0, data);
    return same;
}

/* Whether another handle keeps `path` from being opened for `access`. */
static BOOL OpenRefused(const WCHAR *path, DWORD access)
{
    HANDLE file = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(file);
    return FALSE;
}

/* Whether nothing else holds `path` open. */
static BOOL OpensAlone(const WCHAR *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(file);
    return TRUE;
}

/* A file, or a folder with what it holds, under the private root only; a link
 * is never followed. */
static BOOL RemovePrivateTree(const WCHAR *path)
{
    WCHAR pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    DWORD attributes;
    BOOL removed = TRUE;
    if (!g_root[0] || !Core_PathUnder(path, g_root)) return FALSE;
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path);
    if (!JoinPath(path, L"*", pattern, ARRAYSIZE(pattern))) return FALSE;
    search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if (!JoinPath(path, found.cFileName, child, ARRAYSIZE(child)) || !RemovePrivateTree(child)) removed = FALSE;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return removed && RemoveDirectoryW(path);
}

/* --------------------------------------------------------------- update.c */

#define CACHED_RELEASE_TAG L"v99.0.0"

/* What an exe's RT_VERSION resource starts with: the VS_VERSIONINFO header,
 * then its VS_FIXEDFILEINFO on a 32-bit boundary. */
typedef struct VersionResourceFixture {
    WORD             length;
    WORD             valueLength;
    WORD             type;
    WCHAR            key[16];
    WORD             padding;
    VS_FIXEDFILEINFO fixed;
} VersionResourceFixture;

static VersionResourceFixture g_versionResource;
static BOOL g_versionResourcePresent, g_versionReadAsData;
static int g_versionImageLoads, g_versionImageFrees;

static int g_updateMessages, g_jobThreadRequests, g_spawns, g_signatureChecks;
static WPARAM g_postedUpdateResult;
static LPARAM g_postedUpdateError;
static BOOL g_failJobAllocation, g_checkTimeCached, g_spawnFails, g_spawnedToInstall;
static BOOL g_writeRefusedAtSpawn, g_deleteRefusedAtSpawn, g_signatureCheckedOnOpenFile;
static DWORD g_cachedCheckMinute;
static LONG g_signatureStatus;
static const WCHAR *g_signerName = APP_SIGNER;
static const WCHAR *g_cachedReleaseTag = CACHED_RELEASE_TAG;
static CRYPT_PROVIDER_DATA g_provider;
static CRYPT_PROVIDER_SGNR g_signer;
static CRYPT_PROVIDER_CERT g_certificate;

#define CACHE_ROOT_KEY   ((HKEY)(INT_PTR)0x41)
#define CACHE_UPDATE_KEY ((HKEY)(INT_PTR)0x42)
static BOOL g_cacheRootPresent, g_cacheRootDeletedAfterOpen, g_cacheSubkeyIsUpdate, g_cacheTagWritten, g_cacheTimeWritten;
static int g_cacheWrites, g_cacheWritesElsewhere, g_cacheRootCloses, g_cacheUpdateCloses;
static WCHAR g_writtenReleaseTag[32];

/* The fixture server: no request leaves the machine. */
#define HTTP_SESSION     ((HINTERNET)(INT_PTR)0x51)
#define HTTP_CONNECTION  ((HINTERNET)(INT_PTR)0x52)
#define HTTP_REQUEST     ((HINTERNET)(INT_PTR)0x53)
#define HTTP_CHUNK_BYTES (1024 * 1024)
static DWORD g_httpStatus, g_httpBodyBytes, g_httpBodyServed;
static const char *g_httpBody;   /* NULL: a generated body of g_httpBodyBytes */
static BOOL g_httpBodyIsProgram, g_httpSecure;
static int g_httpOpenHandles;
static WCHAR g_httpHost[64], g_httpPath[256];
/* The download's clock, which only the fixture server moves: each read takes g_httpMillisecondsPerRead. */
static DWORD g_httpChunkBytes = HTTP_CHUNK_BYTES;
static ULONGLONG g_updateClockMs, g_httpMillisecondsPerRead;

static ULONGLONG WINAPI FixtureUpdateClock(void)
{
    return g_updateClockMs;
}

static void DeclareVersion(WORD major, WORD minor, WORD patch, WORD build)
{
    ZeroMemory(&g_versionResource, sizeof g_versionResource);
    g_versionResource.length = (WORD)sizeof g_versionResource;
    g_versionResource.valueLength = (WORD)sizeof g_versionResource.fixed;
    memcpy(g_versionResource.key, L"VS_VERSION_INFO", sizeof g_versionResource.key);
    g_versionResource.fixed.dwSignature = VS_FFI_SIGNATURE;
    g_versionResource.fixed.dwStrucVersion = VS_FFI_STRUCVERSION;
    g_versionResource.fixed.dwFileVersionMS = (DWORD)MAKELONG(minor, major);
    g_versionResource.fixed.dwFileVersionLS = (DWORD)MAKELONG(build, patch);
    g_versionResourcePresent = TRUE;
}

static HMODULE WINAPI FixtureLoadVersionImage(LPCWSTR file, HANDLE reserved, DWORD flags)
{
    (void)reserved;
    g_versionImageLoads++;
    g_versionReadAsData = Core_PathEquals(file, g_downloadFile) &&
                          (flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) != 0;
    return (HMODULE)&g_versionResource;
}

static HRSRC WINAPI FixtureFindVersion(HMODULE image, LPCWSTR name, LPCWSTR type)
{
    if (image != (HMODULE)&g_versionResource || !g_versionResourcePresent || name != MAKEINTRESOURCEW(VS_VERSION_INFO) ||
        type != RT_VERSION) {
        SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return NULL;
    }
    return (HRSRC)&g_versionResource;
}

static HGLOBAL WINAPI FixtureLoadVersion(HMODULE image, HRSRC found)
{
    (void)image;
    return (HGLOBAL)found;
}

static LPVOID WINAPI FixtureLockVersion(HGLOBAL loaded)
{
    return loaded;
}

static DWORD WINAPI FixtureVersionSize(HMODULE image, HRSRC found)
{
    (void)image; (void)found;
    return sizeof g_versionResource;
}

static BOOL WINAPI FixtureFreeVersionImage(HMODULE image)
{
    if (image == (HMODULE)&g_versionResource) g_versionImageFrees++;
    return TRUE;
}

static LPVOID WINAPI FixtureAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    if (g_failJobAllocation) return NULL;
    return HeapAlloc(heap, flags, bytes);
}

/* No release check or download reaches the network: no job thread starts. */
static HANDLE WINAPI FixtureNoJobThread(LPSECURITY_ATTRIBUTES security, SIZE_T stack, LPTHREAD_START_ROUTINE routine,
                                        LPVOID argument, DWORD flags, LPDWORD id)
{
    (void)security; (void)stack; (void)routine; (void)argument; (void)flags; (void)id;
    g_jobThreadRequests++;
    SetLastError(ERROR_MAX_THRDS_REACHED);
    return NULL;
}

static BOOL WINAPI FixtureUpdateNotice(HWND window, UINT message, WPARAM result, LPARAM error)
{
    (void)window; (void)message;
    g_updateMessages++;
    g_postedUpdateResult = result;
    g_postedUpdateError = error;
    return TRUE;
}

/* What the fixture server answers next: `text`, or a generated body of
 * `generatedBytes` that starts as a program does when `program`. */
static void ServeAnswer(DWORD status, const char *text, DWORD generatedBytes, BOOL program)
{
    g_httpStatus = status;
    g_httpBody = text;
    g_httpBodyBytes = text ? (DWORD)strlen(text) : generatedBytes;
    g_httpBodyIsProgram = program;
    g_httpBodyServed = 0;
    g_httpHost[0] = g_httpPath[0] = 0;
    g_httpSecure = FALSE;
    g_httpOpenHandles = 0;
}

static HINTERNET WINAPI FixtureHttpOpen(LPCWSTR agent, DWORD accessType, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
    (void)agent; (void)accessType; (void)proxy; (void)bypass; (void)flags;
    g_httpOpenHandles++;
    return HTTP_SESSION;
}

static BOOL WINAPI FixtureHttpTimeouts(HINTERNET handle, int resolve, int connect, int send, int receive)
{
    (void)handle; (void)resolve; (void)connect; (void)send; (void)receive;
    return TRUE;
}

static HINTERNET WINAPI FixtureHttpConnect(HINTERNET session, LPCWSTR host, INTERNET_PORT port, DWORD reserved)
{
    (void)session; (void)port; (void)reserved;
    StringCchCopyW(g_httpHost, ARRAYSIZE(g_httpHost), host);
    g_httpOpenHandles++;
    return HTTP_CONNECTION;
}

static HINTERNET WINAPI FixtureHttpRequest(HINTERNET connection, LPCWSTR verb, LPCWSTR path, LPCWSTR version, LPCWSTR referrer,
                                           LPCWSTR *acceptTypes, DWORD flags)
{
    (void)connection; (void)verb; (void)version; (void)referrer; (void)acceptTypes;
    StringCchCopyW(g_httpPath, ARRAYSIZE(g_httpPath), path);
    g_httpSecure = (flags & WINHTTP_FLAG_SECURE) != 0;
    g_httpOpenHandles++;
    return HTTP_REQUEST;
}

static DWORD g_httpSendError;   /* the server cannot be reached: WinHTTP's error */

static BOOL WINAPI FixtureHttpSend(HINTERNET request, LPCWSTR headers, DWORD headersLength, LPVOID optional, DWORD optionalLength,
                                   DWORD totalLength, DWORD_PTR context)
{
    (void)request; (void)headers; (void)headersLength; (void)optional; (void)optionalLength; (void)totalLength; (void)context;
    if (!g_httpSendError) return TRUE;
    SetLastError(g_httpSendError);
    return FALSE;
}

static BOOL WINAPI FixtureHttpReceive(HINTERNET request, LPVOID reserved)
{
    (void)request; (void)reserved;
    return TRUE;
}

static BOOL WINAPI FixtureHttpStatus(HINTERNET request, DWORD infoLevel, LPCWSTR name, LPVOID buffer, LPDWORD bytes, LPDWORD index)
{
    (void)request; (void)infoLevel; (void)name; (void)index;
    if (*bytes < sizeof g_httpStatus) return FALSE;
    memcpy(buffer, &g_httpStatus, sizeof g_httpStatus);
    return TRUE;
}

static BOOL WINAPI FixtureHttpAvailable(HINTERNET request, LPDWORD available)
{
    (void)request;
    *available = min(g_httpBodyBytes - g_httpBodyServed, g_httpChunkBytes);
    return TRUE;
}

static BOOL WINAPI FixtureHttpRead(HINTERNET request, LPVOID buffer, DWORD wanted, LPDWORD read)
{
    BYTE *out = (BYTE *)buffer;
    DWORD served = min(wanted, g_httpBodyBytes - g_httpBodyServed);
    (void)request;
    if (g_httpBody) {
        memcpy(out, g_httpBody + g_httpBodyServed, served);
    } else {
        memset(out, 'x', served);
        if (g_httpBodyServed == 0 && served >= 2) {
            out[0] = (BYTE)(g_httpBodyIsProgram ? 'M' : 'P');
            out[1] = (BYTE)(g_httpBodyIsProgram ? 'Z' : 'K');
        }
    }
    g_httpBodyServed += served;
    g_updateClockMs += g_httpMillisecondsPerRead;
    *read = served;
    return TRUE;
}

static BOOL WINAPI FixtureHttpClose(HINTERNET handle)
{
    (void)handle;
    g_httpOpenHandles--;
    return TRUE;
}

/* Where the release check keeps what it found: update.c's REG_UPDATE and its
 * values. A read or a write anywhere else finds or keeps nothing. */
#define RELEASE_CACHE_KEY        REG_ROOT L"\\Update"
#define RELEASE_TAG_VALUE        L"LatestRelease"
#define RELEASE_CHECK_TIME_VALUE L"ReleaseCheckMinute"
static int g_cacheReadsElsewhere;

static BOOL IsReleaseCacheValue(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *expected)
{
    if (root == HKEY_CURRENT_USER && wcscmp(key, RELEASE_CACHE_KEY) == 0 && wcscmp(value, expected) == 0) return TRUE;
    g_cacheReadsElsewhere++;
    return FALSE;
}

static BOOL FixtureCachedTag(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, size_t cch)
{
    if (cch) out[0] = 0;
    return IsReleaseCacheValue(root, key, value, RELEASE_TAG_VALUE) && g_cachedReleaseTag &&
           SUCCEEDED(StringCchCopyW(out, cch, g_cachedReleaseTag));
}

static BOOL FixtureCachedCheckTime(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out)
{
    if (!IsReleaseCacheValue(root, key, value, RELEASE_CHECK_TIME_VALUE) || !g_checkTimeCached) return FALSE;
    *out = g_cachedCheckMinute;
    return TRUE;
}

static LSTATUS WINAPI FixtureCacheOpen(HKEY root, LPCWSTR path, DWORD options, REGSAM access, PHKEY key)
{
    (void)options; (void)access;
    if (root != HKEY_CURRENT_USER || wcscmp(path, REG_ROOT) != 0) {
        g_cacheReadsElsewhere++;
        return ERROR_FILE_NOT_FOUND;
    }
    if (!g_cacheRootPresent) return ERROR_FILE_NOT_FOUND;
    if (g_cacheRootDeletedAfterOpen) g_cacheRootPresent = FALSE;
    *key = CACHE_ROOT_KEY;
    return ERROR_SUCCESS;
}

/* A deleted key takes no new subkey, as Windows answers. */
static LSTATUS WINAPI FixtureCacheCreate(HKEY parent, LPCWSTR subkey, DWORD reserved, LPWSTR className, DWORD options,
                                         REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY key, LPDWORD disposition)
{
    (void)reserved; (void)className; (void)options; (void)access; (void)security; (void)disposition;
    if (parent != CACHE_ROOT_KEY || !g_cacheRootPresent) return ERROR_KEY_DELETED;
    g_cacheSubkeyIsUpdate = wcscmp(subkey, L"Update") == 0;
    *key = CACHE_UPDATE_KEY;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI FixtureCacheWrite(HKEY key, LPCWSTR value, DWORD reserved, DWORD type, const BYTE *data, DWORD bytes)
{
    (void)reserved;
    if (key != CACHE_UPDATE_KEY) {
        g_cacheWritesElsewhere++;
        return ERROR_ACCESS_DENIED;
    }
    g_cacheWrites++;
    if (wcscmp(value, RELEASE_TAG_VALUE) == 0 && type == REG_SZ && bytes >= sizeof(WCHAR) && bytes <= sizeof g_writtenReleaseTag &&
        ((const WCHAR *)data)[bytes / sizeof(WCHAR) - 1] == 0) {
        memcpy(g_writtenReleaseTag, data, bytes);
        g_cacheTagWritten = TRUE;
    } else if (wcscmp(value, RELEASE_CHECK_TIME_VALUE) == 0 && type == REG_DWORD && bytes == sizeof(DWORD)) {
        g_cacheTimeWritten = TRUE;
    } else {
        g_cacheWritesElsewhere++;
    }
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI FixtureCacheClose(HKEY key)
{
    if (key == CACHE_ROOT_KEY) g_cacheRootCloses++;
    else if (key == CACHE_UPDATE_KEY) g_cacheUpdateCloses++;
    return ERROR_SUCCESS;
}

static DWORD WINAPI FixtureTemp(DWORD cch, LPWSTR out)
{
    if (FAILED(StringCchPrintfW(out, cch, L"%s\\", g_root))) return cch;
    return (DWORD)wcslen(out);
}

static int g_trustStatesOpen;
static BOOL g_trustAskedAsRequired;

/* Records how the verification is asked for; each verification opens a state
 * that must be closed. */
static LONG WINAPI FixtureTrust(HWND window, GUID *action, LPVOID data)
{
    static const GUID kGenericVerify = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA *trust = (WINTRUST_DATA *)data;
    (void)window;
    if (trust->dwStateAction == WTD_STATEACTION_CLOSE) {
        if (trust->hWVTStateData == (HANDLE)&g_provider) g_trustStatesOpen--;
        return ERROR_SUCCESS;
    }
    g_signatureChecks++;
    g_trustStatesOpen++;
    g_signatureCheckedOnOpenFile = trust->dwUnionChoice == WTD_CHOICE_FILE && trust->pFile && trust->pFile->hFile &&
                                   trust->pFile->hFile != INVALID_HANDLE_VALUE;
    g_trustAskedAsRequired = IsEqualGUID(action, &kGenericVerify) && trust->dwUIChoice == WTD_UI_NONE &&
                             trust->fdwRevocationChecks == WTD_REVOKE_WHOLECHAIN &&
                             (trust->dwProvFlags & WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT) &&
                             trust->dwStateAction == WTD_STATEACTION_VERIFY;
    trust->hWVTStateData = (HANDLE)&g_provider;
    return g_signatureStatus;
}

static CRYPT_PROVIDER_DATA *WINAPI FixtureProvider(HANDLE state)
{
    return state == (HANDLE)&g_provider ? &g_provider : NULL;
}

static CRYPT_PROVIDER_SGNR *WINAPI FixtureSigner(CRYPT_PROVIDER_DATA *provider, DWORD signer, BOOL counter, DWORD counterSigner)
{
    (void)provider; (void)signer; (void)counter; (void)counterSigner;
    return &g_signer;
}

static CRYPT_PROVIDER_CERT *WINAPI FixtureCertificate(CRYPT_PROVIDER_SGNR *signer, DWORD index)
{
    (void)signer; (void)index;
    return &g_certificate;
}

/* The signer's common name is g_signerName; any other name it is asked for
 * (its display name) is APP_SIGNER, so only a check of the common name
 * refuses another signer. */
static DWORD WINAPI FixtureCertificateName(PCCERT_CONTEXT certificate, DWORD type, DWORD flags, void *parameter,
                                           LPWSTR out, DWORD cch)
{
    BOOL commonName = type == CERT_NAME_ATTR_TYPE && parameter && strcmp((const char *)parameter, szOID_COMMON_NAME) == 0;
    (void)certificate;
    if (FAILED(StringCchCopyW(out, cch, (flags & CERT_NAME_ISSUER_FLAG) ? L"Fixture issuer" : commonName ? g_signerName : APP_SIGNER)))
        return 0;
    return (DWORD)wcslen(out) + 1;
}

/* An uninstall deletes REG_ROOT; a download that ends after it sees it gone. */
static BOOL g_uninstalledDuringDownload;

static BOOL FixtureStillInstalled(HKEY root, const WCHAR *key)
{
    if (root != HKEY_CURRENT_USER || wcscmp(key, REG_ROOT) != 0) {
        g_cacheReadsElsewhere++;
        return FALSE;
    }
    return !g_uninstalledDuringDownload;
}

/* Who may take the foreground: recorded, never granted, so that no other
 * process on the desktop can come to the front because of this test. */
static int g_foregroundGrants;
static DWORD g_foregroundGrantedTo;

static BOOL WINAPI FixtureAllowForeground(DWORD processId)
{
    g_foregroundGrants++;
    g_foregroundGrantedTo = processId;
    return TRUE;
}

static BOOL AnyProcessMayComeToFront(void)
{
    return g_foregroundGrants > 0 && g_foregroundGrantedTo == ASFW_ANY;
}

static BOOL g_frontAllowedAtSpawn;

/* Windows must still be able to start the download while it is held. The
 * child is this test's own exe, which ends at once with --install. */
static BOOL FixtureSpawn(const WCHAR *file, const WCHAR *args, DWORD *pid)
{
    WCHAR command[MAX_PATH + 64];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exitCode = 1;
    BOOL ended;
    if (pid) *pid = 0;
    g_spawns++;
    g_frontAllowedAtSpawn = AnyProcessMayComeToFront();
    g_spawnedToInstall = wcscmp(args, L"--install") == 0;
    g_writeRefusedAtSpawn = OpenRefused(file, GENERIC_WRITE);
    g_deleteRefusedAtSpawn = OpenRefused(file, DELETE);
    if (g_spawnFails) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (FAILED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" %s", file, args))) return FALSE;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (!CreateProcessW(file, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) return FALSE;
    CloseHandle(process.hThread);
    /* A child left running would keep the download mapped for every check after. */
    if (WaitForSingleObject(process.hProcess, CHILD_EXIT_WAIT_MS) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, FIXTURE_WAIT_MS);
        CloseHandle(process.hProcess);
        printf("  note  the child started as the download did not end within %d s\n", CHILD_EXIT_WAIT_MS / 1000);
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    ended = GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0;
    if (pid) *pid = process.dwProcessId;
    CloseHandle(process.hProcess);
    if (!ended) SetLastError(ERROR_GEN_FAILURE);
    return ended;
}

#define HeapAlloc FixtureAlloc
#define CreateThread FixtureNoJobThread
#define PostMessageW FixtureUpdateNotice
#define GetTempPathW FixtureTemp
#define WinVerifyTrust FixtureTrust
#define WTHelperProvDataFromStateData FixtureProvider
#define WTHelperGetProvSignerFromChain FixtureSigner
#define WTHelperGetProvCertFromChain FixtureCertificate
#define CertGetNameStringW FixtureCertificateName
#define LoadLibraryExW FixtureLoadVersionImage
#define FindResourceW FixtureFindVersion
#define LoadResource FixtureLoadVersion
#define LockResource FixtureLockVersion
#define SizeofResource FixtureVersionSize
#define FreeLibrary FixtureFreeVersionImage
#define RegOpenKeyExW FixtureCacheOpen
#define RegCreateKeyExW FixtureCacheCreate
#define RegSetValueExW FixtureCacheWrite
#define RegCloseKey FixtureCacheClose
#define WinHttpOpen FixtureHttpOpen
#define WinHttpSetTimeouts FixtureHttpTimeouts
#define WinHttpConnect FixtureHttpConnect
#define WinHttpOpenRequest FixtureHttpRequest
#define WinHttpSendRequest FixtureHttpSend
#define WinHttpReceiveResponse FixtureHttpReceive
#define WinHttpQueryHeaders FixtureHttpStatus
#define WinHttpQueryDataAvailable FixtureHttpAvailable
#define WinHttpReadData FixtureHttpRead
#define WinHttpCloseHandle FixtureHttpClose
#define GetTickCount64 FixtureUpdateClock
#define Util_RegGetString FixtureCachedTag
#define Util_RegGetDword FixtureCachedCheckTime
#define Util_Log FixtureLog
#define Util_Spawn FixtureSpawn
#define Util_RegKeyExists FixtureStillInstalled
#define AllowSetForegroundWindow FixtureAllowForeground
#define Update_Check TestedUpdate_Check
#define Update_Available TestedUpdate_Available
#define Update_Download TestedUpdate_Download
#define Update_Run TestedUpdate_Run
#define Update_RemoveDownload TestedUpdate_RemoveDownload
#define Update_DownloadPath TestedUpdate_DownloadPath
#include "../src/update.c"
#undef HeapAlloc
#undef CreateThread
#undef PostMessageW
#undef GetTempPathW
#undef WinVerifyTrust
#undef WTHelperProvDataFromStateData
#undef WTHelperGetProvSignerFromChain
#undef WTHelperGetProvCertFromChain
#undef CertGetNameStringW
#undef LoadLibraryExW
#undef FindResourceW
#undef LoadResource
#undef LockResource
#undef SizeofResource
#undef FreeLibrary
#undef RegOpenKeyExW
#undef RegCreateKeyExW
#undef RegSetValueExW
#undef RegCloseKey
#undef WinHttpOpen
#undef WinHttpSetTimeouts
#undef WinHttpConnect
#undef WinHttpOpenRequest
#undef WinHttpSendRequest
#undef WinHttpReceiveResponse
#undef WinHttpQueryHeaders
#undef WinHttpQueryDataAvailable
#undef WinHttpReadData
#undef WinHttpCloseHandle
#undef GetTickCount64
#undef Util_RegGetString
#undef Util_RegGetDword
#undef Util_Log
#undef Util_Spawn
#undef Util_RegKeyExists
#undef AllowSetForegroundWindow
#undef Update_Check
#undef Update_Available
#undef Update_Download
#undef Update_Run
#undef Update_RemoveDownload
#undef Update_DownloadPath

typedef struct ReleaseTagCase {
    const char  *name;
    const WCHAR *tag;
    BOOL         accepted;
} ReleaseTagCase;

static void CheckReleaseTags(void)
{
    static const ReleaseTagCase tags[] = {
        { "v1", L"v1", TRUE }, { "1.2", L"1.2", TRUE }, { "V1.2.3", L"V1.2.3", TRUE }, { "1.2.3.4", L"1.2.3.4", TRUE },
        { "empty", L"", FALSE }, { "v", L"v", FALSE }, { "1..2", L"1..2", FALSE }, { "1.", L"1.", FALSE },
        { ".1", L".1", FALSE }, { "1.2.3.4.5", L"1.2.3.4.5", FALSE }, { "1.2-beta", L"1.2-beta", FALSE },
        { "v65536", L"v65536", FALSE },
    };
    size_t i;
    for (i = 0; i < ARRAYSIZE(tags); i++)
        CheckFor("one to four numbers of at most 65535, with or without a v, and nothing else, are a release tag", tags[i].name,
                 !IsReleaseTag(tags[i].tag) == !tags[i].accepted);
}

/* Update_Check with its last check `minutesAgo` minutes before now, made again
 * when the minute turns meanwhile: whether it asked GitHub. */
static BOOL ReleaseCheckAsks(DWORD minutesAgo)
{
    DWORD before = 0;
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        before = MinutesSince1601();
        g_checkTimeCached = TRUE;
        g_cachedCheckMinute = before - minutesAgo;
        g_updateMessages = g_jobThreadRequests = 0;
        TestedUpdate_Check(NULL, WM_APP);
        if (MinutesSince1601() == before) break;
    }
    g_checkTimeCached = FALSE;
    return g_jobThreadRequests > 0;
}

static void CheckUpdateJobs(void)
{
    WCHAR version[32];
    g_failJobAllocation = TRUE;
    g_updateMessages = g_jobThreadRequests = 0;
    TestedUpdate_Check(NULL, WM_APP);
    Check("a release check whose job cannot be allocated still answers once", g_updateMessages == 1 && g_jobThreadRequests == 0);
    g_updateMessages = 0;
    TestedUpdate_Download(NULL, WM_APP);
    Check("a download whose job cannot be allocated answers that nothing was downloaded, and why",
          g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_DOWNLOADED && g_postedUpdateError == ERROR_NOT_ENOUGH_MEMORY &&
          g_jobThreadRequests == 0);
    g_failJobAllocation = FALSE;
    g_updateMessages = g_jobThreadRequests = 0;
    TestedUpdate_Check(NULL, WM_APP);
    Check("a release check whose thread cannot start still answers once", g_updateMessages == 1 && g_jobThreadRequests == 1);
    g_updateMessages = g_jobThreadRequests = 0;
    TestedUpdate_Download(NULL, WM_APP);
    Check("a download whose thread cannot start answers that nothing was downloaded, and why",
          g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_DOWNLOADED && g_postedUpdateError == ERROR_MAX_THRDS_REACHED &&
          g_jobThreadRequests == 1);
    Check("a release check CHECK_MINUTES after the last one answers without asking GitHub",
          !ReleaseCheckAsks(CHECK_MINUTES) && g_updateMessages == 1);
    Check("a release check a minute later asks GitHub", ReleaseCheckAsks(CHECK_MINUTES + 1));
    Check("a clock set back before the last check asks at once", ReleaseCheckAsks((DWORD)-5));
    g_cachedReleaseTag = L"v99.0.0/../../evil";
    g_updateMessages = g_jobThreadRequests = 0;
    TestedUpdate_Download(NULL, WM_APP);
    Check("a cached tag that is not a version never reaches a download URL",
          g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_DOWNLOADED && g_postedUpdateError == ERROR_SUCCESS &&
          g_jobThreadRequests == 0);
    Check("a cached tag that is not a version offers no update", !TestedUpdate_Available(version, ARRAYSIZE(version)));
    g_cachedReleaseTag = L"v1.0.0";
    Check("a release older than this one offers no update", !TestedUpdate_Available(version, ARRAYSIZE(version)));
    g_cachedReleaseTag = L"v" APP_VERSION_WSTR;
    Check("the release this copy is offers no update", !TestedUpdate_Available(version, ARRAYSIZE(version)));
    g_cachedReleaseTag = CACHED_RELEASE_TAG;
    Check("a newer release is offered as its version without the v",
          TestedUpdate_Available(version, ARRAYSIZE(version)) && wcscmp(version, L"99.0.0") == 0);
    Check("the release check's tag and time are read where it keeps them, in REG_ROOT\\Update", g_cacheReadsElsewhere == 0);
}

static void ResetCacheRecords(void)
{
    g_cacheWrites = g_cacheWritesElsewhere = g_cacheRootCloses = g_cacheUpdateCloses = 0;
    g_cacheSubkeyIsUpdate = g_cacheTagWritten = g_cacheTimeWritten = FALSE;
    g_writtenReleaseTag[0] = 0;
}

static void CheckCacheLifecycle(void)
{
    g_cacheRootPresent = TRUE;
    ResetCacheRecords();
    CacheCheckResult(CACHED_RELEASE_TAG);
    Check("a check keeps its tag and the minute it was made in REG_ROOT\\Update",
          g_cacheSubkeyIsUpdate && g_cacheWrites == 2 && wcscmp(g_writtenReleaseTag, CACHED_RELEASE_TAG) == 0 && g_cacheTimeWritten &&
          g_cacheWritesElsewhere == 0);
    Check("a check closes both keys it opened", g_cacheRootCloses == 1 && g_cacheUpdateCloses == 1);
    ResetCacheRecords();
    CacheCheckResult(L"");
    Check("a check that found no tag keeps the cached one and records its time",
          g_cacheWrites == 1 && !g_cacheTagWritten && g_cacheTimeWritten && g_cacheRootCloses == 1 && g_cacheUpdateCloses == 1);
    g_cacheRootPresent = FALSE;
    ResetCacheRecords();
    CacheCheckResult(CACHED_RELEASE_TAG);
    Check("a check that ends after an uninstall cannot recreate its state",
          g_cacheWrites == 0 && g_cacheRootCloses == 0 && g_cacheUpdateCloses == 0 && !g_cacheSubkeyIsUpdate);
    g_cacheRootPresent = TRUE;
    g_cacheRootDeletedAfterOpen = TRUE;
    ResetCacheRecords();
    CacheCheckResult(CACHED_RELEASE_TAG);
    Check("an uninstall between opening REG_ROOT and creating Update leaves nothing behind",
          !g_cacheRootPresent && g_cacheWrites == 0 && g_cacheRootCloses == 1 && g_cacheUpdateCloses == 0);
    g_cacheRootDeletedAfterOpen = FALSE;
}

/* The download as the download thread saves it: a copy of this test's exe. */
static BOOL PlaceDownload(void)
{
    WCHAR self[MAX_PATH];
    return Util_SelfExe(self, ARRAYSIZE(self)) && CopyFileW(self, g_downloadFile, FALSE);
}

static void TrustDownload(void)
{
    g_signatureStatus = ERROR_SUCCESS;
    g_signerName = APP_SIGNER;
    DeclareVersion(99, 0, 0, 0);
}

typedef struct RefusedDownload {
    const char  *name;
    LONG         signatureStatus;
    const WCHAR *signer;
    BOOL         declaresVersion;
    WORD         version[4];
    const WCHAR *tag;
    UpdateResult expected;
    DWORD        expectedError;
} RefusedDownload;

static void CheckRefusedDownloads(void)
{
    static const RefusedDownload refused[] = {
        { "unsigned", TRUST_E_NOSIGNATURE, APP_SIGNER, TRUE, { 99, 0, 0, 0 }, CACHED_RELEASE_TAG, UPDATE_NOT_SIGNED, 0 },
        { "signed by someone else", ERROR_SUCCESS, L"Another publisher", TRUE, { 99, 0, 0, 0 }, CACHED_RELEASE_TAG,
          UPDATE_NOT_SIGNED, 0 },
        { "signed by a certificate without a common name", ERROR_SUCCESS, L"", TRUE, { 99, 0, 0, 0 }, CACHED_RELEASE_TAG,
          UPDATE_NOT_SIGNED, 0 },
        { "revocation lists unreachable", CRYPT_E_REVOCATION_OFFLINE, APP_SIGNER, TRUE, { 99, 0, 0, 0 }, CACHED_RELEASE_TAG,
          UPDATE_NOT_VERIFIED, (DWORD)CRYPT_E_REVOCATION_OFFLINE },
        { "no version resource", ERROR_SUCCESS, APP_SIGNER, FALSE, { 0, 0, 0, 0 }, CACHED_RELEASE_TAG, UPDATE_WRONG_VERSION, 0 },
        { "another version than the announced one", ERROR_SUCCESS, APP_SIGNER, TRUE, { 98, 0, 0, 0 }, CACHED_RELEASE_TAG,
          UPDATE_WRONG_VERSION, 0 },
        { "the announced version, older than this one", ERROR_SUCCESS, APP_SIGNER, TRUE, { 1, 0, 0, 0 }, L"v1.0.0",
          UPDATE_WRONG_VERSION, 0 },
        { "the announced version, this copy's own", ERROR_SUCCESS, APP_SIGNER, TRUE,
          { APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, 0 }, L"v" APP_VERSION_WSTR, UPDATE_WRONG_VERSION, 0 },
    };
    size_t i;
    DWORD error;
    UpdateResult result;
    for (i = 0; i < ARRAYSIZE(refused); i++) {
        if (!Prepared("place a download", PlaceDownload())) return;
        g_signatureStatus = refused[i].signatureStatus;
        g_signerName = refused[i].signer;
        DeclareVersion(refused[i].version[0], refused[i].version[1], refused[i].version[2], refused[i].version[3]);
        g_versionResourcePresent = refused[i].declaresVersion;
        error = ERROR_SUCCESS;
        result = PrepareDownload(g_downloadFile, refused[i].tag, &error);
        CheckFor("a download that fails verification is refused with its reason", refused[i].name,
                 result == refused[i].expected && error == refused[i].expectedError);
        CheckFor("a refused download is deleted", refused[i].name, !Util_FileExists(g_downloadFile));
        g_spawns = 0;
        CheckFor("a refused download cannot be started", refused[i].name,
                 TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED && g_spawns == 0);
    }
    Check("the version reader frees every image it loads", g_versionImageLoads == g_versionImageFrees);
    Check("every signature verification's state is closed, whatever its answer", g_trustStatesOpen == 0);
    TrustDownload();
}

static void CheckDownloadVerification(void)
{
    DWORD error = ERROR_GEN_FAILURE;
    UpdateResult result;
    HANDLE writer;
    g_spawns = g_signatureChecks = 0;
    Check("with nothing verified, Update_Run starts nothing",
          TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED && error == ERROR_SUCCESS && g_spawns == 0);
    if (!Prepared("place a download", PlaceDownload())) return;
    Check("Update_Run refuses a file that the download step did not verify",
          TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED && g_spawns == 0 && g_signatureChecks == 0);
    if (!Prepared("delete the download", DeleteFileW(g_downloadFile))) return;
    TrustDownload();
    error = ERROR_SUCCESS;
    Check("a missing download is reported with Windows' error, unchecked",
          PrepareDownload(g_downloadFile, CACHED_RELEASE_TAG, &error) == UPDATE_NOT_DOWNLOADED && error == ERROR_FILE_NOT_FOUND &&
          g_signatureChecks == 0);
    if (!Prepared("place a download", PlaceDownload())) return;
    writer = CreateFileW(g_downloadFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (Prepared("hold the download open for writing", writer != INVALID_HANDLE_VALUE)) {
        result = PrepareDownload(g_downloadFile, CACHED_RELEASE_TAG, &error);
        CloseHandle(writer);
        Check("a download that cannot be held unchanged is refused unchecked, and deleted",
              result == UPDATE_NOT_DOWNLOADED && error == ERROR_SHARING_VIOLATION && g_signatureChecks == 0 &&
              !Util_FileExists(g_downloadFile));
    }

    CheckRefusedDownloads();

    if (!Prepared("place a download", PlaceDownload())) return;
    g_signatureChecks = 0;
    g_versionReadAsData = g_trustAskedAsRequired = FALSE;
    result = PrepareDownload(g_downloadFile, CACHED_RELEASE_TAG, &error);
    Check("a download signed by APP_SIGNER, of the announced version and newer than this one, is ready", result == UPDATE_READY);
    Check("the signature is checked on the opened download", g_signatureChecks == 1 && g_signatureCheckedOnOpenFile);
    Check("the signature is checked without any window, revocation included along the whole chain",
          g_trustAskedAsRequired && g_trustStatesOpen == 0);
    Check("the download's version is read without loading its code", g_versionReadAsData);
    Check("a ready download cannot be changed or deleted",
          OpenRefused(g_downloadFile, GENERIC_WRITE) && OpenRefused(g_downloadFile, DELETE));
    g_spawns = g_foregroundGrants = 0;
    g_frontAllowedAtSpawn = FALSE;
    error = ERROR_GEN_FAILURE;
    result = TestedUpdate_Run(&error);
    Check("Update_Run starts the ready download with --install",
          result == UPDATE_STARTED && error == ERROR_SUCCESS && g_spawns == 1 && g_spawnedToInstall);
    Check("the update the user asked for may bring its windows to the front: that is allowed before it starts",
          g_frontAllowedAtSpawn);
    Check("the download cannot be changed or deleted until Windows has opened it", g_writeRefusedAtSpawn && g_deleteRefusedAtSpawn);
    Check("a started download is let go", OpensAlone(g_downloadFile));
    Check("a download starts once", TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED && g_spawns == 1);

    if (!Prepared("make a download ready", PrepareDownload(g_downloadFile, CACHED_RELEASE_TAG, &error) == UPDATE_READY)) return;
    g_spawnFails = TRUE;
    result = TestedUpdate_Run(&error);
    g_spawnFails = FALSE;
    Check("a download Windows cannot start is reported with Windows' error",
          result == UPDATE_NOT_STARTED && error == ERROR_ACCESS_DENIED);
    Check("a download that could not start is deleted", !Util_FileExists(g_downloadFile));

    if (!Prepared("place a download", PlaceDownload()) ||
        !Prepared("make a download ready", PrepareDownload(g_downloadFile, CACHED_RELEASE_TAG, &error) == UPDATE_READY))
        return;
    TestedUpdate_RemoveDownload();
    Check("removing the download lets go of it and deletes it",
          !Util_FileExists(g_downloadFile) && TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED);
}

/* A job as StartJob gives it to its thread, which frees it. */
static UpdateJob *NewUpdateJob(const WCHAR *tag)
{
    UpdateJob *job = (UpdateJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *job);
    if (!job) return NULL;
    job->message = WM_APP;
    if (tag) StringCchCopyW(job->tag, ARRAYSIZE(job->tag), tag);
    return job;
}

static void RunReleaseCheck(const char *answer, DWORD status)
{
    UpdateJob *job = NewUpdateJob(NULL);
    ServeAnswer(status, answer, 0, FALSE);
    g_cacheRootPresent = TRUE;
    ResetCacheRecords();
    g_updateMessages = 0;
    if (Prepared("allocate a release check job", job != NULL)) CheckThread(job);
}

static void CheckReleaseRequests(void)
{
    RunReleaseCheck("{\"name\":\"Release\",\"tag_name\":\"v99.1.0\"}", HTTP_STATUS_OK);
    Check("the release check asks GitHub's API over HTTPS",
          wcscmp(g_httpHost, API_HOST) == 0 && wcscmp(g_httpPath, API_PATH) == 0 && g_httpSecure);
    Check("the release check keeps the tag it read and answers once",
          wcscmp(g_writtenReleaseTag, L"v99.1.0") == 0 && g_cacheTimeWritten && g_updateMessages == 1);
    Check("the release check closes every WinHTTP handle", g_httpOpenHandles == 0);
    RunReleaseCheck("{\"tag_name\":\"nightly\"}", HTTP_STATUS_OK);
    Check("a release whose tag is not a version is not kept, the time of the check is",
          !g_cacheTagWritten && g_cacheTimeWritten && g_updateMessages == 1);
    RunReleaseCheck("{\"tag_name\":\"v99.1.0\"}", HTTP_STATUS_FORBIDDEN);
    Check("a refused release check keeps no tag but records its time, so the next one waits",
          !g_cacheTagWritten && g_cacheTimeWritten && g_updateMessages == 1 && g_httpOpenHandles == 0);
}

static void RunDownload(DWORD status, DWORD bodyBytes, BOOL program)
{
    UpdateJob *job = NewUpdateJob(CACHED_RELEASE_TAG);
    ServeAnswer(status, NULL, bodyBytes, program);
    g_updateMessages = 0;
    g_postedUpdateResult = UPDATE_READY;
    g_postedUpdateError = ERROR_GEN_FAILURE;
    if (Prepared("allocate a download job", job != NULL)) DownloadThread(job);
}

static BOOL DownloadRefusedUnsaved(void)
{
    return g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_DOWNLOADED && !Util_FileExists(g_downloadFile) &&
           g_httpOpenHandles == 0;
}

static void CheckDownloadRequests(void)
{
    WIN32_FILE_ATTRIBUTE_DATA saved;
    DWORD error;
    TrustDownload();
    RunDownload(HTTP_STATUS_NOT_FOUND, MIN_DOWNLOAD + 1, TRUE);
    Check("the download asks github.com over HTTPS for the release's exe",
          wcscmp(g_httpHost, DOWNLOAD_HOST) == 0 &&
          wcscmp(g_httpPath, DOWNLOAD_PATH_START CACHED_RELEASE_TAG DOWNLOAD_PATH_END) == 0 && g_httpSecure);
    Check("a download the server refuses saves nothing", DownloadRefusedUnsaved());
    RunDownload(HTTP_STATUS_OK, 0, TRUE);
    Check("an empty answer saves nothing and has no Windows error to report",
          DownloadRefusedUnsaved() && g_postedUpdateError == ERROR_SUCCESS);
    g_httpSendError = ERROR_WINHTTP_CANNOT_CONNECT;
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, TRUE);
    g_httpSendError = 0;
    Check("a server that cannot be reached is reported with WinHTTP's error",
          DownloadRefusedUnsaved() && g_postedUpdateError == ERROR_WINHTTP_CANNOT_CONNECT);
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, FALSE);
    Check("an answer that is not a program is not saved", DownloadRefusedUnsaved());
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD, TRUE);
    Check("a program of at most MIN_DOWNLOAD bytes is not saved", DownloadRefusedUnsaved());
    RunDownload(HTTP_STATUS_OK, MAX_DOWNLOAD + 1, TRUE);
    Check("an answer larger than MAX_DOWNLOAD is dropped before its end",
          DownloadRefusedUnsaved() && g_httpBodyServed <= MAX_DOWNLOAD && g_postedUpdateError == ERROR_FILE_TOO_LARGE);
    g_httpChunkBytes = 1024;
    g_httpMillisecondsPerRead = DOWNLOAD_DEADLINE_MS / 10;
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, TRUE);
    g_httpChunkBytes = HTTP_CHUNK_BYTES;
    g_httpMillisecondsPerRead = 0;
    Check("a download still arriving at its deadline is dropped, reported as timed out",
          DownloadRefusedUnsaved() && g_postedUpdateError == ERROR_TIMEOUT && g_httpBodyServed < MIN_DOWNLOAD + 1);
    if (Prepared("put a folder where the download is saved", CreateDirectoryW(g_downloadFile, NULL))) {
        RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, TRUE);
        Check("a download that cannot be saved is reported with Windows' error",
              g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_DOWNLOADED && g_postedUpdateError == ERROR_ACCESS_DENIED);
        Prepared("remove the folder where the download is saved", RemoveDirectoryW(g_downloadFile));
    }
    g_signatureStatus = TRUST_E_NOSIGNATURE;
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, TRUE);
    Check("a saved download that fails verification is reported and deleted",
          g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_SIGNED && !Util_FileExists(g_downloadFile));
    TrustDownload();
    g_uninstalledDuringDownload = TRUE;
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, TRUE);
    g_uninstalledDuringDownload = FALSE;
    Check("a verified download that ends after an uninstall is deleted, not kept for Update_Run",
          g_updateMessages == 1 && g_postedUpdateResult == UPDATE_NOT_DOWNLOADED && !Util_FileExists(g_downloadFile) &&
          TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED);
    RunDownload(HTTP_STATUS_OK, MIN_DOWNLOAD + 1, TRUE);
    Check("a verified download is saved whole and reported ready",
          g_updateMessages == 1 && g_postedUpdateResult == UPDATE_READY && g_postedUpdateError == ERROR_SUCCESS &&
          GetFileAttributesExW(g_downloadFile, GetFileExInfoStandard, &saved) && saved.nFileSizeHigh == 0 &&
          saved.nFileSizeLow == MIN_DOWNLOAD + 1);
    Check("a ready download stays held for Update_Run", OpenRefused(g_downloadFile, GENERIC_WRITE));
    RunDownload(HTTP_STATUS_NOT_FOUND, 0, FALSE);
    Check("a new download first lets go of the one not started",
          OpensAlone(g_downloadFile) && TestedUpdate_Run(&error) == UPDATE_NOT_DOWNLOADED);
    TestedUpdate_RemoveDownload();
    Check("a download looks for REG_ROOT itself to know whether the program was uninstalled", g_cacheReadsElsewhere == 0);
}

/* ------------------------------------------------------ refused changes */

/* What the copies of install.c, taskbar.c, gui.c, util.c and profiles.c compiled
 * here could change outside the private folder, refused: no check reaches it today,
 * and none may ever reach the user's registry, profiles, icons, shortcuts, pins,
 * tray, links, language or Claude, or start a process. */
static BOOL FixtureRefuseProfileCreation(const WCHAR *name, int color, WCHAR *folder, size_t folderCch, WCHAR *error, size_t errorCch)
{
    (void)name; (void)color;
    if (folderCch) folder[0] = 0;
    StringCchCopyW(error, errorCch, L"Refused by the platform tests.");
    return FALSE;
}

static BOOL FixtureRefuseProfileChange(const WCHAR *folder, const WCHAR *label, int color)
{
    (void)folder; (void)label; (void)color;
    return FALSE;
}

static BOOL FixtureRefuseDefaultProfile(const WCHAR *folder)
{
    (void)folder;
    return FALSE;
}

static void FixtureNoSettingsCopy(const Profile *from, const Profile *to) { (void)from; (void)to; }

static RemoveResult FixtureRefuseProfileRemoval(HWND owner, const Profile *profile)
{
    (void)owner; (void)profile;
    return REMOVE_FAILED;
}

static HRESULT FixtureRefuseShortcut(const ClaudePackage *package, const Profile *profile, const WCHAR *lnk)
{
    (void)package; (void)profile; (void)lnk;
    return E_ACCESSDENIED;
}

static HRESULT FixtureRefuseStartMenuEntry(const ClaudePackage *package, const Profile *profile)
{
    (void)package; (void)profile;
    return E_ACCESSDENIED;
}

static HRESULT FixtureRefuseStartMenuRemoval(const Profile *profile)
{
    (void)profile;
    return E_ACCESSDENIED;
}

static BOOL FixtureNoShortcutRefresh(const Profile *before, const Profile *after, const WCHAR *icon)
{
    (void)before; (void)after; (void)icon;
    return FALSE;
}

static HRESULT FixtureRefusePin(const ClaudePackage *package, const Profile *profile)
{
    (void)package; (void)profile;
    return E_ACCESSDENIED;
}

static void FixtureNoPinRefresh(const Profile *before, const Profile *after, const WCHAR *icon) { (void)before; (void)after; (void)icon; }
static void FixtureNoPinRepair(void) { }
static void FixtureNoWatcherRefresh(const Profile *profile, BOOL linksChanged) { (void)profile; (void)linksChanged; }
static void FixtureKeepIcons(const Profile *profile, const WCHAR *keep) { (void)profile; (void)keep; }
static void FixtureKeepShortcuts(const Profile *profile) { (void)profile; }
static void FixtureNoReleaseCheck(HWND notify, UINT message) { (void)notify; (void)message; }
static void FixtureNoDownload(HWND notify, UINT message) { (void)notify; (void)message; }
static void FixtureNoRepair(void) { }
static void FixtureNoUninstallEnd(void) { }
static void FixtureNoTrayChange(const ClaudePackage *package, const Profile *profile) { (void)package; (void)profile; }
static void FixtureNoTrayGiveBack(const ClaudePackage *package, DWORD pid) { (void)package; (void)pid; }
static void FixtureNoShortcutNotice(const WCHAR *lnk) { (void)lnk; }

static BOOL FixtureNoIcon(const ClaudePackage *package, const Profile *profile, WCHAR *out, size_t cch)
{
    (void)package; (void)profile;
    if (cch) out[0] = 0;
    return FALSE;
}

static BOOL FixtureRefuseHandler(const WCHAR *exe)
{
    (void)exe;
    return FALSE;
}

static BOOL FixtureRefuseLinkChooser(void) { return FALSE; }
static BOOL FixtureNotTheInstalledCopy(void) { return FALSE; }
static BOOL FixtureNotRegistered(void) { return FALSE; }

static BOOL FixtureRefuseLanguage(int language, BOOL persist)
{
    (void)language; (void)persist;
    return FALSE;
}

static void FixtureNoLanguageApplied(const ClaudePackage *package, const ProfileList *list) { (void)package; (void)list; }

static HRESULT FixtureRefuseLaunch(const ClaudePackage *package, const Profile *profile, const WCHAR *url, DWORD *pid, BOOL *identity)
{
    (void)package; (void)profile; (void)url;
    if (pid) *pid = 0;
    if (identity) *identity = FALSE;
    return E_ACCESSDENIED;
}

static BOOL FixtureRefuseSpawn(const WCHAR *exe, const WCHAR *args, DWORD *pid)
{
    (void)exe; (void)args;
    if (pid) *pid = 0;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL FixtureRefuseInstall(BOOL openManager)
{
    (void)openManager;
    return FALSE;
}

static BOOL FixtureNoPendingPath(const Profile *profile, WCHAR *out, size_t cch)
{
    (void)profile;
    if (cch) out[0] = 0;
    return FALSE;
}

static BOOL FixtureRefuseRegistryString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data)
{
    (void)root; (void)key; (void)value; (void)data;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL FixtureRefuseRegistryNumber(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data)
{
    (void)root; (void)key; (void)value; (void)data;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static LSTATUS FixtureRefuseRegistryTreeRemoval(HKEY root, const WCHAR *key)
{
    (void)root; (void)key;
    return ERROR_ACCESS_DENIED;
}

static LSTATUS FixtureRefuseRegistryValueRemoval(HKEY root, const WCHAR *key, const WCHAR *value)
{
    (void)root; (void)key; (void)value;
    return ERROR_ACCESS_DENIED;
}

static BOOL FixtureNoRegistryKey(HKEY root, const WCHAR *key)
{
    (void)root; (void)key;
    return FALSE;
}

static LSTATUS WINAPI FixtureRefuseKeyOpen(HKEY root, LPCWSTR path, DWORD options, REGSAM access, PHKEY key)
{
    (void)root; (void)path; (void)options; (void)access;
    *key = NULL;
    return ERROR_ACCESS_DENIED;
}

static LSTATUS WINAPI FixtureRefuseKeyCreation(HKEY root, LPCWSTR path, DWORD reserved, LPWSTR className, DWORD options,
                                               REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY key, LPDWORD disposition)
{
    (void)root; (void)path; (void)reserved; (void)className; (void)options; (void)access; (void)security; (void)disposition;
    *key = NULL;
    return ERROR_ACCESS_DENIED;
}

static LSTATUS WINAPI FixtureRefuseValueRead(HKEY root, LPCWSTR path, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data,
                                             LPDWORD bytes)
{
    (void)root; (void)path; (void)value; (void)flags; (void)type; (void)data; (void)bytes;
    return ERROR_ACCESS_DENIED;
}

/* RegDeleteTreeW, RegDeleteKeyW and RegDeleteValueW alike. */
static LSTATUS WINAPI FixtureRefuseRegistryRemoval(HKEY key, LPCWSTR name)
{
    (void)key; (void)name;
    return ERROR_ACCESS_DENIED;
}

static BOOL WINAPI FixtureRefuseProcess(LPCWSTR application, LPWSTR commandLine, LPSECURITY_ATTRIBUTES processSecurity,
                                        LPSECURITY_ATTRIBUTES threadSecurity, BOOL inherit, DWORD flags, LPVOID environment,
                                        LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    (void)application; (void)commandLine; (void)processSecurity; (void)threadSecurity; (void)inherit; (void)flags;
    (void)environment; (void)directory; (void)startup;
    ZeroMemory(process, sizeof *process);
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static HINSTANCE WINAPI FixtureRefuseShellOpen(HWND window, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory,
                                               INT show)
{
    (void)window; (void)operation; (void)file; (void)parameters; (void)directory; (void)show;
    return (HINSTANCE)(INT_PTR)SE_ERR_ACCESSDENIED;
}

static HANDLE WINAPI FixtureNoNamedMutexCreation(LPSECURITY_ATTRIBUTES security, BOOL owner, LPCWSTR name)
{
    if (name) {
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    return CreateMutexW(security, owner, NULL);
}

/* -------------------------------------------------------------- install.c */

typedef enum StagedCopy {
    STAGED_COPY_WRITTEN,     /* the new copy is written next to the installed exe */
    STAGED_COPY_READ_ONLY,   /* written and marked read-only, as a copy from a read-only medium is */
    STAGED_COPY_HELD,        /* written, then held open so that it cannot be renamed */
    STAGED_COPY_REFUSED,     /* nothing is written */
    STAGED_COPY_PARTIAL      /* a part is written, then the copy fails */
} StagedCopy;

typedef enum RegistrationFailure {
    REGISTRATION_SUCCEEDS,
    REGISTRATION_STRING_FAILS,
    REGISTRATION_NUMBER_FAILS,
    REGISTRATION_HANDLER_FAILS,
    REGISTRATION_MANAGER_LINK_FAILS,
    REGISTRATION_FAILURE_COUNT
} RegistrationFailure;

static const char *const kRegistrationFailureNames[REGISTRATION_FAILURE_COUNT] = {
    "nothing fails", "a string value", "a number value", "the claude:// handler", "the manager's shortcut"
};

#define ORIGINAL_IMAGE "original image"
#define NEW_IMAGE      "new image"

/* The profiles an install finds: one running with a watcher, one running
 * without, one closed. */
#define WATCHED_PROFILE    L"Claude-Watched"
#define UNWATCHED_PROFILE  L"Claude-Unwatched"
#define CLOSED_PROFILE     L"Claude-Closed"
#define KEPT_PROFILE       L"Claude-NeverOpened"
#define MANAGER_PROCESS_ID 999

static WCHAR g_installDir[MAX_PATH], g_installExe[MAX_PATH], g_installSourceFixture[MAX_PATH], g_uninstallStateDir[MAX_PATH];
static const WCHAR *g_installSource = g_installSourceFixture;   /* where this exe runs from, as Util_SelfExe says */
static WCHAR g_stagedCopyPath[MAX_PATH], g_registeredInstallPath[MAX_PATH];
static StagedCopy g_stagedCopy;
static HANDLE g_stagedCopyHolder = INVALID_HANDLE_VALUE;
static RegistrationFailure g_registrationFailure;
static int g_copies, g_retryPauses, g_watcherStops, g_watchedProfileRestarts, g_otherProfileRestarts, g_registrations;
static int g_installMessages, g_movesAside, g_movesBack;
static BOOL g_windowsTooOld, g_refuseWatcherStop, g_stopGaveBack, g_profileStillRunning = TRUE, g_profileStartsWhileStopping;
static BOOL g_profileFolderIsLink;

/* What the uninstall removed, by the order it removed it in (0: not removed). */
typedef struct UninstallSteps {
    int handler, pins, shortcuts, managerLink, uninstallKey, rootKey, download;
} UninstallSteps;
static UninstallSteps g_removedAt;
static int g_uninstallStep, g_emptyFolderRemovals, g_downloadRemovals, g_handlerRemovals;
static BOOL g_everyProfileShortcutRemoved;

static void ReleaseStagedCopy(void)
{
    if (g_stagedCopyHolder != INVALID_HANDLE_VALUE) CloseHandle(g_stagedCopyHolder);
    g_stagedCopyHolder = INVALID_HANDLE_VALUE;
}

static BOOL WINAPI FixtureCopyToStaged(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
    WCHAR zone[MAX_PATH + 32];
    DWORD written = 0;
    (void)from; (void)failIfExists;
    g_copies++;
    StringCchCopyW(g_stagedCopyPath, ARRAYSIZE(g_stagedCopyPath), to);
    switch (g_stagedCopy) {
    case STAGED_COPY_WRITTEN:
        /* As a copy of a download, it carries the zone mark the installed exe must lose. */
        return WriteFixtureFile(to, NEW_IMAGE) && SUCCEEDED(StringCchPrintfW(zone, ARRAYSIZE(zone), L"%s:Zone.Identifier", to)) &&
               WriteFixtureFile(zone, "[ZoneTransfer]\r\nZoneId=3\r\n");
    case STAGED_COPY_READ_ONLY:
        return WriteFixtureFile(to, NEW_IMAGE) && SetFileAttributesW(to, FILE_ATTRIBUTE_READONLY);
    case STAGED_COPY_HELD:
        ReleaseStagedCopy();
        g_stagedCopyHolder = CreateFileW(to, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        return g_stagedCopyHolder != INVALID_HANDLE_VALUE &&
               WriteFile(g_stagedCopyHolder, NEW_IMAGE, (DWORD)strlen(NEW_IMAGE), &written, NULL);
    case STAGED_COPY_PARTIAL:
        WriteFixtureFile(to, "partial");
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    default:
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

/* The real move, recording the installed exe moved aside and put back. */
static BOOL WINAPI FixtureMove(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    BOOL moved = MoveFileExW(from, to, flags);
    DWORD error = GetLastError();
    if (moved && Core_PathEquals(from, g_installExe)) g_movesAside++;
    if (moved && Core_PathEquals(to, g_installExe)) g_movesBack++;
    SetLastError(error);
    return moved;
}

/* A manager started during the retries removes the staged copy it finds:
 * with g_stagedCopyRemovedAtPause, the first pause does so, and lets go of
 * g_installedExeHolder. */
static BOOL g_stagedCopyRemovedAtPause;
static HANDLE g_installedExeHolder = INVALID_HANDLE_VALUE;

static void WINAPI FixtureRetryPause(DWORD milliseconds)
{
    (void)milliseconds;
    g_retryPauses++;
    if (!g_stagedCopyRemovedAtPause) return;
    g_stagedCopyRemovedAtPause = FALSE;
    DeleteFileW(g_stagedCopyPath);
    if (g_installedExeHolder != INVALID_HANDLE_VALUE) CloseHandle(g_installedExeHolder);
    g_installedExeHolder = INVALID_HANDLE_VALUE;
}

/* An open manager is a stand-in window whose process (an event) ends once it
 * is asked to close. */
#define MANAGER_WINDOW_STAND_IN ((HWND)(INT_PTR)0x61)
static BOOL g_managerOpen;
static HANDLE g_managerProcessStandIn;
static int g_managerCloseRequests, g_managerWaits;
static DWORD g_managerWaitLimit, g_managerWaitResult;

static HWND WINAPI FixtureManagerWindow(LPCWSTR className, LPCWSTR title)
{
    (void)className; (void)title;
    return g_managerOpen ? MANAGER_WINDOW_STAND_IN : NULL;
}

static DWORD WINAPI FixtureManagerProcess(HWND window, LPDWORD processId)
{
    if (processId) *processId = window == MANAGER_WINDOW_STAND_IN ? MANAGER_PROCESS_ID : 0;
    return window == MANAGER_WINDOW_STAND_IN ? 1 : 0;
}

static HANDLE WINAPI FixtureOpenManagerProcess(DWORD access, BOOL inherit, DWORD processId)
{
    HANDLE copy = NULL;
    (void)access; (void)inherit;
    if (processId != MANAGER_PROCESS_ID || !g_managerProcessStandIn) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    DuplicateHandle(GetCurrentProcess(), g_managerProcessStandIn, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return copy;
}

static BOOL WINAPI FixtureCloseManagerWindow(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    if (window == MANAGER_WINDOW_STAND_IN && message == WM_CLOSE) {
        g_managerCloseRequests++;
        SetEvent(g_managerProcessStandIn);
    }
    return TRUE;
}

static DWORD WINAPI FixtureWaitForManager(HANDLE process, DWORD milliseconds)
{
    g_managerWaits++;
    g_managerWaitLimit = milliseconds;
    g_managerWaitResult = WaitForSingleObject(process, min(milliseconds, FIXTURE_WAIT_MS));
    return g_managerWaitResult;
}

/* Renames over the installed name fail while g_refuseRenameAfterAside and the
 * installed exe is moved aside (FixtureMove counts it). */
static BOOL g_refuseRenameAfterAside;

static BOOL WINAPI FixtureRenameOpenFile(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, LPVOID information, DWORD bytes)
{
    if (type == FileRenameInfo && g_refuseRenameAfterAside && g_movesAside > g_movesBack) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return SetFileInformationByHandle(file, type, information, bytes);
}

/* The cleanup cmd an update's download or the uninstall starts: recorded, never
 * run. The handles given back are this process's and thread's pseudo handles,
 * which closing leaves alone. g_cleanupBreakawayRefused: the job this process
 * runs in does not let a child leave it. */
static int g_cleanupAttempts, g_cleanupStarts;
static DWORD g_cleanupFlags;
static BOOL g_cleanupBreakawayRefused;
static WCHAR g_cleanupCommand[4096];

static BOOL WINAPI FixtureCleanupProcess(LPCWSTR application, LPWSTR commandLine, LPSECURITY_ATTRIBUTES processSecurity,
                                         LPSECURITY_ATTRIBUTES threadSecurity, BOOL inherit, DWORD flags, LPVOID environment,
                                         LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    (void)application; (void)processSecurity; (void)threadSecurity; (void)inherit; (void)environment; (void)directory; (void)startup;
    g_cleanupAttempts++;
    if (g_cleanupBreakawayRefused && (flags & CREATE_BREAKAWAY_FROM_JOB)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    g_cleanupStarts++;
    g_cleanupFlags = flags;
    StringCchCopyW(g_cleanupCommand, ARRAYSIZE(g_cleanupCommand), commandLine);
    ZeroMemory(process, sizeof *process);
    process->hProcess = GetCurrentProcess();
    process->hThread = GetCurrentThread();
    return TRUE;
}

static BOOL FixtureDownloadPath(WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchCopyW(out, cch, g_downloadFile));
}

static int g_managerLinkRestores, g_uninstallQuestions, g_waitingSessionChanges;
static BOOL g_uninstallAnyway;

static HRESULT FixtureRestoreManagerLink(const WCHAR *exe)
{
    (void)exe;
    g_managerLinkRestores++;
    return S_FALSE;
}

static int FixtureWaitingChanges(const Profile *profile, PendingEdit *edits, int capacity)
{
    (void)profile; (void)edits; (void)capacity;
    return g_waitingSessionChanges;
}

static BOOL FixtureAskUninstall(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel)
{
    (void)owner; (void)icon; (void)text; (void)ok; (void)cancel; (void)defaultCancel;
    g_uninstallQuestions++;
    return g_uninstallAnyway;
}

static BOOL WINAPI FixtureWindowsVersion(LPOSVERSIONINFOEXW version, DWORD type, DWORDLONG conditions)
{
    (void)version; (void)type; (void)conditions;
    return !g_windowsTooOld;
}

static BOOL FixtureInstallSource(WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchCopyW(out, cch, g_installSource));
}

static BOOL FixtureInstallExe(WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchCopyW(out, cch, g_installExe));
}

static BOOL FixtureInstallDir(WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchCopyW(out, cch, g_installDir));
}

static BOOL FixtureUninstallStateDir(WCHAR *out, size_t cch)
{
    return g_uninstallStateDir[0] && SUCCEEDED(StringCchCopyW(out, cch, g_uninstallStateDir));
}

/* What the installer starts (the installed manager): recorded, never run. */
static int g_installSpawns;
static BOOL g_installSpawnFails;
static WCHAR g_installSpawnExe[MAX_PATH], g_installSpawnArguments[64];

static BOOL FixtureInstallSpawn(const WCHAR *exe, const WCHAR *args, DWORD *pid)
{
    if (pid) *pid = 0;
    g_installSpawns++;
    g_frontAllowedAtSpawn = AnyProcessMayComeToFront();
    StringCchCopyW(g_installSpawnExe, ARRAYSIZE(g_installSpawnExe), exe);
    StringCchCopyW(g_installSpawnArguments, ARRAYSIZE(g_installSpawnArguments), args);
    if (!g_installSpawnFails) return TRUE;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static int FixtureInstallMessage(HWND owner, UINT flags, const WCHAR *format, ...)
{
    (void)owner; (void)flags; (void)format;
    g_installMessages++;
    return IDOK;
}

static BOOL FixtureNoRegistryString(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, size_t cch)
{
    (void)root; (void)key; (void)value;
    if (cch) out[0] = 0;
    return FALSE;
}

static BOOL FixtureRegisterString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data, BOOL *changed)
{
    (void)root; (void)key;
    if (changed) *changed = TRUE;
    g_registrations++;
    if (wcscmp(value, L"InstallPath") == 0) StringCchCopyW(g_registeredInstallPath, ARRAYSIZE(g_registeredInstallPath), data);
    return g_registrationFailure != REGISTRATION_STRING_FAILS;
}

static BOOL FixtureNoRegistryNumber(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out)
{
    (void)root; (void)key; (void)value; (void)out;
    return FALSE;
}

static BOOL FixtureRegisterNumber(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data)
{
    (void)root; (void)key; (void)value; (void)data;
    g_registrations++;
    return g_registrationFailure != REGISTRATION_NUMBER_FAILS;
}

static LSTATUS FixtureEraseRegistryTree(HKEY root, const WCHAR *key)
{
    (void)root;
    if (wcscmp(key, REG_UNINSTALL) == 0) g_removedAt.uninstallKey = ++g_uninstallStep;
    if (wcscmp(key, REG_ROOT) == 0) g_removedAt.rootKey = ++g_uninstallStep;
    return ERROR_SUCCESS;
}

static void FixtureInstallProfiles(ProfileList *list, const ClaudePackage *package)
{
    static const WCHAR *const kFolders[] = { WATCHED_PROFILE, UNWATCHED_PROFILE, CLOSED_PROFILE };
    size_t i;
    (void)package;
    ZeroMemory(list, sizeof *list);
    for (i = 0; i < ARRAYSIZE(kFolders); i++) {
        Profile *profile = &list->items[list->count++];
        StringCchCopyW(profile->folder, ARRAYSIZE(profile->folder), kFolders[i]);
        StringCchCopyW(profile->name, ARRAYSIZE(profile->name), kFolders[i] + wcslen(PROFILE_PREFIX));
        JoinPath(g_root, kFolders[i], profile->dataDir, ARRAYSIZE(profile->dataDir));
        profile->running = !Core_EqualsI(kFolders[i], CLOSED_PROFILE);
    }
}

static BOOL FixtureProfileFolderIsLink(const Profile *profile)
{
    (void)profile;
    return g_profileFolderIsLink;
}

static RemoveResult FixtureRecycleProfileData(HWND owner, const Profile *profile)
{
    (void)owner; (void)profile;
    return REMOVE_FAILED;
}

static BOOL FixtureProfileRunning(const Profile *profile)
{
    (void)profile;
    return g_profileStillRunning;
}

static BOOL FixtureProfileWatched(const Profile *profile)
{
    return Core_EqualsI(profile->folder, WATCHED_PROFILE);
}

static BOOL FixtureStopWatchersForInstall(BOOL giveBack)
{
    g_watcherStops++;
    g_stopGaveBack = giveBack;
    if (g_profileStartsWhileStopping) g_profileStillRunning = TRUE;
    return !g_refuseWatcherStop;
}

static void FixtureRestartWatcher(const Profile *profile)
{
    if (Core_EqualsI(profile->folder, WATCHED_PROFILE)) g_watchedProfileRestarts++;
    else g_otherProfileRestarts++;
}

static BOOL FixtureRegisterHandler(const WCHAR *exe)
{
    (void)exe;
    return g_registrationFailure != REGISTRATION_HANDLER_FAILS;
}

static HRESULT FixtureCreateManagerLink(const WCHAR *exe)
{
    (void)exe;
    return g_registrationFailure == REGISTRATION_MANAGER_LINK_FAILS ? E_ACCESSDENIED : S_OK;
}

static void FixtureUnregisterHandler(void)
{
    g_handlerRemovals++;
    g_removedAt.handler = ++g_uninstallStep;
}

static void FixtureRemovePins(void)
{
    g_removedAt.pins = ++g_uninstallStep;
}

static void FixtureRemoveShortcuts(const Profile *profile)
{
    g_everyProfileShortcutRemoved = profile == NULL;
    g_removedAt.shortcuts = ++g_uninstallStep;
}

static void FixtureRemoveManagerLink(void)
{
    g_removedAt.managerLink = ++g_uninstallStep;
}

static void FixtureRemoveDownload(void)
{
    g_downloadRemovals++;
    g_removedAt.download = ++g_uninstallStep;
}

static BOOL WINAPI FixtureEmptyFolderRemoval(LPCWSTR path)
{
    (void)path;
    g_emptyFolderRemovals++;
    return TRUE;
}

/* The refreshes a change of language makes, recorded and never made: one
 * pass for all profiles, each profile with its own icon, "<folder>.ico",
 * except the one whose icon cannot be made. */
static int g_languageIcons, g_languageShortcutPasses, g_languagePinPasses, g_languageShortcutRefreshes, g_languagePinRefreshes;
static BOOL g_languageRefreshesMatch, g_iconlessProfileRefreshed;
static const WCHAR *g_iconlessProfile;

static BOOL FixtureLanguageIcon(const ClaudePackage *package, const Profile *profile, WCHAR *out, size_t cch)
{
    (void)package;
    g_languageIcons++;
    if (g_iconlessProfile && wcscmp(profile->folder, g_iconlessProfile) == 0) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s.ico", profile->folder));
}

/* How many profiles a pass refreshes; an icon that is not the profile's, or
 * a profile with an icon left out, is a mismatch. */
static int RecordLanguageRefreshes(const ProfileList *list, const WCHAR *const *icons)
{
    WCHAR expected[MAX_PATH];
    int i, refreshed = 0;
    for (i = 0; i < list->count; i++) {
        BOOL iconless = g_iconlessProfile && wcscmp(list->items[i].folder, g_iconlessProfile) == 0;
        if (!icons[i]) {
            if (!iconless) g_languageRefreshesMatch = FALSE;
            continue;
        }
        if (iconless) g_iconlessProfileRefreshed = TRUE;
        if (FAILED(StringCchPrintfW(expected, ARRAYSIZE(expected), L"%s.ico", list->items[i].folder)) || wcscmp(icons[i], expected) != 0)
            g_languageRefreshesMatch = FALSE;
        refreshed++;
    }
    return refreshed;
}

static void FixtureLanguageShortcutRefresh(const ProfileList *list, const WCHAR *const *icons)
{
    g_languageShortcutPasses++;
    g_languageShortcutRefreshes += RecordLanguageRefreshes(list, icons);
}

static void FixtureLanguagePinRefresh(const ProfileList *list, const WCHAR *const *icons)
{
    g_languagePinPasses++;
    g_languagePinRefreshes += RecordLanguageRefreshes(list, icons);
}
#define CopyFileW FixtureCopyToStaged
#define MoveFileExW FixtureMove
#define SetFileInformationByHandle FixtureRenameOpenFile
#define RemoveDirectoryW FixtureEmptyFolderRemoval
#define Sleep FixtureRetryPause
#define FindWindowW FixtureManagerWindow
#define GetWindowThreadProcessId FixtureManagerProcess
#define OpenProcess FixtureOpenManagerProcess
#define PostMessageW FixtureCloseManagerWindow
#define WaitForSingleObject FixtureWaitForManager
#define CreateProcessW FixtureCleanupProcess
#define VerifyVersionInfoW FixtureWindowsVersion
#define Util_SelfExe FixtureInstallSource
#define Util_InstallExe FixtureInstallExe
#define Util_InstallDir FixtureInstallDir
#define Util_StateDir FixtureUninstallStateDir
#define Util_Spawn FixtureInstallSpawn
#define AllowSetForegroundWindow FixtureAllowForeground
#define Util_Log FixtureLog
#define Util_RegGetString FixtureNoRegistryString
#define Util_RegSetStringIfDifferent FixtureRegisterString
#define Util_RegGetDword FixtureNoRegistryNumber
#define Util_RegSetDword FixtureRegisterNumber
#define Util_RegDeleteTree FixtureEraseRegistryTree
#define Ui_Message FixtureInstallMessage
#define Profiles_Load FixtureInstallProfiles
#define Profiles_IsLinked FixtureProfileFolderIsLink
#define Profiles_RecycleData FixtureRecycleProfileData
#define Claude_IsRunning FixtureProfileRunning
#define Taskbar_IsWatched FixtureProfileWatched
#define Taskbar_StopWatchers FixtureStopWatchersForInstall
#define Taskbar_Watch FixtureRestartWatcher
#define Handler_Register FixtureRegisterHandler
#define Handler_Unregister FixtureUnregisterHandler
#define TaskbarPin_RemoveOurs FixtureRemovePins
#define Shortcut_RemoveOurs FixtureRemoveShortcuts
#define Shortcut_CreateManagerLink FixtureCreateManagerLink
#define Shortcut_RestoreManagerLink FixtureRestoreManagerLink
#define Shortcut_RemoveManagerLink FixtureRemoveManagerLink
#define Update_RemoveDownload FixtureRemoveDownload
#define Update_DownloadPath FixtureDownloadPath
#define SessionStore_LoadPending FixtureWaitingChanges
#define Ui_Ask FixtureAskUninstall
#define Install_IsInstalledCopy TestedInstall_IsInstalledCopy
#define Install_IsRegistered TestedInstall_IsRegistered
#define Install_Run TestedInstall_Run
#define Install_Repair TestedInstall_Repair
#define Install_Uninstall TestedInstall_Uninstall
#define Install_FinishUninstall TestedInstall_FinishUninstall
#define Install_ApplyLanguage TestedInstall_ApplyLanguage
#define Icons_Ensure FixtureLanguageIcon
#define Shortcut_RefreshProfiles FixtureLanguageShortcutRefresh
#define TaskbarPin_RefreshProfiles FixtureLanguagePinRefresh
#include "../src/install.c"
#undef CopyFileW
#undef MoveFileExW
#undef SetFileInformationByHandle
#undef RemoveDirectoryW
#undef Sleep
#undef FindWindowW
#undef GetWindowThreadProcessId
#undef OpenProcess
#undef PostMessageW
#undef WaitForSingleObject
#undef CreateProcessW
#undef VerifyVersionInfoW
#undef Util_SelfExe
#undef Util_InstallExe
#undef Util_InstallDir
#undef Util_StateDir
#undef Util_Spawn
#undef AllowSetForegroundWindow
#undef Util_Log
#undef Util_RegGetString
#undef Util_RegSetStringIfDifferent
#undef Util_RegGetDword
#undef Util_RegSetDword
#undef Util_RegDeleteTree
#undef Ui_Message
#undef Profiles_Load
#undef Profiles_IsLinked
#undef Profiles_RecycleData
#undef Claude_IsRunning
#undef Taskbar_IsWatched
#undef Taskbar_StopWatchers
#undef Taskbar_Watch
#undef Handler_Register
#undef Handler_Unregister
#undef TaskbarPin_RemoveOurs
#undef Shortcut_RemoveOurs
#undef Shortcut_CreateManagerLink
#undef Shortcut_RestoreManagerLink
#undef Shortcut_RemoveManagerLink
#undef Update_RemoveDownload
#undef Update_DownloadPath
#undef SessionStore_LoadPending
#undef Ui_Ask
#undef Install_IsInstalledCopy
#undef Install_IsRegistered
#undef Install_Run
#undef Install_Repair
#undef Install_Uninstall
#undef Install_FinishUninstall
#undef Install_ApplyLanguage
#undef Icons_Ensure
#undef Shortcut_RefreshProfiles
#undef TaskbarPin_RefreshProfiles

static void ResetInstallRecords(void)
{
    g_copies = g_retryPauses = g_watcherStops = g_watchedProfileRestarts = g_otherProfileRestarts = g_registrations = 0;
    g_installMessages = g_movesAside = g_movesBack = 0;
    g_stopGaveBack = FALSE;
    g_stagedCopyPath[0] = g_registeredInstallPath[0] = 0;
    ZeroMemory(&g_removedAt, sizeof g_removedAt);
    g_uninstallStep = g_emptyFolderRemovals = g_downloadRemovals = g_handlerRemovals = 0;
    g_everyProfileShortcutRemoved = FALSE;
    g_managerCloseRequests = g_managerWaits = 0;
    g_managerWaitLimit = g_managerWaitResult = 0;
    if (g_managerProcessStandIn) ResetEvent(g_managerProcessStandIn);
    g_cleanupAttempts = g_cleanupStarts = 0;
    g_cleanupFlags = 0;
    g_cleanupCommand[0] = 0;
    g_installSpawns = g_foregroundGrants = 0;
    g_frontAllowedAtSpawn = FALSE;
    g_installSpawnExe[0] = g_installSpawnArguments[0] = 0;
    g_managerLinkRestores = g_uninstallQuestions = 0;
}

/* The files of the private install folder whose name ends with `suffix`. */
static int CountInstallFiles(const WCHAR *suffix)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    int count = 0;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*%s", g_installDir, suffix))) return -1;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        count++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return count;
}

static BOOL PrepareInstallFixture(void)
{
    g_managerProcessStandIn = CreateEventW(NULL, TRUE, FALSE, NULL);
    return Prepared("make the private install paths",
                    JoinPath(g_root, L"install", g_installDir, ARRAYSIZE(g_installDir)) &&
                    JoinPath(g_installDir, L"installed.exe", g_installExe, ARRAYSIZE(g_installExe)) &&
                    JoinPath(g_root, L"download.exe", g_installSourceFixture, ARRAYSIZE(g_installSourceFixture))) &&
           Prepared("create the private install folder", CreateDirectoryW(g_installDir, NULL)) &&
           Prepared("write the installed exe", WriteFixtureFile(g_installExe, ORIGINAL_IMAGE)) &&
           Prepared("make a stand-in for an open manager's process", g_managerProcessStandIn != NULL);
}

static void EndInstallFixture(void)
{
    ReleaseStagedCopy();
    if (g_managerProcessStandIn) CloseHandle(g_managerProcessStandIn);
    g_managerProcessStandIn = NULL;
}

/* The one kept profile the uninstall checks use: closed, never opened. */
static void MakeUninstallList(ProfileList *list)
{
    ZeroMemory(list, sizeof *list);
    list->count = 1;
    StringCchCopyW(list->items[0].folder, ARRAYSIZE(list->items[0].folder), KEPT_PROFILE);
    StringCchCopyW(list->items[0].name, ARRAYSIZE(list->items[0].name), KEPT_PROFILE + wcslen(PROFILE_PREFIX));
    JoinPath(g_root, KEPT_PROFILE, list->items[0].dataDir, ARRAYSIZE(list->items[0].dataDir));
}

static void CheckInstallRefusals(void)
{
    ProfileList list;
    BOOL removeData[MAX_PROFILES] = { 0 };
    ResetInstallRecords();
    g_windowsTooOld = TRUE;
    Check("an unsupported Windows is refused before anything is stopped or copied",
          !TestedInstall_Run(FALSE) && g_watcherStops == 0 && g_copies == 0 && g_installMessages == 1);
    g_windowsTooOld = FALSE;
    ResetInstallRecords();
    g_refuseWatcherStop = TRUE;
    Check("the installer gives up when a watcher does not acknowledge its stop",
          !TestedInstall_Run(FALSE) && g_installMessages == 1 && g_copies == 0);
    Check("an install asks the watchers to hand over, not to give the windows back", g_watcherStops == 1 && !g_stopGaveBack);
    Check("after a failed stop, the profile that ran with a watcher gets one again, and no other profile does",
          g_watchedProfileRestarts == 1 && g_otherProfileRestarts == 0);
    MakeUninstallList(&list);
    ResetInstallRecords();
    Check("an uninstall gives up when a watcher does not acknowledge its stop, removing nothing",
          !TestedInstall_Uninstall(NULL, &list, removeData) && g_handlerRemovals == 0 && !g_removedAt.uninstallKey &&
          !g_removedAt.rootKey && !g_removedAt.download);
    Check("an uninstall asks the watchers to give the windows back to Claude", g_watcherStops == 1 && g_stopGaveBack);
    Check("an uninstall that gives up says why", g_installMessages == 1);
    FixtureInstallProfiles(&list, NULL);
    ResetInstallRecords();
    TestedInstall_Uninstall(NULL, &list, removeData);
    Check("after a failed stop, the uninstall gives the profile that ran with a watcher one again, and no other profile one",
          g_watcherStops == 1 && g_watchedProfileRestarts == 1 && g_otherProfileRestarts == 0);
    g_refuseWatcherStop = FALSE;
}

static void CheckAtomicInstall(void)
{
    WCHAR staged[MAX_PATH], interrupted[MAX_PATH], zone[MAX_PATH + 32], self[MAX_PATH];
    HANDLE holder;
    HMODULE runningImage;

    ResetInstallRecords();
    g_stagedCopy = STAGED_COPY_REFUSED;
    Check("an install whose copy keeps failing is reported once", !TestedInstall_Run(FALSE) && g_installMessages == 1);
    Check("a failing copy is tried INSTALL_ATTEMPTS times, a pause apart",
          g_copies == INSTALL_ATTEMPTS && g_retryPauses == INSTALL_ATTEMPTS - 1);
    Check("a failed copy keeps the installed exe", FileHolds(g_installExe, ORIGINAL_IMAGE));
    Check("a failed copy gives the running profile its stopped watcher back",
          g_watcherStops == 1 && g_watchedProfileRestarts == 1 && g_otherProfileRestarts == 0);
    Check("a failed copy registers nothing", g_registrations == 0);
    ResetInstallRecords();
    g_managerOpen = TRUE;
    TestedInstall_Run(FALSE);
    g_managerOpen = FALSE;
    Check("an open manager is asked to close, then waited for until it has exited",
          g_managerCloseRequests == 1 && g_managerWaits == 1 && g_managerWaitLimit == MANAGER_CLOSE_WAIT_MS &&
          g_managerWaitResult == WAIT_OBJECT_0);
    Check("a failed install opens again the manager it closed, which may come to the front",
          g_installMessages == 1 && g_installSpawns == 1 && Core_PathEquals(g_installSpawnExe, g_installExe) &&
          g_installSpawnArguments[0] == 0 && g_frontAllowedAtSpawn);

    ResetInstallRecords();
    g_stagedCopy = STAGED_COPY_PARTIAL;
    Check("an install whose copy breaks off is reported", !TestedInstall_Run(FALSE));
    Check("a copy that broke off keeps the installed exe and leaves no .new",
          FileHolds(g_installExe, ORIGINAL_IMAGE) && CountInstallFiles(STAGED_SUFFIX) == 0);

    if (!Prepared("make the staged, moved-aside and zone mark paths",
                  SUCCEEDED(StringCchPrintfW(staged, ARRAYSIZE(staged), L"%s" STAGED_SUFFIX, g_installExe)) &&
                  SUCCEEDED(StringCchPrintfW(interrupted, ARRAYSIZE(interrupted), L"%s.1234" ASIDE_SUFFIX, g_installExe)) &&
                  SUCCEEDED(StringCchPrintfW(zone, ARRAYSIZE(zone), L"%s:Zone.Identifier", g_installExe))) ||
        !Prepared("leave a .new and an .old of an interrupted install",
                  WriteFixtureFile(staged, "interrupted") && WriteFixtureFile(interrupted, "interrupted")))
        return;
    ResetInstallRecords();
    g_stagedCopy = STAGED_COPY_WRITTEN;
    Check("a successful install is reported without a message", TestedInstall_Run(FALSE) && g_installMessages == 0);
    Check("the new copy is written next to the installed exe as .new", Core_PathEquals(g_stagedCopyPath, staged));
    Check("the new copy takes the installed exe's name", FileHolds(g_installExe, NEW_IMAGE));
    Check("an install leaves no .new or .old behind", CountInstallFiles(STAGED_SUFFIX) == 0 && CountInstallFiles(ASIDE_SUFFIX) == 0);
    Check("the installed exe does not keep the download's zone mark", GetFileAttributesW(zone) == INVALID_FILE_ATTRIBUTES);
    Check("a successful install registers the installed exe's path",
          g_registrations > 0 && Core_PathEquals(g_registeredInstallPath, g_installExe));
    Check("a successful install gives the running profile its watcher back, and no other profile one",
          g_watcherStops == 1 && g_watchedProfileRestarts == 1 && g_otherProfileRestarts == 0);
    Check("an install from a file other than the update's download leaves that file", g_cleanupStarts == 0);
    ResetInstallRecords();
    g_installSource = g_downloadFile;
    Check("an update installed from its download deletes it once it has exited",
          TestedInstall_Run(FALSE) && g_cleanupStarts == 1 && wcsstr(g_cleanupCommand, L"del /f /q \"") != NULL &&
          wcsstr(g_cleanupCommand, g_downloadFile) != NULL);
    g_installSource = g_installSourceFixture;
    ResetInstallRecords();
    g_stagedCopy = STAGED_COPY_READ_ONLY;
    Check("a copy marked read-only, as from a read-only medium, is still installed",
          TestedInstall_Run(FALSE) && FileHolds(g_installExe, NEW_IMAGE) &&
          !(GetFileAttributesW(g_installExe) & FILE_ATTRIBUTE_READONLY));
    g_stagedCopy = STAGED_COPY_WRITTEN;

    if (!Prepared("write the installed exe", WriteFixtureFile(g_installExe, ORIGINAL_IMAGE))) return;
    holder = CreateFileW(g_installExe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Prepared("hold the installed exe open without sharing, as a scanner can", holder != INVALID_HANDLE_VALUE)) {
        ResetInstallRecords();
        Check("an install over a held exe is reported", !TestedInstall_Run(FALSE) && g_installMessages == 1);
        Check("over a held exe, the copy is made once and only its renaming is retried",
              g_copies == 1 && g_retryPauses == INSTALL_ATTEMPTS - 1 && g_movesAside == 0);
        CloseHandle(holder);
        Check("a held exe keeps its content, with no .new or .old left",
              FileHolds(g_installExe, ORIGINAL_IMAGE) && CountInstallFiles(STAGED_SUFFIX) == 0 && CountInstallFiles(ASIDE_SUFFIX) == 0);
    }
    g_installedExeHolder = CreateFileW(g_installExe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Prepared("hold the installed exe open without sharing", g_installedExeHolder != INVALID_HANDLE_VALUE)) {
        ResetInstallRecords();
        g_stagedCopyRemovedAtPause = TRUE;
        Check("a staged copy that a manager started meanwhile removed is made again",
              TestedInstall_Run(FALSE) && g_copies == 2 && FileHolds(g_installExe, NEW_IMAGE));
        g_stagedCopyRemovedAtPause = FALSE;
        if (g_installedExeHolder != INVALID_HANDLE_VALUE) CloseHandle(g_installedExeHolder);
        g_installedExeHolder = INVALID_HANDLE_VALUE;
        if (!Prepared("write the installed exe", WriteFixtureFile(g_installExe, ORIGINAL_IMAGE))) return;
    }

    /* A new copy that something else holds cannot take the installed name. */
    ResetInstallRecords();
    g_stagedCopy = STAGED_COPY_HELD;
    Check("an install whose new copy cannot take the installed name is reported",
          !TestedInstall_Run(FALSE) && g_installMessages == 1 && g_copies == 1);
    Check("the installed exe stays in place while its new copy is held",
          g_movesAside == 0 && FileHolds(g_installExe, ORIGINAL_IMAGE) && CountInstallFiles(ASIDE_SUFFIX) == 0);
    ReleaseStagedCopy();
    ResetInstallRecords();
    TestedInstall_Repair();
    Check("a .new that could not be deleted goes at the next start", CountInstallFiles(STAGED_SUFFIX) == 0);
    Check("each start restores the manager's shortcut if it is missing", g_managerLinkRestores == 1);
    g_stagedCopy = STAGED_COPY_WRITTEN;

    if (!Prepared("copy an exe as the installed one", Util_SelfExe(self, ARRAYSIZE(self)) && CopyFileW(self, g_installExe, FALSE)))
        return;
    /* Mapped as an image, the file behaves as a running exe: it cannot be
     * replaced or deleted, but it can be renamed. */
    runningImage = LoadLibraryExW(g_installExe, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!Prepared("map the installed exe as Windows maps a running one", runningImage != NULL)) return;
    ResetInstallRecords();
    g_refuseRenameAfterAside = TRUE;
    Check("a running exe moved aside for a new copy that cannot take its name is put back at once",
          !TestedInstall_Run(FALSE) && g_movesAside == INSTALL_ATTEMPTS && g_movesBack == INSTALL_ATTEMPTS &&
          Util_FileExists(g_installExe) && !FileHolds(g_installExe, NEW_IMAGE) && CountInstallFiles(ASIDE_SUFFIX) == 0);
    g_refuseRenameAfterAside = FALSE;
    ResetInstallRecords();
    Check("a running installed exe is replaced", TestedInstall_Run(FALSE) && FileHolds(g_installExe, NEW_IMAGE));
    Check("the running exe is moved aside as .old until it exits",
          g_movesAside == 1 && g_movesBack == 0 && CountInstallFiles(ASIDE_SUFFIX) == 1 && CountInstallFiles(STAGED_SUFFIX) == 0);
    FreeLibrary(runningImage);
    ResetInstallRecords();
    TestedInstall_Repair();
    Check("the next start removes the exe moved aside and the update download",
          CountInstallFiles(ASIDE_SUFFIX) == 0 && g_downloadRemovals == 1);
}

static void CheckInstallRegistration(void)
{
    int failure;
    g_stagedCopy = STAGED_COPY_WRITTEN;
    for (failure = REGISTRATION_STRING_FAILS; failure < REGISTRATION_FAILURE_COUNT; failure++) {
        g_registrationFailure = (RegistrationFailure)failure;
        ResetInstallRecords();
        CheckFor("an install whose registration fails is reported as incomplete", kRegistrationFailureNames[failure],
                 !TestedInstall_Run(FALSE) && g_installMessages == 1);
        CheckFor("a failed registration gives the running profile its watcher back", kRegistrationFailureNames[failure],
                 g_watcherStops == 1 && g_watchedProfileRestarts == 1);
    }
    g_registrationFailure = REGISTRATION_HANDLER_FAILS;
    ResetInstallRecords();
    g_managerOpen = TRUE;
    g_installSource = g_downloadFile;
    Check("a failed registration opens again the manager the installer closed, and the update's download still goes",
          !TestedInstall_Run(FALSE) && g_installSpawns == 1 && g_installSpawnArguments[0] == 0 && g_cleanupStarts == 1 &&
          wcsstr(g_cleanupCommand, L"del /f /q \"") != NULL && wcsstr(g_cleanupCommand, g_downloadFile) != NULL);
    g_managerOpen = FALSE;
    g_installSource = g_installSourceFixture;
    ResetInstallRecords();
    Check("after a failed registration, the manager the user asked for opens, without asking about claude:// links",
          !TestedInstall_Run(TRUE) && g_installSpawns == 1 && g_installSpawnArguments[0] == 0 && g_installMessages == 1);
    g_registrationFailure = REGISTRATION_SUCCEEDS;
    ResetInstallRecords();
    g_profileStillRunning = FALSE;
    Check("a profile closed during the install gets no watcher",
          TestedInstall_Run(FALSE) && g_watcherStops == 1 && g_watchedProfileRestarts == 0);
    g_profileStillRunning = TRUE;
}

/* Install_Run(TRUE): the manager the user installed starts at once. */
static void CheckInstalledManagerStart(void)
{
    g_stagedCopy = STAGED_COPY_WRITTEN;
    ResetInstallRecords();
    Check("an update starts the installed manager without asking about claude:// links again",
          TestedInstall_Run(TRUE) && g_installSpawns == 1 && Core_PathEquals(g_installSpawnExe, g_installExe) &&
          g_installSpawnArguments[0] == 0);
    Check("the manager the user installed may come to the front: that is allowed before it starts", g_frontAllowedAtSpawn);
    if (!Prepared("remove the installed exe, as before a first install", DeleteFileW(g_installExe))) return;
    ResetInstallRecords();
    Check("a first install starts the manager offering to set up claude:// links",
          TestedInstall_Run(TRUE) && g_installSpawns == 1 && wcscmp(g_installSpawnArguments, L"--set-up-links") == 0);
    ResetInstallRecords();
    g_installSpawnFails = TRUE;
    Check("an installed manager that cannot be started is reported",
          !TestedInstall_Run(TRUE) && g_installSpawns == 1 && g_installMessages == 1);
    g_installSpawnFails = FALSE;
}

static void ResetLanguageRecords(const WCHAR *iconlessProfile)
{
    ResetInstallRecords();
    g_languageIcons = g_languageShortcutPasses = g_languagePinPasses = g_languageShortcutRefreshes = g_languagePinRefreshes = 0;
    g_languageRefreshesMatch = TRUE;
    g_iconlessProfile = iconlessProfile;
    g_iconlessProfileRefreshed = FALSE;
}

/* After a change of the interface language, what Windows shows of the
 * program is written again in it. */
static void CheckLanguageApplied(void)
{
    ClaudePackage package;
    ProfileList list;
    ZeroMemory(&package, sizeof package);
    FixtureInstallProfiles(&list, &package);
    ResetLanguageRecords(NULL);
    TestedInstall_ApplyLanguage(&package, &list);
    Check("a change of language writes the Apps & features entry and the manager's shortcut again",
          Core_PathEquals(g_registeredInstallPath, g_installExe) && g_managerLinkRestores == 1);
    Check("a change of language refreshes every profile's shortcuts and pins in one pass each, with its own icon",
          g_languageIcons == list.count && g_languageShortcutPasses == 1 && g_languagePinPasses == 1 &&
          g_languageShortcutRefreshes == list.count && g_languagePinRefreshes == list.count && g_languageRefreshesMatch);
    ResetLanguageRecords(UNWATCHED_PROFILE);
    TestedInstall_ApplyLanguage(&package, &list);
    Check("a profile whose icon cannot be made keeps its shortcuts and pin as they are, the others are refreshed",
          g_languageIcons == list.count && g_languageShortcutRefreshes == list.count - 1 &&
          g_languagePinRefreshes == list.count - 1 && !g_iconlessProfileRefreshed && g_languageRefreshesMatch);
    ResetLanguageRecords(NULL);
}

/* The files a profile never opened may hold: the settings copied to it. */
static BOOL WriteCopiedSettings(const WCHAR *dataDir, WCHAR *desktopSettings, size_t desktopCch, WCHAR *appSettings, size_t appCch)
{
    return (Util_DirExists(dataDir) || CreateDirectoryW(dataDir, NULL)) &&
           JoinPath(dataDir, CLAUDE_DESKTOP_SETTINGS, desktopSettings, desktopCch) && WriteFixtureFile(desktopSettings, "{}") &&
           JoinPath(dataDir, CLAUDE_APP_SETTINGS, appSettings, appCch) && WriteFixtureFile(appSettings, "{}");
}

static void CheckUninstall(void)
{
    ProfileList list;
    BOOL removeData[MAX_PROFILES] = { 0 };
    WCHAR stateFile[MAX_PATH], desktopSettings[MAX_PATH], appSettings[MAX_PATH], localState[MAX_PATH] = L"", userFile[MAX_PATH] = L"";
    MakeUninstallList(&list);
    if (!Prepared("make a private state folder holding a file",
                  JoinPath(g_root, L"uninstall-state", g_uninstallStateDir, ARRAYSIZE(g_uninstallStateDir)) &&
                  CreateDirectoryW(g_uninstallStateDir, NULL) &&
                  JoinPath(g_uninstallStateDir, L"fixture.log", stateFile, ARRAYSIZE(stateFile)) && WriteFixtureFile(stateFile, "log"))) {
        g_uninstallStateDir[0] = 0;
        return;
    }
    ResetInstallRecords();
    g_profileStillRunning = FALSE;
    g_profileStartsWhileStopping = TRUE;
    Check("a kept profile that starts during the uninstall keeps its folder",
          TestedInstall_Uninstall(NULL, &list, removeData) && g_emptyFolderRemovals == 0);
    Check("an uninstall removes the claude:// handler, the pins, every profile's shortcuts and both registry keys",
          g_handlerRemovals == 1 && g_removedAt.pins && g_removedAt.shortcuts && g_everyProfileShortcutRemoved &&
          g_removedAt.uninstallKey && g_removedAt.rootKey);
    Check("the update download goes after REG_ROOT, so that a download ending meanwhile deletes itself",
          g_downloadRemovals == 1 && g_removedAt.download > g_removedAt.rootKey);
    Check("the manager's shortcut and the state folder stay until the uninstall ends: a line logged meanwhile would make it again",
          !g_removedAt.managerLink && Util_DirExists(g_uninstallStateDir));
    TestedInstall_FinishUninstall();
    Check("the end of the uninstall removes the manager's shortcut and its state folder",
          g_removedAt.managerLink && !Util_DirExists(g_uninstallStateDir));
    g_profileStartsWhileStopping = FALSE;
    g_profileStillRunning = FALSE;
    g_emptyFolderRemovals = 0;
    if (Prepared("write the settings copied to a profile never opened",
                 WriteCopiedSettings(list.items[0].dataDir, desktopSettings, ARRAYSIZE(desktopSettings), appSettings,
                                     ARRAYSIZE(appSettings))))
        Check("a closed kept profile that was never opened loses the settings copied to it, then its folder",
              TestedInstall_Uninstall(NULL, &list, removeData) && g_emptyFolderRemovals == 1 &&
              !Util_FileExists(desktopSettings) && !Util_FileExists(appSettings));
    g_emptyFolderRemovals = 0;
    if (Prepared("write the settings copied to a profile never opened, and a file of the user's",
                 WriteCopiedSettings(list.items[0].dataDir, desktopSettings, ARRAYSIZE(desktopSettings), appSettings,
                                     ARRAYSIZE(appSettings)) &&
                 JoinPath(list.items[0].dataDir, L"notes.txt", userFile, ARRAYSIZE(userFile)) && WriteFixtureFile(userFile, "mine")))
        Check("a kept profile never opened that holds anything else keeps all its files",
              TestedInstall_Uninstall(NULL, &list, removeData) && g_emptyFolderRemovals == 0 && Util_FileExists(desktopSettings) &&
              Util_FileExists(appSettings) && Util_FileExists(userFile));
    DeleteFileW(userFile);
    if (Prepared("write the files of a profile opened once",
                 WriteCopiedSettings(list.items[0].dataDir, desktopSettings, ARRAYSIZE(desktopSettings), appSettings,
                                     ARRAYSIZE(appSettings)) &&
                 JoinPath(list.items[0].dataDir, L"Local State", localState, ARRAYSIZE(localState)) &&
                 WriteFixtureFile(localState, "{}")))
        Check("a kept profile opened once keeps all its files",
              TestedInstall_Uninstall(NULL, &list, removeData) && g_emptyFolderRemovals == 0 && Util_FileExists(appSettings));
    DeleteFileW(localState);
    g_profileFolderIsLink = TRUE;
    g_emptyFolderRemovals = 0;
    if (Prepared("write the settings in the folder a kept profile's link leads to",
                 WriteCopiedSettings(list.items[0].dataDir, desktopSettings, ARRAYSIZE(desktopSettings), appSettings,
                                     ARRAYSIZE(appSettings))))
        Check("a kept profile folder that is a link is never removed, nor what it leads to",
              TestedInstall_Uninstall(NULL, &list, removeData) && g_emptyFolderRemovals == 0 &&
              Util_FileExists(desktopSettings) && Util_FileExists(appSettings));
    g_profileFolderIsLink = FALSE;
    list.items[0].running = TRUE;
    removeData[0] = TRUE;
    ResetInstallRecords();
    Check("an uninstall refuses to remove the data of a running profile, before stopping anything",
          !TestedInstall_Uninstall(NULL, &list, removeData) && g_installMessages == 1 && g_watcherStops == 0);
    removeData[0] = FALSE;
    g_waitingSessionChanges = 1;
    g_uninstallAnyway = FALSE;
    ResetInstallRecords();
    Check("session changes waiting for a running kept profile are not discarded without asking",
          !TestedInstall_Uninstall(NULL, &list, removeData) && g_uninstallQuestions == 1 && g_watcherStops == 0 &&
          g_handlerRemovals == 0);
    g_uninstallAnyway = TRUE;
    Check("the user may uninstall without them",
          TestedInstall_Uninstall(NULL, &list, removeData) && g_uninstallQuestions == 2 && g_handlerRemovals == 1);
    g_waitingSessionChanges = 0;
    g_uninstallAnyway = FALSE;
    g_profileStillRunning = TRUE;
    g_uninstallStateDir[0] = 0;
}

/* `verb "path"`, as the cleanup command removes a path. */
static BOOL CommandRemoves(const WCHAR *command, const WCHAR *verb, const WCHAR *path)
{
    WCHAR expected[MAX_PATH + 32];
    return SUCCEEDED(StringCchPrintfW(expected, ARRAYSIZE(expected), L"%s \"%s\"", verb, path)) && wcsstr(command, expected) != NULL;
}

/* What an uninstall run from the installed copy leaves to a hidden cmd once
 * it has exited. cmd expands a % even inside quotes: no such path is given. */
static void CheckCleanupAfterExit(void)
{
    WCHAR stateDir[MAX_PATH], installDir[MAX_PATH];
    if (!Prepared("make a private state folder",
                  JoinPath(g_root, L"cleanup-state", stateDir, ARRAYSIZE(stateDir)) && CreateDirectoryW(stateDir, NULL)))
        return;
    StringCchCopyW(g_uninstallStateDir, ARRAYSIZE(g_uninstallStateDir), stateDir);
    StringCchCopyW(installDir, ARRAYSIZE(installDir), g_installDir);
    g_installSource = g_installExe;
    ResetInstallRecords();
    TestedInstall_FinishUninstall();
    Check("the end of an uninstall from the installed copy starts one hidden cleanup, which waits for this process to exit",
          g_cleanupStarts == 1 && (g_cleanupFlags & CREATE_NO_WINDOW) && wcsstr(g_cleanupCommand, L"ping.exe\" -n ") != NULL);
    Check("the cleanup's cmd runs no AutoRun command and expands no !, whatever the registry says",
          wcsstr(g_cleanupCommand, L"cmd.exe\" /d /v:off /s /c \"") != NULL);
    Check("the cleanup removes the install folder, the state folder and the update download",
          CommandRemoves(g_cleanupCommand, L"rd /s /q", installDir) && CommandRemoves(g_cleanupCommand, L"rd /s /q", stateDir) &&
          CommandRemoves(g_cleanupCommand, L"del /f /q", g_downloadFile));
    Check("the cleanup leaves the job this process runs in", (g_cleanupFlags & CREATE_BREAKAWAY_FROM_JOB) != 0);
    ResetInstallRecords();
    g_cleanupBreakawayRefused = TRUE;
    TestedInstall_FinishUninstall();
    g_cleanupBreakawayRefused = FALSE;
    Check("a job that keeps its children still gets the cleanup started, in the job",
          g_cleanupAttempts == 2 && g_cleanupStarts == 1 && g_cleanupFlags == CREATE_NO_WINDOW);
    if (Prepared("make a state folder path holding a %",
                 JoinPath(g_root, L"state%TEMP%", g_uninstallStateDir, ARRAYSIZE(g_uninstallStateDir)))) {
        ResetInstallRecords();
        TestedInstall_FinishUninstall();
        Check("a state folder whose path holds a % is left out of the cleanup, the rest is not",
              g_cleanupStarts == 1 && wcsstr(g_cleanupCommand, L"%TEMP%") == NULL &&
              CommandRemoves(g_cleanupCommand, L"rd /s /q", installDir));
    }
    if (Prepared("make an install folder path holding a %", JoinPath(g_root, L"install%TEMP%", g_installDir, ARRAYSIZE(g_installDir)))) {
        ResetInstallRecords();
        TestedInstall_FinishUninstall();
        Check("an install folder whose path holds a % is never given to cmd", g_cleanupAttempts == 0);
    }
    StringCchCopyW(g_installDir, ARRAYSIZE(g_installDir), installDir);
    g_installSource = g_installSourceFixture;
    g_uninstallStateDir[0] = 0;
}

/* -------------------------------------------------------------- taskbar.c */

#define FIRST_WATCHER_WINDOW  ((HWND)(INT_PTR)1)
#define SECOND_WATCHER_WINDOW ((HWND)(INT_PTR)2)
#define WATCHER_PROCESS_ID    777
#define WATCHER_THREAD_ID     888
#define WATCHER_WINDOW_CLASS  L"ClaudeDesktopProfilesManagerWatch"   /* taskbar.c's WATCH_CLASS */

static HANDLE g_watcherStopEvent, g_registrationMutex, g_stopReachedRegistration;
static HANDLE g_processStandIn;   /* signaled when the process it stands for has exited */
static BOOL g_noWatcherWindows, g_watcherHasTwoWindows, g_registrationLockFails, g_watcherWaitTimesOut;
static BOOL g_watcherBeingRegistered;   /* its stop events and window do not exist until its registration ends */
static BOOL g_stopEventLookedForDuringRegistration;
static WCHAR g_openedStopEvent[80];
static DWORD g_watcherOpenError;
static int g_watcherOpens;

#define WAIT_FIXTURE_FOLDER L"Claude-WaitFixture"
static WCHAR g_waitDataDir[MAX_PATH], g_waitStorageDir[MAX_PATH], g_watchedFolder[MAX_PATH];
static BOOL g_waitProfileGone, g_waitProfileIsStock, g_watchedSubtree, g_hookedWindowCreations;
static BOOL g_creationHookFails, g_folderRearmFails;
static HANDLE g_folderWatchArmed, g_creationHookSet;
static int g_folderWatches, g_folderRearms, g_folderWatchCloses, g_creationHooks, g_creationHookRemovals;
static DWORD g_watchedChanges, g_hookedProcess, g_hookFlags;

static HANDLE WINAPI FixtureOpenStopEvent(DWORD access, BOOL inherit, LPCWSTR name)
{
    HANDLE copy = NULL;
    (void)access; (void)inherit;
    StringCchCopyW(g_openedStopEvent, ARRAYSIZE(g_openedStopEvent), name);
    if (g_watcherBeingRegistered) g_stopEventLookedForDuringRegistration = TRUE;
    if (!g_watcherStopEvent || g_watcherBeingRegistered) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    DuplicateHandle(GetCurrentProcess(), g_watcherStopEvent, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return copy;
}

static BOOL WINAPI FixtureEnumWatcherWindows(WNDENUMPROC callback, LPARAM parameter)
{
    if (g_noWatcherWindows || g_watcherBeingRegistered) return TRUE;
    if (!callback(FIRST_WATCHER_WINDOW, parameter)) return FALSE;
    return !g_watcherHasTwoWindows || callback(SECOND_WATCHER_WINDOW, parameter);
}

static BOOL IsWatcherWindowFixture(HWND window)
{
    return window == FIRST_WATCHER_WINDOW || window == SECOND_WATCHER_WINDOW;
}

static int WINAPI FixtureWindowClass(HWND window, LPWSTR out, int cch)
{
    if (!IsWatcherWindowFixture(window)) return GetClassNameW(window, out, cch);
    if (FAILED(StringCchCopyW(out, (size_t)cch, WATCHER_WINDOW_CLASS))) return 0;
    return (int)wcslen(out);
}

static DWORD WINAPI FixtureWindowProcess(HWND window, LPDWORD pid)
{
    if (!IsWatcherWindowFixture(window)) return GetWindowThreadProcessId(window, pid);
    if (pid) *pid = WATCHER_PROCESS_ID;
    return WATCHER_THREAD_ID;
}

static HANDLE WINAPI FixtureOpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    HANDLE copy = NULL;
    (void)access; (void)inherit; (void)pid;
    g_watcherOpens++;
    if (g_watcherOpenError || !g_processStandIn) {
        SetLastError(g_watcherOpenError ? g_watcherOpenError : ERROR_INVALID_PARAMETER);
        return NULL;
    }
    DuplicateHandle(GetCurrentProcess(), g_processStandIn, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return copy;
}

static DWORD WINAPI FixtureWaitForWatchers(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds)
{
    if (g_watcherWaitTimesOut) return WAIT_TIMEOUT;
    return WaitForMultipleObjects(count, handles, all, milliseconds);
}

/* The registration lock is a private mutex, whatever its name. */
static HANDLE WINAPI FixtureRegistrationMutex(LPSECURITY_ATTRIBUTES security, BOOL owner, LPCWSTR name)
{
    HANDLE copy = NULL;
    (void)security; (void)owner; (void)name;
    if (g_registrationLockFails) {
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    if (g_stopReachedRegistration) SetEvent(g_stopReachedRegistration);
    DuplicateHandle(GetCurrentProcess(), g_registrationMutex, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return copy;
}

/* No named event or mutex is ever opened or made: the program's are the
 * user's watchers'. */
static HANDLE WINAPI FixtureUnnamedEvent(LPSECURITY_ATTRIBUTES security, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    if (name) {
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    return CreateEventW(security, manualReset, initialState, NULL);
}

static HANDLE WINAPI FixtureNoNamedMutex(DWORD access, BOOL inherit, LPCWSTR name)
{
    (void)access; (void)inherit; (void)name;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return NULL;
}

/* Running is what claude.c finds: a Chrome_MessageWindow titled with the data folder. */
static void FixtureWaitProfiles(ProfileList *list, const ClaudePackage *package)
{
    (void)package;
    ZeroMemory(list, sizeof *list);
    if (g_waitProfileGone) return;
    list->count = 1;
    StringCchCopyW(list->items[0].folder, ARRAYSIZE(list->items[0].folder), g_waitProfileIsStock ? STOCK_FOLDER : WAIT_FIXTURE_FOLDER);
    StringCchCopyW(list->items[0].name, ARRAYSIZE(list->items[0].name), L"Wait fixture");
    StringCchCopyW(list->items[0].dataDir, ARRAYSIZE(list->items[0].dataDir), g_waitDataDir);
    StringCchCopyW(list->items[0].storageDir, ARRAYSIZE(list->items[0].storageDir), g_waitStorageDir);
    list->items[0].isStock = g_waitProfileIsStock;
    list->items[0].running = Claude_IsRunning(&list->items[0]);
}

static HANDLE WINAPI FixtureWatchFolder(LPCWSTR folder, BOOL subtree, DWORD changes)
{
    HANDLE change = FindFirstChangeNotificationW(folder, subtree, changes);
    g_folderWatches++;
    StringCchCopyW(g_watchedFolder, ARRAYSIZE(g_watchedFolder), folder);
    g_watchedSubtree = subtree;
    g_watchedChanges = changes;
    if (change != INVALID_HANDLE_VALUE && g_folderWatchArmed) SetEvent(g_folderWatchArmed);
    return change;
}

/* g_windowComesAfterChange: the profile's Claude shows its window a moment
 * after the folder changed, on the waiting thread, with no change after it. */
#define WINDOW_AFTER_CHANGE_MS 30   /* under taskbar.c's START_RECHECK_MS, checked below */
static BOOL g_windowComesAfterChange;
static UINT_PTR g_windowAfterChangeTimer;
static HWND g_windowShownAfterChange;

static void CALLBACK ShowWindowAfterChange(HWND window, UINT message, UINT_PTR timer, DWORD time)
{
    (void)window; (void)message; (void)time;
    KillTimer(NULL, timer);
    g_windowAfterChangeTimer = 0;
    g_windowShownAfterChange = CreateWindowExW(0, L"Chrome_MessageWindow", g_waitDataDir, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                               GetModuleHandleW(NULL), NULL);
}

static BOOL WINAPI FixtureRearmFolderWatch(HANDLE change)
{
    g_folderRearms++;
    if (g_folderRearmFails) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (g_windowComesAfterChange) {
        g_windowComesAfterChange = FALSE;
        g_windowAfterChangeTimer = SetTimer(NULL, 0, WINDOW_AFTER_CHANGE_MS, ShowWindowAfterChange);
    }
    return FindNextChangeNotification(change);
}

static BOOL WINAPI FixtureCloseFolderWatch(HANDLE change)
{
    g_folderWatchCloses++;
    return FindCloseChangeNotification(change);
}

/* No real hook is set: what is asked for is recorded. */
static HWINEVENTHOOK WINAPI FixtureSetCreationHook(DWORD firstEvent, DWORD lastEvent, HMODULE module, WINEVENTPROC callback,
                                                   DWORD process, DWORD thread, DWORD flags)
{
    (void)module; (void)callback; (void)thread;
    g_creationHooks++;
    g_hookedWindowCreations = firstEvent == EVENT_OBJECT_CREATE && lastEvent == EVENT_OBJECT_CREATE;
    g_hookedProcess = process;
    g_hookFlags = flags;
    if (g_creationHookFails) {
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    if (g_creationHookSet) SetEvent(g_creationHookSet);
    return (HWINEVENTHOOK)(INT_PTR)(0x1000 + g_creationHooks);
}

static BOOL WINAPI FixtureRemoveCreationHook(HWINEVENTHOOK hook)
{
    (void)hook;
    g_creationHookRemovals++;
    return TRUE;
}

/* The watcher a start is handed to: a private window of this test, found by
 * the watcher's class and the profile's folder. */
static HWND g_handoverWatcher;

static HWND WINAPI FixtureFindWatcher(LPCWSTR className, LPCWSTR title)
{
    if (!g_handoverWatcher || !className || wcscmp(className, WATCHER_WINDOW_CLASS) != 0 || !title ||
        !Core_EqualsI(title, WAIT_FIXTURE_FOLDER))
        return NULL;
    return g_handoverWatcher;
}

/* The reopening after a Claude update: the package Claude ran, then the new
 * one, which the g_packageLooksBeforeUpdate first looks do not find yet. */
#define RAN_PACKAGE         L"Claude_1.0.0.0_x64__fixture"
#define UPDATED_PACKAGE     L"Claude_2.0.0.0_x64__fixture"
#define PACKAGE_LIST_KEY    ((HKEY)(INT_PTR)0x71)
#define REOPENED_CLAUDE_PID 5432
static BOOL g_closedForUpdate, g_packageListUnavailable, g_packageListChangesAtOnce, g_appsFolderChangesBeforeUpdate;
static HANDLE g_appsFolderChange;   /* taskbar.c's g_appsChanged, set by its window when the Apps folder changes */
static int g_packageLooks, g_packageLooksBeforeUpdate, g_packageListOpenKeys, g_packageListArms, g_reopenLaunches;
static HRESULT g_reopenLaunchResult;

static BOOL FixtureClosedForUpdate(const ClaudePackage *package, const Profile *profile)
{
    (void)package; (void)profile;
    return g_closedForUpdate;
}

static BOOL FixtureInstalledPackage(ClaudePackage *package)
{
    BOOL updated = g_packageLooks++ >= g_packageLooksBeforeUpdate;
    ZeroMemory(package, sizeof *package);
    package->found = TRUE;
    StringCchCopyW(package->fullName, ARRAYSIZE(package->fullName), updated ? UPDATED_PACKAGE : RAN_PACKAGE);
    StringCchCopyW(package->version, ARRAYSIZE(package->version), updated ? L"2.0.0.0" : L"1.0.0.0");
    /* The Apps folder changes while the new package installs. */
    if (!updated && g_appsFolderChangesBeforeUpdate) SetEvent(g_appsFolderChange);
    return TRUE;
}

static HRESULT FixtureReopenLaunch(const ClaudePackage *package, const Profile *profile, const WCHAR *url, DWORD *pid, BOOL *identity)
{
    (void)package; (void)profile; (void)url;
    g_reopenLaunches++;
    if (pid) *pid = REOPENED_CLAUDE_PID;
    if (identity) *identity = TRUE;
    return g_reopenLaunchResult;
}

static LSTATUS WINAPI FixturePackageListOpen(HKEY root, LPCWSTR path, DWORD options, REGSAM access, PHKEY key)
{
    (void)root; (void)options;
    if (g_packageListUnavailable || wcscmp(path, REG_PACKAGES) != 0 || !(access & KEY_NOTIFY)) return ERROR_ACCESS_DENIED;
    g_packageListOpenKeys++;
    *key = PACKAGE_LIST_KEY;
    return ERROR_SUCCESS;
}

/* g_packageListChangesAtOnce: each wait for a change sees one at once. */
static LSTATUS WINAPI FixturePackageListNotify(HKEY key, BOOL subtree, DWORD filter, HANDLE event, BOOL asynchronous)
{
    (void)subtree; (void)filter; (void)asynchronous;
    if (key != PACKAGE_LIST_KEY) return ERROR_INVALID_HANDLE;
    g_packageListArms++;
    if (g_packageListChangesAtOnce) SetEvent(event);
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI FixturePackageListClose(HKEY key)
{
    if (key == PACKAGE_LIST_KEY) g_packageListOpenKeys--;
    return ERROR_SUCCESS;
}

#define OpenEventW FixtureOpenStopEvent
#define CreateEventW FixtureUnnamedEvent
#define OpenMutexW FixtureNoNamedMutex
#define EnumWindows FixtureEnumWatcherWindows
#define GetClassNameW FixtureWindowClass
#define GetWindowThreadProcessId FixtureWindowProcess
#define OpenProcess FixtureOpenProcess
#define WaitForMultipleObjects FixtureWaitForWatchers
#define CreateMutexW FixtureRegistrationMutex
#define FindFirstChangeNotificationW FixtureWatchFolder
#define FindNextChangeNotification FixtureRearmFolderWatch
#define FindCloseChangeNotification FixtureCloseFolderWatch
#define SetWinEventHook FixtureSetCreationHook
#define UnhookWinEvent FixtureRemoveCreationHook
#define FindWindowW FixtureFindWatcher
#define RegOpenKeyExW FixturePackageListOpen
#define RegNotifyChangeKeyValue FixturePackageListNotify
#define RegCloseKey FixturePackageListClose
#define Profiles_Load FixtureWaitProfiles
#define Claude_ClosedForUpdate FixtureClosedForUpdate
#define Claude_FindPackage FixtureInstalledPackage
#define Claude_Launch FixtureReopenLaunch
#define Util_Log FixtureLog
#define Util_Spawn FixtureRefuseSpawn
#define Icons_Ensure FixtureNoIcon
#define Tray_Apply FixtureNoTrayChange
#define Tray_GiveBack FixtureNoTrayGiveBack
#define TaskbarPin_TellShortcutChanged FixtureNoShortcutNotice
#define Taskbar_IsWatched TestedTaskbar_IsWatched
#define Taskbar_Watch TestedTaskbar_Watch
#define Taskbar_Refresh TestedTaskbar_Refresh
#define Taskbar_StopWatchers TestedTaskbar_StopWatchers
#define Taskbar_WatchRun TestedTaskbar_WatchRun
/* The program's taskbar object defines these SDK constants once. */
#define __UIA_OtherConstants_MODULE_DEFINED__
#define __UIA_PatternIds_MODULE_DEFINED__
#define __UIA_EventIds_MODULE_DEFINED__
#define __UIA_PropertyIds_MODULE_DEFINED__
#define __UIA_TextAttributeIds_MODULE_DEFINED__
#define __UIA_ControlTypeIds_MODULE_DEFINED__
#define __UIA_AnnotationTypes_MODULE_DEFINED__
#define __UIA_StyleIds_MODULE_DEFINED__
#define __UIA_LandmarkTypeIds_MODULE_DEFINED__
#define __UIA_HeadingLevelIds_MODULE_DEFINED__
#define __UIA_ChangeIds_MODULE_DEFINED__
#define __UIA_MetadataIds_MODULE_DEFINED__
#include "../src/taskbar.c"
#undef OpenEventW
#undef CreateEventW
#undef OpenMutexW
#undef EnumWindows
#undef GetClassNameW
#undef GetWindowThreadProcessId
#undef OpenProcess
#undef WaitForMultipleObjects
#undef CreateMutexW
#undef FindFirstChangeNotificationW
#undef FindNextChangeNotification
#undef FindCloseChangeNotification
#undef SetWinEventHook
#undef UnhookWinEvent
#undef FindWindowW
#undef RegOpenKeyExW
#undef RegNotifyChangeKeyValue
#undef RegCloseKey
#undef Profiles_Load
#undef Claude_ClosedForUpdate
#undef Claude_FindPackage
#undef Claude_Launch
#undef Util_Log
#undef Util_Spawn
#undef Icons_Ensure
#undef Tray_Apply
#undef Tray_GiveBack
#undef TaskbarPin_TellShortcutChanged
#undef Taskbar_IsWatched
#undef Taskbar_Watch
#undef Taskbar_Refresh
#undef Taskbar_StopWatchers
#undef Taskbar_WatchRun

#define BUSY_WATCHER_MS 200   /* how long a stopped watcher stays busy giving its windows back */
#define LAUNCHED_CLAUDE_PID  4321
#define LONG_WAIT_MS         20000   /* a wait each scenario must end long before */
#define QUICK_END_MS         3000
#define SHORT_WAIT_MS        300
#define STAND_IN_LIFETIME_MS 20000   /* a stand-in Claude's window lives at most this long; its scenario releases it */

static HANDLE g_busyWatcherRelease;
static volatile LONG g_busyWatcherSawStop, g_busyWatcherFinished, g_stopAssertedAtWatcherExit;
static BOOL g_concurrentStopResult;

/* A thread stands in for a watcher process: its handle is signaled when it
 * exits, as a process's is. Told to stop, it stays busy for a moment. */
static DWORD WINAPI BusyWatcher(void *context)
{
    HANDLE signals[2];
    (void)context;
    signals[0] = g_watcherStopEvent;
    signals[1] = g_busyWatcherRelease;
    if (WaitForMultipleObjects(ARRAYSIZE(signals), signals, FALSE, FIXTURE_WAIT_MS) == WAIT_OBJECT_0)
        InterlockedExchange(&g_busyWatcherSawStop, TRUE);
    WaitForSingleObject(g_busyWatcherRelease, BUSY_WATCHER_MS);
    InterlockedExchange(&g_stopAssertedAtWatcherExit, WaitForSingleObject(g_watcherStopEvent, 0) == WAIT_OBJECT_0);
    InterlockedExchange(&g_busyWatcherFinished, TRUE);
    return 0;
}

static BOOL StartBusyWatcher(void)
{
    g_watcherStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_busyWatcherRelease = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_busyWatcherSawStop = g_busyWatcherFinished = g_stopAssertedAtWatcherExit = FALSE;
    if (g_watcherStopEvent && g_busyWatcherRelease) g_processStandIn = CreateThread(NULL, 0, BusyWatcher, NULL, 0, NULL);
    return g_processStandIn != NULL;
}

static void EndBusyWatcher(void)
{
    if (g_busyWatcherRelease) SetEvent(g_busyWatcherRelease);
    if (g_processStandIn) {
        WaitForSingleObject(g_processStandIn, FIXTURE_WAIT_MS);
        CloseHandle(g_processStandIn);
    }
    if (g_watcherStopEvent) CloseHandle(g_watcherStopEvent);
    if (g_busyWatcherRelease) CloseHandle(g_busyWatcherRelease);
    g_processStandIn = g_watcherStopEvent = g_busyWatcherRelease = NULL;
}

static DWORD WINAPI ConcurrentStop(void *context)
{
    (void)context;
    g_concurrentStopResult = TestedTaskbar_StopWatchers(FALSE);
    return 0;
}

static BOOL StopSignalAsserted(void)
{
    return WaitForSingleObject(g_watcherStopEvent, 0) == WAIT_OBJECT_0;
}

/* A watcher registers its stop events and window under the lock: a stop that
 * comes meanwhile must find them once the registration is over. */
static void CheckStopDuringRegistration(void)
{
    HANDLE concurrentStop;
    if (!Prepared("start a watcher stand-in being registered", StartBusyWatcher())) {
        EndBusyWatcher();
        return;
    }
    g_stopReachedRegistration = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Prepared("create the private registration signal", g_stopReachedRegistration != NULL) &&
        Prepared("hold the private registration mutex", WaitForSingleObject(g_registrationMutex, FIXTURE_WAIT_MS) == WAIT_OBJECT_0)) {
        g_watcherBeingRegistered = TRUE;
        g_stopEventLookedForDuringRegistration = FALSE;
        concurrentStop = CreateThread(NULL, 0, ConcurrentStop, NULL, 0, NULL);
        if (Prepared("start a concurrent stop", concurrentStop != NULL)) {
            Check("a stop reaches the registration lock",
                  WaitForSingleObject(g_stopReachedRegistration, FIXTURE_WAIT_MS) == WAIT_OBJECT_0);
            /* A stop that did not wait would be over long before SHORT_WAIT_MS. */
            Check("a stop waits until the watcher's registration is complete, looking for nothing meanwhile",
                  WaitForSingleObject(concurrentStop, SHORT_WAIT_MS) == WAIT_TIMEOUT && !g_stopEventLookedForDuringRegistration);
            g_watcherBeingRegistered = FALSE;
            ReleaseMutex(g_registrationMutex);
            Check("a stop completes once the registration is",
                  WaitForSingleObject(concurrentStop, FIXTURE_WAIT_MS) == WAIT_OBJECT_0 && g_concurrentStopResult);
            Check("the watcher registered meanwhile receives the stop signal", g_busyWatcherSawStop);
            CloseHandle(concurrentStop);
        } else {
            g_watcherBeingRegistered = FALSE;
            ReleaseMutex(g_registrationMutex);
        }
    }
    if (g_stopReachedRegistration) CloseHandle(g_stopReachedRegistration);
    g_stopReachedRegistration = NULL;
    EndBusyWatcher();
}

static void CheckWatcherStops(void)
{
    g_registrationMutex = CreateMutexW(NULL, FALSE, NULL);
    if (!Prepared("create the private registration mutex", g_registrationMutex != NULL)) return;
    Check("stopping watchers succeeds when no watcher ever ran", TestedTaskbar_StopWatchers(FALSE));

    if (Prepared("start a busy watcher stand-in", StartBusyWatcher())) {
        Check("a stop waits for a busy watcher to exit", TestedTaskbar_StopWatchers(FALSE) && g_busyWatcherFinished);
        Check("a busy watcher receives the stop signal", g_busyWatcherSawStop);
        Check("the stop signal is still asserted when the watcher exits, and reset after",
              g_stopAssertedAtWatcherExit && !StopSignalAsserted());
    }
    EndBusyWatcher();

    if (Prepared("start a watcher stand-in that does not exit in time", StartBusyWatcher())) {
        g_watcherWaitTimesOut = TRUE;
        Check("a watcher that does not exit in time fails the stop", !TestedTaskbar_StopWatchers(TRUE));
        Check("a stop that was not acknowledged is withdrawn", !StopSignalAsserted());
        g_watcherWaitTimesOut = FALSE;
    }
    EndBusyWatcher();

    if (Prepared("start a watcher stand-in whose process cannot be opened", StartBusyWatcher())) {
        g_watcherOpenError = ERROR_ACCESS_DENIED;
        Check("a watcher process that cannot be opened fails the stop", !TestedTaskbar_StopWatchers(FALSE));
        Check("the stop of a watcher that cannot be opened is withdrawn", !StopSignalAsserted());
        g_watcherOpenError = 0;
    }
    EndBusyWatcher();

    g_watcherStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Prepared("create a private stop event", g_watcherStopEvent != NULL)) {
        g_noWatcherWindows = TRUE;
        Check("a stop with no watcher window to wait for succeeds and is reset",
              TestedTaskbar_StopWatchers(FALSE) && !StopSignalAsserted());
        Check("a stop for an install asks the watchers to hand over", wcscmp(g_openedStopEvent, WATCH_HANDOVER_EVENT) == 0);
        g_noWatcherWindows = FALSE;
        g_watcherOpenError = ERROR_INVALID_PARAMETER;
        Check("a watcher that exits before its process is opened counts as stopped",
              TestedTaskbar_StopWatchers(FALSE) && !StopSignalAsserted());
        g_watcherOpenError = 0;
        g_registrationLockFails = TRUE;
        Check("a registration lock that cannot be taken fails the stop without asserting it",
              !TestedTaskbar_StopWatchers(FALSE) && !StopSignalAsserted());
        g_registrationLockFails = FALSE;
        g_noWatcherWindows = TRUE;
        Check("an uninstall's stop stays asserted, so that no watcher starts after it",
              TestedTaskbar_StopWatchers(TRUE) && StopSignalAsserted() && g_uninstalledQuit != NULL);
        Check("a stop for an uninstall asks the watchers to give the windows back", wcscmp(g_openedStopEvent, WATCH_QUIT_EVENT) == 0);
        g_noWatcherWindows = FALSE;
        if (g_uninstalledQuit) CloseHandle(g_uninstalledQuit);
        g_uninstalledQuit = NULL;
        CloseHandle(g_watcherStopEvent);
    }
    g_watcherStopEvent = NULL;

    if (Prepared("start a watcher stand-in with two windows", StartBusyWatcher())) {
        g_watcherHasTwoWindows = TRUE;
        g_watcherOpens = 0;
        SetEvent(g_busyWatcherRelease);
        Check("two windows of one watcher are one process to wait for", TestedTaskbar_StopWatchers(FALSE) && g_watcherOpens == 1);
        g_watcherHasTwoWindows = FALSE;
    }
    EndBusyWatcher();

    CheckStopDuringRegistration();
    CloseHandle(g_registrationMutex);
    g_registrationMutex = NULL;
}

/* C89's static assertion: the window must come before the wait looks again. */
typedef char WindowComesBeforeTheRecheck[WINDOW_AFTER_CHANGE_MS < START_RECHECK_MS ? 1 : -1];

typedef enum ClaudeStart {
    START_AFTER_FOLDER_CHANGE,   /* its lock file in the watched folder, its window, then more writes there */
    START_REPORTED_BY_HOOK,      /* its window, as the creation hook reports it */
    START_FOLDER_THEN_HOOK,      /* its lock file, then its window, which only a creation hook set after can report */
    START_LOCK_FILE_ONLY         /* its lock file; its window comes on the waiting thread (g_windowComesAfterChange) */
} ClaudeStart;

static HANDLE g_waitQuit, g_waitHandover, g_standInRelease;
static ClaudeStart g_claudeStart;
static WCHAR g_lockFile[MAX_PATH];

/* Stands in for a starting Claude of the fixture profile. Its window answers
 * the title requests of Claude_IsRunning until it is released. */
static DWORD WINAPI StartingClaude(void *context)
{
    HWND window;
    MSG message;
    ULONGLONG deadline;
    (void)context;
    if (g_claudeStart != START_REPORTED_BY_HOOK &&
        (WaitForSingleObject(g_folderWatchArmed, FIXTURE_WAIT_MS) != WAIT_OBJECT_0 || !WriteFixtureFile(g_lockFile, "")))
        return 1;
    if (g_claudeStart == START_LOCK_FILE_ONLY) return 0;
    if (g_claudeStart != START_AFTER_FOLDER_CHANGE && WaitForSingleObject(g_creationHookSet, FIXTURE_WAIT_MS) != WAIT_OBJECT_0)
        return 1;
    window = CreateWindowExW(0, L"Chrome_MessageWindow", g_waitDataDir, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                             GetModuleHandleW(NULL), NULL);
    if (!window) return 1;
    /* A starting Claude keeps writing in its folder after its window shows:
     * however late this thread runs, a change after the window comes. */
    if (g_claudeStart == START_AFTER_FOLDER_CHANGE) WriteFixtureFile(g_lockFile, "started");
    else MessageWindowCreated(NULL, EVENT_OBJECT_CREATE, window, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    deadline = GetTickCount64() + STAND_IN_LIFETIME_MS;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline ||
            MsgWaitForMultipleObjects(1, &g_standInRelease, FALSE, (DWORD)(deadline - now), QS_ALLINPUT) != WAIT_OBJECT_0 + 1)
            break;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    }
    DestroyWindow(window);
    return 0;
}

static HANDLE StartClaudeStandIn(ClaudeStart start)
{
    g_claudeStart = start;
    ResetEvent(g_standInRelease);
    return CreateThread(NULL, 0, StartingClaude, NULL, 0, NULL);
}

/* Its window goes, and its lock file, so that the next start is a new change. */
static void EndClaudeStandIn(HANDLE standIn)
{
    SetEvent(g_standInRelease);
    WaitForSingleObject(standIn, FIXTURE_WAIT_MS);
    CloseHandle(standIn);
    DeleteFileW(g_lockFile);
}

static DWORD WINAPI SignalOnceFolderWatched(void *event)
{
    if (WaitForSingleObject(g_folderWatchArmed, FIXTURE_WAIT_MS) == WAIT_OBJECT_0) SetEvent((HANDLE)event);
    return 0;
}

static void ResetWaitRecords(void)
{
    g_folderWatches = g_folderRearms = g_folderWatchCloses = g_creationHooks = g_creationHookRemovals = 0;
    g_watchedFolder[0] = 0;
    g_hookedProcess = g_hookFlags = g_watchedChanges = 0;
    ResetEvent(g_folderWatchArmed);
    ResetEvent(g_creationHookSet);
}

static int WaitForFixtureProfile(DWORD pid, DWORD ms, ProfileList *list, BOOL *interrupted, ULONGLONG *elapsed)
{
    ULONGLONG start = GetTickCount64();
    int found = WaitForProfile(WAIT_FIXTURE_FOLDER, pid, list, g_waitQuit, g_waitHandover, ms, interrupted);
    *elapsed = GetTickCount64() - start;
    return found;
}

static void CheckWaitEndsAtOnce(void)
{
    ProfileList list;
    ULONGLONG elapsed;
    BOOL interrupted = TRUE;
    HANDLE exitedClaude;
    ResetWaitRecords();
    g_waitProfileGone = TRUE;
    Check("a profile that is gone ends the wait at once",
          WaitForFixtureProfile(0, LONG_WAIT_MS, &list, &interrupted, &elapsed) == -1 && !interrupted && g_folderWatches == 0);
    g_waitProfileGone = FALSE;
    Check("a zero wait only looks", WaitForFixtureProfile(0, 0, &list, &interrupted, &elapsed) == -1 && !interrupted &&
                                    g_folderWatches == 0 && g_creationHooks == 0);
    g_watcherOpenError = ERROR_INVALID_PARAMETER;
    Check("a launched Claude already gone ends the wait at once",
          WaitForFixtureProfile(LAUNCHED_CLAUDE_PID, LONG_WAIT_MS, &list, &interrupted, &elapsed) == -1 && !interrupted &&
          elapsed < QUICK_END_MS && g_creationHooks == 0 && g_folderWatches == 0);
    g_watcherOpenError = 0;

    exitedClaude = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!Prepared("make a stand-in for a Claude that has exited", exitedClaude != NULL)) return;
    g_processStandIn = exitedClaude;
    ResetWaitRecords();
    Check("a launched Claude that exits ends the wait",
          WaitForFixtureProfile(LAUNCHED_CLAUDE_PID, LONG_WAIT_MS, &list, &interrupted, &elapsed) == -1 && !interrupted &&
          elapsed < QUICK_END_MS);
    Check("with a pid, only that process's window creations are watched",
          g_creationHooks == 1 && g_hookedWindowCreations && g_hookedProcess == LAUNCHED_CLAUDE_PID &&
          (g_hookFlags & WINEVENT_SKIPOWNPROCESS) && g_folderWatches == 0);
    Check("the creation hook is removed when the wait ends", g_creationHookRemovals == 1);
    g_processStandIn = NULL;
    CloseHandle(exitedClaude);
}

static void CheckWaitForLaunchedClaude(void)
{
    ProfileList list;
    ULONGLONG elapsed;
    BOOL interrupted = TRUE;
    HANDLE runningClaude = CreateEventW(NULL, TRUE, FALSE, NULL), standIn;
    int found;
    if (!Prepared("make a stand-in for a running Claude process", runningClaude != NULL)) return;
    g_processStandIn = runningClaude;
    ResetWaitRecords();
    standIn = StartClaudeStandIn(START_REPORTED_BY_HOOK);
    if (Prepared("start a Claude stand-in reported by the hook", standIn != NULL)) {
        found = WaitForFixtureProfile(LAUNCHED_CLAUDE_PID, LONG_WAIT_MS, &list, &interrupted, &elapsed);
        Check("with a pid, the window that process creates ends the wait with the profile",
              found == 0 && list.items[0].running && !interrupted && elapsed < QUICK_END_MS);
        EndClaudeStandIn(standIn);
    }
    ResetWaitRecords();
    g_creationHookFails = TRUE;
    standIn = StartClaudeStandIn(START_AFTER_FOLDER_CHANGE);
    if (Prepared("start a Claude stand-in that writes its lock file first", standIn != NULL)) {
        found = WaitForFixtureProfile(LAUNCHED_CLAUDE_PID, LONG_WAIT_MS, &list, &interrupted, &elapsed);
        Check("a launched Claude whose window creations cannot be hooked is waited for through the profile's folder",
              found == 0 && !interrupted && g_creationHooks == 1 && g_creationHookRemovals == 0 && g_folderWatches == 1 &&
              elapsed < QUICK_END_MS);
        EndClaudeStandIn(standIn);
    }
    g_creationHookFails = FALSE;
    g_processStandIn = NULL;
    CloseHandle(runningClaude);
    ResetWaitRecords();
    g_watcherOpenError = ERROR_ACCESS_DENIED;
    standIn = StartClaudeStandIn(START_AFTER_FOLDER_CHANGE);
    if (Prepared("start a Claude stand-in that writes its lock file first", standIn != NULL)) {
        found = WaitForFixtureProfile(LAUNCHED_CLAUDE_PID, LONG_WAIT_MS, &list, &interrupted, &elapsed);
        Check("a launched Claude that cannot be opened is waited for through the profile's folder",
              found == 0 && !interrupted && g_creationHooks == 0 && g_folderWatches == 1 && elapsed < QUICK_END_MS);
        EndClaudeStandIn(standIn);
    }
    g_watcherOpenError = 0;
}

static void CheckWaitForFolderChange(void)
{
    ProfileList list;
    ULONGLONG elapsed;
    BOOL interrupted = TRUE;
    HANDLE standIn;
    int found;
    ResetWaitRecords();
    standIn = StartClaudeStandIn(START_AFTER_FOLDER_CHANGE);
    if (!Prepared("start a Claude stand-in that writes its lock file first", standIn != NULL)) return;
    found = WaitForFixtureProfile(0, LONG_WAIT_MS, &list, &interrupted, &elapsed);
    Check("without a pid, the profile's storage folder is watched with what it holds",
          g_folderWatches == 1 && Core_PathEquals(g_watchedFolder, g_waitStorageDir) && g_watchedSubtree &&
          g_watchedChanges == (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE) && g_creationHooks == 0);
    Check("a lock file, then a window titled with the data folder, end the wait with the profile",
          found == 0 && list.items[0].running && !interrupted && elapsed < QUICK_END_MS);
    Check("the folder is watched again after each change", g_folderRearms >= 1);
    Check("the folder watch is closed when the wait ends", g_folderWatchCloses == 1);
    ResetWaitRecords();
    found = WaitForFixtureProfile(0, LONG_WAIT_MS, &list, &interrupted, &elapsed);
    Check("a profile already running is found without watching anything",
          found == 0 && !interrupted && g_folderWatches == 0 && g_creationHooks == 0);
    EndClaudeStandIn(standIn);

    ResetWaitRecords();
    g_windowComesAfterChange = TRUE;
    standIn = StartClaudeStandIn(START_LOCK_FILE_ONLY);
    if (Prepared("start a Claude stand-in that only writes its lock file", standIn != NULL)) {
        found = WaitForFixtureProfile(0, LONG_WAIT_MS, &list, &interrupted, &elapsed);
        Check("a window that shows a moment after the folder changed, with no change after it, is found by one more look",
              found == 0 && !interrupted && g_windowShownAfterChange != NULL && elapsed < QUICK_END_MS);
        EndClaudeStandIn(standIn);
    }
    g_windowComesAfterChange = FALSE;
    if (g_windowAfterChangeTimer) KillTimer(NULL, g_windowAfterChangeTimer);
    g_windowAfterChangeTimer = 0;
    if (g_windowShownAfterChange) DestroyWindow(g_windowShownAfterChange);
    g_windowShownAfterChange = NULL;

    ResetWaitRecords();
    g_folderRearmFails = TRUE;
    standIn = StartClaudeStandIn(START_FOLDER_THEN_HOOK);
    if (Prepared("start a Claude stand-in that writes its lock file, then is reported by the hook", standIn != NULL)) {
        found = WaitForFixtureProfile(0, LONG_WAIT_MS, &list, &interrupted, &elapsed);
        Check("a folder that can no longer be watched leaves the wait to every process's window creations",
              found == 0 && !interrupted && g_folderWatches == 1 && g_folderWatchCloses == 1 && g_creationHooks == 1 &&
              g_hookedProcess == 0 && g_creationHookRemovals == 1 && elapsed < QUICK_END_MS);
        EndClaudeStandIn(standIn);
    }
    g_folderRearmFails = FALSE;
}

static void CheckWaitStops(void)
{
    static const char *const kStopNames[] = { "quit", "handover" };
    HANDLE stops[2], signaller, unwaitableQuit;
    ProfileList list;
    ULONGLONG elapsed, start;
    BOOL interrupted;
    size_t i;
    int found;
    stops[0] = g_waitQuit;
    stops[1] = g_waitHandover;
    for (i = 0; i < ARRAYSIZE(stops); i++) {
        ResetWaitRecords();
        interrupted = FALSE;
        signaller = CreateThread(NULL, 0, SignalOnceFolderWatched, stops[i], 0, NULL);
        if (!Prepared("start a stop request", signaller != NULL)) return;
        found = WaitForFixtureProfile(0, LONG_WAIT_MS, &list, &interrupted, &elapsed);
        CheckFor("a stop request ends the wait at once, as interrupted", kStopNames[i],
                 found == -1 && interrupted && elapsed < QUICK_END_MS);
        WaitForSingleObject(signaller, FIXTURE_WAIT_MS);
        CloseHandle(signaller);
        ResetEvent(stops[i]);
    }

    /* A handle the wait has no right to wait on makes it fail. */
    if (Prepared("make a quit handle that cannot be waited on",
                 DuplicateHandle(GetCurrentProcess(), g_waitQuit, GetCurrentProcess(), &unwaitableQuit, EVENT_MODIFY_STATE, FALSE, 0))) {
        ResetWaitRecords();
        interrupted = FALSE;
        start = GetTickCount64();
        found = WaitForProfile(WAIT_FIXTURE_FOLDER, 0, &list, unwaitableQuit, g_waitHandover, LONG_WAIT_MS, &interrupted);
        Check("a wait that fails ends at once, as interrupted", found == -1 && interrupted && GetTickCount64() - start < QUICK_END_MS);
        CloseHandle(unwaitableQuit);
    }

    if (!Prepared("make a storage path that does not exist",
                  JoinPath(g_root, L"missing-storage", g_waitStorageDir, ARRAYSIZE(g_waitStorageDir))))
        return;
    ResetWaitRecords();
    interrupted = TRUE;
    found = WaitForFixtureProfile(0, SHORT_WAIT_MS, &list, &interrupted, &elapsed);
    Check("a profile folder that cannot be watched falls back to every process's window creations",
          g_creationHooks == 1 && g_hookedWindowCreations && g_hookedProcess == 0 && g_creationHookRemovals == 1);
    Check("without a folder to watch, the wait still lasts until its deadline",
          found == -1 && !interrupted && elapsed >= SHORT_WAIT_MS);
    StringCchCopyW(g_waitStorageDir, ARRAYSIZE(g_waitStorageDir), g_waitDataDir);
}

/* Only a Chrome_MessageWindow titled with the profile's data folder is its Claude. */
static void CheckStartedWindowTitle(void)
{
    WCHAR otherFolder[MAX_PATH];
    HWND window;
    if (!Prepared("make another data folder path", JoinPath(g_root, L"Claude-Other", otherFolder, ARRAYSIZE(otherFolder))))
        return;
    window = CreateWindowExW(0, L"Chrome_MessageWindow", otherFolder, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!Prepared("create a Chrome_MessageWindow of another profile", window != NULL)) return;
    StringCchCopyW(g_startedDir, ARRAYSIZE(g_startedDir), g_waitDataDir);
    ResetEvent(g_started);
    MessageWindowCreated(NULL, EVENT_OBJECT_CREATE, window, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    Check("the window of another profile's Claude does not end the wait", WaitForSingleObject(g_started, 0) == WAIT_TIMEOUT);
    SetWindowTextW(window, g_waitDataDir);
    MessageWindowCreated(NULL, EVENT_OBJECT_CREATE, window, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    Check("the window titled with the profile's data folder ends the wait", WaitForSingleObject(g_started, 0) == WAIT_OBJECT_0);
    DestroyWindow(window);
}

static void ResetReopenRecords(int looksBeforeUpdate)
{
    g_closedForUpdate = TRUE;
    g_packageLooks = 0;
    g_packageLooksBeforeUpdate = looksBeforeUpdate;
    g_packageListUnavailable = g_packageListChangesAtOnce = g_appsFolderChangesBeforeUpdate = FALSE;
    g_packageListOpenKeys = g_packageListArms = g_reopenLaunches = 0;
    g_reopenLaunchResult = S_OK;
    ZeroMemory(&g_watchPkg, sizeof g_watchPkg);
    ResetEvent(g_reopened);
    g_reopenedPid = 0;
}

static BOOL ReopenFixtureProfile(DWORD *pid)
{
    *pid = 0;
    return ReopenAfterUpdate(g_waitProfileIsStock ? STOCK_FOLDER : WAIT_FIXTURE_FOLDER, RAN_PACKAGE, g_waitQuit, g_waitHandover, pid);
}

/* After a Claude update the watcher opens the profile again once the new
 * package is installed, unless something else does it first. */
static void CheckReopenAfterUpdate(void)
{
    HANDLE liveClaude, standIn;
    ULONGLONG start;
    DWORD pid;
    ResetReopenRecords(0);
    g_closedForUpdate = FALSE;
    Check("a Claude that closed for no update is not opened again", !ReopenFixtureProfile(&pid) && g_reopenLaunches == 0);
    ResetReopenRecords(0);
    Check("once the new package is installed, the profile is opened again with it",
          ReopenFixtureProfile(&pid) && g_reopenLaunches == 1 && pid == REOPENED_CLAUDE_PID &&
          wcscmp(g_watchPkg.fullName, UPDATED_PACKAGE) == 0);
    Check("the package list's key is closed after the wait", g_packageListOpenKeys == 0);
    ResetReopenRecords(1);
    g_packageListChangesAtOnce = TRUE;
    Check("a change to the package list brings a new look, the list being watched again",
          ReopenFixtureProfile(&pid) && g_reopenLaunches == 1 && g_packageListArms == 2 && g_packageListOpenKeys == 0);
    ResetReopenRecords(1);
    g_packageListUnavailable = TRUE;
    g_appsFolderChangesBeforeUpdate = TRUE;
    Check("without the package list, a change to the Apps folder brings a new look",
          ReopenFixtureProfile(&pid) && g_reopenLaunches == 1 && g_packageListArms == 0);
    ResetReopenRecords(0);
    g_reopenLaunchResult = E_ACCESSDENIED;
    Check("a profile that cannot be opened again ends the watcher", !ReopenFixtureProfile(&pid) && g_reopenLaunches == 1);
    ResetReopenRecords(1000);
    SetEvent(g_waitQuit);
    start = GetTickCount64();
    Check("a stop request during the wait for the new package ends it at once, opening nothing",
          !ReopenFixtureProfile(&pid) && g_reopenLaunches == 0 && g_packageListOpenKeys == 0 && GetTickCount64() - start < QUICK_END_MS);
    ResetEvent(g_waitQuit);

    liveClaude = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Prepared("make a stand-in for the Claude the user just started", liveClaude != NULL)) {
        ResetReopenRecords(1000);
        g_processStandIn = liveClaude;
        g_reopenedPid = LAUNCHED_CLAUDE_PID;
        SetEvent(g_reopened);
        start = GetTickCount64();
        Check("a profile the user opens again during the update is left at once to that start",
              !ReopenFixtureProfile(&pid) && g_reopenLaunches == 0 && GetTickCount64() - start < QUICK_END_MS);
        g_processStandIn = NULL;
        CloseHandle(liveClaude);
    }
    ResetReopenRecords(1);
    g_watcherOpenError = ERROR_INVALID_PARAMETER;
    g_reopenedPid = LAUNCHED_CLAUDE_PID;
    SetEvent(g_reopened);
    Check("a start of the profile that ended at once does not stop its reopening",
          ReopenFixtureProfile(&pid) && g_reopenLaunches == 1);
    g_watcherOpenError = 0;

    ResetReopenRecords(0);
    ResetWaitRecords();
    g_waitProfileIsStock = TRUE;
    standIn = StartClaudeStandIn(START_AFTER_FOLDER_CHANGE);
    if (Prepared("start a stand-in for the Claude Windows opens again", standIn != NULL)) {
        Check("the stock profile, which Windows opens again by itself, is waited for, not opened",
              ReopenFixtureProfile(&pid) && g_reopenLaunches == 0 && pid == 0);
        EndClaudeStandIn(standIn);
    }
    g_waitProfileIsStock = FALSE;
}

/* A start of a profile that already has a watcher goes to it: the watcher,
 * about to end, takes it under the registration lock. */
static void CheckStartHandover(void)
{
    WNDCLASSEXW watcherClass;
    HANDLE liveClaude;
    ATOM registered;
    DWORD pid = 0;
    ZeroMemory(&watcherClass, sizeof watcherClass);
    watcherClass.cbSize = sizeof watcherClass;
    watcherClass.lpfnWndProc = WatchProc;
    watcherClass.hInstance = GetModuleHandleW(NULL);
    watcherClass.lpszClassName = L"PlatformTestWatcher";
    registered = RegisterClassExW(&watcherClass);
    if (!Prepared("register a private class for a stand-in watcher's window", registered != 0)) return;
    g_watchWindow = CreateWindowExW(0, L"PlatformTestWatcher", WAIT_FIXTURE_FOLDER, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                    GetModuleHandleW(NULL), NULL);
    liveClaude = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Prepared("create the stand-in watcher's window", g_watchWindow != NULL) &&
        Prepared("make a stand-in for the Claude started for the profile", liveClaude != NULL)) {
        ResetEvent(g_reopened);
        g_reopenedPid = 0;
        g_handoverWatcher = g_watchWindow;
        g_processStandIn = liveClaude;
        Check("a start of a profile whose watcher has a window goes to that watcher",
              HandStartToWatcher(WAIT_FIXTURE_FOLDER, LAUNCHED_CLAUDE_PID));
        Check("the watcher, about to end, takes the start with the Claude started for it",
              TakeReopenedClaude(&pid) && pid == LAUNCHED_CLAUDE_PID);
        Check("a start handed over is taken once", !TakeReopenedClaude(&pid));
        g_processStandIn = NULL;
        Check("a start handed over is posted again", HandStartToWatcher(WAIT_FIXTURE_FOLDER, LAUNCHED_CLAUDE_PID));
        Check("a start whose Claude has already ended lets the watcher end",
              !TakeReopenedClaude(&pid));
        g_handoverWatcher = NULL;
        Check("with no watcher window, the start is not handed over: the new watcher watches the profile",
              !HandStartToWatcher(WAIT_FIXTURE_FOLDER, LAUNCHED_CLAUDE_PID));
    }
    if (g_watchWindow) DestroyWindow(g_watchWindow);
    if (liveClaude) CloseHandle(liveClaude);
    g_watchWindow = NULL;
    UnregisterClassW(L"PlatformTestWatcher", GetModuleHandleW(NULL));
}

static HANDLE g_lockHeldByStop, g_lockRelease;

/* A stop holds the registration lock while it waits for the watchers. */
static DWORD WINAPI StopHoldingRegistration(void *context)
{
    (void)context;
    if (WaitForSingleObject(g_registrationMutex, FIXTURE_WAIT_MS) != WAIT_OBJECT_0) return 1;
    SetEvent(g_waitHandover);
    SetEvent(g_lockHeldByStop);
    WaitForSingleObject(g_lockRelease, FIXTURE_WAIT_MS);
    ReleaseMutex(g_registrationMutex);
    return 0;
}

/* A watcher ends under the registration lock, unless a stop, which holds it,
 * asks it to end at once. */
static void CheckRegistrationToEnd(void)
{
    HANDLE lock, stop;
    ULONGLONG start;
    g_registrationMutex = CreateMutexW(NULL, FALSE, NULL);
    g_lockHeldByStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lockRelease = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Prepared("create the private registration mutex and its signals", g_registrationMutex && g_lockHeldByStop && g_lockRelease)) {
        lock = LockRegistrationToEnd(g_waitQuit, g_waitHandover);
        Check("a watcher about to end takes the registration lock", lock != NULL);
        UnlockWatcherRegistration(lock);
        stop = CreateThread(NULL, 0, StopHoldingRegistration, NULL, 0, NULL);
        if (Prepared("start a stop that holds the registration lock",
                     stop != NULL && WaitForSingleObject(g_lockHeldByStop, FIXTURE_WAIT_MS) == WAIT_OBJECT_0)) {
            start = GetTickCount64();
            lock = LockRegistrationToEnd(g_waitQuit, g_waitHandover);
            Check("a watcher about to end while a stop holds the lock ends at once, without it",
                  lock == NULL && GetTickCount64() - start < QUICK_END_MS);
            UnlockWatcherRegistration(lock);
        }
        SetEvent(g_lockRelease);
        if (stop) {
            WaitForSingleObject(stop, FIXTURE_WAIT_MS);
            CloseHandle(stop);
        }
        ResetEvent(g_waitHandover);
        g_registrationLockFails = TRUE;
        Check("a registration lock that cannot be had ends the watcher without it",
              LockRegistrationToEnd(g_waitQuit, g_waitHandover) == NULL);
        g_registrationLockFails = FALSE;
    }
    if (g_lockHeldByStop) CloseHandle(g_lockHeldByStop);
    if (g_lockRelease) CloseHandle(g_lockRelease);
    if (g_registrationMutex) CloseHandle(g_registrationMutex);
    g_lockHeldByStop = g_lockRelease = g_registrationMutex = NULL;
}

/* The life of a watcher after its start: the wait for the profile's Claude,
 * its reopening after an update, a start handed to it, its end. */
static void CheckWatcherLife(void)
{
    WNDCLASSEXW messageWindowClass;
    ATOM registered;
    if (!Prepared("make the private profile folder",
                  JoinPath(g_root, WAIT_FIXTURE_FOLDER, g_waitDataDir, ARRAYSIZE(g_waitDataDir)) &&
                  SUCCEEDED(StringCchCopyW(g_waitStorageDir, ARRAYSIZE(g_waitStorageDir), g_waitDataDir)) &&
                  JoinPath(g_waitStorageDir, L"lockfile", g_lockFile, ARRAYSIZE(g_lockFile)) && CreateDirectoryW(g_waitDataDir, NULL)))
        return;
    ZeroMemory(&messageWindowClass, sizeof messageWindowClass);
    messageWindowClass.cbSize = sizeof messageWindowClass;
    messageWindowClass.lpfnWndProc = DefWindowProcW;
    messageWindowClass.hInstance = GetModuleHandleW(NULL);
    messageWindowClass.lpszClassName = L"Chrome_MessageWindow";
    registered = RegisterClassExW(&messageWindowClass);
    g_waitQuit = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_waitHandover = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_started = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_appsChanged = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_appsFolderChange = g_appsChanged;
    g_reopened = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_folderWatchArmed = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_creationHookSet = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_standInRelease = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Prepared("register a private Chrome_MessageWindow class", registered != 0) &&
        Prepared("create the private wait events", g_waitQuit && g_waitHandover && g_started && g_appsChanged && g_reopened &&
                                                   g_folderWatchArmed && g_creationHookSet && g_standInRelease)) {
        CheckWaitEndsAtOnce();
        CheckWaitForLaunchedClaude();
        CheckWaitForFolderChange();
        CheckWaitStops();
        CheckStartedWindowTitle();
        CheckReopenAfterUpdate();
        CheckStartHandover();
        CheckRegistrationToEnd();
    }
    if (g_waitQuit) CloseHandle(g_waitQuit);
    if (g_waitHandover) CloseHandle(g_waitHandover);
    if (g_started) CloseHandle(g_started);
    if (g_appsChanged) CloseHandle(g_appsChanged);
    if (g_reopened) CloseHandle(g_reopened);
    if (g_folderWatchArmed) CloseHandle(g_folderWatchArmed);
    if (g_creationHookSet) CloseHandle(g_creationHookSet);
    if (g_standInRelease) CloseHandle(g_standInRelease);
    g_waitQuit = g_waitHandover = g_started = g_appsChanged = g_appsFolderChange = g_reopened = NULL;
    g_folderWatchArmed = g_creationHookSet = g_standInRelease = NULL;
    if (registered) UnregisterClassW(L"Chrome_MessageWindow", GetModuleHandleW(NULL));
}

/* ------------------------------------------------------------------ gui.c */

static int g_downloadStarts, g_downloadDeletions, g_captionChanges, g_guiWarnings, g_guiQuestions, g_guiNotices, g_pagesOpened;
static int g_startupCalls, g_uninstallDialogs;
static UpdateResult g_downloadStartResult = UPDATE_STARTED;
static DWORD g_downloadStartError;
static int g_guiAnswer = IDNO;
static BOOL g_uninstallDialogCancelled, g_startupEnabled, g_startupFails;
static WCHAR g_footer[2048], g_openedPage[256];

static UpdateResult FixtureStartDownload(DWORD *error)
{
    g_downloadStarts++;
    *error = g_downloadStartError;
    return g_downloadStartResult;
}

static void FixtureDeleteDownload(void)
{
    g_downloadDeletions++;
}

static BOOL FixtureNoNewerRelease(WCHAR *version, size_t cch)
{
    (void)version; (void)cch;
    return FALSE;
}

static HRESULT FixtureStartupSetting(const ClaudePackage *package, const Profile *profile, BOOL enabled)
{
    (void)package; (void)profile;
    g_startupCalls++;
    g_startupEnabled = enabled;
    return g_startupFails ? E_ACCESSDENIED : S_OK;
}

static int FixtureGuiMessage(HWND owner, UINT flags, const WCHAR *format, ...)
{
    (void)owner; (void)format;
    if ((flags & MB_TYPEMASK) == MB_YESNO) g_guiQuestions++;
    else if ((flags & MB_ICONMASK) == MB_ICONWARNING) g_guiWarnings++;
    else if ((flags & MB_ICONMASK) == MB_ICONINFORMATION) g_guiNotices++;
    return g_guiAnswer;
}

static BOOL FixtureOpenPage(const WCHAR *url)
{
    g_pagesOpened++;
    return SUCCEEDED(StringCchCopyW(g_openedPage, ARRAYSIZE(g_openedPage), url));
}

static UINT WINAPI FixtureCaption(HWND window, int id, LPWSTR text, int cch)
{
    (void)window; (void)id;
    if (cch > 0) text[0] = 0;
    return 0;
}

static BOOL WINAPI FixtureSetCaption(HWND window, int id, LPCWSTR text)
{
    (void)window;
    g_captionChanges++;
    if (id == IDC_ABOUT) StringCchCopyW(g_footer, ARRAYSIZE(g_footer), text);
    return TRUE;
}

static BOOL FixtureGuiPackage(ClaudePackage *package) { ZeroMemory(package, sizeof *package); return FALSE; }
static void FixtureGuiRunning(ProfileList *profiles) { (void)profiles; }
static void FixtureGuiSetProfiles(const ProfileList *profiles) { (void)profiles; }
static void FixtureGuiReady(BOOL allowChanges) { (void)allowChanges; }
static void FixtureGuiReload(void) { }
static UserChoiceState FixtureGuiChoice(void) { return USERCHOICE_OURS; }

static void FixtureGuiProfiles(ProfileList *profiles, const ClaudePackage *package)
{
    (void)package;
    ZeroMemory(profiles, sizeof *profiles);
}

static BOOL FixtureGuiUninstall(HWND owner, const ProfileList *profiles, const BOOL *removeData)
{
    (void)owner; (void)profiles; (void)removeData;
    return FALSE;
}

static INT_PTR FixtureUninstallDialog(HWND owner, int id, DLGPROC procedure, LPARAM parameter)
{
    (void)owner; (void)id; (void)procedure; (void)parameter;
    g_uninstallDialogs++;
    return g_uninstallDialogCancelled ? IDCANCEL : IDOK;
}

#define Update_Run FixtureStartDownload
#define Update_RemoveDownload FixtureDeleteDownload
#define Update_Available FixtureNoNewerRelease
#define Update_Check FixtureNoReleaseCheck
#define Update_Download FixtureNoDownload
#define Shortcut_SetStartup FixtureStartupSetting
#define Shortcut_CreateForProfile FixtureRefuseShortcut
#define Shortcut_AddToStartMenu FixtureRefuseStartMenuEntry
#define Shortcut_RemoveFromStartMenu FixtureRefuseStartMenuRemoval
#define Shortcut_Refresh FixtureNoShortcutRefresh
#define TaskbarPin_Pin FixtureRefusePin
#define TaskbarPin_Refresh FixtureNoPinRefresh
#define TaskbarPin_RepairOurs FixtureNoPinRepair
#define Taskbar_Refresh FixtureNoWatcherRefresh
#define Icons_Ensure FixtureNoIcon
#define Icons_DeleteStale FixtureKeepIcons
#define Profiles_Create FixtureRefuseProfileCreation
#define Profiles_Update FixtureRefuseProfileChange
#define Profiles_SetDefault FixtureRefuseDefaultProfile
#define Profiles_CopySettings FixtureNoSettingsCopy
#define Profiles_Delete FixtureRefuseProfileRemoval
#define Handler_Register FixtureRefuseHandler
#define Handler_AskUser FixtureRefuseLinkChooser
#define Localize_SetLanguage FixtureRefuseLanguage
#define Install_ApplyLanguage FixtureNoLanguageApplied
#define Launcher_Open FixtureRefuseLaunch
#define Install_IsInstalledCopy FixtureNotTheInstalledCopy
#define Install_Run FixtureRefuseInstall
#define Install_Repair FixtureNoRepair
#define Install_FinishUninstall FixtureNoUninstallEnd
#define ShellExecuteW FixtureRefuseShellOpen
#define CreateEventW FixtureUnnamedEvent
#define CreateMutexW FixtureNoNamedMutexCreation
#define Ui_Message FixtureGuiMessage
#define Ui_Dialog FixtureUninstallDialog
#define Util_OpenUrl FixtureOpenPage
#define Util_Log FixtureLog
#define GetDlgItemTextW FixtureCaption
#define SetDlgItemTextW FixtureSetCaption
#define Claude_FindPackage FixtureGuiPackage
#define Claude_UpdateRunning FixtureGuiRunning
#define Profiles_Load FixtureGuiProfiles
#define SessionsView_SetProfiles FixtureGuiSetProfiles
#define SessionsView_Ready FixtureGuiReady
#define SessionsView_Reload FixtureGuiReload
#define Handler_UserChoice FixtureGuiChoice
#define Install_Uninstall FixtureGuiUninstall
#define Gui_Run TestedGui_Run
#define Gui_ShowSessions TestedGui_ShowSessions
#define Gui_LayoutProfileColumns TestedGui_LayoutProfileColumns
#define Gui_MainWindowGeometry TestedGui_MainWindowGeometry
#include "../src/gui.c"
#undef Update_Run
#undef Update_RemoveDownload
#undef Update_Available
#undef Update_Check
#undef Update_Download
#undef Shortcut_SetStartup
#undef Shortcut_CreateForProfile
#undef Shortcut_AddToStartMenu
#undef Shortcut_RemoveFromStartMenu
#undef Shortcut_Refresh
#undef TaskbarPin_Pin
#undef TaskbarPin_Refresh
#undef TaskbarPin_RepairOurs
#undef Taskbar_Refresh
#undef Icons_Ensure
#undef Icons_DeleteStale
#undef Profiles_Create
#undef Profiles_Update
#undef Profiles_SetDefault
#undef Profiles_CopySettings
#undef Profiles_Delete
#undef Handler_Register
#undef Handler_AskUser
#undef Localize_SetLanguage
#undef Install_ApplyLanguage
#undef Launcher_Open
#undef Install_IsInstalledCopy
#undef Install_Run
#undef Install_Repair
#undef Install_FinishUninstall
#undef ShellExecuteW
#undef CreateEventW
#undef CreateMutexW
#undef Ui_Message
#undef Ui_Dialog
#undef Util_OpenUrl
#undef Util_Log
#undef GetDlgItemTextW
#undef SetDlgItemTextW
#undef Claude_FindPackage
#undef Claude_UpdateRunning
#undef Profiles_Load
#undef SessionsView_SetProfiles
#undef SessionsView_Ready
#undef SessionsView_Reload
#undef Handler_UserChoice
#undef Install_Uninstall
#undef Gui_Run
#undef Gui_ShowSessions
#undef Gui_LayoutProfileColumns
#undef Gui_MainWindowGeometry

static void ResetManagerRecords(void)
{
    ZeroMemory(&g_manager, sizeof g_manager);
    g_downloadStarts = g_downloadDeletions = g_captionChanges = g_guiWarnings = g_guiQuestions = g_guiNotices = g_pagesOpened = 0;
    g_uninstallDialogs = 0;
    g_footer[0] = g_openedPage[0] = 0;
}

static void CheckLateDownloads(void)
{
    static const char *const kQuietStates[] = { "uninstall in progress", "uninstalled", "closing" };
    size_t state;
    for (state = 0; state < ARRAYSIZE(kQuietStates); state++) {
        ResetManagerRecords();
        g_manager.uninstallInProgress = state == 0;
        g_manager.uninstalled = state == 1;
        g_manager.closing = state == 2;
        g_manager.updating = g_manager.installing = TRUE;
        Downloaded(UPDATE_READY, ERROR_SUCCESS);
        CheckFor("a late download is not started", kQuietStates[state], g_downloadStarts == 0);
        CheckFor("a late download is deleted without touching the window", kQuietStates[state],
                 g_downloadDeletions == 1 && g_captionChanges == 0);
        CheckFor("a late download clears the update state", kQuietStates[state], !g_manager.updating && !g_manager.installing);
    }
    ResetManagerRecords();
    g_manager.updating = TRUE;
    Downloaded(UPDATE_READY, ERROR_SUCCESS);
    Check("a ready download is started and shows that it installs", g_downloadStarts == 1 && g_manager.installing && g_manager.updating);
    ResetManagerRecords();
    g_manager.updating = TRUE;
    DoUninstall();
    Check("an uninstall asked while an update downloads or installs is refused with a note, the update going on",
          g_uninstallDialogs == 0 && g_guiNotices == 1 && g_manager.updating && !g_manager.uninstallInProgress);
    for (state = 0; state < 2; state++) {
        ResetManagerRecords();
        g_uninstallDialogCancelled = state == 0;
        DoUninstall();
        CheckFor("the window and its version footer come back after the uninstall dialog", state == 0 ? "cancelled" : "failed",
                 g_uninstallDialogs == 1 && !g_manager.uninstallInProgress && !g_manager.uninstalled &&
                 wcsstr(g_footer, APP_VERSION_WSTR) != NULL && wcsstr(g_footer, L"Freenitial") != NULL);
    }
}

typedef struct DownloadOutcome {
    UpdateResult result;
    const char  *name;
    BOOL         offersPage;   /* a question: the releases page can help */
} DownloadOutcome;

static void CheckDownloadOutcomes(void)
{
    static const DownloadOutcome outcomes[] = {
        { UPDATE_NOT_DOWNLOADED, "not downloaded", TRUE },
        { UPDATE_NOT_SIGNED, "not signed", FALSE },
        { UPDATE_NOT_VERIFIED, "signature not verified", FALSE },
        { UPDATE_WRONG_VERSION, "wrong version", FALSE },
        { UPDATE_NOT_STARTED, "not started", TRUE },
    };
    static const int answers[] = { IDNO, IDYES };
    char which[96];
    size_t i, a;
    BOOL pageExpected;
    for (i = 0; i < ARRAYSIZE(outcomes); i++) {
        for (a = 0; a < ARRAYSIZE(answers); a++) {
            ResetManagerRecords();
            g_manager.updating = TRUE;
            g_guiAnswer = answers[a];
            StringCchPrintfA(which, ARRAYSIZE(which), "%s, answered %s", outcomes[i].name, answers[a] == IDYES ? "yes" : "no");
            Downloaded(outcomes[i].result, ERROR_ACCESS_DENIED);
            CheckFor("a failed update is explained once, asking about the page only when it can help", which,
                     g_downloadStarts == 0 && (outcomes[i].offersPage ? g_guiQuestions == 1 && g_guiWarnings == 0
                                                                : g_guiWarnings == 1 && g_guiQuestions == 0));
            pageExpected = outcomes[i].offersPage && answers[a] == IDYES;
            CheckFor("the releases page opens only when asked for", which,
                     g_pagesOpened == (pageExpected ? 1 : 0) && (!pageExpected || wcscmp(g_openedPage, APP_RELEASES_URL) == 0));
            CheckFor("a failed update clears the update state", which, !g_manager.updating && !g_manager.installing);
        }
    }
    g_guiAnswer = IDNO;
    ResetManagerRecords();
    g_manager.updating = TRUE;
    g_downloadStartResult = UPDATE_NOT_STARTED;
    g_downloadStartError = ERROR_ACCESS_DENIED;
    Downloaded(UPDATE_READY, ERROR_SUCCESS);
    Check("a ready download that Windows cannot start asks about the page",
          g_downloadStarts == 1 && g_guiQuestions == 1 && !g_manager.updating && !g_manager.installing);
    g_downloadStartResult = UPDATE_STARTED;
    g_downloadStartError = ERROR_SUCCESS;
}

static void CheckStartupResults(void)
{
    Profile profile;
    ZeroMemory(&profile, sizeof profile);
    ResetManagerRecords();
    StringCchCopyW(profile.name, ARRAYSIZE(profile.name), L"Private startup fixture");
    g_startupCalls = 0;
    SetProfileStartup(&profile, TRUE);
    Check("a sign-in setting that is saved shows no warning", g_startupCalls == 1 && g_startupEnabled && g_guiWarnings == 0);
    g_startupFails = TRUE;
    SetProfileStartup(&profile, FALSE);
    Check("a sign-in setting that cannot be saved shows a warning", g_startupCalls == 2 && !g_startupEnabled && g_guiWarnings == 1);
    g_startupFails = FALSE;
}

/* --------------------------------------------------------------- router.c */

#define RUNNING_CLAUDE_PID  2468
#define ROUTE_PROFILE_COUNT 3
#define ROUTE_SELF_EXE      L"C:\\private-fixture\\" APP_EXE   /* never started: the manager start is recorded */
#define SIGN_IN_CODE        L"fixture-sign-in-code"

/* The profiles a link can go to, by index: whether each runs, when it last
 * started a sign-in (minutes ago, 0: never) and whether Claude starts for it. */
static const WCHAR *const kRouteFolders[ROUTE_PROFILE_COUNT] = { L"Claude-A", L"Claude-B", L"Claude-C" };
static int g_routeProfileCount, g_routeDefaultProfile, g_routeTopmostProfile;
static BOOL g_routeProfileRunning[ROUTE_PROFILE_COUNT], g_routeLaunchSucceeds[ROUTE_PROFILE_COUNT];
static ULONGLONG g_routeSignInMinutesAgo[ROUTE_PROFILE_COUNT];
static BOOL g_routeClaudeMissing, g_routeProfileWatched;
static int g_routeAnswer = IDNO;

static int g_routeLaunches, g_routeLaunchesOf[ROUTE_PROFILE_COUNT], g_routeWatches, g_routeWatchQueries, g_routePendingApplied;
static int g_routeMessages, g_routePagesOpened, g_routeManagerStarts;
static UINT g_routeMessageFlags;
static DWORD g_routeWatchedPid;
static WCHAR g_routeLaunchedUrl[URL_CCH], g_routeMessageText[1024], g_routeOpenedPage[256], g_routeLog[4096];
static WCHAR g_routeManagerExe[MAX_PATH], g_routeManagerArguments[64];

static int RouteProfileIndex(const WCHAR *folder)
{
    int i;
    for (i = 0; i < ROUTE_PROFILE_COUNT; i++)
        if (Core_EqualsI(folder, kRouteFolders[i])) return i;
    return -1;
}

static void FixtureRouteProfiles(ProfileList *profiles, const ClaudePackage *package)
{
    int i;
    (void)package;
    ZeroMemory(profiles, sizeof *profiles);
    for (i = 0; i < g_routeProfileCount; i++) {
        Profile *profile = &profiles->items[profiles->count++];
        StringCchCopyW(profile->folder, ARRAYSIZE(profile->folder), kRouteFolders[i]);
        StringCchCopyW(profile->name, ARRAYSIZE(profile->name), kRouteFolders[i] + wcslen(PROFILE_PREFIX));
        profile->running = g_routeProfileRunning[i];
        profile->pid = g_routeProfileRunning[i] ? RUNNING_CLAUDE_PID : 0;
    }
    if (g_routeProfileCount > 0)
        StringCchCopyW(profiles->defaultFolder, ARRAYSIZE(profiles->defaultFolder), kRouteFolders[g_routeDefaultProfile]);
}

static BOOL FixtureRoutePackage(ClaudePackage *package)
{
    ZeroMemory(package, sizeof *package);
    package->found = !g_routeClaudeMissing;
    return package->found;
}

static int FixtureRouteTopmost(const ProfileList *profiles)
{
    (void)profiles;
    return g_routeTopmostProfile;
}

static BOOL FixtureRouteSignIn(const ClaudePackage *package, const Profile *profile, ULONGLONG *ticks)
{
    int i = RouteProfileIndex(profile->folder);
    (void)package;
    *ticks = i >= 0 && g_routeSignInMinutesAgo[i] ? Util_LocalNowTicks() - g_routeSignInMinutesAgo[i] * 60 * TICKS_PER_SECOND : 0;
    return *ticks != 0;
}

static int FixtureRoutePending(HWND owner, const Profile *profile)
{
    (void)owner;
    (void)profile;
    g_routePendingApplied++;
    return 0;
}

static HRESULT FixtureRouteLaunch(const ClaudePackage *package, const Profile *profile, const WCHAR *url, DWORD *pid,
                                  BOOL *identity)
{
    int i = RouteProfileIndex(profile->folder);
    (void)package; (void)identity;
    g_routeLaunches++;
    if (i >= 0) g_routeLaunchesOf[i]++;
    StringCchCopyW(g_routeLaunchedUrl, ARRAYSIZE(g_routeLaunchedUrl), url ? url : L"");
    if (pid) *pid = LAUNCHED_CLAUDE_PID;
    return i >= 0 && g_routeLaunchSucceeds[i] ? S_OK : E_ACCESSDENIED;
}

static void FixtureRouteWatch(const Profile *profile)
{
    g_routeWatches++;
    g_routeWatchedPid = profile->pid;
}

static BOOL FixtureRouteWatched(const Profile *profile)
{
    (void)profile;
    g_routeWatchQueries++;
    return g_routeProfileWatched;
}

/* Whether the profile runs now, whatever the caller's copy of it says. */
static void FixtureRouteRunningNow(Profile *profile)
{
    int i = RouteProfileIndex(profile->folder);
    profile->running = i >= 0 && g_routeProfileRunning[i];
    profile->pid = profile->running ? RUNNING_CLAUDE_PID : 0;
}

static int FixtureRouteMessage(HWND owner, UINT flags, const WCHAR *format, ...)
{
    va_list arguments;
    (void)owner;
    g_routeMessages++;
    g_routeMessageFlags = flags;
    va_start(arguments, format);
    StringCchVPrintfW(g_routeMessageText, ARRAYSIZE(g_routeMessageText), format, arguments);
    va_end(arguments);
    return g_routeAnswer;
}

/* The router's log, kept to check what it says and what it never says. */
static void FixtureRouteLog(const WCHAR *format, ...)
{
    va_list arguments;
    size_t used = wcslen(g_routeLog);
    va_start(arguments, format);
    StringCchVPrintfW(g_routeLog + used, ARRAYSIZE(g_routeLog) - used, format, arguments);
    va_end(arguments);
    StringCchCatW(g_routeLog, ARRAYSIZE(g_routeLog), L"\n");
}

static BOOL FixtureRouteOpenPage(const WCHAR *url)
{
    g_routePagesOpened++;
    return SUCCEEDED(StringCchCopyW(g_routeOpenedPage, ARRAYSIZE(g_routeOpenedPage), url));
}

static BOOL FixtureRouteSelfExe(WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchCopyW(out, cch, ROUTE_SELF_EXE));
}

static BOOL FixtureRouteSpawn(const WCHAR *exe, const WCHAR *args, DWORD *pid)
{
    if (pid) *pid = 0;
    g_routeManagerStarts++;
    StringCchCopyW(g_routeManagerExe, ARRAYSIZE(g_routeManagerExe), exe);
    StringCchCopyW(g_routeManagerArguments, ARRAYSIZE(g_routeManagerArguments), args);
    return TRUE;
}

#define Profiles_Load FixtureRouteProfiles
#define Claude_FindPackage FixtureRoutePackage
#define Claude_TopmostProfile FixtureRouteTopmost
#define Claude_LastSignInStart FixtureRouteSignIn
#define Claude_RefreshRunning FixtureRouteRunningNow
#define Claude_Launch FixtureRouteLaunch
#define SessionEdit_ApplyPending FixtureRoutePending
#define Taskbar_Watch FixtureRouteWatch
#define Taskbar_IsWatched FixtureRouteWatched
#define Ui_Message FixtureRouteMessage
#define Util_OpenUrl FixtureRouteOpenPage
#define Util_SelfExe FixtureRouteSelfExe
#define Util_Spawn FixtureRouteSpawn
#define Util_Log FixtureRouteLog
#define Router_Run TestedRouter_Run
#define Launcher_Run TestedLauncher_Run
#define Launcher_Open TestedLauncher_Open
#include "../src/router.c"
#undef Profiles_Load
#undef Claude_FindPackage
#undef Claude_TopmostProfile
#undef Claude_LastSignInStart
#undef Claude_RefreshRunning
#undef Claude_Launch
#undef SessionEdit_ApplyPending
#undef Taskbar_Watch
#undef Taskbar_IsWatched
#undef Ui_Message
#undef Util_OpenUrl
#undef Util_SelfExe
#undef Util_Spawn
#undef Util_Log
#undef Router_Run
#undef Launcher_Run
#undef Launcher_Open

static void ResetRouteRecords(void)
{
    g_routeLaunches = g_routeWatches = g_routeWatchQueries = g_routePendingApplied = g_routeMessages = 0;
    g_routePagesOpened = g_routeManagerStarts = 0;
    ZeroMemory(g_routeLaunchesOf, sizeof g_routeLaunchesOf);
    g_routeMessageFlags = 0;
    g_routeWatchedPid = 0;
    g_routeLaunchedUrl[0] = g_routeMessageText[0] = g_routeOpenedPage[0] = g_routeLog[0] = 0;
    g_routeManagerExe[0] = g_routeManagerArguments[0] = 0;
}

/* `count` profiles, none running, the first the default, Claude installed and
 * starting for none of them. */
static void ResetRoute(int count)
{
    g_routeProfileCount = count;
    g_routeDefaultProfile = 0;
    g_routeTopmostProfile = -1;
    ZeroMemory(g_routeProfileRunning, sizeof g_routeProfileRunning);
    ZeroMemory(g_routeLaunchSucceeds, sizeof g_routeLaunchSucceeds);
    ZeroMemory(g_routeSignInMinutesAgo, sizeof g_routeSignInMinutesAgo);
    g_routeClaudeMissing = g_routeProfileWatched = FALSE;
    g_routeAnswer = IDNO;
    ResetRouteRecords();
}

static void CheckLauncherWatchers(void)
{
    ClaudePackage package;
    Profile profile;
    DWORD pid = 0;
    BOOL identity = FALSE;
    HRESULT result;
    ZeroMemory(&package, sizeof package);
    package.found = TRUE;
    ZeroMemory(&profile, sizeof profile);
    StringCchCopyW(profile.folder, ARRAYSIZE(profile.folder), kRouteFolders[0]);
    profile.running = TRUE;
    profile.pid = RUNNING_CLAUDE_PID;
    ResetRoute(1);
    g_routeProfileRunning[0] = TRUE;
    g_routeLaunchSucceeds[0] = TRUE;
    g_routeProfileWatched = TRUE;
    result = TestedLauncher_Open(&package, &profile, L"claude://resume?session=fixture", &pid, &identity);
    Check("a link for a running profile that has a watcher starts no second watcher",
          SUCCEEDED(result) && g_routeLaunches == 1 && g_routeWatches == 0 && pid == LAUNCHED_CLAUDE_PID);
    Check("a running profile is given no queued session changes", g_routePendingApplied == 0);
    g_routeProfileWatched = FALSE;
    profile.pid = RUNNING_CLAUDE_PID + 1;   /* a Claude of the profile that ended since its caller looked */
    ResetRouteRecords();
    result = TestedLauncher_Open(&package, &profile, L"claude://resume?session=fixture", &pid, &identity);
    Check("a running profile without a watcher gets one for the Claude running now",
          SUCCEEDED(result) && g_routeWatches == 1 && g_routeWatchedPid == RUNNING_CLAUDE_PID);
    profile.running = FALSE;
    profile.pid = 0;
    ResetRouteRecords();
    TestedLauncher_Open(&package, &profile, NULL, &pid, &identity);
    Check("a profile started since its caller looked is given no queued session changes", g_routePendingApplied == 0);
    g_routeProfileRunning[0] = FALSE;
    g_routeProfileWatched = TRUE;
    ResetRouteRecords();
    result = TestedLauncher_Open(&package, &profile, NULL, &pid, &identity);
    Check("a profile about to start first gets its queued session changes", g_routePendingApplied == 1);
    Check("a profile just started is watched for the Claude launched for it, whatever watcher was registered",
          SUCCEEDED(result) && g_routeWatches == 1 && g_routeWatchedPid == LAUNCHED_CLAUDE_PID && g_routeWatchQueries == 0);
    profile.running = TRUE;   /* as its caller last saw it: its Claude was quit since, its watcher still ending */
    profile.pid = RUNNING_CLAUDE_PID;
    ResetRouteRecords();
    result = TestedLauncher_Open(&package, &profile, NULL, &pid, &identity);
    Check("a profile quit since its caller looked starts with its queued session changes and a watcher for the Claude launched",
          SUCCEEDED(result) && g_routePendingApplied == 1 && g_routeWatches == 1 && g_routeWatchedPid == LAUNCHED_CLAUDE_PID);
    profile.running = FALSE;
    profile.pid = 0;
    g_routeLaunchSucceeds[0] = FALSE;
    ResetRouteRecords();
    Check("a failed launch starts no watcher", FAILED(TestedLauncher_Open(&package, &profile, NULL, &pid, &identity)) &&
                                                   g_routeWatches == 0);
}

/* Which window a link goes to. */
static void CheckRouteTargets(void)
{
    static const WCHAR kUncleanLink[] = L"claude://resume?session=fixture\"--flag value\\end";
    WCHAR cleanLink[URL_CCH];
    ResetRoute(2);
    g_routeDefaultProfile = 1;
    g_routeLaunchSucceeds[1] = TRUE;
    Check("with nothing running, a link goes to the default profile and is delivered",
          TestedRouter_Run(L"claude://resume?session=fixture") == 0 && g_routeLaunches == 1 && g_routeLaunchesOf[1] == 1 &&
          g_routeMessages == 0);
    ResetRoute(3);
    g_routeProfileRunning[1] = g_routeProfileRunning[2] = TRUE;
    g_routeLaunchSucceeds[0] = g_routeLaunchSucceeds[1] = g_routeLaunchSucceeds[2] = TRUE;
    g_routeTopmostProfile = 2;
    Check("any other link goes to the window used last, among the running ones",
          TestedRouter_Run(L"claude://resume?session=fixture") == 0 && g_routeLaunches == 1 && g_routeLaunchesOf[2] == 1);
    ResetRoute(2);
    g_routeProfileRunning[0] = g_routeProfileRunning[1] = TRUE;
    g_routeLaunchSucceeds[0] = g_routeLaunchSucceeds[1] = TRUE;
    g_routeTopmostProfile = 0;
    g_routeSignInMinutesAgo[0] = 10;
    g_routeSignInMinutesAgo[1] = 2;
    Check("a sign-in link goes to the window that started the last sign-in",
          TestedRouter_Run(L"claude://login?code=" SIGN_IN_CODE) == 0 && g_routeLaunches == 1 && g_routeLaunchesOf[1] == 1);
    Check("the sign-in code stays out of the log", g_routeLog[0] && wcsstr(g_routeLog, SIGN_IN_CODE) == NULL);
    g_routeSignInMinutesAgo[0] = SIGNIN_MAX_AGE_MINUTES + 5;
    g_routeSignInMinutesAgo[1] = 0;
    ResetRouteRecords();
    Check("a sign-in started too long ago claims no link: every window gets it",
          TestedRouter_Run(L"claude://login?code=" SIGN_IN_CODE) == 0 && g_routeLaunches == 2 && g_routeLaunchesOf[0] == 1 &&
          g_routeLaunchesOf[1] == 1);
    ResetRoute(1);
    g_routeLaunchSucceeds[0] = TRUE;
    if (Prepared("clean a link holding a quote, a space and a backslash",
                 Core_SanitizeUrl(kUncleanLink, cleanLink, ARRAYSIZE(cleanLink)))) {
        TestedRouter_Run(kUncleanLink);
        Check("a link reaches Claude cleaned, without a quote, a space or a backslash",
              wcscmp(g_routeLaunchedUrl, cleanLink) == 0 && !wcspbrk(g_routeLaunchedUrl, L"\" \\"));
    }
}

static void CheckRouteFailures(void)
{
    ResetRoute(2);
    g_routeLaunchSucceeds[1] = TRUE;
    Check("a failed delivery to the default profile returns failure", TestedRouter_Run(L"claude://resume?session=fixture") == 1);
    Check("a failed default delivery tries no other profile, even one that would start, and starts no watcher",
          g_routeLaunches == 1 && g_routeLaunchesOf[0] == 1 && g_routeLaunchesOf[1] == 0 && g_routeWatches == 0);
    Check("a link no window received is reported, with Windows' error in the log",
          g_routeMessages == 1 && (g_routeMessageFlags & MB_ICONMASK) == MB_ICONERROR && wcsstr(g_routeLog, L"0x80070005") != NULL);
    ResetRoute(2);
    g_routeProfileRunning[0] = g_routeProfileRunning[1] = TRUE;
    Check("a sign-in broadcast that reaches no window returns failure, once reported",
          TestedRouter_Run(L"claude://login?code=fixture") == 1 && g_routeMessages == 1);
    Check("a failed broadcast tries each running window once", g_routeLaunches == 2 && g_routeWatches == 0);
    g_routeLaunchSucceeds[1] = TRUE;
    ResetRouteRecords();
    Check("a sign-in broadcast that reaches one window returns success, without a message",
          TestedRouter_Run(L"claude://login?code=fixture") == 0 && g_routeMessages == 0);
    Check("a partial broadcast watches only the window it reached", g_routeLaunches == 2 && g_routeWatches == 1);

    ResetRoute(1);
    g_routeClaudeMissing = TRUE;
    Check("a link without Claude Desktop returns failure after one question",
          TestedRouter_Run(L"claude://resume?session=fixture") == 1 && g_routeMessages == 1 &&
          (g_routeMessageFlags & MB_TYPEMASK) == MB_YESNO && g_routeLaunches == 0);
    Check("declining opens no download page", g_routePagesOpened == 0);
    g_routeAnswer = IDYES;
    ResetRouteRecords();
    TestedRouter_Run(L"claude://resume?session=fixture");
    Check("accepting opens Claude's download page", g_routePagesOpened == 1 && wcscmp(g_routeOpenedPage, APP_DOWNLOAD_URL) == 0);

    ResetRoute(0);
    Check("a link with no profile found returns failure without launching",
          TestedRouter_Run(L"claude://resume?session=fixture") == 1 && g_routeLaunches == 0);
    ResetRoute(1);
    Check("a link that is not a claude:// link is ignored",
          TestedRouter_Run(L"https://example.com/") == 1 && g_routeLaunches == 0 && g_routeMessages == 0);
}

static void CheckLauncherRun(void)
{
    ResetRoute(1);
    Check("a shortcut to a profile that no longer exists says so, by the name it had, and launches nothing",
          TestedLauncher_Run(L"Claude-Gone") == 1 && g_routeMessages == 1 && g_routeLaunches == 0 &&
          wcsstr(g_routeMessageText, L"Gone") != NULL && wcsstr(g_routeMessageText, L"Claude-Gone") == NULL);
    Check("declining to open the manager starts nothing", g_routeManagerStarts == 0);
    g_routeAnswer = IDYES;
    ResetRouteRecords();
    TestedLauncher_Run(L"Claude-Gone");
    Check("accepting starts the manager, this same program, with no argument",
          g_routeManagerStarts == 1 && wcscmp(g_routeManagerExe, ROUTE_SELF_EXE) == 0 && g_routeManagerArguments[0] == 0);
    ResetRoute(1);
    g_routeClaudeMissing = TRUE;
    Check("a shortcut opened without Claude Desktop asks about its download page and launches nothing",
          TestedLauncher_Run(kRouteFolders[0]) == 1 && g_routeLaunches == 0 && g_routeMessages == 1 &&
          (g_routeMessageFlags & MB_TYPEMASK) == MB_YESNO);
    ResetRoute(1);
    Check("a shortcut whose Claude does not start says so",
          TestedLauncher_Run(kRouteFolders[0]) == 1 && g_routeLaunches == 1 && g_routeMessages == 1 &&
          (g_routeMessageFlags & MB_ICONMASK) == MB_ICONERROR);
    g_routeLaunchSucceeds[0] = TRUE;
    ResetRouteRecords();
    Check("a shortcut opens its profile", TestedLauncher_Run(kRouteFolders[0]) == 0 && g_routeLaunchesOf[0] == 1 && g_routeMessages == 0);
}

/* ----------------------------------------------------------------- util.c */

#define REMOVED_PATH        L"C:\\private-fixture\\removed-file"
#define SECOND_REMOVED_PATH L"C:\\private-fixture\\second-file"

#define REMOVAL_LISTING ((HANDLE)(INT_PTR)0x61)

static DWORD g_removalErrorBefore, g_removalErrorAfter, g_secondRemovalErrorBefore;
static DWORD g_removalAttributes = FILE_ATTRIBUTE_NORMAL, g_removalReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
static int g_removalShellCode, g_removalShellCalls, g_linkRemovals;
static BOOL g_removalShellRan, g_removalAborted, g_linkRemovalFails;

/* Removal results come from simulated metadata and shell calls: nothing
 * reaches the Recycle Bin. The second path, when there, is a file. */
static DWORD WINAPI FixtureRemovalAttributes(LPCWSTR path)
{
    DWORD error;
    BOOL second = wcscmp(path, SECOND_REMOVED_PATH) == 0;
    if (second) error = g_removalShellRan ? ERROR_FILE_NOT_FOUND : g_secondRemovalErrorBefore;
    else error = g_removalShellRan ? g_removalErrorAfter : g_removalErrorBefore;
    if (!error) return second ? FILE_ATTRIBUTE_NORMAL : g_removalAttributes;
    SetLastError(error);
    return INVALID_FILE_ATTRIBUTES;
}

/* The listing of a path's own entry: its reparse tag tells a link from, say, a cloud placeholder. */
static HANDLE WINAPI FixtureRemovalListing(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                           LPVOID filter, DWORD flags)
{
    WIN32_FIND_DATAW *entry = (WIN32_FIND_DATAW *)data;
    DWORD attributes = FixtureRemovalAttributes(path);
    (void)level; (void)search; (void)filter; (void)flags;
    if (attributes == INVALID_FILE_ATTRIBUTES) return INVALID_HANDLE_VALUE;
    ZeroMemory(entry, sizeof *entry);
    entry->dwFileAttributes = attributes;
    entry->dwReserved0 = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? g_removalReparseTag : 0;
    return REMOVAL_LISTING;
}

static BOOL WINAPI FixtureRemovalListingClose(HANDLE listing)
{
    return listing == REMOVAL_LISTING;
}

static int WINAPI FixtureRemovalShell(LPSHFILEOPSTRUCTW operation)
{
    g_removalShellCalls++;
    g_removalShellRan = TRUE;
    operation->fAnyOperationsAborted = g_removalAborted;
    return g_removalShellCode;
}

static BOOL WINAPI FixtureLinkRemoval(LPCWSTR path)
{
    (void)path;
    g_linkRemovals++;
    if (!g_linkRemovalFails) return TRUE;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

/* The copy of util.c compiled here finds no real folder: its log has nowhere to go. */
static HRESULT WINAPI FixtureNoKnownFolder(REFKNOWNFOLDERID folder, DWORD flags, HANDLE token, PWSTR *path)
{
    (void)folder; (void)flags; (void)token;
    *path = NULL;
    return E_FAIL;
}

/* Nor does it ever share the program's log lock. */
static HANDLE WINAPI FixtureUnnamedLogMutex(LPSECURITY_ATTRIBUTES security, BOOL owner, LPCWSTR name)
{
    (void)name;
    return CreateMutexW(security, owner, NULL);
}

#define CreateMutexW FixtureUnnamedLogMutex
#define RegCreateKeyExW FixtureRefuseKeyCreation
#define RegOpenKeyExW FixtureRefuseKeyOpen
#define RegGetValueW FixtureRefuseValueRead
#define RegDeleteTreeW FixtureRefuseRegistryRemoval
#define RegDeleteKeyW FixtureRefuseRegistryRemoval
#define RegDeleteValueW FixtureRefuseRegistryRemoval
#define CreateProcessW FixtureRefuseProcess
#define ShellExecuteW FixtureRefuseShellOpen
#define GetFileAttributesW FixtureRemovalAttributes
#define FindFirstFileExW FixtureRemovalListing
#define FindClose FixtureRemovalListingClose
#define SHFileOperationW FixtureRemovalShell
#define RemoveDirectoryW FixtureLinkRemoval
#define SHGetKnownFolderPath FixtureNoKnownFolder
#define g_hInst TestedUtil_Instance
#define Util_KnownFolder TestedUtil_KnownFolder
#define Util_AppData TestedUtil_AppData
#define Util_LocalAppData TestedUtil_LocalAppData
#define Util_SelfExe TestedUtil_SelfExe
#define Util_InstallDir TestedUtil_InstallDir
#define Util_InstallExe TestedUtil_InstallExe
#define Util_SetStateDir TestedUtil_SetStateDir
#define Util_StateDir TestedUtil_StateDir
#define Util_FileExists TestedUtil_FileExists
#define Util_DirExists TestedUtil_DirExists
#define Util_QueryPath TestedUtil_QueryPath
#define Util_ExistingDir TestedUtil_ExistingDir
#define Util_EnsureDir TestedUtil_EnsureDir
#define Util_RegGetString TestedUtil_RegGetString
#define Util_RegSetString TestedUtil_RegSetString
#define Util_RegSetStringIfDifferent TestedUtil_RegSetStringIfDifferent
#define Util_RegGetDword TestedUtil_RegGetDword
#define Util_RegSetDword TestedUtil_RegSetDword
#define Util_RegKeyExists TestedUtil_RegKeyExists
#define Util_RegValueExists TestedUtil_RegValueExists
#define Util_RegDeleteValue TestedUtil_RegDeleteValue
#define Util_RegDeleteTree TestedUtil_RegDeleteTree
#define Util_LocalNowTicks TestedUtil_LocalNowTicks
#define Util_Log TestedUtil_Log
#define Util_OpenUrl TestedUtil_OpenUrl
#define Util_Spawn TestedUtil_Spawn
#define Util_IsDirectoryLink TestedUtil_IsDirectoryLink
#define Util_FitsRecycleBin TestedUtil_FitsRecycleBin
#define Util_Recycle TestedUtil_Recycle
#define Util_ReadFile TestedUtil_ReadFile
#define Util_ExtendedPath TestedUtil_ExtendedPath
#define Util_FindFiles TestedUtil_FindFiles
#include "../src/util.c"
#undef CreateMutexW
#undef RegCreateKeyExW
#undef RegOpenKeyExW
#undef RegGetValueW
#undef RegDeleteTreeW
#undef RegDeleteKeyW
#undef RegDeleteValueW
#undef CreateProcessW
#undef ShellExecuteW
#undef GetFileAttributesW
#undef FindFirstFileExW
#undef FindClose
#undef SHFileOperationW
#undef RemoveDirectoryW
#undef SHGetKnownFolderPath
#undef g_hInst
#undef Util_KnownFolder
#undef Util_AppData
#undef Util_LocalAppData
#undef Util_SelfExe
#undef Util_InstallDir
#undef Util_InstallExe
#undef Util_SetStateDir
#undef Util_StateDir
#undef Util_FileExists
#undef Util_DirExists
#undef Util_QueryPath
#undef Util_ExistingDir
#undef Util_EnsureDir
#undef Util_RegGetString
#undef Util_RegSetString
#undef Util_RegSetStringIfDifferent
#undef Util_RegGetDword
#undef Util_RegSetDword
#undef Util_RegKeyExists
#undef Util_RegValueExists
#undef Util_RegDeleteValue
#undef Util_RegDeleteTree
#undef Util_LocalNowTicks
#undef Util_Log
#undef Util_OpenUrl
#undef Util_Spawn
#undef Util_IsDirectoryLink
#undef Util_FitsRecycleBin
#undef Util_Recycle
#undef Util_ReadFile
#undef Util_ExtendedPath
#undef Util_FindFiles

/* The program's own Util_EnsureDir, linked from util.c. */
static void CheckEnsureDir(void)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    if (!Prepared("make a private folder path", JoinPath(g_root, L"directory", path, ARRAYSIZE(path)))) return;
    Check("EnsureDir creates a missing folder", Util_EnsureDir(path) && Util_DirExists(path));
    Check("EnsureDir accepts an existing folder", Util_EnsureDir(path));
    if (!Prepared("remove the private folder", RemoveDirectoryW(path))) return;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!Prepared("create a file where the folder was", file != INVALID_HANDLE_VALUE)) return;
    CloseHandle(file);
    Check("EnsureDir refuses an existing file", !Util_EnsureDir(path));
}

static void ResetRemoval(DWORD errorBefore, DWORD errorAfter, int shellCode, BOOL aborted)
{
    g_removalErrorBefore = errorBefore;
    g_removalErrorAfter = errorAfter;
    g_removalShellCode = shellCode;
    g_removalAborted = aborted;
    g_secondRemovalErrorBefore = ERROR_FILE_NOT_FOUND;
    g_removalAttributes = FILE_ATTRIBUTE_NORMAL;
    g_removalReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    g_removalShellRan = g_linkRemovalFails = FALSE;
    g_removalShellCalls = g_linkRemovals = 0;
}

/* A path of exactly `length` characters (less than `cch`) under the made-up removal folder. */
static void MakePathOfLength(WCHAR *out, size_t cch, size_t length)
{
    size_t prefix;
    StringCchCopyW(out, cch, L"C:\\private-fixture\\");
    prefix = wcslen(out);
    wmemset(out + prefix, L'x', length - prefix);
    out[length] = 0;
}

/* The Recycle Bin takes no path of MAX_PATH characters or more, nor the \\?\
 * form: such a path fails the whole removal before anything is removed. */
static void CheckRecycleBinPaths(void)
{
    WCHAR tooLong[MAX_PATH + 1], longest[MAX_PATH];
    const WCHAR *withTooLong[2], *withExtended[2], *alone[1];
    MakePathOfLength(tooLong, ARRAYSIZE(tooLong), MAX_PATH);
    MakePathOfLength(longest, ARRAYSIZE(longest), MAX_PATH - 1);
    withTooLong[0] = withExtended[0] = REMOVED_PATH;
    withTooLong[1] = tooLong;
    withExtended[1] = L"\\\\?\\C:\\private-fixture\\extended";
    ResetRemoval(0, ERROR_FILE_NOT_FOUND, 0, FALSE);
    g_removalAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    Check("a path of MAX_PATH characters fails the removal before any link or file is removed",
          TestedUtil_Recycle(NULL, withTooLong, ARRAYSIZE(withTooLong)) == REMOVE_FAILED && g_linkRemovals == 0 &&
          g_removalShellCalls == 0);
    Check("a \\\\?\\ path fails the removal before any link or file is removed",
          TestedUtil_Recycle(NULL, withExtended, ARRAYSIZE(withExtended)) == REMOVE_FAILED && g_linkRemovals == 0 &&
          g_removalShellCalls == 0);
    ResetRemoval(0, ERROR_FILE_NOT_FOUND, 0, FALSE);
    alone[0] = longest;
    Check("a path of MAX_PATH - 1 characters still goes to the Recycle Bin",
          TestedUtil_Recycle(NULL, alone, ARRAYSIZE(alone)) == REMOVE_DONE && g_removalShellCalls == 1);
}

typedef struct RemovalCase {
    const char  *name;
    DWORD        errorBefore, errorAfter;
    int          shellCode;
    BOOL         aborted;
    RemoveResult expected;
    int          shellCalls;
} RemovalCase;

static void CheckRecycleResults(void)
{
    static const RemovalCase cases[] = {
        { "already gone", ERROR_FILE_NOT_FOUND, 0, 0, FALSE, REMOVE_DONE, 0 },
        { "denied before", ERROR_ACCESS_DENIED, 0, 0, FALSE, REMOVE_FAILED, 0 },
        { "share unreachable before", ERROR_BAD_NETPATH, 0, 0, FALSE, REMOVE_FAILED, 0 },
        { "shell error, file gone", 0, ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED, FALSE, REMOVE_FAILED, 1 },
        { "denied after", 0, ERROR_ACCESS_DENIED, 0, FALSE, REMOVE_FAILED, 1 },
        { "share unreachable after", 0, ERROR_BAD_NETPATH, 0, FALSE, REMOVE_FAILED, 1 },
        { "removed", 0, ERROR_FILE_NOT_FOUND, 0, FALSE, REMOVE_DONE, 1 },
        { "removed with its folder", 0, ERROR_PATH_NOT_FOUND, 0, FALSE, REMOVE_DONE, 1 },
        { "still there", 0, 0, 0, FALSE, REMOVE_FAILED, 1 },
        { "cancelled", 0, ERROR_FILE_NOT_FOUND, ERROR_CANCELLED, FALSE, REMOVE_CANCELLED, 1 },
        { "aborted", 0, ERROR_FILE_NOT_FOUND, 0, TRUE, REMOVE_CANCELLED, 1 }
    };
    static const DWORD kLinkTags[] = { IO_REPARSE_TAG_MOUNT_POINT, IO_REPARSE_TAG_SYMLINK };
    static const char *const kLinkTagNames[] = { "junction", "directory symlink" };
    static const struct { const char *name; int shellCode; RemoveResult expected; } kLinkWithFile[] = {
        { "cancelled", ERROR_CANCELLED, REMOVE_CANCELLED },
        { "failed", ERROR_ACCESS_DENIED, REMOVE_FAILED },
        { "done", 0, REMOVE_DONE }
    };
    const WCHAR *path = REMOVED_PATH;
    const WCHAR *const both[] = { REMOVED_PATH, SECOND_REMOVED_PATH };
    size_t i;
    for (i = 0; i < ARRAYSIZE(cases); i++) {
        ResetRemoval(cases[i].errorBefore, cases[i].errorAfter, cases[i].shellCode, cases[i].aborted);
        CheckFor("a removal reports what is left, never success over a failure", cases[i].name,
                 TestedUtil_Recycle(NULL, &path, 1) == cases[i].expected && g_removalShellCalls == cases[i].shellCalls);
    }
    ResetRemoval(0, ERROR_FILE_NOT_FOUND, 0, FALSE);
    g_removalAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    g_secondRemovalErrorBefore = ERROR_BAD_NETPATH;
    Check("one unreachable path stops a removal before any link or file is removed",
          TestedUtil_Recycle(NULL, both, ARRAYSIZE(both)) == REMOVE_FAILED && g_linkRemovals == 0 && g_removalShellCalls == 0);
    for (i = 0; i < ARRAYSIZE(kLinkTags); i++) {
        ResetRemoval(0, ERROR_FILE_NOT_FOUND, 0, FALSE);
        g_removalAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        g_removalReparseTag = kLinkTags[i];
        CheckFor("a folder link is removed as a link, never through the shell", kLinkTagNames[i],
                 TestedUtil_Recycle(NULL, &path, 1) == REMOVE_DONE && g_linkRemovals == 1 && g_removalShellCalls == 0);
        g_linkRemovalFails = TRUE;
        CheckFor("a folder link that cannot be removed fails the removal", kLinkTagNames[i],
                 TestedUtil_Recycle(NULL, &path, 1) == REMOVE_FAILED && g_removalShellCalls == 0);
    }
    ResetRemoval(0, ERROR_FILE_NOT_FOUND, 0, FALSE);
    g_removalAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    g_removalReparseTag = IO_REPARSE_TAG_CLOUD;
    Check("a cloud placeholder folder goes to the Recycle Bin as a folder",
          TestedUtil_Recycle(NULL, &path, 1) == REMOVE_DONE && g_linkRemovals == 0 && g_removalShellCalls == 1);
    /* A folder link and a file at once: the link goes once the file is in the bin. */
    for (i = 0; i < ARRAYSIZE(kLinkWithFile); i++) {
        ResetRemoval(0, 0, kLinkWithFile[i].shellCode, FALSE);
        g_removalAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        g_secondRemovalErrorBefore = 0;
        CheckFor("a folder link is removed only once the rest of its removal is in the Recycle Bin", kLinkWithFile[i].name,
                 TestedUtil_Recycle(NULL, both, ARRAYSIZE(both)) == kLinkWithFile[i].expected && g_removalShellCalls == 1 &&
                 g_linkRemovals == (kLinkWithFile[i].expected == REMOVE_DONE ? 1 : 0));
    }
    CheckRecycleBinPaths();
    ResetRemoval(0, 0, 0, FALSE);
}

/* ------------------------------------------------------------- profiles.c */

static RemoveResult g_failingLocationResult;
static int g_locationRemovals, g_failingLocation, g_localAppDataLength;
static int g_profileStartsAfterLocations = -1;
static BOOL g_claudePackageInstalled, g_roamingMissing, g_localMissing;

static BOOL FixtureProfileStartsRunning(const Profile *profile)
{
    (void)profile;
    return g_profileStartsAfterLocations >= 0 && g_locationRemovals >= g_profileStartsAfterLocations;
}

static PathState FixtureProfilePathPresent(const WCHAR *path, DWORD *attributes)
{
    (void)path;
    if (attributes) *attributes = FILE_ATTRIBUTE_DIRECTORY;
    return PATH_PRESENT;
}

static BOOL FixtureProfileRoaming(WCHAR *out, size_t cch)
{
    return !g_roamingMissing && SUCCEEDED(StringCchCopyW(out, cch, L"C:\\private-fixture\\roaming"));
}

/* g_localAppDataLength: a path of that many characters, to overflow what is built on it. */
static BOOL FixtureProfileLocal(WCHAR *out, size_t cch)
{
    if (g_localMissing) return FALSE;
    if (g_localAppDataLength) {
        if ((size_t)g_localAppDataLength >= cch) return FALSE;
        wmemset(out, L'x', (size_t)g_localAppDataLength);
        out[g_localAppDataLength] = 0;
        return TRUE;
    }
    return SUCCEEDED(StringCchCopyW(out, cch, L"C:\\private-fixture\\local"));
}

static BOOL FixtureProfileFolderExists(const WCHAR *path)
{
    (void)path;
    return TRUE;
}

static BOOL FixtureProfilePackage(ClaudePackage *package)
{
    ZeroMemory(package, sizeof *package);
    StringCchCopyW(package->family, ARRAYSIZE(package->family), L"Fixture_package");
    return g_claudePackageInstalled;
}

static RemoveResult FixtureRecycleLocation(HWND owner, const WCHAR *const *paths, int count)
{
    (void)owner; (void)paths; (void)count;
    return ++g_locationRemovals == g_failingLocation ? g_failingLocationResult : REMOVE_DONE;
}

#define Util_QueryPath FixtureProfilePathPresent
#define Util_AppData FixtureProfileRoaming
#define Util_LocalAppData FixtureProfileLocal
#define Util_DirExists FixtureProfileFolderExists
#define Util_Recycle FixtureRecycleLocation
#define Util_Log FixtureLog
#define Claude_FindPackage FixtureProfilePackage
#define Claude_IsRunning FixtureProfileStartsRunning
#define Util_RegGetString FixtureNoRegistryString
#define Util_RegGetDword FixtureNoRegistryNumber
#define Util_RegSetString FixtureRefuseRegistryString
#define Util_RegSetDword FixtureRefuseRegistryNumber
#define Util_RegDeleteTree FixtureRefuseRegistryTreeRemoval
#define Util_RegDeleteValue FixtureRefuseRegistryValueRemoval
#define Util_RegKeyExists FixtureNoRegistryKey
#define RegOpenKeyExW FixtureRefuseKeyOpen
#define Install_IsRegistered FixtureNotRegistered
#define Shortcut_RemoveOurs FixtureKeepShortcuts
#define Icons_DeleteStale FixtureKeepIcons
#define SessionStore_PendingPath FixtureNoPendingPath
#define SetValue ProfilesSetValue   /* taskbar.c, included above, has its own */
#define Profiles_ResolveStorage TestedProfiles_ResolveStorage
#define Profiles_Load TestedProfiles_Load
#define Profiles_Find TestedProfiles_Find
#define Profiles_DefaultIndex TestedProfiles_DefaultIndex
#define Profiles_Create TestedProfiles_Create
#define Profiles_Update TestedProfiles_Update
#define Profiles_SetDefault TestedProfiles_SetDefault
#define Profiles_CopySettings TestedProfiles_CopySettings
#define Profiles_IsLinked TestedProfiles_IsLinked
#define Profiles_LinkTarget TestedProfiles_LinkTarget
#define Profiles_RecycleData TestedProfiles_RecycleData
#define Profiles_Delete TestedProfiles_Delete
/* Profiles_Load calls it before its definition. */
int TestedProfiles_Find(const ProfileList *list, const WCHAR *folder);
#include "../src/profiles.c"
#undef Util_QueryPath
#undef Util_AppData
#undef Util_LocalAppData
#undef Util_DirExists
#undef Util_Recycle
#undef Util_Log
#undef Claude_FindPackage
#undef Claude_IsRunning
#undef Util_RegGetString
#undef Util_RegGetDword
#undef Util_RegSetString
#undef Util_RegSetDword
#undef Util_RegDeleteTree
#undef Util_RegDeleteValue
#undef Util_RegKeyExists
#undef RegOpenKeyExW
#undef Install_IsRegistered
#undef Shortcut_RemoveOurs
#undef Icons_DeleteStale
#undef SessionStore_PendingPath
#undef SetValue
#undef Profiles_ResolveStorage
#undef Profiles_Load
#undef Profiles_Find
#undef Profiles_DefaultIndex
#undef Profiles_Create
#undef Profiles_Update
#undef Profiles_SetDefault
#undef Profiles_CopySettings
#undef Profiles_IsLinked
#undef Profiles_LinkTarget
#undef Profiles_RecycleData
#undef Profiles_Delete

/* The locations of a profile's data, removed in turn: its folder and "<folder>-Data" in
 * %LOCALAPPDATA%, then the data folder; with Claude installed, each of the first two
 * also in the package's LocalCache\\Local, and its LocalCache\\Roaming copy before
 * the data folder. */
#define LOCATIONS_WITHOUT_PACKAGE 3
#define LOCATIONS_WITH_PACKAGE    6
/* A %LOCALAPPDATA% of this many characters leaves room for "\\<folder>-Data", not
 * for the package cache paths built on it. */
#define LOCAL_PATH_ROOM_FOR_LOCAL_COPIES_ONLY 220

static RemoveResult RecycleFixtureProfile(const Profile *profile)
{
    g_locationRemovals = 0;
    return TestedProfiles_RecycleData(NULL, profile);
}

static void CheckProfileRemovalErrors(void)
{
    static const RemoveResult failures[] = { REMOVE_FAILED, REMOVE_CANCELLED };
    Profile profile;
    char which[96];
    int package, locations, failing;
    size_t failure;
    ZeroMemory(&profile, sizeof profile);
    StringCchCopyW(profile.folder, ARRAYSIZE(profile.folder), L"Claude-private-fixture");
    StringCchCopyW(profile.dataDir, ARRAYSIZE(profile.dataDir), L"C:\\private-fixture\\roaming\\Claude-private-fixture");
    for (package = 0; package < 2; package++) {
        g_claudePackageInstalled = package != 0;
        locations = g_claudePackageInstalled ? LOCATIONS_WITH_PACKAGE : LOCATIONS_WITHOUT_PACKAGE;
        g_failingLocation = 0;
        CheckFor("a complete removal visits every location of the profile", package ? "Claude installed" : "Claude missing",
                 RecycleFixtureProfile(&profile) == REMOVE_DONE && g_locationRemovals == locations);
        for (failing = 1; failing <= locations; failing++) {
            for (failure = 0; failure < ARRAYSIZE(failures); failure++) {
                g_failingLocationResult = failures[failure];
                g_failingLocation = failing;
                StringCchPrintfA(which, ARRAYSIZE(which), "%s, location %d of %d %s", package ? "Claude installed" : "Claude missing",
                                 failing, locations, failures[failure] == REMOVE_FAILED ? "failed" : "cancelled");
                CheckFor("a location that fails or is cancelled stops the removal and is reported", which,
                         RecycleFixtureProfile(&profile) == failures[failure] && g_locationRemovals == failing);
            }
        }
    }
    g_failingLocation = 0;
    g_roamingMissing = TRUE;
    Check("an unavailable Roaming folder fails before anything is removed",
          RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 0);
    g_roamingMissing = FALSE;
    g_localMissing = TRUE;
    Check("an unavailable Local folder cannot report a complete removal",
          RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 0);
    g_localMissing = FALSE;
    g_localAppDataLength = MAX_PATH - 1;
    Check("a Local path too long to build cannot be skipped silently",
          RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 0);
    g_claudePackageInstalled = TRUE;
    g_localAppDataLength = LOCAL_PATH_ROOM_FOR_LOCAL_COPIES_ONLY;
    Check("a package cache path too long to build is reported",
          RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 1);
    g_localAppDataLength = 0;
    g_profileStartsAfterLocations = 0;
    Check("a profile found running is refused before any of its data is removed",
          RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 0);
    g_profileStartsAfterLocations = 1;
    Check("a profile that starts during its removal keeps the locations not yet removed",
          RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 1);
    g_profileStartsAfterLocations = -1;
    profile.isStock = TRUE;
    Check("the stock profile's data is never removed", RecycleFixtureProfile(&profile) == REMOVE_FAILED && g_locationRemovals == 0);
}

/* ------------------------------------------------------------------- main */

static BOOL CreatePrivateRoot(void)
{
    WCHAR temp[MAX_PATH];
    DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    return length && length < ARRAYSIZE(temp) &&
           SUCCEEDED(StringCchPrintfW(g_root, ARRAYSIZE(g_root), L"%sProfilesPlatform-%lu-%llu", temp, GetCurrentProcessId(),
                                      GetTickCount64())) &&
           CreateDirectoryW(g_root, NULL);
}

int wmain(int argc, WCHAR **argv)
{
    const char *setupFailure = NULL;
    DWORD setupError;
    BOOL rootCreated;
    /* The update checks start a copy of this exe as the downloaded release. */
    if (argc == 2 && wcscmp(argv[1], L"--install") == 0) return 0;
    rootCreated = CreatePrivateRoot();
    if (!rootCreated)
        setupFailure = "no private folder could be created in %TEMP%";
    else if (!JoinPath(g_root, L"state", g_stateDir, ARRAYSIZE(g_stateDir)) || !CreateDirectoryW(g_stateDir, NULL))
        setupFailure = "the private state folder could not be created";
    else if (!TestedUpdate_DownloadPath(g_downloadFile, ARRAYSIZE(g_downloadFile)))
        setupFailure = "the private download path could not be made";
    if (setupFailure) {
        setupError = GetLastError();
        printf("Platform tests could not start: %s (error %lu).\n", setupFailure, setupError);
        if (rootCreated) RemovePrivateTree(g_root);
        return 1;
    }
    /* Whatever the program's own code logs goes to the private folder. */
    Util_SetStateDir(g_stateDir);

    CheckReleaseTags();
    CheckUpdateJobs();
    CheckCacheLifecycle();
    CheckDownloadVerification();
    CheckReleaseRequests();
    CheckDownloadRequests();
    if (PrepareInstallFixture()) {
        CheckInstallRefusals();
        CheckAtomicInstall();
        CheckInstallRegistration();
        CheckInstalledManagerStart();
        CheckLanguageApplied();
        CheckUninstall();
        CheckCleanupAfterExit();
    }
    EndInstallFixture();
    CheckWatcherStops();
    CheckWatcherLife();
    CheckLateDownloads();
    CheckDownloadOutcomes();
    CheckStartupResults();
    CheckLauncherWatchers();
    CheckRouteTargets();
    CheckRouteFailures();
    CheckLauncherRun();
    CheckEnsureDir();
    CheckRecycleResults();
    CheckProfileRemovalErrors();

    Check("the private folder is removed", RemovePrivateTree(g_root));
    printf("Platform tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
