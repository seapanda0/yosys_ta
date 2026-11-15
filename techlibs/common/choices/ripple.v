/*
 *  yosys -- Yosys Open SYnthesis Suite
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

// Ripple carry chain: the smallest (and slowest) lookahead carry unit. Used
// by timing-aware synthesis for adders that are not timing critical.
(* techmap_celltype = "$lcu" *)
module _80_lcu_ripple (P, G, CI, CO);
	parameter WIDTH = 2;

	(* force_downto *)
	input [WIDTH-1:0] P, G;
	input CI;

	(* force_downto *)
	output [WIDTH-1:0] CO;

	integer i;

	(* force_downto *)
	reg [WIDTH-1:0] g;

	wire [1023:0] _TECHMAP_DO_ = "proc; opt -fast";

	always @* begin
		g = G;
		g[0] = g[0] | (P[0] & CI);
		for (i = 1; i < WIDTH; i = i + 1)
			g[i] = g[i] | (P[i] & g[i-1]);
	end

	assign CO = g;
endmodule
