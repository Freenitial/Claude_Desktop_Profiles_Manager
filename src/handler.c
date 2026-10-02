/*
 * The claude:// handler.
 *
 * Windows opens a claude:// link with the default app picked for claude://
 * (HKCU\...\UrlAssociations\claude\UserChoiceLatest\ProgId on current Windows 11
 * builds, UserChoice on the others). HKCU\Software\Classes\claude is Claude's
 * own key: Claude rewrites it with its exe at every start, so it is left to
 * Claude.
 *
 * Programs cannot pick the default app themselves: the choice is signed, and
 * only Windows' own chooser or Settings write it. Claude Desktop Profiles
 * Manager registers itself as an app that opens claude:// links (a ProgId,
 * capabilities and an ApplicationAssociationToasts entry, as other per-user
 * handlers do) and lets Windows ask the user.
 */
#include "app.h"
#include <shlobj.h>

#define REG_TOASTS  L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts"
#define TOAST_VALUE PROGID_NAME L"_claude"

/* The ProgId and capabilities that list Claude Desktop Profiles Manager as an
 * app for claude://. Windows is told only when something changed (the notice
 * refreshes Explorer). */
BOOL Handler_Register(const WCHAR *exe)
{
    WCHAR command[MAX_PATH + 32], icon[MAX_PATH + 4];
    BOOL ok, changed = FALSE;
    if (FAILED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" --url \"%%1\"", exe)) ||
        FAILED(StringCchPrintfW(icon, ARRAYSIZE(icon), L"%s,0", exe)))
        return FALSE;
    ok = Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_PROGID, NULL, TR(L"Claude link"), &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_PROGID, L"URL Protocol", L"", &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_PROGID L"\\DefaultIcon", NULL, icon, &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_PROGID L"\\shell\\open\\command", NULL, command, &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_CAPABILITIES, L"ApplicationName", APP_NAME, &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_CAPABILITIES, L"ApplicationDescription",
                                      TR(L"Opens claude:// links in the right Claude Desktop profile"), &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_CAPABILITIES, L"ApplicationIcon", icon, &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_CAPABILITIES L"\\URLAssociations", L"claude", PROGID_NAME, &changed) &&
         Util_RegSetStringIfDifferent(HKEY_CURRENT_USER, REG_REGISTERED, APP_NAME, REG_CAPABILITIES, &changed);
    if (ok && !Util_RegValueExists(HKEY_CURRENT_USER, REG_TOASTS, TOAST_VALUE)) {
        if (Util_RegSetDword(HKEY_CURRENT_USER, REG_TOASTS, TOAST_VALUE, 0))
            changed = TRUE;
        else
            Util_Log(L"could not turn off Windows' notice about a new app for claude:// links (error %lu)", GetLastError());
    }
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return ok;
}

static BOOL ReadChoice(const WCHAR *key, WCHAR *progId, size_t cch)
{
    return Util_RegGetString(HKEY_CURRENT_USER, key, L"ProgId", progId, cch) && progId[0];
}

/* The default app picked for claude://, whichever key Windows keeps it in. */
UserChoiceState Handler_UserChoice(void)
{
    WCHAR legacyProgId[256], latestProgId[256];
    BOOL hasLegacy = ReadChoice(REG_USERCHOICE, legacyProgId, ARRAYSIZE(legacyProgId));
    BOOL hasLatest = ReadChoice(REG_USERCHOICE_LATEST L"\\ProgId", latestProgId, ARRAYSIZE(latestProgId)) ||
                     ReadChoice(REG_USERCHOICE_LATEST, latestProgId, ARRAYSIZE(latestProgId));
    if (!hasLegacy && !hasLatest) return USERCHOICE_NONE;
    if ((hasLegacy && !Core_EqualsI(legacyProgId, PROGID_NAME)) || (hasLatest && !Core_EqualsI(latestProgId, PROGID_NAME)))
        return USERCHOICE_OTHER;
    return USERCHOICE_OURS;
}

static void ForgetChoice(void)
{
    LSTATUS legacy = Util_RegDeleteTree(HKEY_CURRENT_USER, REG_USERCHOICE);
    LSTATUS latest = Util_RegDeleteTree(HKEY_CURRENT_USER, REG_USERCHOICE_LATEST);
    Util_Log(L"cleared the claude:// default-app choice (%ld, %ld)", legacy, latest);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

/* Lets Windows ask which app opens claude:// links: its chooser appears when
 * no app is picked, for this empty link (which the router ignores when this
 * app is picked). A choice of another app is forgotten first. FALSE when
 * Windows kept that other choice or the link could not be opened. */
BOOL Handler_AskUser(void)
{
    if (Handler_UserChoice() == USERCHOICE_OTHER) {
        ForgetChoice();
        if (Handler_UserChoice() == USERCHOICE_OTHER) return FALSE;
    }
    return Util_OpenUrl(L"claude:");
}

static void RemoveValue(const WCHAR *key, const WCHAR *value)
{
    LSTATUS status = Util_RegDeleteValue(HKEY_CURRENT_USER, key, value);
    if (status != ERROR_SUCCESS) Util_Log(L"could not remove the value %s of %s (error %ld)", value, key, status);
}

/* At uninstall: forget our default-app choice (claude:// goes back to Claude)
 * and remove the registration. */
void Handler_Unregister(void)
{
    LSTATUS status;
    if (Handler_UserChoice() == USERCHOICE_OURS) ForgetChoice();
    if ((status = Util_RegDeleteTree(HKEY_CURRENT_USER, REG_PROGID)) != ERROR_SUCCESS)
        Util_Log(L"could not remove the key %s (error %ld)", REG_PROGID, status);
    RemoveValue(REG_REGISTERED, APP_NAME);
    RemoveValue(REG_TOASTS, TOAST_VALUE);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}
