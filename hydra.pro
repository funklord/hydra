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
# **And handed to the program**, because qmake's VERSION does nothing for an
# app target beyond naming artifacts: without this the binary cannot say what
# it is. One source still -- the file -- read once here.
DEFINES += HYDRA_VERSION=\\\"$$VERSION\\\"

# **Qt 6.8, refused here rather than discovered in the compiler.** The floor
# used to be stated as 6.4, derived once from the menu bar's addAction
# overload and never derived again as the code moved. Two things have raised
# it since and neither updated it: `theme.h` names `Qt::ColorScheme`, which
# arrived in 6.5, and `qtwebengine_view.cpp` includes `QWebEnginePermission`,
# which arrived in 6.8 -- and that include sits *outside* the
# `#if QT_VERSION >= 6.8` guard around the code using it, so the guard never
# bought anything.
#
# Found by CI on Qt 6.4, as two hundred lines of "'ColorScheme' is not a
# member of 'Qt'" a long way from any statement about versions. A configure
# that stops and says the number is worth more than every one of them.
#
# 6.8.2 and 6.11 are the versions actually built against; 6.5 to 6.7 are
# untested and are excluded by the include above rather than by evidence.
lessThan(QT_MAJOR_VERSION, 6) {
	error("Hydra needs Qt 6.8 or newer; this is Qt $$QT_VERSION")
}
equals(QT_MAJOR_VERSION, 6):lessThan(QT_MINOR_VERSION, 8) {
	error("Hydra needs Qt 6.8 or newer; this is Qt $$QT_VERSION")
}

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
	# And the single-instance guard, which answers a question Android does not
	# ask: the system runs one process per application and a launcher tap
	# resumes the task that exists, so there is no second process to keep out
	# of the profile directory. `main.cpp` guards the call the same way, so
	# this is what the build does about it and not the only thing saying so.
	SOURCES -= src/single_instance.cpp
	HEADERS -= src/single_instance.h
} else {
	# printsupport rides with webenginewidgets rather than sitting in the
	# unconditional QT line: the only thing that includes it is
	# qtwebengine_view.cpp, which this branch is what keeps. Android prints
	# through the system PrintManager and has no QPrinter to link against.
	QT      += webenginewidgets printsupport
	SOURCES -= $$files(src/android_*.cpp)
	HEADERS -= $$files(src/android_*.h)
}

# **Every header carrying Q_OBJECT has to be in HEADERS or moc never sees it.**
# CMake's AUTOMOC finds `annoyed_dialog.h` and `cosmetic_filters.h` through the
# same-named source file, and both are absent from the CMakeLists source list
# for exactly that reason. Globbing the directory is what makes that difference
# stop mattering; a hand-written list would have inherited the omission and
# failed at link with an undefined vtable.

RESOURCES = icon/hydra.qrc hydra_seed.qrc ublock.qrc

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
	# **OpenSSL, which Qt for Android does not ship.** Without it every
	# request hydra makes itself fails -- found 2026-10-23, when the phone
	# had subscribed to six filter lists and fetched none, so it blocked no
	# ads. The WebView has TLS of its own, which is why pages loaded and
	# nothing else looked wrong. Built by tool/build-openssl-android.sh from
	# the distribution's source; the _3 suffix is the name Qt's OpenSSL
	# backend asks for. Conditional, so a tree that has not built it still
	# packages, and says why it cannot fetch.
	HYDRA_SSL_LIB = $$PWD/deps/android/$$ANDROID_TARGET_ARCH/lib
	exists($$HYDRA_SSL_LIB/libssl_3.so) {
		ANDROID_EXTRA_LIBS += $$HYDRA_SSL_LIB/libcrypto_3.so $$HYDRA_SSL_LIB/libssl_3.so
		message("openssl: bundling $$HYDRA_SSL_LIB")
	} else {
		warning("openssl: $$HYDRA_SSL_LIB/libssl_3.so is missing -- the package " \
		        "will have no TLS. Run tool/build-openssl-android.sh.")
	}
	# **libtorrent, embedded as on the desktop** (architecture sec 19.6: "Build
	# Android with the engine embedded"). Static, built by
	# tool/build-libtorrent-android.sh from the same Debian source the desktop
	# links, against the OpenSSL above. The definitions are the ones its
	# CMake package exports and must match what it was compiled with; Boost is
	# headers only, through the script's own link to them.
	#
	# **The decision is a header, not a define**, because qmake does not
	# recompile when DEFINES changes: built once before the library existed,
	# the object stayed torrent-less after it did, linked cleanly, and the
	# package had no BitTorrent and no error (measured 2026-10-31: 14 KB
	# larger instead of 2 MB). `hydra_android_torrent.h` is rewritten only
	# when the answer changes, so its timestamp moves exactly then and the
	# dependency tracking rebuilds the one file that reads it.
	HYDRA_LT_LIB = $$HYDRA_SSL_LIB/libtorrent-rasterbar.a
	HYDRA_LT_CONFIG = $$OUT_PWD/hydra_android_torrent.h
	exists($$HYDRA_LT_LIB):exists($$HYDRA_SSL_LIB/libssl_3.so) {
		INCLUDEPATH += $$PWD/deps/android/$$ANDROID_TARGET_ARCH/include \
		               $$PWD/deps/build/libtorrent-$$ANDROID_TARGET_ARCH/boost-include
		LIBS        += $$HYDRA_LT_LIB $$HYDRA_SSL_LIB/libssl_3.so $$HYDRA_SSL_LIB/libcrypto_3.so
		QMAKE_CXXFLAGS += -fexceptions
		# What its CMake package exports, which its headers must be compiled
		# with too: two of them change Asio's own declarations.
		HYDRA_LT_LINES = "$${LITERAL_HASH}define HYDRA_HAVE_LIBTORRENT 1" \
		                 "$${LITERAL_HASH}define BOOST_ASIO_ENABLE_CANCELIO" \
		                 "$${LITERAL_HASH}define BOOST_ASIO_NO_DEPRECATED" \
		                 "$${LITERAL_HASH}define TORRENT_USE_OPENSSL" \
		                 "$${LITERAL_HASH}define TORRENT_USE_LIBCRYPTO" \
		                 "$${LITERAL_HASH}define TORRENT_SSL_PEERS" \
		                 "$${LITERAL_HASH}define OPENSSL_NO_SSL2"
		message("libtorrent: linking $$HYDRA_LT_LIB")
	} else {
		HYDRA_LT_LINES = "/* no libtorrent for this ABI: tool/build-libtorrent-android.sh */"
		warning("libtorrent: $$HYDRA_LT_LIB is missing -- the package will have no " \
		        "BitTorrent. Run tool/build-libtorrent-android.sh.")
	}
	HYDRA_LT_OLD = $$cat($$HYDRA_LT_CONFIG, lines)
	!equals(HYDRA_LT_OLD, $$HYDRA_LT_LINES): write_file($$HYDRA_LT_CONFIG, HYDRA_LT_LINES)
	# **Pinned, not left to Qt.** Google Play has accepted only apps
	# targeting API 36 or later since 2026-08-31. Unset, this came from
	# Qt 6.12's default, measured as 36 in the built apk with aapt2 -- correct
	# by accident, and liable to move with the next kit. Pinned on the
	# holder's instruction 2026-10-22, as beerssh and bbq-predictor do.
	ANDROID_TARGET_SDK_VERSION = 36
	# A package with no versionCode installs, and then nothing can ever be an
	# upgrade of it: Android compares that integer and treats absent as zero.
	ANDROID_VERSION_NAME = $$VERSION
	# **Only a fallback.** `tool/android.mk` derives the real one from VERSION
	# -- major*10000 + minor*100 + patch, so 0.1 is 100 -- and passes it on the
	# qmake command line, which is set before this file is read. Assigning
	# unconditionally here clobbered it: the preflight printed "versionCode
	# 100" and the built apk carried 1, so the number reported and the number
	# shipped were different, and the fragment's own warning about a hardcoded
	# code allowing exactly one upload applied to us while looking fixed.
	#
	# Kept for a bare `qmake hydra.pro`, which has no Makefile to ask.
	isEmpty(ANDROID_VERSION_CODE) {
		ANDROID_VERSION_CODE = 1
	}
}
