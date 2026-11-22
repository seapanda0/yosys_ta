#!/bin/bash
# usage: prep.sh <design>    -- writes pre-mapping RTLIL snapshots for the ABC recipe study:
#   study/pre/<design>.default.il  (stock 'synth -flatten' + dfflibmap)
#   study/pre/<design>.ks.il       (same, with Kogge-Stone adders)
set -e
. "$(dirname "$0")/common.sh"
D=$1
LINE=$(awk -F'\t' -v d="$D" '$1==d' $TA_ROOT/bench/designs.tsv)
TOP=$(echo "$LINE" | cut -f2); FILES=$(echo "$LINE" | cut -f4); VFLAGS=$(echo "$LINE" | cut -f5); [ "$VFLAGS" = "-" ] && VFLAGS=""
mkdir -p $TA_ROOT/study/pre; cd $TA_ROOT/bench/$D
$YOSYS -q -p "read_verilog $VFLAGS $FILES; synth -top $TOP -flatten; dfflibmap -liberty $LIB $DONT_USE; write_rtlil $TA_ROOT/study/pre/$D.default.il" > /dev/null 2>&1
$YOSYS -q -p "read_verilog $VFLAGS $FILES; synth -top $TOP -flatten -extra-map +/choices/kogge-stone.v; dfflibmap -liberty $LIB $DONT_USE; write_rtlil $TA_ROOT/study/pre/$D.ks.il" > /dev/null 2>&1
echo "prepared $D"
