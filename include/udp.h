#ifndef UDP_H
#define UDP_H

#include <stdint.h>

#define UDP_MAX_SOCKETS 8
#define UDP_QUEUE       8      /* datagrams buffered per socket */
#define UDP_MAX_PAYLOAD 1400

void udp_init(void);

/* Sockets: bind a local port, then sendto/recvfrom. Port 0 picks an ephemeral port. */
int  udp_open(uint16_t port);                       /* socket id >= 0, or -1 (port taken / no slots) */
void udp_close(int sock);
int  udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void *data, uint16_t len);
/* Wait up to timeout_ms for a datagram. Returns its length (may be truncated to cap), or -1 on timeout. */
int  udp_recvfrom(int sock, void *buf, uint16_t cap, uint32_t *src_ip, uint16_t *src_port, uint32_t timeout_ms);
uint16_t udp_local_port(int sock);

struct udp_stats {
	uint32_t rx, tx, rx_no_socket, rx_bad_checksum, rx_dropped;
};
void udp_get_stats(struct udp_stats *out);

#endif
