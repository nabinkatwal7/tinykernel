#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* 2D drawing on the 320x200x256 framebuffer (see vga.h). All primitives clip to the screen. */
void    gfx_putpixel(int x, int y, uint8_t color);
uint8_t gfx_getpixel(int x, int y);               /* 0 outside the screen */
void    gfx_hline(int x0, int x1, int y, uint8_t color);
void    gfx_vline(int x, int y0, int y1, uint8_t color);
void    gfx_line(int x0, int y0, int x1, int y1, uint8_t color);    /* Bresenham, any slope */
void    gfx_rect(int x, int y, int w, int h, uint8_t color);        /* outline */
void    gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void    gfx_circle(int cx, int cy, int r, uint8_t color);           /* midpoint algorithm */
void    gfx_fill_circle(int cx, int cy, int r, uint8_t color);
#define GFX_FONT_W 8
#define GFX_FONT_H 16
void    gfx_char(int x, int y, char c, int fg, int bg);             /* bg < 0: transparent */
void    gfx_text(int x, int y, const char *s, int fg, int bg);
int     gfx_text_width(const char *s);
void    gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color);

#endif
