/*
 * Unit tests for src/core.c (the pure helpers). Built and run by build.cmd;
 * exits non-zero when a check fails.
 */
#include "../src/app.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_failures = 0, g_checks = 0;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

static void CheckStr(const char *name, const WCHAR *expected, const WCHAR *actual)
{
    BOOL ok = wcscmp(expected, actual) == 0;
    Check(name, ok);
    if (!ok) wprintf(L"        expected: [%s]\n        actual:   [%s]\n", expected, actual);
}

static ULONGLONG At(WORD h, WORD m, WORD s)
{
    SYSTEMTIME st;
    ZeroMemory(&st, sizeof st);
    st.wYear = 2026; st.wMonth = 9; st.wDay = 21;
    st.wHour = h; st.wMinute = m; st.wSecond = s;
    return Core_SystemTimeTicks(&st);
}

static void TestLaunchArgs(void)
{
    WCHAR out[ARGS_CCH];
    const WCHAR *url = L"claude://login/google-auth?code=abc";
    const WCHAR *spacey = L"C:\\Users\\John Doe\\AppData\\Roaming\\Claude-Client A";

    Check("stock, no link -> no argument", Core_BuildLaunchArgs(NULL, NULL, out, ARRAYSIZE(out)));
    CheckStr("stock, no link", L"", out);
    Core_BuildLaunchArgs(NULL, url, out, ARRAYSIZE(out));
    CheckStr("stock + link -> quoted link only", L"\"claude://login/google-auth?code=abc\"", out);
    Core_BuildLaunchArgs(spacey, NULL, out, ARRAYSIZE(out));
    CheckStr("path with spaces stays one quoted token",
             L"--user-data-dir=\"C:\\Users\\John Doe\\AppData\\Roaming\\Claude-Client A\"", out);
    Core_BuildLaunchArgs(spacey, url, out, ARRAYSIZE(out));
    CheckStr("path + link",
             L"--user-data-dir=\"C:\\Users\\John Doe\\AppData\\Roaming\\Claude-Client A\" \"claude://login/google-auth?code=abc\"", out);
    Core_BuildLaunchArgs(L"C:\\Users\\me\\AppData\\Roaming\\Claude-Work\\", NULL, out, ARRAYSIZE(out));
    CheckStr("trailing backslash cannot escape the closing quote",
             L"--user-data-dir=\"C:\\Users\\me\\AppData\\Roaming\\Claude-Work\"", out);
    Check("a quote in the link is refused", !Core_BuildLaunchArgs(NULL, L"claude://x\" --inspect \"", out, ARRAYSIZE(out)));
    Check("a backslash in the link is refused", !Core_BuildLaunchArgs(NULL, L"claude://x\\", out, ARRAYSIZE(out)));
    Check("too small a buffer fails", !Core_BuildLaunchArgs(spacey, url, out, 20));
}

static void TestSanitizeUrl(void)
{
    WCHAR out[URL_CCH], big[URL_CCH + 16];
    int i;

    Check("claude:// accepted", Core_SanitizeUrl(L"claude://login/google-auth?code=4%2F0A&x=1", out, ARRAYSIZE(out)));
    CheckStr("claude:// untouched", L"claude://login/google-auth?code=4%2F0A&x=1", out);
    Check("scheme is case-insensitive", Core_SanitizeUrl(L"CLAUDE://x", out, ARRAYSIZE(out)));
    Check("surrounding spaces trimmed", Core_SanitizeUrl(L"  claude://x  ", out, ARRAYSIZE(out)) && wcscmp(out, L"claude://x") == 0);
    Check("http refused", !Core_SanitizeUrl(L"https://claude.ai", out, ARRAYSIZE(out)));
    Check("look-alike scheme refused", !Core_SanitizeUrl(L"claudex://x", out, ARRAYSIZE(out)));
    Check("bare scheme refused", !Core_SanitizeUrl(L"claude:", out, ARRAYSIZE(out)));
    Check("empty refused", !Core_SanitizeUrl(L"", out, ARRAYSIZE(out)));
    Core_SanitizeUrl(L"claude://a\" --inspect=9229 \"b", out, ARRAYSIZE(out));
    CheckStr("quotes and spaces are percent-encoded", L"claude://a%22%20--inspect=9229%20%22b", out);
    Core_SanitizeUrl(L"claude://a\\b\\", out, ARRAYSIZE(out));
    CheckStr("backslashes are percent-encoded", L"claude://a%5Cb%5C", out);
    Core_SanitizeUrl(L"claude://a\tb", out, ARRAYSIZE(out));
    CheckStr("control characters are percent-encoded", L"claude://a%09b", out);
    Check("non-breaking space refused", !Core_SanitizeUrl(L"claude://a\x00A0" L"b", out, ARRAYSIZE(out)));
    Core_SanitizeUrl(L"claude://caf\x00E9", out, ARRAYSIZE(out));
    CheckStr("non-ASCII kept", L"claude://caf\x00E9", out);
    for (i = 0; i < URL_CCH + 8; i++) big[i] = L'a';
    memcpy(big, L"claude://", 9 * sizeof(WCHAR));
    big[URL_CCH + 8] = 0;
    Check("overlong link refused", !Core_SanitizeUrl(big, out, ARRAYSIZE(out)));

    Check("google callback is a sign-in link", Core_IsSignInUrl(L"claude://login/google-auth?code=x"));
    Check("magic link is a sign-in link", Core_IsSignInUrl(L"claude://claude.ai/magic-link#abc"));
    Check("SSO callback is a sign-in link", Core_IsSignInUrl(L"claude://sso/callback?x"));
    Check("a chat link is not a sign-in link", !Core_IsSignInUrl(L"claude://claude.ai/new?q=hello"));
    Check("sign-in words in the query do not count", !Core_IsSignInUrl(L"claude://claude.ai/new?q=espresso+author+login"));
    Check("sign-in words in the fragment do not count", !Core_IsSignInUrl(L"claude://claude.ai/chat/1#oauth"));
}

static void TestNames(void)
{
    WCHAR name[LABEL_CCH], folder[FOLDER_CCH], label[LABEL_CCH];
    const WCHAR *err = NULL;

    Check("simple name", Core_ValidateNewName(L"Work", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    CheckStr("simple name -> folder", L"Claude-Work", folder);
    Check("trimmed", Core_ValidateNewName(L"  Client A  ", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err) &&
                         wcscmp(name, L"Client A") == 0 && wcscmp(folder, L"Claude-Client A") == 0);
    Check("accents allowed", Core_ValidateNewName(L"Zo\x00EB perso", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("digits, dash, dot, underscore allowed", Core_ValidateNewName(L"team_2.0-b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("empty refused", !Core_ValidateNewName(L"   ", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err) && err);
    Check("33 characters refused", !Core_ValidateNewName(L"abcdefghijabcdefghijabcdefghijabc", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("32 characters accepted", Core_ValidateNewName(L"abcdefghijabcdefghijabcdefghijab", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("slash refused", !Core_ValidateNewName(L"a/b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("backslash refused", !Core_ValidateNewName(L"a\\b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("colon refused", !Core_ValidateNewName(L"a:b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("quote refused", !Core_ValidateNewName(L"a\"b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("leading dot refused", !Core_ValidateNewName(L".work", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("trailing dot refused", !Core_ValidateNewName(L"work.", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("3p reserved", !Core_ValidateNewName(L"3P", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("Data reserved", !Core_ValidateNewName(L"data", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("<x>-Data reserved", !Core_ValidateNewName(L"Work-Data", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("<x>-3p reserved", !Core_ValidateNewName(L"Work-3p", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("Database allowed", Core_ValidateNewName(L"Database", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));

    Check("label: anything printable", Core_ValidateLabel(L"  Perso (Zo\x00EB) \x2022 2026 ", label, ARRAYSIZE(label), &err) &&
                                           wcscmp(label, L"Perso (Zo\x00EB) \x2022 2026") == 0);
    Check("label: empty refused", !Core_ValidateLabel(L"", label, ARRAYSIZE(label), &err));
    Check("label: control char refused", !Core_ValidateLabel(L"a\nb", label, ARRAYSIZE(label), &err));

    Check("folder Claude-Work", Core_IsProfileFolder(L"Claude-Work"));
    Check("folder case-insensitive prefix", Core_IsProfileFolder(L"claude-work"));
    Check("folder with spaces", Core_IsProfileFolder(L"Claude-Client A"));
    Check("folder Claude-3p is Claude's own", !Core_IsProfileFolder(L"Claude-3p"));
    Check("folder Claude-Work-Data is Claude's own", !Core_IsProfileFolder(L"Claude-Work-Data"));
    Check("folder Claude is the stock one", !Core_IsProfileFolder(L"Claude"));
    Check("folder Claude- is empty", !Core_IsProfileFolder(L"Claude-"));
    Check("folder Codex is unrelated", !Core_IsProfileFolder(L"Codex"));
}

static void TestLogParsing(void)
{
    static const char log[] =
        "ogin/app-google-auth\n"
        "2026-09-21 07:01:06 [info] Starting app {\n"
        "2026-09-21 07:01:24 [info] [Auth] Using system browser for: /login/app-google-auth\r\n"
        "2026-09-21 07:02:54 [info] [Auth] Using system browser for: /login/app-google-auth\r\n"
        "2026-09-21 07:02:31 [info] [account] User is logged out\n"
        "  [Auth] Using system browser for: continuation line without a timestamp\n"
        "2026-09-21 07:03:08 [info] second-instance: suppressing duplicate argv";
    SYSTEMTIME st;

    Check("latest sign-in start found", Core_LatestSignInStart(log, sizeof log - 1, &st));
    Check("latest sign-in start is 07:02:54", st.wHour == 7 && st.wMinute == 2 && st.wSecond == 54 && st.wDay == 21);
    Check("no sign-in start", !Core_LatestSignInStart("2026-09-21 07:01:06 [info] Starting app {\n", 42, &st));
    Check("empty log", !Core_LatestSignInStart("", 0, &st));
    Check("invalid date refused",
          !Core_LatestSignInStart("2026-13-21 07:01:06 [info] [Auth] Using system browser for: /x\n", 62, &st));
}

static BOOL LastQuit(const char *log, BOOL *forUpdate, SYSTEMTIME *st)
{
    return Core_LastQuit(log, strlen(log), st, forUpdate);
}

static void TestQuitParsing(void)
{
    /* The window whose updater installs the update (Claude 2.9939). */
    static const char updater[] =
        "2026-09-28 20:48:10 [info] [stealth-relaunch] Saved navigation history (14 entries, active=12)\n"
        "2026-09-28 20:48:10 [info] [CCD] Stopping 4 active session(s) on quit\n"
        "2026-09-28 20:48:12 [info] Session stop before update took 1460ms\n"
        "2026-09-28 20:48:12 [info] beforeQuitForUpdate handler fired, going down for update\n"
        "2026-09-28 20:48:13 [info] Windows session ending (close-app) - quitting the app\n";
    /* Another window, closed by Windows for the same update; the new version
     * already logs its start. */
    static const char closed[] =
        "2026-09-22 20:36:32 [info] willQuit: handler is ready for quit, so quitting\n"
        "2026-09-28 20:48:01 [info] [process-memory] trigger=interval\n"
        "2026-09-28 20:48:13 [info] Windows session ending (close-app) - quitting the app\r\n"
        "2026-09-28 20:48:14 [info] Starting app {\n"
        "  appVersion: '2.9939.4',\n"
        "2026-09-28 20:48:14 [info] [quit-cleanup] previous quit: {\n"
        "  for_update: false,\n";
    static const char userQuit[] =
        "2026-09-23 01:35:53 [info] Windows session ending (close-app) - quitting the app\n"
        "2026-09-24 12:38:15 [info] Quitting app on main window close since tray is disabled\n"
        "2026-09-24 12:38:36 [info] beforeQuit: handler fired, going down\n"
        "2026-09-24 12:38:36 [info] beforeQuit: handler is ready for quit, so quitting\n"
        "2026-09-24 12:38:36 [info] willQuit: handler is ready for quit, so quitting\n";
    static const char shutdown[] =
        "2026-09-28 20:48:13 [info] Windows session ending (close-app) - quitting the app\n"
        "2026-09-29 09:40:15 [info] Windows session ending (shutdown) - quitting the app\n";
    static const char noQuit[] =
        "2026-09-28 20:48:14 [info] Starting app {\n"
        "2026-09-28 20:48:14 [info] [quit-cleanup] previous quit: {\n"
        "  Windows session ending (close-app) in a continuation line\n"
        "2026-09-28 20:48:21 [info] [CCD] Removing old version: 2.1.280\n";
    BOOL forUpdate = FALSE;
    SYSTEMTIME st;

    Check("updater: quit found", LastQuit(updater, &forUpdate, &st));
    Check("updater: for an update", forUpdate);
    Check("updater: time of the last quit line", st.wDay == 28 && st.wHour == 20 && st.wMinute == 48 && st.wSecond == 13);
    Check("closed by Windows: for an update", LastQuit(closed, &forUpdate, &st) && forUpdate && st.wSecond == 13);
    Check("quit from the window: not an update", LastQuit(userQuit, &forUpdate, &st) && !forUpdate && st.wDay == 24);
    Check("Windows shutdown: not an update", LastQuit(shutdown, &forUpdate, &st) && !forUpdate && st.wDay == 29);
    Check("no quit line", !LastQuit(noQuit, &forUpdate, &st));
    Check("empty log", !LastQuit("", &forUpdate, &st));
}

static void TestRouting(void)
{
    const ULONGLONG window = SIGNIN_WINDOW_MIN * 60ULL * 10000000ULL;
    ULONGLONG ticks[3];
    int targets[3], n;
    RouteReason why;

    n = Core_SelectTargets(0, NULL, At(7, 3, 0), window, TRUE, -1, -1, targets, &why);
    Check("nothing running -> default profile", n == 0 && why == ROUTE_NOTHING_RUNNING);

    n = Core_SelectTargets(1, NULL, At(7, 3, 0), window, TRUE, -1, -1, targets, &why);
    Check("one window -> it", n == 1 && targets[0] == 0 && why == ROUTE_ONLY_ONE);

    ticks[0] = At(7, 2, 54); ticks[1] = 0;
    n = Core_SelectTargets(2, ticks, At(7, 2, 59), window, TRUE, 1, 1, targets, &why);
    Check("sign-in -> the window that opened the browser, not the last used", n == 1 && targets[0] == 0 && why == ROUTE_SIGNIN);

    ticks[0] = At(7, 2, 54); ticks[1] = At(7, 3, 2);
    n = Core_SelectTargets(2, ticks, At(7, 3, 7), window, TRUE, 0, 0, targets, &why);
    Check("sign-in -> the most recent browser opening", n == 1 && targets[0] == 1);

    ticks[0] = At(6, 30, 0); ticks[1] = At(6, 40, 0);
    n = Core_SelectTargets(2, ticks, At(7, 3, 0), window, TRUE, 0, 0, targets, &why);
    Check("stale sign-ins -> every window checks the link", n == 2 && why == ROUTE_BROADCAST);

    ticks[0] = At(7, 5, 0); ticks[1] = 0;
    n = Core_SelectTargets(2, ticks, At(7, 3, 0), window, TRUE, 1, 1, targets, &why);
    Check("a sign-in stamped two minutes ahead is ignored", n == 2 && why == ROUTE_BROADCAST);

    ticks[0] = At(7, 3, 30); ticks[1] = 0;
    n = Core_SelectTargets(2, ticks, At(7, 3, 0), window, TRUE, 1, 1, targets, &why);
    Check("a sign-in stamped seconds ahead (clock skew) still counts", n == 1 && targets[0] == 0 && why == ROUTE_SIGNIN);

    n = Core_SelectTargets(3, NULL, At(7, 3, 0), window, TRUE, 2, 0, targets, &why);
    Check("sign-in link, no log -> every window", n == 3 && why == ROUTE_BROADCAST);

    ticks[0] = At(7, 2, 54); ticks[1] = 0;
    n = Core_SelectTargets(2, ticks, At(7, 3, 0), window, FALSE, 1, 0, targets, &why);
    Check("other link -> last used window, whatever the sign-ins", n == 1 && targets[0] == 1 && why == ROUTE_LAST_USED);

    n = Core_SelectTargets(2, NULL, At(7, 3, 0), window, FALSE, -1, 1, targets, &why);
    Check("other link, no window in front -> default profile", n == 1 && targets[0] == 1 && why == ROUTE_DEFAULT);

    n = Core_SelectTargets(2, NULL, At(7, 3, 0), window, FALSE, -1, -1, targets, &why);
    Check("other link, default not running -> first window", n == 1 && targets[0] == 0 && why == ROUTE_FIRST);
}

static void TestShortcuts(void)
{
    const WCHAR *work = L"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work";
    const WCHAR *stock = L"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude";
    LinkInfo li;
    WCHAR out[MAX_PATH];

    Check("--launch quoted", Core_ArgsSelectProfile(L"--launch \"Claude-Work\"", L"Claude-Work"));
    Check("--launch unquoted", Core_ArgsSelectProfile(L"--launch Claude-Work", L"claude-work"));
    Check("--launch with spaces", Core_ArgsSelectProfile(L"--launch \"Claude-Client A\"", L"Claude-Client A"));
    Check("--launch another profile", !Core_ArgsSelectProfile(L"--launch \"Claude-Work\"", L"Claude"));
    Check("no --launch", !Core_ArgsSelectProfile(L"\"Claude-Work\"", L"Claude-Work"));
    Check("--launch without value", !Core_ArgsSelectProfile(L"--launch", L"Claude-Work"));

    Check("quoted dir argument", Core_ArgsReferenceDir(L"\"C:\\Tools\\start.cmd\" \"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\"", work));
    Check("--user-data-dir=\"dir\"", Core_ArgsReferenceDir(L"--user-data-dir=\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude\"", stock));
    Check("dir with trailing backslash", Core_ArgsReferenceDir(L"--user-data-dir=\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude\\\"", stock));
    Check("stock dir is not a prefix match of Claude-Work", !Core_ArgsReferenceDir(L"\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\"", stock));
    Check("dir inside a longer path is not a match", !Core_ArgsReferenceDir(L"\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude\\sub\"", stock));

    ZeroMemory(&li, sizeof li);
    StringCchCopyW(li.target, ARRAYSIZE(li.target), L"C:\\Users\\x\\AppData\\Local\\Programs\\Claude Desktop Profiles Manager\\ClaudeDesktopProfilesManager.exe");
    StringCchCopyW(li.args, ARRAYSIZE(li.args), L"--launch \"Claude-Work\"");
    Check("our shortcut opens Work", Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    Check("our Work shortcut does not open stock", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    li.args[0] = 0;
    Check("our manager shortcut opens no profile", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));

    ZeroMemory(&li, sizeof li);
    StringCchCopyW(li.target, ARRAYSIZE(li.target), L"C:\\Program Files\\WindowsApps\\Claude_2.2553.13.0_x64__pzs8sxrjxfjjc\\app\\Claude.exe");
    Check("plain Claude.exe shortcut opens stock", Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    Check("plain Claude.exe shortcut does not open Work", !Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    StringCchCopyW(li.args, ARRAYSIZE(li.args), L"--user-data-dir=\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\"");
    Check("Claude.exe --user-data-dir=Work opens Work", Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    Check("Claude.exe --user-data-dir=Work does not open stock", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));

    ZeroMemory(&li, sizeof li);
    StringCchCopyW(li.parsing, ARRAYSIZE(li.parsing), L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\Claude_pzs8sxrjxfjjc!Claude");
    Check("Start-menu style Claude shortcut opens stock", Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    StringCchCopyW(li.parsing, ARRAYSIZE(li.parsing), L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\Claude_pzs8sxrjxfjjc!SshAskpass");
    Check("another app of the package is not Claude", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));

    Core_ShortcutFileName(L"Work", 1, out, ARRAYSIZE(out));
    CheckStr("shortcut file name", L"Claude (Work).lnk", out);
    Core_ShortcutFileName(L"A/B:C*", 2, out, ARRAYSIZE(out));
    CheckStr("shortcut file name, sanitized, second copy", L"Claude (A_B_C_) (2).lnk", out);
}

static void TestMisc(void)
{
    WCHAR a[64], b[64];
    Check("path equals ignores case and trailing slash", Core_PathEquals(L"C:\\Users\\X\\Claude\\", L"c:\\users\\x\\claude"));
    Check("path equals is not a prefix test", !Core_PathEquals(L"C:\\Users\\X\\Claude", L"C:\\Users\\X\\Claude-Work"));
    Check("ends with", Core_EndsWithI(L"x\\APP\\claude.EXE", L"\\app\\Claude.exe"));
    Check("contains", Core_ContainsI(L"abc --User-Data-Dir=x", L"--user-data-dir"));
    Check("our exe by name", Core_IsOurExe(L"D:\\portable\\claudedesktopprofilesmanager.exe"));
    Check("not our exe", !Core_IsOurExe(L"C:\\x\\Claude.exe"));
    Check("hash is case-insensitive", Core_Hash(L"Claude-Work") == Core_Hash(L"CLAUDE-WORK"));
    Check("hash differs", Core_Hash(L"Claude-Work") != Core_Hash(L"Claude-Perso"));
    Core_ProfileAumid(L"Claude-Work", a, ARRAYSIZE(a));
    Core_ProfileAumid(L"claude-work", b, ARRAYSIZE(b));
    Check("AUMID stable and without spaces", wcscmp(a, b) == 0 && !wcschr(a, L' ') && wcsncmp(a, L"ClaudeDesktopProfilesManager.Profile.", 37) == 0);
}

static void TestPathsAndTimes(void)
{
    SYSTEMTIME st;
    FILETIME t0, t1, t2, t3;
    Check("file inside the folder", Core_PathUnder(L"C:\\Users\\X\\Desktop\\a.lnk", L"C:\\Users\\X\\Desktop"));
    Check("folder with trailing slash", Core_PathUnder(L"c:\\users\\x\\desktop\\sub\\a.lnk", L"C:\\Users\\X\\Desktop\\"));
    Check("the folder itself", Core_PathUnder(L"C:\\Users\\X\\Desktop", L"C:\\Users\\X\\Desktop"));
    Check("a sibling with the same prefix is outside", !Core_PathUnder(L"C:\\Users\\X\\Desktop2\\a.lnk", L"C:\\Users\\X\\Desktop"));
    Check("another folder", !Core_PathUnder(L"D:\\a.lnk", L"C:\\Users\\X\\Desktop"));

    ZeroMemory(&st, sizeof st);
    st.wYear = 2026; st.wMonth = 9; st.wDay = 27; st.wHour = 8; st.wMinute = 15; st.wSecond = 40; st.wMilliseconds = 500;
    SystemTimeToFileTime(&st, &t0);
    st.wSecond = 41;
    SystemTimeToFileTime(&st, &t1);
    st.wSecond = 42;
    SystemTimeToFileTime(&st, &t2);
    st.wMinute = 16; st.wSecond = 40;
    SystemTimeToFileTime(&st, &t3);
    Check("same FAT time within a second", Core_SameFatTime(&t0, &t1));
    Check("2 seconds later is another FAT time", !Core_SameFatTime(&t0, &t2));
    Check("another minute", !Core_SameFatTime(&t0, &t3));
}

static void TestProfileFilePaths(void)
{
    Profile p;
    WCHAR out[MAX_PATH], physical[MAX_PATH], large[MAX_PATH];
    const WCHAR *logical = L"C:\\Users\\X\\AppData\\Roaming\\Claude";
    const WCHAR *storage = L"C:\\Users\\X\\AppData\\Local\\Packages\\Claude_test\\LocalCache\\Roaming\\Claude";
    ZeroMemory(&p, sizeof p);
    StringCchCopyW(p.dataDir, ARRAYSIZE(p.dataDir), logical);
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), storage);
    StringCchPrintfW(physical, ARRAYSIZE(physical), L"%s\\scratch-workspaces\\a\\b\\work", storage);
    Check("profile path: logical root resolves", Core_ProfileFilePath(&p, logical, out, ARRAYSIZE(out)) &&
          wcscmp(out, storage) == 0);
    Check("profile path: logical descendant resolves", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: case does not affect ownership", Core_ProfileFilePath(&p,
          L"c:\\users\\x\\appdata\\roaming\\claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: physical descendant stays physical", Core_ProfileFilePath(&p, physical, out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: an external project stays external", Core_ProfileFilePath(&p, L"D:\\Project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"D:\\Project") == 0);
    Check("profile path: similarly named sibling stays separate", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude-Work\\project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Roaming\\Claude-Work\\project") == 0);
    StringCchCatW(p.dataDir, ARRAYSIZE(p.dataDir), L"\\");
    StringCchCatW(p.storageDir, ARRAYSIZE(p.storageDir), L"\\");
    Check("profile path: trailing root separators are bounded", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    StringCchCopyW(out, ARRAYSIZE(out), logical);
    Check("profile path: in-place logical root resolves", Core_ProfileFilePath(&p, out, out, ARRAYSIZE(out)) &&
          wcscmp(out, storage) == 0);
    StringCchCopyW(out, ARRAYSIZE(out), physical);
    Check("profile path: in-place physical path is preserved", Core_ProfileFilePath(&p, out, out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: a small output fails empty", !Core_ProfileFilePath(&p, logical, out, 8) && !out[0]);
    Check("profile path: a small external output fails empty", !Core_ProfileFilePath(&p, L"D:\\Project", out, 8) && !out[0]);
    wmemset(large, L'x', ARRAYSIZE(large) - 1);
    large[ARRAYSIZE(large) - 1] = 0;
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), large);
    Check("profile path: a long mapped path fails empty", !Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\project", out, ARRAYSIZE(out)) && !out[0]);
    p.storageDir[0] = 0;
    Check("profile path: unresolved storage refuses logical I/O", !Core_ProfileFilePath(&p, logical, out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: unresolved storage permits external I/O", Core_ProfileFilePath(&p, L"D:\\Project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"D:\\Project") == 0);
    Check("profile path: a missing input fails empty", !Core_ProfileFilePath(&p, NULL, out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: an empty input fails empty", !Core_ProfileFilePath(&p, L"", out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: a missing profile fails empty", !Core_ProfileFilePath(NULL, logical, out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: a missing output is refused", !Core_ProfileFilePath(&p, logical, NULL, ARRAYSIZE(out)));
    Check("profile path: an empty output buffer is refused", !Core_ProfileFilePath(&p, logical, out, 0));
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), p.dataDir);
    Check("profile path: a physical profile preserves its path", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Roaming\\Claude\\project") == 0);
}

static BOOL Member(const char *json, const char *key, const char *expect)
{
    const char *v = NULL;
    size_t n = 0;
    if (!Core_JsonMember(json, strlen(json), key, &v, &n)) return expect == NULL;
    return expect && n == strlen(expect) && memcmp(v, expect, n) == 0;
}

static void TestJsonAndVersions(void)
{
    const char *cfg = "\xEF\xBB\xBF{ \"preferences\": {\"menuBarEnabled\": true, \"x\": [1, \"]\"]},\n"
                      "  \"mcpServers\": {\"a\": {\"command\": \"c:\\\\t\\\"x.exe\", \"args\": []}}, \"n\": 12 }";
    DWORD a[4], b[4];

    Check("json: object member", Member(cfg, "mcpServers", "{\"a\": {\"command\": \"c:\\\\t\\\"x.exe\", \"args\": []}}"));
    Check("json: nested object kept whole", Member(cfg, "preferences", "{\"menuBarEnabled\": true, \"x\": [1, \"]\"]}"));
    Check("json: number", Member(cfg, "n", "12"));
    Check("json: nested key is not a member", Member(cfg, "menuBarEnabled", NULL));
    Check("json: missing key", Member(cfg, "locale", NULL));
    Check("json: string with its quotes", Member("{\"tag_name\":\"v1.2.3\",\"x\":1}", "tag_name", "\"v1.2.3\""));
    Check("json: not an object", Member("[1]", "a", NULL));
    Check("json: truncated", Member("{\"a\": {\"b\": 1", "a", NULL));

    {
        /* A session record as Claude Desktop writes it. */
        static const char rec[] = "{\"sessionId\":\"local_1\",\"title\":\"Cause du dernier red\xC3\xA9marrage\","
                                  "\"cwd\":\"C:\\\\Users\\\\L\\u00e9o\\\\a \\\"b\\\"\",\"isStarred\":true,\"isArchived\":false,"
                                  "\"lastActivityAt\":1790639580461,\"emoji\":\"\\ud83d\\ude00\",\"bad\":\"\\x\"}";
        const char *v;
        size_t n;
        WCHAR s[64];
        ULONGLONG t;
        Check("json string: UTF-8 kept", Core_JsonMember(rec, strlen(rec), "title", &v, &n) &&
                                          Core_JsonString(v, n, s, ARRAYSIZE(s)) && wcscmp(s, L"Cause du dernier red\x00E9marrage") == 0);
        Check("json string: escapes", Core_JsonMember(rec, strlen(rec), "cwd", &v, &n) &&
                                      Core_JsonString(v, n, s, ARRAYSIZE(s)) && wcscmp(s, L"C:\\Users\\L\x00E9o\\a \"b\"") == 0);
        Check("json string: surrogate pair", Core_JsonMember(rec, strlen(rec), "emoji", &v, &n) &&
                                             Core_JsonString(v, n, s, ARRAYSIZE(s)) && s[0] == 0xD83D && s[1] == 0xDE00 && s[2] == 0);
        Check("json string: bad escape refused", Core_JsonMember(rec, strlen(rec), "bad", &v, &n) && !Core_JsonString(v, n, s, ARRAYSIZE(s)));
        Check("json string: too small a buffer fails", Core_JsonMember(rec, strlen(rec), "title", &v, &n) && !Core_JsonString(v, n, s, 8));
        Check("json string: not a string", Core_JsonMember(rec, strlen(rec), "isStarred", &v, &n) && !Core_JsonString(v, n, s, ARRAYSIZE(s)));
        Check("json string: empty", Core_JsonString("\"\"", 2, s, ARRAYSIZE(s)) && s[0] == 0);
        Check("json number", Core_JsonMember(rec, strlen(rec), "lastActivityAt", &v, &n) && Core_JsonNumber(v, n, &t) && t == 1790639580461ULL);
        Check("json number: not a number", !Core_JsonNumber("12a", 3, &t) && !Core_JsonNumber("", 0, &t));
        Check("json true", Core_JsonMember(rec, strlen(rec), "isStarred", &v, &n) && Core_JsonTrue(v, n));
        Check("json false", Core_JsonMember(rec, strlen(rec), "isArchived", &v, &n) && !Core_JsonTrue(v, n));
    }

    Check("version: v1.2.3", Core_ParseVersion(L"v1.2.3", a) && a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 0);
    Check("version: 2.1 and 2.1.0 are equal", Core_ParseVersion(L"2.1", a) && Core_ParseVersion(L"2.1.0", b) && Core_CompareVersions(a, b) == 0);
    Check("version: 1.10 is newer than 1.9", Core_ParseVersion(L"1.10", a) && Core_ParseVersion(L"1.9", b) && Core_CompareVersions(a, b) > 0);
    Check("version: 1.0.0 is older than 1.0.1", Core_ParseVersion(L"v1.0.0", a) && Core_ParseVersion(L"v1.0.1", b) && Core_CompareVersions(a, b) < 0);
    Check("version: suffix ignored", Core_ParseVersion(L"v3.4-beta", a) && a[0] == 3 && a[1] == 4);
    Check("version: not a version", !Core_ParseVersion(L"latest", a) && !Core_ParseVersion(NULL, a));
}

static BOOL Kind(const char *json, SessionEntryKind expected)
{
    return Core_SessionEntryKind(json, strlen(json)) == expected;
}

static void TestSessionEntries(void)
{
    Check("entry: a session of this PC", Kind("{\"sessionId\":\"local_1\",\"cwd\":\"C:\\\\x\",\"sshConfig\":null}", ENTRY_LOCAL));
    Check("entry: over SSH", Kind("{\"sessionId\":\"local_2\",\"sshConfig\":{\"host\":\"box\"}}", ENTRY_ELSEWHERE));
    Check("entry: in WSL", Kind("{\"sessionId\":\"local_3\",\"wslConfig\":{\"distro\":\"Ubuntu\"}}", ENTRY_ELSEWHERE));
    Check("entry: in the cloud", Kind("{\"sessionId\":\"local_4\",\"cloudSessionId\":\"session_01\"}", ENTRY_ELSEWHERE));
    Check("entry: moved to the cloud", Kind("{\"sessionId\":\"local_5\",\"movedToCloud\":true}", ENTRY_ELSEWHERE));
    Check("entry: not moved", Kind("{\"sessionId\":\"local_6\",\"movedToCloud\":false,\"cloudSessionId\":null}", ENTRY_LOCAL));
    Check("entry: a nested sshConfig is not the entry's",
          Kind("{\"sessionId\":\"local_7\",\"meta\":{\"sshConfig\":{\"host\":\"box\"}}}", ENTRY_LOCAL));
    Check("entry: no sessionId", Kind("{\"title\":\"x\"}", ENTRY_NOT_ONE));
    Check("entry: sessionId not a string", Kind("{\"sessionId\":12}", ENTRY_NOT_ONE));
    Check("entry: empty sessionId", Kind("{\"sessionId\":\"\"}", ENTRY_NOT_ONE));
    Check("entry: not JSON", Kind("local_1", ENTRY_NOT_ONE) && Core_SessionEntryKind(NULL, 0) == ENTRY_NOT_ONE);
}

static BOOL SetMember(const char *json, const char *key, const char *raw, const char *expected)
{
    char out[256];
    size_t n = 0;
    BOOL ok = Core_JsonSetMember(json, strlen(json), key, raw, out, sizeof out, &n);
    if (!expected) return !ok;
    return ok && n == strlen(expected) && memcmp(out, expected, n) == 0;
}

static BOOL Quote(const WCHAR *s, const char *expected)
{
    char out[64];
    return Core_JsonQuote(s, out, sizeof out) && strcmp(out, expected) == 0;
}

/* Runs `in` through Core_ReplaceChunk cut in two at `cut`, as a file read in
 * two chunks, and compares with `expected`. */
static BOOL ReplaceCut(const char *in, size_t cut, const CoreSwap *swaps, int count, const char *expected)
{
    char buf[256], out[512];
    size_t len = strlen(in), held, used = 0, n = 0;
    memcpy(buf, in, cut);
    n += Core_ReplaceChunk(buf, cut, swaps, count, FALSE, out + n, &used);
    held = cut - used;
    memmove(buf, buf + used, held);
    memcpy(buf + held, in + cut, len - cut);
    n += Core_ReplaceChunk(buf, held + len - cut, swaps, count, TRUE, out + n, &used);
    return n == strlen(expected) && memcmp(out, expected, n) == 0;
}

static void TestSessionEdits(void)
{
    static const CoreSwap swaps[] = {
        { "\"sessionId\":\"a1\"", "\"sessionId\":\"b2\"" },
        { "\"cwd\":\"C:\\\\old\"", "\"cwd\":\"D:\\\\newer\"" },
    };
    const char *line = "{\"sessionId\":\"a1\",\"cwd\":\"C:\\\\old\",\"x\":\"\\\"sessionId\\\":\\\"a1\\\"\"}\n{\"sessionId\":\"a1\"}";
    const char *swapped = "{\"sessionId\":\"b2\",\"cwd\":\"D:\\\\newer\",\"x\":\"\\\"sessionId\\\":\\\"a1\\\"\"}\n{\"sessionId\":\"b2\"}";
    PendingEdit e, back;
    WCHAR text[400], name[64];
    SYSTEMTIME day;
    size_t cut;
    BOOL allCuts = TRUE;

    Check("json set: a member is replaced in place", SetMember("{\"title\":\"a\",\"n\":1}", "title", "\"b\"", "{\"title\":\"b\",\"n\":1}"));
    Check("json set: a missing member is added last", SetMember("{\"n\":1}\n", "isStarred", "true", "{\"n\":1,\"isStarred\":true}\n"));
    Check("json set: into an empty object", SetMember("{ }", "k", "1", "{\"k\":1 }"));
    Check("json set: a nested member of that name is not the one",
          SetMember("{\"x\":{\"title\":\"n\"}}", "title", "\"t\"", "{\"x\":{\"title\":\"n\"},\"title\":\"t\"}"));
    Check("json set: the BOM stays", SetMember("\xEF\xBB\xBF{\"a\":0}", "a", "2", "\xEF\xBB\xBF{\"a\":2}"));
    Check("json set: not an object", SetMember("[1]", "a", "1", NULL) && SetMember("", "a", "1", NULL));
    {
        char small[8];
        size_t n;
        Check("json set: too small a buffer fails", !Core_JsonSetMember("{\"a\":1}", 7, "title", "\"long\"", small, sizeof small, &n));
    }

    Check("json quote: plain", Quote(L"Trading", "\"Trading\""));
    Check("json quote: quotes and backslashes", Quote(L"C:\\a \"b\"", "\"C:\\\\a \\\"b\\\"\""));
    Check("json quote: a control character", Quote(L"a\nb", "\"a\\u000ab\""));
    Check("json quote: UTF-8", Quote(L"red\x00E9marrage", "\"red\xC3\xA9marrage\""));
    Check("json quote: a surrogate pair", Quote(L"\xD83D\xDE00", "\"\xF0\x9F\x98\x80\""));
    Check("json quote: a lone surrogate becomes U+FFFD", Quote(L"a\xD800", "\"a\xEF\xBF\xBD\""));
    {
        char small[4];
        Check("json quote: too small a buffer fails", !Core_JsonQuote(L"abcdef", small, sizeof small));
    }

    Check("project folder: as Claude Code names it", Core_ProjectDirName(L"C:\\Users\\L\x00E9o GILLET\\Desktop\\claude-windows-multiprofile", name, ARRAYSIZE(name)) &&
                                                     wcscmp(name, L"C--Users-L-o-GILLET-Desktop-claude-windows-multiprofile") == 0);
    {
        WCHAR longPath[CORE_PROJECT_NAME_MAX + 2], big[CORE_PROJECT_NAME_MAX + 8];
        wmemset(longPath, L'a', CORE_PROJECT_NAME_MAX);
        longPath[CORE_PROJECT_NAME_MAX] = 0;
        Check("project folder: 200 characters are kept", Core_ProjectDirName(longPath, big, ARRAYSIZE(big)) && wcslen(big) == CORE_PROJECT_NAME_MAX);
        longPath[CORE_PROJECT_NAME_MAX] = L'a';
        longPath[CORE_PROJECT_NAME_MAX + 1] = 0;
        Check("project folder: longer ones get a hash we do not make", !Core_ProjectDirName(longPath, big, ARRAYSIZE(big)));
    }

    Check("session id: valid", Core_IsSessionId(L"9652d3ce-0a2e-47d1-b8f2-11bcdb88c5f4"));
    Check("session id: wrong length or character", !Core_IsSessionId(L"9652d3ce-0a2e-47d1-b8f2-11bcdb88c5f") &&
                                                  !Core_IsSessionId(L"9652d3ce-0a2e-47d1-b8f2-11bcdb88c5g4") &&
                                                  !Core_IsSessionId(L"9652d3ce00a2e-47d1-b8f2-11bcdb88c5f4") && !Core_IsSessionId(NULL));
    Check("resume link", Core_ResumeLink(L"9652d3ce-0a2e-47d1-b8f2-11bcdb88c5f4", text, ARRAYSIZE(text)) &&
                         wcscmp(text, L"claude://resume?session=9652d3ce-0a2e-47d1-b8f2-11bcdb88c5f4") == 0);
    Check("resume link: never with something else", !Core_ResumeLink(L"x&evil=1", text, ARRAYSIZE(text)));

    ZeroMemory(&day, sizeof day);
    day.wYear = 2026;
    day.wMonth = 9;
    day.wDay = 15;
    Core_ScratchName(&day, 0x1242EDE0, name, ARRAYSIZE(name));
    CheckStr("scratch folder: as Claude Desktop names them", L"scratch-2026-09-15-42ede0", name);

    for (cut = 0; cut <= strlen(line); cut++)
        if (!ReplaceCut(line, cut, swaps, 2, swapped)) {
            printf("        cut at %u\n", (unsigned)cut);
            allCuts = FALSE;
        }
    Check("replace: the same wherever the file is cut into chunks", allCuts);
    Check("replace: nothing to swap", ReplaceCut("{\"x\":1}", 3, swaps, 2, "{\"x\":1}"));
    Check("replace: the start of a swap at the very end stays", ReplaceCut("ab\"sessionId\":\"a", 5, swaps, 1, "ab\"sessionId\":\"a"));

    ZeroMemory(&e, sizeof e);
    e.op = PENDING_TITLE;
    StringCchCopyW(e.key, ARRAYSIZE(e.key), L"9652d3ce-0a2e-47d1-b8f2-11bcdb88c5f4");
    StringCchCopyW(e.value, ARRAYSIZE(e.value), L"Plan\tbot\n");
    Check("pending: written", Core_PendingFormat(&e, text, ARRAYSIZE(text)) &&
                             wcscmp(text, L"title\t9652d3ce-0a2e-47d1-b8f2-11bcdb88c5f4\tPlan bot ") == 0);
    Check("pending: read back", Core_PendingParse(text, &back) && back.op == PENDING_TITLE &&
                               wcscmp(back.key, e.key) == 0 && wcscmp(back.value, L"Plan bot ") == 0);
    Check("pending: a favorite", Core_PendingParse(L"star\tk\t1", &back) && back.op == PENDING_STAR && back.value[0] == L'1');
    Check("pending: a removal", Core_PendingParse(L"remove\tk\t", &back) && back.op == PENDING_REMOVE && back.value[0] == 0);
    Check("pending: an unknown change or a missing field is skipped",
          !Core_PendingParse(L"rename\tk\tx", &back) && !Core_PendingParse(L"title\tk", &back) && !Core_PendingParse(L"title\t\tx", &back));
    StringCchCopyW(e.key, ARRAYSIZE(e.key), L"a\tb");
    Check("pending: a key with a tab is refused", !Core_PendingFormat(&e, text, ARRAYSIZE(text)));
}

static void TestDrawingMath(void)
{
    int pending, frames, units, moved;

    Check("scroll: nothing left, nothing to do", Core_ScrollStep(0, 16) == 0);
    Check("scroll: the first frame covers a good part at once", Core_ScrollStep(90, 16) >= 30 && Core_ScrollStep(-90, 16) <= -30);
    Check("scroll: a late frame covers more, so the pace holds", Core_ScrollStep(90, 48) > Core_ScrollStep(90, 16));
    Check("scroll: never past the end", Core_ScrollStep(90, 1000) == 90 && Core_ScrollStep(-90, 1000) == -90);
    Check("scroll: at least a pixel, so it ends", Core_ScrollStep(1, 1) == 1 && Core_ScrollStep(-1, 1) == -1);
    /* A notch of three 30 px lines at 16 ms a frame: most of it within 100
     * ms, all of it within 250, the whole way exactly. */
    for (pending = 90, moved = 0, frames = 0; (units = Core_ScrollStep(pending, 16)) != 0 && frames < 100; frames++) {
        pending -= units;
        moved += units;
        if (frames == 5) Check("scroll: most of a notch within 100 ms", moved >= 80);
    }
    Check("scroll: all of it within 250 ms", frames * 16 <= 250);
    Check("scroll: the whole way exactly", moved == 90);
    Check("scroll: more notches start faster", Core_ScrollStep(270, 16) > Core_ScrollStep(90, 16));

    Check("hash: FNV-1a start", CORE_HASH_START == 0xCBF29CE484222325ULL);
    Check("hash: FNV-1a of \"a\"", Core_HashBytes(CORE_HASH_START, "a", 1) == 0xAF63DC4C8601EC8CULL);
    Check("hash: FNV-1a of \"foobar\"", Core_HashBytes(CORE_HASH_START, "foobar", 6) == 0x85944171F73967E8ULL);
    Check("hash: goes on piece by piece", Core_HashBytes(Core_HashBytes(CORE_HASH_START, "foo", 3), "bar", 3) ==
                                          Core_HashBytes(CORE_HASH_START, "foobar", 6));
    {
        RECT work = { 0, 0, 1920, 1040 }, owner = { 100, 100, 900, 700 }, edge = { 1500, 800, 1900, 1000 }, got;
        Core_CenterRect(&owner, 400, 200, &work, &got);
        Check("center: on its owner", got.left == 300 && got.top == 300 && got.right == 700 && got.bottom == 500);
        Core_CenterRect(&edge, 600, 400, &work, &got);
        Check("center: kept inside the work area", got.right == 1920 && got.bottom == 1040 && got.left == 1320 && got.top == 640);
        Core_CenterRect(&owner, 2000, 1200, &work, &got);
        Check("center: bigger than the work area, its top left corner inside", got.left == 0 && got.top == 0);
        Core_CenterRect(&work, 400, 200, &work, &got);
        Check("center: on the work area itself (owner hidden)", got.left == 760 && got.top == 420);
    }
}

int wmain(void)
{
    TestSessionEntries();
    TestSessionEdits();
    TestDrawingMath();
    TestLaunchArgs();
    TestSanitizeUrl();
    TestNames();
    TestLogParsing();
    TestQuitParsing();
    TestRouting();
    TestShortcuts();
    TestMisc();
    TestPathsAndTimes();
    TestProfileFilePaths();
    TestJsonAndVersions();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
