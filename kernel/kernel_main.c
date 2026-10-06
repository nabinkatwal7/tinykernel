#include "acpi.h"
#include "apic.h"
#include "clock.h"
#include "hrtime.h"
#include "ata.h"
#include "bcache.h"
#include "cmdline.h"
#include "console.h"
#include "cpustat.h"
#include "cpu.h"
#include "fat12.h"
#include "fs.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "kmalloc.h"
#include "mouse.h"
#include "oom.h"
#include "net.h"
#include "kprintf.h"
#include "paging.h"
#include "pci.h"
#include "percpu.h"
#include "pic.h"
#include "pmm.h"
#include "sched.h"
#include "serial.h"
#include "shell.h"
#include "speaker.h"
#include "timer.h"
#include "vfs.h"
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
	cmdline_init();
	if (cmdline_has("debug"))
		klog_set_console_level(LOG_INFO); /* boot messages on the screen */
	klog(LOG_INFO, "%s %s booting", KERNEL_NAME, KERNEL_VERSION);

	gdt_init();
	idt_init();
	pic_init();
	pmm_init();
	oom_init();
	klog(LOG_INFO, "ram: %u MiB (E820), %u MiB usable, CMOS says %u MiB", (pmm_ram_kib() + 512) / 1024,
	     pmm_usable_kib() / 1024, (pmm_cmos_ram_kib() + 512) / 1024);
	heap_init();
	paging_init();
	keyboard_init();
	timer_init(100);
	sched_init();
	keyboard_use_irq();
	serial_rx_init();
	if (!cmdline_has("safe") && !cmdline_has("nomouse")) {
		if (mouse_init() == 0)
			mousecursor_init();
	}
	sti();
	klog(LOG_INFO, "interrupts enabled");

	cpu_init();
	acpi_init();
	apic_init();
	if (apic_present() && !cmdline_has("nopic") && !cmdline_has("safe"))
		apic_timer_start(100);
	hrtime_init();
	percpu_init();
	cpustat_init();
	clock_init();
	pci_scan();
	vfs_init();
	if (ata_init() == 0) {
		if (ata_dev_present(1) && fat12_mount(1) != FS_OK)
			klog(LOG_INFO, "ata: drive 1 is not FAT12");
		int rc = fs_mount();

		klog(LOG_INFO, "fs: %s", rc == FS_OK ? "mounted" : fs_strerror(rc));
	}

	cmdline_load_config(); /* /boot.cfg may add more words (e.g. gfx) */
	if (cmdline_has("gfx"))
		gfxcon_enable(1);
	task_create("status", status_task, 0, 7);
	if (!cmdline_has("safe") && !cmdline_has("nonet"))
		rtl8139_init(); /* needs the scheduler (rx task) and IRQs */
	else
		klog(LOG_INFO, "net: disabled by the kernel command line");
	bc_start_flusher();
	console_status(" " KERNEL_NAME " " KERNEL_VERSION);
	if (!cmdline_has("safe") && !cmdline_has("nobeep"))
		speaker_beep(880, 70); /* power-on chirp */
	kinfo("speaker: boot beep done");
	shell_run();
}
