#!/bin/bash
# Random-simulation equivalence check: RTL reference vs. synthesized sky130 netlist.
#
# usage :  verify/check.sh <design> <netlist.v> [cycles=3000] [seed=1]
#   <design> must be a name in bench/designs.tsv.
#   <netlist.v> is a flat gate-level netlist whose top module is the design's top.
#
# Prints MISMATCH lines (if any) and finally exactly one line:
#   VERIFY design=<d> cycles=<n> mismatches=<m> first_mismatch_cycle=<c|-1> status=<PASS|FAIL> toggled_outputs=<k>/<total>
# or, on tool errors, VERIFY design=<d> status=ERROR <reason>  (exit 2).
# Exit status is 0 for PASS and FAIL, 2 for ERROR.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VDIR=$ROOT/verify
YOSYS=${YOSYS:-$ROOT/snap/yosys}
LIB=${PDKPATH:?}/libs.ref/sky130_fd_sc_hd/lib/sky130_fd_sc_hd__tt_025C_1v80.lib
MANIFEST=$ROOT/bench/designs.tsv

design=${1:-}
net=${2:-}
cycles=${3:-3000}
seed=${4:-1}

fail_err() {
    # Diagnostics go to stdout before this; the VERIFY line must be last.
    echo "VERIFY design=${design:-?} status=ERROR $*"
    exit 2
}

[ -n "$design" ] && [ -n "$net" ] || { echo "usage: check.sh <design> <netlist.v> [cycles] [seed]"; exit 2; }
[ -f "$net" ] || fail_err "netlist not found: $net"
net=$(realpath "$net")

line=$(awk -F'\t' -v d="$design" '$1==d {print; exit}' "$MANIFEST")
[ -n "$line" ] || fail_err "design not in $MANIFEST"
IFS=$'\t' read -r _name top clock files flags <<<"$line"
[ "$flags" = "-" ] && flags=""
BENCH=$ROOT/bench/$design
[ -d "$BENCH" ] || fail_err "bench dir missing: $BENCH"
[ -x "$YOSYS" ] || fail_err "yosys not executable: $YOSYS"

WORK=$VDIR/work/${design}.$(basename "$(dirname "$net")")
mkdir -p "$WORK" || fail_err "cannot create $WORK"
rm -f "$WORK"/ref.v "$WORK"/gate.v "$WORK"/tb.v "$WORK"/sim "$WORK"/*.log

# ---- 1. RTL reference ------------------------------------------------------
# Run from the bench dir so relative includes (-I.) and file names resolve.
# zinit -all gives every flop (incl. memory words) a zero init value; the
# setundef lines only cover existing x-init attributes.
(
    cd "$BENCH" && "$YOSYS" -l "$WORK/ref_yosys.log" -q -p "
        read_verilog $flags $files;
        hierarchy -check -top $top;
        proc; flatten; memory_map; opt_clean;
        setundef -zero; setundef -init -zero;
        zinit -all; opt_clean;
        rename $top ref_top;
        write_verilog -noattr $WORK/ref.v"
) > "$WORK/ref_yosys.out" 2>&1 || { tail -20 "$WORK/ref_yosys.out"; fail_err "yosys failed on RTL (see $WORK/ref_yosys.log)"; }

# ---- 2. Gate-level netlist (functional sky130 cell models) -----------------
# -ignore_miss_func: some cells (e.g. dlclkp) lack a function on an output.
"$YOSYS" -l "$WORK/gate_yosys.log" -q -p "
    read_liberty -ignore_miss_func -ignore_miss_dir $LIB;
    read_verilog $net;
    hierarchy -check -top $top;
    flatten; proc; opt_clean;
    setundef -zero; setundef -init -zero;
    zinit -all; opt_clean;
    rename $top gate_top;
    write_verilog -noattr $WORK/gate.v" > "$WORK/gate_yosys.out" 2>&1 \
    || { tail -20 "$WORK/gate_yosys.out"; fail_err "yosys failed on netlist (see $WORK/gate_yosys.log)"; }

# ---- 3. Testbench ----------------------------------------------------------
python3 "$VDIR/gen_tb.py" "$WORK/ref.v" "$WORK/gate.v" "$design" "$clock" "$cycles" "$seed" "$WORK/tb.v" \
    > "$WORK/gen_tb.out" 2>&1 || { tail -5 "$WORK/gen_tb.out"; fail_err "testbench generation failed: $(tail -1 "$WORK/gen_tb.out")"; }

# ---- 4. Compile and run ----------------------------------------------------
iverilog -g2012 -o "$WORK/sim" "$WORK/ref.v" "$WORK/gate.v" "$WORK/tb.v" > "$WORK/iverilog.log" 2>&1 \
    || { head -20 "$WORK/iverilog.log"; fail_err "iverilog failed (see $WORK/iverilog.log)"; }
(cd "$WORK" && timeout 3600 vvp -n sim) > "$WORK/sim.log" 2>&1
vrc=$?
[ $vrc -eq 0 ] || { tail -5 "$WORK/sim.log"; fail_err "vvp exited with $vrc"; }

grep '^MISMATCH' "$WORK/sim.log" | head -n 20
verify=$(grep '^VERIFY ' "$WORK/sim.log" | tail -n 1)
[ -n "$verify" ] || { tail -5 "$WORK/sim.log"; fail_err "no VERIFY line from simulation"; }
echo "$verify"
exit 0
