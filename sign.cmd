@echo off
rem Signs build\ClaudeDesktopProfilesManager.exe (made by build.cmd) with the release
rem code signing certificate, and timestamps it so the signature outlives the certificate.
rem The certificate's key stays on its hardware token: plug it in, then give its PIN
rem when the token's software asks.
setlocal EnableExtensions

rem Who the release certificate is issued to, and the timestamp server of its authority.
set "SUBJECT=Leo Antonin Gillet"
set "TIMESTAMP=http://ts.harica.gr"

set "EXE=%~dp0build\ClaudeDesktopProfilesManager.exe"
if not exist "%EXE%" (
    echo "%EXE%" not found: run build.cmd first.
    exit /b 1
)

rem signtool.exe comes with the Windows SDK, which the C++ workload installs.
set "SIGNTOOL="
for /f "delims=" %%v in ('dir /b /ad /on "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
    if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\signtool.exe" set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\signtool.exe"
)
if not defined SIGNTOOL (
    echo signtool.exe not found: install the Windows SDK ^(Visual Studio's C++ workload has it^).
    exit /b 1
)

"%SIGNTOOL%" sign /a /n "%SUBJECT%" /fd sha256 /tr "%TIMESTAMP%" /td sha256 ^
    /d "Claude Desktop Profiles Manager" /du "https://github.com/Freenitial/Claude_Desktop_Profiles_Manager" "%EXE%" || (
    echo Signing failed: is the token plugged in, and its certificate issued to "%SUBJECT%"?
    exit /b 1
)
"%SIGNTOOL%" verify /pa /q "%EXE%" || (
    echo The signature does not verify.
    exit /b 1
)

echo.
echo Signed "%EXE%"
