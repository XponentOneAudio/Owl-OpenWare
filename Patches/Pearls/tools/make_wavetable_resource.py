#!/usr/bin/env python3
"""Makes the OpenWare resource holding the Beads wavetables, for the Lich.

The wavetables (100 kB) don't fit in a Lich patch, so the patch loads them
from the module's flash, where this resource must be stored once.

Usage: make_wavetable_resource.py [output.res]
Writes the resource (default build/pearls-waves.res) in the format of
OwlProgram's Tools/makeresource.py and prints the checksum the patch checks.
"""

import os
import re
import struct
import sys

NAME = 'pearls-waves'
HERE = os.path.dirname(os.path.abspath(__file__))
RESOURCES = os.path.join(HERE, '..', 'third_party', 'eurorack', 'beads',
                         'resources.cc')
COUNT = 49920  # WAV_INTEGRATED_WAVES_SIZE


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        HERE, '..', 'build', 'pearls-waves.res')
    src = open(RESOURCES).read()
    m = re.search(r'const int16_t wav_integrated_waves\[\] = \{(.*?)\};', src,
                  re.S)
    values = [int(v) for v in m.group(1).replace('\n', ' ').split(',')
              if v.strip()]
    assert len(values) == COUNT, len(values)
    data = struct.pack('<%dh' % COUNT, *values)
    header = struct.pack('<II', 0xdadadeed, len(data))
    name = NAME.encode('ascii').ljust(24, b'\0')[:23] + b'\0'
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(header + name + data)
    print('%s: resource "%s", %d bytes of data, checksum 0x%08x'
          % (out, NAME, len(data), fnv1a(data)))


if __name__ == '__main__':
    main()
