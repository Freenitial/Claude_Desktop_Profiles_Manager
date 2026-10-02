@echo off
rem Signs build\ClaudeDesktopProfilesManager.exe (made by build.cmd) with the release
rem code signing certificate, and timestamps it so the signature outlives the certificate.
rem The certificate's key stays on its hardware token: plug it in, then give its PIN
rem when the token's software asks.
setlocal EnableExtensions

rem The timestamp server of the certificate's authority.
set "TIMESTAMP=http://ts.harica.gr"

set "ROOT=%~dp0"
set "EXE=%ROOT%build\ClaudeDesktopProfilesManager.exe"
if not exist "%EXE%" (
    echo "%EXE%" not found: run build.cmd first.
    exit /b 1
)

rem An update runs only when its certificate is issued to exactly APP_SIGNER (src\app.h); the
rem signature names the program and its page as src\app.h does.
call :ReadDefine APP_SIGNER SIGNER || exit /b 1
call :ReadDefine APP_NAME PRODUCT || exit /b 1
call :ReadDefine APP_REPO REPOSITORY || exit /b 1

rem signtool.exe comes with the Windows SDK, which the C++ workload installs.
set "SIGNTOOL="
call :FindSignTool "%ProgramFiles(x86)%\Windows Kits\10"
if not defined SIGNTOOL for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 /reg:32 2^>nul') do (
    if /i "%%a"=="REG_SZ" call :FindSignTool "%%b"
)
if not defined SIGNTOOL (
    echo signtool.exe not found: install the Windows SDK ^(Visual Studio's C++ workload has it^).
    exit /b 1
)

"%SIGNTOOL%" sign /a /n "%SIGNER%" /fd sha256 /tr "%TIMESTAMP%" /td sha256 ^
    /d "%PRODUCT%" /du "https://github.com/%REPOSITORY%" "%EXE%" || (
    echo Signing failed: is the token plugged in, and its certificate issued to "%SIGNER%"?
    exit /b 1
)
rem /tw: a signature without its timestamp would stop verifying when the certificate expires.
"%SIGNTOOL%" verify /pa /tw /q "%EXE%" || (
    echo The signature does not verify, or it has no timestamp.
    exit /b 1
)
rem /n accepts any certificate whose subject contains the name. update.c compares the signer's
rem common name (CN) with APP_SIGNER ordinally: so does this. .NET rather than
rem Get-AuthenticodeSignature, whose module fails to load when PowerShell 7 started this.
powershell -NoProfile -Command "$signer = [Security.Cryptography.X509Certificates.X509Certificate2][Security.Cryptography.X509Certificates.X509Certificate]::CreateFromSignedFile($env:EXE); $cn = @($signer.SubjectName.Format($true) -split '\r?\n' | Where-Object { $_ -like 'CN=*' })[0]; $name = if ($cn) { $cn.Substring(3) } else { '' }; if (-not [string]::Equals($name, $env:SIGNER, [StringComparison]::Ordinal)) { Write-Output ('Signed by ' + $name + ', not ' + $env:SIGNER + '.'); exit 1 }" || (
    "%SIGNTOOL%" remove /s /q "%EXE%" && (
        echo The signature was removed: sign with the certificate issued to "%SIGNER%".
    ) || (
        echo The signature could not be removed: delete "%EXE%" and build again.
    )
    exit /b 1
)

echo.
echo Signed "%EXE%"
exit /b 0

rem Sets the variable %2 to the string that src\app.h defines as %1 (#define %1 L"...").
:ReadDefine
set "%~2="
for /f tokens^=2^ delims^=^" %%s in ('findstr /b /c:"#define %~1 " "%ROOT%src\app.h"') do set "%~2=%%s"
if not defined %~2 (
    echo %~1 not found in src\app.h.
    exit /b 1
)
exit /b 0

rem Sets SIGNTOOL to the x64 signtool.exe of the newest Windows 10 SDK under the Windows Kits root %1.
:FindSignTool
set "KITS=%~1"
if "%KITS:~-1%"=="\" set "KITS=%KITS:~0,-1%"
for /f "delims=" %%v in ('dir /b /ad /on "%KITS%\bin\10.*" 2^>nul') do (
    if exist "%KITS%\bin\%%v\x64\signtool.exe" set "SIGNTOOL=%KITS%\bin\%%v\x64\signtool.exe"
)
exit /b 0
