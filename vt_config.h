#pragma once

typedef struct {
	char ssid[64];
	char password[80];
	char tz[64];
	char ntp[64];
	int  model;
	int  sound;                     /* sound=off in virtualt.cfg turns it off */
} vt_config_t;

extern vt_config_t vt_cfg;

enum { VT_TIME_SAVED, VT_TIME_NTP, VT_TIME_KEPT };

int  vt_config_load(void);                         /* 1 if the file was found */
int  vt_time_init(void (*say)(const char *));      /* VT_TIME_NTP / VT_TIME_SAVED */
void vt_time_save(void);
void vt_time_share(void);                         /* copy the time into the always-on timer */
