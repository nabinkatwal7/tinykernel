#ifndef SHM_H
#define SHM_H

#include <stdint.h>

/*
 * System V-style shared memory: segments are named by an integer key, created on first use, and
 * attached into a process at a fixed address (SHM_BASE + id * SHM_SLOT). Attached pages are mapped
 * to the same physical frames in every attaching process and stay shared across fork().
 */
#define SHM_MAX_SEGMENTS 8
#define SHM_MAX_PAGES    16                   /* 64 KiB per segment */
#define SHM_BASE         0x900000u
#define SHM_SLOT         0x10000u

int      shm_get(int key, uint32_t size);     /* segment id (existing or new), or -1 */
uint32_t shm_attach(int id);                   /* address in the calling process, or 0 */
int      shm_detach(uint32_t addr);            /* 0 on success */
int      shm_remove(int key);                  /* destroy the segment; frames go once every process detached */
int      shm_info(int index, int *key, uint32_t *size, int *attaches);   /* for listing: 0 if the slot is used */

#endif
