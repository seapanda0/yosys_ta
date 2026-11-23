#!/bin/bash
# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
# usage: study_run.sh <design> <variant: default|ks> <abc script file>
# Maps the snapshot with the given ABC script, then reports timing before and after 'tsize'.
# Output: one TSV line: design variant script period_abc area_abc period_sized area_sized seconds
. "$(dirname "$0")/common.sh"
D=$1; V=$2; S=$3; SN=$(basename $S .abc)${SUFFIX}
# optional env: LIBARGS (passed to ABC read_lib via abc -liberty_args), SUFFIX (appended to the script name)
LA=""; [ -n "$LIBARGS" ] && LA="-liberty_args \"$LIBARGS\""
OUT=$TA_ROOT/study/out/$D.$V.$SN; mkdir -p $OUT
printf 'set_driving_cell sky130_fd_sc_hd__buf_2\nset_load 7.0\n' > $OUT/abc.constr
T0=$(date +%s.%N)
timeout ${TIMEOUT:-1800} $YOSYS -q -l $OUT/yosys.log -p "read_rtlil $TA_ROOT/study/pre/$D.$V.il; abc -liberty $LIB $LA -constr $OUT/abc.constr $DONT_USE -script $S; opt_clean -purge; tee -o $OUT/pre.rpt tsta -quiet -liberty $LIB $DONT_USE -period 0; tsize -liberty $LIB $DONT_USE -period 0; tee -o $OUT/post.rpt tsta -quiet -liberty $LIB $DONT_USE -period 0" > $OUT/yosys.out 2>&1
RC=$?
T1=$(date +%s.%N)
get() { grep "min period" $1 2>/dev/null | tail -1 | sed -E 's/.*min period ([0-9.]+) ns.*area ([0-9.]+),.*/\1\t\2/'; }
PRE=$(get $OUT/pre.rpt); POST=$(get $OUT/post.rpt)
[ $RC -ne 0 ] || [ -z "$POST" ] && { PRE="FAIL	FAIL"; POST="FAIL	FAIL"; }
printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$D" "$V" "$SN" "$PRE" "$POST" "$(python3 -c "print(round($T1-$T0,1))")"
