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
	uint32_t tx_frames, tx_errors, rx_frames, rx_errors, rx_dropped;
};

extern struct netif netif;

int  net_send_frame(const void *frame, uint16_t len);   /* a complete Ethernet frame (no FCS); 0 on success */
int  rtl8139_init(void);                            /* 0 if a card was found and started */
int  rtl8139_link_up(void);
/* Build and send an Ethernet II frame: dst MAC, our MAC, ethertype, payload. */
int  eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype, const void *payload, uint16_t len);
const char *mac_str(const uint8_t mac[ETH_ALEN], char out[18]);

#endif
