/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Timing-aware synthesis: timing driven gate sizing and buffering
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

#ifndef TIMING_OPT_H
#define TIMING_OPT_H

#include "passes/timing/timing_sta.h"

YOSYS_NAMESPACE_BEGIN

namespace Timing {

struct SizerOptions {
	int max_iter = 0;       // maximum number of timing moves (0: automatic)
	double max_time = 0;    // time limit in seconds for the timing phase (0: none)
	double max_tran = 0;    // maximum transition time in ns (0: library default)
	int lookahead = 2;      // number of additional moves that may follow a move that does not pay off on its own
	bool resize = true;     // allow cell resizing
	bool buffer = true;     // allow buffer insertion
	bool repair = true;     // fix max capacitance / max transition violations
	bool area = true;       // recover area on paths with positive slack
	bool verbose = false;
	bool selfcheck = false; // compare every incremental timing update against a full analysis
};

struct SizerResult {
	double wns = 0, tns = 0, min_period = 0, area = 0;
	int n_resize = 0, n_buffer = 0, n_downsize = 0, n_repair = 0;
};

// Timing driven sizing and buffering of a mapped module. With a clock period
// the optimization stops once all paths meet timing and then recovers area;
// without one (period 0) it minimizes the critical path delay.
SizerResult timing_optimize(RTLIL::Module *module, const Library *lib, const StaOptions &sta_opt, const SizerOptions &opt);

} // namespace Timing

YOSYS_NAMESPACE_END

#endif
