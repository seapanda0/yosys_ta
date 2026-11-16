/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: timing driven gate sizing and buffering
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

#include "passes/timing/timing_opt.h"
#include <chrono>
#include <cmath>

YOSYS_NAMESPACE_BEGIN

namespace Timing {

namespace {

// worst delay and output slew over all combinational arcs from->to for the
// given input/output transition pair
bool eval_comb(const Cell *lc, int from, int to, int irf, int orf, double slew, double load, double &d, double &s)
{
	bool found = false;
	d = s = 0;
	for (auto &arc : lc->arcs) {
		if (arc.type != Arc::COMB || arc.from != from || arc.to != to || !arc.delay[orf].valid())
			continue;
		if (arc.sense == Arc::POS && irf != orf)
			continue;
		if (arc.sense == Arc::NEG && irf == orf)
			continue;
		double dd = arc.delay[orf].lookup(slew, load);
		double ss = arc.trans[orf].valid() ? arc.trans[orf].lookup(slew, load) : 0;
		d = found ? std::max(d, dd) : dd;
		s = found ? std::max(s, ss) : ss;
		found = true;
	}
	return found;
}

bool eval_clk(const Cell *lc, int to, int orf, double clock_slew, double load, double &d, double &s)
{
	bool found = false;
	d = s = 0;
	for (auto &arc : lc->arcs) {
		if (arc.type != Arc::CLK || arc.to != to || !arc.delay[orf].valid())
			continue;
		double dd = arc.delay[orf].lookup(clock_slew, load);
		double ss = arc.trans[orf].valid() ? arc.trans[orf].lookup(clock_slew, load) : 0;
		d = found ? std::max(d, dd) : dd;
		s = found ? std::max(s, ss) : ss;
		found = true;
	}
	return found;
}

struct Move {
	enum Kind { RESIZE, BUFFER } kind = RESIZE;
	double gain = 0;
	// RESIZE
	RTLIL::Cell *cell = nullptr;
	const Cell *old_lc = nullptr, *new_lc = nullptr;
	// BUFFER: the sinks of 'src' in each group are moved behind a buffer of their own
	typedef std::vector<std::pair<RTLIL::Cell *, IdString>> SinkList;
	SigBit src;
	const Cell *buf = nullptr;
	std::vector<SinkList> groups;
	std::vector<RTLIL::Cell *> buf_cells;
	// BUFFER with decoupled primary output: the driver moves to the new net
	// as well, the buffer drives the port (single group)
	RTLIL::Cell *drv_cell = nullptr;
	IdString drv_port;

	int n_sinks() const
	{
		int n = 0;
		for (auto &g : groups)
			n += GetSize(g);
		return n;
	}

	std::string describe() const
	{
		if (kind == RESIZE)
			return stringf("resize %s %s -> %s", log_id(cell), old_lc->name, new_lc->name);
		if (drv_cell != nullptr)
			return stringf("decouple output %s with %s (%d sinks)", log_signal(src), buf->name, n_sinks());
		return stringf("buffer %s with %d x %s (%d sinks)", log_signal(src), GetSize(groups), buf->name, n_sinks());
	}
};

struct Sizer {
	typedef Sta::PathNode PathNode;
	typedef Sta::Net Net;
	typedef Sta::Inst Inst;

	RTLIL::Module *module;
	const Library *lib;
	SizerOptions opt;
	Sta sta;
	SizerResult res;
	double max_tran;
	std::set<std::pair<RTLIL::Cell *, const Cell *>> rejected_resize;
	std::set<std::pair<SigBit, const Cell *>> rejected_buffer;
	std::set<std::pair<SigBit, const Cell *>> rejected_decouple;
	bool ignore_rejected = false;
	double best_wns = -1e30;

	Sizer(RTLIL::Module *module, const Library *lib, const StaOptions &sta_opt, const SizerOptions &opt)
		: module(module), lib(lib), opt(opt), sta(module, lib, sta_opt)
	{
		max_tran = opt.max_tran > 0 ? opt.max_tran : (lib->default_max_tran > 0 ? lib->default_max_tran : 1.5);
	}

	// ---- applying and undoing moves ----

	void update(const std::vector<int> &changed)
	{
		sta.update(changed);
		if (!opt.selfcheck)
			return;
		std::vector<Net> nets = sta.nets;
		double wns = sta.wns, tns = sta.tns;
		sta.propagate();
		if (fabs(wns - sta.wns) > 1e-6 || fabs(tns - sta.tns) > 1e-5)
			log_error("Incremental timing update mismatch: wns %.6f vs %.6f, tns %.6f vs %.6f\n", wns, sta.wns, tns, sta.tns);
		for (int n = 0; n < GetSize(nets); n++)
			for (int rf = 0; rf < 2; rf++) {
				if (fabs(nets[n].arr[rf] - sta.nets[n].arr[rf]) > 1e-6 || fabs(nets[n].slew[rf] - sta.nets[n].slew[rf]) > 1e-6)
					log_error("Incremental timing update mismatch: arrival of net %s: %.6f vs %.6f\n", log_signal(nets[n].bit), nets[n].arr[rf], sta.nets[n].arr[rf]);
				if (nets[n].timed(rf) && fabs(nets[n].req[rf] - sta.nets[n].req[rf]) > 1e-6)
					log_error("Incremental timing update mismatch: required time of net %s: %.6f vs %.6f\n", log_signal(nets[n].bit), nets[n].req[rf], sta.nets[n].req[rf]);
			}
	}

	// netlist edit of a BUFFER move
	void insert_buffers(Move &m)
	{
		m.buf_cells.clear();
		for (auto &group : m.groups) {
			RTLIL::Wire *wire = module->addWire(NEW_ID_SUFFIX("tbuf"));
			RTLIL::Cell *cell = module->addCell(NEW_ID_SUFFIX("tbuf"), sta.type_id(m.buf));
			if (m.drv_cell != nullptr) {
				m.drv_cell->setPort(m.drv_port, wire);
				cell->setPort(sta.pin_id(m.buf, m.buf->buf_in), wire);
				cell->setPort(sta.pin_id(m.buf, m.buf->buf_out), m.src);
			} else {
				cell->setPort(sta.pin_id(m.buf, m.buf->buf_in), m.src);
				cell->setPort(sta.pin_id(m.buf, m.buf->buf_out), wire);
			}
			for (auto &it : group)
				it.first->setPort(it.second, wire);
			m.buf_cells.push_back(cell);
		}
	}

	void apply(Move &m)
	{
		if (m.kind == Move::RESIZE) {
			int i = sta.inst_of.at(m.cell);
			m.cell->type = sta.type_id(m.new_lc);
			sta.insts[i].lc = m.new_lc;
			update({i});
		} else {
			insert_buffers(m);
			sta.build();
			sta.propagate();
		}
	}

	void undo(Move &m)
	{
		if (m.kind == Move::RESIZE) {
			int i = sta.inst_of.at(m.cell);
			m.cell->type = sta.type_id(m.old_lc);
			sta.insts[i].lc = m.old_lc;
			update({i});
		} else {
			// the unused wires are left behind for opt_clean
			for (auto cell : m.buf_cells)
				module->remove(cell);
			m.buf_cells.clear();
			for (auto &group : m.groups)
				for (auto &it : group)
					it.first->setPort(it.second, m.src);
			if (m.drv_cell != nullptr)
				m.drv_cell->setPort(m.drv_port, m.src);
			sta.build();
			sta.propagate();
		}
	}

	// resize several cells at once (forward: old -> new, otherwise new -> old)
	void resize_batch(std::vector<Move> &moves, bool forward)
	{
		std::vector<int> changed;
		for (auto &m : moves) {
			int i = sta.inst_of.at(m.cell);
			const Cell *lc = forward ? m.new_lc : m.old_lc;
			m.cell->type = sta.type_id(lc);
			sta.insts[i].lc = lc;
			changed.push_back(i);
		}
		if (!changed.empty())
			update(changed);
	}

	// ---- local timing estimates ----

	// Path arrival time and slew at path[k].net if the load of that net
	// changes by dload.
	bool eval_node(const std::vector<PathNode> &path, int k, double dload, double &arr, double &slew)
	{
		const PathNode &pn = path[k];
		const Net &out = sta.nets[pn.net];
		arr = out.arr[pn.rf];
		slew = out.slew[pn.rf];
		if (pn.inst < 0)
			return dload == 0;
		const Cell *lc = sta.insts[pn.inst].lc;
		double d0, s0, d1, s1;
		if (pn.in_pin < 0) {
			if (!eval_clk(lc, out.drv_pin, pn.rf, sta.opt.clock_slew, out.load[pn.rf], d0, s0))
				return false;
			eval_clk(lc, out.drv_pin, pn.rf, sta.opt.clock_slew, out.load[pn.rf] + dload, d1, s1);
		} else {
			if (k == 0)
				return false;
			const Net &in = sta.nets[path[k - 1].net];
			int irf = path[k - 1].rf;
			if (!eval_comb(lc, pn.in_pin, out.drv_pin, irf, pn.rf, in.slew[irf], out.load[pn.rf], d0, s0))
				return false;
			eval_comb(lc, pn.in_pin, out.drv_pin, irf, pn.rf, in.slew[irf], out.load[pn.rf] + dload, d1, s1);
		}
		arr += d1 - d0;
		slew = std::max(0.0, slew + s1 - s0);
		return true;
	}

	// Change of the delay of the path stage after path[k] if the slew at
	// path[k].net changes to new_slew.
	double next_stage_penalty(const std::vector<PathNode> &path, int k, double new_slew)
	{
		if (k + 1 >= GetSize(path))
			return 0;
		const PathNode &pn = path[k], &nx = path[k + 1];
		const Net &out = sta.nets[pn.net], &nout = sta.nets[nx.net];
		double d0, s0, d1, s1;
		if (nx.inst < 0 || nx.in_pin < 0)
			return 0;
		const Cell *lc = sta.insts[nx.inst].lc;
		if (!eval_comb(lc, nx.in_pin, nout.drv_pin, pn.rf, nx.rf, out.slew[pn.rf], nout.load[nx.rf], d0, s0))
			return 0;
		eval_comb(lc, nx.in_pin, nout.drv_pin, pn.rf, nx.rf, new_slew, nout.load[nx.rf], d1, s1);
		return d1 - d0;
	}

	// Estimated reduction of the path delay if the driver of path[k].net is
	// replaced by 'alt'.
	double eval_resize(const std::vector<PathNode> &path, int k, const Cell *alt)
	{
		const PathNode &pn = path[k];
		const Cell *cur = sta.insts[pn.inst].lc;
		const Net &out = sta.nets[pn.net];
		int orf = pn.rf, opin = out.drv_pin;
		double arr_new, slew_new, d, s;
		if (pn.in_pin < 0) {
			if (!eval_clk(alt, opin, orf, sta.opt.clock_slew, out.load[orf], d, s))
				return -1;
			arr_new = d;
			slew_new = s;
		} else {
			if (k == 0)
				return -1;
			int irf = path[k - 1].rf;
			double dcap = alt->pins[pn.in_pin].cap[irf] - cur->pins[pn.in_pin].cap[irf];
			double arr_in, slew_in;
			if (!eval_node(path, k - 1, dcap, arr_in, slew_in))
				return -1;
			if (!eval_comb(alt, pn.in_pin, opin, irf, orf, slew_in, out.load[orf], d, s))
				return -1;
			arr_new = arr_in + d;
			slew_new = s;
		}
		return out.arr[orf] - arr_new - next_stage_penalty(path, k, slew_new);
	}

	// largest increase of the delay through an instance if it is replaced by 'alt'
	double delay_increase(int i, const Cell *alt)
	{
		const Inst &inst = sta.insts[i];
		double worst = 0;
		for (size_t a = 0; a < inst.lc->arcs.size() && a < alt->arcs.size(); a++) {
			const Arc &arc = inst.lc->arcs[a], &narc = alt->arcs[a];
			if (arc.from != narc.from || arc.to != narc.to || arc.type != narc.type)
				return 1e9;
			int no = inst.pin_net[arc.to], ni = inst.pin_net[arc.from];
			if (no < 0 || ni < 0)
				continue;
			const Net &out = sta.nets[no], &in = sta.nets[ni];
			for (int orf = 0; orf < 2; orf++) {
				if (!arc.delay[orf].valid() || !narc.delay[orf].valid())
					continue;
				for (int irf = 0; irf < 2; irf++) {
					double slew = arc.type == Arc::CLK ? sta.opt.clock_slew : in.slew[irf];
					double dd = narc.delay[orf].lookup(slew, out.load[orf]) - arc.delay[orf].lookup(slew, out.load[orf]);
					if (arc.trans[orf].valid() && narc.trans[orf].valid())
						dd += 0.5 * std::max(0.0, narc.trans[orf].lookup(slew, out.load[orf]) - arc.trans[orf].lookup(slew, out.load[orf]));
					worst = std::max(worst, dd);
				}
			}
		}
		return worst;
	}

	double inst_out_slack(int i)
	{
		const Inst &inst = sta.insts[i];
		double s = Sta::POS_INF;
		for (int p = 0; p < GetSize(inst.lc->pins); p++)
			if (inst.lc->pins[p].output && inst.pin_net[p] >= 0)
				s = std::min(s, sta.nets[inst.pin_net[p]].slack());
		return s;
	}

	// worst slack over all pins of an instance
	double inst_slack(int i)
	{
		const Inst &inst = sta.insts[i];
		double s = inst_out_slack(i);
		for (int p = 0; p < GetSize(inst.lc->pins); p++)
			if (inst.lc->pins[p].input && inst.pin_net[p] >= 0)
				s = std::min(s, sta.pin_slack(i, p));
		return s;
	}

	double near_critical_cost(double thr)
	{
		double c = 0;
		for (auto &n : sta.nets) {
			double s = n.slack();
			if (s < thr)
				c += thr - s;
		}
		return c;
	}

	// ---- candidate generation ----

	void collect_moves(const std::vector<PathNode> &path, std::vector<Move> &moves)
	{
		const double min_gain = 2e-4;
		auto &we = sta.endpoints[sta.worst_endpoint];

		for (int k = 0; k < GetSize(path); k++) {
			const PathNode &pn = path[k];
			const Net &net = sta.nets[pn.net];

			// resize the driver of this path node
			if (opt.resize && pn.inst >= 0) {
				const Inst &inst = sta.insts[pn.inst];
				if (inst.lc->family >= 0)
					for (auto alt : lib->families[inst.lc->family]) {
						if (alt == inst.lc || alt->dont_use || (!ignore_rejected && rejected_resize.count({inst.cell, alt})))
							continue;
						double g = eval_resize(path, k, alt);
						if (g > min_gain) {
							Move m;
							m.kind = Move::RESIZE;
							m.gain = g;
							m.cell = inst.cell;
							m.old_lc = inst.lc;
							m.new_lc = alt;
							moves.push_back(m);
						}
					}
			}

			if (pn.inst < 0 || net.clock)
				continue;

			// the sink the critical path continues through
			int next_inst = -1, next_pin = -1;
			if (k + 1 < GetSize(path))
				next_inst = path[k + 1].inst, next_pin = path[k + 1].in_pin;
			else
				next_inst = we.inst, next_pin = we.pin;

			// downsize side loads
			if (opt.resize && GetSize(net.sinks) > 1) {
				for (auto &sink : net.sinks) {
					if (sink.first == next_inst)
						continue;
					const Inst &si = sta.insts[sink.first];
					if (si.lc->family < 0 || si.lc->size_idx == 0)
						continue;
					const Cell *alt = lib->families[si.lc->family][si.lc->size_idx - 1];
					if (alt->dont_use || (!ignore_rejected && rejected_resize.count({si.cell, alt})))
						continue;
					double dcap = alt->pins[sink.second].cap[pn.rf] - si.lc->pins[sink.second].cap[pn.rf];
					if (dcap >= 0)
						continue;
					double arr, slew;
					if (!eval_node(path, k, dcap, arr, slew))
						continue;
					double g = net.arr[pn.rf] - arr - next_stage_penalty(path, k, slew);
					if (g < min_gain)
						continue;
					if (inst_out_slack(sink.first) - 1.5 * delay_increase(sink.first, alt) < sta.wns + 2 * g + 0.01)
						continue;
					Move m;
					m.kind = Move::RESIZE;
					m.gain = g;
					m.cell = si.cell;
					m.old_lc = si.lc;
					m.new_lc = alt;
					moves.push_back(m);
				}
			}

			if (!opt.buffer || lib->buffers.empty())
				continue;

			// buffer moves
			int rf = pn.rf;
			for (auto buf : lib->buffers) {
				double cin = buf->pins[buf->buf_in].cap[rf];

				// (3) decouple the load of a primary output: the driver and the
				// internal sinks move to a new net, the buffer drives the port
				if (net.n_po > 0 && !(k + 1 == GetSize(path) && we.inst < 0) &&
						(ignore_rejected || !rejected_decouple.count({net.bit, buf}))) {
					double po_load = net.n_po * (sta.opt.output_load + sta.opt.wire_cap);
					double arr, slew, bd, bs;
					if (po_load > 1.5 * cin && eval_node(path, k, cin + sta.opt.wire_cap - po_load, arr, slew) &&
							eval_comb(buf, buf->buf_in, buf->buf_out, rf, rf, slew, po_load, bd, bs) && bs <= max_tran) {
						double g = net.arr[rf] - arr - next_stage_penalty(path, k, slew);
						double po_slack = sta.opt.period - sta.opt.output_delay - (arr + bd);
						if (g > min_gain && po_slack > sta.wns + g + 0.01) {
							Move m;
							m.kind = Move::BUFFER;
							m.gain = g;
							m.src = net.bit;
							m.buf = buf;
							m.drv_cell = sta.insts[pn.inst].cell;
							m.drv_port = sta.pin_id(sta.insts[pn.inst].lc, net.drv_pin);
							m.groups.emplace_back();
							for (auto &sink : net.sinks)
								m.groups.back().push_back({sta.insts[sink.first].cell, sta.pin_id(sta.insts[sink.first].lc, sink.second)});
							moves.push_back(m);
						}
					}
				}

				if (!ignore_rejected && rejected_buffer.count({net.bit, buf}))
					continue;

				if (net.sinks.empty())
					continue;

				// (1) isolate non-critical sinks behind a buffer
				if (GetSize(net.sinks) > 1) {
					std::vector<std::pair<double, int>> cand; // (slack, sink index)
					for (int si = 0; si < GetSize(net.sinks); si++) {
						auto &sink = net.sinks[si];
						if (sink.first == next_inst && sink.second == next_pin)
							continue;
						double sl = sta.pin_slack(sink.first, sink.second);
						if (sl > sta.wns + 0.03)
							cand.push_back({sl, si});
					}
					std::sort(cand.begin(), cand.end());
					// Drop the most critical candidates until the remaining ones fit
					// the buffer and tolerate its delay. (A net with a very large
					// fanout is unloaded step by step this way.)
					int first = 0;
					double g = 0;
					double cap_limit = buf->pins[buf->buf_out].max_cap > 0 ? 0.9 * buf->pins[buf->buf_out].max_cap : 1e9;
					for (int round = 0; round < 4 && first < GetSize(cand); round++) {
						double cmoved = 0;
						for (int c = GetSize(cand) - 1; c >= first; c--) {
							auto &sink = net.sinks[cand[c].second];
							double pc = sta.insts[sink.first].lc->pins[sink.second].cap[rf] + sta.opt.wire_cap;
							if (cmoved + pc > cap_limit) {
								first = c + 1;
								break;
							}
							cmoved += pc;
						}
						double arr, slew, bd, bs;
						g = 0;
						if (first >= GetSize(cand) || cmoved < 1.2 * cin)
							{ first = GetSize(cand); break; }
						if (!eval_node(path, k, cin + sta.opt.wire_cap - cmoved, arr, slew))
							{ first = GetSize(cand); break; }
						if (!eval_comb(buf, buf->buf_in, buf->buf_out, rf, rf, slew, cmoved, bd, bs) || bs > max_tran)
							{ first = GetSize(cand); break; }
						g = net.arr[rf] - arr - next_stage_penalty(path, k, slew);
						double penalty = arr + bd + 0.5 * bs - net.arr[rf];
						int nf = first;
						while (nf < GetSize(cand) && cand[nf].first - penalty < sta.wns + g + 0.01)
							nf++;
						if (nf == first)
							break;
						first = nf;
						g = 0;
					}
					if (g > min_gain && first < GetSize(cand)) {
						Move m;
						m.kind = Move::BUFFER;
						m.gain = g;
						m.src = net.bit;
						m.buf = buf;
						m.groups.emplace_back();
						for (int c = first; c < GetSize(cand); c++) {
							auto &sink = net.sinks[cand[c].second];
							m.groups.back().push_back({sta.insts[sink.first].cell, sta.pin_id(sta.insts[sink.first].lc, sink.second)});
						}
						moves.push_back(m);
					}
				}

				// (2) drive all sinks through one or several parallel buffers
				{
					std::vector<std::pair<double, int>> order; // (slack, sink index)
					double call = 0;
					for (int si = 0; si < GetSize(net.sinks); si++) {
						auto &sink = net.sinks[si];
						call += sta.insts[sink.first].lc->pins[sink.second].cap[rf] + sta.opt.wire_cap;
						order.push_back({sta.pin_slack(sink.first, sink.second), si});
					}
					if (call < 2 * cin)
						continue;
					double max_cap = buf->pins[buf->buf_out].max_cap > 0 ? 0.9 * buf->pins[buf->buf_out].max_cap : 1e9;
					int g_min = std::max(1, int(ceil(call / max_cap)));
					int best_g = 0;
					double best_gain = min_gain;
					for (int g = g_min; g <= 4 * g_min && g <= GetSize(net.sinks); g *= 2) {
						double arr, slew, bd, bs;
						if (g * cin > 0.7 * call)
							break;
						if (!eval_node(path, k, g * (cin + sta.opt.wire_cap) - call, arr, slew))
							break;
						if (!eval_comb(buf, buf->buf_in, buf->buf_out, rf, rf, slew, call / g, bd, bs))
							break;
						// the slew at the sinks changes from net.slew to bs
						double gain = net.arr[rf] - (arr + bd) - next_stage_penalty(path, k, bs);
						if (gain > best_gain) {
							best_gain = gain;
							best_g = g;
						}
					}
					if (best_g > 0) {
						Move m;
						m.kind = Move::BUFFER;
						m.gain = best_gain;
						m.src = net.bit;
						m.buf = buf;
						// groups of similar slack and equal capacitance
						std::sort(order.begin(), order.end());
						double c = 0;
						m.groups.emplace_back();
						for (auto &it : order) {
							auto &sink = net.sinks[it.second];
							if (c >= call / best_g && GetSize(m.groups) < best_g) {
								m.groups.emplace_back();
								c = 0;
							}
							m.groups.back().push_back({sta.insts[sink.first].cell, sta.pin_id(sta.insts[sink.first].lc, sink.second)});
							c += sta.insts[sink.first].lc->pins[sink.second].cap[rf] + sta.opt.wire_cap;
						}
						moves.push_back(m);
					}
				}
			}
		}
		std::sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) { return a.gain > b.gain; });
	}

	// ---- max capacitance / max transition repair ----

	void repair()
	{
		for (int pass = 0; pass < 4; pass++) {
			int fixed = 0;
			std::vector<Move> buf_moves;
			for (int n = 0; n < GetSize(sta.nets); n++) {
				Net &net = sta.nets[n];
				if (net.drv < 0 || net.clock)
					continue;
				Inst &drv = sta.insts[net.drv];
				double load = std::max(net.load[RISE], net.load[FALL]);
				double slew = std::max(net.slew[RISE], net.slew[FALL]);
				double max_cap = drv.lc->pins[net.drv_pin].max_cap;
				if ((max_cap <= 0 || load <= max_cap) && slew <= max_tran)
					continue;
				if (!net.timed(RISE) && !net.timed(FALL))
					continue;
				// try a stronger driver
				bool ok = false;
				if (opt.resize && drv.lc->family >= 0) {
					auto &fam = lib->families[drv.lc->family];
					for (int s = drv.lc->size_idx + 1; s < GetSize(fam) && !ok; s++) {
						if (fam[s]->dont_use)
							continue;
						double mc = fam[s]->pins[net.drv_pin].max_cap;
						if (mc > 0 && load > mc)
							continue;
						// the slew scales roughly with the drive resistance
						if (slew > max_tran && slew * fam[s]->drive_res / drv.lc->drive_res > max_tran)
							continue;
						drv.cell->type = sta.type_id(fam[s]);
						drv.lc = fam[s];
						ok = true;
					}
				}
				if (ok) {
					fixed++;
					continue;
				}
				if (!opt.buffer || lib->buffers.empty() || GetSize(net.sinks) < 2)
					continue;
				// split the sinks into groups, each driven by a buffer
				const Cell *buf = lib->buffers[GetSize(lib->buffers) / 2];
				double group_cap = buf->pins[buf->buf_out].max_cap > 0 ? buf->pins[buf->buf_out].max_cap / 3 : 0.05;
				std::vector<std::pair<double, int>> sinks;
				for (int si = 0; si < GetSize(net.sinks); si++)
					sinks.push_back({sta.pin_slack(net.sinks[si].first, net.sinks[si].second), si});
				std::sort(sinks.begin(), sinks.end());
				Move m;
				m.kind = Move::BUFFER;
				m.src = net.bit;
				m.buf = buf;
				double c = 0;
				m.groups.emplace_back();
				for (auto &it : sinks) {
					auto &sink = net.sinks[it.second];
					double pc = sta.insts[sink.first].lc->pins[sink.second].cap[RISE] + sta.opt.wire_cap;
					if (!m.groups.back().empty() && c + pc > group_cap) {
						m.groups.emplace_back();
						c = 0;
					}
					m.groups.back().push_back({sta.insts[sink.first].cell, sta.pin_id(sta.insts[sink.first].lc, sink.second)});
					c += pc;
				}
				buf_moves.push_back(m);
				fixed++;
			}
			if (fixed == 0)
				break;
			for (auto &m : buf_moves) {
				insert_buffers(m);
				res.n_buffer += GetSize(m.groups);
			}
			res.n_repair += fixed;
			sta.build();
			sta.propagate();
		}
	}

	// ---- critical path optimization ----

	// Smooth approximation of the critical path delay: log-sum-exp of the
	// endpoint slacks with temperature tau. Unlike WNS it rewards progress on
	// all near-critical endpoints.
	double soft_delay(double tau)
	{
		double sum = 0;
		for (auto &ep : sta.endpoints)
			if (ep.slack < Sta::POS_INF / 2)
				sum += std::exp(-(ep.slack - sta.wns) / tau);
		return sum > 0 ? -sta.wns + tau * std::log(sum) : 0;
	}

	// reference state for accepting moves
	struct Ref {
		double wns, cost, thr, tau, soft;
	};

	Ref reference()
	{
		Ref r;
		r.wns = sta.wns;
		r.thr = r.wns + std::max(0.02, 0.03 * (sta.opt.period - sta.wns));
		r.cost = near_critical_cost(r.thr);
		r.tau = std::max(0.002, 0.004 * (sta.opt.period - sta.wns));
		r.soft = soft_delay(r.tau);
		return r;
	}

	// A move (sequence) is kept if it improves WNS, or keeps WNS and reduces
	// the negative slack of the near-critical nets. In designs with many
	// equally critical paths most moves make some other path a little worse;
	// such a move is also kept if it clearly reduces the smooth critical
	// delay and WNS stays within a small tolerance of the best value seen.
	bool improved(const Ref &r)
	{
		if (sta.wns > r.wns + 1e-6)
			return true;
		if (sta.wns > r.wns - 1e-7 && near_critical_cost(r.thr) < r.cost - 1e-6)
			return true;
		if (sta.wns > best_wns - 2 * r.tau && soft_delay(r.tau) < r.soft - 0.02 * r.tau)
			return true;
		return false;
	}

	void reject(const Move &m)
	{
		if (m.kind == Move::RESIZE)
			rejected_resize.insert({m.cell, m.new_lc});
		else if (m.drv_cell != nullptr)
			rejected_decouple.insert({m.src, m.buf});
		else
			rejected_buffer.insert({m.src, m.buf});
	}

	// Tries the candidate moves for the current worst path. A move that does
	// not improve the design on its own (typically because it makes another
	// path critical) is kept tentatively while up to 'depth' further moves
	// are tried on the new worst path. On success the accepted moves are
	// appended to 'seq'.
	bool search(int depth, int width, const Ref &ref, std::vector<Move> &seq)
	{
		if (sta.worst_endpoint < 0)
			return false;
		auto path = sta.worst_path();
		std::vector<Move> moves;
		collect_moves(path, moves);
		int tries = 0;
		for (auto &m : moves) {
			if (++tries > width)
				break;
			apply(m);
			if (improved(ref)) {
				seq.push_back(m);
				return true;
			}
			if (depth > 0) {
				if (search(depth - 1, std::max(2, width / 2), ref, seq)) {
					seq.push_back(m);
					return true;
				}
			} else if (!ignore_rejected) {
				if (opt.verbose)
					log("         rejected: %s (est. gain %.4f) -> wns %.4f (was %.4f)\n", m.describe(), m.gain, sta.wns, ref.wns);
				reject(m);
			}
			undo(m);
		}
		return false;
	}

	void timing_phase()
	{
		auto t0 = std::chrono::steady_clock::now();
		int max_iter = opt.max_iter > 0 ? opt.max_iter : std::max(2000, 3 * GetSize(sta.insts));
		int accepted_since_clear = 0;
		for (int iter = 0; iter < max_iter; iter++) {
			if (sta.opt.period > 0 && sta.wns >= 0)
				break;
			if (sta.worst_endpoint < 0)
				break;
			if (opt.max_time > 0 && (iter & 15) == 0 &&
					std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > opt.max_time)
				break;

			best_wns = std::max(best_wns, sta.wns);
			Ref ref = reference();
			std::vector<Move> seq;

			// single moves first, then sequences of up to three moves
			ignore_rejected = false;
			bool accepted = search(0, 16, ref, seq);
			if (!accepted && opt.lookahead > 0) {
				ignore_rejected = true;
				accepted = search(opt.lookahead, 6, ref, seq);
				ignore_rejected = false;
				if (accepted) {
					rejected_resize.clear();
					rejected_buffer.clear();
					rejected_decouple.clear();
				}
			}
			if (!accepted) {
				if (opt.verbose)
					log("  %5d: no acceptable move, stopping.\n", iter);
				break;
			}
			for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
				if (it->kind == Move::RESIZE)
					res.n_resize++;
				else
					res.n_buffer += GetSize(it->groups);
				if (opt.verbose)
					log("  %5d: %s (est. gain %.4f)\n", iter, it->describe(), it->gain);
			}
			if (opt.verbose)
				log("         -> wns %.4f\n", sta.wns);
			if (++accepted_since_clear >= 200) {
				accepted_since_clear = 0;
				rejected_resize.clear();
				rejected_buffer.clear();
				rejected_decouple.clear();
			}
		}
	}

	// ---- area recovery ----

	void area_phase()
	{
		for (int pass = 0; pass < 8; pass++) {
			double floor = sta.opt.period > 0 ? std::min(0.0, sta.wns) : sta.wns;
			double margin = 0.01 * std::max(0.1, sta.opt.period - sta.wns);
			std::vector<Move> batch;
			std::vector<double> delta(sta.nets.size(), 0.0);
			for (int i : sta.order) {
				const Inst &inst = sta.insts[i];
				double din = 0;
				for (int p = 0; p < GetSize(inst.lc->pins); p++)
					if (inst.lc->pins[p].comb_from && inst.pin_net[p] >= 0)
						din = std::max(din, delta[inst.pin_net[p]]);
				double own = 0;
				if (inst.lc->family >= 0 && inst.lc->size_idx > 0) {
					const Cell *alt = lib->families[inst.lc->family][inst.lc->size_idx - 1];
					bool cap_ok = true;
					for (int p = 0; p < GetSize(inst.lc->pins); p++)
						if (inst.lc->pins[p].output && inst.pin_net[p] >= 0 && alt->pins[p].max_cap > 0 &&
								std::max(sta.nets[inst.pin_net[p]].load[RISE], sta.nets[inst.pin_net[p]].load[FALL]) > alt->pins[p].max_cap)
							cap_ok = false;
					if (!alt->dont_use && cap_ok && alt->area < inst.lc->area) {
						double inc = 1.3 * delay_increase(i, alt) + 0.002;
						double slack = inst_out_slack(i);
						if (slack < Sta::POS_INF / 2 ? din + inc < slack - floor - margin : true) {
							Move m;
							m.kind = Move::RESIZE;
							m.cell = inst.cell;
							m.old_lc = inst.lc;
							m.new_lc = alt;
							batch.push_back(m);
							own = inc;
						}
					}
				}
				for (int p = 0; p < GetSize(inst.lc->pins); p++)
					if (inst.lc->pins[p].output && inst.pin_net[p] >= 0)
						delta[inst.pin_net[p]] = din + own;
			}
			if (batch.empty())
				break;
			resize_batch(batch, true);
			int n_applied = GetSize(batch);
			for (int round = 0; round < 20 && sta.wns < floor - 1e-6; round++) {
				// revert the moves on violating paths
				std::vector<Move> keep, revert;
				bool last = round == 19;
				for (auto &m : batch) {
					int i = sta.inst_of.at(m.cell);
					if (last || inst_slack(i) < floor + 0.5 * margin)
						revert.push_back(m);
					else
						keep.push_back(m);
				}
				if (revert.empty()) {
					revert.swap(keep);
				}
				resize_batch(revert, false);
				batch.swap(keep);
			}
			res.n_downsize += GetSize(batch);
			if (opt.verbose)
				log("  area recovery pass %d: %d of %d downsizing moves kept, wns %.4f, area %.2f\n", pass, GetSize(batch), n_applied, sta.wns, sta.area());
			if (GetSize(batch) * 200 < GetSize(sta.insts))
				break;
		}
	}

	SizerResult run()
	{
		sta.quiet = true;
		sta.run();
		sta.report_summary("  initial:   ");
		if (opt.repair) {
			repair();
			if (res.n_repair)
				sta.report_summary("  repaired:  ");
		}
		timing_phase();
		sta.report_summary("  optimized: ");
		if (opt.area && opt.resize) {
			area_phase();
			sta.report_summary("  recovered: ");
		}
		log("  %d cells resized, %d buffers inserted, %d cells downsized for area, %d limit violations repaired.\n",
				res.n_resize, res.n_buffer, res.n_downsize, res.n_repair);
		res.wns = sta.wns;
		res.tns = sta.tns;
		res.min_period = sta.min_period();
		res.area = sta.area();
		return res;
	}
};

} // namespace

SizerResult timing_optimize(RTLIL::Module *module, const Library *lib, const StaOptions &sta_opt, const SizerOptions &opt)
{
	Sizer sizer(module, lib, sta_opt, opt);
	return sizer.run();
}

} // namespace Timing

YOSYS_NAMESPACE_END
