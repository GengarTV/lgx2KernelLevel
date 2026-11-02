#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installer / Uninstaller for AVerMedia Live Gamer Extreme (GC550) Kernel Driver
# Usage:
#   ./install.sh             # Install via DKMS (or direct make install) + load module
#   ./install.sh --direct    # Compile & install for current kernel only (without DKMS)
#   ./install.sh --uninstall # Unload and remove driver, DKMS package, and udev rules

set -euo pipefail

PKG_NAME="avermedia-gc550"
PKG_VER="0.1.0"
MOD_NAME="gc550"
UDEV_RULE="99-avermedia-gc550.rules"
UDEV_DEST="/etc/udev/rules.d/${UDEV_RULE}"
DKMS_SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VER}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

info()  { printf "\033[1;34m[INFO]\033[0m %s\n" "$*"; }
ok()    { printf "\033[1;32m[ OK ]\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33m[WARN]\033[0m %s\n" "$*"; }
error() { printf "\033[1;31m[ERR ]\033[0m %s\n" "$*" >&2; exit 1; }

require_root() {
	if [[ "${EUID}" -ne 0 ]]; then
		info "Elevating privileges with sudo..."
		exec sudo "$0" "$@"
	fi
}

check_headers() {
	local kver
	kver="$(uname -r)"
	if [[ ! -d "/lib/modules/${kver}/build" ]]; then
		error "Kernel headers for ${kver} not found at /lib/modules/${kver}/build.
Please install your distribution's kernel headers first:
  - Arch / CachyOS: sudo pacman -S linux-headers (or linux-cachyos-headers)
  - Debian / Ubuntu: sudo apt install linux-headers-\$(uname -r)
  - Fedora: sudo dnf install kernel-devel-${kver}"
	fi
}

uninstall_driver() {
	require_root "$@"
	info "Uninstalling ${PKG_NAME} (${MOD_NAME})..."

	if lsmod | grep -q "^${MOD_NAME} "; then
		info "Unloading running kernel module ${MOD_NAME}..."
		modprobe -r "${MOD_NAME}" || rmmod "${MOD_NAME}" || true
	fi

	if command -v dkms >/dev/null 2>&1; then
		if dkms status -m "${PKG_NAME}" -v "${PKG_VER}" 2>/dev/null | grep -q "${PKG_NAME}"; then
			info "Removing DKMS module ${PKG_NAME}/${PKG_VER}..."
			dkms remove -m "${PKG_NAME}" -v "${PKG_VER}" --all || true
		fi
	fi

	if [[ -d "${DKMS_SRC_DIR}" ]]; then
		info "Removing ${DKMS_SRC_DIR}..."
		rm -rf "${DKMS_SRC_DIR}"
	fi

	local extra_ko="/lib/modules/$(uname -r)/updates/${MOD_NAME}.ko"
	local extra_ko_zst="/lib/modules/$(uname -r)/updates/${MOD_NAME}.ko.zst"
	rm -f "${extra_ko}" "${extra_ko_zst}"
	depmod -a

	if [[ -f "${UDEV_DEST}" ]]; then
		info "Removing udev rules ${UDEV_DEST}..."
		rm -f "${UDEV_DEST}"
		udevadm control --reload-rules && udevadm trigger || true
	fi

	ok "${PKG_NAME} has been completely uninstalled."
}

install_udev_rules() {
	if [[ -f "${SCRIPT_DIR}/${UDEV_RULE}" ]]; then
		info "Installing udev rule to ${UDEV_DEST}..."
		install -m 0644 "${SCRIPT_DIR}/${UDEV_RULE}" "${UDEV_DEST}"
		udevadm control --reload-rules && udevadm trigger || true
	fi
}

load_module() {
	info "Loading kernel dependencies (videodev, videobuf2, snd-pcm)..."
	modprobe videodev
	modprobe videobuf2-common
	modprobe videobuf2-v4l2
	modprobe videobuf2-vmalloc
	modprobe snd-pcm

	if lsmod | grep -q "^${MOD_NAME} "; then
		info "Reloading ${MOD_NAME}..."
		rmmod "${MOD_NAME}" || true
	fi

	info "Loading ${MOD_NAME} kernel module..."
	modprobe "${MOD_NAME}"
	ok "Module ${MOD_NAME} is loaded and active!"
}

install_dkms() {
	info "Installing ${PKG_NAME} v${PKG_VER} via DKMS..."

	if dkms status -m "${PKG_NAME}" -v "${PKG_VER}" 2>/dev/null | grep -q "${PKG_NAME}"; then
		info "Cleaning previous DKMS registration..."
		dkms remove -m "${PKG_NAME}" -v "${PKG_VER}" --all || true
	fi

	rm -rf "${DKMS_SRC_DIR}"
	mkdir -p "${DKMS_SRC_DIR}/kernel"

	cp "${SCRIPT_DIR}/Makefile" "${SCRIPT_DIR}/Kbuild" "${SCRIPT_DIR}/dkms.conf" "${DKMS_SRC_DIR}/"
	cp "${SCRIPT_DIR}/kernel/"*.c "${SCRIPT_DIR}/kernel/"*.h "${DKMS_SRC_DIR}/kernel/"

	dkms add -m "${PKG_NAME}" -v "${PKG_VER}"
	dkms build -m "${PKG_NAME}" -v "${PKG_VER}"
	dkms install -m "${PKG_NAME}" -v "${PKG_VER}" --force
}

install_direct() {
	info "Building and installing ${MOD_NAME} directly for kernel $(uname -r)..."
	make -C "${SCRIPT_DIR}" clean
	make -C "${SCRIPT_DIR}" modules
	make -C "${SCRIPT_DIR}" install
}

main() {
	local mode="auto"

	for arg in "$@"; do
		case "${arg}" in
			--uninstall|-u|uninstall)
				mode="uninstall"
				;;
			--direct|-d)
				mode="direct"
				;;
			--help|-h)
				echo "Usage: $0 [--direct | --uninstall | --help]"
				exit 0
				;;
			*)
				error "Unknown option: ${arg}"
				;;
		esac
	done

	if [[ "${mode}" == "uninstall" ]]; then
		uninstall_driver "$@"
		exit 0
	fi

	require_root "$@"
	check_headers

	if [[ "${mode}" == "auto" ]] && command -v dkms >/dev/null 2>&1; then
		install_dkms
	else
		if [[ "${mode}" == "auto" ]]; then
			warn "dkms not found; falling back to direct installation for kernel $(uname -r)."
		fi
		install_direct
	fi

	install_udev_rules
	load_module

	echo ""
	ok "AVerMedia Live Gamer Extreme (GC550) driver installation complete!"
	info "When you plug in your GC550 over USB 3.0, it will appear as /dev/videoX and an ALSA capture device."
}

main "$@"
