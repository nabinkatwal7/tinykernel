# Network card basics

## Layers we implement, bottom up

| layer | what it does | here |
| --- | --- | --- |
| Ethernet II | frames of `dst MAC (6) | src MAC (6) | ethertype (2) | payload (46-1500) | FCS (4)`; the NIC adds padding and the FCS | `net.c` |
| ARP (`0x0806`) | "who has IP a.b.c.d?" -> a MAC address | `arp.c` |
| IPv4 (`0x0800`) | addressing and a header checksum | `ip.c` |
| ICMP (IP proto 1) | echo request/reply = `ping` | `icmp.c` |
| UDP (proto 17) | connectionless datagrams with ports | `udp.c` |
| TCP (proto 6) | reliable streams: the SYN / SYN-ACK / ACK handshake first | `tcp.c` |

All multi-byte header fields are big endian ("network byte order"); x86 is little endian, so every 16/32-bit field needs a byte swap.

## The RTL8139 (QEMU `-device rtl8139`)

A PCI device (vendor `10EC`, device `8139`). Its BAR0 is a 256-byte block of I/O ports:

| offset | register |
| --- | --- |
| `0x00` | `IDR0-5`: the MAC address |
| `0x10 + 4n` | `TSD0-3`: transmit status/size of descriptor n (bits 0-12 = length, bit 13 OWN = DMA finished, bit 15 TOK = sent OK) |
| `0x20 + 4n` | `TSAD0-3`: physical address of the packet to send |
| `0x30` | `RBSTART`: physical address of the receive ring |
| `0x37` | `CR`: bit 4 reset, bit 3 receiver enable, bit 2 transmitter enable, bit 0 receive buffer empty |
| `0x38` | `CAPR`: where the driver has read up to (minus 16) |
| `0x3C`, `0x3E` | `IMR`, `ISR`: interrupt mask and status (write 1 to acknowledge) |
| `0x40`, `0x44` | `TCR`, `RCR`: transmit/receive configuration (accept-all bits, wrap bit, loopback mode) |
| `0x52` | `CONFIG1`: write 0 to power the chip on |

Setup: enable PCI bus mastering (command bit 2) so the card can DMA, power on, reset, point `RBSTART` at an 8 KiB + 16 + 1500 byte ring in physical memory, program `RCR`, enable receive and transmit.

Received packets land in the ring as `u16 status, u16 length, data...` (length includes the 4-byte CRC), each padded to 4 bytes; the driver advances its read offset and writes it to `CAPR`. To send, copy the frame into a buffer, write its physical address to `TSAD`, then its length to `TSD` and wait for the OWN/TOK bits.
