#ifndef GUI_H
#define GUI_H

#include <stdint.h>

#include "wm.h"

/* Push-button widget, positioned relative to its window's client area. */
struct gui_button {
	int x, y, w, h;
	const char *label;
	void (*on_click)(void *arg);
	void *arg;
	int down;              /* currently held (mouse pressed on it and not yet released) */
};

void gui_button_draw(const struct gui_button *b, int client_x, int client_y);
int  gui_button_hit(const struct gui_button *b, int client_x, int client_y, int mx, int my);

/*
 * Mouse handling for a set of buttons: call on every pointer update with the window that owns
 * them. A click is "pressed on the button, released on the same button". Returns 1 if a redraw
 * is needed (a button changed state).
 */
int  gui_buttons_mouse(struct gui_button *buttons, int n, struct window *win, int mx, int my,
		       uint8_t mouse_buttons);

#endif
