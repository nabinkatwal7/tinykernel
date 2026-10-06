#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18 /* use with |3 */
#define SEL_UDATA 0x20
#define SEL_TSS   0x28
#define SEL_DF    0x30 /* TSS used as the double-fault task gate target */

/* Flat 4 GiB segments for ring 0 and ring 3, plus a TSS for the ring3->ring0 stack switch. */
void gdt_init(void);
void gdt_set_kernel_stack(uint32_t esp0);
void gdt_load_current(void);      /* lgdt + reload segments on the calling core (used by application processors) */
void gdt_set_df_cr3(uint32_t cr3); /* the double-fault task needs the kernel page directory */

#endif
