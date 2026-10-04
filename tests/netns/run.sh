#!/bin/sh
# End-to-end tests inside a throwaway network namespace; no sudo needed.
# The kernel side of tap0 is 10.0.0.5, lvl-ip is 10.0.0.4.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)

if [ "${LVLIP_IN_NS:-}" != 1 ]; then
    # Private net and mount namespaces: own tap0, own /tmp for /tmp/lvlip.socket
    exec env LVLIP_IN_NS=1 unshare -rnm "$0" "$@"
fi

mount -t tmpfs tmpfs /tmp

cd "$root"
log=$(mktemp)
pass=0; fail=0
check() { name=$1; shift; t0=$(date +%s%N); if "$@" >>"$log" 2>&1; then echo "ok   $name ($(( ($(date +%s%N) - t0) / 1000000 ))ms)"; pass=$((pass+1)); else echo "FAIL $name"; fail=$((fail+1)); fi; }

ip link set lo up
./lvl-ip >"$log.stack" 2>&1 &
stack=$!
trap 'kill $stack 2>/dev/null; kill $(jobs -p) 2>/dev/null; rm -f "$log" "$log.stack"' EXIT
for _ in $(seq 50); do
    [ -S /tmp/lvlip.socket ] && ip -4 addr show tap0 2>/dev/null | grep -q 10.0.0.5 && break
    sleep 0.1
done

udp_client() { # host port payload-size -> sends from the kernel stack and checks the echo
    python3 - "$@" <<'PY'
import socket, sys
host, port, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
data = bytes(i % 251 for i in range(size))
s.sendto(data, (host, port))
reply, _ = s.recvfrom(70000)
sys.exit(0 if reply == data else 1)
PY
}

udp_echo_server() { # port -> kernel-side echo server
    python3 - "$1" <<'PY' &
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("10.0.0.5", int(sys.argv[1])))
while True:
    d, a = s.recvfrom(70000); s.sendto(d, a)
PY
}

port_unreachable() {
    python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
s.connect(("10.0.0.4", 9)); s.send(b"x")
try:
    s.recv(10)
except ConnectionRefusedError:
    sys.exit(0)
sys.exit(1)
PY
}

check "arp + icmp echo"                 ping -c 2 -W 2 10.0.0.4

tools/level-ip apps/udp/udp echo 7000 >>"$log" 2>&1 &
sleep 0.5
check "udp: kernel -> lvl-ip echo"      udp_client 10.0.0.4 7000 64
check "udp: 4000B, fragmented both ways" udp_client 10.0.0.4 7000 4000
check "udp: icmp port unreachable"      port_unreachable

udp_echo_server 7001
sleep 0.3
check "udp: lvl-ip client -> kernel"    sh -c 'tools/level-ip apps/udp/udp send 10.0.0.5 7001 hello | grep -qx hello'

mkdir -p "$log.www"
python3 -c 'import sys; sys.stdout.write("".join("line %05d served by the kernel\n" % i for i in range(2000)))' \
    > "$log.www/index.html"   # ~60 KB, many segments
python3 -m http.server 8000 --bind 10.0.0.5 --directory "$log.www" >/dev/null 2>&1 &
sleep 0.5

http_get() { # app -> fetches index.html through lvl-ip and compares the body byte for byte
    timeout 120 tools/level-ip "apps/$1/$1" 10.0.0.5 8000 | sed '1,/^\r$/d' | cmp - "$log.www/index.html"
}

check "tcp: connection refused (RST)"   sh -c 'tools/level-ip apps/curl/curl 10.0.0.5 9999 2>&1 | grep -q "Connection refused"'

# Upload from lvl-ip: the kernel only sends ACKs back, so losing them makes
# lvl-ip's own retransmission timer (RFC 6298, Karn) do the recovery.
python3 - <<'PY' >"$log.sink" 2>&1 &
import hashlib, socket
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("10.0.0.5", 8100)); srv.listen()
while True:
    c, _ = srv.accept(); h = hashlib.sha256()
    while (d := c.recv(65536)): h.update(d)
    c.sendall(h.hexdigest().encode()); c.close()
PY
sleep 0.3

upload() {
    timeout 120 tools/level-ip python3 - <<'PY'
import hashlib, socket, sys
data = bytes(i * 31 % 256 for i in range(200000))
s = socket.socket(); s.connect(("10.0.0.5", 8100)); s.sendall(data); s.shutdown(socket.SHUT_WR)
sys.exit(0 if s.recv(64).decode() == hashlib.sha256(data).hexdigest() else 1)
PY
}

# Impairments are applied to tap0 egress, i.e. kernel -> lvl-ip, as upstream's suites did
for netem in "" "delay 300ms" "duplicate 50%" "loss 25%"; do
    [ -n "$netem" ] && tc qdisc replace dev tap0 root netem $netem
    label=${netem:-clean link}
    check "tcp: curl, $label"      http_get curl
    check "tcp: curl-poll, $label" http_get curl-poll
    check "tcp: 200KB upload, $label" upload
    [ -n "${LVLIP_E2E_VERBOSE:-}" ] && [ -n "$netem" ] && tc -s qdisc show dev tap0 | sed -n '1,3p'
    [ -n "$netem" ] && tc qdisc del dev tap0 root
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || { echo "--- test log"; cat "$log"; echo "--- stack log"; tail -30 "$log.stack"; }
rm -rf "$log.www"
[ "$fail" -eq 0 ]
