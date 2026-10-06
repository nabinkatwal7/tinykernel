#include "gdbstub.h"

#include "console.h"
#include "io.h"
#include "kstring.h"
#include "paging.h"

#define COM2     0x2F8
#define PKT_MAX  1024
#define EFLAGS_TF 0x100

static int armed, ready;
static char in[PKT_MAX + 8], out[PKT_MAX + 8];

static void uart_init(void)
{
	outb(COM2 + 1, 0x00);
	outb(COM2 + 3, 0x80);
	outb(COM2 + 0, 0x01); /* 115200 baud */
	outb(COM2 + 1, 0x00);
	outb(COM2 + 3, 0x03);
	outb(COM2 + 2, 0xC7);
	outb(COM2 + 4, 0x03);
	ready = 1;
}

static char uart_get(void)
{
	while (!(inb(COM2 + 5) & 1))
		;
	return (char)inb(COM2);
}

static void uart_put(char c)
{
	while (!(inb(COM2 + 5) & 0x20))
		;
	outb(COM2, (uint8_t)c);
}

static const char hex[] = "0123456789abcdef";

static int unhex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Reads "$data#cc", acknowledges it, and leaves the data in 'in'. Ignores noise before '$'. */
static void get_packet(void)
{
	for (;;) {
		unsigned n = 0, sum = 0;
		char c;

		while (uart_get() != '$')
			;
		while ((c = uart_get()) != '#') {
			if (n < PKT_MAX)
				in[n++] = c;
			sum += (uint8_t)c;
		}
		in[n] = 0;
		c = uart_get();
		if (unhex(c) * 16 + unhex(uart_get()) == (int)(sum & 0xFF)) {
			uart_put('+');
			return;
		}
		uart_put('-');
	}
}

static void put_packet(const char *data)
{
	for (;;) {
		unsigned sum = 0;
		const char *p;

		uart_put('$');
		for (p = data; *p; p++) {
			uart_put(*p);
			sum += (uint8_t)*p;
		}
		uart_put('#');
		uart_put(hex[(sum >> 4) & 15]);
		uart_put(hex[sum & 15]);
		if (uart_get() == '+')
			return;
	}
}

static int readable(uint32_t addr)
{
	uint32_t *pte = paging_pte(0, addr);

	return pte && (*pte & 1);
}

static char *put_hex32(char *p, uint32_t v) /* little-endian bytes, as gdb expects */
{
	int i;

	for (i = 0; i < 4; i++, v >>= 8) {
		*p++ = hex[(v >> 4) & 15];
		*p++ = hex[v & 15];
	}
	return p;
}

static uint32_t get_hex32(const char *p)
{
	uint32_t v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		v |= (uint32_t)(unhex(p[0]) * 16 + unhex(p[1])) << (8 * i);
		p += 2;
	}
	return v;
}

static uint32_t parse_num(const char **p)
{
	uint32_t v = 0;
	int d;

	while ((d = unhex(**p)) >= 0) {
		v = v * 16 + (uint32_t)d;
		(*p)++;
	}
	return v;
}

/* gdb's i386 register order: eax ecx edx ebx esp ebp esi edi eip eflags cs ss ds es fs gs */
static void regs_get(const struct regs *r, uint32_t v[16])
{
	v[0] = r->eax; v[1] = r->ecx; v[2] = r->edx; v[3] = r->ebx;
	v[4] = (r->cs & 3) ? r->useresp : (uint32_t)&r->useresp;
	v[5] = r->ebp; v[6] = r->esi; v[7] = r->edi;
	v[8] = r->eip; v[9] = r->eflags; v[10] = r->cs;
	v[11] = (r->cs & 3) ? r->ss : 0x10;
	v[12] = r->ds; v[13] = r->es; v[14] = r->fs; v[15] = r->gs;
}

static void regs_set(struct regs *r, const uint32_t v[16])
{
	r->eax = v[0]; r->ecx = v[1]; r->edx = v[2]; r->ebx = v[3];
	r->ebp = v[5]; r->esi = v[6]; r->edi = v[7];
	r->eip = v[8]; r->eflags = v[9];
}

void gdbstub_trap(struct regs *r)
{
	if (!ready)
		uart_init();
	put_packet("S05"); /* stopped by SIGTRAP */
	for (;;) {
		const char *p = in + 1;
		uint32_t addr, len, i;

		get_packet();
		out[0] = 0;
		switch (in[0]) {
		case '?':
			kstrlcpy(out, "S05", sizeof out);
			break;
		case 'g': {
			uint32_t v[16];
			char *o = out;

			regs_get(r, v);
			for (i = 0; i < 16; i++)
				o = put_hex32(o, v[i]);
			*o = 0;
			break;
		}
		case 'G': {
			uint32_t v[16];

			regs_get(r, v);
			for (i = 0; i < 16 && kstrlen(in + 1) >= (i + 1) * 8; i++)
				v[i] = get_hex32(in + 1 + i * 8);
			regs_set(r, v);
			kstrlcpy(out, "OK", sizeof out);
			break;
		}
		case 'm': {
			char *o = out;

			addr = parse_num(&p);
			p++; /* ',' */
			len = parse_num(&p);
			if (len > PKT_MAX / 2)
				len = PKT_MAX / 2;
			for (i = 0; i < len; i++) {
				uint8_t b;

				if (!readable(addr + i))
					break;
				b = *(volatile uint8_t *)(addr + i);
				*o++ = hex[b >> 4];
				*o++ = hex[b & 15];
			}
			*o = 0;
			if (!out[0] && len)
				kstrlcpy(out, "E01", sizeof out);
			break;
		}
		case 'M': {
			addr = parse_num(&p);
			p++;
			len = parse_num(&p);
			p++; /* ':' */
			for (i = 0; i < len; i++) {
				if (!readable(addr + i)) {
					kstrlcpy(out, "E01", sizeof out);
					break;
				}
				*(volatile uint8_t *)(addr + i) = (uint8_t)(unhex(p[0]) * 16 + unhex(p[1]));
				p += 2;
			}
			if (!out[0])
				kstrlcpy(out, "OK", sizeof out);
			break;
		}
		case 'c':
		case 's':
			if (*p)
				r->eip = parse_num(&p);
			if (in[0] == 's')
				r->eflags |= EFLAGS_TF;
			else
				r->eflags &= ~EFLAGS_TF;
			return; /* resume; the next trap sends a fresh stop reply */
		case 'k':
		case 'D':
			if (in[0] == 'D')
				put_packet("OK");
			r->eflags &= ~EFLAGS_TF;
			armed = 0;
			return;
		case 'H':
			kstrlcpy(out, "OK", sizeof out);
			break;
		case 'q':
			if (!kstrncmp(in, "qSupported", 10))
				kstrlcpy(out, "PacketSize=400", sizeof out);
			else if (!kstrncmp(in, "qAttached", 9))
				kstrlcpy(out, "1", sizeof out);
			break;
		default: /* unsupported: the empty reply */
			break;
		}
		put_packet(out);
	}
}

int gdbstub_active(void)
{
	return armed;
}

void gdbstub_arm(void)
{
	armed = 1;
	console_write("gdbstub: waiting for gdb on COM2 (target remote ...)\n");
	__asm__ volatile ("int3");
	console_write("gdbstub: resumed\n");
}
