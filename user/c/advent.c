/* A tiny text adventure: find the key, unlock the vault, bring the lost kernel back to the boot sector. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

enum { N, S, E, W, NDIR };
enum { BOOT, HALL, LIBRARY, SERVERS, VAULT, NROOMS };
enum { KEY, LAMP, KERNEL, NITEMS };

static const char *const room_name[NROOMS] = { "Boot Sector", "Hallway", "Library", "Server Room", "Vault" };
static const char *const room_text[NROOMS] = {
	"A cold, quiet room. A faded sign reads: BRING BACK THE KERNEL.",
	"A long corridor. Doors lead to the library (west) and the server room (east).",
	"Shelves of dusty manuals. Something small glints between two of them.",
	"Racks of blinking machines. It is very dark in the corner.",
	"A heavy steel vault, lit by one flickering bulb.",
};
/* exits[room][dir] = room or -1 */
static const int exits[NROOMS][NDIR] = {
	/* N     S     E     W */
	{ HALL, -1, -1, -1 },        /* boot */
	{ -1, BOOT, SERVERS, LIBRARY },
	{ -1, -1, HALL, -1 },
	{ VAULT, -1, -1, HALL },
	{ -1, SERVERS, -1, -1 },
};
static const char *const item_name[NITEMS] = { "key", "lamp", "kernel" };
static int item_room[NITEMS] = { LIBRARY, BOOT, VAULT }; /* -2 = carried */
static int vault_open, moves, room = BOOT;

static int has(int item)
{
	return item_room[item] == -2;
}

static void look(void)
{
	int i;

	printf("\n== %s ==\n%s\n", room_name[room], room_text[room]);
	if (room == SERVERS && !has(LAMP))
		puts("You can barely see; a lamp would help.");
	if (room == SERVERS && !vault_open)
		puts("A steel door to the north is locked.");
	for (i = 0; i < NITEMS; i++) {
		if (item_room[i] == room && !(i == KERNEL && !vault_open)) {
			printf("There is a %s here.\n", item_name[i]);
		}
	}
	printf("Exits:");
	for (i = 0; i < NDIR; i++)
		if (exits[room][i] >= 0)
			printf(" %c", "nsew"[i]);
	puts("");
}

static int find_item(const char *name)
{
	int i;

	for (i = 0; i < NITEMS; i++)
		if (!strcmp(name, item_name[i]))
			return i;
	return -1;
}

static void go(int dir)
{
	int to = exits[room][dir];

	if (to < 0) {
		puts("You can't go that way.");
		return;
	}
	if (room == SERVERS && to == VAULT && !vault_open) {
		puts("The vault door is locked. (try: use key)");
		return;
	}
	if (to == SERVERS && !has(LAMP) && room == HALL)
		puts("It is pitch dark in there, but you feel your way in.");
	room = to;
	moves++;
	look();
	if (room == BOOT && has(KERNEL)) {
		printf("\nYou place the kernel on the boot sector. The machine hums to life!\n"
		       "You win in %d moves.\n", moves);
		exit(0);
	}
}

int main(void)
{
	char line[64];
	int n;

	puts("THE LOST KERNEL  (commands: n s e w, look, take X, drop X, use X, inv, help, quit)");
	look();
	for (;;) {
		char *arg;
		int it;

		printf("\n> ");
		n = read(0, line, sizeof line - 1);
		if (n <= 0)
			break;
		line[n] = 0;
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
			line[--n] = 0;
		arg = strchr(line, ' ');
		if (arg)
			*arg++ = 0;
		if (!strcmp(line, "n") || !strcmp(line, "north")) {
			go(N);
		} else if (!strcmp(line, "s") || !strcmp(line, "south")) {
			go(S);
		} else if (!strcmp(line, "e") || !strcmp(line, "east")) {
			go(E);
		} else if (!strcmp(line, "w") || !strcmp(line, "west")) {
			go(W);
		} else if (!strcmp(line, "look") || !strcmp(line, "l")) {
			look();
		} else if (!strcmp(line, "inv") || !strcmp(line, "i")) {
			int i, any = 0;

			printf("You carry:");
			for (i = 0; i < NITEMS; i++)
				if (has(i)) {
					printf(" %s", item_name[i]);
					any = 1;
				}
			puts(any ? "" : " nothing");
		} else if (!strcmp(line, "take") && arg) {
			it = find_item(arg);
			if (it < 0 || item_room[it] != room || (it == KERNEL && !vault_open))
				puts("There is no such thing here.");
			else {
				item_room[it] = -2;
				puts("Taken.");
			}
		} else if (!strcmp(line, "drop") && arg) {
			it = find_item(arg);
			if (it < 0 || !has(it)) {
				puts("You don't have that.");
			} else {
				item_room[it] = room;
				puts("Dropped.");
			}
		} else if (!strcmp(line, "use") && arg) {
			it = find_item(arg);
			if (it == KEY && has(KEY) && room == SERVERS) {
				vault_open = 1;
				puts("The key turns with a clunk. The vault door to the north swings open!");
			} else if (it == KEY && has(KEY)) {
				puts("There is nothing to unlock here.");
			} else {
				puts("Nothing happens.");
			}
		} else if (!strcmp(line, "help")) {
			puts("Move with n/s/e/w. Pick things up with 'take', and bring the kernel back to the Boot Sector.");
		} else if (!strcmp(line, "quit") || !strcmp(line, "q")) {
			puts("Goodbye.");
			return 0;
		} else if (*line) {
			puts("I don't understand.");
		}
	}
	return 0;
}
