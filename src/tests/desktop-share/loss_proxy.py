"""Bounded localhost validation proxy. Drops selected forwarded UDP fragments.

TCP control is relayed to the real host; only hello.udpPort is mapped to this
proxy's UDP listener. Credentials/payloads are never logged or persisted.
"""
import argparse
import json
import pathlib
import select
import socket
import socketserver
import struct
import threading
import time
from control_probe import MEDIA_HEADER, ROUTE, TCP_MAGIC, ZTS1_UDP

p = argparse.ArgumentParser()
p.add_argument('--listen-port', type=int, default=18141)
p.add_argument('--server-port', type=int, default=18140)
p.add_argument('--stats', required=True)
p.add_argument('--seconds', type=int, default=120)
# Stage-4 default keeps dropping one selected forwarded fragment. Stage 5 can
# disable that and instead cut the whole server->client UDP direction for a
# bounded window to reproduce an egress stall without touching control.
p.add_argument('--drop-mode', choices=('none', 'sequence'), default='sequence')
p.add_argument('--block-after-server-udp', type=int, default=0)
p.add_argument('--block-for-ms', type=int, default=0)
a = p.parse_args()
stats = {'clientUdp': 0, 'serverUdp': 0, 'droppedVideoFragments': 0,
         'droppedSequences': [], 'blockedServerUdp': 0, 'blockWindows': 0,
         'blockStartMs': 0}
deadline = time.monotonic() + a.seconds
block_until = 0.0


class Control(socketserver.BaseRequestHandler):
    def handle(self):
        upstream = socket.create_connection(('127.0.0.1', a.server_port), timeout=3)
        buffers = {self.request: bytearray(), upstream: bytearray()}
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select(list(buffers), [], [], .1)
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    buf = buffers[source]
                    buf.extend(data)
                    if len(buf) > 2 * 1024 * 1024:
                        raise RuntimeError('control buffer bound exceeded')
                    while len(buf) >= 4:
                        length = struct.unpack('>I', buf[:4])[0]
                        if length < 7 or length > 1024 * 1024:
                            raise RuntimeError('invalid control length')
                        if len(buf) < length + 4:
                            break
                        frame = bytes(buf[4:length + 4])
                        del buf[:length + 4]
                        target = upstream if source is self.request else self.request
                        if source is upstream and frame.startswith(TCP_MAGIC):
                            offset = 6 + frame[5]
                            message = json.loads(frame[offset:])
                            body = message.get('data', {})
                            if message.get('ok') and 'udpPort' in body:
                                body['udpPort'] = a.listen_port
                                frame = frame[:offset] + json.dumps(message, separators=(',', ':')).encode()
                        target.sendall(struct.pack('>I', len(frame)) + frame)
        finally:
            upstream.close()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


server = Server(('127.0.0.1', a.listen_port), Control)
threading.Thread(target=server.serve_forever, daemon=True).start()
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(('127.0.0.1', a.listen_port))
routes = {}
try:
    print('READY localhost loss proxy; real host uses separate TCP/UDP port', flush=True)
    while time.monotonic() < deadline:
        readable, _, _ = select.select([udp], [], [], .1)
        if readable:
            packet, source = udp.recvfrom(2048)
            if not packet.startswith(ZTS1_UDP) or len(packet) < len(ZTS1_UDP) + MEDIA_HEADER.size:
                continue
            h = MEDIA_HEADER.unpack_from(packet, len(ZTS1_UDP))
            token = h[4]
            if source[1] == a.server_port:
                stats['serverUdp'] += 1
                if not stats['blockStartMs'] and a.block_after_server_udp > 0 \
                        and stats['serverUdp'] >= a.block_after_server_udp \
                        and a.block_for_ms > 0:
                    block_until = time.monotonic() + a.block_for_ms / 1000.0
                    stats['blockStartMs'] = int(time.time() * 1000)
                    stats['blockWindows'] += 1
                    print('BLOCK server->client UDP for %d ms' % a.block_for_ms,
                          flush=True)
                if time.monotonic() < block_until:
                    stats['blockedServerUdp'] += 1
                    target = None
                    pathlib.Path(a.stats).write_text(
                        json.dumps(stats, indent=2), encoding='utf-8')
                    continue
                target = routes.get(token)
                # Stage-4 default: lose one fragment of a packet after normal
                # first display; keep control, registration and every recovery
                # key untouched.
                if a.drop_mode == 'sequence' and h[2] == 3 and not (h[3] & 1) \
                        and h[8] >= 20 and h[8] % 47 == 20 and h[13] == 0:
                    stats['droppedVideoFragments'] += 1
                    if h[8] not in stats['droppedSequences']:
                        stats['droppedSequences'].append(h[8])
                elif target:
                    udp.sendto(packet, target)
            else:
                if token not in routes and len(routes) >= 72:
                    continue
                routes[token] = source
                stats['clientUdp'] += 1
                udp.sendto(packet, ('127.0.0.1', a.server_port))
        pathlib.Path(a.stats).write_text(json.dumps(stats, indent=2), encoding='utf-8')
finally:
    udp.close()
    server.shutdown()
    server.server_close()
