@echo off
@chcp 65001 >nul
rem ============================================================
rem  Flowtary release script (MSVC Release, single-file green)
rem
rem  Usage:
rem    release.bat                        build and output to project\dist\
rem    release.bat "D:\Tools\Flowtary"    output to specified directory
rem
rem  Output: <target_dir>\flowtary-<version>.exe
rem  Version from src\version.h FT_VER_DOT
rem
rem  Note: exe uses /MT static C runtime, no VC++ runtime needed.
rem        Config written to HKCU\Software\Flowtary, no files needed.
rem  Note: file dialog jump module includes filedlg_hook64.dll /
rem        filedlg_hook32.dll / filedlg_agent32.exe, must be in same dir.
rem ============================================================
setlocal

set "ROOT=%~dp0"
set "OUTDIR=%~1"
if "%OUTDIR%"=="" set "OUTDIR=%ROOT%dist"

rem ---- Read version from src\version.h ----
set "VER=0.0.0.0"
for /f "usebackq tokens=3" %%v in (`findstr /c:"#define FT_VER_DOT" "%ROOT%src\version.h"`) do set "VER=%%~v"
echo [info] version before bump = %VER%

rem ---- Auto bump BUILD digit, write back to src\version.h ----
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\version_bump.ps1" "%ROOT%src\version.h"
if errorlevel 1 (
    echo [error] Version auto-bump failed.
    exit /b 1
)

rem ---- Re-read version after bump ----
set "VER=0.0.0.0"
for /f "usebackq tokens=3" %%v in (`findstr /c:"#define FT_VER_DOT" "%ROOT%src\version.h"`) do set "VER=%%~v"
echo [info] version = %VER%

rem ---- Locate Visual Studio (C++ desktop workload) ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (
    echo [error] Visual Studio with C++ desktop workload not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

if not exist "%ROOT%build" mkdir "%ROOT%build"

rem ---- Step 1: Clean up old artifacts ----
echo [1/4] Cleaning old artifacts...
del /q "%ROOT%build\flowtary_old.exe"    2>nul
del /q "%ROOT%build\flowtary_fixed.exe"  2>nul
del /q "%ROOT%build\flowtary_test.exe"   2>nul
del /q "%ROOT%build\*.manifest"          2>nul
del /q "%ROOT%build\*.obj"               2>nul
del /q "%ROOT%build\*.log"               2>nul
del /q "%ROOT%build\*.png"               2>nul

rem ---- Step 2: Compile version info resource ----
echo [2/4] Compiling version info resource...
rc /nologo /fo "%ROOT%build\flowtary.res" "%ROOT%src\flowtary.rc"
if errorlevel 1 (
    echo [error] Resource compilation failed.
    exit /b 1
)

rem ---- Step 3: Compile main program + file dialog jump module (/MT static) ----
echo [3/4] Compiling main program...
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   "%ROOT%src\main.cpp" /Fo:"%ROOT%build\main.obj"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   "%ROOT%src\filedlg_jump.cpp" /Fo:"%ROOT%build\filedlg_jump.obj"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%build\main.obj" "%ROOT%build\filedlg_jump.obj" "%ROOT%build\flowtary.res" ^
   /Fe:"%ROOT%build\flowtary.exe" ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem ---- Step 3b: Compile hook DLLs (32/64) and 32-bit agent ----
echo [3b] Compiling hook DLLs and 32-bit agent...
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\hookdlg.cpp" /Fe:"%ROOT%build\filedlg_hook64.dll" /Fo:"%ROOT%build\hookdlg64.obj" ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\hookdlg.cpp" /Fe:"%ROOT%build\filedlg_hook32.dll" /Fo:"%ROOT%build\hookdlg32.obj" ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\agent32.cpp" /Fe:"%ROOT%build\filedlg_agent32.exe" /Fo:"%ROOT%build\agent32.obj" ^
   /link /SUBSYSTEM:WINDOWS user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem ---- Step 4: Output to target directory ----
echo [4/4] Output to %OUTDIR% ...
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
copy /y "%ROOT%build\flowtary.exe" "%OUTDIR%\flowtary-%VER%.exe" >nul
if errorlevel 1 (
    echo [error] Copy main program failed.
    exit /b 1
)
copy /y "%ROOT%build\filedlg_hook64.dll" "%OUTDIR%\filedlg_hook64.dll" >nul
if errorlevel 1 (
    echo [error] Copy filedlg_hook64.dll failed.
    exit /b 1
)
copy /y "%ROOT%build\filedlg_hook32.dll" "%OUTDIR%\filedlg_hook32.dll" >nul
if errorlevel 1 (
    echo [error] Copy filedlg_hook32.dll failed.
    exit /b 1
)
copy /y "%ROOT%build\filedlg_agent32.exe" "%OUTDIR%\filedlg_agent32.exe" >nul
if errorlevel 1 (
    echo [error] Copy filedlg_agent32.exe failed.
    exit /b 1
)

echo.
echo [ok] %OUTDIR%\flowtary-%VER%.exe
echo.
echo Notes:
echo   1) Put the exe in a fixed location (overwrite old version).
echo   2) Right-click tray icon > Settings > Behavior > "Auto-open launcher".
echo   3) Old version runs in background until new version prompts takeover.
exit /b 0
