#include <verilated.h>
#include "Vemu.h"
#include "Vemu___024root.h"

#include "imgui.h"
#include "implot.h"
#ifndef _MSC_VER
#include <stdio.h>
#include <SDL.h>
#include <SDL_opengl.h>
#else
#define WIN32
#include <dinput.h>
#endif

#include "sim_console.h"
#include "sim_bus.h"
#include "sim_blkdevice.h"
#include "sim_video.h"
#include "sim_audio.h"
#include "sim_input.h"
#include "sim_clock.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <vector>
#include <algorithm>
#include <map>

#define FMT_HEADER_ONLY
#include <fmt/core.h>


#include "../imgui/imgui_memory_editor.h"
#include "../imgui/ImGuiFileDialog.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <iomanip>
#include <thread>
#include <chrono>

using namespace std;

#define VERILATOR_MAJOR_VERSION (VERILATOR_VERSION_INTEGER / 1000000)

#if VERILATOR_MAJOR_VERSION >= 5
#define VERTOPINTERN top->rootp
#else
#define VERTOPINTERN top
#endif


// Simulation control
// ------------------
int initialReset = 48;
bool run_enable = 1;
int batchSize = 650000;
bool single_step = 0;
bool multi_step = 0;
int multi_step_amount = 1024;


bool stop_on_log_mismatch = 1;
bool debug_6502 = 1;
bool cpu_log = false;
int watch_lo = -1, watch_hi = -1;   // --watch: log RAM writes in this range
int watch_from = 0;                 // --watch-from: first frame to log
unsigned int last_fetch_pc = 0;      // per-instruction log to the debug console (slow)
bool headless = false;
int cpu_sync;
long cpu_instruction_count;
int cpu_clock;
int cpu_clock_last;
const int ins_size = 48;
int ins_index = 0;
unsigned short ins_pc[ins_size];
unsigned char ins_in[ins_size];
unsigned long ins_ma[ins_size];
unsigned char ins_dbr[ins_size];
bool ins_formatted[ins_size];
std::string ins_str[ins_size];



// Debug GUI 
// ---------
const char* windowTitle = "Verilator Sim: TK2000";
const char* windowTitle_Control = "Simulation control";
const char* windowTitle_DebugLog = "Debug log";
const char* windowTitle_Video = "VGA output";
const char* windowTitle_Audio = "Audio output";
bool showDebugLog = true;
DebugConsole console;
MemoryEditor mem_edit;

// HPS emulator
// ------------
SimBus bus(console);
SimBlockDevice blockdevice(console);

// Input handling
// --------------
SimInput input(13, console);
const int input_right = 0;
const int input_left = 1;
const int input_down = 2;
const int input_up = 3;
const int input_a = 4;
const int input_b = 5;
const int input_x = 6;
const int input_y = 7;
const int input_l = 8;
const int input_r = 9;
const int input_select = 10;
const int input_start = 11;
const int input_menu = 12;

// Video
// -----
#define VGA_WIDTH 284
#define VGA_HEIGHT 192
#define VGA_ROTATE 0  // 90 degrees anti-clockwise
#define VGA_SCALE_X vga_scale
#define VGA_SCALE_Y vga_scale
SimVideo video(VGA_WIDTH, VGA_HEIGHT, VGA_ROTATE);
float vga_scale = 2.5;

// Verilog module
// --------------
Vemu* top = NULL;

vluint64_t main_time = 0;	// Current simulation time.
double sc_time_stamp() {	// Called by $time in Verilog.
	return main_time;
}

int clk_sys_freq = 24000000;
SimClock clk_sys(1);

int soft_reset=0;
vluint64_t soft_reset_time=0;

// Audio
// -----
//#define DISABLE_AUDIO
#ifndef DISABLE_AUDIO
SimAudio audio(clk_sys_freq, false);
#endif

enum instruction_type {
	formatted,
	implied,
	immediate,
	absolute,
	absoluteX,
	absoluteY,
	zeroPage,
	zeroPageX,
	zeroPageY,
	relative,
	relativeLong,
	accumulator,
	direct24,
	direct24X,
	direct24Y,
	indirect,
	indirectX,
	indirectY,
	longValue,
	longX,
	longY,
	stackmode,
	srcdst
};

enum operand_type {
	none,
	byte2,
	byte3
};

struct dasm_data
{
	unsigned short addr;
	const char* name;
};

struct dasm_data32
{
	unsigned long addr;
	const char* name;
};

std::vector<std::string> log_cpu;
long log_index;


bool writeLog(const char* line)
{
		// Write to cpu log
		if (headless) { printf("%s\n", line); return true; }
		log_cpu.push_back(line);
		std::string c_line = std::string(line);
		std::string c = "%6d  CPU > " + c_line;
		console.AddLog(c.c_str(), cpu_instruction_count);
		log_index++;

		return true;
#if 0
		// Compare with MAME log
		bool match = true;

		std::string c_line = std::string(line);
		std::string c = "%6d  CPU > " + c_line;
		//printf("%s (%x)\n",line,ins_in[0]); // this has the instruction number
		printf("%s\n",line);

		if (log_index < log_mame.size()) {
			std::string m_line = log_mame.at(log_index);
			std::string m = "%6d MAME > " + m_line;
			if (stop_on_log_mismatch && m_line != c_line) {
				console.AddLog("DIFF at %06d - %06x", cpu_instruction_count, ins_pc[0]);
				console.AddLog(m.c_str(), cpu_instruction_count);
				console.AddLog(c.c_str(), cpu_instruction_count);
				match = false;
			}
			else {
				console.AddLog(c.c_str(), cpu_instruction_count);
			}
		}
		else {
			console.AddLog(c.c_str(), cpu_instruction_count);
		}

		log_index++;
		return match;
	}
	return true;
#endif
}

void DumpInstruction() {

	std::string log = "{0:02X}:{1:04X}: ";
	const char* f = "";
	const char* sta;

	instruction_type type = implied;
	operand_type opType = none;

	std::string arg1 = "";
	std::string arg2 = "";

	switch (ins_in[0])
	{
	case 0x00: sta = "brk"; break;
	case 0x98: sta = "tya"; break;
	case 0xA8: sta = "tay"; break;
	case 0xAA: sta = "tax"; break;
	case 0x8A: sta = "txa"; break;
	case 0x9B: sta = "txy"; break;
	case 0x40: sta = "rti"; break;
	case 0x60: sta = "rts"; break;
	case 0x9A: sta = "txs"; break;
	case 0xBA: sta = "tsx"; break;
	case 0xBB: sta = "tyx"; break;
	case 0x0C: sta = "tsb"; type = absolute; opType = byte3; break;
	case 0x1B: sta = "tcs"; break;
	case 0x5B: sta = "tcd"; break;

	case 0x08: sta = "php"; break;
	case 0x0B: sta = "phd"; break;
	case 0x2B: sta = "pld"; break;
	case 0xAB: sta = "plb"; break;
	case 0x8B: sta = "phb"; break;
	case 0x4B: sta = "phk"; break;
	case 0x28: sta = "plp"; break;
	case 0xfb: sta = "xce"; break;

	case 0x18: sta = "clc"; break;
	case 0x58: sta = "cli"; break;
	case 0xB8: sta = "clv"; break;
	case 0xD8: sta = "cld"; break;

	case 0xE8: sta = "inx"; break;
	case 0xC8: sta = "iny"; break;
	case 0x1A: sta = "ina"; break;

	case 0x70: sta = "bvs"; type = relativeLong; break;
	case 0x80: sta = "bra"; type = relativeLong; break;

	case 0x38: sta = "sec"; break;
	case 0xe2: sta = "sep"; type = immediate;  break;
	case 0x78: sta = "sei"; break;
	case 0xF8: sta = "sed"; break;

	case 0x48: sta = "pha"; break;
	case 0xDA: sta = "phx"; break;
	case 0x5A: sta = "phy"; break;
	case 0x68: sta = "pla"; break;
	case 0xFA: sta = "plx"; break;
	case 0x7A: sta = "ply"; break;

	case 0xF4: sta = "pea"; type = absolute; break;
	case 0x62: sta = "per"; type = relativeLong; break;
	case 0xD4: sta = "pei"; type = zeroPage; break;

	case 0x0A: sta = "asl"; type = accumulator; break;
	case 0x06: sta = "asl"; type = zeroPage; break;
	case 0x16: sta = "asl"; type = zeroPageX; break;
	case 0x0E: sta = "asl"; type = absolute; break;
	case 0x1E: sta = "asl"; type = absoluteX; break;

	case 0x01: sta = "ora"; type = indirectX; break;
	case 0x03: sta = "ora"; type = stackmode; break;
	case 0x05: sta = "ora"; type = zeroPage; break;
	case 0x07: sta = "ora"; type = direct24; break;
	case 0x09: sta = "ora"; type = immediate; break;
	case 0x0D: sta = "ora"; type = absolute; opType = byte2; break;
	case 0x0F: sta = "ora"; type = longValue; opType = byte3; break;
	case 0x11: sta = "ora"; type = indirectY; break;
	case 0x15: sta = "ora"; type = zeroPageX; break;
	case 0x17: sta = "ora"; type = direct24Y; break;
	case 0x19: sta = "ora"; type = absoluteY; break;
	case 0x1D: sta = "ora"; type = absoluteX; break;
	case 0x1F: sta = "ora"; type = longX; break;

	case 0x43: sta = "eor"; type = stackmode; break;
	case 0x47: sta = "eor"; type = direct24; break;
	case 0x49: sta = "eor"; type = immediate; break;
	case 0x4d: sta = "eor"; type = absolute; break;
	case 0x45: sta = "eor"; type = zeroPage; break;
	case 0x55: sta = "eor"; type = zeroPageX; break;
	case 0x57: sta = "eor"; type = direct24Y; break;
	case 0x5d: sta = "eor"; type = absoluteX; break;
	case 0x59: sta = "eor"; type = absoluteY; break;
	case 0x41: sta = "eor"; type = indirectX; break;
	case 0x51: sta = "eor"; type = indirectY; break;

	case 0x23: sta = "and"; type = stackmode; break;
	case 0x25: sta = "and"; type = zeroPage; break;
	case 0x27: sta = "and"; type = direct24; break;
	case 0x29: sta = "and"; type = immediate; break;
	case 0x2D: sta = "and"; type = absolute; break;
	case 0x35: sta = "and"; type = zeroPageX; break;
	case 0x37: sta = "and"; type = direct24Y; break;
	case 0x39: sta = "and"; type = absoluteY; break;
	case 0x3D: sta = "and"; type = absoluteX; break;


	case 0xE1: sta = "sbc"; type = indirectX; break;
	case 0xE3: sta = "sbc"; type = stackmode; break;
	case 0xE5: sta = "sbc"; type = zeroPage; break;
	case 0xE7: sta = "sbc"; type = direct24; break;
	case 0xE9: sta = "sbc"; type = immediate; break;
	case 0xED: sta = "sbc"; type = absolute; break;
	case 0xF1: sta = "sbc"; type = indirectY; break;
	case 0xF5: sta = "sbc"; type = zeroPageX; break;
	case 0xF7: sta = "sbc"; type = direct24Y; break;
	case 0xF9: sta = "sbc"; type = absoluteY; break;
	case 0xFD: sta = "sbc"; type = absoluteX; break;

	case 0xC3: sta = "cmp"; type = stackmode; break;
	case 0xC5: sta = "cmp"; type = zeroPage; break;
	case 0xC7: sta = "cmp"; type = direct24; break;
	case 0xC9: sta = "cmp"; type = immediate; break;
	case 0xCD: sta = "cmp"; type = absolute; break;
	case 0xCF: sta = "cmp"; type = longValue; opType=byte3; break;
	case 0xD1: sta = "cmp"; type = indirectY; break;
	case 0xD5: sta = "cmp"; type = zeroPageX; break;
	case 0xD7: sta = "cmp"; type = direct24Y; break;
	case 0xD9: sta = "cmp"; type = absoluteY; break;
	case 0xDD: sta = "cmp"; type = absoluteX; break;
	case 0xDF: sta = "cmp"; type = longX; break;


	case 0xE0: sta = "cpx"; type = immediate; break;
	case 0xE4: sta = "cpx"; type = zeroPage; break;
	case 0xEC: sta = "cpx"; type = absolute; break;

	case 0xC0: sta = "cpy"; type = immediate; break;
	case 0xC4: sta = "cpy"; type = zeroPage; break;
	case 0xCC: sta = "cpy"; type = absolute; break;

	case 0xC2: sta = "rep"; type = immediate; break;

	case 0xA2: sta = "ldx"; type = immediate; break;
	case 0xA6: sta = "ldx"; type = zeroPage; break;
	case 0xB6: sta = "ldx"; type = zeroPageY; break;
	case 0xAE: sta = "ldx"; type = absolute; break;
	case 0xBE: sta = "ldx"; type = absoluteY; break;

	case 0xA0: sta = "ldy"; type = immediate; break;
	case 0xA4: sta = "ldy"; type = zeroPage; break;
	case 0xB4: sta = "ldy"; type = zeroPageX; break;
	case 0xAC: sta = "ldy"; type = absolute; break;
	case 0xBC: sta = "ldy"; type = absoluteX; break;

	case 0xA1: sta = "lda"; type = indirectX; break;
	case 0xA3: sta = "lda"; type = stackmode; break;
	case 0xA5: sta = "lda"; type = zeroPage; break;
	case 0xA7: sta = "lda"; type = direct24; break;
	case 0xA9: sta = "lda"; type = immediate; break;
	case 0xAD: sta = "lda"; type = absolute; opType = byte3; break;
	case 0xAF: sta = "lda"; type = longValue; opType=byte3; break;
	case 0xB1: sta = "lda"; type = indirectY; break;
	case 0xB2: sta = "lda"; type = indirect; break;
	case 0xB5: sta = "lda"; type = zeroPageX; break;
	case 0xB7: sta = "lda"; type = direct24Y; break;
	case 0xB9: sta = "lda"; type = absoluteY; break;
	case 0xBD: sta = "lda"; type = absoluteX; break;
	case 0xBF: sta = "lda"; type = longX; break;


	case 0x1C: sta = "trb"; type = absolute; break;

	case 0x81: sta = "sta"; type = indirectX; break;
	case 0x83: sta = "sta"; type = stackmode; break;
	case 0x85: sta = "sta"; type = zeroPage; break;
	case 0x87: sta = "sta"; type = direct24; break;
	case 0x8D: sta = "sta"; type = absolute; opType = byte3; break;
	case 0x8F: sta = "sta"; type = longValue; opType = byte3; break;
	case 0x91: sta = "sta"; type = indirectY; break;
	case 0x95: sta = "sta"; type = zeroPageX; break;
	case 0x97: sta = "sta"; type = direct24Y; break;
	case 0x99: sta = "sta"; type = absoluteY; break;
	case 0x9D: sta = "sta"; type = absoluteX; break;
	case 0x9F: sta = "sta"; type = longX; break;


	case 0x86: sta = "stx"; type = zeroPage; break;
	case 0x96: sta = "stx"; type = zeroPageY; break;
	case 0x8E: sta = "stx"; type = absolute; break;
	case 0x84: sta = "sty"; type = zeroPage; break;
	case 0x94: sta = "sty"; type = zeroPageX; break;
	case 0x8C: sta = "sty"; type = absolute; break;
	case 0x64: sta = "stz"; type = zeroPage;  break;
	case 0x9C: sta = "stz"; type = absolute;  opType = byte3; break;
	case 0x9E: sta = "stz"; type = absoluteX; break;

	case 0x63: sta = "adc"; type = stackmode; break;
	case 0x65: sta = "adc"; type = zeroPage; break;
	case 0x67: sta = "adc"; type = direct24; break;
	case 0x69: sta = "adc"; type = immediate; break;
	case 0x6D: sta = "adc"; type = absolute; break;
	case 0x75: sta = "adc"; type = zeroPageX; break;
	case 0x77: sta = "adc"; type = direct24Y; break;
	case 0x79: sta = "adc"; type = absoluteY; break;
	case 0x7D: sta = "adc"; type = absoluteX; break;

	case 0x3b: sta = "tsc"; break;
	case 0x7b: sta = "tdc"; break;

	case 0xC6: sta = "dec"; type = zeroPage;  break;
	case 0xD6: sta = "dec"; type = zeroPageX;  break;
	case 0xCE: sta = "dec"; type = absolute;  break;
	case 0xDE: sta = "dec"; type = absoluteX;  break;

	case 0x3A: sta = "dea"; break;
	case 0xCA: sta = "dex"; break;
	case 0x88: sta = "dey"; break;

	case 0xEB: sta = "xba"; break;

	case 0x24: sta = "bit"; type = zeroPage; break;
	case 0x2C: sta = "bit"; type = absolute; break;
	case 0x3C: sta = "bit"; type = absoluteX; break;
	case 0x89: sta = "bit"; type = immediate; break;

	case 0x30: sta = "bmi"; type = relativeLong; break;
	case 0x90: sta = "bcc"; type = relative; break;
	case 0xB0: sta = "bcs"; type = relative; break;
	case 0xD0: sta = "bne"; type = relative; break;
	case 0xF0: sta = "beq"; type = relative; break;
	case 0x50: sta = "bvc"; type = relative; break;
	case 0x10: sta = "bpl"; type = relative; break;

	case 0x26: sta = "rol"; type = zeroPage; break;
	case 0x2a: sta = "rol"; type = accumulator; break;
	case 0x2e: sta = "rol"; type = absolute ; break;
	case 0x3e: sta = "rol"; type = absoluteX; break;

	case 0x66: sta = "ror"; type = zeroPage; break;
	case 0x6a: sta = "ror"; type = accumulator; break;
	case 0x6e: sta = "ror"; type = absolute ; break;
	case 0x7e: sta = "ror"; type = absoluteX; break;

	case 0x46: sta = "lsr"; type = zeroPage; break;
	case 0x4A: sta = "lsr"; type = accumulator; break;
	case 0x4e: sta = "lsr"; type = absolute ; break;
	case 0x5e: sta = "lsr"; type = absoluteX; break;

	case 0x54: sta = "mvn"; type = srcdst; break;
	case 0x44: sta = "mvp"; type = srcdst; break;

	case 0xE6: sta = "inc"; type = zeroPage; break;
	case 0xF6: sta = "inc"; type = zeroPageX; break;
	case 0xEE: sta = "inc"; type = absolute; break;
	case 0xFE: sta = "inc"; type = absoluteX; break;

	case 0x20: sta = "jsr"; type = absolute; opType = byte3; break;
	case 0xFC: sta = "jsr"; type = absoluteX; break;

	case 0x22: sta = "jsl"; type = longValue; opType = byte3; break;

	case 0x4C: sta = "jmp"; type = absolute; break;
	case 0x5C: sta = "jmp"; type = longValue; opType=byte3; break;
	case 0x6C: sta = "jmp"; type = indirect; break;
	case 0x7C: sta = "jmp"; type = absoluteX; break;

	case 0x6B: sta = "rtl";  break;

	case 0xEA: sta = "nop";  break;

	default: sta = "???";  f = "\t\tPC={0:X} arg1={1:X} arg2={2:X} IN0={3:X} IN1={4:X} IN2={5:X} IN3={6:X} IN4={7:X} MA0={8:X} MA1={9:X} MA2={10:X} MA3={11:X} MA4={12:X}";
	}

	// replace out named values?

	if (ins_index > 1) {

		if (opType == byte3) {
			unsigned long operand = ins_in[1];
			operand |= (ins_in[2] << 8);
			operand |= (ins_in[3] << 16);

			if (ins_index <= 3) {
				operand = ins_in[1];
				operand |= (ins_in[2] << 8);
				operand |= (ins_dbr[1] << 16);
			}
			//console.AddLog("%d %x", ins_index, operand);

		}

		if (type != formatted && (opType == byte2 || (opType == byte3 && ins_index == 3))) {

			unsigned short operand = ins_in[1];
			if (ins_index > 2) {
				operand |= (ins_in[2] << 8);
			}

			int item = 0;
		}
	}


	f = "{2:s}";
	unsigned long relativeAddress = ins_ma[0] + ((signed char)ins_in[1]) + 2;
	if (sta == "per") {
		relativeAddress++; // I HATE THIS
	}
	unsigned char maHigh0 = (unsigned char)(ins_ma[0] >> 16) & 0xff;
	unsigned char maHigh1 = (unsigned char)(ins_ma[1] >> 16) & 0xff;

	signed char signedIn1 = ins_in[1];
	std::string signedIn1Formatted = signedIn1 < 0 ? fmt::format("-${0:x}", signedIn1 * -1) : fmt::format("${0:x}", signedIn1);

	switch (type) {
	case implied: f = ""; break;
	case formatted: arg1 = ins_str[1]; f = " {2:s}"; break;
	case immediate:
		if (ins_index == 3) {
			arg1 = fmt::format(" #${0:02x}{1:02x}", ins_in[2], ins_in[1]);
		}
		else {
			arg1 = fmt::format(" #${0:02x}", ins_in[1]);
		}
		break;
	case srcdst: arg1 = fmt::format(" ${0:02x}, ${1:02x}", ins_in[2], ins_in[1]); break;
	case absolute: arg1 = fmt::format(" ${0:02x}{1:02x}", ins_in[2], ins_in[1]); break;
	case absoluteX: arg1 = fmt::format(" ${0:02x}{1:02x},x", ins_in[2], ins_in[1]); break;
	case absoluteY: arg1 = fmt::format(" ${0:02x}{1:02x},y", ins_in[2], ins_in[1]); break;
	case zeroPage: arg1 = fmt::format(" ${0:02x}", ins_in[1]); break;
	case direct24: arg1 = fmt::format(" [${0:02x}]", ins_in[1]); break;
	case direct24X: arg1 = fmt::format(" [${0:02x}],x", ins_in[1]); break;
	case direct24Y: arg1 = fmt::format(" [${0:02x}],y", ins_in[1]); break;
	case zeroPageX: arg1 = fmt::format(" ${0:02x},x", ins_in[1]); break;
	case zeroPageY: arg1 = fmt::format(" ${0:02x},y", ins_in[1]); break;
	case indirect: arg1 = fmt::format(" (${0:04x})", ins_in[1]); break;
	case indirectX: arg1 = fmt::format(" (${0:02x}),x", ins_in[1]); break;
	case indirectY: arg1 = fmt::format(" (${0:02x}),y", ins_in[1]); break;
	case stackmode: arg1 = fmt::format(" ${0:x},s", ins_in[1]); break;
	case longValue: arg1 = fmt::format(" ${0:02x}{1:02x}{2:02x}", ins_in[3], ins_in[2], ins_in[1]); break;
	case longX: arg1 = fmt::format(" ${0:02x}{1:02x}{2:02x},x", ins_in[3], ins_in[2], ins_in[1]); break;
	case longY: arg1 = fmt::format(" ${0:02x}{1:02x}{2:02x},y", ins_in[3], ins_in[2], ins_in[1]); break;
		//case longX: arg1 = fmt::format(" ${0:02x}{1:02x}{2:02x},x", maHigh1, ins_in[2], ins_in[1]); break;
		//case longY: arg1 = fmt::format(" ${0:02x}{1:02x}{2:02x},y", maHigh1, ins_in[2], ins_in[1]); break;
	case accumulator: arg1 = "a"; break;
	case relative: arg1 = fmt::format(" {0:06x} ({1})", relativeAddress, signedIn1Formatted);		break;
	case relativeLong: arg1 = fmt::format(" {0:06x} ({1})", relativeAddress, signedIn1Formatted);		break;
	default: arg1 = "UNSUPPORTED TYPE!";
	}

	log.append(sta);
	log.append(f);
	log = fmt::format(log, maHigh0, (unsigned short)ins_pc[0], arg1);

	if (!writeLog(log.c_str())) {
	//	run_state = RunState::Stopped;
	}
	cpu_instruction_count++;
	//if (sta == "???") {
	//	console.AddLog(log.c_str());
	//	run_enable = 0;
	//}

}


// Reset simulation variables and clocks
void resetSim() {
	main_time = 0;
	top->reset = 1;
	clk_sys.Reset();
}

	//MSM6242B layout
void send_clock() {
	//printf("Update RTC %ld %d\n",main_time,send_clock_done);
	uint8_t rtc[8];
	
//	printf("Update RTC %ld %d\n",main_time,send_clock_done);
	
	time_t t;

	time(&t);

	struct tm tm;
        localtime_r(&t,&tm);

	
	rtc[0] = (tm.tm_sec % 10) | ((tm.tm_sec / 10) << 4);
	rtc[1] = (tm.tm_min % 10) | ((tm.tm_min / 10) << 4);
	rtc[2] = (tm.tm_hour % 10) | ((tm.tm_hour / 10) << 4);
	rtc[3] = (tm.tm_mday % 10) | ((tm.tm_mday / 10) << 4);

	rtc[4] = ((tm.tm_mon + 1) % 10) | (((tm.tm_mon + 1) / 10) << 4);
	rtc[5] = (tm.tm_year % 10) | (((tm.tm_year / 10) % 10) << 4);
	rtc[6] = tm.tm_wday;
	rtc[7] = 0x40;

	// 64:0
	 
	//top->RTC_l = 0;
	top->RTC_l = rtc[0] | rtc[1] << 8 | rtc[2] << 16 | rtc[3] << 24 ;
	printf("RTC: %x 0: %x",top->RTC_l,rtc[0]);
	top->RTC_h = rtc[4] | rtc[5] << 8 | rtc[6] << 16 | rtc[7] << 24 ;
	//t += t - mktime(gmtime(&t));
	top->RTC_toggle=~top->RTC_toggle;
	// 32:0
	//top->TIMESTAMP=t;//|0x01<<32;


}


int verilate() {

	if (!Verilated::gotFinish()) {
		if (soft_reset){
			fprintf(stderr,"soft_reset.. in gotFinish\n");
			top->soft_reset = 1;
			soft_reset=0;
			soft_reset_time=0;
			fprintf(stderr,"turning on %x\n",top->soft_reset);
		}
		if (clk_sys.IsRising()) {
			soft_reset_time++;
		}
		if (soft_reset_time==initialReset) {
			top->soft_reset = 0; 
		} 

		// Assert reset during startup
		if (main_time < initialReset) { top->reset = 1; }
		// Deassert reset after startup
		if (main_time == initialReset) { top->reset = 0; }

		// Clock dividers
		clk_sys.Tick();

		// Set system clock in core
		top->clk_sys = clk_sys.clk;

		// Simulate both edges of system clock
		if (clk_sys.clk != clk_sys.old) {
			if (clk_sys.IsRising() && *bus.ioctl_download!=1	) blockdevice.BeforeEval(main_time);
			if (clk_sys.clk) {
				input.BeforeEval();
				bus.BeforeEval();
			}
			// 6502 trace: sample the CPU state just before the rising clock edge
			// on which an opcode-fetch cycle (MCycle 0) completes.
			bool trace_edge = false;
			unsigned int t_pc = 0, t_op = 0, t_a = 0, t_x = 0, t_y = 0, t_s = 0, t_p = 0;
			if (watch_lo >= 0 && clk_sys.clk && VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__Enable &&
			    VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__MCycle == 0)
				last_fetch_pc = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__A & 0xFFFF;
			if (watch_lo >= 0 && clk_sys.clk && VERTOPINTERN->emu__DOT__ram_we) {
				static int last_wa = -1, last_wd = -1;
				int wa = VERTOPINTERN->emu__DOT__ram_addr, wd = VERTOPINTERN->emu__DOT__ram_data;
				if (video.count_frame >= watch_from && wa >= watch_lo && wa <= watch_hi && (wa != last_wa || wd != last_wd))
					printf("WATCH F%05d PC~%04X write %04X = %02X\n", video.count_frame, last_fetch_pc, wa, wd);
				last_wa = wa; last_wd = wd;
			}
			if (cpu_log && clk_sys.clk && !top->reset &&
			    VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__Enable &&
			    VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__MCycle == 0) {
				trace_edge = true;
				t_pc = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__A & 0xFFFF;
				t_op = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__DI;
				t_a = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__ABC & 0xFF;
				t_x = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__X & 0xFF;
				t_y = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__Y & 0xFF;
				t_s = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__S & 0xFF;
				t_p = VERTOPINTERN->emu__DOT__tk2000__DOT__cpu6502__DOT__P;
			}
			top->eval();
			if (trace_edge) {
				char line[96];
				snprintf(line, sizeof(line), "F%05d %04X: %02X  A=%02X X=%02X Y=%02X S=%02X P=%02X",
				         video.count_frame, t_pc, t_op, t_a, t_x, t_y, t_s, t_p);
				if (headless) printf("%s\n", line); else console.AddLog(line);
				cpu_instruction_count++;
			}
			cpu_clock = 0;
			cpu_clock_last = cpu_clock;
			if (clk_sys.clk) { bus.AfterEval(); blockdevice.AfterEval(); }
		}
		
#ifndef DISABLE_AUDIO
		if (clk_sys.IsRising())
		{
			audio.Clock(top->AUDIO_L, top->AUDIO_R);
		}
#endif

		// Output pixels on rising edge of pixel clock
		if (clk_sys.IsRising() && top->CE_PIXEL ) {
			uint32_t colour = 0xFF000000 | top->VGA_B << 16 | top->VGA_G << 8 | top->VGA_R;
			video.Clock(top->VGA_HB, top->VGA_VB, top->VGA_HS, top->VGA_VS, colour);
		}

		if (clk_sys.IsRising()) {
			main_time++;
		}
		return 1;
	}

	// Stop verilating and cleanup
	top->final();
	delete top;
	exit(0);
	return 0;
}

unsigned char mouse_clock = 0;
unsigned char mouse_clock_reduce = 0;
unsigned char mouse_buttons = 0;
unsigned char mouse_x = 0;
unsigned char mouse_y = 0;

char spinner_toggle = 0;

// ===========================================================================
// Command line / automation
// ===========================================================================

struct DiskSwap { int frame; int drive; std::string path; bool done; };
struct KeyEvent { int frame; unsigned char code; bool ext; bool pressed; };
struct JoyEvent { int frame; unsigned int mask; };

static std::string opt_floppy[2];
static int opt_mount_frame[2] = { 0, 0 };
static std::vector<DiskSwap> opt_swaps;
static int opt_stop_at_frame = -1;
static std::vector<int> opt_screenshot_frames;
static int opt_screenshot_every = 0;
static std::string opt_screenshot_dir = ".";
static std::string opt_screenshot_prefix = "frame";
static std::vector<int> opt_reset_frames;
static std::vector<KeyEvent> key_events;   // sorted by frame (stable)
static size_t key_event_pos = 0;
static int key_gap_cycles = 0;
static bool ps2_toggle = false;
static std::vector<JoyEvent> joy_events;
static unsigned int joy_mask = 0;
static int opt_key_hold = 3;      // frames a key stays down
static int opt_key_gap = 3;       // frames between keys
static bool opt_hash_every_frame = false;
static int reset_hold = 0;
static int opt_cpu_log_from = -1;
static int opt_cpu_log_to = -1;
static bool floppy_mounted[2] = { false, false };
static std::string opt_tape;
static int opt_tape_frame = 0;
static std::vector<int> opt_tape_rewind, opt_tape_play;
struct TapeSwap { int frame; std::string path; bool done; };
static std::vector<TapeSwap> opt_tape_swaps;
static std::vector<std::pair<int,std::string>> opt_ram_dumps;
static int tape_pulse = 0;          // cycles left on a rewind/play button pulse
static bool tape_last_playing = false, tape_last_loaded = false;

static void usage() {
	printf("TK2000 Verilator sim\n\n");
	printf("Usage: ./obj_dir/Vemu [options]\n\n");
	printf("  --headless                    Run without a window (also HEADLESS=1)\n");
	printf("  --floppy <file>, --disk1 <f>  Insert image in drive 1 (.nib raw, .dsk/.do/.po nibblized like Main_MiSTer)\n");
	printf("  --disk2 <file>                Insert image in drive 2\n");
	printf("  --mount-frame <n>             Insert the drive 1 image at frame n instead of power-on\n");
	printf("  --swap <frame>:<drive>:<file> Swap/insert an image at a frame (drive 1|2; empty file = eject). Repeatable\n");
	printf("  --no-disk-rom                 OSD 'Disk Rom: Off' (boot to BASIC without trying the drive; use for tapes)\n");
	printf("  --tape-normal                 OSD Tape speed: Normal (default 7x turbo while the tape plays)\n");
	printf("  --tape-overlay-off            OSD Tape overlay: Off\n");
	printf("  --tape <file.ct2>             Load a CT2 tape (OSD Tape slot); it starts when the ROM reads the tape (LOADT)\n");
	printf("  --tape-frame <n>              Load the tape at frame n (default 0; type LOAD first, then load the tape)\n");
	printf("  --watch-from <frame>          Only log --watch writes from this frame on\n");
	printf("  --dump-ram <frame>:<file>     Write the 64 KB main RAM to a file at a frame. Repeatable\n");
	printf("  --tape-swap <frame>:<file>    Load another tape at a frame (next side / part). Repeatable\n");
	printf("  --tape-play <n>               Press Tape play/stop at frame n. Repeatable\n");
	printf("  --tape-rewind <n>             Press Rewind tape (rewind + play) at frame n. Repeatable\n");
	printf("  --stop-at-frame <n>           Exit after frame n\n");
	printf("  --screenshot <n[,n...]>       Save PNG at the listed frames\n");
	printf("  --screenshot-every <n>        Save PNG every n frames\n");
	printf("  --screenshot-dir <dir>        Directory for PNGs (default .)\n");
	printf("  --screenshot-prefix <name>    PNG name prefix (default 'frame' -> frame_0300.png)\n");
	printf("  --send-keys <frame>:<text>    Type text starting at frame. Escapes: \\n Enter, \\b Backspace,\n");
	printf("                                \\U \\D \\L \\R arrows, \\^X Ctrl+X, \\\\ backslash. Repeatable\n");
	printf("  --key-hold <frames>           Frames each key is held (default %d)\n", opt_key_hold);
	printf("  --key-gap <frames>            Frames between keys (default %d)\n", opt_key_gap);
	printf("  --joy <frame>:<hexmask>       Set joystick bits from frame (0 R,1 L,2 D,3 U,4 btn1,5 btn2). Repeatable\n");
	printf("  --reset-at-frame <n>          Pulse reset at frame n. Repeatable\n");
	printf("  --sd-ack-delay <cycles>       SD block latency (default 1200; real HPS is much slower, try 30000+)\n");
	printf("  --writeback                   Persist floppy writes to the image file (default: in-memory only)\n");
	printf("  --verbose-disk                Log every SD block transfer\n");
	printf("  --legacy-main-interleave      Nibblize .dsk with the old (buggy) Main_MiSTer sector layout, for A/B tests\n");
	printf("  --hash                        Print a framebuffer hash every frame (FBHASH lines)\n");
	printf("  --cpu-log                     Per-instruction CPU log (debug console; stdout when headless)\n");
	printf("  --watch <lo[-hi]>             Log RAM writes in a hex address range with frame and approx PC\n");
	printf("  --cpu-log-from <n>/--cpu-log-to <n>  Limit the CPU log to a frame range\n");
	printf("  -h, --help                    This help\n");
}

static unsigned long long fb_hash() {
	unsigned long long h = 1469598103934665603ULL;
	if (!output_ptr) return 0;
	for (int i = 0; i < video.output_width * video.output_height; i++) {
		h ^= output_ptr[i] & 0xFFFFFF;
		h *= 1099511628211ULL;
	}
	return h;
}

static void save_screenshot(int frame) {
	if (!output_ptr) return;
	char filename[1024];
	snprintf(filename, sizeof(filename), "%s/%s_%04d.png", opt_screenshot_dir.c_str(), opt_screenshot_prefix.c_str(), frame);
	int w = video.output_width, h = video.output_height;
	std::vector<uint8_t> rgb(w * h * 3);
	for (int i = 0; i < w * h; i++) {
		uint32_t p = output_ptr[i];
		rgb[i * 3 + 0] = p & 0xFF;
		rgb[i * 3 + 1] = (p >> 8) & 0xFF;
		rgb[i * 3 + 2] = (p >> 16) & 0xFF;
	}
	if (stbi_write_png(filename, w, h, 3, rgb.data(), w * 3))
		printf("SCREENSHOT frame %d %s FBHASH %016llx\n", frame, filename, fb_hash());
	else
		printf("ERROR: could not write %s\n", filename);
}

// ASCII -> PS/2 set-2 scancode (+ whether SHIFT is needed) for the TK2000
// keyboard.sv mapping (US layout keys; TK2000 BASIC is upper case).
static bool ascii_to_ps2(char c, unsigned char &code, bool &shift) {
	static const char *letters = "abcdefghijklmnopqrstuvwxyz";
	static const unsigned char lcode[26] = { 0x1C,0x32,0x21,0x23,0x24,0x2B,0x34,0x33,0x43,0x3B,0x42,0x4B,0x3A,
	                                         0x31,0x44,0x4D,0x15,0x2D,0x1B,0x2C,0x3C,0x2A,0x1D,0x22,0x35,0x1A };
	static const unsigned char dcode[10] = { 0x45,0x16,0x1E,0x26,0x25,0x2E,0x36,0x3D,0x3E,0x46 };
	shift = false;
	char l = tolower(c);
	if (l >= 'a' && l <= 'z') { code = lcode[strchr(letters, l) - letters]; return true; }
	if (c >= '0' && c <= '9') { code = dcode[c - '0']; return true; }
	switch (c) {
	case ' ': code = 0x29; return true;
	case '\n': case '\r': code = 0x5A; return true;
	case ',': code = 0x41; return true;
	case '.': code = 0x49; return true;
	case '/': code = 0x4A; return true;
	case ';': code = 0x4C; return true;
	case ':': code = 0x4C; shift = true; return true;
	case '-': code = 0x4E; return true;
	case '=': code = 0x55; return true;
	case '+': code = 0x55; shift = true; return true;
	case '\'': code = 0x0E; return true;               // keyboard.sv: ' " on the ` key
	case '"': code = 0x0E; shift = true; return true;
	case '!': code = 0x16; shift = true; return true;
	case '#': code = 0x26; shift = true; return true;
	case '$': code = 0x25; shift = true; return true;
	case '%': code = 0x2E; shift = true; return true;
	case '&': code = 0x3D; shift = true; return true;
	case '*': code = 0x3E; shift = true; return true;
	case '(': code = 0x46; shift = true; return true;
	case ')': code = 0x45; shift = true; return true;
	case '?': code = 0x4A; shift = true; return true;
	case '<': code = 0x41; shift = true; return true;
	case '>': code = 0x49; shift = true; return true;
	case '@': code = 0x1E; shift = true; return true;
	}
	return false;
}

static void add_key_text(int frame, const std::string &text) {
	int f = frame;
	auto push = [&](unsigned char code, bool ext, bool shift, bool ctrl) {
		if (shift) key_events.push_back({ f, 0x12, false, true });
		if (ctrl) key_events.push_back({ f, 0x14, false, true });
		key_events.push_back({ f, code, ext, true });
		key_events.push_back({ f + opt_key_hold, code, ext, false });
		if (ctrl) key_events.push_back({ f + opt_key_hold, 0x14, false, false });
		if (shift) key_events.push_back({ f + opt_key_hold, 0x12, false, false });
		f += opt_key_hold + opt_key_gap;
	};
	for (size_t j = 0; j < text.size(); j++) {
		char c = text[j];
		if (c == '\\' && j + 1 < text.size()) {
			char n = text[++j];
			if (n == 'n' || n == 'r') { push(0x5A, false, false, false); continue; }
			if (n == 'b') { push(0x66, false, false, false); continue; }
			if (n == 'U') { push(0x75, true, false, false); continue; }
			if (n == 'D') { push(0x72, true, false, false); continue; }
			if (n == 'L') { push(0x6B, true, false, false); continue; }
			if (n == 'R') { push(0x74, true, false, false); continue; }
			if (n == 'e') { push(0x76, false, false, false); continue; }
			if (n == '^' && j + 1 < text.size()) {
				unsigned char code; bool shift;
				if (ascii_to_ps2(text[++j], code, shift)) push(code, false, false, true);
				continue;
			}
			c = n;
		}
		unsigned char code; bool shift;
		if (ascii_to_ps2(c, code, shift)) push(code, false, shift, false);
		else fprintf(stderr, "WARNING: no TK2000 key for '%c'\n", c);
	}
	std::stable_sort(key_events.begin(), key_events.end(),
	                 [](const KeyEvent &a, const KeyEvent &b) { return a.frame < b.frame; });
}

// One simulated clock step plus cycle-accurate injection of queued key events.
static inline void step() {
	if (key_gap_cycles > 0) key_gap_cycles--;
	else if (key_event_pos < key_events.size() && key_events[key_event_pos].frame <= video.count_frame) {
		const KeyEvent &e = key_events[key_event_pos++];
		ps2_toggle = !ps2_toggle;
		top->ps2_key = e.code | (e.ext ? 1 << 8 : 0) | (e.pressed ? 1 << 9 : 0) | (ps2_toggle ? 1 << 10 : 0);
		key_gap_cycles = 20000;
	}
	if (tape_pulse > 0 && --tape_pulse == 0) { top->tape_rewind = 0; top->tape_play_toggle = 0; }
	if (reset_hold > 0) {
		top->reset = 1;
		if (--reset_hold == 0) top->reset = 0;
	}
	verilate();
}

static void mount_drive(int drive, const std::string &path) {
	if (path.empty()) { blockdevice.EjectDisk(drive); floppy_mounted[drive] = false; return; }
	floppy_mounted[drive] = blockdevice.MountDisk(path, drive);
}

static void print_stats() {
	for (int d = 0; d < 2; d++) {
		if (blockdevice.disk_name[d].empty() && !blockdevice.blocks_read[d]) continue;
		printf("DISKSTATS drive %d %s blocks_read %ld blocks_written %ld sectors_written_back %ld\n", d + 1,
		       blockdevice.disk_name[d].c_str(), blockdevice.blocks_read[d], blockdevice.blocks_written[d],
		       blockdevice.sectors_written_back[d]);
	}
}

// Called once per new frame (both GUI and headless).
static bool on_frame(int frame) {
	for (int d = 0; d < 2; d++) {
		if (!opt_floppy[d].empty() && !floppy_mounted[d] && opt_mount_frame[d] >= 0 && frame >= opt_mount_frame[d]) {
			printf("FRAME %d: insert drive %d %s\n", frame, d + 1, opt_floppy[d].c_str());
			mount_drive(d, opt_floppy[d]);
			opt_mount_frame[d] = -1;
		}
	}
	for (auto &s : opt_swaps) {
		if (!s.done && frame >= s.frame) {
			printf("FRAME %d: %s drive %d %s\n", frame, s.path.empty() ? "eject" : "swap", s.drive + 1, s.path.c_str());
			mount_drive(s.drive, s.path);
			s.done = true;
		}
	}
	if (opt_cpu_log_from >= 0 && frame == opt_cpu_log_from) cpu_log = true;
	if (opt_cpu_log_to >= 0 && frame == opt_cpu_log_to) cpu_log = false;
	if (!opt_tape.empty() && opt_tape_frame >= 0 && frame >= opt_tape_frame) {
		printf("FRAME %d: load tape %s\n", frame, opt_tape.c_str());
		bus.QueueDownload(opt_tape, 2, true);
		opt_tape_frame = -1;
	}
	for (auto &t : opt_tape_swaps) if (!t.done && frame >= t.frame) {
		printf("FRAME %d: load tape %s\n", frame, t.path.c_str());
		bus.QueueDownload(t.path, 2, true);
		t.done = true;
	}
	for (int f : opt_tape_rewind) if (f == frame) { printf("FRAME %d: tape rewind\n", frame); top->tape_rewind = 1; tape_pulse = 64; }
	for (int f : opt_tape_play) if (f == frame) { printf("FRAME %d: tape play/stop\n", frame); top->tape_play_toggle = 1; tape_pulse = 64; }
	if (top->tape_loaded_o != tape_last_loaded || top->tape_playing_o != tape_last_playing) {
		printf("FRAME %d: TAPE loaded=%d playing=%d turbo=%d\n", frame, top->tape_loaded_o, top->tape_playing_o, top->turbo_o);
		tape_last_loaded = top->tape_loaded_o; tape_last_playing = top->tape_playing_o;
	}
	for (auto &j : joy_events) if (j.frame == frame) joy_mask = j.mask;
	top->joystick_0 = joy_mask;
	for (int r : opt_reset_frames) if (r == frame) { printf("FRAME %d: reset\n", frame); reset_hold = 2000; }

	for (auto &d : opt_ram_dumps) if (d.first == frame) {
		FILE *f = fopen(d.second.c_str(), "wb");
		if (f) {
			for (int i = 0; i < 65536; i++) fputc(VERTOPINTERN->emu__DOT__ram__DOT__mem[i], f);
			fclose(f);
			printf("FRAME %d: RAM dumped to %s\n", frame, d.second.c_str());
		}
	}
	if (opt_hash_every_frame) printf("FBHASH frame %d %016llx\n", frame, fb_hash());
	bool shot = std::find(opt_screenshot_frames.begin(), opt_screenshot_frames.end(), frame) != opt_screenshot_frames.end();
	if (opt_screenshot_every > 0 && frame > 0 && frame % opt_screenshot_every == 0) shot = true;
	if (shot) save_screenshot(frame);

	if (opt_stop_at_frame >= 0 && frame >= opt_stop_at_frame) {
		printf("STOP at frame %d (sim time %lu)\n", frame, (unsigned long)main_time);
		print_stats();
		return false;
	}
	return true;
}

static int parse_args(int argc, char **argv) {
	const char *eh = getenv("HEADLESS");
	if (eh && eh[0] && eh[0] != '0') headless = true;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](void) -> std::string {
			if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
			return argv[++i];
		};
		if (a == "-h" || a == "--help") { usage(); exit(0); }
		else if (a == "--headless" || a == "--no-gui") headless = true;
		else if (a == "--floppy" || a == "--disk1" || a == "--disk") opt_floppy[0] = next();
		else if (a == "--disk2") opt_floppy[1] = next();
		else if (a == "--mount-frame") opt_mount_frame[0] = atoi(next().c_str());
		else if (a == "--swap") {
			std::string v = next();
			size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
			if (c1 == std::string::npos || c2 == std::string::npos) { fprintf(stderr, "--swap wants frame:drive:file\n"); return 2; }
			int drive = atoi(v.substr(c1 + 1, c2 - c1 - 1).c_str());
			if (drive < 1 || drive > 2) { fprintf(stderr, "--swap drive must be 1 or 2\n"); return 2; }
			opt_swaps.push_back({ atoi(v.substr(0, c1).c_str()), drive - 1, v.substr(c2 + 1), false });
		}
		else if (a == "--stop-at-frame") opt_stop_at_frame = atoi(next().c_str());
		else if (a == "--screenshot" || a == "-screenshot") {
			std::stringstream ss(next()); std::string t;
			while (std::getline(ss, t, ',')) if (!t.empty()) opt_screenshot_frames.push_back(atoi(t.c_str()));
		}
		else if (a == "--screenshot-every") opt_screenshot_every = atoi(next().c_str());
		else if (a == "--screenshot-dir") opt_screenshot_dir = next();
		else if (a == "--screenshot-prefix") opt_screenshot_prefix = next();
		else if (a == "--key-hold") opt_key_hold = atoi(next().c_str());
		else if (a == "--key-gap") opt_key_gap = atoi(next().c_str());
		else if (a == "--send-keys") {
			std::string v = next();
			size_t c = v.find(':');
			if (c == std::string::npos) { fprintf(stderr, "--send-keys wants frame:text\n"); return 2; }
			add_key_text(atoi(v.substr(0, c).c_str()), v.substr(c + 1));
		}
		else if (a == "--joy") {
			std::string v = next();
			size_t c = v.find(':');
			if (c == std::string::npos) { fprintf(stderr, "--joy wants frame:hexmask\n"); return 2; }
			joy_events.push_back({ atoi(v.substr(0, c).c_str()), (unsigned)strtoul(v.substr(c + 1).c_str(), NULL, 16) });
		}
		else if (a == "--reset-at-frame") opt_reset_frames.push_back(atoi(next().c_str()));
		else if (a == "--sd-ack-delay") blockdevice.sd_ack_delay = atoi(next().c_str());
		else if (a == "--no-disk-rom") top->disk_rom_off = 1;
		else if (a == "--tape-normal") top->tape_normal_speed = 1;
		else if (a == "--tape-overlay-off") top->tape_overlay_off = 1;
		else if (a == "--tape") { opt_tape = next(); }
		else if (a == "--tape-frame") opt_tape_frame = atoi(next().c_str());
		else if (a == "--watch-from") watch_from = atoi(next().c_str());
		else if (a == "--dump-ram") {
			std::string v = next(); size_t c = v.find(':');
			if (c == std::string::npos) { fprintf(stderr, "--dump-ram wants frame:file\n"); return 2; }
			opt_ram_dumps.push_back({ atoi(v.substr(0, c).c_str()), v.substr(c + 1) });
		}
		else if (a == "--tape-swap") {
			std::string v = next(); size_t c = v.find(':');
			if (c == std::string::npos) { fprintf(stderr, "--tape-swap wants frame:file\n"); return 2; }
			opt_tape_swaps.push_back({ atoi(v.substr(0, c).c_str()), v.substr(c + 1), false });
		}
		else if (a == "--tape-rewind") opt_tape_rewind.push_back(atoi(next().c_str()));
		else if (a == "--tape-play") opt_tape_play.push_back(atoi(next().c_str()));
		else if (a == "--writeback") blockdevice.writeback = true;
		else if (a == "--verbose-disk") blockdevice.verbose = true;
		else if (a == "--legacy-main-interleave") blockdevice.legacy_interleave = true;
		else if (a == "--hash") opt_hash_every_frame = true;
		else if (a == "--cpu-log") cpu_log = true;
		else if (a == "--cpu-log-from") opt_cpu_log_from = atoi(next().c_str());
		else if (a == "--cpu-log-to") opt_cpu_log_to = atoi(next().c_str());
		else if (a == "--watch") {
			std::string v = next(); size_t d = v.find('-');
			watch_lo = strtol(v.substr(0, d).c_str(), NULL, 16);
			watch_hi = d == std::string::npos ? watch_lo : strtol(v.substr(d + 1).c_str(), NULL, 16);
		}
		else { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
	}
	return 0;
}

int main(int argc, char** argv, char** env) {

	// Create core and initialise
	top = new Vemu();
	Verilated::commandArgs(argc, argv);
	if (int r = parse_args(argc, argv)) return r;

	// Attach bus
	bus.ioctl_addr = &top->ioctl_addr;
	bus.ioctl_index = &top->ioctl_index;
	bus.ioctl_wait = &top->ioctl_wait;
	bus.ioctl_download = &top->ioctl_download;
	bus.ioctl_wr = &top->ioctl_wr;
	bus.ioctl_dout = &top->ioctl_dout;
	input.ps2_key = &top->ps2_key;

	// hookup blk device
	blockdevice.sd_lba[0] = &top->sd_lba[0];
	blockdevice.sd_lba[1] = &top->sd_lba[1];
	blockdevice.sd_lba[2] = &top->sd_lba[2];
	blockdevice.sd_rd = &top->sd_rd;
	blockdevice.sd_wr = &top->sd_wr;
	blockdevice.sd_ack = &top->sd_ack;
	blockdevice.sd_buff_addr= &top->sd_buff_addr;
	blockdevice.sd_buff_dout= &top->sd_buff_dout;
	blockdevice.sd_buff_din[0]= &top->sd_buff_din[0];
	blockdevice.sd_buff_din[1]= &top->sd_buff_din[1];
	blockdevice.sd_buff_din[2]= &top->sd_buff_din[2];
	blockdevice.sd_buff_wr= &top->sd_buff_wr;
	blockdevice.img_mounted= &top->img_mounted;
	blockdevice.img_readonly= &top->img_readonly;
	blockdevice.img_size= &top->img_size;

	top->paddle_0 = 128;

	// Setup video output (allocates the framebuffer; no window when headless)
	if (video.Initialise(windowTitle) == 1) { return 1; }

	if (!headless) {
		send_clock();
#ifndef DISABLE_AUDIO
		audio.Initialise();
#endif
		input.Initialise();
		input.SetMapping(input_up, SDL_SCANCODE_UP);
		input.SetMapping(input_right, SDL_SCANCODE_RIGHT);
		input.SetMapping(input_down, SDL_SCANCODE_DOWN);
		input.SetMapping(input_left, SDL_SCANCODE_LEFT);
		input.SetMapping(input_a, SDL_SCANCODE_A);
		input.SetMapping(input_b, SDL_SCANCODE_B);
		input.SetMapping(input_x, SDL_SCANCODE_X);
		input.SetMapping(input_y, SDL_SCANCODE_Y);
		input.SetMapping(input_l, SDL_SCANCODE_L);
		input.SetMapping(input_r, SDL_SCANCODE_E);
		input.SetMapping(input_start, SDL_SCANCODE_1);
		input.SetMapping(input_select, SDL_SCANCODE_2);
		input.SetMapping(input_menu, SDL_SCANCODE_M);
	}

	// Legacy default: floppy.nib in the current directory when nothing is given.
	if (opt_floppy[0].empty() && opt_floppy[1].empty()) {
		std::ifstream f("floppy.nib");
		if (f.good()) opt_floppy[0] = "floppy.nib";
	}

	int last_frame = -1;
	auto check_frame = [&]() -> bool {
		if (video.count_frame != last_frame) {
			last_frame = video.count_frame;
			return on_frame(last_frame);
		}
		return true;
	};

	if (headless) {
		auto t0 = std::chrono::steady_clock::now();
		check_frame();
		while (true) {
			for (int s = 0; s < 4096; s++) step();
			if (!check_frame()) break;
		}
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		printf("Ran %d frames in %.1fs (%.1f fps) video x %d..%d y %d..%d\n", last_frame, secs, last_frame / secs, video.stats_xMin, video.stats_xMax, video.stats_yMin, video.stats_yMax);
		top->final();
		return 0;
	}

	bool done = false;
	while (!done)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			ImGui_ImplSDL2_ProcessEvent(&event);
			if (event.type == SDL_QUIT)
				done = true;
		}
		video.StartFrame();
		input.Read();

		ImGui::NewFrame();

		// Simulation control window
		ImGui::Begin(windowTitle_Control);
		ImGui::SetWindowPos(windowTitle_Control, ImVec2(0, 0), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Control, ImVec2(500, 170), ImGuiCond_Once);
		if (ImGui::Button("Reset simulation")) { resetSim(); } ImGui::SameLine();
		if (ImGui::Button("Start running")) { run_enable = 1; } ImGui::SameLine();
		if (ImGui::Button("Stop running")) { run_enable = 0; } ImGui::SameLine();
		ImGui::Checkbox("RUN", &run_enable);
		ImGui::SliderInt("Run batch size", &batchSize, 1, 1750000);
		if (single_step == 1) { single_step = 0; }
		if (ImGui::Button("Single Step")) { run_enable = 0; single_step = 1; }
		ImGui::SameLine();
		if (multi_step == 1) { multi_step = 0; }
		if (ImGui::Button("Multi Step")) { run_enable = 0; multi_step = 1; }
		ImGui::SliderInt("Multi step amount", &multi_step_amount, 8, 1024);
		if (ImGui::Button("Reset core")) { reset_hold = 2000; } ImGui::SameLine();
		if (ImGui::Button("Screenshot")) { save_screenshot(video.count_frame); } ImGui::SameLine();
		ImGui::Checkbox("CPU log", &cpu_log);
		ImGui::Text("D1: %s  D2: %s", blockdevice.disk_name[0].c_str(), blockdevice.disk_name[1].c_str());
		ImGui::End();

		console.Draw(windowTitle_DebugLog, &showDebugLog, ImVec2(500, 700));
		ImGui::SetWindowPos(windowTitle_DebugLog, ImVec2(0, 180), ImGuiCond_Once);

		int windowX = 550;
		int windowWidth = (VGA_WIDTH * VGA_SCALE_X) + 24;
		int windowHeight = (VGA_HEIGHT * VGA_SCALE_Y) + 90;

		ImGui::Begin(windowTitle_Video);
		ImGui::SetWindowPos(windowTitle_Video, ImVec2(windowX, 0), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Video, ImVec2(windowWidth, windowHeight), ImGuiCond_Once);
		ImGui::SliderFloat("Zoom", &vga_scale, 1, 8); ImGui::SameLine();
		ImGui::SliderInt("Rotate", &video.output_rotate, -1, 1); ImGui::SameLine();
		ImGui::Checkbox("Flip V", &video.output_vflip);
		ImGui::Text("main_time: %ld frame_count: %d sim FPS: %f", main_time, video.count_frame, video.stats_fps);
		ImGui::Image(video.texture_id, ImVec2(video.output_width * VGA_SCALE_X, video.output_height * VGA_SCALE_Y));
		ImGui::End();

#ifndef DISABLE_AUDIO
		ImGui::Begin(windowTitle_Audio);
		ImGui::SetWindowPos(windowTitle_Audio, ImVec2(windowX, windowHeight), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Audio, ImVec2(windowWidth, 250), ImGuiCond_Once);
		if (run_enable) {
			audio.CollectDebug((signed short)top->AUDIO_L, (signed short)top->AUDIO_R);
		}
		int channelWidth = (windowWidth / 2)  -16;
		ImPlot::CreateContext();
		if (ImPlot::BeginPlot("Audio - L", ImVec2(channelWidth, 220), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
			ImPlot::SetupAxes("T", "A", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks);
			ImPlot::SetupAxesLimits(0, 1, -1, 1, ImPlotCond_Once);
			ImPlot::PlotStairs("", audio.debug_positions, audio.debug_wave_l, audio.debug_max_samples, audio.debug_pos);
			ImPlot::EndPlot();
		}
		ImPlot::DestroyContext();
		ImGui::End();
#endif

		video.UpdateTexture();

		// Keyboard from the window, joystick from the window unless injected
		if (joy_events.empty()) {
			joy_mask = 0;
			for (int i = 0; i < input.inputCount; i++)
				if (input.inputs[i]) joy_mask |= (1 << i);
		}
		top->menu = input.inputs[input_menu];

		if (run_enable) {
			for (int s = 0; s < batchSize; s++) { step(); if (!check_frame()) { done = true; break; } }
		}
		else {
			if (single_step) { step(); }
			if (multi_step) { for (int s = 0; s < multi_step_amount; s++) step(); }
			check_frame();
		}
	}

#ifndef DISABLE_AUDIO
	audio.CleanUp();
#endif
	video.CleanUp();
	input.CleanUp();
	print_stats();
	return 0;
}
