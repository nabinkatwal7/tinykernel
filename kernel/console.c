#include "console.h"

#include <stdarg.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_ATTR   0x0F /* white on black */
#define VGA_ADDR   ((volatile unsigned short *)0xB8000)

static int cursor_x;
static int cursor_y;

static void scroll_if_needed(void)
{
	int i;

	if (cursor_y < VGA_HEIGHT)
		return;

	for (i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
		VGA_ADDR[i] = VGA_ADDR[i + VGA_WIDTH];
	for (i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
		VGA_ADDR[i] = (unsigned short)(' ' | (VGA_ATTR << 8));
	cursor_y = VGA_HEIGHT - 1;
}

void console_clear(void)
{
	int i;

	for (i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
		VGA_ADDR[i] = (unsigned short)(' ' | (VGA_ATTR << 8));
	cursor_x = 0;
	cursor_y = 0;
}

void console_putchar(char c)
{
	if (c == '\n') {
		cursor_x = 0;
		cursor_y++;
		scroll_if_needed();
		return;
	}

	if (c == '\r') {
		cursor_x = 0;
		return;
	}

	VGA_ADDR[cursor_y * VGA_WIDTH + cursor_x] =
		(unsigned short)((unsigned char)c | (VGA_ATTR << 8));
	cursor_x++;
	if (cursor_x >= VGA_WIDTH) {
		cursor_x = 0;
		cursor_y++;
		scroll_if_needed();
	}
}

void console_write(const char *s)
{
	while (*s)
		console_putchar(*s++);
}

static void print_uint(unsigned int n)
{
	char buf[10];
	int i = 0;

	if (n == 0) {
		console_putchar('0');
		return;
	}
	while (n) {
		buf[i++] = (char)('0' + (n % 10));
		n /= 10;
	}
	while (i--)
		console_putchar(buf[i]);
}

static void print_int(int n)
{
	if (n < 0) {
		console_putchar('-');
		/* ponytail: INT_MIN not handled */
		print_uint((unsigned int)(-n));
	} else {
		print_uint((unsigned int)n);
	}
}

/* %c %s %d %u %% — enough for early kernel logging */
void console_printf(const char *fmt, ...)
{
	va_list ap;
	char c;

	va_start(ap, fmt);
	while ((c = *fmt++) != '\0') {
		if (c != '%') {
			console_putchar(c);
			continue;
		}
		c = *fmt++;
		switch (c) {
		case 'c':
			console_putchar((char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			console_write(s ? s : "(null)");
			break;
		}
		case 'd':
			print_int(va_arg(ap, int));
			break;
		case 'u':
			print_uint(va_arg(ap, unsigned int));
			break;
		case '%':
			console_putchar('%');
			break;
		case '\0':
			goto done;
		default:
			console_putchar('%');
			console_putchar(c);
			break;
		}
	}
done:
	va_end(ap);
}
