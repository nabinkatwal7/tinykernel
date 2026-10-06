/* Snake in text mode. Arrow keys or WASD steer, q quits. Uses the screen directly (putat) and polls the keyboard. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

#define W 40
#define H 20
#define X0 20 /* the board is drawn at columns 20.. and rows 2.. */
#define Y0 2
#define MAX_LEN (W * H)

static int sx[MAX_LEN], sy[MAX_LEN];
static int len, dx, dy, fx, fy, score;
static unsigned rng = 12345;

static int rnd(int n)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (int)(rng % (unsigned)n);
}

static void text(int x, int y, const char *s, int attr)
{
	for (; *s; s++)
		putat(x++, y, *s, attr);
}

static int on_snake(int x, int y)
{
	int i;

	for (i = 0; i < len; i++)
		if (sx[i] == x && sy[i] == y)
			return 1;
	return 0;
}

static void place_food(void)
{
	do {
		fx = rnd(W);
		fy = rnd(H);
	} while (on_snake(fx, fy));
	putat(X0 + fx, Y0 + fy, '*', 0x0C);
}

static void show_score(void)
{
	char buf[32];

	snprintf(buf, sizeof buf, "Score: %d ", score);
	text(X0, Y0 + H + 1, buf, 0x0F);
}

static void draw_frame(void)
{
	int i;

	for (i = -1; i <= W; i++) {
		putat(X0 + i, Y0 - 1, '#', 0x07);
		putat(X0 + i, Y0 + H, '#', 0x07);
	}
	for (i = 0; i < H; i++) {
		putat(X0 - 1, Y0 + i, '#', 0x07);
		putat(X0 + W, Y0 + i, '#', 0x07);
	}
	text(X0 + 14, Y0 + H + 1, "SNAKE: arrows/WASD steer, q quits", 0x0E);
}

int main(void)
{
	int i, over = 0, step = 0;

	cls();
	rng ^= (unsigned)ticks() * 2654435761u;
	draw_frame();
	len = 3;
	dx = 1;
	dy = 0;
	for (i = 0; i < len; i++) {
		sx[i] = W / 2 - i;
		sy[i] = H / 2;
		putat(X0 + sx[i], Y0 + sy[i], i ? 'o' : '@', 0x0A);
	}
	place_food();
	show_score();

	while (!over) {
		int k, nx, ny;

		while ((k = trykey()) >= 0) {
			if ((k == KEY_UP || k == 'w') && dy == 0) {
				dx = 0;
				dy = -1;
			} else if ((k == KEY_DOWN || k == 's') && dy == 0) {
				dx = 0;
				dy = 1;
			} else if ((k == KEY_LEFT || k == 'a') && dx == 0) {
				dx = -1;
				dy = 0;
			} else if ((k == KEY_RIGHT || k == 'd') && dx == 0) {
				dx = 1;
				dy = 0;
			} else if (k == 'q') {
				cls();
				return 0;
			}
		}
		nx = sx[0] + dx;
		ny = sy[0] + dy;
		if (nx < 0 || nx >= W || ny < 0 || ny >= H || on_snake(nx, ny)) {
			over = 1;
			break;
		}
		if (nx == fx && ny == fy) { /* grow: the tail stays where it is */
			len++;
			score += 10;
			for (i = len - 1; i > 0; i--) {
				sx[i] = sx[i - 1];
				sy[i] = sy[i - 1];
			}
			sx[0] = nx;
			sy[0] = ny;
			putat(X0 + sx[1], Y0 + sy[1], 'o', 0x0A);
			putat(X0 + nx, Y0 + ny, '@', 0x0A);
			place_food();
			show_score();
		} else {
			putat(X0 + sx[len - 1], Y0 + sy[len - 1], ' ', 0x07); /* erase the tail */
			for (i = len - 1; i > 0; i--) {
				sx[i] = sx[i - 1];
				sy[i] = sy[i - 1];
			}
			sx[0] = nx;
			sy[0] = ny;
			putat(X0 + sx[1], Y0 + sy[1], 'o', 0x0A);
			putat(X0 + nx, Y0 + ny, '@', 0x0A);
		}
		step++;
		sleep_ms(score > 100 ? 60 : score > 50 ? 80 : 110); /* faster as you grow */
	}
	text(X0 + W / 2 - 5, Y0 + H / 2, " GAME OVER ", 0x4F);
	text(X0, Y0 + H + 2, "Press any key.", 0x07);
	while (trykey() >= 0)
		;
	getkey();
	cls();
	printf("snake: final score %d after %d moves\n", score, step);
	return 0;
}
