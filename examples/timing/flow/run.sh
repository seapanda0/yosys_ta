#!/bin/bash
# usage: run.sh <design> <flow> <period_ns> [tag]     
# Runs flow/<flow>.ys.in on the design, then OpenSTA. Prints one RESULT line.
set -e
. "$(dirname "$0")/../flow/common.sh"
D=$1; FLOW=$2; PERIOD=$3; TAG=${4:-$FLOW-$PERIOD}
LINE=$(awk -F'\t' -v d="$D" '$1==d' $TA_ROOT/bench/designs.tsv)
[ -n "$LINE" ] || { echo "unknown design $D"; exit 1; }
TOP=$(echo "$LINE" | cut -f2); CLK=$(echo "$LINE" | cut -f3); FILES=$(echo "$LINE" | cut -f4); VFLAGS=$(echo "$LINE" | cut -f5)
[ "$VFLAGS" = "-" ] && VFLAGS=""
OUT=$TA_ROOT/runs/$D/$TAG; mkdir -p $OUT
PERIOD_PS=$(python3 -c "print(int(round($PERIOD*1000)))")
EVAL_PERIOD=$PERIOD; [ "$PERIOD" = "0" ] && EVAL_PERIOD=10
printf 'set_driving_cell sky130_fd_sc_hd__inv_2\nset_load 33.442\n' > $OUT/abc.constr
sed -e "s#@TOP@#$TOP#g" -e "s#@CLK@#$CLK#g" -e "s#@FILES@#$FILES#g" -e "s#@VFLAGS@#$VFLAGS#g" \
    -e "s#@LIB@#$LIB#g" -e "s#@OUT@#$OUT#g" -e "s#@PERIOD@#$PERIOD#g" -e "s#@EVAL_PERIOD@#$EVAL_PERIOD#g" -e "s#@PERIOD_PS@#$PERIOD_PS#g" \
    -e "s#@DONT_USE@#$DONT_USE#g" -e "s#@TA_OPTS@#$TA_OPTS#g" -e "s#@SYNTH_OPTS@#$SYNTH_OPTS#g" -e "s#@ABC_OPTS@#$ABC_OPTS#g" -e "s#@FLOWDIR@#$TA_ROOT/flow#g" $TA_ROOT/flow/$FLOW.ys.in > $OUT/run.ys
cd $TA_ROOT/bench/$D
T0=$(date +%s.%N)
if ! timeout ${TIMEOUT:-3600} $YOSYS -q -l $OUT/yosys.log $OUT/run.ys > $OUT/yosys.out 2>&1; then
  echo "RESULT design=$D flow=$TAG status=YOSYS_FAIL"; tail -3 $OUT/yosys.out; exit 0
fi
T1=$(date +%s.%N)
# OpenSTA/OpenROAD do not accept "signed" in port and wire declarations
sed -i -E "s/^(\s*(input|output|inout|wire)) signed /\1 /" $OUT/net.v
AREA=$(grep "Chip area for" $OUT/yosys.log | tail -1 | awk '{print $NF}')
CELLS=$(grep -E "^ +[0-9]+ .* cells$" $OUT/stat.rpt | tail -1 | awk '{print $1}')
STA=$(LIB=$LIB NET=$OUT/net.v TOP=$TOP CLK=$CLK PERIOD=$EVAL_PERIOD RPT=$OUT/sta.rpt sta -no_splash -exit $TA_ROOT/flow/sta_eval.tcl 2>&1 | grep STA_RESULT || echo "STA_RESULT status=STA_FAIL")
TSTA=$(grep "min period" $OUT/tsta.rpt 2>/dev/null | tail -1 | sed -E "s/.*min period ([0-9.]+) ns.*/\1/")
echo "RESULT design=$D flow=$TAG area=$AREA cells=$CELLS ${STA#STA_RESULT } tsta_min_period=$TSTA synth_s=$(python3 -c "print(round($T1-$T0,1))")"
