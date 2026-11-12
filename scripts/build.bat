@echo off
rem Build the game on Windows from any shell (cmd, PowerShell, the VS Code
rem terminal): runs scripts/build.sh in MSYS2's CLANG64 environment, so no
rem MSYS2 shell has to be opened. Whatever the build needs and this machine
rem lacks is installed first: MSYS2 itself (through winget), then the CLANG64
rem packages. See docs/WINDOWS.md.
rem
rem Usage: scripts\build.bat [--regen] [--release]
rem   --regen    lift default.xbe again from scratch
rem   --release  configure a Release build (only when build\ is new)
rem
rem MSYS2 is looked for in %MSYS2_ROOT%, else C:\msys64 (where winget puts it).
setlocal EnableExtensions

rem This script's folder and the repository, before the argument loop below:
rem shift moves %0 as well, and %~dp0 with it.
set "HERE=%~dp0"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

rem The packages the build uses. SDL2 is installed as sdl2-compat (SDL2's API
rem over SDL3), which MSYS2 marks as replacing the SDL2 package; the check asks
rem for SDL2 by the name both provide, so either one counts. gdb is for
rem debugging, installed along with the rest but not required.
set "P=mingw-w64-clang-x86_64-"
set "INSTALL=git %P%clang %P%cmake %P%ninja %P%pkgconf %P%sdl2-compat %P%libepoxy %P%python %P%python-capstone %P%gdb"
set "NEED=git %P%clang %P%cmake %P%ninja %P%pkgconf %P%SDL2 %P%libepoxy %P%python %P%python-capstone"

set "ARGS="
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage
set "ARGS=%ARGS% %1"
shift
goto parse

:usage
echo Usage: scripts\build.bat [--regen] [--release]
echo   --regen    lift default.xbe again from scratch
echo   --release  configure a Release build (only when build\ is new)
echo The first run installs what is missing: MSYS2 (with winget) and its build
echo packages. MSYS2 is looked for in %%MSYS2_ROOT%%, else C:\msys64.
exit /b 0

:parsed
if defined MSYS2_ROOT (set "MSYS2=%MSYS2_ROOT%") else set "MSYS2=C:\msys64"
set "BASH=%MSYS2%\usr\bin\bash.exe"
set "PACMAN=%MSYS2%\usr\bin\pacman.exe"
rem The CLANG64 environment; its login profile puts /clang64/bin first and
rem leaves the rest of the Windows PATH out, so no other cmake or python on
rem this machine is picked up.
set "MSYSTEM=CLANG64"
set "CHERE_INVOKING=1"

rem -- MSYS2 ----------------------------------------------------------------
if exist "%BASH%" goto check_packages
if defined MSYS2_ROOT goto no_msys2_at_root
where winget >nul 2>&1
if errorlevel 1 goto no_winget
echo ==^> Installing MSYS2 with winget (first run only)
winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements
if exist "%BASH%" goto check_packages
echo error: MSYS2 is still not in %MSYS2% after winget.
exit /b 1

:no_msys2_at_root
echo error: MSYS2_ROOT is set to %MSYS2_ROOT%, but there is no MSYS2 there.
exit /b 1

:no_winget
echo error: MSYS2 is not installed, and winget is not here to install it.
echo   Install MSYS2 from https://www.msys2.org/ into C:\msys64, or set
echo   MSYS2_ROOT to where it is, then run this again.
exit /b 1

rem -- The CLANG64 packages -------------------------------------------------
:check_packages
"%PACMAN%" -T %NEED% >nul 2>&1
if not errorlevel 1 goto build
echo ==^> Installing the build tools into MSYS2 (first run only; a few minutes)
rem Brought up to date first, as MSYS2 asks before installing anything. Twice:
rem when pacman or the MSYS2 runtime itself is updated, the first run stops
rem after that and the second updates the rest.
"%BASH%" -lc "pacman -Syu --noconfirm"
"%BASH%" -lc "pacman -Syu --noconfirm"
"%BASH%" -lc "pacman -S --needed --noconfirm %INSTALL%"
"%PACMAN%" -T %NEED% >nul 2>&1
if not errorlevel 1 goto build
echo error: these build packages could not be installed:
"%PACMAN%" -T %NEED%
exit /b 1

rem -- The build ------------------------------------------------------------
:build
rem build.sh finds the repository from its own path; forward slashes, which
rem MSYS2's bash takes as a Windows path and dirname can split.
set "SCRIPT=%HERE%build.sh"
set "SCRIPT=%SCRIPT:\=/%"
"%BASH%" -l "%SCRIPT%" %ARGS%
if errorlevel 1 goto fail
echo.
echo Done. The game: %ROOT%\build\bin\thug.exe
exit /b 0

:fail
echo.
echo error: the build failed (see above).
exit /b 1
