#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define SECTOR_SIZE 512
#define ATA_DEVICES 2   /* primary bus: master (0) and slave (1) */

/* ATA PIO (LBA28) driver for the primary bus. */
int      ata_init(void);                 /* probes both drives; 0 if the master is present */
int      ata_dev_present(int dev);
uint32_t ata_dev_sectors(int dev);
int      ata_dev_read(int dev, uint32_t lba, uint32_t count, void *buf);        /* 0 on success */
int      ata_dev_write(int dev, uint32_t lba, uint32_t count, const void *buf); /* 0 on success */

/* Shorthand for the primary master, the disk TinyFS lives on. */
int      ata_present(void);
uint32_t ata_sectors(void);
int      ata_read(uint32_t lba, uint32_t count, void *buf);
int      ata_write(uint32_t lba, uint32_t count, const void *buf);

#endif
