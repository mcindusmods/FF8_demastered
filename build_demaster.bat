@echo off
setlocal EnableExtensions
title Build ff8_demaster.dll
cd /d "%~dp0"

echo.
echo  Building ff8_demaster.dll
echo  ==========================
echo.

rem --- 1. Find Visual Studio / Build Tools 2022 (or 2026) with the C++ compiler ---------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto no_vs
set "VSVER="
for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VSVER=%%v"
rem The project is set up for Visual Studio 2022 (toolset v143). Visual Studio 2026 (version 18) builds it with its own v145.
set "TOOLSET="
if not "%VSVER:~0,3%"=="17." set "TOOLSET=/p:PlatformToolset=v145"
echo  Compiler: %VSPATH% (version %VSVER%) %TOOLSET%
rem VsDevCmd would otherwise switch to %USERPROFILE%\source
set "VSCMD_START_DIR=%~dp0"
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=amd64 >nul
cd /d "%~dp0"
where msbuild >nul 2>nul
if errorlevel 1 goto no_vs

rem --- 2. Git (vcpkg needs it to download the two libraries the project uses) ----------------------------
where git >nul 2>nul
if errorlevel 1 goto no_git

rem --- 3. vcpkg, kept next to this file (downloaded and set up the first time) -----------------------------
if exist "%~dp0vcpkg\vcpkg.exe" goto have_vcpkg
if not exist "%~dp0vcpkg\bootstrap-vcpkg.bat" (
    echo  Downloading vcpkg, the first time only...
    git clone https://github.com/microsoft/vcpkg.git "%~dp0vcpkg"
    if errorlevel 1 goto failed
)
call "%~dp0vcpkg\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 goto failed
:have_vcpkg
"%~dp0vcpkg\vcpkg.exe" integrate install
if errorlevel 1 goto failed

rem --- 4. Build -------------------------------------------------------------------------------------------
echo.
echo  Compiling. The first time takes several minutes (vcpkg builds glfw3 and xxhash first).
echo.
msbuild ff8_demaster.sln /m /nologo /verbosity:minimal /p:Configuration=Debug /p:Platform=x86 /p:OutDir=..\.build\Debug\ %TOOLSET% "/flp:logfile=%~dp0build_log.txt;verbosity=normal"
if errorlevel 1 goto failed
if not exist "%~dp0.build\Debug\ff8_demaster.dll" goto failed
if not exist "%~dp0.build\Debug\fieldprep.exe" goto failed

rem --- 5. The Release build of the DLL: the same code built for speed (COMPILING.md, "Debug and Release") ---
rem     If it fails, the Debug DLL above is still good.
echo.
echo  Compiling the Release build of the DLL...
echo.
set "RELEASE=ok"
if exist "%~dp0.build\Release\ff8_demaster.dll" del "%~dp0.build\Release\ff8_demaster.dll"
msbuild ff8_demaster\ff8_demaster.vcxproj /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=Win32 %TOOLSET% "/flp:logfile=%~dp0build_log_release.txt;verbosity=normal"
if errorlevel 1 set "RELEASE=failed"
if not exist "%~dp0.build\Release\ff8_demaster.dll" set "RELEASE=failed"

echo.
echo  ==========================================================================
echo   BUILD SUCCEEDED
echo   %~dp0.build\Debug\ff8_demaster.dll
echo   Copy it into your FINAL FANTASY VIII Remastered folder (see COMPILING.md)
echo.
echo   %~dp0.build\Debug\fieldprep.exe
echo   The PBR preparation tool (not part of the game). See README_field_pbr.md
echo.
if not "%RELEASE%"=="ok" goto release_failed
echo   %~dp0.build\Release\ff8_demaster.dll
echo   The same DLL built for speed. Use it in place of the Debug one once it has
echo   shown it runs the same; keep the Debug one to go back to.
goto built
:release_failed
echo   The Release build of the DLL FAILED. The Debug DLL above is not affected.
echo   Its log is in %~dp0build_log_release.txt
echo   Search it for "error" and send those lines, or the whole file.
:built
echo  ==========================================================================
start "" explorer "%~dp0.build"
pause
exit /b 0

:no_vs
echo.
echo  Build Tools for Visual Studio 2022 (or Visual Studio 2022/2026) with "Desktop development with C++"
echo  was not found. Install it first (COMPILING.md, step 2), then run this file again.
pause
exit /b 1

:no_git
echo.
echo  Git was not found. Install Git for Windows (COMPILING.md, step 1), then run this file again.
pause
exit /b 1

:failed
echo.
echo  ==========================================================================
echo   BUILD FAILED
echo   The full log is in %~dp0build_log.txt
echo   Search it for "error" and send those lines (or the whole file).
echo  ==========================================================================
pause
exit /b 1
