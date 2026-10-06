#include "tcp.h"

#include "console.h"
#include "io.h"
#include "ip.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"

/*
 * TCP. Active and passive open; a send buffer with retransmission (RTT estimation as in RFC 6298: smoothed RTT and
 * variance give the timeout, which doubles on every retry); in-order receive with cumulative acknowledgements;
 * flow control in both directions (see window_limit() and rx_window()). No congestion control, no options,
 * and TIME_WAIT is skipped. One mutex serialises the connection table; the network task, the timer task and the
 * applications all take it.
 */

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define MSS 1400
#define MIN_RTO_MS 100
#define MAX_RTO_MS 3000
#define INITIAL_RTO_MS 400
#define MAX_RETRIES 8

struct tcp_header {
	uint16_t src_port, dst_port;
	uint32_t seq, ack;
	uint8_t data_off;     /* header length in 32-bit words, in the high nibble */
	uint8_t flags;
	uint16_t window, checksum, urgent;
} __attribute__((packed));

struct conn {
	int used;
	enum tcp_state state;
	uint32_t remote_ip;
	uint16_t local_port, remote_port;
	uint32_t snd_una;     /* oldest unacknowledged sequence number */
	uint32_t snd_nxt;     /* next sequence number to send */
	uint32_t snd_wnd;     /* the window the peer last advertised */
	uint32_t rcv_nxt;     /* next sequence number we expect */
	int refused, failed;
	uint8_t *rx;          /* receive ring */
	uint32_t rx_head, rx_tail;
	uint32_t last_adv;    /* the window we last told the peer about */
	int fin_received;
	/* send side: tx[] holds the bytes from snd_una onwards (sent but unacknowledged, then not yet sent) */
	uint8_t *tx;
	uint32_t tx_start, tx_len;
	int fin_queued, fin_sent; /* an application close is pending / the FIN has been transmitted */
	/* retransmission */
	int rtx_active;
	uint32_t rtx_deadline, rto_ms, srtt, rttvar, retries;
	int rtt_active;
	uint32_t rtt_seq, rtt_start;
	uint32_t stat_retransmits, stat_probes;
	int backlog[TCP_BACKLOG];   /* LISTEN: established connections waiting for tcp_accept() */
	int nback;
	int listener;               /* SYN_RCVD: the listener that spawned it, or -1 */
};

static struct conn conns[TCP_MAX_CONN];
static mutex_t lock;
static uint16_t next_port = 40000;
static uint32_t iss_counter = 0x1000;
static struct tcp_stats stats;
static uint32_t drop_out, drop_in, drop_skip;   /* test hooks: segments still to be discarded (after letting SKIP pass) */

const char *tcp_state_name(enum tcp_state s)
{
	static const char *const names[] = { "CLOSED", "SYN_SENT", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
					     "CLOSE_WAIT", "LAST_ACK", "TIME_WAIT", "LISTEN", "SYN_RCVD" };

	return names[s];
}

enum tcp_state tcp_state(int c)
{
	return c >= 0 && c < TCP_MAX_CONN && conns[c].used ? conns[c].state : TCP_CLOSED;
}

int tcp_conn_info(int c, uint32_t *ip, uint16_t *rport, uint16_t *lport, enum tcp_state *st)
{
	if (c < 0 || c >= TCP_MAX_CONN || !conns[c].used)
		return -1;
	*ip = conns[c].remote_ip;
	*rport = conns[c].remote_port;
	*lport = conns[c].local_port;
	*st = conns[c].state;
	return 0;
}

static uint32_t rx_count(const struct conn *c)
{
	return (c->rx_head - c->rx_tail + TCP_RXBUF) % TCP_RXBUF;
}

int tcp_conn_stats(int c, struct tcp_conn_stats *out)
{
	if (c < 0 || c >= TCP_MAX_CONN || !conns[c].used)
		return -1;
	out->rto_ms = conns[c].rto_ms;
	out->srtt_ms = conns[c].srtt / 8;
	out->peer_window = conns[c].snd_wnd;
	out->in_flight = conns[c].snd_nxt - conns[c].snd_una;
	out->queued = conns[c].tx_len;
	out->retransmits = conns[c].stat_retransmits;
	out->window_probes = conns[c].stat_probes;
	out->rx_buffered = rx_count(&conns[c]);
	out->advertised = conns[c].last_adv;
	return 0;
}

void tcp_get_stats(struct tcp_stats *out)
{
	*out = stats;
}

void tcp_drop_next(uint32_t outgoing, uint32_t incoming, uint32_t skip)
{
	drop_out = outgoing;
	drop_in = incoming;
	drop_skip = skip;
}

static uint32_t ms_to_ticks(uint32_t ms)
{
	uint32_t t = ms * timer_hz() / 1000;

	return t ? t : 1;
}

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
	uint32_t sum = ip_pseudo_sum(src, dst, IP_PROTO_TCP, len), i;

	for (i = 0; i + 1 < len; i += 2)
		sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
	if (len & 1)
		sum += (uint32_t)(seg[len - 1] << 8);
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}


/* Receive window: the free space in the receive ring. */
static uint32_t rx_window(const struct conn *c)
{
	return c->rx ? TCP_RXBUF - 1 - rx_count(c) : 0;
}

static int send_segment(struct conn *c, uint8_t flags, uint32_t seq, const void *data, uint16_t len)
{
	uint8_t pkt[sizeof(struct tcp_header) + MSS];
	struct tcp_header *h = (struct tcp_header *)pkt;
	uint32_t win = rx_window(c);

	if (len > MSS)
		return -1;
	if (drop_skip && drop_out) {
		drop_skip--; /* let this one through first */
	} else if (drop_out) { /* test hook: pretend the network lost this segment */
		drop_out--;
		stats.dropped_by_hook++;
		return 0;
	}
	h->src_port = htons(c->local_port);
	h->dst_port = htons(c->remote_port);
	h->seq = htonl(seq);
	h->ack = htonl(flags & F_ACK ? c->rcv_nxt : 0);
	h->data_off = (uint8_t)(5 << 4);
	h->flags = flags;
	h->window = htons((uint16_t)win);
	c->last_adv = win;
	h->checksum = 0;
	h->urgent = 0;
	memcpy(pkt + sizeof *h, data, len);
	h->checksum = htons(tcp_checksum(netif.ip, c->remote_ip, pkt, (uint16_t)(sizeof *h + len)));
	stats.segments_sent++;
	return ip_send(c->remote_ip, IP_PROTO_TCP, pkt, (uint16_t)(sizeof *h + len));
}

/* Answer a segment for which no connection exists with a RST, as the RFC requires. */
static void send_rst(uint32_t dst_ip, const struct tcp_header *in, uint16_t datalen)
{
	struct conn tmp;
	uint32_t seq, ack = ntohl(in->seq) + datalen + ((in->flags & (F_SYN | F_FIN)) ? 1 : 0);
	uint8_t flags = F_RST;

	memset(&tmp, 0, sizeof tmp);
	tmp.remote_ip = dst_ip;
	tmp.remote_port = ntohs(in->src_port);
	tmp.local_port = ntohs(in->dst_port);
	if (in->flags & F_ACK) {
		seq = ntohl(in->ack);
	} else {
		seq = 0;
		flags |= F_ACK;
		tmp.rcv_nxt = ack;
	}
	stats.resets_sent++;
	send_segment(&tmp, flags, seq, 0, 0);
}

static struct conn *find(uint32_t rip, uint16_t rport, uint16_t lport)
{
	int i;

	for (i = 0; i < TCP_MAX_CONN; i++)
		if (conns[i].used && conns[i].state != TCP_LISTEN && conns[i].remote_ip == rip && conns[i].remote_port == rport
		    && conns[i].local_port == lport)
			return &conns[i];
	return 0;
}

static struct conn *find_listener(uint16_t lport)
{
	int i;

	for (i = 0; i < TCP_MAX_CONN; i++)
		if (conns[i].used && conns[i].state == TCP_LISTEN && conns[i].local_port == lport)
			return &conns[i];
	return 0;
}

static struct conn *alloc_conn(void)
{
	struct conn *c = 0;
	int i;

	for (i = 0; i < TCP_MAX_CONN && !c; i++)
		if (!conns[i].used)
			c = &conns[i];
	if (c) {
		memset(c, 0, sizeof *c);
		c->listener = -1;
		c->rto_ms = INITIAL_RTO_MS;
		c->rx = kmalloc(TCP_RXBUF);
		c->tx = kmalloc(TCP_TXBUF);
		if (!c->rx || !c->tx) {
			kfree(c->rx);
			kfree(c->tx);
			c->rx = c->tx = 0;
			return 0;
		}
		c->used = 1;
	}
	return c;
}

static void free_conn(struct conn *c)
{
	kfree(c->rx);
	kfree(c->tx);
	c->rx = c->tx = 0;
	c->used = 0;
	c->state = TCP_CLOSED;
}

/* ---- sending ---- */

static void rtx_arm(struct conn *c)
{
	c->rtx_active = 1;
	c->rtx_deadline = timer_ticks() + ms_to_ticks(c->rto_ms);
}

/*
 * Sliding window: how many bytes may be in flight (sent, not yet acknowledged) at once. The peer's advertised window
 * is the limit - it says how much room its receive buffer has - and the send buffer bounds it anyway. Several segments
 * travel back to back; each cumulative ACK slides the left edge (snd_una) forward and lets more out.
 */
static uint32_t window_limit(const struct conn *c)
{
	return c->snd_wnd < TCP_TXBUF ? c->snd_wnd : TCP_TXBUF;
}

/* Send whatever the window allows, then the FIN if the application has closed and everything went out. */
static void tcp_output(struct conn *c)
{
	if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT && c->state != TCP_FIN_WAIT_1 && c->state != TCP_LAST_ACK)
		return;
	for (;;) {
		uint32_t flight = c->snd_nxt - c->snd_una - (c->fin_sent ? 1 : 0);
		uint32_t unsent = c->tx_len - flight, room, n;
		uint8_t seg[MSS];
		uint32_t i, off;

		if (c->fin_sent || !unsent)
			break;
		if (window_limit(c) <= flight)
			break; /* the peer's window is full (zero-window probing is done by the timer) */
		room = window_limit(c) - flight;
		n = unsent < MSS ? unsent : MSS;
		if (n > room)
			n = room;
		off = (c->tx_start + flight) % TCP_TXBUF;
		for (i = 0; i < n; i++)
			seg[i] = c->tx[(off + i) % TCP_TXBUF];
		send_segment(c, F_ACK | F_PSH, c->snd_nxt, seg, (uint16_t)n);
		if (!c->rtt_active) { /* time one segment per round trip */
			c->rtt_active = 1;
			c->rtt_seq = c->snd_nxt + n;
			c->rtt_start = timer_ticks();
		}
		c->snd_nxt += n;
		if (!c->rtx_active)
			rtx_arm(c);
	}
	if (c->fin_queued && !c->fin_sent && c->tx_len == c->snd_nxt - c->snd_una) {
		send_segment(c, F_FIN | F_ACK, c->snd_nxt, 0, 0);
		c->snd_nxt++; /* the FIN takes a sequence number */
		c->fin_sent = 1;
		if (!c->rtx_active)
			rtx_arm(c);
	}
}

/* The timer fired: go back to the oldest unacknowledged byte and resend from there (go-back-N), with the timeout doubled. */
static void tcp_output(struct conn *c);

static void retransmit(struct conn *c)
{
	uint32_t flight = c->snd_nxt - c->snd_una;

	if (++c->retries > MAX_RETRIES) { /* give up: the connection is dead */
		stats.aborted++;
		c->failed = 1;
		c->state = TCP_CLOSED;
		c->rtx_active = 0;
		return;
	}
	c->rto_ms = c->rto_ms * 2 > MAX_RTO_MS ? MAX_RTO_MS : c->rto_ms * 2;
	c->rtt_active = 0; /* Karn: no sample from a segment that was sent twice */
	c->stat_retransmits++;
	stats.retransmits++;
	if (c->state == TCP_SYN_SENT) {
		send_segment(c, F_SYN, c->snd_una, 0, 0);
	} else if (c->state == TCP_SYN_RCVD) {
		send_segment(c, F_SYN | F_ACK, c->snd_una, 0, 0);
	} else if (flight == 0 && c->tx_len) { /* zero window with data waiting: probe with one byte */
		uint8_t b = c->tx[c->tx_start];

		c->stat_probes++;
		stats.window_probes++;
		send_segment(c, F_ACK, c->snd_nxt, &b, 1);
		c->snd_nxt++;
	} else if (flight) { /* go back to the left edge of the window and send it all again (the receiver keeps no out-of-order data) */
		c->fin_sent = 0;
		c->snd_nxt = c->snd_una;
		tcp_output(c);
	}
	rtx_arm(c);
}

static void timer_task(void *arg)
{
	(void)arg;
	for (;;) {
		int i;

		task_sleep(25);
		mutex_lock(&lock);
		for (i = 0; i < TCP_MAX_CONN; i++) {
			struct conn *c = &conns[i];

			if (!c->used || c->state == TCP_LISTEN)
				continue;
			if (c->rtx_active) {
				if ((int32_t)(timer_ticks() - c->rtx_deadline) >= 0)
					retransmit(c);
			} else if (c->state == TCP_ESTABLISHED && c->tx_len && c->snd_nxt == c->snd_una) {
				rtx_arm(c); /* zero window with data waiting: keep asking */
			}
		}
		mutex_unlock(&lock);
	}
}

/* ---- receiving ---- */

static void rtt_sample(struct conn *c, uint32_t ack)
{
	uint32_t r;

	if (!c->rtt_active || (int32_t)(ack - c->rtt_seq) < 0)
		return;
	c->rtt_active = 0;
	r = (timer_ticks() - c->rtt_start) * 1000 / timer_hz();
	if (!c->srtt) { /* first measurement */
		c->srtt = r * 8;
		c->rttvar = r * 2;
	} else { /* RFC 6298 in fixed point: srtt scaled by 8, rttvar by 4 */
		int32_t err = (int32_t)r - (int32_t)(c->srtt / 8);

		c->srtt = (uint32_t)((int32_t)c->srtt + err);
		c->rttvar = (uint32_t)((int32_t)c->rttvar + ((err < 0 ? -err : err) - (int32_t)(c->rttvar / 4)));
	}
	c->rto_ms = c->srtt / 8 + (c->rttvar > 4 ? c->rttvar : 4);
	if (c->rto_ms < MIN_RTO_MS)
		c->rto_ms = MIN_RTO_MS;
	if (c->rto_ms > MAX_RTO_MS)
		c->rto_ms = MAX_RTO_MS;
}

/* Take an acknowledgement: drop the acknowledged bytes from the send buffer and restart or stop the timer. */
static void process_ack(struct conn *c, uint32_t ack, uint32_t window)
{
	c->snd_wnd = window;
	if ((int32_t)(ack - c->snd_una) > 0 && (int32_t)(ack - c->snd_nxt) <= 0) {
		uint32_t acked = ack - c->snd_una, data = acked;

		if (c->state == TCP_SYN_RCVD || c->state == TCP_SYN_SENT) {
			data = 0; /* the SYN was acknowledged */
		} else if (c->fin_sent && ack == c->snd_nxt) {
			data = acked - 1; /* the FIN went with it */
		}
		if (data > c->tx_len)
			data = c->tx_len;
		c->tx_start = (c->tx_start + data) % TCP_TXBUF;
		c->tx_len -= data;
		c->snd_una = ack;
		rtt_sample(c, ack);
		c->retries = 0;
		if (c->snd_una == c->snd_nxt)
			c->rtx_active = 0;
		else
			rtx_arm(c);
	}
}

static void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
	const struct tcp_header *h = (const struct tcp_header *)seg;
	struct conn *c;
	uint16_t hlen, dlen;
	uint32_t seq, ack;
	const uint8_t *data;

	if (len < sizeof *h)
		return;
	hlen = (uint16_t)((h->data_off >> 4) * 4);
	if (hlen < sizeof *h || hlen > len || tcp_checksum(src, dst, seg, len) != 0) {
		stats.bad_checksum++;
		return;
	}
	if (drop_in) {
		drop_in--;
		stats.dropped_by_hook++;
		return;
	}
	stats.segments_received++;
	dlen = (uint16_t)(len - hlen);
	data = seg + hlen;
	seq = ntohl(h->seq);
	ack = ntohl(h->ack);

	mutex_lock(&lock);
	c = find(src, ntohs(h->src_port), ntohs(h->dst_port));
	if (!c) {
		struct conn *l = find_listener(ntohs(h->dst_port));

		if ((h->flags & (F_SYN | F_ACK | F_RST)) == F_SYN && l) { /* a client is calling a listening port */
			struct conn *n = l->nback < TCP_BACKLOG ? alloc_conn() : 0;

			if (n) {
				n->remote_ip = src;
				n->remote_port = ntohs(h->src_port);
				n->local_port = ntohs(h->dst_port);
				n->rcv_nxt = seq + 1;
				n->snd_wnd = ntohs(h->window);
				n->snd_nxt = n->snd_una = iss_counter += 64000 + timer_ticks();
				n->listener = (int)(l - conns);
				n->state = TCP_SYN_RCVD;
				send_segment(n, F_SYN | F_ACK, n->snd_nxt, 0, 0);
				n->snd_nxt++; /* the SYN takes a sequence number */
				rtx_arm(n);
			}
			mutex_unlock(&lock);
			return;
		}
		mutex_unlock(&lock);
		if (!(h->flags & F_RST))
			send_rst(src, h, dlen);
		return;
	}
	if (h->flags & F_RST) {
		if (c->state == TCP_SYN_RCVD) {
			free_conn(c);
		} else {
			c->refused = c->state == TCP_SYN_SENT;
			c->state = TCP_CLOSED;
			c->rtx_active = 0;
		}
		mutex_unlock(&lock);
		return;
	}

	if (c->state == TCP_SYN_SENT) {
		if ((h->flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == c->snd_nxt) {
			process_ack(c, ack, ntohs(h->window));
			c->rcv_nxt = seq + 1;
			c->state = TCP_ESTABLISHED;
			send_segment(c, F_ACK, c->snd_nxt, 0, 0); /* third step of the handshake */
		}
		mutex_unlock(&lock);
		return;
	}
	if (c->state == TCP_SYN_RCVD) { /* waiting for the third step of the handshake */
		struct conn *l = c->listener >= 0 ? &conns[c->listener] : 0;

		if (!(h->flags & F_ACK) || ack != c->snd_nxt) {
			mutex_unlock(&lock);
			return;
		}
		process_ack(c, ack, ntohs(h->window));
		if (!l || l->state != TCP_LISTEN || l->nback >= TCP_BACKLOG) { /* nobody is listening any more */
			free_conn(c);
			mutex_unlock(&lock);
			return;
		}
		c->state = TCP_ESTABLISHED;
		l->backlog[l->nback++] = (int)(c - conns);
		/* the ACK may already carry data: carry on below as for any established connection */
	}
	if (c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT_1 && c->state != TCP_FIN_WAIT_2 && c->state != TCP_LAST_ACK
	    && c->state != TCP_CLOSE_WAIT) {
		mutex_unlock(&lock);
		return;
	}

	if (h->flags & F_ACK)
		process_ack(c, ack, ntohs(h->window));
	if (c->state == TCP_FIN_WAIT_1 && c->fin_sent && c->snd_una == c->snd_nxt)
		c->state = TCP_FIN_WAIT_2;
	if (c->state == TCP_LAST_ACK && c->fin_sent && c->snd_una == c->snd_nxt) {
		c->state = TCP_CLOSED;
		mutex_unlock(&lock);
		return;
	}

	/* data: take what is next in sequence (and fits); anything else is re-acknowledged so the sender can resync */
	if (dlen) {
		uint32_t skip = 0, room = rx_window(c), take;

		if ((int32_t)(seq - c->rcv_nxt) <= 0 && (int32_t)(seq + dlen - c->rcv_nxt) > 0) {
			skip = c->rcv_nxt - seq; /* a retransmission that overlaps what we already have */
			take = dlen - skip;
			if (take > room)
				take = room;
			while (take--) {
				c->rx[c->rx_head] = data[skip++];
				c->rx_head = (c->rx_head + 1) % TCP_RXBUF;
				c->rcv_nxt++;
			}
		}
	}
	if ((h->flags & F_FIN) && seq + dlen == c->rcv_nxt) {
		c->rcv_nxt++;
		c->fin_received = 1;
		if (c->state == TCP_ESTABLISHED)
			c->state = TCP_CLOSE_WAIT;
		else if (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2)
			c->state = TCP_TIME_WAIT;
	}
	if (dlen || (h->flags & F_FIN))
		send_segment(c, F_ACK, c->snd_nxt, 0, 0);
	tcp_output(c); /* the ACK may have opened the window */
	mutex_unlock(&lock);
}

/* ---- the application interface ---- */

/* Wait without holding the lock; cond is evaluated with it held. */
static int wait_for(int (*cond)(struct conn *), struct conn *c, uint32_t timeout_ms)
{
	uint32_t deadline = timer_ticks() + ms_to_ticks(timeout_ms);

	for (;;) {
		int ok;

		mutex_lock(&lock);
		ok = cond(c);
		mutex_unlock(&lock);
		if (ok)
			return 0;
		if ((int32_t)(deadline - timer_ticks()) <= 0)
			return -1;
		task_sleep(5);
	}
}

static int cond_established(struct conn *c)
{
	return c->state != TCP_SYN_SENT;
}

int tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms)
{
	struct conn *c;
	int id;

	if (!netif.up)
		return -3;
	mutex_lock(&lock);
	c = alloc_conn();
	if (!c) {
		mutex_unlock(&lock);
		return -3;
	}
	c->remote_ip = ip;
	c->remote_port = port;
	c->local_port = next_port++;
	c->snd_nxt = c->snd_una = iss_counter += 64000 + timer_ticks();
	c->snd_wnd = MSS;
	c->state = TCP_SYN_SENT;
	id = (int)(c - conns);
	send_segment(c, F_SYN, c->snd_nxt, 0, 0);
	c->snd_nxt++; /* the SYN takes a sequence number; the timer task resends it if needed */
	rtx_arm(c);
	mutex_unlock(&lock);

	wait_for(cond_established, c, timeout_ms);
	mutex_lock(&lock);
	if (c->state != TCP_ESTABLISHED) {
		int refused = c->refused;

		free_conn(c);
		mutex_unlock(&lock);
		return refused ? -2 : -1;
	}
	mutex_unlock(&lock);
	return id;
}

static int cond_tx_space(struct conn *c)
{
	return c->tx_len < TCP_TXBUF || c->state == TCP_CLOSED;
}

/* Queue data for sending (blocking while the send buffer is full). 0 when everything is queued: acknowledgement comes
   later, see tcp_flush(). */
int tcp_send(int conn, const void *data, uint32_t len, uint32_t timeout_ms)
{
	struct conn *c;
	const uint8_t *src = data;
	uint32_t deadline = timer_ticks() + ms_to_ticks(timeout_ms);

	if (conn < 0 || conn >= TCP_MAX_CONN || !conns[conn].used)
		return -1;
	c = &conns[conn];
	while (len) {
		uint32_t n, i, left_ms = (int32_t)(deadline - timer_ticks()) > 0 ? (deadline - timer_ticks()) * 1000 / timer_hz() : 0;

		if (wait_for(cond_tx_space, c, left_ms))
			return -1;
		mutex_lock(&lock);
		if ((c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) || c->fin_queued) {
			mutex_unlock(&lock);
			return -1;
		}
		n = TCP_TXBUF - c->tx_len;
		if (n > len)
			n = len;
		for (i = 0; i < n; i++)
			c->tx[(c->tx_start + c->tx_len + i) % TCP_TXBUF] = src[i];
		c->tx_len += n;
		src += n;
		len -= n;
		tcp_output(c);
		mutex_unlock(&lock);
	}
	return 0;
}

static int cond_flushed(struct conn *c)
{
	return c->tx_len == 0 || c->state == TCP_CLOSED;
}

/* Wait until every queued byte has been acknowledged. 0, or -1 on timeout / a dead connection. */
int tcp_flush(int conn, uint32_t timeout_ms)
{
	struct conn *c;

	if (conn < 0 || conn >= TCP_MAX_CONN || !conns[conn].used)
		return -1;
	c = &conns[conn];
	if (wait_for(cond_flushed, c, timeout_ms))
		return -1;
	return c->state == TCP_CLOSED ? -1 : 0;
}

static int cond_data(struct conn *c)
{
	return rx_count(c) > 0 || c->fin_received || c->state == TCP_CLOSED;
}

int tcp_recv(int conn, void *buf, uint16_t cap, uint32_t timeout_ms)
{
	struct conn *c;
	uint8_t *out = buf;
	uint16_t n = 0;
	if (conn < 0 || conn >= TCP_MAX_CONN || !conns[conn].used)
		return -1;
	c = &conns[conn];
	if (wait_for(cond_data, c, timeout_ms))
		return -1;
	mutex_lock(&lock);
	while (n < cap && rx_count(c) > 0) {
		out[n++] = c->rx[c->rx_tail];
		c->rx_tail = (c->rx_tail + 1) % TCP_RXBUF;
	}
	/* The application made room. The sender only knows the window we last advertised: when the real one has grown by
	   a segment (or half the buffer) since, say so, or a sender that was held back by a small window would wait forever
	   (receiver-side silly window avoidance, RFC 1122 4.2.3.3). */
	if (n && c->state != TCP_CLOSED && rx_window(c) >= c->last_adv + (MSS < TCP_RXBUF / 2 ? MSS : TCP_RXBUF / 2))
		send_segment(c, F_ACK, c->snd_nxt, 0, 0);
	mutex_unlock(&lock);
	return n; /* 0 with nothing buffered = the peer has closed */
}

int tcp_listen(uint16_t port)
{
	struct conn *c;
	int id = -1;

	if (!port || !netif.up)
		return -1;
	mutex_lock(&lock);
	if (!find_listener(port) && (c = alloc_conn())) {
		c->local_port = port;
		c->state = TCP_LISTEN;
		id = (int)(c - conns);
	}
	mutex_unlock(&lock);
	return id;
}

static int cond_backlog(struct conn *c)
{
	return c->nback > 0 || c->state != TCP_LISTEN;
}

int tcp_accept(int listener, uint32_t timeout_ms, uint32_t *peer_ip, uint16_t *peer_port)
{
	struct conn *l, *n;
	int id, i;

	if (listener < 0 || listener >= TCP_MAX_CONN || !conns[listener].used || conns[listener].state != TCP_LISTEN)
		return -1;
	l = &conns[listener];
	if (wait_for(cond_backlog, l, timeout_ms))
		return -1;
	mutex_lock(&lock);
	if (l->state != TCP_LISTEN || !l->nback) {
		mutex_unlock(&lock);
		return -1;
	}
	id = l->backlog[0];
	for (i = 1; i < l->nback; i++)
		l->backlog[i - 1] = l->backlog[i];
	l->nback--;
	n = &conns[id];
	n->listener = -1;
	if (peer_ip)
		*peer_ip = n->remote_ip;
	if (peer_port)
		*peer_port = n->remote_port;
	mutex_unlock(&lock);
	return id;
}

static int cond_closed(struct conn *c)
{
	return c->state == TCP_CLOSED || c->state == TCP_TIME_WAIT || c->state == TCP_FIN_WAIT_2
	       || (c->fin_sent && c->snd_una == c->snd_nxt);
}

int tcp_close(int conn)
{
	struct conn *c;
	int rc = 0;

	if (conn < 0 || conn >= TCP_MAX_CONN || !conns[conn].used)
		return -1;
	c = &conns[conn];
	mutex_lock(&lock);
	if (c->state == TCP_LISTEN) { /* stop listening; connections that were already established stay usable */
		int i;

		for (i = 0; i < c->nback; i++)
			free_conn(&conns[c->backlog[i]]);
		free_conn(c);
		mutex_unlock(&lock);
		return 0;
	}
	if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
		c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
		c->fin_queued = 1; /* sent after the queued data has gone out */
		tcp_output(c);
		mutex_unlock(&lock);
		if (wait_for(cond_closed, c, 4000))
			rc = -1;
		task_sleep(100); /* TIME_WAIT is skipped: the peer's FIN, if it comes late, is still acknowledged below */
		mutex_lock(&lock);
	}
	free_conn(c);
	mutex_unlock(&lock);
	return rc;
}

void tcp_init(void)
{
	mutex_init(&lock);
	ip_register_proto(IP_PROTO_TCP, tcp_input);
	task_create("tcp-timer", timer_task, 0, PRIO_DEFAULT);
}

/* tcpserve PORT [CLIENTS] : an echo server for testing: replies "echo: <what you sent>" to each message until the client closes */
int cmd_tcpserve(int argc, char **argv)
{
	uint32_t port, clients = 1, peer;
	uint16_t pport;
	char buf[256], reply[300], s[16];
	int l, c, n;

	if (argc < 2 || kstrtoul(argv[1], &port) || !port || port > 65535 || (argc > 2 && kstrtoul(argv[2], &clients))) {
		console_write("usage: tcpserve PORT [CLIENTS]\n");
		return 1;
	}
	l = tcp_listen((uint16_t)port);
	if (l < 0) {
		console_write("tcpserve: cannot listen (port in use or no network)\n");
		return 1;
	}
	console_printf("listening on port %u\n", (uint32_t)port);
	while (clients--) {
		c = tcp_accept(l, 30000, &peer, &pport);
		if (c < 0) {
			console_write("tcpserve: no client\n");
			break;
		}
		console_printf("client %s:%u connected\n", ip_str(peer, s), pport);
		while ((n = tcp_recv(c, buf, sizeof buf - 1, 10000)) > 0) {
			buf[n] = '\0';
			n = ksnprintf(reply, sizeof reply, "echo: %s", buf);
			tcp_send(c, reply, (uint32_t)n, 3000);
		}
		tcp_close(c);
		console_write("client gone\n");
	}
	tcp_close(l);
	return 0;
}

/* tcpstat : counters and per-connection timers; tcpstat drop OUT IN : lose the next OUT sent / IN received segments; tcpstat reset */
int cmd_tcpstat(int argc, char **argv)
{
	struct tcp_stats s;
	uint32_t out, in;
	int i;

	if (argc >= 4 && !kstrcmp(argv[1], "drop") && !kstrtoul(argv[2], &out) && !kstrtoul(argv[3], &in)) {
		uint32_t skip = 0;

		if (argc > 4)
			kstrtoul(argv[4], &skip);
		tcp_drop_next(out, in, skip);
		console_printf("the next %u outgoing (after %u that pass) and %u incoming segment(s) will be lost\n", out, skip, in);
		return 0;
	}
	tcp_get_stats(&s);
	console_printf("tcp: %u segments sent, %u received, %u retransmissions, %u window probes, %u connections aborted\n", s.segments_sent,
		       s.segments_received, s.retransmits, s.window_probes, s.aborted);
	console_printf("     %u bad checksums, %u resets sent, %u segments dropped by the test hook\n", s.bad_checksum, s.resets_sent,
		       s.dropped_by_hook);
	for (i = 0; i < TCP_MAX_CONN; i++) {
		struct tcp_conn_stats cs;
		uint32_t ip;
		uint16_t rp, lp;
		enum tcp_state st;
		char ips[16];

		if (tcp_conn_info(i, &ip, &rp, &lp, &st) || tcp_conn_stats(i, &cs))
			continue;
		console_printf("  %u -> %s:%u %-11s srtt %ums rto %ums peer window %u, %u in flight, %u queued, %u retransmits, %u buffered for the app, advertised %u\n", lp,
			       ip_str(ip, ips), rp, tcp_state_name(st), cs.srtt_ms, cs.rto_ms, cs.peer_window, cs.in_flight, cs.queued,
			       cs.retransmits, cs.rx_buffered, cs.advertised);
	}
	return 0;
}

/* tcpsend IP PORT BYTES : send BYTES of the pattern i % 251 to a server, wait until they are acknowledged */
int cmd_tcpsend(int argc, char **argv)
{
	uint32_t ip, port, bytes, sent = 0, t0 = timer_ticks();
	uint8_t chunk[1000];
	int c, rc = 0;

	if (argc != 4 || ip_parse(argv[1], &ip) || kstrtoul(argv[2], &port) || kstrtoul(argv[3], &bytes) || !port || port > 65535) {
		console_write("usage: tcpsend IP PORT BYTES\n");
		return 1;
	}
	c = tcp_connect(ip, (uint16_t)port, 4000);
	if (c < 0) {
		console_printf("tcpsend: connect failed (%d)\n", c);
		return 1;
	}
	while (sent < bytes) {
		uint32_t n = bytes - sent < sizeof chunk ? bytes - sent : (uint32_t)sizeof chunk, i;

		for (i = 0; i < n; i++)
			chunk[i] = (uint8_t)((sent + i) % 251);
		if (tcp_send(c, chunk, n, 10000)) {
			rc = 1;
			break;
		}
		sent += n;
	}
	if (!rc && tcp_flush(c, 15000))
		rc = 1;
	{
		struct tcp_conn_stats cs;

		if (!tcp_conn_stats(c, &cs))
			console_printf("tcpsend: %u bytes in %u ticks, %u retransmissions, peer window %u, srtt %u ms%s\n", sent, timer_ticks() - t0,
				       cs.retransmits, cs.peer_window, cs.srtt_ms, rc ? ", FAILED" : "");
	}
	tcp_close(c);
	return rc;
}

/* tcpget IP PORT [DELAY_MS] : read a stream until the server closes (sleeping DELAY_MS after each read to make the
   receive window fill up) and check the pattern i % 251 */
int cmd_tcpget(int argc, char **argv)
{
	uint32_t ip, port, delay = 0, total = 0, bad = 0, t0 = timer_ticks();
	uint8_t buf[512];
	int c, n;

	if (argc < 3 || ip_parse(argv[1], &ip) || kstrtoul(argv[2], &port) || !port || port > 65535 || (argc > 3 && kstrtoul(argv[3], &delay))) {
		console_write("usage: tcpget IP PORT [DELAY_MS]\n");
		return 1;
	}
	c = tcp_connect(ip, (uint16_t)port, 4000);
	if (c < 0) {
		console_printf("tcpget: connect failed (%d)\n", c);
		return 1;
	}
	while ((n = tcp_recv(c, buf, sizeof buf, 10000)) > 0) {
		int i;

		for (i = 0; i < n; i++)
			bad += buf[i] != (uint8_t)((total + (uint32_t)i) % 251);
		total += (uint32_t)n;
		if (delay)
			task_sleep(delay);
	}
	{
		struct tcp_conn_stats cs;

		if (!tcp_conn_stats(c, &cs))
			console_printf("tcpget: %u bytes in %u ticks, %u wrong, %u retransmissions\n", total, timer_ticks() - t0, bad, cs.retransmits);
	}
	tcp_close(c);
	return bad != 0;
}
