cmake_minimum_required(VERSION 3.21)
project(flowtary_slint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Slint C++ SDK（预编译，位于 tools/slint_poc/sdk）
set(SLINT_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/slint_poc/sdk")
list(APPEND CMAKE_PREFIX_PATH "${SLINT_SDK}")
find_package(Slint 1.19 REQUIRED COMPONENTS Compiler)

add_executable(flowtary-slint main.cpp)
if(MSVC)
  target_compile_options(flowtary-slint PRIVATE /utf-8 /W3)
endif()
target_include_directories(flowtary-slint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../)
slint_target_sources(flowtary-slint main.slint)
target_link_libraries(flowtary-slint PRIVATE Slint::Slint user32 gdi32 shell32 ole32 uuid)

# 设置窗：数据层与 IPC 复用主程序的 settings_model / settings_ipc
# 产物名固定 flowtary-settings.exe（主进程只认这个文件名）
add_executable(flowtary-settings WIN32
  settings_main.cpp
  ../settings_model.cpp
  ../search_history.cpp)
if(MSVC)
  target_compile_options(flowtary-settings PRIVATE /utf-8 /W3)
endif()
target_include_directories(flowtary-settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
slint_target_sources(flowtary-settings settings.slint)
target_link_libraries(flowtary-settings PRIVATE
  Slint::Slint user32 gdi32 shell32 ole32 uuid dwmapi advapi32 urlmon)

# 部署时把 Slint 运行时 DLL 一起拷到产物目录
add_custom_command(TARGET flowtary-slint POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SLINT_SDK}/lib/slint_cpp.dll" $<TARGET_FILE_DIR:flowtary-slint>
)
add_custom_command(TARGET flowtary-settings POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SLINT_SDK}/lib/slint_cpp.dll" $<TARGET_FILE_DIR:flowtary-settings>
)
