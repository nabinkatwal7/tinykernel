#include "timer.h"

#include "idt.h"
#include "io.h"
#include "pic.h"
#include "sched.h"

#define PIT_CMD  0x43
#define PIT_CH0  0x40
#define PIT_FREQ 1193182u

static volatile uint32_t ticks;
static uint32_t hz = 100;

static void timer_irq(struct regs *r)
{
	(void)r;
	ticks++;
	sched_tick();
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
