#
# The test tree, for plain Make. Written to be compared against a
# CMakeLists.txt beside it, and now the only build it has: `make test` at the
# top level drives this file. See project.md for what the comparison found.
#
#   make -C test -j2          # everything
#   make -C test offline -j2  # just the suites that need nothing but a build
#
# Always name a job count. `-j` with no number is *unlimited* under Make, and
# each live driver links Qt WebEngine -- that has taken this machine's desktop
# session down twice.
#
# **Per-binary link sets, and no archive.** This used to build the app's
# sources into one `libhydra_app.a` and link all seventy-odd binaries against
# it, on the reasoning that an archive contributes only the members that
# resolve something -- true, and it still meant rebuilding the archive whenever
# any input changed and relinking everything that used it, a flat 19s whether
# the change reached three files or eight. `build-and-commit.md` has a rule
# against exactly that.
#
# The archive was standing in for a computation: which objects does this binary
# actually need. `tool/objsets.py` asks fmake to perform it -- compile, read
# the symbol tables, close transitively over undefined symbols, which is what
# the linker does when picking archive members -- and writes the answer to
# `objsets.mk`. Each binary now links its own closure and relinks only when
# something in it changed. It also stops linking the test-side moc into all of
# them: two programs need it.

BUILD_DIR ?= build-make
SRC        = ../src
CXX       ?= g++
JOBS      ?= 2

# The same flags the app is built with. Qt's pkg-config files are the whole
# discovery mechanism here: `find_package` has no equivalent, and everything
# CMake knew about Qt comes back through these.
# **Two link sets, because Qt WebEngine is not like the other modules.** Not one
# of the thirty-eight offline suites references the engine, and linking it into
# all of them anyway is most of what made this slower than the CMake build it
# replaces: WebEngineCore is the largest library on the machine and every link
# paid to scan it. Thirty of thirty-two live drivers do need it, and say so by
# building the real shell.
#
# Compile flags stay uniform. An unused `-I` costs nothing; an unused `-l`
# costs a scan of every symbol in the library.
QT_MODULES  = Qt6Widgets Qt6Network Qt6Qml Qt6WebChannel Qt6Test Qt6DBus
QT_ENGINE   = Qt6WebEngineWidgets
QT_CFLAGS  := $(shell pkg-config --cflags $(QT_MODULES) $(QT_ENGINE))
QT_LIBS    := $(shell pkg-config --libs $(QT_MODULES))
ENGINE_LIBS := $(shell pkg-config --libs $(QT_ENGINE))

# Optional dependencies, each costing one feature and no more. Asked with
# `--exists` so a missing one is a smaller build rather than a failure -- and
# the qualified `libtorrent-rasterbar`, never the bare `libtorrent`, which
# resolves to a different library with a different API.
OPT_PKGS := $(strip $(foreach p,libsodium libsecret-1 liblz4 libtorrent-rasterbar,\
              $(shell pkg-config --exists $(p) && echo $(p))))
OPT_CFLAGS := $(if $(OPT_PKGS),$(shell pkg-config --cflags $(OPT_PKGS)))
OPT_LIBS   := $(if $(OPT_PKGS),$(shell pkg-config --libs $(OPT_PKGS)))
OPT_DEFS   := $(if $(filter libsodium,$(OPT_PKGS)),-DHYDRA_HAVE_SODIUM) \
               $(if $(filter libsecret-1,$(OPT_PKGS)),-DHYDRA_HAVE_SECRET) \
               $(if $(filter liblz4,$(OPT_PKGS)),-DHYDRA_HAVE_LZ4) \
               $(if $(filter libtorrent-rasterbar,$(OPT_PKGS)),-DHYDRA_HAVE_LIBTORRENT) \
               -DHYDRA_HAVE_DBUS

# -Os, and `?=` so a debug build can replace it *from the environment*:
# `CXXFLAGS='-Og -g' make ...`. Not as a command-line argument, which overrides
# the whole variable including every -I appended below, leaving compiles that
# fail instantly -- measured, and it read as a suspiciously fast rebuild.
CXXFLAGS ?= -Os -g
CXXFLAGS += -std=gnu++17 -fPIC -Wall -Wextra -I$(SRC) -I. -I$(BUILD_DIR) \
             $(QT_CFLAGS) $(OPT_CFLAGS) $(OPT_DEFS)
LDFLAGS  += -Wl,--as-needed
LDLIBS   += $(QT_LIBS) $(OPT_LIBS)

# --- the app, compiled once ------------------------------------------------
#
# Android's four sources are excluded by the word in front of their names, the
# way the app's own build excludes them. Nothing else in src/ names a platform.
APP_SRCS  := $(filter-out $(SRC)/android_%,$(wildcard $(SRC)/*.cpp))
APP_OBJS  := $(patsubst $(SRC)/%.cpp,$(BUILD_DIR)/app/%.o,$(APP_SRCS))

# **Every header carrying Q_OBJECT needs moc run on it**, and the list is
# computed rather than written: a header that gains Q_OBJECT and is not moc'd
# links with an undefined vtable, which is a confusing error a long way from
# its cause. `grep -l` is the whole mechanism CMake's AUTOMOC wraps.
# **Emptiness has to read as absence, not as a directory called "".**
#
# pkg-config answers an UNDEFINED variable on a PRESENT module with an empty
# string and exit 0, and an ABSENT module with an empty string and exit 1 --
# so the status is the only thing separating "no such Qt" from "no such
# variable", and appending /moc to either gives the literal `/moc`. The build
# then fails with a shell error naming a path that was never a path, instead
# of naming the Qt that is missing. fmake's PkgConfig.variable records the
# same rule; this is that rule in make.
QT_LIBEXECDIR := $(shell pkg-config --variable=libexecdir Qt6Core 2>/dev/null)
MOC       := $(if $(QT_LIBEXECDIR),$(QT_LIBEXECDIR)/moc,)

# Checked where moc is USED, not here: a missing Qt must not stop `make
# clean`, which needs no moc at all. One logical line, so it can be a recipe
# line rather than a `define` -- a multi-line define expanded at column zero
# is parsed as rules, and make reports it as "overriding recipe for target
# '@test'", which names nothing a reader can act on.
MOC_GUARD = test -n "$(MOC)" || { \
	   echo "tests: cannot find moc." >&2; \
	   echo "tests:   pkg-config gave no libexecdir for Qt6Core: either Qt 6" >&2; \
	   echo "tests:   is absent or its .pc file defines no such variable." >&2; \
	   echo "tests:   Install the Qt 6 development files, or name it:" >&2; \
	   echo "tests:   make -C test MOC=/path/to/moc" >&2; \
	   exit 1; }
MOC_HDRS  := $(shell grep -l Q_OBJECT $(SRC)/*.h | grep -v '$(SRC)/android_')
MOC_SRCS  := $(patsubst $(SRC)/%.h,$(BUILD_DIR)/moc/moc_%.cpp,$(MOC_HDRS))

# **One object per header, not one for all of them.** A unity translation unit
# the way AUTOMOC builds it was tried and measured, and it is the wrong trade
# *here*: it compiles the test tree about 20% faster and makes every test binary
# seven times larger -- 1.25 MB against 172 KB stripped, 2.5 GB against 1.7 GB
# across seventy -- because one object holding forty-six mocs is pulled whole by
# any suite needing one of them, and it drags most of the app in behind it.
#
# CMake gets both, and not by being cleverer: each of its targets has its own
# mocs_compilation.cpp of its own headers, which it can only do because each
# target also has its own hand-written source list. With one archive serving
# seventy binaries, granularity is what makes the archive work at all.
MOC_OBJS  := $(patsubst %.cpp,%.o,$(MOC_SRCS))

# A Q_OBJECT inside a .cpp is included by the file itself as `<name>.moc`, so
# it is generated but never compiled separately. src/network_fetcher.cpp is the
# one such file; the rule finds any other by the same test.
MOC_CPP_HDRS := $(shell grep -l Q_OBJECT $(SRC)/*.cpp)
MOC_CPP_SRCS := $(patsubst $(SRC)/%.cpp,$(BUILD_DIR)/%.moc,$(MOC_CPP_HDRS))

# The test tree has its own Q_OBJECT users, and both shapes appear here.
# `test_bridge.cpp` and `test_extloop.cpp` carry one in the .cpp and include
# their own `<name>.moc`; `fake_sources.h` carries one in a header, which needs
# a separate translation unit. CMake's AUTOMOC covers both without being told,
# and this is what being told looks like.
TMOC_CPP  := $(shell grep -l Q_OBJECT test_*.cpp live/*.cpp 2>/dev/null)
TMOC_INC  := $(patsubst %.cpp,$(BUILD_DIR)/%.moc,$(notdir $(TMOC_CPP)))
TMOC_HDRS := $(shell grep -l Q_OBJECT *.h live/*.h 2>/dev/null)
TMOC_SRCS := $(patsubst %.h,$(BUILD_DIR)/tmoc/moc_%.cpp,$(notdir $(TMOC_HDRS)))
TMOC_OBJS := $(patsubst %.cpp,%.o,$(TMOC_SRCS))

# --- the link sets ---------------------------------------------------------
#
# Generated, and committed, so an ordinary build needs `make` and nothing else.
# fmake is needed only to regenerate them, which is `make -C test objsets`.
include objsets.mk

# **The guard, and it is the point of the file carrying its own source list.**
# A generated link set that describes a tree which has moved on does not fail
# where the mistake is: it fails as an undefined symbol while linking a binary
# nobody touched, or -- worse -- links successfully against a stale set and
# leaves a suite testing code that is no longer reached. So the comparison is
# made on every build, against the same glob the rules below use, and a
# mismatch stops the build with the command that fixes it.
#
# Sorted on both sides: the file is written in sorted order and `$(wildcard)`
# is not guaranteed to be, so comparing them raw reports a difference that is
# only an ordering.
OBJSETS_FOUND := $(sort $(wildcard test_*.cpp) $(patsubst %,live/%,$(notdir $(wildcard live/try_*.cpp))))
OBJSETS_HAVE  := $(sort $(patsubst test/%,%,$(OBJSETS_SOURCES)))

ifneq ($(OBJSETS_FOUND),$(OBJSETS_HAVE))
$(warning objsets.mk was generated from a different set of sources)
$(warning   only in the tree: $(filter-out $(OBJSETS_HAVE),$(OBJSETS_FOUND)))
$(warning   only in the file:  $(filter-out $(OBJSETS_FOUND),$(OBJSETS_HAVE)))
$(error run `make -C test objsets` to regenerate it)
endif

# --- the suites ------------------------------------------------------------
OFFLINE_SRCS := $(wildcard test_*.cpp)
LIVE_SRCS    := $(wildcard live/try_*.cpp)
OFFLINE      := $(patsubst %.cpp,$(BUILD_DIR)/%,$(OFFLINE_SRCS))
LIVE         := $(patsubst live/%.cpp,$(BUILD_DIR)/%,$(LIVE_SRCS))

# Which of them reach the engine. **Structural for the drivers, grepped for the
# suites**, and the first version had it the other way round: a grep over the
# driver sources missed the two that reach the engine through the shell rather
# than by naming it, and they failed at link on `QWebEngineProfile`. Every live
# driver builds the real window, so every live driver gets the engine; that is a
# property of what a driver *is*, not of what its text happens to mention.
#
# The suites are still grepped, because they are the ones where the answer
# varies -- and today not one of the thirty-eight reaches it.
NEEDS_ENGINE := $(shell grep -lE 'main_window\.h|qtwebengine|QWebEngine|web_view_factory' \
                  $(OFFLINE_SRCS) 2>/dev/null)
ENGINE_BINS  := $(LIVE) $(patsubst %.cpp,$(BUILD_DIR)/%,$(NEEDS_ENGINE))
$(ENGINE_BINS): LDLIBS += $(ENGINE_LIBS)

# **Where a live driver's browsing profile goes.** `live/live_paths.cpp` puts
# `QStandardPaths` into test mode, which moves application data, cache and
# settings under `~/.qttest`; the file says what it cost not to. It is linked
# into every live driver and into nothing else -- the app under `src/` must
# keep the real paths, and an offline suite has no profile to move.
#
# **Linked rather than called**, because the failure it prevents is silent: a
# driver that forgot the call would write into the user's own browser profile
# and report nothing. Adding it here covers the driver somebody writes next
# month, which a rule in a comment does not.
LIVE_PATHS_OBJ := $(BUILD_DIR)/live_paths.o
$(LIVE): EXTRA_OBJS = $(LIVE_PATHS_OBJ)
$(LIVE): $(LIVE_PATHS_OBJ)

.PHONY: all offline live live-objs clean help objsets
all: offline live
offline: $(OFFLINE)
live: $(LIVE)

# **Compile every live driver without linking one.** A driver's object needs
# only headers, so this costs thirty-five translation units; linking them costs
# thirty-five Qt WebEngine links and a display to run them on, which is why CI
# does not build them and why a change that breaks a driver's *compilation* has
# been reaching the tree unnoticed. It did repeatedly in one session: the
# drivers were fixed by hand each time, after the fact.
#
# This is the half that is cheap and catches most of it.
live-objs: $(patsubst live/%.cpp,$(BUILD_DIR)/%.o,$(LIVE_SRCS)) \
            $(LIVE_PATHS_OBJ)

# --- rules -----------------------------------------------------------------
$(BUILD_DIR)/moc/moc_%.cpp: $(SRC)/%.h | $(BUILD_DIR)/moc
	@$(MOC_GUARD)
	@$(MOC) $< -o $@

$(BUILD_DIR)/%.moc: $(SRC)/%.cpp | $(BUILD_DIR)
	@$(MOC_GUARD)
	@$(MOC) $< -o $@

$(BUILD_DIR)/%.moc: %.cpp | $(BUILD_DIR)
	@$(MOC_GUARD)
	@$(MOC) $< -o $@

$(BUILD_DIR)/tmoc/moc_%.cpp: %.h | $(BUILD_DIR)/tmoc
	@$(MOC_GUARD)
	@$(MOC) $< -o $@

$(BUILD_DIR)/tmoc/%.o: $(BUILD_DIR)/tmoc/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/moc/%.o: $(BUILD_DIR)/moc/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/app/%.o: $(SRC)/%.cpp $(MOC_CPP_SRCS) | $(BUILD_DIR)/app
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp $(TMOC_INC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: live/%.cpp $(TMOC_INC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# **Secondary expansion**, because the object set depends on which program is
# being linked and `$*` is not available in a prerequisite list on the first
# pass. `$$(OBJS_$$*)` is evaluated on the second, by which point it is.
.SECONDEXPANSION:
$(BUILD_DIR)/%: $(BUILD_DIR)/%.o $$(OBJS_$$*)
	$(CXX) $(LDFLAGS) $< $(OBJS_$*) $(EXTRA_OBJS) $(LDLIBS) -o $@

$(BUILD_DIR) $(BUILD_DIR)/app $(BUILD_DIR)/moc $(BUILD_DIR)/tmoc:
	@mkdir -p $@

# **Named, never bare.** A bare `.SECONDARY:` applies to every target matched by
# any pattern rule, and Make does not rebuild a *missing* secondary file when
# the thing depending on it already exists -- which is how adding a source once
# left an archive unchanged and every symbol in it undefined at link time.
.SECONDARY: $(MOC_SRCS) $(MOC_CPP_SRCS) $(APP_OBJS) $(MOC_OBJS) \
             $(LIVE_PATHS_OBJ) \
             $(TMOC_INC) $(TMOC_SRCS) $(TMOC_OBJS) \
             $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(OFFLINE_SRCS)) \
             $(patsubst live/%.cpp,$(BUILD_DIR)/%.o,$(LIVE_SRCS))

# Read back, test objects included. A test object whose `.d` is never read does
# not rebuild when a header it includes changes, so a struct that gains a field
# ends up with one layout in the archive and another in the binary linked
# against it -- a pile of nonsense assertion failures, not a build error.
-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

# Named targets only: no wildcard sweep, no `rm -rf` of a directory. A clean
# target is the one thing everybody runs without reading.
clean:
	@rm -f $(APP_OBJS) $(MOC_OBJS) $(MOC_SRCS) $(MOC_CPP_SRCS) \
	       $(TMOC_INC) $(TMOC_SRCS) $(TMOC_OBJS) \
	       $(OFFLINE) $(LIVE) $(LIVE_PATHS_OBJ) \
	       $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(OFFLINE_SRCS)) \
	       $(patsubst live/%.cpp,$(BUILD_DIR)/%.o,$(LIVE_SRCS))
	@find $(BUILD_DIR) -name '*.d' -delete 2>/dev/null || true
	@echo "removed the test objects and $(words $(OFFLINE) $(LIVE)) binaries"

# Recompute the link sets. Needs fmake; nothing else here does.
objsets:
	@python3 ../tool/objsets.py

help:
	@echo "make -C test [offline|live|all|clean] -j2"
	@echo "  $(words $(OFFLINE_SRCS)) offline suites, $(words $(LIVE_SRCS)) live drivers"
	@echo "  optional packages found: $(OPT_PKGS)"
