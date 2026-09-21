// ===========================================================================
// tb_rs_compare.sv -- run the RTL RS decoder on the exact vectors the C decoder
//                     just ran, then write the results to a file.
//
// The comparison is done outside by run_cosim.sh: the two files must be
// identical on the errors-only path.  If one byte differs, substituting the
// block onto the PS is NOT legitimate.
//
// ★ The RTL has no erasure port, so this testbench marks no erasures.
// ===========================================================================
`timescale 1ns/1ps

module tb_rs_compare;

    localparam int N   = 255;
    localparam int K   = 215;
    localparam int NCW = `NCW;

    logic       clk = 0, rst = 1;
    logic       i_start = 0, i_valid = 0;
    logic [7:0] i_data  = 8'h00;
    wire        i_ready;
    logic [8:0] i_k_cfg = K[8:0];
    wire  [7:0] o_data;
    wire        o_valid, o_last;
    logic       o_ready = 1'b1;
    wire        o_err_detected, o_err_corrected, o_fail;

    always #5 clk = ~clk;                       // 100 MHz

    rs_255_215_decoder dut (
        .clk(clk), .rst(rst),
        .i_start(i_start), .i_data(i_data), .i_valid(i_valid),
        .i_ready(i_ready), .i_k_cfg(i_k_cfg),
        .o_data(o_data), .o_valid(o_valid), .o_last(o_last), .o_ready(o_ready),
        .o_err_detected(o_err_detected),
        .o_err_corrected(o_err_corrected),
        .o_fail(o_fail)
    );

    logic [7:0] coded [0:NCW*N-1];
    integer     fo, ff, c, i, got;
    logic [7:0] msg [0:K-1];
    int         tong_sua, tong_fail;
    logic       thay_fail;

    initial begin
        $readmemh("coded_flat.hex", coded);
        fo = $fopen("rtl_out.hex", "w");
        ff = $fopen("rtl_flags.txt", "w");
        tong_sua = 0; tong_fail = 0;

        repeat (20) @(posedge clk);
        rst <= 1'b0;
        repeat (5) @(posedge clk);

        for (c = 0; c < NCW; c = c + 1) begin
            thay_fail = 1'b0;

            // --- push the 255 symbols in ---
            @(posedge clk);
            i_start <= 1'b1;
            @(posedge clk);
            i_start <= 1'b0;

            i = 0;
            while (i < N) begin
                i_data  <= coded[c*N + i];
                i_valid <= 1'b1;
                @(posedge clk);
                if (i_ready) i = i + 1;
            end
            i_valid <= 1'b0;
            i_data  <= 8'h00;

            // --- collect the 215 message bytes out ---
            got = 0;
            while (got < K) begin
                @(posedge clk);
                if (o_valid && o_ready) begin
                    msg[got] = o_data;
                    got = got + 1;
                end
                if (o_fail) thay_fail = 1'b1;
            end
            // drain the rest of the codeword (parity) if the RTL still emits it
            while (!o_last && got < N) begin
                @(posedge clk);
                if (o_valid && o_ready) got = got + 1;
                if (o_fail) thay_fail = 1'b1;
            end

            for (i = 0; i < K; i = i + 1) $fwrite(fo, "%02X", msg[i]);
            $fwrite(fo, "\n");
            if (thay_fail) tong_fail = tong_fail + 1;
            $fwrite(ff, "%0d %0d\n", c, thay_fail);

            repeat (64) @(posedge clk);
        end

        $fclose(fo); $fclose(ff);
        $display("TB-RS: ran %0d codewords, %0d reported failure", NCW, tong_fail);
        $finish;
    end

    // Safety latch: do not let the simulation hang if the RTL never returns data.
    initial begin
        // ★ Scaled to the codeword count: each codeword takes ~2000 cycles = 20 us.
        //   A fixed 20 ms latch only covers ~1000 codewords; a large run would be
        //   cut off mid-way.
        #(NCW * 40_000 + 1_000_000);
        $display("TB-RS: TIMEOUT -- the RTL did not return enough data");
        $fclose(fo); $fclose(ff);
        $finish;
    end

endmodule
