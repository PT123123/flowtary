# =====================================================================
# Flowtary — Make 驱动的构建 / 发布入口（替代 build.bat / release.bat）
#
#   用法：
#     make build      完整构建：x64（flowtary.exe / evtest.exe / filedlg_hook64.dll）
#                     + x86（filedlg_hook32.dll / filedlg_agent32.exe），全部汇到 build\
#     make run        构建后启动 build\flowtary.exe（工作目录 build\，返回终端）
#     make release    版本自增（tools\version_bump.ps1）+ 完整构建 + 输出到 dist\
#                     （默认 dist\；可覆盖：make release OUT=D:\Tools\Flowtary）
#     make clean      清理 CMake 生成的产物
#     make distclean  删除整个 build\ 目录
#
#   前置：Visual Studio C++ 工作负载（vswhere 定位）、CMake、GNU make、ninja（需在 PATH）。
#   底层：CMake 生成 Ninja 构建文件；入口仍是 make build / make release。
#   注意：本 Makefile 以 cmd.exe 作为 shell，请在 cmd / PowerShell 中运行
#         make（GnuWin32）或 mingw32-make（Strawberry）。
# =====================================================================

SHELL := cmd.exe

CMAKE  := cmake
CONFIG := Release
ROOT   := $(CURDIR)
BUILD  := $(ROOT)\build
BUILD32:= $(BUILD)\x86
DIST   := $(ROOT)\dist
OUT    ?= $(DIST)

# 定位 Visual Studio 根目录（vswhere，默认位于 ProgramFiles(x86)）
VS := $(shell powershell -NoProfile -Command "$$w=\"$${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe\"; if(-not(Test-Path $$w)){$$w=\"$$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe\"}; if(Test-Path $$w){ & $$w -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }")

VCVARS64 := "$(VS)\VC\Auxiliary\Build\vcvars64.bat"
VCVARS32 := "$(VS)\VC\Auxiliary\Build\vcvars32.bat"

.PHONY: all build run release clean distclean help

all: build

# ---------------- 构建（x64 + x86，产物汇到 build\） ----------------
build:
	@if "$(VS)"=="" (echo [error] Visual Studio with C++ workload not found. & exit /b 1)
	@call $(VCVARS64) >nul 2>&1 && $(CMAKE) -S "$(ROOT)" -B "$(BUILD)" -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG)
	@call $(VCVARS64) >nul 2>&1 && $(CMAKE) --build "$(BUILD)" --config $(CONFIG)
	@call $(VCVARS32) >nul 2>&1 && $(CMAKE) -S "$(ROOT)" -B "$(BUILD32)" -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG) -DFLOWTARY_ARCH=x86
	@call $(VCVARS32) >nul 2>&1 && $(CMAKE) --build "$(BUILD32)" --config $(CONFIG)
	@copy /y "$(BUILD32)\filedlg_hook32.dll" "$(BUILD)\" >nul
	@copy /y "$(BUILD32)\filedlg_agent32.exe" "$(BUILD)\" >nul
	@echo [ok] build\flowtary.exe / build\evtest.exe
	@echo [ok] build\filedlg_hook64.dll / build\filedlg_hook32.dll / build\filedlg_agent32.exe

# ---------------- 运行（增量构建后启动主程序） ----------------
run: build
	@start "" /d "$(BUILD)" "$(BUILD)\flowtary.exe"

# ---------------- 发布（版本自增 + 构建 + 输出到 dist\） ----------------
release:
	@if "$(VS)"=="" (echo [error] Visual Studio with C++ workload not found. & exit /b 1)
	@echo [release] bumping build digit...
	@powershell -NoProfile -ExecutionPolicy Bypass -File "$(ROOT)\tools\version_bump.ps1" "$(ROOT)\src\version.h"
	@if errorlevel 1 (echo [error] Version auto-bump failed. & exit /b 1)
	@$(MAKE) build
	@echo [release] copying artifacts to $(OUT)...
	@powershell -NoProfile -ExecutionPolicy Bypass -File "$(ROOT)\tools\release_copy.ps1" -Root "$(ROOT)" -Dist "$(OUT)"

# ---------------- 清理 ----------------
clean:
	-@if exist "$(BUILD)\CMakeCache.txt" (call $(VCVARS64) >nul 2>&1 && $(CMAKE) --build "$(BUILD)" --target clean)
	-@if exist "$(BUILD32)\CMakeCache.txt" (call $(VCVARS32) >nul 2>&1 && $(CMAKE) --build "$(BUILD32)" --target clean)

distclean:
	@if exist "$(BUILD)" rmdir /s /q "$(BUILD)"

help:
	@echo Flowtary build targets: build, run, release, clean, distclean
	@echo   make build     - x64 + x86 full build into build/
	@echo   make run       - build then launch build\flowtary.exe
	@echo   make release   - bump version + build + copy into dist/
