#include "ata.h"

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

static int present;
static uint32_t total_sectors;

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

static void select_sector(uint32_t lba, uint32_t count, uint8_t cmd)
{
	outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
	outb(ATA_COUNT, (uint8_t)count);
	outb(ATA_LBA_LO, (uint8_t)lba);
	outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
	outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
	outb(ATA_CMD, cmd);
}

int ata_init(void)
{
	uint16_t id[256];
	int i;

	present = 0;
	outb(ATA_CTRL, 0x02); /* nIEN: we poll, never take IRQ14 */
	outb(ATA_DRIVE, 0xA0);
	if (inb(ATA_STATUS) == 0xFF) { /* floating bus: no controller */
		klog(LOG_INFO, "ata: no disk");
		return -1;
	}
	outb(ATA_COUNT, 0);
	outb(ATA_LBA_LO, 0);
	outb(ATA_LBA_MID, 0);
	outb(ATA_LBA_HI, 0);
	outb(ATA_CMD, 0xEC); /* IDENTIFY */
	if (inb(ATA_STATUS) == 0 || wait_not_busy() < 0) {
		klog(LOG_INFO, "ata: no disk");
		return -1;
	}
	if (inb(ATA_LBA_MID) || inb(ATA_LBA_HI)) { /* ATAPI / SATA signature */
		klog(LOG_INFO, "ata: primary master is not a plain ATA disk");
		return -1;
	}
	if (wait_drq() < 0)
		return -1;
	for (i = 0; i < 256; i++)
		id[i] = inw(ATA_DATA);

	total_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
	present = total_sectors != 0;
	klog(LOG_INFO, "ata: %u sectors (%u KiB)", total_sectors, total_sectors / 2);
	return present ? 0 : -1;
}

int ata_present(void)
{
	return present;
}

uint32_t ata_sectors(void)
{
	return total_sectors;
}

int ata_read(uint32_t lba, uint32_t count, void *buf)
{
	uint16_t *w = buf;
	uint32_t i, s;

	if (!present || lba + count > total_sectors)
		return -1;
	for (s = 0; s < count; s++) {
		if (wait_not_busy() < 0)
			return -1;
		select_sector(lba + s, 1, 0x20);
		if (wait_drq() < 0)
			return -1;
		for (i = 0; i < 256; i++)
			*w++ = inw(ATA_DATA);
	}
	return 0;
}

int ata_write(uint32_t lba, uint32_t count, const void *buf)
{
	const uint16_t *w = buf;
	uint32_t i, s;

	if (!present || lba + count > total_sectors)
		return -1;
	for (s = 0; s < count; s++) {
		if (wait_not_busy() < 0)
			return -1;
		select_sector(lba + s, 1, 0x30);
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
