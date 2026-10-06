#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#define COLOR_BLACK       0
#define COLOR_BLUE        1
#define COLOR_GREEN       2
#define COLOR_CYAN        3
#define COLOR_RED         4
#define COLOR_MAGENTA     5
#define COLOR_BROWN       6
#define COLOR_LIGHT_GREY  7
#define COLOR_DARK_GREY   8
#define COLOR_LIGHT_BLUE  9
#define COLOR_LIGHT_GREEN 10
#define COLOR_LIGHT_CYAN  11
#define COLOR_LIGHT_RED   12
#define COLOR_PINK        13
#define COLOR_YELLOW      14
#define COLOR_WHITE       15

void console_clear(void);           /* clears the scrolling area (row 0 is the status bar) */
void console_putchar(char c);       /* handles \n \r \b \t; mirrored to COM1 */
void console_write(const char *s);
void console_printf(const char *fmt, ...);
void console_set_color(uint8_t fg, uint8_t bg);
void console_status(const char *text); /* paints the reserved top row */

#endif
