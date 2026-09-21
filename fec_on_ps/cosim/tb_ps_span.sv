// ===========================================================================
// tb_ps_span.sv -- check that ps_tap does four things right:
//   1. en=0  -> straight wire, no added latency, data unchanged
//   2. en=1, before `go` -> the downstream block receives NOTHING (pipeline stalls)
//   3. after `go` -> the downstream block receives EXACTLY what the PS wrote, in
//                    order, the right count
//   4. the copy for the PS always mirrors the PL stream, even while the span is on
//
// Run with W = 32 to also cover the complex-sample case at B2/B3/B4, not just
// the byte case of the existing span (B5,B6).
// ===========================================================================
`timescale 1ns/1ps

module tb_ps_span;

    localparam int W = 32;
    localparam int N = 64;

    logic clk = 0, rst = 1;
    logic en = 0, ptr_rst = 0, go = 0, wr_tog = 0, pl_valid = 0;
    logic [15:0]  len = 0;
    logic [W-1:0] wr_data = 0, pl_data = 0;
    wire  [W-1:0] out_data, cap_data;
    wire          out_valid, cap_valid;

    int err_cnt = 0;
    logic [W-1:0] recv [0:1023];
    int  n_nhan = 0, n_cap = 0;

    always #5 clk = ~clk;

    wire busy;
    ps_inject #(.W(W), .DEPTH(1024)) u_inj (
        .clk(clk), .rst(rst), .en(en), .ptr_rst(ptr_rst), .go(go), .len(len),
        .wr_data(wr_data), .wr_tog(wr_tog),
        .pl_data(pl_data), .pl_valid(pl_valid),
        .out_data(out_data), .out_valid(out_valid), .busy(busy)
    );

    // The capture half runs in parallel with its OWN width -- at span (B4,B5)
    // this is a 32-bit complex sample while the inject half is a byte.
    logic [15:0] rd_addr = 0;
    wire  [31:0] cap_rd;
    wire  [15:0] cap_n;
    ps_capture #(.W(32), .DEPTH(1024)) u_cap (
        .clk(clk), .rst(rst), .arm(1'b1), .clr(1'b0),
        .pl_data(pl_data[31:0]), .pl_valid(pl_valid),
        .rd_addr(rd_addr), .rd_data(cap_rd), .count(cap_n)
    );
    assign cap_data  = cap_rd;
    assign cap_valid = pl_valid;

    always @(posedge clk) begin
        if (out_valid) begin recv[n_nhan] = out_data; n_nhan++; end
        if (cap_valid) n_cap++;
    end

    task pl_stream(input int n, input logic [W-1:0] goc);
        for (int i = 0; i < n; i++) begin
            @(negedge clk); pl_data = goc + i; pl_valid = 1'b1;
        end
        @(negedge clk); pl_valid = 1'b0;
    endtask

    task ps_write(input int n, input logic [W-1:0] goc);
        @(negedge clk); ptr_rst = 1'b1; @(negedge clk); ptr_rst = 1'b0;
        for (int i = 0; i < n; i++) begin
            @(negedge clk); wr_data = goc + i; wr_tog = ~wr_tog;
        end
        @(negedge clk);
    endtask

    initial begin
        repeat (4) @(negedge clk); rst = 1'b0; repeat (2) @(negedge clk);

        // ---- 1. en = 0: straight wire -------------------------------------
        n_nhan = 0; n_cap = 0; en = 1'b0;
        pl_stream(N, 32'h1000);
        if (n_nhan != N) begin
            $display("  1. FAILED: downstream received %0d, expected %0d", n_nhan, N); err_cnt++;
        end else begin
            for (int i = 0; i < N; i++)
                if (recv[i] !== 32'h1000 + i) begin
                    $display("  1. FAILED: element %0d = %h, expected %h",
                             i, recv[i], 32'h1000 + i); err_cnt++; break;
                end
            if (err_cnt == 0) $display("  1. en=0 straight wire: %0d elements correct", N);
        end
        if (n_cap != N) begin $display("  1b. capture copy mismatch: %0d", n_cap); err_cnt++; end

        // ---- 2. en = 1, before go: downstream must be SILENT -----------------
        n_nhan = 0; n_cap = 0; en = 1'b1;
        pl_stream(N, 32'h2000);
        if (n_nhan != 0) begin
            $display("  2. FAILED: pipeline did NOT stall, %0d elements leaked through", n_nhan); err_cnt++;
        end else $display("  2. en=1 before go: downstream silent -- the pipeline STALLED");
        // ★ REAL check: read the capture buffer back and compare every element.
        //   An earlier version only counted cap_valid -- i.e. checked a wire, not
        //   the memory.
        if (cap_n != 2*N) begin
            $display("  2b. FAILED: capture count %0d, expected %0d", cap_n, 2*N);
            err_cnt++;
        end else begin
            int wrong = 0;
            for (int i = 0; i < 2*N; i++) begin
                @(negedge clk); rd_addr = i[15:0];
                @(posedge clk); @(negedge clk);
                if (cap_rd !== ((i < N) ? (32'h1000 + i) : (32'h2000 + (i-N))))
                    wrong++;
            end
            if (wrong) begin
                $display("  2b. FAILED: %0d/%0d captured elements wrong", wrong, 2*N);
                err_cnt++;
            end else
                $display("  2b. capture: read back %0d elements, all correct", 2*N);
        end

        // ---- 3. PS writes then go: downstream gets exactly what the PS wrote --
        ps_write(N, 32'hA000);
        n_nhan = 0;
        @(negedge clk); len = N; go = 1'b1; @(negedge clk); go = 1'b0;
        repeat (N + 20) @(posedge clk);
        if (n_nhan != N) begin
            $display("  3. FAILED: emitted %0d elements, expected %0d", n_nhan, N); err_cnt++;
        end else begin
            int wrong = 0;
            for (int i = 0; i < N; i++)
                if (recv[i] !== 32'hA000 + i) wrong++;
            if (wrong) begin
                $display("  3. FAILED: %0d elements wrong", wrong); err_cnt++;
            end else
                $display("  3. after go: downstream received the PS's %0d elements correctly", N);
        end

        // ---- 4. back to the PL path -----------------------------------------
        n_nhan = 0; en = 1'b0;
        pl_stream(16, 32'h3000);
        if (n_nhan != 16) begin
            $display("  4. FAILED: PL path did not reconnect, received %0d/16", n_nhan); err_cnt++;
        end else $display("  4. back to PL path: reconnects immediately");

        // ★ Do NOT use the ternary operator on STRINGS here: SystemVerilog treats
        //   two strings as a NUMERIC expression and prints garbage (bitten once).
        if (err_cnt) $display("TB-PSTAP: FAILED (%0d errors)", err_cnt);
        else     $display("TB-PSTAP: PASSED");
        $finish;
    end

    initial begin #200000; $display("TB-PSTAP: TIMEOUT"); $finish; end
endmodule
