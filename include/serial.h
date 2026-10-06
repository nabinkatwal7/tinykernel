#ifndef SERIAL_H
#define SERIAL_H

/* COM1, 38400 8N1. Used to mirror the console and kernel log (handy with qemu -serial). */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

#endif
