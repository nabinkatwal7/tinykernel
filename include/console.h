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

/* Cursor movement without erasing, for line editing: written like characters. The cursor wraps between rows. */
#define CON_LEFT  '\x11'
#define CON_RIGHT '\x12'

void console_clear(void);           /* clears the scrolling area (row 0 is the status bar) */
void console_putchar(char c);       /* handles \n \r \b \t; mirrored to COM1 */
void console_write(const char *s);
void console_printf(const char *fmt, ...);
/* Output capture: while active, console output is appended to the buffer instead of being shown
   (serial too). Used by the self-test runner. */
void console_capture_begin(char *buf, unsigned cap);
int  console_capture_full(void);    /* 1 while a capture is active and its buffer has no room left */
int  console_capture_end(void);      /* returns the number of characters captured */

void console_set_color(uint8_t fg, uint8_t bg);
void console_status(const char *text);

/* Graphical console (kernel/gfxcon.c): the console drawn on the 320x200 screen, 40 x 12 cells. */
int  gfxcon_enable(int on);          /* switches the video mode; 0 on success */
int  gfxcon_active(void);
void gfxcon_putchar(char c);
void gfxcon_clear(void);
void gfxcon_set_attr(uint8_t attr);


/* Direct cell access for full-screen programs (the editor). Rows 1-24, columns 0-79. */
#define CONSOLE_COLS 80
#define CONSOLE_FIRST_ROW 1
#define CONSOLE_LAST_ROW 24
void console_putat(int x, int y, char c, uint8_t attr);
uint8_t console_get_attr(int x, int y);               /* attribute byte of a cell */
void console_set_attr(int x, int y, uint8_t attr);     /* recolour a cell, keeping its character */
void console_set_hw_cursor(int x, int y);   /* move the blinking cursor without touching the text cursor */

#endif
