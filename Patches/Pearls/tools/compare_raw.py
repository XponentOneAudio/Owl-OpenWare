#!/usr/bin/env python3
"""compare_raw.py a.raw b.raw: compares two scenario_render outputs
(interleaved float32 L, R, gate per frame, 6 s per scenario)."""
import struct
import sys

FRAMES = 48000 * 6


def main():
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    if len(a) != len(b):
        print('length differs: %d vs %d' % (len(a), len(b)))
        return 1
    n = len(a) // 4
    fa = struct.unpack('%df' % n, a)
    fb = struct.unpack('%df' % n, b)
    worst = 0.0
    for s in range(n // (3 * FRAMES)):
        base = s * 3 * FRAMES
        max_diff = 0.0
        peak = 0.0
        gate_diff = 0
        first = None
        for i in range(FRAMES):
            for ch in range(2):
                k = base + 3 * i + ch
                d = abs(fa[k] - fb[k])
                peak = max(peak, abs(fa[k]))
                if d > max_diff:
                    max_diff = d
                if d > 1e-6 and first is None:
                    first = i
            if fa[base + 3 * i + 2] != fb[base + 3 * i + 2]:
                gate_diff += 1
        worst = max(worst, max_diff)
        print('scenario %d: peak %.3f  max diff %.3g  first diff at %s  gate diffs %d'
              % (s, peak, max_diff, first, gate_diff))
    print('worst difference: %.3g' % worst)
    return 0


if __name__ == '__main__':
    sys.exit(main())
