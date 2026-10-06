#include "debug.h"

#include <stdarg.h>

#include "console.h"
#include "io.h"
#include "klog.h"
#include "kprintf.h"
#include "ksym.h"

void debug_hexdump(const void *addr, uint32_t len)
{
	const uint8_t *p = addr;
	uint32_t i, j;

	for (i = 0; i < len; i += 16) {
		console_printf("%08x  ", (uint32_t)(p + i));
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				console_printf("%02x ", p[i + j]);
			else
				console_write("   ");
		}
		console_putchar(' ');
		for (j = 0; j < 16 && i + j < len; j++) {
			uint8_t c = p[i + j];

			console_putchar(c >= 32 && c < 127 ? (char)c : '.');
		}
		console_putchar('\n');
	}
}

void debug_dump_regs(const struct regs *r)
{
	console_printf("eax=%08x ebx=%08x ecx=%08x edx=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
	console_printf("esi=%08x edi=%08x ebp=%08x esp=%08x\n", r->esi, r->edi, r->ebp, r->esp);
	console_printf("eip=%08x cs=%04x eflags=%08x int=%u err=%x\n", r->eip, r->cs, r->eflags,
		       r->int_no, r->err_code);
}

#define BT_MAX_FRAMES 16

static void bt_line(int n, uint32_t eip)
{
	uint32_t off;
	const char *name = ksym_lookup(eip - 1, &off); /* -1: a call that never returns must not look like the next function */

	if (name)
		console_printf("  #%d %08x %s+0x%x\n", n, eip, name, off + 1);
	else
		console_printf("  #%d %08x\n", n, eip);
}

/* Each frame is [saved ebp][return address]; the chain must climb and stay close, or we stop. */
void debug_backtrace(uint32_t ebp)
{
	int n = 0;

	while (n < BT_MAX_FRAMES && ebp && !(ebp & 3) && ebp >= 0x1000 && ebp < 0xFFFFFF00u) {
		uint32_t *frame = (uint32_t *)ebp;
		uint32_t next = frame[0];

		if (!frame[1])
			break;
		bt_line(n++, frame[1]);
		if (next <= ebp || next - ebp > 0x10000)
			break;
		ebp = next;
	}
}

void panic(const char *fmt, ...)
{
	char msg[160];
	va_list ap;

	cli();
	va_start(ap, fmt);
	kvsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	console_set_color(COLOR_WHITE, COLOR_RED);
	console_printf("\n*** KERNEL PANIC: %s ***\n", msg);
	klog(LOG_ERROR, "PANIC: %s", msg);
	console_write("stack trace:\n");
	debug_here();
	for (;;)
		hlt();
}
