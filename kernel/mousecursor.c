#include "console.h"
#include "mouse.h"

/*
 * Text-mode mouse pointer: the cell under the pointer is shown in reverse video. The mouse
 * surface is 640x400 with 8x16-pixel cells, which maps one-to-one onto the 80x25 screen.
 */
static int shown;            /* is a highlighted cell currently on screen? */
static int cx, cy;
static uint8_t saved, hilite;
static int enabled = 1;

static void restore(void)
{
	/* only undo our own highlight: if text was printed there meanwhile, leave it alone */
	if (shown && console_get_attr(cx, cy) == hilite)
		console_set_attr(cx, cy, saved);
	shown = 0;
}

static void on_mouse(void)
{
	struct mouse_state m;
	int x, y;

	mouse_get(&m);
	x = m.x / 8;
	y = m.y / 16;
	restore();
	if (!enabled)
		return;
	cx = x;
	cy = y;
	saved = console_get_attr(x, y);
	hilite = (uint8_t)((saved << 4) | (saved >> 4)); /* swap foreground and background */
	if ((hilite & 0x0F) == (hilite >> 4))
		hilite = 0x70;
	console_set_attr(x, y, hilite);
	shown = 1;
}

void mousecursor_init(void)
{
	mouse_set_hook(on_mouse); /* the pointer appears at the first movement */
}

void mousecursor_enable(int on)
{
	enabled = on;
	if (!on)
		restore();
	else if (mouse_present())
		on_mouse();
}
