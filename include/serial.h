#ifndef SERIAL_H
#define SERIAL_H

/* COM1, 38400 8N1. Used to mirror the console and kernel log (handy with qemu -serial). */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

/* Receive side: IRQ4 feeds typed characters into the keyboard queue, so the shell works over
   the serial line too (Enter = CR, Backspace = DEL/BS, Ctrl+C, and ANSI arrow keys). */
void serial_rx_init(void);

#endif
