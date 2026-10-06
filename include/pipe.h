#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_SIZE 4096

/*
 * Pipes: a byte FIFO between tasks. Readers block while it is empty (until the last writer closes: then
 * they see end of file); writers block while it is full (and fail once the last reader has gone).
 * Each end is reference counted so that dup and fork can share it; the pipe frees itself when both
 * ends are fully closed.
 */
struct pipe;

struct pipe *pipe_new(void);                        /* one reader and one writer reference; NULL if out of memory */
void pipe_ref_read(struct pipe *p);
void pipe_ref_write(struct pipe *p);
void pipe_close_read(struct pipe *p);
void pipe_close_write(struct pipe *p);
int  pipe_read(struct pipe *p, void *buf, uint32_t n);        /* bytes read (>= 1), 0 at end of file */
int  pipe_write(struct pipe *p, const void *buf, uint32_t n); /* n, or -1 if no reader is left */
uint32_t pipe_count(struct pipe *p);                          /* bytes currently buffered */

#endif
