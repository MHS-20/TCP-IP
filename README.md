# TCP-IP

A Linux userspace TCP/IP stack on a TAP device, built as a reference
implementation for learning: Ethernet, ARP, IPv4 (with fragmentation),
ICMPv4, TCP and UDP, plus an `LD_PRELOAD` shim that runs unmodified
programs (`curl`, `python3`, …) on top of it.

It started from [Level-IP](https://github.com/saminiir/level-ip), the code
behind the blog series *Let's code a TCP/IP stack* (parts 1–5), and extends it
with UDP, IP fragmentation and reassembly, input validation and a test suite.
See [THIRD_PARTY.md](THIRD_PARTY.md) for the upstream notice.

## What it implements

| Layer     | Implemented                                                                 |
|-----------|-----------------------------------------------------------------------------|
| Link      | TAP device, Ethernet II, ARP request/reply with a cache                     |
| Network   | IPv4 header validation, reassembly of fragments, fragmentation on output, unique datagram ids, one static route table |
| ICMPv4    | Echo reply, checksum validation, Destination Unreachable (protocol, port)   |
| TCP       | RFC 793 state machine (client side), checksums, RFC 6298 RTO with Karn's algorithm, out-of-order queue, SACK blocks, half-close, RFC 6528 ISNs, random ephemeral ports |
| UDP       | RFC 768 datagrams, checksums, `bind`/`connect`/`sendto`/`recvfrom`, port unreachable |
| Sockets   | `socket`, `bind`, `connect`, `read`/`write`, `send`/`recv`, `sendto`/`recvfrom`, `shutdown`, `close`, `poll`, `fcntl`, `getsockopt(SO_ERROR)`, `getsockname`, `getpeername` |

Not implemented: TCP `listen`/`accept` (no server side), congestion control,
window scaling, IP options, IPv6, `select`/`epoll`, more than one interface.

## Layout

```
src/core/       main loop and threads, timers, sk_buff, checksums, CLI
src/link/       TAP device, netdev rx/tx, ARP
src/net/        IPv4 input/output, fragmentation (ip_frag.c), routes, ICMPv4
src/transport/  TCP (tcp*.c) and UDP (udp.c)
src/socket/     socket table, inet layer, IPC server for the shim
tools/          liblevelip.so (LD_PRELOAD shim) and the level-ip wrapper
apps/           curl, curl-poll (TCP clients), udp (echo server / client)
tests/unit/     host-only tests, no privileges
tests/netns/    end-to-end tests over a real TAP device
```

## Build and test

```
make all     # lvl-ip, tools/liblevelip.so, apps
make test    # unit tests, then end-to-end tests
```

`make test` needs no root: the end-to-end suite runs inside an unprivileged
user + network + mount namespace (`unshare -rnm`), creates the TAP device
there, and talks to lvl-ip from the namespace's kernel stack. It covers ARP,
ping, UDP both ways (including 4000-byte datagrams fragmented in both
directions and ICMP port unreachable), TCP RST, and TCP downloads and uploads
under `tc netem` delay, duplication and 25% loss.

## Running it

See [Documentation/getting-started.md](Documentation/getting-started.md).
In short, inside a namespace:

```
unshare -rnm sh -c 'mount -t tmpfs tmpfs /tmp; ./lvl-ip & sleep 1; \
    tools/level-ip apps/udp/udp echo 7000'
```

