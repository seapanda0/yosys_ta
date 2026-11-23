// 32x32 unsigned multiplier between registered inputs and a registered 64-bit product.
module mul32 (
    input  wire        clk,
    input  wire [31:0] a,
    input  wire [31:0] b,
    output reg  [63:0] p
);
    reg [31:0] a_r, b_r;

    always @(posedge clk) begin
        a_r <= a;
        b_r <= b;
        p   <= a_r * b_r;
    end
endmodule
