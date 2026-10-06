#include "console.h"
#include "gfx.h"
#include "kstring.h"
#include "vga.h"

/*
 * Graphical console: the same character stream as the text console, drawn with the bitmap font
 * on the 320x200 framebuffer as a 40 x 12 grid of 8x16 cells. console.c hands every character
 * to gfxcon_putchar() while this is enabled.
 */
#define COLS (VGA_GFX_W / GFX_FONT_W)   /* 40 */
#define ROWS (VGA_GFX_H / GFX_FONT_H)   /* 12 */

static int enabled;
static int cx, cy;
static uint8_t cur_attr = 0x0F;       /* foreground in the low nibble, background in the high one */
static int cursor_drawn;

int gfxcon_active(void)
{
	return enabled;
}

static void cursor(int on)
{
	/* an underline under the next cell to be written */
	gfx_hline(cx * GFX_FONT_W, cx * GFX_FONT_W + GFX_FONT_W - 1, cy * GFX_FONT_H + GFX_FONT_H - 1,
		  on ? 15 : (uint8_t)(cur_attr >> 4));
	cursor_drawn = on;
}

static void scroll(void)
{
	uint8_t *fb = vga_framebuffer();
	int bytes = (ROWS - 1) * GFX_FONT_H * VGA_GFX_W;

	memmove(fb, fb + GFX_FONT_H * VGA_GFX_W, (size_t)bytes);
	gfx_fill_rect(0, (ROWS - 1) * GFX_FONT_H, VGA_GFX_W, GFX_FONT_H, (uint8_t)(cur_attr >> 4));
}

static void newline(void)
{
	cx = 0;
	if (++cy >= ROWS) {
		scroll();
		cy = ROWS - 1;
	}
}

void gfxcon_clear(void)
{
	gfx_fill_rect(0, 0, VGA_GFX_W, VGA_GFX_H, (uint8_t)(cur_attr >> 4));
	cx = cy = 0;
	cursor(1);
}

void gfxcon_set_attr(uint8_t attr)
{
	cur_attr = attr;
}

void gfxcon_putchar(char c)
{
	if (cursor_drawn)
		cursor(0);
	switch (c) {
	case '\n':
		newline();
		break;
	case '\r':
		cx = 0;
		break;
	case '\b':
		if (cx > 0) {
			cx--;
		} else if (cy > 0) {
			cy--;
			cx = COLS - 1;
		}
		gfx_char(cx * GFX_FONT_W, cy * GFX_FONT_H, ' ', cur_attr & 0x0F, cur_attr >> 4);
		break;
	case '\t':
		do
			gfxcon_putchar(' ');
		while (cx % 4);
		return;
	default:
		gfx_char(cx * GFX_FONT_W, cy * GFX_FONT_H, c, cur_attr & 0x0F, cur_attr >> 4);
		if (++cx >= COLS)
			newline();
		break;
	}
	cursor(1);
}

int gfxcon_enable(int on)
{
	if (on) {
		if (vga_set_graphics())
			return -1;
		enabled = 1;
		gfxcon_clear();
	} else if (enabled) {
		enabled = 0;
		vga_set_text();
	}
	return 0;
}
