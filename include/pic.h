#ifndef PIC_H
#define PIC_H

void pic_init(void);          /* remap IRQ0-15 to vectors 0x20-0x2F, all masked */
void pic_unmask(int irq);
void pic_mask(int irq);
void pic_eoi(int irq);
int  pic_is_spurious(int irq); /* true for a fake IRQ7/15 (sends the master EOI for 15) */

#endif
