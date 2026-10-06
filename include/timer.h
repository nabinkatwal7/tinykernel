#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void     timer_init(uint32_t hz);  /* program the PIT, hook IRQ0 */
uint32_t timer_ticks(void);
uint32_t timer_hz(void);
uint32_t timer_ms(void);           /* uptime in milliseconds */

/*
 * Kernel timers. The callback runs in timer-IRQ context with interrupts off, so keep it short
 * and never block. Returns a timer id >= 0, or -1 if all slots are in use.
 */
#define KTIMER_MAX 16
int  ktimer_add(uint32_t ms, int periodic, void (*fn)(void *), void *arg);
int  ktimer_cancel(int id);                 /* 0 on success */
void ktimer_set_arg(int id, void *arg);     /* change the argument of a pending timer */
void ktimer_list(void);                     /* for the shell */

#endif
