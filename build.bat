@echo off
rem Flowtary build script (MSVC Release)
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (
    echo [error] Visual Studio with C++ workload not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

if not exist build mkdir build

rem 版本信息资源（右键属性可见版本号/产品名等）；版本号取自 src\version.h
rc /nologo /fo build\flowtary.res src\flowtary.rc
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\main.cpp build\flowtary.res /Fe:build\flowtary.exe /Fo:build\main.obj ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   tools\evtest.cpp /Fe:build\evtest.exe /Fo:build\evtest.obj ^
   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

echo.
echo [ok] build\flowtary.exe / build\evtest.exe
exit /b 0
