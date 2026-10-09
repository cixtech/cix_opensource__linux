#!/usr/bin/env bash
#
# Xenomai 3 (Cobalt) userspace build script
# Builds libcobalt and Xenomai APIs for ARM64
#
# Typical usage:
#   export CROSS_COMPILE=aarch64-none-linux-gnu-
#   export PATH=/path/to/toolchain/bin:$PATH
#   ./build-xenomai3-userspace.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

# ---------------------------------------------------------------------------
# Defaults (override via environment)
# ---------------------------------------------------------------------------
: "${XENOMAI_SRC:=${PARENT_DIR}/xenomai3-src}"
: "${XENOMAI_GIT:=https://gitlab.com/Xenomai/xenomai3/xenomai.git}"
: "${XENOMAI_BRANCH:=stable/v3.3.x}"

# Script is inside kernel source directory
: "${KERNEL_SRC:=${SCRIPT_DIR}}"

: "${CROSS_COMPILE:=aarch64-none-linux-gnu-}"

# Output directories (at parent level)
: "${STAGING_ROOT:=${PARENT_DIR}/xenomai3-out/staging}"
: "${DEB_OUT:=${PARENT_DIR}/xenomai3-out/debs}"

: "${DEB_PKG_NAME:=cix-libcobalt-xenomai3}"
: "${DEB_VERSION:=3.3.2}"

: "${JOBS:=$(nproc 2>/dev/null || echo 4)}"

log() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# Clone Xenomai from git
clone_xenomai_if_needed() {
    if [[ -d "${XENOMAI_SRC}/.git" ]] || [[ -f "${XENOMAI_SRC}/scripts/prepare-kernel.sh" ]]; then
        return 0
    fi
    if [[ -d "${XENOMAI_SRC}" ]] && [[ -z "$(ls -A "${XENOMAI_SRC}" 2>/dev/null)" ]]; then
        rmdir "${XENOMAI_SRC}" 2>/dev/null || true
    fi
    log "Cloning Xenomai 3 into ${XENOMAI_SRC} ..."
    mkdir -p "$(dirname "${XENOMAI_SRC}")"
    git clone --progress -b "${XENOMAI_BRANCH}" "${XENOMAI_GIT}" "${XENOMAI_SRC}"
}

# Check sources
clone_xenomai_if_needed
[[ -d "${XENOMAI_SRC}" ]] || die "Xenomai source not found at ${XENOMAI_SRC}"
[[ -f "${XENOMAI_SRC}/configure" ]] || [[ -f "${XENOMAI_SRC}/configure.ac" ]] || die "Xenomai configure not found"
[[ -d "${KERNEL_SRC}" ]] || die "Kernel source not found at ${KERNEL_SRC}"

log "============================================="
log "Xenomai 3 Userspace Build"
log "============================================="
log "Xenomai source: ${XENOMAI_SRC}"
log "Kernel source:  ${KERNEL_SRC}"
log "Staging dir:    ${STAGING_ROOT}"
log "Deb output:     ${DEB_OUT}"
log "============================================="

# Clean and create staging
rm -rf "${STAGING_ROOT}"
mkdir -p "${STAGING_ROOT}" "${DEB_OUT}" "${XENOMAI_SRC}/build"

# Bootstrap if needed (if there's no configure)
if [[ ! -f "${XENOMAI_SRC}/configure" ]]; then
    log "Running autoreconf..."
    (cd "${XENOMAI_SRC}" && autoreconf -vif)
fi

# Configure
log "Running configure for ARM64..."
cd "${XENOMAI_SRC}/build"
"${XENOMAI_SRC}/configure" \
    --host=aarch64-none-linux-gnu \
    --with-core=cobalt \
    --prefix=/usr/xenomai \
    --enable-smp \
    --enable-pshared \
    CC="${CROSS_COMPILE}gcc" \
    CXX="${CROSS_COMPILE}g++"

# Build
log "Building (JOBS=${JOBS})..."
make -j"${JOBS}"

# Install to staging
log "Installing to staging directory..."
make DESTDIR="${STAGING_ROOT}" install

# Fix .la files for cross-build staging paths
libdir_staging="${STAGING_ROOT}/usr/xenomai/lib"
if [[ -d "${libdir_staging}" ]]; then
    shopt -s nullglob
    for la in "${libdir_staging}"/*.la; do
        [[ -f "$la" ]] && sed -i "s#/usr/xenomai#${STAGING_ROOT}/usr/xenomai#g" "$la"
    done
    shopt -u nullglob
fi

# Build DEB package
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
Description: Xenomai 3 Cobalt userspace libraries and tools
 This package contains libcobalt and Xenomai APIs (Alchemy, Copperplate, etc.)
 for real-time applications on Xenomai 3 Cobalt kernel.
EOF

chmod -R u+rwX,go+rX "${STAGING_ROOT}"
deb_path="${DEB_OUT}/${DEB_PKG_NAME}_${DEB_VERSION}_arm64.deb"
rm -f "${deb_path}"
dpkg-deb -Zxz --root-owner-group -b "${STAGING_ROOT}" "${deb_path}"

log "============================================="
log "Build complete!"
log "============================================="
log "DEB package: ${deb_path}"
log "Staging files: ${STAGING_ROOT}/usr/xenomai/"
ls -lh "${deb_path}"
