#=============================================================================
# FindUSRSCTP.cmake —— 定位 usrsctp（WebRTC 的 SCTP over DTLS / DataChannel）
#
# 用户态 SCTP 协议栈：AF_CONN 把 SCTP 报文交给 DTLS 传输，而不是走内核 SCTP socket。
#
# 查找顺序（先命中者优先）：
#   1) -DUSRSCTP_ROOT=/path/to/usrsctp
#   2) 环境变量 $USRSCTP_ROOT
#   3) 仓库内 third_party/usrsctp（默认，交叉编译 aarch64 静态库）
#   4) 系统安装位置（/usr、/usr/local，便于宿主机原生调试）
#
# 注意 1：第 3 项对**所有目标架构**都参与搜索（与 FindSRTP.cmake / FindEXosip2.cmake
#   的约定一致）。此前把它限制在 aarch64 下，宿主机（x86_64）时搜索列表为空，
#   独立测试工程（tests/webrtc，本机原生编译）在 configure 阶段就以
#   "Could NOT find USRSCTP" 中断——仓库里明明有库却用不上。
# 注意 2：third_party 下是 aarch64 静态库，宿主机 x86_64 测试**真正链接**时仍必须用
#   -DUSRSCTP_ROOT=<同架构前缀> 覆盖（可参考 docs/datachannel.md 自行构建一份），
#   否则会落到"库架构不匹配"的链接错误上。第 1/2 项的优先级高于 third_party。
#   （主工程不走这条路：CMakeLists.txt 里已强制交叉编译，宿主机原生配置直接报错退出。）
# 注意 3：这里用 HINTS 而不是 PATHS：CMake 的搜索顺序里 HINTS（第 4 步）在
#   /usr、/usr/local 等平台路径（第 6 步）之前，PATHS 反而排在最后。用 PATHS 的话，
#   宿主机一旦装了 usrsctp 开发包，板端构建会优先命中宿主机的 x86_64 库。
# 注意 4：与 FindEXosip2.cmake 同理：交叉编译时 CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
#   会给所有搜索路径加 sysroot 前缀而找不到，因此统一使用 NO_CMAKE_FIND_ROOT_PATH。
#
# 输出变量：
#   USRSCTP_FOUND          是否找到
#   USRSCTP_INCLUDE_DIR    usrsctp.h 所在目录（#include <usrsctp.h>）
#   USRSCTP_LIBRARY        libusrsctp.a / libusrsctp.so 的绝对路径
#
# 导入目标：
#   USRSCTP::USRSCTP       头文件 + 库 + pthread（栈内部自带收发/定时线程）
#=============================================================================

find_package(Threads REQUIRED)   # usrsctp 内部使用 pthread 收发线程与定时器

# ---- 1. 候选根目录 ----------------------------------------------------------
set(_usrsctp_root_candidates "")

if(USRSCTP_ROOT)
  list(APPEND _usrsctp_root_candidates "${USRSCTP_ROOT}")
endif()
if(DEFINED ENV{USRSCTP_ROOT} AND NOT "$ENV{USRSCTP_ROOT}" STREQUAL "")
  list(APPEND _usrsctp_root_candidates "$ENV{USRSCTP_ROOT}")
endif()
# 仓库内默认前缀。这里用模块自身的相对路径（cmake/../third_party/usrsctp）而不是
# PROJECT_SOURCE_DIR：独立测试工程（tests/webrtc）也直接引用了这个模块，那时
# PROJECT_SOURCE_DIR 指向 tests/webrtc 而不是仓库根目录。
list(APPEND _usrsctp_root_candidates "${CMAKE_CURRENT_LIST_DIR}/../third_party/usrsctp")

list(REMOVE_DUPLICATES _usrsctp_root_candidates)
list(GET _usrsctp_root_candidates 0 _usrsctp_primary_root)

# find_path/find_library 的结果会写进 cache，换了 USRSCTP_ROOT 也不会重新搜索（老构建
# 目录里切换前缀会静默沿用旧路径），所以前缀变化时手动清掉缓存项，强制重新查找。
set(UAV_USRSCTP_ROOT_USED "" CACHE INTERNAL "上次查找使用的 USRSCTP_ROOT")
if(NOT UAV_USRSCTP_ROOT_USED STREQUAL _usrsctp_primary_root)
  unset(USRSCTP_INCLUDE_DIR CACHE)
  unset(USRSCTP_LIBRARY CACHE)
  set(UAV_USRSCTP_ROOT_USED "${_usrsctp_primary_root}" CACHE INTERNAL "上次查找使用的 USRSCTP_ROOT" FORCE)
endif()

# ---- 2. 头文件 --------------------------------------------------------------
find_path(USRSCTP_INCLUDE_DIR
  NAMES usrsctp.h
  HINTS ${_usrsctp_root_candidates}
  PATH_SUFFIXES include
  NO_CMAKE_FIND_ROOT_PATH)

# ---- 3. 库 ------------------------------------------------------------------
find_library(USRSCTP_LIBRARY
  NAMES usrsctp libusrsctp
  HINTS ${_usrsctp_root_candidates}
  PATH_SUFFIXES lib lib64
  NO_CMAKE_FIND_ROOT_PATH)

# ---- 4. 未找到时的提示 ------------------------------------------------------
if(NOT USRSCTP_INCLUDE_DIR OR NOT USRSCTP_LIBRARY)
  string(CONCAT _usrsctp_hint
    "未找到可用的 usrsctp。\n"
    "仓库内的 aarch64 静态库需要先交叉编译生成：\n"
    "    ./third_party/usrsctp/build-aarch64.sh\n"
    "或指定已有安装前缀：-DUSRSCTP_ROOT=/path/to/usrsctp\n"
    "（宿主机 x86_64 上跑独立测试需要同架构的库，构建方法见 docs/datachannel.md）")
endif()

# ---- 5. 标准 find_package 收尾 ----------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(USRSCTP
  REQUIRED_VARS USRSCTP_INCLUDE_DIR USRSCTP_LIBRARY
  FAIL_MESSAGE  "${_usrsctp_hint}")

mark_as_advanced(USRSCTP_INCLUDE_DIR USRSCTP_LIBRARY)

# ---- 6. 导入目标 ------------------------------------------------------------
if(USRSCTP_FOUND AND NOT TARGET USRSCTP::USRSCTP)
  # UNKNOWN 而不是 STATIC：宿主机调试时找到的可能是 .so，UNKNOWN + 绝对路径对两者都成立
  add_library(USRSCTP::USRSCTP UNKNOWN IMPORTED)
  set_target_properties(USRSCTP::USRSCTP PROPERTIES
    IMPORTED_LOCATION             "${USRSCTP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${USRSCTP_INCLUDE_DIR}"
    # 静态库的传递依赖：pthread（usrsctp 不用 libm/dl，上游 usrsctp.pc 里也没有额外 Libs）
    INTERFACE_LINK_LIBRARIES      "Threads::Threads")
endif()
