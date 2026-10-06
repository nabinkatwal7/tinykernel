#include "console.h"

#include <stdarg.h>

#include "io.h"
#include "kprintf.h"
#include "serial.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_TOP    1 /* row 0 is reserved for the status bar */
#define VGA_ADDR   ((volatile unsigned short *)0xB8000)
#define STATUS_ATTR 0x70 /* black on light grey */

static int cursor_x;
static int cursor_y = VGA_TOP;
static uint8_t attr = 0x0F; /* white on black */

static unsigned short cell(char c)
{
	return (unsigned short)((unsigned char)c | (attr << 8));
}

static void update_cursor(void)
{
	unsigned short pos = (unsigned short)(cursor_y * VGA_WIDTH + cursor_x);

	outb(0x3D4, 0x0F);
	outb(0x3D5, (uint8_t)(pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (uint8_t)(pos >> 8));
}

static void scroll_if_needed(void)
{
	int i;

	if (cursor_y < VGA_HEIGHT)
		return;

	for (i = VGA_TOP * VGA_WIDTH; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
		VGA_ADDR[i] = VGA_ADDR[i + VGA_WIDTH];
	for (i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
		VGA_ADDR[i] = cell(' ');
	cursor_y = VGA_HEIGHT - 1;
}

static void newline(void)
{
	cursor_x = 0;
	cursor_y++;
	scroll_if_needed();
}

static void put_screen(char c)
{
	switch (c) {
	case '\n':
		newline();
		return;
	case '\r':
		cursor_x = 0;
		return;
	case CON_LEFT:
		if (cursor_x > 0) {
			cursor_x--;
		} else if (cursor_y > VGA_TOP) {
			cursor_y--;
			cursor_x = VGA_WIDTH - 1;
		}
		return;
	case CON_RIGHT:
		if (cursor_x < VGA_WIDTH - 1) {
			cursor_x++;
		} else if (cursor_y < VGA_HEIGHT - 1) {
			cursor_y++;
			cursor_x = 0;
		}
		return;
	case '\b':
		if (cursor_x > 0) {
			cursor_x--;
		} else if (cursor_y > VGA_TOP) {
			cursor_y--;
			cursor_x = VGA_WIDTH - 1;
		} else {
			return;
		}
		VGA_ADDR[cursor_y * VGA_WIDTH + cursor_x] = cell(' ');
		return;
	case '\t':
		do
			put_screen(' ');
		while (cursor_x % 4);
		return;
	}

	VGA_ADDR[cursor_y * VGA_WIDTH + cursor_x] = cell(c);
	if (++cursor_x >= VGA_WIDTH)
		newline();
}

void console_clear(void)
{
	uint32_t f = irq_save();
	int i;

	if (gfxcon_active()) {
		gfxcon_clear();
		irq_restore(f);
		return;
	}

	for (i = VGA_TOP * VGA_WIDTH; i < VGA_WIDTH * VGA_HEIGHT; i++)
		VGA_ADDR[i] = cell(' ');
	cursor_x = 0;
	cursor_y = VGA_TOP;
	update_cursor();
	irq_restore(f);
}

static char *cap_buf;
static unsigned cap_size, cap_len;
static struct { char *buf; unsigned size, len; } cap_stack[4]; /* captures nest: a pipeline inside a captured self-test */
static int cap_depth;

void console_capture_begin(char *buf, unsigned cap)
{
	uint32_t f = irq_save();

	if (cap_buf && cap_depth < 4) {
		cap_stack[cap_depth].buf = cap_buf;
		cap_stack[cap_depth].size = cap_size;
		cap_stack[cap_depth].len = cap_len;
		cap_depth++;
	}
	cap_buf = buf;
	cap_size = cap;
	cap_len = 0;
	if (cap)
		buf[0] = '\0';
	irq_restore(f);
}

int console_capture_full(void)
{
	return cap_buf && cap_len + 1 >= cap_size;
}

int console_capture_end(void)
{
	uint32_t f = irq_save();
	int n = (int)cap_len;

	cap_buf = 0;
	if (cap_depth > 0) { /* resume the enclosing capture */
		cap_depth--;
		cap_buf = cap_stack[cap_depth].buf;
		cap_size = cap_stack[cap_depth].size;
		cap_len = cap_stack[cap_depth].len;
	}
	irq_restore(f);
	return n;
}

void console_putchar(char c)
{
	uint32_t f = irq_save();

	if (cap_buf && (c == CON_LEFT || c == CON_RIGHT)) { /* cursor movement is not output */
		irq_restore(f);
		return;
	}
	if (cap_buf) { /* captured, not displayed */
		if (cap_len + 1 < cap_size) {
			cap_buf[cap_len++] = c;
			cap_buf[cap_len] = '\0';
		}
		irq_restore(f);
		return;
	}

	if (c == '\b') {
		serial_putc('\b');
		serial_putc(' ');
		serial_putc('\b');
	} else if (c == CON_LEFT || c == CON_RIGHT) {
		serial_write(c == CON_LEFT ? "\x1b[D" : "\x1b[C");
	} else {
		serial_putc(c);
	}
	if (gfxcon_active()) {
		gfxcon_putchar(c);
		irq_restore(f);
		return;
	}
	put_screen(c);
	update_cursor();
	irq_restore(f);
}

void console_write(const char *s)
{
	while (*s)
		console_putchar(*s++);
}

void console_putat(int x, int y, char c, uint8_t a)
{
	if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
		return;
	VGA_ADDR[y * VGA_WIDTH + x] = (unsigned short)((unsigned char)c | (a << 8));
}

uint8_t console_get_attr(int x, int y)
{
	if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
		return 0;
	return (uint8_t)(VGA_ADDR[y * VGA_WIDTH + x] >> 8);
}

void console_set_attr(int x, int y, uint8_t a)
{
	if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
		return;
	VGA_ADDR[y * VGA_WIDTH + x] = (unsigned short)((VGA_ADDR[y * VGA_WIDTH + x] & 0xFF) | (a << 8));
}

void console_set_hw_cursor(int x, int y)
{
	unsigned short pos = (unsigned short)(y * VGA_WIDTH + x);
	uint32_t f = irq_save();

	outb(0x3D4, 0x0F);
	outb(0x3D5, (uint8_t)(pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (uint8_t)(pos >> 8));
	irq_restore(f);
}

void console_set_color(uint8_t fg, uint8_t bg)
{
	attr = (uint8_t)((bg << 4) | (fg & 0x0F));
	gfxcon_set_attr(attr);
}

void console_status(const char *text)
{
	uint32_t f = irq_save();
	int i;

	for (i = 0; i < VGA_WIDTH; i++)
		VGA_ADDR[i] = (unsigned short)((unsigned char)(*text ? *text++ : ' ')
					       | (STATUS_ATTR << 8));
	irq_restore(f);
}

static void put_cb(char c, void *ctx)
{
	(void)ctx;
	console_putchar(c);
}

void console_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	kvformat(put_cb, 0, fmt, ap);
	va_end(ap);
}
