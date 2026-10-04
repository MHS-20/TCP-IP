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
check() { name=$1; shift; if "$@" >>"$log" 2>&1; then echo "ok   $name"; pass=$((pass+1)); else echo "FAIL $name"; fail=$((fail+1)); fi; }

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

mkdir -p "$log.www" && echo "served by the kernel" > "$log.www/index.html"
python3 -m http.server 8000 --bind 10.0.0.5 --directory "$log.www" >/dev/null 2>&1 &
sleep 0.5
check "tcp: lvl-ip curl -> kernel http" sh -c 'tools/level-ip apps/curl/curl 10.0.0.5 8000 | grep -q "served by the kernel"'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || { echo "--- test log"; cat "$log"; echo "--- stack log"; tail -30 "$log.stack"; }
rm -rf "$log.www"
[ "$fail" -eq 0 ]
