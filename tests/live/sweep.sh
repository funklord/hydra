#!/bin/bash
# Run every live driver and say what happened. Needs a display.
#
#   DISPLAY=:0 tests/live/sweep.sh            # all of them
#   DISPLAY=:0 tests/live/sweep.sh try_import # just these
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
BIN=tests/build
OUT=${HYDRA_SWEEP_OUT:-/tmp/hydra-sweep}
mkdir -p "$OUT"

drivers=${*:-}
if [ -z "$drivers" ]; then
	# Globbed off what was built, so a new try_*.cpp joins the sweep by
	# existing. The alternative is a list here that silently stops covering
	# whatever was added last.
	drivers=$(ls "$BIN"/try_* 2>/dev/null | xargs -n1 basename)
fi
[ -z "$drivers" ] && { echo "no drivers built -- run: make drivers"; exit 1; }

pass=0 fail=0 report=0 failed=""
for d in $drivers; do
	log="$OUT/$d.log"
	HYDRA_TEST_OUT="$OUT/$d.out" timeout "${SWEEP_TIMEOUT:-300}" "$BIN/$d" \
		>"$log" 2>&1
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
