// Maximum of sixteen 16-bit unsigned values (packed 256-bit bus) and its 4-bit index.
// Input and output registered; the comparison is a 4-level binary reduction tree.
module cmp_tree (
    input  wire         clk,
    input  wire [255:0] din,
    output reg  [15:0]  max_o,
    output reg  [3:0]   idx_o
);
    reg [255:0] din_r;

    // Each item is {value[15:0], index[3:0]} = 20 bits. Level 0: items 0..15,
    // level 1: 16..23, level 2: 24..27, level 3: 28..29, level 4 (root): 30.
    reg [31*20-1:0] t;
    integer j;

    function [19:0] mx2;
        input [19:0] a;
        input [19:0] b;
        begin
            mx2 = (b[19:4] > a[19:4]) ? b : a;
        end
    endfunction

    always @* begin
        for (j = 0; j < 16; j = j + 1)
            t[j*20 +: 20] = {din_r[j*16 +: 16], j[3:0]};
        for (j = 0; j < 8; j = j + 1)
            t[(16+j)*20 +: 20] = mx2(t[(2*j)*20 +: 20], t[(2*j+1)*20 +: 20]);
        for (j = 0; j < 4; j = j + 1)
            t[(24+j)*20 +: 20] = mx2(t[(16+2*j)*20 +: 20], t[(17+2*j)*20 +: 20]);
        for (j = 0; j < 2; j = j + 1)
            t[(28+j)*20 +: 20] = mx2(t[(24+2*j)*20 +: 20], t[(25+2*j)*20 +: 20]);
        t[30*20 +: 20] = mx2(t[28*20 +: 20], t[29*20 +: 20]);
    end

    always @(posedge clk) begin
        din_r <= din;
        max_o <= t[30*20+4 +: 16];
        idx_o <= t[30*20 +: 4];
    end
endmodule
