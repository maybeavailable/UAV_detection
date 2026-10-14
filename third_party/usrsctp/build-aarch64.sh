#!/usr/bin/env bash
# WebRTC SCTP over DTLS 用户态协议栈；安装到 third_party/usrsctp，保留独立宿主构建。
set -euo pipefail
USRSCTP_VERSION="${USRSCTP_VERSION:-0.9.5.0}"
USRSCTP_SHA256="${USRSCTP_SHA256:-260107caf318650a57a8caa593550e39bca6943e93f970c80d6c17e59d62cd92}"
USRSCTP_HOST="${USRSCTP_HOST:-aarch64-linux-gnu}"
USRSCTP_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USRSCTP_WORK="${USRSCTP_WORK:-${USRSCTP_HERE}/../../build/thirdparty-src}"
mkdir -p "${USRSCTP_WORK}"
USRSCTP_ARCHIVE="${USRSCTP_WORK}/usrsctp-${USRSCTP_VERSION}.tar.gz"
if [[ ! -f "${USRSCTP_ARCHIVE}" ]]; then
  curl -fL --retry 3 "https://codeload.github.com/sctplab/usrsctp/tar.gz/refs/tags/${USRSCTP_VERSION}" -o "${USRSCTP_ARCHIVE}"
fi
printf '%s  %s\n' "${USRSCTP_SHA256}" "${USRSCTP_ARCHIVE}" | sha256sum -c -
if [[ ! -d "${USRSCTP_WORK}/usrsctp-${USRSCTP_VERSION}" ]]; then
  tar -xf "${USRSCTP_ARCHIVE}" -C "${USRSCTP_WORK}"
fi
cmake -S "${USRSCTP_WORK}/usrsctp-${USRSCTP_VERSION}" -B "${USRSCTP_WORK}/usrsctp-${USRSCTP_VERSION}/build-${USRSCTP_HOST}" \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="${USRSCTP_HOST}-gcc" -DCMAKE_INSTALL_PREFIX="${USRSCTP_HERE}" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF -Dsctp_debug=OFF
cmake --build "${USRSCTP_WORK}/usrsctp-${USRSCTP_VERSION}/build-${USRSCTP_HOST}" -j"${JOBS:-4}"
cmake --install "${USRSCTP_WORK}/usrsctp-${USRSCTP_VERSION}/build-${USRSCTP_HOST}"
