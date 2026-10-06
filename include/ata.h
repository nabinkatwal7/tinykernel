#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define SECTOR_SIZE 512

/* ATA PIO (LBA28) driver for the primary bus, master drive. */
int      ata_init(void);                 /* 0 if a disk was found */
int      ata_present(void);
uint32_t ata_sectors(void);
int      ata_read(uint32_t lba, uint32_t count, void *buf);        /* 0 on success */
int      ata_write(uint32_t lba, uint32_t count, const void *buf); /* 0 on success */

#endif
