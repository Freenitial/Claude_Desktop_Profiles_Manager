@echo off
rem Builds build\ClaudeDesktopProfilesManager.exe and runs the unit tests.
rem Needs Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload.
setlocal EnableExtensions

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo vswhere.exe not found: install Visual Studio 2022 or its Build Tools with the C++ workload.
    exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo The Visual Studio C++ build tools were not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (
    echo Loading the x64 compiler failed: run "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" to see why.
    exit /b 1
)
where cl.exe >nul 2>&1 && where rc.exe >nul 2>&1 || (
    echo The x64 compiler or the Windows SDK is not set up: run "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" to see why.
    exit /b 1
)

if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\test" mkdir "%OUT%\test"

set "CFLAGS=/nologo /W4 /WX /O2 /MT /GS /guard:cf /sdl /utf-8 /DUNICODE /D_UNICODE"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib advapi32.lib uuid.lib uxtheme.lib dwmapi.lib winhttp.lib oleaut32.lib wintrust.lib crypt32.lib"

pushd "%ROOT%src" || exit /b 1
rc /nologo /fo "%OUT%\obj\app.res" app.rc
if errorlevel 1 (popd & exit /b 1)
popd

cl %CFLAGS% /Fo"%OUT%\obj\\" /Fe"%OUT%\ClaudeDesktopProfilesManager.exe" "%ROOT%src\*.c" "%OUT%\obj\app.res" ^
   /link /SUBSYSTEM:WINDOWS /GUARD:CF /DYNAMICBASE /NXCOMPAT /DEPENDENTLOADFLAG:0x800 /OPT:REF /OPT:ICF %LIBS% || exit /b 1

cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_core.exe" "%ROOT%tests\test_core.c" "%ROOT%src\core.c" ^
   /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
"%OUT%\test\test_core.exe" || exit /b 1

cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_pin.exe" "%ROOT%tests\test_pin.c" "%OUT%\obj\*.obj" ^
   /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
"%OUT%\test\test_pin.exe" || exit /b 1

rem Session storage and copies in private temporary fixtures.
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_sessions.exe" "%ROOT%tests\test_sessions.c" "%OUT%\obj\*.obj" ^
   /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
"%OUT%\test\test_sessions.exe" || exit /b 1

rem What theme.c draws, against Windows' own drawing (its manifest, for common controls 6, is embedded like the program's).
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_theme.exe" "%ROOT%tests\test_theme.c" "%OUT%\obj\*.obj" ^
   /link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED %LIBS% || exit /b 1
"%OUT%\test\test_theme.exe" || exit /b 1

rem What the program relies on in the installed Claude Desktop (skipped without it).
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_claude.exe" "%ROOT%tests\test_claude.c" "%OUT%\obj\*.obj" ^
   /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
"%OUT%\test\test_claude.exe" || exit /b 1

echo.
echo Built "%OUT%\ClaudeDesktopProfilesManager.exe"
