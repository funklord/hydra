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
#     * **plain Make** (`tests/Makefile`) builds the test tree.
#     * **fmake** builds the same sources from no build file at all, as a
#       cross-check and a second opinion. What it has to be told is six
#       annotations in the sources -- `@target` and five `@pkg_optional` --
#       and nothing else.
#
#   The four problems a migration had to solve, and what solved them:
#
#     * **72 executables**, globbed rather than listed, so adding a suite or a
#       driver is no build-system work. `tests/Makefile` globs `test_*.cpp`
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
#                        or a model are listed but not run (see tests/README.md)
#   make test-one T=x -- build and run a single suite, e.g. T=test_theme
#   make drivers      -- build the live drivers (expensive; see JOBS below)
#   make sweep        -- build them and run them all, with a summary (offscreen;
#                        SWEEP_ONSCREEN=1 uses the real display)
#   make replay       -- re-score recorded model replies against the gate (no model)
#   make deb          -- build a .deb into build/deb (dependencies computed)
#   make deb-check    -- build it and print what it declares and contains
#   make apk          -- build a signed-for-debug Android .apk
#   make android      -- same as `apk`, kept because it was the older name
#   make install      -- install the binary, desktop entry and icon set
#   make uninstall    -- remove what install put there
#   make clean        -- remove build output, leaving the source tree alone
#   make style        -- the shared source gate plus this repo's project.md
#                        heading checks (see DOC CHECKS)
#   make check        -- style and test together, the one to run before a commit
#   make hooks        -- install the git hooks from tools/hooks/
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
#     make android QT_ANDROID_ROOT=$HOME/Qt/6.11.1/android_arm64_v8a
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
TESTS_DIR  ?= tests/build-make
TREE       ?= sample-tree.txt

PREFIX ?= $(HOME)/.local
SHARE  ?= $(PREFIX)/share

# Android. Kept together so the "you are missing one" message can name them.
QT_ANDROID_ROOT   ?= $(HOME)/Qt/6.11.1/android_arm64_v8a
# Only used to name the copied apk; the kit above decides the real ABI.
ANDROID_ABI       ?= arm64-v8a
ANDROID_NDK_ROOT  ?= $(HOME)/android-ndk-r29
ANDROID_SDK_ROOT  ?= $(HOME)/Android/Sdk
JAVA_HOME         ?= $(HOME)/android-studio/jbr
ANDROID_BUILD_DIR ?= build-android

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
# property the live-driver glob has, and the reason the CMake side was kept.
# The excluded ones each need something the machine may not have; tests/README.md
# says which, and they are named at the end of a run rather than silently
# skipped.
NEEDS_MORE = test_headers test_dlheaders test_helpers_live test_probe \
             test_probe_ui test_torrent test_watch test_live_model \
             test_ytdlp_live test_replay
ALL_SUITES = $(basename $(notdir $(wildcard tests/test_*.cpp)))
SUITES     = $(filter-out $(NEEDS_MORE),$(ALL_SUITES))

# Offscreen because none of these want a window, and a keyring item of their
# own because `test_credstore` writes and deletes one: under the real name that
# item is the KeePassXC pairing the user actually uses, and the suite refuses to
# run rather than risk it.
# `--mute-audio` because a suite that loads a page should not play it at
# whoever is sitting at the machine. Chromium's own flag; WebEngine honours it.
TEST_ENV = QT_QPA_PLATFORM=offscreen HYDRA_SECRET_KIND=hydra-make-test \
           QTWEBENGINE_CHROMIUM_FLAGS=--mute-audio

# Where a failing suite's whole output is kept. This target used to print the
# tail line and the first five FAIL lines and throw the rest away, which is fine
# for a suite that fails every time and useless for one that does not: an
# intermittent failure is the case where the output matters most and it was the
# case being discarded. Seen twice on test_extloop, both times on the first run
# after a source change and never reproducible afterwards, with nothing kept.
FAILED_DIR = $(TESTS_DIR)/failed

.PHONY: all run test test-one drivers sweep replay deb deb-check apk android install uninstall clean help style style-docs style-source check hooks

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
# `tests/Makefile` is the test tree's own build and needs no configure step,
# so there is nothing here standing in for a cache file. Its BUILD_DIR is
# `build-make` relative to `tests/`, which is why the target named below is
# spelled without the `tests/` that TESTS_DIR carries.
test:
	@for t in $(SUITES); do $(MAKE) --no-print-directory -C tests -j$(JOBS) \
	   build-make/$$t >/dev/null || exit 1; done
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
	 echo "  see tests/README.md for what each one wants"; \
	 exit $$fail

test-one:
	@test -n "$(T)" || { echo "usage: make test-one T=test_theme"; exit 2; }
	@$(MAKE) --no-print-directory -C tests -j$(JOBS) build-make/$(T)
	@$(TEST_ENV) ./$(TESTS_DIR)/$(T)

# Separate from `test` because it is a different order of cost: each driver
# compiles the whole app and links WebEngine, and they want a real display.
drivers:
	@$(MAKE) --no-print-directory -C tests -j$(JOBS) all
	@echo "live drivers built. Run them offscreen: QT_QPA_PLATFORM=offscreen ./$(TESTS_DIR)/try_cookies"
	@echo "tests/README.md says which need a helper server, KeePassXC or a model."

# Score the recorded model replies against the current gate. No model, no
# network, milliseconds -- but it needs the corpus in evidence/replies, which is
# not in git, so it cannot be part of `make test`.
replay:
	@$(MAKE) --no-print-directory -C tests -j$(JOBS) build-make/test_replay >/dev/null
	@$(TEST_ENV) ./$(TESTS_DIR)/test_replay

# Run them all and summarise. Offscreen by default, so it does not take over a
# desktop; SWEEP_ONSCREEN=1 uses the real display, which is only needed when
# appearance rather than behaviour is the question.
# Pass DRIVERS=... for a subset: make sweep DRIVERS="try_import try_delete".
sweep: drivers
	@tests/live/sweep.sh $(DRIVERS)

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
version-check:
	@file=$$(cat VERSION); \
	changelog=$$(dpkg-parsechangelog -SVersion 2>/dev/null); \
	if [ -z "$$changelog" ]; then \
		echo "version-check: skipped (dpkg-parsechangelog unavailable)"; \
	elif [ "$$file" != "$$changelog" ]; then \
		echo "version-check: VERSION says $$file but"; \
		echo "               debian/changelog says $$changelog"; \
		exit 1; \
	elif ! grep -q 'VERSION *= *\$$\$$cat(VERSION' hydra.pro; then \
		echo "version-check: hydra.pro states a version instead of reading"; \
		echo "               VERSION; the two will drift"; \
		exit 1; \
	else \
		echo "version-check: $$file, in step"; \
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
apk: android
	@src=$$(find $(ANDROID_BUILD_DIR) -name '*.apk' -newer $(ANDROID_BUILD_DIR) \
	         -print 2>/dev/null | head -1); \
	 test -n "$$src" || src=$$(find $(ANDROID_BUILD_DIR) -name '*.apk' | head -1); \
	 test -n "$$src" || { echo "no apk produced"; exit 1; }; \
	 out=$(ANDROID_BUILD_DIR)/hydra-$(VERSION)-$(ANDROID_ABI)-debug.apk; \
	 cp "$$src" "$$out"; \
	 echo "$$out"; \
	 echo "install it with: adb install -r $$out"

# **The kit's own qmake, not the host's.** `~/Qt/<ver>/android_<abi>/bin/qmake`
# is configured for that ABI and knows where the kit's Qt libraries are; the
# system qmake6 would produce an x86-64 build with Android in its name.
#
# The SDK, NDK and JDK go in the environment rather than on the command line
# because that is where Qt's android mkspec reads them from, and androiddeployqt
# reads the same variables again in the second step.
android:
	@test -x "$(QT_ANDROID_ROOT)/bin/qmake" || \
	  { echo "no Qt-for-Android kit at $(QT_ANDROID_ROOT)"; \
	    echo "set QT_ANDROID_ROOT to the kit for your ABI"; exit 2; }
	@test -d "$(ANDROID_NDK_ROOT)" || { echo "no NDK at $(ANDROID_NDK_ROOT)"; exit 2; }
	@test -d "$(ANDROID_SDK_ROOT)" || { echo "no SDK at $(ANDROID_SDK_ROOT)"; exit 2; }
	@test -d "$(JAVA_HOME)" || { echo "no JDK at $(JAVA_HOME) -- Gradle will not run on a JRE"; exit 2; }
	@mkdir -p $(ANDROID_BUILD_DIR)
	cd $(ANDROID_BUILD_DIR) && \
	  ANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT) ANDROID_NDK_ROOT=$(ANDROID_NDK_ROOT) \
	  JAVA_HOME=$(JAVA_HOME) \
	  $(QT_ANDROID_ROOT)/bin/qmake $(CURDIR)/hydra.pro CONFIG+="$(QMAKE_CONFIG)"
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
	@rm -f $(DESTDIR)$(SHARE)/icons/hicolor/*/apps/hydra.png
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
	@for d in $(BUILD_DIR) $(TESTS_DIR) $(ANDROID_BUILD_DIR); do \
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
	python3 tools/style_gate.py docs
	@# The shared gate checks backticked paths in table rows, which covers
	@# most of what this target used to do by hand, and checks repeated
	@# headings at every level rather than just `###`. What it cannot read is
	@# this document's `foo.{h,cpp}` shorthand, used 31 times in the layout
	@# table and nowhere in any sibling project, so that stays here.
	@miss=0; for f in $$(sed -n '/^| Area | Files | Notes |/,/^$$/p' project.md | \
	       grep -oE '`[a-z_]+\.\{h,cpp\}`' | tr -d '`' | sed 's/\.{h,cpp}/.h/' | sort -u); do \
	   [ -f "src/$$f" ] || { echo "project.md names a file that does not exist: src/$$f"; miss=1; }; \
	 done; exit $$miss

help:
	@sed -n '/^# TARGETS/,/^# BUILD FLAGS/p' $(firstword $(MAKEFILE_LIST)) | \
	   sed '$$d' | sed 's/^#  \{0,1\}//' | sed '/^#*$$/d'

# `style` is every consistency gate this project has: the shared source gate,
# and the doc check that keeps project.md honest about the tree it describes.
style: style-source style-docs

style-source:
	python3 tools/style_gate.py check

# `check` is everything that must pass before committing; `test` is the suite
# alone. GNU's meaning of check, and what most of these projects already did.
check: style test

# The clean ladder, matching the sibling projects: `clean` removes build
# products, `veryclean` adds the build directories themselves, `distclean`
# adds editor and tool droppings.
veryclean: clean
	@for d in $(BUILD_DIR) $(ANDROID_BUILD_DIR) $(TESTS_DIR); do \
	   case "$$d" in \
	     "" | /* | . | ./ ) echo "veryclean: refusing to remove '$$d'" ;; \
	     * ) rm -rf "$$d" && echo "veryclean: removed $$d" ;; \
	   esac; \
	 done

distclean: veryclean
	find . -name '*~' -o -name '*.swp' -o -name '*.orig' | xargs -r rm -f
	find . -name __pycache__ -type d -prune -exec rm -rf {} +

# The commit-msg hook lives in the tree so it is reviewable, survives a
# clone, and can be kept in sync. .git/hooks is untracked, so a hook that
# exists only there enforces a rule nobody can see and vanishes silently on
# a fresh clone.
hooks:
	@test -d .git || { echo "hooks: not a git repository" >&2; exit 1; }
	@install -m 0755 tools/hooks/commit-msg .git/hooks/commit-msg
	@echo "hooks: commit-msg installed from tools/hooks/"
