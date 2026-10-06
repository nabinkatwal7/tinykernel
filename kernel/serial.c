#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
	outb(COM1 + 1, 0x00); /* no interrupts */
	outb(COM1 + 3, 0x80); /* DLAB on */
	outb(COM1 + 0, 0x03); /* divisor 3 = 38400 baud */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
	outb(COM1 + 2, 0xC7); /* FIFO on */
	outb(COM1 + 4, 0x0B);
}

void serial_putc(char c)
{
	int spin = 20000; /* bounded: a missing port must not hang the kernel */

	while (!(inb(COM1 + 5) & 0x20) && --spin)
		;
	outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
	while (*s)
		serial_putc(*s++);
}
