cmake_minimum_required(VERSION 3.20)

project(Flowtary LANGUAGES C CXX RC)
include(CTest)

# Explicit development target only; stages the query host but never starts it.
option(FLOWTARY_EXPERIMENTAL_INDEXER "Expose the experimental Rust indexer build target" OFF)
if(FLOWTARY_EXPERIMENTAL_INDEXER)
  find_program(FLOWTARY_CARGO cargo REQUIRED)
  add_custom_target(flowtary-indexer-experimental
    COMMAND "${FLOWTARY_CARGO}" build --release --locked --jobs 1
      --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/indexer/Cargo.toml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/indexer/target/release/flowtary-indexer.exe"
      "$<TARGET_FILE_DIR:flowtary>/flowtary-indexer.exe"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/indexer"
    COMMENT "Build and stage snapshot query helper (does not collect volumes)"
    VERBATIM)
endif()

# =====================================================================
# MSVC include 路径自动检测（必须在 add_subdirectory(vendor) 之前）
# =====================================================================
include(cmake/msvc-include.cmake)

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


# =====================================================================
# FLTK（设置窗 sidecar 的 UI 库）
#   静态库由 tools/build-fltk.bat 预编译到 vendor/fltk-install（/MT，与本项目一致）。
#   未构建时只跳过 flowtary-settings 目标，主程序不受影响。
# =====================================================================
set(FLTK_PREFIX "${CMAKE_SOURCE_DIR}/vendor/fltk-install")
set(FLTK_FOUND FALSE)
if(FLOWTARY_ARCH STREQUAL "x64" AND EXISTS "${FLTK_PREFIX}/CMake/FLTKConfig.cmake")
  find_package(FLTK CONFIG QUIET PATHS "${FLTK_PREFIX}/CMake" NO_DEFAULT_PATH)
  if(TARGET fltk::fltk)
    set(FLTK_FOUND TRUE)
    message(STATUS "FLTK: ${FLTK_PREFIX}")
  endif()
endif()
if(NOT FLTK_FOUND)
  message(STATUS "FLTK 未就绪：flowtary-settings.exe 不会构建（请先运行 tools\\build-fltk.bat）")
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
  if(BUILD_TESTING)
    add_executable(file_url_path_test tests/file_url_path_test.cpp)
    target_link_libraries(file_url_path_test PRIVATE shlwapi)
    add_test(NAME file_url_path COMMAND file_url_path_test)
    add_executable(index_dirs_test tests/index_dirs_test.cpp src/settings_model.cpp)
    target_include_directories(index_dirs_test PRIVATE src)
    target_link_libraries(index_dirs_test PRIVATE shell32)
    add_test(NAME index_dirs COMMAND index_dirs_test)
    add_executable(startup_sync_test tests/startup_sync_test.cpp src/settings_model.cpp)
    target_include_directories(startup_sync_test PRIVATE src)
    target_link_libraries(startup_sync_test PRIVATE shell32 advapi32)
    add_test(NAME startup_sync COMMAND startup_sync_test)
    set_tests_properties(startup_sync PROPERTIES TIMEOUT 10)
    add_executable(search_history_test tests/search_history_test.cpp src/search_history.cpp
      src/settings_model.cpp)
    target_include_directories(search_history_test PRIVATE src)
    target_link_libraries(search_history_test PRIVATE shell32)
    add_test(NAME search_history COMMAND search_history_test)
  endif()

  add_executable(flowtary WIN32
    src/main.cpp
    src/filedlg_jump.cpp
    src/settings_model.cpp
    src/search_history.cpp
    src/df_search.cpp
    src/indexer_client.cpp
    src/workshop_catalog.cpp
    src/flowtary.rc
    src/flowtary.ico)
  set_source_files_properties(src/flowtary.ico PROPERTIES HEADER_FILE_ONLY TRUE)
  set_target_properties(flowtary PROPERTIES OUTPUT_NAME flowtary)
  target_link_libraries(flowtary PRIVATE ${FLOWTARY_LIBS})
  if(FLOWTARY_EXPERIMENTAL_INDEXER)
    add_dependencies(flowtary flowtary-indexer-experimental)
  endif()

  # 第三方源码子项目（vendor/CMakeLists.txt）：
  #   ScreenCapture.exe / ImageReader.exe 均为源码构建，输出目录即 build\
  #   （ImageReader 由 vendor 的 ImageReader 目标直接产出，无需复制）。
  add_subdirectory(vendor)


  add_executable(evtest tools/evtest.cpp)
  set_target_properties(evtest PROPERTIES OUTPUT_NAME evtest)
  target_link_libraries(evtest PRIVATE user32 shell32)

  # 应用版本目录解析的断言（结果排序把最新版抬到前面；规则见 src/app_ver.h）
  add_executable(appver_test tools/appver_test.cpp)
  set_target_properties(appver_test PROPERTIES OUTPUT_NAME appver_test)

  # 大小写还原三通道实测（整目录枚举 / 精确名 FindFirstFile / GetLongPathNameW）
  add_executable(case_probe tools/case_probe.cpp)
  set_target_properties(case_probe PROPERTIES OUTPUT_NAME case_probe)
  target_link_libraries(case_probe PRIVATE shell32)

  # 路径副标题省略号取证：同宽同字体画 PATH/END 两种省略方式到 BMP
  add_executable(path_ellipsis_probe tools/path_ellipsis_probe.cpp)
  set_target_properties(path_ellipsis_probe PROPERTIES OUTPUT_NAME path_ellipsis_probe)
  target_link_libraries(path_ellipsis_probe PRIVATE gdi32 user32 shell32)

  # d/f 搜索后端诊断工具（仅查询已有 Windows Search 索引）
  # （df_search 只按文件名/路径文本匹配）
  add_executable(wsquery tools/wsquery.cpp src/df_search.cpp)
  set_target_properties(wsquery PROPERTIES OUTPUT_NAME wsquery)
  target_include_directories(wsquery PRIVATE src)
  target_link_libraries(wsquery PRIVATE ole32 shell32 uuid)

  # 兼容旧启动命令：独立索引已移除，此程序只提示并退出
  add_executable(flowtary-search tools/ft_search_main.cpp)
  set_target_properties(flowtary-search PROPERTIES OUTPUT_NAME flowtary-search)
  target_include_directories(flowtary-search PRIVATE src)
  target_link_libraries(flowtary-search PRIVATE ole32 shell32 uuid ws2_32 user32)
  if(MSVC)
    target_compile_options(flowtary-search PRIVATE /Zi)
    target_link_options(flowtary-search PRIVATE /DEBUG "/MAP:$<TARGET_FILE_DIR:flowtary-search>/flowtary-search.map")
  endif()
  if(BUILD_TESTING)
    add_executable(app-catalog-probe tools/app_catalog_probe.cpp src/df_search.cpp src/indexer_client.cpp
      src/workshop_catalog.cpp src/filedlg_jump.cpp src/settings_model.cpp src/search_history.cpp)
    target_include_directories(app-catalog-probe PRIVATE src)
    target_link_libraries(app-catalog-probe PRIVATE ${FLOWTARY_LIBS})
    add_executable(app_catalog_test tests/app_catalog_test.cpp)
    target_include_directories(app_catalog_test PRIVATE src)
    target_link_libraries(app_catalog_test PRIVATE shell32 ole32 uuid advapi32 user32)
    add_test(NAME app_catalog COMMAND app_catalog_test)
    set_tests_properties(app_catalog PROPERTIES TIMEOUT 15)
    add_executable(existing_index_search_test tests/existing_index_search_test.cpp)
    target_include_directories(existing_index_search_test PRIVATE src)
    target_link_libraries(existing_index_search_test PRIVATE ole32 shell32 uuid ws2_32 user32)
    add_test(NAME existing_index_search COMMAND existing_index_search_test)
    set_tests_properties(existing_index_search PROPERTIES TIMEOUT 15)
    add_executable(indexer_client_test tests/indexer_client_test.cpp src/indexer_client.cpp)
    target_include_directories(indexer_client_test PRIVATE src)
    target_compile_definitions(indexer_client_test PRIVATE FLOWTARY_TEST_SNAPSHOT_OVERRIDE=1)
    add_test(NAME indexer_client COMMAND indexer_client_test)
    set_tests_properties(indexer_client PROPERTIES TIMEOUT 10)
    add_executable(existing_index_ui_test tests/existing_index_ui_test.cpp
      src/filedlg_jump.cpp src/settings_model.cpp src/search_history.cpp)
    target_include_directories(existing_index_ui_test PRIVATE src)
    target_link_libraries(existing_index_ui_test PRIVATE ${FLOWTARY_LIBS})
    add_test(NAME existing_index_ui COMMAND existing_index_ui_test)
    set_tests_properties(existing_index_ui PROPERTIES TIMEOUT 20)
    add_executable(workshop_catalog_test tests/workshop_catalog_test.cpp src/workshop_catalog.cpp)
    target_include_directories(workshop_catalog_test PRIVATE src)
    add_test(NAME workshop_catalog COMMAND workshop_catalog_test)
    set_tests_properties(workshop_catalog PROPERTIES TIMEOUT 10)
    add_executable(workshop-catalog-probe tools/workshop_catalog_probe.cpp src/workshop_catalog.cpp)
    target_include_directories(workshop-catalog-probe PRIVATE src)
    add_executable(directory-api-probe tools/directory_api_probe.cpp)
    target_link_libraries(directory-api-probe PRIVATE Psapi)
  endif()

  add_library(filedlg_hook64 SHARED src/hookdlg.cpp)
  set_target_properties(filedlg_hook64 PROPERTIES
    OUTPUT_NAME filedlg_hook64
    PREFIX "")
  target_link_libraries(filedlg_hook64 PRIVATE ole32 uuid shell32 shlwapi user32)

  if(TARGET fltk::fltk)
    # 独立进程的设置窗（FLTK 静态链接，无额外 DLL 依赖）
    add_executable(flowtary-settings WIN32
      src/settings_main.cpp
      src/settings_model.cpp
      src/search_history.cpp
      src/settings.rc)
    set_target_properties(flowtary-settings PROPERTIES OUTPUT_NAME flowtary-settings)
    target_link_libraries(flowtary-settings PRIVATE
      fltk::fltk fltk::images comctl32 ws2_32 gdiplus user32 gdi32 shell32 ole32 advapi32 dwmapi)
  endif()
endif()
