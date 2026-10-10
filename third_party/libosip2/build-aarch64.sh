#!/usr/bin/env bash
#=============================================================================
# build-aarch64.sh —— 交叉编译 libosip2（昇腾 aarch64 静态库）
#
# libosip2 是 libeXosip2 的底层依赖（SIP 消息解析 / 事务状态机），
# 必须先于 third_party/libeXosip2/build-aarch64.sh 执行。
#
# 产物直接安装到本目录（third_party/libosip2）：
#   include/osip2/osip.h           上游目录布局
#   lib/libosip2.a                 aarch64 静态库（供 CMakeLists.txt 链接）
#   lib/pkgconfig/libosip2.pc      供 libeXosip2 的 configure 用 pkg-config 查找
#
# 用法：
#   ./third_party/libosip2/build-aarch64.sh
#
# 说明：
#   * 只编静态库（--disable-shared）：与 third_party/libxml2、boost、openssl
#     的约定一致，板端不需要额外部署 libosip2.so。
#   * 许可证：libosip2 为 LGPL-2.1+（见源码包 COPYING），与 GPL 的 libeXosip2
#     不同，单独分发/使用时约束更宽松。
#   * 目标架构固定 aarch64-linux-gnu-*，如需其他架构改 HOST 即可。
#=============================================================================
set -euo pipefail

LIBOSIP2_VERSION="${LIBOSIP2_VERSION:-5.3.2}"
LIBOSIP2_SHA256="${LIBOSIP2_SHA256:-16186f6f5540936b62c3aaca6e8409e1af25cd22abc3882b393be215f49d3b00}"
HOST="${HOST:-aarch64-linux-gnu}"
JOBS="${JOBS:-$(nproc)}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="${WORK:-${REPO_ROOT}/build/thirdparty-src}"
SRC_DIR="${WORK}/libosip2-${LIBOSIP2_VERSION}"
BUILD_DIR="${SRC_DIR}/build-${HOST}"
STAGE_DIR="${WORK}/stage-${HOST}-libosip2"

TARBALL="${WORK}/libosip2-${LIBOSIP2_VERSION}.tar.gz"
URL="https://ftp.gnu.org/gnu/osip/libosip2-${LIBOSIP2_VERSION}.tar.gz"

for tool in "${HOST}-gcc" "${HOST}-ar" "${HOST}-strip"; do
  command -v "${tool}" >/dev/null || { echo "错误: 找不到交叉编译工具 ${tool}" >&2; exit 1; }
done

mkdir -p "${WORK}"
if [[ ! -f "${TARBALL}" ]]; then
  echo "==> 下载 ${URL}"
  curl -fSL -o "${TARBALL}" "${URL}"
fi
echo "==> 校验 sha256"
echo "${LIBOSIP2_SHA256}  ${TARBALL}" | sha256sum -c -

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "==> 解压到 ${SRC_DIR}"
  tar -C "${WORK}" -xf "${TARBALL}"
fi

echo "==> configure (host=${HOST})"
mkdir -p "${BUILD_DIR}"
(
  cd "${BUILD_DIR}"
  "${SRC_DIR}/configure" \
    --host="${HOST}" \
    --build="$(uname -m)-linux-gnu" \
    --prefix="${STAGE_DIR}" \
    --disable-shared --enable-static \
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
mkdir -p "${HERE}/include" "${HERE}/lib/pkgconfig"
cp -a "${STAGE_DIR}/include/osip2" "${HERE}/include/"
cp -a "${STAGE_DIR}/include/osipparser2" "${HERE}/include/"
# 去掉调试符号，仓库里少占体积
# 注意 libosip2 上游产出两个库：libosip2(事务/状态机) 与 libosipparser2(报文解析)，
# 链接时必须两个都带（.pc 里的 Libs 也是 -losipparser2 -losip2），缺一会报大量 undefined reference
for lib in libosip2.a libosipparser2.a; do
  cp -a "${STAGE_DIR}/lib/${lib}" "${HERE}/lib/${lib}"
  "${HOST}-strip" -g "${HERE}/lib/${lib}" || true
done
cp -a "${STAGE_DIR}/lib/pkgconfig/libosip2.pc" "${HERE}/lib/pkgconfig/" 2>/dev/null || true
# .pc 里的 prefix 指向临时 stage 目录，改成 third_party 下的真实位置
# （libeXosip2 的 configure 会靠这个 .pc 找 libosip2）
if [[ -f "${HERE}/lib/pkgconfig/libosip2.pc" ]]; then
  sed -i "s|^prefix=.*|prefix=${HERE}|" "${HERE}/lib/pkgconfig/libosip2.pc"
fi

echo "==> 完成"
echo "    头文件: ${HERE}/include/osip2/osip.h"
echo "    静态库: ${HERE}/lib/libosip2.a + libosipparser2.a"
echo "    pkgconfig: ${HERE}/lib/pkgconfig/libosip2.pc"
echo "    架构  : $(ar p "${HERE}/lib/libosip2.a" osip.o 2>/dev/null | file -b - || echo '未知')"
