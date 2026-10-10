# 3.18：cmake/toolchain-ascend-aarch64.cmake 用到 find_program(... REQUIRED)；
# CMakePresets.json 需要 3.21（本机 cmake 3.22）
cmake_minimum_required(VERSION 3.18)

project(UAV_detection LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -Wall -Wextra")

# 不指定构建类型时 CMAKE_CXX_FLAGS_DEBUG/RELEASE 都不会生效（-Wall -Wextra 随之失效），
# 因此显式给一个默认值，保证警告始终可见
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug/Release)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()


set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)


list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost)
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl)

find_package(Boost 1.88 REQUIRED COMPONENTS thread filesystem system)
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# libxml2（GB28181 / ONVIF 的 XML 报文字段解析）
#
# 默认用随仓库提交的 aarch64 静态库（third_party/libxml2，2.9.13，与 boost/openssl
# 的约定一致），由 third_party/libxml2/build-aarch64.sh 交叉编译生成：
#   third_party/libxml2/include/libxml2/libxml/*.h   → #include <libxml/parser.h>
#   third_party/libxml2/lib/libxml2.a
#
# 这里有两个必须显式处理的坑：
#   1) CMake 自带的 FindLibXml2 会先拿 pkg-config 的提示路径，交叉编译时可能命中
#      宿主机的 x86_64 版本（/usr/include/libxml2 + /usr/lib/x86_64-linux-gnu/libxml2.so，
#      头文件与库架构不一致），必须把 third_party 顶到搜索顺序最前面；
#   2) 交叉工具链把 CMAKE_FIND_ROOT_PATH_MODE_* 设成了 ONLY，只有 CMAKE_FIND_ROOT_PATH
#      下的路径会被搜索——只设 LibXml2_ROOT 是找不到的（实测报 Could NOT find LibXml2），
#      所以前缀必须同时作为 root path 追加进去。
#
# 想改用系统安装的版本（例如 x86_64 本机调试需要真正调用 libxml2 时）：
#   -DLibXml2_ROOT=/usr
# ---------------------------------------------------------------------------
set(LibXml2_ROOT "" CACHE PATH "libxml2 安装前缀；留空 = third_party/libxml2（aarch64 静态库）")
if(NOT LibXml2_ROOT)
  set(LibXml2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libxml2)
endif()
# FindLibXml2 把结果写进 cache，换了 LibXml2_ROOT 也不会重新搜索（老构建目录里切换前缀
# 会静默沿用旧路径），所以前缀变化时手动清掉这两个缓存项，强制重新查找。
set(UAV_LIBXML2_ROOT_USED "" CACHE INTERNAL "上次配置使用的 LibXml2_ROOT")
if(NOT UAV_LIBXML2_ROOT_USED STREQUAL LibXml2_ROOT)
  unset(LIBXML2_INCLUDE_DIR CACHE)
  unset(LIBXML2_LIBRARY CACHE)
  set(UAV_LIBXML2_ROOT_USED "${LibXml2_ROOT}" CACHE INTERNAL "上次配置使用的 LibXml2_ROOT" FORCE)
endif()
list(APPEND CMAKE_FIND_ROOT_PATH ${LibXml2_ROOT})
find_package(LibXml2 2.9 REQUIRED)
message(STATUS "libxml2: ${LIBXML2_VERSION_STRING}")
message(STATUS "  头文件: ${LIBXML2_INCLUDE_DIR}")
message(STATUS "  库文件: ${LIBXML2_LIBRARY}")

# 静态库的隐藏依赖：libxml2 内部用到 pow/fmod/log10（上游 libxml-2.0.pc 里的
# Libs.private: -lm），以及 xmlmodule 的 dlopen（glibc 2.34 起已并入 libc，带上更保险）。
# CMake 3.22 的 FindLibXml2 不处理这两项，挂在 imported target 上，使用方无需关心。
set_property(TARGET LibXml2::LibXml2 APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES m ${CMAKE_DL_LIBS})


# ---------------------------------------------------------------------------
# 昇腾（Ascend / CANN）编译环境
#
# 默认开启：直接使用“当前系统”已安装的 CANN（头文件 + libascendcl），架构必须与
# 本次编译的目标架构一致。本机的 CANN 是 aarch64 版本，因此请用交叉编译预设：
#
#   cmake --preset ascend-aarch64        # 昇腾 aarch64（CANN + aarch64-linux-gnu-*）
#   cmake --preset native                # 仅本机 x86_64，不含昇腾（UAV_ASCEND=OFF）
#
# 也可以手动指定 CANN 路径：-DAscend_ROOT=/usr/local/Ascend/ascend-toolkit/latest
# 完全不需要昇腾环境时：-DUAV_ASCEND=OFF
# ---------------------------------------------------------------------------
option(UAV_ASCEND "使用当前系统的昇腾（CANN/ACL）编译环境" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(UAV_ASCEND)
  find_package(Ascend REQUIRED)
  message(STATUS "昇腾编译环境: ${ASCEND_HOME} (架构目录 ${ASCEND_ARCH_DIR}, 版本 ${ASCEND_VERSION})")
  message(STATUS "  ACL 头文件: ${ASCEND_INCLUDE_DIR}")
  message(STATUS "  ACL 库    : ${ASCEND_ACL_LIBRARY}")
else()
  message(STATUS "昇腾编译环境: 未启用 (UAV_ASCEND=OFF)")
endif()



include_directories(${PROJECT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# GB28181 国标接入（可选）
#
# SIP 信令使用 eXosip2（third_party/libeXosip2 + libosip2 的 aarch64 静态库），
# 首次构建前需要先生成这两个库：
#   ./third_party/libosip2/build-aarch64.sh
#   ./third_party/libeXosip2/build-aarch64.sh
#
# 暂不做国标对接时可 -DUAV_GB28181=OFF 关掉（源码也不会参与编译）。
# ---------------------------------------------------------------------------
option(UAV_GB28181 "启用 GB28181 国标接入（XML 报文 + eXosip2 SIP 信令）" ON)

set(UAV_GB28181_SOURCES "")
if(UAV_GB28181)
  find_package(EXosip2 REQUIRED)   # 见 cmake/FindEXosip2.cmake
  message(STATUS "GB28181 SIP 栈: ${EXOSIP2_LIBRARY}")
  message(STATUS "  依赖 libosip2: ${OSIP2_LIBRARIES}")
  set(UAV_GB28181_SOURCES
    src/gb28181/xmlpacket.cpp
    src/gb28181/xmlpacketparser.cpp
    src/gb28181/sipsession.cpp
    src/gb28181/ps_mux.cpp
    src/gb28181/ps_rtp_pusher.cpp
    src/gb28181/gb28181_media.cpp
    src/gb28181/gb28181device.cpp)
endif()

# 注意：新增模块的 .cpp 必须显式列在这里（旧版本漏掉整个 log 模块，
# 导致日志代码从未被编译、里面的编译错误一直没被发现）
set(UAV_SOURCES
  src/main.cpp
  src/log/log.cpp
  src/log/RotateFileWriter.cpp
  src/rtsp/rtspserver.cpp
  src/rtsp/rtp_source.cpp
  src/rtsp/rtp_transport.cpp
  src/rtsp/file_source.cpp
  ${UAV_GB28181_SOURCES}
)

add_executable(uav_detection
  ${UAV_SOURCES}
)


target_include_directories(uav_detection PRIVATE src)
target_link_libraries(uav_detection PRIVATE
  Boost::headers                 # header-only（asio/beast/json…）
  Boost::thread
  Boost::system
  OpenSSL::SSL OpenSSL::Crypto
  LibXml2::LibXml2)              # XML 解析（GB28181/ONVIF），见上方 third_party/libxml2
# 日志模块已改用 std::filesystem（C++17 标准库），不再需要 Boost::filesystem；
# 其他模块若要使用 Boost.Filesystem，在此处加回 Boost::filesystem 即可。

if(UAV_GB28181)
  # SIP 信令栈（eXosip2 + osip2 + pthread，传递依赖都在导入目标里带好）
  target_link_libraries(uav_detection PRIVATE EXosip2::EXosip2)
endif()

if(UAV_ASCEND)
  # ACL 运行时（acl/*.h + libascendcl）。当前代码尚未调用 ACL 符号，链接器
  # （--as-needed）不会把它写进 NEEDED；接入 detect 等模块后自动成为真实依赖。
  # 板卡上运行时需要先 source CANN 的 setenv.bash（或用 LD_LIBRARY_PATH）加载 libascendcl。
  target_link_libraries(uav_detection PRIVATE Ascend::ascendcl)
endif()