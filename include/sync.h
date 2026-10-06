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

/* Counting semaphore. */
typedef struct {
	int count;
	struct waitq q;
} sem_t;

void sem_init(sem_t *s, int count);
void sem_wait(sem_t *s);        /* P: block until a permit is available */
int  sem_trywait(sem_t *s);     /* 0 on success, -1 if none available */
void sem_post(sem_t *s);        /* V: add a permit, wake one waiter */
int  sem_value(sem_t *s);

#endif
