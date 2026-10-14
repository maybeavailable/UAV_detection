#=============================================================================
# FindSRTP.cmake —— 定位 libsrtp2（WebRTC 的 DTLS-SRTP 媒体加密）
#
# 需要的只有 libsrtp2 一个库，但它是“带 OpenSSL 后端”编出来的静态库，链接期还
# 需要 libcrypto（third_party/openssl）与 pthread/dl，见下面的导入目标。
#
# 查找顺序（先命中者优先）：
#   1) -DSRTP_ROOT=/path/to/libsrtp
#   2) 环境变量 $SRTP_ROOT
#   3) 仓库内 third_party/libsrtp（默认，交叉编译 aarch64 静态库）
#   4) 系统安装位置（/usr、/usr/local，便于宿主机原生调试）
#
# 注意：这个库在 third_party 下是“目标架构”的静态库，但位于宿主机路径上。
# 交叉编译时 CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY 会给所有搜索路径加上 sysroot
# 前缀，导致找不到，因此这里统一使用 NO_CMAKE_FIND_ROOT_PATH（与 FindEXosip2.cmake
# 同理）。
#
# 输出变量：
#   SRTP_FOUND          是否找到
#   SRTP_INCLUDE_DIR    srtp2/ 的上一级目录（#include <srtp2/srtp.h>）
#   SRTP_LIBRARY        libsrtp2.a 的绝对路径
#   SRTP_VERSION        从 libsrtp2.pc 读到的版本号（找不到 .pc 时为空）
#
# 导入目标：
#   SRTP::SRTP          头文件 + 静态库 + OpenSSL/pthread/dl 传递依赖
#                       （上游自带的 CMake 包把同样的东西导出成 libSRTP::srtp2）
#=============================================================================

find_package(OpenSSL REQUIRED)   # 加密后端：libsrtp 的 AES-GCM/AES-CM 都走 libcrypto
find_package(Threads REQUIRED)

# ---- 1. 候选根目录 ----------------------------------------------------------
set(_srtp_root_candidates "")

if(SRTP_ROOT)
  list(APPEND _srtp_root_candidates "${SRTP_ROOT}")
endif()
if(DEFINED ENV{SRTP_ROOT} AND NOT "$ENV{SRTP_ROOT}" STREQUAL "")
  list(APPEND _srtp_root_candidates "$ENV{SRTP_ROOT}")
endif()
# 仓库内默认前缀：与 third_party/openssl、libxml2、usrsctp 的约定一致。
# 用模块自身的相对路径（cmake/../third_party/libsrtp）而不是 PROJECT_SOURCE_DIR：
# 独立测试工程（tests/webrtc）也引用了这个模块，那时 PROJECT_SOURCE_DIR 指向
# tests/webrtc，默认前缀会落空。
list(APPEND _srtp_root_candidates "${CMAKE_CURRENT_LIST_DIR}/../third_party/libsrtp")
# 系统安装位置交给 find_path/find_library 的默认搜索路径（候选为空时它们照样会找）

list(REMOVE_DUPLICATES _srtp_root_candidates)
list(GET _srtp_root_candidates 0 _srtp_primary_root)

# find_path/find_library 的结果会写进 cache，换了 SRTP_ROOT 也不会重新搜索（老构建
# 目录里切换前缀会静默沿用旧路径），所以前缀变化时手动清掉缓存项，强制重新查找。
# （与根 CMakeLists.txt 里处理 LibXml2_ROOT 是同一个坑）
set(UAV_SRTP_ROOT_USED "" CACHE INTERNAL "上次查找使用的 SRTP_ROOT")
if(NOT UAV_SRTP_ROOT_USED STREQUAL _srtp_primary_root)
  unset(SRTP_INCLUDE_DIR CACHE)
  unset(SRTP_LIBRARY CACHE)
  set(UAV_SRTP_ROOT_USED "${_srtp_primary_root}" CACHE INTERNAL "上次查找使用的 SRTP_ROOT" FORCE)
endif()

# ---- 2. 头文件 --------------------------------------------------------------
# 用 HINTS 而不是 PATHS：CMake 的搜索顺序里 HINTS（第 4 步）在 /usr、/usr/local 等
# 平台路径（第 6 步）之前，而 PATHS 排在最后。若用 PATHS，宿主机一旦 apt 装了
# libsrtp2-dev（上面提示里推荐的宿主调试方式），板端构建就会优先命中宿主机的
# x86_64 libsrtp2，报"架构不匹配"而不是用仓库里那份 aarch64 库。
find_path(SRTP_INCLUDE_DIR
  NAMES srtp2/srtp.h
  HINTS ${_srtp_root_candidates}
  PATH_SUFFIXES include
  NO_CMAKE_FIND_ROOT_PATH)

# ---- 3. 静态库 --------------------------------------------------------------
# 先按候选根目录找（SRTP_ROOT/环境变量优先，其次 third_party/libsrtp）；
# 都没命中再退回系统搜索路径（宿主机原生调试场景）。
# 上游的库名是 libsrtp2.a：pkg-config 名字 libsrtp2，SDP 里的 profile 才叫 SRTP_AES128_CM_SHA1_80。
find_library(SRTP_LIBRARY
  NAMES srtp2 libsrtp2
  HINTS ${_srtp_root_candidates}
  PATH_SUFFIXES lib lib64
  NO_CMAKE_FIND_ROOT_PATH)

# ---- 4. 未找到时的提示 ------------------------------------------------------
if(NOT SRTP_INCLUDE_DIR OR NOT SRTP_LIBRARY)
  string(CONCAT _srtp_hint
    "未找到可用的 libsrtp2。\n"
    "仓库内的 aarch64 静态库需要先交叉编译生成（依赖 third_party/openssl）：\n"
    "    ./third_party/libsrtp/build-aarch64.sh\n"
    "或指定已有安装前缀：-DSRTP_ROOT=/path/to/libsrtp\n"
    "（宿主机 x86_64 调试需要系统版本时：先 apt install libsrtp2-dev，再用 -DSRTP_ROOT=/usr）")
endif()

# ---- 5. 标准 find_package 收尾 ----------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SRTP
  REQUIRED_VARS SRTP_INCLUDE_DIR SRTP_LIBRARY
  FAIL_MESSAGE  "${_srtp_hint}")

# 顺带读出 libsrtp2.pc 里的版本号（上游没有把头文件版本号做成宏，
# 运行时要用 srtp_get_version_string() 才拿得到）
set(SRTP_VERSION "")
foreach(_srtp_root IN LISTS _srtp_root_candidates)
  if(EXISTS "${_srtp_root}/lib/pkgconfig/libsrtp2.pc")
    file(STRINGS "${_srtp_root}/lib/pkgconfig/libsrtp2.pc" _srtp_pc_version
      REGEX "^Version:[ \t]+[0-9]")
    string(REGEX REPLACE "^Version:[ \t]+" "" SRTP_VERSION "${_srtp_pc_version}")
    break()
  endif()
endforeach()

mark_as_advanced(SRTP_INCLUDE_DIR SRTP_LIBRARY)

# ---- 6. 导入目标 ------------------------------------------------------------
if(SRTP_FOUND AND NOT TARGET SRTP::SRTP)
  add_library(SRTP::SRTP STATIC IMPORTED)
  set_target_properties(SRTP::SRTP PROPERTIES
    IMPORTED_LOCATION             "${SRTP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SRTP_INCLUDE_DIR}"
    # 静态库的传递依赖：OpenSSL 加密后端（libsrtp 只用到 libcrypto，不用 libssl）、
    # 线程库，以及 libcrypto 静态库自身的 -ldl（glibc 2.34 起已并入 libc，
    # 带上更保险，与根 CMakeLists.txt 里给 LibXml2 处理的方式一致）。
    INTERFACE_LINK_LIBRARIES      "OpenSSL::Crypto;Threads::Threads;${CMAKE_DL_LIBS}")
endif()
