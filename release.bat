@echo off
rem ============================================================
rem  Flowtary 发布脚本（MSVC Release，产出单文件绿色版）
rem
rem  用法：
rem    release.bat                        编译并输出到 项目目录\dist\
rem    release.bat "D:\Tools\Flowtary"    输出到指定目录
rem
rem  产物：<目标目录>\flowtary-<版本号>.exe
rem  版本号取自 src\version.h 的 FT_VER_DOT（单一来源，改那里即可）
rem
rem  说明：exe 用 /MT 静态链接 C 运行时，不依赖 VC++ 运行库；
rem        所有设置存注册表 HKCU\Software\Flowtary，不往 exe 旁边写任何文件，
rem        因此只复制这一个 exe 就能用，无需安装。
rem ============================================================
setlocal

set "ROOT=%~dp0"
set "OUTDIR=%~1"
if "%OUTDIR%"=="" set "OUTDIR=%ROOT%dist"

rem ---- 读取版本号（解析 src\version.h 的 FT_VER_DOT） ----
rem 行形如：#define FT_VER_DOT   "1.0.0.0"   → 第 3 个 token，%%~v 去掉引号
set "VER=0.0.0.0"
for /f "usebackq tokens=3" %%v in (`findstr /c:"#define FT_VER_DOT" "%ROOT%src\version.h"`) do set "VER=%%~v"
echo [info] version = %VER%

rem ---- 定位 Visual Studio（需要 C++ 工作负载） ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (
    echo [error] 未找到带 C++ 工作负载的 Visual Studio。
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

if not exist "%ROOT%build" mkdir "%ROOT%build"

rem ---- 步骤 1：清理历史残留（换名覆盖产生的旧版、调试日志/截图、中间文件） ----
echo [1/4] 清理历史残留...
del /q "%ROOT%build\flowtary_old.exe"    2>nul
del /q "%ROOT%build\flowtary_new.exe"    2>nul
del /q "%ROOT%build\flowtary_fixed.exe"  2>nul
del /q "%ROOT%build\flowtary_test.exe"   2>nul
del /q "%ROOT%build\*.manifest"          2>nul
del /q "%ROOT%build\*.obj"               2>nul
del /q "%ROOT%build\*.log"               2>nul
del /q "%ROOT%build\*.png"               2>nul

rem ---- 步骤 2：编译版本信息资源 ----
echo [2/4] 编译版本信息资源...
rc /nologo /fo "%ROOT%build\flowtary.res" "%ROOT%src\flowtary.rc"
if errorlevel 1 (
    echo [error] 资源编译失败。
    exit /b 1
)

rem ---- 步骤 3：编译主程序（/MT 静态链接 → 单文件绿色） ----
rem 注：输出到 build\flowtary_new.exe 临时名。开发期托盘实例常占用 build\flowtary.exe，
rem     若直接写该名会被链接器以 LNK1104 拒斥；输出到临时名可彻底规避此锁冲突。
echo [3/4] 编译主程序...
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\main.cpp" "%ROOT%build\flowtary.res" ^
   /Fe:"%ROOT%build\flowtary_new.exe" /Fo:"%ROOT%build\main.obj" ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem ---- 步骤 4：输出到目标目录（按版本号改名） ----
echo [4/4] 输出到 %OUTDIR% ...
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
copy /y "%ROOT%build\flowtary_new.exe" "%OUTDIR%\flowtary-%VER%.exe" >nul
if errorlevel 1 (
    echo [error] 复制失败。
    exit /b 1
)
rem 若 build\flowtary.exe 未被占用则顺手刷新（占用时忽略，不影响发布产物）
copy /y "%ROOT%build\flowtary_new.exe" "%ROOT%build\flowtary.exe" >nul 2>nul

echo.
echo [ok] %OUTDIR%\flowtary-%VER%.exe
echo.
echo 提示：
echo   1) 先把这个 exe 放到最终固定的位置，再运行它；
echo   2) 然后在「托盘右键 → 设置 → 常规」勾选「开机自动启动」——
echo      该注册表项记录的是勾选那一刻的 exe 绝对路径，先勾后挪会导致开机启动失效；
echo   3) 若旧版本正在后台运行，直接运行新版会自动弹窗提示并接管（无需手动退出旧版）。
exit /b 0
