#!/bin/sh
# Run the live sweep on a private X display, with a window manager.
#
# **The recipe was in sweep.sh's header and had to be retyped.** Three lines
# that everybody who wants an on-screen run types again, and the one that is
# easiest to leave out -- the window manager -- is the one whose absence lies
# loudly: a bare X server has nothing to assign input focus, so every dialog
# opens with `focusWidget()` null and `try_phone` reports "there is no keyboard
# way in" against each one in turn. That came back seven times in one run for
# seven unrelated dialogs, which is the shape of an environment fault rather
# than seven bugs.
#
# **Why bother running on screen at all.** Offscreen hid three real defects
# that the first on-screen sweep found at once, and one driver -- try_handoff
# -- is skipped offscreen and passes here. Measured again 2026-09-07: 27
# passed, 8 report-only, 0 failed on screen against 26 passed offscreen, the
# single difference being that driver.
#
# **Nothing here touches the desktop you are sitting at.** The display is :77
# unless HYDRA_DISPLAY says otherwise, so this can run while somebody works.
#
# Everything it starts is killed by PID, and the PIDs are written down before
# anything else runs: a trap catches a signal, and a task-kill is not a signal
# the trap gets to catch, so the file is what makes cleanup possible from
# another shell afterwards.
#
#   test/live/onscreen.sh            # the whole sweep
#   test/live/onscreen.sh try_phone  # or named drivers, as sweep.sh takes them

set -u

DISP="${HYDRA_DISPLAY:-:77}"
GEOM="${HYDRA_SCREEN:-1280x900x24}"
PIDS="${TMPDIR:-/tmp}/hydra-onscreen.pids"

for need in Xvfb; do
	command -v "$need" >/dev/null 2>&1 || {
		echo "onscreen: no $need on PATH -- install xvfb, or run the sweep" >&2
		echo "onscreen:   offscreen with: make sweep" >&2
		exit 2
	}
done

# Any of these will do; the point is that something assigns focus. Named in
# preference order rather than hard-coded, because which one a machine has is
# not this script's business.
WM=""
for w in xfwm4 openbox marco mutter kwin_x11; do
	if command -v "$w" >/dev/null 2>&1; then WM="$w"; break; fi
done
if [ -z "$WM" ]; then
	echo "onscreen: no window manager found (xfwm4, openbox, marco, ...)." >&2
	echo "onscreen:   Without one every dialog opens with no focus and the" >&2
	echo "onscreen:   focus checks fail for the environment rather than for" >&2
	echo "onscreen:   the code. Refusing rather than reporting that." >&2
	exit 2
fi

: > "$PIDS"

Xvfb "$DISP" -screen 0 "$GEOM" -nolisten tcp >/dev/null 2>&1 &
XPID=$!
echo "$XPID" >> "$PIDS"

cleanup() {
	# Killed by pid rather than by pattern: `pkill -f Xvfb` matches this
	# script's own command line, which is how a cleanup ends up killing the
	# thing it was cleaning up for.
	kill "$WMPID" "$XPID" 2>/dev/null
	sleep 1
	kill -9 "$WMPID" "$XPID" 2>/dev/null
	rm -f "$PIDS"
	return 0
}
WMPID=""
trap 'cleanup; exit 130' INT TERM
trap cleanup EXIT

# The server needs a moment before anything can connect to it.
sleep 2

DISPLAY="$DISP" "$WM" --replace >/dev/null 2>&1 &
WMPID=$!
echo "$WMPID" >> "$PIDS"

# **And the window manager needs one too.** The seventh of those seven dialogs
# was the filter dialog opening while the WM was still taking over, and it has
# passed every run since this wait was added.
sleep 4

echo "onscreen: $DISP at $GEOM, window manager $WM"
DISPLAY="$DISP" SWEEP_ONSCREEN=1 "$(dirname "$0")/sweep.sh" "$@"
rc=$?
echo "onscreen: sweep exited $rc"
exit $rc
