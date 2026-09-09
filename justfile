# ============================================================
# Flowtary — just 任务入口（委托现有 Makefile）
#
#   用法：
#     just                列出所有可用命令
#     just build          完整构建 x64 + x86，产物汇到 build\
#     just run            构建（增量）并启动 flowtary.exe
#     just release        版本自增 + 完整构建 + 输出到 dist\
#     just release-to DIR 发版到指定目录（如 just release-to "D:\Tools\Flowtary"）
#     just clean          清理 CMake 生成的产物（保留 build 目录）
#     just distclean      删除整个 build\ 缓存（换工具链/配置异常时重来）
#
#   底层调用链：just -> make -> cmake -> ninja
#   just 只是更友好的命令入口，不是编译系统；构建引擎仍是 Makefile。
#   前置：make / cmake / ninja 在 PATH，VS 2022/Build Tools C++ 工作负载已装。
# ============================================================

# 在 Windows 上用 cmd.exe 作为 recipe shell（无 sh 时的可靠选择）
set shell := ["cmd.exe", "/c"]

# 列出所有可用命令（无参数时的默认动作）
default:
    @just --list

# 完整构建：x64（主程序/hook）+ x86（hook/agent），产物汇到 build\
build:
    @make build

# 构建（增量）并启动主程序，工作目录为 build\
run:
    @make run

# 版本自增 + 完整构建 + 输出到 dist\
release:
    @make release

# 发版到指定目录，如：just release-to "D:\Tools\Flowtary"
release-to out:
    @make release OUT={{out}}

# 清理 CMake 生成的产物（保留 build 目录）
clean:
    @make clean

# 删除整个 build\ 缓存（换工具链/配置异常时重来）
distclean:
    @make distclean
