#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* 2D drawing on the 320x200x256 framebuffer (see vga.h). All primitives clip to the screen. */
void    gfx_putpixel(int x, int y, uint8_t color);
uint8_t gfx_getpixel(int x, int y);               /* 0 outside the screen */
void    gfx_hline(int x0, int x1, int y, uint8_t color);
void    gfx_vline(int x, int y0, int y1, uint8_t color);
void    gfx_line(int x0, int y0, int x1, int y1, uint8_t color);    /* Bresenham, any slope */

#endif
