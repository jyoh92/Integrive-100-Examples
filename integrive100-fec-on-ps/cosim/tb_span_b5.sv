// ===========================================================================
// tb_span_b5.sv -- prove span (B4,B5) works: the DEMODULATOR block is fully
//                  replaced by data from the PS.
//
// This test supplies NO PAM decision (dec_valid held 0), i.e. the PL path from
// the equalizer to the demodulator produces nothing.  All 765 coded bytes are
// written by the PS through ps_ctl, then `go` is pulsed.  If the RS decoder
// still emits the correct 645 message bytes, the demodulator HAS been replaced
// by software.
//
// This is the definition of Guide 6A: data leaves the pipeline at one boundary,
// the ARM processes it, and it re-enters at the next boundary -- every other
// block still runs in fabric.
// ===========================================================================
`timescale 1ns/1ps

module tb_span_b5;

    localparam int NCW   = 3;
    localparam int N     = 255;
    localparam int K     = 215;
    localparam int TOTIN = NCW*N;      // 765 coded bytes
    localparam int TOTOUT= NCW*K;      // 645 message bytes

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

    v39ct_mm_decode #(.NGROUP_MAX(10), .N(N), .NCW(NCW), .K(K)) dut (
        .ps_ctl(ps_ctl),
        .clk(clk), .rst(rst), .n_bpsc(4'd8), .n_rsgroup(5'd1), .start(start),
        .dec_valid(dec_valid), .dec_i(dec_i), .dec_q(dec_q), .dec_last(dec_last),
        .msg_valid(msg_valid), .msg_byte(msg_byte), .msg_cw(msg_cw),
        .msg_pos(msg_pos), .msg_group(msg_group),
        .sym_err_seen(sym_err_seen), .grp_fail(grp_fail)
    );

    logic [7:0] coded  [0:TOTIN-1];
    logic [7:0] expected_v [0:TOTOUT-1];
    logic [7:0] got    [0:2047];
    int n_got = 0, err_cnt = 0;

    always @(posedge clk)
        if (msg_valid && n_got < 2048) begin got[n_got] = msg_byte; n_got++; end

    // Write one element into the return buffer: set the data, then FLIP wr_tog.
    task ghi(input logic [7:0] b);
        @(negedge clk);
        ps_ctl[39:32] = b;
        ps_ctl[3]     = ~ps_ctl[3];
    endtask

    initial begin
        $readmemh("b5_bytes.hex",  coded);
        $readmemh("b5_expect.hex", expected_v);

        repeat (8) @(negedge clk); rst = 1'b0; repeat (4) @(negedge clk);

        // ---- enable the span at boundary B5 -----------------------------
        ps_ctl[6:4] = 3'd5;      // bnd = B5
        ps_ctl[0]   = 1'b1;      // en
        @(negedge clk);
        ps_ctl[1] = 1'b1; @(negedge clk); ps_ctl[1] = 1'b0;   // ptr_rst

        // ---- PS writes all 765 coded bytes -------------------------------
        for (int i = 0; i < TOTIN; i++) ghi(coded[i]);
        @(negedge clk);

        // ---- pulse `go`: emit downstream to the RS decoder ---------------
        start = 1'b1; @(negedge clk); start = 1'b0;
        ps_ctl[31:16] = TOTIN[15:0];
        @(negedge clk); ps_ctl[2] = 1'b1; @(negedge clk); ps_ctl[2] = 1'b0;

        // ---- wait for the message out ------------------------------------
        repeat (60000) @(posedge clk);

        $display("  message bytes received : %0d  (expected %0d)", n_got, TOTOUT);
        if (n_got != TOTOUT) err_cnt++;
        else begin
            int wrong = 0;
            for (int i = 0; i < TOTOUT; i++) if (got[i] !== expected_v[i]) wrong++;
            if (wrong) begin
                $display("  %0d/%0d bytes WRONG", wrong, TOTOUT); err_cnt++;
            end else
                $display("  all %0d bytes MATCH the original message", TOTOUT);
        end
        $display("  grp_fail = %b   sym_err_seen = %b", grp_fail, sym_err_seen);

        if (err_cnt) $display("TB-SPAN-B5: FAILED");
        else     $display("TB-SPAN-B5: PASSED -- the demodulator ran on the PS");
        $finish;
    end

    initial begin #3_000_000; $display("TB-SPAN-B5: TIMEOUT (n_got=%0d)", n_got); $finish; end
endmodule
