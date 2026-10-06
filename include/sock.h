#ifndef SOCK_H
#define SOCK_H

#include <stdint.h>

/* BSD-style sockets (kernel side). The user-visible interface is the SYS_SOCKET family in syscall.h. */
#define SOCK_STREAM 1   /* TCP */
#define SOCK_DGRAM  2   /* UDP */

struct sock;

struct sock *sock_new(int type);
void         sock_free(struct sock *s);                          /* closes the connection or releases the port */
int          sock_bind(struct sock *s, uint16_t port);
int          sock_listen(struct sock *s);
struct sock *sock_accept(struct sock *s, int *err);
int          sock_connect(struct sock *s, uint32_t ip, uint16_t port);
int          sock_recv(struct sock *s, void *buf, uint32_t cap, uint32_t *from_ip, uint16_t *from_port); /* bytes, 0 = closed, or FS_E* */
int          sock_send(struct sock *s, const void *buf, uint32_t len, uint32_t ip, uint16_t port);       /* bytes sent or FS_E* */
int          sock_resolve(const char *name, uint32_t *ip);

#endif
