@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM One-click build for RD Desktop Software.
REM
REM   build.bat            Release build into .\build  (double-click works)
REM   build.bat debug      Debug build
REM   build.bat run        Release build, then start the exe
REM   build.bat clean      Delete .\build and rebuild from scratch
REM
REM Toolchain lookup order: CLion bundled CMake/Ninja/MinGW -> tools on PATH ->
REM Visual Studio (via vswhere + VsDevCmd). No arguments or setup required.
REM ---------------------------------------------------------------------------
cd /d "%~dp0"

set "CONFIG=Release"
set "RUN_AFTER=0"
set "BUILD_DIR=build"
for %%A in (%*) do (
    if /i "%%A"=="debug" set "CONFIG=Debug"
    if /i "%%A"=="run"   set "RUN_AFTER=1"
    if /i "%%A"=="clean" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

REM ---- CMake ----------------------------------------------------------------
set "CMAKE="
set "CLION_BIN=%LOCALAPPDATA%\Programs\CLion\bin"
if exist "%CLION_BIN%\cmake\win\x64\bin\cmake.exe" set "CMAKE=%CLION_BIN%\cmake\win\x64\bin\cmake.exe"
if not defined CMAKE for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
if not defined CMAKE for /d %%D in ("%ProgramFiles%\JetBrains\CLion*") do (
    if exist "%%D\bin\cmake\win\x64\bin\cmake.exe" set "CMAKE=%%D\bin\cmake\win\x64\bin\cmake.exe" & set "CLION_BIN=%%D\bin"
)
if not defined CMAKE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE (
    echo [build] CMake not found. Install CLion or CMake ^(https://cmake.org^) and retry.
    goto :fail
)

REM ---- Ninja ----------------------------------------------------------------
set "NINJA="
if exist "%CLION_BIN%\ninja\win\x64\ninja.exe" set "NINJA=%CLION_BIN%\ninja\win\x64\ninja.exe"
if not defined NINJA for /f "delims=" %%I in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%I"

REM ---- Compiler -------------------------------------------------------------
set "COMPILER="
set "TOOLCHAIN_NAME="
if exist "%CLION_BIN%\mingw\bin\g++.exe" (
    set "PATH=%CLION_BIN%\mingw\bin;%PATH%"
    set "COMPILER=g++"
    set "TOOLCHAIN_NAME=CLion bundled MinGW"
) else (
    where g++ >nul 2>nul
    if not errorlevel 1 (
        set "COMPILER=g++"
        set "TOOLCHAIN_NAME=MinGW g++ on PATH"
    )
)
if not defined TOOLCHAIN_NAME (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
    )
    if defined VSDIR if exist "!VSDIR!\Common7\Tools\VsDevCmd.bat" (
        call "!VSDIR!\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
        set "COMPILER=cl"
        set "TOOLCHAIN_NAME=Visual Studio (!VSDIR!)"
    )
)
if not defined TOOLCHAIN_NAME (
    echo [build] No C++ compiler found. Install CLion ^(bundled MinGW^) or Visual Studio with C++ tools.
    goto :fail
)

set "GEN_ARGS=-G Ninja"
if defined NINJA set "GEN_ARGS=-G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA%"
if not defined NINJA (
    where ninja >nul 2>nul
    if errorlevel 1 (
        echo [build] Ninja not found; falling back to the default CMake generator.
        set "GEN_ARGS="
    )
)

echo [build] CMake     : %CMAKE%
echo [build] Toolchain : %TOOLCHAIN_NAME%
echo [build] Config    : %CONFIG%
echo.

REM A build dir configured with a different compiler must be regenerated.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /c:"CMAKE_CXX_COMPILER:" "%BUILD_DIR%\CMakeCache.txt" | findstr /i /c:"%COMPILER%" >nul
    if errorlevel 1 (
        echo [build] Existing build dir uses another compiler; reconfiguring from scratch.
        rmdir /s /q "%BUILD_DIR%"
    )
)

"%CMAKE%" -S . -B "%BUILD_DIR%" %GEN_ARGS% -DCMAKE_CXX_COMPILER=%COMPILER% -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (
    echo.
    echo [build] Configure failed. The first configure needs internet access to download ImGui and json.
    goto :fail
)

"%CMAKE%" --build "%BUILD_DIR%" --config %CONFIG% --parallel
if errorlevel 1 (
    echo.
    echo [build] Build failed. If the message mentions LNK1168 / "cannot open ... for writing", close RD_Desktop_Software.exe and retry.
    goto :fail
)

set "EXE=%BUILD_DIR%\RD_Desktop_Software.exe"
if not exist "%EXE%" if exist "%BUILD_DIR%\%CONFIG%\RD_Desktop_Software.exe" set "EXE=%BUILD_DIR%\%CONFIG%\RD_Desktop_Software.exe"
echo.
echo [build] Success: %CD%\%EXE%
if "%RUN_AFTER%"=="1" start "" "%EXE%"
if "%RUN_AFTER%"=="0" if "%~1"=="" pause
exit /b 0

:fail
echo.
pause
exit /b 1
