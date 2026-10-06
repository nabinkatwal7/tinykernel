#ifndef NTP_H
#define NTP_H

#include <stdint.h>

/* SNTP: ask a time server for the current time. */
int ntp_query(uint32_t server, uint32_t *unix_time, uint32_t timeout_ms);   /* 0 and seconds since 1970, or -1 */

/* ntpdate [-q] [SERVER] */
int cmd_ntpdate(int argc, char **argv);

#endif
