# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
# Common settings (sourced by the scripts)
export TA_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export LIB=${LIB:-${PDKPATH:?set PDKPATH to the sky130A directory}/libs.ref/sky130_fd_sc_hd/lib/sky130_fd_sc_hd__tt_025C_1v80.lib}
export YOSYS=${YOSYS:-$TA_ROOT/build_dev/yosys}
DONT_USE_PATTERNS="sky130_fd_sc_hd__clkbuf* sky130_fd_sc_hd__clkinv* sky130_fd_sc_hd__clkinvlp* sky130_fd_sc_hd__clkdlybuf* sky130_fd_sc_hd__dlygate* sky130_fd_sc_hd__dlymetal* sky130_fd_sc_hd__tap* sky130_fd_sc_hd__decap* sky130_fd_sc_hd__fill* sky130_fd_sc_hd__diode* sky130_fd_sc_hd__lpflow_* sky130_fd_sc_hd__macro_sparecell sky130_fd_sc_hd__probe* sky130_fd_sc_hd__s*"
DONT_USE=""
for p in $DONT_USE_PATTERNS; do DONT_USE="$DONT_USE -dont_use $p"; done
export DONT_USE
