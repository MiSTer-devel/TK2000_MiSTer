//
// TK2000 tape deck: CT2 player + deck control + turbo request + status bar /
// next-step hint overlay. Shared by TK2000.sv and the Verilator sim (sim.v).
// Follows ~/dev2/tape-standard.md (reference: Sharp X1 / Basic Master Jr).
//
// Deck. The TK2000 has no motor control in LOAD, so the deck is a PLAY key:
// a mount or Rewind leaves the tape rewound and stopped, and it starts by
// itself the moment the ROM's tape-bit routine (RDBIT, $FCFA-$FD0B) runs
// while the tape is at its start - the user typed LOADT. OSD Tape play/stop
// pauses and resumes it by hand (Karateka: "stop the recorder when PLANSOFT
// appears").
//
// Turbo (OSD Tape speed 7x). The ROM times tape bits with CPU instruction
// loops and the player counts its pulses in CPU cycles, so running the CPU
// 7x faster (tk2000.v: a CPU cycle every 2 clocks of 14.318 MHz instead of
// 14) plays the tape 7x faster with exactly the same pulses in CPU time.
// turbo_req is only set while the deck plays; TK2000.sv also drops it while
// a disk drive runs (the Disk II timing is tied to the 1 MHz bus).
//
// Status bar (rules 19-24, but in the top-left corner: the TK2000 ROM prints
// its own NAME/blocks/WAIT line on the bottom row while loading): while the tape plays,
//     TAPE > 0:09/ 4:51 x7
//     [=======-----------]
// elapsed / total tape time (CPU cycles of tape time / 1.0227 MHz; total is
// computed from the CT2 chunks while the image downloads), the turbo factor
// while turbo is active, and a 64-step bar. Hint (rules 25-28): while the
// tape is loaded, rewound and stopped, the box says TYPE LOADT <ret> - the
// TK2000 command that loads the next file from tape, the same for every CT2;
// it goes away when Return is pressed. Everything runs on clk_sys, so no
// clock-domain crossing is needed; the overlay is mixed into the RGB after
// the machine and before the framework's scaler.
//

module tape_deck
(
	input             clk,          // clk_sys (14.318 MHz)
	input             cpu_ce,
	input             cpu_sync,
	input      [15:0] cpu_addr,

	input             load,         // ioctl download for the Tape slot
	input             load_wr,
	input      [24:0] load_addr,
	input       [7:0] load_data,

	input             rewind,       // pulses (edge-detected by the caller)
	input             play_toggle,
	input             speed_7x,     // OSD Tape speed = 7x
	input             overlay_en,   // OSD Tape overlay = On
	input      [10:0] ps2_key,
	input             turbo_active, // from tk2000.v (for the "x7")

	output            cas_level,
	output            playing,
	output            loaded,
	output            turbo_req,
	output            info_req,     // to hps_io: 1 Tape ready, 2 not supported
	output      [7:0] info,

	input             hblank,
	input             vblank,
	input      [23:0] rgb_in,
	output reg [23:0] rgb_out
);

// ------------------------------------------------------------------ player
wire        at_start;
wire [31:0] elapsed, total;
wire        mount_ok, mount_bad;
wire        rd_bit = cpu_sync && cpu_ce && cpu_addr >= 16'hFCFA && cpu_addr <= 16'hFD0B;

ct2_player player
(
	.clk(clk),
	.cpu_ce(cpu_ce),
	.load(load),
	.load_wr(load_wr),
	.load_addr(load_addr),
	.load_data(load_data),
	.rewind(rewind),
	.play_toggle(play_toggle),
	.auto_play(rd_bit),
	.cas_level(cas_level),
	.playing(playing),
	.loaded(loaded),
	.position(),
	.size(),
	.at_start(at_start),
	.elapsed(elapsed),
	.total(total),
	.mount_ok(mount_ok),
	.mount_bad(mount_bad)
);

assign turbo_req = playing && speed_7x;

// info message: hps_io latches 'info' on the rising edge of info_req
reg       info_r = 1'b0;
reg [7:0] info_n = 8'd0;
always @(posedge clk) begin
	if (mount_ok)  begin info_n <= 8'd1; info_r <= ~info_r; end
	if (mount_bad) begin info_n <= 8'd2; info_r <= ~info_r; end
end
assign info_req = info_r;
assign info = info_n;

// ------------------------------------------------------------------ times
localparam [19:0] CPS = 20'd1022727;   // CPU cycles per second

function automatic [15:0] bcd_tick(input [15:0] t);
	reg [15:0] n;
	begin
		n = t;
		if (t != 16'h9959) begin
			if (t[3:0] != 4'd9) n[3:0] = t[3:0] + 4'd1;
			else begin
				n[3:0] = 4'd0;
				if (t[7:4] != 4'd5) n[7:4] = t[7:4] + 4'd1;
				else begin
					n[7:4] = 4'd0;
					if (t[11:8] != 4'd9) n[11:8] = t[11:8] + 4'd1;
					else begin n[11:8] = 4'd0; n[15:12] = t[15:12] + 4'd1; end
				end
			end
		end
		bcd_tick = n;
	end
endfunction

reg [15:0] el_bcd = 16'd0, tot_bcd = 16'd0;   // m1 m0 s1 s0
reg [19:0] el_frac = 20'd0;
reg [31:0] last_el = 32'd0, last_total = 32'd0, rem = 32'd0;
reg        tot_busy = 1'b0;
reg [25:0] cell_sz = 26'd0, cell_n = 26'd0;
reg  [6:0] fill = 7'd0;

always @(posedge clk) begin
	last_el <= elapsed;
	if (elapsed == 32'd0) begin
		el_bcd <= 16'd0; el_frac <= 20'd0; fill <= 7'd0; cell_n <= 26'd0;
	end else if (elapsed != last_el) begin           // steps by one
		if (el_frac == CPS - 20'd1) begin el_frac <= 20'd0; el_bcd <= bcd_tick(el_bcd); end
		else el_frac <= el_frac + 20'd1;
		if (cell_n + 26'd1 >= cell_sz) begin
			cell_n <= 26'd0;
			if (fill != 7'd64) fill <= fill + 7'd1;
		end else cell_n <= cell_n + 26'd1;
	end
	// total -> m:ss, one second per clock
	last_total <= total;
	if (total != last_total) begin
		rem <= total; tot_bcd <= 16'd0; tot_busy <= 1'b1;
		cell_sz <= (total[31:6] == 26'd0) ? 26'd1 : total[31:6];
	end else if (tot_busy) begin
		if (rem >= {12'd0, CPS}) begin rem <= rem - {12'd0, CPS}; tot_bcd <= bcd_tick(tot_bcd); end
		else tot_busy <= 1'b0;
	end
end

// ------------------------------------------------------------------ hint
// "TYPE LOADT <ret>" while loaded, rewound and stopped; Return hides it until
// the next mount / rewind.
reg hint_off = 1'b0, old_strobe = 1'b0, old_at_start = 1'b0;
always @(posedge clk) begin
	old_strobe <= ps2_key[10];
	old_at_start <= at_start;
	if (at_start && !old_at_start) hint_off <= 1'b0;
	if (ps2_key[10] != old_strobe && ps2_key[9] && !ps2_key[8] && ps2_key[7:0] == 8'h5A)
		hint_off <= 1'b1;
end

wire show_bar  = overlay_en && loaded && playing;
wire show_hint = overlay_en && loaded && !playing && at_start && !hint_off;

// ------------------------------------------------------------------ overlay
// Pixels are 7.16 MHz (every other clk_sys), as CE_PIXEL samples them.
reg        pce = 1'b0;
reg  [9:0] x = 10'd0, width = 10'd0;
reg  [8:0] y = 9'd0, height = 9'd0;
reg        line_active = 1'b0, old_hb = 1'b0, old_vb = 1'b0;
always @(posedge clk) begin
	pce <= ~pce;
	if (pce) begin
		old_hb <= hblank; old_vb <= vblank;
		if (!hblank && !vblank) begin x <= x + 10'd1; line_active <= 1'b1; end
		if (hblank && !old_hb) begin
			if (line_active) begin width <= x; y <= y + 9'd1; end
			x <= 10'd0; line_active <= 1'b0;
		end
		if (vblank && !old_vb) begin height <= y; y <= 9'd0; end
	end
end

localparam CHARS = 21;
wire  [4:0] chars = show_bar ? 5'd21 : 5'd12;
wire  [9:0] box_w = {2'd0, chars, 3'd0} + 10'd4;
wire  [8:0] box_h = show_bar ? 9'd18 : 9'd12;
wire  [9:0] x0 = 10'd4;
// Status bar top-left (the ROM prints its own load progress on the bottom
// line); the hint bottom-left (the BASIC prompt being typed on is at the top).
wire  [8:0] y0 = show_bar ? 9'd4 : height - box_h - 9'd4;
wire  [9:0] lx = x - x0;
wire  [8:0] ly = y - y0;
wire        in_box = x >= x0 && y >= y0 && lx < box_w && ly < box_h;
wire  [9:0] tx = lx - 10'd2;
wire  [8:0] ty = ly - 9'd2;
wire  [4:0] col = tx[7:3];
wire        fast = turbo_active;

function automatic [7:0] digit(input [3:0] d, input blank_zero);
	digit = (blank_zero && d == 4'd0) ? " " : (8'h30 + {4'd0, d});
endfunction

localparam [7:0] C_PLAY = 8'h01, C_RET = 8'h02;

reg [7:0] ch;
always @(*) begin
	ch = " ";
	if (show_bar) case (col)
		5'd0: ch = "T"; 5'd1: ch = "A"; 5'd2: ch = "P"; 5'd3: ch = "E";
		5'd5: ch = C_PLAY;
		5'd7: ch = digit(el_bcd[15:12], 1'b1);  5'd8: ch = digit(el_bcd[11:8], 1'b0);
		5'd9: ch = ":";
		5'd10: ch = digit(el_bcd[7:4], 1'b0);   5'd11: ch = digit(el_bcd[3:0], 1'b0);
		5'd12: ch = "/";
		5'd13: ch = digit(tot_bcd[15:12], 1'b1); 5'd14: ch = digit(tot_bcd[11:8], 1'b0);
		5'd15: ch = ":";
		5'd16: ch = digit(tot_bcd[7:4], 1'b0);  5'd17: ch = digit(tot_bcd[3:0], 1'b0);
		5'd19: ch = fast ? "x" : " ";
		5'd20: ch = fast ? "7" : " ";
		default: ch = " ";
	endcase else case (col)
		5'd0: ch = "T"; 5'd1: ch = "Y"; 5'd2: ch = "P"; 5'd3: ch = "E";
		5'd5: ch = "L"; 5'd6: ch = "O"; 5'd7: ch = "A"; 5'd8: ch = "D"; 5'd9: ch = "T";
		5'd11: ch = C_RET;
		default: ch = " ";
	endcase
end

function automatic [7:0] glyph(input [7:0] c, input [2:0] r);
	reg [63:0] g;
	begin
		case (c)
			"0": g = 64'h3C666E7666663C00;  "1": g = 64'h1838181818187E00;
			"2": g = 64'h3C66060C30607E00;  "3": g = 64'h3C66061C06663C00;
			"4": g = 64'h0C1C3C6C7E0C0C00;  "5": g = 64'h7E607C0606663C00;
			"6": g = 64'h1C30607C66663C00;  "7": g = 64'h7E060C1830303000;
			"8": g = 64'h3C66663C66663C00;  "9": g = 64'h3C66663E060C3800;
			":": g = 64'h0018180018180000;  "/": g = 64'h02060C1830604000;
			"T": g = 64'h7E18181818181800;  "A": g = 64'h183C66667E666600;
			"P": g = 64'h7C66667C60606000;  "E": g = 64'h7E60607C60607E00;
			"Y": g = 64'h6666663C18181800;  "L": g = 64'h6060606060607E00;
			"O": g = 64'h3C66666666663C00;  "D": g = 64'h786C6666666C7800;
			"x": g = 64'h0000663C183C6600;
			C_PLAY: g = 64'h4060707870604000;
			C_RET:  g = 64'h0002022262FE6020;
			default: g = 64'h0;
		endcase
		glyph = g[63 - 8*r -: 8];
	end
endfunction

wire [7:0] grow    = glyph(ch, ty[2:0]);
wire       text_on = ty < 9'd8 && tx < {2'd0, chars, 3'd0} && grow[3'd7 - tx[2:0]];
wire       bar_area = show_bar && ly >= 9'd12 && ly < 9'd16 && lx >= 10'd2 && lx < 10'd130;
wire       bar_on   = bar_area && (lx - 10'd2) < {2'd0, fill, 1'b0};

always @(*) begin
	rgb_out = rgb_in;
	if ((show_bar || show_hint) && in_box)
		rgb_out = text_on ? 24'hFFFFFF : bar_on ? 24'h44CC44 : bar_area ? 24'h333333 : 24'h000000;
end

endmodule
