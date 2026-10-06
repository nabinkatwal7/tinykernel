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

void gfx_rect(int x, int y, int w, int h, uint8_t color)
{
	if (w <= 0 || h <= 0)
		return;
	gfx_hline(x, x + w - 1, y, color);
	gfx_hline(x, x + w - 1, y + h - 1, color);
	gfx_vline(x, y, y + h - 1, color);
	gfx_vline(x + w - 1, y, y + h - 1, color);
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color)
{
	int row;

	for (row = 0; row < h; row++)
		gfx_hline(x, x + w - 1, y + row, color);
}

/* Midpoint circle: plot the eight symmetric points of each step. */
void gfx_circle(int cx, int cy, int r, uint8_t color)
{
	int x = r, y = 0, err = 1 - r;

	if (r < 0)
		return;
	while (x >= y) {
		gfx_putpixel(cx + x, cy + y, color);
		gfx_putpixel(cx - x, cy + y, color);
		gfx_putpixel(cx + x, cy - y, color);
		gfx_putpixel(cx - x, cy - y, color);
		gfx_putpixel(cx + y, cy + x, color);
		gfx_putpixel(cx - y, cy + x, color);
		gfx_putpixel(cx + y, cy - x, color);
		gfx_putpixel(cx - y, cy - x, color);
		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
}

/* Same walk, but each step fills the horizontal spans instead of plotting points. */
void gfx_fill_circle(int cx, int cy, int r, uint8_t color)
{
	int x = r, y = 0, err = 1 - r;

	if (r < 0)
		return;
	while (x >= y) {
		gfx_hline(cx - x, cx + x, cy + y, color);
		gfx_hline(cx - x, cx + x, cy - y, color);
		gfx_hline(cx - y, cx + y, cy + x, color);
		gfx_hline(cx - y, cx + y, cy - x, color);
		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
}

/* Scanline fill: for every row, intersect with the three edges and fill between the extremes. */
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color)
{
	int px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
	int ymin = y0, ymax = y0, y;

	if (y1 < ymin) ymin = y1;
	if (y2 < ymin) ymin = y2;
	if (y1 > ymax) ymax = y1;
	if (y2 > ymax) ymax = y2;
	for (y = ymin; y <= ymax; y++) {
		int lo = 100000, hi = -100000, i;

		for (i = 0; i < 3; i++) {
			int a = px[i], b = py[i], c = px[(i + 1) % 3], d = py[(i + 1) % 3];

			if (b == d) { /* horizontal edge: it contributes both endpoints if on this row */
				if (y == b) {
					if (a < lo) lo = a;
					if (c < lo) lo = c;
					if (a > hi) hi = a;
					if (c > hi) hi = c;
				}
				continue;
			}
			if ((y >= b && y <= d) || (y >= d && y <= b)) {
				int x = a + (c - a) * (y - b) / (d - b);

				if (x < lo) lo = x;
				if (x > hi) hi = x;
			}
		}
		if (lo <= hi)
			gfx_hline(lo, hi, y, color);
	}
}

/*
 * Text uses the BIOS 8x16 font that vga.c rescued from video memory before leaving text mode:
 * glyph c is 16 bytes at font[c * 32], one byte per row, most significant bit = leftmost pixel.
 */
void gfx_char(int x, int y, char c, int fg, int bg)
{
	const uint8_t *g = vga_font();
	int row, col;

	if (!g)
		return;
	g += (uint8_t)c * 32;
	for (row = 0; row < GFX_FONT_H; row++) {
		for (col = 0; col < GFX_FONT_W; col++) {
			if (g[row] & (0x80 >> col))
				gfx_putpixel(x + col, y + row, (uint8_t)fg);
			else if (bg >= 0)
				gfx_putpixel(x + col, y + row, (uint8_t)bg);
		}
	}
}

void gfx_text(int x, int y, const char *s, int fg, int bg)
{
	for (; *s; s++, x += GFX_FONT_W)
		gfx_char(x, y, *s, fg, bg);
}

int gfx_text_width(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n * GFX_FONT_W;
}
