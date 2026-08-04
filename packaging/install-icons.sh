#!/bin/sh
# Install the icon set and the desktop entry into a hicolor theme.
#
# Sizes go in as separate files rather than one scalable SVG on purpose: the
# 16px cut is drawn pixel by pixel, so letting a theme engine rescale a large
# one would throw away the only version that reads in a tab strip.
#
#   sh packaging/install-icons.sh            # into ~/.local/share
#   sh packaging/install-icons.sh /usr/share # or a system prefix
set -e

share="${1:-$HOME/.local/share}"
here="$(dirname "$0")/.."

for size in 16 24 32 48 64 128 256; do
	dir="$share/icons/hicolor/${size}x${size}/apps"
	mkdir -p "$dir"
	cp "$here/icons/hydra-$size.png" "$dir/hydra.png"
done

mkdir -p "$share/applications"
cp "$here/packaging/hydra.desktop" "$share/applications/hydra.desktop"

# Skipped when staging into a package. Refreshing a cache while building a .deb
# would touch the *build* host, and on the installing machine dpkg's own
# triggers do both of these after unpacking, which is the only correct moment.
if [ -n "${HYDRA_SKIP_CACHE_UPDATE:-}" ]; then
	echo "installed into $share (caches left for the package manager)"
	exit 0
fi

# Harmless if absent; without it a running desktop may not notice the new icon.
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
	gtk-update-icon-cache -q -t -f "$share/icons/hicolor" 2>/dev/null || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
	update-desktop-database -q "$share/applications" 2>/dev/null || true
fi

echo "installed into $share"
