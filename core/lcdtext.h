#pragma once
#include <stdint.h>

extern uint8_t vt_lcdram[10][256];
extern uint8_t vt_top_row[10];
extern volatile uint16_t vt_lcd_dirty;

uint8_t vt_lcd_column(int x, int row);

/* receives one 8x16 RGB565 cell for text column 'col' (0-39), row 'row' (0-7) */
typedef void (*vt_cell_blit_fn)(int col, int row, const uint16_t *pix);

/* redraw cells that changed since the last call; returns the number of
   cells that hold pixel graphics rather than a ROM character */
int  vt_render_cells(vt_cell_blit_fn blit, uint16_t fg, uint16_t bg);
void vt_invalidate_cells(void);

/* Model 200 LCD */
extern uint8_t vt_t200_ram[8192];
extern volatile int vt_t200_dirty;
int     vt_t200_dstart(void);
int     vt_t200_graphics(void);
uint8_t vt_t200_byte(int col, int y);          /* 6 pixels, bit 0 leftmost */
