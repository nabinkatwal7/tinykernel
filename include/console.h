#ifndef CONSOLE_H
#define CONSOLE_H

void console_clear(void);
void console_putchar(char c);
void console_write(const char *s);
void console_printf(const char *fmt, ...);

#endif
