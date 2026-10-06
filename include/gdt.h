#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18 /* use with |3 */
#define SEL_UDATA 0x20
#define SEL_TSS   0x28

/* Flat 4 GiB segments for ring 0 and ring 3, plus a TSS for the ring3->ring0 stack switch. */
void gdt_init(void);
void gdt_set_kernel_stack(uint32_t esp0);

#endif
