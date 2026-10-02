/*
 * New releases. When the manager opens, at most every CHECK_MINUTES, the tag
 * of the latest GitHub release is compared with this version; nothing else is
 * sent and nothing runs on a timer. Updating downloads that release's exe,
 * holds it open so that nothing can change it, checks with Windows that it is
 * signed by APP_SIGNER and that it declares the release's version, newer than
 * this one, then runs it: it installs itself over this copy, as a manual
 * download would, and is deleted once it has exited. What the check found is
 * kept under REG_UPDATE, apart from the profiles' keys that the manager
 * watches.
 */
#include "app.h"
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>

#define API_HOST            L"api.github.com"
#define API_PATH            L"/repos/" APP_REPO L"/releases/latest"
#define DOWNLOAD_HOST       L"github.com"
#define DOWNLOAD_PATH_START L"/" APP_REPO L"/releases/download/"
#define DOWNLOAD_PATH_END   L"/" APP_EXE
#define UPDATE_SUBKEY       L"Update"
#define REG_UPDATE          REG_ROOT L"\\" UPDATE_SUBKEY
#define TAG_VALUE           L"LatestRelease"
#define CHECK_TIME_VALUE    L"ReleaseCheckMinute"   /* counted from 1601 */
#define CHECK_MINUTES       (4 * 60)
#define MAX_RELEASE_ANSWER  (256 * 1024)
#define MIN_DOWNLOAD        (64 * 1024)
#define MAX_DOWNLOAD        (32 * 1024 * 1024)
#define TAG_CCH             32
#define VERSION_PARTS       4

/* Each WinHTTP step gives up after its own timeout; a server that keeps
 * sending a few bytes at a time is stopped by the request's deadline. */
#define HTTP_RESOLVE_TIMEOUT_MS 10000
#define HTTP_CONNECT_TIMEOUT_MS 10000
#define HTTP_SEND_TIMEOUT_MS    15000
#define HTTP_RECEIVE_TIMEOUT_MS 30000
#define CHECK_DEADLINE_MS       60000
#define DOWNLOAD_DEADLINE_MS    (10 * 60 * 1000)   /* MAX_DOWNLOAD at about 55 KB/s */

typedef struct UpdateJob {
    HWND  notify;
    UINT  message;
    WCHAR tag[TAG_CCH];
} UpdateJob;

static const DWORD kRunningVersion[VERSION_PARTS] = { APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, 0 };

/* The verified download, held open from its check until it runs. */
static HANDLE volatile g_readyDownload;

/* A DWORD until the year 9767. */
static DWORD MinutesSince1601(void)
{
    FILETIME now;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&now);
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    return (DWORD)(ticks.QuadPart / (60 * TICKS_PER_SECOND));
}

/* Where a release is downloaded to, and run from to install itself. */
BOOL Update_DownloadPath(WCHAR *out, size_t cch)
{
    WCHAR temp[MAX_PATH];
    DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    return length > 0 && length < ARRAYSIZE(temp) && SUCCEEDED(StringCchPrintfW(out, cch, L"%supdate-" APP_EXE, temp));
}

/* "v1.2.3" or "1.2.3": one to VERSION_PARTS numbers, none empty. Anything
 * else is not a release of this program, and would not be safe in the
 * download's URL. `parts` gets the version, `*number` the tag without its v. */
static BOOL ParseReleaseTag(const WCHAR *tag, DWORD parts[VERSION_PARTS], const WCHAR **number)
{
    const WCHAR *character = (tag[0] == L'v' || tag[0] == L'V') ? tag + 1 : tag;
    int numbers = 0;
    BOOL inNumber = FALSE;
    *number = character;
    for (; *character; character++) {
        if (*character >= L'0' && *character <= L'9') {
            if (!inNumber) numbers++;
            inNumber = TRUE;
        } else if (*character == L'.' && inNumber) {
            inNumber = FALSE;
        } else {
            return FALSE;
        }
    }
    return inNumber && numbers <= VERSION_PARTS && Core_ParseVersion(*number, parts);
}

static BOOL IsReleaseTag(const WCHAR *tag)
{
    DWORD parts[VERSION_PARTS];
    const WCHAR *number;
    return ParseReleaseTag(tag, parts, &number);
}

static HANDLE TakeReadyDownload(void)
{
    return (HANDLE)InterlockedExchangePointer((PVOID volatile *)&g_readyDownload, NULL);
}

static void ReleaseReadyDownload(void)
{
    HANDLE locked = TakeReadyDownload();
    if (locked) CloseHandle(locked);
}

/* GET https://host/path, given up after `deadlineMs`: the body
 * (NUL-terminated, HeapFree it), or NULL once the reason is logged, with
 * Windows' error in `*error` (none for an HTTP status other than 200 or an
 * empty answer). */
static char *HttpGet(const WCHAR *host, const WCHAR *path, const WCHAR *headers, DWORD maxBytes, DWORD deadlineMs,
                     DWORD *size, DWORD *error)
{
    HINTERNET session, connection = NULL, request = NULL;
    DWORD status = 0, statusBytes = sizeof status, used = 0, capacity = 0, available, received;
    ULONGLONG deadline = GetTickCount64() + deadlineMs;
    char *body = NULL, *grown;
    BOOL complete = FALSE;

    *size = 0;
    *error = ERROR_SUCCESS;
    session = WinHttpOpen(APP_NAME L"/" APP_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session ||
        !WinHttpSetTimeouts(session, HTTP_RESOLVE_TIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS, HTTP_SEND_TIMEOUT_MS,
                            HTTP_RECEIVE_TIMEOUT_MS) ||
        (connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) == NULL ||
        (request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE)) == NULL ||
        !WinHttpSendRequest(request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) {
        *error = GetLastError();
    } else if (status == HTTP_STATUS_OK) {
        for (;;) {
            if (GetTickCount64() >= deadline) {
                *error = ERROR_TIMEOUT;
                break;
            }
            available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                *error = GetLastError();
                break;
            }
            if (available == 0) {
                complete = body != NULL;
                break;
            }
            if (available > maxBytes - used) {
                *error = ERROR_FILE_TOO_LARGE;
                break;
            }
            if (used + available + 1 > capacity) {
                capacity = used + available + 1 > capacity * 2 ? used + available + 1 : capacity * 2;
                if (capacity > maxBytes + 1) capacity = maxBytes + 1;
                grown = body ? (char *)HeapReAlloc(GetProcessHeap(), 0, body, capacity)
                             : (char *)HeapAlloc(GetProcessHeap(), 0, capacity);
                if (!grown) {
                    *error = ERROR_NOT_ENOUGH_MEMORY;
                    break;
                }
                body = grown;
            }
            if (!WinHttpReadData(request, body + used, available, &received)) {
                *error = GetLastError();
                break;
            }
            used += received;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!complete) {
        if (*error != ERROR_SUCCESS) Util_Log(L"GET https://%s%s failed (HTTP status %lu, error %lu)", host, path, status, *error);
        else if (status == HTTP_STATUS_OK) Util_Log(L"GET https://%s%s answered nothing", host, path);
        else Util_Log(L"GET https://%s%s answered HTTP status %lu", host, path, status);
        if (body) HeapFree(GetProcessHeap(), 0, body);
        return NULL;
    }
    body[used] = 0;
    *size = used;
    return body;
}

/* The tag, when the check found one, and the minute of the check, also after
 * a failure: offline or refused, the next request still waits CHECK_MINUTES.
 * A request that completes after an uninstall cannot recreate its state, nor
 * log (the log would make the state folder again): REG_ROOT is opened, not
 * created, and a deleted key takes no new subkey. */
static void CacheCheckResult(const WCHAR *tag)
{
    HKEY root, update;
    DWORD minute = MinutesSince1601();
    LSTATUS status;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, KEY_CREATE_SUB_KEY, &root) != ERROR_SUCCESS) return;
    status = RegCreateKeyExW(root, UPDATE_SUBKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &update, NULL);
    if (status == ERROR_SUCCESS) {
        if (tag[0] && (status = RegSetValueExW(update, TAG_VALUE, 0, REG_SZ, (const BYTE *)tag,
                                               (DWORD)((wcslen(tag) + 1) * sizeof(WCHAR)))) != ERROR_SUCCESS)
            Util_Log(L"could not keep the latest release %s (error %ld)", tag, status);
        if ((status = RegSetValueExW(update, CHECK_TIME_VALUE, 0, REG_DWORD, (const BYTE *)&minute, sizeof minute)) != ERROR_SUCCESS)
            Util_Log(L"could not keep the time of the release check, the next start asks again (error %ld)", status);
        RegCloseKey(update);
    } else if (status != ERROR_KEY_DELETED) {
        Util_Log(L"could not open %s to keep the release check (error %ld)", REG_UPDATE, status);
    }
    RegCloseKey(root);
}

static DWORD WINAPI CheckThread(void *argument)
{
    UpdateJob *job = (UpdateJob *)argument;
    const char *value;
    size_t valueLength;
    DWORD size, error;
    WCHAR tag[TAG_CCH] = L"";
    char *body = HttpGet(API_HOST, API_PATH, L"Accept: application/vnd.github+json\r\n", MAX_RELEASE_ANSWER, CHECK_DEADLINE_MS,
                         &size, &error);
    if (body) {
        if (!Core_JsonMember(body, size, "tag_name", &value, &valueLength) ||
            !Core_JsonString(value, valueLength, tag, ARRAYSIZE(tag)) || !IsReleaseTag(tag)) {
            Util_Log(L"the latest release has no version tag");
            tag[0] = 0;
        }
        HeapFree(GetProcessHeap(), 0, body);
    }
    CacheCheckResult(tag);
    PostMessageW(job->notify, job->message, 0, 0);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

/* FALSE: the last error says why the job did not start. */
static BOOL StartJob(LPTHREAD_START_ROUTINE routine, HWND notify, UINT message, const WCHAR *tag)
{
    HANDLE thread;
    DWORD error;
    UpdateJob *job = (UpdateJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *job);
    if (!job) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    job->notify = notify;
    job->message = message;
    if (tag && FAILED(StringCchCopyW(job->tag, ARRAYSIZE(job->tag), tag))) {
        HeapFree(GetProcessHeap(), 0, job);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    thread = CreateThread(NULL, 0, routine, job, 0, NULL);
    if (!thread) {
        error = GetLastError();
        HeapFree(GetProcessHeap(), 0, job);
        SetLastError(error);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

/* Asks GitHub for the latest release unless it was asked CHECK_MINUTES ago or
 * less: counted in whole minutes, the time since the last request is then
 * never shorter, and a clock set back asks at once. `message` reaches
 * `notify` once Update_Available can tell. */
void Update_Check(HWND notify, UINT message)
{
    DWORD checked;
    if (Util_RegGetDword(HKEY_CURRENT_USER, REG_UPDATE, CHECK_TIME_VALUE, &checked) &&
        MinutesSince1601() - checked <= CHECK_MINUTES) {
        PostMessageW(notify, message, 0, 0);
        return;
    }
    if (!StartJob(CheckThread, notify, message, NULL)) {
        Util_Log(L"could not start the release check (error %lu)", GetLastError());
        PostMessageW(notify, message, 0, 0);
    }
}

/* The version of a newer release ("1.2.0"), or FALSE when this one is the latest. */
BOOL Update_Available(WCHAR *version, size_t cch)
{
    WCHAR tag[TAG_CCH];
    DWORD latest[VERSION_PARTS];
    const WCHAR *number;
    return Util_RegGetString(HKEY_CURRENT_USER, REG_UPDATE, TAG_VALUE, tag, ARRAYSIZE(tag)) && ParseReleaseTag(tag, latest, &number) &&
           Core_CompareVersions(latest, kRunningVersion) > 0 && SUCCEEDED(StringCchCopyW(version, cch, number));
}

/* The common name (CN) of a certificate's subject, or with
 * CERT_NAME_ISSUER_FLAG of its issuer; "?" when it has none. Unlike the
 * display name, it never stands in another attribute (O, OU, email). */
static void CertificateCommonName(PCCERT_CONTEXT certificate, DWORD flags, WCHAR *out, DWORD cch)
{
    if (CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, flags, (void *)szOID_COMMON_NAME, out, cch) <= 1)
        StringCchCopyW(out, cch, L"?");
}

/* Whether Windows finds the opened file signed by APP_SIGNER: the signature,
 * the certificate chain up to a trusted root (an expired certificate still
 * counts when the signature was timestamped), that no certificate of it was
 * revoked, and the signer's common name. The release comes over HTTPS
 * already; this also holds when the release itself was replaced.
 * UPDATE_NOT_VERIFIED: Windows could not read the revocation lists, `*error`
 * is its answer. */
static UpdateResult CheckSignature(const WCHAR *file, HANDLE opened, DWORD *error)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO info;
    WINTRUST_DATA trust;
    CRYPT_PROVIDER_DATA *provider;
    CRYPT_PROVIDER_SGNR *signer;
    CRYPT_PROVIDER_CERT *certificate;
    WCHAR subject[128], issuer[128];
    LONG status;
    UpdateResult result = UPDATE_NOT_SIGNED;

    ZeroMemory(&info, sizeof info);
    info.cbStruct = sizeof info;
    info.pcwszFilePath = file;
    info.hFile = opened;
    ZeroMemory(&trust, sizeof trust);
    trust.cbStruct = sizeof trust;
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    status = WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &trust);
    if (status != ERROR_SUCCESS) {
        Util_Log(L"the download's signature does not verify (0x%08lX)", (unsigned long)status);
        /* Unreachable revocation lists say nothing about the file. */
        if (status == CERT_E_REVOCATION_FAILURE || status == CRYPT_E_REVOCATION_OFFLINE || status == CRYPT_E_NO_REVOCATION_CHECK) {
            result = UPDATE_NOT_VERIFIED;
            *error = (DWORD)status;
        }
    } else if ((provider = WTHelperProvDataFromStateData(trust.hWVTStateData)) == NULL ||
               (signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) == NULL ||
               (certificate = WTHelperGetProvCertFromChain(signer, 0)) == NULL) {
        Util_Log(L"the download's signer could not be read");
    } else {
        CertificateCommonName(certificate->pCert, 0, subject, ARRAYSIZE(subject));
        CertificateCommonName(certificate->pCert, CERT_NAME_ISSUER_FLAG, issuer, ARRAYSIZE(issuer));
        Util_Log(L"the download is signed by %s, issued by %s", subject, issuer);
        if (CompareStringOrdinal(subject, -1, APP_SIGNER, -1, FALSE) == CSTR_EQUAL) result = UPDATE_READY;
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &trust);
    return result;
}

/* The version an exe declares (its VS_FIXEDFILEINFO), read as a resource:
 * nothing of the file runs. */
static BOOL FileVersion(const WCHAR *file, DWORD parts[VERSION_PARTS])
{
    static const WCHAR kVersionKey[] = L"VS_VERSION_INFO";
    const size_t fixedOffset = (3 * sizeof(WORD) + sizeof kVersionKey + 3) & ~(size_t)3;
    HMODULE image = LoadLibraryExW(file, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    HRSRC found;
    HGLOBAL loaded;
    const BYTE *data = NULL;
    DWORD size = 0;
    WORD valueBytes;
    VS_FIXEDFILEINFO fixed;
    BOOL ok = FALSE;
    if (!image) return FALSE;
    found = FindResourceW(image, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (found && (loaded = LoadResource(image, found)) != NULL) {
        data = (const BYTE *)LockResource(loaded);
        size = SizeofResource(image, found);
    }
    if (data && size >= fixedOffset + sizeof fixed) {
        memcpy(&valueBytes, data + sizeof(WORD), sizeof valueBytes);
        memcpy(&fixed, data + fixedOffset, sizeof fixed);
        if (valueBytes >= sizeof fixed && memcmp(data + 3 * sizeof(WORD), kVersionKey, sizeof kVersionKey) == 0 &&
            fixed.dwSignature == VS_FFI_SIGNATURE) {
            parts[0] = HIWORD(fixed.dwFileVersionMS);
            parts[1] = LOWORD(fixed.dwFileVersionMS);
            parts[2] = HIWORD(fixed.dwFileVersionLS);
            parts[3] = LOWORD(fixed.dwFileVersionLS);
            ok = TRUE;
        }
    }
    FreeLibrary(image);
    return ok;
}

/* A signed release older than this copy, or published under another tag,
 * would install a build the user did not ask for. */
static BOOL IsReleaseVersion(const WCHAR *file, const WCHAR *tag)
{
    DWORD released[VERSION_PARTS], declared[VERSION_PARTS];
    if (!Core_ParseVersion(tag, released) || !FileVersion(file, declared)) {
        Util_Log(L"the download declares no version");
        return FALSE;
    }
    if (Core_CompareVersions(declared, released) == 0 && Core_CompareVersions(declared, kRunningVersion) > 0) return TRUE;
    Util_Log(L"the download is version %lu.%lu.%lu.%lu, not release %s", declared[0], declared[1], declared[2], declared[3], tag);
    return FALSE;
}

/* The download, opened so that nothing can change or delete it until it
 * runs, then checked. UPDATE_READY keeps it open for Update_Run; otherwise it
 * is deleted. */
static UpdateResult PrepareDownload(const WCHAR *file, const WCHAR *tag, DWORD *error)
{
    HANDLE locked = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    UpdateResult result;
    if (locked == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        Util_Log(L"could not open the download %s (error %lu)", file, *error);
        DeleteFileW(file);
        return UPDATE_NOT_DOWNLOADED;
    }
    result = CheckSignature(file, locked, error);
    if (result == UPDATE_READY && !IsReleaseVersion(file, tag)) result = UPDATE_WRONG_VERSION;
    if (result == UPDATE_READY) {
        ReleaseReadyDownload();
        InterlockedExchangePointer((PVOID volatile *)&g_readyDownload, locked);
        return UPDATE_READY;
    }
    CloseHandle(locked);
    DeleteFileW(file);
    return result;
}

/* The downloaded bytes in `file`. FALSE: nothing is left of it, `*error` says
 * why. */
static BOOL SaveDownload(const WCHAR *file, const char *body, DWORD size, DWORD *error)
{
    HANDLE output = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0, failure = ERROR_SUCCESS;
    if (output == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        Util_Log(L"could not save the download to %s (error %lu)", file, *error);
        return FALSE;
    }
    if (!WriteFile(output, body, size, &written, NULL)) failure = GetLastError();
    else if (written != size) failure = ERROR_WRITE_FAULT;
    CloseHandle(output);
    if (failure == ERROR_SUCCESS) return TRUE;
    *error = failure;
    Util_Log(L"could not save the download to %s (error %lu)", file, failure);
    DeleteFileW(file);
    return FALSE;
}

static DWORD WINAPI DownloadThread(void *argument)
{
    UpdateJob *job = (UpdateJob *)argument;
    WCHAR path[ARRAYSIZE(DOWNLOAD_PATH_START) + TAG_CCH + ARRAYSIZE(DOWNLOAD_PATH_END)], file[MAX_PATH];
    DWORD size = 0, error = ERROR_SUCCESS;
    char *body = NULL;
    UpdateResult result = UPDATE_NOT_DOWNLOADED;
    /* A download that was not run holds the file. */
    ReleaseReadyDownload();
    if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), DOWNLOAD_PATH_START L"%s" DOWNLOAD_PATH_END, job->tag)) &&
        Update_DownloadPath(file, ARRAYSIZE(file)))
        body = HttpGet(DOWNLOAD_HOST, path, NULL, MAX_DOWNLOAD, DOWNLOAD_DEADLINE_MS, &size, &error);
    if (body && (size <= MIN_DOWNLOAD || body[0] != 'M' || body[1] != 'Z')) {
        Util_Log(L"the download is not a program (%lu bytes)", size);
    } else if (body && SaveDownload(file, body, size, &error)) {
        result = PrepareDownload(file, job->tag, &error);
        /* The uninstall deletes REG_ROOT before the download (Install_Uninstall):
         * a download it could not see yet is deleted here. */
        if (result == UPDATE_READY && !Util_RegKeyExists(HKEY_CURRENT_USER, REG_ROOT)) {
            ReleaseReadyDownload();
            DeleteFileW(file);
            result = UPDATE_NOT_DOWNLOADED;
        }
    }
    if (body) HeapFree(GetProcessHeap(), 0, body);
    PostMessageW(job->notify, job->message, (WPARAM)result, (LPARAM)error);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

/* Downloads the newer release and checks it; `message` reaches `notify` with
 * an UpdateResult in wParam and Windows' error, if any, in lParam. */
void Update_Download(HWND notify, UINT message)
{
    WCHAR tag[TAG_CCH];
    if (!Util_RegGetString(HKEY_CURRENT_USER, REG_UPDATE, TAG_VALUE, tag, ARRAYSIZE(tag)) || !IsReleaseTag(tag))
        PostMessageW(notify, message, (WPARAM)UPDATE_NOT_DOWNLOADED, ERROR_SUCCESS);
    else if (!StartJob(DownloadThread, notify, message, tag))
        PostMessageW(notify, message, (WPARAM)UPDATE_NOT_DOWNLOADED, (LPARAM)GetLastError());
}

/* Runs the verified download: it installs itself and closes this manager. The
 * user asked for it, so its windows may come to the front. UPDATE_NOT_STARTED:
 * `*error` says why, and the download is deleted. */
UpdateResult Update_Run(DWORD *error)
{
    WCHAR file[MAX_PATH];
    HANDLE locked = TakeReadyDownload();
    BOOL started;
    *error = ERROR_SUCCESS;
    if (!locked) return UPDATE_NOT_DOWNLOADED;
    if (!Update_DownloadPath(file, ARRAYSIZE(file))) {
        CloseHandle(locked);
        return UPDATE_NOT_DOWNLOADED;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    started = Util_Spawn(file, L"--install", NULL);
    if (!started) *error = GetLastError();
    /* Windows has opened the image it runs: the file can be let go. */
    CloseHandle(locked);
    if (started) return UPDATE_STARTED;
    Util_Log(L"could not start the download %s (error %lu)", file, *error);
    DeleteFileW(file);
    return UPDATE_NOT_STARTED;
}

/* The download left by an update the manager did not run, or that could not
 * remove itself once installed (Install_Run). */
void Update_RemoveDownload(void)
{
    WCHAR file[MAX_PATH], self[MAX_PATH];
    ReleaseReadyDownload();
    if (Update_DownloadPath(file, ARRAYSIZE(file)) && !(Util_SelfExe(self, ARRAYSIZE(self)) && Core_PathEquals(self, file)))
        DeleteFileW(file);
}
