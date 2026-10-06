#include "ata.h"
#include "console.h"
#include "fs.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "pic.h"
#include "pmm.h"
#include "sched.h"
#include "serial.h"
#include "shell.h"
#include "timer.h"
#include "version.h"

/* Background task: keeps the status bar (row 0) fresh once a second. */
static void status_task(void *arg)
{
	char text[96];

	(void)arg;
	for (;;) {
		uint32_t s = timer_ms() / 1000;

		ksnprintf(text, sizeof text, " %s %s | up %02u:%02u:%02u | tasks %u | free %u KiB",
			  KERNEL_NAME, KERNEL_VERSION, s / 3600, s / 60 % 60, s % 60, task_count(),
			  pmm_free_frames() * 4);
		console_status(text);
		task_sleep(1000);
	}
}

void kernel_main(void)
{
	serial_init();
	console_clear();
	klog(LOG_INFO, "%s %s booting", KERNEL_NAME, KERNEL_VERSION);

	gdt_init();
	idt_init();
	pic_init();
	pmm_init();
	heap_init();
	keyboard_init();
	timer_init(100);
	sched_init();
	keyboard_use_irq();
	sti();
	klog(LOG_INFO, "interrupts enabled");

	if (ata_init() == 0) {
		int rc = fs_mount();

		klog(LOG_INFO, "fs: %s", rc == FS_OK ? "mounted" : fs_strerror(rc));
	}

	task_create("status", status_task, 0, 1);
	console_status(" " KERNEL_NAME " " KERNEL_VERSION);
	shell_run();
}
