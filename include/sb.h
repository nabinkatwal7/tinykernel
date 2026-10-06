#ifndef SB_H
#define SB_H

#include <stdint.h>

/* Sound Blaster (ISA) detection and 8-bit PCM playback; see kernel/sb.c. */
int         sb_detect(void);          /* probes ports 220-280; 1 if a card answered */
int         sb_present(void);
uint16_t    sb_base(void);
const char *sb_model(void);

int         sb_play(const uint8_t *samples, uint32_t len, uint32_t rate);   /* unsigned 8-bit mono; ticks taken, or -1 */
uint32_t    sb_make_tone(uint8_t *buf, uint32_t cap, uint32_t freq, uint32_t ms, uint32_t rate);
int cmd_play(int argc, char **argv);  /* play tone|scale|file */
int cmd_sb(int argc, char **argv);    /* sb : detect and describe the card */

#endif
