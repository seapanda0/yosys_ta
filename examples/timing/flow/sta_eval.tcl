# Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
# OpenSTA evaluation of a mapped netlist. Inputs via env: LIB NET TOP CLK PERIOD [RPT]
# Convention: ideal clock, ideal input drivers, input/output delay 0, 33.4 fF on outputs.
set T $::env(PERIOD)
read_liberty $::env(LIB)
read_verilog $::env(NET)
link_design $::env(TOP)
# all clock-like input ports get the same clock (multi-clock designs are timed as one synchronous domain)
set clks [get_ports $::env(CLK)]
set ins {}
foreach p [all_inputs] {
  set n [get_full_name $p]
  if {$n eq $::env(CLK)} continue
  if {[string match -nocase "*clk*" $n]} { lappend clks $p } else { lappend ins $p }
}
create_clock -name clk -period $T $clks
if {[llength $ins]} { set_input_delay 0 -clock clk $ins }
set_output_delay 0 -clock clk [all_outputs]
set_load 0.0334 [all_outputs]
if {[info exists ::env(RPT)]} {
  report_checks -path_delay max -fields {slew cap fanout} -digits 3 > $::env(RPT)
}
set wns [sta::worst_slack -max]
set tns [sta::total_negative_slack -max]
set pmin [expr {$T - $wns}]
puts [format "STA_RESULT period=%.4f wns=%.4f tns=%.3f min_period=%.4f fmax_mhz=%.2f" $T $wns $tns $pmin [expr {1000.0/$pmin}]]
exit
