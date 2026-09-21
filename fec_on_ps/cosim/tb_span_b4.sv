// ===========================================================================
// tb_span_b4.sv -- prove span (B3,B4): the CHANNEL PROCESSING block runs on the PS.
//
// The test supplies no PAM decision from the PL path (dec_valid held 0).
// All 7650 decisions -- which are the REAL B4 stream, captured from a full PHY
// run -- are written by the PS through ps_ctl and emitted with `go`.
//
// If the 6450 message bytes come out correct, then everything BEFORE B4 (sync,
// FFT, bin extraction, channel estimation, equalization) has been replaced by
// software, while the demodulator and the RS decoder still run in fabric.
// ===========================================================================
`timescale 1ns/1ps

module tb_span_b4;

    localparam int NCW    = 3;
    localparam int N      = 255;
    localparam int K      = 215;
    localparam int NGRP   = 10;              // 10 RS groups per 256-QAM frame
    localparam int NSC    = NGRP*NCW*N;      // 7650 subcarriers
    localparam int TOTOUT = NGRP*NCW*K;      // 6450 message bytes

    logic clk = 0, rst = 1;
    always #5 clk = ~clk;

    logic [63:0] ps_ctl = 64'd0;
    logic        start = 0, dec_valid = 0, dec_last = 0;
    logic signed [7:0] dec_i = 0, dec_q = 0;

    wire        msg_valid; wire [7:0] msg_byte;
    wire [1:0]  msg_cw;    wire [8:0] msg_pos;
    wire [4:0]  msg_group;
    wire        sym_err_seen;
    wire [9:0]  grp_fail;
    wire        b4_valid_o; wire [7:0] b4_byte_o;

    v39ct_mm_decode #(.NGROUP_MAX(NGRP), .N(N), .NCW(NCW), .K(K)) dut (
        .ps_ctl(ps_ctl), .b4_valid_o(b4_valid_o), .b4_byte_o(b4_byte_o),
        .clk(clk), .rst(rst), .n_bpsc(4'd8), .n_rsgroup(5'd10), .start(start),
        .dec_valid(dec_valid), .dec_i(dec_i), .dec_q(dec_q), .dec_last(dec_last),
        .msg_valid(msg_valid), .msg_byte(msg_byte), .msg_cw(msg_cw),
        .msg_pos(msg_pos), .msg_group(msg_group),
        .sym_err_seen(sym_err_seen), .grp_fail(grp_fail)
    );

    logic [15:0] b4      [0:NSC-1];
    logic [7:0]  expected_v[0:TOTOUT-1];
    logic [7:0]  got     [0:8191];
    int n_got = 0, err_cnt = 0;

    always @(posedge clk)
        if (msg_valid && n_got < 8192) begin got[n_got] = msg_byte; n_got++; end

    task ghi16(input logic [15:0] v);
        @(negedge clk);
        ps_ctl[47:32] = v;
        ps_ctl[3]     = ~ps_ctl[3];
    endtask

    initial begin
        $readmemh("b4_pl.hex",  b4);
        $readmemh("b4_msg.hex", expected_v);

        repeat (8) @(negedge clk); rst = 1'b0; repeat (4) @(negedge clk);

        ps_ctl[6:4] = 3'd4;          // bnd = B4
        ps_ctl[0]   = 1'b1;          // en
        @(negedge clk);
        ps_ctl[1] = 1'b1; @(negedge clk); ps_ctl[1] = 1'b0;

        for (int i = 0; i < NSC; i++) ghi16(b4[i]);
        @(negedge clk);

        start = 1'b1; @(negedge clk); start = 1'b0;
        ps_ctl[31:16] = NSC[15:0];
        @(negedge clk); ps_ctl[2] = 1'b1; @(negedge clk); ps_ctl[2] = 1'b0;

        repeat (400000) @(posedge clk);

        $display("  message bytes received : %0d  (expected %0d)", n_got, TOTOUT);
        if (n_got != TOTOUT) err_cnt++;
        else begin
            int wrong = 0;
            for (int i = 0; i < TOTOUT; i++) if (got[i] !== expected_v[i]) wrong++;
            if (wrong) begin
                int dau = -1, theo_nhom[10];
                for (int g = 0; g < 10; g++) theo_nhom[g] = 0;
                for (int i = 0; i < TOTOUT; i++)
                    if (got[i] !== expected_v[i]) begin
                        if (dau < 0) dau = i;
                        theo_nhom[i/645]++;
                    end
                $display("  %0d/%0d bytes WRONG, first mismatch at %0d", wrong, TOTOUT, dau);
                $write("  wrong per 645-byte group:");
                for (int g = 0; g < 10; g++) $write(" %0d", theo_nhom[g]);
                $display("");
                $write("  got[0..7]      :"); for (int i=0;i<8;i++) $write(" %02x", got[i]); $display("");
                $write("  expected[0..7] :"); for (int i=0;i<8;i++) $write(" %02x", expected_v[i]); $display("");
                $write("  expected[3225..]:"); for (int i=3225;i<3233;i++) $write(" %02x", expected_v[i]); $display("");
                $write("  got[3225..]    :"); for (int i=3225;i<3233;i++) $write(" %02x", got[i]); $display("");
                err_cnt++;
            end else
                $display("  all %0d bytes MATCH the original message", TOTOUT);
        end
        $display("  grp_fail = %b", grp_fail);

        if (err_cnt) $display("TB-SPAN-B4: FAILED");
        else     $display("TB-SPAN-B4: PASSED -- channel processing ran on the PS");
        $finish;
    end

    initial begin #20_000_000; $display("TB-SPAN-B4: TIMEOUT (n_got=%0d)", n_got); $finish; end
endmodule
