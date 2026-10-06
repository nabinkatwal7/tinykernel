#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/*
 * A deliberately small TCP: active and passive open (the three-way handshake from either side), one segment
 * in flight, in-order receive only, no congestion control, one retransmission.
 */
#define TCP_MAX_CONN 8
#define TCP_TXBUF    8192   /* send buffer per connection */
#define TCP_BACKLOG  4      /* connections that completed the handshake but were not accepted yet, per listener */
#define TCP_RXBUF    4096

enum tcp_state {
	TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT,
	TCP_LAST_ACK, TCP_TIME_WAIT, TCP_LISTEN, TCP_SYN_RCVD,
};

void tcp_init(void);
int  tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);    /* connection id >= 0, or -1 (no reply), -2 (refused), -3 (no slot/route) */
int  tcp_send(int conn, const void *data, uint32_t len, uint32_t timeout_ms);  /* 0 when all of it is queued (blocks while the send buffer is full), -1 on error */
int  tcp_flush(int conn, uint32_t timeout_ms);                                 /* wait until everything queued has been acknowledged; 0 or -1 */
int  tcp_recv(int conn, void *buf, uint16_t cap, uint32_t timeout_ms);        /* bytes, 0 = peer closed, -1 = timeout */
/* Server side: tcp_listen() claims a local port; tcp_accept() waits for a client and returns its connection,
   which is used exactly like one from tcp_connect(). tcp_close() on the listener stops listening. */
int  tcp_listen(uint16_t port);                                        /* listener id >= 0, or -1 (port busy / no slot) */
int  tcp_accept(int listener, uint32_t timeout_ms, uint32_t *peer_ip, uint16_t *peer_port); /* connection id, or -1 on timeout */
int  tcp_close(int conn);                                              /* FIN handshake; 0 on a clean close */
int  cmd_tcpstat(int argc, char **argv);    /* tcpstat [drop OUT IN [SKIP]] */
int  cmd_tcpserve(int argc, char **argv);   /* tcpserve PORT [CLIENTS]: echo server for testing */
/* Statistics and test hooks. */
struct tcp_stats {
	uint32_t segments_sent, segments_received, retransmits, aborted, bad_checksum, resets_sent, window_probes, dropped_by_hook;
};
struct tcp_conn_stats {
	uint32_t rto_ms, srtt_ms, peer_window, in_flight, queued, retransmits, window_probes;
};
void tcp_get_stats(struct tcp_stats *out);
int  tcp_conn_stats(int conn, struct tcp_conn_stats *out);
void tcp_drop_next(uint32_t outgoing, uint32_t incoming, uint32_t skip);   /* silently discard the next N segments we send (after letting SKIP pass) / receive: loss simulation */
enum tcp_state tcp_state(int conn);
const char *tcp_state_name(enum tcp_state s);
/* For listing: 0 if the slot is in use. */
int  tcp_conn_info(int conn, uint32_t *ip, uint16_t *rport, uint16_t *lport, enum tcp_state *st);

#endif
