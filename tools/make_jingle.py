#!/usr/bin/env python3
"""Synthesises NSA's HOME banner jingle (2.5 s, 44.1 kHz stereo) into meta/audio_3ds.wav.

Four rising bell notes (C5 E5 G5 C6) and then a soft C major chord that rings out. Plain
Python, no dependencies. Run from the repository root.
"""
import math
import struct
import wave

RATE = 44100
LENGTH = 2.5
OUT = "meta/audio_3ds.wav"


def bell(t, f):
    # Two slightly detuned partials plus an inharmonic one that dies fast: a soft bell.
    return (math.sin(2 * math.pi * f * t)
            + 0.35 * math.sin(2 * math.pi * f * 2.0 * t) * math.exp(-4 * t)
            + 0.25 * math.sin(2 * math.pi * f * 2.76 * t) * math.exp(-9 * t)
            + 0.15 * math.sin(2 * math.pi * f * 3.003 * t))


def main():
    n = int(RATE * LENGTH)
    left = [0.0] * n
    right = [0.0] * n
    # (start s, frequency Hz, level, pan -1..1, decay rate)
    notes = [
        (0.00, 523.25, 0.55, -0.3, 3.0),
        (0.16, 659.25, 0.55, 0.0, 3.0),
        (0.32, 783.99, 0.55, 0.3, 3.0),
        (0.48, 1046.50, 0.60, 0.0, 2.6),
        (0.72, 261.63, 0.40, -0.5, 1.7),
        (0.72, 523.25, 0.40, -0.2, 1.7),
        (0.72, 659.25, 0.38, 0.2, 1.7),
        (0.72, 783.99, 0.38, 0.5, 1.7),
        (0.72, 1046.50, 0.34, 0.0, 1.7),
    ]
    for start, f, level, pan, decay in notes:
        s0 = int(start * RATE)
        gl = level * (1 - pan) / 2
        gr = level * (1 + pan) / 2
        for i in range(s0, n):
            t = (i - s0) / RATE
            attack = min(1.0, t / 0.004)
            v = bell(t, f) * attack * math.exp(-decay * t)
            left[i] += v * gl
            right[i] += v * gr
    # Normalise to -2 dB and fade the last 0.3 s so it ends in silence.
    peak = max(max(abs(x) for x in left), max(abs(x) for x in right))
    gain = 0.79 / peak
    fade = int(0.3 * RATE)
    with wave.open(OUT, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for i in range(n):
            k = 1.0 if i < n - fade else (n - i) / fade
            frames += struct.pack("<hh", int(left[i] * gain * k * 32767), int(right[i] * gain * k * 32767))
        w.writeframes(bytes(frames))


if __name__ == "__main__":
    main()
