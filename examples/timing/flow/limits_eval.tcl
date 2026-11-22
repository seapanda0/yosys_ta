read_liberty $::env(LIB); read_verilog $::env(NET); link_design $::env(TOP)
create_clock -name clk -period 10 [get_ports $::env(CLK)]
set_load 0.0334 [all_outputs]
puts "SLEW [sta::max_slew_violation_count] CAP [sta::max_capacitance_violation_count]"
exit
