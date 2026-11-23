// Multiply-accumulate: acc <= acc + a*b (16-bit operands, 40-bit accumulator), synchronous clear.
module mac16 (
    input  wire        clk,
    input  wire        clr,
    input  wire [15:0] a,
    input  wire [15:0] b,
    output reg  [39:0] acc
);
    reg [15:0] a_r, b_r;
    reg        clr_r;

    always @(posedge clk) begin
        a_r   <= a;
        b_r   <= b;
        clr_r <= clr;
        if (clr_r)
            acc <= 40'd0;
        else
            acc <= acc + (a_r * b_r);
    end
endmodule
