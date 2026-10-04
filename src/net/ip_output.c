#include "syshead.h"
#include "skbuff.h"
#include "utils.h"
#include "ip.h"
#include "dst.h"
#include "route.h"

static uint16_t ip_next_id(void)
{
    static uint16_t id = 0;

    return __atomic_add_fetch(&id, 1, __ATOMIC_RELAXED);
}

/* Converts a host-order header to network order and fills in the checksum */
static void ip_finalize_header(struct iphdr *ihdr)
{
    ip_dbg("out", ihdr);

    ihdr->len = htons(ihdr->len);
    ihdr->id = htons(ihdr->id);
    ihdr->daddr = htonl(ihdr->daddr);
    ihdr->saddr = htonl(ihdr->saddr);
    ihdr->frag_offset = htons(ihdr->frag_offset);
    ihdr->csum = 0;
    ihdr->csum = checksum(ihdr, ihdr->ihl * 4, 0);
}

/*
 * Splits an oversized datagram into fragments carrying a multiple of 8 payload
 * bytes each (RFC 791). The original skb is left for the caller to free.
 */
static int ip_fragment(struct sk_buff *skb, struct iphdr *orig)
{
    uint32_t mtu = skb->dev->mtu;
    uint32_t payload = orig->len - IP_HDR_LEN;
    uint32_t chunk_max = (mtu - IP_HDR_LEN) & ~7u;
    int rc = 0;

    for (uint32_t off = 0; off < payload; off += chunk_max) {
        uint32_t chunk = min(chunk_max, payload - off);
        int last = off + chunk == payload;
        struct sk_buff *frag = alloc_skb(ETH_HDR_LEN + IP_HDR_LEN + chunk);

        skb_reserve(frag, ETH_HDR_LEN + IP_HDR_LEN + chunk);
        memcpy(skb_push(frag, chunk), orig->data + off, chunk);

        struct iphdr *ihdr = (struct iphdr *)skb_push(frag, IP_HDR_LEN);
        memcpy(ihdr, orig, IP_HDR_LEN);
        ihdr->len = IP_HDR_LEN + chunk;
        ihdr->frag_offset = (last ? 0 : IP_FRAG_MF) | (off / 8);

        frag->dev = skb->dev;
        frag->rt = skb->rt;
        frag->protocol = skb->protocol;

        ip_finalize_header(ihdr);

        if (dst_neigh_output(frag) < 0) rc = -1;
        free_skb(frag);
    }

    return rc;
}

/*
 * Prepends an IPv4 header to skb->data and sends it towards daddr.
 * With dont_fragment, datagrams larger than the MTU are refused; otherwise
 * they are fragmented. Returns -1 if a route or neighbour is missing.
 */
int ip_output_daddr(uint32_t daddr, struct sk_buff *skb, int dont_fragment)
{
    struct rtentry *rt = route_lookup(daddr);

    if (!rt) {
        print_err("IP output route lookup fail\n");
        return -1;
    }

    skb->dev = rt->dev;
    skb->rt = rt;

    struct iphdr *ihdr = (struct iphdr *)skb_push(skb, IP_HDR_LEN);

    ihdr->version = IPV4;
    ihdr->ihl = 0x05;
    ihdr->tos = 0;
    ihdr->len = skb->len;
    ihdr->id = ip_next_id();
    ihdr->frag_offset = dont_fragment ? IP_FRAG_DF : 0;
    ihdr->ttl = 64;
    ihdr->proto = skb->protocol;
    ihdr->saddr = skb->dev->addr;
    ihdr->daddr = daddr;

    if (skb->len > skb->dev->mtu) {
        if (dont_fragment) return -EMSGSIZE;
        return ip_fragment(skb, ihdr);
    }

    ip_finalize_header(ihdr);

    return dst_neigh_output(skb);
}

int ip_output(struct sock *sk, struct sk_buff *skb)
{
    return ip_output_daddr(sk->daddr, skb, 1);
}
