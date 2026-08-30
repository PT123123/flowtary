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

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   src\main.cpp /Fo:build\main.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   src\filedlg_jump.cpp /Fo:build\filedlg_jump.obj
if errorlevel 1 exit /b 1
rem 主程序先链到临时名，再拷回 flowtary.exe；避免开发期有实例占用 build\flowtary.exe
rem 时被链接器以 LNK1104 拒斥（占用时仅跳过拷贝，不影响本次产物生成）。
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   build\main.obj build\filedlg_jump.obj build\flowtary.res /Fe:build\flowtary_new.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
copy /y build\flowtary_new.exe build\flowtary.exe >nul 2>nul
if errorlevel 1 echo [warn] build\flowtary.exe 被占用，未覆盖（旧实例仍在运行）；新产物在 build\flowtary_new.exe

cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   tools\evtest.cpp /Fe:build\evtest.exe /Fo:build\evtest.obj ^
   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem 64 位注入 DLL（由 64 位宿主直接安装 WH_CBT 钩子，在对话框所属进程内完成跳转）
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\hookdlg.cpp /Fe:build\filedlg_hook64_new.dll /Fo:build\hookdlg64.obj ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
copy /y build\filedlg_hook64_new.dll build\filedlg_hook64.dll >nul 2>nul
if errorlevel 1 (
  echo [warn] filedlg_hook64.dll 被占用，未覆盖；新产物在 build\filedlg_hook64_new.dll
  echo        占用进程（关掉它们即可释放，再跑一次 build）：
  tasklist /m filedlg_hook64.dll 2>nul
)

rem 32 位注入 DLL + 32 位钩子安装助手（64 位宿主无法加载 32 位 DLL，故由 agent32.exe 安装）
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\hookdlg.cpp /Fe:build\filedlg_hook32_new.dll /Fo:build\hookdlg32.obj ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
copy /y build\filedlg_hook32_new.dll build\filedlg_hook32.dll >nul 2>nul
if errorlevel 1 (
  echo [warn] filedlg_hook32.dll 被占用，未覆盖；新产物在 build\filedlg_hook32_new.dll
  echo        占用进程（关掉它们即可释放，再跑一次 build）：
  tasklist /m filedlg_hook32.dll 2>nul
)
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   src\agent32.cpp /Fe:build\filedlg_agent32_new.exe /Fo:build\agent32.obj ^
   /link /SUBSYSTEM:WINDOWS user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
copy /y build\filedlg_agent32_new.exe build\filedlg_agent32.exe >nul 2>nul
if errorlevel 1 (
  echo [warn] filedlg_agent32.exe 被占用，未覆盖；新产物在 build\filedlg_agent32_new.exe
  echo        占用进程（关掉它们即可释放，再跑一次 build）：
  tasklist /m filedlg_agent32.exe 2>nul
)

echo.
echo [ok] build\flowtary.exe / build\evtest.exe
echo [ok] build\filedlg_hook64.dll / build\filedlg_hook32.dll / build\filedlg_agent32.exe
exit /b 0
