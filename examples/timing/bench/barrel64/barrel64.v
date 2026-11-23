// 64-bit rotate/shift unit. mode: 0 rotate-left, 1 rotate-right, 2 shift-left (logical),
// 3 shift-right (logical). amt is a 6-bit amount. Registered inputs and output.
module barrel64 (
    input  wire        clk,
    input  wire [63:0] x,
    input  wire [5:0]  amt,
    input  wire [1:0]  mode,
    output reg  [63:0] y
);
    reg [63:0] x_r;
    reg [5:0]  amt_r;
    reg [1:0]  mode_r;
    reg [63:0] res_c;
    wire [6:0] amt_comp = 7'd64 - {1'b0, amt_r};

    always @* begin
        case (mode_r)
            2'd0: res_c = (x_r << amt_r) | (x_r >> amt_comp);
            2'd1: res_c = (x_r >> amt_r) | (x_r << amt_comp);
            2'd2: res_c = x_r << amt_r;
            default: res_c = x_r >> amt_r;
        endcase
    end

    always @(posedge clk) begin
        x_r    <= x;
        amt_r  <= amt;
        mode_r <= mode;
        y      <= res_c;
    end
endmodule
