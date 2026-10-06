#include "wm.h"

#include "gfx.h"
#include "kstring.h"
#include "vga.h"

static struct window store[WM_MAX_WINDOWS];
static struct window *order[WM_MAX_WINDOWS]; /* back to front */
static int count, next_id = 1;
static uint8_t desktop;

static struct window *dragging;
static int drag_dx, drag_dy;
static int ptr_x, ptr_y;
static uint8_t last_buttons;

void wm_init(uint8_t desktop_color)
{
	memset(store, 0, sizeof store);
	count = 0;
	next_id = 1;
	desktop = desktop_color;
	dragging = 0;
	last_buttons = 0;
}

struct window *wm_create(int x, int y, int w, int h, const char *title, uint8_t body)
{
	struct window *win = 0;
	int i;

	for (i = 0; i < WM_MAX_WINDOWS && !win; i++)
		if (!store[i].id)
			win = &store[i];
	if (!win)
		return 0;
	memset(win, 0, sizeof *win);
	win->id = next_id++;
	win->x = x;
	win->y = y;
	win->w = w;
	win->h = h;
	win->body = body;
	kstrlcpy(win->title, title, sizeof win->title);
	order[count++] = win; /* new windows start on top */
	return win;
}

void wm_destroy(struct window *win)
{
	int i, j;

	for (i = 0; i < count; i++) {
		if (order[i] == win) {
			for (j = i; j + 1 < count; j++)
				order[j] = order[j + 1];
			count--;
			break;
		}
	}
	if (dragging == win)
		dragging = 0;
	win->id = 0;
}

void wm_raise(struct window *win)
{
	int i, j;

	for (i = 0; i < count; i++) {
		if (order[i] == win) {
			for (j = i; j + 1 < count; j++)
				order[j] = order[j + 1];
			order[count - 1] = win;
			return;
		}
	}
}

struct window *wm_top(void)
{
	return count ? order[count - 1] : 0;
}

int wm_count(void)
{
	return count;
}

struct window *wm_dragging(void)
{
	return dragging;
}

struct window *wm_window_at(int x, int y)
{
	int i;

	for (i = count - 1; i >= 0; i--) {
		struct window *w = order[i];

		if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
			return w;
	}
	return 0;
}

void wm_client_rect(const struct window *w, int *x, int *y, int *cw, int *ch)
{
	*x = w->x + 1;
	*y = w->y + WM_TITLE_H;
	*cw = w->w - 2;
	*ch = w->h - WM_TITLE_H - 1;
}

int wm_mouse(int x, int y, uint8_t buttons)
{
	int changed = x != ptr_x || y != ptr_y;
	int pressed = (buttons & 1) && !(last_buttons & 1);

	ptr_x = x;
	ptr_y = y;
	if (pressed) {
		struct window *w = wm_window_at(x, y);

		if (w) {
			if (w != wm_top()) {
				wm_raise(w);
				changed = 1;
			}
			if (y < w->y + WM_TITLE_H) { /* grabbed the title bar */
				dragging = w;
				drag_dx = x - w->x;
				drag_dy = y - w->y;
			}
		}
	}
	if (!(buttons & 1))
		dragging = 0;
	if (dragging) {
		int nx = x - drag_dx, ny = y - drag_dy;

		if (nx < -dragging->w + 20)
			nx = -dragging->w + 20; /* keep a handle on screen */
		if (nx > VGA_GFX_W - 20)
			nx = VGA_GFX_W - 20;
		if (ny < 0)
			ny = 0;
		if (ny > VGA_GFX_H - WM_TITLE_H)
			ny = VGA_GFX_H - WM_TITLE_H;
		if (nx != dragging->x || ny != dragging->y) {
			dragging->x = nx;
			dragging->y = ny;
			changed = 1;
		}
	}
	if ((buttons & 1) != (last_buttons & 1))
		changed = 1;
	last_buttons = buttons;
	return changed;
}

static void draw_window(struct window *w, int active)
{
	int cx, cy, cw, ch;

	gfx_fill_rect(w->x, w->y, w->w, w->h, w->body);
	gfx_rect(w->x, w->y, w->w, w->h, 0);
	gfx_fill_rect(w->x + 1, w->y + 1, w->w - 2, WM_TITLE_H - 2, active ? 1 : 8);
	gfx_text(w->x + 4, w->y - 1, w->title, 15, -1);
	gfx_hline(w->x, w->x + w->w - 1, w->y + WM_TITLE_H - 1, 0);
	if (w->paint) {
		wm_client_rect(w, &cx, &cy, &cw, &ch);
		w->paint(w, cx, cy);
	}
}

/* A classic arrow, 8 wide x 12 tall: 1 = black outline, 2 = white fill. */
static const char *const arrow[12] = {
	"1.......", "11......", "121.....", "1221....", "12221...", "122221..",
	"1222221.", "12222221", "122221..", "12121...", "1..121..", "....11..",
};

static void draw_pointer(void)
{
	int r, c;

	for (r = 0; r < 12; r++)
		for (c = 0; c < 8; c++)
			if (arrow[r][c] == '1')
				gfx_putpixel(ptr_x + c, ptr_y + r, 0);
			else if (arrow[r][c] == '2')
				gfx_putpixel(ptr_x + c, ptr_y + r, 15);
}

void wm_render(void)
{
	int i;

	vga_fill(desktop);
	for (i = 0; i < count; i++)
		draw_window(order[i], i == count - 1);
	draw_pointer();
}
