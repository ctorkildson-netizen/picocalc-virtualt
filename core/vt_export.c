/* vt_export.c -- write the Model 100/200's RAM files out as files on the
 * host (the SD card on the PicoCalc), the way VirtualT's "Save to Host"
 * does (file.cpp save_file):
 *
 *   NAME.BA   tokenized BASIC, exactly as VirtualT saves it (loads back
 *             into VirtualT, and into this emulator from /virtualt)
 *   NAME.BAS  the same program as a plain-text listing
 *   NAME.DO   text files, up to (not including) the ^Z end marker
 *   NAME.CO   machine code, with its 6-byte header (start, length, exec)
 *
 * The host provides the file and folder calls (vt_export.h).
 */
#include <stdio.h>
#include <string.h>
#include "VirtualT.h"
#include "roms.h"
#include "vt_export.h"

extern RomDescription_t *gStdRomDesc;
unsigned char  get_memory8(unsigned short a);
unsigned short get_memory16(unsigned short a);

#define TYPE_BA 0x80
#define TYPE_DO 0xC0
#define TYPE_CO 0xA0

#include "vt_basic.h"
#define kw vt_basic_kw
#define NKW 127

/* small write buffer so the host isn't called a byte at a time */
static unsigned char obuf[512];
static int olen, ofail;
static vt_file_t ofile;

static void out_flush(void)
{
	if (olen && !ofail && vt_host_write(ofile, obuf, olen) != olen)
		ofail = 1;
	olen = 0;
}

static void out(const void *p, int n)
{
	const unsigned char *b = p;
	while (n--)
	{
		obuf[olen++] = *b++;
		if (olen == sizeof obuf)
			out_flush();
	}
}

static void outs(const char *s) { out(s, (int)strlen(s)); }

static int open_out(const char *dir, const char *name)
{
	char path[160];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	olen = 0;
	ofail = 0;
	return vt_host_create(path, &ofile);
}

static int close_out(void)
{
	out_flush();
	vt_host_close(ofile);
	return !ofail;
}

static void save_ba(unsigned short a1)
{
	unsigned short a2;
	while ((a2 = get_memory16(a1)) != 0)
	{
		for (unsigned short a = a1; a < a2; a++)
		{
			unsigned char ch = get_memory8(a);
			out(&ch, 1);
		}
		if (a2 <= a1)           /* broken link: stop rather than loop */
			break;
		a1 = a2;
	}
}

/* Bytes of 80H and up are keywords only outside strings, REM and DATA;
   inside them they are the Model 100's graphics characters, kept as is. */
static void save_bas(unsigned short a1)
{
	unsigned short a2;
	char num[8];
	while ((a2 = get_memory16(a1)) != 0 && a2 > a1)
	{
		int quote = 0, rem = 0, data = 0;
		a1 += 2;
		snprintf(num, sizeof num, "%u ", get_memory16(a1));
		outs(num);
		a1 += 2;
		while (a1 < a2)
		{
			unsigned char ch = get_memory8(a1++);
			if (ch == 0)
			{
				outs("\r\n");
				continue;
			}
			if (rem || quote || (data && ch != ':'))
			{
				if (ch == '"' && !rem)
					quote = !quote;
				out(&ch, 1);
				continue;
			}
			data = 0;
			if (ch == '"')
			{
				quote = 1;
				out(&ch, 1);
			}
			else if (ch == ':')
			{
				unsigned char nx = get_memory8(a1);
				if (nx == 0x8E && get_memory8(a1 + 1) == 0xFF)
				{
					outs("'");      /* ' is stored as :REM' */
					a1 += 2;
					rem = 1;
					continue;
				}
				if (nx == 0x91)     /* ELSE is stored as :ELSE */
					continue;
				outs(":");
			}
			else if (ch >= 0x80 && ch - 0x80 < NKW)
			{
				outs(kw[ch - 0x80]);
				if (ch == 0x8E) rem = 1;
				if (ch == 0x83) data = 1;
			}
			else
				out(&ch, 1);
		}
	}
}

int vt_export_files(const char *dir, vt_export_report_fn report)
{
	int count = 0;
	unsigned short d = gStdRomDesc->sDirectory;

	vt_host_mkdir(dir);
	for (int i = 0; i < gStdRomDesc->sDirCount; i++, d += 11)
	{
		unsigned char type = get_memory8(d);
		if (type != TYPE_BA && type != TYPE_DO && type != TYPE_CO)
			continue;

		/* "ROG   BA" -> "ROG" + "BA" */
		char base[8], name[16];
		int n = 0;
		for (int x = 0; x < 6; x++)
		{
			unsigned char c = get_memory8(d + 3 + x);
			if (c != ' ' && c > 32 && c < 127 && c != '/' && c != '\\' && c != ':')
				base[n++] = c;
		}
		base[n] = 0;
		if (!n)
			continue;
		char ext[3] = { (char)get_memory8(d + 9), (char)get_memory8(d + 10), 0 };
		unsigned short addr = get_memory16(d + 1);
		int ok = 1;

		snprintf(name, sizeof name, "%s.%s", base, ext);
		if (!open_out(dir, name))
		{
			report(name, 0);
			continue;
		}
		if (type == TYPE_BA)
			save_ba(addr);
		else if (type == TYPE_DO)
		{
			unsigned char ch;
			for (unsigned a = addr; a < 0x10000 && (ch = get_memory8(a)) != 0x1A; a++)
				out(&ch, 1);
		}
		else
		{
			unsigned len = get_memory16(addr + 2) + 6u;
			for (unsigned a = 0; a < len && addr + a < 0x10000; a++)
			{
				unsigned char ch = get_memory8(addr + a);
				out(&ch, 1);
			}
		}
		ok = close_out();
		report(name, ok);
		count += ok;

		if (type == TYPE_BA)
		{
			snprintf(name, sizeof name, "%s.BAS", base);
			if (open_out(dir, name))
			{
				save_bas(addr);
				report(name, close_out());
			}
		}
	}
	return count;
}
