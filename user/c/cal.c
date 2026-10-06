/* cal [month [year]]: a month calendar; the current month by default, with today marked by '*'. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

static const char *const month_name[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December",
};

static int leap(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int m, int y)
{
	static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	return m == 2 && leap(y) ? 29 : d[m - 1];
}

/* 0 = Sunday (Sakamoto's method) */
static int weekday(int y, int m, int d)
{
	static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

	if (m < 3)
		y--;
	return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* Days since 1970-01-01 -> year, month, day (Howard Hinnant's civil_from_days). */
static void civil(unsigned days, int *y, int *m, int *d)
{
	int z = (int)days + 719468;
	int era = z / 146097;
	int doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int mp = (5 * doy + 2) / 153;

	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

int main(int argc, char **argv)
{
	struct timespec now;
	int ty = 1970, tm = 1, td = 1, y, m, first, n, i, pad;
	char title[32];

	if (clock_gettime(CLOCK_REALTIME, &now) == 0)
		civil(now.tv_sec / 86400, &ty, &tm, &td);
	m = argc > 1 ? atoi(argv[1]) : tm;
	y = argc > 2 ? atoi(argv[2]) : ty;
	if (m < 1 || m > 12 || y < 1 || y > 9999) {
		puts("usage: cal [month 1-12 [year]]");
		return 1;
	}
	snprintf(title, sizeof title, "%s %d", month_name[m - 1], y);
	pad = (20 - (int)strlen(title)) / 2;
	for (i = 0; i < pad; i++)
		printf(" ");
	puts(title);
	puts("Su Mo Tu We Th Fr Sa");
	first = weekday(y, m, 1);
	n = days_in(m, y);
	for (i = 0; i < first; i++)
		printf("   ");
	for (i = 1; i <= n; i++) {
		int today = y == ty && m == tm && i == td;

		printf("%2d%c", i, today ? '*' : ' ');
		if ((first + i) % 7 == 0 || i == n)
			printf("\n");
	}
	return 0;
}
