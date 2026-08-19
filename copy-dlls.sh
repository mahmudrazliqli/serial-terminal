#!/usr/bin/env bash
# Run in MSYS2 MinGW64 after `make`. Creates win-runtime/ for NSIS.
set -e
rm -rf win-runtime
mkdir -p win-runtime/lib win-runtime/share

for dll in $(ldd serialterm.exe | awk '/mingw64/{print $3}'); do
  cp -n "$dll" win-runtime/
done

cp -r /mingw64/lib/gdk-pixbuf-2.0 win-runtime/lib/  2>/dev/null || true
cp -r /mingw64/lib/gtk-3.0       win-runtime/lib/  2>/dev/null || true
cp -r /mingw64/share/glib-2.0    win-runtime/share/ 2>/dev/null || true
cp -r /mingw64/share/icons       win-runtime/share/ 2>/dev/null || true
cp -r /mingw64/share/themes      win-runtime/share/ 2>/dev/null || true
cp -r /mingw64/share/locale      win-runtime/share/ 2>/dev/null || true
echo "Runtime ready. Build installer with: makensis installer.nsi"
