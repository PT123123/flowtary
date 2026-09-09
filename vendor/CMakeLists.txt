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
    ${SC_RESOURCE}
    # DPI 感知清单：上游 vcxproj 的 EnableDpiAwareness=CMake 不会自动做，
    # 缺失时缩放>100% 会被 DWM 放大截图覆盖窗（见 cmake/DpiAware.manifest 注释）
    "${CMAKE_SOURCE_DIR}/cmake/DpiAware.manifest")

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

# =====================================================================
# vcpkg 静态三元组定位（opencv / onnxruntime）
#
# TinyOCR 链接的是 onnxruntime 的 **static-mt** 版本，与本项目 /MT 一致；
# 官方 GitHub Release 只提供动态库版，故必须用 vcpkg 的 x64-windows-static。
# =====================================================================
function(_flowtary_find_triplet _triplet _out)
  foreach(_root IN ITEMS "C:/vcpkg" "${CMAKE_SOURCE_DIR}/../vcpkg" "$ENV{VCPKG_ROOT}")
    if(_root AND EXISTS "${_root}/installed/${_triplet}/include")
      set(${_out} "${_root}/installed/${_triplet}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${_out} "" PARENT_SCOPE)
endfunction()

_flowtary_find_triplet("x64-windows-static" FLOWTARY_VCPKG_STATIC)

# ---- Yoga（Ling 内置的 flexbox 布局引擎，上游为独立 vcxproj）----
set(LING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Ling")
if(EXISTS "${LING_DIR}/src/Ling.cpp")
  # Yoga 头文件在 vendor/Ling/yoga/ 下。上游 vcxproj 的 SolutionDir 是 Ling 根,
  # #include <yoga/Yoga.h> 实际映射到 Ling/yoga/Yoga.h。CMake 中 include 目录用
  # Ling 根（而非 yoga 子目录），以匹配 MSBuild $(SolutionDir) 语义。
  file(GLOB_RECURSE YOGA_SOURCES "${LING_DIR}/yoga/*.cpp")
  add_library(yoga STATIC ${YOGA_SOURCES})
  target_include_directories(yoga PUBLIC "${LING_DIR}")
  set_target_properties(yoga PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  # ---- Ling（GUI 框架）----
  file(GLOB_RECURSE LING_SOURCES "${LING_DIR}/src/*.cpp")
  add_library(Ling STATIC ${LING_SOURCES})
  # include 基准为 Ling 仓库根（上游 vcxproj 的 $(SolutionDir)），
  # 供 ImageReader 以 <Ling/...> 之外的写法包含时也能命中。
  #
  # 注意：App.cpp 使用相对路径 "../include/Util.h" 包含 Util.h，这会绕过
  # _include_路径搜索。为了替换有问题的 COMPILE_TIME_RAND_STR 宏，我们
  # 使用 /FI 编译器选项强制在编译单元开头 include wrapper/Util.h，
  # 它会先 include 原始的 Util.h，然后 #undef 并重新定义宏。
  target_include_directories(Ling PUBLIC
    "${LING_DIR}"
    "${LING_DIR}/include"
    "${FLOWTARY_CPPWINRT_DIR}")
  target_link_libraries(Ling PUBLIC yoga d2d1 dwrite windowscodecs)
  target_compile_options(Ling PRIVATE
    /std:c++latest /Zc:preprocessor
    /FI${CMAKE_SOURCE_DIR}/cmake/wrapper/Util.h
  )
  target_compile_definitions(Ling PRIVATE NOMINMAX)
  set_target_properties(Ling PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

# ---- Clipper2（TinyOCR 内置）----
set(TYOCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/TinyOCR")
if(EXISTS "${TYOCR_DIR}/Clipper2/src")
  file(GLOB_RECURSE CLIPPER_SOURCES "${TYOCR_DIR}/Clipper2/src/*.cpp")
  add_library(Clipper2 STATIC ${CLIPPER_SOURCES})
  target_include_directories(Clipper2 PUBLIC "${TYOCR_DIR}/Clipper2/include")
  set_target_properties(Clipper2 PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

# =====================================================================
# OCR 链：TinyOCR → ImageReader
#   依赖 vcpkg 的 opencv(core+imgproc) 与 onnxruntime 静态库。
#   依赖未就绪时整体跳过，主构建不受影响（此时 OCR 沿用预编译 exe）。
# =====================================================================
set(FLOWTARY_OCR_READY FALSE)
if(FLOWTARY_VCPKG_STATIC AND TARGET Ling AND TARGET Clipper2)
  # x64-windows-static triplet 生成的 onnxruntime 静态库名为 onnxruntime_common.lib
  file(GLOB _ORT_LIB "${FLOWTARY_VCPKG_STATIC}/lib/onnxruntime_common.lib")
  file(GLOB _OCV_CORE "${FLOWTARY_VCPKG_STATIC}/lib/opencv_core*.lib")
  file(GLOB _OCV_IMGPROC "${FLOWTARY_VCPKG_STATIC}/lib/opencv_imgproc*.lib")
  if(_ORT_LIB AND _OCV_CORE AND _OCV_IMGPROC)
    set(FLOWTARY_OCR_READY TRUE)
  endif()
endif()

if(FLOWTARY_OCR_READY)
  message(STATUS "[vendor] OCR 依赖就绪: ${FLOWTARY_VCPKG_STATIC}")

  # ---- TinyOCR ----
  set(TYOCR_SOURCES
    "${TYOCR_DIR}/TinyOCR/src/cal_rec_boxes.cpp"
    "${TYOCR_DIR}/TinyOCR/src/ctc_decoder.cpp"
    "${TYOCR_DIR}/TinyOCR/src/db_post_process.cpp"
    "${TYOCR_DIR}/TinyOCR/src/image_utils.cpp"
    "${TYOCR_DIR}/TinyOCR/src/ort_session.cpp"
    "${TYOCR_DIR}/TinyOCR/src/text_detector.cpp"
    "${TYOCR_DIR}/TinyOCR/src/text_recognizer.cpp"
    "${TYOCR_DIR}/TinyOCR/src/tiny_ocr.cpp"
  )
  add_library(TinyOCR STATIC ${TYOCR_SOURCES})
  target_include_directories(TinyOCR PUBLIC
    "${TYOCR_DIR}"
    "${TYOCR_DIR}/TinyOCR/include"
    "${FLOWTARY_VCPKG_STATIC}/include/opencv4"
    "${FLOWTARY_VCPKG_STATIC}/include/onnxruntime"
    "${FLOWTARY_VCPKG_STATIC}/include")
  target_link_libraries(TinyOCR PUBLIC Clipper2)
  set_target_properties(TinyOCR PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  # ---- ImageReader（OCR 独立进程）----
  set(IR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ImageReader")
  file(GLOB IR_SOURCES "${IR_DIR}/ImageReader/*.cpp")
  add_executable(ImageReader WIN32
    ${IR_SOURCES}
    "${IR_DIR}/ImageReader/Res/Resource.rc"
    "${CMAKE_SOURCE_DIR}/cmake/DpiAware.manifest")
  set_target_properties(ImageReader PROPERTIES
    OUTPUT_NAME ImageReader
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  # include 基准为 Ling 与 TinyOCR 两个仓库根（对齐上游 vcxproj）
  # 注意：wrapper 目录必须在 Ling include 目录之前，替换有问题的宏
  target_include_directories(ImageReader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/wrapper"
    "${LING_DIR}"
    "${TYOCR_DIR}")

  file(GLOB _OCV_3RD
    "${FLOWTARY_VCPKG_STATIC}/lib/zlib*.lib"
    "${FLOWTARY_VCPKG_STATIC}/lib/libpng*.lib"
    "${FLOWTARY_VCPKG_STATIC}/lib/jpeg*.lib"
    "${FLOWTARY_VCPKG_STATIC}/lib/turbojpeg*.lib"
    "${FLOWTARY_VCPKG_STATIC}/lib/ippiw*.lib"
    "${FLOWTARY_VCPKG_STATIC}/lib/ippicv*.lib"
    "${FLOWTARY_VCPKG_STATIC}/lib/ittnotify*.lib")

  # 链接所有 vcpkg 静态库（包括 abseil、re2、flatbuffers、protobuf、zlib 等）
  file(GLOB _ALL_VCPKG_LIBS "${FLOWTARY_VCPKG_STATIC}/lib/*.lib")

  target_link_libraries(ImageReader PRIVATE
    Ling yoga TinyOCR Clipper2
    ${_ALL_VCPKG_LIBS}
    WindowsApp runtimeobject d3d11 d2d1 dwrite
    user32 shcore dwmapi windowscodecs shell32 ole32
    version.lib)
else()
  message(STATUS "[vendor] OCR 依赖未就绪（缺少 opencv/onnxruntime 静态库），"
                 "跳过 TinyOCR/ImageReader 源码构建，沿用预编译 ImageReader.exe")
endif()

foreach(_t yoga Ling Clipper2 TinyOCR)
  if(TARGET ${_t})
    set_target_properties(${_t} PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endforeach()
