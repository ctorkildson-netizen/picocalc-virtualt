/* mem_vt.c -- a slim replacement for VirtualT's memory.c, for the
 * Model 100 and the Model 200 only.
 *
 * VirtualT's memory.c supports every model plus ReMem, Rampac, REX and
 * QUAD, and reserves ~650 KB of tables for them -- more than the
 * RP2350's 520 KB of SRAM. The two machines we emulate need only:
 *
 *   Model 100:  32 KB ROM at 0000-7FFF (or the option ROM, bank 1),
 *               32 KB RAM at 8000-FFFF.
 *   Model 200:  40 KB ROM at 0000-9FFF -- bank 0 the main ROM, bank 1
 *               the 32 KB Multiplan ROM, bank 2 an option ROM --
 *               and three 24 KB RAM banks, one at a time at A000-FFFF.
 *
 * This file provides exactly the symbols io.c and the CPU core use, with
 * the same behaviour as memory.c's non-ReMem paths. The ROM image comes
 * from the platform: platform_load_rom().
 */
#include <string.h>
#include "VirtualT.h"
#include "roms.h"

typedef unsigned char uchar;

#define M200_ROM   40960
#define M200_RAM   24576
#define MSPLAN     32768

uchar	gBaseMemory[65536];			/* flat CPU address space */
uchar	gSysROM[M200_ROM + MSPLAN];		/* system ROM (+ Multiplan, Model 200) */
uchar	gOptROM[32768];				/* option ROM image (empty unless loaded) */
uchar	rambanks[3 * M200_RAM];			/* Model 200 RAM banks */
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
extern RomDescription_t gM100_Desc, gM200_Desc;
extern int gModel;

/* supplied by the platform layer: fill buf with up to len bytes of ROM;
   returns the number of bytes read */
int platform_load_rom(uchar *buf, int len);

#define ROMSIZE   gRomSize
#define RAMBOTTOM gRamBottom
#define IS_200    (gModel == MODEL_T200)

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
	gRamBottom = IS_200 ? 0xA000 : 0x8000;
	gRomSize = IS_200 ? M200_ROM : 32768;
	memset(gOptROM, 0, sizeof gOptROM);
	memset(rambanks, 0, sizeof rambanks);
}

void reinit_mem(void)
{
	gRomSize = IS_200 ? M200_ROM : 32768;
	memcpy(gBaseMemory, gSysROM, ROMSIZE);
	gRomBank = 0;
}

/* returns the number of ROM bytes read (32768, or 73728 for a Model 200
   ROM file that includes Multiplan) */
int vt_rom_bytes;

void load_sys_rom(void)
{
	gStdRomDesc = IS_200 ? &gM200_Desc : &gM100_Desc;
	gRomSize = IS_200 ? M200_ROM : 32768;
	vt_rom_bytes = platform_load_rom(gSysROM, IS_200 ? M200_ROM + MSPLAN : 32768);
	memcpy(gBaseMemory, gSysROM, ROMSIZE);
}

void set_rom_bank(unsigned char bank)
{
	if (IS_200)
	{
		/* bank 0 main ROM, 1 Multiplan, 2 option ROM */
		gRomSize = M200_ROM;
		if (gOptRomRW && gRomBank == 2)
			memcpy(gOptROM, gBaseMemory, sizeof gOptROM);
		gRomBank = bank;
		switch (bank)
		{
		case 0: memcpy(gBaseMemory, gSysROM, M200_ROM); break;
		case 1: memcpy(gBaseMemory, gSysROM + M200_ROM, MSPLAN); break;
		case 2:
			memcpy(gBaseMemory, gOptROM, sizeof gOptROM);
			gRomSize = gOptRomRW ? 0 : M200_ROM;
			break;
		}
		return;
	}

	/* Model 100: bank 1 maps the option ROM over 0000-7FFF */
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

/* Model 200 RAM banks: swap the 24 KB at A000 in and out */
void set_ram_bank(unsigned char bank)
{
	if (!IS_200 || bank > 2)
		return;
	memcpy(&rambanks[gRamBank * M200_RAM], &gBaseMemory[0xA000], M200_RAM);
	gRamBank = bank;
	memcpy(&gBaseMemory[0xA000], &rambanks[gRamBank * M200_RAM], M200_RAM);
}

/* For saving: bring the bank that's switched in up to date in rambanks */
void vt_sync_ram_banks(void)
{
	if (IS_200)
		memcpy(&rambanks[gRamBank * M200_RAM], &gBaseMemory[0xA000], M200_RAM);
}

/* For loading: put the current bank back in place from rambanks */
void vt_load_ram_banks(int bank)
{
	if (!IS_200)
		return;
	gRamBank = bank > 2 ? 0 : bank;
	memcpy(&gBaseMemory[0xA000], &rambanks[gRamBank * M200_RAM], M200_RAM);
}

int  remem_in(unsigned char port, unsigned char *data) { (void)port; (void)data; return 0; }
int  remem_out(unsigned char port, unsigned char data) { (void)port; (void)data; return 0; }
