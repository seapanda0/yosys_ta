// 16x16 unsigned multiplier between registered inputs and a registered 32-bit product.
module mul16 (
    input  wire        clk,
    input  wire [15:0] a,
    input  wire [15:0] b,
    output reg  [31:0] p
);
    reg [15:0] a_r, b_r;

    always @(posedge clk) begin
        a_r <= a;
        b_r <= b;
        p   <= a_r * b_r;
    end
endmodule
