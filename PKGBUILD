# Maintainer: Øystein Lie <oeystein@gengartv.com>
# Contributor: Chris Sawczuk (ChrisAJS) - USB protocol reverse-engineering & lgx2userspace

pkgname=avermedia-gc550-dkms
_pkgname=avermedia-gc550
pkgver=0.1.2
pkgrel=1
pkgdesc="AVerMedia Live Gamer Extreme (GC550) V4L2 + ALSA USB 3.0 Capture Kernel Driver (DKMS)"
arch=('x86_64' 'aarch64')
url="https://github.com/GengarTV/lgx2KernelLevel"
license=('GPL-3.0-or-later')
depends=('dkms')
provides=('avermedia-gc550')
conflicts=('avermedia-gc550')

package() {
	local src_root="${startdir}"
	local dkms_dest="${pkgdir}/usr/src/${_pkgname}-${pkgver}"

	install -dm755 "${dkms_dest}/kernel"
	install -Dm644 "${src_root}/Makefile" "${dkms_dest}/Makefile"
	install -Dm644 "${src_root}/Kbuild" "${dkms_dest}/Kbuild"
	install -Dm644 "${src_root}/dkms.conf" "${dkms_dest}/dkms.conf"
	install -Dm644 "${src_root}/kernel/"*.c "${src_root}/kernel/"*.h "${dkms_dest}/kernel/"

	install -Dm644 "${src_root}/99-avermedia-gc550.rules" \
		"${pkgdir}/usr/lib/udev/rules.d/99-avermedia-gc550.rules"
	install -Dm644 "${src_root}/LICENCE.md" \
		"${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
