#include "sync.h"

#include "io.h"
#include "klog.h"

void mutex_init(mutex_t *m)
{
	m->locked = 0;
	m->owner = 0;
	m->q.head = m->q.tail = 0;
}

void mutex_lock(mutex_t *m)
{
	uint32_t f = irq_save();

	if (m->locked && m->owner == task_current())
		klog(LOG_ERROR, "mutex: recursive lock by %s would deadlock", task_current()->name);
	while (m->locked)
		wq_wait(&m->q);
	m->locked = 1;
	m->owner = task_current();
	irq_restore(f);
}

int mutex_trylock(mutex_t *m)
{
	uint32_t f = irq_save();
	int rc = -1;

	if (!m->locked) {
		m->locked = 1;
		m->owner = task_current();
		rc = 0;
	}
	irq_restore(f);
	return rc;
}

void mutex_unlock(mutex_t *m)
{
	uint32_t f = irq_save();

	if (!m->locked || m->owner != task_current()) {
		klog(LOG_ERROR, "mutex: unlock by non-owner %s", task_current()->name);
	} else {
		m->locked = 0;
		m->owner = 0;
		wq_wake_one(&m->q);
	}
	irq_restore(f);
}

void sem_init(sem_t *s, int count)
{
	s->count = count;
	s->q.head = s->q.tail = 0;
}

void sem_wait(sem_t *s)
{
	uint32_t f = irq_save();

	while (s->count <= 0)
		wq_wait(&s->q);
	s->count--;
	irq_restore(f);
}

int sem_trywait(sem_t *s)
{
	uint32_t f = irq_save();
	int rc = -1;

	if (s->count > 0) {
		s->count--;
		rc = 0;
	}
	irq_restore(f);
	return rc;
}

void sem_post(sem_t *s)
{
	uint32_t f = irq_save();

	s->count++;
	wq_wake_one(&s->q);
	irq_restore(f);
}

int sem_value(sem_t *s)
{
	return s->count;
}

/* xchg is implicitly locked, so this stays correct if we ever run on more than one CPU. */
static int test_and_set(volatile int *p)
{
	int old = 1;

	__asm__ volatile ("xchgl %0, %1" : "+r"(old), "+m"(*p) : : "memory");
	return old;
}

uint32_t spin_lock_irqsave(spinlock_t *l)
{
	uint32_t flags = irq_save();

	while (test_and_set(&l->locked))
		__asm__ volatile ("pause");
	return flags;
}

void spin_unlock_irqrestore(spinlock_t *l, uint32_t flags)
{
	__asm__ volatile ("" : : : "memory");
	l->locked = 0;
	irq_restore(flags);
}

int spin_trylock(spinlock_t *l, uint32_t *flags)
{
	uint32_t f = irq_save();

	if (test_and_set(&l->locked)) {
		irq_restore(f);
		return -1;
	}
	*flags = f;
	return 0;
}
