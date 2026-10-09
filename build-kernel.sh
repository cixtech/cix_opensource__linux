#!/usr/bin/env bash
#
# Xenomai 3 (Cobalt) kernel build script
# Kernel already has Xenomai 3 integrated (Cobalt + Dovetail)
#
# Typical usage (aarch64 cross-build from a Linux host):
#
#   export CROSS_COMPILE=aarch64-none-linux-gnu-
#   export PATH=/path/to/toolchain/bin:$PATH
#   ./build-kernel.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

# ---------------------------------------------------------------------------
# Defaults (override via environment)
# ---------------------------------------------------------------------------
# Script is inside kernel source directory, so use SCRIPT_DIR directly
: "${KERNEL_SRC:=${SCRIPT_DIR}}"
KERNEL_SRC="$(cd "${KERNEL_SRC}" && pwd)"

: "${CROSS_COMPILE:=aarch64-none-linux-gnu-}"

# When non-empty, prepended to PATH (e.g. .../gcc-arm-*/bin)
: "${TOOLCHAIN_BIN:=}"

: "${JOBS:=$(nproc 2>/dev/null || echo 4)}"

# Kernel configuration
: "${KERNEL_CONFIG_CMD:=defconfig cix.config}"

# Build targets
: "${KERNEL_TARGETS:=Image modules dtbs}"

# If 1, build .deb packages
: "${KERNEL_BIN_DEB:=1}"

# Output directory for .deb packages (output at parent level)
: "${OUTPUT_DEBS:=$(dirname "${SCRIPT_DIR}")/xenomai3-out/debs}"

log() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

ensure_toolchain() {
    if [[ -n "${TOOLCHAIN_BIN}" ]]; then
        export PATH="${TOOLCHAIN_BIN}:${PATH}"
    fi
    command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 \
        || die "Cross compiler not found: ${CROSS_COMPILE}gcc (set PATH or TOOLCHAIN_BIN)"
}

kernel_enable_xenomai_config() {
    cd "${KERNEL_SRC}"
    # Always start from a clean config so defconfig + cix.config are merged
    # fresh — otherwise stale .config from a prior build skips the
    # cix.config fragment and our cix-specific options never land.
    rm -f .config .config.old
    log "Cleaning stale .config, running: make ... LOCALVERSION= ${KERNEL_CONFIG_CMD}"
    # shellcheck disable=SC2086
    make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= ${KERNEL_CONFIG_CMD}
    if [[ -f ./scripts/config ]]; then
        # Enable Xenomai/Cobalt
        if ! grep -q '^CONFIG_XENOMAI=y' .config 2>/dev/null; then
            ./scripts/config --enable CONFIG_XENOMAI
        fi
        # Enable Dovetail (interrupt pipeline)
        if ! grep -q '^CONFIG_DOVETAIL=y' .config 2>/dev/null; then
            ./scripts/config --enable CONFIG_DOVETAIL
        fi
        make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= olddefconfig
    else
        die "Kernel tree has no scripts/config; cannot enable Xenomai automatically"
    fi
}

build_kernel() {
    ensure_toolchain
    cd "${KERNEL_SRC}"
    kernel_enable_xenomai_config
    log "Building Xenomai 3 kernel (${KERNEL_TARGETS}) ..."
    # shellcheck disable=SC2086
    make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= -j"${JOBS}" ${KERNEL_TARGETS}

    if [[ "${KERNEL_BIN_DEB}" == "1" ]]; then
        log "Building kernel Debian packages (bindeb-pkg) ..."
        mkdir -p "${OUTPUT_DEBS}"
        OUTPUT_DEBS="$(cd "${OUTPUT_DEBS}" && pwd)"
        local deb_parent
        deb_parent="$(cd "$(dirname "${KERNEL_SRC}")" && pwd)"
        rm -f "${deb_parent}"/linux-*.deb "${OUTPUT_DEBS}"/linux-*.deb
        make ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= -j"${JOBS}" bindeb-pkg
        shopt -s nullglob
        local f
        for f in "${deb_parent}"/linux-*.deb; do
            mv -f "${f}" "${OUTPUT_DEBS}/"
            log " -> ${OUTPUT_DEBS}/$(basename "${f}")"
        done
        shopt -u nullglob

        # Rename deb packages to remove git version hash
        # Handles both formats:
        #   -ge0b20dfbb767-2_ (g prefix hash)
        #   -03636-gf54d4fef73a5-2_ (number-g hash-number format)
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

        # bindeb-pkg on an x86_64 host leaves x86_64 host tools (fixdep,
        # modpost) in the headers deb, but DKMS on the target needs aarch64
        # ones; overlay the prebuilt tree when present. The strip step below
        # matches x86_64 ELF only, so it leaves these alone.
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
                # prebuilt tree's basic/, mod/, etc. mirror deb's scripts/ layout
                cp -rfp "${prebuilt_dir}"/* \
                    "${ov_extract}"/usr/src/linux-headers-*/scripts/
                # Preserve control metadata
                dpkg-deb -e "$f" "${ov_extract}/DEBIAN"
                chmod -R u+rwX,go+rX "${ov_extract}"
                dpkg-deb -Zxz --root-owner-group -b "${ov_extract}" "${f}.tmp"
                mv -f "${f}.tmp" "${f}"
                rm -rf "${ov_tmpdir}"
            done
        fi

        # Remove x86_64 host binaries from linux-headers package
        # These are built with host gcc during cross-compilation
        # and won't work on aarch64 target systems. They will
        # be automatically rebuilt with target gcc if needed.
        for f in linux-headers-*.deb; do
            [[ -e "$f" ]] || continue
            log "Cleaning x86_64 host binaries from $f ..."
            local tmpdir
            tmpdir=$(mktemp -d)
            dpkg-deb -R "$f" "$tmpdir"

            # Find and remove x86_64 ELF executables and object files
            # Use wildcard pattern to match actual kernel header directory name
            local headers_dir
            headers_dir=$(find "$tmpdir/usr/src" -maxdepth 1 -type d -name "linux-headers-*" | head -1)
            if [[ -n "$headers_dir" ]] && [[ -d "$headers_dir/scripts" ]]; then
                local scripts_dir="$headers_dir/scripts"
                # Remove x86_64 ELF files (executables and .o files)
                find "$scripts_dir" -type f | while read elf_file; do
                    if file "$elf_file" 2>/dev/null | grep -qE "ELF.*x86-64"; then
                        log "    Removing x86_64 ELF: $(basename "$elf_file")"
                        rm -f "$elf_file"
                    fi
                done
                # Remove .cmd files that reference host build paths
                find "$scripts_dir" -name ".*.cmd" -delete
            fi

            # Remove -ftrivial-auto-var-init=zero flag from Makefile
            # This flag is not supported by older GCC versions on target systems
            if [[ -n "$headers_dir" ]] && [[ -f "$headers_dir/Makefile" ]]; then
                log "    Removing -ftrivial-auto-var-init=zero from Makefile"
                sed -i '/KBUILD_CFLAGS.*-ftrivial-auto-var-init=zero/d' "$headers_dir/Makefile"
            fi

            # Repackage deb
            dpkg-deb -Zxz --root-owner-group -b "$tmpdir" "$f"
            rm -rf "$tmpdir"
        done
    fi
    log "Xenomai 3 kernel build finished!"
}

build_kernel
