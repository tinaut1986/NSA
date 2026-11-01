#!/usr/bin/env python3
"""Synthesises NSA's HOME banner jingle (2.5 s, 44.1 kHz stereo) with the instruments of the
Shop background music (marimba, electric piano, bossa bass, shaker, cross-stick).

    python3 tools/make_jingle.py                 # writes meta/audio_3ds.wav (variant "bossa")
    python3 tools/make_jingle.py --all DIR       # writes every variant into DIR to compare them
    python3 tools/make_jingle.py --variant NAME [--out FILE]

Variants: bossa (a bar of the shop groove), marimba (a rising marimba run into a chord),
chime (a shop door chime), pixel (a chiptune arpeggio for the Pixel theme).
Needs numpy. Run from the repository root.
"""
import argparse
import math
import os
import wave

import numpy as np

RATE = 44100
LENGTH = 2.5
N = int(RATE * LENGTH)

MARIMBA = [(1.0, 1.0, 1.0), (3.98, 0.35, 9.0), (9.9, 0.12, 22.0)]    # ratio, amplitude, decay per s
EPIANO = [(1.0, 1.0, 2.2), (2.0, 0.28, 4.0)]
BASS = [(1.0, 1.0, 3.5), (2.0, 0.35, 8.0), (3.0, 0.08, 14.0)]
CLICK = [(1.0, 1.0, 90.0), (1.4727, 0.5, 90.0)]
BELL = [(1.0, 1.0, 0.0), (2.0, 0.3, 4.0), (2.76, 0.25, 9.0), (3.003, 0.15, 0.0)]


def hz(midi):
    return 440.0 * 2.0 ** ((midi - 69.0) / 12.0)


class Mix:
    def __init__(self):
        self.l = np.zeros(N)
        self.r = np.zeros(N)

    def add(self, sig, start, gain=1.0, pan=0.0):
        s = int(start * RATE)
        if s >= N:
            return
        sig = sig[:N - s]
        self.l[s:s + len(sig)] += sig * gain * (1 - pan) / 2
        self.r[s:s + len(sig)] += sig * gain * (1 + pan) / 2


def note(midi, partials, seconds, decay=0.0, attack=0.004, freq=None):
    t = np.arange(int(seconds * RATE)) / RATE
    f = freq if freq else hz(midi)
    out = np.zeros_like(t)
    for ratio, amp, d in partials:
        out += amp * np.sin(2 * math.pi * f * ratio * t) * np.exp(-(d + decay) * t)
    return out * np.minimum(1.0, t / attack) if attack else out


def kick(seconds=0.22):
    t = np.arange(int(seconds * RATE)) / RATE
    phase = 2 * math.pi * (52.0 * t + 60.0 * (1 - np.exp(-28.0 * t)) / 28.0)
    return np.sin(phase) * np.exp(-16.0 * t)


def shaker(rng, seconds=0.06):
    t = np.arange(int(seconds * RATE)) / RATE
    x = rng.uniform(-1, 1, len(t))
    return np.diff(x, prepend=0.0) * np.exp(-55.0 * t)


def echo(m, delay, feedback, mixlevel):
    """A soft stereo echo, left and right a little apart."""
    for sig, d in ((m.l, delay), (m.r, delay * 1.27)):
        k = int(d * RATE)
        wet = np.zeros_like(sig)
        for rep in range(1, 5):
            at = k * rep
            if at < N:
                wet[at:] += sig[:N - at] * (feedback ** rep)
        sig += wet * mixlevel


def lowpass(x, cutoff):
    a = 1 - math.exp(-2 * math.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc += a * (v - acc)
        y[i] = acc
    return y


def bossa():
    """One bar of the shop's groove (C maj9) at 104 BPM with its swing, a short marimba tune on top."""
    m = Mix()
    rng = np.random.default_rng(7)
    beat = 60.0 / 104.0

    def at(b):
        frac = b - math.floor(b)
        if abs(frac - 0.5) < 1e-3:
            b = math.floor(b) + 0.58
        return b * beat

    for b, midi, g in ((0.0, 48, 0.9), (1.5, 55, 0.6), (2.5, 48, 0.7), (3.5, 55, 0.5)):
        m.add(note(midi, BASS, 0.55, attack=0.006), at(b), g * 0.55, -0.2)
    for b in (0.5, 1.5, 3.0):
        for i, midi in enumerate((59, 62, 64, 67, 71)):
            m.add(note(midi, EPIANO, 0.9, 0.2, 0.006), at(b) + i * 0.012, 0.13, 0.25)
    for b in (0.0, 2.0):
        m.add(kick(), at(b), 0.35)
    for b in (0.0, 1.5, 2.5, 3.0):
        m.add(note(0, CLICK, 0.05, attack=0, freq=1650.0), at(b), 0.14, 0.3)
    for k in range(8):
        m.add(shaker(rng), at(k / 2.0), 0.06 if k % 2 == 0 else 0.10, 0.4)
    # The tune: E5 G5 A5 then C6 held, with a chord under the last note.
    for b, midi, g in ((0.0, 76, 0.30), (0.5, 79, 0.28), (1.5, 81, 0.30), (2.5, 84, 0.34)):
        m.add(note(midi, MARIMBA, 1.8, 0.6, 0.002), at(b), g, 0.0)
    for i, midi in enumerate((60, 64, 67, 71, 74)):
        m.add(note(midi, EPIANO, 2.0, 0.5, 0.006), at(2.5) + i * 0.02, 0.14, -0.2 + i * 0.1)
    echo(m, 0.21, 0.45, 0.45)
    return m


def marimba():
    """A rising run of marimba over C maj9, then the chord in electric piano with a bass note."""
    m = Mix()
    run = (72, 76, 79, 83, 86, 91)
    for i, midi in enumerate(run):
        m.add(note(midi, MARIMBA, 1.8, 0.8, 0.002), i * 0.11, 0.30, -0.5 + i * 0.2)
    t0 = 0.75
    m.add(note(48, BASS, 1.5, 0.3, 0.006), t0, 0.55, -0.1)
    for i, midi in enumerate((59, 64, 67, 71, 74, 79)):
        m.add(note(midi, EPIANO, 2.0, 0.7, 0.006), t0 + i * 0.02, 0.14, -0.4 + i * 0.16)
    m.add(note(91, MARIMBA, 1.6, 0.9, 0.002), t0, 0.22, 0.3)
    echo(m, 0.19, 0.5, 0.5)
    return m


def chime():
    """A shop door chime: a high note and a lower one, as a bell on a spring, then a soft chord."""
    m = Mix()
    m.add(note(0, BELL, 2.2, 1.6, 0.002, freq=hz(88)), 0.0, 0.40, -0.25)
    m.add(note(0, BELL, 2.2, 1.6, 0.002, freq=hz(83) * 1.0), 0.28, 0.42, 0.25)
    m.add(note(0, BELL, 2.2, 1.4, 0.002, freq=hz(79)), 0.56, 0.40, 0.0)
    for i, midi in enumerate((60, 64, 67, 71)):
        m.add(note(midi, EPIANO, 1.9, 0.9, 0.006), 0.56 + i * 0.015, 0.13, -0.3 + i * 0.2)
    m.add(note(48, BASS, 1.2, 0.6, 0.006), 0.56, 0.5)
    m.add(note(0, MARIMBA, 1.4, 1.0, 0.002, freq=hz(91)), 0.56, 0.18, 0.4)
    echo(m, 0.23, 0.45, 0.4)
    return m


def pixel():
    """For the Pixel theme: a square-wave arpeggio over a triangle bass, with a short echo."""
    m = Mix()

    def square(f, seconds, duty=0.25, decay=3.0):
        t = np.arange(int(seconds * RATE)) / RATE
        wave_ = np.where((t * f) % 1.0 < duty, 1.0, -1.0)
        return lowpass(wave_, 6000.0) * np.exp(-decay * t) * np.minimum(1.0, t / 0.002)

    def tri(f, seconds, decay=2.0):
        t = np.arange(int(seconds * RATE)) / RATE
        return (2 * np.abs(2 * ((t * f) % 1.0) - 1) - 1) * np.exp(-decay * t)

    step = 0.075
    arp = (72, 76, 79, 84, 79, 84, 88, 91)
    for i, midi in enumerate(arp):
        m.add(square(hz(midi), 0.28, 0.25, 9.0), i * step, 0.22, -0.3 if i % 2 else 0.3)
    t0 = len(arp) * step
    for midi, g in ((84, 0.20), (88, 0.18), (91, 0.18), (96, 0.16)):
        m.add(square(hz(midi), 1.6, 0.5, 2.4), t0, g, 0.0)
    m.add(tri(hz(36), 1.9, 1.6), 0.0, 0.5)
    m.add(tri(hz(48), 1.5, 2.2), t0, 0.4)
    echo(m, 0.15, 0.4, 0.35)
    return m


VARIANTS = {"bossa": bossa, "marimba": marimba, "chime": chime, "pixel": pixel}


def write(m, path):
    # Soft highs off, -2 dB peak, and the last 0.3 s fade so it ends in silence.
    left = lowpass(m.l, 9000.0)
    right = lowpass(m.r, 9000.0)
    peak = max(np.abs(left).max(), np.abs(right).max())
    gain = 0.79 / peak
    fade = np.ones(N)
    k = int(0.3 * RATE)
    fade[-k:] = np.linspace(1.0, 0.0, k)
    data = np.stack([left * gain * fade, right * gain * fade], axis=1)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((data * 32767).astype("<i2").tobytes())


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--variant", default="bossa", choices=VARIANTS)
    p.add_argument("--out", default="meta/audio_3ds.wav")
    p.add_argument("--all", metavar="DIR")
    a = p.parse_args()
    if a.all:
        os.makedirs(a.all, exist_ok=True)
        for name, fn in VARIANTS.items():
            write(fn(), os.path.join(a.all, "jingle-%s.wav" % name))
    else:
        write(VARIANTS[a.variant](), a.out)


if __name__ == "__main__":
    main()
