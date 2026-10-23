# ============================================================
# 一个调度 · 双构建（host / device 同源）
#   host   ：WSL2 Ubuntu g++，跑模拟设备树测试
#   device ：NDK 交叉编译 aarch64-linux-android，静态 libc++
# 用法：
#   host    cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   device  cmake -S . -B build-device -G Ninja
#           -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake
#           -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
#           -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
# ============================================================
cmake_minimum_required(VERSION 3.20)
project(xindiaodu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT ANDROID)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# CONFIGURE_DEPENDS：新增 src/*.cpp 后自动重新 glob，无需手动清缓存
file(GLOB XDD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_executable(xindiaodud ${XDD_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xindiaodud PRIVATE -Wall -Wextra)
endif()

# 看门狗线程（pthread）：Android bionic 内置，host 需显式链接
find_package(Threads REQUIRED)
target_link_libraries(xindiaodud PRIVATE Threads::Threads)

if(ANDROID)
  # 设备构建：单二进制零依赖（ANDROID_STL=c++_static 由命令行传入）
  target_compile_definitions(xindiaodud PRIVATE XDD_DEVICE=1)
else()
  target_compile_definitions(xindiaodud PRIVATE XDD_HOST=1)
endif()
