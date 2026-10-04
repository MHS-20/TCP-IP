#include "syshead.h"
#include "skbuff.h"
#include "ip.h"
#include "timer.h"
#include "utils.h"

/*
 * IPv4 fragment reassembly (RFC 791, section 3.2).
 *
 * Fragments are grouped by <saddr, daddr, proto, id>. Each fragment's payload
 * range is kept in skb->seq / skb->end_seq (byte offsets inside the original
 * datagram), and the queue is ordered by offset. Overlapping fragments are
 * dropped. Only the rx thread touches the queues, so no locking is needed.
 */

#define IP_FRAG_TIMEOUT 30000   /* ms, same as Linux ipfrag_time */
#define IP_FRAG_MAX_QUEUES 64

struct ipq {
    struct list_head list;
    uint32_t saddr;
    uint32_t daddr;
    uint16_t id;
    uint8_t proto;
    uint32_t total;             /* payload length, known once the last fragment arrived */
    uint32_t received;
    uint32_t expires;
    struct sk_buff_head frags;
};

static LIST_HEAD(ipqs);
static int ipq_count = 0;

static void ipq_free(struct ipq *q)
{
    list_del(&q->list);
    skb_queue_free(&q->frags);
    free(q);
    ipq_count--;
}

static void ipq_expire(uint32_t now)
{
    struct list_head *item, *tmp;

    list_for_each_safe(item, tmp, &ipqs) {
        struct ipq *q = list_entry(item, struct ipq, list);

        if (q->expires <= now) {
            print_err("IP reassembly timed out for id %hu\n", q->id);
            ipq_free(q);
        }
    }
}

static struct ipq *ipq_find(struct iphdr *ih, uint32_t now)
{
    struct list_head *item;

    list_for_each(item, &ipqs) {
        struct ipq *q = list_entry(item, struct ipq, list);

        if (q->id == ih->id && q->proto == ih->proto &&
            q->saddr == ih->saddr && q->daddr == ih->daddr) {
            return q;
        }
    }

    if (ipq_count >= IP_FRAG_MAX_QUEUES) return NULL;

    struct ipq *q = calloc(1, sizeof(struct ipq));
    q->saddr = ih->saddr;
    q->daddr = ih->daddr;
    q->id = ih->id;
    q->proto = ih->proto;
    q->expires = now + IP_FRAG_TIMEOUT;
    skb_queue_init(&q->frags);
    list_add_tail(&q->list, &ipqs);
    ipq_count++;

    return q;
}

/* Insert ordered by offset. Returns -1 for duplicates and overlaps. */
static int ipq_insert(struct ipq *q, struct sk_buff *skb)
{
    struct list_head *item;
    struct sk_buff *prev = NULL;

    list_for_each(item, &q->frags.head) {
        struct sk_buff *next = list_entry(item, struct sk_buff, list);

        if (next->seq >= skb->seq) {
            if (skb->end_seq > next->seq) return -1;
            if (prev && prev->end_seq > skb->seq) return -1;

            skb_queue_add(&q->frags, skb, next);
            return 0;
        }

        prev = next;
    }

    if (prev && prev->end_seq > skb->seq) return -1;

    skb_queue_tail(&q->frags, skb);
    return 0;
}

static struct sk_buff *ipq_build(struct ipq *q)
{
    struct sk_buff *first = skb_peek(&q->frags);
    struct iphdr *fh = ip_hdr(first);
    int hlen = fh->ihl * 4;
    struct sk_buff *skb = alloc_skb(ETH_HDR_LEN + hlen + q->total);
    struct list_head *item;

    memcpy(skb->data, first->head, ETH_HDR_LEN + hlen);

    list_for_each(item, &q->frags.head) {
        struct sk_buff *frag = list_entry(item, struct sk_buff, list);
        struct iphdr *ih = ip_hdr(frag);

        memcpy(skb->data + ETH_HDR_LEN + hlen + frag->seq,
               (uint8_t *)ih + ih->ihl * 4, frag->end_seq - frag->seq);
    }

    struct iphdr *ih = ip_hdr(skb);
    ih->len = hlen + q->total;
    ih->frag_offset = 0;
    skb->len = ETH_HDR_LEN + ih->len;

    return skb;
}

/*
 * Takes a fragment whose header was already validated and converted to host
 * byte order. Returns the reassembled datagram once complete, NULL otherwise.
 * The fragment skb is consumed in both cases.
 */
struct sk_buff *ip_reassemble(struct sk_buff *skb)
{
    struct iphdr *ih = ip_hdr(skb);
    uint32_t now = timer_get_tick();
    uint32_t offset = (ih->frag_offset & IP_FRAG_OFFSET) * 8;
    uint32_t len = ip_len(ih);
    int more = ih->frag_offset & IP_FRAG_MF;
    struct ipq *q;

    ipq_expire(now);

    /* All but the last fragment carry a multiple of 8 bytes */
    if ((more && (len % 8 != 0)) || offset + len > 0xffff - ih->ihl * 4) goto drop;

    if ((q = ipq_find(ih, now)) == NULL) goto drop;

    skb->seq = offset;
    skb->end_seq = offset + len;

    if (!more) {
        if (q->total && q->total != skb->end_seq) goto drop;
        q->total = skb->end_seq;
    } else if (q->total && skb->end_seq > q->total) {
        goto drop;
    }

    if (ipq_insert(q, skb) != 0) goto drop;

    q->received += len;

    if (q->total == 0 || q->received < q->total) return NULL;

    struct sk_buff *whole = ipq_build(q);
    ipq_free(q);

    return whole;

drop:
    free_skb(skb);
    return NULL;
}
