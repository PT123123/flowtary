@echo off
rem =====================================================================
rem  build-fltk.bat - build FLTK 1.4.5 static libs (/MT, matches flowtary)
rem  Output: vendor\fltk-install\{include,lib}
rem  Usage : tools\build-fltk.bat    (first run takes ~2-4 min)
rem  NOTE  : keep this file ASCII-only + CRLF; cmd misparses UTF-8 batch.
rem =====================================================================
setlocal
cd /d "%~dp0.."

set "FLTK_SRC=%CD%\vendor\fltk-1.4.5"
set "FLTK_BUILD=%CD%\vendor\fltk-build"
set "FLTK_PREFIX=%CD%\vendor\fltk-install"

if not exist "%FLTK_SRC%\CMakeLists.txt" (
  echo [error] FLTK source not found: %FLTK_SRC%
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere.exe not found
  exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo [error] no Visual Studio with C++ workload found
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] vcvars64.bat failed
  exit /b 1
)

cmake -S "%FLTK_SRC%" -B "%FLTK_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DFLTK_MSVC_RUNTIME_DLL=OFF -DBUILD_SHARED_LIBS=OFF -DFLTK_BUILD_SHARED_LIBS=OFF -DFLTK_BUILD_EXAMPLES=OFF -DFLTK_BUILD_TEST=OFF -DFLTK_BUILD_FLUID=OFF -DFLTK_BUILD_FLTK_OPTIONS=OFF -DFLTK_BUILD_FORMS=OFF -DFLTK_OPTION_PRINT_SUPPORT=OFF -DFLTK_USE_SYSTEM_ZLIB=OFF -DFLTK_USE_SYSTEM_LIBPNG=OFF -DFLTK_USE_SYSTEM_LIBJPEG=OFF -DCMAKE_INSTALL_PREFIX="%FLTK_PREFIX%"
if errorlevel 1 (
  echo [error] FLTK configure failed
  exit /b 1
)

cmake --build "%FLTK_BUILD%"
if errorlevel 1 (
  echo [error] FLTK build failed
  exit /b 1
)

cmake --install "%FLTK_BUILD%"
if errorlevel 1 (
  echo [error] FLTK install failed
  exit /b 1
)

echo [ok] FLTK static libs installed to %FLTK_PREFIX%
dir /b "%FLTK_PREFIX%\lib\*.lib"
endlocal
