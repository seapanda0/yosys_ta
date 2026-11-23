# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
# pnr_eval.tcl -- post-placement max-frequency evaluation of a sky130A (sky130_fd_sc_hd)
# gate-level netlist. Floorplan -> pin placement -> global placement -> legalization ->
# repair (repair_design / repair_timing) -> legalization, with an ideal clock (no CTS, no routing).
#
# Environment inputs:
#   NET     gate-level Verilog netlist (flat, structural)           [required]
#   TOP     top module name                                         [required]
#   CLK     clock port name                                         [required]
#   PERIOD  clock period in ns (constraints + repair target)        [required]
#   OUT     output directory                                        [required]
#   UTIL    core utilization in percent                             [optional, default 40]
#   LIB, TLEF, CLEF  override PDK file locations (optional)
#
# Prints, for each stage (placed, repaired), one line:
#   PNR_RESULT stage=<placed|repaired> period=.. wns=.. tns=.. min_period=.. fmax_mhz=.. area=.. cells=.. util=..
# The "placed" stage is legal placement, before any repair. The "repaired" stage is after repair.

proc envreq {name} {
    if {![info exists ::env($name)] || $::env($name) eq ""} {
        puts "PNR_ERROR missing required environment variable $name"
        exit 2
    }
    return $::env($name)
}

set PDK_HD $::env(PDKPATH)/libs.ref/sky130_fd_sc_hd
set NET    [envreq NET]
set TOP    [envreq TOP]
set CLK    [envreq CLK]
set PERIOD [envreq PERIOD]
set OUT    [envreq OUT]
set UTIL   [expr {[info exists ::env(UTIL)] && $::env(UTIL) ne "" ? $::env(UTIL) : 40}]
set LIB  [expr {[info exists ::env(LIB)]  ? $::env(LIB)  : "$PDK_HD/lib/sky130_fd_sc_hd__tt_025C_1v80.lib"}]
set TLEF [expr {[info exists ::env(TLEF)] ? $::env(TLEF) : "$PDK_HD/techlef/sky130_fd_sc_hd__nom.tlef"}]
set CLEF [expr {[info exists ::env(CLEF)] ? $::env(CLEF) : "$PDK_HD/lef/sky130_fd_sc_hd.lef"}]

# LibreLane sky130A conventions (libs.tech/librelane/config.tcl, sky130_fd_sc_hd/config.tcl, tracks.info)
set SITE        unithd
set SIG_RC_LAYER met2
set CLK_RC_LAYER met5
set IO_H_LAYER  met3
set IO_V_LAYER  met2
set CORE_MARGIN 2.0      ;# um, all four sides

file mkdir $OUT
puts "PNR_INFO top=$TOP net=$NET clk=$CLK period=$PERIOD util=$UTIL out=$OUT"

# ---------------------------------------------------------------- a. read + constrain
read_lef $TLEF
read_lef $CLEF
read_liberty $LIB
read_verilog $NET
link_design $TOP

create_clock -name clk -period $PERIOD [get_ports $CLK]
set clk_ports [list $CLK]
# Other clock-like ports (name contains "clk"): same clock, own name.
set extra_clk 0
foreach p [all_inputs] {
    set pn [get_full_name $p]
    if {$pn ne $CLK && [string match *clk* $pn]} {
        create_clock -name clk_$extra_clk -period $PERIOD $p
        lappend clk_ports $pn
        incr extra_clk
    }
}
# Non-clock inputs: input delay 0 relative to clk, no driving cell (ideal), as in flow/sta_eval.tcl
set ins {}
foreach p [all_inputs] {
    if {[lsearch -exact $clk_ports [get_full_name $p]] < 0} { lappend ins $p }
}
if {[llength $ins]} { set_input_delay 0 -clock clk $ins }
set_output_delay 0 -clock clk [all_outputs]
set_load 0.0334 [all_outputs]
puts "PNR_INFO clock ports: [join $clk_ports { }] ; data inputs: [llength $ins] ; outputs: [llength [all_outputs]]"

# RC for estimate_parasitics -placement (tech LEF values; same layers as LibreLane)
set_wire_rc -signal -layer $SIG_RC_LAYER
set_wire_rc -clock -layer $CLK_RC_LAYER

# ---------------------------------------------------------------- b. floorplan, tracks, pins
initialize_floorplan -utilization $UTIL -aspect_ratio 1 -core_space $CORE_MARGIN -site $SITE
# Tracks exactly as LibreLane generates them for sky130A (openroad.py, old_to_new_tracks)
make_tracks li1   -x_offset 0.23 -x_pitch 0.46 -y_offset 0.17 -y_pitch 0.34
make_tracks met1  -x_offset 0.17 -x_pitch 0.34 -y_offset 0.17 -y_pitch 0.34
make_tracks met2  -x_offset 0.23 -x_pitch 0.46 -y_offset 0.23 -y_pitch 0.46
make_tracks met3  -x_offset 0.34 -x_pitch 0.68 -y_offset 0.34 -y_pitch 0.68
make_tracks met4  -x_offset 0.46 -x_pitch 0.92 -y_offset 0.46 -y_pitch 0.92
make_tracks met5  -x_offset 1.70 -x_pitch 3.40 -y_offset 1.70 -y_pitch 3.40

# Pins: global placement without IOs, then pins matched to cell locations (ioplacer-like),
# then the final global placement. Density as LibreLane's default rule: min(util + 10, 100) %.
# (place_pins -random is obsolete in this OpenROAD version, so no random initial pin placement.)
set DENSITY [expr {min($UTIL + 10, 100) / 100.0}]
global_placement -skip_io -density $DENSITY
place_pins -hor_layers $IO_H_LAYER -ver_layers $IO_V_LAYER
global_placement -density $DENSITY

# ---------------------------------------------------------------- c. legalize + parasitics
detailed_placement
if {[catch {check_placement -verbose} msg]} { puts "PNR_WARN check_placement after GPL: $msg" }
estimate_parasitics -placement

# ---------------------------------------------------------------- helpers
set BLOCK [ord::get_db_block]
set DBU   [$BLOCK getDbUnitsPerMicron]

proc design_area_um2 {block dbu} {
    set a 0
    foreach inst [$block getInsts] {
        set m [$inst getMaster]
        set a [expr {$a + double([$m getWidth]) * double([$m getHeight])}]
    }
    return [expr {$a / ($dbu * $dbu)}]
}

proc pnr_result {stage period util block dbu} {
    set wns [sta::worst_slack -max]
    set tns [sta::total_negative_slack -max]
    set pmin [expr {$period - $wns}]
    set fmax [expr {1000.0 / $pmin}]
    set area [design_area_um2 $block $dbu]
    set cells [llength [$block getInsts]]
    puts [format "PNR_RESULT stage=%s period=%.4f wns=%.4f tns=%.3f min_period=%.4f fmax_mhz=%.2f area=%.1f cells=%d util=%s" \
        $stage $period $wns $tns $pmin $fmax $area $cells $util]
}

# ---------------------------------------------------------------- d. placed timing
pnr_result placed $PERIOD $UTIL $BLOCK $DBU

# ---------------------------------------------------------------- e. repair
# Buffer/resize for max slew, max cap and max fanout (sky130 lib limits).
repair_design -verbose
# Setup repair: target slack >= 0 at PERIOD; -repair_tns 100 works on all failing endpoints.
# RT_ARGS (optional) appends extra repair_timing options for experiments.
set rt_extra [expr {[info exists ::env(RT_ARGS)] ? $::env(RT_ARGS) : ""}]
eval repair_timing -setup -setup_margin 0 -repair_tns 100 -verbose $rt_extra
# New cells may sit on illegal sites: legalize again, then refresh parasitics.
detailed_placement
if {[catch {check_placement -verbose} msg]} { puts "PNR_WARN check_placement after repair: $msg" }
estimate_parasitics -placement

# ---------------------------------------------------------------- f. repaired timing
pnr_result repaired $PERIOD $UTIL $BLOCK $DBU

sta::redirect_file_begin $OUT/pnr_path.rpt
report_checks -path_delay max -fields {slew cap fanout} -digits 3
sta::redirect_file_end
write_def $OUT/repaired.def
exit 0
