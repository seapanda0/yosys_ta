# Timing-aware synthesis: benchmark and validation scripts

These are the scripts that produced the results in `docs/timing_aware_synthesis.md`. They are
written for the IIC-OSIC-TOOLS container (sky130A PDK, OpenSTA, OpenROAD, Icarus Verilog) and
assume the following layout inside the container (`TA_ROOT=/foss/designs/yosys_ta`; the path is
hard-coded at the top of the scripts, adjust it there if needed):

```
$TA_ROOT/build_dev/yosys     Yosys built from this branch (see sync_build.sh)
$TA_ROOT/bench/<design>/     Verilog sources of the benchmark designs
$TA_ROOT/bench/designs.tsv   manifest: name, top module, clock port, source files, read_verilog flags
$TA_ROOT/flow/               the files from flow/
$TA_ROOT/pnr/                the files from pnr/
$TA_ROOT/verify/             the files from verify/
$TA_ROOT/runs/<design>/<tag> output of one flow run (netlist, logs, reports)
```

The benchmark sources are not part of this repository; `designs.tsv` lists where each design
comes from.

## Building

`sync_build.sh` (run on the host) mirrors the source tree into the container mount and builds it
there with CMake/Ninja (no Tcl, readline or slang needed).

## Synthesis flows (`flow/`)

| File | Purpose |
|---|---|
| `run.sh <design> <flow> <period_ns> [tag]` | Runs `flow/<flow>.ys.in` on a design, then OpenSTA (`sta_eval.tcl`). Prints one `RESULT` line with area, WNS, minimum period from OpenSTA and from `tsta`. |
| `run_all.sh <flow> <period_ns> [tag] [designs]` | The same for all designs, in parallel. |
| `base.ys.in` | Stock flow: `synth -flatten`, `dfflibmap`, `abc -liberty -D <period> -constr`. |
| `orfs.ys.in`, `orfs.abc` | Stock front-end with the ABC speed script of OpenROAD-flow-scripts (LibreLane strategy "DELAY 4"). |
| `ta.ys.in` | The timing-aware flow: `synth_timing -period <period>` (period 0: fastest). |
| `sta_eval.tcl` | OpenSTA evaluation: ideal clock, ideal inputs, input/output delay 0, 33.4 fF on outputs. |
| `prep.sh`, `study_run.sh`, `study_all.sh`, `study_rank.py` | The ABC recipe study: map a pre-synthesized snapshot with an ABC script, size it with `tsize`, rank the scripts by the geometric mean of the achieved period. |

Example: `flow/run.sh picorv32 ta 0` (fastest implementation), `flow/run.sh picorv32 ta 4`
(smallest implementation for a 4 ns clock), `flow/run.sh picorv32 base 0.001 base-fast` (stock
flow asked for minimum delay).

## Post-placement evaluation (`pnr/`)

`pnr_run.sh <design> <tag> <period_ns> [util]` runs `pnr_eval.tcl` in OpenROAD on the netlist of a
flow run: floorplan, global and detailed placement, placement based parasitics, `repair_design`,
`repair_timing`, and reports the minimum period before and after repair (`PNR_RESULT` lines).
Clock tree synthesis and routing are not included.

## Functional verification (`verify/`)

`check.sh <design> <netlist.v> [cycles] [seed]` compares a netlist with its RTL by random
simulation in Icarus Verilog: both are converted by Yosys into X-free Verilog with zero-initialized
flip-flops (the netlist with the functional cell models from the liberty file), driven with the
same stimulus, and all outputs are compared every cycle. `check_all.sh <tag>` does this for all
designs of a flow run.
