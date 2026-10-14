#!/usr/bin/env bash
#=============================================================================
# build-aarch64.sh —— 交叉编译 libsrtp（昇腾 aarch64 静态库）
#
# libsrtp2 是 WebRTC 的媒体加密库：拿到 DTLS 握手导出的密钥材料
# （dtlstransport.cpp 里的 EXTRACTOR-dtls_srtp）后，用它做 RTP/RTCP 的
# 加解密与重放保护，即 DTLS-SRTP。
#
# 产物直接安装到本目录（third_party/libsrtp）：
#   include/srtp2/srtp.h           上游目录布局 → #include <srtp2/srtp.h>
#   include/srtp2/cipher.h         自定义 cipher（高级用法）
#   include/srtp2/auth.h           自定义 auth（高级用法）
#   include/srtp2/crypto_types.h   加密后端类型
#   lib/libsrtp2.a                 aarch64 静态库（供 CMakeLists.txt 链接）
#   lib/pkgconfig/libsrtp2.pc      可选，便于用 PKG_CONFIG_PATH 查询
#
# 用法：
#   ./third_party/libsrtp/build-aarch64.sh
#
# 说明：
#   * 只编静态库（make 目标 libsrtp2.a）：与 third_party/libxml2、libosip2、
#     boost、openssl 的约定一致，板端不需要额外部署 libsrtp2.so。
#     上游 Makefile 的 `all` 会连带编 test/srtp_driver、test/rtp_decoder 等
#     测试程序（后者还要 -lpcap），交叉编译时既没用又容易缺依赖，所以这里
#     显式只 build 库目标；共享库也是单独目标，不受影响。
#   * --enable-openssl 必须显式打开（上游默认 no）：默认后端是 libsrtp 自带的
#     AES-CM + HMAC-SHA1 实现，不含 AES-GCM，而 WebRTC 协商的
#     SRTP_AEAD_AES_128_GCM / SRTP_AEAD_AES_256_GCM 只有 OpenSSL 后端才有；
#     同时复用 third_party/openssl 也避免板端多背一份加密实现。
#   * --with-openssl-dir 指向 third_party/openssl，并把它作为 PKG_CONFIG_LIBDIR
#     限定住 pkg-config 的搜索范围：configure 的 PKG_CHECK_MODULES([crypto])
#     否则可能命中宿主机的 x86_64 libcrypto.pc，把 -I/usr/include 和
#     /usr/lib/x86_64-linux-gnu/libcrypto.so 塞进 aarch64 的编译/链接命令。
#   * 目标架构固定 aarch64-linux-gnu-*，如需其他架构改 HOST 即可。
#   * 许可证：libsrtp 为 3-clause BSD（见源码包 LICENSE），可自由分发。
#=============================================================================
set -euo pipefail

LIBSRTP_VERSION="${LIBSRTP_VERSION:-2.8.1}"
LIBSRTP_SHA256="${LIBSRTP_SHA256:-ef5569220749529d778013aae1178391d972570a2b4f7288dda22effa875b07c}"
HOST="${HOST:-aarch64-linux-gnu}"
JOBS="${JOBS:-$(nproc)}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="${WORK:-${REPO_ROOT}/build/thirdparty-src}"
SRC_DIR="${WORK}/libsrtp-${LIBSRTP_VERSION}"
BUILD_DIR="${SRC_DIR}/build-${HOST}"
STAGE_DIR="${WORK}/stage-${HOST}-libsrtp"
# 加密后端：third_party/openssl（aarch64 静态库 + 自带 .pc）
OPENSSL_DIR="${OPENSSL_DIR:-${REPO_ROOT}/third_party/openssl}"

# GitHub 的 tag 归档：解压出来的顶层目录不带 v 前缀（libsrtp-2.8.1）
TARBALL="${WORK}/libsrtp-${LIBSRTP_VERSION}.tar.gz"
URL="https://github.com/cisco/libsrtp/archive/refs/tags/v${LIBSRTP_VERSION}.tar.gz"

for tool in "${HOST}-gcc" "${HOST}-ar" "${HOST}-strip"; do
  command -v "${tool}" >/dev/null || { echo "错误: 找不到交叉编译工具 ${tool}" >&2; exit 1; }
done

if [[ ! -f "${OPENSSL_DIR}/lib/pkgconfig/libcrypto.pc" ]]; then
  echo "错误: 找不到 ${OPENSSL_DIR}/lib/pkgconfig/libcrypto.pc" >&2
  echo "      libsrtp 的 OpenSSL 后端需要它（可用 OPENSSL_DIR=... 指定其他前缀）" >&2
  exit 1
fi

mkdir -p "${WORK}"
if [[ ! -f "${TARBALL}" ]]; then
  echo "==> 下载 ${URL}"
  curl -fSL -o "${TARBALL}" "${URL}"
fi
# 注意：GitHub 自动生成的 tag 归档理论上可能被重新打包而改变字节（校验失败时
# 用 LIBSRTP_SHA256=<实际值> 覆盖，或预先放好 ${TARBALL} 再执行本脚本）
echo "==> 校验 sha256"
echo "${LIBSRTP_SHA256}  ${TARBALL}" | sha256sum -c -

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "==> 解压到 ${SRC_DIR}"
  tar -C "${WORK}" -xf "${TARBALL}"
fi

echo "==> configure (host=${HOST}, openssl=${OPENSSL_DIR})"
mkdir -p "${BUILD_DIR}"
(
  cd "${BUILD_DIR}"
  # 只在 configure 期间生效：把 pkg-config 锁到 third_party/openssl，防止
  # 命中宿主机 x86_64 的 libcrypto.pc（交叉编译时头文件/库架构会不一致）
  export PKG_CONFIG_PATH="${OPENSSL_DIR}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${OPENSSL_DIR}/lib/pkgconfig"
  "${SRC_DIR}/configure" \
    --host="${HOST}" \
    --build="$(uname -m)-linux-gnu" \
    --prefix="${STAGE_DIR}" \
    --enable-openssl \
    --with-openssl-dir="${OPENSSL_DIR}" \
    --disable-pcap \
    CFLAGS="-O2 -fPIC"
)

# 只编静态库（见文件头说明：上游 `all` 会连带交叉编译一堆测试程序）
echo "==> make libsrtp2.a -j${JOBS}"
make -C "${BUILD_DIR}" -j"${JOBS}" libsrtp2.a

# 硬性检查：OpenSSL 后端是本脚本的前提。没有它就没有 AES-GCM，WebRTC 协商的
# SRTP_AEAD_AES_128/256_GCM 会直接失败——宁可在这里报错，也不要产出一个残废的库。
if ! grep -q 'define OPENSSL' "${BUILD_DIR}/crypto/include/config.h" 2>/dev/null; then
  echo "错误: libsrtp 没有启用 OpenSSL 后端（crypto/include/config.h 里没有 OPENSSL 宏）" >&2
  echo "      请检查 --enable-openssl / --with-openssl-dir 与 ${OPENSSL_DIR} 是否可用" >&2
  exit 1
fi

echo "==> install 到 ${STAGE_DIR}"
rm -rf "${STAGE_DIR}"
make -C "${BUILD_DIR}" install

echo "==> 拷贝到 ${HERE}"
rm -rf "${HERE}/include" "${HERE}/lib"
mkdir -p "${HERE}/include" "${HERE}/lib/pkgconfig"
cp -a "${STAGE_DIR}/include/srtp2" "${HERE}/include/"
# 去掉调试符号，仓库里少占体积
cp -a "${STAGE_DIR}/lib/libsrtp2.a" "${HERE}/lib/libsrtp2.a"
"${HOST}-strip" -g "${HERE}/lib/libsrtp2.a" || true
cp -a "${STAGE_DIR}/lib/pkgconfig/libsrtp2.pc" "${HERE}/lib/pkgconfig/" 2>/dev/null || true
# .pc 里的 prefix / exec_prefix 都指向临时 stage 目录，改成 third_party 下的真实位置。
# 注意 libsrtp 的 libsrtp2.pc.in 里 exec_prefix 是独立的一行（写成 @prefix@ 而不是
# ${prefix}），只改 prefix 的话 libdir=${exec_prefix}/lib 仍会指向 stage 目录，必须一起改。
if [[ -f "${HERE}/lib/pkgconfig/libsrtp2.pc" ]]; then
  sed -i -e "s|^prefix=.*|prefix=${HERE}|" \
         -e 's|^exec_prefix=.*|exec_prefix=${prefix}|' \
         "${HERE}/lib/pkgconfig/libsrtp2.pc"
fi

echo "==> 完成"
echo "    头文件: ${HERE}/include/srtp2/srtp.h"
echo "    静态库: ${HERE}/lib/libsrtp2.a"
# 上游没有把版本号写成头文件宏（只有运行时 API srtp_get_version_string()），这里回显本次编译的版本
echo "    版本  : ${LIBSRTP_VERSION}"
echo "    加密后端: openssl（$(grep -c 'define OPENSSL' "${BUILD_DIR}/crypto/include/config.h" 2>/dev/null || true) 处 config.h 宏，含 AES-GCM）"
echo "    架构  : $(ar p "${HERE}/lib/libsrtp2.a" srtp.o 2>/dev/null | file -b - || echo '未知')"
