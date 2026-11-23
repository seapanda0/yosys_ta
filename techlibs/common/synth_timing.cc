/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis script
 *
 *  Written by Claude, an AI model made by Anthropic (see docs/timing_aware_synthesis.md).
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/celltypes.h"
#include "kernel/rtlil.h"
#include "kernel/log.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct SynthTimingPass : public ScriptPass {
	SynthTimingPass() : ScriptPass("synth_timing", "timing-aware synthesis script for standard cell libraries") {}

	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    synth_timing [options]\n");
		log("\n");
		log("This command runs timing-aware synthesis for a standard cell library. The\n");
		log("result is a flat netlist of library cells that has been optimized against\n");
		log("the given timing constraints, using the delay tables of the library.\n");
		log("\n");
		log("With '-period <ns>' the script produces the smallest implementation it can\n");
		log("find that meets the clock period (area is traded for timing only as far as\n");
		log("needed). Without a period it produces the fastest implementation.\n");
		log("\n");
		log("    -top <module>\n");
		log("        use the specified module as top module\n");
		log("\n");
		log("    -liberty <file>\n");
		log("        liberty file of the standard cell library (required, can be given\n");
		log("        multiple times)\n");
		log("\n");
		log("    -dont_use <pattern>\n");
		log("        do not use cells matching this glob pattern (can be given multiple\n");
		log("        times)\n");
		log("\n");
		log("    -period <ns>, -input_delay <ns>, -output_delay <ns>, -load <fF>,\n");
		log("    -input_slew <ns>, -clock_slew <ns>, -wire_cap <fF>\n");
		log("        timing constraints, see 'help timing_setup'\n");
		log("\n");
		log("    -effort <0|1|2>\n");
		log("        number of implementation candidates to explore, see 'help tmap'\n");
		log("        (default: 1)\n");
		log("\n");
		log("    -arch <name>, -recipe <name>\n");
		log("        restrict the exploration to the given architecture variants and\n");
		log("        mapping recipes, see 'help tmap'\n");
		log("\n");
		log("    -nofsm\n");
		log("        do not run FSM optimization\n");
		log("\n");
		log("    -run <from_label>:<to_label>\n");
		log("        only run the commands between the labels (see below). an empty\n");
		log("        from label is synonymous to 'begin', and empty to label is\n");
		log("        synonymous to the end of the command list.\n");
		log("\n");
		log("\n");
		log("The following commands are executed by this synthesis command:\n");
		help_script();
		log("\n");
	}

	string top_module, timing_opts, tmap_opts;
	bool nofsm;

	void clear_flags() override
	{
		top_module.clear();
		timing_opts.clear();
		tmap_opts.clear();
		nofsm = false;
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		string run_from, run_to;
		clear_flags();

		static const pool<std::string> timing_args = {"-liberty", "-dont_use", "-period", "-input_delay", "-output_delay",
				"-load", "-input_slew", "-clock_slew", "-wire_cap"};
		static const pool<std::string> tmap_args = {"-effort", "-arch", "-recipe", "-refine", "-max_time"};

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-top" && argidx + 1 < args.size()) {
				top_module = args[++argidx];
				continue;
			}
			if (args[argidx] == "-run" && argidx + 1 < args.size()) {
				size_t pos = args[argidx + 1].find(':');
				if (pos == std::string::npos) {
					run_from = args[++argidx];
					run_to = args[argidx];
				} else {
					run_from = args[++argidx].substr(0, pos);
					run_to = args[argidx].substr(pos + 1);
				}
				continue;
			}
			if (timing_args.count(args[argidx]) && argidx + 1 < args.size()) {
				std::string value = args[argidx + 1];
				// run() tokenizes the command again
				if (value.find(' ') != std::string::npos && value.front() != '"')
					value = "\"" + value + "\"";
				timing_opts += " " + args[argidx] + " " + value;
				argidx++;
				continue;
			}
			if (tmap_args.count(args[argidx]) && argidx + 1 < args.size()) {
				tmap_opts += " " + args[argidx] + " " + args[argidx + 1];
				argidx++;
				continue;
			}
			if (args[argidx] == "-nofsm") {
				nofsm = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (!design->full_selection())
			log_cmd_error("This command only operates on fully selected designs!\n");

		log_header(design, "Executing SYNTH_TIMING pass.\n");
		log_push();

		run_script(design, run_from, run_to);

		log_pop();
	}

	void script() override
	{
		if (check_label("begin")) {
			if (help_mode)
				run("hierarchy -check [-top <top> | -auto-top]");
			else if (top_module.empty())
				run("hierarchy -check -auto-top");
			else
				run(stringf("hierarchy -check -top %s", top_module));
			if (help_mode)
				run("timing_setup <timing options>");
			else
				run("timing_setup" + timing_opts);
		}

		if (check_label("coarse")) {
			run("proc");
			run("flatten");
			run("opt_expr");
			run("opt_clean");
			run("check");
			run("opt -nodffe -nosdff");
			if (!nofsm || help_mode)
				run("fsm", "      (unless -nofsm)");
			run("opt");
			run("wreduce");
			run("peepopt");
			run("opt_clean");
		}

		if (check_label("map")) {
			if (help_mode)
				run("tmap <mapping options>", "  (arithmetic architecture selection, bit-blasting,");
			else
				run("tmap" + tmap_opts);
			if (help_mode)
				run("", "                         flip-flop mapping, ABC mapping, sizing and buffering)");
		}

		if (check_label("finalize")) {
			run("opt_clean -purge");
		}

		if (check_label("check")) {
			run("stat");
			run("tsta");
		}
	}
} SynthTimingPass;

PRIVATE_NAMESPACE_END
