/* vt_glue.c -- everything the VirtualT CPU core needs that normally lives
 * in m100emu.c / the FLTK front end: globals, inert peripheral stubs,
 * interrupt dispatch, the run loop, and a .BA loader.  Shared by the host
 * test harness (hl_main.c) and the PicoCalc build (pico_main.c).
 * The platform provides drawbyte(), lcdcommand(), show_error() and
 * platform_load_rom(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "VirtualT.h"
#include "cpu.h"
#include "io.h"
#include "serial.h"
#include "m100emu.h"
#include "memory.h"
#include "roms.h"
#include "setup.h"
#include "doins.h"
#include "vt_basic.h"

extern int gRamBottom;
peripheral_setup_t setup;
memory_setup_t mem_setup;

/* ---- globals normally defined in m100emu.c ---- */
int				gModel = MODEL_M100;
volatile uchar	cpu[14];
mem_monitor_cb	gMemMonitor1 = NULL;
mem_monitor_cb	gMemMonitor2 = NULL;
int				fullspeed = 1;	/* LCD busy check off (it uses wall-clock time) */
volatile UINT64	cycles = 0;
volatile int	cycle_delta;
volatile UINT64	instructs = 0;
int				trace = 0;
FILE			*tracefile = NULL;
volatile DWORD	rst7cycles = 9830;
volatile int	gExitApp = 0;
volatile int	gExitLoop = 0;
char			gsOptRomFile[256];
int				gShowVersion = 0;
int				gNoGUI = 1;
char			file[512];
RomDescription_t *gStdRomDesc = NULL;
char			gDebugActive = 0;
char			gDebugInts = 1;
char			gIntActive = 0;
unsigned short	gIntSP = 0;
char			gStopped = 0;
char			gSingleStep = 0;
debug_monitor_callback gpDebugMonitors[3] = { NULL, NULL, NULL };
static volatile UINT64 last_isr_cycle = 0;

extern int		gDelayUpdateKeys;
extern unsigned char gKeyStates[128];
extern unsigned long gSpecialKeys;
extern uchar	gReMem;
void update_keys(void);

/* the platform replaces this to show the Model 100 switched off */
__attribute__((weak)) void power_down(void) {}
void display_map_mode(char *s) { (void)s; }
void get_emulation_path(char *f, int model) { (void)model; strcpy(f, "./"); }
int  load_hex_file(char *f, char *m) { (void)f; (void)m; return 0; }

/* sound / serial / printer / clock / dock: all inert */
int  sound_enable = 0;           /* the platform turns it on */
__attribute__((weak)) void sound_start_tone(int f) { (void)f; }
__attribute__((weak)) void sound_stop_tone(void) {}
__attribute__((weak)) void sound_click(void) { sound_stop_tone(); }
__attribute__((weak)) void sound_toggle_speaker(int s) { (void)s; }
int  tdock_read(uchar p) { (void)p; return 0; }
void tdock_write(uchar p, uchar d) { (void)p; (void)d; }
void send_to_lpt(uchar b) { (void)b; }
void handle_wheel_keys(void) {}
int  ser_init(void) { return 0; }
int  ser_deinit(void) { return 0; }
int  ser_get_flags(unsigned char *f) { *f = 0; return SER_PORT_NOT_OPEN; }   /* "no port": the Model 200 then treats it as ready */
int  ser_read_byte(char *b) { (void)b; return 1; }
int  ser_write_byte(char b) { (void)b; return 0; }
int  ser_set_baud(int b) { (void)b; return 0; }
int  ser_set_bit_size(int b) { (void)b; return 0; }
int  ser_set_parity(char p) { (void)p; return 0; }
int  ser_set_stop_bits(int s) { (void)s; return 0; }
int  ser_set_signals(unsigned char s) { (void)s; return 0; }
int  ser_set_callback(ser_callback cb) { (void)cb; return 0; }
void cb_int65(int pinLevel) { if (pinLevel) IM |= 0x20; else IM &= 0xDF; }

/* ---- interrupts: copied from m100emu.c check_interrupts() ---- */
static void check_interrupts(void)
{
	static UINT64 last_rst75 = 0;

	if (((last_rst75 + rst7cycles) < cycles) && !INTDIS)
	{
		IM |= 0x40;
		last_rst75 = cycles;
	}
	if (RST75PEND && !INTDIS && !RST75MASK)
	{
		gIntSP = SP;
		DECSP2;
		gBaseMemory[SP] = PCL;
		gBaseMemory[SP + 1] = PCH;
		PCL = 60; PCH = 0;
		cycle_delta += 10;
		IM = IM & 0xBF;
		last_isr_cycle = cycles;
		if (gDelayUpdateKeys == 1)
			update_keys();
	}
	else if (RST65PEND && !INTDIS && !RST65MASK)
	{
		gIntSP = SP;
		DECSP2;
		gBaseMemory[SP] = PCL;
		gBaseMemory[SP + 1] = PCH;
		PCL = 52; PCH = 0;
		cycle_delta += 10;
		last_isr_cycle = cycles;
	}
}

/* Run the CPU for 'n' emulated cycles (2.4576 MHz). */
#ifdef PICO_BUILD
/* The CPU loop is ~33 KB -- bigger than the RP2350's 16 KB XIP cache --
   so run it from SRAM instead of flash. */
__attribute__((section(".time_critical.run_cycles"), noinline))
#endif
void run_cycles(UINT64 n)
{
	UINT64 stop = cycles + n;
	unsigned int i, j, v;
	int ins;
	int nxtmaint = 1;
	(void)i; (void)j; (void)v; (void)ins;

	while (cycles < stop)
	{
		#define NO_REMEM
		#include "cpu.h"
		#include "do_instruct.h"

		if ((get_memory8(PC) == 0xF3) || ((IM & 0x2A) == 0x20) ||
			((IM & 0x4C) == 0x40))
			check_interrupts();

		/* what m100emu.c's throttle() does, minus the real-time wait */
		cycles += cycle_delta;
		cycle_delta = 0;
		instructs++;

		if (!(--nxtmaint & 0x3FF))
			check_interrupts();
	}
}

/* ---- .BA loader: a port of VirtualT file.cpp cb_LoadFromHost() for
   already-tokenized BASIC (relocate + RAM directory insert) ---- */
static int relocate_ba(unsigned char *in, unsigned char *out, int len, unsigned short addr)
{
	int c = 0, this_line = 0;
	unsigned short next_line = in[0] | (in[1] << 8);
	while (next_line != 0)
	{
		c += 2;
		if (c + 2 >= len) return VT_BASIC_BAD;
		out[c] = in[c]; c++; out[c] = in[c]; c++;	/* line number */
		while (c < len && in[c] != 0) { out[c] = in[c]; c++; }
		if (c >= len) return VT_BASIC_BAD;	/* no line end: not a .BA image */
		out[c] = 0; c++;
		out[this_line] = (addr + c) & 0xFF;
		out[this_line + 1] = (addr + c) >> 8;
		this_line = c;
		next_line = in[c] | (in[c + 1] << 8);	/* in[len], in[len+1] are 0 */
	}
	out[c++] = 0; out[c++] = 0;
	return c;
}

/* Is a file of this 8-char Model 100 name ("ROG   BA") in the RAM directory? */
int vt_find_file(const char *mtname)
{
	RomDescription_t *r = gStdRomDesc;
	for (int d = 0; d < r->sDirCount; d++)
	{
		unsigned short da = r->sDirectory + 11 * d;
		if (get_memory8(da) == 0)
			continue;
		int same = 1;
		for (int i = 0; i < 8; i++)
			if (get_memory8(da + 3 + i) != (unsigned char)mtname[i]) { same = 0; break; }
		if (same)
			return 1;
	}
	return 0;
}

/* data: a .BA file, len bytes, with 2 spare bytes after it -- either a
   tokenized image (relocated in place) or a plain-text listing (tokenized
   into a separate buffer). Then copied into emulated RAM.
   Returns the length, or -1 no memory, -2 directory full, VT_BASIC_BAD
   (not a BASIC program), VT_BASIC_TOOBIG. */
static unsigned char tokbuf[32768 + 2];
int load_ba(unsigned char *data, int len, const char *mtname)
{
	RomDescription_t *r = gStdRomDesc;
	unsigned char *conv = data;
	data[len] = data[len + 1] = 0;

	unsigned short addr1 = get_memory16(r->sFilePtrBA);
	if (vt_basic_is_ascii(data, len))
	{
		conv = tokbuf;
		len = vt_basic_tokenize(data, len, tokbuf, sizeof tokbuf, addr1);
	}
	else
		len = relocate_ba(data, conv, len, addr1);
	if (len < 0)
		return len;
	if (len <= 2)
		return VT_BASIC_BAD;                 /* no lines at all */
	if (get_memory16(r->sBasicStrings) - get_memory16(r->sBeginVar) < len)
	{ fprintf(stderr, "load_ba: out of memory\n"); return -1; }

	unsigned short addr4 = r->sDirectory + 11 * r->sFirstDirEntry;
	int idx = 0;
	while (get_memory8(addr4) != 0)
	{
		addr4 += 11;
		if (++idx >= r->sDirCount) { fprintf(stderr, "load_ba: directory full\n"); return -2; }
	}

	int move_len = get_memory16(r->sUnusedMem) - addr1;
	for (int x = move_len - 1; x >= 0; x--)
		set_memory8(addr1 + len + x, get_memory8(addr1 + x));

	/* shift existing directory entries */
	for (int d = 0; d < r->sDirCount; d++)
	{
		unsigned short da = r->sDirectory + 11 * d;
		if (get_memory8(da) == 0) continue;
		unsigned short fa = get_memory16(da + 1);
		if (fa >= addr1) set_memory16(da + 1, fa + len);
	}

	set_memory8(addr4, 0x80);	/* TYPE_BA */
	set_memory16(addr4 + 1, addr1);
	for (int x = 0; x < 8; x++)
		set_memory8(addr4 + 3 + x, mtname[x]);

	/* renumber the line links of the unsaved BASIC program that moved */
	unsigned short a3 = get_memory16(r->sFilePtrBA);
	while (get_memory16(a3) != 0)
	{
		set_memory16(a3, get_memory16(a3) + len);
		a3 = get_memory16(a3);
	}

	a3 = get_memory16(r->sBeginArray); if (a3 >= addr1) set_memory16(r->sBeginArray, a3 + len);
	a3 = get_memory16(r->sBeginCO);    if (a3 > addr1)  set_memory16(r->sBeginCO, a3 + len);
	a3 = get_memory16(r->sBeginDO);    if (a3 > addr1)  set_memory16(r->sBeginDO, a3 + len);
	a3 = get_memory16(r->sBasicSize);  set_memory16(r->sBasicSize, a3 + len - 2);
	a3 = get_memory16(r->sBeginVar);   if (a3 >= addr1) set_memory16(r->sBeginVar, a3 + len);
	a3 = get_memory16(r->sUnusedMem);  if (a3 >= addr1) set_memory16(r->sUnusedMem, a3 + len);

	for (int x = 0; x < len; x++)
		set_memory8(addr1 + x, conv[x]);
	fprintf(stderr, "loaded %s: %d bytes at %04X, dir entry %04X\n", mtname, len, addr1, addr4);
	return len;
}


/* .CO (machine code, with its 6-byte header) or .DO (text) file into RAM,
   as VirtualT's cb_LoadFromHost() does: a .CO goes at the start of the
   variables, a .DO at the next .DO position, everything above moves up.
   type is 0xA0 (CO) or 0xC0 (DO); a .DO's LF line ends become CR LF and
   it gets the ^Z end mark (converted into a separate buffer).
   Returns the length, -1 no memory, -2 directory full, VT_BASIC_BAD. */
int load_codo(unsigned char *data, int len, const char *mtname, int type)
{
	RomDescription_t *r = gStdRomDesc;
	unsigned char *conv = data;
	unsigned short addr1;

	if (type == 0xA0)
	{
		/* header: load address, length, entry address */
		if (len < 7 || (data[2] | data[3] << 8) != len - 6)
			return VT_BASIC_BAD;
		addr1 = get_memory16(r->sBeginVar);
	}
	else
	{
		int x = 0, ch = 0;
		conv = tokbuf;
		for (int i = 0; i < len; i++)
		{
			if (data[i] == 0x1A) { ch = 0x1A; break; }
			if (data[i] == 0x0A && ch != 0x0D)
				conv[x++] = 0x0D;
			ch = data[i];
			conv[x++] = ch;
			if (ch == 0x0D && (i + 1 >= len || data[i + 1] != 0x0A))
				conv[x++] = ch = 0x0A;      /* a lone CR (old Mac files): add the LF */
			if (x >= (int)sizeof tokbuf - 2)
				return VT_BASIC_TOOBIG;
		}
		conv[x++] = 0x1A;
		len = x;
		addr1 = get_memory16(r->sFilePtrDO);
	}

	if (get_memory16(r->sBasicStrings) - get_memory16(r->sBeginVar) < len)
		return -1;

	unsigned short addr4 = r->sDirectory + 11 * r->sFirstDirEntry;
	int idx = 0;
	while (get_memory8(addr4) != 0)
	{
		addr4 += 11;
		if (++idx >= r->sDirCount) return -2;
	}

	int move_len = get_memory16(r->sUnusedMem) - addr1;
	for (int x = move_len - 1; x >= 0; x--)
		set_memory8(addr1 + len + x, get_memory8(addr1 + x));

	for (int d = 0; d < r->sDirCount; d++)
	{
		unsigned short da = r->sDirectory + 11 * d;
		if (get_memory8(da) == 0) continue;
		unsigned short fa = get_memory16(da + 1);
		if (fa >= addr1) set_memory16(da + 1, fa + len);
	}

	set_memory8(addr4, type);
	set_memory16(addr4 + 1, addr1);
	for (int x = 0; x < 8; x++)
		set_memory8(addr4 + 3 + x, mtname[x]);

	unsigned short a3;
	a3 = get_memory16(r->sBeginArray); if (a3 >= addr1) set_memory16(r->sBeginArray, a3 + len);
	if (type != 0xA0)
	{
		a3 = get_memory16(r->sBeginCO); if (a3 > addr1) set_memory16(r->sBeginCO, a3 + len);
	}
	a3 = get_memory16(r->sBeginVar);   if (a3 >= addr1) set_memory16(r->sBeginVar, a3 + len);
	a3 = get_memory16(r->sUnusedMem);  if (a3 >= addr1) set_memory16(r->sUnusedMem, a3 + len);

	for (int x = 0; x < len; x++)
		set_memory8(addr1 + x, conv[x]);
	fprintf(stderr, "loaded %s: %d bytes at %04X, dir entry %04X\n", mtname, len, addr1, addr4);
	return len;
}

/* bytes free for a new file, as the loaders check it */
int vt_free_bytes(void)
{
	RomDescription_t *r = gStdRomDesc;
	return get_memory16(r->sBasicStrings) - get_memory16(r->sBeginVar);
}

/* ---- RAM files: list and delete (a port of VirtualT's delete_file) ---- */

/* bytes a RAM file occupies, from its directory entry */
static int ram_file_len(unsigned short dir_addr)
{
	unsigned short fa = get_memory16(dir_addr + 1), a = fa;
	int type = get_memory8(dir_addr);
	if (type == 0xC0)
	{
		int len = 0;
		while (get_memory8(a) != 0x1A && len < 65535) { len++; a++; }
		return len + 1;
	}
	if (type == 0xA0)
		return get_memory16(fa + 2) + 6;
	int guard = 0;
	while (get_memory16(a) != 0 && guard++ < 20000)
		a = get_memory16(a);
	return a - fa + 2;
}

/* the user's .BA/.DO/.CO files in the RAM directory (in the current
   bank on a Model 200): "NAME.BA", size and directory address */
int vt_ram_files(char names[][10], int *sizes, unsigned short *dirs, int max)
{
	RomDescription_t *r = gStdRomDesc;
	int n = 0;
	for (int d = 0; d < r->sDirCount && n < max; d++)
	{
		unsigned short da = r->sDirectory + 11 * d;
		int type = get_memory8(da);
		if (type != 0x80 && type != 0xC0 && type != 0xA0)
			continue;
		char *s = names[n];
		int k = 0;
		for (int i = 0; i < 6; i++)
		{
			char c = get_memory8(da + 3 + i);
			if (c != ' ') s[k++] = c;
		}
		s[k++] = '.';
		s[k++] = get_memory8(da + 9);
		s[k++] = get_memory8(da + 10);
		s[k] = 0;
		sizes[n] = ram_file_len(da);
		dirs[n++] = da;
	}
	return n;
}

/* remove one file from RAM, closing up the space; returns 1 if done */
int vt_delete_file(unsigned short dir_addr)
{
	RomDescription_t *r = gStdRomDesc;
	int type = get_memory8(dir_addr);
	if (type != 0x80 && type != 0xC0 && type != 0xA0)
		return 0;
	unsigned short file_addr = get_memory16(dir_addr + 1);
	int len = ram_file_len(dir_addr);

	set_memory8(dir_addr, 0);              /* directory entry gone */

	int move_len = get_memory16(r->sUnusedMem) - (file_addr + len);
	for (int x = 0; x < move_len; x++)
		set_memory8(file_addr + x, get_memory8(file_addr + len + x));

	for (int d = 0; d < r->sDirCount; d++)
	{
		unsigned short da = r->sDirectory + 11 * d;
		if (get_memory8(da) == 0) continue;
		unsigned short fa = get_memory16(da + 1);
		if (fa >= file_addr) set_memory16(da + 1, fa - len);
	}

	/* line links of BASIC programs that moved down */
	for (int d = 0; d < r->sDirCount; d++)
	{
		unsigned short da = r->sDirectory + 11 * d;
		if (get_memory8(da) != 0x80) continue;
		unsigned short ra = get_memory16(da + 1);
		if (ra < file_addr) continue;
		int guard = 0;
		while (get_memory16(ra) != 0 && guard++ < 20000)
		{
			set_memory16(ra, get_memory16(ra) - len);
			ra = get_memory16(ra);
		}
	}
	if (type == 0x80)
	{
		unsigned short ra = get_memory16(r->sFilePtrBA);
		int guard = 0;
		while (get_memory16(ra) != 0 && guard++ < 20000)
		{
			set_memory16(ra, get_memory16(ra) - len);
			ra = get_memory16(ra);
		}
	}

	unsigned short a3;
	a3 = get_memory16(r->sBeginArray); if (a3 >= file_addr) set_memory16(r->sBeginArray, a3 - len);
	if (type != 0xA0)
	{
		a3 = get_memory16(r->sBeginCO); if (a3 > file_addr) set_memory16(r->sBeginCO, a3 - len);
	}
	if (type == 0x80)
	{
		a3 = get_memory16(r->sBeginDO); if (a3 > file_addr) set_memory16(r->sBeginDO, a3 - len);
		a3 = get_memory16(r->sBasicSize); set_memory16(r->sBasicSize, a3 - (len - 2));
	}
	a3 = get_memory16(r->sBeginVar); if (a3 >= file_addr) set_memory16(r->sBeginVar, a3 - len);
	a3 = get_memory16(r->sUnusedMem); if (a3 >= file_addr) set_memory16(r->sUnusedMem, a3 - len);
	return 1;
}
