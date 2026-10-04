/* Unit tests for IPv4 reassembly; no TAP device or privileges needed. */
#include "syshead.h"
#include "skbuff.h"
#include "ip.h"

static uint32_t fake_tick = 0;
int timer_get_tick() { return fake_tick; }

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Fragment with host-order header fields, as ip_rcv hands it over */
static struct sk_buff *frag(uint16_t id, uint32_t off, uint32_t len, int more, uint8_t fill)
{
    struct sk_buff *skb = alloc_skb(ETH_HDR_LEN + IP_HDR_LEN + len);
    struct iphdr *ih = ip_hdr(skb);

    ih->version = 4;
    ih->ihl = 5;
    ih->len = IP_HDR_LEN + len;
    ih->id = id;
    ih->proto = 17;
    ih->saddr = 0x0a000005;
    ih->daddr = 0x0a000004;
    ih->frag_offset = (more ? IP_FRAG_MF : 0) | (off / 8);
    memset(ih->data, fill, len);
    skb->len = ETH_HDR_LEN + ih->len;

    return skb;
}

static int payload_is(struct sk_buff *skb, uint32_t off, uint32_t len, uint8_t fill)
{
    uint8_t *p = ip_hdr(skb)->data + off;

    for (uint32_t i = 0; i < len; i++) if (p[i] != fill) return 0;
    return 1;
}

static void test_in_order()
{
    CHECK(ip_reassemble(frag(1, 0, 16, 1, 'a')) == NULL);
    CHECK(ip_reassemble(frag(1, 16, 16, 1, 'b')) == NULL);
    struct sk_buff *whole = ip_reassemble(frag(1, 32, 5, 0, 'c'));

    CHECK(whole != NULL);
    if (!whole) return;
    CHECK(ip_hdr(whole)->len == IP_HDR_LEN + 37);
    CHECK(ip_hdr(whole)->frag_offset == 0);
    CHECK(payload_is(whole, 0, 16, 'a') && payload_is(whole, 16, 16, 'b') && payload_is(whole, 32, 5, 'c'));
    free_skb(whole);
}

static void test_out_of_order_and_duplicate()
{
    CHECK(ip_reassemble(frag(2, 32, 4, 0, 'c')) == NULL);
    CHECK(ip_reassemble(frag(2, 0, 16, 1, 'a')) == NULL);
    CHECK(ip_reassemble(frag(2, 0, 16, 1, 'a')) == NULL);   /* duplicate dropped */
    struct sk_buff *whole = ip_reassemble(frag(2, 16, 16, 1, 'b'));

    CHECK(whole != NULL);
    if (!whole) return;
    CHECK(ip_hdr(whole)->len == IP_HDR_LEN + 36);
    CHECK(payload_is(whole, 16, 16, 'b'));
    free_skb(whole);
}

static void test_overlap_rejected()
{
    CHECK(ip_reassemble(frag(3, 0, 16, 1, 'a')) == NULL);
    CHECK(ip_reassemble(frag(3, 8, 16, 1, 'x')) == NULL);   /* overlaps, dropped */
    struct sk_buff *whole = ip_reassemble(frag(3, 16, 8, 0, 'b'));

    CHECK(whole != NULL && payload_is(whole, 8, 8, 'a'));
    if (whole) free_skb(whole);
}

static void test_bad_length_and_timeout()
{
    CHECK(ip_reassemble(frag(4, 0, 10, 1, 'a')) == NULL);   /* not multiple of 8 */
    CHECK(ip_reassemble(frag(5, 0, 16, 1, 'a')) == NULL);
    fake_tick += 31000;                                    /* queue for id 5 expires */
    CHECK(ip_reassemble(frag(5, 16, 8, 0, 'b')) == NULL);   /* first half is gone */

    /* Expire the leftover queue; a lone last fragment at offset 0 completes at once */
    fake_tick += 31000;
    struct sk_buff *whole = ip_reassemble(frag(6, 0, 8, 0, 'z'));
    CHECK(whole != NULL);
    if (whole) free_skb(whole);
}

int main()
{
    test_in_order();
    test_out_of_order_and_duplicate();
    test_overlap_rejected();
    test_bad_length_and_timeout();

    printf("ip_frag: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
