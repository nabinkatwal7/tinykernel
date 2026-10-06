#include "syscall.h"

#include "console.h"
#include "keyboard.h"
#include "klog.h"
#include "sched.h"
#include "timer.h"
#include "user.h"

static int user_ptr_ok(uint32_t p, uint32_t len)
{
	return p >= USER_BASE && len <= USER_END - USER_BASE && p + len <= USER_END;
}

void syscall_dispatch(struct regs *r)
{
	switch (r->eax) {
	case SYS_EXIT:
		user_exit((int)r->ebx);
	case SYS_WRITE: {
		uint32_t i;

		if (!user_ptr_ok(r->ebx, r->ecx)) {
			r->eax = (uint32_t)-1;
			break;
		}
		for (i = 0; i < r->ecx; i++)
			console_putchar(((const char *)r->ebx)[i]);
		r->eax = r->ecx;
		break;
	}
	case SYS_PUTCHAR:
		console_putchar((char)r->ebx);
		r->eax = 0;
		break;
	case SYS_TICKS:
		r->eax = timer_ticks();
		break;
	case SYS_GETKEY:
		r->eax = (uint32_t)keyboard_getkey();
		break;
	case SYS_SLEEP:
		task_sleep(r->ebx);
		r->eax = 0;
		break;
	default:
		klog(LOG_WARN, "unknown syscall %u", r->eax);
		r->eax = (uint32_t)-1;
		break;
	}
}
