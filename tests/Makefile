# SPDX-License-Identifier: GPL-3.0-or-later
#
# The test tree, for plain Make. Written to be compared against the
# CMakeLists.txt beside it; see project.md for what the comparison found.
#
#   make -C tests -j2          # everything
#   make -C tests offline -j2  # just the suites that need nothing but a build
#
# Always name a job count. `-j` with no number is *unlimited* under Make, and
# each live driver links Qt WebEngine -- that has taken this machine's desktop
# session down twice.
#
# **One archive, not one source list per suite.** The CMakeLists names the
# sources each suite needs, thirty-eight times, and compiles them again for
# every target that mentions them. This builds the app's sources once into
# `libhydra_app.a` and lets the linker decide: a static archive contributes only
# the members that resolve something, so a suite still links only what it
# reaches. The bookkeeping disappears and so do the duplicate compiles.

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
MOC       := $(shell pkg-config --variable=libexecdir Qt6Core)/moc
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

ARCHIVE = $(BUILD_DIR)/libhydra_app.a

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

.PHONY: all offline live clean help
all: offline live
offline: $(OFFLINE)
live: $(LIVE)

# --- rules -----------------------------------------------------------------
$(BUILD_DIR)/moc/moc_%.cpp: $(SRC)/%.h | $(BUILD_DIR)/moc
	@$(MOC) $< -o $@

$(BUILD_DIR)/%.moc: $(SRC)/%.cpp | $(BUILD_DIR)
	@$(MOC) $< -o $@

$(BUILD_DIR)/%.moc: %.cpp | $(BUILD_DIR)
	@$(MOC) $< -o $@

$(BUILD_DIR)/tmoc/moc_%.cpp: %.h | $(BUILD_DIR)/tmoc
	@$(MOC) $< -o $@

$(BUILD_DIR)/tmoc/%.o: $(BUILD_DIR)/tmoc/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/moc/%.o: $(BUILD_DIR)/moc/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/app/%.o: $(SRC)/%.cpp $(MOC_CPP_SRCS) | $(BUILD_DIR)/app
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(ARCHIVE): $(APP_OBJS) $(MOC_OBJS)
	@$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: %.cpp $(TMOC_INC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: live/%.cpp $(TMOC_INC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# A test's own moc, for the fakes that carry Q_OBJECT in their headers.
$(BUILD_DIR)/%: $(BUILD_DIR)/%.o $(TMOC_OBJS) $(ARCHIVE)
	$(CXX) $(LDFLAGS) $< $(TMOC_OBJS) $(ARCHIVE) $(LDLIBS) -o $@

$(BUILD_DIR) $(BUILD_DIR)/app $(BUILD_DIR)/moc $(BUILD_DIR)/tmoc:
	@mkdir -p $@

# **Named, never bare.** A bare `.SECONDARY:` applies to every target matched by
# any pattern rule, and Make does not rebuild a *missing* secondary file when
# the thing depending on it already exists -- which is how adding a source once
# left an archive unchanged and every symbol in it undefined at link time.
.SECONDARY: $(MOC_SRCS) $(MOC_CPP_SRCS) $(APP_OBJS) $(MOC_OBJS) \
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
	@rm -f $(APP_OBJS) $(MOC_OBJS) $(MOC_SRCS) $(MOC_CPP_SRCS) $(ARCHIVE) \
	       $(TMOC_INC) $(TMOC_SRCS) $(TMOC_OBJS) \
	       $(OFFLINE) $(LIVE) \
	       $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(OFFLINE_SRCS)) \
	       $(patsubst live/%.cpp,$(BUILD_DIR)/%.o,$(LIVE_SRCS))
	@find $(BUILD_DIR) -name '*.d' -delete 2>/dev/null || true
	@echo "removed the test objects, the archive and $(words $(OFFLINE) $(LIVE)) binaries"

help:
	@echo "make -C tests [offline|live|all|clean] -j2"
	@echo "  $(words $(OFFLINE_SRCS)) offline suites, $(words $(LIVE_SRCS)) live drivers"
	@echo "  optional packages found: $(OPT_PKGS)"
