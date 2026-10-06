#include "pipe.h"

#include "io.h"
#include "kmalloc.h"
#include "sched.h"

struct pipe {
	uint8_t buf[PIPE_SIZE];
	uint32_t head, count;       /* read position, bytes buffered */
	int readers, writers;
	struct waitq rq, wq;        /* blocked readers (empty) and writers (full) */
};

struct pipe *pipe_new(void)
{
	struct pipe *p = kcalloc(1, sizeof *p);

	if (p) {
		p->readers = 1;
		p->writers = 1;
	}
	return p;
}

void pipe_ref_read(struct pipe *p)
{
	uint32_t f = irq_save();

	p->readers++;
	irq_restore(f);
}

void pipe_ref_write(struct pipe *p)
{
	uint32_t f = irq_save();

	p->writers++;
	irq_restore(f);
}

/* Drop one reference of an end; wake the other side so it notices EOF / EPIPE, and free when both are gone. */
static void drop(struct pipe *p, int *end)
{
	uint32_t f = irq_save();
	int dead;

	(*end)--;
	wq_wake_all(&p->rq);
	wq_wake_all(&p->wq);
	dead = p->readers <= 0 && p->writers <= 0;
	irq_restore(f);
	if (dead)
		kfree(p);
}

void pipe_close_read(struct pipe *p)
{
	drop(p, &p->readers);
}

void pipe_close_write(struct pipe *p)
{
	drop(p, &p->writers);
}

int pipe_read(struct pipe *p, void *buf, uint32_t n)
{
	uint8_t *out = buf;
	uint32_t f = irq_save(), i;

	if (!n) {
		irq_restore(f);
		return 0;
	}
	while (!p->count && p->writers > 0)
		wq_wait(&p->rq, WAIT_OTHER);
	for (i = 0; i < n && p->count; i++) {
		out[i] = p->buf[p->head];
		p->head = (p->head + 1) % PIPE_SIZE;
		p->count--;
	}
	if (i)
		wq_wake_all(&p->wq);
	irq_restore(f);
	return (int)i;
}

int pipe_write(struct pipe *p, const void *buf, uint32_t n)
{
	const uint8_t *in = buf;
	uint32_t f = irq_save(), done = 0;

	while (done < n) {
		if (p->readers <= 0) {
			irq_restore(f);
			return -1;
		}
		while (done < n && p->count < PIPE_SIZE) {
			p->buf[(p->head + p->count) % PIPE_SIZE] = in[done++];
			p->count++;
		}
		wq_wake_all(&p->rq);
		if (done < n)
			wq_wait(&p->wq, WAIT_OTHER);
	}
	irq_restore(f);
	return (int)done;
}

uint32_t pipe_count(struct pipe *p)
{
	return p->count;
}
