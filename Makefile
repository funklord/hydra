# =============================================================================
# Top-level Makefile -- Hydra
#
# PURPOSE
#   The single entry point for the whole tree, and the reason it exists is
#   consistency rather than capability: beerssh and fuzzypickles' gui/ subtree
#   both present `make`, `make test`, `make android`, `make install` with
#   DEBUG=1 / SANITIZE=1, and hydra presented two different build invocations
#   and a per-binary test run you had to know to prefix with
#   QT_QPA_PLATFORM=offscreen. This file makes the three look like one
#   interface.
#
#   **Two build systems are maintained, and CMake is not one of them.** The
#   migration this header used to describe as deliberately open is closed:
#
#     * **qmake, driven from here** (`hydra.pro`) builds the app and the APK.
#     * **plain Make** (`test/Makefile`) builds the test tree.
#     * **fmake** builds the same sources from no build file at all, as a
#       cross-check and a second opinion. What it has to be told is six
#       annotations in the sources -- `@target` and five `@pkg_optional` --
#       and nothing else.
#
#   The four problems a migration had to solve, and what solved them:
#
#     * **72 executables**, globbed rather than listed, so adding a suite or a
#       driver is no build-system work. `test/Makefile` globs `test_*.cpp`
#       and `live/try_*.cpp` and needs no configure step to notice a new one.
#     * **The library that is two libraries.** rasterbar's and rakshasa's
#       unrelated torrent libraries both install a pkg-config file called
#       `libtorrent`, and the bare name resolves to rakshasa's, which has a
#       different API. `find_package(LibtorrentRasterbar)` used to be what
#       disambiguated it; `hydra.pro` asks for `libtorrent-rasterbar` by name
#       and the source carries the same qualified name in an `@pkg_optional`
#       beside the include, so both remaining builds ask for the right one.
#     * **pkg-config lying about the target.** On an Android cross build the
#       *host* pkg-config answers cheerfully about the host. `hydra.pro` asks
#       for none of the optional packages under `android {}`, which is why the
#       APK is built from the same file without the feature detection running
#       in the wrong architecture.
#     * **The APK.** This was the one CMake held alone, through `qt-cmake` and
#       androiddeployqt. The kit ships `qmake` too, and `make android` now
#       drives it; the artifact was checked rather than trusted -- correct ABI,
#       `se.vibes.hydra` in the manifest, our three Java classes in the dex
#       (in `classes4.dex`, since the package is multidex), and debug-signed.
#
# TARGETS
#   make              -- build the desktop app
#   make run          -- build and run it on a copy of the sample tree, so
#                        running the browser never dirties a tracked file
#   make test         -- build and run every suite that needs nothing but a
#                        build; the ones needing a helper, a torrent library
#                        or a model are listed but not run (see test/README.md)
#   make test-one T=x -- build and run a single suite, e.g. T=test_theme
#   make drivers      -- build the live drivers (expensive; see JOBS below)
#   make sweep        -- build them and run them all, with a summary (offscreen;
#                        SWEEP_ONSCREEN=1 uses the real display)
#   make replay       -- re-score recorded model replies against the gate (no model)
#   make deb          -- build a .deb into build/deb (dependencies computed)
#   make deb-check    -- build it and print what it declares and contains
#   make android      -- build a debug Android .apk, check the ABI
#                        it really carries, and name it after that
#   make android-build -- the build alone, without the check-and-name step
#                        (ANDROID_ABI picks the architecture; see ANDROID)
#   make android-install / -run / -log / -uninstall -- over adb
#   make install      -- install the binary, desktop entry and icon set
#   make uninstall    -- remove what install put there
#   make clean        -- remove build output, leaving the source tree alone
#   make style        -- the shared source gate, this repo's project.md heading
#                        checks (see DOC CHECKS), and the JNI name check
#   make jni          -- just the JNI check: every Java `native` method has a
#                        C++ symbol JNI can actually resolve
#   make check        -- style and test together, the one to run before a commit
#   make hooks        -- install the git hooks from tool/hooks/
#   make veryclean    -- clean, plus the generated artefacts it leaves
#   make distclean    -- veryclean, plus anything a release build produced
#   make help         -- this list
#
# BUILD FLAGS
#   DEBUG and SANITIZE are never given a `?=` default, following beerssh:
#   every check is `ifdef`, so a `?= 0` default would make them permanently
#   set and impossible to turn off.
#
#     make DEBUG=1        -Og and symbol-rich, for a debugger
#
#   Optimization is -Os everywhere else. Debug is the only exception, and it
#   is an exception because a debugger needs the code to match the source.
#     make SANITIZE=1     ASan + UBSan, independent of DEBUG
#
#   Everything else is `?=`, so the command line and the environment win:
#     make CXX=clang++ JOBS=4
#
# ⚠️ JOBS
#   **Defaults to 2, and that is not timidity.** Each live driver compiles the
#   app's sources and links Qt WebEngine, and there are 34 of them. `make -j`
#   with no number is *unlimited* under this generator, and it has taken this
#   machine's desktop session down twice -- the kernel OOM killer took dbus,
#   pipewire, both xdg-desktop-portals and the running browsers with it, with
#   31 GB of RAM and 31 GB of swap in the machine. Raise it deliberately if
#   you know the machine has the headroom, and stop any local model first: a
#   14B holds ~10 GB before the compiler starts.
#
# ANDROID
#   Needs a Qt-for-Android kit, an NDK, an SDK and a JDK -- none of which the
#   desktop build wants. Each is a separate variable because each fails
#   unhelpfully when missing, and Qt WebEngine does not exist for Android at
#   all, so that build goes through the WebViewBackend seam (arch §19.2).
#
#   **ANDROID_ABI picks the architecture and the kit follows it.**
#
#     make android                       arm64-v8a, which is a phone
#     make android ANDROID_ABI=x86_64    which is what an emulator usually is
#
#   The kit is found under QT_ROOT (~/Qt), newest Qt first, and is asked what
#   it actually builds before anything compiles. Naming one by hand still
#   works and is checked the same way:
#
#     make android ANDROID_ABI=x86_64 QT_ANDROID_ROOT=$HOME/Qt/6.10.0/android_x86_64
#
# =============================================================================

CXX   ?= g++
# qmake's name varies by distribution: Debian ships `qmake6`, a Qt installer
# kit ships `qmake`. Asked for in that order rather than assumed, so neither
# kind of machine has to be told.
QMAKE ?= $(shell command -v qmake6 2>/dev/null || command -v qmake 2>/dev/null || echo qmake6)

# See the JOBS warning above before raising this.
JOBS ?= 2

TARGET     = hydra
BUILD_DIR  ?= build
TESTS_DIR  ?= test/build-make
TREE       ?= sample-tree.txt

PREFIX ?= $(HOME)/.local
SHARE  ?= $(PREFIX)/share

# Android. Kept together so the "you are missing one" message can name them.
#
# **ANDROID_ABI selects the build.** It used to select nothing: the kit named
# by QT_ANDROID_ROOT decided the real architecture and this variable was only
# pasted into the output filename, so `make android ANDROID_ABI=x86_64` produced
# an arm64 apk called x86_64. That is the combination somebody actually types
# -- an emulator is x86_64 on almost every desktop -- and adb then refuses the
# install with a message about the package rather than about the architecture,
# which is a long way from the cause. project.md records the session that lost
# to it.
# The ABI, the kit discovery, the API levels, the SDK/NDK/JDK resolution and
# the adb plumbing all come from tool/android.mk, included below `all`. The
# kit discovery there and the libQt6Core ABI confirmation are this project's
# own, moved to where the other three Android projects read them too.
ANDROID_ABI ?= arm64-v8a
ANDROID_ABIS = arm64-v8a armeabi-v7a x86_64 x86

# The application id, which the fragment's install, run, log and uninstall
# targets give to adb. It matches android/AndroidManifest.xml's package.
APP_ID = se.vibes.hydra

# **Per ABI, because two architectures' objects must not meet.** One shared
# build-android/ meant switching ABI reused the previous one's generated
# Makefile and objects -- the same foot-gun one level down, and the one that
# would have made the fix above look like it had not worked.
ANDROID_BUILD_DIR ?= build-android-$(ANDROID_ABI)
# Named, not globbed, so `clean` can say what it removes. The bare
# `build-android` is what versions before the split left behind.
ANDROID_BUILD_DIRS = $(foreach a,$(ANDROID_ABIS),build-android-$(a)) build-android

# What android-install and android-run in tool/android.mk reach for, and
# what the `android` rule below verifies before naming.
ANDROID_ARTIFACT = $(ANDROID_BUILD_DIR)/hydra-$(VERSION)-$(ANDROID_ABI).apk

# **-Os, and -Og under DEBUG.** Both live in `hydra.pro`, which subtracts
# qmake's own -O2 and -O0 before adding them: two -O flags on one command line
# leave the last one winning, so appending is not enough. Nothing is passed
# from here except which of the two configurations to build, because a flag
# set in two places is a flag that disagrees with itself eventually.
QMAKE_CONFIG ?= release
ifdef DEBUG
QMAKE_CONFIG = debug
endif
ifdef SANITIZE
# qmake's own sanitizer support, rather than hand-written flags: it puts them
# on the compile *and* the link line, which is the half that is easy to forget
# and fails as an undefined `__asan_init` a long way from the cause.
QMAKE_CONFIG += sanitizer sanitize_address sanitize_undefined
endif

# Suites that need nothing but a build. Written as an exclusion rather than a
# list, so a new test_*.cpp is picked up without editing this file -- the same
# property the live-driver glob has. It used to be the argument for keeping
# CMake, which globbed while the hand-written half of the source list did not;
# it survived dropping CMake instead, since `test/Makefile` globs `test_*.cpp`
# and fmake wants no source list at all.
# The excluded ones each need something the machine may not have; test/README.md
# says which, and they are named at the end of a run rather than silently
# skipped.
NEEDS_MORE = test_headers test_dlheaders test_helpers_live test_probe \
             test_probe_ui test_torrent test_watch test_live_model \
             test_ytdlp_live test_replay
ALL_SUITES = $(basename $(notdir $(wildcard test/test_*.cpp)))
SUITES     = $(filter-out $(NEEDS_MORE),$(ALL_SUITES))

# Offscreen because none of these want a window, and a keyring item of their
# own because `test_credstore` writes and deletes one: under the real name that
# item is the KeePassXC pairing the user actually uses, and the suite refuses to
# run rather than risk it.
# `--mute-audio` because a suite that loads a page should not play it at
# whoever is sitting at the machine. Chromium's own flag; WebEngine honours it.
#
# **TMPDIR because seven suites name a fixed path in the shared one.**
# `QDir::tempPath()` honours TMPDIR, and the suites build their scratch
# directories from it: `hydra-tree-test`, `hydra-model-test`,
# `hydra-state-test`, `hydra-asm-test`, `hydra-bundle-test`,
# `hydra-settings-test`, `hydra-extractors.json` -- and `test_seam` writes
# `clip.mp4` into the root by that bare name. Every one of those is a
# predictable name in a directory shared with every other account on the
# machine, which costs twice.
#
# It reads as a test failure. Measured 2026-08-31 running the suite as a
# second uid against trees whose `/tmp/hydra-*` a first uid had created and
# owned: 22 failures across test_tree, test_extractor, test_settings and
# test_seam, reported as "it saves", "the folder comes back", "the job
# completes" -- every one of them naming code that was correct. All four pass
# unchanged with a TMPDIR the runner owns. A gate that names the wrong suspect
# costs more than one that says nothing, and this named four.
#
# And it writes over somebody else's file. `test_seam` removes and recreates
# `$TMPDIR/clip.mp4` unconditionally; under a shared /tmp that is not this
# suite's file to destroy. The other seven at least collide inside a name that
# says hydra.
#
# Under the build directory rather than a per-uid name in /tmp, because it is
# then covered by `clean` -- twelve stale `/tmp/hydra-*` directories had
# accumulated here since 14 August, which a name nobody removes is how. Absolute
# because a suite is free to chdir and a relative TMPDIR would follow it.
TEST_TMP = $(TESTS_DIR)/tmp
TEST_ENV = QT_QPA_PLATFORM=offscreen HYDRA_SECRET_KIND=hydra-make-test \
           QTWEBENGINE_CHROMIUM_FLAGS=--mute-audio \
           TMPDIR=$(CURDIR)/$(TEST_TMP)

# Where a failing suite's whole output is kept. This target used to print the
# tail line and the first five FAIL lines and throw the rest away, which is fine
# for a suite that fails every time and useless for one that does not: an
# intermittent failure is the case where the output matters most and it was the
# case being discarded. Seen twice on test_extloop, both times on the first run
# after a source change and never reproducible afterwards, with nothing kept.
FAILED_DIR = $(TESTS_DIR)/failed

.PHONY: all run test test-one drivers sweep replay deb deb-check version-check android android-build android-aab install uninstall clean veryclean distclean help style style-docs style-source check hooks jni

# Always delegates, never compares timestamps itself. The first version made
# the binary a real target depending on the configure output, and `make` after
# editing a source said "Nothing to be done for 'all'" -- make was answering a
# question about two files while the generated build is the thing that knows
# about the other sixty. A wrapper that second-guesses the tool it wraps is
# worse than no wrapper, because it is wrong silently.
#
# **qmake runs every time, deliberately.** `hydra.pro` globs `src/*.cpp`, and
# qmake resolves a glob once when it generates the Makefile -- unlike CMake's
# `file(GLOB CONFIGURE_DEPENDS)`, which re-checks. A new source would otherwise
# be invisible until somebody happened to re-run configure, which is the kind
# of failure that looks like a linker problem. Re-running costs about a second
# and is what makes globbing safe here.
all:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(QMAKE) $(CURDIR)/hydra.pro CONFIG+="$(QMAKE_CONFIG)"
	@$(MAKE) --no-print-directory -C $(BUILD_DIR) -j$(JOBS)

# The shared Android vocabulary and everything it needs. Included AFTER
# `all`, because `include` is where make first sees a target and pulling it
# in above would make android-check the default goal.
include tool/android.mk

# **Run against a copy, not the tracked sample.** The app saves its tree on
# exit, so pointing it at `sample-tree.txt` means merely starting the browser
# rewrites a file in git -- a page title here, a type changed from suspended to
# open there, a fresh `seen=` timestamp on every run. It has been reverted three
# times in one session, twice by somebody who did not run it.
#
# The copy lives beside the build output and is refreshed only when it is
# missing, so state survives between runs, which is the point of running against
# a tree at all. `make run TREE=...` still overrides it for anyone who means to
# use a particular file.
RUN_TREE = $(BUILD_DIR)/run-tree.txt

run: all
	@test -f "$(RUN_TREE)" || cp $(TREE) "$(RUN_TREE)"
	@./$(BUILD_DIR)/$(TARGET) "$(RUN_TREE)"

# Built one target at a time on purpose: naming them individually keeps the
# live drivers out of it. `make drivers` is where that cost is opted into.
#
# `test/Makefile` is the test tree's own build and needs no configure step,
# so there is nothing here standing in for a cache file. Its BUILD_DIR is
# `build-make` relative to `test/`, which is why the target named below is
# spelled without the `test/` that TESTS_DIR carries.
test:
	@# **A floor, because this list is computed and an empty one passes.**
	@# SUITES comes from $$(wildcard test/test_*.cpp) minus NEEDS_MORE, so
	@# anything that makes the wildcard match nothing -- a rename, a move, a
	@# run from somewhere unexpected -- leaves both loops below iterating an
	@# empty list, setting fail=0, printing the reassuring "not run here"
	@# trailer and exiting 0 having built and run nothing at all.
	@#
	@# `.github/workflows/ci.yml` already says this in as many words and
	@# guards it with `-ge 30`. The guard was only there, which made it
	@# something a person could get wrong locally and hear about remotely --
	@# and `check: style test` is what a person runs before committing. So the
	@# floor belongs here, where both paths cross it.
	@#
	@# 33 run today, of 43 files with 10 in NEEDS_MORE. The floor is below
	@# that rather than equal to it: suites come and go, and the failure worth
	@# catching is the list collapsing, not one suite being retired.
	@n=$$(echo $(SUITES) | wc -w); \
	 if [ "$$n" -lt 30 ]; then \
		echo "test: only $$n suite(s) in the list; expected at least 30." >&2; \
		echo "      Running none of them and reporting success is the failure" >&2; \
		echo "      this floor exists to prevent. Check that test/test_*.cpp" >&2; \
		echo "      still matches from $$(pwd)." >&2; \
		exit 1; \
	 fi
	@for t in $(SUITES); do $(MAKE) --no-print-directory -C test -j$(JOBS) \
	   build-make/$$t >/dev/null || exit 1; done
	@mkdir -p $(TEST_TMP)
	@fail=0; for t in $(SUITES); do \
	   out=$$($(TEST_ENV) ./$(TESTS_DIR)/$$t 2>&1); \
	   if [ $$? -eq 0 ]; then printf '  ok   %-16s %s\n' "$$t" "$$(echo "$$out" | tail -1)"; \
	   else fail=1; printf '  FAIL %-16s %s\n' "$$t" "$$(echo "$$out" | tail -1)"; \
	        echo "$$out" | grep FAIL | head -5; \
	        mkdir -p $(FAILED_DIR); echo "$$out" > $(FAILED_DIR)/$$t.log; \
	        printf '       full output: %s/%s.log\n' "$(FAILED_DIR)" "$$t"; fi; \
	 done; \
	 echo; echo "not run here, each needs something this target does not provide:"; \
	 echo "  $(NEEDS_MORE)"; \
	 echo "  see test/README.md for what each one wants"; \
	 exit $$fail

test-one:
	@test -n "$(T)" || { echo "usage: make test-one T=test_theme"; exit 2; }
	@$(MAKE) --no-print-directory -C test -j$(JOBS) build-make/$(T)
	@mkdir -p $(TEST_TMP)
	@$(TEST_ENV) ./$(TESTS_DIR)/$(T)

# Separate from `test` because it is a different order of cost: each driver
# compiles the whole app and links WebEngine, and they want a real display.
drivers:
	@$(MAKE) --no-print-directory -C test -j$(JOBS) all
	@echo "live drivers built. Run them offscreen: QT_QPA_PLATFORM=offscreen ./$(TESTS_DIR)/try_cookies"
	@echo "test/README.md says which need a helper server, KeePassXC or a model."

# Score the recorded model replies against the current gate. No model, no
# network, milliseconds -- but it needs the corpus in evidence/replies, which is
# not in git, so it cannot be part of `make test`.
replay:
	@$(MAKE) --no-print-directory -C test -j$(JOBS) build-make/test_replay >/dev/null
	@mkdir -p $(TEST_TMP)
	@$(TEST_ENV) ./$(TESTS_DIR)/test_replay

# Run them all and summarise. Offscreen by default, so it does not take over a
# desktop; SWEEP_ONSCREEN=1 uses the real display, which is only needed when
# appearance rather than behaviour is the question.
# Pass DRIVERS=... for a subset: make sweep DRIVERS="try_import try_delete".
sweep: drivers
	@test/live/sweep.sh $(DRIVERS)

# --- Packaging -------------------------------------------------------------
#
# The one place the version is stated. It used to be parsed out of
# CMakeLists.txt, which is a source that disappears with the build system;
# hydra.pro reads the same file, and `make version-check` holds
# debian/changelog to it as well.
VERSION     := $(shell cat VERSION)
DEB_DIR      = $(BUILD_DIR)/deb

# Native Debian packaging: debian/ holds the metadata, debhelper does the
# work. packaging/make-deb.sh assembled it by hand and is gone.
deb: version-check
	@test -n "$(BUILD_DIR)" || { echo "deb: BUILD_DIR is empty, refusing" >&2; exit 1; }
	dpkg-buildpackage -b -us -uc
	@mkdir -p $(BUILD_DIR)/deb
	@for f in ../hydra_$(VERSION)_*.deb ../hydra-dbgsym_$(VERSION)_*.deb \
	          ../hydra_$(VERSION)_*.buildinfo ../hydra_$(VERSION)_*.changes; do \
		[ -e "$$f" ] && mv -f "$$f" $(BUILD_DIR)/deb/ || true; \
	done
	@ls -1 $(BUILD_DIR)/deb/*.deb

# The VERSION file is the source; debian/changelog is checked against it.
#
# **Two readers, and the second one is the point.** This called
# `dpkg-parsechangelog` alone and printed "skipped" when it was absent -- a
# check reporting success over nothing, on exactly the machine least likely to
# have dpkg-dev installed. Measured before changing it: with a stub on PATH
# reading 9.9 against a VERSION of 0.1 the rule failed correctly, and with the
# same disagreement and the tool merely missing it printed "skipped" and exited
# 0. `debian/rules` and `debian/changelog` both name this rule as what fails a
# build on drift, so that silence had somewhere to go.
#
# The topmost changelog entry *is* the current version, by the format's own
# definition, so `sed` can read it and there is no machine where this has to
# give up. `dpkg-parsechangelog` is still asked where it exists, and the two
# are compared with each other rather than one being preferred -- a fallback
# that only ever runs where nothing can check it is wrong the first time it
# matters.
#
# **And the hydra.pro arm is no longer behind the tool.** It was the third
# branch of an if/elif chain whose first branch caught a missing
# `dpkg-parsechangelog`, so a check that reads one local file with `grep` and
# needs no dpkg at all was unreachable on any machine without dpkg-dev. Two
# checks were lost to one absent tool.
version-check:
	@file=$$(cat VERSION); \
	line=$$(sed -n '1s/^[^ ]* (\([^)]*\)).*/\1/p' debian/changelog); \
	if [ -z "$$line" ]; then \
		echo "version-check: debian/changelog line 1 names no version" >&2; \
		sed -n '1p' debian/changelog >&2; \
		exit 1; \
	fi; \
	tool=$$(dpkg-parsechangelog -SVersion 2>/dev/null); \
	if [ -n "$$tool" ] && [ "$$tool" != "$$line" ]; then \
		echo "version-check: dpkg-parsechangelog reads $$tool where" >&2; \
		echo "               line 1 reads $$line -- this rule's own" >&2; \
		echo "               fallback is wrong, not the changelog" >&2; \
		exit 1; \
	fi; \
	if [ "$$file" != "$$line" ]; then \
		echo "version-check: VERSION says $$file but" >&2; \
		echo "               debian/changelog says $$line" >&2; \
		exit 1; \
	fi; \
	if ! grep -q 'VERSION *= *\$$\$$cat(VERSION' hydra.pro; then \
		echo "version-check: hydra.pro states a version instead of reading" >&2; \
		echo "               VERSION; the two will drift" >&2; \
		exit 1; \
	fi; \
	if [ -n "$$tool" ]; then \
		echo "version-check: $$file, in step (both readers agree)"; \
	else \
		echo "version-check: $$file, in step (no dpkg-dev; read line 1)"; \
	fi

deb-check: deb
	@deb=$$(ls -t $(DEB_DIR)/*.deb | head -1); \
	 dpkg-deb --info "$$deb"; \
	 echo "--- contents ---"; \
	 dpkg-deb --contents "$$deb"

# The apk androiddeployqt produces is called `android-build-debug.apk` and sits
# five directories down inside the build tree. Copied out under a name that says
# what it is, because "which of these is the one I just built" is a question a
# packaging target should not leave anyone asking.
#
# **The apk is asked what it contains before it is named after it.** The name
# is what made the old bug invisible: it was the only thing that ever said
# which architecture the file was, and it said so by string substitution. So
# the zip is opened -- `lib/<abi>/` is where Android looks for native code --
# and a build that did not honour the kit is refused rather than published
# under a name it has not earned.
#
# python3 rather than aapt2: the check must run wherever the build does, and
# aapt2 lives in a build-tools version the Makefile would then have to pick.
# The gate already requires python3. `aapt2 dump badging` is still the richer
# thing to run by hand, and is what project.md's verification used.
# The Play bundle. Its own target because a bundle is not an APK: Play
# resigns it, so an unsigned or debug-signed one is not a thing to upload
# and a versionCode Play has already taken cannot be reused.
#
# Refuses without a keystore rather than producing a debug-signed bundle
# under a message announcing a release -- which is the failure beerssh paid
# for, and the reason tool/android.mk verifies signatures at all.
#
# Depends on android-build rather than on a qmake step of its own: qmake
# runs inside that rule, so there is nothing else to hang this off without
# splitting it, and the generated tree is what `aab` needs.
android-aab: android-build
	@if [ -z "$(ANDROID_KEYSTORE)" ]; then \
		echo "android-aab: ANDROID_KEYSTORE is not set." >&2; \
		echo "android-aab:   Play will not take a debug-signed bundle, and a" >&2; \
		echo "android-aab:   versionCode it HAS taken cannot be reused." >&2; \
		exit 1; \
	fi
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ANDROID_NDK_ROOT=$(ANDROID_NDK_ROOT) \
	  JAVA_HOME=$(JAVA_HOME) \
	  $(MAKE) --no-print-directory -C $(ANDROID_BUILD_DIR) aab

android: android-build
	@src=$$(find $(ANDROID_BUILD_DIR) -name '*.apk' -newer $(ANDROID_BUILD_DIR) \
	         -print 2>/dev/null | head -1); \
	 test -n "$$src" || src=$$(find $(ANDROID_BUILD_DIR) -name '*.apk' | head -1); \
	 test -n "$$src" || { echo "no apk produced" >&2; exit 1; }; \
	 abis=$$(python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print(' '.join(sorted({n.split('/')[1] for n in z.namelist() if n.startswith('lib/') and n.count('/') > 1})))" "$$src"); \
	 test -n "$$abis" || { \
	   echo "$$src carries no native code at all; not naming it for an ABI" >&2; \
	   exit 1; }; \
	 test "$$abis" = "$(ANDROID_ABI)" || { \
	   echo "apk carries native code for: $$abis" >&2; \
	   echo "  but ANDROID_ABI=$(ANDROID_ABI); refusing to name it that" >&2; \
	   echo "  apk: $$src" >&2; \
	   exit 1; }; \
	 out=$(ANDROID_ARTIFACT); \
	 cp "$$src" "$$out"; \
	 echo "$$out"; \
	 echo "  native code: $$abis (read from the apk, not assumed)"; \
	 echo "  install it with: adb install -r $$out"

# **Ask the kit what it is; do not read it off the path.** The Qt6Core a kit
# ships carries the ABI in its filename in Android's own spelling --
# `libQt6Core_arm64-v8a.so` -- and that is the same string that ends up in the
# apk's `lib/` directory, which makes it the thing worth comparing against.
#
# `qmake -query QT_ARCH` was the obvious source and was tried first: all four
# kits on this machine answer `**Unknown**`, so it cannot tell them apart. The
# directory name would work today and is Qt's to change; the library name is
# what the linker and Android both actually use.
#
# A kit that will not say what it is fails rather than passes. An unconfirmed
# ABI is the state this whole check exists to refuse.
# android-abi-check is gone: the fragment's android-check does all of it,
# including the libQt6Core_<abi>.so confirmation that started here.

# **The kit's own qmake, not the host's.** `~/Qt/<ver>/android_<abi>/bin/qmake`
# is configured for that ABI and knows where the kit's Qt libraries are; the
# system qmake6 would produce an x86-64 build with Android in its name.
#
# The SDK, NDK and JDK go in the environment rather than on the command line
# because that is where Qt's android mkspec reads them from, and androiddeployqt
# reads the same variables again in the second step.
# **Does our vendored build.gradle still match the kit it was taken from?**
#
# android/build.gradle exists only to add one Gradle dependency
# (androidx.webkit, for document-start script injection), and androiddeployqt
# offers no way to add one without shipping the whole file. So it is Qt's own
# template with a header and one line added -- and it is therefore coupled to
# the Qt version, exactly as AndroidManifest.xml beside it already is.
#
# The failure this prevents is silent: a kit update changes the template, our
# copy keeps the old contents, and the build carries on using them. Nothing
# would say so, and the difference could be anything from a new lint option to
# a Gradle plugin version the rest of the toolchain expects.
#
# Compared with the added line and the added header removed, so the comparison
# is against what we actually took. It fails rather than warns: if this
# diverges, the vendored copy is stale and what it is building with is not
# what the kit intends.
android-gradle-check:
	@tmpl="$(QT_ANDROID_ROOT)/src/android/templates/build.gradle"; \
	 if [ ! -f "$$tmpl" ]; then \
		echo "android-gradle-check: no template at $$tmpl" >&2; \
		echo "  QT_ANDROID_ROOT does not look like a Qt-for-Android kit." >&2; \
		exit 2; \
	 fi; \
	 mkdir -p $(BUILD_DIR); \
	 sed '/HYDRA-ADDED-BEGIN/,/HYDRA-ADDED-END/d' android/build.gradle \
	   > $(BUILD_DIR)/gradle-ours.tmp; \
	 if [ ! -s $(BUILD_DIR)/gradle-ours.tmp ]; then \
		echo "android-gradle-check: stripping the markers left nothing." >&2; \
		echo "  The markers are wrong, not the template." >&2; \
		exit 1; \
	 fi; \
	 if diff -q "$$tmpl" $(BUILD_DIR)/gradle-ours.tmp >/dev/null 2>&1; then \
		echo "android-gradle-check: android/build.gradle is the kit's template plus our one dependency"; \
	 else \
		echo "android-gradle-check: android/build.gradle no longer matches" >&2; \
		echo "  $$tmpl" >&2; \
		echo "  The kit has moved. Re-apply the androidx.webkit dependency on" >&2; \
		echo "  top of the new template, keeping the HYDRA-ADDED markers." >&2; \
		echo "  What differs (kit on the left, ours on the right):" >&2; \
		diff "$$tmpl" $(BUILD_DIR)/gradle-ours.tmp | head -20 >&2; \
		exit 1; \
	 fi

android-build: android-check android-gradle-check
	@test -d "$(ANDROID_NDK_ROOT)" || { echo "no NDK at $(ANDROID_NDK_ROOT)"; exit 2; }
	@test -d "$(ANDROID_SDK_ROOT)" || { echo "no SDK at $(ANDROID_SDK_ROOT)"; exit 2; }
	@# **Asks for javac, because that is what the message is about.** This was
	@# `test -d`, which a JRE passes -- a JRE is a directory -- so the one
	@# cause it named was the one cause it could not detect, and it fired only
	@# for a cause it did not name. `bin/javac` is absent from a JRE and from a
	@# path that is not there, so the condition now covers both and the message
	@# can say so honestly.
	@test -x "$(JAVA_HOME)/bin/javac" || { \
		echo "no JDK at $(JAVA_HOME): no bin/javac there." >&2; \
		echo "  Either the path is wrong or it is a JRE; Gradle needs a JDK." >&2; \
		exit 2; }
	@mkdir -p $(ANDROID_BUILD_DIR)
	cd $(ANDROID_BUILD_DIR) && \
	  ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ANDROID_NDK_ROOT=$(ANDROID_NDK_ROOT) \
	  JAVA_HOME=$(JAVA_HOME) \
	  $(QT_ANDROID_ROOT)/bin/qmake $(CURDIR)/hydra.pro CONFIG+="$(QMAKE_CONFIG)" \
	    ANDROID_VERSION_CODE=$(ANDROID_VERSION_CODE)
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ANDROID_NDK_ROOT=$(ANDROID_NDK_ROOT) \
	  JAVA_HOME=$(JAVA_HOME) \
	  $(MAKE) --no-print-directory -C $(ANDROID_BUILD_DIR) -j$(JOBS)
	ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ANDROID_NDK_ROOT=$(ANDROID_NDK_ROOT) \
	  JAVA_HOME=$(JAVA_HOME) \
	  $(MAKE) --no-print-directory -C $(ANDROID_BUILD_DIR) apk

# A staged install -- DESTDIR set -- never refreshes a cache: it would touch
# the build host, and the caches it writes end up inside the package, where
# lintian rejects the mimeinfo one outright. dpkg's triggers do both on the
# installing machine, which is the only correct moment. Setting this here
# rather than in each packaging caller means a new caller cannot forget it,
# which is how it was forgotten.
install: all
	@install -Dm755 $(BUILD_DIR)/$(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@HYDRA_SKIP_CACHE_UPDATE=$${DESTDIR:+1} \
	 sh packaging/install-icons.sh $(DESTDIR)$(SHARE)
	@echo "installed $(TARGET) to $(DESTDIR)$(PREFIX)/bin, icons and desktop entry to $(DESTDIR)$(SHARE)"

uninstall:
	@rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@rm -f $(DESTDIR)$(SHARE)/applications/hydra.desktop
	@rm -f $(DESTDIR)$(SHARE)/icon/hicolor/*/apps/hydra.png
	@echo "removed $(TARGET), its desktop entry and its icons"

# The build output only. `evidence/` is deliberately not touched: it is
# gitignored, it is what the numbers in project.md refer to, and re-capturing
# is not the same as re-reading because the sites move on.
# Each directory is checked before it is removed, and named as it goes. These
# are build trees, created by the build and disposable by construction, which is
# the one shape where clearing a directory wholesale is acceptable at all -- but
# every one of them is an overridable variable, and an unset or mistyped
# override in an `rm -rf $$(VAR)` is exactly how a clean target eats something
# it should not. `make clean BUILD_DIR=$$HOME` must not be a way to lose a home
# directory.
#
# The test is deliberately strict rather than clever: non-empty, relative, no
# `..`, and not `.` itself. Anything else is refused and said out loud, because
# a clean that silently skipped what it was asked to remove is its own problem.
clean:
	@for d in $(sort $(BUILD_DIR) $(TESTS_DIR) $(ANDROID_BUILD_DIR) $(ANDROID_BUILD_DIRS)); do \
	   case "$$d" in \
	     "" ) echo "refusing to remove an empty path"; continue ;; \
	     /* ) echo "refusing to remove absolute path: $$d"; continue ;; \
	     . | ./ ) echo "refusing to remove the source tree: $$d"; continue ;; \
	     *..* ) echo "refusing to remove a path containing '..': $$d"; continue ;; \
	   esac; \
	   test -e "$$d" || continue; \
	   rm -rf "$$d" && echo "removed $$d"; \
	 done
	@echo "evidence/ and the source tree are untouched"

# DOC CHECKS
#   Both of these have caught real drift in project.md, which is the file the
#   next session trusts: a section that was said twice in two contradicting
#   versions, and four filenames that had been renamed years of commits ago
#   while the prose beside them stayed true. Nothing compiles a table of
#   filenames, so nothing else can notice.
style-docs:
	python3 tool/style_gate.py docs
	@# The shared gate checks backticked paths in table rows, which covers
	@# most of what this target used to do by hand, and checks repeated
	@# headings at every level rather than just `###`. What it cannot read is
	@# this document's `foo.{h,cpp}` shorthand, used 72 times in the layout
	@# table and nowhere in any sibling project, so that stays here.
	@#
	@# **The count is asserted, not just stated.** The loop below iterates a
	@# list this recipe computes, so an extraction that matches nothing runs
	@# zero times, reports nothing missing and exits 0 -- a clean pass over an
	@# empty set, which is the shape this repository keeps meeting. Changing
	@# one word in the table's heading is enough to do it.
	@#
	@# **And the range no longer ends at a blank line.** `/^$$/` as the
	@# terminator meant a blank line inserted mid-table silently truncated the
	@# set, losing every name below it without changing the verdict. Ending at
	@# the first line that starts with something other than `|` cannot be
	@# tripped that way, because a blank line has no first character to match.
	@#
	@# The floor is deliberately below today's count rather than equal to it:
	@# this table grows and shrinks with the source, and the failure worth
	@# catching is collapse, not drift. `ci.yml` makes the same trade for the
	@# suite list.
	@names=$$(sed -n '/^| Area | Files | Notes |/,/^[^|]/p' project.md | \
	          grep -oE '`[a-z_]+\.\{h,cpp\}`' | tr -d '`' | \
	          sed 's/\.{h,cpp}/.h/' | sort -u); \
	 n=$$(printf '%s\n' "$$names" | grep -c .); \
	 if [ "$$n" -lt 60 ]; then \
		echo "style-docs: $$n shorthand names found in the layout table," >&2; \
		echo "            fewer than the 60 expected. The table did not shrink" >&2; \
		echo "            by itself: the heading probably changed, so the" >&2; \
		echo "            extraction now matches nothing and would pass." >&2; \
		exit 1; \
	 fi; \
	 miss=0; for f in $$names; do \
	   [ -f "src/$$f" ] || { echo "project.md names a file that does not exist: src/$$f" >&2; miss=1; }; \
	 done; \
	 if [ $$miss -eq 0 ]; then \
		echo "style-docs: $$n names in the layout table, every one present in src/"; \
	 fi; \
	 exit $$miss

help:
	@sed -n '/^# TARGETS/,/^# BUILD FLAGS/p' $(firstword $(MAKEFILE_LIST)) | \
	   sed '$$d' | sed 's/^#  \{0,1\}//' | sed '/^#*$$/d'

# `style` is every consistency gate this project has: the shared source gate,
# and the doc check that keeps project.md honest about the tree it describes.
# The JNI names, checked as text: a native method and its C++ symbol are
# joined at runtime by string equality and by nothing else, and this tree
# shipped a rename that broke seven of them. Costs milliseconds and needs no
# Android tooling, so it runs with the other gates rather than only when
# somebody builds an APK.
style: style-source style-docs jni

jni:
	@python3 tool/jni_check.py

style-source:
	python3 tool/style_gate.py check

# `check` is everything that must pass before committing; `test` is the suite
# alone. GNU's meaning of check, and what most of these projects already did.
check: style test

# The clean ladder, matching the sibling projects: `clean` removes build
# products, `veryclean` adds the build directories themselves, `distclean`
# adds editor and tool droppings.
veryclean: clean
	@for d in $(sort $(BUILD_DIR) $(ANDROID_BUILD_DIR) $(ANDROID_BUILD_DIRS) $(TESTS_DIR)); do \
	   case "$$d" in \
	     "" | /* | . | ./ ) echo "veryclean: refusing to remove '$$d'" ;; \
	     * ) rm -rf "$$d" && echo "veryclean: removed $$d" ;; \
	   esac; \
	 done

# **`distclean` no longer sweeps the tree for editor droppings.** `*~`,
# `*.swp` and `*.orig` are not build output: they belong to somebody's
# editor, and a `.orig` belongs to a merge they may be in the middle of.
# The sweep was also unbounded -- `find .` walks `.git` and `third_party/`,
# and it was measured deleting files inside both, neither of which this
# build created. `git clean -xdn` lists that class and is the person's call
# rather than the build system's.
#
# What is left is what the tooling here really wrote. The search is a
# wildcard because a `__pycache__` appears beside whatever Python ran, but
# the thing removed is named exactly and is disposable by construction;
# `.git` and `third_party/` are pruned, and every removal is printed,
# because a clean target that deletes silently is one nobody can check.
distclean: veryclean
	@find . -name .git -prune -o -name third_party -prune -o \
	        -name __pycache__ -type d -prune -print -exec rm -rf {} +

# The commit-msg hook lives in the tree so it is reviewable, survives a
# clone, and can be kept in sync. .git/hooks is untracked, so a hook that
# exists only there enforces a rule nobody can see and vanishes silently on
# a fresh clone.
# **Where the hooks live is git's question, not the filesystem's.** This
# asked `test -d .git`, which is false in a linked worktree and in a
# submodule checkout: there `.git` is a regular FILE naming the real
# gitdir, and both are git repositories. So `make hooks` refused with
# "not a git repository" inside one that is.
#
# Testing -e instead would only move the failure one line down, because
# the install writes into `.git/hooks/`, which is not a directory there
# either. Asking git answers both halves at once, and from a worktree it
# returns the MAIN repository's hooks directory -- which is the one git
# actually runs.
#
# git's absence is reported as its own thing rather than as "not a git
# repository", which would be a message naming a cause nothing tested.
hooks:
	@if ! command -v git >/dev/null 2>&1; then \
		echo "hooks: git is not installed, so there is nowhere to install to." >&2; \
		exit 1; \
	fi; \
	dir=$$(git rev-parse --git-common-dir 2>/dev/null); \
	if [ -z "$$dir" ]; then \
		echo "hooks: not a git repository, so there is nowhere to install to." >&2; \
		exit 1; \
	fi; \
	mkdir -p "$$dir/hooks"; \
	install -m 0755 tool/hooks/commit-msg "$$dir/hooks/commit-msg"; \
	echo "hooks: commit-msg installed from tool/hooks/ into $$dir/hooks/"
