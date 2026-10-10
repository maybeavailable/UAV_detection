#!/usr/bin/env bash
#=============================================================================
# build-aarch64.sh —— 交叉编译 libxml2（昇腾 aarch64 静态库）
#
# 产物直接安装到本目录（third_party/libxml2）：
#   include/libxml2/libxml/*.h    上游目录布局，和 /usr/include/libxml2 一致
#   lib/libxml2.a                 aarch64 静态库（供 CMakeLists.txt 链接）
#   lib/pkgconfig/libxml-2.0.pc   可选，便于用 PKG_CONFIG_PATH 查询
#
# 用法：
#   ./third_party/libxml2/build-aarch64.sh
#
# 说明：
#   * 只编静态库（--disable-shared）：与 third_party/boost、third_party/openssl
#     的约定一致，板端不需要额外部署 libxml2.so。
#   * 关掉 zlib/lzma：解析 GB28181 等明文 XML 不需要压缩/压缩包支持，
#     关掉可以避免多出 -lz / -llzma 链接依赖。iconv 保留（glibc 自带，
#     不引入额外库），用于非 UTF-8 编码的 XML。
#   * 目标架构固定 aarch64-linux-gnu-*，如需其他架构改 HOST 即可。
#=============================================================================
set -euo pipefail

LIBXML2_VERSION="${LIBXML2_VERSION:-2.9.13}"
LIBXML2_SHA256="${LIBXML2_SHA256:-276130602d12fe484ecc03447ee5e759d0465558fbc9d6bd144e3745306ebf0e}"
HOST="${HOST:-aarch64-linux-gnu}"
JOBS="${JOBS:-$(nproc)}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="${WORK:-${REPO_ROOT}/build/thirdparty-src}"
SRC_DIR="${WORK}/libxml2-${LIBXML2_VERSION}"
BUILD_DIR="${SRC_DIR}/build-${HOST}"
STAGE_DIR="${WORK}/stage-${HOST}-libxml2"

TARBALL="${WORK}/libxml2-${LIBXML2_VERSION}.tar.xz"
URL="https://download.gnome.org/sources/libxml2/${LIBXML2_VERSION%.*}/libxml2-${LIBXML2_VERSION}.tar.xz"

for tool in "${HOST}-gcc" "${HOST}-ar" "${HOST}-strip"; do
  command -v "${tool}" >/dev/null || { echo "错误: 找不到交叉编译工具 ${tool}" >&2; exit 1; }
done

mkdir -p "${WORK}"
if [[ ! -f "${TARBALL}" ]]; then
  echo "==> 下载 ${URL}"
  curl -fSL -o "${TARBALL}" "${URL}"
fi
echo "==> 校验 sha256"
echo "${LIBXML2_SHA256}  ${TARBALL}" | sha256sum -c -

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
    --without-python --without-zlib --without-lzma \
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
cp -a "${STAGE_DIR}/include/libxml2" "${HERE}/include/"
# 去掉调试符号，仓库里少占体积
cp -a "${STAGE_DIR}/lib/libxml2.a" "${HERE}/lib/libxml2.a"
"${HOST}-strip" -g "${HERE}/lib/libxml2.a" || true
cp -a "${STAGE_DIR}/lib/pkgconfig/libxml-2.0.pc" "${HERE}/lib/pkgconfig/" 2>/dev/null || true
# .pc 里的 prefix 指向临时 stage 目录，改成 third_party 下的真实位置
if [[ -f "${HERE}/lib/pkgconfig/libxml-2.0.pc" ]]; then
  sed -i "s|^prefix=.*|prefix=${HERE}|" "${HERE}/lib/pkgconfig/libxml-2.0.pc"
fi

echo "==> 完成"
echo "    头文件: ${HERE}/include/libxml2/libxml/xmlversion.h"
echo "    静态库: ${HERE}/lib/libxml2.a"
echo "    版本  : $(sed -n 's/^#define LIBXML_DOTTED_VERSION "\(.*\)"/\1/p' \
        "${HERE}/include/libxml2/libxml/xmlversion.h")"
echo "    架构  : $(ar p "${HERE}/lib/libxml2.a" parser.o | file -b -)"
