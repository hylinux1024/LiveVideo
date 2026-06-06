cmake_minimum_required(VERSION 3.22.1)
project("dronemedia")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# libsrt: SRT 协议库, 作为子项目一起编译 (静态库)
# ============================================================
# Android NDK 默认 -fno-exceptions / -fno-rtti, libsrt 用了 throw + typeid,
# 必须在 add_subdirectory 之前改掉 CXX flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti")

set(ENABLE_UNITTESTS  OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(ENABLE_APPS       OFF CACHE BOOL "" FORCE)
set(ENABLE_CXX11      ON  CACHE BOOL "" FORCE)
set(ENABLE_SHARED     OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC     ON  CACHE BOOL "" FORCE)
set(USE_STATIC_LIBS   ON  CACHE BOOL "" FORCE)
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)  # 关闭加密避免拉 OpenSSL 依赖
set(SRT_DEBUG          OFF CACHE BOOL "" FORCE)
add_subdirectory(srt)

# ============================================================
# 主引擎
# ============================================================
add_library(dronemedia SHARED
            DroneMediaEngine.cpp
            SrtListener.cpp)

target_compile_options(dronemedia PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections)

target_link_options(dronemedia PRIVATE
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL)

target_include_directories(dronemedia PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/srt
    ${CMAKE_CURRENT_SOURCE_DIR}/srt/srtcore  # libsrt 把 srt.h 放在 srtcore/ 下
    ${CMAKE_CURRENT_SOURCE_DIR}/srt/haicrypt  # libsrt 加密库头文件
    ${CMAKE_CURRENT_SOURCE_DIR}/srt/common   # libsrt 通用工具头文件
    ${CMAKE_CURRENT_BINARY_DIR}/srt)         # libsrt 生成的 version.h 在这里

# libsrt 自带的 pthread 依赖, Android NDK 已经提供
find_library(log-lib log)
target_link_libraries(dronemedia
    android
    ${log-lib}
    mediandk
    EGL
    GLESv2
    srt_static)   # libsrt 静态库目标名 (CMake 目标, 实际产物 libsrt.a)
