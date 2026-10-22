/* pico_main.c -- VirtualT (TRS-80 Model 100) on the ClockworkPi PicoCalc.
 *
 * Boots the Model 100 ROM from the SD card, shows its LCD on the PicoCalc
 * screen, maps the PicoCalc keyboard onto the Model 100 matrix, sets the
 * clock (NTP or saved), keeps the Model 100's RAM on the card, and loads
 * programs from a library folder on the card (Ctrl+F4).
 *
 * SD card:  /virtualt/M100ROM.BIN   (32768 bytes, your own ROM image)
 *           /virtualt/m100/         (.BA .CO .DO library; m200/ for the 200)
 *           /virtualt/M100RAM.BIN   (the Model 100's RAM, saved on exit and
 *                                    every 2 min; hold Esc at launch to
 *                                    ignore it and cold start)
 *
 * Keys: F1-F5 = F1-F5, Shift+F1..F3 = F6-F8, Shift+F4 = LABEL,
 *       Shift+F5 = PRINT, Shift+Tab (Home) = PASTE, Shift+Del (End) = PAUSE,
 *       Shift+Esc = BREAK, Alt = GRPH, Del = DEL, Ctrl+F1 = help/info
 *       panel, Ctrl+F5 = text/pixel view, Ctrl+Esc = exit. Shift+arrows reach the M100 as Shift+arrows
 *       even though the BIOS reports Shift+Up/Down as PgUp/PgDn.
 *       Not mapped yet: CODE.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "drivers/lcd.h"
#include "drivers/audio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "drivers/southbridge.h"
#include "drivers/fat32.h"
#include "drivers/keyboard.h"
#include "drivers/font.h"
#include "picocalc_shell.h"

#include "vt_glue.h"
#include "vt_export.h"
#include "vt_basic.h"
#include "vt_config.h"
#include <time.h>
#include "memory.h"
#include "setup.h"
#include "m100emu.h"

#define BA_DIR     "/virtualt"

/* Model 100 by default; model=200 in virtualt.cfg, or 2 during the
   startup countdown, runs a Model 200 instead */
static int is200;
#define MODEL_NAME (is200 ? "Model 200" : "Model 100")
#define ROM_PATH   (is200 ? BA_DIR "/T200ROM.BIN" : BA_DIR "/M100ROM.BIN")
#define ROM_BYTES  (is200 ? 73728 : 32768)
#define RAM_PATH   (is200 ? BA_DIR "/M200RAM.BIN" : BA_DIR "/M100RAM.BIN")
#define RAM_NEW    (is200 ? BA_DIR "/M200RAM.NEW" : BA_DIR "/M100RAM.NEW")
#define LIB_DIR    (is200 ? BA_DIR "/m200" : BA_DIR "/m100")
#define LIB_NAME   (is200 ? "m200/" : "m100/")

#define CPU_HZ     2457600
#define CHUNK      (CPU_HZ / 100)          /* run 10 ms of emulated time per slice */
#define MIN_HOLD   (CPU_HZ / 25)           /* hold each key >= 40 ms emulated */

/* ---------------------------------------------------------------- LCD */

/* M100 240x64 -> PicoCalc 320x96: x*4/3 (each 6-px character cell becomes
   exactly 8 px, so 40 columns fill the 320-px width), y*3/2. */
#define Y_OFF      24
#define FG         0x07E0                   /* green on black, like a backlit LCD */
#define BG         0x0000

#include "lcdtext.h"
#define lcdram  vt_lcdram
#define top_row vt_top_row
#define dirty   vt_lcd_dirty

/* text view: 40x8 (Model 200: 40x16) cells of 8x16 = 320x128 (320x256),
   see core/lcdtext.c. Pixel view: the raw LCD scaled -- Model 100 to
   320x96 centred in the band, Model 200 to 320x256. */
#define BAND_H     (is200 ? 256 : 128)
static int overlay;         /* help/info panel drawn over the Model 200 screen */
static int pixel_view;      /* Ctrl+F5 toggles */
static int raw_cells;

static uint16_t blitbuf[67 * 48];

/* Redraw one controller's 50x32 block (40 px wide for the last column). */
static void flush_controller(int d)
{
	int sx0 = (d % 5) * 50;
	int sw = sx0 + 50 > 240 ? 240 - sx0 : 50;
	int sy0 = d > 4 ? 32 : 0;
	int dx0 = sx0 * 4 / 3, dx1 = (sx0 + sw) * 4 / 3;
	int dy0 = sy0 * 3 / 2, dy1 = (sy0 + 32) * 3 / 2;
	int dw = dx1 - dx0, dh = dy1 - dy0;

	for (int dy = 0; dy < dh; dy++)
	{
		int sy = (dy0 + dy) * 2 / 3 - sy0;         /* 0..31 within controller */
		int bank = ((sy >> 3) + top_row[d]) & 3;    /* honour scroll start line */
		int bit = sy & 7;
		for (int dx = 0; dx < dw; dx++)
		{
			int sx = (dx0 + dx) * 3 / 4 - sx0;
			if (sx < 0) sx = 0;              /* first column can map just left of this controller */
			uint8_t v = lcdram[d][(bank << 6) | sx];
			blitbuf[dy * dw + dx] = (v >> bit) & 1 ? FG : BG;
		}
	}
	lcd_blit(blitbuf, dx0, Y_OFF + 16 + dy0, dw, dh);
}

/* Model 200 pixel view: one text row (8 LCD pixel rows) at a time,
   x*4/3 (each 6-px byte -> 8 px), y*2 */
static uint16_t rowbuf[320 * 16];
static void flush_t200_row(int r)
{
	static const uint8_t xmap[8] = { 0, 0, 1, 2, 3, 3, 4, 5 };
	for (int y = 0; y < 8; y++)
		for (int c = 0; c < 40; c++)
		{
			uint8_t b = vt_t200_byte(c, r * 8 + y);
			for (int x = 0; x < 8; x++)
			{
				uint16_t px = (b >> xmap[x]) & 1 ? FG : BG;
				rowbuf[(y * 2) * 320 + c * 8 + x] = px;
				rowbuf[(y * 2 + 1) * 320 + c * 8 + x] = px;
			}
		}
	lcd_blit(rowbuf, 0, Y_OFF + r * 16, 320, 16);
}

static void cell_blit(int col, int row, const uint16_t *pix)
{
	lcd_blit(pix, col * 8, Y_OFF + row * 16, 8, 16);
}

static void flush_lcd(void)
{
	if (overlay)
		return;
	if (is200)
	{
		int d = vt_t200_dirty;
		if (!d)
			return;
		vt_t200_dirty = 0;
		if (pixel_view)
		{
			for (int r = 0; r < 16; r++)
				if (d & (1 << r))
					flush_t200_row(r);
		}
		else
			raw_cells = vt_render_cells(cell_blit, FG, BG);
		return;
	}
	uint16_t d = dirty;
	if (!d)
		return;
	dirty = 0;
	if (pixel_view)
	{
		for (int i = 0; i < 10; i++)
			if (d & (1u << i))
				flush_controller(i);
	}
	else
		raw_cells = vt_render_cells(cell_blit, FG, BG);
}

static void toggle_view(void)
{
	pixel_view = !pixel_view;
	lcd_solid_rectangle(BG, 0, Y_OFF, 320, BAND_H);
	vt_invalidate_cells();
	dirty = 0x3FF;
	vt_t200_dirty = 0xFFFF;
}

/* -------------------------------------------------------- platform hooks */

/* The Model 100 switches itself off (auto power-off after 10 minutes
   idle, or POWER OFF) by setting a bit in port BA; VirtualT calls this.
   The ROM then sits with interrupts disabled until power-on. */
static volatile int m100_off;
void power_down(void) { m100_off = 1; }
static volatile int key_event;     /* any key pressed (used to switch back on) */

/* ------------------------------------------------------------- sound */

/* Two ways the Model 100/200 makes sound:
   - BEEP / SOUND: the 8155 timer generates the tone -> sound_start_tone();
   - key clicks and some games toggle the speaker bit directly -> we
     measure the toggle rate and play that tone until the toggling stops
     (VirtualT's approach). Both channels, for the headphone jack. */
extern int sound_enable;
extern volatile int cycle_delta;
static int tone_on, spk_on;
static int cur_freq;
static uint64_t click_until;              /* end of a SOUND 0 click */
static int cur_duty = 500;                 /* per mille, of the speaker square wave */
static uint64_t spk_last;

extern PIO pio;                            /* the audio driver's */
static void play(int freq)
{
	if (freq == cur_freq)
		return;
	cur_freq = freq;
	cur_duty = 500;
	/* drop any duty values still queued for the old tone: a stopped
	   generator never takes them, and the driver's blocking put would
	   then wait for room forever */
	pio_sm_clear_fifos(pio, LEFT_CHANNEL);
	pio_sm_clear_fifos(pio, RIGHT_CHANNEL);
	if (freq > 0)
		audio_play_sound(freq, freq);
	else
		audio_stop();
}

void sound_start_tone(int freq)
{
	if (!sound_enable)
		return;
	tone_on = freq > 0 && freq < 20000;
	click_until = 0;
	play(tone_on ? freq : 0);
}

/* SOUND 0: just the click of the speaker switching on (see io.c) */
void sound_click(void)
{
	if (!sound_enable)
		return;
	tone_on = 0;
	play(1500);
	click_until = cycles + CPU_HZ * 3 / 1000;   /* 3 ms */
}

void sound_stop_tone(void)
{
	tone_on = 0;
	if (!spk_on)
		play(0);
}

/* A program driving the speaker bit itself makes a square wave whose
   period is two toggles; its duty cycle is its loudness (CHIME.200 fades
   a bell by shrinking the on-time from 5% to 0.5%).  Measure both from
   the last two toggles: the pitch from the whole period, and pass the
   duty cycle to the PWM, which plays it the same way. */
static void set_duty(int duty)
{
	/* only while the generator is running (the driver stops it outside
	   20-20000 Hz) and has room; otherwise skip this change */
	if (cur_freq < 20 || cur_freq > 20000 ||     /* the driver's audio_pwm_is_not_silence() */
	    pio_sm_is_tx_fifo_full(pio, LEFT_CHANNEL) || pio_sm_is_tx_fifo_full(pio, RIGHT_CHANNEL))
		return;
	uint32_t period = clock_get_hz(clk_sys) / (cur_freq * 3) & ~1u;
	uint32_t on = (uint32_t)((uint64_t)period * duty / 1000);
	if (on < 1) on = 1;
	pio_sm_put(pio, LEFT_CHANNEL, on);     /* picked up at the next period: no click */
	pio_sm_put(pio, RIGHT_CHANNEL, on);
	cur_duty = duty;
}

void sound_toggle_speaker(int bit)
{
	(void)bit;
	static uint64_t prev_d;
	if (!sound_enable)
		return;
	uint64_t now = cycles + cycle_delta;
	uint64_t d = now - spk_last;
	spk_last = now;
	if (d == 0 || d >= 5000)
	{
		prev_d = 0;                        /* a pause: start measuring afresh */
		return;
	}
	if (prev_d)
	{
		uint64_t per = d + prev_d;
		int f = (int)(2457600 / per);
		int duty = (int)((d < prev_d ? d : prev_d) * 1000 / per);
		/* only retune on a real change, so small jitter doesn't warble */
		if (!spk_on || cur_freq == 0 || f * 100 / cur_freq < 97 || f * 100 / cur_freq > 103)
		{
			play(f);
			cur_duty = 500;
		}
		int step = cur_duty > 10 ? cur_duty : 10;        /* change of over 10% */
		if (cur_freq > 0 && ((duty - cur_duty) * 10 > step || (cur_duty - duty) * 10 > step))
			set_duty(duty);
		spk_on = 1;
	}
	prev_d = d;
}

/* called each slice: end a toggled tone once the toggling has stopped */
static void sound_service(void)
{
	if (click_until && cycles >= click_until)
	{
		click_until = 0;
		if (!tone_on && !spk_on)
			play(0);
	}
	if (spk_on && cycles - spk_last > CPU_HZ / 200)      /* 5 ms quiet */
	{
		spk_on = 0;
		if (!tone_on)
			play(0);
	}
}

static char g_err[64];
void show_error(const char *s) { strncpy(g_err, s, sizeof g_err - 1); }

int platform_load_rom(uchar *buf, int len)
{
	fat32_file_t f;
	size_t n = 0;
	if (fat32_open(&f, ROM_PATH) != FAT32_OK)
		return 0;
	fat32_read(&f, buf, len, &n);
	fat32_close(&f);
	return (int)n;
}

/* ------------------------------------------------------------- keyboard */

/* Model 100 keyboard layout as probed from the ROM: which unshifted key
   produces each shifted character. */
static const char sh_from[] = "!@#$%^&*()_+]:\"<>?";
static const char sh_to[]   = "1234567890-=[;',./";

typedef struct {
	uint8_t  active;     /* key is down (or waiting out its min hold) */
	uint8_t  is_char;    /* 1 = gKeyStates[val], 0 = gSpecialKeys bit */
	int8_t   shift;      /* -1 = leave Shift alone, 0/1 = force it */
	uint8_t  release;    /* release seen, waiting for MIN_HOLD */
	uint32_t val;
	uint64_t down;       /* emulated cycle count at press */
} keyslot_t;

static keyslot_t slots[256];
static int phys_shift, phys_ctrl;
static int8_t last_forced = -1;
static int want_exit;
static volatile int want_panel_cycle;
static volatile int want_test_freeze;
static volatile int want_export;
static volatile int want_dump;
static volatile int want_lib, lib_key, want_panel_close;
static int lib_open;

static void apply_shift(void)
{
	int s = phys_shift;
	if (last_forced >= 0)
		for (int i = 0; i < 256; i++)
			if (slots[i].active && slots[i].shift >= 0) { s = slots[i].shift; break; }
	if (s) gSpecialKeys &= ~MT_SHIFT; else gSpecialKeys |= MT_SHIFT;
}

static void slot_down(int code, int is_char, uint32_t val, int shift)
{
	keyslot_t *k = &slots[code];
	k->active = 1; k->is_char = is_char; k->val = val; k->shift = shift;
	k->release = 0; k->down = cycles;
	if (shift >= 0) last_forced = shift;
	if (is_char) gKeyStates[val] = 1; else gSpecialKeys &= ~val;
	apply_shift();
	update_keys();
}

static void slot_up(keyslot_t *k)
{
	k->active = 0; k->release = 0;
	if (k->is_char) gKeyStates[k->val] = 0; else gSpecialKeys |= k->val;
	apply_shift();
	update_keys();
}

static void mod_set(uint32_t bit, int down)
{
	if (down) gSpecialKeys &= ~bit; else gSpecialKeys |= bit;
	update_keys();
}

/* map one PicoCalc key code to a Model 100 key */
static int help_or_info_showing(void);

static void key_press(int code)
{
	/* the library panel takes the keys while it is open */
	if (lib_open && !(phys_ctrl && (code == KEY_F1 || code == KEY_F4 || code == KEY_ESC || code == KEY_F5)))
	{
		lib_key = code;
		return;
	}
	/* with Help or Info showing, Space, Enter and Esc just close it (as
	   "more" would), instead of reaching the Model 100 */
	if (help_or_info_showing() && !phys_ctrl &&
	    (code == KEY_SPACE || code == KEY_ENTER || code == KEY_RETURN || code == KEY_ESC))
	{
		want_panel_close = 1;
		return;
	}
	switch (code)
	{
	case KEY_ENTER: case KEY_RETURN: slot_down(code, 0, MT_ENTER, -1); return;
	case KEY_BACKSPACE: slot_down(code, 0, MT_BKSP, -1); return;
	case KEY_DEL:   slot_down(code, 0, MT_BKSP, 1); return;
	case KEY_TAB:   slot_down(code, 0, MT_TAB, -1); return;
	case KEY_ESC:
		if (phys_ctrl) { want_exit = 1; return; }
		slot_down(code, 0, MT_ESC, -1); return;
	case KEY_SPACE: slot_down(code, 0, MT_SPACE, -1); return;
	case KEY_UP:    slot_down(code, 0, MT_UP, -1); return;
	case KEY_DOWN:  slot_down(code, 0, MT_DOWN, -1); return;
	case KEY_LEFT:  slot_down(code, 0, MT_LEFT, -1); return;
	case KEY_RIGHT: slot_down(code, 0, MT_RIGHT, -1); return;
	case KEY_F1:
		if (phys_ctrl) { want_panel_cycle = 1; return; }
		slot_down(code, 0, MT_F1, -1); return;
	case KEY_F3:
		if (phys_ctrl) { want_export = 1; return; }
		slot_down(code, 0, MT_F3, -1); return;
	case KEY_F2:
		if (phys_ctrl) { want_dump = 1; return; }
		slot_down(code, 0, MT_F2, -1); return;
	case KEY_F4:
		if (phys_ctrl) { want_lib = 1; return; }
		slot_down(code, 0, MT_F4, -1); return;
	case KEY_F5:
		if (phys_ctrl) { toggle_view(); return; }
		slot_down(code, 0, MT_F5, -1); return;
	/* the BIOS turns Shift+F1..F5 into F6..F10; the M100 has no
	   shifted F-keys worth keeping, so drop Shift for these */
	case KEY_F6: slot_down(code, 0, MT_F6, 0); return;
	case KEY_F7: slot_down(code, 0, MT_F7, 0); return;
	case KEY_F8: slot_down(code, 0, MT_F8, 0); return;
	case KEY_F9: slot_down(code, 0, MT_LABEL, 0); return;
	case KEY_F10: slot_down(code, 0, MT_PRINT, 0); return;
	case KEY_HOME: slot_down(code, 0, MT_PASTE, -1); return;
	/* The PicoCalc BIOS turns Shift+Up/Down into PgUp/PgDn and Shift+Enter
	   into Insert; give the Model 100 the Shift+key it actually wanted. */
	case KEY_PAGE_UP:   slot_down(code, 0, MT_UP, 1); return;
	case KEY_PAGE_DOWN: slot_down(code, 0, MT_DOWN, 1); return;
	case KEY_INSERT:    slot_down(code, 0, MT_ENTER, 1); return;
	case KEY_END:  slot_down(code, 0, MT_PAUSE, -1); return;
	case KEY_BREAK: slot_down(code, 0, MT_PAUSE, 1); return;
	}

	/* Ctrl+letter may arrive as a control code */
	if (code >= 1 && code <= 26) { slot_down(code, 1, 'a' + code - 1, 0); return; }
	if (code >= 'a' && code <= 'z') { slot_down(code, 1, code, 0); return; }
	if (code >= 'A' && code <= 'Z') { slot_down(code, 1, code - 'A' + 'a', 1); return; }
	if (code >= '0' && code <= '9') { slot_down(code, 1, code, 0); return; }
	const char *p = strchr(sh_from, code);
	if (code && p) { slot_down(code, 1, sh_to[p - sh_from], 1); return; }
	if (code && strchr("-=[;',./", code)) { slot_down(code, 1, code, 0); return; }
	/* ` \ { } | ~ have no Model 100 key: ignore */
}

static void poll_keyboard(void)
{
	for (int n = 0; n < 16; n++)
	{
		uint16_t ev = sb_read_keyboard();
		if (ev == 0)
			break;
		int state = ev >> 8, code = ev & 0xFF;
		int down = state == KEY_STATE_PRESSED, up = state == KEY_STATE_RELEASED;
		if (!down && !up)
			continue;

		switch (code)
		{
		case KEY_MOD_SHL: case KEY_MOD_SHR: phys_shift = down; apply_shift(); update_keys(); continue;
		case KEY_MOD_CTRL: phys_ctrl = down; mod_set(MT_CTRL, down); continue;
		case KEY_MOD_ALT:  mod_set(MT_GRAPH, down); continue;
		case KEY_CAPS_LOCK: continue;   /* the BIOS already applies Caps */
		}

		if (down)
		{
			key_event = 1;
			if (m100_off)
				continue;           /* the press only switches it on */
			if (slots[code].active)
				slot_up(&slots[code]);
			key_press(code);
		}
		else if (slots[code].active)
			slots[code].release = 1;
		else
		{
			/* release for a code we never saw pressed (e.g. Shift changed
			   the code mid-press): let go of every char key */
			for (int i = 0; i < 256; i++)
				if (slots[i].active && slots[i].is_char)
					slots[i].release = 1;
		}
	}
}

/* complete releases once each key has been down long enough for the
   ROM's keyboard scan to have seen it */
static void service_releases(void)
{
	for (int i = 0; i < 256; i++)
		if (slots[i].active && slots[i].release && cycles - slots[i].down >= MIN_HOLD)
			slot_up(&slots[i]);
}

/* synthetic tap of a special key, used once at startup */
static void tap_special(uint32_t bit)
{
	gSpecialKeys &= ~bit; update_keys(); run_cycles(CPU_HZ / 20);
	gSpecialKeys |= bit;  update_keys(); run_cycles(CPU_HZ / 2);
}

/* ------------------------------------------------------------ status UI */

/* Screen layout (8x10 font, 40 columns x 32 rows):
     row 0         top bar: model, SHIFT CTRL GRPH CAPS, saved/unsaved, battery
     y 24..151     the Model 100 screen (text or pixel view)
     rows 16..31   panel: startup log, key help, or info (Ctrl+F1 cycles) */

#define C_WHITE  0xFFFF
#define C_DIM    0x39E7
#define C_LIT    0xFFE0
#define C_OK     0x07E0
#define C_WARN   0xFD20
#define C_ERR    0xF81F
/* the area under the screen: rows 16-31 below a Model 100, rows 29-31
   below a Model 200 (help and info are then drawn over its screen) */
static int panel_row = 16, panel_rows = 16;
#define PANEL_ROW  panel_row
#define PANEL_ROWS panel_rows
#define LOG_MAX    16

static void put_at(int col, int row, uint16_t colour, const char *s)
{
	lcd_set_foreground(colour);
	lcd_putstr(col, row, s);
	lcd_set_foreground(C_WHITE);
}

static void status(int row, const char *s)
{
	char line[41];
	snprintf(line, sizeof line, "%-40s", s);
	put_at(0, row, C_WHITE, line);
}

enum { PANEL_LOG, PANEL_NONE, PANEL_HELP, PANEL_INFO, PANEL_LIB };
static int panel = PANEL_LOG;
static int help_or_info_showing(void) { return panel == PANEL_HELP || panel == PANEL_INFO; }

static char     log_text[LOG_MAX][41];
static uint16_t log_colour[LOG_MAX];
static int      log_n, log_errors;

/* draw the newest lines of the log that fit in the panel */
static void draw_log(void)
{
	int first = log_n > PANEL_ROWS ? log_n - PANEL_ROWS : 0;
	for (int i = first; i < log_n; i++)
		put_at(0, PANEL_ROW + i - first, log_colour[i], log_text[i]);
}

static void log_line(uint16_t colour, const char *s)
{
	if (log_n == LOG_MAX)
	{
		memmove(log_text[0], log_text[1], sizeof log_text[0] * (LOG_MAX - 1));
		memmove(&log_colour[0], &log_colour[1], sizeof log_colour[0] * (LOG_MAX - 1));
		log_n--;
	}
	snprintf(log_text[log_n], sizeof log_text[0], "%-40s", s);
	log_colour[log_n] = colour;
	if (colour == C_ERR || colour == C_WARN)
		log_errors++;
	log_n++;
	if (panel == PANEL_LOG)
		draw_log();
}

/* what the info panel reports */
static unsigned st_cap, st_load;
static int  st_time_src = -1, st_warm;
static char st_saved_at[16] = "not yet";

static const char *help_lines[] = {
	"PicoCalc          Model 100/200",
	"F1-F5             F1-F5",
	"Shift+F1..F3      F6-F8",
	"Shift+F4 / F5     LABEL / PRINT",
	"Alt (hold)        GRPH",
	"Shift+Esc         BREAK (or Ctrl+C)",
	"Shift+Tab / Del   PASTE / PAUSE",
	"Del               DEL",
	"Shift+Up/Down     Shift+Up/Down",
	"Ctrl+F1           help / info / off",
	"Ctrl+F2           save screen dump",
	"Ctrl+F3           export files to card",
	"Ctrl+F4           library / remove files",
	"Ctrl+F5           text / pixel view",
	"Ctrl+Esc          save and exit",
	"Full guide: VIRTUALT.TXT",
};

static void draw_info(void)
{
	char l[48], t[32];
	int r = PANEL_ROW;
	snprintf(l, sizeof l, "Build     " __DATE__ " " __TIME__);
	status(r++, l);
	extern uchar gRamBank;
	if (is200) snprintf(l, sizeof l, "Model     200, RAM bank #%d", gRamBank + 1);
	else snprintf(l, sizeof l, "Model     100");
	status(r++, l);
	snprintf(l, sizeof l, "Speed     %u.%02ux real time, load %u%%", st_cap / 100, st_cap % 100, st_load);
	status(r++, l);
	if (pixel_view) snprintf(l, sizeof l, "View      pixel");
	else snprintf(l, sizeof l, "View      text, %d cell%s drawn as pixels", raw_cells, raw_cells == 1 ? "" : "s");
	status(r++, l);
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	if (now > 1600000000) strftime(t, sizeof t, "%a %b %d %H:%M:%S", &lt);
	else strcpy(t, "not set");
	snprintf(l, sizeof l, "Clock     %s%s", t, st_time_src == VT_TIME_NTP ? " (NTP)" :
	         st_time_src == VT_TIME_KEPT ? " (kept)" : now > 1600000000 ? " (saved)" : "");
	status(r++, l);
	snprintf(l, sizeof l, "Memory    %s", st_warm ? "restored from card" : "cold start");
	status(r++, l);
	snprintf(l, sizeof l, "          last saved %s", st_saved_at);
	status(r++, l);
	snprintf(l, sizeof l, "Library   virtualt/%s (Ctrl+F4)", LIB_NAME);
	status(r++, l);
	uint8_t b = sb_read_battery();
	snprintf(l, sizeof l, "Battery   %d%%%s", b & 0x7F, b & 0x80 ? ", charging" : "");
	status(r++, l);
	snprintf(l, sizeof l, "WiFi      %s", vt_cfg.ssid[0] ? vt_cfg.ssid : "not configured");
	status(r++, l);
	while (r < PANEL_ROW + PANEL_ROWS && r < 32)
		status(r++, "");
}

static void draw_lib(void);
static void lib_close(void);

static void draw_panel(void)
{
	static int saved_row, saved_rows;
	int want_overlay = is200 && (overlay ? saved_rows : panel_rows) < 16 &&
	                   (panel == PANEL_HELP || panel == PANEL_INFO || panel == PANEL_LIB);
	if (want_overlay && !overlay)
	{
		/* Model 200: draw help/info over its screen, rows 3-18 */
		overlay = 1;
		saved_row = panel_row; saved_rows = panel_rows;
		lcd_solid_rectangle(BG, 0, Y_OFF, 320, BAND_H);
		panel_row = 3; panel_rows = 16;
	}
	else if (!want_overlay && overlay)
	{
		overlay = 0;
		panel_row = saved_row; panel_rows = saved_rows;
		lcd_solid_rectangle(BG, 0, Y_OFF, 320, BAND_H);
		vt_invalidate_cells();
		vt_t200_dirty = 0xFFFF;
	}
	lcd_solid_rectangle(BG, 0, PANEL_ROW * 10, 320, PANEL_ROWS * 10);
	switch (panel)
	{
	case PANEL_LOG:
		draw_log();
		break;
	case PANEL_HELP:
		for (int i = 0; i < PANEL_ROWS && i < (int)(sizeof help_lines / sizeof help_lines[0]); i++)
			put_at(0, PANEL_ROW + i, i == 0 ? C_LIT : C_WHITE, help_lines[i]);
		break;
	case PANEL_INFO:
		draw_info();
		break;
	case PANEL_LIB:
		draw_lib();
		break;
	}
}

static void cycle_panel(void)
{
	if (lib_open)
		lib_close();
	panel = panel == PANEL_HELP ? PANEL_INFO : panel == PANEL_INFO ? PANEL_NONE : PANEL_HELP;
	draw_panel();
}

/* ---- top bar ---- */

static int bar_shift = -1, bar_ctrl = -1, bar_grph = -1, bar_caps = -1, bar_dirty = -1, bar_batt = -1;
static int caps_on, batt_raw, ram_dirty;

static void draw_bar(void)
{
	int sh = !(gSpecialKeys & MT_SHIFT), ct = !(gSpecialKeys & MT_CTRL), gr = !(gSpecialKeys & MT_GRAPH);
	if (bar_shift < 0)
		put_at(0, 0, C_WHITE, is200 ? "M200" : "M100");
	if (sh != bar_shift) put_at(6, 0, sh ? C_LIT : C_DIM, "SHIFT");
	if (ct != bar_ctrl)  put_at(12, 0, ct ? C_LIT : C_DIM, "CTRL");
	if (gr != bar_grph)  put_at(17, 0, gr ? C_LIT : C_DIM, "GRPH");
	if (caps_on != bar_caps) put_at(22, 0, caps_on ? C_LIT : C_DIM, "CAPS");
	if (ram_dirty != bar_dirty) put_at(28, 0, ram_dirty ? C_WARN : C_OK, ram_dirty ? "unsaved" : "saved  ");
	if (batt_raw != bar_batt)
	{
		char b[8];
		int pct = batt_raw & 0x7F;
		snprintf(b, sizeof b, "%3d%%%c", pct, batt_raw & 0x80 ? '+' : ' ');
		put_at(35, 0, batt_raw & 0x80 ? C_OK : pct <= 15 ? C_ERR : C_WHITE, b);
	}
	bar_shift = sh; bar_ctrl = ct; bar_grph = gr; bar_caps = caps_on; bar_dirty = ram_dirty; bar_batt = batt_raw;
}

/* ------------------------------------------------------- the library */

/* Programs live on the card in virtualt/m100 or virtualt/m200 and are
   loaded into RAM on request from the library panel (Ctrl+F4). Saved RAM
   keeps them across sessions, so nothing is loaded at startup. */

extern uchar gRamBank;
static uint8_t babuf[32768 + 2];

static int prog_type(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || name[0] == '.') return 0;
	/* .100, .200 and .BAS are how archives such as Club 100 name BASIC
	   listings; they load as NAME.BA */
	return !strcasecmp(dot, ".ba") || !strcasecmp(dot, ".bas") || !strcasecmp(dot, ".100") ||
	       !strcasecmp(dot, ".200") ? 0x80 :
	       !strcasecmp(dot, ".co") ? 0xA0 :
	       /* Club 100's .DOC files are plain text that programs often
	          open as NAME.DO (STARF reads STARFI.DO) */
	       !strcasecmp(dot, ".do") || !strcasecmp(dot, ".doc") ? 0xC0 : 0;
}

/* "solit.ba" -> "SOLIT BA", the 8-char form the RAM directory uses */
static void mt_name(const char *name, int type, char mt[9])
{
	const char *dot = strrchr(name, '.');
	memcpy(mt, "        ", 8);
	mt[8] = 0;
	for (int i = 0; i < 6 && name + i < dot; i++)
		mt[i] = toupper((unsigned char)name[i]);
	mt[6] = type == 0xA0 ? 'C' : type == 0xC0 ? 'D' : 'B';
	mt[7] = type == 0x80 ? 'A' : 'O';
}

/* load one library file into RAM; returns the load result, with a
   word or two for the log in *what */
static int load_prog(const char *name, uint32_t size, const char **what)
{
	int type = prog_type(name);
	char mt[9], path[300];
	mt_name(name, type, mt);
	*what = "loaded";
	if (vt_find_file(mt)) { *what = "already in RAM"; return 0; }
	if (size == 0 || size > 32768) { *what = "wrong size"; return VT_BASIC_BAD; }
	snprintf(path, sizeof path, "%s/%s", LIB_DIR, name);
	fat32_file_t f;
	size_t n = 0;
	if (fat32_open(&f, path) != FAT32_OK) { *what = "can't open"; return VT_BASIC_BAD; }
	fat32_read(&f, babuf, size, &n);
	fat32_close(&f);
	if (n != size) { *what = "read error"; return VT_BASIC_BAD; }
	int r = type == 0x80 ? load_ba(babuf, (int)n, mt) : load_codo(babuf, (int)n, mt, type);
	if (r < 0)
		*what = r == -1 ? "not enough memory" : r == -2 ? "directory full" :
		        r == VT_BASIC_TOOBIG ? "too big" :
		        type == 0xA0 ? "not a .CO program" : type == 0xC0 ? "not a text file" : "not a BASIC program";
	return r;
}

/* Earlier builds kept Model 100 programs loose in virtualt/: move them
   into virtualt/m100 once (copy, check, then delete the original) */
static void migrate_loose(void)
{
	fat32_file_t dir;
	fat32_entry_t e;
	int moved = 0, failed = 0;
	static char names[64][41];
	static uint32_t sizes[64];
	int n = 0;

	if (fat32_open(&dir, BA_DIR) != FAT32_OK)
		return;
	while (n < 64 && fat32_dir_read(&dir, &e) == FAT32_OK && e.filename[0])
		if (!(e.attr & FAT32_ATTR_DIRECTORY) && prog_type(e.filename) && strlen(e.filename) <= 40)
		{
			strcpy(names[n], e.filename);
			sizes[n++] = e.size;
		}
	fat32_close(&dir);
	if (!n)
		return;
	vt_host_mkdir(BA_DIR "/m100");
	for (int i = 0; i < n; i++)
	{
		char from[64], to[64];
		snprintf(from, sizeof from, BA_DIR "/%s", names[i]);
		snprintf(to, sizeof to, BA_DIR "/m100/%s", names[i]);
		fat32_file_t f;
		size_t got = 0, put = 0;
		int ok = sizes[i] <= 32768 && fat32_open(&f, from) == FAT32_OK;
		if (ok) { fat32_read(&f, babuf, sizes[i], &got); fat32_close(&f); ok = got == sizes[i]; }
		if (ok)
		{
			fat32_file_t check;
			if (fat32_open(&check, to) == FAT32_OK) { fat32_close(&check); ok = 0; }   /* never overwrite */
		}
		if (ok && fat32_create(&f, to) == FAT32_OK)
		{
			if (got) fat32_write(&f, babuf, got, &put);
			fat32_close(&f);
			ok = put == got;
		}
		else
			ok = 0;
		if (ok && fat32_delete(from) == FAT32_OK)
			moved++;
		else
			failed++;
	}
	char l[48];
	snprintf(l, sizeof l, "Moved %d program%s into virtualt/m100", moved, moved == 1 ? "" : "s");
	log_line(C_WARN, l);
	if (failed)
	{
		snprintf(l, sizeof l, "  %d left in virtualt/ (couldn't move)", failed);
		log_line(C_ERR, l);
	}
}

/* ---- library panel ---- */

#define LIB_MAX 128
static char     lib_names[LIB_MAX][41];
static uint32_t lib_sizes[LIB_MAX];
static int      lib_n, lib_sel, lib_top, lib_loaded, lib_skipped;
static char     lib_msg[41];
static uint16_t lib_msg_colour;

static void lib_scan(void)
{
	fat32_file_t dir;
	fat32_entry_t e;
	lib_n = lib_skipped = 0;
	vt_host_mkdir(LIB_DIR);
	if (fat32_open(&dir, LIB_DIR) != FAT32_OK)
		return;
	while (fat32_dir_read(&dir, &e) == FAT32_OK && e.filename[0])
	{
		if ((e.attr & FAT32_ATTR_DIRECTORY) || !prog_type(e.filename))
			continue;
		if (lib_n == LIB_MAX || strlen(e.filename) > 40) { lib_skipped++; continue; }
		strcpy(lib_names[lib_n], e.filename);
		lib_sizes[lib_n++] = e.size;
	}
	fat32_close(&dir);
	/* alphabetical, ignoring case */
	for (int i = 1; i < lib_n; i++)
		for (int j = i; j > 0 && strcasecmp(lib_names[j - 1], lib_names[j]) > 0; j--)
		{
			char t[41]; uint32_t ts = lib_sizes[j];
			strcpy(t, lib_names[j]); strcpy(lib_names[j], lib_names[j - 1]); strcpy(lib_names[j - 1], t);
			lib_sizes[j] = lib_sizes[j - 1]; lib_sizes[j - 1] = ts;
		}
}

/* the panel's second view: the files in RAM, which Del can remove */
static int  lib_ram, lib_confirm;
static char ram_names[64][10];
static int  ram_sizes[64], ram_n;
static unsigned short ram_dirs[64];

static void ram_scan(void) { ram_n = vt_ram_files(ram_names, ram_sizes, ram_dirs, 64); }

static int lib_count(void) { return lib_ram ? ram_n : lib_n; }

static void draw_lib(void)
{
	char l[48];
	int list_rows = PANEL_ROWS - 2, n = lib_count();
	char left[24], right[20];
	const char *what = lib_ram ? "In RAM" : "Library";
	if (is200)
		snprintf(left, sizeof left, "%s %s bank %d", what, lib_ram ? "" : LIB_NAME, gRamBank + 1);
	else
		snprintf(left, sizeof left, "%s %s", what, lib_ram ? "" : LIB_NAME);
	snprintf(right, sizeof right, "%5d bytes free", vt_free_bytes());
	snprintf(l, sizeof l, "%-24s%16s", left, right);
	status(PANEL_ROW, l);
	put_at(0, PANEL_ROW, C_LIT, what);
	if (lib_sel >= n) lib_sel = n ? n - 1 : 0;
	if (lib_sel < lib_top) lib_top = lib_sel;
	if (lib_sel >= lib_top + list_rows) lib_top = lib_sel - list_rows + 1;
	for (int i = 0; i < list_rows; i++)
	{
		int k = lib_top + i, row = PANEL_ROW + 1 + i;
		if (k >= n)
		{
			status(row, i == 0 && !n ? (lib_ram ? "  (no files in RAM)" : "  (no .BA .CO .DO .100 .200 .DOC here)") : "");
			continue;
		}
		int in_ram = 0;
		if (lib_ram)
			snprintf(l, sizeof l, "%c %-24.24s%6d", k == lib_sel ? '>' : ' ', ram_names[k], ram_sizes[k]);
		else
		{
			char mt[9];
			mt_name(lib_names[k], prog_type(lib_names[k]), mt);
			in_ram = vt_find_file(mt);
			snprintf(l, sizeof l, "%c %-24.24s%6lu  %s", k == lib_sel ? '>' : ' ', lib_names[k],
			         (unsigned long)lib_sizes[k], in_ram ? "in RAM" : "      ");
		}
		status(row, l);
		if (k == lib_sel)
			put_at(0, row, C_LIT, l);
		else if (in_ram)
			put_at(34, row, C_OK, "in RAM");
	}
	int last = PANEL_ROW + PANEL_ROWS - 1;
	if (lib_msg[0])
	{
		status(last, lib_msg);
		put_at(0, last, lib_msg_colour, lib_msg);
	}
	else
		status(last, lib_ram ? "Del: remove from RAM   Tab: library  Esc"
		                     : "Enter: load   Tab: what's in RAM   Esc");
}

static void tap_char(int c)
{
	gKeyStates[c] = 1; update_keys(); run_cycles(CPU_HZ / 20);
	gKeyStates[c] = 0; update_keys(); run_cycles(CPU_HZ / 20);
}

/* is the ROM's main menu on the screen? (its top line ends "(C)Microsoft") */
static int at_menu(void)
{
	int top = is200 ? gBaseMemory[0xFEAE] % 16 : 0;
	unsigned short row = is200 ? 0xFA30 + top * 40 : 0xFE00;
	for (int c = 20; c <= 28; c++)          /* column 27 on a 100, 28 on a 200 */
		if (!memcmp(&gBaseMemory[row + c], "(C)Microsoft", 12))
			return 1;
	return 0;
}

static void lib_close(void)
{
	lib_open = 0;
	if (lib_loaded && at_menu())
	{
		/* redraw the menu so the new files show: BASIC, then back. Type
		   MENU rather than press F8: a program may have blanked F8's
		   definition (MAZE3D does KEY8,""), which would leave us in BASIC */
		for (const char *p = "basic"; *p; p++) tap_char(*p);
		tap_special(MT_ENTER);
		for (const char *p = "menu"; *p; p++) tap_char(*p);
		tap_special(MT_ENTER);
	}
	lib_loaded = 0;
}

static void lib_show(void)
{
	lib_ram = lib_confirm = 0;
	lib_scan();
	if (lib_sel >= lib_n) lib_sel = lib_n ? lib_n - 1 : 0;
	lib_msg[0] = 0;
	if (lib_skipped)
	{
		snprintf(lib_msg, sizeof lib_msg, "%d file%s not listed (name too long?)", lib_skipped, lib_skipped == 1 ? "" : "s");
		lib_msg_colour = C_WARN;
	}
	lib_open = 1;
	panel = PANEL_LIB;
	draw_panel();
}

static void lib_handle(int code)
{
	int list_rows = PANEL_ROWS - 2, n = lib_count();
	if (lib_confirm)
	{
		/* answer to "Remove NAME from RAM? Y/N" */
		lib_confirm = 0;
		if ((code == 'y' || code == 'Y') && lib_sel < ram_n)
		{
			char nm[10];
			strcpy(nm, ram_names[lib_sel]);
			int ok = vt_delete_file(ram_dirs[lib_sel]);
			snprintf(lib_msg, sizeof lib_msg, ok ? "%s removed from RAM" : "Couldn't remove %s", nm);
			lib_msg_colour = ok ? C_OK : C_ERR;
			if (ok) { lib_loaded = 1; ram_dirty = 1; }
			ram_scan();
		}
		else
		{
			snprintf(lib_msg, sizeof lib_msg, "Kept %s", lib_sel < ram_n ? ram_names[lib_sel] : "it");
			lib_msg_colour = C_WHITE;
		}
		draw_panel();
		return;
	}
	switch (code)
	{
	case KEY_UP:        if (lib_sel > 0) lib_sel--; break;
	case KEY_DOWN:      if (lib_sel < n - 1) lib_sel++; break;
	case KEY_PAGE_UP:   lib_sel = lib_sel > list_rows ? lib_sel - list_rows : 0; break;
	case KEY_PAGE_DOWN: lib_sel = lib_sel + list_rows < n ? lib_sel + list_rows : (n ? n - 1 : 0); break;
	case KEY_TAB:
		lib_ram = !lib_ram;
		lib_sel = lib_top = 0;
		if (lib_ram) ram_scan();
		break;
	case KEY_ESC:
		lib_close();
		panel = PANEL_NONE;
		draw_panel();
		return;
	case KEY_DEL: case KEY_BACKSPACE:
		if (!lib_ram || !ram_n)
			return;
		if (!at_menu())
		{
			/* moving memory under a running program would wreck it */
			snprintf(lib_msg, sizeof lib_msg, "Go to the main menu first");
			lib_msg_colour = C_WARN;
		}
		else
		{
			snprintf(lib_msg, sizeof lib_msg, "Remove %s from RAM? Y/N", ram_names[lib_sel]);
			lib_msg_colour = C_LIT;
			lib_confirm = 1;
		}
		draw_panel();
		return;
	case KEY_ENTER: case KEY_RETURN:
		if (!lib_ram && lib_n)
		{
			const char *what;
			int r = load_prog(lib_names[lib_sel], lib_sizes[lib_sel], &what);
			snprintf(lib_msg, sizeof lib_msg, "%.24s: %s", lib_names[lib_sel], what);
			lib_msg_colour = r > 0 ? C_OK : r == 0 ? C_WHITE : C_ERR;
			if (r > 0) { lib_loaded = 1; ram_dirty = 1; }
			draw_panel();
		}
		return;
	default:
		return;
	}
	lib_msg[0] = 0;
	draw_panel();
}

/* ------------------------------------------------------- RAM on the card */

extern uchar rambanks[];
extern uchar gRamBank;
void vt_sync_ram_banks(void);
void vt_load_ram_banks(int bank);

/* Hash of what matters for "has anything changed": the file area (RAM
   bottom up to the stack) and the RAM file directory -- plus, on a
   Model 200, the two banks not switched in. The stack and the ROM's
   counters change constantly even when the machine sits idle. */
static uint32_t ram_hash(void)
{
	uint32_t h = 2166136261u;
	int lo = is200 ? 0xA000 : 0x8000, hi = is200 ? 0xEC00 : 0xF380;
	for (int i = lo; i < hi; i++)
		h = (h ^ gBaseMemory[i]) * 16777619u;
	int d0 = gStdRomDesc->sDirectory, d1 = d0 + 11 * gStdRomDesc->sDirCount;
	for (int i = d0; i < d1 && i < 0x10000; i++)
		h = (h ^ gBaseMemory[i]) * 16777619u;
	if (is200)
		for (int b = 0; b < 3; b++)
			if (b != gRamBank)
				for (int i = 0; i < 24576; i++)
					h = (h ^ rambanks[b * 24576 + i]) * 16777619u;
	return h;
}

static uint32_t saved_hash;

/* Model 100: the 32 KB at 8000H. Model 200: all three 24 KB banks plus
   one byte saying which is switched in. */
static int pc_load_ram(void)
{
	fat32_file_t f;
	size_t n = 0;
	if (fat32_open(&f, RAM_PATH) != FAT32_OK)
		return 0;
	if (is200)
	{
		uint8_t bank = 0;
		size_t nb = 0;
		fat32_read(&f, rambanks, 3 * 24576, &n);
		fat32_read(&f, &bank, 1, &nb);
		fat32_close(&f);
		if (n != 3 * 24576)
		{
			memset(rambanks, 0, 3 * 24576);
			return 0;
		}
		vt_load_ram_banks(bank);
		return 1;
	}
	fat32_read(&f, &gBaseMemory[32768], 32768, &n);
	fat32_close(&f);
	if (n != 32768)
	{
		memset(&gBaseMemory[32768], 0, 32768);
		return 0;
	}
	return 1;
}

/* write to a new file first, then swap, so a power-off mid-save can't
   destroy the previous good image */
static void pc_save_ram(int force)
{
	uint32_t h = ram_hash();
	if (!force && h == saved_hash)
		return;
	fat32_file_t f;
	size_t w = 0, w2 = 0;
	int want;
	fat32_delete(RAM_NEW);
	if (fat32_create(&f, RAM_NEW) != FAT32_OK)
		return;
	if (is200)
	{
		uint8_t bank = gRamBank;
		vt_sync_ram_banks();
		fat32_write(&f, rambanks, 3 * 24576, &w);
		fat32_write(&f, &bank, 1, &w2);
		w += w2;
		want = 3 * 24576 + 1;
	}
	else
	{
		fat32_write(&f, &gBaseMemory[32768], 32768, &w);
		want = 32768;
	}
	fat32_close(&f);
	if ((int)w != want)
		return;
	fat32_delete(RAM_PATH);
	if (fat32_rename(RAM_NEW, RAM_PATH) == FAT32_OK)
	{
		saved_hash = h;
		time_t now = time(NULL);
		struct tm lt;
		localtime_r(&now, &lt);
		if (now > 1600000000) strftime(st_saved_at, sizeof st_saved_at, "%H:%M:%S", &lt);
		else strcpy(st_saved_at, "this session");
	}
}

/* 3-second countdown: Esc = cold start, Enter = go now, 1 / 2 = run a
   Model 100 / Model 200 this time (restarts the count). Returns 1 for a
   cold start. */
static int esc_pressed(int row)
{
	char line[48];
	for (int left = 3; left > 0; left--)
	{
		snprintf(line, sizeof line, "%s  %s  Esc:cold Enter:go (%d)",
		         is200 ? "Model 200" : "Model 100", is200 ? "1:M100" : "2:M200", left);
		put_at(0, row, C_LIT, line);
		absolute_time_t until = make_timeout_time_ms(1000);
		while (absolute_time_diff_us(get_absolute_time(), until) > 0)
		{
			uint16_t ev = sb_read_keyboard();
			if ((ev >> 8) == KEY_STATE_PRESSED)
			{
				int k = ev & 0xFF;
				if (k == KEY_ESC)
					return 1;
				if (k == KEY_ENTER || k == KEY_RETURN)
					return 0;
				if ((k == '1' && is200) || (k == '2' && !is200))
				{
					is200 = !is200;
					left = 4;       /* show the new choice for a full 3 s */
					break;
				}
			}
			sleep_ms(20);
		}
	}
	return 0;
}

/* ----------------------------------------------------------------- main */

/* ------------------------------------------------ export to the card */

static fat32_file_t exp_file;
int vt_host_create(const char *path, vt_file_t *f)
{
	fat32_delete(path);                     /* overwrite an earlier export */
	if (fat32_create(&exp_file, path) != FAT32_OK)
		return 0;
	*f = &exp_file;
	return 1;
}
int vt_host_write(vt_file_t f, const void *d, int n)
{
	size_t w = 0;
	fat32_write((fat32_file_t *)f, d, n, &w);
	return (int)w;
}
void vt_host_close(vt_file_t f) { fat32_close((fat32_file_t *)f); }
void vt_host_mkdir(const char *path)
{
	fat32_file_t d;
	if (fat32_dir_create(&d, path) == FAT32_OK)
		fat32_close(&d);
}

static int exp_fail;
static void exp_report(const char *name, int ok)
{
	watchdog_update();
	if (!ok)
	{
		char l[48];
		snprintf(l, sizeof l, "  %s: couldn't write", name);
		log_line(C_ERR, l);
		exp_fail++;
	}
}

/* Ctrl+F2: the screen's raw state -> /virtualt/screen.bin, for chasing
   display bugs: "VTSCR1", display start (2 bytes, low first), 8192 bytes
   of LCD RAM (Model 200) or zeros, then the 64 KB the 8085 sees */
static void dump_screen(void)
{
	extern uint8_t vt_t200_ram[8192];
	vt_file_t f;
	char l[48];
	if (!vt_host_create(BA_DIR "/screen.bin", &f))
	{
		log_line(C_ERR, "Couldn't create virtualt/screen.bin");
		return;
	}
	int ds = is200 ? vt_t200_dstart() : 0;
	uint8_t hdr[8] = { 'V', 'T', 'S', 'C', 'R', '1', (uint8_t)ds, (uint8_t)(ds >> 8) };
	int ok = vt_host_write(f, hdr, 8) == 8;
	static const uint8_t zero[256];
	for (int i = 0; ok && i < 8192; i += 256)
		ok = vt_host_write(f, is200 ? vt_t200_ram + i : zero, 256) == 256;
	for (int i = 0; ok && i < 65536; i += 4096)
		ok = vt_host_write(f, gBaseMemory + i, 4096) == 4096;
	vt_host_close(f);
	snprintf(l, sizeof l, ok ? "Screen saved to virtualt/screen.bin (%d raw)" : "Write to screen.bin failed", raw_cells);
	log_line(ok ? C_OK : C_ERR, l);
}

/* Ctrl+F3: every .BA/.DO/.CO in RAM -> /virtualt/export (Model 200:
   /virtualt/m200/export/bank1..3, each bank in turn) */
static void export_files(void)
{
	char l[48];
	int n = 0;
	exp_fail = 0;
	log_line(C_WHITE, "Exporting files to the card...");
	if (!is200)
	{
		vt_host_mkdir(BA_DIR "/m100");
		n = vt_export_files(BA_DIR "/m100/export", exp_report);
		snprintf(l, sizeof l, "Exported %d file%s to m100/export", n, n == 1 ? "" : "s");
	}
	else
	{
		extern uchar gRamBank;
		void set_ram_bank(unsigned char bank);
		int orig = gRamBank;
		vt_host_mkdir(BA_DIR "/m200");
		vt_host_mkdir(BA_DIR "/m200/export");
		for (int b = 0; b < 3; b++)
		{
			char dir[48];
			set_ram_bank(b);                /* switch the bank in to read it */
			snprintf(dir, sizeof dir, BA_DIR "/m200/export/bank%d", b + 1);
			n += vt_export_files(dir, exp_report);
		}
		set_ram_bank(orig);
		snprintf(l, sizeof l, "Exported %d files to m200/export", n);
	}
	log_line(exp_fail ? C_WARN : C_OK, l);
}

/* -------------------------------------------- watchdog breadcrumbs */

/* If the main loop ever stops for WD_MS, the hardware watchdog reboots
   the chip (which restarts VirtualT). Before each step of the loop we
   note in watchdog scratch registers what we were doing, so the next
   start can say where it stopped. They survive that reboot. UF2 Loader
   and the shell use 0-2 and the SDK's watchdog code uses 4 (and 5-7
   only when rebooting to an address), so: 3 = our marker, 5-7 = data. */
#define WD_MS      8000
#define WD_MAGIC   0x56545744u            /* 'VTWD' */
enum { PH_EMU = 1, PH_KEYS, PH_LCD, PH_PANEL, PH_TSAVE, PH_RSAVE, PH_STAT, PH_SLEEP, PH_RELEASE, PH_TEST };
static const char *phase_name[] = { "?", "emulating", "reading keyboard", "drawing screen",
	"drawing panel", "saving time", "saving memory", "status/battery", "idle", "key release",
	"a Ctrl+F2 test freeze" };
static uint64_t wd_t0;

static inline void crumb(int phase)
{
	watchdog_hw->scratch[5] = phase;
	watchdog_hw->scratch[6] = (uint32_t)((time_us_64() - wd_t0) / 1000000);
	watchdog_hw->scratch[7] = (uint32_t)PC;
	watchdog_update();
}

static void report_last_crash(void)
{
	if (!watchdog_caused_reboot() || watchdog_hw->scratch[3] != WD_MAGIC)
	{
		watchdog_hw->scratch[3] = 0;
		return;
	}
	unsigned ph = watchdog_hw->scratch[5], up = watchdog_hw->scratch[6], pc = watchdog_hw->scratch[7];
	watchdog_hw->scratch[3] = 0;
	char line[64];
	snprintf(line, sizeof line, "Last run froze while %s,", ph < sizeof phase_name / sizeof phase_name[0] ? phase_name[ph] : "?");
	log_line(C_ERR, line);
	snprintf(line, sizeof line, "  after %uh %02um, 8085 PC=%04X", up / 3600, (up / 60) % 60, pc & 0xFFFF);
	log_line(C_ERR, line);
	log_line(C_ERR, "  (details added to crashlog.txt)");

	/* append to /virtualt/crashlog.txt */
	static char buf[4096];
	fat32_file_t f;
	size_t n = 0, w;
	if (fat32_open(&f, BA_DIR "/crashlog.txt") == FAT32_OK)
	{
		fat32_read(&f, buf, sizeof buf - 200, &n);
		fat32_close(&f);
	}
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	char when[32] = "time unknown";
	if (now > 1600000000) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &lt);
	n += snprintf(buf + n, sizeof buf - n, "%s  build %s  froze while %s after %us, PC=%04X\r\n",
	              when, __TIME__, ph < sizeof phase_name / sizeof phase_name[0] ? phase_name[ph] : "?", up, pc & 0xFFFF);
	fat32_delete(BA_DIR "/crashlog.txt");
	if (fat32_create(&f, BA_DIR "/crashlog.txt") == FAT32_OK)
	{
		fat32_write(&f, buf, n, &w);
		fat32_close(&f);
	}
}

static void say_clock(const char *m)
{
	int bad = strstr(m, "couldn't") || strstr(m, "failed") || strstr(m, "no answer");
	log_line(bad ? C_WARN : C_WHITE, m);
}

static void fatal(const char *a, const char *b)
{
	log_line(C_ERR, a);
	log_line(C_ERR, b);
	log_line(C_WHITE, "Press any key to return to the menu.");
	while (!(sb_read_keyboard() & 0xFF00))
		sleep_ms(50);
	picocalc_shell_exit();
	watchdog_reboot(0, 0, 0);
	for (;;) tight_loop_contents();
}

int main(void)
{
	sb_init();
	lcd_init();
	fat32_init();
	lcd_enable_cursor(false);
	lcd_set_font(&font_8x10);
	lcd_clear_screen();

	put_at(0, 0, C_WHITE, "VirtualT  build " __TIME__);
	lcd_solid_rectangle(0x4208, 0, Y_OFF - 4, 320, 2);
	lcd_solid_rectangle(0x4208, 0, Y_OFF + BAND_H + 2, 320, 2);

	/* settings, then the real time (must be set before the M100 reads its clock) */
	vt_config_load();
	st_time_src = vt_time_init(say_clock);
	report_last_crash();

	/* sound: after WiFi is shut down again, since both use a PIO block */
	if (vt_cfg.sound)
	{
		audio_init();
		sound_enable = 1;
	}

	/* which machine, and warm or cold */
	is200 = vt_cfg.model == 200;
	int cd_row = PANEL_ROW + (log_n < PANEL_ROWS ? log_n : PANEL_ROWS - 1);
	int cold = esc_pressed(cd_row);
	status(cd_row, "");

	/* bring up the Model 100 / 200 */
	gModel = is200 ? MODEL_T200 : MODEL_M100;
	mem_setup.mem_mode = SETUP_MEM_BASE;
	init_mem();
	init_io();
	memset((void *)cpu, 0, sizeof cpu);
	IM = 0x08;
	load_sys_rom();
	extern int vt_rom_bytes;
	if (vt_rom_bytes != ROM_BYTES)
	{
		char m1[48], m2[48];
		snprintf(m1, sizeof m1, "Can't read %s", ROM_PATH);
		snprintf(m2, sizeof m2, "Copy your %s ROM image there.", MODEL_NAME);
		fatal(m1, is200 ? "(73728 bytes, with Multiplan)" : m2);
	}
	if (is200)
		memset(&gBaseMemory[0xA000], 0, 0x6000);
	else
		memset(&gBaseMemory[32768], 0, 32768);

	if (cold)
		log_line(C_WHITE, "Memory: cold start (Esc pressed)");
	else
	{
		st_warm = pc_load_ram();
		log_line(C_WHITE, st_warm ? "Memory: restored from card" : "Memory: cold start (nothing saved yet)");
	}
	reinit_mem();
	update_keys();

	/* programs from earlier builds move into the library folder */
	migrate_loose();
	vt_host_mkdir(LIB_DIR);

	extern int vt_clock_ignore_writes;
	vt_clock_ignore_writes = 1;         /* keep a cold boot from zeroing the clock */
	run_cycles(CPU_HZ * 2);
	vt_clock_ignore_writes = 0;
	if (!st_warm)
	{
		/* a fresh machine: put README.DO from the library on the menu, so
		   a new user has something to read first */
		fat32_file_t f;
		size_t n = 0;
		char rp[64];
		snprintf(rp, sizeof rp, "%s/README.DO", LIB_DIR);
		if (fat32_open(&f, rp) == FAT32_OK)
		{
			fat32_read(&f, babuf, 32768, &n);
			fat32_close(&f);
			if (n && !vt_find_file("README DO") && load_codo(babuf, (int)n, "README DO", 0xC0) > 0 && at_menu())
			{
				for (const char *q = "basic"; *q; q++) tap_char(*q);
				tap_special(MT_ENTER);
				for (const char *q = "menu"; *q; q++) tap_char(*q);
				tap_special(MT_ENTER);
				log_line(C_WHITE, "Memory: README.DO put on the menu");
			}
		}
	}
	saved_hash = st_warm ? ram_hash() : 0;

	{
		char msg[48];
		time_t now = time(NULL);
		struct tm lt;
		localtime_r(&now, &lt);
		if (now > 1600000000)
		{
			strftime(msg, sizeof msg, "Clock: %a %b %d %H:%M", &lt);
			strcat(msg, st_time_src == VT_TIME_NTP ? " (NTP)" : st_time_src == VT_TIME_KEPT ? " (kept)" : " (saved)");
			log_line(C_WHITE, msg);
		}
		else
			log_line(C_WARN, "Clock: not set -- use TIME$ and DATE$");
	}
	if (log_errors)
		log_line(C_LIT, "Ctrl+F1: help, info, hide this");

	uint64_t start = time_us_64(), emu_us = 0;
	uint64_t win_busy = 0, win_emu = 0, win_total0 = start;
	uint64_t next_poll = start, next_flush = start, next_stat = start + 1000000;
	uint64_t next_tsave = start + 600000000ull;     /* every 10 minutes */
	uint64_t next_rsave = start + 120000000ull;     /* RAM: every 2 min, if changed */
	uint64_t log_clear = log_errors ? 0 : start + 3000000;   /* clean start: hide log after 3 s */
	uint64_t clock_guard_until = 0;

	status(0, "");                      /* build stamp -> top bar */
	if (is200)
	{
		/* the Model 200 screen covers rows 2-28: clear the startup log
		   from there and keep a 3-line panel underneath */
		lcd_solid_rectangle(BG, 0, Y_OFF - 4, 320, 320 - (Y_OFF - 4));
		lcd_solid_rectangle(0x4208, 0, Y_OFF - 4, 320, 2);
		lcd_solid_rectangle(0x4208, 0, Y_OFF + BAND_H + 2, 320, 2);
		panel_row = 29;
		panel_rows = 3;
		vt_invalidate_cells();
		vt_t200_dirty = 0xFFFF;
		draw_panel();
	}
	draw_bar();

	wd_t0 = time_us_64();
	watchdog_hw->scratch[3] = WD_MAGIC;
	watchdog_enable(WD_MS, true);

	while (!want_exit)
	{
		if (m100_off)
		{
			/* Model 100 is "off": save, blank its screen, wait for a key */
			tone_on = spk_on = 0;
			play(0);
			crumb(PH_RSAVE);
			pc_save_ram(0);
			lcd_solid_rectangle(BG, 0, Y_OFF, 320, BAND_H);
			{
				char m[40];
				snprintf(m, sizeof m, "%s is off.", MODEL_NAME);
				put_at(6, is200 ? 12 : 8, C_DIM, m);
				put_at(6, is200 ? 14 : 10, C_DIM, "Press any key to switch it on.");
			}
			key_event = 0;
			while (!key_event && !want_exit)
			{
				crumb(PH_SLEEP);
				poll_keyboard();
				draw_bar();
				sleep_ms(50);
			}
			if (want_exit)
				break;
			/* power on: the CPU restarts at 0000 and the ROM finds its
			   RAM intact, so it resumes where it was (a warm start) */
			for (int i = 0; i < 256; i++)
				if (slots[i].active) slot_up(&slots[i]);
			memset((void *)cpu, 0, sizeof cpu);
			IM = 0x08;
			gSpecialKeys = 0xFFFFFFFF;
			init_io();
			reinit_mem();
			update_keys();
			vt_clock_ignore_writes = 1;     /* as at startup: keep the time */
			clock_guard_until = time_us_64() + 3000000;
			lcd_solid_rectangle(BG, 0, Y_OFF, 320, BAND_H);
			vt_invalidate_cells();
			dirty = 0x3FF;
			vt_t200_dirty = 0xFFFF;
			m100_off = 0;
			start = time_us_64();
			emu_us = 0;
			continue;
		}

		if (clock_guard_until && time_us_64() > clock_guard_until)
		{
			vt_clock_ignore_writes = 0;
			clock_guard_until = 0;
		}

		uint64_t t0 = time_us_64();
		crumb(PH_EMU);
		run_cycles(CHUNK);
		crumb(PH_RELEASE);
		service_releases();
		sound_service();
		uint64_t t1 = time_us_64();
		win_busy += t1 - t0;
		win_emu += 10000;
		emu_us += 10000;

		if (t1 >= next_poll) { crumb(PH_KEYS); poll_keyboard(); draw_bar(); next_poll = t1 + 15000; }
		if (t1 >= next_flush) { crumb(PH_LCD); flush_lcd(); next_flush = t1 + 40000; }
		if (want_panel_cycle) { crumb(PH_PANEL); want_panel_cycle = 0; cycle_panel(); }
		if (want_panel_close) { crumb(PH_PANEL); want_panel_close = 0; panel = PANEL_NONE; draw_panel(); }
		if (want_export)
		{
			crumb(PH_RSAVE);
			want_export = 0;
			if (panel != PANEL_LOG)
			{
				panel = PANEL_LOG;
				draw_panel();
			}
			export_files();
			log_clear = time_us_64() + 5000000;     /* hide again after 5 s */
			start = time_us_64();                   /* don't race to catch up */
			emu_us = 0;
		}
		if (want_lib)
		{
			crumb(PH_PANEL);
			want_lib = 0;
			if (lib_open)
			{
				lib_close();
				panel = PANEL_NONE;
				draw_panel();
			}
			else
				lib_show();
			log_clear = 0;
			start = time_us_64();
			emu_us = 0;
		}
		if (lib_key)
		{
			crumb(PH_PANEL);
			int k = lib_key;
			lib_key = 0;
			lib_handle(k);
			start = time_us_64();
			emu_us = 0;
		}
		if (want_dump)
		{
			want_dump = 0;
			if (panel != PANEL_LOG) { panel = PANEL_LOG; draw_panel(); }
			dump_screen();
			log_clear = time_us_64() + 5000000;
			start = time_us_64();
			emu_us = 0;
		}
		if (want_test_freeze)
		{
			crumb(PH_TEST);
			log_line(C_ERR, "Test freeze: watchdog restart in 8 s...");
			for (;;) tight_loop_contents();
		}

		if (log_clear && t1 >= log_clear && panel == PANEL_LOG)
		{
			panel = PANEL_NONE;
			draw_panel();
			log_clear = 0;
		}

		if (t1 >= next_tsave) { crumb(PH_TSAVE); vt_time_save(); next_tsave = t1 + 600000000ull; }
		if (t1 >= next_rsave) { crumb(PH_RSAVE); pc_save_ram(0); next_rsave = t1 + 120000000ull; }

		if (t1 >= next_stat)
		{
			crumb(PH_STAT);
			uint64_t wall = t1 - win_total0;
			st_cap = (unsigned)(win_emu * 100 / (win_busy ? win_busy : 1));
			st_load = (unsigned)(win_busy * 100 / (wall ? wall : 1));
			win_busy = win_emu = 0;
			win_total0 = t1;
			next_stat = t1 + 1000000;

			batt_raw = sb_read_battery();
			caps_on = (sb_read_keyboard_state() & (1 << 5)) != 0;
			ram_dirty = ram_hash() != saved_hash;
			draw_bar();
			if (panel == PANEL_INFO)
				draw_info();
		}

		/* throttle to real time; if we fall far behind, don't try to catch up */
		crumb(PH_SLEEP);
		int64_t ahead = (int64_t)(start + emu_us) - (int64_t)time_us_64();
		if (ahead > 0)
			sleep_us(ahead);
		else if (ahead < -200000)
			start += -ahead;
	}

	watchdog_disable();
	watchdog_hw->scratch[3] = 0;
	play(0);
	panel = PANEL_LOG;
	log_n = 0;
	draw_panel();
	log_line(C_WHITE, "Saving memory to card...");
	pc_save_ram(1);
	vt_time_save();
	lcd_clear_screen();
	picocalc_shell_exit();
	watchdog_reboot(0, 0, 0);
	for (;;) tight_loop_contents();
}
