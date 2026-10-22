#pragma once
/* Model 100/200 BASIC keywords and the plain-text tokenizer (vt_basic.c) */

extern const char *const vt_basic_kw[128];   /* tokens 80H-FEH, 0-terminated */

#define VT_BASIC_BAD    (-10)   /* not a BASIC listing (a line with no number...) */
#define VT_BASIC_TOOBIG (-11)

/* 1 if d looks like a plain-text listing rather than a tokenized image */
int vt_basic_is_ascii(const unsigned char *d, int len);

/* tokenize len bytes of listing into out, linked for a program at addr;
   returns the length including the 0000 end, or VT_BASIC_BAD/TOOBIG */
int vt_basic_tokenize(const unsigned char *in, int len, unsigned char *out, int outmax,
                      unsigned short addr);
