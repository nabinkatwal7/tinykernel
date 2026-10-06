#include "gui.h"

#include "gfx.h"

void gui_button_draw(const struct gui_button *b, int cx, int cy)
{
	int x = cx + b->x, y = cy + b->y;
	int tw = gfx_text_width(b->label);

	gfx_fill_rect(x, y, b->w, b->h, 7);
	if (b->down) { /* pressed: dark top/left edge, light bottom/right edge */
		gfx_hline(x, x + b->w - 1, y, 0);
		gfx_vline(x, y, y + b->h - 1, 0);
		gfx_hline(x + 1, x + b->w - 1, y + b->h - 1, 15);
		gfx_vline(x + b->w - 1, y + 1, y + b->h - 1, 15);
	} else {
		gfx_hline(x, x + b->w - 1, y, 15);
		gfx_vline(x, y, y + b->h - 1, 15);
		gfx_hline(x + 1, x + b->w - 1, y + b->h - 1, 0);
		gfx_vline(x + b->w - 1, y + 1, y + b->h - 1, 0);
	}
	gfx_text(x + (b->w - tw) / 2 + b->down, y + (b->h - GFX_FONT_H) / 2 + b->down, b->label, 0, -1);
}

int gui_button_hit(const struct gui_button *b, int cx, int cy, int mx, int my)
{
	return mx >= cx + b->x && mx < cx + b->x + b->w && my >= cy + b->y && my < cy + b->y + b->h;
}

int gui_buttons_mouse(struct gui_button *buttons, int n, struct window *win, int mx, int my, uint8_t mb)
{
	int cx, cy, cw, ch, i, redraw = 0;

	wm_client_rect(win, &cx, &cy, &cw, &ch);
	for (i = 0; i < n; i++) {
		struct gui_button *b = &buttons[i];
		int over = gui_button_hit(b, cx, cy, mx, my) && wm_top() == win;

		if ((mb & 1) && over && !b->down && !wm_dragging()) {
			b->down = 1;           /* pressed on the button */
			redraw = 1;
		} else if (!(mb & 1) && b->down) {
			b->down = 0;           /* released: a click if still over the same button */
			redraw = 1;
			if (over && b->on_click)
				b->on_click(b->arg);
		}
	}
	return redraw;
}
