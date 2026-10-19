# 3.18：cmake/toolchain-ascend-aarch64.cmake 用到 find_program(... REQUIRED)；
# CMakePresets.json 需要 3.21（本机 cmake 3.22）
cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# 默认指定昇腾（aarch64）交叉编译工具链
#
# uav_detection 是板端程序（见下面「强制交叉编译」一节），所以不要求每次都用
# --preset 或 -DCMAKE_TOOLCHAIN_FILE=... 传工具链：这里直接把
# cmake/toolchain-ascend-aarch64.cmake 指定成默认值，裸敲
#     cmake -S . -B build
# 就是昇腾 aarch64 交叉编译。
#
# 优先级（高 → 低）：
#   1) 显式指定：命令行 -DCMAKE_TOOLCHAIN_FILE=... 或预设里的 toolchainFile
#      （下面这个 set(... CACHE ...) 不带 FORCE，已有值不会被覆盖）
#   2) 本机子集：UAV_GATEWAY_ONLY / UAV_RTSP_ONLY —— 这两个模式是
#      gateway-native / rtsp-native 两个宿主机回归测试入口，故意不做交叉编译
#   3) 默认：cmake/toolchain-ascend-aarch64.cmake（昇腾 aarch64 + aarch64-linux-gnu-*）
#
# 注意：必须写在 project() 之前。CMake 只在第一次 project()/enable_language()
# 时加载工具链文件，放到 project() 之后就不生效了（编译器已经选定了）。
# ---------------------------------------------------------------------------
if(NOT CMAKE_TOOLCHAIN_FILE AND NOT UAV_GATEWAY_ONLY AND NOT UAV_RTSP_ONLY)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-ascend-aarch64.cmake"
      CACHE FILEPATH "交叉编译工具链文件（默认昇腾 aarch64；见 CMakeLists.txt 顶部说明）")
  message(STATUS "未指定工具链，默认使用昇腾 aarch64 交叉编译：cmake/toolchain-ascend-aarch64.cmake")
endif()

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

# 导出 compile_commands.json（写到各自的 build/<preset>/ 目录）：
# 编辑器的 C/C++ IntelliSense（VSCode cpptools / clangd）不读 CMakeLists，只认这个编译
# 数据库——没有它，第三方的 acl.h / hi_mpi_*.h 这些工程外的头文件一律标红波浪线。
# 见 .vscode/c_cpp_properties.json 与 .vscode/settings.json 里的指向。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(UAV_GATEWAY "构建 HTTP / WebSocket Gateway" ON)
option(UAV_GATEWAY_ONLY "仅构建 Gateway，不查找板端媒体和国标依赖" OFF)
option(UAV_GATEWAY_TESTS "构建并注册 Gateway 主机集成测试" OFF)
option(UAV_RTSP_ONLY "仅构建 RTSP，不查找板端和 SIP 依赖" OFF)
option(UAV_RTSP_TESTS "构建并注册 RTSP 主机回归测试" OFF)
if(CMAKE_CROSSCOMPILING)
  list(APPEND CMAKE_FIND_ROOT_PATH "${PROJECT_SOURCE_DIR}/third_party/openssl")
endif()
if(UAV_GATEWAY_TESTS OR UAV_RTSP_TESTS)
  enable_testing()
endif()
if(UAV_GATEWAY)
  add_subdirectory(src/gateway)
endif()
if(UAV_GATEWAY_ONLY)
  if(NOT UAV_GATEWAY)
    message(FATAL_ERROR "UAV_GATEWAY_ONLY requires UAV_GATEWAY=ON")
  endif()
  return()
endif()
if(UAV_RTSP_ONLY OR UAV_RTSP_TESTS)
  add_subdirectory(src/rtsp)
endif()
if(UAV_RTSP_ONLY)
  return()
endif()

# ---------------------------------------------------------------------------
# 强制交叉编译：uav_detection 是板端程序，只允许交叉编译到板端架构
#
# 工具链本身已经在文件顶部默认指定（cmake/toolchain-ascend-aarch64.cmake），
# 这里只是兜底：万一 CMAKE_TOOLCHAIN_FILE 被显式换成了非交叉的，就在**所有
# find_package 之前**停下来。
#
# 为什么必须卡在这里（上面两个 return() 之后、所有 find_package 之前）：
#   * third_party/ 下的依赖全是 **aarch64 静态库**（boost / openssl / libxml2 /
#     libosip2 / libeXosip2 / libsrtp / usrsctp）。宿主机 x86_64 构建时它们照样能被
#     find_package 找到，configure 一路通过，直到链接期才炸：
#         /usr/bin/ld: third_party/openssl/lib/libssl.a: error adding symbols: file in wrong format
#     放在这里先拦下来，报一条能看懂的错误，而不是让人去猜一堆找不到符号。
#   * 板端运行时依赖（CANN/ACL、海思媒体库 hi_mpi_*/hi_dvpp）也只有目标架构上才有意义。
#
# 本机 x86_64 编译的入口只剩「不需要板端依赖」的那些，各自都是独立工程：
#   cmake --preset gateway-native     # 只编 Gateway 子集
#   cmake --preset rtsp-native        # 只编 RTSP 子集
#   cmake -S tests/webrtc -B ...      # 独立单元测试（宿主机原生编译）
#
# 需要 aarch64 但不要昇腾环境时，仍然是交叉编译 + 关掉昇腾：
#   cmake -S . -B build -DUAV_ASCEND=OFF
# ---------------------------------------------------------------------------
if(NOT CMAKE_CROSSCOMPILING)
  # 注意：CMake 打印 message 时按 ~79 字节软换行，中文一个字 3 字节，所以每行都
  # 控制在 25 个汉字以内，否则会在句子中间断开。
  message(FATAL_ERROR
    "uav_detection 是板端程序，必须交叉编译；\n"
    "当前是宿主机原生构建（${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}）。\n"
    "本工程已在 CMakeLists.txt 顶部默认指定昇腾工具链，\n"
    "走到这里说明 CMAKE_TOOLCHAIN_FILE 被显式指定成了非交叉的。\n"
    "去掉该覆盖，或用：cmake --preset ascend-aarch64\n"
    "本机 x86_64 只用于子集与单元测试：\n"
    "  cmake --preset gateway-native | cmake --preset rtsp-native\n"
    "  cmake -S tests/<模块> -B build/<模块>-tests")
endif()

# third_party/ 下提交的都是 aarch64 静态库，交叉编译到其他架构同样会在链接期失败
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  message(FATAL_ERROR
    "third_party/ 下的第三方库都是 aarch64 静态库，\n"
    "当前目标架构是 ${CMAKE_SYSTEM_PROCESSOR}。\n"
    "要编其他架构，需要先为对应架构交叉编译这些库\n"
    "（参考各库目录下的 build-aarch64.sh，改 HOST 即可）。")
endif()

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
# 默认开启：直接使用“当前系统”已安装的 CANN（头文件 + libascendcl），它的架构必须
# 与本次编译的目标架构一致。本机的 CANN 是 aarch64 版本，而工具链已经在文件顶部
# 默认指定成昇腾 aarch64 交叉工具链，所以裸敲就是板端构建：
#
#   cmake -S . -B build                 # 昇腾 aarch64（CANN + aarch64-linux-gnu-*）
#   cmake --preset ascend-aarch64       # 同上，预设里显式写了工具链
#   cmake -S . -B build -DUAV_ASCEND=OFF  # 交叉编译但不要昇腾环境
#
# 也可以手动指定 CANN 路径：-DAscend_ROOT=/usr/local/Ascend/ascend-toolkit/latest
# ---------------------------------------------------------------------------
option(UAV_ASCEND "使用当前系统的昇腾（CANN/ACL）编译环境" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---------------------------------------------------------------------------
# libsrtp（WebRTC 的 DTLS-SRTP 媒体加密）
#
# 默认用随仓库提交的 aarch64 静态库（third_party/libsrtp，2.8.1，与 boost/openssl/
# libxml2 的约定一致），由 third_party/libsrtp/build-aarch64.sh 交叉编译生成：
#   third_party/libsrtp/include/srtp2/srtp.h   → #include <srtp2/srtp.h>
#   third_party/libsrtp/lib/libsrtp2.a
#
# 该库是用 --enable-openssl 编出来的：WebRTC 协商的 SRTP_AEAD_AES_128_GCM /
# SRTP_AEAD_AES_256_GCM 只有 OpenSSL 后端才提供，所以链接期还要带上
# third_party/openssl 的 libcrypto —— 已经挂在 FindSRTP.cmake 的导入目标里了。
#
# 想改用系统安装的版本（例如 x86_64 本机调试、真的要调用 libsrtp 时）：
#   apt install libsrtp2-dev && cmake ... -DSRTP_ROOT=/usr
#
# 注意：必须放在上面 CMAKE_MODULE_PATH 之后——FindSRTP.cmake 是本仓库的
# find 模块（cmake/FindSRTP.cmake），不在 CMake 内置模块目录里。
# ---------------------------------------------------------------------------
find_package(SRTP REQUIRED)
message(STATUS "libsrtp: ${SRTP_VERSION}")
message(STATUS "  头文件: ${SRTP_INCLUDE_DIR}")
message(STATUS "  库文件: ${SRTP_LIBRARY}")

# ---------------------------------------------------------------------------
# usrsctp（WebRTC 的 SCTP over DTLS / DataChannel）
#
# 默认用随仓库提交的 aarch64 静态库（third_party/usrsctp，0.9.5.0，与上面几个库的
# 约定一致），由 third_party/usrsctp/build-aarch64.sh 交叉编译生成：
#   third_party/usrsctp/include/usrsctp.h   → #include <usrsctp.h>
#   third_party/usrsctp/lib/libusrsctp.a
#
# 宿主机 x86_64 上真正链接（独立测试工程 tests/webrtc）需要同架构的库：先按
# docs/datachannel.md 自行构建一份，再用 -DUSRSCTP_ROOT=<该前缀> 覆盖——
# third_party 里那份是 aarch64 的，不能链进 x86_64 可执行文件。
#
# 注意：同样必须放在 CMAKE_MODULE_PATH 之后（FindUSRSCTP.cmake 在 cmake/ 下）。
# ---------------------------------------------------------------------------
find_package(USRSCTP REQUIRED)
message(STATUS "usrsctp:")
message(STATUS "  头文件: ${USRSCTP_INCLUDE_DIR}")
message(STATUS "  库文件: ${USRSCTP_LIBRARY}")

if(UAV_ASCEND)
  find_package(Ascend REQUIRED)
  message(STATUS "昇腾编译环境: ${ASCEND_HOME} (架构目录 ${ASCEND_ARCH_DIR}, 版本 ${ASCEND_VERSION})")
  message(STATUS "  ACL 头文件: ${ASCEND_INCLUDE_DIRS}")
  message(STATUS "  ACL 库    : ${ASCEND_ACL_LIBRARY}")
  message(STATUS "  媒体库    : ${ASCEND_MEDIA_LIBRARIES}")
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

# 依赖昇腾头文件/库的源文件：只有 UAV_ASCEND=ON 时才参与编译。否则 acl.h / hi_mpi_*.h
# 根本不在 include 路径里，UAV_ASCEND=OFF 的构建（交叉编译但不带 CANN）会直接编译失败。
# 与下面 UAV_GB28181_SOURCES 是同一套处理方式。
set(UAV_ASCEND_SOURCES "")
if(UAV_ASCEND)
  set(UAV_ASCEND_SOURCES
    src/camera/mipi_ctl.cpp          # MIPI/DVPP 采集链路（acl.h + hi_mpi_*/hi_dvpp.h）
    src/camera/rgb_camera_ctl.cpp    # 可见光相机总控（串口控制 + 整条出图链路编排）
    src/camera/thermal_camera_ctl.cpp # 热成像单通道 640x512 采集和串口控制
  )
endif()

# 注意：新增模块的 .cpp 必须显式列在这里（旧版本漏掉整个 log 模块，
# 导致日志代码从未被编译、里面的编译错误一直没被发现）
set(UAV_SOURCES
  src/main.cpp
  src/log/log.cpp
  src/log/RotateFileWriter.cpp
  src/common/udp_socket.cpp      # 通用 Asio UDP 数据报收发（MoQ / WebRTC）
  src/webrtc/ice_candidate.cpp   # ICE 候选 SDP 与优先级
  src/webrtc/sdp.cpp             # WebRTC SDP 会话/媒体描述解析与生成
  src/webrtc/stun_protocol.cpp   # STUN Binding / ICE 属性及完整性校验
  src/webrtc/ice_agent.cpp       # UDP ICE 候选收集、连通性检查、提名与 consent
  src/webrtc/ice_transport.cpp   # WebRTC ICE 传输、报文分流与 DTLS 接入参数
  src/webrtc/dtlstransport.cpp   # DTLS 握手、证书指纹认证、应用数据与 SRTP exporter
  src/webrtc/datachannel.cpp    # SCTP over DTLS、DCEP 和 DataChannel
  src/webrtc/peerconnection.cpp # Offer/Answer 协商、trickle ICE 和 ICE/DTLS 生命周期
  src/webrtc/srtp_session.cpp   # libsrtp 公共上下文、密钥代次及认证/重放保护
  src/webrtc/srtp_ctl.cpp       # RTP/SRTP 加解密控制
  src/webrtc/srtcp_ctl.cpp      # RTCP/SRTCP 加解密控制
  src/webrtc/rtcp_protocol.cpp # SR/RR、NACK/PLI/FIR、TWCC 编解码
  src/webrtc/rtcp_session.cpp  # 自动 RTCP 报告、反馈与 SRTP 密文重传
  src/webrtc/transport_adapter.cpp # libwebrtc 风格发送历史、在途字节及 TWCC 适配
  src/webrtc/congestion_control/pacer.cpp # RTP 优先级队列、按速率放行和有界等待
  src/webrtc/congestion_control/interval_budget.cpp # 媒体/padding 的周期字节预算
  src/webrtc/congestion_control/bitrate_estimator.cpp # 窗口采样与贝叶斯吞吐滤波
  src/webrtc/congestion_control/acknowledged_bitrate_estimator.cpp # TWCC 确认吞吐及 ALR 状态
  src/webrtc/congestion_control/inter_arrival_delta.cpp # 发送包分组及组间发送/接收/大小差值
  src/webrtc/congestion_control/alr_detector.cpp # 实际发送预算与应用受限区域检测
  src/rtsp/rtspserver.cpp
  src/rtsp/rtp_source.cpp
  src/rtsp/rtp_transport.cpp
  src/rtsp/file_source.cpp
  src/rtsp/h264_access_unit.cpp
  src/ptz/ptz_protocol.cpp        # 转台 Pelco-D 及扩展控制协议
  src/camera/shihui_protocol.cpp   # 世辉相机串口控制协议（依赖 common/urt.h）
  src/camera/airui_protocol.cpp    # 艾睿 F384/F640 热成像串口协议
  src/camera/rgb_camera.cpp        # 可见光相机控制封装（工程值 ↔ 协议值换算）
  src/camera/thermal_camera.cpp    # 热成像相机业务控制封装（艾睿协议）
  ${UAV_ASCEND_SOURCES}
  ${UAV_GB28181_SOURCES}
)

add_executable(uav_detection
  ${UAV_SOURCES}
)


target_include_directories(uav_detection PRIVATE src)
if(UAV_GATEWAY)
  target_link_libraries(uav_detection PRIVATE uav_gateway_lib)
  target_compile_definitions(uav_detection PRIVATE UAV_HAS_GATEWAY=1)
endif()
target_link_libraries(uav_detection PRIVATE
  Boost::headers                 # header-only（asio/beast/json…）
  Boost::thread
  Boost::system
  OpenSSL::SSL OpenSSL::Crypto
  LibXml2::LibXml2               # XML 解析（GB28181/ONVIF），见上方 third_party/libxml2
  SRTP::SRTP                    # WebRTC DTLS-SRTP 媒体加解密
  USRSCTP::USRSCTP)              # WebRTC SCTP over DTLS / DataChannel
# 日志模块已改用 std::filesystem（C++17 标准库），不再需要 Boost::filesystem；
# 其他模块若要使用 Boost.Filesystem，在此处加回 Boost::filesystem 即可。

if(UAV_GB28181)
  # SIP 信令栈（eXosip2 + osip2 + pthread，传递依赖都在导入目标里带好）
  target_link_libraries(uav_detection PRIVATE EXosip2::EXosip2)
endif()

if(UAV_ASCEND)
  # main.cpp 用这个宏决定要不要编采集链路：它依赖海思头文件，而 main.cpp 在公共
  # UAV_SOURCES 里（UAV_ASCEND=OFF 的构建也会编它），所以必须靠宏隔离，否则编不过。
  target_compile_definitions(uav_detection PRIVATE UAV_HAS_ASCEND=1)

  # ACL 运行时 + DVPP/媒体 MPI 库（Ascend::ascend_media = libascendcl + libacl_dvpp[_mpi]
  # + libacl_vi_mpi + libacl_vpss_mpi + libacl_vo_mpi + libacl_hdmi_mpi + libacl_isp_* 等，
  # 头文件里声明的 hi_mpi_sys_* / hi_mpi_vpss_* / hi_mpi_vi_* 等符号就来自这些库；
  # 头文件 ↔ 库 的对应关系与查证方法见 cmake/FindAscend.cmake 顶部注释）。
  # 这些是共享库，链接器 --as-needed 不会把尚未调用的库写进 NEEDED —— 所以现在就链上
  # 没有副作用，接入 detect / mipi_ctl 等模块后会自然成为真实依赖；也可以只链需要的那个
  # （如 Ascend::acl_vi_mpi）。
  # 板卡上运行时需要这些 .so 在库搜索路径里：先 source CANN 的 setenv.bash，或自行设置
  # LD_LIBRARY_PATH；注意板卡侧的 .so 版本可能与编译期用的 CANN 版本不完全一致。
  target_link_libraries(uav_detection PRIVATE Ascend::ascend_media)
endif()
