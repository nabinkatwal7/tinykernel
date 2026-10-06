#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

#define MOUSE_MAX_X  639   /* virtual surface: 640x400, 8x16 pixels per text cell */
#define MOUSE_MAX_Y  399

struct mouse_state {
	int x, y;            /* position, clamped to the surface */
	uint8_t buttons;
	uint32_t packets;    /* complete packets decoded so far */
	uint32_t resyncs;    /* bytes dropped to regain packet alignment */
};

int  mouse_init(void);                         /* 0 if a mouse answered */
int  mouse_present(void);
void mouse_get(struct mouse_state *out);
void mouse_feed(uint8_t byte);                 /* called by the 8042 interrupt code for aux bytes */
void mousecursor_init(void);                   /* draw a reverse-video pointer in text mode */
void mousecursor_enable(int on);
void mouse_set_hook(void (*hook)(void));       /* called after every decoded packet (IRQ context) */

#endif
