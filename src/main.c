#include "app.h"
#include <objbase.h>
#include <shellapi.h>

static BOOL Is(const WCHAR *arg, const WCHAR *name)
{
    return CompareStringOrdinal(arg, -1, name, -1, TRUE) == CSTR_EQUAL;
}

/* Kernel32 only: decided before any other library is loaded. */
static BOOL RunningFromInstallFolder(void)
{
    WCHAR self[MAX_PATH], local[MAX_PATH], installed[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, ARRAYSIZE(self)), m = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    return n > 0 && n < ARRAYSIZE(self) && m > 0 && m < ARRAYSIZE(local) &&
           SUCCEEDED(StringCchPrintfW(installed, ARRAYSIZE(installed), L"%s\\Programs\\" APP_NAME L"\\" APP_EXE, local)) &&
           CompareStringOrdinal(self, -1, installed, -1, TRUE) == CSTR_EQUAL;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR cmdLine, int show)
{
    LPWSTR *argv;
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
        WCHAR sys[MAX_PATH];
        if (GetSystemDirectoryW(sys, ARRAYSIZE(sys))) SetCurrentDirectoryW(sys);
    }
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Theme_Init();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && Is(argv[1], L"--launch"))
        rc = argc >= 3 ? Launcher_Run(argv[2]) : 1;
    else if (argv && argc >= 2 && Is(argv[1], L"--url"))
        rc = Router_Run(argc >= 3 ? argv[2] : L"");
    else if (argv && argc >= 2 && Is(argv[1], L"--install"))
        rc = Install_Run(!(argc >= 3 && Is(argv[2], L"--quiet"))) ? 0 : 1;
    else if (argv && argc >= 2 && Is(argv[1], L"--uninstall"))
        rc = Gui_Run(GUI_UNINSTALL);
    else if (argv && argc >= 2 && Is(argv[1], L"--set-up-links"))
        rc = Gui_Run(GUI_SET_UP_LINKS);
    else if (argv && argc >= 3 && Is(argv[1], L"--watch")) {
        rc = Taskbar_WatchRun(argv[2]);
        /* The profile's Claude has closed: the session changes that waited
         * for it can be made now (it runs again when it was only handed over). */
        SessionEdit_ApplyPendingFor(argv[2]);
    }
    else if (argv && argc >= 2 && argv[1][0] == L'-') {
        /* Not a request for the window: never open it for an unknown switch. */
        Util_Log(L"ignored unknown option %s", argv[1]);
        rc = 1;
    } else
        rc = Gui_Run(GUI_MANAGER);

    if (argv) LocalFree(argv);
    if (SUCCEEDED(hr)) CoUninitialize();
    return rc;
}
