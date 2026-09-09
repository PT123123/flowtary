cmake_minimum_required(VERSION 3.20)

project(Flowtary LANGUAGES C CXX RC)

# =====================================================================
# MSVC include 路径自动检测（必须在 add_subdirectory(vendor) 之前）
# =====================================================================
include(cmake/msvc-include.cmake)

# =====================================================================
# vcpkg 集成（cpp-pinyin 拼音库）
# =====================================================================
# vcpkg 根目录候选列表。
# 注意：不能只信 $ENV{VCPKG_ROOT} —— VS 的 vcvars64.bat 会把它劫持为 VS 内置 vcpkg
# （如 .../VC/vcpkg），而 cpp-pinyin 实际装在 C:/vcpkg，导致后续头文件/库全部找不到。
# 因此改为「候选列表逐个校验」，命中真正含 cpp-pinyin 的那个根。
set(_VCPKG_CANDIDATES "")
if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
  list(APPEND _VCPKG_CANDIDATES "$ENV{VCPKG_ROOT}")
endif()
list(APPEND _VCPKG_CANDIDATES
  "C:/vcpkg"
  "${CMAKE_SOURCE_DIR}/../vcpkg"
  "$ENV{USERPROFILE}/vcpkg")
list(REMOVE_DUPLICATES _VCPKG_CANDIDATES)

# 布局优先级：installed/x64-windows（标准安装布局）> packages/cpp-pinyin_x64-windows（构建中间产物）
set(_CPP_PINYIN_LAYOUTS "installed/x64-windows" "packages/cpp-pinyin_x64-windows")

set(VCPKG_ROOT "")
set(CPP_PINYIN_INCLUDE_DIR "")
set(CPP_PINYIN_LIBRARY "")
set(CPP_PINYIN_DLL "")
set(CPP_PINYIN_DICT "")
foreach(_root IN LISTS _VCPKG_CANDIDATES)
  foreach(_layout IN LISTS _CPP_PINYIN_LAYOUTS)
    set(_base "${_root}/${_layout}")
    if(EXISTS "${_base}/include/cpp-pinyin/Pinyin.h" AND
       EXISTS "${_base}/lib/cpp-pinyin.lib")
      set(VCPKG_ROOT "${_root}")
      set(CPP_PINYIN_INCLUDE_DIR "${_base}/include")
      set(CPP_PINYIN_LIBRARY "${_base}/lib/cpp-pinyin.lib")
      if(EXISTS "${_base}/bin/cpp-pinyin.dll")
        set(CPP_PINYIN_DLL "${_base}/bin/cpp-pinyin.dll")
      endif()
      if(EXISTS "${_base}/share/cpp-pinyin/dict")
        set(CPP_PINYIN_DICT "${_base}/share/cpp-pinyin/dict")
      endif()
      break()
    endif()
  endforeach()
  if(CPP_PINYIN_LIBRARY)
    break()
  endif()
endforeach()
unset(_VCPKG_CANDIDATES)
unset(_CPP_PINYIN_LAYOUTS)

# vcpkg toolchain is disabled to avoid Windows SDK include path conflicts
# if(VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/vcpkg")
#   set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
#   set(VCPKG_MANIFEST_MODE ON)
#   message(STATUS "Using vcpkg from: ${VCPKG_ROOT}")
# endif()

# =====================================================================
# cpp-pinyin 拼音库
# =====================================================================
if(CPP_PINYIN_LIBRARY)
  message(STATUS "cpp-pinyin: ${CPP_PINYIN_LIBRARY} (vcpkg root: ${VCPKG_ROOT})")
else()
  message(STATUS "cpp-pinyin not found; searched vcpkg roots: C:/vcpkg, ../vcpkg, %USERPROFILE%/vcpkg")
endif()

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

# cpp-pinyin 拼音库
if(CPP_PINYIN_LIBRARY)
  list(APPEND FLOWTARY_LIBS ${CPP_PINYIN_LIBRARY})
endif()
if(CPP_PINYIN_INCLUDE_DIR)
  include_directories(${CPP_PINYIN_INCLUDE_DIR})
endif()

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
    src/flowtary.rc
    src/flowtary.ico)
  set_source_files_properties(src/flowtary.ico PROPERTIES HEADER_FILE_ONLY TRUE)
  set_target_properties(flowtary PROPERTIES OUTPUT_NAME flowtary)
  target_link_libraries(flowtary PRIVATE ${FLOWTARY_LIBS})

  # 第三方源码子项目（vendor/CMakeLists.txt）：
  #   ScreenCapture.exe / ImageReader.exe 均为源码构建，输出目录即 build\
  #   （ImageReader 由 vendor 的 ImageReader 目标直接产出，无需复制）。
  add_subdirectory(vendor)

  # 复制 cpp-pinyin DLL 和字典文件到输出目录
  if(CPP_PINYIN_DLL)
    add_custom_command(TARGET flowtary POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CPP_PINYIN_DLL}"
        "$<TARGET_FILE_DIR:flowtary>/cpp-pinyin.dll"
      COMMENT "Copying cpp-pinyin.dll to output directory")
  endif()
  if(CPP_PINYIN_DICT)
    add_custom_command(TARGET flowtary POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CPP_PINYIN_DICT}"
        "$<TARGET_FILE_DIR:flowtary>/dict"
      COMMENT "Copying cpp-pinyin dictionary files to output directory")
  endif()

  add_executable(evtest tools/evtest.cpp)
  set_target_properties(evtest PROPERTIES OUTPUT_NAME evtest)
  target_link_libraries(evtest PRIVATE user32 shell32)

  add_library(filedlg_hook64 SHARED src/hookdlg.cpp)
  set_target_properties(filedlg_hook64 PROPERTIES
    OUTPUT_NAME filedlg_hook64
    PREFIX "")
  target_link_libraries(filedlg_hook64 PRIVATE ole32 uuid shell32 shlwapi user32)
endif()
