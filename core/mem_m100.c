/* mem_m100.c -- a Model 100-only replacement for VirtualT's memory.c.
 *
 * VirtualT's memory.c supports every model plus ReMem, Rampac, REX and
 * QUAD, and reserves ~650 KB of tables for them -- more than the
 * RP2350's 520 KB of SRAM.  For a plain 32K Model 100 the whole memory
 * system is: 32 KB of ROM (or the option ROM when bank 1 is selected)
 * copied into the bottom half of a flat 64 KB space, and 32 KB of RAM
 * in the top half.  This file provides exactly the symbols io.c and the
 * CPU core use, with the same semantics as memory.c's non-ReMem paths.
 *
 * The ROM image itself comes from the platform: platform_load_rom().
 */
#include <string.h>
#include "VirtualT.h"
#include "roms.h"

typedef unsigned char uchar;

uchar	gBaseMemory[65536];		/* flat CPU address space */
uchar	gSysROM[32768];			/* system ROM image */
uchar	gOptROM[32768];			/* option ROM image (empty unless loaded) */
int	gRamBottom = 0x8000;
int	gRomSize = 32768;
int	gRomBank = 0;
int	gRom0Bank = 0;
int	gOptRomRW = 0;
uchar	gQuad = 0;
uchar	gReMem = 0;
int	gRex = 0;
uchar	gRamBank = 0;

extern RomDescription_t *gStdRomDesc;
extern RomDescription_t gM100_Desc;
extern int gModel;

/* supplied by the platform layer: fill buf with up to len bytes of ROM */
int platform_load_rom(uchar *buf, int len);

#define ROMSIZE gRomSize
#define RAMBOTTOM gRamBottom

unsigned char get_memory8(unsigned short a)
{
	return (a > ROMSIZE && a < RAMBOTTOM) ? 0xFF : gBaseMemory[a];
}

unsigned short get_memory16(unsigned short a)
{
	return (a > ROMSIZE && a < RAMBOTTOM) ? 0xFFFF :
		gBaseMemory[a] + (gBaseMemory[(unsigned short)(a + 1)] << 8);
}

void set_memory8(unsigned short a, unsigned char d) { gBaseMemory[a] = d; }

void set_memory16(unsigned short a, unsigned short d)
{
	set_memory8(a, d & 0xFF);
	set_memory8((unsigned short)(a + 1), d >> 8);
}

void init_mem(void)
{
	gRamBank = 0;
	gRomBank = 0;
	memset(gOptROM, 0, sizeof gOptROM);
}

void reinit_mem(void)
{
	gRomSize = 32768;
	memcpy(gBaseMemory, gSysROM, ROMSIZE);
	gRomBank = 0;
}

void load_sys_rom(void)
{
	gStdRomDesc = &gM100_Desc;
	gRomSize = 32768;
	platform_load_rom(gSysROM, 32768);
	memcpy(gBaseMemory, gSysROM, ROMSIZE);
}

/* Model 100 bank switching: bank 1 maps the option ROM over 0000-7FFF */
void set_rom_bank(unsigned char bank)
{
	gRomSize = 32768;
	if (gOptRomRW && gRomBank == 1)
		memcpy(gOptROM, gBaseMemory, ROMSIZE);
	gRomBank = bank;
	if (bank & 0x01)
	{
		memcpy(gBaseMemory, gOptROM, ROMSIZE);
		gRomSize = gOptRomRW ? 0 : 32768;
	}
	else
		memcpy(gBaseMemory, gSysROM, ROMSIZE);
}

void set_rom0_bank(unsigned char bank)
{
	gRom0Bank = bank;
	if (gRomBank == 0)
		set_rom_bank(gRomBank);
}

void set_ram_bank(unsigned char bank) { (void)bank; }	/* no QUAD */
int  remem_in(unsigned char port, unsigned char *data) { (void)port; (void)data; return 0; }
int  remem_out(unsigned char port, unsigned char data) { (void)port; (void)data; return 0; }
