#!/bin/bash
# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
# usage: study_all.sh <variant> "<designs>" <script.abc>...
# Runs all design x script combinations (7 in parallel), appends to study/results.tsv
. "$(dirname "$0")/common.sh"
V=$1; DESIGNS=$2; shift 2
for d in $DESIGNS; do for s in "$@"; do echo "$d $V $s"; done; done | \
  xargs -P ${JOBS:-7} -L 1 $TA_ROOT/flow/study_run.sh | tee -a $TA_ROOT/study/results.tsv
