#ifndef UDP_H_
#define UDP_H_

#include "syshead.h"
#include "skbuff.h"
#include "sock.h"

#define UDP_HDR_LEN sizeof(struct udphdr)
#define UDP_MAX_PAYLOAD (0xffff - 20 - UDP_HDR_LEN)
#define UDP_RCV_QLEN 64          /* datagrams queued per socket before dropping */

struct udphdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t csum;
    uint8_t data[];
} __attribute__((packed));

void udp_in(struct sk_buff *skb);

#endif
