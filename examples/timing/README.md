# Timing-aware synthesis: benchmark and validation scripts

These scripts produce the results in `docs/timing_aware_synthesis.md`.

## Requirements

- Yosys built from this tree, as `yosys` with `yosys-abc` next to it (set `YOSYS`)
- the sky130A PDK (set `PDKPATH` to its `sky130A` directory)
- OpenSTA (`sta`), OpenROAD, Icarus Verilog
- Python 3 with the `librelane` package, from which the stock ABC strategies are generated
- the benchmark sources, see `bench/README.md` (the ten small blocks are included)

## Running

```
export YOSYS=/path/to/yosys PDKPATH=/path/to/pdks/sky130A
python3 flow/campaign.py stock speed sweep limits pnr verify report
```

The stages are resumable (`results/<stage>.jsonl`). The full campaign takes hours.

## Content

| File | Purpose |
|---|---|
| `flow/campaign.py` | The campaign. Every design is compared with the best of 17 stock configurations. |
| `flow/run.sh <design> <flow> <period> [tag]` | One synthesis run (`stock` or `ta`), timed by OpenSTA. |
| `flow/sta_eval.tcl`, `flow/limits_eval.tcl` | OpenSTA: timing, slew and capacitance violations. |
| `pnr/pnr_run.sh <design> <tag> <period>` | OpenROAD: placement, `repair_design`, `repair_timing`. No clock tree, no routing. |
| `verify/formal.sh <design> <netlist>` | Sequential equivalence (ABC) of a netlist and the design at the input of `tmap`. |
| `verify/check.sh <design> <netlist>` | Random simulation of a netlist against the RTL. A weak check for processors, which random instructions send into a trap. |
| `flow/prep.sh`, `flow/study_*` | The study that the mapping recipes were selected with. |
