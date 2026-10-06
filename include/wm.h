#ifndef WM_H
#define WM_H

#include <stdint.h>

/*
 * A very small window manager for the 320x200 graphics screen: a stack of rectangular windows
 * with title bars, drawn back to front, raised on click and dragged by their title bar.
 * Widgets (buttons) are layered on top in gui.c.
 */
#define WM_MAX_WINDOWS 8
#define WM_TITLE_H     14
#define WM_TITLE_LEN   24

struct window {
	int id;
	int x, y, w, h;               /* outer rectangle, title bar included */
	char title[WM_TITLE_LEN];
	uint8_t body;                 /* body colour */
	void (*paint)(struct window *w, int cx, int cy);  /* optional: draws the client area at (cx, cy) */
	void *data;
};

void wm_init(uint8_t desktop_color);
struct window *wm_create(int x, int y, int w, int h, const char *title, uint8_t body);
void wm_destroy(struct window *win);
void wm_raise(struct window *win);
struct window *wm_top(void);
struct window *wm_window_at(int x, int y);        /* topmost window under a point, or NULL */
int  wm_count(void);

/* Feed pointer state (screen pixels, buttons bit 0 = left). Returns 1 if something changed on screen. */
int  wm_mouse(int x, int y, uint8_t buttons);
void wm_render(void);                             /* desktop, all windows back to front, pointer */
struct window *wm_dragging(void);

/* Client area of a window in screen coordinates. */
void wm_client_rect(const struct window *w, int *x, int *y, int *cw, int *ch);

#endif
