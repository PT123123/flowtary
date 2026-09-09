# =====================================================================
# MSVC include 路径自动检测
#
# 问题：CMake + ninja 生成器不会自动把 MSVC 和 Windows SDK 的
# include 目录传给 CL，导致 <stdarg.h>、<cstdint>、<windows.h> 等
# 头文件找不到。
#
# 解决：在配置阶段自动检测这些目录，并通过 include_directories()
# 注入到所有 target。
# =====================================================================

if(NOT MSVC)
  return()
endif()

message(STATUS "[msvc-include] Detected MSVC, adding include paths...")

# ---------------------------------------------------------------------
# 1. MSVC include 目录（<stdarg.h>、<cstdint>、<type_traits> 等）
#    编译器路径形如 .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe
#    取其父目录的父目录的父目录的父目录 = VC/Tools/MSVC/<ver>/
# ---------------------------------------------------------------------
get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" ABSOLUTE)
set(_n 0)
while(_n LESS 4)
  get_filename_component(_cl_dir "${_cl_dir}" DIRECTORY)
  math(EXPR _n "${_n} + 1")
endwhile()
if(EXISTS "${_cl_dir}/include")
  include_directories("${_cl_dir}/include")
  message(STATUS "[msvc-include] MSVC: ${_cl_dir}/include")
endif()

# ---------------------------------------------------------------------
# 2. Windows SDK include 目录（<windows.h>、<commctrl.h> 等）
#    从注册表读取 KitsRoot10，然后枚举版本号
# ---------------------------------------------------------------------
get_filename_component(_kits_root
  "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
  ABSOLUTE)
if(_kits_root AND EXISTS "${_kits_root}/Include")
  file(GLOB _sdk_vers RELATIVE "${_kits_root}/Include" "${_kits_root}/Include/10.0.*")
  list(SORT _sdk_vers)
  list(REVERSE _sdk_vers)                 # 版本号从新到旧
  list(GET _sdk_vers 0 _sdk_ver)          # 取最新版本
  if(_sdk_ver)
    foreach(_subdir IN ITEMS um ucrt shared winrt)
      if(EXISTS "${_kits_root}/Include/${_sdk_ver}/${_subdir}")
        include_directories("${_kits_root}/Include/${_sdk_ver}/${_subdir}")
        message(STATUS "[msvc-include] SDK ${_subdir}: ${_kits_root}/Include/${_sdk_ver}/${_subdir}")
      endif()
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------
# 3. Windows SDK / MSVC library 目录（WindowsApp.lib、libcpmt.lib 等）
#    注意：ninja 生成器不会自动添加 SDK/VC 库路径，需要手动添加。
#    库目录与目标架构一致：x86 子构建（FLOWTARY_ARCH=x86）若链到
#    x64 库，会报 LNK4272 和 __DllMainCRTStartup@12 等符号无法解析。
# ---------------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_lib_arch x64)
else()
  set(_lib_arch x86)
endif()

# MSVC 库目录
if(EXISTS "${_cl_dir}/lib/${_lib_arch}")
  link_directories("${_cl_dir}/lib/${_lib_arch}")
  message(STATUS "[msvc-include] MSVC lib: ${_cl_dir}/lib/${_lib_arch}")
endif()

# Windows SDK 库目录
if(_kits_root AND EXISTS "${_kits_root}/Lib")
  file(GLOB _sdk_lib_vers RELATIVE "${_kits_root}/Lib" "${_kits_root}/Lib/10.0.*")
  list(SORT _sdk_lib_vers)
  list(REVERSE _sdk_lib_vers)
  list(GET _sdk_lib_vers 0 _sdk_lib_ver)
  if(_sdk_lib_ver)
    foreach(_subdir IN ITEMS "um/${_lib_arch}" "ucrt/${_lib_arch}")
      if(EXISTS "${_kits_root}/Lib/${_sdk_lib_ver}/${_subdir}")
        link_directories("${_kits_root}/Lib/${_sdk_lib_ver}/${_subdir}")
        message(STATUS "[msvc-include] SDK lib ${_subdir}: ${_kits_root}/Lib/${_sdk_lib_ver}/${_subdir}")
      endif()
    endforeach()
  endif()
endif()
