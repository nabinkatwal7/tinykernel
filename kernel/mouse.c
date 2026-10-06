#include "mouse.h"

#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "pic.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

static struct mouse_state st;
static int present;
static uint8_t pkt[3];
static int have;
static void (*hook)(void);

static int wait_write(void)
{
	int spin = 100000;

	while ((inb(KBD_STATUS) & 2) && --spin)
		;
	return spin != 0;
}

static int wait_read(void)
{
	int spin = 100000;

	while (!(inb(KBD_STATUS) & 1) && --spin)
		;
	return spin != 0;
}

/* Send one byte to the mouse and wait for its 0xFA acknowledge. */
static int mouse_cmd(uint8_t b)
{
	if (!wait_write())
		return -1;
	outb(KBD_STATUS, 0xD4); /* next data byte goes to the aux device */
	if (!wait_write())
		return -1;
	outb(KBD_DATA, b);
	if (!wait_read())
		return -1;
	return inb(KBD_DATA) == 0xFA ? 0 : -1;
}

void mouse_feed(uint8_t byte)
{
	if (!present)
		return;
	if (have == 0 && !(byte & 0x08)) { /* byte 0 always has bit 3 set: otherwise we are out of step */
		st.resyncs++;
		return;
	}
	pkt[have++] = byte;
	if (have < 3)
		return;
	have = 0;

	{
		int dx = pkt[1], dy = pkt[2];

		if (pkt[0] & 0x10)
			dx -= 256; /* 9-bit two's complement: the sign lives in the first byte */
		if (pkt[0] & 0x20)
			dy -= 256;
		if (!(pkt[0] & 0xC0)) { /* ignore packets whose movement overflowed */
			st.x += dx;
			st.y -= dy; /* the mouse counts up, the screen counts down */
			if (st.x < 0)
				st.x = 0;
			if (st.x > MOUSE_MAX_X)
				st.x = MOUSE_MAX_X;
			if (st.y < 0)
				st.y = 0;
			if (st.y > MOUSE_MAX_Y)
				st.y = MOUSE_MAX_Y;
		}
		st.buttons = pkt[0] & 7;
		st.packets++;
	}
	if (hook)
		hook();
}

static void mouse_irq(struct regs *r)
{
	(void)r;
	keyboard_poll_controller(); /* drains the 8042; aux bytes arrive via mouse_feed() */
}

int mouse_init(void)
{
	uint8_t cfg;

	present = 0;
	st.x = MOUSE_MAX_X / 2;
	st.y = MOUSE_MAX_Y / 2;
	if (!wait_write())
		return -1;
	outb(KBD_STATUS, 0xA8); /* enable the aux port */
	if (!wait_write())
		return -1;
	outb(KBD_STATUS, 0x20); /* read the controller configuration byte */
	if (!wait_read())
		return -1;
	cfg = inb(KBD_DATA);
	cfg |= 0x02;   /* enable the mouse interrupt */
	cfg &= (uint8_t)~0x20; /* ... and the mouse clock */
	if (!wait_write())
		return -1;
	outb(KBD_STATUS, 0x60);
	if (!wait_write())
		return -1;
	outb(KBD_DATA, cfg);

	if (mouse_cmd(0xF6) || mouse_cmd(0xF4)) { /* defaults, then start streaming */
		klog(LOG_INFO, "mouse: no PS/2 mouse");
		return -1;
	}
	present = 1;
	have = 0;
	irq_install_handler(12, mouse_irq);
	pic_unmask(12);
	klog(LOG_INFO, "mouse: PS/2 mouse enabled");
	return 0;
}

int mouse_present(void)
{
	return present;
}

void mouse_get(struct mouse_state *out)
{
	uint32_t f = irq_save();

	*out = st;
	irq_restore(f);
}

void mouse_set_hook(void (*h)(void))
{
	hook = h;
}
