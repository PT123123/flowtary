@echo off
@chcp 65001 >nul
rem ============================================================
rem  Flowtary �����ű���MSVC Release���������ļ���ɫ�棩
rem
rem  �÷���
rem    release.bat                        ���벢����� ��ĿĿ¼\dist\
rem    release.bat "D:\Tools\Flowtary"    �����ָ��Ŀ¼
rem
rem  ���<Ŀ��Ŀ¼>\flowtary-<�汾��>.exe
rem  �汾��ȡ�� src\version.h �� FT_VER_DOT����һ��Դ�������Ｔ�ɣ�
rem
rem  ˵����exe �� /MT ��̬���� C ����ʱ�������� VC++ ���п⣻
rem        �������ô�ע��� HKCU\Software\Flowtary������ exe �Ա�д�κ��ļ���
rem  ע�⣺�ļ��Ի�����תģ������ filedlg_hook64.dll / filedlg_hook32.dll /
rem        filedlg_agent32.exe���������ļ������� flowtary-<�汾>.exe ͬĿ¼����
rem ============================================================
setlocal

set "ROOT=%~dp0"
set "OUTDIR=%~1"
if "%OUTDIR%"=="" set "OUTDIR=%ROOT%dist"

rem ---- ��ȡ�汾�ţ����� src\version.h �� FT_VER_DOT�� ----
rem �����磺#define FT_VER_DOT   "1.0.0.0"   �� �� 3 �� token��%%~v ȥ������
set "VER=0.0.0.0"
for /f "usebackq tokens=3" %%v in (`findstr /c:"#define FT_VER_DOT" "%ROOT%src\version.h"`) do set "VER=%%~v"
echo [info] version = %VER%

rem ---- ��λ Visual Studio����Ҫ C++ �������أ� ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (
    echo [error] δ�ҵ��� C++ �������ص� Visual Studio��
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

if not exist "%ROOT%build" mkdir "%ROOT%build"

rem ---- ���� 1��������ʷ�������������ǲ����ľɰ桢������־/��ͼ���м��ļ��� ----
echo [1/4] ������ʷ����...
del /q "%ROOT%build\flowtary_old.exe"    2>nul
del /q "%ROOT%build\flowtary_new.exe"    2>nul
del /q "%ROOT%build\flowtary_fixed.exe"  2>nul
del /q "%ROOT%build\flowtary_test.exe"   2>nul
del /q "%ROOT%build\*_new.*"             2>nul
del /q "%ROOT%build\*.manifest"          2>nul
del /q "%ROOT%build\*.obj"               2>nul
del /q "%ROOT%build\*.log"               2>nul
del /q "%ROOT%build\*.png"               2>nul

rem ---- ���� 2������汾��Ϣ��Դ ----
echo [2/4] ����汾��Ϣ��Դ...
rc /nologo /fo "%ROOT%build\flowtary.res" "%ROOT%src\flowtary.rc"
if errorlevel 1 (
    echo [error] ��Դ����ʧ�ܡ�
    exit /b 1
)

rem ---- ���� 3������������ + �ļ��Ի�����תģ�飨/MT ��̬���� �� ���ļ���ɫ�� ----
rem ע������� build\flowtary_new.exe ��ʱ��������������ʵ����ռ�� build\flowtary.exe��
rem     ��ֱ��д�����ᱻ�������� LNK1104 �ܳ⣻�������ʱ���ɳ��׹�ܴ�����ͻ��
echo [3/4] ����������...
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   "%ROOT%src\main.cpp" /Fo:"%ROOT%build\main.obj"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /c ^
   "%ROOT%src\filedlg_jump.cpp" /Fo:"%ROOT%build\filedlg_jump.obj"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%build\main.obj" "%ROOT%build\filedlg_jump.obj" "%ROOT%build\flowtary.res" ^
   /Fe:"%ROOT%build\flowtary_new.exe" ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem ---- ���� 3b������ע�� DLL��32/64 λ���� 32 λ���Ӱ�װ���� ----
echo [3b] ���빳�� DLL �� 32 λ����...
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\hookdlg.cpp" /Fe:"%ROOT%build\filedlg_hook64_new.dll" /Fo:"%ROOT%build\hookdlg64.obj" ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\hookdlg.cpp" /Fe:"%ROOT%build\filedlg_hook32_new.dll" /Fo:"%ROOT%build\hookdlg32.obj" ^
   /link /DLL /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHsc /utf-8 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\agent32.cpp" /Fe:"%ROOT%build\filedlg_agent32_new.exe" /Fo:"%ROOT%build\agent32.obj" ^
   /link /SUBSYSTEM:WINDOWS user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem ---- ���� 4�������Ŀ��Ŀ¼�����汾�Ÿ����� ----
echo [4/4] ����� %OUTDIR% ...
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
copy /y "%ROOT%build\flowtary_new.exe" "%OUTDIR%\flowtary-%VER%.exe" >nul
if errorlevel 1 (
    echo [error] ����ʧ�ܡ�
    exit /b 1
)
rem �ļ��Ի�����תģ�������� 32/64 λע�� DLL �� 32 λ���֣������� exe ͬĿ¼
copy /y "%ROOT%build\filedlg_hook64_new.dll" "%OUTDIR%\filedlg_hook64.dll" >nul
if errorlevel 1 (
    echo [error] copy filedlg_hook64.dll failed: close ALL running Flowtary instances and retry.
    exit /b 1
)
copy /y "%ROOT%build\filedlg_hook32_new.dll" "%OUTDIR%\filedlg_hook32.dll" >nul
if errorlevel 1 (
    echo [error] copy filedlg_hook32.dll failed: close ALL running Flowtary instances (incl. filedlg_agent32.exe) and retry.
    exit /b 1
)
copy /y "%ROOT%build\filedlg_agent32_new.exe" "%OUTDIR%\filedlg_agent32.exe" >nul
if errorlevel 1 (
    echo [error] copy filedlg_agent32.exe failed: close ALL running Flowtary instances and retry.
    exit /b 1
)
rem �� build\flowtary.exe δ��ռ����˳��ˢ�£�ռ��ʱ���ԣ���Ӱ�췢�����
copy /y "%ROOT%build\flowtary_new.exe" "%ROOT%build\flowtary.exe" >nul 2>nul
copy /y "%ROOT%build\filedlg_hook64_new.dll" "%ROOT%build\filedlg_hook64.dll" >nul 2>nul
copy /y "%ROOT%build\filedlg_hook32_new.dll" "%ROOT%build\filedlg_hook32.dll" >nul 2>nul
copy /y "%ROOT%build\filedlg_agent32_new.exe" "%ROOT%build\filedlg_agent32.exe" >nul 2>nul

echo.
echo [ok] %OUTDIR%\flowtary-%VER%.exe
echo.
echo ��ʾ��
echo   1) �Ȱ���� exe �ŵ����չ̶���λ�ã�����������
echo   2) Ȼ���ڡ������Ҽ� �� ���� �� ���桹��ѡ�������Զ�����������
echo      ��ע������¼���ǹ�ѡ��һ�̵� exe ����·�����ȹ���Ų�ᵼ�¿�������ʧЧ��
echo   3) ���ɰ汾���ں�̨���У�ֱ�������°���Զ�������ʾ���ӹܣ������ֶ��˳��ɰ棩��
exit /b 0
