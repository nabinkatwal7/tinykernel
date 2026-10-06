#include "serial.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "pic.h"

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

static int esc_state; /* 0 = normal, 1 = after ESC, 2 = after ESC [ */

static void rx_char(uint8_t c, struct regs *r)
{
	if (esc_state == 1) {
		esc_state = c == '[' ? 2 : 0;
		return;
	}
	if (esc_state == 2) {
		esc_state = 0;
		switch (c) {
		case 'A': keyboard_inject(KEY_UP); break;
		case 'B': keyboard_inject(KEY_DOWN); break;
		case 'C': keyboard_inject(KEY_RIGHT); break;
		case 'D': keyboard_inject(KEY_LEFT); break;
		}
		return;
	}
	switch (c) {
	case 0x1B:
		esc_state = 1;
		break;
	case '\r':
	case '\n':
		keyboard_inject('\n');
		break;
	case 0x7F:
	case '\b':
		keyboard_inject('\b');
		break;
	case 0x03:
		keyboard_inject_sigint(r);
		break;
	default:
		if (c >= 1 && c < 127)
			keyboard_inject(c);
		break;
	}
}

static void serial_irq(struct regs *r)
{
	while (inb(COM1 + 5) & 0x01)
		rx_char(inb(COM1), r);
}

void serial_rx_init(void)
{
	irq_install_handler(4, serial_irq);
	outb(COM1 + 4, 0x0B);  /* DTR, RTS and OUT2 (OUT2 gates the interrupt line) */
	outb(COM1 + 1, 0x01);  /* interrupt on received data */
	pic_unmask(4);
}

void serial_write(const char *s)
{
	while (*s)
		serial_putc(*s++);
}
