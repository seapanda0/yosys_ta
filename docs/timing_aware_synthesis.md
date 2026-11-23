# Timing-aware synthesis for Yosys

This document describes the timing-aware synthesis flow that was added to Yosys on the
`timing-aware-synth` branch: what was built, why, how it is structured, how to use it, and what it
achieves on a set of benchmark designs with the SkyWater 130 nm (sky130_fd_sc_hd) library.

> **Authorship.** The timing-aware synthesis flow, its tests, the benchmark and validation
> scripts, the benchmark campaign and this document were written by **Claude**, an AI model made by
> Anthropic, working in Claude Code at the request of the owner of this repository. It is not a
> product of the Yosys maintainers (YosysHQ) and has not been reviewed by them. Every commit on the
> branch carries a `Co-Authored-By: Claude` trailer.

**Results in short** (20 designs, sky130_fd_sc_hd, section 6). Each design is compared with the
fastest of 17 stock Yosys configurations for that design, among them the ORFS script and all
LibreLane ABC strategies:

| | fmax, geometric mean | range | area |
|---|---|---|---|
| pre-layout | **1.57x** | 1.06x to 2.60x, faster on 20 of 20 | 1.15x |
| after placement and repair (OpenROAD) | **1.33x** | 0.90x to 2.06x, faster on 19 of 20 | 1.11x |
| two held-out designs, pre-layout / placed | 1.48x / 1.16x | | 1.03x / 1.00x |

With a clock period, `synth_timing` returns the smallest netlist it finds that meets it.
Relaxing the period from the fastest achievable to 1.2x saves up to 35% area (7% for a typical
design), and to 3x saves 10% to 58%. Where stock configurations also meet the period, the
timing-aware netlist is smaller in 44 of 50 cases. None of the 140 timing-aware netlists violates
max slew or max capacitance limits; 75 of the 337 stock netlists do. Equivalence: 99 of the 140
netlists are formally proven and none is found different; for the 8 designs with undecided
netlists, the fastest netlist matches a stock netlist in random simulation.

## 1. Why stock Yosys is not timing-aware

The standard ASIC flow in Yosys is `synth` followed by `dfflibmap` and `abc -liberty`. Looking at
what this flow actually does with timing explains most of the results below.

1. **The default ABC mapper sees a unit-delay library.** `abc -liberty` loads the library into
   ABC and runs a script built around the `&nf` mapper. Unless `read_lib` is given a gain
   (`-G`), ABC derives a gate library in which *every gate has delay 1*
   (`Abc_SclProduceGenlibStrSimple` in `abc/src/map/scl/sclLibUtil.c`). The mapper therefore
   minimises the number of logic levels: an inverter, an XOR and a 5-input AOI all cost the same.
2. **The delay target is meaningless in that model.** `abc -D <picoseconds>` passes the target to
   `&nf`, which compares it with a delay measured in gate levels. A target of 5000 means "5000
   levels are fine", so the mapper only recovers area. With `-D` set to a clock period of a few
   thousand picoseconds, the 32-bit adder of picorv32 comes out as a chain of majority gates.
3. **There is no timing analysis in the loop.** Nothing measures the netlist that comes back from
   ABC. The sizing commands that ABC runs afterwards (`upsize`, `dnsize`) know nothing about the
   flip-flops, their clock-to-output delay, their setup time or their drive strength, because
   ABC only sees the combinational logic.
4. **Arithmetic architecture is fixed.** Adders are always mapped to the same (Brent-Kung)
   carry structure and multipliers to the same array structure, regardless of how critical they
   are.

The work described here closes these gaps inside Yosys: it adds a timing engine that reads the
delay tables of the library, a timing driven optimizer for mapped netlists, and a mapping command
that uses real delays and keeps what the timing analysis says is best.

## 2. What was added

All new code lives in `passes/timing/` and `techlibs/common/synth_timing.cc`. ABC itself was not
modified (the `abc` submodule is used as it is).

| Command | File | Purpose |
|---|---|---|
| `timing_setup` | `passes/timing/tsta.cc` | Stores the liberty file and the timing constraints (clock period, input/output delay, output load, ...) in the design scratchpad (`timing.*`). |
| `tsta` | `passes/timing/tsta.cc` | Static timing analysis of a mapped netlist: WNS, TNS, minimum period, critical path report. |
| `tsize` | `passes/timing/tsize.cc` | Timing driven gate sizing and buffer insertion, followed by area recovery. |
| `tmap` | `passes/timing/tmap.cc` | Timing driven technology mapping: creates several implementation candidates, sizes and times each of them, keeps the best. |
| `synth_timing` | `techlibs/common/synth_timing.cc` | The top-level script: RTL to an optimized netlist of library cells. |

Supporting code:

| File | Content |
|---|---|
| `passes/timing/timing_lib.{h,cc}` | `Timing::Library`: the timing view of a liberty library (NLDM tables, pin capacitances, setup checks, drive-strength families, buffers). |
| `passes/timing/timing_sta.{h,cc}` | `Timing::Sta`: the timing graph and the analysis (full and incremental); `Timing::TimingConfig`: option handling shared by all commands. |
| `passes/timing/timing_opt.{h,cc}` | `Timing::timing_optimize()`: the sizing/buffering optimizer used by `tsize` and `tmap`. |
| `techlibs/common/choices/ripple.v` | A ripple-carry `$lcu` implementation, the cheapest adder architecture for non-critical adders. |
| `tests/timing/` | Regression tests with a small NLDM library whose tables are linear, so that expected delays can be computed by hand. |

### Architecture

```
                       synth_timing -top <top> -liberty <lib> [-period <ns>]
                                             |
        +------------------------------------+------------------------------------+
        | begin/coarse: hierarchy, proc, flatten, opt, fsm, wreduce, peepopt       |
        |               (word-level netlist: $add, $mul, $mux, $dff, ...)          |
        +------------------------------------+------------------------------------+
                                             |
                                           tmap
          +----------------------------------+----------------------------------+
          |  for each architecture variant (adder / multiplier implementation): |
          |      alumacc, [booth], techmap (+ lcu choice), opt, abc, dfflibmap  |
          |      for each mapping recipe (ABC script, real-delay mapper):       |
          |          abc -liberty ... -script <recipe with delay target>        |
          |          timing_optimize()   <---- sizing, buffering, area recovery |
          |          candidate = (wns, min period, area)                        |
          |  keep the best candidate; with a period: adjust the delay target    |
          |  of the mapper from the measured slack and try again                |
          +----------------------------------+----------------------------------+
                                             |
                               opt_clean, stat, tsta (report)

   timing_optimize()                       Timing::Sta                    Timing::Library
   +---------------------------+           +------------------------+     +------------------+
   | repair max cap / slew     |  queries  | timing graph of module |     | cells, pins, caps|
   | critical path moves:      | --------> | arrival / slew / req.  | --> | NLDM delay tables|
   |   resize, side-load down- |  update() | incremental update     |     | setup checks     |
   |   size, isolate, buffer   | <-------- | endpoints, wns, tns    |     | size families    |
   | area recovery             |           +------------------------+     +------------------+
   +---------------------------+
```

## 3. Using it

```
read_verilog design.v
synth_timing -top top -liberty sky130_fd_sc_hd__tt_025C_1v80.lib            # fastest implementation
synth_timing -top top -liberty sky130_fd_sc_hd__tt_025C_1v80.lib -period 4  # smallest one that meets 4 ns
write_verilog -noattr netlist.v
```

* Without `-period` (or with `-period 0`) the flow minimises the critical path delay.
* With `-period <ns>` it looks for the *smallest* implementation that meets the period: the mapper
  gets the delay budget of the logic as its target, cheaper architectures and area oriented
  recipes are explored, sizing stops as soon as the period is met and then recovers area. If the
  period cannot be met the fastest implementation is returned.
* Further constraints: `-input_delay`, `-output_delay` (ns), `-load` (fF on outputs), `-input_slew`,
  `-clock_slew` (ns), `-wire_cap` (fF per fanout, a simple wire load estimate). `-dont_use <glob>`
  excludes cells. `-effort 0|1|2` controls how many candidates are explored.
* The commands can also be used on their own, e.g. on a netlist from another flow:

```
read_verilog netlist.v                       # structural netlist of library cells
timing_setup -liberty lib.lib -period 5
tsta                                         # report timing
tsize                                        # size / buffer for the period, recover area
tsta -paths 10
```

`help synth_timing`, `help tmap`, `help tsize`, `help tsta` and `help timing_setup` document all
options. `tmap -list` prints the architecture variants and mapping recipes.

## 4. How it works

### 4.1 Timing library (`Timing::Library`)

The liberty file is parsed with the existing Yosys liberty parser (`passes/techmap/libparse`),
then converted into a compact timing view:

* per pin: direction, rise/fall capacitance, `max_capacitance`, `max_transition`, clock flag;
* per timing arc: sense (positive/negative/non-unate), type (combinational, clock-to-output,
  setup/recovery check) and the NLDM tables `cell_rise/fall`, `rise/fall_transition`,
  `rise/fall_constraint`, normalised to ns and pF. Lookups use bilinear interpolation and linear
  extrapolation outside of the table, like OpenSTA;
* drive-strength *families*, ordered by drive resistance: cells with the same `cell_footprint`,
  pins, functions *and sequential behavior* (`ff`/`latch`/`statetable` groups, clock edges,
  three-state conditions). The footprint alone is not enough, flip-flops of both edges can share
  one. Cells with buses or inout pins are never substituted. Sizing moves a cell within its family;
* the list of usable buffers.

Names are kept as plain strings because identifiers in this Yosys version belong to the name pool
of a design, while the library is cached and shared between designs.

### 4.2 Static timing analysis (`Timing::Sta`, command `tsta`)

A graph based analysis of a flat module of library cells:

* one node per net (after `SigMap`), one instance per cell; instances are sorted topologically
  along their combinational arcs;
* loads are lumped capacitances: sink pin capacitances (separately for rise and fall), the output
  load on primary outputs and an optional wire capacitance per fanout;
* forward pass: arrival time and slew per net and transition; flip-flop outputs start at the
  clock-to-output delay for their load, primary inputs at the input delay. Slews are merged with
  the worst slew, as OpenSTA does;
* endpoints: setup (and recovery) checks at sequential cells, with the setup time looked up for
  the actual data slew, and primary outputs;
* backward pass: required times per net and per instance pin, which gives a slack for every net
  and every sink pin (the optimizer needs both);
* the model is a single ideal clock: every clock pin is launched at time 0 and captured at the
  clock period.

`Sta::update()` is an incremental version for the case that only the library cell of some
instances changed (a sizing move): it recomputes the loads of the affected nets, re-evaluates the
affected instances in topological order as long as arrival times or slews change, then
re-evaluates required times backwards as long as they change, and finally the endpoints on changed
nets. `tsize -selfcheck` compares every incremental update against a full analysis.

**Clock edges.** Only one arrival time is kept per net, so the launching edge of a path is not
known. If a design uses the falling edge anywhere (a falling edge cell, a clock that goes through
an inverter or through logic that may invert it), *every* path is constrained to half a period,
with a warning. This is exact for paths between opposite edges and pessimistic for all others.
`dfflibmap` produces such clock inverters for every `negedge` flip-flop.

**Validation.** See section 6.

### 4.3 Timing driven sizing and buffering (`timing_optimize`, command `tsize`)

Three phases:

1. **Repair.** Nets that violate `max_capacitance` of their driver or the maximum transition time
   get a stronger driver, or their sinks are split into groups behind buffers.
2. **Critical path optimization.** Repeat: trace the worst path and generate candidate moves for
   it:
   * *resize* a cell on the path to any other member of its family (this includes the launching
     flip-flop; making a cell smaller can pay off when it unloads the previous stage);
   * *downsize a side load*: a cell that is not on the path but loads a net of the path, if it has
     slack;
   * *isolate*: move the sinks of a path net that have slack behind a buffer, so that the driver
     only sees the critical sinks;
   * *buffer*: drive all sinks of a path net through one or several parallel buffers (weak drivers
     such as flip-flops, nets with very large fanout);
   * *decouple*: drive a primary output through a buffer, so that its load is off the path.

   Each candidate gets a local estimate of the path delay gain (previous stage with the changed
   load, the cell itself with the changed input slew, next stage with the changed slew). The
   candidates are tried in order of estimated gain and the first one that an exact timing update
   confirms is kept: it has to improve WNS, or keep WNS and reduce the total negative slack of the
   near-critical nets, or reduce a smooth maximum of all endpoint delays while WNS stays within a
   small tolerance (designs like AES have hundreds of equally critical paths). It must not add a
   capacitance or transition violation. If no single move qualifies, sequences of up to three moves
   are tried (`-lookahead`).
   Rejected moves are undone. The phase ends when the period is met, or when no candidate for the
   worst path is accepted.
3. **Area recovery.** Cells are moved to the next smaller size in batches: in topological order,
   each cell may consume slack only as far as the accumulated delay increase of its fanin still
   fits its own slack. The batch is verified with a timing update; moves on violating paths are
   reverted until the timing limit holds again. The limit is zero slack if a period is given, and
   the achieved WNS otherwise. Downsizing must not add limit violations either. A last repair
   pass follows if any are left.

Cells with the `keep` attribute are never resized.

### 4.4 Timing driven mapping (`tmap`)

A candidate is an *architecture variant* combined with a *mapping recipe*.

**Architecture variants** decide how arithmetic is bit-blasted before ABC sees it:

| Variant | Adders | Multipliers | Explored |
|---|---|---|---|
| `default` | Brent-Kung (as `synth`) | array (as `synth`) | always |
| `ks` | Kogge-Stone | array | designs without multipliers |
| `booth_ks` | Kogge-Stone | Booth | designs with multipliers |
| `ripple` | ripple-carry | array | only with a clock period |
| `booth`, `sklansky`, `hc`, `booth_sklansky` | | | at `-effort 2` |

**Mapping recipes** are ABC scripts (`tmap -list` prints them). The speed recipes restructure the
logic for delay (SOP balancing, `&if -g` / `if -g`), compute structural choices (`dch`) and map
with the classic `map` command. Unlike `&nf`, `map` derives a load dependent delay for every gate
class from the NLDM tables (gain based model), so both the mapping and its delay target `-D` work
in real picoseconds. The area recipe skips the restructuring. ABC's `buffer` command is not used;
buffering is left to `timing_optimize`, which also knows about the flip-flops.

**Exploration.**

Before mapping, the bit-blasted netlist is cleaned up with the default `abc` script. For designs
with multipliers a script without `&fraig` is used, which does not finish in reasonable time on a
32x32 Booth multiplier.

* *No period:* all architecture variants are mapped with the first speed recipe; the remaining
  speed recipes are tried on the best architecture; the fastest candidate wins.
* *With a period:* the delay budget of the logic (period minus clock-to-output delay and setup
  time of the flip-flops used) is passed to the mapper and to ABC's sizing as delay target. All
  variants are mapped with the speed and the area recipes; the smallest candidate that meets the
  period after `timing_optimize` wins (the fastest one if none does).
* *Delay target refinement:* the delay model of the mapper is only an estimate of the timing after
  sizing. The measured slack of the selected candidate is therefore fed back: if the candidate
  misses the period, the target is tightened by the relative violation and the candidate is
  re-mapped; if it meets the period with slack to spare, the target is relaxed to recover area
  (`-refine <n>` iterations, default 3).
* *Fallback:* if still no candidate meets the period, every architecture variant is also mapped
  with every speed recipe and no delay target, and the fastest candidate is kept. A delay target
  that is too tight can give a slower netlist than none.

Every candidate is evaluated by the same timing engine that `tsta` uses, so the selection is made
on measured timing, not on the estimate of the mapper.

## 5. The ABC recipe study

The mapping recipes of `tmap` were chosen in a study on 18 of the 20 benchmark designs (`aes` and
`ethmac` were held out). Every design was bit-blasted once, mapped by ABC with each candidate
script, sized with `tsize`, and timed with `tsta`. The score is the geometric mean of the minimum
period relative to Yosys' default `abc -liberty` script, sized in the same way (lower is better).

| Script (round 1, 18 designs) | Period |
|---|---|
| SOP balancing (`&if -g` twice), `dch -f`, `map` | 0.547 |
| `if -g`, `dch -f`, `map` | 0.555 |
| five rounds of `&syn2; &if -g; &synch2`, `dch -f`, `map` | 0.562 |
| ORFS area script (`dch; map -B 0.9`) | 0.670 |
| `dch -f; map` | 0.701 |
| ORFS speed script (`&nf` based) | 0.798 |
| `amap` | 0.995 |
| Yosys default (`&nf`, unit delay) | 1.000 |
| `&dch -f; &nf` | 1.035 |

A third round (13 designs) refined the winners. The scripts that became the `speed`, `speed2` and
`speed3` recipes of `tmap` scored 0.480, 0.485 and 0.505, at 1.34, 1.31 and 1.22 times the area
of the default script. Findings:

* **The mapper is the main lever.** Every script that ends in `map` beats every script that ends
  in `&nf`, because `&nf` sees a unit-delay library (section 1). Giving `read_lib` a gain (`-G`)
  fixes that for `&nf` (1.002 to 0.727) and makes no difference to `map`, which derives its own
  delays.
* **Delay oriented restructuring before mapping pays off** (SOP balancing with `&if -g` / `if -g`,
  then structural choices with `dch -f`).
* **`map -B 0.9`** (area recovery under the delay constraint) costs no speed after sizing and saves
  area.
* **ABC's own `buffer` and `upsize` are not needed** once `tsize` runs after mapping; dropping them
  made no measurable difference.

## 6. Results

### 6.1 Method

* **Library and conditions.** sky130_fd_sc_hd, typical corner (`tt_025C_1v80`), the usual sky130
  don't-use list (clock buffers, delay cells, physical cells, `lpflow`).
* **Designs.** 20 designs: picorv32, sha256, aes, gcd, riscv32i, dynamic_node, ethmac, md5,
  usb_cdc_core, spm (from the OpenROAD flow scripts and similar collections) and ten hand-written
  arithmetic and datapath blocks (`examples/timing/bench`). `aes` and `ethmac` were not used during
  development and are reported separately.
* **Baseline: the best of 17 stock configurations, per design.** All of them are stock Yosys
  (`synth -flatten`, `dfflibmap`, `abc -liberty`) asked for minimum delay: plain `abc`; `abc` with
  a driver/load constraint file, with and without `-D 1`; the ORFS ABC script; the ten LibreLane
  ABC strategies (`AREA 0-4`, `DELAY 0-4`, with buffering and sizing); Kogge-Stone adders with
  the `AREA 3` and with the `DELAY 4` strategy; and Booth multipliers with Kogge-Stone adders and
  `AREA 3`. For each
  design the fastest of these is the baseline. This is a stronger baseline than any single flow.
* **Timing-aware flow.** `synth_timing` at its default effort without a period (fastest
  implementation).
* **Timing measurement.** Pre-layout timing is measured by OpenSTA on the written netlist, with an
  ideal clock and ideal inputs, no wire load. For all 477 netlists of the campaign `tsta` agrees
  with OpenSTA to within 1.1 ps (0.2 ps except for one stock ethmac netlist).
* **After placement.** Every netlist (stock and timing-aware) is placed by OpenROAD at 40%
  utilization, repaired (`repair_design`, `repair_timing`) and timed with parasitics estimated from
  the placement. The repair target is the same for all netlists of a design: 0.8 times the
  period of the timing-aware netlist, a target that no netlist meets, so that the repair works as
  hard on all of them. There is no clock tree and no routing.

### 6.2 Maximum frequency

Pre-layout:

| design | best stock configuration | stock fmax [MHz] | synth_timing fmax [MHz] | fmax gain | stock area | synth_timing area | area change |
|---|---|---|---|---|---|---|---|
| picorv32 | ks_delay4 | 207.5 | 529.2 | **2.55x** | 82625 | 88858 | +8% |
| sha256 | ks_area3 | 176.7 | 238.3 | **1.35x** | 73990 | 83423 | +13% |
| aes (held out) | ll_area3 | 290.8 | 336.7 | **1.16x** | 198852 | 194728 | -2% |
| gcd | ks_area3 | 434.8 | 647.9 | **1.49x** | 2630 | 3212 | +22% |
| riscv32i | ks_area3 | 207.0 | 365.4 | **1.77x** | 63392 | 69793 | +10% |
| dynamic_node | ks_area3 | 172.3 | 447.7 | **2.60x** | 101848 | 109279 | +7% |
| ethmac (held out) | orfs | 245.5 | 465.1 | **1.89x** | 425693 | 459765 | +8% |
| md5 | ks_area3 | 122.6 | 195.8 | **1.60x** | 53722 | 60135 | +12% |
| usb_cdc_core | ll_area3 | 435.5 | 627.3 | **1.44x** | 17264 | 18308 | +6% |
| spm | ll_area3 | 1368.6 | 1444.0 | **1.06x** | 3999 | 3301 | -17% |
| add64 | ks_delay4 | 262.8 | 569.3 | **2.17x** | 7453 | 7141 | -4% |
| mul16 | booth_ks_area3 | 267.5 | 356.3 | **1.33x** | 11979 | 16068 | +34% |
| mul32 | ll_area3 | 124.4 | 286.2 | **2.30x** | 38756 | 50023 | +29% |
| mac16 | ks_area3 | 228.4 | 350.1 | **1.53x** | 13314 | 16731 | +26% |
| alu32 | ks_area3 | 319.7 | 568.4 | **1.78x** | 9309 | 11815 | +27% |
| fir8 | booth_ks_area3 | 240.0 | 309.1 | **1.29x** | 20018 | 24390 | +22% |
| cmp_tree | ll_area3 | 217.5 | 255.8 | **1.18x** | 15664 | 21377 | +36% |
| barrel64 | ll_area3 | 545.2 | 623.8 | **1.14x** | 11616 | 12084 | +4% |
| div16 | ll_area3 | 61.5 | 98.1 | **1.59x** | 8517 | 16055 | +89% |
| crc32x64 | ll_area3 | 216.1 | 285.9 | **1.32x** | 13467 | 15078 | +12% |

Geometric mean over all 20 designs: **1.57x fmax** (1.06x to 2.60x, faster on all 20), at
**1.15x area**. Held-out designs: 1.48x fmax, 1.03x area.

After placement and repair:

| design | best stock configuration | stock fmax [MHz] | synth_timing fmax [MHz] | fmax gain | stock area | synth_timing area | area change |
|---|---|---|---|---|---|---|---|
| picorv32 | ks_delay4 | 220.2 | 438.3 | **1.99x** | 89164 | 93373 | +5% |
| sha256 | ks_area3 | 159.8 | 206.2 | **1.29x** | 77922 | 85717 | +10% |
| aes (held out) | ll_area3 | 241.0 | 256.4 | **1.06x** | 208186 | 200408 | -4% |
| gcd | ks_area3 | 468.5 | 602.2 | **1.29x** | 3097 | 3617 | +17% |
| riscv32i | ks_area3 | 195.3 | 272.3 | **1.39x** | 67998 | 74356 | +9% |
| dynamic_node | ks_area3 | 318.9 | 379.7 | **1.19x** | 106546 | 114688 | +8% |
| ethmac (held out) | ll_delay0 | 254.4 | 322.9 | **1.27x** | 451111 | 472323 | +5% |
| md5 | ks_area3 | 112.8 | 166.9 | **1.48x** | 55170 | 63704 | +15% |
| usb_cdc_core | ll_delay2 | 448.1 | 550.3 | **1.23x** | 22492 | 20387 | -9% |
| spm | ll_area3 | 1312.0 | 1395.3 | **1.06x** | 4011 | 3388 | -16% |
| add64 | ks_delay4 | 262.6 | 524.9 | **2.00x** | 8274 | 7234 | -13% |
| mul16 | booth_ks_area3 | 265.3 | 316.0 | **1.19x** | 13617 | 16657 | +22% |
| mul32 | ll_area3 | 118.5 | 244.1 | **2.06x** | 39926 | 53729 | +35% |
| mac16 | ks_area3 | 217.4 | 308.5 | **1.42x** | 14066 | 18463 | +31% |
| alu32 | ks_area3 | 297.5 | 454.9 | **1.53x** | 10140 | 12199 | +20% |
| fir8 | booth_ks_area3 | 228.1 | 279.8 | **1.23x** | 22077 | 25682 | +16% |
| cmp_tree | ll_area3 | 210.0 | 226.8 | **1.08x** | 16969 | 21642 | +28% |
| barrel64 | ll_area3 | 555.9 | 502.1 | **0.90x** | 13432 | 13016 | -3% |
| div16 | ll_area3 | 60.4 | 87.3 | **1.45x** | 9977 | 17297 | +73% |
| crc32x64 | ll_area3 | 217.3 | 253.7 | **1.17x** | 16498 | 15823 | -4% |

Geometric mean over all 20 designs: **1.33x fmax** (0.90x to 2.06x, faster on 19 of 20), at
**1.11x area**. Held-out designs: 1.16x fmax, 1.00x area.

* The gain shrinks after placement. OpenROAD's repair improves the stock netlists more than the
  already sized timing-aware ones, and wire delay, which the pre-layout model ignores, is added to
  both. `dynamic_node` is the extreme case (2.60x pre-layout, 1.19x placed): repair takes its
  best stock netlist from 172 to 319 MHz.
* `barrel64` is the one design that is slower after placement (0.90x): its timing-aware netlist
  drops from 624 to 502 MHz, while the stock one goes from 545 to 556 MHz. The timing-aware netlist
  was sized for a model without wires; `-wire_cap` (a capacitance per fanout) was not used in the
  campaign and has not been evaluated on this design.
* Area grows most where a faster architecture is selected for wide arithmetic: `div16` (+89%
  pre-layout), `cmp_tree`, `mul16`, `mul32`, `mac16`, `alu32` (+26% to +36%). For several designs
  the timing-aware netlist is both faster and smaller (`aes`, `spm`, `add64`).

Against single stock flows, the geometric mean fmax gain is larger: 2.06x pre-layout and 1.69x
placed against the ORFS script, 1.76x and 1.51x against the best LibreLane strategy (`AREA 3`),
1.58x and 1.36x against the best single stock configuration (Kogge-Stone adders with `AREA 3`; 19
designs, it does not finish on `mul32` within the time limit), 2.59x and 2.12x against
`abc -constr`. Against plain `abc -liberty`, whose netlists are not sized at all, it is 5.14x
pre-layout and 2.11x after OpenROAD's repair.

### 6.3 Trading area for timing

Each design was synthesized again with a clock period of 1.05, 1.2, 1.5, 2 and 3 times the
period of its fastest implementation. The table shows the area relative to the fastest
implementation:

| design | T = 1.05x fastest | T = 1.2x fastest | T = 1.5x fastest | T = 2.0x fastest | T = 3.0x fastest |
|---|---|---|---|---|---|
| picorv32 | -3% MISSED (met at -1% with the fallback, see below) | -5% | -7% | -8% | -10% |
| sha256 | +4% | -2% | -15% | -17% | -20% |
| aes | -5% | -7% | -8% | -16% | -20% |
| gcd | -12% | -16% | -29% | -31% | -33% |
| riscv32i | -0% | -7% | -8% | -14% | -17% |
| dynamic_node | -1% | -7% | -10% | -11% | -12% |
| ethmac | -4% | -4% | -8% | -11% | -12% |
| md5 | -2% | -3% | -12% | -14% | -15% |
| usb_cdc_core | -4% | -6% | -10% | -10% | -11% |
| spm | +0% | +0% | -13% | -15% | -13% |
| add64 | -2% | -3% | -3% | -7% | -12% |
| mul16 | -10% | -15% | -36% | -37% | -45% |
| mul32 | +2% | -3% | -7% | -36% | -36% |
| mac16 | -5% | -8% | -8% | -32% | -38% |
| alu32 | -7% | -13% | -15% | -27% | -34% |
| fir8 | -4% | -17% | -23% | -23% | -35% |
| cmp_tree | -25% | -35% | -37% | -38% | -44% |
| barrel64 | -8% | -11% | -12% | -18% | -20% |
| div16 | -6% | -13% | -50% | -57% | -58% |
| crc32x64 | -6% | -12% | -18% | -47% | -47% |

* In the campaign, 99 of 100 targets were met. The miss was `picorv32` at 1.05x: 2.048 ns for a
  1.984 ns target, although the fastest implementation reaches 1.890 ns. All candidates of the
  period mode were mapped with a delay target, and none of them met the period. This led to the
  fallback described in section 4.4, which was added after the campaign; with it, `picorv32`
  meets the target (1.981 ns with `ks/speed2`, at 1% less area than the fastest implementation).
  The fallback only runs when no candidate meets the period, so the other 99 results are
  unchanged.
* Relaxing the period by 20% saves 0% (`spm`) to 35% area, relaxing it to 3x saves 10% to 58%.
* For 49 of the 99 targets met, no stock configuration of the design is fast enough. For the
  other 50, the smallest stock netlist that meets the period was compared: the timing-aware one is
  smaller in 44 of them and 6.5% smaller in the geometric mean.

### 6.4 Electrical limits

OpenSTA counts no max slew or max capacitance violation in any of the 140 timing-aware netlists.
Among the 337 stock netlists, 75 violate them (285,834 slew and 2,998 capacitance violations in
total). Every stock configuration has violations on 4 of the 20 designs, plain `abc -liberty` on 11.

### 6.5 Equivalence

Two independent checks were run on the timing-aware netlists.

**Formal.** `examples/timing/verify/formal.sh` checks sequential equivalence between a netlist and
the design at the input of `tmap` (bit-blasted without ABC), with all flip-flops starting at zero.
Yosys builds the miter, with the ports paired by name; ABC proves it with signal correspondence
(`&scorr`), then `dprove`; if neither succeeds, `bmc3` looks for a counterexample for 300 s. Of the
140 timing-aware netlists of the campaign (20 designs, the fastest at two efforts and five clock
periods), **99 are proven and none is different.** The other 41 are undecided: no counterexample
within the 2 to 7 clock cycles that `bmc3` reaches. All netlists of 12 designs are proven; none of
`aes`, `ethmac` and `fir8`, and only some of the multiplier designs, `crc32x64` and `riscv32i`.
The checker reports both planted bugs (one gate changed in an `alu32` and in a `picorv32`
netlist) as different.

A first version of the checker let ABC build the miter, which pairs outputs by position. The port
order of a netlist written by Yosys differs from the RTL for `riscv32i` and `ethmac`, so all their
netlists, stock ones included, were reported as different. The results above are from the
corrected checker.

**Simulation.** `examples/timing/verify/check.sh` simulates a netlist and the RTL with the same
random stimulus (3000 cycles, three seeds, the fastest netlist of every design). 16 designs pass
on all seeds. `aes` (one seed), `dynamic_node`, `picorv32` (one seed) and `ethmac` fail. Two stock
netlists of each of the first three fail in the same cycles with the same values, and a stock
`ethmac` netlist fails as well. The cause lies in the testbench or in the RTL semantics (for
example `x` values that synthesis may resolve either way; the testbench also drives all clocks of
`ethmac` with one clock), not in the timing-aware flow. For these designs and for all designs with
undecided formal results, the fastest timing-aware netlist was also simulated against the stock
ORFS netlist of the same design (`REF_NET`). All 30 runs (`aes`, `ethmac`, `dynamic_node`,
`picorv32`, `riscv32i`, `mul16`, `mul32`, `mac16`, `fir8`, `crc32x64`, three seeds each) agree in
every cycle. All outputs toggle except for
`ethmac` (36 of 120), `dynamic_node` (321 of 331), `mac16` (39 or 40 of 40) and `picorv32`.

Random simulation is weak for the processors: it reaches 6 to 33 of the 307 outputs of
`picorv32` and does not detect the planted bug, which the formal check finds.

**What is not verified.** The formal check starts at the input of `tmap`; the steps before it
(`synth_timing -run begin:map`, similar to `synth`) are covered only by simulation. Netlists after
placement and repair were not checked.

### 6.6 Runtime

`synth_timing` maps and sizes several candidates, so it is slower than a single stock run. On the
machine of the campaign (8 cores, several jobs in parallel) its default effort took 2.3 times as
long as the ORFS flow in the geometric mean, from 0.7x (`spm`) to 13.8x (`dynamic_node`, 546 s)
and 9.2x (`aes`, 1847 s). `-effort 0` maps a single candidate: it took 0.35 times as long as the
default effort for 0.95 times its fmax and 1.02 times its area.

## 7. Reproducing the results

`examples/timing` contains the scripts of the campaign (see its `README.md`): the stock and
timing-aware flows, the OpenSTA and OpenROAD evaluation, the formal and simulation checks, the
recipe study, the ten hand-written benchmark designs and a list of where the other ten come from.

```
export YOSYS=/path/to/yosys PDKPATH=/path/to/pdks/sky130A
cd examples/timing
python3 flow/campaign.py stock speed sweep limits pnr verify report
```

The regression tests of the commands are in `tests/timing` (`make -C tests/timing`). They use a
generated library with linear delay tables, so that every expected delay can be checked by hand.

## 8. Limitations and future work

* **One ideal clock.** All clocks are timed as one synchronous domain (with a warning). There are
  no hold checks, false paths, multicycle paths, generated clocks, or latch time borrowing.
  Constraints are command options, SDC is not read.
* **Mixed clock edges are pessimistic.** See section 4.2. A design with a single `negedge` register
  is optimized as if it had half the period.
* **Pre-layout timing.** Wires are either ignored or estimated with a capacitance per fanout.
* **Inputs are ideal.** Primary inputs have no driver, so their nets are never buffered.
* **Flat designs only.** `synth_timing` flattens the design.
* **Global architecture choice.** One adder/multiplier architecture is chosen for the whole
  design. Choosing it per operator from the slack of the paths through it would save area at a
  given period.
* **No retiming.** Moving registers (e.g. with `abc -dff`) is not part of the flow.
* **Runtime.** Each candidate is a full ABC run plus sizing; `-effort 0` maps a single candidate.
* **ABC `buffer` with a gain based library.** `read_lib -G` without `-a` followed by `upsize` or
  `buffer` crashes ABC (the resized cells are not part of the derived gate library); the recipes
  avoid this combination.
