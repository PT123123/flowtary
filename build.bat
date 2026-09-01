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

rc /nologo /fo build\flowtary.res src\flowtary.rc
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   src\main.cpp /Fo:build\main.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   src\filedlg_jump.cpp /Fo:build\filedlg_jump.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   build\main.obj build\filedlg_jump.obj build\flowtary.res /Fe:build\flowtary.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   tools\evtest.cpp /Fe:build\evtest.exe /Fo:build\evtest.obj ^
   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem 64-bit hook DLL
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\hookdlg.cpp /Fe:build\filedlg_hook64.dll /Fo:build\hookdlg64.obj ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem 32-bit hook DLL + agent
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\hookdlg.cpp /Fe:build\filedlg_hook32.dll /Fo:build\hookdlg32.obj ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\agent32.cpp /Fe:build\filedlg_agent32.exe /Fo:build\agent32.obj ^
   /link /SUBSYSTEM:WINDOWS user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

echo.
echo [ok] build\flowtary.exe / build\evtest.exe
echo [ok] build\filedlg_hook64.dll / build\filedlg_hook32.dll / build\filedlg_agent32.exe
exit /b 0
