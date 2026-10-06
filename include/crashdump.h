#ifndef CRASHDUMP_H
#define CRASHDUMP_H

#include <stdint.h>

/*
 * Crash dumps: on panic the kernel writes the message, a stack trace and the tail of the kernel log
 * to the last CRASH_SECTORS sectors of the primary disk (outside TinyFS). The next boot reports it;
 * the 'crashdump' command shows or clears it.
 */
#define CRASH_SECTORS 16

void crash_save(const char *msg, const uint32_t *frames, int nframes); /* from panic(): polled disk I/O only */
int  crash_present(void);                                              /* 1 if a dump is stored */
int  crash_show(void);                                                 /* print it; 0 on success */
int  crash_clear(void);

#endif
