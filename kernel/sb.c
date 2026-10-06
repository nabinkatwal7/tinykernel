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

/* ======================= PCM playback (8-bit, mono, DMA channel 1) ======================= */

#include "kmalloc.h"
#include "pmm.h"
#include "timer.h"
#include "vfs.h"

#define DMA_CH1_ADDR  0x02
#define DMA_CH1_COUNT 0x03
#define DMA_PAGE_CH1  0x83
#define DMA_MASK      0x0A
#define DMA_MODE      0x0B
#define DMA_FLIPFLOP  0x0C
#define PLAY_CHUNK    (32 * 1024)

static uint8_t *dma_area;   /* PLAY_CHUNK bytes below 16 MiB that do not cross a 64 KiB boundary */

static const uint8_t sine64[64] = { 127, 137, 147, 156, 165, 174, 183, 190, 198, 204, 210, 215, 219, 223, 225, 227, 227, 227, 225, 223, 219, 215, 210, 204, 198, 190, 183, 174, 165, 156, 147, 137, 127, 117, 107, 98, 89, 80, 71, 64, 56, 50, 44, 39, 35, 31, 29, 27, 27, 27, 29, 31, 35, 39, 44, 50, 56, 64, 71, 80, 89, 98, 107, 117 };

static int dma_buffer(void)
{
	uint32_t frame, start;

	if (dma_area)
		return 0;
	frame = pmm_alloc_contig(24); /* 96 KiB: room to pick a 64 KiB-safe stretch of 32 KiB */
	if (!frame)
		return -1;
	start = frame;
	if ((start & 0xFFFF) + PLAY_CHUNK > 0x10000) /* would cross a boundary: use the next 64 KiB page instead */
		start = (start + 0xFFFF) & ~0xFFFFu;
	if (start + PLAY_CHUNK > frame + 24 * 4096 || start + PLAY_CHUNK > 0x1000000) {
		pmm_free_range(frame, 24);
		return -1;
	}
	dma_area = (uint8_t *)start;
	return 0;
}

/* Has the card raised its 8-bit interrupt? The SB16 mixer register 0x82 says so (bit 0); older cards only signal on the IRQ line. */
static int irq_pending(void)
{
	if (major >= 4) {
		outb((uint16_t)(base + 4), 0x82);
		return inb((uint16_t)(base + 5)) & 1;
	}
	return inb((uint16_t)(base + DSP_STATUS)) & 0x80;
}

/* Play one chunk of unsigned 8-bit samples and wait until the card has consumed it. */
static int play_chunk(const uint8_t *samples, uint32_t len, uint32_t rate)
{
	uint32_t addr = (uint32_t)dma_area, t0, expect_ticks;

	if (!len || len > PLAY_CHUNK)
		return -1;
	memcpy(dma_area, samples, len);
	outb(DMA_MASK, 0x05);                       /* mask channel 1 while it is programmed */
	outb(DMA_FLIPFLOP, 0);
	outb(DMA_MODE, 0x49);                       /* single transfer, memory to device, channel 1 */
	outb(DMA_CH1_ADDR, (uint8_t)addr);
	outb(DMA_CH1_ADDR, (uint8_t)(addr >> 8));
	outb(DMA_PAGE_CH1, (uint8_t)(addr >> 16));
	outb(DMA_CH1_COUNT, (uint8_t)(len - 1));
	outb(DMA_CH1_COUNT, (uint8_t)((len - 1) >> 8));
	outb(DMA_MASK, 0x01);                       /* unmask */
	(void)inb((uint16_t)(base + DSP_STATUS));   /* clear any old interrupt */
	if (dsp_write(0x40) || dsp_write((uint8_t)(256 - 1000000 / rate)))   /* time constant = sampling rate */
		return -1;
	if (dsp_write(0x14) || dsp_write((uint8_t)(len - 1)) || dsp_write((uint8_t)((len - 1) >> 8))) /* 8-bit single-cycle output */
		return -1;
	t0 = timer_ticks();
	expect_ticks = (uint32_t)((uint64_t)len * timer_hz() / rate) + 1;
	/* done = the card raises its interrupt, which shows in the status port; give it generous slack */
	while (!irq_pending()) {
		if (timer_ticks() - t0 > expect_ticks + timer_hz() * 2)
			return -1;
		task_sleep(1);
	}
	(void)inb((uint16_t)(base + DSP_STATUS)); /* reading this port acknowledges the interrupt */
	return (int)(timer_ticks() - t0);
}

/* Play unsigned 8-bit mono samples at the given rate (4000-44100 Hz). Returns the ticks it took, or -1. */
int sb_play(const uint8_t *samples, uint32_t len, uint32_t rate)
{
	uint32_t done = 0, t0 = timer_ticks();

	if (!base || rate < 4000 || rate > 44100 || dma_buffer())
		return -1;
	if (dsp_write(0xD1)) /* speaker on */
		return -1;
	while (done < len) {
		uint32_t n = len - done > PLAY_CHUNK ? PLAY_CHUNK : len - done;

		if (play_chunk(samples + done, n, rate) < 0) {
			dsp_write(0xD3); /* speaker off */
			return -1;
		}
		done += n;
	}
	dsp_write(0xD3);
	return (int)(timer_ticks() - t0);
}

/* Fill buf with a sine tone of freq Hz; returns the sample count. */
uint32_t sb_make_tone(uint8_t *buf, uint32_t cap, uint32_t freq, uint32_t ms, uint32_t rate)
{
	uint32_t n = (uint32_t)((uint64_t)rate * ms / 1000), i, phase = 0, step;

	if (n > cap)
		n = cap;
	step = freq * 64 * 256 / rate; /* phase in 1/256 table steps per sample */
	for (i = 0; i < n; i++) {
		buf[i] = sine64[(phase >> 8) & 63];
		phase += step;
	}
	return n;
}

/* play tone FREQ MS [RATE] | play scale | play file PATH [RATE]  (a WAV file's own header is used when there is one) */
int cmd_play(int argc, char **argv)
{
	static uint8_t buf[64 * 1024];
	uint32_t freq, ms, rate = 22050, n;
	int ticks;

	if (!sb_present() && !sb_detect()) {
		console_write("play: no Sound Blaster (see 'sb')\n");
		return 1;
	}
	if (argc >= 4 && !kstrcmp(argv[1], "tone") && !kstrtoul(argv[2], &freq) && !kstrtoul(argv[3], &ms)) {
		if (argc > 4)
			kstrtoul(argv[4], &rate);
		n = sb_make_tone(buf, sizeof buf, freq, ms, rate);
		ticks = sb_play(buf, n, rate);
		console_printf("played %u samples at %u Hz in %d tick(s) (expected about %u)\n", n, rate, ticks, (uint32_t)((uint64_t)n * timer_hz() / rate));
		return ticks < 0;
	}
	if (argc == 2 && !kstrcmp(argv[1], "scale")) {
		static const uint16_t notes[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
		int i;

		for (i = 0; i < 8; i++) {
			n = sb_make_tone(buf, sizeof buf, notes[i], 250, rate);
			if (sb_play(buf, n, rate) < 0) {
				console_write("play: the card did not finish\n");
				return 1;
			}
		}
		console_write("played a C major scale\n");
		return 0;
	}
	if (argc >= 3 && !kstrcmp(argv[1], "file")) {
		int size = vfs_size(argv[2]), got;
		uint32_t skip = 0;

		if (argc > 3)
			kstrtoul(argv[3], &rate);
		if (size <= 0 || (uint32_t)size > sizeof buf) {
			console_write("play: cannot read the file (or it is larger than 64 KiB)\n");
			return 1;
		}
		got = vfs_read(argv[2], buf, (uint32_t)size);
		if (got < 0)
			return 1;
		if (got > 44 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) { /* canonical 44-byte header: 8-bit mono PCM */
			rate = (uint32_t)buf[24] | (uint32_t)buf[25] << 8 | (uint32_t)buf[26] << 16;
			skip = 44;
		}
		ticks = sb_play(buf + skip, (uint32_t)got - skip, rate);
		console_printf("played %u bytes at %u Hz in %d tick(s)\n", (uint32_t)got - skip, rate, ticks);
		return ticks < 0;
	}
	console_write("usage: play tone FREQ MS [RATE] | play scale | play file PATH [RATE]\n");
	return 1;
}
