#!/usr/bin/env bash
#=============================================================================
# build-aarch64.sh —— 交叉编译 libeXosip2（昇腾 aarch64 静态库）
#
# libeXosip2 建立在 libosip2 之上，提供 SIP 事务/dialog/注册的完整状态机，
# 用于 GB28181 与平台的 SIP 通信（REGISTER 注册、MESSAGE 指令、INVITE 媒体会话）。
#
# 依赖：必须先执行 third_party/libosip2/build-aarch64.sh
#       （本脚本通过 PKG_CONFIG_PATH 找 libosip2，找不到会直接报错退出）
#
# 产物直接安装到本目录（third_party/libeXosip2）：
#   include/eXosip2/*.h      上游目录布局（eXosip.h / eXosip2_authentication.h ...）
#   lib/libeXosip2.a         aarch64 静态库（供 CMakeLists.txt 链接）
#   （上游不生成 .pc 文件，头/库路径由 cmake/FindEXosip2.cmake 直接定位）
#
# 用法：
#   ./third_party/libeXosip2/build-aarch64.sh
#
# 说明：
#   * 只编静态库（--disable-shared），与 third_party 下其它依赖的约定一致。
#   * --disable-openssl：GB28181 平台接入通常用明文 UDP/TCP 传输，关掉 TLS
#     可以彻底去掉对 OpenSSL 的链接依赖（configure 里 openssl 是用 pkg-config
#     探测的，交叉编译时容易误命中宿主机 x86_64 版本）。摘要认证用的 MD5 来自
#     libosip2 自带的实现，不依赖 OpenSSL。将来要做 SIP over TLS 时，
#     把该选项改为 --enable-openssl 并指向 third_party/openssl 即可。
#   * --disable-tools：不交叉编译 sip_reg/sip_message 等示例程序（板端用不上，
#     还会拖慢构建、引入额外依赖）。
#   * --disable-srvrec：GB28181 平台地址都是直接给 IP:端口，不需要 DNS SRV 查询，
#     关掉可避免多出 -lresolv 依赖。
#   * 许可证提示：libeXosip2 为 GPL-2+（libosip2 才是 LGPL-2.1+）。闭源商用需
#     向作者购买商业授权，或整体按 GPL-2 发布——落地前请与法务确认。
#   * 目标架构固定 aarch64-linux-gnu-*，如需其他架构改 HOST 即可。
#=============================================================================
set -euo pipefail

LIBEXOSIP2_VERSION="${LIBEXOSIP2_VERSION:-5.3.0}"
LIBEXOSIP2_SHA256="${LIBEXOSIP2_SHA256:-5b7823986431ea5cedc9f095d6964ace966f093b2ae7d0b08404788bfcebc9c2}"
HOST="${HOST:-aarch64-linux-gnu}"
JOBS="${JOBS:-$(nproc)}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="${WORK:-${REPO_ROOT}/build/thirdparty-src}"
SRC_DIR="${WORK}/libexosip2-${LIBEXOSIP2_VERSION}"
BUILD_DIR="${SRC_DIR}/build-${HOST}"
STAGE_DIR="${WORK}/stage-${HOST}-libexosip2"

# libosip2 的位置（可用 OSIP_ROOT 覆盖）
OSIP_ROOT="${OSIP_ROOT:-${REPO_ROOT}/third_party/libosip2}"

TARBALL="${WORK}/libexosip2-${LIBEXOSIP2_VERSION}.tar.gz"
URL="https://download.savannah.gnu.org/releases/exosip/libexosip2-${LIBEXOSIP2_VERSION}.tar.gz"

for tool in "${HOST}-gcc" "${HOST}-ar" "${HOST}-strip"; do
  command -v "${tool}" >/dev/null || { echo "错误: 找不到交叉编译工具 ${tool}" >&2; exit 1; }
done

if [[ ! -f "${OSIP_ROOT}/lib/libosip2.a" || ! -f "${OSIP_ROOT}/lib/libosipparser2.a" ]]; then
  echo "错误: 未找到 ${OSIP_ROOT}/lib/libosip2.a（或 libosipparser2.a）" >&2
  echo "      请先执行: ./third_party/libosip2/build-aarch64.sh" >&2
  exit 1
fi

mkdir -p "${WORK}"
if [[ ! -f "${TARBALL}" ]]; then
  echo "==> 下载 ${URL}"
  curl -fSL -o "${TARBALL}" "${URL}"
fi
echo "==> 校验 sha256"
echo "${LIBEXOSIP2_SHA256}  ${TARBALL}" | sha256sum -c -

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "==> 解压到 ${SRC_DIR}"
  tar -C "${WORK}" -xf "${TARBALL}"
fi

# configure 用 PKG_CHECK_MODULES 找 libosip2。PKG_CONFIG_LIBDIR 会顶掉默认搜索
# 路径，避免交叉编译时命中宿主机 x86_64 的 libosip2（头/库架构不一致）
export PKG_CONFIG_PATH="${OSIP_ROOT}/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="${OSIP_ROOT}/lib/pkgconfig"

echo "==> configure (host=${HOST})"
mkdir -p "${BUILD_DIR}"
(
  cd "${BUILD_DIR}"
  "${SRC_DIR}/configure" \
    --host="${HOST}" \
    --build="$(uname -m)-linux-gnu" \
    --prefix="${STAGE_DIR}" \
    --disable-shared --enable-static \
    --disable-openssl --disable-tools --disable-srvrec \
    --disable-dependency-tracking \
    CFLAGS="-O2 -fPIC"
)

echo "==> make -j${JOBS}"
make -C "${BUILD_DIR}" -j"${JOBS}"

echo "==> install 到 ${STAGE_DIR}"
rm -rf "${STAGE_DIR}"
make -C "${BUILD_DIR}" install

echo "==> 拷贝到 ${HERE}"
rm -rf "${HERE}/include" "${HERE}/lib"
mkdir -p "${HERE}/include" "${HERE}/lib"
cp -a "${STAGE_DIR}/include/eXosip2" "${HERE}/include/"
cp -a "${STAGE_DIR}/lib/libeXosip2.a" "${HERE}/lib/libeXosip2.a"
"${HOST}-strip" -g "${HERE}/lib/libeXosip2.a" || true
# 注：libeXosip2 上游不生成 .pc 文件（只有 libosip2 生成），因此这里不再拷贝 pkgconfig，
# CMake 侧由 cmake/FindEXosip2.cmake 直接按路径查找头文件与静态库。

echo "==> 完成"
echo "    头文件: ${HERE}/include/eXosip2/eXosip.h"
echo "    静态库: ${HERE}/lib/libeXosip2.a"
echo "    依赖  : ${OSIP_ROOT}/lib/libosip2.a + libosipparser2.a"
echo "    架构  : $(ar p "${HERE}/lib/libeXosip2.a" eXosip.o 2>/dev/null | file -b - || echo '未知')"
