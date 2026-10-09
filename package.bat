@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM Build RD Desktop Software and wrap it in a Windows installer (.exe).
REM
REM   package.bat            build (via build.bat) + create dist\RD_Desktop_Software-<ver>-setup.exe
REM   package.bat 1.2.0      same, after writing 1.2.0 into version.txt (the single version source)
REM   package.bat nobuild    skip the build, package the existing build\RD_Desktop_Software.exe
REM
REM Afterwards dist\ contains only the new installer plus update.json (version, URL, size,
REM SHA-256) - the file the app downloads from GitHub to find out whether an update exists.
REM Older installers in dist\ are deleted.
REM
REM Optional code signing (signtool.exe from the Windows SDK), applied to the exe, the
REM installer and its uninstaller when these variables are set before running the script:
REM   RD_SIGN_PFX        path of the .pfx certificate      RD_SIGN_PASSWORD   its password
REM   RD_SIGN_TIMESTAMP  RFC 3161 server (default http://timestamp.digicert.com)
REM
REM Needs Inno Setup 6. If it is missing the script offers to install it with
REM     winget install JRSoftware.InnoSetup
REM ---------------------------------------------------------------------------
cd /d "%~dp0"

set "REPO_RAW=https://raw.githubusercontent.com/Jeremiahstauffur/RD-Desktop-Software/master"

REM ---- Version --------------------------------------------------------------
set "VERSION="
set "DO_BUILD=1"
for %%A in (%*) do (
    if /i "%%A"=="nobuild" (set "DO_BUILD=0") else set "VERSION=%%A"
)
if defined VERSION (
    REM Keep version.txt in sync so the exe reports the same version as the installer.
    <nul set /p "=%VERSION%" > version.txt
    echo.>> version.txt
) else (
    if not exist version.txt (
        echo [package] version.txt not found.
        goto :fail
    )
    set /p VERSION=<version.txt
)
set "VERSION=%VERSION: =%"
if not defined VERSION (
    echo [package] version.txt is empty. Put a version such as 1.2.0 on its first line.
    goto :fail
)

REM ---- Inno Setup compiler --------------------------------------------------
set "ISCC="
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC for /f "delims=" %%I in ('where ISCC 2^>nul') do if not defined ISCC set "ISCC=%%I"
if not defined ISCC (
    echo [package] Inno Setup 6 was not found.
    where winget >nul 2>nul
    if errorlevel 1 (
        echo [package] Install it from https://jrsoftware.org/isdl.php and run package.bat again.
        goto :fail
    )
    echo [package] Installing it with winget ^(user-level, no admin rights needed^)...
    winget install --id JRSoftware.InnoSetup -e --silent --accept-package-agreements --accept-source-agreements
    if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
    if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    if not defined ISCC (
        echo [package] Inno Setup installation failed. Install it from https://jrsoftware.org/isdl.php and retry.
        goto :fail
    )
)

REM ---- Build the app --------------------------------------------------------
if "%DO_BUILD%"=="1" (
    call build.bat release
    if errorlevel 1 goto :fail
)
set "EXE=build\RD_Desktop_Software.exe"
if not exist "%EXE%" if exist "build\Release\RD_Desktop_Software.exe" set "EXE=build\Release\RD_Desktop_Software.exe"
if not exist "%EXE%" (
    echo [package] %EXE% not found. Run build.bat first or drop the nobuild argument.
    goto :fail
)

REM ---- Optional code signing ------------------------------------------------
set "SIGN_ARGS="
if defined RD_SIGN_PFX (
    if not defined RD_SIGN_TIMESTAMP set "RD_SIGN_TIMESTAMP=http://timestamp.digicert.com"
    set "SIGNTOOL="
    for /f "delims=" %%I in ('where signtool 2^>nul') do if not defined SIGNTOOL set "SIGNTOOL=%%I"
    if not defined SIGNTOOL for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do (
        if exist "%%D\x64\signtool.exe" set "SIGNTOOL=%%D\x64\signtool.exe"
    )
    if not defined SIGNTOOL (
        echo [package] RD_SIGN_PFX is set but signtool.exe was not found. Install the Windows SDK or add signtool to PATH.
        goto :fail
    )
    echo [package] Signing %EXE% with !RD_SIGN_PFX!
    "!SIGNTOOL!" sign /f "!RD_SIGN_PFX!" /p "!RD_SIGN_PASSWORD!" /fd sha256 /tr "!RD_SIGN_TIMESTAMP!" /td sha256 "%EXE%"
    if errorlevel 1 (
        echo [package] Signing failed.
        goto :fail
    )
    REM Inno Setup signs setup.exe and the uninstaller with the same command ($f = file to sign).
    set "SIGN_ARGS=/DSignToolName=rdsign "/Srdsign=$q!SIGNTOOL!$q sign /f $q!RD_SIGN_PFX!$q /p $q!RD_SIGN_PASSWORD!$q /fd sha256 /tr $q!RD_SIGN_TIMESTAMP!$q /td sha256 $f""
) else (
    echo [package] Not code-signed ^(set RD_SIGN_PFX / RD_SIGN_PASSWORD to sign^).
)

REM ---- Compile the installer ------------------------------------------------
if not exist dist mkdir dist
echo.
echo [package] Inno Setup : %ISCC%
echo [package] Version    : %VERSION%
echo.
set "SETUP=dist\RD_Desktop_Software-%VERSION%-setup.exe"
if exist "%SETUP%" del /q "%SETUP%"
"%ISCC%" /Q "/DAppVersion=%VERSION%" "/DSourceExe=%CD%\%EXE%" "/DOutputDir=%CD%\dist" %SIGN_ARGS% installer\RD_Desktop_Software.iss
if errorlevel 1 (
    echo.
    echo [package] Installer compilation failed.
    goto :fail
)
if not exist "%SETUP%" (
    echo [package] %SETUP% was not produced.
    goto :fail
)

REM ---- Publish: one installer in dist\ + update.json for the in-app update check -----
for %%F in (dist\RD_Desktop_Software-*-setup.exe) do (
    if /i not "%%~nxF"=="RD_Desktop_Software-%VERSION%-setup.exe" (
        echo [package] Removing old installer %%~nxF
        del /q "%%F"
    )
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$f = Get-Item '%SETUP%';" ^
    "$h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower();" ^
    "$o = [ordered]@{ version = '%VERSION%'; installer = '%REPO_RAW%/dist/' + $f.Name; sha256 = $h; size = $f.Length; date = (Get-Date -Format 'yyyy-MM-dd') };" ^
    "[IO.File]::WriteAllText('%CD%\dist\update.json', (($o | ConvertTo-Json) -replace \"`r`n\", \"`n\") + \"`n\");" ^
    "Write-Host ('[package] SHA-256    : ' + $h)"
if errorlevel 1 (
    echo [package] Could not write dist\update.json.
    goto :fail
)

echo.
echo [package] Success: %CD%\%SETUP%
echo [package] Manifest: %CD%\dist\update.json  ^(commit both so installed apps see the update^)
if "%~1"=="" pause
exit /b 0

:fail
echo.
pause
exit /b 1
