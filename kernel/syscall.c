#include "syscall.h"

#include "console.h"
#include "file.h"
#include "keyboard.h"
#include "klog.h"
#include "sched.h"
#include "timer.h"
#include "user.h"

static int user_ptr_ok(uint32_t p, uint32_t len)
{
	return p >= USER_BASE && len <= USER_END - USER_BASE && p + len <= USER_END;
}

/* A NUL-terminated string that lies entirely inside the program window. */
static int user_str_ok(uint32_t p)
{
	if (p < USER_BASE || p >= USER_END)
		return 0;
	for (; p < USER_END; p++)
		if (!*(const char *)p)
			return 1;
	return 0;
}

void syscall_dispatch(struct regs *r)
{
	if (user_abort_requested()) { /* Ctrl+C arrived while we were in the kernel */
		console_write("^C\n");
		user_abort();
	}
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
	case SYS_SLEEP: {
		uint32_t left = r->ebx;

		while (left && !user_abort_requested()) { /* sleep in slices so Ctrl+C can interrupt */
			uint32_t slice = left > 20 ? 20 : left;

			task_sleep(slice);
			left -= slice;
		}
		if (user_abort_requested()) {
			console_write("^C\n");
			user_abort();
		}
		r->eax = 0;
		break;
	}
	case SYS_OPEN:
		r->eax = user_str_ok(r->ebx) ? (uint32_t)file_open((const char *)r->ebx, (int)r->ecx)
					     : (uint32_t)-1;
		break;
	case SYS_CLOSE:
		r->eax = (uint32_t)file_close((int)r->ebx);
		break;
	case SYS_READ:
		if (!user_ptr_ok(r->ecx, r->edx)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)file_read((int)r->ebx, (void *)r->ecx, r->edx);
		break;
	case SYS_FWRITE: {
		uint32_t i;

		if (!user_ptr_ok(r->ecx, r->edx)) {
			r->eax = (uint32_t)-1;
			break;
		}
		if (r->ebx == 1 || r->ebx == 2) { /* standard output / error */
			for (i = 0; i < r->edx; i++)
				console_putchar(((const char *)r->ecx)[i]);
			r->eax = r->edx;
		} else {
			r->eax = (uint32_t)file_write((int)r->ebx, (const void *)r->ecx, r->edx);
		}
		break;
	}
	default:
		klog(LOG_WARN, "unknown syscall %u", r->eax);
		r->eax = (uint32_t)-1;
		break;
	}
}
