/* vt_config.c -- /virtualt/virtualt.cfg, and setting the clock at startup.
 *
 * virtualt.cfg is plain text, one setting per line, '#' starts a comment:
 *   ssid=MyNetwork
 *   password=secret
 *   tz=PST8PDT,M3.2.0,M11.1.0     (POSIX TZ string; default US Pacific)
 *   ntp=pool.ntp.org               (default)
 *
 * Time comes from NTP over WiFi when ssid is set and the network answers;
 * otherwise from /virtualt/lasttime.txt (saved on exit and every 10 min),
 * so the clock at least moves forward from where it was last seen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"
#include "pico/aon_timer.h"
#include "drivers/fat32.h"
#include "vt_config.h"

#define CFG_PATH  "/virtualt/virtualt.cfg"
#define TIME_PATH "/virtualt/lasttime.txt"

vt_config_t vt_cfg = {
	.tz = "PST8PDT,M3.2.0,M11.1.0",
	.ntp = "pool.ntp.org",
	.model = 100,
	.sound = 1,
};

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
	return s;
}

static void set_str(char *dst, size_t n, const char *v)
{
	strncpy(dst, v, n - 1);
	dst[n - 1] = 0;
}

int vt_config_load(void)
{
	static char buf[2048];
	fat32_file_t f;
	size_t n = 0;

	if (fat32_open(&f, CFG_PATH) != FAT32_OK)
		return 0;
	fat32_read(&f, buf, sizeof buf - 1, &n);
	fat32_close(&f);
	buf[n] = 0;

	for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
	{
		char *l = trim(line);
		if (*l == '#' || !*l)
			continue;
		char *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1);
		if (!strcasecmp(k, "ssid"))          set_str(vt_cfg.ssid, sizeof vt_cfg.ssid, v);
		else if (!strcasecmp(k, "password")) set_str(vt_cfg.password, sizeof vt_cfg.password, v);
		else if (!strcasecmp(k, "tz"))       set_str(vt_cfg.tz, sizeof vt_cfg.tz, v);
		else if (!strcasecmp(k, "ntp"))      set_str(vt_cfg.ntp, sizeof vt_cfg.ntp, v);
		else if (!strcasecmp(k, "model"))    vt_cfg.model = atoi(v);
		else if (!strcasecmp(k, "sound"))    vt_cfg.sound = !(strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcasecmp(v, "no") == 0);
	}
	return 1;
}

/* ------------------------------------------------------------- time */

static volatile int g_sntp_done;

void vt_sntp_set_time(unsigned long sec)
{
	struct timeval tv = { .tv_sec = (time_t)sec, .tv_usec = 0 };
	settimeofday(&tv, NULL);
	g_sntp_done = 1;
}

static void restore_saved_time(void)
{
	fat32_file_t f;
	char buf[32];
	size_t n = 0;
	if (fat32_open(&f, TIME_PATH) != FAT32_OK)
		return;
	fat32_read(&f, buf, sizeof buf - 1, &n);
	fat32_close(&f);
	buf[n] = 0;
	long long t = atoll(buf);
	if (t > 1600000000LL)
	{
		struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
		settimeofday(&tv, NULL);
	}
}

void vt_time_save(void)
{
	time_t now = time(NULL);
	if (now < 1600000000)
		return;     /* never learned the time: don't save garbage */
	char buf[32];
	int len = snprintf(buf, sizeof buf, "%lld\n", (long long)now);
	fat32_file_t f;
	size_t w;
	fat32_delete(TIME_PATH);
	if (fat32_create(&f, TIME_PATH) != FAT32_OK)
		return;
	fat32_write(&f, buf, len, &w);
	fat32_close(&f);
}

/* The RP2350's always-on (POWMAN) timer runs from the crystal and is
   outside main RAM. If it survives the soft reboot uf2loader does when
   switching programs, it can carry the time from one program (or one
   VirtualT session) to the next until the power is switched off.
   Only NTP or a user-set TIME$/DATE$ go into it -- never the saved
   lasttime.txt guess -- so anything read back from it is real. */
static int aon_has_time(struct timespec *ts)
{
	return aon_timer_is_running() && aon_timer_get_time(ts) && ts->tv_sec > 1600000000;
}

void vt_time_share(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (tv.tv_sec < 1600000000)
		return;
	struct timespec ts = { .tv_sec = tv.tv_sec, .tv_nsec = tv.tv_usec * 1000 };
	if (aon_timer_is_running())
		aon_timer_set_time(&ts);
	else
		aon_timer_start(&ts);
}

int vt_time_init(void (*say)(const char *))
{
	char msg[64];
	struct timespec ts;

	setenv("TZ", vt_cfg.tz, 1);
	tzset();

	if (aon_has_time(&ts))
	{
		struct timeval tv = { .tv_sec = ts.tv_sec, .tv_usec = ts.tv_nsec / 1000 };
		settimeofday(&tv, NULL);
		say("Clock: kept from earlier, no WiFi needed");
		return VT_TIME_KEPT;
	}
	restore_saved_time();

	if (!vt_cfg.ssid[0])
	{
		say("Clock: no ssid in virtualt.cfg");
		return VT_TIME_SAVED;
	}

	snprintf(msg, sizeof msg, "WiFi: joining %.24s...", vt_cfg.ssid);
	say(msg);
	if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA) != 0)
	{
		say("WiFi: chip init failed");
		return VT_TIME_SAVED;
	}
	cyw43_arch_enable_sta_mode();

	int result = VT_TIME_SAVED;
	/* joins sometimes fail once (router slow to answer right after the
	   radio powers up): try up to three times */
	int err = -1;
	for (int attempt = 1; attempt <= 3 && err; attempt++)
	{
		if (attempt > 1)
		{
			snprintf(msg, sizeof msg, "WiFi: retrying (%d of 3)...", attempt);
			say(msg);
		}
		err = cyw43_arch_wifi_connect_timeout_ms(vt_cfg.ssid, vt_cfg.password,
		                                         CYW43_AUTH_WPA2_AES_PSK, 15000);
	}
	if (err != 0)
	{
		snprintf(msg, sizeof msg, "WiFi: couldn't join (%d)", err);
		say(msg);
	}
	else
	{
		snprintf(msg, sizeof msg, "NTP: asking %.28s...", vt_cfg.ntp);
		say(msg);
		g_sntp_done = 0;
		sntp_setoperatingmode(SNTP_OPMODE_POLL);
		sntp_setservername(0, vt_cfg.ntp);
		sntp_init();
		absolute_time_t until = make_timeout_time_ms(10000);
		while (!g_sntp_done && absolute_time_diff_us(get_absolute_time(), until) > 0)
		{
			cyw43_arch_poll();
			sleep_ms(5);
		}
		sntp_stop();
		if (g_sntp_done)
		{
			result = VT_TIME_NTP;
			vt_time_save();
			vt_time_share();
		}
		else
			say("NTP: no answer");
	}
	cyw43_arch_deinit();    /* radio off again until something needs it */
	return result;
}
