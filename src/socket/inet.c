#include "syshead.h"
#include "inet.h"
#include "socket.h"
#include "sock.h"
#include "tcp.h"
#include "wait.h"
#include "netdev.h"

extern struct netdev *netdev;

extern struct net_ops tcp_ops;
extern struct net_ops udp_ops;

static int inet_stream_connect(struct socket *sock, const struct sockaddr *addr,
                               int addr_len, int flags);

static int inet_dgram_connect(struct socket *sock, const struct sockaddr *addr,
                              int addr_len, int flags);

static int INET_OPS = 2;

struct net_family inet = {
    .create = inet_create,
};

static struct sock_ops inet_stream_ops = {
    .connect = &inet_stream_connect,
    .write = &inet_write,
    .read = &inet_read,
    .bind = &inet_bind,
    .sendto = &inet_sendto,
    .recvfrom = &inet_recvfrom,
    .close = &inet_close,
    .shutdown = &inet_shutdown,
    .free = &inet_free,
    .abort = &inet_abort,
    .getpeername = &inet_getpeername,
    .getsockname = &inet_getsockname,
};

static struct sock_ops inet_dgram_ops = {
    .connect = &inet_dgram_connect,
    .write = &inet_write,
    .read = &inet_read,
    .bind = &inet_bind,
    .sendto = &inet_sendto,
    .recvfrom = &inet_recvfrom,
    .close = &inet_close,
    .shutdown = &inet_shutdown,
    .free = &inet_free,
    .abort = &inet_abort,
    .getpeername = &inet_getpeername,
    .getsockname = &inet_getsockname,
};

static struct sock_type inet_ops[] = {
    {
        .sock_ops = &inet_stream_ops,
        .net_ops = &tcp_ops,
        .type = SOCK_STREAM,
        .protocol = IPPROTO_TCP,
    },
    {
        .sock_ops = &inet_dgram_ops,
        .net_ops = &udp_ops,
        .type = SOCK_DGRAM,
        .protocol = IPPROTO_UDP,
    }
};

int inet_create(struct socket *sock, int protocol)
{
    struct sock *sk;
    struct sock_type *skt = NULL;

    int type = sock->type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

    for (int i = 0; i < INET_OPS; i++) {
        if (inet_ops[i].type == type &&
            (protocol == 0 || protocol == inet_ops[i].protocol)) {
            skt = &inet_ops[i];
            break;
        }
    }

    if (!skt) {
        print_err("Could not find socktype for socket\n");
        return 1;
    }

    sock->ops = skt->sock_ops;

    if (sock->type & SOCK_NONBLOCK) {
        sock->flags |= O_NONBLOCK;
    }

    sk = sk_alloc(skt->net_ops, skt->protocol);
    sk->protocol = skt->protocol;
    
    sock_init_data(sock, sk);
    
    return 0;
}

int inet_socket(struct socket *sock, int protocol)
{
    return 0;
}

int inet_connect(struct socket *sock, struct sockaddr *addr,
                 int addr_len, int flags)
{
    return 0;
}

static int inet_stream_connect(struct socket *sock, const struct sockaddr *addr,
                        int addr_len, int flags)
{
    struct sock *sk = sock->sk;
    int rc = 0;
    
    if (addr_len < sizeof(addr->sa_family)) {
        return -EINVAL;
    }

    if (addr->sa_family == AF_UNSPEC) {
        sk->ops->disconnect(sk, flags);
        return -EAFNOSUPPORT;
    }

    switch (sock->state) {
    default:
        sk->err = -EINVAL;
        goto out;
    case SS_CONNECTED:
        sk->err = -EISCONN;
        goto out;
    case SS_CONNECTING:
        sk->err = -EALREADY;
        goto out;
    case SS_UNCONNECTED:
        sk->err = -EISCONN;
        if (sk->state != TCP_CLOSE) {
            goto out;
        }

        sk->ops->connect(sk, addr, addr_len, flags);
        sock->state = SS_CONNECTING;
        sk->err = -EINPROGRESS;

        if (sock->flags & O_NONBLOCK) {
            goto out;
        }

        pthread_mutex_lock(&sock->sleep.lock);
        while (sock->state == SS_CONNECTING && sk->err == -EINPROGRESS) {
            socket_release(sock);
            wait_sleep(&sock->sleep);
            socket_wr_acquire(sock);
        }
        pthread_mutex_unlock(&sock->sleep.lock);
        socket_wr_acquire(sock);
        
        switch (sk->err) {
        case -ETIMEDOUT:
        case -ECONNREFUSED:
            goto sock_error;
        }

        if (sk->err != 0) {
            goto out;
        }

        sock->state = SS_CONNECTED;
        break;
    }
    
out:
    return sk->err;
sock_error:
    rc = sk->err;
    return rc;
}

static int inet_dgram_connect(struct socket *sock, const struct sockaddr *addr,
                              int addr_len, int flags)
{
    if (addr_len < sizeof(addr->sa_family)) return -EINVAL;

    int rc = sock->sk->ops->connect(sock->sk, addr, addr_len, flags);

    if (rc == 0) {
        sock->state = addr->sa_family == AF_UNSPEC ? SS_UNCONNECTED : SS_CONNECTED;
    }

    return rc;
}

int inet_write(struct socket *sock, const void *buf, int len)
{
    struct sock *sk = sock->sk;

    return sk->ops->write(sk, buf, len);
}

int inet_read(struct socket *sock, void *buf, int len)
{
    struct sock *sk = sock->sk;

    return sk->ops->read(sk, buf, len);
}

static int inet_parse_addr(const struct sockaddr *addr, socklen_t addr_len,
                           struct sockaddr_in *out)
{
    if (addr == NULL || addr_len < sizeof(struct sockaddr_in)) return -EINVAL;
    memcpy(out, addr, sizeof(struct sockaddr_in));
    if (out->sin_family != AF_INET) return -EAFNOSUPPORT;

    return 0;
}

/* Binds to the stack's own address (or INADDR_ANY) and a local port.
 * Port 0 picks an ephemeral port. */
int inet_bind(struct socket *sock, const struct sockaddr *addr, socklen_t addr_len)
{
    struct sock *sk = sock->sk;
    struct sockaddr_in sin;
    int rc;

    if ((rc = inet_parse_addr(addr, addr_len, &sin)) != 0) return rc;
    if (sk->sport != 0) return -EINVAL;

    uint32_t local = ntohl(sin.sin_addr.s_addr);
    if (local != INADDR_ANY && local != netdev->addr) return -EADDRNOTAVAIL;

    uint16_t port = ntohs(sin.sin_port);
    if (port == 0) {
        if ((port = socket_ephemeral_port(sk->protocol)) == 0) return -EADDRINUSE;
    } else if (socket_port_in_use(sk->protocol, port)) {
        return -EADDRINUSE;
    }

    sk->sport = port;
    sk->saddr = netdev->addr;

    return 0;
}

int inet_sendto(struct socket *sock, const void *buf, int len,
                const struct sockaddr *addr, socklen_t addr_len)
{
    struct sock *sk = sock->sk;
    struct sockaddr_in sin;

    if (!sk->ops->sendto) return sk->ops->write(sk, buf, len);

    if (addr == NULL) return sk->ops->sendto(sk, buf, len, NULL);

    int rc = inet_parse_addr(addr, addr_len, &sin);
    if (rc != 0) return rc;

    return sk->ops->sendto(sk, buf, len, &sin);
}

int inet_recvfrom(struct socket *sock, void *buf, int len,
                  struct sockaddr *addr, socklen_t *addr_len)
{
    struct sock *sk = sock->sk;
    struct sockaddr_in sin = { .sin_family = AF_INET };
    int rc;

    if (sk->ops->recvfrom) {
        rc = sk->ops->recvfrom(sk, buf, len, &sin);
    } else {
        rc = sk->ops->read(sk, buf, len);
        sin.sin_port = htons(sk->dport);
        sin.sin_addr.s_addr = htonl(sk->daddr);
    }

    if (rc >= 0 && addr != NULL && addr_len != NULL) {
        socklen_t n = min(*addr_len, sizeof(sin));
        memcpy(addr, &sin, n);
        *addr_len = sizeof(sin);
    }

    return rc;
}

struct sock *inet_lookup(struct sk_buff *skb, int protocol, uint16_t sport, uint16_t dport)
{
    struct socket *sock = socket_lookup(protocol, sport, dport);
    if (sock == NULL) return NULL;
    
    return sock->sk;
}

int inet_close(struct socket *sock)
{
    if (!sock) {
        return 0;
    }

    struct sock *sk = sock->sk;

    return sock->sk->ops->close(sk);
}

int inet_shutdown(struct socket *sock, int how)
{
    struct sock *sk = sock->sk;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;
    if (sk->ops->shutdown) return sk->ops->shutdown(sk, how);

    return sk->dport ? 0 : -ENOTCONN;
}

int inet_free(struct socket *sock)
{
    struct sock *sk = sock->sk;
    sock_free(sk);
    free(sock->sk);
    
    return 0;
}

int inet_abort(struct socket *sock)
{
    struct sock *sk = sock->sk;
    
    if (sk) {
        sk->ops->abort(sk);
    }

    return 0;
}

int inet_getpeername(struct socket *sock, struct sockaddr *restrict address,
                     socklen_t *address_len)
{
    struct sock *sk = sock->sk;

    if (sk == NULL) {
        return -1;
    }

    if (sk->dport == 0) {
        return -ENOTCONN;
    }

    struct sockaddr_in *res = (struct sockaddr_in *) address;
    res->sin_family = AF_INET;
    res->sin_port = htons(sk->dport);
    res->sin_addr.s_addr = htonl(sk->daddr);
    *address_len = sizeof(struct sockaddr_in);

    inet_dbg(sock, "geetpeername sin_family %d sin_port %d sin_addr %d addrlen %d",
             res->sin_family, ntohs(res->sin_port), ntohl(res->sin_addr.s_addr), *address_len);
    
    return 0;
}
int inet_getsockname(struct socket *sock, struct sockaddr *restrict address,
                     socklen_t *address_len)
{
    struct sock *sk = sock->sk;

    if (sk == NULL) {
        return -1;
    }
    
    struct sockaddr_in *res = (struct sockaddr_in *) address;
    res->sin_family = AF_INET;
    res->sin_port = htons(sk->sport);
    res->sin_addr.s_addr = htonl(sk->saddr);
    *address_len = sizeof(struct sockaddr_in);

    inet_dbg(sock, "getsockname sin_family %d sin_port %d sin_addr %d addrlen %d",
             res->sin_family, ntohs(res->sin_port), ntohl(res->sin_addr.s_addr), *address_len);
    
    return 0;
}
