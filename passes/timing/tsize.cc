/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: tsize command
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

#include "kernel/yosys.h"
#include "passes/timing/timing_opt.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct TsizePass : public Pass {
	TsizePass() : Pass("tsize", "timing driven gate sizing and buffering") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    tsize [options] [selection]\n");
		log("\n");
		log("This command optimizes the timing of a mapped netlist by changing the drive\n");
		log("strength of cells and by inserting buffers. It works on flat modules that\n");
		log("consist of cells from the liberty library and uses the same timing engine\n");
		log("as the 'tsta' command.\n");
		log("\n");
		log("With a clock period, the critical paths are optimized until all paths meet\n");
		log("the period, then cells with positive slack are downsized to recover area.\n");
		log("Without a clock period (or with -period 0) the critical path delay is\n");
		log("minimized and area is only recovered where it does not affect it.\n");
		log("\n");
		log("The optimization proceeds in three phases:\n");
		log("\n");
		log("  1. repair: maximum capacitance and maximum transition violations are\n");
		log("     fixed by upsizing drivers or by splitting the fanout with buffers.\n");
		log("  2. timing: the worst path is traced and candidate moves are generated\n");
		log("     for it (resizing a cell on the path, downsizing a side load, isolating\n");
		log("     non-critical sinks behind a buffer, driving a net through a buffer,\n");
		log("     decoupling the load of a primary output with a buffer).\n");
		log("     The moves are ranked by a local delay estimate, and a move is kept if\n");
		log("     a full timing update confirms that it improves the design.\n");
		log("  3. area: cells are downsized as far as their slack allows.\n");
		log("\n");
		log("Cells with the 'keep' attribute are never resized. No move is accepted that\n");
		log("adds a violation of an electrical limit.\n");
		log("\n");
		log("    -max_iter <n>\n");
		log("        maximum number of accepted moves in the timing phase\n");
		log("\n");
		log("    -max_time <seconds>\n");
		log("        time limit for the timing phase\n");
		log("\n");
		log("    -lookahead <n>\n");
		log("        if no single move improves the worst path, try sequences of up to\n");
		log("        <n>+1 moves (default: 2)\n");
		log("\n");
		log("    -max_tran <ns>\n");
		log("        maximum transition time (default: default_max_transition of the\n");
		log("        library)\n");
		log("\n");
		log("    -noresize, -nobuffer, -norepair, -noarea\n");
		log("        disable resizing, buffer insertion, the repair phase or the area\n");
		log("        recovery phase\n");
		log("\n");
		log("    -v\n");
		log("        print all accepted moves\n");
		log("\n");
		log("    -selfcheck\n");
		log("        compare every incremental timing update against a full analysis\n");
		log("        (slow, for debugging)\n");
		log("\n");
		Timing::TimingConfig::help();
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing TSIZE pass (timing driven sizing and buffering).\n");
		Timing::TimingConfig cfg;
		cfg.load(design);
		Timing::SizerOptions opt;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (cfg.parse_arg(args, argidx))
				continue;
			if (args[argidx] == "-max_iter" && argidx + 1 < args.size()) {
				opt.max_iter = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-max_time" && argidx + 1 < args.size()) {
				opt.max_time = atof(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-lookahead" && argidx + 1 < args.size()) {
				opt.lookahead = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-max_tran" && argidx + 1 < args.size()) {
				opt.max_tran = atof(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-noresize") {
				opt.resize = false;
				continue;
			}
			if (args[argidx] == "-nobuffer") {
				opt.buffer = false;
				continue;
			}
			if (args[argidx] == "-norepair") {
				opt.repair = false;
				continue;
			}
			if (args[argidx] == "-noarea") {
				opt.area = false;
				continue;
			}
			if (args[argidx] == "-selfcheck") {
				opt.selfcheck = true;
				continue;
			}
			if (args[argidx] == "-v") {
				opt.verbose = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);
		auto lib = cfg.library();

		for (auto module : design->selected_modules()) {
			log("Optimizing module %s.\n", Timing::log_id(module));
			auto res = Timing::timing_optimize(module, lib.get(), cfg.sta, opt);
			design->scratchpad_set_string("timing.result.wns", stringf("%.6f", res.wns));
			design->scratchpad_set_string("timing.result.tns", stringf("%.6f", res.tns));
			design->scratchpad_set_string("timing.result.min_period", stringf("%.6f", res.min_period));
			design->scratchpad_set_string("timing.result.area", stringf("%.6f", res.area));
		}
	}
} TsizePass;

PRIVATE_NAMESPACE_END
