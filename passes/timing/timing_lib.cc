/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: Liberty NLDM timing model
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

#include "passes/timing/timing_lib.h"
#include "passes/techmap/libparse.h"
#include "kernel/gzip.h"

YOSYS_NAMESPACE_BEGIN

namespace Timing {

static inline void axis_pos(const std::vector<double> &idx, double x, int &i, double &f)
{
	int n = GetSize(idx);
	i = 0;
	f = 0;
	if (n < 2)
		return;
	while (i < n - 2 && x > idx[i + 1])
		i++;
	f = (x - idx[i]) / (idx[i + 1] - idx[i]);
}

double Table::lookup(double slew, double load, double cslew) const
{
	if (values.size() == 1)
		return values[0];
	auto pick = [&](Var v, const std::vector<double> &idx) {
		switch (v) {
		case SLEW: return slew;
		case LOAD: return load;
		case CSLEW: return cslew;
		default: return idx.empty() ? 0.0 : idx[0];
		}
	};
	int n2 = std::max(1, GetSize(idx2));
	int i, j;
	double fi, fj;
	axis_pos(idx1, pick(var1, idx1), i, fi);
	axis_pos(idx2, pick(var2, idx2), j, fj);
	auto at = [&](int a, int b) { return values[a * n2 + b]; };
	if (GetSize(idx1) < 2) {
		if (n2 < 2)
			return at(0, 0);
		return at(0, j) + fj * (at(0, j + 1) - at(0, j));
	}
	if (n2 < 2)
		return at(i, 0) + fi * (at(i + 1, 0) - at(i, 0));
	double v0 = at(i, j) + fj * (at(i, j + 1) - at(i, j));
	double v1 = at(i + 1, j) + fj * (at(i + 1, j + 1) - at(i + 1, j));
	return v0 + fi * (v1 - v0);
}

namespace {

struct TableTemplate {
	Table::Var var1 = Table::NONE, var2 = Table::NONE;
	std::vector<double> idx1, idx2;
};

std::vector<double> parse_floats(const std::vector<std::string> &args, double scale)
{
	std::vector<double> out;
	for (auto &s : args) {
		const char *p = s.c_str();
		while (*p) {
			while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\\' || *p == '"')
				p++;
			if (!*p)
				break;
			char *end;
			double v = strtod(p, &end);
			if (end == p)
				break;
			out.push_back(v * scale);
			p = end;
		}
	}
	return out;
}

Table::Var parse_var(const std::string &s)
{
	if (s == "input_net_transition" || s == "related_pin_transition")
		return Table::SLEW;
	if (s == "total_output_net_capacitance")
		return Table::LOAD;
	if (s == "constrained_pin_transition")
		return Table::CSLEW;
	return Table::NONE;
}

struct Loader {
	Library &lib;
	double time_scale = 1.0; // library time unit in ns
	double cap_scale = 1.0;  // library capacitance unit in pF
	dict<std::string, TableTemplate> templates;

	Loader(Library &lib) : lib(lib) {}

	double axis_scale(Table::Var v) const { return v == Table::LOAD ? cap_scale : time_scale; }

	void parse_units(const LibertyAst *ast)
	{
		if (auto n = ast->find("time_unit")) {
			std::string v = n->value;
			double num = atof(v.c_str());
			if (num == 0)
				num = 1;
			if (v.find("ps") != std::string::npos)
				time_scale = num * 1e-3;
			else if (v.find("ns") != std::string::npos)
				time_scale = num;
			else if (v.find("us") != std::string::npos)
				time_scale = num * 1e3;
		}
		if (auto n = ast->find("capacitive_load_unit")) {
			if (n->args.size() >= 2) {
				double num = atof(n->args[0].c_str());
				std::string u = n->args[1];
				for (auto &c : u)
					c = tolower(c);
				if (u == "ff")
					cap_scale = num * 1e-3;
				else if (u == "pf")
					cap_scale = num;
			}
		}
		if (auto n = ast->find("default_max_transition"))
			lib.default_max_tran = atof(n->value.c_str()) * time_scale;
	}

	void parse_templates(const LibertyAst *ast)
	{
		for (auto child : ast->children) {
			if (child->id != "lu_table_template" || child->args.empty())
				continue;
			TableTemplate t;
			if (auto n = child->find("variable_1"))
				t.var1 = parse_var(n->value);
			if (auto n = child->find("variable_2"))
				t.var2 = parse_var(n->value);
			if (auto n = child->find("index_1"))
				t.idx1 = parse_floats(n->args, axis_scale(t.var1));
			if (auto n = child->find("index_2"))
				t.idx2 = parse_floats(n->args, axis_scale(t.var2));
			templates[child->args[0]] = t;
		}
	}

	Table parse_table(const LibertyAst *node)
	{
		Table t;
		if (node == nullptr)
			return t;
		if (!node->args.empty()) {
			auto it = templates.find(node->args[0]);
			if (it != templates.end()) {
				t.var1 = it->second.var1;
				t.var2 = it->second.var2;
				t.idx1 = it->second.idx1;
				t.idx2 = it->second.idx2;
			}
		}
		if (auto n = node->find("index_1"))
			t.idx1 = parse_floats(n->args, axis_scale(t.var1));
		if (auto n = node->find("index_2"))
			t.idx2 = parse_floats(n->args, axis_scale(t.var2));
		if (auto n = node->find("values"))
			t.values = parse_floats(n->args, time_scale);
		size_t expect = std::max<size_t>(1, t.idx1.size()) * std::max<size_t>(1, t.idx2.size());
		if (t.values.size() == 1) {
			t.idx1.clear();
			t.idx2.clear();
		} else if (t.values.size() != expect) {
			t.values.clear();
		}
		return t;
	}

	static std::string serialize(const LibertyAst *ast)
	{
		std::string s = ast->id + "(";
		for (auto &arg : ast->args)
			s += arg + ",";
		s += ")=" + ast->value + "{";
		for (auto child : ast->children)
			s += serialize(child) + ";";
		return s + "}";
	}

	// Everything besides pin names and functions that has to match before one
	// cell may replace another. The footprint alone is not sufficient: flip-flops
	// of both clock edges can share one.
	static std::string behavior(const LibertyAst *ast, const Cell &cell)
	{
		std::string s;
		for (auto child : ast->children) {
			if (child->id == "ff" || child->id == "ff_bank" || child->id == "latch" || child->id == "latch_bank" ||
					child->id == "statetable" || child->id == "clock_gating_integrated_cell")
				s += serialize(child);
			if (child->id == "pin")
				for (auto attr : child->children)
					if (attr->id == "three_state" || attr->id == "state_function" || attr->id == "internal_node")
						s += child->args.empty() ? serialize(attr) : child->args[0] + serialize(attr);
		}
		std::set<std::string> edges;
		for (auto arcs : {&cell.arcs, &cell.checks})
			for (auto &arc : *arcs)
				if (arc.type != Arc::COMB)
					edges.insert(stringf("%d:%d:%d:%d", int(arc.type), arc.from, arc.to, int(arc.falling_clock)));
		for (auto &edge : edges)
			s += edge;
		return s;
	}

	// pins that are not modeled (buses, inouts) rule out substitution
	static bool fully_modeled(const LibertyAst *ast)
	{
		for (auto child : ast->children) {
			if (child->id == "bus" || child->id == "bundle")
				return false;
			if (child->id == "pin")
				if (auto dir = child->find("direction"))
					if (dir->value == "inout")
						return false;
		}
		return true;
	}

	void parse_cell(const LibertyAst *ast, const std::vector<std::string> &dont_use)
	{
		// the first definition of a cell wins
		if (lib.cells.count(ast->args[0]))
			return;
		bool substitutable = fully_modeled(ast);
		auto cell = std::make_unique<Cell>();
		cell->name = ast->args[0];
		if (auto n = ast->find("area"))
			cell->area = atof(n->value.c_str());
		if (ast->find("ff") || ast->find("latch") || ast->find("statetable"))
			cell->seq = true;
		if (auto n = ast->find("dont_use"))
			if (n->value == "true")
				cell->dont_use = true;
		for (auto &pat : dont_use)
			if (patmatch(pat.c_str(), ast->args[0].c_str()))
				cell->dont_use = true;

		// first pass: pins
		std::vector<std::pair<const LibertyAst *, int>> pin_asts;
		for (auto child : ast->children) {
			if (child->id != "pin")
				continue;
			auto dir = child->find("direction");
			if (dir == nullptr || (dir->value != "input" && dir->value != "output"))
				continue;
			for (auto &pname : child->args) {
				Pin pin;
				pin.name = pname;
				pin.input = dir->value == "input";
				pin.output = dir->value == "output";
				if (auto n = child->find("clock"))
					pin.clock = n->value == "true";
				double c = 0;
				if (auto n = child->find("capacitance"))
					c = atof(n->value.c_str()) * cap_scale;
				pin.cap[RISE] = pin.cap[FALL] = c;
				if (auto n = child->find("rise_capacitance"))
					pin.cap[RISE] = atof(n->value.c_str()) * cap_scale;
				if (auto n = child->find("fall_capacitance"))
					pin.cap[FALL] = atof(n->value.c_str()) * cap_scale;
				if (auto n = child->find("max_capacitance"))
					pin.max_cap = atof(n->value.c_str()) * cap_scale;
				if (auto n = child->find("max_transition"))
					pin.max_tran = atof(n->value.c_str()) * time_scale;
				if (auto n = child->find("function"))
					pin.function = n->value;
				cell->pin_idx[pin.name] = GetSize(cell->pins);
				pin_asts.push_back({child, GetSize(cell->pins)});
				cell->pins.push_back(pin);
			}
		}

		// second pass: timing groups
		for (auto &it : pin_asts) {
			int to = it.second;
			for (auto tg : it.first->children) {
				if (tg->id != "timing")
					continue;
				std::string ttype = "combinational";
				if (auto n = tg->find("timing_type"))
					ttype = n->value;
				Arc arc;
				arc.to = to;
				if (ttype == "combinational" || ttype == "combinational_rise" || ttype == "combinational_fall")
					arc.type = Arc::COMB;
				else if (ttype == "rising_edge" || ttype == "falling_edge")
					arc.type = Arc::CLK, arc.falling_clock = ttype == "falling_edge";
				else if (ttype == "setup_rising" || ttype == "recovery_rising")
					arc.type = Arc::SETUP;
				else if (ttype == "setup_falling" || ttype == "recovery_falling")
					arc.type = Arc::SETUP, arc.falling_clock = true;
				else
					continue;
				arc.sense = Arc::NON;
				if (auto n = tg->find("timing_sense")) {
					if (n->value == "positive_unate")
						arc.sense = Arc::POS;
					else if (n->value == "negative_unate")
						arc.sense = Arc::NEG;
				}
				if (arc.type == Arc::SETUP) {
					arc.constraint[RISE] = parse_table(tg->find("rise_constraint"));
					arc.constraint[FALL] = parse_table(tg->find("fall_constraint"));
					if (!arc.constraint[RISE].valid() && !arc.constraint[FALL].valid())
						continue;
				} else {
					arc.delay[RISE] = parse_table(tg->find("cell_rise"));
					arc.delay[FALL] = parse_table(tg->find("cell_fall"));
					arc.trans[RISE] = parse_table(tg->find("rise_transition"));
					arc.trans[FALL] = parse_table(tg->find("fall_transition"));
					if (!arc.delay[RISE].valid() && !arc.delay[FALL].valid())
						continue;
				}
				auto rel = tg->find("related_pin");
				if (rel == nullptr)
					continue;
				for (auto &rname : split_tokens(rel->value, " \t,")) {
					int from = cell->pin(rname);
					if (from < 0)
						continue;
					arc.from = from;
					if (arc.type == Arc::SETUP)
						cell->checks.push_back(arc);
					else
						cell->arcs.push_back(arc);
					if (arc.type == Arc::CLK || arc.type == Arc::SETUP)
						cell->pins[from].clock = true;
					if (arc.type == Arc::COMB)
						cell->pins[from].comb_from = true;
				}
			}
		}

		// buffer detection
		int n_in = 0, n_out = 0, in_idx = -1, out_idx = -1;
		for (int i = 0; i < GetSize(cell->pins); i++) {
			if (cell->pins[i].input)
				n_in++, in_idx = i;
			if (cell->pins[i].output)
				n_out++, out_idx = i;
		}
		if (substitutable && !cell->seq && n_in == 1 && n_out == 1) {
			std::string f;
			for (char c : cell->pins[out_idx].function)
				if (c != ' ' && c != '(' && c != ')' && c != '"')
					f += c;
			if (f == cell->pins[in_idx].name) {
				cell->is_buf = true;
				cell->buf_in = in_idx;
				cell->buf_out = out_idx;
			}
		}

		// drive strength estimate from the first delay arc
		for (auto &arc : cell->arcs) {
			const Table &t = arc.delay[RISE].valid() ? arc.delay[RISE] : arc.delay[FALL];
			const std::vector<double> *lidx = t.var1 == Table::LOAD ? &t.idx1 : t.var2 == Table::LOAD ? &t.idx2 : nullptr;
			const std::vector<double> *sidx = t.var1 == Table::SLEW ? &t.idx1 : t.var2 == Table::SLEW ? &t.idx2 : nullptr;
			if (lidx == nullptr || lidx->size() < 2)
				continue;
			double slew = (sidx && !sidx->empty()) ? (*sidx)[sidx->size() / 2] : 0.1;
			double c0 = lidx->front(), c1 = lidx->back();
			cell->drive_res = (t.lookup(slew, c1) - t.lookup(slew, c0)) / (c1 - c0);
			break;
		}

		std::string footprint;
		if (auto n = ast->find("cell_footprint"))
			footprint = n->value;
		if (substitutable && !footprint.empty() && cell->drive_res > 0) {
			std::string key = footprint + behavior(ast, *cell);
			for (auto &pin : cell->pins)
				key += stringf("|%s:%d:%s", pin.name, pin.input ? 0 : 1, pin.function);
			if (!family_of.count(key)) {
				family_of[key] = GetSize(lib.families);
				lib.families.push_back({});
			}
			cell->family = family_of.at(key);
			lib.families[cell->family].push_back(cell.get());
		}

		lib.cells[cell->name] = cell.get();
		lib.cell_store.push_back(std::move(cell));
	}

	dict<std::string, int> family_of;

	void finish()
	{
		for (auto &fam : lib.families) {
			std::sort(fam.begin(), fam.end(), [](const Cell *a, const Cell *b) {
				if (a->drive_res != b->drive_res)
					return a->drive_res > b->drive_res;
				return a->area < b->area;
			});
			for (int i = 0; i < GetSize(fam); i++)
				fam[i]->size_idx = i;
		}
		for (auto &c : lib.cell_store)
			if (c->is_buf && !c->dont_use)
				lib.buffers.push_back(c.get());
		std::sort(lib.buffers.begin(), lib.buffers.end(), [](const Cell *a, const Cell *b) {
			if (a->drive_res != b->drive_res)
				return a->drive_res > b->drive_res;
			return a->area < b->area;
		});
	}
};

} // namespace

std::shared_ptr<Library> Library::get(const std::vector<std::string> &files, const std::vector<std::string> &dont_use)
{
	static dict<std::string, std::shared_ptr<Library>> cache;
	std::string key;
	for (auto &f : files)
		key += f + "\n";
	key += "\n";
	for (auto &d : dont_use)
		key += d + "\n";
	auto it = cache.find(key);
	if (it != cache.end())
		return it->second;

	auto lib = std::make_shared<Library>();
	Loader loader(*lib);
	for (auto &fn : files) {
		log("Reading timing data from liberty file `%s'.\n", fn);
		std::istream *f = uncompressed(fn);
		if (f == nullptr || f->fail())
			log_cmd_error("Can't open liberty file `%s'.\n", fn);
		yosys_input_files.insert(fn);
		LibertyParser parser(*f, fn);
		delete f;
		const LibertyAst *ast = parser.ast;
		if (ast == nullptr || ast->id != "library")
			log_cmd_error("Liberty file `%s' does not contain a library.\n", fn);
		loader.time_scale = loader.cap_scale = 1.0;
		loader.templates.clear();
		loader.parse_units(ast);
		loader.parse_templates(ast);
		for (auto child : ast->children)
			if (child->id == "cell" && child->args.size() == 1)
				loader.parse_cell(child, dont_use);
	}
	loader.finish();
	log("Loaded %d cells (%d sizing families, %d usable buffers).\n", GetSize(lib->cells), GetSize(lib->families), GetSize(lib->buffers));
	cache[key] = lib;
	return lib;
}

} // namespace Timing

YOSYS_NAMESPACE_END
