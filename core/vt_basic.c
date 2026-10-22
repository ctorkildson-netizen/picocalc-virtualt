/* vt_basic.c -- the Model 100/200 BASIC keyword table, and a tokenizer
 * that turns a plain-text (ASCII) BASIC listing into the tokenized form
 * BASIC keeps in RAM.
 *
 * Most .BA files in the Club 100 and other archives are plain-text
 * listings, not tokenized images. VirtualT has a tokenizer for them
 * (file.cpp tokenize()), but it is case-sensitive, never inserts ':'
 * before an ELSE that already follows one, and can't take lines out of
 * order. This one follows what the ROM itself does when you type a line,
 * checked against the Model 100 ROM in the test harness:
 *
 *   - letters outside strings, REM and DATA are upper-cased
 *   - keywords are found anywhere, even with no spaces ("FORT=1TO3")
 *   - '?' is PRINT, ' is stored as :REM followed by FFH
 *   - ELSE is always stored with a ':' in front (":ELSE" becomes "::ELSE")
 *   - after REM the rest of the line is copied as is; after DATA up to
 *     the next ':' outside quotes
 *   - numbers are kept as text, as the Model 100 does
 *   - the space after the line number is dropped, other spaces are kept
 *
 * Lines may come in any order; a repeated line number replaces the
 * earlier line, as typing it again would.
 */
#include <string.h>
#include "vt_basic.h"

/* tokens 80H-FFH, in ROM order: the tokenizer takes the first match */
const char *const vt_basic_kw[128] = {
	"END",    "FOR",   "NEXT",  "DATA",    "INPUT", "DIM",    "READ",   "LET",
	"GOTO",   "RUN",   "IF",    "RESTORE", "GOSUB", "RETURN", "REM",    "STOP",
	"WIDTH",  "ELSE",  "LINE",  "EDIT",    "ERROR", "RESUME", "OUT",    "ON",
	"DSKO$",  "OPEN",  "CLOSE", "LOAD",    "MERGE", "FILES",  "SAVE",   "LFILES",
	"LPRINT", "DEF",   "POKE",  "PRINT",   "CONT",  "LIST",   "LLIST",  "CLEAR",
	"CLOAD",  "CSAVE", "TIME$", "DATE$",   "DAY$",  "COM",    "MDM",    "KEY",
	"CLS",    "BEEP",  "SOUND", "LCOPY",   "PSET",  "PRESET", "MOTOR",  "MAX",
	"POWER",  "CALL",  "MENU",  "IPL",     "NAME",  "KILL",   "SCREEN", "NEW",
	"TAB(",   "TO",    "USING", "VARPTR",  "ERL",   "ERR",    "STRING$","INSTR",
	"DSKI$",  "INKEY$","CSRLIN","OFF",     "HIMEM", "THEN",   "NOT",    "STEP",
	"+",      "-",     "*",     "/",       "^",     "AND",    "OR",     "XOR",
	"EQV",    "IMP",   "MOD",   "\\",      ">",     "=",      "<",
	"SGN",    "INT",   "ABS",   "FRE",     "INP",   "LPOS",   "POS",    "SQR",
	"RND",    "LOG",   "EXP",   "COS",     "SIN",   "TAN",    "ATN",    "PEEK",
	"EOF",    "LOC",   "LOF",   "CINT",    "CSNG",  "CDBL",   "FIX",    "LEN",
	"STR$",   "VAL",   "ASC",   "CHR$",    "SPACE$","LEFT$",  "RIGHT$", "MID$",
	0
};

#define TOK_DATA 0x83
#define TOK_REM  0x8E
#define TOK_ELSE 0x91
#define TOK_PRINT 0xA3

static int upc(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

int vt_basic_is_ascii(const unsigned char *d, int len)
{
	/* a tokenized image always holds 00 bytes (line ends, link high
	   bytes); a listing never does before its ^Z.  A listing may start
	   with a few lines of prose (many archive downloads do), so look for
	   any line that starts with a line number. */
	int numbered = 0, bol = 1;
	for (int i = 0; i < len && d[i] != 0x1A; i++)
	{
		if (d[i] == 0)
			return 0;
		if (d[i] == '\r' || d[i] == '\n') { bol = 1; continue; }
		if (bol && d[i] == ' ') continue;
		if (bol && d[i] >= '0' && d[i] <= '9') numbered = 1;
		bol = 0;
	}
	return numbered;
}

/* crunch one line's text (after the line number) into t; returns length */
static int crunch(const unsigned char *s, int n, unsigned char *t, int tmax)
{
	int o = 0, i = 0;
	int quote = 0, data = 0;
#define PUT(b) do { if (o >= tmax) return -1; t[o++] = (unsigned char)(b); } while (0)

	while (i < n)
	{
		unsigned char c = s[i];
		if (c == '\r') { i++; continue; }
		if (quote)
		{
			PUT(c); i++;
			if (c == '"') quote = 0;
			continue;
		}
		if (c == '"') { PUT(c); i++; quote = 1; continue; }
		if (data)
		{
			if (c == ':') data = 0;
			PUT(c); i++;
			continue;
		}
		if (c >= 0x80) { i++; continue; }        /* would read as a token */
		if (c == '?') { PUT(TOK_PRINT); i++; continue; }
		if (c == '\'')
		{
			PUT(':'); PUT(TOK_REM); PUT(0xFF);
			for (i++; i < n; i++)
				if (s[i] != '\r') PUT(s[i]);
			break;
		}
		if (c >= '0' && c <= ';') { PUT(c); i++; continue; }

		int tok = -1, tl = 0;
		for (int k = 0; vt_basic_kw[k]; k++)
		{
			const char *w = vt_basic_kw[k];
			int l = (int)strlen(w), j = 0;
			if (i + l > n) continue;
			while (j < l && upc(s[i + j]) == (unsigned char)w[j]) j++;
			if (j == l) { tok = k; tl = l; break; }
		}
		if (tok < 0) { PUT(upc(c)); i++; continue; }

		tok += 0x80;
		if (tok == TOK_ELSE) PUT(':');
		PUT(tok);
		i += tl;
		if (tok == TOK_REM)
		{
			for (; i < n; i++)
				if (s[i] != '\r') PUT(s[i]);
			break;
		}
		if (tok == TOK_DATA) data = 1;
	}
	return o;
#undef PUT
}

int vt_basic_tokenize(const unsigned char *in, int len, unsigned char *out, int outmax,
                      unsigned short addr)
{
	unsigned char t[256];
	int olen = 0;                   /* bytes of lines in out, without the 0000 end */
	int pos = 0, seen = 0;          /* seen: a numbered line has been read */

	while (pos < len && in[pos] != 0x1A)
	{
		int e = pos;               /* a line ends at LF, CR+LF or a lone CR (old Mac files) */
		while (e < len && in[e] != '\n' && in[e] != '\r' && in[e] != 0x1A) e++;
		int s = pos;
		pos = e;
		if (pos < len && in[pos] == '\r') pos++;
		if (pos < len && in[pos] == '\n') pos++;

		while (s < e && (in[s] == ' ' || in[s] == '\r' || in[s] == '\t')) s++;
		if (s == e) continue;                    /* blank line */
		if (in[s] < '0' || in[s] > '9')
		{
			if (!seen) continue;                 /* prose before the program */
			return VT_BASIC_BAD;                 /* a line with no number */
		}
		seen = 1;
		long num = 0;
		while (s < e && in[s] >= '0' && in[s] <= '9')
		{
			num = num * 10 + in[s++] - '0';
			if (num > 65529) return VT_BASIC_BAD;
		}
		if (s < e && in[s] == ' ') s++;
		int tl = crunch(in + s, e - s, t, sizeof t);
		if (tl < 0) return VT_BASIC_BAD;         /* longer than BASIC allows */

		/* find where this line goes: out holds [link][num][text]00 ... */
		int at = 0, old = 0;
		while (at < olen)
		{
			int ln = out[at + 2] | out[at + 3] << 8;
			int l = 4;
			while (out[at + l]) l++;
			l++;
			if (ln == num) { old = l; break; }
			if (ln > num) break;
			at += l;
		}
		int need = tl ? tl + 5 : 0;               /* "10" alone deletes line 10 */
		if (olen - old + need + 2 > outmax)
			return VT_BASIC_TOOBIG;
		memmove(out + at + need, out + at + old, olen - at - old);
		olen += need - old;
		if (need)
		{
			out[at + 2] = num & 0xFF;
			out[at + 3] = num >> 8;
			memcpy(out + at + 4, t, tl);
			out[at + 4 + tl] = 0;
		}
	}

	/* fill in the line links for a program starting at addr */
	int at = 0;
	while (at < olen)
	{
		int l = 4;
		while (out[at + l]) l++;
		l++;
		out[at] = (addr + at + l) & 0xFF;
		out[at + 1] = (addr + at + l) >> 8;
		at += l;
	}
	out[olen] = out[olen + 1] = 0;
	return olen + 2;
}
