# SPDX-License-Identifier: GPL-3.0-or-later
#
# The desktop and Android app, for qmake.
#
# **This is the build now.** It was written to be compared against a
# CMakeLists.txt beside it, and that comparison is what settled the question:
# the CMake build is gone and the two kept are this one and fmake. What each of
# them has to be told, and what it works out, is in project.md.
#
# **Sources are globbed and then filtered by platform**, which is not laziness:
# the tree gains files often, and a list that has to be edited is a list that
# ends up wrong. CMake's `file(GLOB CONFIGURE_DEPENDS)` re-runs itself when the
# directory changes; qmake does not, so a new file needs `qmake` run again --
# which the Makefile wrapper does on every build anyway.

TEMPLATE = app
TARGET   = hydra
# Read rather than restated: the Makefile, debian/changelog and this file
# would otherwise be three places to change one number.
VERSION  = $$cat(VERSION, singleline)

QT     += widgets network webchannel qml
CONFIG += c++17 link_pkgconfig

# The whole of src/ compiles clean under these, which was measured rather than
# hoped. Not -Werror: a warning on somebody else's compiler should not stop
# them building.
QMAKE_CXXFLAGS += -Wall -Wextra

# **-Os, not qmake's -O2 release default.** Replaced rather than appended: two
# -O flags on one command line leave the last one winning, which makes the
# setting depend on where in the line qmake happened to put it.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os

# And -Og rather than qmake's -O0 for a debug build, for the same reason: it
# stays followable in a debugger without giving up everything.
QMAKE_CXXFLAGS_DEBUG -= -O0
QMAKE_CXXFLAGS_DEBUG += -Og

SOURCES = $$files(src/*.cpp)
HEADERS = $$files(src/*.h)

# The only files that name a browser engine. Everything else is
# platform-neutral, and keeping it that way is what the WebViewBackend seam
# exists for (architecture doc §19.2).
#
# Qt WebEngine does not exist for Android at all, so the component is asked for
# where it exists and simply not asked for where it does not.
android {
	SOURCES -= $$files(src/qtwebengine_*.cpp)
	HEADERS -= $$files(src/qtwebengine_*.h)
} else {
	QT      += webenginewidgets
	SOURCES -= $$files(src/android_*.cpp)
	HEADERS -= $$files(src/android_*.h)
}

# **Every header carrying Q_OBJECT has to be in HEADERS or moc never sees it.**
# CMake's AUTOMOC finds `annoyed_dialog.h` and `cosmetic_filters.h` through the
# same-named source file, and both are absent from the CMakeLists source list
# for exactly that reason. Globbing the directory is what makes that difference
# stop mattering; a hand-written list would have inherited the omission and
# failed at link with an undefined vtable.

RESOURCES = icons/hydra.qrc hydra_seed.qrc

# The desktop's colour scheme comes over the XDG portal, which is DBus.
# Optional on purpose: it is how *Linux* answers, and every other platform has
# its own that Qt already reads.
qtHaveModule(dbus) {
	QT      += dbus
	DEFINES += HYDRA_HAVE_DBUS
} else {
	message("Qt6 DBus not found - the desktop colour-scheme portal will not be asked")
}

# The optional dependencies, each of which costs one feature and no more. A
# cross build must not ask the host's pkg-config: it answers cheerfully about
# the host, so a build for another architecture reports the feature enabled and
# fails at link looking like a toolchain fault.
!android {
	packagesExist(libsodium) {
		PKGCONFIG += libsodium
		DEFINES   += HYDRA_HAVE_SODIUM
	} else {
		warning("libsodium not found - KeePassXC password manager disabled")
	}

	# NOTE the name. rasterbar's and rakshasa's unrelated torrent libraries
	# both install a pkg-config file called `libtorrent`, and the bare name
	# resolves to rakshasa's, which is a different library with a different
	# API (architecture doc 11.4). Ask for the qualified name, never the bare
	# one -- CMake reaches `find_package(LibtorrentRasterbar)` for this and
	# pkg-config has no equivalent, so the discipline is the whole safeguard.
	packagesExist(libtorrent-rasterbar) {
		PKGCONFIG += libtorrent-rasterbar
		DEFINES   += HYDRA_HAVE_LIBTORRENT
	} else {
		warning("libtorrent-rasterbar not found - BitTorrent downloads disabled")
	}

	# This one degrades rather than disappearing: without it the KeePassXC
	# pairing still works, it just lives in memory and has to be confirmed
	# again after a restart. A warning would say "something is wrong" about a
	# build that is merely smaller, so it is a message.
	packagesExist(libsecret-1) {
		PKGCONFIG += libsecret-1
		DEFINES   += HYDRA_HAVE_SECRET
	} else {
		message("libsecret not found - the KeePassXC pairing will not survive a restart")
	}

	# And this one degrades to our own code. Firefox's session file is a raw
	# LZ4 block and `session_import` carries a bounds-checked decoder for it,
	# so the feature works without the library. Where liblz4 is present it is
	# used instead: it parses a file another program wrote, and "we have a
	# verified implementation" is a weaker argument than "we use the one
	# everyone else uses".
	packagesExist(liblz4) {
		PKGCONFIG += liblz4
		DEFINES   += HYDRA_HAVE_LZ4
	} else {
		message("liblz4 not found - the built-in LZ4 decoder will be used instead")
	}
}

# The sample tree, beside the binary, so the app finds it on first run. On
# Android there is no such directory and the copy inside the resources is what
# seeds app storage instead.
!android {
	QMAKE_POST_LINK += $$quote(cp -f $$PWD/sample-tree.txt $$OUT_PWD/sample-tree.txt)
}

android {
	ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
	# A package with no versionCode installs, and then nothing can ever be an
	# upgrade of it: Android compares that integer and treats absent as zero.
	ANDROID_VERSION_NAME = $$VERSION
	ANDROID_VERSION_CODE = 1
}
