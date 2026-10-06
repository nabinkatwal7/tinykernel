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
