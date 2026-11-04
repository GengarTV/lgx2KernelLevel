#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Smart Installer / Uninstaller for AVerMedia Live Gamer Extreme (GC550) Kernel Driver
#
# Automatically selects the best installation method for your Linux distribution:
#   - Arch Linux / CachyOS / Manjaro / EndeavourOS:
#       Builds and installs a native pacman package (`avermedia-gc550-dkms`) via `makepkg -si`,
#       so pacman owns all files, installs `dkms` automatically, and rebuilds `gc550.ko`
#       via pacman hooks on every kernel update.
#   - Other Linux distributions (Ubuntu, Debian, Fedora, openSUSE, etc.):
#       Compiles and installs `gc550.ko` (via DKMS if present, or direct kernel build)
#       and installs the udev rules.
#
# Usage:
#   ./install.sh             # Auto-detect distro and install using best method
#   ./install.sh --arch      # Force Arch Linux / CachyOS PKGBUILD (makepkg -si) install
#   ./install.sh --dkms      # Force generic DKMS install (non-pacman)
#   ./install.sh --direct    # Compile & install directly for current kernel only
#   ./install.sh --uninstall # Unload and remove driver, package/DKMS, and udev rules

set -euo pipefail

PKG_NAME="avermedia-gc550"
ARCH_PKG_NAME="avermedia-gc550-dkms"
PKG_VER="0.1.2"
MOD_NAME="gc550"
UDEV_RULE="99-avermedia-gc550.rules"
UDEV_DEST="/etc/udev/rules.d/${UDEV_RULE}"
DKMS_SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VER}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

info()  { printf "\033[1;34m[INFO]\033[0m %s\n" "$*"; }
ok()    { printf "\033[1;32m[ OK ]\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33m[WARN]\033[0m %s\n" "$*"; }
error() { printf "\033[1;31m[ERR ]\033[0m %s\n" "$*" >&2; exit 1; }

is_arch_distro() {
	if [[ -f /etc/arch-release ]] || command -v pacman >/dev/null 2>&1; then
		return 0
	fi
	if [[ -f /etc/os-release ]]; then
		# shellcheck disable=SC1091
		source /etc/os-release
		if [[ "${ID:-}" == "arch" || "${ID:-}" == "cachyos" || "${ID_LIKE:-}" == *"arch"* ]]; then
			return 0
		fi
	fi
	return 1
}

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

load_module() {
	info "Loading kernel dependencies (videodev, videobuf2, snd-pcm)..."
	sudo modprobe videodev
	sudo modprobe videobuf2-common
	sudo modprobe videobuf2-v4l2
	sudo modprobe videobuf2-vmalloc
	sudo modprobe snd-pcm

	if lsmod | grep -q "^${MOD_NAME} "; then
		info "Reloading ${MOD_NAME}..."
		sudo rmmod "${MOD_NAME}" || true
	fi

	info "Loading ${MOD_NAME} kernel module..."
	sudo modprobe "${MOD_NAME}"
	ok "Module ${MOD_NAME} is loaded and active!"
}

install_arch_pkgbuild() {
	info "Detected Arch-based system ($(source /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-Arch Linux}"))."
	info "Using native pacman + DKMS installation via PKGBUILD (${ARCH_PKG_NAME})..."

	if [[ "${EUID}" -eq 0 ]]; then
		if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
			warn "makepkg cannot run as root; dropping back to user '${SUDO_USER}' for package build..."
			sudo -u "${SUDO_USER}" bash -c "cd '${SCRIPT_DIR}' && makepkg -si --needed --noconfirm"
		else
			error "Do not run ./install.sh as root on Arch/CachyOS (makepkg forbids running as root). Run './install.sh' as your normal user and it will prompt for sudo automatically."
		fi
	else
		(
			cd "${SCRIPT_DIR}"
			makepkg -si --needed
		)
	fi

	# Clean temporary makepkg build artifacts so git working tree stays clean
	rm -rf "${SCRIPT_DIR}/pkg" "${SCRIPT_DIR}/src" "${SCRIPT_DIR}/"*.pkg.tar*

	sudo udevadm control --reload-rules && sudo udevadm trigger || true
	load_module
}

install_udev_rules() {
	if [[ -f "${SCRIPT_DIR}/${UDEV_RULE}" ]]; then
		info "Installing udev rule to ${UDEV_DEST}..."
		install -m 0644 "${SCRIPT_DIR}/${UDEV_RULE}" "${UDEV_DEST}"
		udevadm control --reload-rules && udevadm trigger || true
	fi
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

uninstall_driver() {
	info "Uninstalling ${PKG_NAME} (${MOD_NAME})..."

	if lsmod | grep -q "^${MOD_NAME} "; then
		info "Unloading running kernel module ${MOD_NAME}..."
		sudo modprobe -r "${MOD_NAME}" || sudo rmmod "${MOD_NAME}" || true
	fi

	# 1. If installed via pacman (Arch/CachyOS PKGBUILD), let pacman remove it cleanly
	if command -v pacman >/dev/null 2>&1 && pacman -Qi "${ARCH_PKG_NAME}" >/dev/null 2>&1; then
		info "Removing pacman package ${ARCH_PKG_NAME}..."
		sudo pacman -Rns "${ARCH_PKG_NAME}"
	fi

	# 2. Clean up any standalone DKMS or direct installations if present
	if command -v dkms >/dev/null 2>&1; then
		if dkms status -m "${PKG_NAME}" -v "${PKG_VER}" 2>/dev/null | grep -q "${PKG_NAME}"; then
			info "Removing DKMS module ${PKG_NAME}/${PKG_VER}..."
			sudo dkms remove -m "${PKG_NAME}" -v "${PKG_VER}" --all || true
		fi
	fi

	if [[ -d "${DKMS_SRC_DIR}" ]]; then
		info "Removing ${DKMS_SRC_DIR}..."
		sudo rm -rf "${DKMS_SRC_DIR}"
	fi

	local extra_ko="/lib/modules/$(uname -r)/updates/${MOD_NAME}.ko"
	local extra_ko_zst="/lib/modules/$(uname -r)/updates/${MOD_NAME}.ko.zst"
	sudo rm -f "${extra_ko}" "${extra_ko_zst}"
	sudo depmod -a

	if [[ -f "${UDEV_DEST}" ]]; then
		info "Removing ${UDEV_DEST}..."
		sudo rm -f "${UDEV_DEST}"
	fi

	sudo udevadm control --reload-rules && sudo udevadm trigger || true
	ok "${PKG_NAME} has been completely uninstalled."
}

main() {
	local mode="auto"

	for arg in "$@"; do
		case "${arg}" in
			--uninstall|-u|uninstall)
				mode="uninstall"
				;;
			--arch)
				mode="arch"
				;;
			--dkms)
				mode="dkms"
				;;
			--direct|-d)
				mode="direct"
				;;
			--help|-h)
				echo "Usage: $0 [--arch | --dkms | --direct | --uninstall | --help]"
				exit 0
				;;
			*)
				error "Unknown option: ${arg}"
				;;
		esac
	done

	if [[ "${mode}" == "uninstall" ]]; then
		uninstall_driver
		exit 0
	fi

	check_headers

	if [[ "${mode}" == "auto" ]]; then
		if is_arch_distro && command -v makepkg >/dev/null 2>&1; then
			mode="arch"
		else
			mode="direct"
		fi
	fi

	case "${mode}" in
		arch)
			install_arch_pkgbuild
			;;
		dkms)
			require_root "$@"
			install_dkms
			install_udev_rules
			load_module
			;;
		direct)
			require_root "$@"
			info "Non-Arch distribution detected (or --direct selected); compiling module for kernel $(uname -r)..."
			install_direct
			install_udev_rules
			load_module
			;;
	esac

	echo ""
	ok "AVerMedia Live Gamer Extreme (GC550) driver installation complete!"
	info "When you plug in your GC550 over USB 3.0, it will appear as /dev/videoX (/dev/avermedia-gc550) and an ALSA capture device."
}

main "$@"
