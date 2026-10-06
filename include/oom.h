#ifndef OOM_H
#define OOM_H

#include <stdint.h>

/*
 * Out-of-memory handling. When the frame allocator runs dry it asks oom_reclaim(): first the file page
 * cache is emptied (it can always be re-read), and only if that is not enough the forked user process
 * that holds the most memory is killed. Returns the number of frames recovered.
 */
void     oom_init(void);                  /* registers the reclaim hook with the allocator */
uint32_t oom_reclaim(void);
uint32_t oom_kills(void);                 /* processes killed so far */

#endif
