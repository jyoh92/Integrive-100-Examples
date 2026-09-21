// ===========================================================================
// ps_capture.v -- nua BAT cua mot span: luu luong PL di qua ranh gioi ra, de
//                 PS doc lai qua cong thanh ghi.
//
//        khoi truoc ──pl──►┬──► khoi sau (hoac ps_inject)
//                          └──► [ ps_capture ] ──rd_data──► PS
//
// Ghi lien tuc khi arm = 1, dung khi day hoac khi arm = 0.  PS doc bang cach
// dat rd_addr roi lay rd_data o chu ky sau.
//
// ★ Do rong W tach khoi ps_inject: hai dau cua mot span co the khac kieu.
//   Vi du span (B4,B5): bat mau PHUC 32 bit o B4, chen BYTE o B5.
// ===========================================================================
`timescale 1ns/1ps

module ps_capture #(
    parameter integer W     = 32,
    parameter integer DEPTH = 4096
)(
    input  wire              clk,
    input  wire              rst,

    input  wire              arm,       // 1 = cho phep ghi
    input  wire              clr,       // dat lai con tro ghi

    input  wire [W-1:0]      pl_data,
    input  wire              pl_valid,

    input  wire [15:0]       rd_addr,
    output reg  [W-1:0]      rd_data,
    output wire [15:0]       count      // so phan tu da bat
);

    localparam integer AW = (DEPTH <= 2) ? 1 : $clog2(DEPTH);

    (* ram_style = "block" *) reg [W-1:0] mem [0:DEPTH-1];
    reg [AW-1:0] wp;
    wire         day = (wp == {AW{1'b1}});

    always @(posedge clk) begin
        if (rst || clr) begin
            wp <= {AW{1'b0}};
        end else if (arm && pl_valid && !day) begin
            mem[wp] <= pl_data;
            wp      <= wp + {{(AW-1){1'b0}}, 1'b1};
        end
    end

    always @(posedge clk)
        rd_data <= mem[rd_addr[AW-1:0]];

    assign count = {{(16-AW){1'b0}}, wp};

endmodule
