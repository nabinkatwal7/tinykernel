#ifndef MMAPF_H
#define MMAPF_H

#include <stdint.h>

/*
 * Memory-mapped files: the file's bytes appear in the process's memory at an address the kernel picks.
 * The mapping is a private copy read in when it is created; with MMAP_SHARED, changed pages are written
 * back to the file by munmap()/msync(). Slots of 64 KiB live at MMAP_BASE.
 */
#define MMAP_BASE 0xA00000u
#define MMAP_SLOT 0x10000u
#define MMAP_SLOTS 4
#define MMAP_SHARED 1

uint32_t mmap_file(const char *path, uint32_t length, int flags);   /* address, or 0 */
int      mmap_sync(uint32_t addr);                                   /* write changed pages back (shared mappings) */
int      mmap_unmap(uint32_t addr);                                  /* sync, then unmap and free */

#endif
