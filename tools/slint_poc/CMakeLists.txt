cmake_minimum_required(VERSION 3.21)
project(flowtary_slint_poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 用解包的预编译 Slint C++ SDK（免 Rust 源码编译）
set(SLINT_SDK "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
list(APPEND CMAKE_PREFIX_PATH "${SLINT_SDK}")
find_package(Slint 1.19 REQUIRED COMPONENTS Compiler)

add_executable(flowtary_poc main.cpp)
slint_target_sources(flowtary_poc main.slint)
target_link_libraries(flowtary_poc PRIVATE Slint::Slint)

# 复制 Slint 运行时 DLL 到产物目录
add_custom_command(TARGET flowtary_poc POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SLINT_SDK}/lib/slint_cpp.dll" $<TARGET_FILE_DIR:flowtary_poc>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper.png" $<TARGET_FILE_DIR:flowtary_poc>
)
