/* clock_vt.c -- VirtualT's clock chip emulation (uPD1990AC for the Model
 * 100/102, RP5C01 for the Model 200), lifted from clock.cpp without the
 * FLTK preferences code. It reads the host's time() / localtime(), so on
 * the PicoCalc the time must be set (settimeofday, from NTP or the saved
 * last-run time) before the Model 100 first reads its clock. */
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "VirtualT.h"
#include "gen_defs.h"
#include "roms.h"
#include "io.h"
#include "setup.h"
#include "m100emu.h"
#include "memory.h"

extern int gModel;
int vt_clock_ignore_writes;
uchar			gClockMode = 255;
uchar			gRp5c01_mode;
uchar			gRp5c01_data[4][13];
time_t			clock_time = 0;
time_t			last_clock_time = 1;
time_t			gStartTime = 0;
time_t			gRefTime = 0;
uchar			clock_sr[5];			/* 40 Bit shift register */
uchar			clock_sr_save[5];		/* 40 Bit shift register backup storage */
uchar			clock_sr_index = 0;
int				gClockTimingMode;
int				gTimeElapse;
int				gReload;
extern RomDescription_t	*gStdRomDesc;

struct	tm		*mytime	;
uchar			clock_serial_out = 0;

void init_clock(void)
{
	/* Do clock initialization stuff */
}

/*
===================================================================
Handle commands written to the uPD1990AC clock chip for models
other than T200.
===================================================================
*/
void pd1990ac_chip_cmd(uchar val)
{
	time_t		time_delta;
	struct	tm	when;
	int			x;

	gClockMode = val & 0x07;

	/* Clock chip command strobe */
	switch (gClockMode)
	{
	case 0:		/* NOP */
		break;
	case 1:		/* Serial Shift */
		clock_serial_out = clock_sr[0] & 0x01;
//		clock_sr_index = 0;
		break;
	case 2:		/* Write Clock chip */
		/* The ROM's cold boot sets the clock to Jan 1 00:00:00; the front
		   end sets vt_clock_ignore_writes during its startup cold boot so
		   the real time (NTP / saved) survives. TIME$= etc. work after. */
		if (vt_clock_ignore_writes)
			break;
		// Reset the start time
		gStartTime = time(&gStartTime);
		mytime = localtime(&gStartTime);

		/* Update Clock Chip Shift register Seconds */
		when.tm_sec = (clock_sr[0]& 0x0F) + (clock_sr[0] >> 4) * 10;

		/* Minutes */
		when.tm_min = (clock_sr[1] & 0x0F) + (clock_sr[1] >> 4) * 10;

		/* Hours */
		when.tm_hour = (clock_sr[2] & 0x0F) + (clock_sr[2] >> 4) * 10;

		/* Day of month */
		when.tm_mday = (clock_sr[3] & 0x0F) + (clock_sr[3] >> 4) * 10;

		/* Day of week */
		when.tm_wday = clock_sr[4] & 0x0F;

		/* Month */
		when.tm_mon = (clock_sr[4] >> 4) - 1;

		when.tm_isdst = -1;
		when.tm_year = mytime->tm_year;
		/* PicoCalc: the host clock may never have been set (no WiFi);
		   take the year from the Model 100's own RAM copy instead */
		if (when.tm_year < 120 && gStdRomDesc != NULL)
			when.tm_year = 100 + get_memory8(gStdRomDesc->sYear + 1) * 10
			                   + get_memory8(gStdRomDesc->sYear);
		
		gRefTime = mktime(&when);
#ifdef PICO_BUILD
		/* make TIME$ / DATE$ the PicoCalc's time too, so it is saved
		   in lasttime.txt and survives a restart without WiFi */
		{
			struct timeval tv = { .tv_sec = gRefTime, .tv_usec = 0 };
			settimeofday(&tv, NULL);
			gStartTime = gRefTime;
			last_clock_time = 0;
		}
#endif

		for (x = 0; x < 5; x++)
			clock_sr_save[x] = clock_sr[x];

		break;
	case 3:		/* Read clock chip */
		for (x = 0; x < 5; x++)
			clock_sr[x] = clock_sr_save[x];

		// Get current time and test if it changed from the last check
		clock_time = time(&clock_time);
		if (clock_time == last_clock_time)
			break;

		// Calculate the time delta from start of program
		if (gStartTime == 0)
		{
			gStartTime = clock_time;
			gRefTime = clock_time;
		}
		time_delta = clock_time - gStartTime;
		last_clock_time = clock_time;
		clock_time = gRefTime + time_delta;
		mytime = localtime(&clock_time);
		
		/* Update Clock Chip Shift register Seconds */
		clock_sr[0] = mytime->tm_sec % 10;
		clock_sr[0] |= (mytime->tm_sec / 10) << 4;

		/* Minutes */
		clock_sr[1] = mytime->tm_min % 10;
		clock_sr[1] |= (mytime->tm_min / 10) << 4;

		/* Hours */
		clock_sr[2] = mytime->tm_hour % 10;
		clock_sr[2] |= (mytime->tm_hour / 10) << 4;

		/* Day of month */
		clock_sr[3] = mytime->tm_mday % 10;
		clock_sr[3] |= (mytime->tm_mday / 10) << 4;

		/* Day of week */
		clock_sr[4] = (mytime->tm_wday) % 10;

		/* Month */
		clock_sr[4] |= (mytime->tm_mon + 1) << 4;

		for (x = 0; x < 5; x++)
			clock_sr_save[x] = clock_sr[x];
//		clock_sr_index = 0;

		if (gStdRomDesc != NULL)
		{
			if (gModel == MODEL_PC8201)
			{
				if ((get_memory8(gStdRomDesc->sYear+1) == '8') && (get_memory8(gStdRomDesc->sYear) == '3'))
				{
					set_memory8(gStdRomDesc->sYear, (unsigned char) (mytime->tm_year % 10) + '0');
					set_memory8(gStdRomDesc->sYear+1, (unsigned char) ((mytime->tm_year % 100) / 10) + '0');
				}
			}
			else
			{
				if ((get_memory8(gStdRomDesc->sYear+1) == 0) && (get_memory8(gStdRomDesc->sYear) == 0))
				{
					set_memory8(gStdRomDesc->sYear, (unsigned char) (mytime->tm_year % 10));
					set_memory8(gStdRomDesc->sYear+1, (unsigned char) ((mytime->tm_year % 100) / 10));
				}
			}
		}
	}
}

/*
===================================================================
Handle writes to the T200 clock chip, RP5C01.
===================================================================
*/
void rp5c01_write(uchar port, uchar val)
{
	struct tm	when;

	/* Test if writing to Mode Register */
	if (port == 0x9D)	
	{
		if (gModel == MODEL_T200)
			gRp5c01_mode = val;
	}
	else
	{
		/* PicoCalc: the cold boot zeroes the time registers; while the
		   front end says so, keep the real time instead */
		if (vt_clock_ignore_writes && (gRp5c01_mode & 3) == 0)
		{
			last_clock_time = 0;    /* refresh from the host on the next read */
			return;
		}

		/* Save write data to array */
		gRp5c01_data[gRp5c01_mode & 0x03][port & 0x0F] = val;

		/* Check if the time is being set */
		if ((gRp5c01_mode & 3) == 0)
		{
			// Check if the last byte is being written and update
			// the gStartTime if it is using the new time data
			// (PicoCalc: not during the cold-boot reset, as for the 100)
			if (port == 0x9C && !vt_clock_ignore_writes)
			{
				/* Update Clock Chip Shift register Seconds */
				when.tm_sec = gRp5c01_data[0][0]+ gRp5c01_data[0][1] * 10;;

				/* Minutes */
				when.tm_min = gRp5c01_data[0][2] + gRp5c01_data[0][3] * 10;

				/* Hours */
				/* Test if we are in 12 or 24 hour mode */
				if (gRp5c01_data[1][10] == 1)
				{
					when.tm_hour = gRp5c01_data[0][4] + gRp5c01_data[0][5] * 10;
				}
				else
				{
					when.tm_hour = gRp5c01_data[0][4] + (gRp5c01_data[0][5] & 0x01) * 10 +
						(gRp5c01_data[0][5] & 0x02) ? 12 : 0;
				}

				/* Day of week */
				when.tm_wday = gRp5c01_data[0][6];

				/* Day of month */
				when.tm_mday = gRp5c01_data[0][7] + gRp5c01_data[0][8] * 10;

				/* Month */
				when.tm_mon = (gRp5c01_data[0][9] + gRp5c01_data[0][10] * 10)-1;

				/* Year */
				when.tm_year = gRp5c01_data[0][11] + gRp5c01_data[0][12] * 10 + 100;

				when.tm_isdst = -1;
				when.tm_yday = 1;

				gRefTime = mktime(&when);
			}
			/* Add code here to update the system time */
		}
		/* Check if the alarm is being set */
		else if ((gRp5c01_mode& 3) == 1)
		{
		}
	}
}

/*
===================================================================
Handle reads from the T200 clock chip, RP5C01.
===================================================================
*/
uchar rp5c01_read(uchar port)
{
	time_t		time_delta;
	uchar		ret = 0;

	if (gModel == MODEL_T200)
	{
		/* Test for clock Mode Register */
		if (port == 0x9D)
			return gRp5c01_mode;

		/* Check if the time is being set */
		if ((gRp5c01_mode & 3) == 0)
		{
			/* Read system clock and update "chip" */
			if (port == 0x90)
			{
				clock_time = time(&clock_time);
				if (clock_time != last_clock_time)
				{
					// Check if we just started up
					if (gStartTime == 0)
					{
						gStartTime = clock_time;
						gRefTime = clock_time;
					}

					// Calculated emulated time
					last_clock_time = clock_time;
					time_delta = clock_time - gStartTime;
					clock_time = gRefTime + time_delta;
					mytime = localtime(&clock_time);
					
					/* Update Clock Chip Shift register Seconds */
					gRp5c01_data[0][0] = mytime->tm_sec % 10;
					gRp5c01_data[0][1] = mytime->tm_sec / 10;

					/* Minutes */
					gRp5c01_data[0][2] = mytime->tm_min % 10;
					gRp5c01_data[0][3] = mytime->tm_min / 10;

					/* Hours */
					/* Test if we are in 12 or 24 hour mode */
					if (gRp5c01_data[1][10] == 1)
					{
						gRp5c01_data[0][4] = mytime->tm_hour % 10;
						gRp5c01_data[0][5] = mytime->tm_hour / 10;
					}
					else
					{
						gRp5c01_data[0][4] = (mytime->tm_hour % 12) % 10;
						gRp5c01_data[0][5] = (mytime->tm_hour % 12) / 10;
						if (mytime->tm_hour >=12)
							gRp5c01_data[0][5] |= 2;
					}

					/* Day of week */
					gRp5c01_data[0][6] = mytime->tm_wday;

					/* Day of month */
					gRp5c01_data[0][7] = mytime->tm_mday % 10;
					gRp5c01_data[0][8] = mytime->tm_mday / 10;

					/* Month */
					gRp5c01_data[0][9] = (mytime->tm_mon+1) % 10;
					gRp5c01_data[0][10] = (mytime->tm_mon+1) / 10;

					/* Year */
					gRp5c01_data[0][11] = mytime->tm_year % 10;
					gRp5c01_data[0][12] = (mytime->tm_year % 100) / 10;
				}
			}
		}
		/* Check if the alarm is being set */
		else if ((gRp5c01_mode & 3) == 1)
		{
		}

		/* Return data stored in the RP5C01 "chip" */
		ret = gRp5c01_data[gRp5c01_mode & 0x03][port & 0x0F];
	}

	return ret;
}

/*
===================================================================
Handle a pulse on the chip's CLK pin.  This causes the shift
register to shift 1 bit if in the Shift Mode.
===================================================================
*/
void pd1990ac_clk_pulse(uchar val)
{
	if (gClockMode == 1)
	{
		/* Perform a shift operation */
		clock_sr[0] >>= 1;
		clock_sr[0] |= clock_sr[1] & 1 ? 0x80 : 0;
		clock_sr[1] >>= 1;
		clock_sr[1] |= clock_sr[2] & 1 ? 0x80 : 0;
		clock_sr[2] >>= 1;
		clock_sr[2] |= clock_sr[3] & 1 ? 0x80 : 0;
		clock_sr[3] >>= 1;
		clock_sr[3] |= clock_sr[4] & 1 ? 0x80 : 0;
		clock_sr[4] >>= 1;
		clock_sr[4] |= val & 0x10 ? 0x80 : 0;
		clock_serial_out = clock_sr[0] & 1;
	}
}

