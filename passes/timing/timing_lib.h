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

#ifndef TIMING_LIB_H
#define TIMING_LIB_H

#include "kernel/yosys.h"

YOSYS_NAMESPACE_BEGIN

namespace Timing {

enum { RISE = 0, FALL = 1 };

// name of an RTLIL object without the escape character, for log messages
template <typename T> inline std::string log_id(const T *obj) { return obj->name.unescape(); }

// A 0-, 1- or 2-dimensional NLDM lookup table. All values are normalized
// to ns and pF when the library is loaded.
struct Table {
	enum Var : char {
		NONE,
		SLEW,  // input_net_transition / related_pin_transition
		LOAD,  // total_output_net_capacitance
		CSLEW  // constrained_pin_transition
	};
	Var var1 = NONE, var2 = NONE;
	std::vector<double> idx1, idx2, values;

	bool valid() const { return !values.empty(); }

	// bilinear interpolation, linear extrapolation outside of the table
	double lookup(double slew, double load, double cslew = 0) const;
};

struct Arc {
	enum Type : char {
		COMB,    // combinational (also latch D->Q)
		CLK,     // clock to output (rising_edge / falling_edge)
		SETUP    // setup or recovery check (stored in Cell::checks)
	};
	enum Sense : char { POS, NEG, NON };

	int from = -1; // related pin
	int to = -1;   // pin the timing group belongs to
	Type type = COMB;
	Sense sense = NON;
	bool falling_clock = false;

	// indexed by the transition (RISE/FALL) of the 'to' pin
	Table delay[2], trans[2], constraint[2];
};

// Library cells and pins are identified by their plain liberty names (no
// RTLIL escaping): IdStrings belong to the name pool of a design, while a
// library is shared between designs.
struct Pin {
	std::string name;
	bool input = false, output = false, clock = false;
	bool comb_from = false; // has a combinational arc to some output
	double cap[2] = {0, 0};
	double max_cap = 0;
	double max_tran = 0;
	std::string function;
};

struct Cell {
	std::string name;
	double area = 0;
	std::vector<Pin> pins;
	dict<std::string, int> pin_idx;
	std::vector<Arc> arcs;   // delay arcs
	std::vector<Arc> checks; // setup / recovery checks
	bool seq = false;
	bool dont_use = false;
	bool is_buf = false;
	int buf_in = -1, buf_out = -1;

	// Cells that are functionally identical and pin compatible form a family
	// (e.g. nand2_1, nand2_2, nand2_4), sorted by increasing drive strength.
	int family = -1;
	int size_idx = -1;
	// drive resistance estimate (ns/pF), used to order the family
	double drive_res = 0;

	int pin(const std::string &n) const {
		auto it = pin_idx.find(n);
		return it == pin_idx.end() ? -1 : it->second;
	}
};

struct Library {
	std::vector<std::unique_ptr<Cell>> cell_store;
	dict<std::string, Cell *> cells;
	std::vector<std::vector<Cell *>> families;
	std::vector<Cell *> buffers; // usable (not dont_use) buffers, weakest first
	double default_max_tran = 0;

	const Cell *find(const std::string &type) const {
		auto it = cells.find(type);
		return it == cells.end() ? nullptr : it->second;
	}

	// Loads (and caches) the timing view of the given liberty files.
	static std::shared_ptr<Library> get(const std::vector<std::string> &files, const std::vector<std::string> &dont_use);
};

} // namespace Timing

YOSYS_NAMESPACE_END

#endif
