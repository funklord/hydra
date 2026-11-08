#!/bin/sh
# Drives an EMULATOR through the screen going off and on and the display being
# resized, again and again, and fails if Qt's main thread stays in its wait for
# a surface. The finding is beerssh's (project.md, "Qt's main thread can wedge
# in eglSurface()"); this is its check, aimed at hydra.
#
#   ANDROID_SERIAL=emulator-5590 tool/check-surface-wait.sh [cycles]
#
# First: hydra on screen showing a page whose title changes every 200 ms, so
# hydra's own widgets repaint: painting is what reaches the wait, and an idle
# window proves nothing. Refuses a real phone: it changes the display size and
# the screen state, and a shared phone is somebody's.
#
# Stops by itself after the cycles, or at the first block, where it leaves the
# state as it is, since the state is the evidence.

A=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
P=se.vibes.hydra
cycles=${1:-40}

case "$ANDROID_SERIAL" in
emulator-*) ;;
*) echo "check-surface-wait: ANDROID_SERIAL must name an emulator" >&2; exit 2;;
esac

pid=$($A shell pidof $P | tr -d '\r')
[ -n "$pid" ] || { echo "check-surface-wait: hydra is not running" >&2; exit 2; }

# The switch count and wait channel of the busiest thread Qt names for its loop.
main_thread() {
	$A shell "run-as $P sh -c 'for t in /proc/$pid/task/*; do [ \"\$(cat \$t/comm)\" = qtMainLoopThrea ] && echo \"\$(grep -m1 voluntary_ctxt_switches \$t/status | tr -dc 0-9) \$(cat \$t/wchan)\"; done'" |
		tr -d '\r' | sort -n | tail -1
}

# In a wait twice over, and still there ten seconds on.
check() {
	first=$(main_thread); sleep 1; second=$(main_thread)
	echo "  $1: $first | $second"
	case "$first$second" in
	*futex*futex*)
		sleep 10; third=$(main_thread)
		case "$third" in
		*futex*)
			# Futex both times is also a loop idling between events. Still
			# counting means it is not stuck.
			# Idle and stuck look alike from the count when nothing arrives, so
			# the stack decides, where the image allows root to read it.
			if [ "${first%% *}" = "${third%% *}" ]; then
				stack=$($A shell debuggerd -b "$pid" 2>/dev/null | tr -d '\r' |
				        awk '/"qtMainLoopThrea"/{on=1} on&&/^ *#/{print} on&&/^$/{if(n++)exit}')
				printf '%s\n' "$stack" | head -14
				case "$stack" in
				*eglSurface*|*QAndroidPlatformOpenGLWindow*|*QEGLPlatformContext*|*makeCurrent*)
					echo "check-surface-wait: BLOCKED in eglSurface: $third" >&2; exit 1;;
				"")
					echo "check-surface-wait: BLOCKED, stack unreadable: $third" >&2; exit 1;;
				esac
				echo "  idle, waiting for events, not for a surface"
			fi
			echo "  idle, not stuck: $third";;
		*) echo "  freed: $third";;
		esac;;
	esac
}

i=0
while [ $i -lt "$cycles" ]; do
	i=$((i + 1))
	case $((i % 5)) in 0) gap=0;; 1) gap=0.1;; 2) gap=0.3;; 3) gap=0.8;; *) gap=2;; esac
	mode=$((i % 3))
	echo "cycle $i: gap $gap, mode $mode"
	[ $mode = 1 ] && $A shell input keyevent HOME
	[ $mode = 2 ] && $A shell input keyevent SLEEP
	$A shell "wm size 840x2000; wm density 340"
	sleep $gap
	[ $mode = 0 ] && $A shell input keyevent SLEEP
	sleep $gap
	check stopped
	$A shell input keyevent WAKEUP
	$A shell "wm size reset; wm density reset; wm dismiss-keyguard"
	sleep $gap
	check woken
	$A shell monkey -p $P -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
	sleep 1
	check front
done

[ "$($A shell pidof $P | tr -d '\r')" = "$pid" ] || {
	echo "check-surface-wait: the process changed" >&2
	exit 1
}
echo "check-surface-wait: passed, $cycles cycles"
