#!/usr/bin/env python3
"""Summarise desktop-share UDP traffic on the server without printing payloads.

The tool captures raw IPv4/UDP headers on one interface and reports datagram and
byte counts grouped by (source, destination) endpoint. It never stores payload
bytes, tokens, room codes or control messages; the output is a redacted network
summary suitable for validation evidence.

Requires Linux and CAP_NET_RAW (run as root). Test tool only, never packaged.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time


def parse_udp(frame: bytes, offset: int):
    if len(frame) < offset + 20:
        return None
    version = frame[offset] >> 4
    if version != 4:
        return None
    ihl = (frame[offset] & 0x0F) * 4
    if ihl < 20 or len(frame) < offset + ihl + 8:
        return None
    protocol = frame[offset + 9]
    if protocol != 17:
        return None
    total_length = struct.unpack("!H", frame[offset + 2:offset + 4])[0]
    src = socket.inet_ntoa(frame[offset + 12:offset + 16])
    dst = socket.inet_ntoa(frame[offset + 16:offset + 20])
    udp = offset + ihl
    src_port, dst_port, length = struct.unpack("!HHH", frame[udp:udp + 6])
    return src, src_port, dst, dst_port, length, total_length


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="any")
    parser.add_argument("--port", type=int, default=18100)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--out", default="")
    parser.add_argument("--label", default="capture")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 * 1024 * 1024)
    except OSError:
        pass
    if args.interface != "any":
        sock.bind((args.interface, 0))
    sock.settimeout(0.2)
    groups: dict[tuple[str, int, str, int], dict[str, int]] = {}
    started = time.monotonic()
    frames = 0
    while time.monotonic() - started < args.seconds:
        try:
            frame, _ = sock.recvfrom(65536)
        except socket.timeout:
            continue
        frames += 1
        ethernet = 14
        if len(frame) >= 14 and frame[12:14] != b"\x08\x00":
            # VLAN tag or non-IPv4 Ethernet frame.
            if len(frame) >= 16 and frame[12:14] == b"\x81\x00" and frame[16:18] == b"\x08\x00":
                ethernet = 18
            else:
                continue
        parsed = parse_udp(frame, ethernet)
        if not parsed:
            continue
        src, src_port, dst, dst_port, udp_length, ip_length = parsed
        if args.port not in (src_port, dst_port):
            continue
        key = (src, src_port, dst, dst_port)
        group = groups.setdefault(key, {"datagrams": 0, "udp_bytes": 0, "ip_bytes": 0})
        group["datagrams"] += 1
        group["udp_bytes"] += udp_length
        group["ip_bytes"] += ip_length

    endpoints = [
        {
            "src": f"{src}:{src_port}",
            "dst": f"{dst}:{dst_port}",
            "datagrams": data["datagrams"],
            "udp_bytes": data["udp_bytes"],
            "ip_bytes": data["ip_bytes"],
        }
        for (src, src_port, dst, dst_port), data in sorted(groups.items())
    ]
    drops = 0
    try:
        # Linux constants: SOL_PACKET=263, PACKET_STATISTICS=6.
        stats = sock.getsockopt(263, 6, 8)
        _, drops = struct.unpack("II", stats)
    except (OSError, AttributeError):
        pass
    summary = {
        "label": args.label,
        "port": args.port,
        "seconds": round(args.seconds, 3),
        "frames_seen": frames,
        "kernel_drops": drops,
        "endpoints": endpoints,
    }
    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
