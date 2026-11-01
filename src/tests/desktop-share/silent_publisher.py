"""Real host control + UDP keepalive, deliberately no video. No credentials logged."""
import argparse
import pathlib
import time
from control_probe import Client, UdpEndpoint, hello, create_room, configure, stop_room

p = argparse.ArgumentParser()
p.add_argument('--host', default='127.0.0.1')
p.add_argument('--port', type=int, default=18140)
p.add_argument('--room-file', required=True)
p.add_argument('--seconds', type=int, default=60)
a = p.parse_args()
c = Client(a.host, a.port)
u = UdpEndpoint(a.host, a.port)
try:
    hello(c)
    room = create_room(c)
    rid = int(room['roomId'])
    token = bytes.fromhex(room['token'])
    configure(c, rid)
    u.register(token, c.session_id, rid)
    u.expect_ack(token, c.session_id, rid)
    pathlib.Path(a.room_file).write_text(room['roomCode'], encoding='ascii')
    print('READY control and UDP keepalive active; video packets=0', flush=True)
    until = time.monotonic() + a.seconds
    while time.monotonic() < until:
        assert c.request('ping')['ok']
        u.register(token, c.session_id, rid)
        u.expect_ack(token, c.session_id, rid)
        time.sleep(2)
    stop_room(c, rid)
finally:
    c.close()
    u.close()
    pathlib.Path(a.room_file).unlink(missing_ok=True)
