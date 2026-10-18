#!/usr/bin/env python3
"""Sends a .syx file to the Lich over USB MIDI, and shows what the Lich says.

It sends one message at a time with a pause between them (SysEx tools that
send too fast can run out of buffers, or overrun the module), asks the Lich
for its status every few hundred messages, and stops at the first error,
printing the Lich's own error text. --info asks the Lich for its firmware
version, error status, CPU and memory use, stored patches and resources.

On Windows it needs nothing but Python: it uses the Windows MIDI API
(winmm) directly. On macOS and Linux it uses mido:
pip install mido python-rtmidi. Close MIDI-OX and the OWL web page first:
they can hold the ports.

  python send_sysex.py --list                     the MIDI ports
  python send_sysex.py --info                     what the Lich reports
  python send_sysex.py --erase 1                  deletes the patch in slot 1
  python send_sysex.py pearls-lich.syx            to the first "OWL" port
  python send_sysex.py pearls-waves.syx --port OWL --delay 20
"""

import argparse
import collections
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_sysex  # noqa: E402

MANUFACTURER = 0x7d
REQUEST_SETTINGS = 67  # CC: the Lich answers with SysEx
FIRMWARE_UPLOAD = 0x10
FIRMWARE_STORE = 0x11
FIRMWARE_RUN = 0x12
FIRMWARE_SAVE = 0x17
FLASH_ERASE = 0x14
PRESET_NAME = 0x01
RESOURCE_NAME = 0x04
FIRMWARE_VERSION = 0x20
PROGRAM_MESSAGE = 0x22
DEVICE_STATS = 0x23
PROGRAM_STATS = 0x24
BOOTLOADER_VERSION = 0x25
PROGRAM_ERROR = 0x30


def split_messages(data):
    """The F0 ... F7 messages of a .syx file, with F0 and F7."""
    messages = []
    start = None
    for i, b in enumerate(data):
        if b == 0xf0:
            start = i
        elif b == 0xf7 and start is not None:
            messages.append(bytes(data[start:i + 1]))
            start = None
    return messages


def decode7(data):
    """OpenWare's sysex_to_data."""
    out = bytearray()
    for i in range(0, len(data), 8):
        msbs = data[i]
        for j, b in enumerate(data[i + 1:i + 8]):
            out.append(b | (((msbs >> j) & 1) << 7))
    return bytes(out)


def parse_name(message):
    """A patch or resource name reply as (kind, index, name, size), or
    None. kind is 'old' for firmware before v22, which sends no size."""
    if len(message) < 7 or message[1] != MANUFACTURER:
        return None
    cmd = message[3]
    if cmd not in (PRESET_NAME, RESOURCE_NAME):
        return None
    body = bytes(message[4:-1])
    index = body[0]
    name, nul, rest = body[1:].partition(b'\0')
    name = name.decode('ascii', 'replace')
    if not nul:
        return 'old', index, name, 0
    size = struct.unpack('>I', decode7(rest[:5]))[0] if len(rest) >= 5 else 0
    return 'patch' if cmd == PRESET_NAME else 'resource', index, name, size


def describe(message):
    """A Lich reply as (kind, text), or None for anything else."""
    if len(message) < 5 or message[1] != MANUFACTURER:
        return None
    cmd = message[3]
    body = bytes(message[4:-1])
    named = parse_name(message)
    if named:
        kind, index, name, size = named
        if kind == 'old':
            return kind, name
        return kind, '%2d  %-24s %8d bytes' % (index, name, size)
    text = body.decode('ascii', 'replace')
    kinds = {FIRMWARE_VERSION: 'firmware', BOOTLOADER_VERSION: 'bootloader',
             PROGRAM_MESSAGE: 'message', DEVICE_STATS: 'device',
             PROGRAM_STATS: 'program', PROGRAM_ERROR: 'ERROR'}
    if cmd in kinds:
        return kinds[cmd], text
    return None


class WinMidi(object):
    """MIDI in and out through winmm (Windows), without any package."""

    MHDR_DONE = 1
    BUFFERS = 32
    BUFFER_SIZE = 512

    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.winmm = ctypes.WinDLL('winmm')
        self.out_handle = None
        self.in_handle = None
        self.in_headers = collections.deque()
        self.in_buffers = []
        self.pending = bytearray()

        class MIDIOUTCAPSW(ctypes.Structure):
            _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD),
                        ('vDriverVersion', wintypes.UINT),
                        ('szPname', wintypes.WCHAR * 32),
                        ('wTechnology', wintypes.WORD),
                        ('wVoices', wintypes.WORD), ('wNotes', wintypes.WORD),
                        ('wChannelMask', wintypes.WORD),
                        ('dwSupport', wintypes.DWORD)]

        class MIDIINCAPSW(ctypes.Structure):
            _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD),
                        ('vDriverVersion', wintypes.UINT),
                        ('szPname', wintypes.WCHAR * 32),
                        ('dwSupport', wintypes.DWORD)]

        class MIDIHDR(ctypes.Structure):
            pass
        MIDIHDR._fields_ = [('lpData', ctypes.c_void_p),
                            ('dwBufferLength', wintypes.DWORD),
                            ('dwBytesRecorded', wintypes.DWORD),
                            ('dwUser', ctypes.c_size_t),
                            ('dwFlags', wintypes.DWORD),
                            ('lpNext', ctypes.POINTER(MIDIHDR)),
                            ('reserved', ctypes.c_size_t),
                            ('dwOffset', wintypes.DWORD),
                            ('dwReserved', ctypes.c_size_t * 8)]
        self.MIDIOUTCAPSW = MIDIOUTCAPSW
        self.MIDIINCAPSW = MIDIINCAPSW
        self.MIDIHDR = MIDIHDR
        w = self.winmm
        for f in (w.midiOutOpen, w.midiOutClose, w.midiOutShortMsg,
                  w.midiOutLongMsg, w.midiOutPrepareHeader,
                  w.midiOutUnprepareHeader, w.midiOutGetDevCapsW,
                  w.midiInOpen, w.midiInClose, w.midiInStart, w.midiInStop,
                  w.midiInReset, w.midiInAddBuffer, w.midiInPrepareHeader,
                  w.midiInUnprepareHeader, w.midiInGetDevCapsW):
            f.restype = wintypes.UINT
        w.midiOutShortMsg.argtypes = [ctypes.c_void_p, wintypes.DWORD]

    def check(self, result, what, error_text=None):
        if result != 0:
            text = self.ctypes.create_unicode_buffer(256)
            (error_text or self.winmm.midiOutGetErrorTextW)(result, text, 256)
            raise RuntimeError('%s: %s (error %d)' % (what, text.value, result))

    def _names(self, count, caps_type, get_caps):
        out = []
        for i in range(count):
            caps = caps_type()
            get_caps(i, self.ctypes.byref(caps), self.ctypes.sizeof(caps))
            out.append(caps.szPname)
        return out

    def output_names(self):
        return self._names(self.winmm.midiOutGetNumDevs(), self.MIDIOUTCAPSW,
                           self.winmm.midiOutGetDevCapsW)

    def input_names(self):
        return self._names(self.winmm.midiInGetNumDevs(), self.MIDIINCAPSW,
                           self.winmm.midiInGetDevCapsW)

    def open_output(self, index):
        self.out_handle = self.ctypes.c_void_p()
        self.check(self.winmm.midiOutOpen(self.ctypes.byref(self.out_handle),
                                          index, None, None, 0),
                   'opening the output port')

    def open_input(self, index):
        ctypes = self.ctypes
        self.in_handle = ctypes.c_void_p()
        # No callback: filled buffers are found by their MHDR_DONE flag.
        self.check(self.winmm.midiInOpen(ctypes.byref(self.in_handle),
                                         index, None, None, 0),
                   'opening the input port', self.winmm.midiInGetErrorTextW)
        for _ in range(self.BUFFERS):
            buf = ctypes.create_string_buffer(self.BUFFER_SIZE)
            hdr = self.MIDIHDR()
            hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
            hdr.dwBufferLength = self.BUFFER_SIZE
            self.in_buffers.append((buf, hdr))
            self._add_buffer(buf, hdr)
        self.check(self.winmm.midiInStart(self.in_handle), 'starting input',
                   self.winmm.midiInGetErrorTextW)

    def _add_buffer(self, buf, hdr):
        size = self.ctypes.sizeof(hdr)
        hdr.dwFlags = 0
        hdr.dwBytesRecorded = 0
        self.winmm.midiInPrepareHeader(self.in_handle, self.ctypes.byref(hdr),
                                       size)
        self.winmm.midiInAddBuffer(self.in_handle, self.ctypes.byref(hdr), size)
        self.in_headers.append((buf, hdr))

    def receive(self):
        """The complete SysEx messages received since the last call."""
        messages = []
        if not self.in_handle:
            return messages
        while self.in_headers and self.in_headers[0][1].dwFlags & self.MHDR_DONE:
            buf, hdr = self.in_headers.popleft()
            self.pending += buf.raw[:hdr.dwBytesRecorded]
            self.winmm.midiInUnprepareHeader(self.in_handle,
                                             self.ctypes.byref(hdr),
                                             self.ctypes.sizeof(hdr))
            self._add_buffer(buf, hdr)
        while True:
            end = self.pending.find(b'\xf7')
            if end < 0:
                break
            start = self.pending.rfind(b'\xf0', 0, end)
            if start >= 0:
                messages.append(bytes(self.pending[start:end + 1]))
            del self.pending[:end + 1]
        return messages

    def send_cc(self, cc, value):
        self.check(self.winmm.midiOutShortMsg(self.out_handle,
                                              0xb0 | (cc << 8) | (value << 16)),
                   'sending a control change')

    def send(self, message):
        ctypes = self.ctypes
        buf = ctypes.create_string_buffer(bytes(message), len(message))
        hdr = self.MIDIHDR()
        hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
        hdr.dwBufferLength = len(message)
        hdr.dwBytesRecorded = len(message)
        size = ctypes.sizeof(hdr)
        self.check(self.winmm.midiOutPrepareHeader(self.out_handle,
                                                   ctypes.byref(hdr), size),
                   'preparing a message')
        try:
            self.check(self.winmm.midiOutLongMsg(self.out_handle,
                                                 ctypes.byref(hdr), size),
                       'sending a message')
            # Wait until the driver has sent it (MHDR_DONE).
            deadline = time.time() + 5.0
            while not hdr.dwFlags & self.MHDR_DONE:
                if time.time() > deadline:
                    raise RuntimeError('the driver did not send a message')
                time.sleep(0.001)
        finally:
            self.winmm.midiOutUnprepareHeader(self.out_handle,
                                              ctypes.byref(hdr), size)

    def close(self):
        if self.in_handle:
            self.winmm.midiInStop(self.in_handle)
            self.winmm.midiInReset(self.in_handle)  # returns the buffers
            for buf, hdr in self.in_buffers:
                self.winmm.midiInUnprepareHeader(self.in_handle,
                                                 self.ctypes.byref(hdr),
                                                 self.ctypes.sizeof(hdr))
            self.winmm.midiInClose(self.in_handle)
            self.in_handle = None
        if self.out_handle:
            self.winmm.midiOutClose(self.out_handle)
            self.out_handle = None


class MidoMidi(object):
    """MIDI in and out through mido (macOS, Linux)."""

    def __init__(self):
        try:
            import mido
        except ImportError:
            sys.exit('needs mido: pip install mido python-rtmidi')
        self.mido = mido
        self.out_port = None
        self.in_port = None

    def output_names(self):
        return self.mido.get_output_names()

    def input_names(self):
        return self.mido.get_input_names()

    def open_output(self, index):
        self.out_port = self.mido.open_output(self.output_names()[index])

    def open_input(self, index):
        self.in_port = self.mido.open_input(self.input_names()[index])

    def receive(self):
        if not self.in_port:
            return []
        return [bytes(m.bytes()) for m in self.in_port.iter_pending()
                if m.type == 'sysex']

    def send_cc(self, cc, value):
        self.out_port.send(self.mido.Message('control_change', control=cc,
                                             value=value))

    def send(self, message):
        self.out_port.send(self.mido.Message('sysex', data=message[1:-1]))

    def close(self):
        for port in (self.in_port, self.out_port):
            if port:
                port.close()


def find_port(names, wanted, what):
    matches = [i for i, n in enumerate(names) if wanted.lower() in n.lower()]
    if not matches:
        sys.exit('no MIDI %s port matching "%s"; ports: %s'
                 % (what, wanted, ', '.join(names) or 'none'))
    return matches[0]


def collect(midi, seconds):
    """The Lich's replies over the next few seconds, as (kind, text)."""
    replies = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        for m in midi.receive():
            d = describe(m)
            if d:
                replies.append(d)
        time.sleep(0.01)
    return replies


def print_replies(replies):
    if any(kind == 'old' for kind, _ in replies):
        print('  This Lich runs firmware older than v22: Pearls needs v22 or '
              'later (resources, SAVE). Update the firmware first.')
        replies = [r for r in replies if r[0] != 'old']
    for kind, text in replies:
        if kind in ('patch', 'resource'):
            continue
        print('  %-10s %s' % (kind, text))
    for kind, title in (('patch', 'stored patches'),
                        ('resource', 'stored resources')):
        names = [text for k, text in replies if k == kind]
        if names:
            print('  %s:' % title)
            for text in names:
                print('    ' + text)


def info(midi):
    midi.send_cc(REQUEST_SETTINGS, 127)  # versions, message, stats, status
    replies = collect(midi, 1.0)
    midi.send_cc(REQUEST_SETTINGS, PRESET_NAME)
    replies += collect(midi, 1.5)
    midi.send_cc(REQUEST_SETTINGS, RESOURCE_NAME)
    replies += collect(midi, 1.5)
    if not replies:
        print('  no reply: is the input port the Lich, and is MIDI-OX or '
              'the OWL web page closed?')
    print_replies(replies)


def status_error(midi, wait):
    """The Lich's error text, if it reports one within 'wait' seconds."""
    midi.send_cc(REQUEST_SETTINGS, PROGRAM_STATS)
    deadline = time.time() + wait
    while time.time() < deadline:
        for m in midi.receive():
            d = describe(m)
            if d and d[0] == 'ERROR':
                return d[1]
            if d and d[0] == 'program':
                return None
        time.sleep(0.005)
    return None


def stored(midi, kind):
    """The Lich's stored patches or resources, as (index, name, size)."""
    midi.receive()
    midi.send_cc(REQUEST_SETTINGS,
                 PRESET_NAME if kind == 'patch' else RESOURCE_NAME)
    found = []
    deadline = time.time() + 2.0
    while time.time() < deadline:
        for m in midi.receive():
            named = parse_name(m)
            if named and named[0] == kind:
                found.append(named[1:])
        time.sleep(0.01)
    return found


def upload_target(messages):
    """What the upload stores: ('patch', slot, size), ('resource', name,
    size), or None."""
    first, last = messages[0], messages[-1]
    if len(first) < 15 or first[3] != FIRMWARE_UPLOAD:
        return None
    size = struct.unpack('>I', decode7(first[9:14]))[0]
    if last[3] == FIRMWARE_STORE:
        return 'patch', struct.unpack('>I', decode7(last[4:9]))[0], size
    if last[3] == FIRMWARE_SAVE:
        return 'resource', bytes(last[4:-1]).split(b'\0')[0].decode('ascii'), size
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('file', nargs='?')
    parser.add_argument('--port', default='OWL',
                        help='part of the MIDI port names (default OWL)')
    parser.add_argument('--delay', type=float, default=10.0,
                        help='milliseconds between messages (default 10)')
    parser.add_argument('--list', action='store_true',
                        help='list the MIDI ports')
    parser.add_argument('--info', action='store_true',
                        help='ask the Lich for its status')
    parser.add_argument('--erase', type=int, metavar='SLOT',
                        help='delete the patch stored in a slot')
    args = parser.parse_args()

    midi = WinMidi() if sys.platform == 'win32' else MidoMidi()
    outputs = midi.output_names()
    inputs = midi.input_names()
    if args.list or not (args.file or args.info or args.erase):
        print('MIDI output ports:')
        for n in outputs:
            print('  ' + n)
        print('MIDI input ports:')
        for n in inputs:
            print('  ' + n)
        return
    out_index = find_port(outputs, args.port, 'output')
    in_index = find_port(inputs, args.port, 'input')

    messages = []
    if args.erase:
        if args.file or args.info:
            sys.exit('--erase goes alone')
        messages = [make_sysex.message(FLASH_ERASE,
                                       make_sysex.encode_int(args.erase))]
    elif args.file:
        messages = split_messages(open(args.file, 'rb').read())
        if not messages:
            sys.exit('%s holds no SysEx messages' % args.file)

    midi.open_output(out_index)
    try:
        try:
            midi.open_input(in_index)
        except Exception as e:
            if args.info:
                raise
            print('cannot listen to the Lich (%s): sending without checking'
                  % e)
        if args.info:
            print('%s says:' % outputs[out_index])
            info(midi)
            return
        print('sending %d messages to %s' % (len(messages), outputs[out_index]))
        last = messages[-1][3] if len(messages[-1]) > 3 else None
        if last == FLASH_ERASE:
            print('deleting the patch in slot %d' % args.erase)
        for n, m in enumerate(messages):
            midi.send(m)
            time.sleep(args.delay / 1000.0)
            if n % 50 == 0 or n == len(messages) - 1:
                print('\r  %d / %d' % (n + 1, len(messages)), end='',
                      flush=True)
            # Between messages, ask whether the upload is still going well:
            # a lost or damaged message stops it, and the Lich keeps
            # showing L.
            if n % 250 == 249 or n == len(messages) - 2:
                err = status_error(midi, 0.5)
                if err:
                    print('\nthe Lich reports an error after message %d of '
                          '%d: %s' % (n + 1, len(messages), err))
                    print('try again with a longer --delay, e.g. --delay 30')
                    sys.exit(1)
        print()
        # STORE and SAVE write the flash, then restart the patch.
        if last in (FIRMWARE_STORE, FIRMWARE_SAVE, FLASH_ERASE):
            print('writing the flash...')
            time.sleep(3.0)
        else:
            time.sleep(1.0)
        try:
            midi.receive()
            err = status_error(midi, 2.0)
        except RuntimeError as e:
            print('the Lich no longer answers (%s): it reset, probably '
                  'because the patch crashed as it started' % e)
            sys.exit(1)
        target = upload_target(messages)
        if err and 'Size mismatch' in err and target:
            # OpenWare v22.5 on the Lich reports this after every write to
            # its SPI flash (it checks a stale copy of the header), though
            # the write succeeded. Check what was stored instead.
            kind, key, size = target
            entries = stored(midi, kind)
            ok = any((index == key if kind == 'patch' else name == key)
                     and stored_size == size
                     for index, name, stored_size in entries)
            if ok:
                print('stored: %s %s, %d bytes. (The Lich also reports '
                      '"%s": a false alarm of firmware v22.5, which shows '
                      'E; press the encoder to clear it.)'
                      % (kind, key, size, err))
                return
            print('the Lich reports an error: %s, and the %s is not stored'
                  % (err, kind))
            sys.exit(1)
        if err:
            print('the Lich reports an error: %s' % err)
            sys.exit(1)
        print('done, no error reported')
    finally:
        midi.close()


if __name__ == '__main__':
    main()
