/*
 * The claude:// handler.
 *
 * Windows opens a claude:// link with the default app picked for claude://
 * (HKCU\...\UrlAssociations\claude\UserChoiceLatest\ProgId on current Windows 11
 * builds, UserChoice on the others). HKCU\Software\Classes\claude is Claude's own key: Claude
 * rewrites it with its exe at every start, so it is left to Claude.
 *
 * Programs cannot pick the default app themselves: the choice is signed, and
 * only Windows' own chooser or Settings write it. Claude Desktop Profiles
 * Manager registers itself as an app that opens claude:// links (a ProgId,
 * capabilities and an ApplicationAssociationToasts entry, as other per-user
 * handlers do) and lets Windows ask the user.
 */
#include "app.h"
#include <shellapi.h>
#include <shlobj.h>

#define REG_TOASTS  L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts"
#define TOAST_VALUE PROGID_NAME L"_claude"

/* Writes a string value unless it already holds exactly that. */
static BOOL Put(const WCHAR *key, const WCHAR *value, const WCHAR *data, BOOL *changed)
{
    WCHAR now[MAX_PATH + 64];
    if (Util_RegGetString(HKEY_CURRENT_USER, key, value, now, ARRAYSIZE(now)) &&
        CompareStringOrdinal(now, -1, data, -1, FALSE) == CSTR_EQUAL)
        return TRUE;
    *changed = TRUE;
    return Util_RegSetString(HKEY_CURRENT_USER, key, value, data);
}

/* The ProgId and capabilities that list Claude Desktop Profiles Manager as an
 * app for claude://. Windows is told only when something changed (the notice
 * refreshes Explorer). */
BOOL Handler_Register(const WCHAR *exe)
{
    WCHAR command[MAX_PATH + 32], icon[MAX_PATH + 4];
    BOOL ok, changed = FALSE;
    StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" --url \"%%1\"", exe);
    StringCchPrintfW(icon, ARRAYSIZE(icon), L"%s,0", exe);
    ok = Put(REG_PROGID, NULL, L"Claude link", &changed) &&
         Put(REG_PROGID, L"URL Protocol", L"", &changed) &&
         Put(REG_PROGID L"\\DefaultIcon", NULL, icon, &changed) &&
         Put(REG_PROGID L"\\shell\\open\\command", NULL, command, &changed) &&
         Put(REG_CAPABILITIES, L"ApplicationName", APP_NAME, &changed) &&
         Put(REG_CAPABILITIES, L"ApplicationDescription", L"Opens claude:// links in the right Claude Desktop profile", &changed) &&
         Put(REG_CAPABILITIES, L"ApplicationIcon", icon, &changed) &&
         Put(REG_CAPABILITIES L"\\URLAssociations", L"claude", PROGID_NAME, &changed) &&
         Put(REG_REGISTERED, APP_NAME, REG_CAPABILITIES, &changed);
    if (ok && !Util_RegValueExists(HKEY_CURRENT_USER, REG_TOASTS, TOAST_VALUE)) {
        Util_RegSetDword(HKEY_CURRENT_USER, REG_TOASTS, TOAST_VALUE, 0);
        changed = TRUE;
    }
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return ok;
}

static BOOL ReadChoice(const WCHAR *key, WCHAR *progId, DWORD cch)
{
    return Util_RegGetString(HKEY_CURRENT_USER, key, L"ProgId", progId, cch) && progId[0];
}

/* The default app picked for claude://, whichever key Windows keeps it in. */
UserChoiceState Handler_UserChoice(void)
{
    WCHAR a[256], b[256];
    BOOL hasA = ReadChoice(REG_USERCHOICE, a, ARRAYSIZE(a));
    BOOL hasB = ReadChoice(REG_USERCHOICE2 L"\\ProgId", b, ARRAYSIZE(b)) || ReadChoice(REG_USERCHOICE2, b, ARRAYSIZE(b));
    if (!hasA && !hasB) return USERCHOICE_NONE;
    if ((hasA && CompareStringOrdinal(a, -1, PROGID_NAME, -1, TRUE) != CSTR_EQUAL) ||
        (hasB && CompareStringOrdinal(b, -1, PROGID_NAME, -1, TRUE) != CSTR_EQUAL))
        return USERCHOICE_OTHER;
    return USERCHOICE_OURS;
}

static void ForgetChoice(void)
{
    LSTATUS a = RegDeleteTreeW(HKEY_CURRENT_USER, REG_USERCHOICE);
    LSTATUS b = RegDeleteTreeW(HKEY_CURRENT_USER, REG_USERCHOICE2);
    RegDeleteKeyW(HKEY_CURRENT_USER, REG_USERCHOICE);
    RegDeleteKeyW(HKEY_CURRENT_USER, REG_USERCHOICE2);
    Util_Log(L"cleared the claude:// default-app choice (%ld, %ld)", (long)a, (long)b);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

/* Lets Windows ask which app opens claude:// links: its chooser appears when
 * no app is picked, for this empty link (which the router ignores when this
 * app is picked). A choice of another app is forgotten first. FALSE when
 * Windows kept that other choice. */
BOOL Handler_AskUser(void)
{
    if (Handler_UserChoice() == USERCHOICE_OTHER) {
        ForgetChoice();
        if (Handler_UserChoice() == USERCHOICE_OTHER) return FALSE;
    }
    Util_OpenUrl(L"claude:");
    return TRUE;
}

/* At uninstall: forget our default-app choice (claude:// goes back to Claude)
 * and remove the registration. */
void Handler_Unregister(void)
{
    if (Handler_UserChoice() == USERCHOICE_OURS) ForgetChoice();
    Util_RegDeleteTree(HKEY_CURRENT_USER, REG_PROGID);
    Util_RegDeleteValue(HKEY_CURRENT_USER, REG_REGISTERED, APP_NAME);
    Util_RegDeleteValue(HKEY_CURRENT_USER, REG_TOASTS, TOAST_VALUE);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}
