#include "ethernet.h"
#include "icmpv4.h"
#include "ip.h"
#include "utils.h"

void icmpv4_incoming(struct sk_buff *skb) 
{
    struct iphdr *iphdr = ip_hdr(skb);
    struct icmp_v4 *icmp = (struct icmp_v4 *) iphdr->data;

    uint16_t icmp_len = ip_len(iphdr);

    if (icmp_len < sizeof(struct icmp_v4) || checksum(icmp, icmp_len, 0) != 0) {
        print_err("ICMPv4 dropped: bad length or checksum\n");
        goto drop_pkt;
    }

    switch (icmp->type) {
    case ICMP_V4_ECHO:
        icmpv4_reply(skb);
        return;
    case ICMP_V4_DST_UNREACHABLE:
        print_err("ICMPv4 received 'dst unreachable' code %d, "
                  "check your routes and firewall rules\n", icmp->code);
        goto drop_pkt;
    default:
        print_err("ICMPv4 did not match supported types\n");
        goto drop_pkt;
    }

drop_pkt:
    free_skb(skb);
    return;
}

/*
 * Sends Destination Unreachable for a received datagram whose header is in
 * host byte order. Quotes the original header plus 8 payload bytes (RFC 792).
 * Never answers ICMP errors, so two stacks cannot loop on each other.
 */
void icmpv4_send_unreachable(struct sk_buff *in, uint8_t code)
{
    struct iphdr *orig = ip_hdr(in);
    uint32_t hlen = orig->ihl * 4;
    uint32_t quoted = hlen + min(8, ip_len(orig));

    if (orig->proto == ICMPV4) {
        struct icmp_v4 *icmp = (struct icmp_v4 *)((uint8_t *)orig + hlen);
        if (icmp->type != ICMP_V4_ECHO && icmp->type != ICMP_V4_REPLY) return;
    }

    uint32_t icmp_len = sizeof(struct icmp_v4) + sizeof(struct icmp_v4_dst_unreachable) + quoted;
    struct sk_buff *skb = alloc_skb(ETH_HDR_LEN + IP_HDR_LEN + icmp_len);

    skb_reserve(skb, ETH_HDR_LEN + IP_HDR_LEN + icmp_len);
    struct icmp_v4 *icmp = (struct icmp_v4 *)skb_push(skb, icmp_len);
    struct icmp_v4_dst_unreachable *unreach = (struct icmp_v4_dst_unreachable *)icmp->data;

    icmp->type = ICMP_V4_DST_UNREACHABLE;
    icmp->code = code;

    struct iphdr *copy = (struct iphdr *)unreach->data;
    memcpy(copy, orig, quoted);
    copy->len = htons(copy->len);
    copy->id = htons(copy->id);
    copy->frag_offset = htons(copy->frag_offset);
    copy->saddr = htonl(copy->saddr);
    copy->daddr = htonl(copy->daddr);

    icmp->csum = checksum(icmp, icmp_len, 0);

    skb->protocol = ICMPV4;
    ip_output_daddr(orig->saddr, skb, 1);
    free_skb(skb);
}

void icmpv4_reply(struct sk_buff *skb)
{
    struct iphdr *iphdr = ip_hdr(skb);
    struct icmp_v4 *icmp;
    struct sock sk;
    memset(&sk, 0, sizeof(struct sock));
    
    uint16_t icmp_len = iphdr->len - (iphdr->ihl * 4);

    /* The rx buffer is reused for the reply, rebuilt from an empty length */
    skb->len = 0;
    skb_reserve(skb, ETH_HDR_LEN + IP_HDR_LEN + icmp_len);
    skb_push(skb, icmp_len);
    
    icmp = (struct icmp_v4 *)skb->data;
        
    icmp->type = ICMP_V4_REPLY;
    icmp->csum = 0;
    icmp->csum = checksum(icmp, icmp_len, 0);

    skb->protocol = ICMPV4;
    sk.daddr = iphdr->saddr;

    ip_output(&sk, skb);
    free_skb(skb);
}
