#include "sb.h"

#include "console.h"
#include "io.h"
#include "kstring.h"
#include "sched.h"

/*
 * Sound Blaster (SB16 / SB Pro class, ISA). The card sits at an I/O base of 0x220, 0x240, ... 0x280 and talks through
 * the DSP: a reset sequence on base+6 makes it answer 0xAA on base+0xA; commands go to base+0xC and replies come back
 * on base+0xA (bit 7 of base+0xE says one is waiting).
 */
#define DSP_RESET  0x6
#define DSP_READ   0xA
#define DSP_WRITE  0xC
#define DSP_STATUS 0xE   /* read: bit 7 = data available (also acknowledges an 8-bit interrupt) */

static uint16_t base;
static int major, minor;

static int dsp_wait_read(void)
{
	int spin = 100000;

	while (spin--)
		if (inb((uint16_t)(base + DSP_STATUS)) & 0x80)
			return 0;
	return -1;
}

static int dsp_wait_write(void)
{
	int spin = 100000;

	while (spin--)
		if (!(inb((uint16_t)(base + DSP_WRITE)) & 0x80))
			return 0;
	return -1;
}

static int dsp_write(uint8_t v)
{
	if (dsp_wait_write())
		return -1;
	outb((uint16_t)(base + DSP_WRITE), v);
	return 0;
}

static int dsp_read(void)
{
	if (dsp_wait_read())
		return -1;
	return inb((uint16_t)(base + DSP_READ));
}

/* Reset the DSP at the given base: 1 if a Sound Blaster answered. */
static int dsp_reset(uint16_t port)
{
	int i;

	outb((uint16_t)(port + DSP_RESET), 1);
	for (i = 0; i < 100; i++)
		(void)inb((uint16_t)(port + DSP_RESET)); /* about three microseconds */
	outb((uint16_t)(port + DSP_RESET), 0);
	base = port;
	for (i = 0; i < 1000; i++) {
		if (dsp_wait_read())
			continue;
		return inb((uint16_t)(port + DSP_READ)) == 0xAA;
	}
	return 0;
}

int sb_detect(void)
{
	uint16_t port;

	base = 0;
	for (port = 0x220; port <= 0x280; port += 0x10) {
		if (dsp_reset(port)) {
			base = port;
			if (dsp_write(0xE1) == 0) { /* get DSP version */
				major = dsp_read();
				minor = dsp_read();
			}
			return 1;
		}
	}
	base = 0;
	return 0;
}

int sb_present(void)
{
	return base != 0;
}

uint16_t sb_base(void)
{
	return base;
}

const char *sb_model(void)
{
	if (!base)
		return "none";
	if (major >= 4)
		return "Sound Blaster 16";
	if (major == 3)
		return "Sound Blaster Pro";
	if (major == 2)
		return "Sound Blaster 2.0";
	return "Sound Blaster";
}

int cmd_sb(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!sb_detect()) {
		console_write("no Sound Blaster found at ports 220-280 (QEMU: -audiodev none,id=a -device sb16,audiodev=a)\n");
		return 1;
	}
	console_printf("%s at I/O port %x, DSP version %d.%02d\n", sb_model(), base, major, minor);
	console_printf("  capabilities: 8-bit %s, %s\n", major >= 4 ? "and 16-bit PCM" : "PCM", major >= 3 ? "stereo" : "mono");
	return 0;
}
