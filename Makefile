# =============================================================================
# Top-level Makefile -- Hydra
#
# PURPOSE
#   The single entry point for the whole tree, and the reason it exists is
#   consistency rather than capability: beerssh and fuzzypickles' gui/ subtree
#   both present `make`, `make test`, `make android`, `make install` with
#   DEBUG=1 / SANITIZE=1, and hydra presented two different cmake invocations
#   and a per-binary test run you had to know to prefix with
#   QT_QPA_PLATFORM=offscreen. This file makes the three look like one
#   interface -- the same wrapper split beerssh describes, with CMake
#   underneath instead of qmake.
#
#   **CMake stays underneath for now, and the migration is deliberately left
#   open.** The wrapper is the cheap half of the consistency win, and it is
#   worth being honest that it is the half that does not settle the argument:
#   a Makefile is easier to read than CMakeLists.txt, and the strongest thing
#   CMake brings here is dependency discovery.
#
#   What a later migration would have to solve, so it is written down while it
#   is fresh rather than rediscovered:
#
#     * **54 executables.** 33 offline suites and 21 live drivers, and the
#       drivers are globbed -- `file(GLOB CONFIGURE_DEPENDS live/try_*.cpp)` --
#       so adding one is zero build-system work today. That property is worth
#       preserving; in qmake it wants a generated SUBDIRS tree rather than 54
#       hand-written project files.
#     * **The library that is two libraries.** rasterbar's and rakshasa's
#       unrelated torrent libraries both install a pkg-config file called
#       `libtorrent`, and the bare name resolves to rakshasa's, which has a
#       different API. `find_package(LibtorrentRasterbar)` is unambiguous and
#       pkg-config alone is not, so a qmake build has to reach the qualified
#       `libtorrent-rasterbar` name and never the bare one.
#     * **pkg-config lying about the target.** On an Android cross build the
#       *host* pkg-config answers cheerfully -- it reported libsodium found and
#       handed back `-L/usr/lib/x86_64-linux-gnu`, so the configure announced
#       the feature enabled and the link failed much later looking like a
#       toolchain fault. Whatever replaces `pkg_check_modules` has to be asked
#       in the right architecture.
#
#   Qt 6 also treats CMake as its primary build system and qmake as maintained
#   rather than developed, which is worth knowing but is not decisive: beerssh
#   ships a Qt 6 app and an APK from qmake today.
#
# TARGETS
#   make              -- build the desktop app
#   make run          -- build and run it on the sample tree
#   make test         -- build and run every suite that needs nothing but a
#                        build; the ones needing a helper, a torrent library
#                        or a model are listed but not run (see tests/README.md)
#   make test-one T=x -- build and run a single suite, e.g. T=test_theme
#   make drivers      -- build the live drivers (expensive; see JOBS below)
#   make android      -- build the Android APK
#   make install      -- install the binary, desktop entry and icon set
#   make uninstall    -- remove what install put there
#   make clean        -- remove build output, leaving the source tree alone
#   make style        -- the repo's own checks on project.md (see DOC CHECKS)
#   make help         -- this list
#
# BUILD FLAGS
#   DEBUG and SANITIZE are never given a `?=` default, following beerssh:
#   every check is `ifdef`, so a `?= 0` default would make them permanently
#   set and impossible to turn off.
#
#     make DEBUG=1        unoptimized, symbol-rich
#     make SANITIZE=1     ASan + UBSan, independent of DEBUG
#
#   Everything else is `?=`, so the command line and the environment win:
#     make CXX=clang++ JOBS=4
#
# ⚠️ JOBS
#   **Defaults to 2, and that is not timidity.** Each live driver compiles ~61
#   app sources and links Qt WebEngine, and there are 21 of them. `make -j`
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
CMAKE ?= cmake

# See the JOBS warning above before raising this.
JOBS ?= 2

TARGET     = hydra
BUILD_DIR  ?= build
TESTS_DIR  ?= tests/build
TREE       ?= sample-tree.txt

PREFIX ?= $(HOME)/.local
SHARE  ?= $(PREFIX)/share

# Android. Kept together so the "you are missing one" message can name them.
QT_ANDROID_ROOT   ?= $(HOME)/Qt/6.11.1/android_arm64_v8a
QT_HOST_PATH      ?= $(HOME)/Qt/6.11.1/gcc_64
ANDROID_NDK_ROOT  ?= $(HOME)/android-ndk-r29
ANDROID_SDK_ROOT  ?= $(HOME)/Android/Sdk
JAVA_HOME         ?= $(HOME)/android-studio/jbr
ANDROID_BUILD_DIR ?= build-android

BUILD_TYPE ?= Release
CMAKE_FLAGS =
ifdef DEBUG
BUILD_TYPE = Debug
endif
ifdef SANITIZE
CMAKE_FLAGS += -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
CMAKE_FLAGS += -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
endif

# Suites that need nothing but a build. Written as an exclusion rather than a
# list, so a new test_*.cpp is picked up without editing this file -- the same
# property the live-driver glob has, and the reason the CMake side was kept.
# The excluded ones each need something the machine may not have; tests/README.md
# says which, and they are named at the end of a run rather than silently
# skipped.
NEEDS_MORE = test_headers test_dlheaders test_helpers_live test_probe \
             test_probe_ui test_torrent test_watch test_live_model \
             test_ytdlp_live
ALL_SUITES = $(basename $(notdir $(wildcard tests/test_*.cpp)))
SUITES     = $(filter-out $(NEEDS_MORE),$(ALL_SUITES))

# Offscreen because none of these want a window, and a keyring item of their
# own because `test_credstore` writes and deletes one: under the real name that
# item is the KeePassXC pairing the user actually uses, and the suite refuses to
# run rather than risk it.
TEST_ENV = QT_QPA_PLATFORM=offscreen HYDRA_SECRET_KIND=hydra-make-test

.PHONY: all run test test-one drivers android install uninstall clean help style

# Always delegates, never compares timestamps itself. The first version made
# the binary a real target depending on the cache file, and `make` after
# editing a source said "Nothing to be done for 'all'" -- make was answering a
# question about two files while CMake is the thing that knows about the other
# sixty. A wrapper that second-guesses the tool it wraps is worse than no
# wrapper, because it is wrong silently.
#
# The cache is an order-only prerequisite (`|`), so configure runs once and a
# newer cache does not force a rebuild.
all: | $(BUILD_DIR)/CMakeCache.txt
	@$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

$(BUILD_DIR)/CMakeCache.txt:
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_FLAGS)

run: all
	@./$(BUILD_DIR)/$(TARGET) $(TREE)

$(TESTS_DIR)/CMakeCache.txt:
	@$(CMAKE) -S tests -B $(TESTS_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_FLAGS)

# Built one target at a time on purpose: naming them individually keeps the
# live drivers out of it. `make drivers` is where that cost is opted into.
test: $(TESTS_DIR)/CMakeCache.txt
	@for t in $(SUITES); do $(CMAKE) --build $(TESTS_DIR) -j$(JOBS) --target $$t >/dev/null || exit 1; done
	@fail=0; for t in $(SUITES); do \
	   out=$$($(TEST_ENV) ./$(TESTS_DIR)/$$t 2>&1); \
	   if [ $$? -eq 0 ]; then printf '  ok   %-16s %s\n' "$$t" "$$(echo "$$out" | tail -1)"; \
	   else fail=1; printf '  FAIL %-16s %s\n' "$$t" "$$(echo "$$out" | tail -1)"; \
	        echo "$$out" | grep FAIL | head -5; fi; \
	 done; \
	 echo; echo "not run here, each needs something this target does not provide:"; \
	 echo "  $(NEEDS_MORE)"; \
	 echo "  see tests/README.md for what each one wants"; \
	 exit $$fail

test-one: $(TESTS_DIR)/CMakeCache.txt
	@test -n "$(T)" || { echo "usage: make test-one T=test_theme"; exit 2; }
	@$(CMAKE) --build $(TESTS_DIR) -j$(JOBS) --target $(T)
	@$(TEST_ENV) ./$(TESTS_DIR)/$(T)

# Separate from `test` because it is a different order of cost: each driver
# compiles the whole app and links WebEngine, and they want a real display.
drivers: $(TESTS_DIR)/CMakeCache.txt
	@$(CMAKE) --build $(TESTS_DIR) -j$(JOBS)
	@echo "live drivers built. They need a display: DISPLAY=:0 ./$(TESTS_DIR)/try_cookies"
	@echo "tests/README.md says which need a helper server, KeePassXC or a model."

android:
	@test -x "$(QT_ANDROID_ROOT)/bin/qt-cmake" || \
	  { echo "no Qt-for-Android kit at $(QT_ANDROID_ROOT)"; \
	    echo "set QT_ANDROID_ROOT to the kit for your ABI"; exit 2; }
	@test -d "$(ANDROID_NDK_ROOT)" || { echo "no NDK at $(ANDROID_NDK_ROOT)"; exit 2; }
	@test -d "$(ANDROID_SDK_ROOT)" || { echo "no SDK at $(ANDROID_SDK_ROOT)"; exit 2; }
	@test -d "$(JAVA_HOME)" || { echo "no JDK at $(JAVA_HOME) -- Gradle will not run on a JRE"; exit 2; }
	$(QT_ANDROID_ROOT)/bin/qt-cmake -S . -B $(ANDROID_BUILD_DIR) \
	    -DQT_HOST_PATH=$(QT_HOST_PATH) \
	    -DANDROID_NDK_ROOT=$(ANDROID_NDK_ROOT) \
	    -DANDROID_SDK_ROOT=$(ANDROID_SDK_ROOT)
	JAVA_HOME=$(JAVA_HOME) $(CMAKE) --build $(ANDROID_BUILD_DIR) -j$(JOBS) --target apk

install: all
	@install -Dm755 $(BUILD_DIR)/$(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@sh packaging/install-icons.sh $(DESTDIR)$(SHARE)
	@echo "installed $(TARGET) to $(DESTDIR)$(PREFIX)/bin, icons and desktop entry to $(DESTDIR)$(SHARE)"

uninstall:
	@rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@rm -f $(DESTDIR)$(SHARE)/applications/hydra.desktop
	@rm -f $(DESTDIR)$(SHARE)/icons/hicolor/*/apps/hydra.png
	@echo "removed $(TARGET), its desktop entry and its icons"

# The build output only. `evidence/` is deliberately not touched: it is
# gitignored, it is what the numbers in project.md refer to, and re-capturing
# is not the same as re-reading because the sites move on.
clean:
	@rm -rf $(BUILD_DIR) $(TESTS_DIR) $(ANDROID_BUILD_DIR)
	@echo "removed build output; evidence/ and the source tree are untouched"

# DOC CHECKS
#   Both of these have caught real drift in project.md, which is the file the
#   next session trusts: a section that was said twice in two contradicting
#   versions, and four filenames that had been renamed years of commits ago
#   while the prose beside them stayed true. Nothing compiles a table of
#   filenames, so nothing else can notice.
style:
	@dup=$$(grep '^###' project.md | sort | uniq -d); \
	 if [ -n "$$dup" ]; then echo "project.md says these twice:"; echo "$$dup"; exit 1; fi
	@miss=0; for f in $$(sed -n '/^| Area | Files | Notes |/,/^$$/p' project.md | \
	       grep -oE '`[a-z_]+\.\{h,cpp\}`' | tr -d '`' | sed 's/\.{h,cpp}/.h/' | sort -u); do \
	   [ -f "src/$$f" ] || { echo "project.md names a file that does not exist: src/$$f"; miss=1; }; \
	 done; exit $$miss
	@echo "style: project.md says nothing twice and names no missing files"

help:
	@sed -n '/^# TARGETS/,/^# BUILD FLAGS/p' $(firstword $(MAKEFILE_LIST)) | \
	   sed '$$d' | sed 's/^#  \{0,1\}//' | sed '/^#*$$/d'
