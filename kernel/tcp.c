#include "tcp.h"

#include "console.h"
#include "io.h"
#include "ip.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "sched.h"
#include "timer.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define MSS 1400

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
	uint32_t snd_nxt;     /* next sequence number we will send */
	uint32_t snd_una;     /* oldest unacknowledged sequence number */
	uint32_t rcv_nxt;     /* next sequence number we expect */
	int refused;
	uint8_t *rx;          /* receive ring */
	uint32_t rx_head, rx_tail;
	int fin_received;
	int backlog[TCP_BACKLOG];   /* LISTEN: established connections waiting for tcp_accept() */
	int nback;
	int listener;               /* SYN_RCVD / accepted: the listener that spawned it, or -1 */
};

static struct conn conns[TCP_MAX_CONN];
static uint16_t next_port = 40000;
static uint32_t iss_counter = 0x1000;

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

static int send_segment(struct conn *c, uint8_t flags, uint32_t seq, const void *data, uint16_t len)
{
	uint8_t pkt[sizeof(struct tcp_header) + MSS];
	struct tcp_header *h = (struct tcp_header *)pkt;
	uint32_t src = netif.ip;

	if (len > MSS)
		return -1;
	h->src_port = htons(c->local_port);
	h->dst_port = htons(c->remote_port);
	h->seq = htonl(seq);
	h->ack = htonl(flags & F_ACK ? c->rcv_nxt : 0);
	h->data_off = (uint8_t)(5 << 4);
	h->flags = flags;
	h->window = htons(TCP_RXBUF);
	h->checksum = 0;
	h->urgent = 0;
	memcpy(pkt + sizeof *h, data, len);
	h->checksum = htons(tcp_checksum(src, c->remote_ip, pkt, (uint16_t)(sizeof *h + len)));
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
	uint32_t f = irq_save();
	int i;

	for (i = 0; i < TCP_MAX_CONN && !c; i++)
		if (!conns[i].used)
			c = &conns[i];
	if (c) {
		memset(c, 0, sizeof *c);
		c->listener = -1;
		c->used = 1; /* claimed before the buffer is allocated */
	}
	irq_restore(f);
	return c;
}

static void free_conn(struct conn *c)
{
	kfree(c->rx);
	c->rx = 0;
	c->used = 0;
	c->state = TCP_CLOSED;
}

static uint32_t rx_count(const struct conn *c)
{
	return (c->rx_head - c->rx_tail) % TCP_RXBUF;
}

static void rx_push(struct conn *c, const uint8_t *data, uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; i++) {
		if (rx_count(c) == TCP_RXBUF - 1)
			break; /* full: the rest is dropped (we advertise a window we cannot keep; fine for a demo) */
		c->rx[c->rx_head] = data[i];
		c->rx_head = (c->rx_head + 1) % TCP_RXBUF;
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
	if (hlen < sizeof *h || hlen > len || tcp_checksum(src, dst, seg, len) != 0)
		return;
	dlen = (uint16_t)(len - hlen);
	data = seg + hlen;
	seq = ntohl(h->seq);
	ack = ntohl(h->ack);

	c = find(src, ntohs(h->src_port), ntohs(h->dst_port));
	if (!c) {
		struct conn *l = find_listener(ntohs(h->dst_port));

		if ((h->flags & (F_SYN | F_ACK | F_RST)) == F_SYN && l) { /* a client is calling a listening port */
			struct conn *n = l->nback < TCP_BACKLOG ? alloc_conn() : 0;

			if (n) {
				n->rx = kmalloc(TCP_RXBUF);
				if (!n->rx) {
					n->used = 0;
					return;
				}
				n->remote_ip = src;
				n->remote_port = ntohs(h->src_port);
				n->local_port = ntohs(h->dst_port);
				n->rcv_nxt = seq + 1;
				n->snd_nxt = n->snd_una = iss_counter += 64000 + timer_ticks();
				n->listener = (int)(l - conns);
				n->state = TCP_SYN_RCVD;
				send_segment(n, F_SYN | F_ACK, n->snd_nxt, 0, 0);
				n->snd_nxt++; /* the SYN takes a sequence number */
			}
			return; /* no room: ignore the SYN, the client will retry */
		}
		if (!(h->flags & F_RST))
			send_rst(src, h, dlen);
		return;
	}
	if (h->flags & F_RST) {
		if (c->state == TCP_SYN_RCVD) { /* the half-open connection is simply dropped */
			free_conn(c);
			return;
		}
		c->refused = c->state == TCP_SYN_SENT;
		c->state = TCP_CLOSED;
		return;
	}
	if (c->state == TCP_SYN_RCVD) { /* waiting for the third step of the handshake */
		struct conn *l = c->listener >= 0 ? &conns[c->listener] : 0;

		if (!(h->flags & F_ACK) || ack != c->snd_nxt)
			return;
		c->snd_una = ack;
		if (!l || l->state != TCP_LISTEN || l->nback >= TCP_BACKLOG) { /* nobody is listening any more */
			free_conn(c);
			return;
		}
		c->state = TCP_ESTABLISHED;
		l->backlog[l->nback++] = (int)(c - conns);
		/* the ACK may already carry data: carry on below as for any established connection */
	}

	switch (c->state) {
	case TCP_SYN_SENT:
		if ((h->flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == c->snd_nxt + 1) { /* the SYN used one number */
			c->snd_nxt = ack;
			c->snd_una = ack;
			c->rcv_nxt = seq + 1;
			c->state = TCP_ESTABLISHED;
			send_segment(c, F_ACK, c->snd_nxt, 0, 0); /* third step of the handshake */
		}
		return;
	case TCP_ESTABLISHED:
	case TCP_FIN_WAIT_1:
	case TCP_FIN_WAIT_2:
	case TCP_LAST_ACK:
		break;
	default:
		return;
	}

	if ((h->flags & F_ACK) && (int32_t)(ack - c->snd_una) > 0 && (int32_t)(ack - c->snd_nxt) <= 0)
		c->snd_una = ack;
	if (c->state == TCP_FIN_WAIT_1 && c->snd_una == c->snd_nxt)
		c->state = TCP_FIN_WAIT_2;
	if (c->state == TCP_LAST_ACK && c->snd_una == c->snd_nxt) {
		c->state = TCP_CLOSED;
		return;
	}

	if (dlen && seq == c->rcv_nxt) { /* in-order data only; anything else is simply re-acknowledged */
		rx_push(c, data, dlen);
		c->rcv_nxt += dlen;
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
}

static int wait_for(int (*cond)(struct conn *), struct conn *c, uint32_t timeout_ms)
{
	uint32_t deadline = timer_ticks() + timeout_ms * timer_hz() / 1000;

	while (!cond(c)) {
		if ((int32_t)(deadline - timer_ticks()) <= 0)
			return -1;
		task_sleep(10);
	}
	return 0;
}

static int cond_established(struct conn *c)
{
	return c->state != TCP_SYN_SENT;
}

int tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms)
{
	struct conn *c;
	int attempt;

	c = netif.up ? alloc_conn() : 0;
	if (!c)
		return -3;
	c->rx = kmalloc(TCP_RXBUF);
	if (!c->rx) {
		c->used = 0;
		return -3;
	}
	c->remote_ip = ip;
	c->remote_port = port;
	c->local_port = next_port++;
	c->snd_nxt = iss_counter += 64000 + timer_ticks();
	c->snd_una = c->snd_nxt;
	c->state = TCP_SYN_SENT;

	for (attempt = 0; attempt < 2 && c->state == TCP_SYN_SENT; attempt++) {
		/* SYN occupies one sequence number */
		send_segment(c, F_SYN, c->snd_nxt, 0, 0);
		wait_for(cond_established, c, timeout_ms / 2);
	}
	if (c->state != TCP_ESTABLISHED) {
		int refused = c->refused;

		c->used = 0;
		kfree(c->rx);
		return refused ? -2 : -1;
	}
	return (int)(c - conns);
}

static int cond_acked(struct conn *c)
{
	return c->snd_una == c->snd_nxt || c->state == TCP_CLOSED;
}

int tcp_send(int conn, const void *data, uint16_t len, uint32_t timeout_ms)
{
	struct conn *c;
	uint32_t seq;
	int attempt;

	if (conn < 0 || conn >= TCP_MAX_CONN || !conns[conn].used)
		return -1;
	c = &conns[conn];
	if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
		return -1;
	seq = c->snd_nxt;
	c->snd_nxt += len;
	for (attempt = 0; attempt < 2; attempt++) { /* one retransmission */
		send_segment(c, F_PSH | F_ACK, seq, data, len);
		if (!wait_for(cond_acked, c, timeout_ms / 2) && c->snd_una == c->snd_nxt)
			return 0;
	}
	return -1;
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
	while (n < cap && rx_count(c) > 0) {
		out[n++] = c->rx[c->rx_tail];
		c->rx_tail = (c->rx_tail + 1) % TCP_RXBUF;
	}
	return n; /* 0 with nothing buffered = the peer has closed */
}

static int cond_closed(struct conn *c)
{
	return c->state == TCP_CLOSED || c->state == TCP_TIME_WAIT || c->state == TCP_FIN_WAIT_2;
}

int tcp_listen(uint16_t port)
{
	struct conn *c;

	if (!port || !netif.up || find_listener(port))
		return -1;
	c = alloc_conn();
	if (!c)
		return -1;
	c->local_port = port;
	c->state = TCP_LISTEN;
	return (int)(c - conns);
}

static int cond_backlog(struct conn *c)
{
	return c->nback > 0 || c->state != TCP_LISTEN;
}

int tcp_accept(int listener, uint32_t timeout_ms, uint32_t *peer_ip, uint16_t *peer_port)
{
	struct conn *l, *n;
	uint32_t f;
	int id, i;

	if (listener < 0 || listener >= TCP_MAX_CONN || !conns[listener].used || conns[listener].state != TCP_LISTEN)
		return -1;
	l = &conns[listener];
	if (wait_for(cond_backlog, l, timeout_ms) || l->state != TCP_LISTEN)
		return -1;
	f = irq_save();
	id = l->backlog[0];
	for (i = 1; i < l->nback; i++)
		l->backlog[i - 1] = l->backlog[i];
	l->nback--;
	irq_restore(f);
	n = &conns[id];
	n->listener = -1;
	if (peer_ip)
		*peer_ip = n->remote_ip;
	if (peer_port)
		*peer_port = n->remote_port;
	return id;
}

int tcp_close(int conn)
{
	struct conn *c;
	int rc = 0;

	if (conn < 0 || conn >= TCP_MAX_CONN || !conns[conn].used)
		return -1;
	c = &conns[conn];
	if (c->state == TCP_LISTEN) { /* stop listening; connections that were already established stay usable */
		int i;

		for (i = 0; i < c->nback; i++)
			tcp_close(c->backlog[i]);
		c->nback = 0;
		c->used = 0;
		c->state = TCP_CLOSED;
		return 0;
	}
	if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
		uint32_t seq = c->snd_nxt;

		c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
		c->snd_nxt++; /* FIN takes a sequence number */
		send_segment(c, F_FIN | F_ACK, seq, 0, 0);
		if (wait_for(cond_closed, c, 1500))
			rc = -1;
		/* we do not keep TIME_WAIT: the peer's FIN (if it comes) is still acknowledged by tcp_input */
		task_sleep(100);
	}
	c->state = TCP_CLOSED;
	c->used = 0;
	kfree(c->rx);
	return rc;
}

void tcp_init(void)
{
	ip_register_proto(IP_PROTO_TCP, tcp_input);
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
			tcp_send(c, reply, (uint16_t)n, 3000);
		}
		tcp_close(c);
		console_write("client gone\n");
	}
	tcp_close(l);
	return 0;
}
