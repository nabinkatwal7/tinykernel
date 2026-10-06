#include "syscall.h"

#include "console.h"
#include "clock.h"
#include "file.h"
#include "gfx.h"
#include "vga.h"
#include "keyboard.h"
#include "klog.h"
#include "sched.h"
#include "mmapf.h"
#include "shell.h"
#include "shm.h"
#include "timer.h"
#include "kstring.h"
#include "user.h"
#include "vfs.h"

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
	case SYS_FWRITE:
		if (!user_ptr_ok(r->ecx, r->edx)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)file_write((int)r->ebx, (const void *)r->ecx, r->edx);
		break;
	case SYS_MKDIR:
		r->eax = user_str_ok(r->ebx) ? (uint32_t)vfs_mkdir((const char *)r->ebx) : (uint32_t)-1;
		break;
	case SYS_RMDIR:
		r->eax = user_str_ok(r->ebx) ? (uint32_t)vfs_rmdir((const char *)r->ebx) : (uint32_t)-1;
		break;
	case SYS_GETCWD: {
		const char *c = vfs_getcwd();
		uint32_t len = (uint32_t)kstrlen(c) + 1;

		if (!user_ptr_ok(r->ebx, r->ecx) || len > r->ecx) {
			r->eax = (uint32_t)-1;
			break;
		}
		memcpy((void *)r->ebx, c, len);
		r->eax = len - 1;
		break;
	}
	case SYS_CHDIR:
		r->eax = user_str_ok(r->ebx) ? (uint32_t)vfs_chdir((const char *)r->ebx) : (uint32_t)-1;
		break;
	case SYS_LSEEK:
		r->eax = (uint32_t)file_seek((int)r->ebx, (int32_t)r->ecx, (int)r->edx);
		break;
	case SYS_DUP:
		r->eax = (uint32_t)file_dup((int)r->ebx);
		break;
	case SYS_DUP2:
		r->eax = (uint32_t)file_dup2((int)r->ebx, (int)r->ecx);
		break;
	case SYS_EXEC:
	case SYS_SPAWN: {
		char *const *av = (char *const *)r->ecx;
		uint32_t i;
		int ok = user_str_ok(r->ebx);

		if (ok && av) { /* the argv array and every string in it must lie in the window */
			ok = 0;
			for (i = 0; i < 16 && user_ptr_ok((uint32_t)&av[i], 4); i++) {
				if (!av[i]) {
					ok = 1;
					break;
				}
				if (!user_str_ok((uint32_t)av[i]))
					break;
			}
		}
		if (!ok)
			r->eax = (uint32_t)-1;
		else if (r->eax == SYS_EXEC)
			r->eax = user_exec(r, (const char *)r->ebx, av) ? (uint32_t)-1 : 0;
		else
			r->eax = (uint32_t)user_spawn((const char *)r->ebx, av);
		break;
	}
	case SYS_KCMD:
		r->eax = user_str_ok(r->ebx) ? (uint32_t)shell_exec_from_user((const char *)r->ebx)
					     : (uint32_t)-1;
		break;
	case SYS_CLOCK: {
		struct timespec ts;

		if (!user_ptr_ok(r->ecx, sizeof ts) || clock_gettime((int)r->ebx, &ts)) {
			r->eax = (uint32_t)-1;
			break;
		}
		*(struct timespec *)r->ecx = ts;
		r->eax = 0;
		break;
	}
	case SYS_NANOSLEEP:
		if (r->ecx >= 1000000000u || r->ebx > 3600) {
			r->eax = (uint32_t)-1;
			break;
		}
		task_sleep_ns((uint64_t)r->ebx * 1000000000u + r->ecx);
		r->eax = 0;
		break;
	case SYS_FORK:
		r->eax = (uint32_t)user_fork(r);
		break;
	case SYS_WAITPID: {
		int code = 0;

		if (r->ecx && !user_ptr_ok(r->ecx, 4)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)task_wait(r->ebx, &code);
		if (!r->eax && r->ecx)
			*(int *)r->ecx = code;
		break;
	}
	case SYS_SHMGET:
		r->eax = (uint32_t)shm_get((int)r->ebx, r->ecx);
		break;
	case SYS_SHMAT:
		r->eax = shm_attach((int)r->ebx);
		break;
	case SYS_SHMDT:
		r->eax = (uint32_t)shm_detach(r->ebx);
		break;
	case SYS_MMAP:
		r->eax = user_str_ok(r->ebx) ? mmap_file((const char *)r->ebx, r->ecx, (int)r->edx) : 0;
		break;
	case SYS_MUNMAP:
		r->eax = (uint32_t)mmap_unmap(r->ebx);
		break;
	case SYS_TRYKEY:
		r->eax = (uint32_t)keyboard_trygetkey();
		break;
	case SYS_PUTAT:
		if (!gfxcon_active())
			console_putat((int)r->ebx, (int)r->ecx, (char)(r->edx & 0xFF), (uint8_t)(r->edx >> 8));
		break;
	case SYS_CLS:
		console_clear();
		break;
	case SYS_FLOCK:
		r->eax = (uint32_t)file_flock((int)r->ebx, (int)r->ecx);
		break;
	case SYS_GFX: {
		const int *a = (const int *)r->ecx;

		r->eax = (uint32_t)-1;
		if (!user_ptr_ok(r->ecx, 6 * sizeof(int)) || gfxcon_active())
			break;
		if (r->ebx == GFX_ENTER) {
			r->eax = (uint32_t)vga_set_graphics();
		} else if (!vga_in_graphics()) {
			break; /* everything else needs the graphics screen */
		} else if (r->ebx == GFX_LEAVE) {
			vga_set_text();
			console_clear();
			r->eax = 0;
		} else if (r->ebx == GFX_RECT) {
			gfx_fill_rect(a[0], a[1], a[2], a[3], (uint8_t)a[4]);
			r->eax = 0;
		} else if (r->ebx == GFX_CLEAR) {
			vga_fill((uint8_t)a[0]);
			r->eax = 0;
		} else if (r->ebx == GFX_PALETTE) {
			vga_set_palette((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3]);
			r->eax = 0;
		} else if (r->ebx == GFX_TEXT && user_str_ok((uint32_t)a[4])) {
			char text[64];

			kstrlcpy(text, (const char *)a[4], sizeof text);
			gfx_text(a[0], a[1], text, a[2], a[3]);
			r->eax = 0;
		}
		break;
	}
	case SYS_PIPE: {
		int fds[2];

		if (!user_ptr_ok(r->ebx, sizeof fds)) {
			r->eax = (uint32_t)-1;
			break;
		}
		r->eax = (uint32_t)file_pipe(fds);
		if (!r->eax) {
			((int *)r->ebx)[0] = fds[0];
			((int *)r->ebx)[1] = fds[1];
		}
		break;
	}
	case SYS_MSYNC:
		r->eax = (uint32_t)mmap_sync(r->ebx);
		break;
	case SYS_SBRK:
		r->eax = user_sbrk((int32_t)r->ebx);
		break;
	case SYS_GETPID:
		r->eax = task_current()->id;
		break;
	case SYS_GETPPID:
		r->eax = task_current()->ppid;
		break;
	default:
		klog(LOG_WARN, "unknown syscall %u", r->eax);
		r->eax = (uint32_t)-1;
		break;
	}
}
