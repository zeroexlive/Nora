#!/usr/bin/env bash
# Build a Debian package for Nora with dpkg-deb.
# Usage: packaging/linux/build-deb.sh <build-dir> <version> <out-dir>
#   <build-dir> holds the nora and nora-cli binaries; <version> may start with "v".
set -euo pipefail

BUILD_DIR=${1:?build dir}
VERSION=${2:?version}
OUT_DIR=${3:?output dir}
VERSION=${VERSION#v}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
ARCH=$(dpkg --print-architecture 2>/dev/null || echo amd64)
PKG=nora
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
D="$STAGE/${PKG}_${VERSION}_${ARCH}"

install -Dm755 "$BUILD_DIR/nora"     "$D/usr/bin/nora"
install -Dm755 "$BUILD_DIR/nora-cli" "$D/usr/bin/nora-cli"
strip --strip-unneeded "$D/usr/bin/nora" "$D/usr/bin/nora-cli" 2>/dev/null || true

install -Dm644 "$HERE/nora.desktop" "$D/usr/share/applications/nora.desktop"
install -Dm644 "$HERE/nora.png"     "$D/usr/share/icons/hicolor/512x512/apps/nora.png"
install -Dm644 "$HERE/nora.png"     "$D/usr/share/pixmaps/nora.png"

DOC="$D/usr/share/doc/$PKG"
install -Dm644 "$ROOT/README.md" "$DOC/README.md"
cat > "$DOC/copyright" <<COPY
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Nora
Source: https://github.com/zeroexlive/Nora

Files: *
Copyright: zeroexlive
License: see /usr/share/doc/nora/LICENSE

Files: third_party/obj/*
License: see /usr/share/doc/nora/LICENSE.obj
COPY
install -Dm644 "$ROOT/LICENSE" "$DOC/LICENSE"
install -Dm644 "$ROOT/third_party/obj/LICENSE" "$DOC/LICENSE.obj"

SHARE="$D/usr/share/$PKG"
mkdir -p "$SHARE"
cp -r "$ROOT/examples" "$ROOT/scenes" "$SHARE/"
find "$SHARE" -type d -exec chmod 755 {} +
find "$SHARE" -type f -exec chmod 644 {} +

# Shared-library dependencies: computed with dpkg-shlibdeps when available,
# otherwise the known set for the X11/OpenGL GUI.
FALLBACK_DEPENDS="libc6 (>= 2.34), libx11-6, libgl1"
DEPENDS=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
  SHL=$(mktemp -d)
  mkdir -p "$SHL/debian"
  printf 'Source: nora\n\nPackage: nora\nArchitecture: any\n' > "$SHL/debian/control"
  DEPENDS=$(cd "$SHL" && dpkg-shlibdeps -O "$D/usr/bin/nora" "$D/usr/bin/nora-cli" 2>/dev/null \
            | sed -n 's/^shlibs:Depends=//p') || DEPENDS=""
  rm -rf "$SHL"
fi
[ -n "$DEPENDS" ] || DEPENDS="$FALLBACK_DEPENDS"
# Make sure the GUI runtime libraries are always declared.
for dep in libx11-6 libgl1; do
  case ", $DEPENDS," in *", $dep"[\ ,]*) ;; *) DEPENDS="$DEPENDS, $dep" ;; esac
done

SIZE=$(du -sk "$D/usr" | cut -f1)
mkdir -p "$D/DEBIAN"
cat > "$D/DEBIAN/control" <<CTRL
Package: $PKG
Version: $VERSION
Section: graphics
Priority: optional
Architecture: $ARCH
Maintainer: zeroexlive <84627588+zeroexlive@users.noreply.github.com>
Installed-Size: $SIZE
Depends: $DEPENDS
Homepage: https://github.com/zeroexlive/Nora
Description: Physically based renderer with Metropolis light transport
 Nora is a physically based renderer with PSSMLT over a path tracer with
 next event estimation, RGB and spectral (hero wavelength) modes, a built-in
 OSL-subset shader compiler, a node shader editor and an interactive GUI.
 .
 This package installs the GUI (nora) and the command line renderer
 (nora-cli). Example scenes and shaders are in /usr/share/nora.
CTRL

mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/${PKG}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$D" "$OUT"
dpkg-deb -I "$OUT"
dpkg-deb -c "$OUT"
