#!/bin/bash
# A real split tunnel for test_route, in namespaces nobody else can see.
#
#   unshare -Urn --kill-child bash test/route_tunnel.sh <test binary>
#
# This namespace is the client: its default route is a veth, and wg0 is up with
# no route at all, which is the arrangement where a tunnel carries only what is
# bound to it. A child namespace is the internet, with one server on an address
# reachable both ways, and that server writes down who asked: 10.0.0.2 is the
# ordinary route, 10.9.0.2 is the tunnel. A mistake here reaches no real network.
#
# Stops by itself: everything started is under `timeout`, and the namespaces go
# when the last process in them does.
set -eu
binary=$1

for tool in ip wg python3 nsenter; do
	command -v "$tool" >/dev/null || { echo "HYDRA-SKIP: no $tool"; exit 0; }
done
ip link add probe0 type wireguard 2>/dev/null ||
	{ echo "HYDRA-SKIP: this kernel will not make a wireguard device here"; exit 0; }
ip link del probe0

scratch=$(mktemp -d)
umask 077

ip link set lo up
timeout 170 unshare -n sleep 165 &
keeper=$!
sleep 0.3
internet=$(ps -o pid= --ppid $keeper | tr -d ' ')

# Both by the pid recorded here: the server's `timeout`, then the namespace's.
finish() {
	[ -s "$scratch/server.pid" ] && kill "$(cat "$scratch/server.pid")" 2>/dev/null
	kill "$keeper" 2>/dev/null
	rm -rf "$scratch"
}
trap finish EXIT

ip link add eth0 type veth peer name peer0
ip link set peer0 netns "$internet"
ip addr add 10.0.0.2/24 dev eth0
ip link set eth0 up
ip route add default via 10.0.0.1

wg genkey > "$scratch/client"
wg pubkey < "$scratch/client" > "$scratch/client.pub"
wg genkey > "$scratch/server"
wg pubkey < "$scratch/server" > "$scratch/server.pub"
ip link add wg0 type wireguard
wg set wg0 private-key "$scratch/client" listen-port 51000 \
	peer "$(cat "$scratch/server.pub")" endpoint 10.0.0.1:51001 allowed-ips 0.0.0.0/0
ip addr add 10.9.0.2/24 dev wg0
ip link set wg0 up

cat > "$scratch/serve.py" <<'PY'
import http.server, socketserver, sys, time
seen = open(sys.argv[1], 'a', buffering=1)
class handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'
    def log_message(self, *a): pass
    def do_HEAD(self): self.do_GET()
    def reply(self, kind, body):
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if self.command == 'GET': self.wfile.write(body)
    def do_GET(self):
        seen.write('%s %s\n' % (self.client_address[0], self.path))
        if self.path.startswith('/slow'):
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(4000 * 65536))
            self.end_headers()
            for _ in range(4000):
                self.wfile.write(b'x' * 65536)
                time.sleep(0.05)
        elif self.path.endswith('.m3u8'):
            self.reply('application/vnd.apple.mpegurl',
                       b'#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nseg0.ts\n'
                       b'#EXTINF:1,\nseg1.ts\n#EXT-X-ENDLIST\n')
        elif self.path.endswith('.mp4'):
            self.reply('video/mp4', b'\0\0\0\x18ftypmp42' + b'\0' * 2000)
        else:
            self.reply('video/mp2t', b'G' + b'\0' * 187)
class server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
server(('198.51.100.1', 8080), handler).serve_forever()
PY

nsenter -t "$internet" -n bash -s <<IN
set -eu
ip link set lo up
ip link set peer0 up
ip addr add 10.0.0.1/24 dev peer0
ip link add dummy1 type dummy
ip addr add 198.51.100.1/32 dev dummy1
ip link set dummy1 up
ip link add wg1 type wireguard
wg set wg1 private-key "$scratch/server" listen-port 51001 \
	peer "$(cat "$scratch/client.pub")" allowed-ips 10.9.0.2/32
ip addr add 10.9.0.1/24 dev wg1
ip link set wg1 up
timeout 165 python3 "$scratch/serve.py" "$scratch/seen.log" >/dev/null 2>&1 </dev/null &
echo \$! > "$scratch/server.pid"
IN
sleep 0.5

status=0
HYDRA_ROUTE_SEEN=$scratch/seen.log timeout 150 "$binary" --tunnel || status=$?
exit "$status"
