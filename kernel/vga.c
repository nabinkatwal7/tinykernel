#include "vga.h"

#include "io.h"
#include "klog.h"
#include "kmalloc.h"
#include "kstring.h"

#define VGA_MEM   ((volatile uint8_t *)0xA0000)
#define MISC_W    0x3C2
#define SEQ_I     0x3C4
#define SEQ_D     0x3C5
#define CRTC_I    0x3D4
#define CRTC_D    0x3D5
#define GC_I      0x3CE
#define GC_D      0x3CF
#define AC_I      0x3C0
#define AC_R      0x3C1
#define INSTAT    0x3DA
#define DAC_WIDX  0x3C8
#define DAC_DATA  0x3C9

/* Register images: misc(1) sequencer(5) crtc(25) graphics controller(9) attribute controller(21). */
static const uint8_t mode13h[61] = {
	0x63,
	0x03, 0x01, 0x0F, 0x00, 0x0E,
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
	0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00,
};

static const uint8_t mode03h[61] = {
	0x67,
	0x03, 0x00, 0x03, 0x00, 0x02,
	0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00,
	0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
	0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00,
};

/* The 16 text-mode colours (6-bit DAC values). */
static const uint8_t ega[16][3] = {
	{ 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 },
	{ 42, 21, 0 }, { 42, 42, 42 }, { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
	{ 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
};

static int graphics;
static uint8_t *saved_font; /* plane 2 of text mode: 256 glyphs x 32 bytes */

static void write_regs(const uint8_t *r)
{
	int i;

	outb(MISC_W, r[0]);
	for (i = 0; i < 5; i++) {
		outb(SEQ_I, (uint8_t)i);
		outb(SEQ_D, r[1 + i]);
	}
	/* CRTC registers 0-7 are write-protected until bit 7 of register 0x11 is cleared */
	outb(CRTC_I, 0x03);
	outb(CRTC_D, inb(CRTC_D) | 0x80);
	outb(CRTC_I, 0x11);
	outb(CRTC_D, inb(CRTC_D) & 0x7F);
	for (i = 0; i < 25; i++) {
		uint8_t v = r[6 + i];

		if (i == 0x03)
			v |= 0x80;
		if (i == 0x11)
			v &= 0x7F;
		outb(CRTC_I, (uint8_t)i);
		outb(CRTC_D, v);
	}
	for (i = 0; i < 9; i++) {
		outb(GC_I, (uint8_t)i);
		outb(GC_D, r[31 + i]);
	}
	for (i = 0; i < 21; i++) {
		(void)inb(INSTAT); /* reset the attribute controller's index/data flip-flop */
		outb(AC_I, (uint8_t)i);
		outb(AC_I, r[40 + i]);
	}
	(void)inb(INSTAT);
	outb(AC_I, 0x20); /* re-enable video output */
}

static void set_dac(int index, int r, int g, int b)
{
	outb(DAC_WIDX, (uint8_t)index);
	outb(DAC_DATA, (uint8_t)r);
	outb(DAC_DATA, (uint8_t)g);
	outb(DAC_DATA, (uint8_t)b);
}

/* 0-15 text colours, 16-31 greys, 32-255 a colour cube (6 x 6 x 6 steps) plus extra ramps. */
static void load_palette(void)
{
	int i, r, g, b;

	for (i = 0; i < 16; i++)
		set_dac(i, ega[i][0], ega[i][1], ega[i][2]);
	for (i = 0; i < 16; i++) {
		int v = i * 63 / 15;

		set_dac(16 + i, v, v, v);
	}
	for (i = 0; i < 216; i++) {
		r = i / 36;
		g = i / 6 % 6;
		b = i % 6;
		set_dac(32 + i, r * 63 / 5, g * 63 / 5, b * 63 / 5);
	}
	for (i = 248; i < 256; i++)
		set_dac(i, 0, 0, 0);
}

/* The text-mode colours live in specific DAC slots (see the attribute controller palette). */
static void load_text_palette(void)
{
	static const uint8_t slot[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
	int i;

	for (i = 0; i < 16; i++)
		set_dac(slot[i], ega[i][0], ega[i][1], ega[i][2]);
}

/* Plane 2 holds the font; reach it with the CPU by temporarily switching to a planar view. */
static void font_access(int on, uint8_t saved[4])
{
	if (on) {
		outb(SEQ_I, 2); saved[0] = inb(SEQ_D);
		outb(SEQ_I, 4); saved[1] = inb(SEQ_D);
		outb(GC_I, 4);  saved[2] = inb(GC_D);
		outb(GC_I, 5);  saved[3] = inb(GC_D);
		outb(SEQ_I, 2); outb(SEQ_D, 0x04);          /* writes go to plane 2 only */
		outb(SEQ_I, 4); outb(SEQ_D, 0x07);          /* sequential addressing, no chain-4 */
		outb(GC_I, 4);  outb(GC_D, 0x02);           /* reads come from plane 2 */
		outb(GC_I, 5);  outb(GC_D, 0x00);           /* write mode 0, no odd/even */
		outb(GC_I, 6);  outb(GC_D, 0x00);           /* map the 128 KiB window at 0xA0000 */
	} else {
		outb(SEQ_I, 2); outb(SEQ_D, saved[0]);
		outb(SEQ_I, 4); outb(SEQ_D, saved[1]);
		outb(GC_I, 4);  outb(GC_D, saved[2]);
		outb(GC_I, 5);  outb(GC_D, saved[3]);
		outb(GC_I, 6);  outb(GC_D, 0x0E);           /* text window at 0xB8000 */
	}
}

static void save_font(void)
{
	uint8_t s[4];
	int i;

	if (!saved_font)
		saved_font = kmalloc(8192);
	if (!saved_font)
		return;
	font_access(1, s);
	for (i = 0; i < 8192; i++)
		saved_font[i] = VGA_MEM[i];
	font_access(0, s);
}

static void restore_font(void)
{
	uint8_t s[4];
	int i;

	if (!saved_font)
		return;
	font_access(1, s);
	for (i = 0; i < 8192; i++)
		VGA_MEM[i] = saved_font[i];
	font_access(0, s);
}

int vga_set_graphics(void)
{
	int i;

	if (graphics)
		return 0;
	save_font();
	if (!saved_font)
		return -1;
	write_regs(mode13h);
	load_palette();
	for (i = 0; i < VGA_GFX_W * VGA_GFX_H; i++)
		VGA_MEM[i] = 0;
	graphics = 1;
	return 0;
}

void vga_set_text(void)
{
	if (!graphics)
		return;
	write_regs(mode03h);
	restore_font();
	load_text_palette();
	graphics = 0;
}

int vga_in_graphics(void)
{
	return graphics;
}

uint8_t *vga_framebuffer(void)
{
	return (uint8_t *)VGA_MEM;
}

void vga_fill(uint8_t color)
{
	int i;

	for (i = 0; i < VGA_GFX_W * VGA_GFX_H; i++)
		VGA_MEM[i] = color;
}

void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	set_dac(index, r >> 2, g >> 2, b >> 2); /* callers use 8-bit components; the DAC has 6 bits */
}
