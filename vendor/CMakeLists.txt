# =====================================================================
# vendor — 第三方源码子项目（git submodule）
#
# 设计原则：**不修改 submodule 内部任何文件**，所有构建目标集中在此定义，
# 保证上游源码可被干净地 checkout / 更新 / 切换分支。
#
#   ScreenCapture (xland/ScreenCapture 2.4.25) → ScreenCapture.exe
#   Ling          (xland/Ling)                 → Ling.lib
#   TinyOCR       (xland/TinyOCR)              → TinyOCR.lib
#   ImageReader   (xland/ImageReader 1.0.2)    → ImageReader.exe
#
# 功能裁剪（Flowtary 只需要「截图 / 截图贴图 / OCR」）：
#   ScreenCapture 的录屏（MP4 + GIF）与长截图模块不参与编译。
#   这同时绕开了 WinVideoMp4.hpp 对 ATL（atlbase.h）的依赖——本机
#   BuildTools 未安装 ATL 组件，装它不如直接砍掉用不到的功能。
#
# 本目录只在 x64 主构建中被 add_subdirectory；x86 子构建不涉及。
# =====================================================================

# ---------------------------------------------------------------------
# 定位 Windows SDK 的 C++/WinRT 投影头
#   ScreenCapture 的 pch.h 里有 #include <winrt/Windows.Data.Json.h>。
#   SDK 10.0.17134+ 自带 <SDK>/Include/<ver>/cppwinrt/winrt/*.h 投影头，
#   NTFS 大小写不敏感 ⇒ winrt/Windows.Data.Json.h 命中 windows.data.json.h。
# ---------------------------------------------------------------------
function(_flowtary_find_cppwinrt _out)
  set(_candidate "")
  get_filename_component(_kits_root
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
    ABSOLUTE)
  if(_kits_root AND EXISTS "${_kits_root}/Include")
    file(GLOB _vers RELATIVE "${_kits_root}/Include" "${_kits_root}/Include/10.0.*")
    list(SORT _vers)
    list(REVERSE _vers)                 # 版本号从新到旧
    foreach(_v IN LISTS _vers)
      if(EXISTS "${_kits_root}/Include/${_v}/cppwinrt/winrt/base.h")
        set(_candidate "${_kits_root}/Include/${_v}/cppwinrt")
        break()
      endif()
    endforeach()
  endif()
  set(${_out} "${_candidate}" PARENT_SCOPE)
endfunction()

_flowtary_find_cppwinrt(FLOWTARY_CPPWINRT_DIR)
if(FLOWTARY_CPPWINRT_DIR)
  message(STATUS "[vendor] C++/WinRT 投影头: ${FLOWTARY_CPPWINRT_DIR}")
else()
  message(WARNING "[vendor] 未找到 Windows SDK cppwinrt 投影头，"
                  "ScreenCapture 的 <winrt/...> 包含可能失败")
endif()

# =====================================================================
# ScreenCapture（xland/ScreenCapture 2.4.25）
#   纯 Win32 + Direct2D/D3D11/DComp + MediaFoundation，无第三方依赖；
#   仅借 C++/WinRT 的 Windows.Data.Json 读写 config.json。
#   产物 ScreenCapture.exe 为独立进程，Flowtary 通过 ShellExecute 调用。
# =====================================================================
set(SC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ScreenCapture/ScreenCapture")

if(EXISTS "${SC_DIR}/main.cpp")
  file(GLOB_RECURSE SC_CXX_SOURCES "${SC_DIR}/*.cpp")

  # ---- 裁掉不需要的功能模块（录屏 / 长截图）----
  # 这些模块的入口只被 App.cpp 与 Tray.cpp 引用，二者会在下面生成裁剪副本，
  # 因此排除实现文件后不会留下未定义符号。
  set(_SC_DROP_REGEX
    "Win/WinVideo\\.cpp$"      # 录屏窗口：内含 MP4(ATL/MF) 与 GIF(cgif) 录制
    "Win/WinLong\\.cpp$"       # 长截图窗口
    "Tool/ToolVideo\\.cpp$"    # 录屏工具条
    "Tool/ToolLong\\.cpp$")    # 长截图工具条
  foreach(_re IN LISTS _SC_DROP_REGEX)
    list(FILTER SC_CXX_SOURCES EXCLUDE REGEX "${_re}")
  endforeach()

  # Win/cgif 是 GIF 编码用的 C 库，只被录屏模块使用，一并排除。
  # （若日后重新启用录屏，需把 ${SC_DIR}/*.c 加回并配 SKIP_PRECOMPILE_HEADERS）
  set(SC_C_SOURCES "")

  # ---- 生成 App.cpp / Tray.cpp 的裁剪副本 ----
  # 二者是唯一引用 WinLong / WinVideo 的编译单元。按行剔除对这些模块的引用
  # 后，录屏与长截图的实现便可整体不参与编译（连带 ATL 依赖一并消失）。
  # 副本写入 build 目录，submodule 保持零改动。
  set(_SC_STRIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/stripped/ScreenCapture")
  file(MAKE_DIRECTORY "${_SC_STRIP_DIR}")

  # 命中任一模式的整行都会被删除
  set(_SC_STRIP_PATTERNS
    "WinLong::"                  # 长截图窗口调用
    "WinVideo::"                 # 录屏窗口调用（含 stopIfRecording）
    "unique_ptr<WinLong>"        # extern 声明
    "unique_ptr<WinVideo>"
    "if \\(winLong\\)"           # disposeDeviceIfIdle 中的引用
    "if \\(winVideo\\)"
    "\"Win/WinLong\\.h\""        # 头文件包含
    "\"Win/WinVideo\\.h\""
    "AppendMenu.*capLongMsg"     # 托盘右键菜单项
    "AppendMenu.*capVideoMsg")

  # _extra：裁剪后需要补回的头文件。
  #   Tray.cpp 原本没有直接 include "Util.h"，它是靠 WinVideo.h →
  #   WinVideoGif.hpp → Util.h 间接拿到的；砍掉 WinVideo.h 后需显式补回。
  function(_sc_strip_source _src _name _extra)
    file(READ "${_src}" _txt)
    foreach(_p IN LISTS _SC_STRIP_PATTERNS)
      string(REGEX REPLACE "[^\n]*${_p}[^\n]*\n" "" _txt "${_txt}")
    endforeach()
    if(NOT _extra STREQUAL "")
      string(REGEX REPLACE "#include \"pch\\.h\"" "#include \"pch.h\"\n#include \"${_extra}\""
             _txt "${_txt}")
    endif()
    file(WRITE "${_SC_STRIP_DIR}/${_name}" "${_txt}")
    # 自检：裁剪后若仍残留引用，说明上游代码结构变了，直接报错而不是静默编译出坏东西
    string(REGEX MATCH "WinLong::|WinVideo::|\\(winLong\\)|\\(winVideo\\)" _leftover "${_txt}")
    if(_leftover)
      message(FATAL_ERROR
        "[vendor] ${_name} 裁剪后仍残留 \"${_leftover}\" 引用，"
        "上游代码结构可能已变化，请更新 vendor/CMakeLists.txt 的裁剪规则")
    endif()
  endfunction()

  _sc_strip_source("${SC_DIR}/App.cpp"  "App.cpp"  "")
  _sc_strip_source("${SC_DIR}/Tray.cpp" "Tray.cpp" "Util.h")

  list(FILTER SC_CXX_SOURCES EXCLUDE REGEX "App\\.cpp$")
  list(FILTER SC_CXX_SOURCES EXCLUDE REGEX "Tray\\.cpp$")
  list(APPEND SC_CXX_SOURCES
    "${_SC_STRIP_DIR}/App.cpp"
    "${_SC_STRIP_DIR}/Tray.cpp")

  set(SC_RESOURCE "${SC_DIR}/Res/Resource.rc")

  add_executable(ScreenCapture WIN32
    ${SC_CXX_SOURCES}
    ${SC_C_SOURCES}
    ${SC_RESOURCE})

  set_target_properties(ScreenCapture PROPERTIES
    OUTPUT_NAME ScreenCapture
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")   # 与 flowtary.exe 同目录

  target_include_directories(ScreenCapture PRIVATE
    "${SC_DIR}"                     # pch.h / App.h / Win/... 的包含基准
    "${FLOWTARY_CPPWINRT_DIR}")     # winrt/*.h 投影头

  # 与上游 vcxproj 对齐：只有 NOMINMAX。
  # 切勿加 WIN32_LEAN_AND_MEAN——它会让 windows.h 排除 shellapi.h，
  # 导致 NOTIFYICONDATA（Tray.h）与 ShellExecute（WinSettingAbout.cpp）未声明。
  target_compile_definitions(ScreenCapture PRIVATE
    NOMINMAX)

  target_precompile_headers(ScreenCapture PRIVATE "${SC_DIR}/pch.h")

  # MediaFoundation 相关库只为录屏服务，已随模块裁剪一并移除。
  target_link_libraries(ScreenCapture PRIVATE
    comctl32 d2d1 dcomp dwmapi dwrite imm32
    ole32 shcore shell32 shlwapi
    version windowsapp windowscodecs winmm
    userenv uuid ntdll)
endif()
