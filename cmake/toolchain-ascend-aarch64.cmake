#=============================================================================
# toolchain-ascend-aarch64.cmake
# 昇腾（Ascend / CANN）aarch64 交叉编译工具链
#
# 用法：
#   cmake --preset ascend-aarch64
# 或：
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ascend-aarch64.cmake -S . -B build/ascend
#
# 说明：本机（x86_64）上的昇腾环境是 CANN 的 aarch64-linux 版本，宿主编译器
#       （/usr/bin/c++）无法链接它，因此这里选择 aarch64 交叉编译器；
#       CANN 的路径由 cmake/FindAscend.cmake 负责查找。
#=============================================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 交叉编译器前缀，可用 -DASCEND_CROSS_PREFIX=... 覆盖
set(ASCEND_CROSS_PREFIX "aarch64-linux-gnu-" CACHE STRING "aarch64 交叉编译器前缀")

find_program(_ascend_c_compiler   NAMES ${ASCEND_CROSS_PREFIX}gcc-11 ${ASCEND_CROSS_PREFIX}gcc REQUIRED)
find_program(_ascend_cxx_compiler NAMES ${ASCEND_CROSS_PREFIX}g++-11 ${ASCEND_CROSS_PREFIX}g++ REQUIRED)
find_program(_ascend_ar           NAMES ${ASCEND_CROSS_PREFIX}ar     REQUIRED)
find_program(_ascend_ranlib       NAMES ${ASCEND_CROSS_PREFIX}ranlib REQUIRED)

set(CMAKE_C_COMPILER   "${_ascend_c_compiler}")
set(CMAKE_CXX_COMPILER "${_ascend_cxx_compiler}")
set(CMAKE_AR           "${_ascend_ar}")
set(CMAKE_RANLIB       "${_ascend_ranlib}")

# 目标系统根目录：Ubuntu 多架构布局（/usr/aarch64-linux-gnu、/usr/lib/aarch64-linux-gnu）。
# 交叉 gcc 自带该 sysroot 作为默认搜索路径，这里再加入 CMAKE_FIND_ROOT_PATH 并设为
# ONLY 模式，避免 find_package/find_library 误用宿主机的 x86_64 头文件与库。
set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 运行测试程序需要 aarch64 模拟器（本机不一定安装）；本工程没有编译期自测，保持默认即可。
# set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-L;/usr/aarch64-linux-gnu)
