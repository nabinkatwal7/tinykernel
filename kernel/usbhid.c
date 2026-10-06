#include "usbhid.h"

#include "console.h"
#include "kstring.h"

/* HID usage ID -> character, without and with Shift (0 = not a character key). Usages 4.. are a-z, then digits. */
static char usage_char(uint8_t u, int shift)
{
	static const char plain[] = "1234567890";
	static const char shifted[] = "!@#$%^&*()";
	static const char punct[]  = "\n\x1b\b\t -=[]\\#;'`,./"; /* usages 40..56 */
	static const char punct_s[] = "\n\x1b\b\t _+{}|~:\"~<>?";

	if (u >= 4 && u <= 29)
		return (char)((shift ? 'A' : 'a') + (u - 4));
	if (u >= 30 && u <= 39)
		return shift ? shifted[u - 30] : plain[u - 30];
	if (u >= 40 && u <= 56)
		return shift ? punct_s[u - 40] : punct[u - 40];
	return 0;
}

static uint8_t last[8];

void usbkbd_reset(void)
{
	memset(last, 0, sizeof last);
}

/*
 * Compare a boot report with the previous one and call emit(key) for every newly pressed key:
 * the character, or one of the KEY_* values for arrows. Keys still held do not repeat.
 */
void usbkbd_report(const uint8_t r[8], void (*emit)(int key))
{
	int shift = (r[0] & 0x22) != 0; /* either Shift */
	int i, j;

	if (r[2] == 1) /* rollover error: ignore the report */
		return;
	for (i = 2; i < 8; i++) {
		int was = 0, key;

		if (!r[i])
			continue;
		for (j = 2; j < 8; j++)
			was |= last[j] == r[i];
		if (was)
			continue;
		switch (r[i]) {
		case 79: key = USBKBD_RIGHT; break;
		case 80: key = USBKBD_LEFT; break;
		case 81: key = USBKBD_DOWN; break;
		case 82: key = USBKBD_UP; break;
		default: key = usage_char(r[i], shift); break;
		}
		if (key)
			emit(key);
	}
	memcpy(last, r, 8);
}

static char got[32];
static int ngot;

static void collect(int key)
{
	if (ngot < (int)sizeof got - 1)
		got[ngot++] = key < 256 ? (char)key : '?';
}

/* usbkbd test : feed sample boot-protocol reports and check the keys that come out */
int cmd_usbkbd(int argc, char **argv)
{
	static const uint8_t a_down[8]   = { 0, 0, 4, 0, 0, 0, 0, 0 };
	static const uint8_t none[8]     = { 0 };
	static const uint8_t shift_a[8]  = { 0x02, 0, 4, 0, 0, 0, 0, 0 };
	static const uint8_t two_keys[8] = { 0, 0, 5, 6, 0, 0, 0, 0 };   /* b and c together */
	static const uint8_t held[8]     = { 0, 0, 5, 6, 7, 0, 0, 0 };   /* b, c still down, d added */
	static const uint8_t digits[8]   = { 0, 0, 30, 39, 0, 0, 0, 0 }; /* 1 and 0 */
	static const uint8_t arrow[8]    = { 0, 0, 82, 0, 0, 0, 0, 0 };
	static const uint8_t rollover[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
	int bad = 0;

	(void)argc;
	(void)argv;
	usbkbd_reset();
	ngot = 0;
	usbkbd_report(a_down, collect);
	usbkbd_report(a_down, collect);   /* still held: no repeat */
	usbkbd_report(none, collect);
	usbkbd_report(shift_a, collect);
	usbkbd_report(none, collect);
	usbkbd_report(two_keys, collect);
	usbkbd_report(held, collect);
	usbkbd_report(none, collect);
	usbkbd_report(digits, collect);
	usbkbd_report(rollover, collect);
	usbkbd_report(arrow, collect);
	got[ngot] = '\0';
	console_printf("decoded: \"%s\" (%d keys)\n", got, ngot);
	bad += kstrncmp(got, "aAbcd10", 7) != 0 || ngot != 8 || got[7] != '?';
	console_printf("usbkbd test: %s\n", bad ? "FAILED" : "ok");
	return bad != 0;
}
