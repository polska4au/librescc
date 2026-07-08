pkgname=librescc
pkgver=1.0.0
pkgrel=1
pkgdesc="MIDI to SCC-inspired chiptune player with real-time synthesis and a GTK4 GUI"
arch=('x86_64' 'aarch64')
url="https://github.com/polska4au/librescc"
license=('GPL-3.0-or-later')
depends=('gtk4' 'portaudio' 'lame')
makedepends=('cmake' 'gcc' 'pkgconf')
source=("$pkgname-$pkgver.tar.gz::https://github.com/polska4au/librescc/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('653af569c349fd76037b9a9bd7614f25f49dfbf3c430fda8eb2344fb3684ec3e')

build() {
    cmake -B build -S "$pkgname-$pkgver" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build --parallel
}

check() {
    ctest --test-dir build --output-on-failure
}

package() {
    DESTDIR="$pkgdir" cmake --install build
}
