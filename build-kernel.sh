#!/usr/bin/env bash
#
# Xenomai 4 (EVL) kernel build script.
#
# Typical usage (aarch64 cross-build from a Linux host):
#
#   export CROSS_COMPILE=aarch64-none-linux-gnu-
#   export PATH=/path/to/toolchain/bin:$PATH
#   ./build-kernel.sh
#
# Suffix lives in cix.config (CONFIG_LOCALVERSION="-cix-xenomai4").
# Pass LOCALVERSION= (set but empty) so setlocalversion does not append '+'.
# A non-empty LOCALVERSION here would stack a second suffix.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

: "${KERNEL_SRC:=${SCRIPT_DIR}}"
KERNEL_SRC="$(cd "${KERNEL_SRC}" && pwd)"

: "${CROSS_COMPILE:=aarch64-none-linux-gnu-}"

# When non-empty, prepended to PATH (e.g. .../gcc-arm-*/bin)
: "${TOOLCHAIN_BIN:=}"

: "${JOBS:=$(nproc 2>/dev/null || echo 4)}"

: "${KERNEL_CONFIG_CMD:=defconfig cix.config}"

: "${KERNEL_TARGETS:=Image modules dtbs}"

: "${KERNEL_BIN_DEB:=1}"

: "${OUTPUT_DEBS:=$(dirname "${SCRIPT_DIR}")/xenomai4-out/debs}"

log() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

ensure_toolchain() {
    if [[ -n "${TOOLCHAIN_BIN}" ]]; then
        export PATH="${TOOLCHAIN_BIN}:${PATH}"
    fi
    command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 \
        || die "Cross compiler not found: ${CROSS_COMPILE}gcc (set PATH or TOOLCHAIN_BIN)"
}

kernel_prepare_config() {
    cd "${KERNEL_SRC}"
    # wipe .config so defconfig + cix.config merge fresh
    rm -f .config .config.old
    log "Cleaning stale .config, running: make ... ${KERNEL_CONFIG_CMD}"
    # shellcheck disable=SC2086
    make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= ${KERNEL_CONFIG_CMD}
    local opt
    for opt in CONFIG_IRQ_PIPELINE CONFIG_DOVETAIL CONFIG_EVL; do
        grep -q "^${opt}=y" .config || die "${opt} is not set - check cix.config"
    done
}

build_kernel() {
    ensure_toolchain
    cd "${KERNEL_SRC}"
    kernel_prepare_config
    log "Building Xenomai 4 (EVL) kernel (${KERNEL_TARGETS}) ..."
    # shellcheck disable=SC2086
    make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= -j"${JOBS}" ${KERNEL_TARGETS}

    if [[ "${KERNEL_BIN_DEB}" == "1" ]]; then
        log "Building kernel Debian packages (bindeb-pkg) ..."
        mkdir -p "${OUTPUT_DEBS}"
        OUTPUT_DEBS="$(cd "${OUTPUT_DEBS}" && pwd)"
        local deb_parent
        deb_parent="$(cd "$(dirname "${KERNEL_SRC}")" && pwd)"
        # wipe both dirs so a stale deb cannot overwrite this run's package
        rm -f "${deb_parent}"/linux-*.deb "${OUTPUT_DEBS}"/linux-*.deb
        make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= -j"${JOBS}" bindeb-pkg
        shopt -s nullglob
        local f
        for f in "${deb_parent}"/linux-*.deb; do
            mv -f "${f}" "${OUTPUT_DEBS}/"
            log " -> ${OUTPUT_DEBS}/$(basename "${f}")"
        done
        shopt -u nullglob

        log "Cleaning up deb package names..."
        cd "${OUTPUT_DEBS}"
        for f in linux-*.deb; do
            local newname
            newname=$(echo "$f" | sed -E 's/-g?[0-9]*-?g?[a-f0-9]{7,}-[0-9]+_/_/')
            if [[ "$f" != "$newname" ]]; then
                mv -f "$f" "$newname"
                log "    Renamed: $f -> $newname"
            fi
        done

        # x86_64 bindeb-pkg leaves host fixdep/modpost; board DKMS needs
        # aarch64 copies from build-scripts/debian/dkms/linux/scripts.
        local prebuilt_dir
        prebuilt_dir="${PARENT_DIR}/build-scripts/debian/dkms/linux/scripts"
        if [[ ! -d "${prebuilt_dir}" ]]; then
            log "WARN: prebuilt dir not found: ${prebuilt_dir}"
            log "WARN: headers deb will lack aarch64 host tools; DKMS will fail"
        else
            log "Overwriting headers deb scripts/ with prebuilt aarch64 host tools ..."
            for f in linux-headers-*.deb; do
                [[ -e "$f" ]] || continue
                local ov_tmpdir ov_extract
                ov_tmpdir="$(mktemp -d)"
                ov_extract="${ov_tmpdir}/extract"
                mkdir -p "${ov_extract}/DEBIAN"
                dpkg-deb -x "$f" "${ov_extract}"
                cp -rfp "${prebuilt_dir}"/* \
                    "${ov_extract}"/usr/src/linux-headers-*/scripts/
                dpkg-deb -e "$f" "${ov_extract}/DEBIAN"
                chmod -R u+rwX,go+rX "${ov_extract}"
                dpkg-deb -Zxz --root-owner-group -b "${ov_extract}" "${f}.tmp"
                mv -f "${f}.tmp" "${f}"
                rm -rf "${ov_tmpdir}"
            done
        fi

        # drop leftover x86_64 scripts/* and -ftrivial-auto-var-init=zero
        for f in linux-headers-*.deb; do
            [[ -e "$f" ]] || continue
            log "Cleaning x86_64 host binaries from $f ..."
            local tmpdir
            tmpdir=$(mktemp -d)
            dpkg-deb -R "$f" "$tmpdir"

            local headers_dir
            headers_dir=$(find "$tmpdir/usr/src" -maxdepth 1 -type d -name "linux-headers-*" | head -1)
            if [[ -n "$headers_dir" ]] && [[ -d "$headers_dir/scripts" ]]; then
                local scripts_dir="$headers_dir/scripts"
                find "$scripts_dir" -type f | while read elf_file; do
                    if file "$elf_file" 2>/dev/null | grep -qE "ELF.*x86-64"; then
                        log "    Removing x86_64 ELF: $(basename "$elf_file")"
                        rm -f "$elf_file"
                    fi
                done
                find "$scripts_dir" -name ".*.cmd" -delete
            fi

            if [[ -n "$headers_dir" ]] && [[ -f "$headers_dir/Makefile" ]]; then
                log "    Removing -ftrivial-auto-var-init=zero from Makefile"
                sed -i '/KBUILD_CFLAGS.*-ftrivial-auto-var-init=zero/d' "$headers_dir/Makefile"
            fi

            dpkg-deb -Zxz --root-owner-group -b "$tmpdir" "$f"
            rm -rf "$tmpdir"
        done
    fi
    log "Xenomai 4 (EVL) kernel build finished!"
}

build_kernel
