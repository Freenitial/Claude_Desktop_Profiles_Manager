/*
 * --launch and --url.
 *
 * A sign-in started in one Claude window finishes in the browser, which hands a
 * claude:// link back to Windows. Claude only accepts that link in the window
 * that opened the browser; any other window logs "Google sign-in code does not
 * answer a sign-in this app started; ignoring". So a sign-in link goes to the
 * window whose main.log most recently says "[Auth] Using system browser for:";
 * when no window claims it, every window gets it and each checks it against its
 * own pending sign-in. Any other link goes to the Claude window used last.
 */
#include "app.h"
#include <wchar.h>

#define REDACTED_URL_CCH 160   /* the start of a link, enough to tell it in the log */

/* The link without its query or fragment, which can hold a sign-in code. */
static void Redact(const WCHAR *url, WCHAR *out, size_t cch)
{
    const WCHAR *cut = wcspbrk(url, L"?#");
    if (cut)
        StringCchPrintfW(out, cch, L"%.*s%c...", (int)(cut - url), url, *cut);
    else
        StringCchCopyW(out, cch, url);
}

static const WCHAR *RouteReasonText(RouteReason reason)
{
    switch (reason) {
    case ROUTE_NOTHING_RUNNING: return L"no Claude window open: default profile";
    case ROUTE_ONLY_ONE:        return L"only open window";
    case ROUTE_SIGNIN:          return L"window that started the sign-in";
    case ROUTE_BROADCAST:       return L"sign-in link claimed by no window: sent to every window";
    case ROUTE_LAST_USED:       return L"last used window";
    case ROUTE_DEFAULT:         return L"default profile";
    default:                    return L"first open window";
    }
}

/* The manager, from this same program. */
static void OpenManager(void)
{
    WCHAR exe[MAX_PATH];
    if (!Util_SelfExe(exe, ARRAYSIZE(exe)) || !Util_Spawn(exe, L"", NULL))
        Util_Log(L"could not open the manager (error %lu)", GetLastError());
}

static void ReportMissingClaude(void)
{
    Util_Log(L"Claude Desktop is not installed");
    if (Ui_Message(NULL, MB_ICONWARNING | MB_YESNO, TR(L"Claude Desktop is not installed.\n\nOpen the download page?")) == IDYES)
        Util_OpenUrl(APP_DOWNLOAD_URL);
}

/* Opens a profile, with a claude:// link or without, and watches it. Whether
 * its Claude runs is looked at again: the caller's profile can be older, and
 * one quit a moment ago must start with a watcher of its own. A profile about
 * to start first gets the session changes waiting for it (sessionedit.c): its
 * Claude reads its sessions as it starts. */
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity)
{
    Profile watched = *p;
    DWORD launched = 0;
    HRESULT hr;
    Claude_RefreshRunning(&watched);
    if (!watched.running) SessionEdit_ApplyPending(NULL, p);
    hr = Claude_Launch(pkg, p, url, &launched, identity);
    if (pid) *pid = launched;
    if (SUCCEEDED(hr)) {
        if (!watched.running) watched.pid = launched;   /* the watcher waits for this process only */
        if (!watched.running || !Taskbar_IsWatched(&watched)) Taskbar_Watch(&watched);
    }
    return hr;
}

int Launcher_Run(const WCHAR *folder)
{
    ProfileList list;
    ClaudePackage pkg;
    BOOL identity = FALSE, havePackage = Claude_FindPackage(&pkg);
    DWORD pid = 0;
    HRESULT hr;
    int i;

    Profiles_Load(&list, &pkg);
    i = Profiles_Find(&list, folder);
    if (i < 0) {
        /* Its name went with it: the folder, without the prefix, is the name it had by default. */
        const WCHAR *name = Core_IsProfileFolder(folder) ? folder + wcslen(PROFILE_PREFIX) : folder;
        Util_Log(L"shortcut for %s, which no longer exists", folder);
        if (Ui_Message(NULL, MB_ICONWARNING | MB_YESNO,
                       TR(L"This shortcut opens the Claude profile \x201C%s\x201D, which no longer exists.\n\nOpen " APP_NAME L"?"),
                       name) == IDYES)
            OpenManager();
        return 1;
    }
    if (!havePackage) {
        ReportMissingClaude();
        return 1;
    }
    hr = Launcher_Open(&pkg, &list.items[i], NULL, &pid, &identity);
    if (FAILED(hr)) {
        Util_Log(L"could not open %s (0x%08lX)", list.items[i].folder, (unsigned long)hr);
        Ui_Message(NULL, MB_ICONERROR, TR(L"Claude could not be started (error 0x%08lX)."), (unsigned long)hr);
        return 1;
    }
    Util_Log(L"opened %s (pid %lu)%s", list.items[i].folder, pid, identity ? L"" : L" without package identity");
    return 0;
}

int Router_Run(const WCHAR *rawUrl)
{
    WCHAR url[URL_CCH], redactedUrl[REDACTED_URL_CCH];
    ProfileList list;
    ClaudePackage pkg;
    ULONGLONG signIn[MAX_PROFILES];
    int running[MAX_PROFILES], targets[MAX_PROFILES];
    int runningCount = 0, targetCount, i, topmostIndex, defaultIndex, topmostPosition = -1, defaultPosition = -1;
    BOOL signInUrl, delivered = FALSE;
    HRESULT lastFailure = S_OK;
    RouteReason reason;

    if (!Core_SanitizeUrl(rawUrl, url, ARRAYSIZE(url))) {
        Util_Log(L"ignored a link that is not a claude:// link");
        return 1;
    }
    Redact(url, redactedUrl, ARRAYSIZE(redactedUrl));
    if (!Claude_FindPackage(&pkg)) {
        ReportMissingClaude();
        return 1;
    }

    Profiles_Load(&list, &pkg);
    signInUrl = Core_IsSignInUrl(url);
    topmostIndex = Claude_TopmostProfile(&list);
    defaultIndex = Profiles_DefaultIndex(&list);
    if (defaultIndex < 0) {
        Util_Log(L"link %s: no profile found", redactedUrl);
        return 1;
    }
    for (i = 0; i < list.count; i++) {
        if (!list.items[i].running) continue;
        if (i == topmostIndex) topmostPosition = runningCount;
        if (i == defaultIndex) defaultPosition = runningCount;
        running[runningCount++] = i;
    }
    /* Reading the logs matters only when several windows could claim the link. */
    for (i = 0; i < runningCount; i++) {
        signIn[i] = 0;
        if (signInUrl && runningCount > 1) Claude_LastSignInStart(&pkg, &list.items[running[i]], &signIn[i]);
    }

    targetCount = Core_SelectTargets(runningCount, signIn, Util_LocalNowTicks(), SIGNIN_MAX_AGE_MINUTES * 60 * TICKS_PER_SECOND,
                                     signInUrl, topmostPosition, defaultPosition, targets, &reason);
    if (targetCount == 0) {
        running[0] = defaultIndex;
        targets[0] = 0;
        targetCount = 1;
    }
    for (i = 0; i < targetCount; i++) {
        const Profile *p = &list.items[running[targets[i]]];
        BOOL identity = FALSE;
        lastFailure = Launcher_Open(&pkg, p, url, NULL, &identity);
        if (SUCCEEDED(lastFailure)) {
            delivered = TRUE;
            Util_Log(L"link %s -> %s (%s)%s", redactedUrl, p->folder, RouteReasonText(reason),
                     identity ? L"" : L" without package identity");
        } else {
            Util_Log(L"link %s -> %s (%s) FAILED (0x%08lX)", redactedUrl, p->folder, RouteReasonText(reason),
                     (unsigned long)lastFailure);
        }
    }
    if (delivered) return 0;
    Ui_Message(NULL, MB_ICONERROR, TR(L"Claude could not be started (error 0x%08lX)."), (unsigned long)lastFailure);
    return 1;
}
