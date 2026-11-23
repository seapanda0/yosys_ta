#!/bin/bash
# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
# usage: formal.sh <design> <netlist.v>
# Sequential equivalence of a netlist and the design at the input of tmap (bit-blasted without ABC),
# all flip-flops starting at zero. The miter is built by Yosys, which pairs the ports by name
# (ABC's &miter and dsec pair them by position, and the port order of a netlist need not be the RTL's).
. "$(dirname "$0")/../flow/common.sh"
Y=${FY:-$TA_ROOT/build_dev/yosys}
D=$1; NET=$(realpath "$2")
L=$(awk -F'\t' -v d="$D" '$1==d' $TA_ROOT/bench/designs.tsv)
TOP=$(cut -f2 <<<"$L"); FILES=$(cut -f4 <<<"$L"); FL=$(cut -f5 <<<"$L"); [ "$FL" = "-" ] && FL=""
W=$TA_ROOT/verify/formal/$D.$(basename $(dirname "$NET")); mkdir -p $W
AIG="delete t:\$print t:\$check; setundef -undriven -zero; setundef -zero; dffunmap; async2sync; techmap; dffunmap; setundef -init -zero; opt_clean"
cd $TA_ROOT/bench/$D
$Y -q -l $W/gold.log -p "read_verilog $FL $FILES; synth_timing -top $TOP -liberty $LIB -run begin:map; alumacc; share; opt; memory -nomap; opt_clean; opt -fast -full; memory_map; opt -full; techmap; opt -fast; $AIG; rename $TOP gold; write_rtlil $W/gold.il" > $W/gold.out 2>&1 || { echo "FORMAL design=$D status=GOLD_ERROR $(tail -1 $W/gold.out)"; exit 0; }
$Y -q -l $W/gate.log -p "read_liberty -ignore_miss_func -ignore_miss_dir -ignore_buses $LIB; read_verilog $NET; hierarchy -top $TOP; proc; flatten; $AIG; rename $TOP gate; write_rtlil $W/gate.il" > $W/gate.out 2>&1 || { echo "FORMAL design=$D status=GATE_ERROR $(tail -1 $W/gate.out)"; exit 0; }
cd $W
$Y -q -l miter.log -p "read_rtlil gold.il; read_rtlil gate.il; miter -equiv -flatten gold gate miter; hierarchy -top miter; flatten; techmap; opt_clean; aigmap; opt_clean; write_aiger -symbols miter.aig" > miter.out 2>&1 || { echo "FORMAL design=$D status=MITER_ERROR $(tail -1 miter.out)"; exit 0; }
A=${Y}-abc
T=${FTIME:-300}
S=""
# 1. signal correspondence: proven iff every output of the miter reduces to constant 0
rm -f res.aig
timeout $T $A -c "&r miter.aig; &scorr; &scorr -C 10000; &write res.aig" > scorr.log 2>&1
if [ -f res.aig ] && python3 -I -c '
f = open("res.aig", "rb")
n_latch, n_out = [int(x) for x in f.readline().split()[3:5]]
for _ in range(n_latch):
    f.readline()
raise SystemExit(0 if n_out and all(f.readline().strip() == b"0" for _ in range(n_out)) else 1)'; then S=PROVEN; fi
# 2. the sequential equivalence checker
if [ -z "$S" ]; then
  timeout $T $A -c "read miter.aig; dprove" > dsec.log 2>&1
  if grep -q "Networks are equivalent" dsec.log; then S=PROVEN
  elif grep -q "NOT EQUIVALENT" dsec.log; then S=DIFFERENT; fi
fi
# 3. undecided: look for a counterexample
if [ -z "$S" ]; then
  timeout $((T + 60)) $A -c "read miter.aig; bmc3 -T $T; print_status" > bmc.log 2>&1
  S=UNDECIDED
  grep -qi "was asserted in frame" bmc.log && S=DIFFERENT
  [ $S = UNDECIDED ] && S="UNDECIDED bmc_frames=$(grep -oi "asserted in [0-9]* frames" bmc.log | grep -o "[0-9]*" | tail -1)"
fi
echo "FORMAL design=$D status=$S"
