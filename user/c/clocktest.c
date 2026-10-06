/* clock_gettime(): monotonic never goes backwards and tracks sleep_ms; realtime is a sane date. */
#include "stdio.h"
#include "usys.h"

static int fails;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

/* milliseconds between two readings (32-bit arithmetic only: user programs have no 64-bit divide) */
static int delta_ms(const struct timespec *a, const struct timespec *b)
{
	return (int)(b->tv_sec - a->tv_sec) * 1000 + (int)(b->tv_nsec / 1000000) - (int)(a->tv_nsec / 1000000);
}

int main(void)
{
	struct timespec a, b, rt;
	int d;

	check(clock_gettime(CLOCK_MONOTONIC, &a) == 0, "monotonic clock readable");
	sleep_ms(200);
	clock_gettime(CLOCK_MONOTONIC, &b);
	d = delta_ms(&a, &b);
	check(d >= 0, "monotonic clock does not go backwards");
	check(d >= 150 && d < 400, "a 200 ms sleep measures about 200 ms");
	check(clock_gettime(CLOCK_REALTIME, &rt) == 0 && rt.tv_sec > 1700000000u, "realtime is after 2023");
	check(clock_gettime(99, &a) == -1, "an unknown clock is rejected");
	printf("slept 200 ms, monotonic says %d ms; realtime %d\n", d, (int)rt.tv_sec);
	if (!fails)
		puts("clocktest: all checks passed");
	return fails;
}
