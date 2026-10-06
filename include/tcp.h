#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/*
 * A deliberately small TCP: active open (the three-way handshake), one segment in flight, in-order
 * receive only, no congestion control, one retransmission. Enough to talk to a simple server.
 */
#define TCP_MAX_CONN 4
#define TCP_RXBUF    4096

enum tcp_state {
	TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT,
	TCP_LAST_ACK, TCP_TIME_WAIT,
};

void tcp_init(void);
int  tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);    /* connection id >= 0, or -1 (no reply), -2 (refused), -3 (no slot/route) */
int  tcp_send(int conn, const void *data, uint16_t len, uint32_t timeout_ms);  /* 0 once acknowledged, -1 otherwise */
int  tcp_recv(int conn, void *buf, uint16_t cap, uint32_t timeout_ms);        /* bytes, 0 = peer closed, -1 = timeout */
int  tcp_close(int conn);                                              /* FIN handshake; 0 on a clean close */
enum tcp_state tcp_state(int conn);
const char *tcp_state_name(enum tcp_state s);
/* For listing: 0 if the slot is in use. */
int  tcp_conn_info(int conn, uint32_t *ip, uint16_t *rport, uint16_t *lport, enum tcp_state *st);

#endif
