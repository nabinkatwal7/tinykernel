#include "cpustat.h"

#include "io.h"
#include "percpu.h"
#include "sched.h"
#include "smpsched.h"
#include "timer.h"

/* cumulative (busy, idle) tick counters per core, one snapshot per second */
static struct {
	uint32_t busy, idle;
} hist[CPUSTAT_HISTORY + 1][MAX_CPUS];
static uint32_t nsamples;           /* snapshots taken so far; snapshot k lives in hist[k % (HISTORY+1)] */

static void read_counters(int cpu, uint32_t *busy, uint32_t *idle)
{
	struct percpu *c = percpu_get(cpu);

	*busy = *idle = 0;
	if (!c)
		return;
	if (c->is_bsp) {
		uint32_t idle_ticks = sched_idle_ticks();

		*idle = idle_ticks;
		*busy = timer_ticks() - idle_ticks;
	} else if (c->online) {
		uint32_t sw;

		smpsched_stats(cpu, busy, idle, &sw);
	}
}

/* timer callback: runs in interrupt context on the boot core, so it only reads counters */
static void sample(void *arg)
{
	int cpu;

	(void)arg;
	for (cpu = 0; cpu < percpu_count(); cpu++)
		read_counters(cpu, &hist[nsamples % (CPUSTAT_HISTORY + 1)][cpu].busy,
			      &hist[nsamples % (CPUSTAT_HISTORY + 1)][cpu].idle);
	nsamples++;
}

void cpustat_init(void)
{
	sample(0);                       /* the baseline */
	ktimer_add(1000, 1, sample, 0);
}

uint32_t cpustat_samples(void)
{
	return nsamples;
}

uint32_t cpustat_busy_percent(int cpu, uint32_t seconds)
{
	uint32_t now, then, b, i, db, di;
	uint32_t flags = irq_save();

	if (nsamples < 2 || cpu < 0 || cpu >= MAX_CPUS) {
		irq_restore(flags);
		return 0;
	}
	if (seconds > CPUSTAT_HISTORY)
		seconds = CPUSTAT_HISTORY;
	if (seconds > nsamples - 1)
		seconds = nsamples - 1;
	now = nsamples - 1;
	then = now - seconds;
	b = hist[now % (CPUSTAT_HISTORY + 1)][cpu].busy;
	i = hist[now % (CPUSTAT_HISTORY + 1)][cpu].idle;
	db = b - hist[then % (CPUSTAT_HISTORY + 1)][cpu].busy;
	di = i - hist[then % (CPUSTAT_HISTORY + 1)][cpu].idle;
	irq_restore(flags);
	return db + di ? db * 100 / (db + di) : 0;
}
