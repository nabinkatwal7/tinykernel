#ifndef SB_H
#define SB_H

#include <stdint.h>

/* Sound Blaster (ISA) detection and 8-bit PCM playback; see kernel/sb.c. */
int         sb_detect(void);          /* probes ports 220-280; 1 if a card answered */
int         sb_present(void);
uint16_t    sb_base(void);
const char *sb_model(void);

int cmd_sb(int argc, char **argv);    /* sb : detect and describe the card */

#endif
