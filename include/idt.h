#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Register frame built by isr_common (see isr.S). */
struct regs {
	uint32_t gs, fs, es, ds;
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pusha order */
	uint32_t int_no, err_code;
	uint32_t eip, cs, eflags, useresp, ss; /* useresp/ss only valid if cs&3 */
};

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void double_fault_task(void) __attribute__((noreturn)); /* runs on its own stack and TSS */
void irq_install_handler(int irq, irq_handler_t h);

#endif
