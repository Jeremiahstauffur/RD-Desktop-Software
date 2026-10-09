@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM Build RD Desktop Software and wrap it in a Windows installer (.exe).
REM
REM   package.bat            build (via build.bat) + create dist\RD_Desktop_Software-<ver>-setup.exe
REM   package.bat 1.2.0      same, with an explicit version number
REM   package.bat nobuild    skip the build, package the existing build\RD_Desktop_Software.exe
REM
REM Needs Inno Setup 6. If it is missing the script offers to install it with
REM     winget install JRSoftware.InnoSetup
REM ---------------------------------------------------------------------------
cd /d "%~dp0"

set "VERSION=1.1.0"
set "DO_BUILD=1"
for %%A in (%*) do (
    if /i "%%A"=="nobuild" (set "DO_BUILD=0") else set "VERSION=%%A"
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

REM ---- Compile the installer ------------------------------------------------
if not exist dist mkdir dist
echo.
echo [package] Inno Setup : %ISCC%
echo [package] Version    : %VERSION%
echo.
"%ISCC%" /Q "/DAppVersion=%VERSION%" "/DSourceExe=%CD%\%EXE%" "/DOutputDir=%CD%\dist" installer\RD_Desktop_Software.iss
if errorlevel 1 (
    echo.
    echo [package] Installer compilation failed.
    goto :fail
)

echo.
echo [package] Success: %CD%\dist\RD_Desktop_Software-%VERSION%-setup.exe
if "%~1"=="" pause
exit /b 0

:fail
echo.
pause
exit /b 1
