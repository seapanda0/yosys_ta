# Timing-aware synthesis for Yosys

This document describes the timing-aware synthesis flow that was added to Yosys on the
`timing-aware-synth` branch: what was built, why, how it is structured, how to use it, and what it
achieves on a set of benchmark designs with the SkyWater 130 nm (sky130_fd_sc_hd) library.

@@RESULTS_SUMMARY@@

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

Every candidate is evaluated by the same timing engine that `tsta` uses, so the selection is made
on measured timing, not on the estimate of the mapper.

@@RESULTS_BODY@@

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
