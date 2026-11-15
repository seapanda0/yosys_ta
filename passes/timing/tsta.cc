/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: timing_setup and tsta commands
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
#include "passes/timing/timing_sta.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct TimingSetupPass : public Pass {
	TimingSetupPass() : Pass("timing_setup", "set timing constraints for timing-aware synthesis") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    timing_setup [options]\n");
		log("\n");
		log("This command stores the timing library and the timing constraints that are\n");
		log("used by the timing-aware synthesis commands (tsta, tsize, tmap, synth_timing)\n");
		log("in the design scratchpad (timing.*).\n");
		log("\n");
		log("The timing model is a single ideal clock that drives all sequential cells.\n");
		log("\n");
		Timing::TimingConfig::help();
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing TIMING_SETUP pass.\n");
		Timing::TimingConfig cfg;
		cfg.load(design);
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (cfg.parse_arg(args, argidx))
				continue;
			break;
		}
		extra_args(args, argidx, design, false);
		cfg.store(design);
		log("Timing setup:%s\n", cfg.args());
	}
} TimingSetupPass;

struct TstaPass : public Pass {
	TstaPass() : Pass("tsta", "liberty based static timing analysis") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    tsta [options] [selection]\n");
		log("\n");
		log("This command runs a static timing analysis on the selected modules, which\n");
		log("must be flat netlists of cells from the liberty library. It uses the NLDM\n");
		log("delay tables of the library with lumped capacitive loads, and checks setup\n");
		log("times against a single ideal clock.\n");
		log("\n");
		log("The results (wns, tns, min_period, area) are stored in the scratchpad as\n");
		log("timing.result.<name>.\n");
		log("\n");
		log("    -paths <n>\n");
		log("        print the <n> worst endpoints (default: 1). The full path is\n");
		log("        printed for the worst one.\n");
		log("\n");
		log("    -quiet\n");
		log("        only print the summary line\n");
		log("\n");
		Timing::TimingConfig::help();
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing TSTA pass (static timing analysis).\n");
		Timing::TimingConfig cfg;
		cfg.load(design);
		int n_paths = 1;
		bool quiet = false;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (cfg.parse_arg(args, argidx))
				continue;
			if (args[argidx] == "-paths" && argidx + 1 < args.size()) {
				n_paths = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-quiet") {
				quiet = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);
		auto lib = cfg.library();

		for (auto module : design->selected_modules()) {
			Timing::Sta sta(module, lib.get(), cfg.sta);
			sta.run();
			log("\n");
			sta.report_summary(stringf("%s: ", Timing::log_id(module)).c_str());
			design->scratchpad_set_string("timing.result.wns", stringf("%.6f", sta.wns));
			design->scratchpad_set_string("timing.result.tns", stringf("%.6f", sta.tns));
			design->scratchpad_set_string("timing.result.min_period", stringf("%.6f", sta.min_period()));
			design->scratchpad_set_string("timing.result.area", stringf("%.6f", sta.area()));
			if (quiet || sta.worst_endpoint < 0)
				continue;
			log("\nCritical path:\n");
			sta.report_path(sta.worst_path());
			auto &we = sta.endpoints[sta.worst_endpoint];
			if (we.inst >= 0)
				log("  endpoint: %s (%s) pin %s, required %.4f, slack %.4f\n", Timing::log_id(sta.insts[we.inst].cell),
						sta.insts[we.inst].lc->name, sta.insts[we.inst].lc->pins[we.pin].name,
						sta.preq(we.inst, we.pin, we.rf), we.slack);
			else
				log("  endpoint: primary output, required %.4f, slack %.4f\n", sta.opt.period - sta.opt.output_delay, we.slack);
			if (n_paths > 1) {
				std::vector<int> idx(sta.endpoints.size());
				for (int i = 0; i < GetSize(idx); i++)
					idx[i] = i;
				std::sort(idx.begin(), idx.end(), [&](int a, int b) { return sta.endpoints[a].slack < sta.endpoints[b].slack; });
				log("\nWorst endpoints:\n");
				for (int i = 0; i < std::min(n_paths, GetSize(idx)); i++) {
					auto &e = sta.endpoints[idx[i]];
					log("  %9.4f  %s\n", e.slack, e.inst >= 0 ?
							stringf("%s/%s", Timing::log_id(sta.insts[e.inst].cell), sta.insts[e.inst].lc->pins[e.pin].name) :
							log_signal(sta.nets[e.net].bit));
				}
			}
		}
	}
} TstaPass;

PRIVATE_NAMESPACE_END
