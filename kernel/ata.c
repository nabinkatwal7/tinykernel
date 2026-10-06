#include "ata.h"
#include "crashdump.h"

#include "io.h"
#include "klog.h"

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_COUNT   0x1F2
#define ATA_LBA_LO  0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI  0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

static int present[ATA_DEVICES];
static uint32_t total_sectors[ATA_DEVICES];

/* Waits for BSY to clear; returns the final status, or -1 on timeout. */
static int wait_not_busy(void)
{
	int spin = 1000000;
	int st;

	while ((st = inb(ATA_STATUS)) & ST_BSY)
		if (!--spin)
			return -1;
	return st;
}

static int wait_drq(void)
{
	int spin = 1000000;
	int st;

	do {
		st = inb(ATA_STATUS);
		if (st & (ST_ERR | ST_DF))
			return -1;
		if (!(st & ST_BSY) && (st & ST_DRQ))
			return 0;
	} while (--spin);
	return -1;
}

/* Choose master (0) or slave (1) and give the drive its 400 ns to settle. */
static void select_dev(int dev, uint32_t lba_top)
{
	int i;

	outb(ATA_DRIVE, (uint8_t)(0xE0 | (dev << 4) | (lba_top & 0x0F)));
	for (i = 0; i < 4; i++)
		(void)inb(ATA_CTRL);
}

static void select_sector(int dev, uint32_t lba, uint32_t count, uint8_t cmd)
{
	select_dev(dev, lba >> 24);
	outb(ATA_COUNT, (uint8_t)count);
	outb(ATA_LBA_LO, (uint8_t)lba);
	outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
	outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
	outb(ATA_CMD, cmd);
}

/* IDENTIFY one drive. Returns its size in sectors, 0 if there is no usable ATA disk. */
static uint32_t identify(int dev)
{
	uint16_t id[256];
	int i;

	select_dev(dev, 0);
	outb(ATA_COUNT, 0);
	outb(ATA_LBA_LO, 0);
	outb(ATA_LBA_MID, 0);
	outb(ATA_LBA_HI, 0);
	outb(ATA_CMD, 0xEC); /* IDENTIFY */
	if (inb(ATA_STATUS) == 0 || wait_not_busy() < 0)
		return 0;
	if (inb(ATA_LBA_MID) || inb(ATA_LBA_HI)) /* ATAPI / SATA signature */
		return 0;
	if (wait_drq() < 0)
		return 0;
	for (i = 0; i < 256; i++)
		id[i] = inw(ATA_DATA);
	return (uint32_t)id[60] | ((uint32_t)id[61] << 16);
}

int ata_init(void)
{
	int dev;

	outb(ATA_CTRL, 0x02); /* nIEN: we poll, never take IRQ14 */
	present[0] = present[1] = 0;
	outb(ATA_DRIVE, 0xA0);
	if (inb(ATA_STATUS) == 0xFF) { /* floating bus: no controller */
		klog(LOG_INFO, "ata: no disk");
		return -1;
	}
	for (dev = 0; dev < ATA_DEVICES; dev++) {
		total_sectors[dev] = identify(dev);
		present[dev] = total_sectors[dev] != 0;
		if (present[dev])
			klog(LOG_INFO, "ata: drive %d: %u sectors (%u KiB)", dev, total_sectors[dev],
			     total_sectors[dev] / 2);
	}
	if (!present[0])
		klog(LOG_INFO, "ata: no disk on the primary master");
	return present[0] ? 0 : -1;
}

int ata_dev_present(int dev)
{
	return dev >= 0 && dev < ATA_DEVICES && present[dev];
}

uint32_t ata_dev_sectors(int dev)
{
	return ata_dev_present(dev) ? total_sectors[dev] : 0;
}

int ata_dev_read(int dev, uint32_t lba, uint32_t count, void *buf)
{
	uint16_t *w = buf;
	uint32_t i, s;

	if (!ata_dev_present(dev) || lba + count > total_sectors[dev])
		return -1;
	for (s = 0; s < count; s++) {
		if (wait_not_busy() < 0)
			return -1;
		select_sector(dev, lba + s, 1, 0x20);
		if (wait_drq() < 0)
			return -1;
		for (i = 0; i < 256; i++)
			*w++ = inw(ATA_DATA);
	}
	return 0;
}

int ata_dev_write(int dev, uint32_t lba, uint32_t count, const void *buf)
{
	const uint16_t *w = buf;
	uint32_t i, s;

	if (!ata_dev_present(dev) || lba + count > total_sectors[dev])
		return -1;
	for (s = 0; s < count; s++) {
		if (wait_not_busy() < 0)
			return -1;
		select_sector(dev, lba + s, 1, 0x30);
		if (wait_drq() < 0)
			return -1;
		for (i = 0; i < 256; i++)
			outw(ATA_DATA, *w++);
		outb(ATA_CMD, 0xE7); /* flush cache */
		if (wait_not_busy() < 0)
			return -1;
	}
	return 0;
}

/* The primary master is "the disk" for TinyFS. */
int ata_present(void)                                          { return ata_dev_present(0); }
uint32_t ata_sectors(void)                                     { return ata_dev_sectors(0); }
int ata_read(uint32_t lba, uint32_t count, void *buf)          { return ata_dev_read(0, lba, count, buf); }
uint32_t ata_fs_sectors(void)
{
	uint32_t n = ata_dev_sectors(0);

	return n > CRASH_SECTORS ? n - CRASH_SECTORS : 0;
}
int ata_write(uint32_t lba, uint32_t count, const void *buf)   { return ata_dev_write(0, lba, count, buf); }
