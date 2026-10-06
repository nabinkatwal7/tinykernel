#include "crashdump.h"

#include "ata.h"
#include "console.h"
#include "klog.h"
#include "kstring.h"
#include "ksym.h"
#include "timer.h"

#define CRASH_MAGIC  0x48535243u /* "CRSH" */
#define MAX_FRAMES   16
#define LOG_BYTES    ((CRASH_SECTORS - 1) * SECTOR_SIZE)

/* Sector 0 of the area; the log text follows in sectors 1.. */
struct crash_header {
	uint32_t magic;
	uint32_t ticks;
	uint32_t nframes;
	uint32_t frames[MAX_FRAMES];
	char     msg[160];
};

static uint8_t buf[CRASH_SECTORS * SECTOR_SIZE]; /* static: panic must not allocate */

static uint32_t area_lba(void)
{
	return ata_sectors() - CRASH_SECTORS;
}

void crash_save(const char *msg, const uint32_t *frames, int nframes)
{
	struct crash_header *h = (struct crash_header *)buf;
	int i;

	if (!ata_present() || ata_sectors() <= CRASH_SECTORS)
		return;
	memset(buf, 0, sizeof buf);
	h->magic = CRASH_MAGIC;
	h->ticks = timer_ticks();
	h->nframes = nframes > MAX_FRAMES ? MAX_FRAMES : (uint32_t)nframes;
	for (i = 0; i < (int)h->nframes; i++)
		h->frames[i] = frames[i];
	kstrlcpy(h->msg, msg, sizeof h->msg);
	klog_copy((char *)buf + SECTOR_SIZE, LOG_BYTES);
	ata_write(area_lba(), CRASH_SECTORS, buf);
}

static int load(void)
{
	struct crash_header *h = (struct crash_header *)buf;

	if (!ata_present() || ata_sectors() <= CRASH_SECTORS || ata_read(area_lba(), CRASH_SECTORS, buf))
		return 0;
	return h->magic == CRASH_MAGIC;
}

int crash_present(void)
{
	return load();
}

int crash_show(void)
{
	struct crash_header *h = (struct crash_header *)buf;
	uint32_t i, off;

	if (!load()) {
		console_write("no crash dump stored\n");
		return 1;
	}
	console_printf("crash dump from %u s after boot:\n  %s\nstack trace:\n", h->ticks / timer_hz(), h->msg);
	for (i = 0; i < h->nframes; i++) {
		const char *name = ksym_lookup(h->frames[i] - 1, &off);

		if (name)
			console_printf("  #%u %08x %s+0x%x\n", i, h->frames[i], name, off + 1);
		else
			console_printf("  #%u %08x\n", i, h->frames[i]);
	}
	buf[CRASH_SECTORS * SECTOR_SIZE - 1] = 0;
	console_write("kernel log at the time:\n");
	console_write((char *)buf + SECTOR_SIZE);
	console_putchar('\n');
	return 0;
}

int crash_clear(void)
{
	memset(buf, 0, SECTOR_SIZE);
	return ata_write(area_lba(), 1, buf);
}
