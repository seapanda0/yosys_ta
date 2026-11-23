// 8-tap FIR filter: 12-bit signed samples, fixed 10-bit signed coefficients.
// Delay line (shift register) followed by one combinational sum-of-products stage
// and a full-precision (25-bit signed) registered output.
module fir8 (
    input  wire               clk,
    input  wire signed [11:0] x,
    output reg  signed [24:0] y
);
    localparam signed [9:0] C0 = 10'sd23;
    localparam signed [9:0] C1 = -10'sd57;
    localparam signed [9:0] C2 = 10'sd101;
    localparam signed [9:0] C3 = 10'sd180;
    localparam signed [9:0] C4 = -10'sd150;
    localparam signed [9:0] C5 = 10'sd77;
    localparam signed [9:0] C6 = -10'sd33;
    localparam signed [9:0] C7 = 10'sd9;

    wire [79:0] COEFS = {C7, C6, C5, C4, C3, C2, C1, C0};

    reg [95:0] dl;          // dl[12*i +: 12] is the sample delayed by i cycles
    reg signed [24:0] acc;  // combinational sum of products
    integer i;

    always @* begin
        acc = 25'sd0;
        for (i = 0; i < 8; i = i + 1)
            acc = acc + ($signed(dl[i*12 +: 12]) * $signed(COEFS[i*10 +: 10]));
    end

    always @(posedge clk) begin
        dl <= {dl[83:0], x};
        y  <= acc;
    end
endmodule
