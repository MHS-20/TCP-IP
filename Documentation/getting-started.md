# Getting Started

`lvl-ip` is a single daemon. It opens a TAP device (`tap0`), assigns the
kernel side of the link `10.0.0.5/24`, and acts as host `10.0.0.4` on it.
Programs reach it through `tools/liblevelip.so`, an `LD_PRELOAD` library that
redirects `AF_INET` stream and datagram sockets to the daemon over the UNIX
socket `/tmp/lvlip.socket`. Other file descriptors go to libc as usual.

## Building

    make all

This builds `lvl-ip`, the shim and the apps. The build tries
`sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip`; that step is optional and
only needed to run outside a namespace.

## Option 1: a throwaway namespace (no root)

An unprivileged user namespace gives `CAP_NET_ADMIN` over its own network
namespace, which is enough to create `tap0`. A private `/tmp` keeps the IPC
socket from clashing with another instance:

    unshare -rnm sh
    mount -t tmpfs tmpfs /tmp
    ip link set lo up
    ./lvl-ip &

From that shell, the namespace's kernel stack is the peer at `10.0.0.5`:

    ping 10.0.0.4                                   # ARP + ICMP
    tools/level-ip apps/udp/udp echo 7000 &         # UDP server on lvl-ip
    python3 -c 'import socket; s=socket.socket(2,2); s.sendto(b"hi",("10.0.0.4",7000)); print(s.recv(9))'
    python3 -m http.server 8000 --bind 10.0.0.5 &
    tools/level-ip apps/curl/curl 10.0.0.5 8000     # TCP client on lvl-ip

`tests/netns/run.sh` does exactly this and more.

## Option 2: on the host, routed to the Internet

`lvl-ip` needs `CAP_NET_ADMIN` (from `setcap` above) and runs the `ip` tool to
configure `tap0`, so give that `CAP_NET_ADMIN` too or run it as root.
`/dev/net/tun` must exist (it does on current distributions). Then forward and
NAT the TAP subnet; with `wlp2s0` as the outgoing interface:

    sysctl -w net.ipv4.ip_forward=1
    iptables -I INPUT --source 10.0.0.0/24 -j ACCEPT
    iptables -t nat -I POSTROUTING --out-interface wlp2s0 -j MASQUERADE
    iptables -I FORWARD --in-interface wlp2s0 --out-interface tap0 -j ACCEPT
    iptables -I FORWARD --in-interface tap0 --out-interface wlp2s0 -j ACCEPT

Then run real programs through the stack:

    ./lvl-ip &
    tools/level-ip curl example.com

Name resolution still uses the kernel: glibc's resolver calls its internal
socket functions, which `LD_PRELOAD` does not intercept.

The default gateway is `10.0.0.5` and the stack's address is `10.0.0.4`; both
are compiled in (`src/link/netdev.c`, `src/link/tuntap_if.c`).
