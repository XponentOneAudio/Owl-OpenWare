#!/usr/bin/env python3
"""reverb_compare.py a.raw b.raw: decay and spectrum of two reverb
impulse responses from reverb_compare.cc (mono float32, 48 kHz)."""
import cmath
import math
import struct
import sys

RATE = 48000


def load(path):
    d = open(path, 'rb').read()
    return struct.unpack('%df' % (len(d) // 4), d)


def analyse(x):
    start = RATE + 600  # after the burst
    tail = x[start:]
    # Energy in 50 ms windows, in dB.
    w = RATE // 20
    env = []
    for i in range(0, len(tail) - w, w):
        e = sum(v * v for v in tail[i:i + w]) / w
        env.append(10 * math.log10(e + 1e-20))
    # RT60 from the -5 to -25 dB slope.
    top = env[0]
    t5 = next(i for i, e in enumerate(env) if e < top - 5)
    t25 = next((i for i, e in enumerate(env) if e < top - 25), len(env) - 1)
    rt60 = (t25 - t5) * 0.05 * 60.0 / 20.0
    energy = sum(v * v for v in tail)
    # Spectrum of the first 0.5 s of tail in octave bands (DFT bins).
    n = 4096
    seg = tail[:n]
    bands = [(125, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000),
             (4000, 8000), (8000, 12000), (12000, 20000)]
    spectrum = []
    for lo, hi in bands:
        p = 0.0
        for k in range(int(lo * n / RATE), int(hi * n / RATE) + 1, 2):
            s = sum(seg[t] * cmath.exp(-2j * math.pi * k * t / n)
                    for t in range(0, n, 2))
            p += abs(s) ** 2
        spectrum.append(10 * math.log10(p + 1e-20))
    return rt60, 10 * math.log10(energy + 1e-20), spectrum, bands


def main():
    a = analyse(load(sys.argv[1]))
    b = analyse(load(sys.argv[2]))
    print('RT60 %.2f s vs %.2f s; tail energy %.1f dB vs %.1f dB'
          % (a[0], b[0], a[1], b[1]))
    print('octave band level difference (b - a), relative to the overall:')
    da = [x - a[1] for x in a[2]]
    db = [x - b[1] for x in b[2]]
    for (lo, hi), x, y in zip(a[3], da, db):
        print('  %5d-%5d Hz: %+5.1f dB' % (lo, hi, y - x))


if __name__ == '__main__':
    main()
