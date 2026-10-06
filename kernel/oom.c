#include "oom.h"

#include "klog.h"
#include "paging.h"
#include "pcache.h"
#include "pmm.h"
#include "sched.h"

static uint32_t kills;

uint32_t oom_reclaim(void)
{
	uint32_t before = pmm_free_frames(), id, pages;

	if (pcache_shrink(PCACHE_PAGES))
		return pmm_free_frames() - before;      /* dropping cached file pages was enough */
	if (!sched_largest_uproc(&id, &pages)) {
		klog(LOG_ERROR, "oom: out of memory and nothing to kill");
		return 0;
	}
	klog(LOG_ERROR, "oom: killing process %u (%u user pages)", id, pages);
	if (task_kill(id))
		return 0;
	kills++;
	task_yield(); /* let the scheduler reap the dead task: that releases its address space */
	return pmm_free_frames() > before ? pmm_free_frames() - before : pages;
}

uint32_t oom_kills(void)
{
	return kills;
}

void oom_init(void)
{
	pmm_set_reclaim(oom_reclaim);
}
