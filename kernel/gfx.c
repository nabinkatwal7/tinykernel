#include "gfx.h"

#include "vga.h"

static int clip_x(int x) { return x < 0 ? 0 : x >= VGA_GFX_W ? VGA_GFX_W - 1 : x; }

void gfx_putpixel(int x, int y, uint8_t color)
{
	if (x >= 0 && x < VGA_GFX_W && y >= 0 && y < VGA_GFX_H)
		vga_framebuffer()[y * VGA_GFX_W + x] = color;
}

uint8_t gfx_getpixel(int x, int y)
{
	if (x < 0 || x >= VGA_GFX_W || y < 0 || y >= VGA_GFX_H)
		return 0;
	return vga_framebuffer()[y * VGA_GFX_W + x];
}

void gfx_hline(int x0, int x1, int y, uint8_t color)
{
	uint8_t *row;
	int x;

	if (y < 0 || y >= VGA_GFX_H)
		return;
	if (x0 > x1) {
		int t = x0;

		x0 = x1;
		x1 = t;
	}
	if (x1 < 0 || x0 >= VGA_GFX_W)
		return;
	x0 = clip_x(x0);
	x1 = clip_x(x1);
	row = vga_framebuffer() + y * VGA_GFX_W;
	for (x = x0; x <= x1; x++)
		row[x] = color;
}

void gfx_vline(int x, int y0, int y1, uint8_t color)
{
	int y;

	if (x < 0 || x >= VGA_GFX_W)
		return;
	if (y0 > y1) {
		int t = y0;

		y0 = y1;
		y1 = t;
	}
	if (y0 < 0)
		y0 = 0;
	if (y1 >= VGA_GFX_H)
		y1 = VGA_GFX_H - 1;
	for (y = y0; y <= y1; y++)
		vga_framebuffer()[y * VGA_GFX_W + x] = color;
}

/* Bresenham's algorithm with the error term kept in integers for all eight octants. */
void gfx_line(int x0, int y0, int x1, int y1, uint8_t color)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0; /* negative */
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		int e2;

		gfx_putpixel(x0, y0, color);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}
