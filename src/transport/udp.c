#include "syshead.h"
#include "udp.h"
#include "ip.h"
#include "icmpv4.h"
#include "inet.h"
#include "socket.h"
#include "netdev.h"
#include "utils.h"
#include "wait.h"

/*
 * UDP (RFC 768). Each received datagram stays one skb on the socket's
 * receive_queue, so message boundaries are preserved: one recvfrom returns
 * at most one datagram and silently truncates it to the caller's buffer.
 */

#define UDP_ARP_RETRIES 10
#define UDP_ARP_WAIT_US 10000

extern struct netdev *netdev;

static struct sock *udp_alloc_sock(int protocol)
{
    return calloc(1, sizeof(struct sock));
}

static int udp_init_sock(struct sock *sk)
{
    sk->poll_events = POLLOUT | POLLWRNORM;
    return 0;
}

static int udp_autobind(struct sock *sk)
{
    if (sk->sport != 0) return 0;

    if ((sk->sport = socket_ephemeral_port(IPPROTO_UDP)) == 0) return -EAGAIN;
    sk->saddr = netdev->addr;

    return 0;
}

static int udp_disconnect(struct sock *sk, int flags)
{
    sk->daddr = 0;
    sk->dport = 0;
    return 0;
}

/* connect() only fixes the default destination and filters received datagrams */
static int udp_connect(struct sock *sk, const struct sockaddr *addr, int addr_len, int flags)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    if (addr->sa_family == AF_UNSPEC) return udp_disconnect(sk, flags);
    if (addr_len < sizeof(struct sockaddr_in) || sin->sin_port == 0) return -EINVAL;

    sk->daddr = ntohl(sin->sin_addr.s_addr);
    sk->dport = ntohs(sin->sin_port);

    return udp_autobind(sk);
}

static struct sk_buff *udp_build(struct sock *sk, const void *buf, int len,
                                 uint32_t daddr, uint16_t dport)
{
    int reserved = ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + len;
    struct sk_buff *skb = alloc_skb(reserved);

    skb_reserve(skb, reserved);
    memcpy(skb_push(skb, len), buf, len);

    struct udphdr *uh = (struct udphdr *)skb_push(skb, UDP_HDR_LEN);
    uh->sport = htons(sk->sport);
    uh->dport = htons(dport);
    uh->len = htons(UDP_HDR_LEN + len);
    uh->csum = 0;
    uh->csum = pseudo_checksum(htonl(sk->saddr), htonl(daddr), IP_UDP,
                               skb->data, skb->len);
    /* A computed zero is sent as all ones; zero means "no checksum" */
    if (uh->csum == 0) uh->csum = 0xffff;

    skb->protocol = IP_UDP;

    return skb;
}

static int udp_sendto(struct sock *sk, const void *buf, int len, const struct sockaddr_in *dst)
{
    uint32_t daddr = dst ? ntohl(dst->sin_addr.s_addr) : sk->daddr;
    uint16_t dport = dst ? ntohs(dst->sin_port) : sk->dport;
    int rc;

    if (!dst && sk->dport == 0) return -EDESTADDRREQ;
    if (dport == 0) return -EINVAL;
    if (len < 0 || len > UDP_MAX_PAYLOAD) return -EMSGSIZE;
    if ((rc = udp_autobind(sk)) != 0) return rc;

    /*
     * ip_output returns -1 while ARP resolves the next hop and drops the
     * datagram. Retry briefly so the first datagram to a new neighbour is
     * not lost; UDP gives no stronger delivery guarantee than that.
     */
    for (int attempt = 0; attempt < UDP_ARP_RETRIES; attempt++) {
        struct sk_buff *skb = udp_build(sk, buf, len, daddr, dport);

        rc = ip_output_daddr(daddr, skb, 0);
        free_skb(skb);

        if (rc == 0) return len;
        if (rc != -1) return rc;

        usleep(UDP_ARP_WAIT_US);
    }

    return -EHOSTUNREACH;
}

static int udp_write(struct sock *sk, const void *buf, int len)
{
    return udp_sendto(sk, buf, len, NULL);
}

static int udp_recvfrom(struct sock *sk, void *buf, int len, struct sockaddr_in *src)
{
    struct socket *sock = sk->sock;
    struct sk_buff *skb;

    while ((skb = skb_peek(&sk->receive_queue)) == NULL) {
        if (sk->sport == 0) return -EBADF;
        if (sock->flags & O_NONBLOCK) return -EAGAIN;

        pthread_mutex_lock(&sk->recv_wait.lock);
        socket_release(sock);
        wait_sleep(&sk->recv_wait);
        pthread_mutex_unlock(&sk->recv_wait.lock);
        socket_wr_acquire(sock);
    }

    skb_dequeue(&sk->receive_queue);

    int n = min(len, skb->dlen);
    memcpy(buf, skb->payload, n);

    if (src) {
        struct iphdr *ih = ip_hdr(skb);
        struct udphdr *uh = (struct udphdr *)((uint8_t *)ih + ih->ihl * 4);

        src->sin_family = AF_INET;
        src->sin_port = uh->sport;
        src->sin_addr.s_addr = htonl(ih->saddr);
    }

    if (skb_queue_empty(&sk->receive_queue)) {
        sk->poll_events &= ~(POLLIN | POLLRDNORM);
    }

    free_skb(skb);

    return n;
}

static int udp_read(struct sock *sk, void *buf, int len)
{
    return udp_recvfrom(sk, buf, len, NULL);
}

static int udp_recv_notify(struct sock *sk)
{
    return wait_wakeup(&sk->recv_wait);
}

static int udp_close(struct sock *sk)
{
    /* Unhash right away so the port is free and the rx path stops queueing */
    sk->sport = 0;
    sk->dport = 0;
    skb_queue_free(&sk->receive_queue);
    wait_wakeup(&sk->recv_wait);

    return socket_delete(sk->sock);
}

struct net_ops udp_ops = {
    .alloc_sock = &udp_alloc_sock,
    .init = &udp_init_sock,
    .connect = &udp_connect,
    .disconnect = &udp_disconnect,
    .write = &udp_write,
    .read = &udp_read,
    .sendto = &udp_sendto,
    .recvfrom = &udp_recvfrom,
    .recv_notify = &udp_recv_notify,
    .close = &udp_close,
    .abort = &udp_close,
};

/* A connected socket matching the remote port wins over a merely bound one */
static struct socket *udp_lookup(uint16_t sport, uint16_t dport)
{
    struct socket *sock = socket_lookup(IPPROTO_UDP, sport, dport);

    return sock ? sock : socket_lookup(IPPROTO_UDP, 0, dport);
}

void udp_in(struct sk_buff *skb)
{
    struct iphdr *ih = ip_hdr(skb);
    struct udphdr *uh = (struct udphdr *)((uint8_t *)ih + ih->ihl * 4);
    uint16_t len = ip_len(ih);

    if (len < UDP_HDR_LEN || ntohs(uh->len) < UDP_HDR_LEN || ntohs(uh->len) > len) {
        print_err("UDP datagram dropped: bad length\n");
        goto drop;
    }

    /* The IP payload may carry padding past the UDP length */
    len = ntohs(uh->len);

    if (uh->csum != 0 &&
        pseudo_checksum(htonl(ih->saddr), htonl(ih->daddr), IP_UDP, (uint8_t *)uh, len) != 0) {
        print_err("UDP datagram dropped: bad checksum\n");
        goto drop;
    }

    struct socket *sock = udp_lookup(ntohs(uh->sport), ntohs(uh->dport));

    if (sock == NULL) {
        if (ih->daddr == netdev->addr) {
            icmpv4_send_unreachable(skb, ICMP_V4_PORT_UNREACH);
        }
        goto drop;
    }

    struct sock *sk = sock->sk;
    socket_wr_acquire(sock);

    if (sk->receive_queue.qlen >= UDP_RCV_QLEN) {
        socket_release(sock);
        goto drop;
    }

    skb->dlen = len - UDP_HDR_LEN;
    skb->payload = uh->data;
    skb_queue_tail(&sk->receive_queue, skb);
    sk->poll_events |= POLLIN | POLLRDNORM;

    socket_release(sock);
    sk->ops->recv_notify(sk);
    return;

drop:
    free_skb(skb);
}
