// ===========================================================================
// ps_inject.v -- the INJECT half of a span: stall the pipeline at the entry
//                boundary, then play back the data returned by the PS in place
//                of the PL data.
//
//        upstream block ──pl──► [ ps_inject ] ──out──► downstream block
//                                     ▲
//                                wr_* from PS
//
// en = 0 : straight pass-through, out = pl, no added latency.
// en = 1 : out is SILENT until `go` rises, then plays exactly `len` elements
//          from the PS-written buffer.  That is where the pipeline STALLS.
//
// ★ Kept separate from the CAPTURE half (ps_capture) because the two ends of a
//   span may have different data types: span (B4,B5) takes a 32-bit COMPLEX
//   sample and returns a BYTE.  Merging both halves into one width would only
//   work for a span whose two ends share the same type.
//
// ★ Writing one element = TOGGLE `wr_tog` then set `wr_data`.  A toggled bit,
//   not an AXI write strobe: the PS side only sees a register, not a write
//   pulse, and writing the same value twice must still count as two elements.
// ===========================================================================
`timescale 1ns/1ps

module ps_inject #(
    parameter integer W     = 8,
    parameter integer DEPTH = 8192
)(
    input  wire             clk,
    input  wire             rst,

    input  wire             en,
    input  wire             ptr_rst,
    input  wire             go,
    input  wire [15:0]      len,
    input  wire [W-1:0]     wr_data,
    input  wire             wr_tog,

    input  wire [W-1:0]     pl_data,
    input  wire             pl_valid,

    output wire [W-1:0]     out_data,
    output wire             out_valid,
    output wire             busy        // currently playing the buffer downstream
);

    localparam integer AW = (DEPTH <= 2) ? 1 : $clog2(DEPTH);

    (* ram_style = "block" *) reg [W-1:0] mem [0:DEPTH-1];
    reg [AW-1:0] wp, rp;
    reg          tog_d, go_d, run, v;
    reg [W-1:0]  d;

    always @(posedge clk) begin
        if (rst) begin
            wp <= {AW{1'b0}}; tog_d <= 1'b0;
        end else begin
            tog_d <= wr_tog;
            if (ptr_rst)              wp <= {AW{1'b0}};
            else if (wr_tog != tog_d) begin
                mem[wp] <= wr_data;
                wp      <= wp + {{(AW-1){1'b0}}, 1'b1};
            end
        end
    end

    always @(posedge clk) begin
        if (rst) begin
            rp <= {AW{1'b0}}; run <= 1'b0; v <= 1'b0; go_d <= 1'b0;
        end else begin
            go_d <= go;
            v    <= 1'b0;
            if (go & ~go_d) begin
                rp  <= {AW{1'b0}};
                run <= (len != 16'd0);
            end else if (run) begin
                d  <= mem[rp];
                v  <= 1'b1;
                rp <= rp + {{(AW-1){1'b0}}, 1'b1};
                // ★ 17-bit widened compare: adding 1 on 16 bits would wrap to 0
                //   at len = 65535 and the playback loop would never stop.
                if ({1'b0, {{(17-AW){1'b0}}, rp}} + 17'd1 >= {1'b0, len})
                    run <= 1'b0;
            end
        end
    end

    // ★ GUARD: requesting more elements than the buffer holds means it has
    //   wrapped and the earliest data was overwritten.  Silently wrong -- the
    //   16-09-2026 measurement gave 5 bad groups at the start, 5 good at the
    //   end.  Report it loudly in simulation.
    // synthesis translate_off
    always @(posedge clk)
        if (!rst && go && !go_d && len > DEPTH)
            $display("ps_inject: LEN %0d EXCEEDS DEPTH %0d -- buffer has wrapped",
                     len, DEPTH);
    // synthesis translate_on

    assign out_data  = en ? d   : pl_data;
    assign out_valid = en ? v   : pl_valid;
    assign busy      = en & run;

endmodule
