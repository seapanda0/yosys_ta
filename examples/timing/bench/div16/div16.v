// 16/16 unsigned restoring divider (combinational for-loop in always @*),
// with registered inputs and registered quotient and remainder.
module div16 (
    input  wire        clk,
    input  wire [15:0] dividend,
    input  wire [15:0] divisor,
    output reg  [15:0] quotient,
    output reg  [15:0] remainder
);
    reg [15:0] dvd_r, dvs_r;
    reg [15:0] quo_c, rem_c;
    reg [16:0] rem;
    integer    i;

    always @* begin
        rem   = 17'd0;
        quo_c = 16'd0;
        for (i = 15; i >= 0; i = i - 1) begin
            rem = {rem[15:0], dvd_r[i]};
            if (rem >= {1'b0, dvs_r}) begin
                rem     = rem - {1'b0, dvs_r};
                quo_c[i] = 1'b1;
            end
        end
        rem_c = rem[15:0];
    end

    always @(posedge clk) begin
        dvd_r     <= dividend;
        dvs_r     <= divisor;
        quotient  <= quo_c;
        remainder <= rem_c;
    end
endmodule
