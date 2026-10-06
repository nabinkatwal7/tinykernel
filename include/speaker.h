#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

/* PC speaker: PIT channel 2 generates a square wave, port 0x61 gates it to the speaker. */
void speaker_on(uint32_t hz);          /* start a tone (20 Hz - 20 kHz); 0 or out of range = off */
void speaker_off(void);
int  speaker_is_on(void);
uint32_t speaker_freq(void);
void speaker_beep(uint32_t hz, uint32_t ms);   /* blocking: sleeps via the scheduler, then silences */

#endif
