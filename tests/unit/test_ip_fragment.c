/* Fragmentation round trip: ip_output_daddr -> captured fragments -> ip_reassemble */
#include "syshead.h"
#include "skbuff.h"
#include "netdev.h"
#include "route.h"
#include "ip.h"
#include "utils.h"

int timer_get_tick() { return 0; }
int debug = 0;

static struct netdev dev = { .addr = 0x0a000004, .addr_len = 6, .mtu = 1500 };
static struct rtentry rt = { .dev = &dev };
struct rtentry *route_lookup(uint32_t daddr) { return &rt; }

static struct sk_buff *captured[64];
static int ncaptured = 0;

/* Stand-in for the neighbour layer: keep a copy of each outgoing datagram */
int dst_neigh_output(struct sk_buff *skb)
{
    struct sk_buff *copy = alloc_skb(ETH_HDR_LEN + skb->len);

    memcpy(copy->data + ETH_HDR_LEN, skb->data, skb->len);
    copy->len = ETH_HDR_LEN + skb->len;
    captured[ncaptured++] = copy;
    return 0;
}

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void to_host_order(struct iphdr *ih)
{
    ih->len = ntohs(ih->len);
    ih->id = ntohs(ih->id);
    ih->frag_offset = ntohs(ih->frag_offset);
    ih->saddr = ntohl(ih->saddr);
    ih->daddr = ntohl(ih->daddr);
}

int main()
{
    const uint32_t size = 4000;
    struct sk_buff *skb = alloc_skb(ETH_HDR_LEN + IP_HDR_LEN + size);

    skb_reserve(skb, ETH_HDR_LEN + IP_HDR_LEN + size);
    uint8_t *payload = skb_push(skb, size);
    for (uint32_t i = 0; i < size; i++) payload[i] = i * 7;
    skb->protocol = 17;

    CHECK(ip_output_daddr(0x0a000005, skb, 1) == -EMSGSIZE);

    skb_reserve(skb, IP_HDR_LEN);
    skb->len = size;
    CHECK(ip_output_daddr(0x0a000005, skb, 0) == 0);
    CHECK(ncaptured == 3);

    struct sk_buff *whole = NULL;
    for (int i = 0; i < ncaptured; i++) {
        struct iphdr *ih = ip_hdr(captured[i]);

        CHECK(checksum(ih, ih->ihl * 4, 0) == 0);
        CHECK(ntohs(ih->len) <= dev.mtu);
        to_host_order(ih);
        CHECK(((ih->frag_offset & IP_FRAG_MF) != 0) == (i < ncaptured - 1));
        whole = ip_reassemble(captured[i]);
        CHECK((whole != NULL) == (i == ncaptured - 1));
    }

    CHECK(whole && ip_hdr(whole)->len == IP_HDR_LEN + size);
    if (whole) {
        CHECK(memcmp(ip_hdr(whole)->data, payload, size) == 0);
        free_skb(whole);
    }
    free_skb(skb);

    printf("ip_fragment: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
