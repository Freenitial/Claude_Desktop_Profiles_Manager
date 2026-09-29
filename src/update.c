/*
 * New releases. When the manager opens, at most every CHECK_HOURS, the tag of
 * the latest GitHub release is compared with this version; nothing else is
 * sent and nothing runs on a timer. Updating downloads that release's exe and,
 * once Windows has checked that it is signed by APP_SIGNER, runs it: it
 * installs itself over this copy, as a manual download would.
 */
#include "app.h"
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>

#define API_HOST     L"api.github.com"
#define API_PATH     L"/repos/" APP_REPO L"/releases/latest"
#define SITE_HOST    L"github.com"
#define CHECK_HOURS  4
#define MAX_ANSWER   (256 * 1024)
#define MAX_DOWNLOAD (32 * 1024 * 1024)
#define TAG_CCH      32

typedef struct Job {
    HWND  notify;
    UINT  msg;
    WCHAR tag[TAG_CCH];
} Job;

static DWORD NowHours(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (DWORD)(u.QuadPart / 36000000000ULL);
}

static BOOL DownloadPath(WCHAR *out, size_t cch)
{
    WCHAR temp[MAX_PATH];
    DWORD n = GetTempPathW(ARRAYSIZE(temp), temp);
    return n > 0 && n < ARRAYSIZE(temp) && SUCCEEDED(StringCchPrintfW(out, cch, L"%supdate-" APP_EXE, temp));
}

/* GET https://host/path: the body (NUL-terminated, HeapFree it), or NULL. */
static char *HttpGet(const WCHAR *host, const WCHAR *path, const WCHAR *headers, DWORD max, DWORD *size)
{
    HINTERNET session, connection = NULL, request = NULL;
    DWORD status = 0, statusLen = sizeof status, used = 0, cap = 0, avail, got;
    char *body = NULL, *grown;
    BOOL ok = FALSE;

    *size = 0;
    session = WinHttpOpen(APP_NAME L"/" APP_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return NULL;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection)
        request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (request &&
        WinHttpSendRequest(request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &statusLen, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        for (;;) {
            avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) break;
            if (avail == 0) {
                ok = body != NULL;
                break;
            }
            if (avail > max - used) break;
            if (used + avail + 1 > cap) {
                cap = used + avail + 1 > cap * 2 ? used + avail + 1 : cap * 2;
                if (cap > max + 1) cap = max + 1;
                grown = body ? (char *)HeapReAlloc(GetProcessHeap(), 0, body, cap) : (char *)HeapAlloc(GetProcessHeap(), 0, cap);
                if (!grown) break;
                body = grown;
            }
            if (!WinHttpReadData(request, body + used, avail, &got)) break;
            used += got;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok) {
        if (body) HeapFree(GetProcessHeap(), 0, body);
        return NULL;
    }
    body[used] = 0;
    *size = used;
    return body;
}

static DWORD WINAPI CheckThread(void *arg)
{
    Job *job = (Job *)arg;
    const char *value;
    size_t valueLen;
    DWORD size;
    WCHAR tag[TAG_CCH];
    int chars;
    char *body = HttpGet(API_HOST, API_PATH, L"Accept: application/vnd.github+json\r\n", MAX_ANSWER, &size);
    if (body) {
        if (Core_JsonMember(body, size, "tag_name", &value, &valueLen) && valueLen > 2 && valueLen < TAG_CCH && value[0] == '"' &&
            (chars = MultiByteToWideChar(CP_UTF8, 0, value + 1, (int)valueLen - 2, tag, TAG_CCH - 1)) > 0) {
            tag[chars] = 0;
            Util_RegSetString(HKEY_CURRENT_USER, REG_ROOT, L"LatestRelease", tag);
        }
        Util_RegSetDword(HKEY_CURRENT_USER, REG_ROOT, L"ReleaseCheckHour", NowHours());
        HeapFree(GetProcessHeap(), 0, body);
    }
    PostMessageW(job->notify, job->msg, 0, 0);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

static void Start(LPTHREAD_START_ROUTINE routine, HWND notify, UINT msg, const WCHAR *tag)
{
    HANDLE thread;
    Job *job = (Job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *job);
    if (!job) return;
    job->notify = notify;
    job->msg = msg;
    if (tag) StringCchCopyW(job->tag, ARRAYSIZE(job->tag), tag);
    thread = CreateThread(NULL, 0, routine, job, 0, NULL);
    if (thread) CloseHandle(thread);
    else HeapFree(GetProcessHeap(), 0, job);
}

/* Asks GitHub for the latest release unless it was asked less than CHECK_HOURS
 * ago; `msg` reaches `notify` once Update_Available can tell. */
void Update_Check(HWND notify, UINT msg)
{
    DWORD hour = 0;
    if (Util_RegGetDword(HKEY_CURRENT_USER, REG_ROOT, L"ReleaseCheckHour", &hour) && NowHours() - hour < CHECK_HOURS) {
        PostMessageW(notify, msg, 0, 0);
        return;
    }
    Start(CheckThread, notify, msg, NULL);
}

/* The version of a newer release ("1.2.0"), or FALSE when this one is the latest. */
BOOL Update_Available(WCHAR *version, size_t cch)
{
    WCHAR tag[TAG_CCH];
    DWORD latest[4], self[4] = { APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, 0 };
    if (!Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, L"LatestRelease", tag, ARRAYSIZE(tag)) ||
        !Core_ParseVersion(tag, latest) || Core_CompareVersions(latest, self) <= 0)
        return FALSE;
    return SUCCEEDED(StringCchCopyW(version, cch, (tag[0] == L'v' || tag[0] == L'V') ? tag + 1 : tag));
}

/* Whether `file` is signed by APP_SIGNER: Windows checks the signature, the
 * certificate chain up to a trusted root (an expired certificate still counts
 * when the signature was timestamped) and that no certificate of it was
 * revoked. The release comes over HTTPS already; this also holds when the
 * release itself was replaced. */
static BOOL SignedByAuthor(const WCHAR *file)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO info;
    WINTRUST_DATA trust;
    CRYPT_PROVIDER_DATA *data;
    CRYPT_PROVIDER_SGNR *signer;
    CRYPT_PROVIDER_CERT *cert;
    WCHAR name[128];
    LONG status;
    BOOL ok = FALSE;

    ZeroMemory(&info, sizeof info);
    info.cbStruct = sizeof info;
    info.pcwszFilePath = file;
    ZeroMemory(&trust, sizeof trust);
    trust.cbStruct = sizeof trust;
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    status = WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &trust);
    if (status == ERROR_SUCCESS && (data = WTHelperProvDataFromStateData(trust.hWVTStateData)) != NULL &&
        (signer = WTHelperGetProvSignerFromChain(data, 0, FALSE, 0)) != NULL &&
        (cert = WTHelperGetProvCertFromChain(signer, 0)) != NULL &&
        CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, ARRAYSIZE(name)) > 1) {
        ok = CompareStringOrdinal(name, -1, APP_SIGNER, -1, FALSE) == CSTR_EQUAL;
        if (!ok) Util_Log(L"the download is signed by %s, not by " APP_SIGNER, name);
    } else {
        Util_Log(L"the download's signature does not verify (0x%08lX)", (unsigned long)status);
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &trust);
    return ok;
}

static DWORD WINAPI DownloadThread(void *arg)
{
    Job *job = (Job *)arg;
    WCHAR path[128], file[MAX_PATH];
    DWORD size = 0, written = 0;
    HANDLE h;
    char *body = NULL;
    BOOL ok = FALSE;
    UpdateResult result = UPDATE_NOT_DOWNLOADED;
    if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"/" APP_REPO L"/releases/download/%s/" APP_EXE, job->tag)) &&
        DownloadPath(file, ARRAYSIZE(file)))
        body = HttpGet(SITE_HOST, path, NULL, MAX_DOWNLOAD, &size);
    if (body && size > 64 * 1024 && body[0] == 'M' && body[1] == 'Z') {
        h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            ok = WriteFile(h, body, size, &written, NULL) && written == size;
            CloseHandle(h);
            if (!ok) DeleteFileW(file);
        }
    }
    if (body) HeapFree(GetProcessHeap(), 0, body);
    Util_Log(ok ? L"downloaded release %s" : L"could not download release %s", job->tag);
    if (ok) {
        result = SignedByAuthor(file) ? UPDATE_READY : UPDATE_NOT_SIGNED;
        if (result == UPDATE_NOT_SIGNED) DeleteFileW(file);
    }
    PostMessageW(job->notify, job->msg, (WPARAM)result, 0);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

/* Downloads the newer release and checks its signature; `msg` reaches
 * `notify` with an UpdateResult in wParam. */
void Update_Download(HWND notify, UINT msg)
{
    WCHAR tag[TAG_CCH];
    if (!Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, L"LatestRelease", tag, ARRAYSIZE(tag))) {
        PostMessageW(notify, msg, (WPARAM)UPDATE_NOT_DOWNLOADED, 0);
        return;
    }
    Start(DownloadThread, notify, msg, tag);
}

/* Runs the downloaded release: it installs itself and closes this manager. The
 * user asked for it, so its windows may come to the front. */
BOOL Update_Run(void)
{
    WCHAR file[MAX_PATH];
    if (!DownloadPath(file, ARRAYSIZE(file))) return FALSE;
    AllowSetForegroundWindow(ASFW_ANY);
    return Util_Spawn(file, L"--install");
}

/* The download left by an update, once installed. */
void Update_RemoveDownload(void)
{
    WCHAR file[MAX_PATH], self[MAX_PATH];
    if (DownloadPath(file, ARRAYSIZE(file)) && !(Util_SelfExe(self, ARRAYSIZE(self)) && Core_PathEquals(self, file)))
        DeleteFileW(file);
}
