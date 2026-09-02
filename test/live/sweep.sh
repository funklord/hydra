#!/bin/bash
# Run every live driver and say what happened. Needs a display.
#
#   test/live/sweep.sh                       # all of them, offscreen
#   test/live/sweep.sh try_import            # just these
#   SWEEP_ONSCREEN=1 test/live/sweep.sh      # on the real display instead
#
# **Offscreen by default, and that is not only politeness.** These drivers put
# real windows on a real screen, and a sweep of twenty-seven of them takes over
# somebody's desktop for minutes at a time. Every one of them passes under
# `QT_QPA_PLATFORM=offscreen`, and `try_menus` actually scores *better* there --
# 28 of 28 against 25 of 26 -- because on a live desktop it competes with
# whatever else is mapped.
#
# **What offscreen does not reproduce is appearance.** With no platform theme
# the icon-theme search paths differ, so the toolbar renders with Qt built-in
# icons rather than the desktop's, and a screenshot taken here is not a picture
# of what a user sees. Structure, ordering, mnemonics and behaviour are all
# faithful; colours and icons are not. Use `SWEEP_ONSCREEN=1` when appearance is
# the question, and expect to be looking at somebody's screen while you do.
#
# **On-screen needs a window manager, and without one it lies loudly.** A bare
# X server -- `Xvfb :77 -screen 0 1280x900x24`, which is how this can now run
# without commandeering anybody's desktop -- has nothing to assign input focus,
# so every dialog opens with `focusWidget()` null and `try_phone` reports "there
# is no keyboard way in" against each one in turn. That came back seven times in
# one run, for seven unrelated dialogs, which is the shape of an environment
# fault rather than seven bugs: with `xfwm4` running on the same display, six of
# the seven passed immediately. Start a window manager before believing any
# focus result, and give it a moment -- the seventh was the filter dialog
# opening while the WM was still taking over, and it has passed every run since.
#
#   Xvfb :77 -screen 0 1280x900x24 -nolisten tcp &
#   DISPLAY=:77 xfwm4 --replace &
#   DISPLAY=:77 SWEEP_ONSCREEN=1 test/live/sweep.sh
#
# **This is worth doing rather than avoiding.** Offscreen hid three real bugs
# that the first on-screen sweep found at once: two in `consent_dialog`, whose
# sentence-long buttons could not fit a 360-pixel screen, and `try_handoff`,
# which had been skipped as unrunnable ever since it was written and passes
# here. `try_import` runs nine more checks than it does offscreen.
#
# **Two kinds of driver, and conflating them was worth a bug twice.** Most print
# a trailing "N passed, M failed" and are judged on it. A few -- try_flicker,
# try_settings -- are report-only: they capture screenshots and timings for a
# person to read and have no notion of passing. Judging those on a result line
# they never print reported them as failures in every sweep, which is noise that
# trains you to skip two lines of the summary, and the day one of them breaks
# for real it will look exactly the same as it does now.
#
# So a driver with no result line is not a failure by default; it is a failure
# only if it also did not reach the end. "done" or a non-zero exit is the test.
set -u
cd "$(dirname "$0")/../.." || exit 1

# **Silent, because these drivers load real pages.** A capture run against a
# video site plays it -- through whatever speakers the person at the machine is
# sitting in front of, for as long as the driver takes. `--mute-audio` is
# Chromium's own flag and WebEngine honours it.
#
# Appended rather than assigned, so a flag somebody set for their own reasons
# survives; the app does the same thing in theme.cpp for the colour scheme.
export QTWEBENGINE_CHROMIUM_FLAGS="${QTWEBENGINE_CHROMIUM_FLAGS:+$QTWEBENGINE_CHROMIUM_FLAGS }--mute-audio"
# **Where the drivers are, which moved with the build system.** This said
# `test/build` -- CMake's directory, and CMake is gone. The directory it named
# survived on disk with two binaries in it from before the migration, so the
# sweep did not fail: it found drivers, ran them, and reported a clean sweep of
# two out of thirty-five, every one built before a session's worth of changes.
#
# Overridable, because a second build tree is a real thing to want.
BIN=${HYDRA_SWEEP_BIN:-test/build-make}
# **Per-uid, because a fixed path in /tmp belongs to whoever got there first.**
# It was `/tmp/hydra-sweep`, and on a machine where two accounts work in this
# tree the second one cannot write a single log into the first one's directory.
# That is not hypothetical and it is the second time this exact shape has cost
# something here -- the offline suite's fixed-name temporary paths met another
# uid's directories and produced 22 failures that were nothing to do with the
# code.
OUT=${HYDRA_SWEEP_OUT:-/tmp/hydra-sweep-$(id -u)}
mkdir -p "$OUT"

drivers=${*:-}
if [ -z "$drivers" ]; then
	# Globbed off what was built, so a new try_*.cpp joins the sweep by
	# existing. The alternative is a list here that silently stops covering
	# whatever was added last.
	#
	# **Executable regular files, and that is not pedantry.** The first version
	# globbed `"$BIN"/try_*`, which also matches CMake's `try_*_autogen/`
	# *directories* -- and `ls` on a directory lists what is inside it, so the
	# sweep tried to run `timestamp`, `moc_predefs.h` and `mocs_compilation.cpp`
	# as drivers. It reported 165 failures over 13 real results, which is a
	# summary nobody can read and the second time this script's own bookkeeping
	# has been the thing that lied.
	drivers=$(find "$BIN" -maxdepth 1 -type f -executable -name 'try_*' \
	          -printf '%f\n' 2>/dev/null | sort)
fi
[ -z "$drivers" ] && { echo "no drivers built -- run: make drivers"; exit 1; }

# **A floor, not just a zero check.** Finding *no* drivers was already refused;
# finding a handful was not, and that is the shape this script was in --
# reporting a clean sweep over two stale binaries in a directory the build
# system had stopped using. A count well under what the tree holds means the
# sweep is looking in the wrong place or at an old build, and either way its
# summary is about something other than this tree.
#
# Compared against the sources, so it needs no number maintained here.
if [ -z "${*:-}" ]; then
	have=$(printf '%s
' $drivers | grep -c .)
	want=$(ls test/live/try_*.cpp 2>/dev/null | grep -c .)
	# **Exact, not half.** This read `-lt $((want / 2))`, which asks whether
	# somebody forgot to build at all and tolerates losing up to half the
	# drivers in silence -- a sweep reporting success with nineteen of
	# thirty-eight missing looks exactly like a sweep that ran them. Every
	# driver has a source and every source builds one, and skipping is decided
	# below at run time rather than by not building, so the two counts are
	# equal whenever the build is whole.
	#
	# It is not hypothetical: a stale `objsets.mk` once left every live driver
	# with undefined symbols at link time, and nothing in `make test` could see
	# it because that target builds only the offline suites. A threshold guard
	# would have caught that one -- zero is under half -- and would have said
	# nothing about the same fault affecting forty per cent of them.
	if [ "$want" -gt 0 ] && [ "$have" -lt "$want" ]; then
		echo "only $have driver(s) built in $BIN, against $want source(s)."
		echo "missing:"
		for src in test/live/try_*.cpp; do
			name=$(basename "$src" .cpp)
			[ -x "$BIN/$name" ] || echo "  $name"
		done
		echo "That is not a sweep -- build them with: make drivers"
		exit 1
	fi
fi

pass=0 fail=0 report=0 failed=""
# Drivers that are tools rather than tests: they take arguments, or they need a
# network the sweep has no business assuming. Named with the reason, because
# "it failed" and "it was never going to run here" are different facts and this
# script has already conflated two kinds of outcome once.
skip_reason() {
	case "$1" in
		try_extract) echo "a capture tool; takes a url argument" ;;
		try_watch)   echo "needs a live network" ;;
		# **Four more that take a url, and were not named here.** They ran in
		# every sweep with no argument and were counted as report-only
		# successes -- "ran to the end" is true of a driver that navigated
		# nowhere. try_ytdlp printed `navigated to ` and `detector count for :
		# 0`, which reads as the handoff finding nothing rather than as a page
		# never opened; try_tap and try_taprow printed zeros the same way.
		#
		# try_cancel was the expensive one: it read `argv[2]` without checking
		# argc, which is one past the NULL the standard puts at argv[argc], and
		# on Linux the environment block sits there. It got `environ[0]` --
		# which is HYDRA_TEST_OUT, set on the line below -- and mkpath'd it as
		# a relative tree in the repository root, invisible to `git status`
		# because the directories it made were empty. All four refuse without a
		# url now; they are named here so the sweep says why instead.
		try_cancel)  echo "takes a url and an output directory" ;;
		try_tap)     echo "takes a url; needs a page that plays through MSE" ;;
		try_taprow)  echo "takes a url; needs a page with a video" ;;
		try_ytdlp)   echo "takes a watch-page url to hand to yt-dlp" ;;
		# **Cannot pass offscreen, and that is the platform rather than the
		# driver.** It hands a url to another application through
		# QDesktopServices::openUrl, and the offscreen platform plugin has no
		# desktop services to hand it to -- so the request never leaves and the
		# driver correctly reports that nothing fetched it. On the real display
		# it passes five of five. It was reported as a failure in every
		# offscreen sweep, which is the noise this script's own header warns
		# about: two lines of summary that are always wrong train you to skip
		# the summary.
		try_handoff)
			[ -n "${SWEEP_ONSCREEN:-}" ] || \
				echo "hands a url to another application; offscreen has no desktop services"
			;;
		# **The camera skip moved into the driver, where it belongs.** This used
		# to sit here, keyed on `/dev/video*`, because four of the driver's
		# checks asserted on a camera and could not pass without one -- the
		# engine answers NotFoundError before our decider is consulted, so the
		# assertion that we refused the camera cannot tell our refusal from the
		# device's absence.
		#
		# The driver now decides that for itself, from the decider's own log
		# rather than from a device node: it asks whether a camera request
		# reaches our code at all, and prints a `--   skipped:` line when it does
		# not. That is the better place for it, because the question is about
		# what the engine did rather than about what is plugged in, and because
		# skipping the whole driver here cost the checks that do work --
		# geolocation, notifications, the prompt, and that a grant is per-feature
		# rather than global, which is the decider's whole contract.
		#
		# Accepting NotFoundError as success was the other option and is worse
		# than either. It would make the check incapable of failing, which is
		# the shape this script has twice been caught reporting already.
		#
		# **Needs a session bus of its own.** The driver stands up a stub
		# `org.freedesktop.Notifications` and would otherwise either find the
		# name owned by the desktop's real daemon -- in which case it stops
		# rather than taking it away -- or find no bus at all. `dbus-run-session`
		# gives it a private one, and its absence is a skip rather than a
		# failure about somebody's machine.
		try_notify)
			command -v dbus-run-session >/dev/null 2>&1 || \
				echo "needs dbus-run-session for a private session bus"
			;;
		# Waits on a local model to return a proposal. With none running it
		# reaches the timeout and is killed, which costs the sweep five minutes
		# and reports a failure about the machine rather than the code.
		try_evolve_confirm) echo "needs a local model; start ollama and use SWEEP_ALL" ;;
		*)           echo "" ;;
	esac
}

for d in $drivers; do
	why=$(skip_reason "$d")
	if [ -n "$why" ] && [ -z "${SWEEP_ALL:-}" ]; then
		printf '  skip   %-16s %s\n' "$d" "$why"
		continue
	fi
	log="$OUT/$d.log"
	# **Truncated before the run, and the sweep stops for this driver if it
	# cannot be.** Without this the run below fails to redirect, writes nothing,
	# and the `grep` further down reads *whatever is already at that path* --
	# which on the machine that found this was a log from two weeks earlier. The
	# sweep printed "7 passed, 4 failed" for a driver that had just been changed
	# to pass 19 of 19, and printed it in exactly the format a real result comes
	# in. A stale answer presented as a current one is worse than no answer.
	if ! : >"$log" 2>/dev/null; then
		fail=$((fail+1)); failed="$failed $d"
		printf '  FAIL   %-16s %s\n' "$d" \
			"cannot write $log; refusing to read what is already there"
		continue
	fi
	# A private session bus where the driver needs one, and nothing otherwise.
	# Kept out of the environment of every other driver deliberately: a bus that
	# exists only for one process is not the desktop's, and handing it to a
	# driver that talks to the colour-scheme portal would quietly change what
	# that driver measures.
	prefix=""
	case "$d" in
		try_notify) prefix="dbus-run-session --" ;;
	esac
	if [ -n "${SWEEP_ONSCREEN:-}" ]; then
		HYDRA_TEST_OUT="$OUT/$d.out" timeout "${SWEEP_TIMEOUT:-300}" \
			$prefix "$BIN/$d" >"$log" 2>&1
	else
		QT_QPA_PLATFORM=offscreen HYDRA_TEST_OUT="$OUT/$d.out" \
			timeout "${SWEEP_TIMEOUT:-300}" $prefix "$BIN/$d" >"$log" 2>&1
	fi
	rc=$?
	last=$(grep -E 'passed,' "$log" | tail -1)
	if [ -n "$last" ]; then
		if echo "$last" | grep -q ', 0 failed'; then
			pass=$((pass+1)); printf '  ok     %-16s %s\n' "$d" "$last"
		else
			fail=$((fail+1)); failed="$failed $d"
			printf '  FAIL   %-16s %s\n' "$d" "$last"
			grep -E '^  FAIL' "$log" | head -3
		fi
	elif [ $rc -eq 0 ] && grep -q '^done' "$log"; then
		report=$((report+1))
		printf '  report %-16s ran to the end, see %s\n' "$d" "$log"
	else
		fail=$((fail+1)); failed="$failed $d"
		printf '  FAIL   %-16s no result line and did not finish (rc=%d)\n' "$d" "$rc"
		tail -3 "$log" | sed 's/^/         /'
	fi
done
echo "=== drivers: passed=$pass report-only=$report failed=$fail$failed"
[ $fail -eq 0 ]
