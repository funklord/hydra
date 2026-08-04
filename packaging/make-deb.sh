#!/bin/bash
# Build a .deb from an already-built tree.
#
#   packaging/make-deb.sh <built-binary> <staging-dir> <output-dir> [version]
#
# Normally reached as `make deb`.
#
# **The dependency list is computed, not written down.** `dpkg-shlibdeps` reads
# the binary, resolves every shared library it actually links to the Debian
# package owning it, and gives back a versioned `Depends:`. A hand-kept list
# would be wrong the first time an optional dependency is switched on -- this
# build picks up libsecret, libsodium, liblz4 and libtorrent only if they are
# found at configure time, so the correct list is a property of the binary in
# front of us and of nothing else.
#
# The icon cache and desktop database are deliberately *not* refreshed here.
# Doing that while staging would touch the build host, and on the installing
# machine dpkg's own triggers handle both -- `hicolor-icon-theme` and
# `desktop-file-utils` ship them, and they run after unpacking, which is the
# only moment it is correct to run them.
set -eu

BIN=${1:?usage: make-deb.sh <binary> <staging-dir> <output-dir> [version]}
STAGE=${2:?}
OUT=${3:?}
VERSION=${4:-0.1-1}

here="$(cd "$(dirname "$0")/.." && pwd)"
# **Absolute, before anything changes directory.** dpkg-shlibdeps has to run
# from a directory holding a debian/control, so it is invoked after a `cd` --
# and a relative staging path silently stops resolving at that point. The
# symptom is an empty dependency list, which is to say a package that installs
# and then does not start.
mkdir -p "$(dirname "$2")" "$3"
STAGE=$(cd "$(dirname "$STAGE")" && pwd)/$(basename "$STAGE")
OUT=$(cd "$OUT" && pwd)
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

arch=$(dpkg-architecture -qDEB_HOST_ARCH 2>/dev/null || dpkg --print-architecture)

[ -x "$BIN" ] || { echo "no binary at $BIN -- run make first"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/usr/bin" "$STAGE/DEBIAN" "$OUT"

install -Dm755 "$BIN" "$STAGE/usr/bin/hydra"
# The icons and the desktop entry, through the same script `make install` uses,
# so a deb and a local install cannot disagree about where things go.
HYDRA_SKIP_CACHE_UPDATE=1 sh "$here/packaging/install-icons.sh" "$STAGE/usr/share" >/dev/null

# Strip, unless asked not to. A WebEngine-linked binary carries a lot of debug
# information and the difference is tens of megabytes.
if [ "${DEB_STRIP:-1}" = "1" ] && command -v strip >/dev/null 2>&1; then
	strip --strip-unneeded "$STAGE/usr/bin/hydra" || true
fi

# --- Depends, read off the binary ------------------------------------------
depends=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
	work=$(mktemp -d)
	mkdir -p "$work/debian"
	printf 'Source: hydra\nPackage: hydra\nArchitecture: any\n' > "$work/debian/control"
	: > "$work/debian/substvars"
	depends=$(cd "$work" && dpkg-shlibdeps -O --ignore-missing-info \
		"$STAGE/usr/bin/hydra" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
	rm -rf "$work"
fi
if [ -z "$depends" ]; then
	# Said out loud. A package that quietly declares no dependencies installs
	# cleanly and then fails to start, which is the worst of the outcomes here.
	echo "WARNING: could not compute dependencies; the package will declare none"
fi

# WebEngine needs a sandbox helper and its resources, which live in the
# -bin package; shlibdeps finds that one. What it cannot see is the runtime
# data this program reads through a plugin rather than a link.
recommends="ca-certificates"
suggests="keepassxc, yt-dlp, gnome-keyring | kwalletmanager"

maintainer=$(git -C "$here" config user.name 2>/dev/null || true)
maintainer_email=$(git -C "$here" config user.email 2>/dev/null || true)
[ -n "$maintainer" ] || maintainer="Hydra"
[ -n "$maintainer_email" ] || maintainer_email="root@localhost"

size=$(du -ks "$STAGE" | cut -f1)

cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: hydra
Version: $VERSION
Architecture: $arch
Maintainer: $maintainer <$maintainer_email>
Installed-Size: $size
Section: web
Priority: optional
Depends: $depends
Recommends: $recommends
Suggests: $suggests
Description: Tab-tree browser with a per-site policy engine
 Hydra keeps its tabs in a tree that is saved as a plain text file rather
 than a session blob, so the set of things you have open is something you
 can read, edit and keep. Tabs can be imported from Firefox and Chromium.
 .
 Filtering, script and permission decisions are made per site by a policy
 engine rather than per browser, and every choice is visible and revocable.
 Passwords are filled from KeePassXC over its browser protocol; nothing is
 stored by Hydra itself.
CONTROL

# Modes, which `--root-owner-group` does not touch -- it sets ownership only.
# The build host's umask decides otherwise, and a umask of 002 gives every
# directory in the package mode 775. Debian policy wants 0755, and a package
# that ships group-writable system directories is a package that widens
# permissions on the machine that installs it.
find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE/usr/share" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/usr/bin/hydra"
chmod 0644 "$STAGE/DEBIAN/control"

deb="$OUT/hydra_${VERSION}_${arch}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$deb" >/dev/null
echo "$deb"
