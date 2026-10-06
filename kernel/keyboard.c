#include "keyboard.h"

#include "io.h"
#include "pic.h"
#include "sched.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_OBF    0x01 /* output buffer full */
#define KBD_AUX    0x20 /* byte came from the mouse port */

#define BUF_SIZE 64

static const char map_normal[58] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

static const char map_shift[58] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

/* Ring buffer: producer is the IRQ (or the poller), consumer is keyboard_trygetkey(). */
static volatile int buf[BUF_SIZE];
static volatile unsigned head, tail;
static volatile int irq_mode;
static struct waitq kb_wq; /* tasks blocked in keyboard_getkey() */
static int shift, ctrl, caps, e0;

static void push(int k)
{
	unsigned next = (head + 1) % BUF_SIZE;

	if (next == tail)
		return; /* full: drop */
	buf[head] = k;
	head = next;
}

static int is_letter(char c)
{
	return c >= 'a' && c <= 'z';
}

static void handle_scancode(uint8_t sc)
{
	int release = sc & 0x80;
	uint8_t code = sc & 0x7F;
	char c;

	if (sc == 0xE0) {
		e0 = 1;
		return;
	}

	if (code == 0x2A || code == 0x36) {
		shift = !release;
		return;
	}
	if (code == 0x1D) { /* left or (E0) right ctrl */
		ctrl = !release;
		e0 = 0;
		return;
	}
	if (code == 0x3A) { /* caps lock toggles on press */
		if (!release)
			caps = !caps;
		return;
	}

	if (e0) {
		e0 = 0;
		if (release)
			return;
		switch (code) {
		case 0x48: push(KEY_UP); break;
		case 0x50: push(KEY_DOWN); break;
		case 0x4B: push(KEY_LEFT); break;
		case 0x4D: push(KEY_RIGHT); break;
		case 0x53: push(KEY_DEL); break;
		case 0x47: push(KEY_HOME); break;
		case 0x4F: push(KEY_END); break;
		}
		return;
	}

	if (release || code >= sizeof map_normal)
		return;

	c = map_normal[code];
	if (!c)
		return;
	if (is_letter(c)) {
		if (ctrl) {
			push(c - 'a' + 1);
			return;
		}
		if (shift ^ caps)
			c = map_shift[code];
	} else if (shift) {
		c = map_shift[code];
	}
	push((unsigned char)c);
}

static void poll(void)
{
	if (inb(KBD_STATUS) & KBD_OBF)
		handle_scancode(inb(KBD_DATA));
}

static void keyboard_irq(struct regs *r)
{
	(void)r;
	while (inb(KBD_STATUS) & KBD_OBF) {
		uint8_t aux = inb(KBD_STATUS) & KBD_AUX;
		uint8_t sc = inb(KBD_DATA);

		if (!aux)
			handle_scancode(sc);
	}
	wq_wake_one(&kb_wq); /* a key (or at least a scancode) arrived */
}

void keyboard_init(void)
{
	while (inb(KBD_STATUS) & KBD_OBF) /* drain stale bytes */
		(void)inb(KBD_DATA);
}

void keyboard_use_irq(void)
{
	irq_install_handler(1, keyboard_irq);
	pic_unmask(1);
	irq_mode = 1;
}

/* Interrupts must be off. */
static int pop_locked(void)
{
	int k = -1;

	if (!irq_mode)
		poll();
	if (tail != head) {
		k = buf[tail];
		tail = (tail + 1) % BUF_SIZE;
	}
	return k;
}

int keyboard_trygetkey(void)
{
	uint32_t f = irq_save();
	int k = pop_locked();

	irq_restore(f);
	return k;
}

/* Sleeps on a wait queue until IRQ1 delivers a key: a blocked reader costs no CPU. */
int keyboard_getkey(void)
{
	for (;;) {
		uint32_t f = irq_save();
		int k = pop_locked();

		if (k < 0 && irq_mode)
			wq_wait(&kb_wq, WAIT_KEYBOARD); /* atomic with the empty check: no lost wakeup */
		irq_restore(f);
		if (k >= 0)
			return k;
	}
}

unsigned char keyboard_read_scancode(void)
{
	for (;;) {
		uint8_t sc;

		while (!(inb(KBD_STATUS) & KBD_OBF))
			;
		sc = inb(KBD_DATA);
		if (!(sc & 0x80))
			return sc;
	}
}
