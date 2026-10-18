#!/usr/bin/env python3
"""Turns a WAV file into SysEx that stores it as a Pearls sample slot.

  make_sample_syx.py in.wav SLOT out.syx [--mono] [--rate 24000]

SLOT is 1 to 8: the patch loads resource "pearls-SLOT.wav" when that slot
is chosen on the extras page. OpenWare resources are at most 1 MB, so the
sample is converted to 16 bits and cut to fit: about 5 s of 48 kHz stereo.
--mono and --rate 24000 fit four times as much (the sunny tape and
scorched cassette qualities record at 24 kHz anyway).
"""

import argparse
import io
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_sysex  # noqa: E402

MAX_RESOURCE = 1024 * 1024 - 256  # OpenWare: MAX_SYSEX_PAYLOAD_SIZE, with a margin


def read_wav(path):
    """Frames as lists of floats per channel, and the sample rate. Reads PCM
    (8, 16, 24, 32 bits) and floating point (32, 64 bits) WAV files,
    including WAVE_FORMAT_EXTENSIBLE ones."""
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('%s is not a WAV file' % path)
    pos = 12
    fmt = None
    samples = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            tag, channels, rate = struct.unpack('<HHI', body[:8])
            bits = struct.unpack('<H', body[14:16])[0]
            if tag == 0xfffe and len(body) >= 26:
                tag = struct.unpack('<H', body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b'data':
            samples = body
            break
        pos += 8 + size + (size & 1)
    if not fmt or samples is None:
        raise ValueError('%s: no format or data chunk' % path)
    tag, channels, rate, bits = fmt
    width = bits // 8
    count = len(samples) // width
    samples = samples[:count * width]
    if tag == 3 and bits == 32:
        values = list(struct.unpack('<%df' % count, samples))
    elif tag == 3 and bits == 64:
        values = list(struct.unpack('<%dd' % count, samples))
    elif tag == 1 and bits == 8:
        values = [(b - 128) / 128.0 for b in samples]
    elif tag == 1 and bits == 16:
        values = [v / 32768.0 for v in struct.unpack('<%dh' % count, samples)]
    elif tag == 1 and bits == 24:
        values = [int.from_bytes(samples[i:i + 3], 'little', signed=True)
                  / 8388608.0 for i in range(0, len(samples), 3)]
    elif tag == 1 and bits == 32:
        values = [v / 2147483648.0
                  for v in struct.unpack('<%di' % count, samples)]
    else:
        raise ValueError('%s: unsupported WAV encoding (format %d, %d bits)'
                         % (path, tag, bits))
    frames = len(values) // channels
    return [values[c:frames * channels:channels] for c in range(channels)], rate


def resample(x, src, dst):
    if src == dst:
        return x
    step = src / dst
    n = int((len(x) - 1) / step) + 1
    out = []
    for i in range(n):
        p = i * step
        j = int(p)
        t = p - j
        b = x[min(j + 1, len(x) - 1)]
        out.append(x[j] + (b - x[j]) * t)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('input')
    parser.add_argument('slot', type=int, choices=range(1, 9))
    parser.add_argument('output')
    parser.add_argument('--mono', action='store_true')
    parser.add_argument('--rate', type=int)
    args = parser.parse_args()

    channels, rate = read_wav(args.input)
    channels = channels[:2]
    if args.mono and len(channels) == 2:
        channels = [[(a + b) * 0.5 for a, b in zip(*channels)]]
    if args.rate:
        channels = [resample(c, rate, args.rate) for c in channels]
        rate = args.rate
    header_size = 44
    frame_bytes = 2 * len(channels)
    max_frames = (MAX_RESOURCE - header_size) // frame_bytes
    frames = min(len(channels[0]), max_frames)
    if frames < len(channels[0]):
        print('cut to %.1f s (resources are at most 1 MB)' % (frames / rate))

    buf = io.BytesIO()
    w = wave.open(buf, 'wb')
    w.setnchannels(len(channels))
    w.setsampwidth(2)
    w.setframerate(rate)
    samples = []
    for f in range(frames):
        for c in channels:
            v = max(-1.0, min(1.0, c[f]))
            samples.append(int(round(v * 32767)))
    w.writeframes(struct.pack('<%dh' % len(samples), *samples))
    w.close()
    data = buf.getvalue()

    name = 'pearls-%d.wav' % args.slot
    out = make_sysex.pack(data, save=name)
    open(args.output, 'wb').write(out)
    print('%s: "%s", %d Hz %s, %.1f s, %d bytes' % (
        args.output, name, rate, 'mono' if len(channels) == 1 else 'stereo',
        frames / rate, len(data)))


if __name__ == '__main__':
    main()
