@echo off
rem Builds build\ClaudeDesktopProfilesManager.exe and runs the unit tests.
rem Needs Visual Studio 2022 or later (or its Build Tools) with the "Desktop development with C++" workload.
setlocal EnableExtensions

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "EXE_NAME=ClaudeDesktopProfilesManager.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

rem The exe is linked in obj\ and moved to build\ once every check and test passed: sign.cmd signs
rem build\ClaudeDesktopProfilesManager.exe, so a failed or older build must not stay there.
del /q "%OUT%\%EXE_NAME%" >nul 2>&1
if exist "%OUT%\%EXE_NAME%" (
    echo "%OUT%\%EXE_NAME%" cannot be replaced: close it first.
    exit /b 1
)

if not exist "%VSWHERE%" (
    echo vswhere.exe not found: install Visual Studio or its Build Tools with the C++ workload.
    exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -version "[17.0,)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio 2022 or later with its C++ build tools was not found.
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
rem The tests link every object: one left from a renamed or deleted source must not be among them.
del /q "%OUT%\obj\*.obj" "%OUT%\obj\app.res" "%OUT%\obj\%EXE_NAME%" >nul 2>&1

set "CFLAGS=/nologo /W4 /WX /O2 /MT /GS /guard:cf /sdl /utf-8 /DUNICODE /D_UNICODE"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib advapi32.lib uuid.lib uxtheme.lib dwmapi.lib winhttp.lib oleaut32.lib wintrust.lib crypt32.lib"

pushd "%ROOT%src" || exit /b 1
rc /nologo /fo "%OUT%\obj\app.res" app.rc
if errorlevel 1 (popd & exit /b 1)
popd

cl %CFLAGS% /Fo"%OUT%\obj\\" /Fe"%OUT%\obj\%EXE_NAME%" "%ROOT%src\*.c" "%OUT%\obj\app.res" ^
   /link /SUBSYSTEM:WINDOWS /GUARD:CF /DYNAMICBASE /NXCOMPAT /DEPENDENTLOADFLAG:0x800 /OPT:REF /OPT:ICF %LIBS% || exit /b 1

rem Every interface text has its twelve translations. Needs Python 3.6 or later, which GitHub Actions has;
rem the python.org installer may put only "py" on PATH, and the Microsoft Store's "python" placeholder fails.
rem If blocks, not "a && (b || exit /b 1) || c": there exit /b ends "cmd /c build.cmd" with code 0.
set "PYTHON="
python -c "import sys; sys.exit(sys.version_info < (3, 6))" >nul 2>&1 && set "PYTHON=python"
if not defined PYTHON (py -3 -c "import sys; sys.exit(sys.version_info < (3, 6))" >nul 2>&1 && set "PYTHON=py -3")
if defined PYTHON (
    %PYTHON% "%ROOT%tools\check-localization.py" || exit /b 1
) else (
    echo Python 3.6 or later not found: tools\check-localization.py skipped.
)

rem The pure helpers alone: core.c needs nothing else.
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\test_core.exe" "%ROOT%tests\test_core.c" "%OUT%\obj\core.obj" ^
   /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
"%OUT%\test\test_core.exe" || exit /b 1

rem Complete interface catalogs, format arguments and language selection.
call :Test test_localize "" || exit /b 1
rem Pin entries, records and their commit, on private values and files.
call :Test test_pin "" || exit /b 1
rem Session storage and copies in private temporary fixtures.
call :Test test_sessions "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
rem Update, installation, watcher, routing and removal failures, with private files and controlled callbacks.
call :Test test_platform "" || exit /b 1
rem Shortcut COM failures use private files without launching their targets.
call :Test test_shortcuts "" || exit /b 1
rem What theme.c draws, against Windows' own drawing (with the program's manifest).
call :Test test_theme "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
rem Native resource captions, wrapping and control bounds in every interface language (with the program's manifest).
call :Test test_layout "/MANIFEST:NO" "%OUT%\obj\app.res" || exit /b 1
rem What the program relies on in the installed Claude Desktop (skipped without it).
call :Test test_claude "" || exit /b 1

move /y "%OUT%\obj\%EXE_NAME%" "%OUT%\%EXE_NAME%" >nul || exit /b 1
echo.
echo Built "%OUT%\%EXE_NAME%"
exit /b 0

rem Builds tests\%1.c with the program's objects and runs it. %2: linker options; %3: one more input, quoted.
:Test
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\%~1.exe" "%ROOT%tests\%~1.c" "%OUT%\obj\*.obj" %3 ^
   /link /SUBSYSTEM:CONSOLE %~2 %LIBS% || exit /b 1
"%OUT%\test\%~1.exe" || exit /b 1
exit /b 0
