/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: static timing analysis of mapped netlists
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

#ifndef TIMING_STA_H
#define TIMING_STA_H

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "passes/timing/timing_lib.h"

YOSYS_NAMESPACE_BEGIN

namespace Timing {

// Constraints. Times are in ns, capacitances in pF.
struct StaOptions {
	double period = 0;         // clock period (0: only relative slacks are meaningful)
	double input_delay = 0;    // arrival time of primary inputs
	double output_delay = 0;   // external delay after primary outputs
	double output_load = 0.0334; // capacitance on each primary output
	double input_slew = 0;     // transition time of primary inputs
	double clock_slew = 0;     // transition time of the (ideal) clock
	double wire_cap = 0;       // wire capacitance estimate per fanout
};

// Options shared by all timing commands. Defaults come from the design
// scratchpad (timing.*), which is filled by the 'timing_setup' command.
struct TimingConfig {
	std::vector<std::string> liberty_files, dont_use;
	StaOptions sta;
	bool liberty_from_args = false, dont_use_from_args = false;

	void load(RTLIL::Design *design);
	void store(RTLIL::Design *design) const;
	// Tries to parse args[argidx] (and its value). Returns true and advances
	// argidx to the last consumed argument if it is a timing option.
	bool parse_arg(const std::vector<std::string> &args, size_t &argidx);
	std::shared_ptr<Library> library() const;
	// options in command line syntax, for passing them on to another command
	std::string args() const;
	static void help();
};

// Graph based STA on a flat module consisting of liberty cells.
//  - single ideal clock: every clock pin is launched/captured at time 0/period
//  - NLDM delay calculation with lumped capacitive loads
//  - setup (and recovery) checks only
struct Sta {
	static constexpr double NEG_INF = -1e30, POS_INF = 1e30;

	struct Inst {
		RTLIL::Cell *cell = nullptr;
		const Cell *lc = nullptr;
		std::vector<int> pin_net; // net index per library pin (-1: unconnected or constant)
		int pin_base = 0;
	};

	struct Net {
		SigBit bit;
		int drv = -1, drv_pin = -1; // driving instance and pin (-1: primary input or undriven)
		std::vector<std::pair<int, int>> sinks; // (instance, pin)
		int n_po = 0;
		bool pi = false, clock = false;
		double load[2] = {0, 0};
		double arr[2] = {NEG_INF, NEG_INF};
		double slew[2] = {0, 0};
		double req[2] = {POS_INF, POS_INF};
		int pred_pin[2] = {-1, -1}; // input pin of the driver the worst arrival comes from
		int pred_rf[2] = {0, 0};
		double old_arr[2], old_slew[2]; // scratch space for incremental updates

		bool timed(int rf) const { return arr[rf] > NEG_INF / 2; }
		double slack(int rf) const { return timed(rf) && req[rf] < POS_INF / 2 ? req[rf] - arr[rf] : POS_INF; }
		double slack() const { return std::min(slack(RISE), slack(FALL)); }
	};

	struct Endpoint {
		int net, inst, pin; // inst/pin are -1 for primary outputs
		double slack;
		int rf;
	};

	struct PathNode {
		int net, rf;
		int inst, in_pin; // driver of 'net' and the input pin the path enters through (-1 for startpoints)
	};

	RTLIL::Module *module;
	const Library *lib;
	StaOptions opt;
	SigMap sigmap;

	// Binding of a cell type of the design to the library: IdStrings are only
	// valid within the name pool of the design, so they are resolved here.
	struct TypeBinding {
		const Cell *lc = nullptr;
		std::vector<IdString> pin_ids;
	};
	dict<IdString, TypeBinding> bindings;
	const TypeBinding &bind(IdString type);
	IdString type_id(const Cell *lc) { return module->design->twines().add("\\" + lc->name); }
	IdString pin_id(const Cell *lc, int pin) { return module->design->twines().add("\\" + lc->pins[pin].name); }

	std::vector<Inst> insts;
	std::vector<Net> nets;
	dict<SigBit, int> net_of;
	dict<RTLIL::Cell *, int> inst_of;
	std::vector<int> order;       // topological order of instances
	std::vector<int> topo_pos;    // position of each instance in 'order'
	std::vector<std::vector<int>> net_eps; // endpoints per net
	std::vector<char> queued;
	std::vector<double> pin_req;  // required time per instance pin and transition
	std::vector<Endpoint> endpoints;
	pool<std::string> unknown_types;
	int n_pins = 0;
	bool quiet = false;

	// Only one arrival time is kept per net, so the launching edge of a path is
	// unknown. If any sequential cell uses the falling edge, every path has to
	// fit into half a period: exact for paths between opposite edges and
	// pessimistic for all others, but never optimistic.
	bool half_cycle = false;
	bool warned_half_cycle = false;

	double wns = 0, tns = 0;
	int worst_endpoint = -1;

	Sta(RTLIL::Module *module, const Library *lib, const StaOptions &opt) : module(module), lib(lib), opt(opt) {}

	void build();     // (re)extract the timing graph from the module
	void propagate(); // compute loads, arrivals, required times and slacks
	void run() { build(); propagate(); }
	// incremental version of propagate() after a change of Inst::lc
	void update(const std::vector<int> &changed);

	void compute_load(int n);
	void eval_inst(int i);
	void eval_inst_req(int i);
	void eval_net_req(int n);
	void eval_endpoint(int e);
	int trace_clocks();
	void summarize();

	double &preq(int inst, int pin, int rf) { return pin_req[(insts[inst].pin_base + pin) * 2 + rf]; }
	double pin_slack(int inst, int pin) {
		int n = insts[inst].pin_net[pin];
		double s = POS_INF;
		if (n < 0)
			return s;
		for (int rf = 0; rf < 2; rf++)
			if (nets[n].timed(rf) && preq(inst, pin, rf) < POS_INF / 2)
				s = std::min(s, preq(inst, pin, rf) - nets[n].arr[rf]);
		return s;
	}

	double capture_time() const { return half_cycle ? 0.5 * opt.period : opt.period; }
	double min_period() const { return opt.period - (half_cycle ? 2 : 1) * wns; }
	// electrical limits (max_capacitance, max_transition)
	double limit_excess(int net, double max_tran) const;
	double default_max_tran() const { return lib->default_max_tran > 0 ? lib->default_max_tran : 1.5; }
	double area() const;
	std::vector<PathNode> trace(int net, int rf) const;
	std::vector<PathNode> worst_path() const;
	void report_path(const std::vector<PathNode> &path) const;
	void report_summary(const char *prefix = "") const;
};

} // namespace Timing

YOSYS_NAMESPACE_END

#endif
