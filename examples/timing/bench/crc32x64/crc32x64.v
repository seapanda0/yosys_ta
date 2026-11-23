// CRC-32 (polynomial 0x04C11DB7, MSB-first, non-reflected) updated with 64 data bits per cycle.
// The byte/bit update is a nested for-loop function. Synchronous reset sets the CRC to 0xFFFFFFFF.
module crc32x64 (
    input  wire        clk,
    input  wire        rst,
    input  wire        en,
    input  wire [63:0] din,
    output reg  [31:0] crc
);
    reg [63:0] din_r;
    reg        en_r;
    reg        rst_r;

    function [31:0] crc_update64;
        input [31:0] crc_in;
        input [63:0] data;
        integer b;
        integer k;
        reg [31:0] c;
        reg        fb;
        begin
            c = crc_in;
            for (b = 7; b >= 0; b = b - 1) begin
                for (k = 7; k >= 0; k = k - 1) begin
                    fb = c[31] ^ data[b*8 + k];
                    c  = {c[30:0], 1'b0};
                    if (fb)
                        c = c ^ 32'h04C11DB7;
                end
            end
            crc_update64 = c;
        end
    endfunction

    always @(posedge clk) begin
        din_r <= din;
        en_r  <= en;
        rst_r <= rst;
        if (rst_r)
            crc <= 32'hFFFFFFFF;
        else if (en_r)
            crc <= crc_update64(crc, din_r);
    end
endmodule
