#!/bin/bash
# pnr_run.sh <design> <run_tag> <period_ns> [util]
# Run INSIDE the container (bash -lc). Looks up top/clock for <design> in bench/designs.tsv,
# runs pnr_eval.tcl on runs/<design>/<run_tag>/net.v and prints the PNR_RESULT lines.
set -u

if [ $# -lt 3 ]; then
    echo "usage: pnr_run.sh <design> <run_tag> <period_ns> [util]" >&2
    exit 2
fi
DESIGN=$1
TAG=$2
PERIOD=$3
UTIL=${4:-40}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TSV=$ROOT/bench/designs.tsv
TOP_TSV=""
CLK_TSV=""
if [ -f "$TSV" ]; then
    read -r TOP_TSV CLK_TSV < <(awk -F'\t' -v d="$DESIGN" '$1 == d { print $2, $3; exit }' "$TSV")
fi
if [ -z "$TOP_TSV" ] || [ -z "$CLK_TSV" ]; then
    echo "PNR_RESULT design=$DESIGN flow=$TAG status=FAIL reason=design-not-in-$TSV"
    exit 1
fi

NET=$ROOT/runs/$DESIGN/$TAG/net.v
OUT=$ROOT/runs/$DESIGN/$TAG/pnr
if [ ! -f "$NET" ]; then
    echo "PNR_RESULT design=$DESIGN flow=$TAG status=FAIL reason=missing-$NET"
    exit 1
fi
mkdir -p "$OUT"
LOG=$OUT/openroad.log

START=$(date +%s)
NET="$NET" TOP="$TOP_TSV" CLK="$CLK_TSV" PERIOD="$PERIOD" UTIL="$UTIL" OUT="$OUT" \
    timeout "${TIMEOUT:-3600}" openroad -threads "${THREADS:-2}" -no_splash -exit \
    "$SCRIPT_DIR/pnr_eval.tcl" > "$LOG" 2>&1
RC=$?
END=$(date +%s)
echo "$((END - START))" > "$OUT/runtime_s.txt"

RESULTS=$(grep '^PNR_RESULT stage=' "$LOG")
NRES=$(printf '%s\n' "$RESULTS" | grep -c '^PNR_RESULT stage=')
if [ "$RC" -ne 0 ] || [ "$NRES" -ne 2 ]; then
    echo "PNR_RESULT design=$DESIGN flow=$TAG status=FAIL rc=$RC runtime_s=$((END - START))"
    echo "--- last 5 lines of $LOG:"
    tail -5 "$LOG"
    exit 1
fi

printf '%s\n' "$RESULTS" | sed "s/^PNR_RESULT /PNR_RESULT design=$DESIGN flow=$TAG /"
echo "PNR_INFO design=$DESIGN flow=$TAG runtime_s=$((END - START)) threads=${THREADS:-2} log=$LOG"
exit 0
