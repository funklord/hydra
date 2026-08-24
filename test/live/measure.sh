#!/bin/bash
# Measure the extractor loop against a capture, N runs, recording each reply.
#
#   test/live/measure.sh evidence/kisskh-2026-08-03.json kisskh-new 5
#
# **Built for a machine that is never idle**, which is the normal case here. Two
# things follow from that and both are the point of this script rather than an
# afterthought:
#
# 1. The model runs at `nice 19` and idle IO priority, so it takes whatever is
#    spare and yields the moment anything else wants the CPU. It finishes later
#    and costs the machine nothing, which is the trade that makes it runnable at
#    all. Ollama does the work in its own process, so the server is renice'd
#    too -- nicing only the client would move nothing.
#
# 2. Every reply is written whole into the corpus. That is what makes the *next*
#    measurement free: `make replay` re-scores every reply ever recorded against
#    the current gate in milliseconds, so only a change to the prompt or the
#    model ever needs this script again.
#
# The runs are sequential on purpose. Two 14B generations at once on CPU is not
# twice the throughput, it is two runs that both time out.
set -u
cd "$(dirname "$0")/../.." || exit 1

EV=${1:?usage: measure.sh <evidence.json> <tag> [runs] [model]}
TAG=${2:?usage: measure.sh <evidence.json> <tag> [runs] [model]}
RUNS=${3:-5}
MODEL=${4:-qwen2.5-coder:14b}
REPLIES=${HYDRA_REPLIES:-evidence/replies}
LOGS=${MEASURE_LOGS:-evidence/measure-$TAG}
mkdir -p "$REPLIES" "$LOGS"

BIN=test/build/test_live_model
[ -x "$BIN" ] || { echo "build it first: make test-one T=test_live_model"; exit 1; }

# The server does the generating, so it is the process that matters. Best
# effort: it may not be ours to renice, and that is not a reason to stop.
for pid in $(pgrep -f 'ollama (serve|runner)' 2>/dev/null); do
	renice -n 19 -p "$pid" >/dev/null 2>&1
	ionice -c 3 -p "$pid" >/dev/null 2>&1
done

echo "$RUNS runs of $MODEL against $EV, nice 19, replies into $REPLIES"
for i in $(seq 1 "$RUNS"); do
	name="$TAG-run$i"
	HYDRA_REPLIES="$REPLIES" HYDRA_REPLY_NAME="$name" \
	HYDRA_DUMP_PAYLOAD="$LOGS/$name.payload.txt" \
	HYDRA_MODEL_TIMEOUT_MS=${TIMEOUT_MS:-900000} QT_QPA_PLATFORM=offscreen \
		nice -n 19 ionice -c 3 "$BIN" "$MODEL" "$EV" \
		>"$LOGS/$name.log" 2>&1
	# Three outcomes, not two. Reporting "refused" for a run that never answered
	# is how a loaded machine gets recorded as a model result, and it has already
	# happened once: three runs of five timed out and the summary said nothing
	# about it.
	gate=$(grep -m1 -oE '^gate: [A-Z]+' "$LOGS/$name.log")
	[ -z "$gate" ] && gate="no answer (timed out)"

	# **The probe result belongs beside the verdict, not buried in a log.**
	# A run whose probes all failed to reach the host is asking the model a
	# *different question* -- the same addresses with none of the content-type
	# notes -- and pooling it with annotated runs reports one number for two
	# experiments. Measured: five dramafren runs alternated 3, 0, 3, 0, 3
	# annotations, and the batch would have read as a single score.
	probes=$(grep -m1 -oE '[0-9]+ addresses answered, [0-9]+ refused' \
	          "$LOGS/$name.log")
	printf '  %-16s %-26s %s\n' "$name" "$gate" "${probes:-no payload recorded}"
done
echo
echo
echo "Check the probe column before pooling these: runs with no annotations"
echo "asked a different question and do not belong in the same score."
echo "recorded in $REPLIES. Add them to $REPLIES/corpus.ini with the verdict"
echo "each one got, then 'make replay' scores them for free from now on."
