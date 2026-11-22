// 32-bit ALU with 4-bit opcode, registered inputs and registered outputs.
// op: 0 add, 1 sub, 2 and, 3 or, 4 xor, 5 slt, 6 sltu, 7 sll, 8 srl, 9 sra; others give 0.
module alu32 (
    input  wire        clk,
    input  wire [3:0]  op,
    input  wire [31:0] a,
    input  wire [31:0] b,
    output reg  [31:0] result,
    output reg         zero
);
    reg [3:0]  op_r;
    reg [31:0] a_r, b_r;
    reg [31:0] res_c;

    always @* begin
        case (op_r)
            4'd0: res_c = a_r + b_r;
            4'd1: res_c = a_r - b_r;
            4'd2: res_c = a_r & b_r;
            4'd3: res_c = a_r | b_r;
            4'd4: res_c = a_r ^ b_r;
            4'd5: res_c = ($signed(a_r) < $signed(b_r)) ? 32'd1 : 32'd0;
            4'd6: res_c = (a_r < b_r) ? 32'd1 : 32'd0;
            4'd7: res_c = a_r << b_r[4:0];
            4'd8: res_c = a_r >> b_r[4:0];
            4'd9: res_c = $signed(a_r) >>> b_r[4:0];
            default: res_c = 32'd0;
        endcase
    end

    always @(posedge clk) begin
        op_r   <= op;
        a_r    <= a;
        b_r    <= b;
        result <= res_c;
        zero   <= (res_c == 32'd0);
    end
endmodule
