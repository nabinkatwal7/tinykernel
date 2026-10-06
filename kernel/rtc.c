#include "rtc.h"

#include "io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

static uint8_t cmos(uint8_t reg)
{
	outb(CMOS_INDEX, reg); /* bit 7 clear: leave NMI enabled */
	return inb(CMOS_DATA);
}

static int updating(void)
{
	return cmos(0x0A) & 0x80;
}

static uint8_t bcd(uint8_t v)
{
	return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

static void raw_read(uint8_t r[7])
{
	while (updating())
		;
	r[0] = cmos(0x00);
	r[1] = cmos(0x02);
	r[2] = cmos(0x04);
	r[3] = cmos(0x07);
	r[4] = cmos(0x08);
	r[5] = cmos(0x09);
	r[6] = cmos(0x32);
}

void rtc_read(struct rtc_time *t)
{
	uint8_t a[7], b[7], status_b;
	int i, same;
	int pm;

	do { /* two identical reads in a row means no update tore the value */
		raw_read(a);
		raw_read(b);
		for (same = 1, i = 0; i < 7; i++)
			same &= a[i] == b[i];
	} while (!same);

	status_b = cmos(0x0B);
	pm = !(status_b & 0x02) && (b[2] & 0x80);
	b[2] &= 0x7F;
	if (!(status_b & 0x04)) { /* BCD */
		for (i = 0; i < 7; i++)
			b[i] = bcd(b[i]);
	}
	if (!(status_b & 0x02)) { /* 12-hour clock */
		b[2] %= 12;
		if (pm)
			b[2] += 12;
	}

	t->second = b[0];
	t->minute = b[1];
	t->hour = b[2];
	t->day = b[3];
	t->month = b[4];
	t->year = (uint16_t)((b[6] ? b[6] : 20) * 100 + b[5]);
}

uint32_t rtc_unix(const struct rtc_time *t)
{
	static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	uint32_t y = t->year, days;

	days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
	days += before[(t->month - 1) % 12] + t->day - 1;
	if (t->month > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
		days++;
	return days * 86400 + t->hour * 3600u + t->minute * 60u + t->second;
}

void rtc_from_unix(uint32_t secs, struct rtc_time *t)
{
	int z = (int)(secs / 86400) + 719468;
	int era = z / 146097;
	int doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int mp = (5 * doy + 2) / 153;
	int d = doy - (153 * mp + 2) / 5 + 1;
	int m = mp < 10 ? mp + 3 : mp - 9;

	t->year = (uint16_t)(yoe + era * 400 + (m <= 2));
	t->month = (uint8_t)m;
	t->day = (uint8_t)d;
	t->hour = (uint8_t)(secs % 86400 / 3600);
	t->minute = (uint8_t)(secs % 3600 / 60);
	t->second = (uint8_t)(secs % 60);
}
