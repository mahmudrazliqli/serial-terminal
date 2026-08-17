#!/usr/bin/env bash
#
# Build a .deb package for the serial terminal.
#
# Requires: gcc, pkg-config, make, dpkg-deb (all present on stock Debian/Ubuntu).
# Run from the project directory:  ./make-deb.sh   (or:  make deb)
#
set -euo pipefail
cd "$(dirname "$0")"

VERSION="1.1.0"
ARCH="$(dpkg --print-architecture)"
PKG="serial-terminal_${VERSION}_${ARCH}.deb"
STAGE="build-deb"

echo "==> Building $PKG (arch $ARCH)"

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
mkdir -p "$STAGE/usr/bin"
mkdir -p "$STAGE/usr/share/serial-terminal"
mkdir -p "$STAGE/usr/share/applications"
mkdir -p "$STAGE/usr/share/doc/serial-terminal"
mkdir -p "$STAGE/usr/share/icons/hicolor/scalable/apps"

echo "==> Compiling (PREFIX=/usr)"
make clean >/dev/null 2>&1 || true
make PREFIX=/usr

echo "==> Staging files"
install -m755 serial-terminal "$STAGE/usr/bin/serial-terminal"
install -m644 window1.glade "$STAGE/usr/share/serial-terminal/window1.glade"
install -m644 debian/serial-terminal.desktop "$STAGE/usr/share/applications/serial-terminal.desktop"
install -m644 debian/serial-terminal.svg "$STAGE/usr/share/icons/hicolor/scalable/apps/serial-terminal.svg"
install -m644 debian/copyright "$STAGE/usr/share/doc/serial-terminal/copyright"
gzip -9 -n -c debian/changelog > "$STAGE/usr/share/doc/serial-terminal/changelog.gz"

sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" \
    debian/control > "$STAGE/DEBIAN/control"

echo "==> Building .deb"
dpkg-deb --build --root-owner-group "$STAGE" "$PKG"

rm -rf "$STAGE"

echo
echo "==> Done: $PKG"
dpkg-deb --info "$PKG" | sed -n '1,12p'
