//
// CT2 cassette player for the TK2000 core.
//
// The whole .ct2 image is loaded (ioctl, OSD "Tape" file slot) into a
// 128 KB buffer and played back as the square wave the TK2000 reads on
// KBIN ($C010) bit 7.
//
// CT2 format (fbelavenuto libct2 / emulator Tape.cpp, the reference player):
//   "CTK2" magic, then chunks of  ID[2] size[2](LE) payload
//     "CA"  leader:   silence (1 s before the first file, 0.5 s between files),
//                     then 6000 half periods of 500 CPU cycles (~3 s of ~1 kHz)
//     "CB"  sync:     464, 679, 64 x 679, 199, 250
//     "DA"  data:     'size' bytes, MSB first; bit 1 = 2 x 500, bit 0 = 2 x 250
// Leader/silence timing follows ct2play (the tool that plays CT2 to a real
// TK2000: 3 s leader, 1 s / 0.5 s silence). fbelavenuto's emulator uses a
// 0.5 s leader with no silence, which only works there because its tape
// advances only while the CPU reads it; on a free-running deck the ROM
// misses the next file's header while it is still printing "OK" (Karateka).
// Durations are in TK2000 CPU cycles (1.0227 MHz), so the player is clocked
// by the CPU clock enable: a speed-up of the CPU (turbo) speeds the tape up
// by the same factor, as the tape standard requires.
//
// Deck model: the TK2000 has no tape motor control in LOAD (the MOTOR relays
// are a BASIC command), so this models the PLAY key. A mount or 'rewind'
// leaves the tape rewound and stopped; 'auto_play' (from tape_deck: the ROM's
// tape routine is running while the tape is at its start) or 'play_toggle'
// (OSD Tape play/stop) starts it. The tape stops by itself at its end.
// 'total' (computed while the image loads) and 'elapsed' are tape time in
// CPU cycles, for the status bar.
//

module ct2_player
(
	input             clk,
	input             cpu_ce,       // one pulse per TK2000 CPU cycle

	// image load (ioctl download)
	input             load,         // download in progress for the tape slot
	input             load_wr,
	input      [24:0] load_addr,
	input       [7:0] load_data,

	input             rewind,       // pulse: rewind (stopped)
	input             play_toggle,  // pulse: PLAY/STOP
	input             auto_play,    // pulse: start playing (only acts while at_start)

	output reg        cas_level = 1'b0,   // to KBIN bit 7
	output            playing,
	output reg        loaded = 1'b0,      // a valid CT2 is in the buffer
	output reg [16:0] position = 17'd0,   // byte position (progress)
	output reg [16:0] size = 17'd0,
	output reg        at_start = 1'b0,    // rewound and not played since
	output reg [31:0] elapsed = 32'd0,    // tape time played, CPU cycles
	output reg [31:0] total = 32'd0,      // tape length, CPU cycles
	output reg        mount_ok = 1'b0,    // pulse: a valid CT2 finished loading
	output reg        mount_bad = 1'b0    // pulse: a download that is not CT2
);

// ---------------------------------------------------------------- buffer
wire  [7:0] q;
reg  [16:0] rd_addr;

bram #(8,17) tape_ram
(
	.clock_a(clk),
	.address_a(load_addr[16:0]),
	.wren_a(load & load_wr & ~load_addr[24] & ~|load_addr[23:17]),
	.data_a(load_data),
	.q_a(),

	.clock_b(clk),
	.address_b(rd_addr),
	.wren_b(1'b0),
	.data_b(8'd0),
	.q_b(q)
);

// magic check while loading
reg [3:0] magic_ok;
reg       old_load = 1'b0;
reg       mount_done;
always @(posedge clk) begin
	old_load <= load;
	mount_done <= 1'b0;
	mount_ok <= 1'b0;
	mount_bad <= 1'b0;
	if (load & load_wr) begin
		case (load_addr)
			25'd0: magic_ok[0] <= (load_data == "C");
			25'd1: magic_ok[1] <= (load_data == "T");
			25'd2: magic_ok[2] <= (load_data == "K");
			25'd3: magic_ok[3] <= (load_data == "2");
			default: ;
		endcase
		if (load_addr[24:17] == 0) size <= load_addr[16:0] + 17'd1;
	end
	if (~load & old_load) begin
		loaded <= &magic_ok;
		mount_done <= 1'b1;
		mount_ok <= &magic_ok;
		mount_bad <= ~&magic_ok;
	end
	if (load & ~old_load) loaded <= 1'b0;
end

// ---------------------------------------------------------------- generator
// Produces the next half-period length into 'next_dur' / 'next_valid'.
localparam G_IDLE = 4'd0, G_HDR = 4'd1, G_DISPATCH = 4'd2, G_LEADER = 4'd3,
           G_SYNC = 4'd4, G_DATA_FETCH = 4'd5, G_DATA_BITS = 4'd6, G_END = 4'd7,
           G_WAIT = 4'd8;

reg  [3:0] gstate = G_IDLE;
reg  [3:0] gret;
reg  [1:0] hdr_cnt;
reg  [7:0] id0, id1, sz_lo;
reg [15:0] chunk_left;
reg [12:0] rep;            // leader / sync repeat counter
reg        first_file;
reg        lead_silence;
reg  [2:0] bitn;
reg        half;           // first/second half of a bit
reg  [7:0] cur_byte;
reg [19:0] next_dur;
reg        next_silent;
reg        next_valid = 1'b0;
reg        take;           // timer consumed next_dur
reg        play = 1'b0;
reg        at_end;

assign playing = play;

wire start = (mount_done & &magic_ok) | (rewind & loaded);

// ---------------------------------------------------------------- length
function automatic [3:0] ones4(input [3:0] v);   // ($countones isn't synthesisable in Quartus 17)
	ones4 = {3'd0, v[0]} + {3'd0, v[1]} + {3'd0, v[2]} + {3'd0, v[3]};
endfunction
// Total tape time, from the chunk stream as it downloads (same durations as
// the generator below).
reg  [2:0] ps;            // 0 magic, 1 id0, 2 id1, 3 size lo, 4 size hi, 5 data
reg  [1:0] pmag;
reg  [7:0] pid0, pid1, psz_lo;
reg [15:0] pleft;
reg        pfirst;
always @(posedge clk) begin
	if (load & ~old_load) begin
		ps <= 3'd0; pmag <= 2'd0; total <= 32'd0; pfirst <= 1'b1;
	end else if (load & load_wr & ~load_addr[24]) begin
		case (ps)
			3'd0: begin pmag <= pmag + 2'd1; if (pmag == 2'd3) ps <= 3'd1; end
			3'd1: begin pid0 <= load_data; ps <= 3'd2; end
			3'd2: begin pid1 <= load_data; ps <= 3'd3; end
			3'd3: begin psz_lo <= load_data; ps <= 3'd4; end
			3'd4: begin
				if (pid0 == "C" && pid1 == "A") begin
					total <= total + (pfirst ? 32'd1022727 : 32'd511364) + 32'd3000000;
					pfirst <= 1'b0; ps <= 3'd1;
				end else if (pid0 == "C" && pid1 == "B") begin
					total <= total + 32'd45048; ps <= 3'd1;
				end else if (pid0 == "D" && pid1 == "A" && {load_data, psz_lo} != 16'd0) begin
					pleft <= {load_data, psz_lo}; ps <= 3'd5;
				end else ps <= 3'd1;
			end
			3'd5: begin
				// a byte: 8 bits x 2 half periods; 1 = 500 cycles, 0 = 250
				total <= total + 32'd4000 + 32'd500 * ({28'd0, ones4(load_data[7:4])} + {28'd0, ones4(load_data[3:0])});
				pleft <= pleft - 16'd1;
				if (pleft == 16'd1) ps <= 3'd1;
			end
			default: ps <= 3'd1;
		endcase
	end
end

always @(posedge clk) begin
	if (take) next_valid <= 1'b0;

	if (start) begin
		rd_addr  <= 17'd4;
		position <= 17'd4;
		gstate   <= G_WAIT;
		gret     <= G_HDR;
		hdr_cnt  <= 2'd0;
		next_valid <= 1'b0;
		at_end   <= 1'b0;
		first_file <= 1'b1;
	end
	else if (load) begin
		gstate <= G_IDLE;
		next_valid <= 1'b0;
	end
	else case (gstate)
		G_IDLE: ;

		G_WAIT: gstate <= gret;          // one cycle of BRAM read latency

		// read the 4-byte chunk header
		G_HDR: begin
			if (rd_addr >= size) begin
				gstate <= G_END;
			end else begin
				case (hdr_cnt)
					2'd0: id0 <= q;
					2'd1: id1 <= q;
					2'd2: sz_lo <= q;
					2'd3: chunk_left <= {q, sz_lo};
				endcase
				rd_addr  <= rd_addr + 17'd1;
				position <= rd_addr + 17'd1;
				hdr_cnt  <= hdr_cnt + 2'd1;
				gstate   <= G_WAIT;
				gret     <= (hdr_cnt == 2'd3) ? G_DISPATCH : G_HDR;
			end
		end

		G_DISPATCH: begin
			rep <= 13'd0;
			lead_silence <= 1'b1;
			if (id0 == "C" && id1 == "A") gstate <= G_LEADER;
			else if (id0 == "C" && id1 == "B") gstate <= G_SYNC;
			else if (id0 == "D" && id1 == "A") gstate <= G_DATA_FETCH;
			else gstate <= G_END;            // unknown chunk: stop
		end

		G_LEADER: if (!next_valid && !take) begin
			next_valid <= 1'b1;
			if (lead_silence) begin
				// 1 s before the first file, 0.5 s between files (ct2play)
				next_dur <= first_file ? 20'd1022727 : 20'd511364;
				next_silent <= 1'b1;
				lead_silence <= 1'b0;
				first_file <= 1'b0;
			end else begin
				next_dur <= 20'd500;
				next_silent <= 1'b0;
				rep <= rep + 13'd1;
				if (rep == 13'd5999) begin gstate <= G_HDR; hdr_cnt <= 2'd0; end
			end
		end

		G_SYNC: if (!next_valid && !take) begin
			next_valid <= 1'b1;
			next_silent <= 1'b0;
			rep <= rep + 13'd1;
			if (rep == 13'd0)       next_dur <= 20'd464;
			else if (rep <= 13'd65) next_dur <= 20'd679;     // 1 + 64 x 679
			else if (rep == 13'd66) next_dur <= 20'd199;
			else begin
				next_dur <= 20'd250;
				gstate <= G_HDR; hdr_cnt <= 2'd0;
			end
		end

		G_DATA_FETCH: begin
			if (chunk_left == 16'd0 || rd_addr >= size) begin
				gstate <= (rd_addr >= size) ? G_END : G_HDR;
				hdr_cnt <= 2'd0;
			end else begin
				cur_byte <= q;
				rd_addr  <= rd_addr + 17'd1;
				position <= rd_addr + 17'd1;
				chunk_left <= chunk_left - 16'd1;
				bitn <= 3'd7;
				half <= 1'b0;
				gstate <= G_DATA_BITS;
			end
		end

		G_DATA_BITS: if (!next_valid && !take) begin
			next_valid <= 1'b1;
			next_dur <= cur_byte[bitn] ? 20'd500 : 20'd250;
			next_silent <= 1'b0;
			half <= ~half;
			if (half) begin
				if (bitn == 3'd0) begin gstate <= G_WAIT; gret <= G_DATA_FETCH; end
				else bitn <= bitn - 3'd1;
			end
		end

		G_END: at_end <= 1'b1;
		default: gstate <= G_IDLE;
	endcase
end

// ---------------------------------------------------------------- timer
reg [19:0] count = 20'd0;
reg        running = 1'b0;    // a half period is being timed
reg        cur_silent = 1'b0; // the period being timed is silence (no edge at its end)

always @(posedge clk) begin
	take <= 1'b0;

	if (start) begin
		play <= 1'b0;
		at_start <= 1'b1;
		elapsed <= 32'd0;
	end else if (play_toggle && loaded && !(at_end && !play)) begin
		play <= ~play;
		at_start <= 1'b0;
	end else if (auto_play && loaded && at_start && !play) begin
		play <= 1'b1;
		at_start <= 1'b0;
	end
	if (load) begin play <= 1'b0; at_start <= 1'b0; end
	if (play && cpu_ce && running) elapsed <= elapsed + 32'd1;

	if (start | load) begin
		running   <= 1'b0;
		cas_level <= 1'b0;
	end
	else if (play && cpu_ce) begin
		if (running && count > 20'd1) begin
			count <= count - 20'd1;
		end else begin
			// half period finished (or none yet): toggle and load the next
			if (running && !cur_silent) cas_level <= ~cas_level;
			if (next_valid && !take) begin
				count   <= next_dur;
				cur_silent <= next_silent;
				running <= 1'b1;
				take    <= 1'b1;
			end else begin
				running <= 1'b0;
				if (at_end) play <= 1'b0;   // tape ran out
			end
		end
	end
end

endmodule
