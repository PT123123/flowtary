# OCR 链源码构建踩坑记录

## 背景

将 OCR 链（Ling → yoga → Clipper2 → TinyOCR → ImageReader）从预编译黑盒改为源码引入，
使用 CMake + ninja 生成器构建。

## 踩坑 1：MSVC 标准库头文件找不到

### 症状

```
yoga/YGConfig.h(10): fatal error C1083: 无法打开头文件: "stdarg.h"
Clipper2/clipper.core.h(14): fatal error C1083: 无法打开头文件: "cstdint"
```

### 根因

CMake + ninja 生成器不会自动把 MSVC 的 `VC/Tools/MSVC/<ver>/include/` 目录传给 CL。

### 解决

创建 `cmake/msvc-include.cmake` 工具链文件，在配置阶段自动检测并注入：

```cmake
# 从 CMAKE_CXX_COMPILER 反推 MSVC 根目录
# 编译器路径形如 .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe
# 取其父目录的父目录的父目录的父目录 = VC/Tools/MSVC/<ver>/
get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" ABSOLUTE)
set(_n 0)
while(_n LESS 4)
  get_filename_component(_cl_dir "${_cl_dir}" DIRECTORY)
  math(EXPR _n "${_n} + 1")
endwhile()
include_directories("${_cl_dir}/include")
```

## 踩坑 2：Windows SDK 头文件找不到

### 症状

```
windows.h(171): fatal error C1083: 无法打开头文件: "excpt.h"
commctrl.h(1733): error C3646: "hdr": 未知重写说明符
```

### 根因

ninja 生成器不会自动添加 Windows SDK include 目录。

### 解决

从注册表读取 `KitsRoot10`，枚举版本号，注入 `um`、`ucrt`、`shared`、`winrt` 四个子目录：

```cmake
get_filename_component(_kits_root
  "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
  ABSOLUTE)
file(GLOB _sdk_vers RELATIVE "${_kits_root}/Include" "${_kits_root}/Include/10.0.*")
list(SORT _sdk_vers)
list(REVERSE _sdk_vers)
list(GET _sdk_vers 0 _sdk_ver)
foreach(_subdir IN ITEMS um ucrt shared winrt)
  include_directories("${_kits_root}/Include/${_sdk_ver}/${_subdir}")
endforeach()
```

## 踩坑 3：Windows SDK 库文件找不到

### 症状

```
LINK : fatal error LNK1181: 无法打开文件 "comctl32.lib"
LINK : fatal error LNK1104: 无法打开文件 "libcpmt.lib"
```

### 根因

ninja 生成器不会自动添加 MSVC 和 Windows SDK 的库文件搜索路径。

### 解决

在 `cmake/msvc-include.cmake` 中添加 `link_directories()` 调用：

```cmake
# MSVC 库目录
link_directories("${_cl_dir}/lib/x64")

# Windows SDK 库目录
foreach(_subdir IN ITEMS um/x64 ucrt/x64 um/x86 ucrt/x86)
  link_directories("${_kits_root}/Lib/${_sdk_lib_ver}/${_subdir}")
endforeach()
```

## 踩坑 4：MSVC 不完全支持 C++20 template lambda

### 症状

```
C:\Users\ted\Desktop\flowtary\vendor\Ling\src\App.cpp(12): error C3493:
无法将 "chars" 作为未指定默认参数的模板模式
```

### 根因

Ling 的 `Util.h` 中 `COMPILE_TIME_RAND_STR` 宏使用了 C++20 template lambda
（`[]<size_t... I>`），MSVC 19.44 不完全支持。

### 解决

**方案 B（wrapper header）**：创建 `cmake/wrapper/Util.h`，通过 `/FI` 编译器选项强制在
每个编译单元开头 include，替换有问题的宏：

```cmake
target_compile_options(Ling PRIVATE
  /FI${CMAKE_SOURCE_DIR}/cmake/wrapper/Util.h
)
```

wrapper 头文件内容：

```cpp
#include "../../vendor/Ling/include/Util.h"
#undef COMPILE_TIME_RAND_STR
#define COMPILE_TIME_RAND_STR(LEN) \
    []() -> std::wstring { \
        /* 运行时生成随机字符串，功能等价 */ \
    }()
```

**注意**：`App.cpp` 使用相对路径 `"../include/Util.h"` 包含 Util.h，这会绕过 include
路径搜索。因此不能使用 include 路径顺序来替换，必须使用 `/FI` 强制 include。

## 踩坑 5：vcpkg OpenCV 头文件路径

### 症状

```
TinyOCR/TinyOCR/src/cal_rec_boxes.h(9): fatal error C1083: 无法打开头文件: "opencv2/core.hpp"
```

### 根因

vcpkg 的 OpenCV 头文件在 `include/opencv4/opencv2/` 下，而不是 `include/opencv2/`。

### 解决

添加 `include/opencv4` 到 include 路径：

```cmake
target_include_directories(TinyOCR PUBLIC
  "${FLOWTARY_VCPKG_STATIC}/include/opencv4"
  "${FLOWTARY_VCPKG_STATIC}/include"
)
```

## 踩坑 6：vcpkg onnxruntime 头文件路径

### 症状

```
TinyOCR/TinyOCR/src/ort_session.h(11): fatal error C1083: 无法打开头文件: "onnxruntime_cxx_api.h"
```

### 根因

vcpkg 的 onnxruntime 头文件在 `include/onnxruntime/` 下，源文件直接包含
`onnxruntime_cxx_api.h` 而不带 `onnxruntime/` 前缀。

### 解决

添加 `include/onnxruntime` 到 include 路径：

```cmake
target_include_directories(TinyOCR PUBLIC
  "${FLOWTARY_VCPKG_STATIC}/include/onnxruntime"
  "${FLOWTARY_VCPKG_STATIC}/include"
)
```

## 踩坑 7：vcpkg 静态库依赖复杂

### 症状

```
LNK2001: 无法解析外部符号 "void __cdecl absl::..."
LNK2001: 无法解析外部符号 gzputs / gzgets / gzopen / ...
```

### 根因

onnxruntime 依赖 abseil、re2、flatbuffers、protobuf、zlib 等，手动列举容易遗漏。

### 解决

最简单的做法是链接 vcpkg lib 目录下的所有 `.lib` 文件：

```cmake
file(GLOB _ALL_VCPKG_LIBS "${FLOWTARY_VCPKG_STATIC}/lib/*.lib")
target_link_libraries(ImageReader PRIVATE
  Ling yoga TinyOCR Clipper2
  ${_ALL_VCPKG_LIBS}
  ...
)
```

## 踩坑 8：CRT 一致性

### 症状

```
LNK2038: 检测到 "RuntimeLibrary" 的不匹配: 值 "MD_DynamicRelease" 不匹配 "MT_StaticRelease"
```

### 根因

vcpkg `x64-windows-static` 构建的库是 `/MT`，但 CMake 默认可能是 `/MD`。

### 解决

为 OCR 链 target 显式设置 `MSVC_RUNTIME_LIBRARY`：

```cmake
foreach(_t yoga Ling Clipper2 TinyOCR)
  if(TARGET ${_t})
    set_target_properties(${_t} PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endforeach()

# ImageReader 也需要设置
set_target_properties(ImageReader PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
```

## 经验总结

1. **ninja + MSVC 需要手动注入路径**：include 目录、库目录都需要手动检测并注入
2. **vcpkg 路径约定**：头文件可能在 `include/<package>/` 下，需要分别添加
   `include/<package>` 和 `include` 到搜索路径
3. **MSVC 对 C++20 支持不完整**：template lambda 等特性可能需要 workaround
4. **静态库依赖传递**：最简单的做法是链接所有 `.lib`，避免遗漏
5. **CRT 一致性**：使用 vcpkg static triplet 时，所有 target 必须统一为 `/MT`
