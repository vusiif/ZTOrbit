#!/usr/bin/env python3
"""Deterministically synthesizes the two ZTGUI voice-chat cues.

The output is 48 kHz, mono, PCM16 RIFF/WAVE, 160 ms per cue. Each cue is
two 80 ms tones with a 5 ms fade at every boundary to avoid clicks.
No third-party audio material is used.
"""
from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

RATE = 48_000
TONES = {
    "join.wav": (660.0, 880.0),
    "leave.wav": (880.0, 660.0),
}
SEGMENT_SECONDS = 0.08
FADE_SECONDS = 0.005
AMPLITUDE = 0.35


def envelope(position: int, length: int) -> float:
    fade = max(1, int(RATE * FADE_SECONDS))
    if position < fade:
        return position / fade
    if position >= length - fade:
        return max(0.0, (length - position - 1) / fade)
    return 1.0


def synthesize(frequencies: tuple[float, float]) -> bytes:
    samples = bytearray()
    length = int(RATE * SEGMENT_SECONDS)
    for frequency in frequencies:
        for index in range(length):
            value = (
                math.sin(2.0 * math.pi * frequency * index / RATE)
                * envelope(index, length)
                * AMPLITUDE
            )
            samples += struct.pack("<h", int(round(value * 32767.0)))
    return bytes(samples)


def write_wav(path: Path, payload: bytes) -> None:
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(payload)


def main() -> None:
    root = Path(__file__).resolve().parent
    for name, frequencies in TONES.items():
        write_wav(root / name, synthesize(frequencies))


if __name__ == "__main__":
    main()
