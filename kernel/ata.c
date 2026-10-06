#include "ata.h"
#include "crashdump.h"

#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"

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

/*
 * Bus-master DMA (PCI IDE controller, primary channel). One physical descriptor points at a 64 KiB bounce buffer
 * (128 sectors); the controller copies data between the disk and that buffer on its own while the CPU only polls
 * for completion. Used for reads and writes when available; any failure falls back to the PIO loop.
 */
#define BM_CMD    0
#define BM_STATUS 2
#define BM_PRD    4
#define DMA_SECTORS 128

static int dma_enabled;
static uint16_t bm_base;
static uint8_t *dma_buf;      /* physical == virtual: the first 64 MiB are identity mapped */
static uint32_t *prd;         /* one 8-byte physical region descriptor */
static uint32_t dma_transfers, dma_fallbacks;

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

/* One DMA transfer of 1..128 sectors through the bounce buffer. 0 on success. */
static int dma_run(int dev, uint32_t lba, uint32_t count, int write)
{
	int spin;

	if (wait_not_busy() < 0)
		return -1;
	prd[0] = (uint32_t)dma_buf;
	prd[1] = (count * 512 & 0xFFFF) | 0x80000000u; /* byte count (0 means 64 KiB), end of table */
	outl(bm_base + BM_PRD, (uint32_t)prd);
	outb(bm_base + BM_CMD, write ? 0x00 : 0x08);        /* direction: 0x08 = the device writes to memory */
	outb(bm_base + BM_STATUS, 0x06);                    /* clear the error and interrupt bits (write 1) */
	select_sector(dev, lba, count & 0xFF, write ? 0xCA : 0xC8);
	outb(bm_base + BM_CMD, (write ? 0x00 : 0x08) | 0x01); /* start */
	for (spin = 2000000; spin; spin--) {
		if (!(inb(bm_base + BM_STATUS) & 0x01))         /* the controller finished the transfer */
			break;
	}
	outb(bm_base + BM_CMD, 0x00);
	if (!spin || (inb(bm_base + BM_STATUS) & 0x02) || wait_not_busy() < 0)
		return -1;
	if (inb(ATA_STATUS) & (ST_ERR | ST_DF))
		return -1;
	return 0;
}

/* Set up DMA if a PCI IDE controller with bus mastering exists. */
static void dma_init(void)
{
	int i;
	uint32_t cmd, frame;

	dma_enabled = 0;
	for (i = 0; i < pci_count(); i++) {
		const struct pci_dev *d = pci_get(i);

		if (d->class_code != 0x01 || d->subclass != 0x01 || !(d->bar[4] & 1))
			continue;
		bm_base = (uint16_t)(d->bar[4] & ~3u);
		cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
		pci_write32(d->bus, d->slot, d->func, 0x04, cmd | 0x05); /* I/O space + bus master */
		if (!dma_buf) {
			frame = pmm_alloc_contig(17); /* 16 pages of data and one page for the descriptor */
			if (!frame)
				return;
			dma_buf = (uint8_t *)frame;
			prd = (uint32_t *)(frame + 16 * 4096);
			memset(prd, 0, 8);
		}
		dma_enabled = 1;
		klog(LOG_INFO, "ata: bus-master DMA at port %x (controller %04x:%04x)", bm_base, d->vendor, d->device);
		return;
	}
}

int ata_dma_available(void)
{
	return dma_buf != 0;
}

int ata_dma_enabled(void)
{
	return dma_enabled;
}

void ata_dma_enable(int on)
{
	dma_enabled = on && dma_buf;
}

void ata_dma_stats(uint32_t *transfers, uint32_t *fallbacks)
{
	*transfers = dma_transfers;
	*fallbacks = dma_fallbacks;
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
	dma_init();
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
	while (dma_enabled && count > 0) { /* whole chunks through DMA */
		uint32_t n = count < DMA_SECTORS ? count : DMA_SECTORS;

		if (dma_run(dev, lba, n, 0)) {
			dma_fallbacks++;
			break; /* the PIO loop below redoes this chunk */
		}
		memcpy(w, dma_buf, n * 512);
		dma_transfers++;
		w += n * 256;
		lba += n;
		count -= n;
	}
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
	while (dma_enabled && count > 0) {
		uint32_t n = count < DMA_SECTORS ? count : DMA_SECTORS;

		memcpy(dma_buf, w, n * 512);
		if (dma_run(dev, lba, n, 1)) {
			dma_fallbacks++;
			break;
		}
		outb(ATA_CMD, 0xE7); /* flush cache */
		if (wait_not_busy() < 0)
			return -1;
		dma_transfers++;
		w += n * 256;
		lba += n;
		count -= n;
	}
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
