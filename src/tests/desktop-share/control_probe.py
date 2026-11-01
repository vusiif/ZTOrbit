#!/usr/bin/env python3
"""Real-host protocol probe for the desktop-share phase-2 room-control loop.

The probe only speaks the public wire contracts: one ZTS1 TCP frame per control JSON
object and one ZDS1 datagram per UDP registration. It is a test tool, not part of the
published server package.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import select
import socket
import struct
import sys
import time
from dataclasses import dataclass, field

ROUTE = b"desktop-share"
TCP_MAGIC = b"ZTS1"
ZTS1_UDP = TCP_MAGIC + bytes([1, len(ROUTE)]) + ROUTE
MEDIA_HEADER = struct.Struct(">4sBBH16sQQQQQQIIHHHH")
MEDIA_HEADER_BYTES = MEDIA_HEADER.size


class ProbeError(RuntimeError):
    pass


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def require(condition: bool, message: str) -> None:
    if condition:
        log(f"PASS {message}")
        return
    log(f"FAIL {message}")
    raise ProbeError(message)


def require_equal(actual, expected, message: str) -> None:
    require(actual == expected, f"{message} (expected {expected!r}, got {actual!r})")


def encode_control(document: dict) -> bytes:
    return json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


@dataclass
class Client:
    host: str
    port: int
    source_ip: str = "127.0.0.1"
    timeout: float = 5.0
    socket: socket.socket = field(init=False)
    buffer: bytearray = field(default_factory=bytearray, init=False)
    events: list[dict] = field(default_factory=list, init=False)
    next_id: int = 1
    session_id: int = 0
    udp_port: int = 0

    def __post_init__(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.bind((self.source_ip, 0))
        sock.settimeout(self.timeout)
        sock.connect((self.host, self.port))
        self.socket = sock

    def close(self) -> None:
        try:
            self.socket.close()
        except OSError:
            pass

    def send(self, operation: str, data: dict | None = None) -> int:
        request_id = self.next_id
        self.next_id += 1
        payload = encode_control({
            "v": 1,
            "id": request_id,
            "op": operation,
            "data": data or {},
        })
        frame = TCP_MAGIC + bytes([1, len(ROUTE)]) + ROUTE + payload
        self.socket.sendall(struct.pack(">I", len(frame)) + frame)
        return request_id

    def request(self, operation: str, data: dict | None = None,
                timeout: float | None = None) -> dict:
        request_id = self.send(operation, data)
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProbeError(f"{operation} response timed out")
            message = self.read_message(remaining)
            if "event" in message:
                self.events.append(message)
                continue
            if message.get("id") == request_id:
                return message
            raise ProbeError(f"unexpected control response {message}")

    def read_message(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            message = self._extract()
            if message is not None:
                return message
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("control frame timeout")
            readable, _, _ = select.select([self.socket], [], [], remaining)
            if not readable:
                raise TimeoutError("control frame timeout")
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the control connection")
            self.buffer.extend(chunk)

    def _extract(self) -> dict | None:
        if len(self.buffer) < 4:
            return None
        length = struct.unpack(">I", self.buffer[:4])[0]
        if length < 7 or length > 1024 * 1024:
            raise ProbeError(f"invalid ZTS1 frame length {length}")
        if len(self.buffer) < 4 + length:
            return None
        frame = bytes(self.buffer[4:4 + length])
        del self.buffer[:4 + length]
        if frame[:4] != TCP_MAGIC or frame[4] != 1:
            raise ProbeError("invalid ZTS1 header in response")
        route_length = frame[5]
        route = frame[6:6 + route_length]
        if route != ROUTE:
            raise ProbeError(f"response used route {route!r}")
        return json.loads(frame[6 + route_length:].decode("utf-8"))

    def wait_event(self, kind: str, timeout: float,
                   ping_clients: tuple["Client", ...] = (),
                   ping_interval: float = 3.0) -> dict:
        deadline = time.monotonic() + timeout
        next_ping = time.monotonic() + ping_interval
        while True:
            for index, event in enumerate(self.events):
                if event.get("event") == kind:
                    return self.events.pop(index)
            now = time.monotonic()
            if now >= deadline:
                raise ProbeError(f"event {kind} timed out")
            if ping_clients and now >= next_ping:
                for client in ping_clients:
                    client.request("ping", {}, timeout=2.0)
                next_ping = time.monotonic() + ping_interval
                continue
            try:
                message = self.read_message(min(0.25, max(0.0, deadline - now)))
            except TimeoutError:
                continue
            if "event" in message:
                self.events.append(message)


@dataclass
class UdpEndpoint:
    host: str
    port: int
    source_ip: str = "127.0.0.1"
    socket: socket.socket = field(init=False)

    def __post_init__(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.source_ip, 0))
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket = sock

    @property
    def local_port(self) -> int:
        return self.socket.getsockname()[1]

    def close(self) -> None:
        self.socket.close()

    def register(self, token: bytes, session_id: int, room_id: int) -> None:
        self._send(1, token, session_id, room_id)

    def send_video(self, token: bytes, session_id: int, room_id: int, epoch: int) -> None:
        data = b"\x01\x02\x03\x04"
        payload = MEDIA_HEADER.pack(
            b"ZDS1", 1, 3, 1, token, session_id, room_id, epoch, 1, 0, 0, 0,
            len(data), 0, 1, len(data), 0) + data
        self.socket.sendto(ZTS1_UDP + payload, (self.host, self.port))

    def _send(self, packet_type: int, token: bytes, session_id: int, room_id: int) -> None:
        payload = MEDIA_HEADER.pack(
            b"ZDS1", 1, packet_type, 0, token, session_id, room_id,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        self.socket.sendto(ZTS1_UDP + payload, (self.host, self.port))

    def receive(self, timeout: float) -> tuple | None:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            readable, _, _ = select.select([self.socket], [], [], remaining)
            if not readable:
                return None
            payload, _ = self.socket.recvfrom(2048)
            if payload[:4] == TCP_MAGIC and len(payload) >= 7 and payload[4] == 1:
                route_length = payload[5]
                payload = payload[6 + route_length:]
            if len(payload) != MEDIA_HEADER_BYTES:
                continue
            fields = MEDIA_HEADER.unpack(payload)
            if fields[0] != b"ZDS1" or fields[1] != 1:
                continue
            return fields

    def expect_ack(self, token: bytes, session_id: int, room_id: int,
                   timeout: float = 2.0) -> None:
        fields = self.receive(timeout)
        require(fields is not None, "registration acknowledgement received")
        require_equal(fields[2], 2, "acknowledgement type")
        require(fields[4] == token, "acknowledgement token matches")
        require_equal(fields[5], session_id, "acknowledgement session")
        require_equal(fields[6], room_id, "acknowledgement room")


def hello(client: Client) -> dict:
    response = client.request("hello")
    require(response["ok"], "hello succeeds")
    data = response["data"]
    client.session_id = int(data["sessionId"])
    client.udp_port = int(data["udpPort"])
    require(client.session_id > 0, "hello returns a positive session id")
    require("720p30" in data["capabilities"]["profiles"], "720p30 advertised")
    require_equal(int(data["capabilities"]["maxViewers"]), 8, "max viewers advertised")
    return data


def create_room(client: Client, profile: str = "720p30") -> dict:
    response = client.request("create", {"profile": profile})
    require(response["ok"], f"create {profile} succeeds")
    room = response["data"]
    require(len(room["roomCode"]) == 8 and room["roomCode"].isdigit(),
            "room code is eight digits")
    require(len(room["token"]) == 32, "publisher token is 32 hex characters")
    require_equal(room["role"], "publisher", "create role")
    return room


def join_room(client: Client, code: str, expect_ok: bool = True) -> dict:
    response = client.request("join", {"roomCode": code})
    if expect_ok:
        require(response["ok"], "join succeeds")
    return response


def configure(client: Client, room_id: int, profile: str = "720p30") -> dict:
    if profile == "1080p30":
        width, height, bitrate = 1920, 1080, 4_000_000
    else:
        width, height, bitrate = 1280, 720, 2_000_000
    response = client.request("stream.configure", {
        "roomId": str(room_id),
        "codec": "h264",
        "width": width,
        "height": height,
        "fps": 30,
        "bitrate": bitrate,
        "initializationData": "",
    })
    require(response["ok"], "stream.configure succeeds")
    return response["data"]


def ready(client: Client, room_id: int, epoch: int) -> None:
    response = client.request("stream.ready",
                              {"roomId": str(room_id), "streamEpoch": str(epoch)})
    require(response["ok"], "stream.ready succeeds")


def stop_room(client: Client, room_id: int) -> None:
    response = client.request("stop", {"roomId": str(room_id)})
    require(response["ok"], "stop succeeds")


def close_clients(*clients: Client) -> None:
    for client in clients:
        client.close()


def scenario_flow(host: str, port: int) -> None:
    log("scenario flow: create/join/configure/ready/leave/stop round trip")
    publisher = Client(host, port)
    viewer = Client(host, port)
    late = Client(host, port)
    publisher_udp = UdpEndpoint(host, port)
    viewer_udp = UdpEndpoint(host, port)
    try:
        hello(publisher)
        hello(viewer)
        hello(late)
        room = create_room(publisher)
        code, room_id = room["roomCode"], int(room["roomId"])
        publisher_token = bytes.fromhex(room["token"])

        joined = join_room(viewer, code)["data"]
        require_equal(joined["role"], "viewer", "join role")
        require_equal(joined["roomId"], str(room_id), "join room id")
        require(joined.get("format") is None, "preparing viewer sees no format yet")
        viewer_token = bytes.fromhex(joined["token"])
        require(viewer_token != publisher_token, "viewer token differs from publisher token")
        preparing = publisher.wait_event("room.state", 3)
        require_equal(preparing["data"]["state"], "preparing", "room state after join")
        require_equal(preparing["data"]["viewerCount"], 1, "viewer count after join")

        configured = configure(publisher, room_id)
        epoch = int(configured["streamEpoch"])
        require_equal(epoch, 1, "first stream epoch")
        config_event = viewer.wait_event("stream.config", 3)
        require_equal(int(config_event["data"]["format"]["streamEpoch"]), epoch,
                      "stream.config epoch")
        require_equal(config_event["data"]["format"]["width"], 1280,
                      "stream.config width")
        running = publisher.wait_event("room.state", 3)
        require_equal(running["data"]["state"], "running", "room state after configure")
        ready(viewer, room_id, epoch)

        publisher_udp.register(publisher_token, publisher.session_id, room_id)
        publisher_udp.expect_ack(publisher_token, publisher.session_id, room_id)
        viewer_udp.register(viewer_token, viewer.session_id, room_id)
        viewer_udp.expect_ack(viewer_token, viewer.session_id, room_id)

        late_join = join_room(late, code)["data"]
        require_equal(int(late_join["format"]["streamEpoch"]), epoch,
                      "late viewer receives current format")
        late_udp = UdpEndpoint(host, port)
        try:
            late_udp.register(bytes.fromhex(late_join["token"]), late.session_id, room_id)
            late_udp.expect_ack(bytes.fromhex(late_join["token"]), late.session_id, room_id)
        finally:
            late_udp.close()
        crowd = publisher.wait_event("room.state", 3)
        require_equal(crowd["data"]["viewerCount"], 2, "two viewers reported")

        response = viewer.request("leave", {"roomId": str(room_id)})
        require(response["ok"], "viewer leave succeeds")
        after_leave = publisher.wait_event("room.state", 3)
        require_equal(after_leave["data"]["viewerCount"], 1, "viewer count after leave")
        rejoined = join_room(viewer, code)["data"]
        require(bytes.fromhex(rejoined["token"]) != viewer_token,
                "rejoin issues a new media token")
        require_equal(int(rejoined["format"]["streamEpoch"]), epoch,
                      "rejoin returns current format")

        stop_room(publisher, room_id)
        closed = late.wait_event("room.closed", 3)
        require_equal(closed["data"]["reason"], "owner_stopped", "room closed reason")
        old = join_room(viewer, code, expect_ok=False)
        require(not old["ok"], "stopped room code is invalid")
        require_equal(old["error"]["code"], "ROOM_NOT_FOUND", "old room code error")
    finally:
        publisher_udp.close()
        viewer_udp.close()
        close_clients(publisher, viewer, late)


def scenario_udp_binding(host: str, port: int) -> None:
    log("scenario udp-binding: peer IP, token and fixed port binding")
    publisher = Client(host, port)
    correct = UdpEndpoint(host, port)
    changed = UdpEndpoint(host, port)
    foreign = UdpEndpoint(host, port, source_ip="127.0.0.2")
    try:
        hello(publisher)
        room = create_room(publisher)
        room_id = int(room["roomId"])
        token = bytes.fromhex(room["token"])

        foreign.register(token, publisher.session_id, room_id)
        require(foreign.receive(0.7) is None, "foreign source IP is rejected")

        correct.register(token, publisher.session_id, room_id)
        correct.expect_ack(token, publisher.session_id, room_id)

        changed.register(token, publisher.session_id, room_id)
        require(changed.receive(0.7) is None, "port change after binding is rejected")

        correct.register(token, publisher.session_id, room_id)
        correct.expect_ack(token, publisher.session_id, room_id)
        stop_room(publisher, room_id)
    finally:
        correct.close()
        changed.close()
        foreign.close()
        publisher.close()


def scenario_publisher_disconnect(host: str, port: int) -> None:
    log("scenario publisher-disconnect: immediate room closure")
    publisher = Client(host, port)
    viewer = Client(host, port)
    try:
        hello(publisher)
        hello(viewer)
        room = create_room(publisher)
        join_room(viewer, room["roomCode"])
        publisher.close()
        closed = viewer.wait_event("room.closed", 5)
        require_equal(closed["data"]["reason"], "owner_disconnected",
                      "publisher disconnect reason")
    finally:
        viewer.close()


def scenario_prepare_timeout(host: str, port: int) -> None:
    log("scenario prepare-timeout: no stream.configure within ten seconds")
    publisher = Client(host, port)
    viewer = Client(host, port)
    publisher_udp = UdpEndpoint(host, port)
    viewer_udp = UdpEndpoint(host, port)
    try:
        hello(publisher)
        hello(viewer)
        room = create_room(publisher)
        room_id = int(room["roomId"])
        joined = join_room(viewer, room["roomCode"])["data"]
        publisher_udp.register(bytes.fromhex(room["token"]), publisher.session_id, room_id)
        publisher_udp.expect_ack(bytes.fromhex(room["token"]), publisher.session_id, room_id)
        viewer_udp.register(bytes.fromhex(joined["token"]), viewer.session_id, room_id)
        viewer_udp.expect_ack(bytes.fromhex(joined["token"]), viewer.session_id, room_id)
        started = time.monotonic()
        closed = viewer.wait_event("room.closed", 13)
        elapsed = time.monotonic() - started
        require_equal(closed["data"]["reason"], "config_timeout",
                      "prepare timeout reason")
        require(9.0 <= elapsed <= 12.5, f"prepare timeout elapsed {elapsed:.2f}s")
    finally:
        publisher_udp.close()
        viewer_udp.close()
        close_clients(publisher, viewer)


def scenario_udp_expiry(host: str, port: int) -> None:
    log("scenario udp-expiry: publisher media endpoint expires while control stays alive")
    publisher = Client(host, port)
    viewer = Client(host, port)
    publisher_udp = UdpEndpoint(host, port)
    viewer_udp = UdpEndpoint(host, port)
    try:
        hello(publisher)
        hello(viewer)
        room = create_room(publisher)
        room_id = int(room["roomId"])
        joined = join_room(viewer, room["roomCode"])["data"]
        configure(publisher, room_id)
        publisher_udp.register(bytes.fromhex(room["token"]), publisher.session_id, room_id)
        publisher_udp.expect_ack(bytes.fromhex(room["token"]), publisher.session_id, room_id)
        viewer_udp.register(bytes.fromhex(joined["token"]), viewer.session_id, room_id)
        viewer_udp.expect_ack(bytes.fromhex(joined["token"]), viewer.session_id, room_id)
        started = time.monotonic()
        closed = viewer.wait_event("room.closed", 20,
                                   ping_clients=(publisher, viewer))
        elapsed = time.monotonic() - started
        require_equal(closed["data"]["reason"], "stream_failed",
                      "UDP endpoint expiry reason")
        require(14.0 <= elapsed <= 18.5, f"UDP endpoint expiry elapsed {elapsed:.2f}s")
    finally:
        publisher_udp.close()
        viewer_udp.close()
        close_clients(publisher, viewer)


def scenario_control_idle(host: str, port: int) -> None:
    log("scenario control-idle: silent publisher room closes at fifteen seconds")
    publisher = Client(host, port)
    viewer = Client(host, port)
    publisher_udp = UdpEndpoint(host, port)
    viewer_udp = UdpEndpoint(host, port)
    try:
        hello(publisher)
        hello(viewer)
        room = create_room(publisher)
        room_id = int(room["roomId"])
        joined = join_room(viewer, room["roomCode"])["data"]
        configure(publisher, room_id)
        publisher_udp.register(bytes.fromhex(room["token"]), publisher.session_id, room_id)
        publisher_udp.expect_ack(bytes.fromhex(room["token"]), publisher.session_id, room_id)
        viewer_udp.register(bytes.fromhex(joined["token"]), viewer.session_id, room_id)
        viewer_udp.expect_ack(bytes.fromhex(joined["token"]), viewer.session_id, room_id)
        started = time.monotonic()
        closed = viewer.wait_event("room.closed", 20, ping_clients=(viewer,))
        elapsed = time.monotonic() - started
        require_equal(closed["data"]["reason"], "owner_lost",
                      "control idle reason")
        require(14.0 <= elapsed <= 18.5, f"control idle elapsed {elapsed:.2f}s")
    finally:
        publisher_udp.close()
        viewer_udp.close()
        close_clients(publisher, viewer)


def scenario_join_rate_limit(host: str, port: int) -> None:
    log("scenario join-rate-limit: ten failed joins block the source address")
    publisher = Client(host, port, source_ip="127.0.0.1")
    try:
        hello(publisher)
        room = create_room(publisher)
        wrong_code = "99999999"
        if wrong_code == room["roomCode"]:
            wrong_code = "99999998"
        for attempt in range(10):
            attacker = Client(host, port, source_ip="127.0.0.3")
            try:
                hello(attacker)
                response = join_room(attacker, wrong_code, expect_ok=False)
                require(not response["ok"], f"failed join {attempt + 1} rejected")
                require_equal(response["error"]["code"], "ROOM_NOT_FOUND",
                              "failed join error")
            finally:
                attacker.close()
        blocked = Client(host, port, source_ip="127.0.0.3")
        try:
            hello(blocked)
            response = join_room(blocked, room["roomCode"], expect_ok=False)
            require(not response["ok"], "eleventh join from blocked address rejected")
            require_equal(response["error"]["code"], "RATE_LIMITED",
                          "rate limited error")
        finally:
            blocked.close()
        other = Client(host, port, source_ip="127.0.0.4")
        try:
            hello(other)
            join_room(other, room["roomCode"])
        finally:
            other.close()
        stop_room(publisher, int(room["roomId"]))
    finally:
        publisher.close()


def scenario_hold_room(host: str, port: int, hold_seconds: int,
                       code_out: str | None) -> None:
    log(f"mode hold-room: keeping one room alive for {hold_seconds}s")
    publisher = Client(host, port)
    udp = UdpEndpoint(host, port)
    try:
        hello(publisher)
        room = create_room(publisher)
        room_id = int(room["roomId"])
        configure(publisher, room_id)
        token = bytes.fromhex(room["token"])
        udp.register(token, publisher.session_id, room_id)
        udp.expect_ack(token, publisher.session_id, room_id)
        code = room["roomCode"]
        if code_out:
            pathlib.Path(code_out).write_text(code + "\n", encoding="utf-8")
        log(f"LEFTOVER_ROOM_CODE={code} ROOM_ID={room_id}")
        deadline = time.monotonic() + hold_seconds
        while time.monotonic() < deadline:
            time.sleep(1)
            publisher.request("ping", {}, timeout=2.0)
            udp.register(token, publisher.session_id, room_id)
            if udp.receive(0.3) is not None:
                continue
            break
    except (ConnectionError, OSError, TimeoutError, ProbeError) as error:
        log(f"hold-room ended: {error}")
    finally:
        udp.close()
        publisher.close()


def scenario_join_only(host: str, port: int, code: str) -> None:
    log(f"mode join-only: checking stale code {code}")
    client = Client(host, port)
    try:
        hello(client)
        response = join_room(client, code, expect_ok=False)
        require(not response["ok"], "stale room code is rejected after restart")
        require_equal(response["error"]["code"], "ROOM_NOT_FOUND",
                      "stale room code error")
    finally:
        client.close()


SCENARIOS = {
    "flow": scenario_flow,
    "udp-binding": scenario_udp_binding,
    "publisher-disconnect": scenario_publisher_disconnect,
    "prepare-timeout": scenario_prepare_timeout,
    "udp-expiry": scenario_udp_expiry,
    "control-idle": scenario_control_idle,
    "join-rate-limit": scenario_join_rate_limit,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18100)
    parser.add_argument("--mode", default="all",
                        choices=["all", "hold-room", "join-only", *SCENARIOS])
    parser.add_argument("--code", default="")
    parser.add_argument("--code-out", default="")
    parser.add_argument("--hold-seconds", type=int, default=60)
    args = parser.parse_args()

    if args.mode == "hold-room":
        scenario_hold_room(args.host, args.port, args.hold_seconds, args.code_out)
        return 0
    if args.mode == "join-only":
        if not args.code:
            parser.error("--code is required with --mode join-only")
        try:
            scenario_join_only(args.host, args.port, args.code)
            return 0
        except ProbeError:
            return 1

    selected = (list(SCENARIOS) if args.mode == "all" else [args.mode])
    failed = []
    for name in selected:
        try:
            SCENARIOS[name](args.host, args.port)
            log(f"SCENARIO PASS {name}")
        except Exception as error:  # noqa: BLE001 - the probe reports any failure
            log(f"SCENARIO FAIL {name}: {error}")
            failed.append(name)
    if failed:
        log(f"FAILED SCENARIOS: {', '.join(failed)}")
        return 1
    log(f"ALL SCENARIOS PASSED: {', '.join(selected)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
