#include "timer.h"

#include "idt.h"
#include "io.h"
#include "percpu.h"
#include "smpsched.h"
#include "pic.h"
#include "console.h"
#include "sched.h"

#define PIT_CMD  0x43
#define PIT_CH0  0x40
#define PIT_FREQ 1193182u

static volatile uint32_t ticks;
static uint32_t hz = 100;

struct ktimer {
	int active;
	uint32_t expire, period; /* period == 0: one-shot */
	void (*fn)(void *);
	void *arg;
};

static struct ktimer ktimers[KTIMER_MAX];

static void run_ktimers(void)
{
	int i;

	for (i = 0; i < KTIMER_MAX; i++) {
		struct ktimer *t = &ktimers[i];

		if (!t->active || (int32_t)(ticks - t->expire) < 0)
			continue;
		if (t->period)
			t->expire += t->period;
		else
			t->active = 0;
		t->fn(t->arg);
	}
}

static void timer_irq(struct regs *r)
{
	(void)r;
	if (percpu_count() && !this_cpu()->is_bsp) { /* an application processor: its own scheduler, no global bookkeeping */
		smpsched_tick();
		return;
	}
	ticks++;
	this_cpu()->timer_irqs++;
	run_ktimers();
	sched_tick();
}

int ktimer_add(uint32_t ms, int periodic, void (*fn)(void *), void *arg)
{
	uint32_t f = irq_save();
	uint32_t delay = ms * hz / 1000;
	int i;

	if (delay == 0)
		delay = 1;
	for (i = 0; i < KTIMER_MAX; i++) {
		if (!ktimers[i].active) {
			ktimers[i].expire = ticks + delay;
			ktimers[i].period = periodic ? delay : 0;
			ktimers[i].fn = fn;
			ktimers[i].arg = arg;
			ktimers[i].active = 1;
			irq_restore(f);
			return i;
		}
	}
	irq_restore(f);
	return -1;
}

void ktimer_set_arg(int id, void *arg)
{
	if (id >= 0 && id < KTIMER_MAX)
		ktimers[id].arg = arg;
}

int ktimer_cancel(int id)
{
	uint32_t f = irq_save();
	int rc = -1;

	if (id >= 0 && id < KTIMER_MAX && ktimers[id].active) {
		ktimers[id].active = 0;
		rc = 0;
	}
	irq_restore(f);
	return rc;
}

void ktimer_list(void)
{
	int i, n = 0;

	for (i = 0; i < KTIMER_MAX; i++) {
		if (!ktimers[i].active)
			continue;
		console_printf("  #%d  in %u ticks  %s\n", i, ktimers[i].expire - ticks,
			       ktimers[i].period ? "periodic" : "one-shot");
		n++;
	}
	if (!n)
		console_write("  no active timers\n");
}

void timer_init(uint32_t freq)
{
	uint32_t div = PIT_FREQ / freq;

	hz = freq;
	outb(PIT_CMD, 0x36); /* channel 0, lobyte/hibyte, rate generator */
	outb(PIT_CH0, (uint8_t)(div & 0xFF));
	outb(PIT_CH0, (uint8_t)(div >> 8));
	irq_install_handler(0, timer_irq);
	pic_unmask(0);
}

uint32_t timer_ticks(void) { return ticks; }
uint32_t timer_hz(void) { return hz; }
uint32_t timer_ms(void) { return ticks * (1000 / hz); }
