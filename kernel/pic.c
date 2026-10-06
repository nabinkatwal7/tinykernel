#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define EOI       0x20

void pic_init(void)
{
	outb(PIC1_CMD, 0x11);  /* ICW1: init, expect ICW4 */
	outb(PIC2_CMD, 0x11);
	outb(PIC1_DATA, 0x20); /* ICW2: vector offsets */
	outb(PIC2_DATA, 0x28);
	outb(PIC1_DATA, 0x04); /* ICW3: slave on IRQ2 */
	outb(PIC2_DATA, 0x02);
	outb(PIC1_DATA, 0x01); /* ICW4: 8086 mode */
	outb(PIC2_DATA, 0x01);
	outb(PIC1_DATA, 0xFB); /* mask all but the cascade line */
	outb(PIC2_DATA, 0xFF);
}

void pic_unmask(int irq)
{
	if (irq < 8) {
		outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1 << irq));
	} else {
		outb(PIC2_DATA, inb(PIC2_DATA) & (uint8_t)~(1 << (irq - 8)));
	}
}

void pic_mask(int irq)
{
	if (irq < 8)
		outb(PIC1_DATA, inb(PIC1_DATA) | (uint8_t)(1 << irq));
	else
		outb(PIC2_DATA, inb(PIC2_DATA) | (uint8_t)(1 << (irq - 8)));
}

void pic_eoi(int irq)
{
	if (irq >= 8)
		outb(PIC2_CMD, EOI);
	outb(PIC1_CMD, EOI);
}

int pic_is_spurious(int irq)
{
	if (irq == 7) {
		outb(PIC1_CMD, 0x0B); /* read ISR */
		return !(inb(PIC1_CMD) & 0x80);
	}
	if (irq == 15) {
		outb(PIC2_CMD, 0x0B);
		if (inb(PIC2_CMD) & 0x80)
			return 0;
		outb(PIC1_CMD, EOI);
		return 1;
	}
	return 0;
}
