/* lcdtext.c -- Model 100 LCD capture plus a sharp "text grid" renderer.
 *
 * The Model 100 LCD is 240x64 pixels: ten HD44102-style controllers, each
 * 50 columns x 32 rows, arranged 5 across and 2 down. The ROM draws text
 * as 6x8 cells (40 columns x 8 rows) from two character tables in ROM:
 *   0x7711: codes 20H-7FH, 5 bytes each (6th column blank)
 *   0x78F1: codes 80H-FFH, 6 bytes each
 * and keeps the characters it printed in a 40x8 buffer at 0xFE00.
 *
 * vt_render_cells() compares every 6x8 cell of the LCD with the ROM glyph
 * of the character the buffer says is there:
 *   - matches (or matches inverted, e.g. the cursor or reverse video):
 *     draw it crisply -- ASCII from the PicoCalc's 8x10 font, the 80H-FFH
 *     graphics characters from the ROM table at 2x height;
 *   - doesn't match (PSET/LINE graphics, machine-code drawing, a cell
 *     caught mid-update): draw the cell's actual LCD pixels, scaled.
 * So text is sharp, and pixel graphics still appear exactly where drawn,
 * with no global mode switch. Output cells are 8x16, i.e. 320x128 total.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifdef PICO_BUILD
#include "drivers/font.h"
#else
#include "font.h"
#endif
#include "lcdtext.h"
#include "VirtualT.h"

extern int gModel;

extern unsigned char gSysROM[];
extern unsigned char gBaseMemory[];

uint8_t vt_lcdram[10][256];
uint8_t vt_top_row[10];
volatile uint16_t vt_lcd_dirty;

void drawbyte(int driver, int col, int value)
{
	vt_lcdram[driver][col] = value;
	vt_lcd_dirty |= 1u << driver;
}

void lcdcommand(int driver, int value)
{
	if ((value & 0x3F) == 0x3E)
	{
		vt_top_row[driver] = value >> 6;
		vt_lcd_dirty |= 1u << driver;
	}
}

/* 8 vertical pixels (bit 0 = top) at pixel column x of text row 'row' */
uint8_t vt_lcd_column(int x, int row)
{
	int d = (row >= 4 ? 5 : 0) + x / 50;
	int bank = ((row & 3) + vt_top_row[d]) & 3;
	return vt_lcdram[d][(bank << 6) | (x % 50)];
}


/* ---------------------------------------------------------------------
   Model 200 LCD: one HD61830 graphics controller with 8 KB of display
   RAM. The visible 240x128 screen is 40 bytes per pixel row (6 pixels
   per byte, bit 0 leftmost) x 128 rows, read as a ring starting at the
   display start address. Ported from VirtualT's T200_Disp. */

uint8_t vt_t200_ram[8192];
volatile int vt_t200_dirty;              /* bit per text row (16), changed since last draw */
static uint8_t t2_mcr, t2_dsl, t2_dsh, t2_cal, t2_cah, t2_rd_dummy;
static int t2_rd_addr, t2_last_dstart;

enum { SET_MCR, SET_HPITCH, SET_HCNT, SET_TDIV, SET_CURSPOS,
       SET_DSTARTL = 8, SET_DSTARTH, SET_CURSADDRL, SET_CURSADDRH,
       RAM_WR, RAM_RD, RAM_BIT_CLR, RAM_BIT_SET };

int vt_t200_dstart(void) { return ((t2_dsh << 8) | t2_dsl) & 8191; }
int vt_t200_graphics(void) { return (t2_mcr & 0x02) != 0; }

static void t2_mark(int addr)
{
	int off = (addr - (((t2_dsh << 8) | t2_dsl) & 8191)) & 8191;
	if (off < 40 * 128)
		vt_t200_dirty |= 1 << (off / 320);
}

static void t2_next(void)
{
	if (++t2_cal == 0)
		t2_cah++;
	if (t2_cah >= 0x20)
		t2_cah = 0;
}

void t200_command(unsigned char ir, unsigned char data)
{
	int cursaddr = ((t2_cah << 8) | t2_cal) & 8191;
	int dstart, c;

	switch (ir)
	{
	case SET_MCR:     t2_mcr = data; vt_t200_dirty = 0xFFFF; break;
	case SET_DSTARTL: t2_dsl = data; break;
	case SET_DSTARTH:
		t2_dsh = data;
		dstart = ((t2_dsh << 8) | t2_dsl);
		/* A one-text-row step (0x140) is the ROM scrolling: clear the
		   off-screen RAM "above" the display, as VirtualT does. Any other
		   step leaves the RAM alone, like the real HD61830 -- VirtualT
		   cleared all of it, which wiped programs that scroll by pixel
		   rows (SCROLL.200). */
		c = abs(dstart - t2_last_dstart);
		if (c != 0x140) c = dstart - t2_last_dstart + 0x2000;
		if (c != 0x140) c = t2_last_dstart - dstart + 0x2000;
		t2_last_dstart = dstart;
		if (c == 0x140)
		{
			dstart -= 640;
			if (dstart < 0) dstart += 8192;
			for (c = 0; c < 640; c++)
			{
				vt_t200_ram[dstart++] = 0;
				if (dstart >= 8192) dstart = 0;
			}
		}
		vt_t200_dirty = 0xFFFF;
		break;
	case SET_CURSADDRL:
		if ((t2_cal & 0x80) && !(data & 0x80))
		{
			t2_cah++;
			if (t2_cah >= 0x20) t2_cah = 0;
		}
		t2_cal = data;
		break;
	case SET_CURSADDRH: t2_cah = data; break;
	case RAM_WR:
		vt_t200_ram[cursaddr] = data;
		t2_mark(cursaddr);
		t2_next();
		break;
	case RAM_RD:
		t2_rd_addr = cursaddr;
		break;
	case RAM_BIT_CLR:
		vt_t200_ram[cursaddr] &= (uint8_t)~(1u << (data & 7));
		t2_mark(cursaddr);
		t2_next();
		break;
	case RAM_BIT_SET:
		vt_t200_ram[cursaddr] |= (uint8_t)(1u << (data & 7));
		t2_mark(cursaddr);
		t2_next();
		break;
	default:
		break;          /* pitch, count, divider, cursor: not needed */
	}
}

unsigned char t200_readport(unsigned char port)
{
	(void)port;
	unsigned char r = t2_rd_dummy;          /* reads lag by one, as on the chip */
	t2_rd_dummy = vt_t200_ram[t2_rd_addr++];
	if (t2_rd_addr >= 8192)
		t2_rd_addr = 0;
	return r;
}

/* one 6-pixel horizontal byte of the visible Model 200 screen */
uint8_t vt_t200_byte(int col, int y)
{
	return vt_t200_ram[(vt_t200_dstart() + y * 40 + col) & 8191];
}

#define IS_200 (gModel == MODEL_T200)

/* column i (0-5) of the ROM's glyph for ch, bit 0 = top row */
static uint8_t rom_glyph_col(int ch, int i)
{
	if (ch < 0x20)
		return 0;
	if (IS_200)
	{
		/* Model 200: 9063H, 8 row bytes per character, bit 0 leftmost */
		const uint8_t *g = &gSysROM[0x9063 + (ch - 0x20) * 8];
		uint8_t col = 0;
		for (int y = 0; y < 8; y++)
			col |= ((g[y] >> i) & 1) << y;
		return col;
	}
	if (ch < 0x80)
		return i < 5 ? gSysROM[0x7711 + (ch - 0x20) * 5 + i] : 0;
	return gSysROM[0x78F1 + (ch - 0x80) * 6 + i];
}

/* the LCD's actual pixels in text cell (c, r), as 6 columns */
static void lcd_cell_cols(int c, int r, uint8_t col[6])
{
	if (!IS_200)
	{
		for (int i = 0; i < 6; i++)
			col[i] = vt_lcd_column(c * 6 + i, r);
		return;
	}
	for (int i = 0; i < 6; i++)
		col[i] = 0;
	for (int y = 0; y < 8; y++)
	{
		uint8_t b = vt_t200_byte(c, r * 8 + y);
		for (int i = 0; i < 6; i++)
			col[i] |= ((b >> i) & 1) << y;
	}
}

/* what is drawn in each cell, so only changed cells are re-sent */
typedef struct { uint8_t kind, ch, col[6]; } cellstate_t;
enum { CELL_NONE, CELL_TEXT, CELL_TEXT_INV, CELL_RAW };
static cellstate_t shown[16][40];

static void render_raw(uint16_t *pix, const uint8_t *col, uint16_t fg, uint16_t bg)
{
	/* 6 -> 8 columns (0 and 3 doubled, same pattern in every cell), 2x rows */
	static const uint8_t xmap[8] = { 0, 0, 1, 2, 3, 3, 4, 5 };
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 8; x++)
			pix[y * 8 + x] = (col[xmap[x]] >> (y >> 1)) & 1 ? fg : bg;
}

static void render_text(uint16_t *pix, int ch, int inv, uint16_t fg, uint16_t bg)
{
	if (inv) { uint16_t t = fg; fg = bg; bg = t; }
	if (ch >= 0x80 || ch == 0x7F)
	{
		uint8_t col[6];
		for (int i = 0; i < 6; i++)
			col[i] = rom_glyph_col(ch, i);
		render_raw(pix, col, fg, bg);
		return;
	}
	for (int i = 0; i < 128; i++)
		pix[i] = bg;
	if (ch <= 0x20)
		return;
	const uint8_t *g = &font_8x10.glyphs[ch * GLYPH_HEIGHT];
	for (int y = 0; y < GLYPH_HEIGHT; y++)
		for (int x = 0; x < 8; x++)
			if (g[y] & (0x80 >> x))
				pix[(y + 3) * 8 + x] = fg;
}

void vt_invalidate_cells(void)
{
	memset(shown, 0, sizeof shown);
}

int vt_render_cells(vt_cell_blit_fn blit, uint16_t fg, uint16_t bg)
{
	static uint16_t pix[8 * 16];
	int raw = 0;

	int rows = IS_200 ? 16 : 8;
	unsigned short tbuf = IS_200 ? 0xFA30 : 0xFE00;     /* ROM's text copy */
	/* The Model 200 scrolls by moving the LCD start one text row and
	   leaves its text copy where it is: the copy is a ring of 16 rows,
	   and FEAEH holds the row that is at the top of the screen. */
	int top = IS_200 ? gBaseMemory[0xFEAE] % 16 : 0;
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < 40; c++)
		{
			cellstate_t s;
			uint8_t lc[6];
			int ch = gBaseMemory[tbuf + ((r + top) % rows) * 40 + c];
			int same = 1, inv = 1;
			memset(&s, 0, sizeof s);
			lcd_cell_cols(c, r, lc);
			for (int i = 0; i < 6; i++)
			{
				uint8_t l = lc[i];
				uint8_t g = rom_glyph_col(ch, i);
				s.col[i] = l;
				if (l != g) same = 0;
				if (l != (uint8_t)~g) inv = 0;
			}
			if (same || inv)
			{
				s.kind = same ? CELL_TEXT : CELL_TEXT_INV;
				s.ch = ch;
				memset(s.col, 0, sizeof s.col);
			}
			else
			{
				s.kind = CELL_RAW;
				raw++;
			}

			if (memcmp(&s, &shown[r][c], sizeof s) == 0)
				continue;
			shown[r][c] = s;
			if (s.kind == CELL_RAW)
				render_raw(pix, s.col, fg, bg);
			else
				render_text(pix, ch, s.kind == CELL_TEXT_INV, fg, bg);
			blit(c, r, pix);
		}
	return raw;
}
