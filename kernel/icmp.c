#include "icmp.h"

#include "io.h"
#include "ip.h"
#include "kmalloc.h"
#include "kstring.h"
#include "sched.h"
#include "timer.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8
#define PING_ID           0x7473 /* "ts" */
#define PING_DATA         32

struct icmp_echo {
	uint8_t type, code;
	uint16_t checksum;
	uint16_t id, seq;
} __attribute__((packed));

static struct icmp_stats stats;

/* State of the single outstanding ping. */
static volatile int waiting;
static volatile int got_reply;
static uint16_t want_seq;
static uint32_t want_src, reply_bytes;
static uint32_t sent_tick, reply_tick;
static struct waitq ping_wq;

static void send_echo(uint32_t dst, uint8_t type, uint16_t id, uint16_t seq, const uint8_t *data, uint16_t dlen)
{
	uint8_t *buf;
	struct icmp_echo *e;

	if (dlen > IP_REASM_MAX - sizeof *e)
		return;
	buf = kmalloc(sizeof *e + dlen); /* large echoes (fragmented on the wire) do not fit on a kernel stack */
	if (!buf)
		return;
	e = (struct icmp_echo *)buf;
	e->type = type;
	e->code = 0;
	e->checksum = 0;
	e->id = htons(id);
	e->seq = htons(seq);
	memcpy(buf + sizeof *e, data, dlen);
	e->checksum = htons(ip_checksum(buf, (uint32_t)(sizeof *e + dlen)));
	ip_send(dst, IP_PROTO_ICMP, buf, (uint16_t)(sizeof *e + dlen));
	kfree(buf);
}

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *p, uint16_t len)
{
	const struct icmp_echo *e = (const struct icmp_echo *)p;

	(void)dst;
	if (len < sizeof *e || ip_checksum(p, len) != 0)
		return;
	if (e->type == ICMP_ECHO_REQUEST) {
		stats.echo_requests_in++;
		/* answer with exactly the data we were sent */
		send_echo(src, ICMP_ECHO_REPLY, ntohs(e->id), ntohs(e->seq), p + sizeof *e,
			  (uint16_t)(len - sizeof *e));
		stats.echo_replies_out++;
	} else if (e->type == ICMP_ECHO_REPLY) {
		stats.echo_replies_in++;
		if (waiting && ntohs(e->id) == PING_ID && ntohs(e->seq) == want_seq && src == want_src) {
			reply_bytes = (uint32_t)(len - sizeof *e);
			reply_tick = timer_ticks();
			got_reply = 1;
			wq_wake_one(&ping_wq);
		}
	}
}

int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ms, uint32_t *bytes, uint8_t *ttl)
{
	return icmp_ping_size(dst, seq, PING_DATA, timeout_ms, bytes, ttl);
}

int icmp_ping_size(uint32_t dst, uint16_t seq, uint32_t size, uint32_t timeout_ms, uint32_t *bytes, uint8_t *ttl)
{
	uint8_t *data;
	uint32_t deadline_ticks, flags;
	uint32_t i;

	(void)ttl;
	if (size > IP_REASM_MAX - sizeof(struct icmp_echo))
		return -1;
	data = kmalloc(size);
	if (!data)
		return -1;
	for (i = 0; i < size; i++)
		data[i] = (uint8_t)('a' + i % 23); /* the classic abcdef... filler */
	flags = irq_save();
	want_seq = seq;
	want_src = dst;
	got_reply = 0;
	waiting = 1;
	irq_restore(flags);

	sent_tick = timer_ticks();
	send_echo(dst, ICMP_ECHO_REQUEST, PING_ID, seq, data, (uint16_t)size);
	kfree(data);
	stats.echo_requests_out++;

	deadline_ticks = sent_tick + timeout_ms * timer_hz() / 1000;
	while (!got_reply && (int32_t)(deadline_ticks - timer_ticks()) > 0)
		task_sleep(10);
	waiting = 0;
	if (!got_reply)
		return -2;
	if (bytes)
		*bytes = reply_bytes;
	return (int)((reply_tick - sent_tick) * (1000 / timer_hz()));
}

void icmp_get_stats(struct icmp_stats *out)
{
	*out = stats;
}

void icmp_init(void)
{
	ip_register_proto(IP_PROTO_ICMP, icmp_input);
}
