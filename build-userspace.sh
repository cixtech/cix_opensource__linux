#!/usr/bin/env bash
#
# Xenomai 4 (EVL) userspace build script.
# First run clones https://gitlab.com/Xenomai/xenomai4/libevl.git at tag r49.
#
# Typical usage:
#   export CROSS_COMPILE=aarch64-none-linux-gnu-
#   export PATH=/path/to/toolchain/bin:$PATH
#   ./build-userspace.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

: "${EVL_SRC:=${PARENT_DIR}/libevl}"
: "${EVL_GIT:=https://gitlab.com/Xenomai/xenomai4/libevl.git}"
# libevl tag matching this kernel's EVL UAPI
: "${EVL_TAG:=r49}"
: "${KERNEL_SRC:=${SCRIPT_DIR}}"
: "${CROSS_COMPILE:=aarch64-none-linux-gnu-}"
# When non-empty, prepended to PATH (e.g. .../gcc-arm-*/bin)
: "${TOOLCHAIN_BIN:=}"
: "${STAGING_ROOT:=${PARENT_DIR}/xenomai4-out/staging}"
: "${DEB_OUT:=${PARENT_DIR}/xenomai4-out/debs}"
: "${DEB_PKG_NAME:=cix-libevl-xenomai4}"
: "${DEB_VERSION:=4.0.0}"
: "${JOBS:=$(nproc 2>/dev/null || echo 4)}"

log() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

ensure_toolchain() {
    if [[ -n "${TOOLCHAIN_BIN}" ]]; then
        export PATH="${TOOLCHAIN_BIN}:${PATH}"
    fi
    command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 \
        || die "Cross compiler not found: ${CROSS_COMPILE}gcc (set PATH or TOOLCHAIN_BIN)"
    command -v meson >/dev/null 2>&1 || die "meson not found (apt install meson)"
}

# fresh clone is pinned to EVL_TAG; an existing tree is used as-is
ensure_libevl_source() {
    if [[ ! -d "${EVL_SRC}" ]]; then
        log "Cloning libevl (${EVL_TAG}) into ${EVL_SRC}"
        git clone "${EVL_GIT}" "${EVL_SRC}"
        git -C "${EVL_SRC}" checkout "${EVL_TAG}"
    fi
    log "libevl revision: $(git -C "${EVL_SRC}" describe --tags --always 2>/dev/null || echo unknown)"
}

ensure_toolchain
ensure_libevl_source

[[ -f "${EVL_SRC}/meson.build" ]] || die "libevl meson.build not found: ${EVL_SRC}"
[[ -f "${EVL_SRC}/meson/aarch64-none-linux-gnu" ]] || die "meson cross file not found: ${EVL_SRC}/meson/aarch64-none-linux-gnu"
[[ -d "${KERNEL_SRC}" ]] || die "Kernel source not found at ${KERNEL_SRC}"
[[ -d "${KERNEL_SRC}/include/uapi/evl" ]] || die "EVL kernel UAPI not found at ${KERNEL_SRC}/include/uapi/evl"

log "Xenomai 4 (EVL) userspace build"
log "libevl source:   ${EVL_SRC} (${EVL_TAG})"
log "Kernel source:   ${KERNEL_SRC}"
log "Staging dir:     ${STAGING_ROOT}"
log "Deb output:      ${DEB_OUT}"

rm -rf "${STAGING_ROOT}"
mkdir -p "${STAGING_ROOT}" "${DEB_OUT}"

BUILD_DIR="${EVL_SRC}/build-arm64"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# -Duapi needs the kernel tree; RPATH so binaries find libevl without ldconfig
log "Configuring with meson for ARM64..."
cd "${BUILD_DIR}"
CC="${CROSS_COMPILE}gcc" CXX="${CROSS_COMPILE}g++" \
    LDFLAGS="-Wl,-rpath,/usr/xenomai/lib" meson setup .. \
    --cross-file="${EVL_SRC}/meson/aarch64-none-linux-gnu" \
    --prefix=/usr/xenomai \
    -Duapi="${KERNEL_SRC}"

log "Building (JOBS=${JOBS})..."
ninja -j"${JOBS}"
DESTDIR="${STAGING_ROOT}" ninja install

log "Building ${DEB_PKG_NAME} DEB package..."
mkdir -p "${STAGING_ROOT}/DEBIAN"
cat > "${STAGING_ROOT}/DEBIAN/control" <<EOF
Package: ${DEB_PKG_NAME}
Version: ${DEB_VERSION}
Architecture: arm64
Maintainer: Cix OS team
Depends: libc6 (>= 2.34)
Section: utils
Priority: optional
Description: Xenomai 4 EVL userspace libraries and tools
 This package contains libevl and EVL tools for real-time applications
 on Xenomai 4 EVL kernel.
EOF

chmod -R u+rwX,go+rX "${STAGING_ROOT}"
deb_path="${DEB_OUT}/${DEB_PKG_NAME}_${DEB_VERSION}_arm64.deb"
rm -f "${deb_path}"
dpkg-deb -Zxz --root-owner-group -b "${STAGING_ROOT}" "${deb_path}"

log "Build complete: ${deb_path}"
ls -lh "${deb_path}"
