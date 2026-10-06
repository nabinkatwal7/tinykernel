#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_GFX_W 320
#define VGA_GFX_H 200

/* Mode switching without the BIOS: we program the VGA registers ourselves. */
int      vga_set_graphics(void);        /* 320x200, 256 colours at 0xA0000; 0 on success */
void     vga_set_text(void);            /* back to 80x25 text (font and palette restored) */
int      vga_in_graphics(void);
uint8_t *vga_framebuffer(void);         /* VGA_GFX_W * VGA_GFX_H bytes, one palette index per pixel */
void     vga_fill(uint8_t color);
void     vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b); /* 8-bit components */

#endif
