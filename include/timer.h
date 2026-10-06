#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void     timer_init(uint32_t hz);  /* program the PIT, hook IRQ0 */
uint32_t timer_ticks(void);
uint32_t timer_hz(void);
uint32_t timer_ms(void);           /* uptime in milliseconds */

#endif
