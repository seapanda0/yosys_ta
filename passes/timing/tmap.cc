/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: tmap command (timing driven technology mapping)
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
#include <chrono>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// A front-end variant: the yosys commands that take the module from the
// coarse-grain (word level) representation to generic gates. Variants differ
// in the architecture that is used for arithmetic operators.
struct ArchVariant {
	const char *name;
	const char *descr;
	const char *script;
	int effort;       // minimum effort level at which the variant is explored
	int mul;          // +1: only for designs with multipliers, -1: only for designs without (see below)
	bool period_only; // only explored if there is a clock period to meet
};

// The common tail of all front-end variants ({TECHMAP} is replaced).
static const char *arch_tail =
	"share; opt; memory -nomap; opt_clean; opt -fast -full; memory_map; opt -full; "
	"{TECHMAP}; opt -fast{PREOPT}";

// Parallel prefix adders in the rows of an array multiplier are large and
// highly redundant. For designs with multipliers the prefix adder variants
// are therefore only explored in combination with Booth multipliers (unless
// selected with -arch).
static const std::vector<ArchVariant> arch_variants = {
	{"default", "Brent-Kung adders, array multipliers (same as 'synth')",
		"alumacc; {TAIL:techmap}", 0, 0, false},
	{"ks", "Kogge-Stone adders",
		"alumacc; {TAIL:techmap -map +/techmap.v -map +/choices/kogge-stone.v}", 1, -1, false},
	{"booth_ks", "Booth multipliers, Kogge-Stone adders",
		"booth; alumacc; {TAIL:techmap -map +/techmap.v -map +/choices/kogge-stone.v}", 1, 1, false},
	{"ripple", "ripple-carry adders",
		"alumacc; {TAIL:techmap -map +/techmap.v -map +/choices/ripple.v}", 1, 0, true},
	{"booth", "Booth multipliers, Brent-Kung adders",
		"booth; alumacc; {TAIL:techmap}", 2, 1, false},
	{"sklansky", "Sklansky adders",
		"alumacc; {TAIL:techmap -map +/techmap.v -map +/choices/sklansky.v}", 2, -1, false},
	{"hc", "Han-Carlson adders",
		"alumacc; {TAIL:techmap -map +/techmap.v -map +/choices/han-carlson.v}", 2, -1, false},
	{"booth_sklansky", "Booth multipliers, Sklansky adders",
		"booth; alumacc; {TAIL:techmap -map +/techmap.v -map +/choices/sklansky.v}", 2, 1, false},
};

// A mapping recipe: an ABC script and the options for reading the library.
// In the script {D} is replaced by the delay target option ("-D <ps>", or
// nothing when optimizing for speed).
//
// The recipes use the classic 'map' command of ABC: it derives a load
// dependent delay for each gate class from the NLDM tables of the library
// ("gain based" delay model, see the -G option of 'map'), so the mapper and
// its delay target work with real delays. The '&nf' mapper that the 'abc'
// command of Yosys uses by default sees a unit-delay model of the library
// instead (every gate has delay 1), unless the library is read with the
// read_lib option -G (lib_args).
//
// The scripts do not use the 'buffer' command of ABC, buffering is done by
// the timing driven sizing that follows.
struct MapRecipe {
	std::string name;
	std::string descr;
	std::string lib_args;
	std::string script;
	bool area;         // area oriented recipe, only used if there is a clock period to meet
	int effort;        // minimum effort level at which the recipe is explored when optimizing for speed
	int period_effort; // same, if there is a clock period to meet
};

// The recipes were selected with a study on 18 designs (see
// docs/timing_aware_synthesis.md). Sizing and buffering is left to the
// timing driven optimization that follows the mapping.
#define TMAP_SOPB "&st; &syn2; &if -g -K 6; &synch2"
static const std::vector<MapRecipe> map_recipes = {
	{"speed", "SOP balancing on the AIG (two rounds) and on the logic network, then delay driven mapping with structural choices", "",
		"strash; &get -n; " TMAP_SOPB "; &if -g -K 6; &synch2; &put; "
		"strash; dch -f; if -g -K 6; strash; dch -f; map -B 0.9 {D}", false, 0, 0},
	{"speed2", "two rounds of SOP balancing on the logic network, then delay driven mapping with structural choices", "",
		"strash; dch -f; if -g -K 6; strash; dch -f; if -g -K 6; strash; dch -f; map -B 0.9 {D}", false, 1, 2},
	{"speed3", "three rounds of SOP balancing on the AIG, then delay driven mapping with structural choices", "",
		"strash; &get -n; " TMAP_SOPB "; &if -g -K 6; &synch2; &if -g -K 6; &synch2; &put; "
		"strash; dch -f; map -B 0.9 {D}", false, 2, 2},
	{"area", "delay driven mapping with structural choices and area recovery, no restructuring for delay", "",
		"strash; dch; map -B 0.9 {D}", true, 0, 0},
};

struct Candidate {
	std::string name;
	RTLIL::Module *module = nullptr;
	Timing::SizerResult res;
	double runtime = 0;
	const ArchVariant *arch = nullptr;
	const MapRecipe *recipe = nullptr;
	int d_ps = 0;
};

struct TmapPass : public Pass {
	TmapPass() : Pass("tmap", "timing driven technology mapping") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    tmap [options] [selection]\n");
		log("\n");
		log("This command maps the selected modules to the cells of a liberty library.\n");
		log("In contrast to calling 'abc' directly, it closes the loop with a timing\n");
		log("analysis of the mapped netlist: several implementation candidates are\n");
		log("created, each candidate is sized and buffered (see 'tsize') and timed (see\n");
		log("'tsta') with the constraints given by 'timing_setup', and the best one is\n");
		log("kept.\n");
		log("\n");
		log("A candidate is the combination of an architecture variant (the\n");
		log("implementation of the arithmetic operators) and a mapping recipe (an ABC\n");
		log("script). The recipes restructure the logic for delay and use a mapper with\n");
		log("a load dependent delay model derived from the delay tables of the library.\n");
		log("\n");
		log("Without a clock period (period 0), the fastest candidate is kept: all\n");
		log("architecture variants are mapped with the first speed recipe, then the\n");
		log("remaining speed recipes are tried on the best architecture.\n");
		log("\n");
		log("With a clock period, the smallest candidate that meets the period is kept\n");
		log("(or the fastest one if none does): the delay budget of the combinational\n");
		log("logic is passed to the mapper as delay target, so that area is recovered\n");
		log("on paths with slack, and area oriented recipes and cheaper architectures\n");
		log("are explored as well. Finally the delay target of the selected candidate\n");
		log("is adjusted using the measured slack (see -refine).\n");
		log("\n");
		log("The command expects flat modules in the coarse-grain representation that\n");
		log("'synth -run begin:coarse' produces up to the 'peepopt' step, i.e. with\n");
		log("word-level arithmetic cells. It is usually called by 'synth_timing'.\n");
		log("\n");
		log("    -arch <name>\n");
		log("        only use the given architecture variant (can be given multiple\n");
		log("        times). Use 'tmap -list' to print the known variants.\n");
		log("\n");
		log("    -recipe <name>\n");
		log("        only use the given mapping recipe (can be given multiple times).\n");
		log("        Use 'tmap -list' to print the known recipes.\n");
		log("\n");
		log("    -script <abc_script>\n");
		log("        add a custom mapping recipe. Commands are separated by semicolons,\n");
		log("        {D} is replaced by the delay target option.\n");
		log("\n");
		log("    -liberty_args <args>\n");
		log("        options for the read_lib command of ABC that are used for custom\n");
		log("        recipes, e.g. \"-G 250 -a\" for a gain based delay model in '&nf'\n");
		log("\n");
		log("    -gates\n");
		log("        the modules are already bit-blasted to generic gates and the\n");
		log("        flip-flops are already mapped; skip the architecture exploration.\n");
		log("\n");
		log("    -effort <0|1|2>\n");
		log("        0: one architecture and one recipe, 1: a reduced set of candidates,\n");
		log("        2: all candidates (default: 1)\n");
		log("\n");
		log("    -refine <n>\n");
		log("        maximum number of delay target adjustments for the selected\n");
		log("        candidate when a clock period is given (default: 3)\n");
		log("\n");
		log("    -nosize\n");
		log("        do not run timing driven sizing on the candidates\n");
		log("\n");
		log("    -max_time <seconds>\n");
		log("        time limit for sizing a candidate\n");
		log("\n");
		log("    -list\n");
		log("        print the known architecture variants and mapping recipes\n");
		log("\n");
		Timing::TimingConfig::help();
	}

	Timing::TimingConfig cfg;
	Timing::SizerOptions size_opt;
	std::shared_ptr<Timing::Library> lib;
	bool nosize = false;
	int cand_counter = 0;

	static std::string replace_all(std::string s, const std::string &from, const std::string &to)
	{
		for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
			s.replace(pos, from.size(), to);
		return s;
	}

	// The gate level netlist is cleaned up with the default script of the
	// 'abc' command before it is mapped. Its functional reduction step
	// (&fraig) does not terminate in reasonable time on wide multipliers, so
	// designs with multipliers get a script without it.
	static std::string expand_arch(const std::string &script, bool has_mul)
	{
		std::string s = script;
		size_t pos = s.find("{TAIL:");
		if (pos == std::string::npos)
			return s;
		size_t end = s.find('}', pos);
		std::string techmap = s.substr(pos + 6, end - pos - 6);
		std::string tail = replace_all(arch_tail, "{TECHMAP}", techmap);
		tail = replace_all(tail, "{PREOPT}", has_mul ? "; abc -script +strash;dc2;strash;&get,-n;&dch,-f;&nf;&put; opt -fast" : "; abc; opt -fast");
		return s.substr(0, pos) + tail + s.substr(end + 1);
	}

	std::vector<std::string> lib_args(const char *cmd)
	{
		std::vector<std::string> args = {cmd};
		for (auto &f : cfg.liberty_files)
			args.push_back("-liberty"), args.push_back(f);
		for (auto &d : cfg.dont_use)
			args.push_back("-dont_use"), args.push_back(d);
		return args;
	}

	// delay budget of the combinational logic in ps: the clock period minus
	// clock-to-output delay and setup time of the flip-flops
	int delay_budget(RTLIL::Module *module)
	{
		if (cfg.sta.period <= 0)
			return 0;
		double overhead = 0;
		bool half_cycle = false;
		pool<IdString> seen;
		for (auto cell : module->cells()) {
			if (seen.count(cell->type))
				continue;
			seen.insert(cell->type);
			auto lc = lib->find(cell->type.unescape());
			if (lc == nullptr || !lc->seq)
				continue;
			for (auto arcs : {&lc->arcs, &lc->checks})
				for (auto &arc : *arcs)
					if (arc.type != Timing::Arc::COMB && arc.falling_clock)
						half_cycle = true;
			double clk2q = 0, setup = 0;
			for (auto &arc : lc->arcs)
				if (arc.type == Timing::Arc::CLK)
					for (int rf = 0; rf < 2; rf++)
						if (arc.delay[rf].valid())
							clk2q = std::max(clk2q, arc.delay[rf].lookup(cfg.sta.clock_slew, 0.01));
			for (auto &chk : lc->checks)
				for (int rf = 0; rf < 2; rf++)
					if (chk.constraint[rf].valid())
						setup = std::max(setup, chk.constraint[rf].lookup(cfg.sta.clock_slew, 0, 0.1));
			overhead = std::max(overhead, clk2q + setup);
		}
		return std::max(1, int(((half_cycle ? 0.5 : 1.0) * cfg.sta.period - overhead) * 1000));
	}

	std::vector<std::string> abc_command(const MapRecipe &recipe, int d_ps)
	{
		std::string s = replace_all(recipe.script, "{D}", d_ps > 0 ? stringf("-D %d", d_ps) : std::string());
		// the abc pass takes inline scripts with ',' instead of ' '
		s = replace_all(s, "; ", ";");
		while (!s.empty() && s.back() == ' ')
			s.pop_back();
		s = replace_all(s, " ;", ";");
		s = replace_all(s, " ", ",");
		std::vector<std::string> cmd = lib_args("abc");
		if (!recipe.lib_args.empty())
			cmd.push_back("-liberty_args"), cmd.push_back(recipe.lib_args);
		cmd.push_back("-script");
		cmd.push_back("+" + s);
		return cmd;
	}

	bool meets(const Candidate &c) { return c.res.wns >= 0; }

	bool better(const Candidate &a, const Candidate &b)
	{
		if (cfg.sta.period > 0) {
			if (meets(a) != meets(b))
				return meets(a);
			if (meets(a))
				return a.res.area < b.res.area;
		}
		if (fabs(a.res.wns - b.res.wns) > 1e-4)
			return a.res.wns > b.res.wns;
		return a.res.area < b.res.area;
	}

	// bit-blast a copy of the module with the given architecture variant and map the flip-flops
	RTLIL::Module *make_front(RTLIL::Design *design, RTLIL::Module *src, const ArchVariant *arch, bool has_mul)
	{
		RTLIL::Module *m = src->clone(design, design->twines().add(stringf("\\__tmap_%d_front_%s", cand_counter++, arch->name)));
		log_header(design, "Timing driven mapping: architecture variant '%s'.\n", arch->name);
		log_push();
		// commands are separated by "; " (an inline ABC script contains ';' without blanks)
		std::string script = expand_arch(arch->script, has_mul) + "; ";
		for (size_t pos = 0, end; (end = script.find("; ", pos)) != std::string::npos; pos = end + 2)
			if (end > pos)
				Pass::call_on_module(design, m, script.substr(pos, end - pos));
		Pass::call_on_module(design, m, lib_args("dfflibmap"));
		log_pop();
		return m;
	}

	// run a mapping recipe on a copy of the gate level module 'src', then size and time the result
	Candidate make_candidate(RTLIL::Design *design, RTLIL::Module *src, const ArchVariant *arch, const MapRecipe *recipe, int d_ps)
	{
		auto t0 = std::chrono::steady_clock::now();
		Candidate cand;
		cand.arch = arch;
		cand.recipe = recipe;
		cand.d_ps = d_ps;
		cand.name = arch ? stringf("%s/%s", arch->name, recipe->name) : recipe->name;
		if (d_ps > 0)
			cand.name += stringf("@%d", d_ps);
		cand.module = src->clone(design, design->twines().add(stringf("\\__tmap_%d_cand", cand_counter++)));
		log_header(design, "Timing driven mapping: candidate '%s'.\n", cand.name);
		log_push();
		Pass::call_on_module(design, cand.module, abc_command(*recipe, d_ps));
		Pass::call_on_module(design, cand.module, "opt_clean");
		log_pop();
		if (nosize) {
			Timing::Sta sta(cand.module, lib.get(), cfg.sta);
			sta.quiet = true;
			sta.run();
			cand.res.wns = sta.wns;
			cand.res.tns = sta.tns;
			cand.res.min_period = sta.min_period();
			cand.res.area = sta.area();
		} else
			cand.res = Timing::timing_optimize(cand.module, lib.get(), cfg.sta, size_opt);
		cand.runtime = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		log("Candidate '%s': wns %.4f ns, min period %.4f ns (%.2f MHz), area %.2f, %.1f s\n", cand.name, cand.res.wns,
				cand.res.min_period, cand.res.min_period > 0 ? 1e3 / cand.res.min_period : 0.0, cand.res.area, cand.runtime);
		return cand;
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing TMAP pass (timing driven technology mapping).\n");
		cfg = Timing::TimingConfig();
		cfg.load(design);
		size_opt = Timing::SizerOptions();
		nosize = false;
		int effort = 1, refine = 3;
		bool gates = false, list = false;
		std::vector<std::string> arch_sel, recipe_sel, custom_scripts;
		std::string custom_lib_args;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (cfg.parse_arg(args, argidx))
				continue;
			if (args[argidx] == "-arch" && argidx + 1 < args.size()) {
				arch_sel.push_back(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-recipe" && argidx + 1 < args.size()) {
				recipe_sel.push_back(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-script" && argidx + 1 < args.size()) {
				custom_scripts.push_back(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-liberty_args" && argidx + 1 < args.size()) {
				custom_lib_args = args[++argidx];
				if (custom_lib_args.size() >= 2 && custom_lib_args.front() == '"' && custom_lib_args.back() == '"')
					custom_lib_args = custom_lib_args.substr(1, custom_lib_args.size() - 2);
				continue;
			}
			if (args[argidx] == "-effort" && argidx + 1 < args.size()) {
				effort = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-refine" && argidx + 1 < args.size()) {
				refine = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-max_time" && argidx + 1 < args.size()) {
				size_opt.max_time = atof(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-gates") {
				gates = true;
				continue;
			}
			if (args[argidx] == "-nosize") {
				nosize = true;
				continue;
			}
			if (args[argidx] == "-list") {
				list = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (list) {
			log("\nArchitecture variants:\n");
			for (auto &a : arch_variants)
				log("  %-10s %s\n", a.name, a.descr);
			log("\nMapping recipes:\n");
			for (auto &r : map_recipes)
				log("  %-10s %s\n             read_lib %s\n             %s\n", r.name, r.descr, r.lib_args, r.script);
			return;
		}

		lib = cfg.library();
		bool have_period = cfg.sta.period > 0;

		// mapping recipes
		std::vector<MapRecipe> recipes;
		if (!recipe_sel.empty() || !custom_scripts.empty()) {
			for (auto &n : recipe_sel) {
				bool found = false;
				for (auto &r : map_recipes)
					if (n == r.name)
						recipes.push_back(r), found = true;
				if (!found)
					log_cmd_error("Unknown mapping recipe '%s'.\n", n);
			}
			for (size_t i = 0; i < custom_scripts.size(); i++)
				recipes.push_back({stringf("custom%d", int(i)), "custom script", custom_lib_args, custom_scripts[i], false, 0, 0});
		} else {
			for (auto &r : map_recipes)
				if (have_period ? r.period_effort <= effort : (r.effort <= effort && !r.area))
					recipes.push_back(r);
		}

		std::vector<std::string> mod_names;
		for (auto module : design->selected_modules())
			mod_names.push_back(module->name.str());

		for (auto mod_name : mod_names) {
			RTLIL::Module *module = design->module(design->twines().find(mod_name));
			log("Mapping module %s.\n", RTLIL::unescape_id(mod_name));

			// architecture variants
			std::vector<const ArchVariant *> archs;
			bool has_mul = false;
			if (!gates) {
				for (auto cell : module->cells())
					if (cell->type.in(ID($mul), ID($macc), ID($macc_v2)))
						has_mul = true;
				if (!arch_sel.empty()) {
					for (auto &n : arch_sel) {
						bool found = false;
						for (auto &a : arch_variants)
							if (n == a.name)
								archs.push_back(&a), found = true;
						if (!found)
							log_cmd_error("Unknown architecture variant '%s'.\n", n);
					}
				} else {
					for (auto &a : arch_variants)
						if (a.effort <= effort && (a.mul == 0 || (a.mul > 0) == has_mul) && (have_period || !a.period_only))
							archs.push_back(&a);
				}
			}

			std::vector<Candidate> all;
			Candidate best;
			auto consider = [&](Candidate c) {
				bool is_best = best.module == nullptr || better(c, best);
				if (is_best) {
					if (best.module != nullptr)
						design->remove(best.module);
					best = c;
				} else
					design->remove(c.module);
				c.module = nullptr;
				all.push_back(c);
				return is_best;
			};

			RTLIL::Module *best_front = nullptr; // gate level netlist of the best candidate
			if (gates) {
				int d_ps = delay_budget(module);
				for (auto &r : recipes)
					consider(make_candidate(design, module, nullptr, &r, d_ps));
				best_front = module;
			} else if (!have_period) {
				// speed: all architectures with the first recipe, then the other recipes on the best one
				for (auto arch : archs) {
					RTLIL::Module *front = make_front(design, module, arch, has_mul);
					if (consider(make_candidate(design, front, arch, &recipes[0], 0))) {
						if (best_front != nullptr)
							design->remove(best_front);
						best_front = front;
					} else
						design->remove(front);
				}
				const ArchVariant *arch = best.arch;
				for (size_t i = 1; i < recipes.size(); i++)
					consider(make_candidate(design, best_front, arch, &recipes[i], 0));
			} else {
				// clock period: all architectures with all recipes, mapped for the delay budget
				for (auto arch : archs) {
					RTLIL::Module *front = make_front(design, module, arch, has_mul);
					int d_ps = delay_budget(front);
					bool keep = false;
					for (auto &r : recipes)
						if (consider(make_candidate(design, front, arch, &r, d_ps)))
							keep = true;
					if (keep) {
						if (best_front != nullptr)
							design->remove(best_front);
						best_front = front;
					} else
						design->remove(front);
				}
			}

			// Delay target refinement. The delay model of the mapper is only an
			// estimate of the timing after sizing: use the measured slack of
			// the selected candidate to correct the delay target.
			if (have_period && best.recipe != nullptr && best.recipe->script.find("{D}") != std::string::npos) {
				int budget = best.d_ps;
				double comb = std::max(0.05, budget * 1e-3);
				for (int iter = 0; iter < refine; iter++) {
					// slack relative to the delay budget of the logic
					double rel = best.res.wns / comb;
					if (meets(best) && rel < 0.03)
						break;
					int d_ps = std::max(1, int(best.d_ps * (1.0 + (meets(best) ? 0.7 : 1.2) * rel)));
					if (d_ps == best.d_ps)
						break;
					Candidate c = make_candidate(design, best_front, best.arch, best.recipe, d_ps);
					bool c_meets = meets(c);
					if (!consider(c) && (c_meets || !meets(best)))
						break;
				}
			}
			if (best_front != nullptr && best_front != module)
				design->remove(best_front);

			log("\nTiming driven mapping results for module %s", RTLIL::unescape_id(mod_name));
			if (have_period)
				log(" (period %.4f ns):\n", cfg.sta.period);
			else
				log(" (no period constraint, optimizing for speed):\n");
			log("  %-26s %10s %12s %10s %12s %8s\n", "candidate", "wns [ns]", "period [ns]", "fmax [MHz]", "area", "time [s]");
			bool marked = false;
			for (auto &c : all) {
				bool sel = !marked && c.name == best.name && c.res.area == best.res.area && c.res.wns == best.res.wns;
				marked |= sel;
				log("  %-26s %10.4f %12.4f %10.2f %12.2f %8.1f%s\n", c.name.c_str(), c.res.wns, c.res.min_period,
						c.res.min_period > 0 ? 1e3 / c.res.min_period : 0.0, c.res.area, c.runtime, sel ? "  <-- selected" : "");
			}
			log("\n");

			design->remove(module);
			design->rename(best.module, design->twines().add(mod_name));
			design->scratchpad_set_string("timing.result.wns", stringf("%.6f", best.res.wns));
			design->scratchpad_set_string("timing.result.tns", stringf("%.6f", best.res.tns));
			design->scratchpad_set_string("timing.result.min_period", stringf("%.6f", best.res.min_period));
			design->scratchpad_set_string("timing.result.area", stringf("%.6f", best.res.area));
			design->scratchpad_set_string("timing.result.candidate", best.name);
		}

	}
} TmapPass;

PRIVATE_NAMESPACE_END
