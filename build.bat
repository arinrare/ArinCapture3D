@echo off
setlocal enableextensions enabledelayedexpansion

REM Fast-path packaging commands before attempting to run VsDevCmd.
REM On some machines, VsDevCmd can fail with "The input line is too long" due to PATH length.
if /i "%1"=="release" (
    REM Build + stage + zip a non-Debug package.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" -Config Release
    exit /b %errorlevel%
) else if /i "%1"=="relwithdebinfo" (
    REM Build + stage + zip a release-like build that includes PDBs.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" -Config RelWithDebInfo
    exit /b %errorlevel%
)

REM Ensure MSVC + Windows SDK include/lib paths are set when run from a normal shell.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSINSTALL=%%i"
    )
)

if defined VSINSTALL (
    REM Skip if we're already in a VS Developer Command Prompt.
    if defined VSCMD_VER (
        REM already configured
    ) else (
        REM Avoid invoking VsDevCmd when PATH is extremely long (cmd.exe limit issues).
        set "_PATHLEN="
        for /f %%L in ('powershell -NoProfile -Command "$env:PATH.Length"') do set "_PATHLEN=%%L"
        if not defined _PATHLEN set "_PATHLEN=0"
        if !_PATHLEN! GEQ 7000 (
            echo [build.bat] Warning: PATH is very long !_PATHLEN! chars; skipping VsDevCmd to avoid cmd.exe line-length issues.
        ) else (
            call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
        )
    )
) else (
    echo [build.bat] Warning: vswhere/VsDevCmd not found; build may fail if MSVC/Windows SDK env vars are missing.
)

if "%1"=="configure" (
    if exist build rmdir /s /q build
    cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE
) else if /i "%1"=="debug" (
    if exist build\CMakeCache.txt (
        findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config" build\CMakeCache.txt >nul
        if errorlevel 1 (
            rmdir /s /q build
        )
    )
    if not exist build\CMakeCache.txt (
        cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE
    )
    cmake --build build --config Debug
) else (
    if exist build\CMakeCache.txt (
        findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config" build\CMakeCache.txt >nul
        if errorlevel 1 (
            rmdir /s /q build
        )
    )
    if not exist build\CMakeCache.txt (
        cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE
    )
    cmake --build build --config Debug
)

endlocal