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

	for (i = VGA_TOP * VGA_WIDTH; i < VGA_WIDTH * VGA_HEIGHT; i++)
		VGA_ADDR[i] = cell(' ');
	cursor_x = 0;
	cursor_y = VGA_TOP;
	update_cursor();
	irq_restore(f);
}

void console_putchar(char c)
{
	uint32_t f = irq_save();

	if (c == '\b') {
		serial_putc('\b');
		serial_putc(' ');
		serial_putc('\b');
	} else {
		serial_putc(c);
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

void console_set_color(uint8_t fg, uint8_t bg)
{
	attr = (uint8_t)((bg << 4) | (fg & 0x0F));
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
