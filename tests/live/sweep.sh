#!/bin/bash
# Run every live driver and say what happened. Needs a display.
#
#   tests/live/sweep.sh                       # all of them, offscreen
#   tests/live/sweep.sh try_import            # just these
#   SWEEP_ONSCREEN=1 tests/live/sweep.sh      # on the real display instead
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
# `tests/build` -- CMake's directory, and CMake is gone. The directory it named
# survived on disk with two binaries in it from before the migration, so the
# sweep did not fail: it found drivers, ran them, and reported a clean sweep of
# two out of thirty-five, every one built before a session's worth of changes.
#
# Overridable, because a second build tree is a real thing to want.
BIN=${HYDRA_SWEEP_BIN:-tests/build-make}
OUT=${HYDRA_SWEEP_OUT:-/tmp/hydra-sweep}
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
	want=$(ls tests/live/try_*.cpp 2>/dev/null | grep -c .)
	if [ "$want" -gt 0 ] && [ "$have" -lt $((want / 2)) ]; then
		echo "only $have driver(s) built in $BIN, against $want source(s)."
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
	if [ -n "${SWEEP_ONSCREEN:-}" ]; then
		HYDRA_TEST_OUT="$OUT/$d.out" timeout "${SWEEP_TIMEOUT:-300}" "$BIN/$d" \
			>"$log" 2>&1
	else
		QT_QPA_PLATFORM=offscreen HYDRA_TEST_OUT="$OUT/$d.out" \
			timeout "${SWEEP_TIMEOUT:-300}" "$BIN/$d" >"$log" 2>&1
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
