#!/usr/bin/env python3
"""Stage-4 forwarding probe for the real desktop-share Java server.

The probe speaks only the public ZTS1/ZDS1 wire contracts and never touches the
server implementation. It verifies that the server forwards exactly one copy per
ready viewer, replaces only the target token/session, keeps the encoded payload
verbatim, does not reassemble packets, gates viewers by ready epoch and isolates
rooms. Tokens, room codes and payloads are never printed; only counts and
redacted identities appear in the output.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from control_probe import (  # noqa: E402
    MEDIA_HEADER,
    ZTS1_UDP,
    Client,
    ProbeError,
    UdpEndpoint,
    close_clients,
    configure,
    create_room,
    hello,
    join_room,
    log,
    ready,
    require,
    require_equal,
    stop_room,
)

KEY_FRAME = 0x0001


class VideoUdp(UdpEndpoint):
    def send_fragment(self, token: bytes, session_id: int, room_id: int, epoch: int,
                      sequence: int, flags: int, packet_bytes: int, index: int,
                      count: int, data: bytes) -> None:
        payload = MEDIA_HEADER.pack(
            b"ZDS1", 1, 3, flags, token, session_id, room_id, epoch, sequence,
            0, 0, 0, packet_bytes, index, count, len(data), 0) + data
        self.socket.sendto(ZTS1_UDP + payload, (self.host, self.port))

    def receive_video(self, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            payload = self._receive_payload(remaining)
            if payload is None:
                return None
            if len(payload) < 88:
                continue
            fields = MEDIA_HEADER.unpack(payload[:88])
            if fields[0] != b"ZDS1" or fields[1] != 1:
                continue
            if fields[2] == 2:  # registration ack
                continue
            if fields[2] != 3:
                continue
            return fields, payload[88:]

    def _receive_payload(self, timeout: float):
        import select

        readable, _, _ = select.select([self.socket], [], [], timeout)
        if not readable:
            return None
        payload, _ = self.socket.recvfrom(2048)
        if payload[:4] == ZTS1_UDP[:4] and len(payload) >= 7 and payload[4] == 1:
            route_length = payload[5]
            payload = payload[6 + route_length:]
        return payload

    def drain(self, window: float = 0.5) -> list:
        packets = []
        while True:
            packet = self.receive_video(window)
            if packet is None:
                return packets
            packets.append(packet)


def register(endpoint: VideoUdp, room: dict, session_id: int) -> None:
    token = bytes.fromhex(room["token"])
    endpoint.register(token, session_id, int(room["roomId"]))
    endpoint.expect_ack(token, session_id, int(room["roomId"]))


def scenario_forwarding(host: str, port: int) -> dict:
    log("video scenario: one publisher, two viewers, ready gating, second room")
    publisher = Client(host, port)
    viewer_one = Client(host, port)
    viewer_two = Client(host, port)
    publisher_udp = VideoUdp(host, port)
    one_udp = VideoUdp(host, port)
    two_udp = VideoUdp(host, port)
    summary = {"accepted": 0, "forwarded": 0, "missing_fragments": 0}
    try:
        hello(publisher)
        hello(viewer_one)
        hello(viewer_two)
        room = create_room(publisher)
        room_id = int(room["roomId"])
        publisher_token = bytes.fromhex(room["token"])
        register(publisher_udp, room, publisher.session_id)

        epoch = int(configure(publisher, room_id)["streamEpoch"])
        first = join_room(viewer_one, room["roomCode"])["data"]
        second = join_room(viewer_two, room["roomCode"])["data"]
        one_token = bytes.fromhex(first["token"])
        two_token = bytes.fromhex(second["token"])
        require(one_token != publisher_token and two_token != publisher_token,
                "viewer tokens differ from the publisher token")
        register(one_udp, {"token": first["token"], "roomId": str(room_id)},
                 viewer_one.session_id)
        register(two_udp, {"token": second["token"], "roomId": str(room_id)},
                 viewer_two.session_id)
        ready(viewer_one, room_id, epoch)

        # Three single-fragment packets reach the only ready viewer and nobody else.
        for sequence in range(3):
            payload = bytes([0x10 + sequence]) * 400
            publisher_udp.send_fragment(publisher_token, publisher.session_id, room_id,
                                        epoch, sequence, KEY_FRAME, len(payload),
                                        0, 1, payload)
            summary["accepted"] += 1
        packets = one_udp.drain(1.0)
        require_equal(len(packets), 3, "ready viewer receives each forwarded fragment once")
        summary["forwarded"] += len(packets)
        sent = {sequence: bytes([0x10 + sequence]) * 400 for sequence in range(3)}
        for fields, payload in packets:
            require(fields[4] == one_token, "token is retargeted to the viewer")
            require_equal(fields[5], viewer_one.session_id, "session is retargeted")
            require_equal(fields[6], room_id, "room id preserved")
            require_equal(fields[7], epoch, "stream epoch preserved")
            require(fields[3] & KEY_FRAME, "key frame flag preserved")
            require(payload == sent[fields[8]], "encoded payload preserved verbatim")
        require_equal(one_udp.drain(0.3), [], "no duplicate datagrams to the ready viewer")
        require_equal(two_udp.drain(0.3), [], "a viewer that is not ready receives nothing")

        # After the second viewer confirms the epoch it only sees new fragments.
        ready(viewer_two, room_id, epoch)
        payload = b"\x33" * 300
        publisher_udp.send_fragment(publisher_token, publisher.session_id, room_id, epoch,
                                    3, KEY_FRAME, len(payload), 0, 1, payload)
        summary["accepted"] += 1
        second_packets = two_udp.drain(1.0)
        require_equal(len(second_packets), 1, "late-ready viewer receives new fragments only")
        require(second_packets[0][0][4] == two_token, "second viewer token retargeted")
        require_equal(second_packets[0][0][5], viewer_two.session_id, "second viewer session")
        summary["forwarded"] += len(second_packets)
        first_packets = one_udp.drain(0.5)
        require_equal(len(first_packets), 1, "first viewer keeps receiving")
        summary["forwarded"] += len(first_packets)

        # A three-fragment packet is forwarded fragment by fragment, never reassembled.
        body = bytes(range(256)) * 12
        offset = 0
        for index, size in enumerate((1125, 1125, 822)):
            chunk = body[offset:offset + size]
            offset += size
            publisher_udp.send_fragment(publisher_token, publisher.session_id, room_id,
                                        epoch, 4, KEY_FRAME, 3072, index, 3, chunk)
            summary["accepted"] += 1
        fragments = one_udp.drain(1.0)
        require_equal(len(fragments), 3, "each fragment is forwarded individually")
        require_equal([entry[0][13] for entry in fragments], [0, 1, 2], "fragment order kept")
        require_equal([len(entry[1]) for entry in fragments], [1125, 1125, 822],
                      "fragment sizes preserved")
        for fields, chunk in fragments:
            require_equal(fields[12], 3072, "packet size metadata preserved")
            require_equal(fields[14], 3, "fragment count preserved")
        summary["forwarded"] += len(fragments)

        # Wrong epoch and cross-room session IDs are dropped.
        publisher_udp.send_fragment(publisher_token, publisher.session_id, room_id,
                                    epoch + 5, 5, KEY_FRAME, 10, 0, 1, b"0123456789")
        publisher_udp.send_fragment(publisher_token, publisher.session_id, room_id + 99,
                                    epoch, 6, KEY_FRAME, 10, 0, 1, b"0123456789")
        require_equal(one_udp.drain(0.5), [], "wrong epoch and cross-room fragments are dropped")

        # Second isolated room.
        publisher_two = Client(host, port)
        viewer_three = Client(host, port)
        publisher_two_udp = VideoUdp(host, port)
        three_udp = VideoUdp(host, port)
        hello(publisher_two)
        hello(viewer_three)
        room_two = create_room(publisher_two, "1080p30")
        room_two_id = int(room_two["roomId"])
        publisher_two_token = bytes.fromhex(room_two["token"])
        register(publisher_two_udp, room_two, publisher_two.session_id)
        epoch_two = int(configure(publisher_two, room_two_id, "1080p30")["streamEpoch"])
        joined_three = join_room(viewer_three, room_two["roomCode"])["data"]
        three_token = bytes.fromhex(joined_three["token"])
        register(three_udp, {"token": joined_three["token"], "roomId": str(room_two_id)},
                 viewer_three.session_id)
        ready(viewer_three, room_two_id, epoch_two)
        publisher_two_udp.send_fragment(publisher_two_token, publisher_two.session_id,
                                        room_two_id, epoch_two, 0, KEY_FRAME, 32, 0, 1,
                                        b"\x77" * 32)
        summary["accepted"] += 1
        room_two_packets = three_udp.drain(1.0)
        require_equal(len(room_two_packets), 1, "second room viewer receives its own stream")
        summary["forwarded"] += len(room_two_packets)
        require_equal(one_udp.drain(0.4), [], "room one viewer never sees room two video")
        require_equal(room_two_packets[0][0][6], room_two_id, "room two id preserved")

        # Stopping the publisher ends forwarding immediately.
        stop_room(publisher, room_id)
        require_equal(one_udp.drain(0.6), [], "stopped room forwards nothing")
        require_equal(three_udp.drain(0.3), [], "other room still unaffected")
        stop_room(publisher_two, room_two_id)
        close_clients(publisher, viewer_one, viewer_two, publisher_two, viewer_three)
        return summary
    finally:
        for endpoint in (publisher_udp, one_udp, two_udp):
            endpoint.close()


def scenario_restart(host: str, port: int) -> None:
    """Quick check that a fresh server has no rooms from the previous process."""
    client = Client(host, port)
    try:
        hello(client)
        response = join_room(client, "00000000", expect_ok=False)
        require(not response["ok"], "unknown room rejected after restart")
        require_equal(response["error"]["code"], "ROOM_NOT_FOUND",
                      "restart leaves no recoverable room")
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18100)
    parser.add_argument("--mode", choices=["forwarding", "restart", "all"], default="forwarding")
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    results = {}
    try:
        if args.mode in ("forwarding", "all"):
            results["forwarding"] = scenario_forwarding(args.host, args.port)
        if args.mode in ("restart", "all"):
            scenario_restart(args.host, args.port)
            results["restart"] = "passed"
        results["status"] = "passed"
    except ProbeError as error:
        results["status"] = "failed"
        results["error"] = str(error)
        log(f"FAIL {error}")
        print(json.dumps(results, indent=2, sort_keys=True))
        return 1
    log(json.dumps(results, sort_keys=True))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(json.dumps(results, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
