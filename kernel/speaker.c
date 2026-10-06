#include "speaker.h"

#include "io.h"
#include "sched.h"

#define PIT_CMD  0x43
#define PIT_CH2  0x42
#define PIT_FREQ 1193182u
#define PORT_B   0x61 /* bit 0 = gate PIT channel 2, bit 1 = connect it to the speaker */

static uint32_t current_hz;

void speaker_on(uint32_t hz)
{
	uint32_t div;

	if (hz < 20 || hz > 20000) {
		speaker_off();
		return;
	}
	div = PIT_FREQ / hz;
	outb(PIT_CMD, 0xB6);                  /* channel 2, lobyte/hibyte, square wave */
	outb(PIT_CH2, (uint8_t)(div & 0xFF));
	outb(PIT_CH2, (uint8_t)(div >> 8));
	outb(PORT_B, inb(PORT_B) | 0x03);
	current_hz = hz;
}

void speaker_off(void)
{
	outb(PORT_B, inb(PORT_B) & (uint8_t)~0x03);
	current_hz = 0;
}

int speaker_is_on(void)
{
	return (inb(PORT_B) & 0x03) == 0x03;
}

uint32_t speaker_freq(void)
{
	return current_hz;
}

void speaker_beep(uint32_t hz, uint32_t ms)
{
	speaker_on(hz);
	task_sleep(ms);
	speaker_off();
}
