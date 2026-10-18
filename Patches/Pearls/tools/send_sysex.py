#!/usr/bin/env python3
"""Sends a .syx file to the Lich over USB MIDI, one message at a time with
a pause between them (SysEx tools that send too fast can run out of
buffers, or overrun the module).

On Windows it needs nothing but Python: it uses the Windows MIDI API
(winmm) directly. On macOS and Linux it uses mido:
pip install mido python-rtmidi.

  python send_sysex.py --list                     the MIDI output ports
  python send_sysex.py pearls-lich.syx            to the first "OWL" port
  python send_sysex.py pearls-waves.syx --port OWL --delay 20
"""

import argparse
import sys
import time


def split_messages(data):
    """The F0 ... F7 messages of a .syx file, with F0 and F7."""
    messages = []
    start = None
    for i, b in enumerate(data):
        if b == 0xf0:
            start = i
        elif b == 0xf7 and start is not None:
            messages.append(data[start:i + 1])
            start = None
    return messages


class WinMidiOut(object):
    """MIDI output through winmm (Windows), without any package."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.winmm = ctypes.WinDLL('winmm')
        self.handle = None

        class MIDIOUTCAPSW(ctypes.Structure):
            _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD),
                        ('vDriverVersion', wintypes.UINT),
                        ('szPname', wintypes.WCHAR * 32),
                        ('wTechnology', wintypes.WORD),
                        ('wVoices', wintypes.WORD), ('wNotes', wintypes.WORD),
                        ('wChannelMask', wintypes.WORD),
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
        self.MIDIHDR = MIDIHDR

    def check(self, result, what):
        if result != 0:
            text = self.ctypes.create_unicode_buffer(256)
            self.winmm.midiOutGetErrorTextW(result, text, 256)
            raise RuntimeError('%s: %s (error %d)' % (what, text.value, result))

    def names(self):
        out = []
        for i in range(self.winmm.midiOutGetNumDevs()):
            caps = self.MIDIOUTCAPSW()
            self.winmm.midiOutGetDevCapsW(i, self.ctypes.byref(caps),
                                          self.ctypes.sizeof(caps))
            out.append(caps.szPname)
        return out

    def open(self, index):
        self.handle = self.ctypes.c_void_p()
        self.check(self.winmm.midiOutOpen(self.ctypes.byref(self.handle),
                                          index, None, None, 0),
                   'opening the port')

    def send(self, message):
        ctypes = self.ctypes
        buf = ctypes.create_string_buffer(bytes(message), len(message))
        hdr = self.MIDIHDR()
        hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
        hdr.dwBufferLength = len(message)
        hdr.dwBytesRecorded = len(message)
        size = ctypes.sizeof(hdr)
        self.check(self.winmm.midiOutPrepareHeader(self.handle,
                                                   ctypes.byref(hdr), size),
                   'preparing a message')
        try:
            self.check(self.winmm.midiOutLongMsg(self.handle,
                                                 ctypes.byref(hdr), size),
                       'sending a message')
            # Wait until the driver has sent it (MHDR_DONE).
            deadline = time.time() + 5.0
            while not hdr.dwFlags & 1:
                if time.time() > deadline:
                    raise RuntimeError('the driver did not send a message')
                time.sleep(0.001)
        finally:
            self.winmm.midiOutUnprepareHeader(self.handle, ctypes.byref(hdr),
                                              size)

    def close(self):
        if self.handle:
            self.winmm.midiOutClose(self.handle)
            self.handle = None


class MidoOut(object):
    """MIDI output through mido (macOS, Linux)."""

    def __init__(self):
        try:
            import mido
        except ImportError:
            sys.exit('needs mido: pip install mido python-rtmidi')
        self.mido = mido
        self.port = None
        self.ports = mido.get_output_names()

    def names(self):
        return self.ports

    def open(self, index):
        self.port = self.mido.open_output(self.ports[index])

    def send(self, message):
        self.port.send(self.mido.Message('sysex', data=message[1:-1]))

    def close(self):
        if self.port:
            self.port.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('file', nargs='?')
    parser.add_argument('--port', default='OWL',
                        help='part of the MIDI output port name (default OWL)')
    parser.add_argument('--delay', type=float, default=10.0,
                        help='milliseconds between messages (default 10)')
    parser.add_argument('--list', action='store_true',
                        help='list the MIDI output ports')
    args = parser.parse_args()

    out = WinMidiOut() if sys.platform == 'win32' else MidoOut()
    names = out.names()
    if args.list or not args.file:
        print('MIDI output ports:')
        for n in names:
            print('  ' + n)
        return
    matches = [i for i, n in enumerate(names)
               if args.port.lower() in n.lower()]
    if not matches:
        sys.exit('no MIDI output port matching "%s"; ports: %s'
                 % (args.port, ', '.join(names) or 'none'))

    messages = split_messages(open(args.file, 'rb').read())
    if not messages:
        sys.exit('%s holds no SysEx messages' % args.file)

    out.open(matches[0])
    try:
        print('sending %d messages to %s' % (len(messages), names[matches[0]]))
        for n, m in enumerate(messages):
            out.send(m)
            time.sleep(args.delay / 1000.0)
            if n % 50 == 0 or n == len(messages) - 1:
                print('\r  %d / %d' % (n + 1, len(messages)), end='',
                      flush=True)
        # The last message (store or save) writes the flash: give it time
        # before the port closes.
        time.sleep(1.0)
    finally:
        out.close()
    print('\ndone')


if __name__ == '__main__':
    main()
