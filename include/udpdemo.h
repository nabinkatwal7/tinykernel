#ifndef UDPDEMO_H
#define UDPDEMO_H

/* udpecho [PORT [COUNT]] and udpchat LOCAL_PORT PEER_IP PEER_PORT: small UDP demos (kernel/udpdemo.c) */
int cmd_udpecho(int argc, char **argv);
int cmd_udpchat(int argc, char **argv);

#endif
