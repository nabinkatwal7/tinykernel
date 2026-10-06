#include "klog.h"

#include <stdarg.h>

#include "console.h"
#include "io.h"
#include "kprintf.h"
#include "serial.h"
#include "timer.h"

#define RING_SIZE 4096

static char ring[RING_SIZE];
static unsigned ring_head;  /* next write position */
static unsigned ring_count; /* valid bytes, capped at RING_SIZE */
static int console_level = LOG_WARN;

static const char *const names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

static void ring_put(char c)
{
	ring[ring_head] = c;
	ring_head = (ring_head + 1) % RING_SIZE;
	if (ring_count < RING_SIZE)
		ring_count++;
}

void klog_set_console_level(int level)
{
	console_level = level;
}

void klog(int level, const char *fmt, ...)
{
	char msg[160], line[200];
	uint32_t ticks = timer_ticks();
	uint32_t f = irq_save();
	va_list ap;
	char *p;

	va_start(ap, fmt);
	kvsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	ksnprintf(line, sizeof line, "[%5u.%02u] %s %s\n", ticks / timer_hz(),
		  ticks % timer_hz(), names[level], msg);
	for (p = line; *p; p++)
		ring_put(*p);
	if (level >= console_level)
		console_write(line); /* the console mirrors to serial */
	else
		serial_write(line);
	irq_restore(f);
}

void klog_dump(void)
{
	unsigned i, start = (ring_head + RING_SIZE - ring_count) % RING_SIZE;

	for (i = 0; i < ring_count; i++)
		console_putchar(ring[(start + i) % RING_SIZE]);
}
