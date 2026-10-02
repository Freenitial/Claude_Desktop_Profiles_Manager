/*
 * shortcuts.c on private files and a private registry key: the COM failures
 * of writing, reading and updating a shortcut (every interface released on
 * each path), the AppUserModelID check, renamed names, the pass after a
 * change of language, folders read once, the recorded shortcuts (read,
 * forgotten once deleted, following a rename), what counts as the Start menu
 * and the removal visitors. The shell is not told about the private files,
 * and the log goes to the private folder.
 */
#include "../src/app.h"
#include <shlobj.h>
#include <propsys.h>
#include <stdio.h>
#include <string.h>

/* The COM step made to fail. Those of WriteLink come in its order, from
 * FIRST_WRITE_STEP to LAST_WRITE_STEP. */
typedef enum FailingStep {
    FAIL_NONE,
    FAIL_CREATE_LINK,
    FAIL_SET_PATH,
    FAIL_SET_ARGUMENTS,
    FAIL_SET_ICON_LOCATION,
    FAIL_SET_DESCRIPTION,
    FAIL_SET_WORKING_DIRECTORY,
    FAIL_QUERY_PROPERTY_STORE,
    FAIL_ALLOCATE_APP_ID,
    FAIL_SET_APP_ID,
    FAIL_COMMIT_APP_ID,
    FAIL_QUERY_PERSIST_FILE,
    FAIL_SAVE,
    FAIL_LOAD,
    FAIL_GET_APP_ID,
    FAIL_STEP_COUNT
} FailingStep;

#define FIRST_WRITE_STEP FAIL_CREATE_LINK
#define LAST_WRITE_STEP  FAIL_SAVE

static const char *const kStepNames[FAIL_STEP_COUNT] = {
    "nothing", "CoCreateInstance(ShellLink)", "SetPath", "SetArguments", "SetIconLocation", "SetDescription",
    "SetWorkingDirectory", "QueryInterface(IPropertyStore)", "CoTaskMemAlloc", "IPropertyStore::SetValue",
    "IPropertyStore::Commit", "QueryInterface(IPersistFile)", "IPersistFile::Save", "IPersistFile::Load",
    "IPropertyStore::GetValue",
};

#define FIXTURE_APP_ID     L"Private.Shortcut.Test"
#define FIXTURE_FOLDER     L"Claude-Fixture"
#define RECORDED_FOLDER    L"Claude-Recorded"
#define MANY_RECORDS       300   /* past 256: the records have no fixed limit */
#define DEAD_FOLDER        L"Claude-Dead"
#define FIXTURE_KEY_PREFIX L"Software\\ClaudeProfiles.ShortcutFixture."

static int g_checks, g_failures, g_saveCalls, g_heldInterfaces, g_unexpectedSteps, g_shellNotices, g_attributeReads, g_linkLoads;
static FailingStep g_failingStep;
static BOOL g_reportFirstWriteTime;
static FILETIME g_firstWriteTime;

static void Check(const char *what, BOOL ok)
{
    g_checks++;
    if (!ok) { g_failures++; printf("  FAIL  %s\n", what); }
}

static void CheckStep(FailingStep step, const char *what, BOOL ok)
{
    char text[256];
    StringCchPrintfA(text, ARRAYSIZE(text), "%s (failing: %s)", what, kStepNames[step]);
    Check(text, ok);
}

/* ------------------------------------------------------------- fixtures */

static HRESULT WINAPI FailableCreateInstance(REFCLSID type, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *out)
{
    HRESULT hr;
    if (g_failingStep == FAIL_CREATE_LINK) {
        *out = NULL;
        return E_ACCESSDENIED;
    }
    hr = CoCreateInstance(type, outer, context, iid, out);
    if (SUCCEEDED(hr)) g_heldInterfaces++;
    return hr;
}

static HRESULT FailableSetString(IShellLinkW *link, FailingStep step, const WCHAR *text)
{
    if (g_failingStep == step) return E_ACCESSDENIED;
    switch (step) {
    case FAIL_SET_PATH: return link->lpVtbl->SetPath(link, text);
    case FAIL_SET_ARGUMENTS: return link->lpVtbl->SetArguments(link, text);
    case FAIL_SET_DESCRIPTION: return link->lpVtbl->SetDescription(link, text);
    case FAIL_SET_WORKING_DIRECTORY: return link->lpVtbl->SetWorkingDirectory(link, text);
    default:
        g_unexpectedSteps++;
        return E_UNEXPECTED;
    }
}

static HRESULT FailableSetIconLocation(IShellLinkW *link, const WCHAR *path, int index)
{
    return g_failingStep == FAIL_SET_ICON_LOCATION ? E_ACCESSDENIED : link->lpVtbl->SetIconLocation(link, path, index);
}

static HRESULT FailableQueryInterface(IShellLinkW *link, REFIID iid, void **out)
{
    HRESULT hr;
    if ((g_failingStep == FAIL_QUERY_PROPERTY_STORE && IsEqualIID(iid, &IID_IPropertyStore)) ||
        (g_failingStep == FAIL_QUERY_PERSIST_FILE && IsEqualIID(iid, &IID_IPersistFile))) {
        *out = NULL;
        return E_ACCESSDENIED;
    }
    hr = link->lpVtbl->QueryInterface(link, iid, out);
    if (SUCCEEDED(hr)) g_heldInterfaces++;
    return hr;
}

static ULONG FixtureReleaseLink(IShellLinkW *link)
{
    g_heldInterfaces--;
    return link->lpVtbl->Release(link);
}

static ULONG FixtureReleaseFile(IPersistFile *file)
{
    g_heldInterfaces--;
    return file->lpVtbl->Release(file);
}

static ULONG FixtureReleaseStore(IPropertyStore *store)
{
    g_heldInterfaces--;
    return store->lpVtbl->Release(store);
}

static void *WINAPI FailableTaskMemAlloc(SIZE_T bytes)
{
    return g_failingStep == FAIL_ALLOCATE_APP_ID ? NULL : CoTaskMemAlloc(bytes);
}

static HRESULT FailableSetValue(IPropertyStore *store, REFPROPERTYKEY key, REFPROPVARIANT value)
{
    return g_failingStep == FAIL_SET_APP_ID ? E_ACCESSDENIED : store->lpVtbl->SetValue(store, key, value);
}

static HRESULT FailableGetValue(IPropertyStore *store, REFPROPERTYKEY key, PROPVARIANT *value)
{
    return g_failingStep == FAIL_GET_APP_ID ? E_ACCESSDENIED : store->lpVtbl->GetValue(store, key, value);
}

static HRESULT FailableCommit(IPropertyStore *store)
{
    return g_failingStep == FAIL_COMMIT_APP_ID ? E_ACCESSDENIED : store->lpVtbl->Commit(store);
}

static HRESULT FailableSave(IPersistFile *file, const WCHAR *path, BOOL remember)
{
    g_saveCalls++;
    return g_failingStep == FAIL_SAVE ? E_ACCESSDENIED : file->lpVtbl->Save(file, path, remember);
}

static HRESULT FailableLoad(IPersistFile *file, const WCHAR *path, DWORD mode)
{
    g_linkLoads++;
    return g_failingStep == FAIL_LOAD ? E_ACCESSDENIED : file->lpVtbl->Load(file, path, mode);
}

/* The real attributes; with g_reportFirstWriteTime, every read after the
 * first reports the first one's write time, as a save within the same two
 * seconds would. */
static BOOL WINAPI FixtureFileAttributes(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID data)
{
    WIN32_FILE_ATTRIBUTE_DATA *attributes = (WIN32_FILE_ATTRIBUTE_DATA *)data;
    if (!GetFileAttributesExW(path, level, data)) return FALSE;
    if (g_reportFirstWriteTime && level == GetFileExInfoStandard) {
        if (g_attributeReads == 0) g_firstWriteTime = attributes->ftLastWriteTime;
        else attributes->ftLastWriteTime = g_firstWriteTime;
    }
    g_attributeReads++;
    return TRUE;
}

static void WINAPI FixtureShellNotice(LONG event, UINT flags, LPCVOID first, LPCVOID second)
{
    (void)event; (void)flags; (void)first; (void)second;
    g_shellNotices++;
}

#undef IShellLinkW_SetPath
#undef IShellLinkW_SetArguments
#undef IShellLinkW_SetIconLocation
#undef IShellLinkW_SetDescription
#undef IShellLinkW_SetWorkingDirectory
#undef IShellLinkW_QueryInterface
#undef IShellLinkW_Release
#undef IPersistFile_Release
#undef IPersistFile_Save
#undef IPersistFile_Load
#undef IPropertyStore_Release
#undef IPropertyStore_SetValue
#undef IPropertyStore_GetValue
#undef IPropertyStore_Commit
#define IShellLinkW_SetPath(link, text) FailableSetString(link, FAIL_SET_PATH, text)
#define IShellLinkW_SetArguments(link, text) FailableSetString(link, FAIL_SET_ARGUMENTS, text)
#define IShellLinkW_SetIconLocation(link, path, index) FailableSetIconLocation(link, path, index)
#define IShellLinkW_SetDescription(link, text) FailableSetString(link, FAIL_SET_DESCRIPTION, text)
#define IShellLinkW_SetWorkingDirectory(link, text) FailableSetString(link, FAIL_SET_WORKING_DIRECTORY, text)
#define IShellLinkW_QueryInterface(link, iid, out) FailableQueryInterface(link, iid, out)
#define IShellLinkW_Release(link) FixtureReleaseLink(link)
#define IPersistFile_Release(file) FixtureReleaseFile(file)
#define IPersistFile_Save(file, path, remember) FailableSave(file, path, remember)
#define IPersistFile_Load(file, path, mode) FailableLoad(file, path, mode)
#define IPropertyStore_Release(store) FixtureReleaseStore(store)
#define IPropertyStore_SetValue(store, key, value) FailableSetValue(store, key, value)
#define IPropertyStore_GetValue(store, key, value) FailableGetValue(store, key, value)
#define IPropertyStore_Commit(store) FailableCommit(store)
#define CoCreateInstance FailableCreateInstance
#define CoTaskMemAlloc FailableTaskMemAlloc
#define GetFileAttributesExW FixtureFileAttributes
#define SHChangeNotify FixtureShellNotice
#define Shortcut_Read FixtureShortcutRead
#define Shortcut_HasAppId FixtureShortcutHasAppId
#define Shortcut_WriteForProfile FixtureShortcutWriteForProfile
#define Shortcut_CreateForProfile FixtureShortcutCreateForProfile
#define Shortcut_StartMenuDir FixtureShortcutStartMenuDir
#define Shortcut_CreateManagerLink FixtureShortcutCreateManagerLink
#define Shortcut_RestoreManagerLink FixtureShortcutRestoreManagerLink
#define Shortcut_DesktopPathFor FixtureShortcutDesktopPathFor
#define Shortcut_IsOnDesktop FixtureShortcutIsOnDesktop
#define Shortcut_Update FixtureShortcutUpdate
#define Shortcut_ListOurs FixtureShortcutListOurs
#define Shortcut_IsInStartMenu FixtureShortcutIsInStartMenu
#define Shortcut_AddToStartMenu FixtureShortcutAddToStartMenu
#define Shortcut_RemoveFromStartMenu FixtureShortcutRemoveFromStartMenu
#define Shortcut_RemoveOurs FixtureShortcutRemoveOurs
#define Shortcut_RemoveManagerLink FixtureShortcutRemoveManagerLink
#define Shortcut_IsAtStartup FixtureShortcutIsAtStartup
#define Shortcut_SetStartup FixtureShortcutSetStartup
#define Shortcut_RenamedPath FixtureShortcutRenamedPath
#define Shortcut_Refresh FixtureShortcutRefresh
#define Shortcut_RefreshProfiles FixtureShortcutRefreshProfiles
#include "../src/shortcuts.c"
#undef CoCreateInstance
#undef CoTaskMemAlloc
#undef GetFileAttributesExW
#undef SHChangeNotify

/* -------------------------------------------------------------- helpers */

static WCHAR g_root[MAX_PATH], g_exe[MAX_PATH];

static BOOL PrivatePath(const WCHAR *name, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", g_root, name));
}

static BOOL WriteTextFile(const WCHAR *path, const char *text)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD length = (DWORD)strlen(text), written = 0;
    BOOL ok;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, text, length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

static BOOL WritePlainFile(const WCHAR *path)
{
    return WriteTextFile(path, "");
}

static void SetFixtureProfile(Profile *profile, const WCHAR *name, const WCHAR *folder)
{
    ZeroMemory(profile, sizeof *profile);
    StringCchCopyW(profile->name, ARRAYSIZE(profile->name), name);
    StringCchCopyW(profile->folder, ARRAYSIZE(profile->folder), folder);
}

/* What a saved shortcut shows, read without the fixtures. */
static BOOL ReadIconAndDescription(const WCHAR *lnk, WCHAR *icon, size_t iconCch, WCHAR *description, size_t descriptionCch)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    int index = 0;
    BOOL read = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return FALSE;
    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file)) &&
        SUCCEEDED(file->lpVtbl->Load(file, lnk, STGM_READ)) &&
        SUCCEEDED(link->lpVtbl->GetIconLocation(link, icon, (int)iconCch, &index)) &&
        SUCCEEDED(link->lpVtbl->GetDescription(link, description, (int)descriptionCch)))
        read = TRUE;
    if (file) file->lpVtbl->Release(file);
    link->lpVtbl->Release(link);
    return read;
}

static BOOL HasIcon(const WCHAR *lnk, const WCHAR *expected)
{
    WCHAR icon[MAX_PATH], description[DESCRIPTION_CCH];
    return ReadIconAndDescription(lnk, icon, ARRAYSIZE(icon), description, ARRAYSIZE(description)) && Core_PathEquals(icon, expected);
}

static ULONGLONG WriteTimeOf(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    ULARGE_INTEGER ticks;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) return 0;
    ticks.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    return ticks.QuadPart;
}

static ULONGLONG FileTimeTicks(const FILETIME *time)
{
    ULARGE_INTEGER ticks;
    ticks.LowPart = time->dwLowDateTime;
    ticks.HighPart = time->dwHighDateTime;
    return ticks.QuadPart;
}

/* ---------------------------------------------------------------- tests */

static void CheckWriteFailures(const WCHAR *lnk)
{
    int step;
    HRESULT hr;
    for (step = FIRST_WRITE_STEP; step <= LAST_WRITE_STEP; step++) {
        g_failingStep = (FailingStep)step;
        g_saveCalls = g_heldInterfaces = 0;
        hr = WriteLink(lnk, g_exe, L"--fixture", g_exe, L"Private shortcut", g_root, FIXTURE_APP_ID);
        CheckStep(g_failingStep, "COM and allocation failures reach the caller",
                  hr == (g_failingStep == FAIL_ALLOCATE_APP_ID ? E_OUTOFMEMORY : E_ACCESSDENIED));
        CheckStep(g_failingStep, "no partial shortcut is saved", !Util_FileExists(lnk));
        CheckStep(g_failingStep, "failed setup never reaches Save", g_saveCalls == (g_failingStep == FAIL_SAVE ? 1 : 0));
        CheckStep(g_failingStep, "every interface taken is released", g_heldInterfaces == 0);
        DeleteFileW(lnk);
    }
    g_failingStep = FAIL_NONE;
}

static void CheckReading(const WCHAR *lnk, const WCHAR *missing, const WCHAR *notLink)
{
    static const FailingStep readerSteps[] = { FAIL_CREATE_LINK, FAIL_QUERY_PERSIST_FILE, FAIL_LOAD };
    static const FailingStep appIdSteps[] = { FAIL_CREATE_LINK, FAIL_QUERY_PERSIST_FILE, FAIL_LOAD, FAIL_QUERY_PROPERTY_STORE,
                                              FAIL_GET_APP_ID };
    LinkInfo info;
    size_t i;
    BOOL read;
    g_heldInterfaces = 0;
    Check("a shortcut is read with its target and arguments",
          FixtureShortcutRead(lnk, &info) && Core_PathEquals(info.target, g_exe) && wcscmp(info.args, L"--fixture") == 0);
    Check("a shortcut carries the AppUserModelID it was written with", FixtureShortcutHasAppId(lnk, FIXTURE_APP_ID));
    Check("the AppUserModelID compares without case", FixtureShortcutHasAppId(lnk, L"PRIVATE.SHORTCUT.TEST"));
    Check("another AppUserModelID is not the shortcut's", !FixtureShortcutHasAppId(lnk, L"Another.App"));
    Check("a missing shortcut is not read", !FixtureShortcutRead(missing, &info) && !FixtureShortcutHasAppId(missing, FIXTURE_APP_ID));
    Check("a file that is not a shortcut is not read", !FixtureShortcutRead(notLink, &info));
    Check("reading releases every interface", g_heldInterfaces == 0);
    for (i = 0; i < ARRAYSIZE(readerSteps); i++) {
        g_failingStep = readerSteps[i];
        g_heldInterfaces = 0;
        read = FixtureShortcutRead(lnk, &info);
        CheckStep(g_failingStep, "a reader failure reads nothing", !read && !info.target[0] && !info.args[0]);
        CheckStep(g_failingStep, "a reader failure releases every interface", g_heldInterfaces == 0);
    }
    for (i = 0; i < ARRAYSIZE(appIdSteps); i++) {
        g_failingStep = appIdSteps[i];
        g_heldInterfaces = 0;
        CheckStep(g_failingStep, "an AppUserModelID that cannot be read is not the shortcut's",
                  !FixtureShortcutHasAppId(lnk, FIXTURE_APP_ID));
        CheckStep(g_failingStep, "the AppUserModelID check releases every interface", g_heldInterfaces == 0);
    }
    g_failingStep = FAIL_NONE;
}

static void CheckUpdate(const WCHAR *lnk, const WCHAR *missing)
{
    static const FailingStep updateSteps[] = { FAIL_CREATE_LINK, FAIL_QUERY_PERSIST_FILE, FAIL_LOAD, FAIL_SET_ICON_LOCATION,
                                               FAIL_SET_DESCRIPTION, FAIL_SAVE };
    WCHAR description[DESCRIPTION_CCH], firstIcon[MAX_PATH], secondIcon[MAX_PATH], thirdIcon[MAX_PATH];
    Profile profile;
    HRESULT hr;
    ULONGLONG savedTime;
    size_t i;

    SetFixtureProfile(&profile, L"Fixture", FIXTURE_FOLDER);
    ProfileDescription(&profile, description, ARRAYSIZE(description));
    if (!PrivatePath(L"first.ico", firstIcon, ARRAYSIZE(firstIcon)) || !PrivatePath(L"second.ico", secondIcon, ARRAYSIZE(secondIcon)) ||
        !PrivatePath(L"third.ico", thirdIcon, ARRAYSIZE(thirdIcon)) ||
        FAILED(WriteLink(lnk, g_exe, L"--fixture", firstIcon, description, g_root, FIXTURE_APP_ID))) {
        Check("the shortcut to update is written", FALSE);
        return;
    }
    g_saveCalls = g_heldInterfaces = g_shellNotices = 0;
    hr = FixtureShortcutUpdate(lnk, &profile, firstIcon);
    Check("a shortcut that already shows the icon and description is left as it is (S_FALSE)", hr == S_FALSE && g_saveCalls == 0);
    Check("an update releases every interface", g_heldInterfaces == 0);
    hr = FixtureShortcutUpdate(lnk, &profile, secondIcon);
    Check("a shortcut with another icon is saved with the new one (S_OK)", hr == S_OK && HasIcon(lnk, secondIcon));

    /* A save within the same two seconds as the last one. */
    g_reportFirstWriteTime = TRUE;
    g_attributeReads = 0;
    hr = FixtureShortcutUpdate(lnk, &profile, thirdIcon);
    g_reportFirstWriteTime = FALSE;
    savedTime = WriteTimeOf(lnk);
    Check("a save the taskbar could take for the same one gets a write time two seconds later",
          hr == S_OK && g_attributeReads == 2 && savedTime == FileTimeTicks(&g_firstWriteTime) + FAT_TIME_STEP_TICKS);

    Check("a missing shortcut is reported with Windows' error",
          FixtureShortcutUpdate(missing, &profile, firstIcon) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    for (i = 0; i < ARRAYSIZE(updateSteps); i++) {
        g_failingStep = updateSteps[i];
        g_heldInterfaces = 0;
        hr = FixtureShortcutUpdate(lnk, &profile, firstIcon);
        g_failingStep = FAIL_NONE;
        CheckStep(updateSteps[i], "an update failure reaches the caller", hr == E_ACCESSDENIED);
        CheckStep(updateSteps[i], "a failed update leaves the shortcut as it was", HasIcon(lnk, thirdIcon));
        CheckStep(updateSteps[i], "a failed update releases every interface", g_heldInterfaces == 0);
    }
    Check("an update leaves telling the shell to its caller", g_shellNotices == 0);
}

static void CheckRenamedPaths(void)
{
    WCHAR named[MAX_PATH], numbered[MAX_PATH], other[MAX_PATH], taken[MAX_PATH], expected[MAX_PATH], out[MAX_PATH];
    Profile before, after;
    if (!PrivatePath(L"Claude (Work).lnk", named, ARRAYSIZE(named)) ||
        !PrivatePath(L"Claude (Work) (3).lnk", numbered, ARRAYSIZE(numbered)) ||
        !PrivatePath(L"Other.lnk", other, ARRAYSIZE(other)) || !PrivatePath(L"Claude (Home).lnk", taken, ARRAYSIZE(taken)) ||
        !WritePlainFile(named) || !WritePlainFile(numbered)) {
        Check("the shortcuts to rename are made", FALSE);
        return;
    }
    SetFixtureProfile(&before, L"Work", FIXTURE_FOLDER);
    SetFixtureProfile(&after, L"Home", FIXTURE_FOLDER);
    Check("a shortcut named after the profile takes its new name",
          FixtureShortcutRenamedPath(named, &before, &after, out, ARRAYSIZE(out)) && Core_PathEquals(out, taken));
    Check("a numbered shortcut named after the profile takes its new name",
          FixtureShortcutRenamedPath(numbered, &before, &after, out, ARRAYSIZE(out)) && Core_PathEquals(out, taken));
    if (WritePlainFile(taken) && PrivatePath(L"Claude (Home) (2).lnk", expected, ARRAYSIZE(expected)))
        Check("a new name already taken gets the next number",
              FixtureShortcutRenamedPath(named, &before, &after, out, ARRAYSIZE(out)) && wcscmp(out, expected) == 0);
    DeleteFileW(taken);
    Check("a shortcut with a name of its own keeps it", !FixtureShortcutRenamedPath(other, &before, &after, out, ARRAYSIZE(out)));
    Check("an unchanged name keeps the path", !FixtureShortcutRenamedPath(named, &before, &before, out, ARRAYSIZE(out)));
    SetFixtureProfile(&after, L"WORK", FIXTURE_FOLDER);
    Check("a name changed in case only renames the shortcut in place",
          FixtureShortcutRenamedPath(named, &before, &after, out, ARRAYSIZE(out)) && Core_PathEquals(out, named) &&
          wcsstr(out, L"Claude (WORK).lnk") != NULL);
    DeleteFileW(named);
    DeleteFileW(numbered);
}

/* A private volatile key stands for HKEY_CURRENT_USER, with REG_SHORTCUTS
 * made volatile in it: keys made under a volatile one must be volatile. */
static HKEY OpenPrivateUserKey(WCHAR *path, size_t cch)
{
    HKEY root = NULL, shortcuts = NULL;
    DWORD disposition = 0;
    if (FAILED(StringCchPrintfW(path, cch, FIXTURE_KEY_PREFIX L"%lu.%llu", GetCurrentProcessId(), GetTickCount64())) ||
        RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &root, &disposition) != ERROR_SUCCESS)
        return NULL;
    if (disposition != REG_CREATED_NEW_KEY ||
        RegCreateKeyExW(root, REG_SHORTCUTS, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &shortcuts, NULL) != ERROR_SUCCESS) {
        RegCloseKey(root);
        if (disposition == REG_CREATED_NEW_KEY) RegDeleteTreeW(HKEY_CURRENT_USER, path);
        return NULL;
    }
    RegCloseKey(shortcuts);
    return root;
}

static BOOL SetRecord(const WCHAR *name, DWORD type, const void *data, DWORD bytes)
{
    HKEY key;
    LSTATUS status;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SHORTCUTS, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return FALSE;
    status = RegSetValueExW(key, name, 0, type, (const BYTE *)data, bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static BOOL IsRecorded(const WCHAR *lnk)
{
    return Util_RegValueExists(HKEY_CURRENT_USER, REG_SHORTCUTS, lnk);
}

static BOOL IsRecordedFor(const WCHAR *lnk, const WCHAR *folder)
{
    WCHAR data[FOLDER_CCH];
    return Util_RegGetString(HKEY_CURRENT_USER, REG_SHORTCUTS, lnk, data, ARRAYSIZE(data)) && wcscmp(data, folder) == 0;
}

static void CheckRecordedLinks(void)
{
    static const WCHAR unterminated[] = { L'C', L'l', L'a', L'u', L'd', L'e', L'-', L'R', L'e', L'c', L'o', L'r', L'd', L'e', L'd' };
    WCHAR longFolder[FOLDER_CCH + 16], longName[MAX_PATH + 16], name[64];
    DWORD number = 7;
    PathSet set;
    int i;
    BOOL written = TRUE;

    for (i = 0; i < (int)ARRAYSIZE(longFolder) - 1; i++) longFolder[i] = L'x';
    longFolder[ARRAYSIZE(longFolder) - 1] = 0;
    for (i = 0; i < (int)ARRAYSIZE(longName) - 1; i++) longName[i] = L'n';
    longName[ARRAYSIZE(longName) - 1] = 0;
    written = SetRecord(L"C:\\fixture\\a.lnk", REG_SZ, RECORDED_FOLDER, sizeof RECORDED_FOLDER) &&
              SetRecord(L"C:\\fixture\\b.lnk", REG_SZ, L"claude-recorded", sizeof L"claude-recorded") &&
              SetRecord(L"C:\\fixture\\c.lnk", REG_SZ, L"Claude-Other", sizeof L"Claude-Other") &&
              SetRecord(L"C:\\fixture\\unterminated.lnk", REG_SZ, unterminated, sizeof unterminated) &&
              SetRecord(L"C:\\fixture\\number.lnk", REG_DWORD, &number, sizeof number) &&
              SetRecord(L"C:\\fixture\\long-folder.lnk", REG_SZ, longFolder, sizeof longFolder) &&
              SetRecord(longName, REG_SZ, RECORDED_FOLDER, sizeof RECORDED_FOLDER);
    if (!written) {
        Check("the private records are written", FALSE);
        return;
    }
    LoadRecordedLinks(RECORDED_FOLDER, &set);
    Check("the records of a profile are read, its folder compared without case",
          set.count == 3 && PathSetHas(&set, L"C:\\fixture\\a.lnk") && PathSetHas(&set, L"C:\\fixture\\b.lnk"));
    Check("a record whose folder lacks its terminating NUL is read", PathSetHas(&set, L"C:\\fixture\\unterminated.lnk"));
    if (set.paths) HeapFree(GetProcessHeap(), 0, set.paths);
    LoadRecordedLinks(NULL, &set);
    Check("every profile's records are read, without the wrong type and the oversized ones",
          set.count == 4 && PathSetHas(&set, L"C:\\fixture\\c.lnk") && !PathSetHas(&set, L"C:\\fixture\\number.lnk") &&
          !PathSetHas(&set, L"C:\\fixture\\long-folder.lnk"));
    if (set.paths) HeapFree(GetProcessHeap(), 0, set.paths);

    for (i = 0; i < MANY_RECORDS && written; i++)
        written = SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), L"C:\\fixture\\many-%03d.lnk", i)) &&
                  SetRecord(name, REG_SZ, L"Claude-Many", sizeof L"Claude-Many");
    LoadRecordedLinks(L"Claude-Many", &set);
    Check("every record is read, however many there are", written && set.count == MANY_RECORDS);
    if (set.paths) HeapFree(GetProcessHeap(), 0, set.paths);
}

/* A shortcut that cannot be deleted: held open without sharing, or read-only. */
typedef struct StuckLinks {
    WCHAR  held[MAX_PATH], readOnly[MAX_PATH], removable[MAX_PATH];
    HANDLE holder;
} StuckLinks;

static BOOL MakeStuckLinks(StuckLinks *links, const WCHAR *prefix)
{
    WCHAR name[64];
    ZeroMemory(links, sizeof *links);
    links->holder = INVALID_HANDLE_VALUE;
    if (FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s-held.lnk", prefix)) || !PrivatePath(name, links->held, MAX_PATH) ||
        FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s-read-only.lnk", prefix)) ||
        !PrivatePath(name, links->readOnly, MAX_PATH) ||
        FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s-removable.lnk", prefix)) ||
        !PrivatePath(name, links->removable, MAX_PATH) ||
        !WritePlainFile(links->held) || !WritePlainFile(links->readOnly) || !WritePlainFile(links->removable) ||
        !SetFileAttributesW(links->readOnly, FILE_ATTRIBUTE_READONLY))
        return FALSE;
    links->holder = CreateFileW(links->held, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    RecordLink(links->held, FIXTURE_FOLDER);
    RecordLink(links->readOnly, FIXTURE_FOLDER);
    RecordLink(links->removable, FIXTURE_FOLDER);
    return links->holder != INVALID_HANDLE_VALUE && IsRecorded(links->held) && IsRecorded(links->readOnly) &&
           IsRecorded(links->removable);
}

static void FreeStuckLinks(StuckLinks *links)
{
    if (links->holder != INVALID_HANDLE_VALUE) CloseHandle(links->holder);
    links->holder = INVALID_HANDLE_VALUE;
    SetFileAttributesW(links->readOnly, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(links->held);
    DeleteFileW(links->readOnly);
    DeleteFileW(links->removable);
    ForgetLink(links->held);
    ForgetLink(links->readOnly);
    ForgetLink(links->removable);
}

static void CheckRemovalVisitors(void)
{
    StuckLinks links;
    StartMenuSearch startMenu;
    StartupSearch startup;
    LinkInfo info;
    WCHAR kept[MAX_PATH];
    const HRESULT heldFailure = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);

    ZeroMemory(&info, sizeof info);
    StringCchPrintfW(info.target, ARRAYSIZE(info.target), L"%s\\" APP_EXE, g_root);
    StringCchCopyW(info.args, ARRAYSIZE(info.args), L"--launch \"" FIXTURE_FOLDER L"\"");

    ZeroMemory(&startMenu, sizeof startMenu);
    StringCchCopyW(startMenu.programs, ARRAYSIZE(startMenu.programs), g_root);
    startMenu.remove = TRUE;
    startMenu.result = S_OK;
    if (MakeStuckLinks(&links, L"start-menu")) {
        g_shellNotices = 0;
        StartMenuVisitor(links.held, &info, TRUE, &startMenu);
        StartMenuVisitor(links.readOnly, &info, TRUE, &startMenu);
        StartMenuVisitor(links.removable, &info, TRUE, &startMenu);
        Check("a Start menu removal returns its first failure", startMenu.found && startMenu.result == heldFailure);
        Check("a Start menu shortcut that cannot be deleted stays recorded",
              Util_FileExists(links.held) && IsRecorded(links.held) && IsRecorded(links.readOnly));
        Check("a deleted Start menu shortcut is forgotten", !Util_FileExists(links.removable) && !IsRecorded(links.removable));
        Check("the shell is told about the deleted shortcut only", g_shellNotices == 1);
    } else {
        Check("the Start menu shortcuts are made", FALSE);
    }
    FreeStuckLinks(&links);

    ZeroMemory(&startup, sizeof startup);
    startup.folder = FIXTURE_FOLDER;
    startup.remove = TRUE;
    startup.result = S_OK;
    if (MakeStuckLinks(&links, L"startup")) {
        StartupVisitor(links.held, &info, &startup);
        StartupVisitor(links.readOnly, &info, &startup);
        StartupVisitor(links.removable, &info, &startup);
        Check("a Startup removal returns its first failure", startup.found && startup.result == heldFailure);
        Check("a Startup shortcut that cannot be deleted stays recorded", IsRecorded(links.held) && IsRecorded(links.readOnly));
        Check("a deleted Startup shortcut is forgotten", !Util_FileExists(links.removable) && !IsRecorded(links.removable));
    } else {
        Check("the Startup shortcuts are made", FALSE);
    }
    FreeStuckLinks(&links);

    if (MakeStuckLinks(&links, L"uninstall") && SUCCEEDED(StringCchCopyW(kept, ARRAYSIZE(kept), links.removable))) {
        RemoveVisitor(links.held, &info, TRUE, kept);
        RemoveVisitor(links.removable, &info, TRUE, kept);
        RemoveVisitor(links.readOnly, NULL, TRUE, kept);
        Check("the shortcut kept for last is not removed", Util_FileExists(links.removable) && IsRecorded(links.removable));
        Check("a shortcut that cannot be removed stays recorded", Util_FileExists(links.held) && IsRecorded(links.held));
        Check("a record that does not lead to one of our shortcuts is only forgotten",
              Util_FileExists(links.readOnly) && !IsRecorded(links.readOnly));
    } else {
        Check("the shortcuts to remove are made", FALSE);
    }
    FreeStuckLinks(&links);
}

/* What a search gave its visitor. */
typedef struct VisitRecord {
    int visits, withoutShortcut;
} VisitRecord;

static BOOL RecordVisit(const WCHAR *path, const LinkInfo *info, BOOL recorded, void *context)
{
    VisitRecord *record = (VisitRecord *)context;
    (void)path; (void)recorded;
    record->visits++;
    if (!info) record->withoutShortcut++;
    return TRUE;
}

/* A record whose shortcut was deleted by hand is forgotten by the first
 * search; one on a folder that cannot be found is not. */
static void CheckDeletedRecords(void)
{
    WCHAR deleted[MAX_PATH], away[MAX_PATH], notLink[MAX_PATH];
    VisitRecord record;
    if (!PrivatePath(L"deleted.lnk", deleted, ARRAYSIZE(deleted)) ||
        !PrivatePath(L"missing folder\\away.lnk", away, ARRAYSIZE(away)) || !PrivatePath(L"not-a-link.lnk", notLink, ARRAYSIZE(notLink)) ||
        !RecordLink(deleted, DEAD_FOLDER) || !RecordLink(away, DEAD_FOLDER) || !RecordLink(notLink, DEAD_FOLDER)) {
        Check("the records of missing shortcuts are written", FALSE);
        return;
    }
    ZeroMemory(&record, sizeof record);
    g_heldInterfaces = 0;
    VisitOurLinks(DEAD_FOLDER, 0, RecordVisit, &record);
    Check("a record whose shortcut was deleted from its folder is forgotten, not visited", !IsRecorded(deleted));
    Check("a record whose folder cannot be found, or that leads to no shortcut, is kept and visited without one",
          IsRecorded(away) && IsRecorded(notLink) && record.visits == 2 && record.withoutShortcut == 2);
    Check("a search releases every interface", g_heldInterfaces == 0);
    ForgetLink(away);
    ForgetLink(notLink);
}

/* A recorded shortcut named after the profile, renamed with it. */
static void CheckRenamedRecord(void)
{
    WCHAR named[MAX_PATH], renamed[MAX_PATH], icon[MAX_PATH], description[DESCRIPTION_CCH];
    Profile before, after;
    RefreshSearch search;
    LinkInfo info;
    SetFixtureProfile(&before, L"Work", FIXTURE_FOLDER);
    SetFixtureProfile(&after, L"Home", FIXTURE_FOLDER);
    ProfileDescription(&before, description, ARRAYSIZE(description));
    if (!PrivatePath(L"Claude (Work).lnk", named, ARRAYSIZE(named)) || !PrivatePath(L"Claude (Home).lnk", renamed, ARRAYSIZE(renamed)) ||
        !PrivatePath(L"renamed.ico", icon, ARRAYSIZE(icon)) ||
        FAILED(WriteLink(named, g_exe, L"--launch \"" FIXTURE_FOLDER L"\"", g_exe, description, g_root, FIXTURE_APP_ID)) ||
        !RecordLink(named, FIXTURE_FOLDER)) {
        Check("the recorded shortcut to rename is made", FALSE);
        return;
    }
    ZeroMemory(&info, sizeof info);
    search.before = &before;
    search.after = &after;
    search.icon = icon;
    search.changed = FALSE;
    g_shellNotices = 0;
    RefreshVisitor(named, &info, TRUE, &search);
    Check("a recorded shortcut named after the profile gets its new icon and name",
          search.changed && !Util_FileExists(named) && HasIcon(renamed, icon));
    Check("its record follows it to its new path", !IsRecorded(named) && IsRecordedFor(renamed, FIXTURE_FOLDER));
    Check("the shell is told about the update and the rename", g_shellNotices == 2);
    search.before = &after;
    search.changed = FALSE;
    RefreshVisitor(renamed, &info, TRUE, &search);
    Check("a shortcut already up to date is left as it is, and its record too",
          !search.changed && Util_FileExists(renamed) && IsRecordedFor(renamed, FIXTURE_FOLDER));
    ForgetLink(renamed);
    DeleteFileW(renamed);
}

/* After a change of language, the one pass over every profile's shortcuts:
 * each gets its own profile's icon and description; a profile with no icon
 * keeps its shortcuts as they are. */
static void CheckLanguagePass(void)
{
    WCHAR work[MAX_PATH], home[MAX_PATH], workIcon[MAX_PATH], description[DESCRIPTION_CCH];
    const WCHAR *icons[2];
    ProfileList list;
    LanguageSearch search;
    LinkInfo info;
    ZeroMemory(&list, sizeof list);
    list.count = 2;
    SetFixtureProfile(&list.items[0], L"Work", FIXTURE_FOLDER);
    SetFixtureProfile(&list.items[1], L"Home", RECORDED_FOLDER);
    ProfileDescription(&list.items[0], description, ARRAYSIZE(description));
    if (!PrivatePath(L"language-work.lnk", work, ARRAYSIZE(work)) || !PrivatePath(L"language-home.lnk", home, ARRAYSIZE(home)) ||
        !PrivatePath(L"language-work.ico", workIcon, ARRAYSIZE(workIcon)) ||
        FAILED(WriteLink(work, g_exe, L"--launch \"" FIXTURE_FOLDER L"\"", g_exe, L"Old description", g_root, FIXTURE_APP_ID)) ||
        FAILED(WriteLink(home, g_exe, L"--launch \"" RECORDED_FOLDER L"\"", g_exe, L"Old description", g_root, FIXTURE_APP_ID))) {
        Check("the shortcuts of the language pass are written", FALSE);
        return;
    }
    icons[0] = workIcon;
    icons[1] = NULL;
    search.list = &list;
    search.icons = icons;
    ZeroMemory(&info, sizeof info);
    StringCchCopyW(info.args, ARRAYSIZE(info.args), L"--launch \"" FIXTURE_FOLDER L"\"");
    LanguageVisitor(work, &info, TRUE, &search);
    StringCchCopyW(info.args, ARRAYSIZE(info.args), L"--launch \"" RECORDED_FOLDER L"\"");
    LanguageVisitor(home, &info, TRUE, &search);
    Check("a language pass gives a shortcut its own profile's icon", HasIcon(work, workIcon));
    Check("a language pass leaves the shortcuts of a profile without an icon as they are", HasIcon(home, g_exe));
    DeleteFileW(work);
    DeleteFileW(home);
}

/* What a folder search gave its visitor: how many shortcuts, and the
 * arguments of `watched`. */
typedef struct FolderVisits {
    const WCHAR *watched;
    int          visits;
    WCHAR        args[64];
} FolderVisits;

static BOOL CountFolderVisit(const WCHAR *path, const LinkInfo *info, void *context)
{
    FolderVisits *visits = (FolderVisits *)context;
    visits->visits++;
    if (Core_PathEquals(path, visits->watched)) StringCchCopyW(visits->args, ARRAYSIZE(visits->args), info->args);
    return TRUE;
}

/* A folder's shortcuts are read once, then come from what was read; one that
 * changed is read again. */
static void CheckFolderReadOnce(void)
{
    WCHAR dir[MAX_PATH], first[MAX_PATH], second[MAX_PATH];
    LinkReader reader;
    FolderVisits visits;
    int loads;
    BOOL ready, readerOpen = FALSE;
    ZeroMemory(&visits, sizeof visits);
    ready = PrivatePath(L"read-once", dir, ARRAYSIZE(dir)) && CreateDirectoryW(dir, NULL) &&
            SUCCEEDED(StringCchPrintfW(first, ARRAYSIZE(first), L"%s\\first.lnk", dir)) &&
            SUCCEEDED(StringCchPrintfW(second, ARRAYSIZE(second), L"%s\\second.lnk", dir)) &&
            SUCCEEDED(WriteLink(first, g_exe, L"--first", g_exe, L"First", g_root, FIXTURE_APP_ID)) &&
            SUCCEEDED(WriteLink(second, g_exe, L"--second", g_exe, L"Second", g_root, FIXTURE_APP_ID)) &&
            (readerOpen = OpenLinkReader(&reader)) != FALSE;
    Check("the shortcuts of a folder to search are made", ready);
    if (ready) {
        visits.watched = first;
        g_linkLoads = 0;
        VisitFolderLinks(&reader, dir, NULL, CountFolderVisit, &visits);
        loads = g_linkLoads;
        VisitFolderLinks(&reader, dir, NULL, CountFolderVisit, &visits);
        Check("a folder's shortcuts are read once, then come from what was read", loads == 2 && g_linkLoads == 2 && visits.visits == 4);
        /* Longer arguments: the size changes, whatever the clock's step. */
        if (SUCCEEDED(WriteLink(first, g_exe, L"--first --changed", g_exe, L"First", g_root, FIXTURE_APP_ID))) {
            VisitFolderLinks(&reader, dir, NULL, CountFolderVisit, &visits);
            Check("a shortcut that changed is read again, the others are not",
                  g_linkLoads == 3 && wcscmp(visits.args, L"--first --changed") == 0);
        } else {
            Check("the shortcut to change is written again", FALSE);
        }
    }
    if (readerOpen) CloseLinkReader(&reader);
    DeleteFileW(first);
    DeleteFileW(second);
    RemoveDirectoryW(dir);
}

/* What counts as the Start menu: below Programs, but not in Startup. */
static void CheckStartMenuPlaces(void)
{
    WCHAR inStartMenu[MAX_PATH], inStartup[MAX_PATH], outside[MAX_PATH];
    StartMenuSearch search;
    LinkInfo info;
    ZeroMemory(&info, sizeof info);
    ZeroMemory(&search, sizeof search);
    if (!PrivatePath(L"Programs", search.programs, ARRAYSIZE(search.programs)) ||
        !PrivatePath(L"Programs\\Startup", search.startup, ARRAYSIZE(search.startup)) ||
        !PrivatePath(L"Programs\\" APP_NAME L"\\Claude (Work).lnk", inStartMenu, ARRAYSIZE(inStartMenu)) ||
        !PrivatePath(L"Programs\\Startup\\Claude (Work).lnk", inStartup, ARRAYSIZE(inStartup)) ||
        !PrivatePath(L"Desktop\\Claude (Work).lnk", outside, ARRAYSIZE(outside))) {
        Check("the Start menu paths are made", FALSE);
        return;
    }
    Check("a shortcut in the Startup folder does not put the profile in the Start menu",
          StartMenuVisitor(inStartup, &info, TRUE, &search) && !search.found);
    Check("a shortcut outside Programs does not put the profile in the Start menu",
          StartMenuVisitor(outside, &info, TRUE, &search) && !search.found);
    Check("a record that leads to no shortcut does not put the profile in the Start menu",
          StartMenuVisitor(inStartMenu, NULL, TRUE, &search) && !search.found);
    Check("a shortcut below Programs puts the profile in the Start menu, and ends the search",
          !StartMenuVisitor(inStartMenu, &info, TRUE, &search) && search.found);
}

/* The private folder and what the tests left in it (not a tree). */
static BOOL RemovePrivateFolder(void)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    if (!PrivatePath(L"*", pattern, ARRAYSIZE(pattern))) return FALSE;
    search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !PrivatePath(found.cFileName, path, ARRAYSIZE(path)))
                continue;
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(path);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return RemoveDirectoryW(g_root);
}

int wmain(void)
{
    WCHAR temporary[MAX_PATH], lnk[MAX_PATH], missing[MAX_PATH], notLink[MAX_PATH], keyPath[128];
    GUID id;
    HKEY privateUser;
    BOOL comReady, rootCreated = FALSE;
    const char *setupFailure = NULL;

    comReady = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    if (!comReady) {
        setupFailure = "COM could not be initialized";
    } else if (!GetTempPathW(ARRAYSIZE(temporary), temporary) || FAILED(CoCreateGuid(&id)) ||
               FAILED(StringCchPrintfW(g_root, ARRAYSIZE(g_root), L"%sClaude-shortcuts-%08lX", temporary, id.Data1))) {
        setupFailure = "no private folder name could be made";
    } else {
        rootCreated = CreateDirectoryW(g_root, NULL);
        if (!rootCreated)
            setupFailure = "the private folder could not be created";
        else if (!Util_SelfExe(g_exe, ARRAYSIZE(g_exe)) || !PrivatePath(L"fixture.lnk", lnk, ARRAYSIZE(lnk)) ||
                 !PrivatePath(L"missing.lnk", missing, ARRAYSIZE(missing)) ||
                 !PrivatePath(L"not-a-link.lnk", notLink, ARRAYSIZE(notLink)) ||
                 !WriteTextFile(notLink, "This text is not a shell link, whatever its name says."))
            setupFailure = "the fixture paths could not be made";
    }
    if (setupFailure) {
        printf("Shortcut tests could not start: %s (error %lu).\n", setupFailure, GetLastError());
        if (rootCreated) RemovePrivateFolder();
        if (comReady) CoUninitialize();
        return 1;
    }
    Util_SetStateDir(g_root);   /* what shortcuts.c logs */

    CheckWriteFailures(lnk);
    g_heldInterfaces = 0;
    Check("a valid shortcut is saved",
          SUCCEEDED(WriteLink(lnk, g_exe, L"--fixture", g_exe, L"Private shortcut", g_root, FIXTURE_APP_ID)) && Util_FileExists(lnk));
    Check("a saved shortcut releases every interface", g_heldInterfaces == 0);
    CheckReading(lnk, missing, notLink);
    CheckUpdate(lnk, missing);
    CheckRenamedPaths();
    CheckLanguagePass();
    CheckFolderReadOnce();

    privateUser = OpenPrivateUserKey(keyPath, ARRAYSIZE(keyPath));
    if (privateUser && RegOverridePredefKey(HKEY_CURRENT_USER, privateUser) == ERROR_SUCCESS) {
        CheckRecordedLinks();
        CheckRemovalVisitors();
        CheckDeletedRecords();
        CheckRenamedRecord();
        CheckStartMenuPlaces();
        RegOverridePredefKey(HKEY_CURRENT_USER, NULL);
    } else {
        Check("a private key stands for HKEY_CURRENT_USER", FALSE);
    }
    if (privateUser) {
        RegCloseKey(privateUser);
        Check("the private key is removed", RegDeleteTreeW(HKEY_CURRENT_USER, keyPath) == ERROR_SUCCESS);
    }
    Check("every fixture step is one the fixtures know", g_unexpectedSteps == 0);
    Check("the private folder is removed", RemovePrivateFolder());
    CoUninitialize();
    printf("Shortcut tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
