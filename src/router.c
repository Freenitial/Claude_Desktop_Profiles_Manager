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
#include <shellapi.h>
#include <wchar.h>

static void Redact(const WCHAR *url, WCHAR *out, size_t cch)
{
    const WCHAR *cut = wcspbrk(url, L"?#");
    if (cut)
        StringCchPrintfW(out, cch, L"%.*s?...", (int)(cut - url), url);
    else
        StringCchCopyW(out, cch, url);
}

static void OpenManager(void)
{
    WCHAR exe[MAX_PATH];
    if (Util_InstallExe(exe, ARRAYSIZE(exe)) && Util_FileExists(exe))
        ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL);
}

static BOOL RequirePackage(ClaudePackage *pkg)
{
    if (Claude_FindPackage(pkg)) return TRUE;
    Util_Log(L"Claude Desktop is not installed");
    if (Util_Message(NULL, MB_ICONWARNING | MB_YESNO,
                     L"Claude Desktop is not installed.\n\nOpen the download page?") == IDYES)
        Util_OpenUrl(APP_DOWNLOAD_URL);
    return FALSE;
}

/* Opens a profile, with a claude:// link or without, and watches it. A
 * profile about to start first gets the session changes waiting for it
 * (sessionedit.c): its Claude reads its sessions as it starts. */
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity)
{
    HRESULT hr;
    if (!p->running) SessionEdit_ApplyPending(p);
    hr = Claude_Launch(pkg, p, url, pid, identity);
    if (SUCCEEDED(hr)) Taskbar_Watch(p);
    return hr;
}

int Launcher_Run(const WCHAR *folder)
{
    ProfileList list;
    ClaudePackage pkg;
    BOOL identity = FALSE;
    DWORD pid = 0;
    HRESULT hr;
    int i;

    Profiles_Load(&list);
    i = Profiles_Find(&list, folder);
    if (i < 0) {
        if (Util_Message(NULL, MB_ICONWARNING | MB_YESNO,
                         L"This shortcut opens the Claude profile \x201C%s\x201D, which no longer exists.\n\nOpen " APP_NAME L"?",
                         folder) == IDYES)
            OpenManager();
        return 1;
    }
    if (!RequirePackage(&pkg)) return 1;
    hr = Launcher_Open(&pkg, &list.items[i], NULL, &pid, &identity);
    if (FAILED(hr)) {
        Util_Log(L"could not open %s (0x%08lX)", list.items[i].folder, (unsigned long)hr);
        Util_Message(NULL, MB_ICONERROR, L"Claude could not be started (error 0x%08lX).", (unsigned long)hr);
        return 1;
    }
    Util_Log(L"opened %s (pid %lu)%s", list.items[i].folder, pid, identity ? L"" : L" without package identity");
    return 0;
}

int Router_Run(const WCHAR *rawUrl)
{
    WCHAR url[URL_CCH], shown[160];
    ProfileList list;
    ClaudePackage pkg;
    ULONGLONG signIn[MAX_PROFILES];
    int running[MAX_PROFILES], targets[MAX_PROFILES];
    int n = 0, t, i, top, def, topPos = -1, defPos = -1;
    BOOL signInUrl;
    RouteReason reason;

    if (!Core_SanitizeUrl(rawUrl, url, ARRAYSIZE(url))) {
        Util_Log(L"ignored a link that is not a claude:// link");
        return 1;
    }
    Redact(url, shown, ARRAYSIZE(shown));
    if (!RequirePackage(&pkg)) return 1;

    Profiles_Load(&list);
    signInUrl = Core_IsSignInUrl(url);
    top = Claude_TopmostProfile(&list);
    def = Profiles_DefaultIndex(&list);
    for (i = 0; i < list.count; i++) {
        if (!list.items[i].running) continue;
        if (i == top) topPos = n;
        if (i == def) defPos = n;
        signIn[n] = 0;
        if (signInUrl) Claude_LastSignInStart(&pkg, &list.items[i], &signIn[n]);
        running[n++] = i;
    }

    t = Core_SelectTargets(n, signIn, Util_LocalNowTicks(), SIGNIN_WINDOW_MIN * 60ULL * 10000000ULL,
                           signInUrl, topPos, defPos, targets, &reason);
    if (t == 0) {
        running[0] = def;
        targets[0] = 0;
        t = 1;
    }
    for (i = 0; i < t; i++) {
        const Profile *p = &list.items[running[targets[i]]];
        BOOL identity = FALSE;
        HRESULT hr = Launcher_Open(&pkg, p, url, NULL, &identity);
        Util_Log(L"link %s -> %s (%s)%s%s", shown, p->folder, Core_RouteReasonText(reason),
                 SUCCEEDED(hr) ? L"" : L" FAILED", identity ? L"" : L" without package identity");
    }
    return 0;
}
