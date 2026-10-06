#ifndef NET_H
#define NET_H

#include <stdint.h>

#define ETH_ALEN       6
#define ETH_HLEN       14
#define ETH_MIN_FRAME  60      /* without the FCS; shorter frames are padded */
#define ETH_MAX_FRAME  1514

/* One network interface (we only ever have the RTL8139). */
struct netif {
	int      up;                 /* driver initialised and link usable */
	uint8_t  mac[ETH_ALEN];
	uint16_t io_base;
	uint8_t  irq;
	uint32_t dns;                        /* resolver address (from DHCP), 0 if unknown */
	uint32_t ip, netmask, gateway;       /* IPv4, host byte order; 0 = not configured */
	uint32_t tx_frames, tx_errors, rx_frames, rx_errors, rx_dropped;
};

extern struct netif netif;

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
const char *ip_str(uint32_t ip, char out[16]);
int  ip_parse(const char *s, uint32_t *ip);      /* dotted quad -> host-order address; 0 on success */

void net_tx_stats(uint32_t *queue_full, uint32_t *max_depth);   /* times a sender found all four descriptors busy, deepest queue seen */
int  cmd_txtest(int argc, char **argv);                  /* txtest: queue 200 frames back to back */
int  net_send_frame(const void *frame, uint16_t len);   /* a complete Ethernet frame (no FCS); 0 on success */
/* Received frames are queued by the IRQ handler and delivered, in task context, to the handler
   registered for their ethertype. 'frame' includes the Ethernet header but not the FCS. */
typedef void (*eth_handler_t)(const uint8_t *frame, uint16_t len);
int  net_register_ethertype(uint16_t ethertype, eth_handler_t handler);
void net_set_loopback(int on);                /* card-level loopback: our own frames come back (self-test) */
void net_set_trace(int on);                   /* print a one-line summary of every received frame */
int  rtl8139_init(void);                            /* 0 if a card was found and started */
int  rtl8139_link_up(void);
/* Build and send an Ethernet II frame: dst MAC, our MAC, ethertype, payload. */
int  eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype, const void *payload, uint16_t len);
const char *mac_str(const uint8_t mac[ETH_ALEN], char out[18]);

#endif
