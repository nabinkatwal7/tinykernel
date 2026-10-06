#include "keyboard.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_OBF    0x01 /* output buffer full */

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_BREAK  0x80

static const char map_normal[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

static const char map_shift[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static int shift;

static unsigned char inb(unsigned short port)
{
	unsigned char v;

	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

/* Polls the controller; returns raw scancode including break bit. */
static unsigned char read_raw(void)
{
	while (!(inb(KBD_STATUS) & KBD_OBF))
		;
	return inb(KBD_DATA);
}

unsigned char keyboard_read_scancode(void)
{
	unsigned char sc;

	do {
		sc = read_raw();
	} while (sc & SC_BREAK);
	return sc;
}

char keyboard_getchar(void)
{
	for (;;) {
		unsigned char sc = read_raw();
		unsigned char code = sc & ~SC_BREAK;
		char c;

		if (code == SC_LSHIFT || code == SC_RSHIFT) {
			shift = !(sc & SC_BREAK);
			continue;
		}
		if (sc & SC_BREAK)
			continue;

		/* ponytail: extended (0xE0) keys fall outside the map and are ignored */
		c = (code < sizeof map_normal) ? (shift ? map_shift : map_normal)[code] : 0;
		if (c)
			return c;
	}
}
