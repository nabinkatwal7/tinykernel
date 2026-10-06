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

/*
 * Spinlock for data shared with interrupt handlers. Acquiring disables interrupts first (so an
 * IRQ cannot re-enter the same lock on this CPU) and the previous state is restored on release.
 * Never sleep or call blocking functions while holding one.
 */
typedef struct {
	volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

uint32_t spin_lock_irqsave(spinlock_t *l);               /* returns the saved EFLAGS */
void     spin_unlock_irqrestore(spinlock_t *l, uint32_t flags);
int      spin_trylock(spinlock_t *l, uint32_t *flags);   /* 0 on success */

/* Ticket lock: first come, first served, so no core can starve. Does not touch the interrupt flag. */
typedef struct {
	volatile uint16_t next;    /* next ticket to hand out */
	volatile uint16_t owner;   /* ticket currently allowed in */
} ticketlock_t;

#define TICKETLOCK_INIT { 0, 0 }

void ticket_lock(ticketlock_t *l);
void ticket_unlock(ticketlock_t *l);

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
