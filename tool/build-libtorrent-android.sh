#!/bin/sh
# Cross-compile libtorrent-rasterbar for Android, as a static library.
#
# BitTorrent is a first-class download (architecture sec 11.4) and the
# architecture's rule for Android is to embed the engine there too (sec 19.6:
# "Build Android with the engine embedded"). Until this, the Android build
# had no libtorrent at all and a magnet link answered "This build has no
# BitTorrent support."
#
# Usage:
#   tool/build-libtorrent-android.sh [abi]      # default arm64-v8a
#
# Produces deps/android/<abi>/lib/libtorrent-rasterbar.a and its headers
# under deps/android/<abi>/include/libtorrent, which hydra.pro links when
# they are present. Needs tool/build-openssl-android.sh to have been run for
# the same ABI first: libtorrent's encryption and its https trackers use it.
#
# THE SOURCE COMES FROM APT, as OpenSSL's does (see that script for why):
# `apt-get source libtorrent-rasterbar`, which is also the version the
# desktop build links, so the two platforms run one engine.
#
# **Boost is header-only here, and taken from the host's headers.** Since
# 1.69 Boost.System is header-only, which is all libtorrent 2.0 needs; the
# headers are portable C++. What must NOT happen is CMake finding the host's
# Boost package: its target names /usr/include, which would put glibc's
# headers on an Android compile line. So a package of our own is written
# below whose only include directory holds a single `boost` link.
set -e

abi=${1:-arm64-v8a}
case "$abi" in
	arm64-v8a|armeabi-v7a|x86_64|x86) ;;
	*)
		echo "unknown ABI: $abi" >&2
		echo "expected one of: arm64-v8a armeabi-v7a x86_64 x86" >&2
		exit 1
		;;
esac

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)

if [ -z "$ANDROID_NDK_ROOT" ] || [ ! -d "$ANDROID_NDK_ROOT" ]; then
	echo "ANDROID_NDK_ROOT is not set or does not exist." >&2
	echo "  It must be the NDK the Qt kit names in mkspecs/qdevice.pri;" >&2
	echo "  make android-check compares them." >&2
	exit 1
fi
toolchain="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
[ -f "$toolchain" ] || { echo "no $toolchain" >&2; exit 1; }
for tool in cmake ninja apt-get; do
	command -v "$tool" >/dev/null 2>&1 || { echo "$tool is not installed." >&2; exit 1; }
done
boost_headers=${BOOST_INCLUDE:-/usr/include}
[ -f "$boost_headers/boost/version.hpp" ] || {
	echo "no Boost headers under $boost_headers (install libboost-dev, or set BOOST_INCLUDE)" >&2
	exit 1
}

prefix="$root/deps/android/$abi"
[ -f "$prefix/lib/libssl_3.so" ] || {
	echo "no OpenSSL for $abi: run tool/build-openssl-android.sh $abi first" >&2
	exit 1
}

# The API level the app is built for, as OpenSSL's script takes it.
api=${ANDROID_API:-26}

work="$root/deps/build/libtorrent-$abi"
case "$work" in
	"$root"/deps/build/libtorrent-*) ;;
	*) echo "refusing to use $work" >&2; exit 1 ;;
esac

if [ -n "$LIBTORRENT_SRC" ]; then
	echo "libtorrent: using the source at $LIBTORRENT_SRC"
	src="$LIBTORRENT_SRC"
else
	echo "libtorrent: fetching source through apt"
	rm -rf "$work"
	mkdir -p "$work"
	(cd "$work" && apt-get source libtorrent-rasterbar >/dev/null)
	src=$(ls -d "$work"/libtorrent-rasterbar-*/ | head -1)
	[ -n "$src" ] && [ -f "$src/CMakeLists.txt" ] || {
		echo "apt-get source gave no libtorrent tree" >&2
		exit 1
	}
fi

# The Boost package of our own: headers only, and nothing else of the host.
boost_pkg="$work/boost-cmake"
mkdir -p "$boost_pkg" "$work/boost-include"
ln -sfn "$boost_headers/boost" "$work/boost-include/boost"
version=$(sed -n 's/^#define BOOST_VERSION \([0-9]*\).*/\1/p' "$boost_headers/boost/version.hpp")
major=$((version / 100000))
minor=$((version / 100 % 1000))
patch=$((version % 100))
cat > "$boost_pkg/BoostConfig.cmake" <<EOF
# Written by tool/build-libtorrent-android.sh: Boost's headers and nothing
# else of the host, for a cross build.
set(Boost_FOUND TRUE)
set(Boost_VERSION $major.$minor.$patch)
set(Boost_MAJOR_VERSION $major)
set(Boost_MINOR_VERSION $minor)
set(Boost_SUBMINOR_VERSION $patch)
set(Boost_INCLUDE_DIRS "$work/boost-include")
if(NOT TARGET Boost::headers)
	add_library(Boost::headers INTERFACE IMPORTED)
	set_target_properties(Boost::headers PROPERTIES
		INTERFACE_INCLUDE_DIRECTORIES "$work/boost-include")
endif()
EOF
cat > "$boost_pkg/BoostConfigVersion.cmake" <<EOF
set(PACKAGE_VERSION $major.$minor.$patch)
set(PACKAGE_VERSION_COMPATIBLE TRUE)
EOF

build="$work/build"
rm -rf "$build"
mkdir -p "$build"
jobs=${JOBS:-8}

cmake -S "$src" -B "$build" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
	-DANDROID_ABI="$abi" \
	-DANDROID_PLATFORM="android-$api" \
	-DCMAKE_BUILD_TYPE=MinSizeRel \
	-DCMAKE_CXX_FLAGS="-Os" \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DCMAKE_CXX_STANDARD=17 \
	-DCMAKE_INSTALL_PREFIX="$prefix" \
	-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
	-DCMAKE_POLICY_DEFAULT_CMP0167=NEW \
	-DBoost_DIR="$boost_pkg" \
	-DOPENSSL_ROOT_DIR="$prefix" \
	-DOPENSSL_INCLUDE_DIR="$prefix/include" \
	-DOPENSSL_SSL_LIBRARY="$prefix/lib/libssl_3.so" \
	-DOPENSSL_CRYPTO_LIBRARY="$prefix/lib/libcrypto_3.so" \
	-Dbuild_tests=OFF -Dbuild_examples=OFF -Dbuild_tools=OFF \
	-Dpython-bindings=OFF

cmake --build "$build" -j "$jobs"
cmake --install "$build" >/dev/null

lib="$prefix/lib/libtorrent-rasterbar.a"
[ -f "$lib" ] || { echo "no $lib after install" >&2; exit 1; }
# What was built is checked, not assumed: an object of the wrong machine in
# the archive is what a toolchain mix-up looks like, and it would surface
# only at link time inside the app.
first=$(ar t "$lib" | head -1)
(cd "$build" && ar x "$lib" "$first" && file "$first" && rm -f "$first")
echo "libtorrent: $lib ($(du -h "$lib" | cut -f1))"
