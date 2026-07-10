#!/bin/bash
# Build armcb-isp-dkms .deb (DKMS source only; armcb_isp.ko built on target via dkms).
#
# Usage: ./build-deb.sh
# Output: packaging/armcb-isp-dkms_<ver>_all.deb

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_NAME="armcb-isp-dkms"
DKMS_CONF="$SRC_DIR/dkms.conf"
PKG_VER="$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)"$/\1/p' "$DKMS_CONF" | head -n1)"
DKMS_NAME="$(sed -n 's/^PACKAGE_NAME="\([^"]*\)"$/\1/p' "$DKMS_CONF" | head -n1)"

if [[ -z "$PKG_VER" || -z "$DKMS_NAME" ]]; then
    echo "ERROR: parse PACKAGE_NAME/PACKAGE_VERSION from $DKMS_CONF" >&2
    exit 1
fi

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: '$1' not found. Install: sudo apt install $2" >&2
        exit 1
    }
}
need dpkg-buildpackage dpkg-dev
need dh debhelper
need rsync rsync

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

STAGE="$WORK_DIR/${PKG_NAME}-${PKG_VER}"
mkdir -p "$STAGE"

rsync -a \
    --exclude='*.o' --exclude='*.ko' --exclude='*.mod' --exclude='*.mod.c' \
    --exclude='.*.cmd' --exclude='*.cmd' --exclude='*.bak' \
    --exclude='*.o.d' --exclude='*.a' \
    --exclude='Module.symvers' --exclude='modules.order' \
    --exclude='.git*' --exclude='packaging' \
    "$SRC_DIR/" "$STAGE/"

cp -r "$SCRIPT_DIR/debian" "$STAGE/"
chmod 755 "$STAGE/debian/postinst" "$STAGE/debian/prerm" 2>/dev/null || true

cd "$STAGE"
dpkg-buildpackage -us -uc -b

mv "$WORK_DIR"/*.deb "$SCRIPT_DIR/"
echo
echo "Built DKMS package (install on board, then dkms builds armcb_isp.ko):"
ls -la "$SCRIPT_DIR"/${PKG_NAME}_*.deb
