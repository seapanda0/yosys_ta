/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: static timing analysis of mapped netlists
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

#include "passes/timing/timing_sta.h"
#include <cmath>
#include <queue>

YOSYS_NAMESPACE_BEGIN

namespace Timing {

// ---------------------------------------------------------------------------
// TimingConfig
// ---------------------------------------------------------------------------

void TimingConfig::load(RTLIL::Design *design)
{
	liberty_files = split_tokens(design->scratchpad_get_string("timing.liberty", ""), "\n");
	dont_use = split_tokens(design->scratchpad_get_string("timing.dont_use", ""), " ");
	auto num = [&](const char *key, double dflt) { return atof(design->scratchpad_get_string(key, stringf("%.12g", dflt)).c_str()); };
	sta.period = num("timing.period", sta.period);
	sta.input_delay = num("timing.input_delay", sta.input_delay);
	sta.output_delay = num("timing.output_delay", sta.output_delay);
	sta.output_load = num("timing.load", sta.output_load * 1e3) * 1e-3;
	sta.input_slew = num("timing.input_slew", sta.input_slew);
	sta.clock_slew = num("timing.clock_slew", sta.clock_slew);
	sta.wire_cap = num("timing.wire_cap", sta.wire_cap * 1e3) * 1e-3;
}

void TimingConfig::store(RTLIL::Design *design) const
{
	std::string libs, du;
	for (auto &f : liberty_files)
		libs += (libs.empty() ? "" : "\n") + f; // paths may contain spaces
	for (auto &d : dont_use)
		du += (du.empty() ? "" : " ") + d;
	design->scratchpad_set_string("timing.liberty", libs);
	design->scratchpad_set_string("timing.dont_use", du);
	design->scratchpad_set_string("timing.period", stringf("%.12g", sta.period));
	design->scratchpad_set_string("timing.input_delay", stringf("%.12g", sta.input_delay));
	design->scratchpad_set_string("timing.output_delay", stringf("%.12g", sta.output_delay));
	design->scratchpad_set_string("timing.load", stringf("%.12g", sta.output_load * 1e3));
	design->scratchpad_set_string("timing.input_slew", stringf("%.12g", sta.input_slew));
	design->scratchpad_set_string("timing.clock_slew", stringf("%.12g", sta.clock_slew));
	design->scratchpad_set_string("timing.wire_cap", stringf("%.12g", sta.wire_cap * 1e3));
}

static double number(const std::string &arg, const std::string &val, bool may_be_negative)
{
	char *end;
	double v = strtod(val.c_str(), &end);
	// a wrong period would silently turn into "optimize for speed"
	if (val.empty() || *end != 0 || !std::isfinite(v) || (!may_be_negative && v < 0))
		log_cmd_error("Invalid value '%s' for %s, expected a %snumber without a unit.\n", val, arg, may_be_negative ? "" : "non-negative ");
	return v;
}

bool TimingConfig::parse_arg(const std::vector<std::string> &args, size_t &argidx)
{
	if (argidx + 1 >= args.size())
		return false;
	const std::string &arg = args[argidx];
	const std::string &val = args[argidx + 1];
	if (arg == "-liberty") {
		std::string fn = val;
		rewrite_filename(fn);
		if (!fn.empty() && fn[0] != '/') {
			char buf[4096];
			if (getcwd(buf, sizeof(buf)) != nullptr)
				fn = std::string(buf) + "/" + fn;
		}
		// the first -liberty option on a command line replaces the stored defaults
		if (!liberty_from_args)
			liberty_files.clear();
		liberty_from_args = true;
		liberty_files.push_back(fn);
	} else if (arg == "-dont_use") {
		if (!dont_use_from_args)
			dont_use.clear();
		dont_use_from_args = true;
		dont_use.push_back(val);
	} else if (arg == "-period")
		sta.period = number(arg, val, false);
	else if (arg == "-input_delay")
		sta.input_delay = number(arg, val, true);
	else if (arg == "-output_delay")
		sta.output_delay = number(arg, val, true);
	else if (arg == "-load")
		sta.output_load = number(arg, val, false) * 1e-3;
	else if (arg == "-input_slew")
		sta.input_slew = number(arg, val, false);
	else if (arg == "-clock_slew")
		sta.clock_slew = number(arg, val, false);
	else if (arg == "-wire_cap")
		sta.wire_cap = number(arg, val, false) * 1e-3;
	else
		return false;
	argidx++;
	return true;
}

std::shared_ptr<Library> TimingConfig::library() const
{
	if (liberty_files.empty())
		log_cmd_error("No liberty file given. Use -liberty or run 'timing_setup -liberty <file>' first.\n");
	return Library::get(liberty_files, dont_use);
}

std::string TimingConfig::args() const
{
	std::string s;
	for (auto &f : liberty_files)
		s += stringf(" -liberty \"%s\"", f);
	for (auto &d : dont_use)
		s += stringf(" -dont_use %s", d);
	s += stringf(" -period %.12g -input_delay %.12g -output_delay %.12g -load %.12g -input_slew %.12g -clock_slew %.12g -wire_cap %.12g",
			sta.period, sta.input_delay, sta.output_delay, sta.output_load * 1e3, sta.input_slew, sta.clock_slew, sta.wire_cap * 1e3);
	return s;
}

void TimingConfig::help()
{
	log("    -liberty <file>\n");
	log("        liberty file with NLDM timing data (can be given multiple times)\n");
	log("\n");
	log("    -dont_use <pattern>\n");
	log("        cells matching this glob pattern are not used when sizing or\n");
	log("        buffering (can be given multiple times)\n");
	log("\n");
	log("    -period <ns>\n");
	log("        clock period. With a period of 0 (the default) the optimization\n");
	log("        commands minimize the critical path delay.\n");
	log("\n");
	log("    -input_delay <ns>, -output_delay <ns>\n");
	log("        arrival time of primary inputs and external delay at primary\n");
	log("        outputs, relative to the clock (default: 0)\n");
	log("\n");
	log("    -load <fF>\n");
	log("        capacitive load on primary outputs (default: 33.4)\n");
	log("\n");
	log("    -input_slew <ns>, -clock_slew <ns>\n");
	log("        transition time of primary inputs and of the clock (default: 0)\n");
	log("\n");
	log("    -wire_cap <fF>\n");
	log("        wire capacitance estimate per fanout (default: 0)\n");
	log("\n");
	log("Options that are not given default to the values stored with the\n");
	log("'timing_setup' command.\n");
	log("\n");
}

// ---------------------------------------------------------------------------
// Sta
// ---------------------------------------------------------------------------

const Sta::TypeBinding &Sta::bind(IdString type)
{
	auto it = bindings.find(type);
	if (it != bindings.end())
		return it->second;
	TypeBinding b;
	b.lc = lib->find(module->design->twines().unescaped_str(type));
	if (b.lc != nullptr)
		for (int p = 0; p < GetSize(b.lc->pins); p++)
			b.pin_ids.push_back(pin_id(b.lc, p));
	return bindings[type] = b;
}

void Sta::build()
{
	sigmap.set(module);
	insts.clear();
	nets.clear();
	net_of.clear();
	inst_of.clear();
	order.clear();
	n_pins = 0;

	auto get_net = [&](SigBit bit) -> int {
		bit = sigmap(bit);
		if (bit.wire == nullptr)
			return -1;
		auto it = net_of.find(bit);
		if (it != net_of.end())
			return it->second;
		int idx = GetSize(nets);
		net_of[bit] = idx;
		nets.emplace_back();
		nets.back().bit = bit;
		return idx;
	};

	for (auto wire : module->wires()) {
		if (!wire->port_input && !wire->port_output)
			continue;
		for (auto bit : SigSpec(wire)) {
			int n = get_net(bit);
			if (n < 0)
				continue;
			if (wire->port_input)
				nets[n].pi = true;
			if (wire->port_output)
				nets[n].n_po++;
		}
	}

	for (auto cell : module->cells()) {
		const TypeBinding &binding = bind(cell->type);
		const Cell *lc = binding.lc;
		if (lc == nullptr) {
			std::string tname = cell->type.unescape();
			if (!unknown_types.count(tname)) {
				unknown_types.insert(tname);
				if (!quiet)
					log_warning("Cell type %s is not in the timing library, paths through it are not timed.\n", tname);
			}
			continue;
		}
		int idx = GetSize(insts);
		insts.emplace_back();
		Inst &inst = insts.back();
		inst.cell = cell;
		inst.lc = lc;
		inst.pin_base = n_pins;
		inst.pin_net.assign(lc->pins.size(), -1);
		n_pins += GetSize(lc->pins);
		inst_of[cell] = idx;
		for (int p = 0; p < GetSize(lc->pins); p++) {
			const Pin &pin = lc->pins[p];
			if (!cell->hasPort(binding.pin_ids[p]))
				continue;
			const SigSpec &sig = cell->getPort(binding.pin_ids[p]);
			if (GetSize(sig) != 1)
				continue;
			int n = get_net(sig[0]);
			inst.pin_net[p] = n;
			if (n < 0)
				continue;
			if (pin.output) {
				nets[n].drv = idx;
				nets[n].drv_pin = p;
			} else {
				nets[n].sinks.push_back({idx, p});
				if (pin.clock)
					nets[n].clock = true;
			}
		}
	}

	// topological sort over combinational arcs
	std::vector<int> indeg(insts.size(), 0);
	for (int i = 0; i < GetSize(insts); i++) {
		const Inst &inst = insts[i];
		for (int p = 0; p < GetSize(inst.lc->pins); p++)
			if (inst.lc->pins[p].comb_from && inst.pin_net[p] >= 0 && nets[inst.pin_net[p]].drv >= 0)
				indeg[i]++;
	}
	order.reserve(insts.size());
	for (int i = 0; i < GetSize(insts); i++)
		if (indeg[i] == 0)
			order.push_back(i);
	size_t head = 0;
	while (true) {
		for (; head < order.size(); head++) {
			const Inst &inst = insts[order[head]];
			for (int p = 0; p < GetSize(inst.lc->pins); p++) {
				if (!inst.lc->pins[p].output || inst.pin_net[p] < 0)
					continue;
				for (auto &sink : nets[inst.pin_net[p]].sinks)
					if (insts[sink.first].lc->pins[sink.second].comb_from && --indeg[sink.first] == 0)
						order.push_back(sink.first);
			}
		}
		if (order.size() == insts.size())
			break;
		// combinational loop: break it at an arbitrary instance
		for (int i = 0; i < GetSize(insts); i++)
			if (indeg[i] > 0) {
				if (!quiet)
					log_warning("Combinational loop found at cell %s, breaking it for timing analysis.\n", log_id(insts[i].cell));
				indeg[i] = 0;
				order.push_back(i);
				break;
			}
	}
	half_cycle = false;
	int n_clocks = trace_clocks();
	for (auto &inst : insts)
		for (auto arcs : {&inst.lc->arcs, &inst.lc->checks})
			for (auto &arc : *arcs)
				if (arc.type != Arc::COMB && arc.falling_clock && inst.pin_net[arc.from] >= 0)
					half_cycle = true;
	if (half_cycle && !warned_half_cycle) {
		log_warning("Module %s has cells that use the falling clock edge: all paths are constrained to half a clock period.\n", log_id(module));
		warned_half_cycle = true;
	}
	if (!quiet && n_clocks > 1)
		log_warning("Module %s has %d clocks: they are timed as one ideal clock.\n", log_id(module), n_clocks);

	topo_pos.assign(insts.size(), 0);
	for (int k = 0; k < GetSize(order); k++)
		topo_pos[order[k]] = k;
	queued.clear();

	// endpoints: setup checks and primary outputs
	endpoints.clear();
	net_eps.assign(nets.size(), {});
	for (int i = 0; i < GetSize(insts); i++) {
		const Inst &inst = insts[i];
		int last_pin = -1;
		for (auto &chk : inst.lc->checks) {
			int nd = inst.pin_net[chk.to];
			if (nd < 0 || inst.pin_net[chk.from] < 0 || chk.to == last_pin || nets[nd].clock)
				continue;
			last_pin = chk.to;
			net_eps[nd].push_back(GetSize(endpoints));
			endpoints.push_back({nd, i, chk.to, POS_INF, 0});
		}
	}
	for (int n = 0; n < GetSize(nets); n++)
		if (nets[n].n_po > 0) {
			net_eps[n].push_back(GetSize(endpoints));
			endpoints.push_back({n, -1, -1, POS_INF, 0});
		}
}

void Sta::compute_load(int n)
{
	Net &net = nets[n];
	for (int rf = 0; rf < 2; rf++) {
		net.load[rf] = net.n_po * opt.output_load + opt.wire_cap * (GetSize(net.sinks) + net.n_po);
		for (auto &s : net.sinks)
			net.load[rf] += insts[s.first].lc->pins[s.second].cap[rf];
	}
}

// Arrival times and slews at the outputs of an instance. The output nets
// must have been reset before.
void Sta::eval_inst(int i)
{
	const Inst &inst = insts[i];
	for (auto &arc : inst.lc->arcs) {
		int no = inst.pin_net[arc.to];
		int ni = inst.pin_net[arc.from];
		if (no < 0 || ni < 0)
			continue;
		Net &out = nets[no];
		if (arc.type == Arc::CLK) {
			for (int rf = 0; rf < 2; rf++) {
				if (!arc.delay[rf].valid())
					continue;
				double d = arc.delay[rf].lookup(opt.clock_slew, out.load[rf]);
				double s = arc.trans[rf].valid() ? arc.trans[rf].lookup(opt.clock_slew, out.load[rf]) : 0;
				if (d > out.arr[rf]) {
					out.arr[rf] = d;
					out.pred_pin[rf] = -1;
				}
				out.slew[rf] = std::max(out.slew[rf], s);
			}
			continue;
		}
		const Net &in = nets[ni];
		for (int orf = 0; orf < 2; orf++) {
			if (!arc.delay[orf].valid())
				continue;
			for (int irf = 0; irf < 2; irf++) {
				if (arc.sense == Arc::POS && irf != orf)
					continue;
				if (arc.sense == Arc::NEG && irf == orf)
					continue;
				if (!in.timed(irf))
					continue;
				double d = arc.delay[orf].lookup(in.slew[irf], out.load[orf]);
				double s = arc.trans[orf].valid() ? arc.trans[orf].lookup(in.slew[irf], out.load[orf]) : 0;
				double a = in.arr[irf] + d;
				if (a > out.arr[orf]) {
					out.arr[orf] = a;
					out.pred_pin[orf] = arc.from;
					out.pred_rf[orf] = irf;
				}
				out.slew[orf] = std::max(out.slew[orf], s);
			}
		}
	}
}

// Required times at the input pins of an instance, from its setup checks and
// from the required times of its output nets.
void Sta::eval_inst_req(int i)
{
	const Inst &inst = insts[i];
	for (int p = 0; p < GetSize(inst.lc->pins); p++)
		if (inst.lc->pins[p].input)
			preq(i, p, RISE) = preq(i, p, FALL) = POS_INF;
	for (auto &chk : inst.lc->checks) {
		int nd = inst.pin_net[chk.to];
		if (nd < 0 || inst.pin_net[chk.from] < 0 || nets[nd].clock)
			continue;
		const Net &dn = nets[nd];
		for (int rf = 0; rf < 2; rf++) {
			if (!chk.constraint[rf].valid() || !dn.timed(rf))
				continue;
			double r = capture_time() - chk.constraint[rf].lookup(opt.clock_slew, 0, dn.slew[rf]);
			double &pr = preq(i, chk.to, rf);
			pr = std::min(pr, r);
		}
	}
	for (auto &arc : inst.lc->arcs) {
		if (arc.type != Arc::COMB)
			continue;
		int no = inst.pin_net[arc.to];
		int ni = inst.pin_net[arc.from];
		if (no < 0 || ni < 0)
			continue;
		const Net &out = nets[no], &in = nets[ni];
		for (int orf = 0; orf < 2; orf++) {
			if (!arc.delay[orf].valid() || out.req[orf] > POS_INF / 2)
				continue;
			for (int irf = 0; irf < 2; irf++) {
				if (arc.sense == Arc::POS && irf != orf)
					continue;
				if (arc.sense == Arc::NEG && irf == orf)
					continue;
				if (!in.timed(irf))
					continue;
				double r = out.req[orf] - arc.delay[orf].lookup(in.slew[irf], out.load[orf]);
				double &pr = preq(i, arc.from, irf);
				pr = std::min(pr, r);
			}
		}
	}
}

// Required time of a net from its sink pins and primary outputs.
void Sta::eval_net_req(int n)
{
	Net &net = nets[n];
	for (int rf = 0; rf < 2; rf++) {
		double r = POS_INF;
		if (net.n_po > 0 && net.timed(rf))
			r = capture_time() - opt.output_delay;
		for (auto &s : net.sinks)
			r = std::min(r, preq(s.first, s.second, rf));
		net.req[rf] = r;
	}
}

void Sta::eval_endpoint(int e)
{
	Endpoint &ep = endpoints[e];
	const Net &net = nets[ep.net];
	ep.slack = POS_INF;
	ep.rf = 0;
	if (ep.inst < 0) {
		for (int rf = 0; rf < 2; rf++)
			if (net.timed(rf) && capture_time() - opt.output_delay - net.arr[rf] < ep.slack) {
				ep.slack = capture_time() - opt.output_delay - net.arr[rf];
				ep.rf = rf;
			}
		return;
	}
	if (net.clock)
		return;
	const Inst &inst = insts[ep.inst];
	for (auto &chk : inst.lc->checks) {
		if (chk.to != ep.pin || inst.pin_net[chk.from] < 0)
			continue;
		for (int rf = 0; rf < 2; rf++) {
			if (!chk.constraint[rf].valid() || !net.timed(rf))
				continue;
			double r = capture_time() - chk.constraint[rf].lookup(opt.clock_slew, 0, net.slew[rf]);
			if (r - net.arr[rf] < ep.slack) {
				ep.slack = r - net.arr[rf];
				ep.rf = rf;
			}
		}
	}
}

void Sta::summarize()
{
	wns = POS_INF;
	tns = 0;
	worst_endpoint = -1;
	for (int e = 0; e < GetSize(endpoints); e++) {
		if (endpoints[e].slack < wns) {
			wns = endpoints[e].slack;
			worst_endpoint = e;
		}
		if (endpoints[e].slack < 0)
			tns += endpoints[e].slack;
	}
	if (worst_endpoint < 0)
		wns = capture_time();
}

// Follows the clock nets back to their sources and marks the clock network.
// A clock that may be inverted (inverter, logic that is not known to preserve
// the polarity) counts as a falling edge clock: dfflibmap maps falling edge
// flip-flops to rising edge cells behind an inverter. Returns the number of
// clock sources.
int Sta::trace_clocks()
{
	std::vector<char> visited(nets.size(), 0);
	std::vector<int> todo;
	int n_sources = 0;
	for (int n = 0; n < GetSize(nets); n++)
		if (nets[n].clock)
			todo.push_back(n);
	while (!todo.empty()) {
		int n = todo.back();
		todo.pop_back();
		if (visited[n])
			continue;
		visited[n] = 1;
		nets[n].clock = true;
		int d = nets[n].drv;
		if (d < 0 || insts[d].lc->seq) {
			// a clock gate passes the clock of its clock pin through
			int from = -1;
			if (d >= 0)
				for (auto &arc : insts[d].lc->arcs)
					if (arc.type == Arc::COMB && arc.to == nets[n].drv_pin && insts[d].lc->pins[arc.from].clock) {
						from = insts[d].pin_net[arc.from];
						if (arc.sense != Arc::POS)
							half_cycle = true;
					}
			if (from >= 0)
				todo.push_back(from);
			else
				n_sources++;
			continue;
		}
		const Inst &inst = insts[d];
		pool<int> inputs;
		bool positive = true;
		for (auto &arc : inst.lc->arcs) {
			if (arc.type != Arc::COMB || arc.to != nets[n].drv_pin || inst.pin_net[arc.from] < 0)
				continue;
			inputs.insert(inst.pin_net[arc.from]);
			if (arc.sense != Arc::POS)
				positive = false;
		}
		if (!positive)
			half_cycle = true;
		if (GetSize(inputs) == 1)
			todo.push_back(*inputs.begin());
		else {
			// gated or multiplexed: the edge of the data inputs is unknown
			n_sources++;
			half_cycle = true;
		}
	}
	return n_sources;
}

void Sta::propagate()
{
	for (int n = 0; n < GetSize(nets); n++) {
		Net &net = nets[n];
		compute_load(n);
		for (int rf = 0; rf < 2; rf++) {
			net.arr[rf] = NEG_INF;
			net.slew[rf] = 0;
			net.req[rf] = POS_INF;
			net.pred_pin[rf] = -1;
			net.pred_rf[rf] = rf;
			if (net.pi && !net.clock && net.drv < 0) {
				net.arr[rf] = opt.input_delay;
				net.slew[rf] = opt.input_slew;
			}
		}
	}
	pin_req.assign(n_pins * 2, POS_INF);

	// forward propagation of arrival times and slews
	for (int i : order)
		eval_inst(i);

	// backward propagation of required times
	for (int i = 0; i < GetSize(insts); i++)
		if (!insts[i].lc->checks.empty())
			eval_inst_req(i);
	for (int k = GetSize(order) - 1; k >= 0; k--) {
		int i = order[k];
		const Inst &inst = insts[i];
		for (int p = 0; p < GetSize(inst.lc->pins); p++)
			if (inst.lc->pins[p].output && inst.pin_net[p] >= 0)
				eval_net_req(inst.pin_net[p]);
		eval_inst_req(i);
	}
	for (int n = 0; n < GetSize(nets); n++)
		if (nets[n].drv < 0)
			eval_net_req(n);

	for (int e = 0; e < GetSize(endpoints); e++)
		eval_endpoint(e);
	summarize();
}

// Incremental timing update after the library cell of the given instances
// has been changed (same pins, different electrical characteristics).
void Sta::update(const std::vector<int> &changed)
{
	typedef std::pair<int, int> QE; // (topological position, instance)
	std::priority_queue<QE, std::vector<QE>, std::greater<QE>> fwd;
	std::priority_queue<QE> bwd;
	if (GetSize(queued) != GetSize(insts))
		queued.assign(insts.size(), 0);
	std::vector<int> touched, changed_nets;

	auto push_fwd = [&](int i) {
		if (!(queued[i] & 1)) {
			queued[i] |= 1;
			fwd.push({topo_pos[i], i});
		}
	};
	// instances without combinational arcs (flip-flops) only contribute their
	// setup checks, so they are handled first in the backward pass
	auto push_bwd = [&](int i) {
		if (!(queued[i] & 2)) {
			queued[i] |= 2;
			bwd.push({insts[i].lc->seq ? GetSize(insts) : topo_pos[i], i});
		}
	};

	for (int i : changed) {
		const Inst &inst = insts[i];
		push_fwd(i);
		for (int p = 0; p < GetSize(inst.lc->pins); p++) {
			int n = inst.pin_net[p];
			if (n < 0)
				continue;
			compute_load(n);
			changed_nets.push_back(n);
			if (nets[n].drv >= 0)
				push_fwd(nets[n].drv);
		}
	}

	while (!fwd.empty()) {
		int i = fwd.top().second;
		fwd.pop();
		touched.push_back(i);
		const Inst &inst = insts[i];
		for (int p = 0; p < GetSize(inst.lc->pins); p++) {
			int n = inst.pin_net[p];
			if (!inst.lc->pins[p].output || n < 0)
				continue;
			Net &net = nets[n];
			for (int rf = 0; rf < 2; rf++) {
				net.old_arr[rf] = net.arr[rf];
				net.old_slew[rf] = net.slew[rf];
				net.arr[rf] = NEG_INF;
				net.slew[rf] = 0;
				net.pred_pin[rf] = -1;
				net.pred_rf[rf] = rf;
			}
		}
		eval_inst(i);
		for (int p = 0; p < GetSize(inst.lc->pins); p++) {
			int n = inst.pin_net[p];
			if (!inst.lc->pins[p].output || n < 0)
				continue;
			Net &net = nets[n];
			bool diff = false;
			for (int rf = 0; rf < 2; rf++)
				if (fabs(net.arr[rf] - net.old_arr[rf]) > 1e-9 || fabs(net.slew[rf] - net.old_slew[rf]) > 1e-9)
					diff = true;
			if (!diff)
				continue;
			changed_nets.push_back(n);
			for (auto &s : net.sinks) {
				if (insts[s.first].lc->pins[s.second].comb_from)
					push_fwd(s.first);
				else
					push_bwd(s.first);
			}
		}
	}

	for (int i : touched)
		push_bwd(i);
	while (!bwd.empty()) {
		int i = bwd.top().second;
		bwd.pop();
		const Inst &inst = insts[i];
		eval_inst_req(i);
		for (int p = 0; p < GetSize(inst.lc->pins); p++) {
			int n = inst.pin_net[p];
			if (!inst.lc->pins[p].input || n < 0)
				continue;
			Net &net = nets[n];
			double old_req[2] = {net.req[0], net.req[1]};
			eval_net_req(n);
			if (net.drv >= 0 && (fabs(net.req[0] - old_req[0]) > 1e-9 || fabs(net.req[1] - old_req[1]) > 1e-9))
				push_bwd(net.drv);
		}
		queued[i] = 0;
	}
	for (int i : touched)
		queued[i] = 0;

	for (int n : changed_nets)
		for (int e : net_eps[n])
			eval_endpoint(e);
	summarize();
}

double Sta::limit_excess(int n, double max_tran) const
{
	const Net &net = nets[n];
	if (net.drv < 0 || net.clock)
		return 0;
	const Pin &out = insts[net.drv].lc->pins[net.drv_pin];
	double excess = 0;
	if (out.max_cap > 0)
		excess += std::max(0.0, std::max(net.load[RISE], net.load[FALL]) / out.max_cap - 1);
	if (!net.timed(RISE) && !net.timed(FALL))
		return excess;
	double limit = max_tran;
	if (out.max_tran > 0)
		limit = std::min(limit, out.max_tran);
	for (auto &sink : net.sinks) {
		double pin_limit = insts[sink.first].lc->pins[sink.second].max_tran;
		if (pin_limit > 0)
			limit = std::min(limit, pin_limit);
	}
	return excess + std::max(0.0, std::max(net.slew[RISE], net.slew[FALL]) / limit - 1);
}

double Sta::area() const
{
	double a = 0;
	for (auto &inst : insts)
		a += inst.lc->area;
	return a;
}

std::vector<Sta::PathNode> Sta::trace(int net, int rf) const
{
	std::vector<PathNode> path;
	int guard = GetSize(nets) + 1;
	while (net >= 0 && guard-- > 0) {
		const Net &n = nets[net];
		PathNode node = {net, rf, n.drv, n.pred_pin[rf]};
		path.push_back(node);
		if (n.drv < 0 || n.pred_pin[rf] < 0)
			break;
		int next_rf = n.pred_rf[rf];
		net = insts[n.drv].pin_net[n.pred_pin[rf]];
		rf = next_rf;
	}
	std::reverse(path.begin(), path.end());
	return path;
}

std::vector<Sta::PathNode> Sta::worst_path() const
{
	if (worst_endpoint < 0)
		return {};
	return trace(endpoints[worst_endpoint].net, endpoints[worst_endpoint].rf);
}

void Sta::report_path(const std::vector<PathNode> &path) const
{
	log("  %9s %9s %8s %8s  %s\n", "arrival", "delay", "slew", "load fF", "driver (cell type) / net");
	double prev = 0;
	for (auto &node : path) {
		const Net &n = nets[node.net];
		std::string drv = "primary input";
		if (node.inst >= 0)
			drv = stringf("%s (%s)", log_id(insts[node.inst].cell), insts[node.inst].lc->name);
		double a = n.arr[node.rf];
		log("  %9.4f %9.4f %8.4f %8.2f  %c %s -> %s\n", a, a - prev, n.slew[node.rf], n.load[node.rf] * 1e3,
				node.rf == RISE ? '^' : 'v', drv, log_signal(n.bit));
		prev = a;
	}
}

void Sta::report_summary(const char *prefix) const
{
	double mp = min_period();
	log("%speriod %.4f ns, wns %.4f ns, tns %.3f ns, min period %.4f ns (%.2f MHz), area %.2f, %d cells, %d endpoints\n",
			prefix, opt.period, wns, tns, mp, mp > 0 ? 1e3 / mp : 0.0, area(), GetSize(insts), GetSize(endpoints));
}

} // namespace Timing

YOSYS_NAMESPACE_END
