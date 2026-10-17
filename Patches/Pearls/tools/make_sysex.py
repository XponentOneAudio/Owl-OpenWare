#!/usr/bin/env python3
"""Packs a patch binary or a resource as OpenWare SysEx (.syx).

The OWL web interface and any SysEx tool can send the file to the Lich; it
does what FirmwareSender does (OwlProgram's "make sysex", "make store").

  make_sysex.py patch.bin out.syx              upload and run from RAM
  make_sysex.py patch.bin out.syx --store 3    upload and store in slot 3
  make_sysex.py data.bin out.syx --save NAME   upload and save as resource NAME

Protocol (OpenWare Source/FirmwareLoader.hpp, MidiHandler.cpp): messages
F0 7D 52 10 <package index> ..., index 0 carrying the size, then the data
7-bit encoded, then the CRC-32; then RUN (0x12), STORE (0x11) or SAVE (0x17).
Each message fits OpenWare's 256-byte SysEx buffer.
"""

import argparse
import struct
import zlib

MANUFACTURER = 0x7d
OMNI_DEVICE = 0x52
FIRMWARE_UPLOAD = 0x10
FIRMWARE_STORE = 0x11
FIRMWARE_RUN = 0x12
FIRMWARE_SAVE = 0x17  # (0x14 is FLASH ERASE)
CHUNK = 196  # data bytes per message: 224 encoded bytes, a multiple of 8


def encode7(data):
    """OpenWare's data_to_sysex: a byte of MSBs, then 7 low-bit bytes."""
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        msbs = 0
        for j, b in enumerate(group):
            msbs |= (b >> 7) << j
        out.append(msbs)
        out.extend(b & 0x7f for b in group)
    return bytes(out)


def encode_int(value):
    return encode7(struct.pack('>I', value))


def message(command, payload):
    return bytes([0xf0, MANUFACTURER, OMNI_DEVICE, command]) + payload + b'\xf7'


def pack(data, store=None, save=None):
    msgs = [message(FIRMWARE_UPLOAD, encode_int(0) + encode_int(len(data)))]
    index = 1
    for i in range(0, len(data), CHUNK):
        msgs.append(message(FIRMWARE_UPLOAD,
                            encode_int(index) + encode7(data[i:i + CHUNK])))
        index += 1
    msgs.append(message(FIRMWARE_UPLOAD,
                        encode_int(index) + encode_int(zlib.crc32(data))))
    if save is not None:
        msgs.append(message(FIRMWARE_SAVE, save.encode('ascii') + b'\0'))
    elif store is not None:
        msgs.append(message(FIRMWARE_STORE, encode_int(store)))
    else:
        msgs.append(message(FIRMWARE_RUN, b''))
    for m in msgs:
        assert len(m) <= 256, len(m)
    return b''.join(msgs)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('input')
    parser.add_argument('output')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--store', type=int, help='patch slot to store in')
    group.add_argument('--save', help='resource name')
    parser.add_argument('--strip-resource-header', action='store_true',
                        help='input is a makeresource.py file: send its data')
    args = parser.parse_args()
    data = open(args.input, 'rb').read()
    if args.strip_resource_header:
        magic, size = struct.unpack('<II', data[:8])
        assert magic == 0xdadadeed
        data = data[32:32 + size]
    out = pack(data, args.store, args.save)
    open(args.output, 'wb').write(out)
    print('%s: %d bytes of data, %d bytes of SysEx' % (
        args.output, len(data), len(out)))


if __name__ == '__main__':
    main()
