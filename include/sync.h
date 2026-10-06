#ifndef SYNC_H
#define SYNC_H

#include "sched.h"

/* Sleeping mutex: contenders block (no spinning) and are woken in FIFO order. Not recursive. */
typedef struct {
	int locked;
	task_t *owner;
	struct waitq q;
} mutex_t;

#define MUTEX_INIT { 0, 0, { 0, 0 } }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_trylock(mutex_t *m);   /* 0 on success, -1 if held */
void mutex_unlock(mutex_t *m);    /* must be called by the owner */

#endif
