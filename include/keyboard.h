#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "idt.h"

/* Non-ASCII keys are returned above 0xFF. Ctrl+letter returns 1..26. */
#define KEY_UP    0x101
#define KEY_DOWN  0x102
#define KEY_LEFT  0x103
#define KEY_RIGHT 0x104
#define KEY_DEL   0x105
#define KEY_HOME  0x106
#define KEY_END   0x107

/* Ctrl+C is not queued as a key: it calls this hook from the keyboard IRQ (regs may be NULL when
   polling). */
void keyboard_set_sigint(void (*hook)(struct regs *r));

void keyboard_init(void);
void keyboard_use_irq(void);      /* switch from polling to IRQ1 (after the IDT is live) */
int  keyboard_trygetkey(void);    /* -1 if nothing is buffered */
int  keyboard_getkey(void);       /* blocks */
unsigned char keyboard_read_scancode(void); /* raw set-1 make code, blocking, bypasses the buffer */

#endif
