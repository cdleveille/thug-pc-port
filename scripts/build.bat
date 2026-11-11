@echo off
rem Build the game on Windows from any shell (cmd, PowerShell, the VS Code
rem terminal): runs scripts/build.sh in MSYS2's CLANG64 environment, so no
rem MSYS2 shell has to be opened first. See docs/WINDOWS.md.
rem
rem Usage: scripts\build.bat [--setup] [--regen] [--release]
rem   --setup    first install or update the toolchain: MSYS2 itself through
rem              winget if it is missing, then pacman -Syu and the CLANG64
rem              packages the build needs
rem   --regen    lift default.xbe again from scratch
rem   --release  configure a Release build (only when build\ is new)
rem
rem MSYS2 is looked for in %MSYS2_ROOT%, else C:\msys64.
setlocal EnableExtensions

rem This script's folder and the repository, before the argument loop below:
rem shift moves %0 as well, and %~dp0 with it.
set "HERE=%~dp0"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

rem SDL2 comes as sdl2-compat (SDL2's API over SDL3): MSYS2 marks it as
rem replacing the SDL2 package, so pacman -Syu would swap one for the other.
set "PACKAGES=git mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-sdl2-compat mingw-w64-clang-x86_64-libepoxy mingw-w64-clang-x86_64-python mingw-w64-clang-x86_64-python-capstone mingw-w64-clang-x86_64-gdb"

set "SETUP=0"
set "ARGS="
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--setup" (
    set "SETUP=1"
) else if /i "%~1"=="-h" (
    goto usage
) else if /i "%~1"=="--help" (
    goto usage
) else (
    set "ARGS=%ARGS% %1"
)
shift
goto parse

:usage
echo Usage: scripts\build.bat [--setup] [--regen] [--release]
echo   --setup    install or update the toolchain first: MSYS2 through winget
echo              if it is missing, then the CLANG64 packages the build needs
echo   --regen    lift default.xbe again from scratch
echo   --release  configure a Release build (only when build\ is new)
echo MSYS2 is looked for in %%MSYS2_ROOT%%, else C:\msys64.
exit /b 0

:parsed
if defined MSYS2_ROOT (set "MSYS2=%MSYS2_ROOT%") else set "MSYS2=C:\msys64"
set "BASH=%MSYS2%\usr\bin\bash.exe"
rem The CLANG64 environment; its login profile puts /clang64/bin first and
rem leaves the rest of the Windows PATH out, so no other cmake or python on
rem this machine is picked up.
set "MSYSTEM=CLANG64"
set "CHERE_INVOKING=1"

if exist "%BASH%" goto have_msys2
if "%SETUP%"=="1" if not defined MSYS2_ROOT goto install_msys2
echo error: MSYS2 was not found in %MSYS2%.
echo   Run scripts\build.bat --setup to install it with winget, or set MSYS2_ROOT
echo   to the folder it is installed in.
exit /b 1

:install_msys2
echo ==^> Installing MSYS2 with winget
winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements
if exist "%BASH%" goto have_msys2
echo error: MSYS2 is still not in %MSYS2% after winget.
exit /b 1

:have_msys2
if "%SETUP%"=="0" goto check_tools
rem Twice: when pacman or the MSYS2 runtime itself is updated, the first run
rem stops after that, and the second updates the rest.
echo ==^> Updating MSYS2 (pacman -Syu)
"%BASH%" -lc "pacman -Syu --noconfirm"
"%BASH%" -lc "pacman -Syu --noconfirm"
if errorlevel 1 goto fail
echo ==^> Installing the CLANG64 build packages
"%BASH%" -lc "pacman -S --needed --noconfirm %PACKAGES%"
if errorlevel 1 goto fail

:check_tools
for %%T in (clang.exe cmake.exe ninja.exe pkgconf.exe python3.exe) do (
    if not exist "%MSYS2%\clang64\bin\%%T" (
        echo error: %MSYS2%\clang64\bin\%%T is missing: the CLANG64 build packages
        echo   are not installed. Run scripts\build.bat --setup to install them.
        exit /b 1
    )
)

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
