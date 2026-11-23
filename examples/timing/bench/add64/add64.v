// 64-bit adder with carry-in: registered inputs and registered 65-bit sum.
module add64 (
    input  wire        clk,
    input  wire [63:0] a,
    input  wire [63:0] b,
    input  wire        cin,
    output reg  [64:0] sum
);
    reg [63:0] a_r, b_r;
    reg        cin_r;

    always @(posedge clk) begin
        a_r   <= a;
        b_r   <= b;
        cin_r <= cin;
        sum   <= {1'b0, a_r} + {1'b0, b_r} + {64'd0, cin_r};
    end
endmodule
