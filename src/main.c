/* Command-line dispatch: the roles are listed at the top of app.h. */
#include "app.h"
#include <objbase.h>
#include <shellapi.h>

/* Kernel32 only: this decides the DLL search order, so it runs before
 * anything loads a library at run time, as SHGetKnownFolderPath (behind
 * Util_InstallExe) may. Should the Programs folder have been moved, the two
 * paths differ and the installed copy only gets the stricter search order. */
static BOOL RunningFromInstallFolder(void)
{
    WCHAR self[MAX_PATH], local[MAX_PATH], installed[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, ARRAYSIZE(self)), m = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    return n > 0 && n < ARRAYSIZE(self) && m > 0 && m < ARRAYSIZE(local) &&
           SUCCEEDED(StringCchPrintfW(installed, ARRAYSIZE(installed), L"%s\\Programs\\" APP_NAME L"\\" APP_EXE, local)) &&
           Core_EqualsI(self, installed);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR cmdLine, int show)
{
    LPWSTR *argv;
    const WCHAR *command, *argument;
    int argc = 0, rc;
    HRESULT hr;

    (void)previous;
    (void)cmdLine;
    (void)show;
    g_hInst = instance;
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    /* Run from a download folder (setup), libraries that Windows components
     * load by name come from System32 only, never from next to the exe. The
     * installed copy keeps the default order: its folder holds only this exe,
     * and the Save dialog's shell extensions may rely on it. */
    if (!RunningFromInstallFolder()) SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");
    /* Every path used here is absolute. A neutral working folder keeps this
     * process and the manager it starts from locking the folder they were
     * started from: a download folder, a USB drive. */
    {
        WCHAR system[MAX_PATH];
        if (GetSystemDirectoryW(system, ARRAYSIZE(system))) SetCurrentDirectoryW(system);
    }
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Localize_Init();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        /* Without its arguments, a link or a watcher must not become a manager window. */
        Util_Log(L"could not read the command line (error %lu)", GetLastError());
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    command = argc >= 2 ? argv[1] : L"";
    argument = argc >= 3 ? argv[2] : NULL;
    /* The watcher shows no window of its own: it skips the theme. */
    if (!Core_EqualsI(command, L"--watch")) Theme_Init();
    if (Core_EqualsI(command, L"--launch") && argument) {
        rc = Launcher_Run(argument);
    } else if (Core_EqualsI(command, L"--url")) {
        rc = Router_Run(argument ? argument : L"");
    } else if (Core_EqualsI(command, L"--install")) {
        rc = Install_Run(!(argument && Core_EqualsI(argument, L"--quiet"))) ? 0 : 1;
    } else if (Core_EqualsI(command, L"--uninstall")) {
        rc = Gui_Run(GUI_UNINSTALL);
    } else if (Core_EqualsI(command, L"--set-up-links")) {
        rc = Gui_Run(GUI_SET_UP_LINKS);
    } else if (Core_EqualsI(command, L"--watch") && argument) {
        /* Each time the profile's Claude closes, the session changes that
         * waited for it are made. */
        rc = Taskbar_WatchRun(argument, argc >= 4 ? (DWORD)wcstoul(argv[3], NULL, 10) : 0, SessionEdit_ApplyPendingFor);
    } else if (command[0] == L'-') {
        /* Not a request for the window: never open it for an unknown switch or one without its folder. */
        BOOL needsFolder = Core_EqualsI(command, L"--launch") || Core_EqualsI(command, L"--watch");
        Util_Log(needsFolder ? L"ignored %s without its folder" : L"ignored unknown option %s", command);
        rc = 1;
    } else {
        rc = Gui_Run(GUI_MANAGER);
    }

    LocalFree(argv);
    if (SUCCEEDED(hr)) CoUninitialize();
    return rc;
}
