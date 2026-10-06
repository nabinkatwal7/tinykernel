/* Tetris on the 320x200 graphics screen. Left/Right move, Up rotates, Down drops one row, Space drops all the way, q quits. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

#define COLS 10
#define ROWS 20
#define CELL 8
#define BX 120 /* board origin on screen */
#define BY 20

/* Each piece as a 4x4 bitmap in its spawn orientation, row by row. */
static const char *const shapes[7] = {
	"....1111........", /* I */
	".11..11.........", /* O */
	".1..111.........", /* T */
	".11.11..........", /* S */
	"11...11.........", /* Z */
	"1...111.........", /* J */
	"..1.111.........", /* L */
};
static const int colors[7] = { 11, 14, 13, 10, 12, 9, 6 };

static unsigned char board[ROWS][COLS]; /* 0 empty, else color */
static int piece[4][4], pcolor, px, py, score, lines, level = 1;
static unsigned rng = 2463534242u;

static int rnd(int n)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (int)(rng % (unsigned)n);
}

static int fits(int p[4][4], int x, int y)
{
	int r, c;

	for (r = 0; r < 4; r++)
		for (c = 0; c < 4; c++)
			if (p[r][c]) {
				int bx = x + c, by = y + r;

				if (bx < 0 || bx >= COLS || by >= ROWS || (by >= 0 && board[by][bx]))
					return 0;
			}
	return 1;
}

static void rotate(int p[4][4], int out[4][4])
{
	int r, c;

	for (r = 0; r < 4; r++)
		for (c = 0; c < 4; c++)
			out[c][3 - r] = p[r][c];
}

static int new_piece(void)
{
	int k = rnd(7), r, c;

	for (r = 0; r < 4; r++)
		for (c = 0; c < 4; c++)
			piece[r][c] = shapes[k][r * 4 + c] == '1';
	pcolor = colors[k];
	px = COLS / 2 - 2;
	py = 0;
	return fits(piece, px, py);
}

static void cell(int x, int y, int color)
{
	gfx_rect(BX + x * CELL, BY + y * CELL, CELL - 1, CELL - 1, color);
}

static void draw(void)
{
	char buf[32];
	int r, c;

	for (r = 0; r < ROWS; r++)
		for (c = 0; c < COLS; c++)
			cell(c, r, board[r][c]);
	for (r = 0; r < 4; r++)
		for (c = 0; c < 4; c++)
			if (piece[r][c] && py + r >= 0)
				cell(px + c, py + r, pcolor);
	snprintf(buf, sizeof buf, "Score %d   ", score);
	gfx_text(8, 20, 15, 0, buf);
	snprintf(buf, sizeof buf, "Lines %d   ", lines);
	gfx_text(8, 40, 15, 0, buf);
	snprintf(buf, sizeof buf, "Level %d  ", level);
	gfx_text(8, 60, 15, 0, buf);
}

static void lock_piece(void)
{
	int r, c, y, cleared = 0;

	for (r = 0; r < 4; r++)
		for (c = 0; c < 4; c++)
			if (piece[r][c] && py + r >= 0)
				board[py + r][px + c] = (unsigned char)pcolor;
	for (y = ROWS - 1; y >= 0; y--) {
		int full = 1;

		for (c = 0; c < COLS; c++)
			if (!board[y][c])
				full = 0;
		if (full) {
			int yy;

			for (yy = y; yy > 0; yy--)
				memcpy(board[yy], board[yy - 1], COLS);
			memset(board[0], 0, COLS);
			cleared++;
			y++; /* look at the same row again */
		}
	}
	if (cleared) {
		static const int pts[5] = { 0, 40, 100, 300, 1200 };

		score += pts[cleared] * level;
		lines += cleared;
		level = 1 + lines / 10;
	}
}

int main(void)
{
	unsigned last_drop;
	int dirty = 1, over = 0;

	rng ^= (unsigned)ticks() * 2654435761u;
	if (gfx_enter()) {
		puts("tetris: cannot switch to graphics mode (is 'gfxmode' on?)");
		return 1;
	}
	gfx_clear(0);
	gfx_rect(BX - 2, BY - 2, COLS * CELL + 3, ROWS * CELL + 3, 8);
	gfx_text(8, 100, 7, 0, "<- -> move");
	gfx_text(8, 116, 7, 0, "up: rotate");
	gfx_text(8, 132, 7, 0, "space: drop");
	gfx_text(8, 148, 7, 0, "q: quit");
	new_piece();
	last_drop = (unsigned)ticks();

	while (!over) {
		int k, moved = 0;
		unsigned now = (unsigned)ticks();

		while ((k = trykey()) >= 0) {
			int t[4][4];

			if (k == KEY_LEFT && fits(piece, px - 1, py)) {
				px--;
				moved = 1;
			} else if (k == KEY_RIGHT && fits(piece, px + 1, py)) {
				px++;
				moved = 1;
			} else if (k == KEY_UP) {
				rotate(piece, t);
				if (fits(t, px, py)) {
					memcpy(piece, t, sizeof piece);
					moved = 1;
				}
			} else if (k == KEY_DOWN && fits(piece, px, py + 1)) {
				py++;
				score++;
				moved = 1;
			} else if (k == ' ') {
				while (fits(piece, px, py + 1)) {
					py++;
					score += 2;
				}
				last_drop = 0; /* lock right away */
				moved = 1;
			} else if (k == 'q') {
				gfx_leave();
				printf("tetris: score %d, %d lines\n", score, lines);
				return 0;
			}
		}
		if (now - last_drop >= (unsigned)(60 - (level > 9 ? 9 : level) * 5)) { /* gravity: ticks per row */
			last_drop = now;
			if (fits(piece, px, py + 1)) {
				py++;
			} else {
				lock_piece();
				if (!new_piece())
					over = 1;
			}
			moved = 1;
		}
		if (moved || dirty) {
			draw();
			dirty = 0;
		}
		sleep_ms(10);
	}
	gfx_text(BX + 8, BY + 70, 15, 4, " GAME OVER ");
	sleep_ms(300);
	while (trykey() >= 0)
		;
	getkey();
	gfx_leave();
	printf("tetris: game over, score %d, %d lines\n", score, lines);
	return 0;
}
