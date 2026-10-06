#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_time {
	uint16_t year; /* full year, e.g. 2026 */
	uint8_t month, day, hour, minute, second;
};

void     rtc_read(struct rtc_time *t);     /* consistent snapshot, binary values, 24-hour */
uint32_t rtc_unix(const struct rtc_time *t); /* seconds since 1970-01-01 (UTC as the RTC says) */

#endif
