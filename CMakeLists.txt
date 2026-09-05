cmake_minimum_required(VERSION 3.20)
project(Flowtary LANGUAGES CXX RC)

# =====================================================================
# Flowtary — CMake 构建定义
#   取代旧的 build.bat / release.bat（原为手写 cl/rc 命令的批处理）。
#
# 双架构说明：
#   64 位目标（flowtary.exe / evtest.exe / filedlg_hook64.dll）在 x64 环境构建；
#   32 位目标（filedlg_hook32.dll / filedlg_agent32.exe）需要 x86 工具链。
#   通过 -DFLOWTARY_ARCH=x86 触发 32 位子构建（由 Makefile 切换 vcvars32 环境完成）。
#
# 产物统一输出到 CMake 二进制目录（build\）：
#   flowtary.exe / evtest.exe / filedlg_hook64.dll
#   filedlg_hook32.dll / filedlg_agent32.exe
# =====================================================================

if(NOT DEFINED FLOWTARY_ARCH)
  set(FLOWTARY_ARCH "x64")
endif()

# 默认 Release（与旧脚本 /O2 /DNDEBUG 对齐）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 与旧 build.bat 对齐的 MSVC 编译选项 ----
# /MT(静态运行库) /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS
# /O2 与 /DNDEBUG 由 Release 配置默认提供。
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS)
endif()

# 主程序（flowtary.exe）链接库 = src/main.cpp + src/filedlg_jump.cpp 的 #pragma comment(lib) 合集
set(FLOWTARY_LIBS
  user32 gdi32 shell32 ole32 imm32 dwmapi advapi32 uxtheme msimg32
  oleaut32 uuid shlwapi)

if(FLOWTARY_ARCH STREQUAL "x86")
  # ================= 32 位子构建 =================
  add_library(filedlg_hook32 SHARED src/hookdlg.cpp)
  set_target_properties(filedlg_hook32 PROPERTIES
    OUTPUT_NAME filedlg_hook32
    PREFIX "")
  target_link_libraries(filedlg_hook32 PRIVATE ole32 uuid shell32 shlwapi user32)

  add_executable(filedlg_agent32 src/agent32.cpp)
  set_target_properties(filedlg_agent32 PROPERTIES
    OUTPUT_NAME filedlg_agent32
    WIN32_EXECUTABLE TRUE)
  target_link_libraries(filedlg_agent32 PRIVATE user32)

else()
  # ================= 64 位主构建 =================
  add_executable(flowtary WIN32
    src/main.cpp
    src/filedlg_jump.cpp
    src/flowtary.rc)
  set_target_properties(flowtary PROPERTIES OUTPUT_NAME flowtary)
  target_link_libraries(flowtary PRIVATE ${FLOWTARY_LIBS})

  # 将 ScreenCapture.exe 和 ImageReader.exe 复制到构建输出目录
  add_custom_command(TARGET flowtary POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/ScreenCapture.exe"
      "$<TARGET_FILE_DIR:flowtary>/ScreenCapture.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/ImageReader.exe"
      "$<TARGET_FILE_DIR:flowtary>/ImageReader.exe"
    COMMENT "Copying ScreenCapture.exe and ImageReader.exe to output directory")

  add_executable(evtest tools/evtest.cpp)
  set_target_properties(evtest PROPERTIES OUTPUT_NAME evtest)
  target_link_libraries(evtest PRIVATE user32 shell32)

  add_library(filedlg_hook64 SHARED src/hookdlg.cpp)
  set_target_properties(filedlg_hook64 PROPERTIES
    OUTPUT_NAME filedlg_hook64
    PREFIX "")
  target_link_libraries(filedlg_hook64 PRIVATE ole32 uuid shell32 shlwapi user32)
endif()
