#pragma once
#include "VirtualT.h"
#include "cpu.h"
#include "io.h"
extern volatile UINT64 cycles;
extern volatile UINT64 instructs;
extern int gDelayUpdateKeys;
extern unsigned char gKeyStates[128];
extern unsigned long gSpecialKeys;
extern int gRamBottom;
void update_keys(void);
void run_cycles(UINT64 n);
int load_ba(unsigned char *data, int len, const char *mtname);
int load_codo(unsigned char *data, int len, const char *mtname, int type);  /* 0xA0 .CO, 0xC0 .DO */
int vt_free_bytes(void);
int vt_ram_files(char names[][10], int *sizes, unsigned short *dirs, int max);
int vt_delete_file(unsigned short dir_addr);
int vt_find_file(const char *mtname);
