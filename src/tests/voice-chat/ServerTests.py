"""Headless room/UDP integration tests. Run locally beside the server binary,
or with --host/--port to validate an already deployed server.
"""
import argparse
import json
import socket
import struct
import subprocess
import time
import unittest
import tempfile
from pathlib import Path
import shutil

OPTIONS = None


ROUTE = b"voice-chat"
def envelope(payload):
    return b"ZTS1" + bytes([1, len(ROUTE)]) + ROUTE + payload
def frame(payload):
    packet = envelope(payload)
    return struct.pack("!I",len(packet)) + packet
def read_response(reader):
    prefix = reader.read(4)
    if len(prefix) != 4: raise EOFError("Connection closed")
    size = struct.unpack("!I",prefix)[0]
    if size > 1048576: raise ValueError("Oversized frame")
    packet = reader.read(size)
    assert packet[:6] == b"ZTS1" + bytes([1,len(ROUTE)]) and packet[6:6+len(ROUTE)] == ROUTE
    return json.loads(packet[6+len(ROUTE):])
class RoutedDatagram:
    def __init__(self): self.socket = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    def __getattr__(self,name): return getattr(self.socket,name)
    def sendto(self,data,destination): return self.socket.sendto(envelope(data),destination)
    def recvfrom(self,size):
        packet,endpoint = self.socket.recvfrom(1233)
        assert packet[:6] == b"ZTS1" + bytes([1,len(ROUTE)]) and packet[6:6+len(ROUTE)] == ROUTE
        return packet[6+len(ROUTE):],endpoint


class Client:
    def __init__(self, host, port, nickname="test"):
        self.tcp = socket.create_connection((host, port), timeout=2)
        self.reader = self.tcp.makefile("rb")
        self.udp = RoutedDatagram()
        self.udp.bind(("0.0.0.0", 0))
        self.udp.settimeout(0.25)
        self.serial = 0
        self.session = 0
        self.token = b""
        hello = self.call("hello", {"nickname": nickname})
        assert hello["ok"], hello
        self.session = hello["data"]["session"]
        self.destination = (host, hello["data"]["udpPort"])

    def call(self, op, data=None, split=False):
        self.serial += 1
        message = frame(json.dumps({"v": 1, "id": self.serial, "op": op, "data": data or {}}).encode())
        if split:
            self.tcp.sendall(message[:7])
            time.sleep(0.01)
            self.tcp.sendall(message[7:])
        else:
            self.tcp.sendall(message)
        while True:
            response = read_response(self.reader)
            if response.get("id") == self.serial:
                return response

    def enter(self, response):
        assert response["ok"], response
        self.token = bytes.fromhex(response["data"]["token"])
        self.udp.sendto(self.packet(1), self.destination)
        data, _ = self.udp.recvfrom(1201)
        assert data == self.packet(2)
        return response["data"]["room"]["id"]

    def packet(self, kind, payload=b"", seq=1):
        return b"ZTV1" + bytes([1, kind]) + self.token + struct.pack("!II", self.session, seq) + payload

    def close(self):
        self.reader.close()
        self.tcp.close()
        self.udp.close()


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.process = None
        self.directory = None
        self.clients = []
        self.host, self.port = OPTIONS.host, OPTIONS.port
        if OPTIONS.binary:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                self.port = probe.getsockname()[1]
            self.host = "127.0.0.1"
            self.directory = tempfile.TemporaryDirectory()
            root = Path(self.directory.name)
            (root/"plugins").mkdir()
            for plugin in (Path(OPTIONS.binary).resolve().parent/"plugins").glob("*.ztplugin"):
                shutil.copyfile(plugin,root/"plugins"/plugin.name)
            config = root/"plugins-data"/"语音聊天"
            config.mkdir(parents=True)
            (config/"settings.json").write_text(json.dumps({"maxRooms":2,"maxMembers":2}),encoding="utf-8")
            self.process = subprocess.Popen([OPTIONS.binary,"--server","--listen",self.host,"--port",str(self.port)],
                cwd=root,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            for _ in range(100):
                try:
                    with socket.create_connection((self.host, self.port), timeout=0.1):
                        break
                except OSError:
                    if self.process.poll() is not None:
                        self.fail("Server exited during startup")
                    time.sleep(0.02)
            else:
                self.fail("Server did not start")

    def tearDown(self):
        for client in self.clients:
            client.close()
        if self.process:
            self.process.terminate()
            self.process.wait(timeout=5)
        if self.directory: self.directory.cleanup()

    def client(self, name="test"):
        client = Client(self.host, self.port, name)
        self.clients.append(client)
        return client

    def test_room_lifecycle_and_validation(self):
        a, b = self.client("A"), self.client("B")
        self.assertFalse(a.call("create", {"name": " "})["ok"])
        room = a.enter(a.call("create", {"name": "integration-room"}, split=True))
        b.enter(b.call("join", {"room": room}))
        self.assertEqual(next(r for r in a.call("rooms")["data"] if r["id"] == room)["members"], 2)
        self.assertEqual(a.call("join", {"room": room})["error"], "already_in_room")
        self.assertTrue(a.call("mute", {"muted": True})["ok"])
        a.call("leave")
        self.assertEqual(next(r for r in b.call("rooms")["data"] if r["id"] == room)["members"], 1)
        b.call("leave")
        self.assertFalse(any(r["id"] == room for r in a.call("rooms")["data"]))
        self.assertEqual(a.call("join", {"room": room})["error"], "room_not_found")

    def test_udp_isolation_mute_and_expired_credentials(self):
        a, b, c = self.client("A"), self.client("B"), self.client("C")
        room = a.enter(a.call("create", {"name": "same-room"}))
        b.enter(b.call("join", {"room": room}))
        c.enter(c.call("create", {"name": "other-room"}))
        payload = b"test-opus-payload"
        a.udp.sendto(a.packet(3, payload), a.destination)
        received, _ = b.udp.recvfrom(1201)
        self.assertEqual(received[6:22], b.token)
        self.assertEqual(struct.unpack("!I", received[22:26])[0], a.session)
        self.assertEqual(received[30:], payload)
        with self.assertRaises(socket.timeout):
            a.udp.recvfrom(1201)
        with self.assertRaises(socket.timeout):
            c.udp.recvfrom(1201)
        a.call("mute", {"muted": True})
        a.udp.sendto(a.packet(3, payload), a.destination)
        with self.assertRaises(socket.timeout):
            b.udp.recvfrom(1201)
        expired = a.packet(3, payload)
        a.call("leave")
        a.enter(a.call("join", {"room": room}))
        a.udp.sendto(expired, a.destination)
        a.udp.sendto(b"invalid", a.destination)
        a.udp.sendto(b"x" * 1201, a.destination)
        with self.assertRaises(socket.timeout):
            b.udp.recvfrom(1201)

    def test_capacity(self):
        if not OPTIONS.binary:
            self.skipTest("requires an isolated server with test capacity limits")
        a, b, c = self.client(), self.client(), self.client()
        room = a.enter(a.call("create", {"name": "full"}))
        b.enter(b.call("join", {"room": room}))
        self.assertEqual(c.call("join", {"room": room})["error"], "room_full")
        c.enter(c.call("create", {"name": "second"}))
        d = self.client()
        self.assertEqual(d.call("create", {"name": "third"})["error"], "room_limit")

    def test_malformed_control_rejected(self):
        for data in [b"{not-json}\n", b"x" * 65537, b'{"v":2,"id":1,"op":"hello"}\n']:
            with socket.create_connection((self.host, self.port), timeout=2) as connection:
                connection.sendall(frame(data))
                if data.startswith(b'{"v":2'):
                    response = read_response(connection.makefile("rb"))
                    self.assertEqual(response["error"], "protocol_mismatch")
                else:
                    try:
                        self.assertEqual(connection.recv(65536), b"")
                    except ConnectionResetError:
                        pass

    def test_dead_session_removed(self):
        if not OPTIONS.binary:
            self.skipTest("requires an isolated server for timeout testing")
        a = self.client()
        room = a.enter(a.call("create", {"name": "timeout"}))
        time.sleep(16.2)
        b = self.client()
        self.assertFalse(any(r["id"] == room for r in b.call("rooms")["data"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18100)
    OPTIONS = parser.parse_args()
    unittest.main(argv=[__file__], verbosity=2)
