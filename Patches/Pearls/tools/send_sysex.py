#!/usr/bin/env python3
"""Sends a .syx file to the Lich over USB MIDI, one message at a time with
a pause between them (SysEx tools that send too fast can run out of
buffers, or overrun the module).

  pip install mido python-rtmidi
  python3 send_sysex.py pearls-lich.syx            # to the first "OWL" port
  python3 send_sysex.py pearls-waves.syx --port Lich --delay 20
  python3 send_sysex.py --list                     # the MIDI output ports
"""

import argparse
import sys
import time


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

    try:
        import mido
    except ImportError:
        sys.exit('needs mido: pip install mido python-rtmidi')

    ports = mido.get_output_names()
    if args.list or not args.file:
        print('MIDI output ports:')
        for p in ports:
            print('  ' + p)
        return
    matches = [p for p in ports if args.port.lower() in p.lower()]
    if not matches:
        sys.exit('no MIDI output port matching "%s"; ports: %s'
                 % (args.port, ', '.join(ports) or 'none'))

    data = open(args.file, 'rb').read()
    messages = []
    start = None
    for i, b in enumerate(data):
        if b == 0xf0:
            start = i
        elif b == 0xf7 and start is not None:
            messages.append(data[start + 1:i])  # mido adds F0 and F7
            start = None
    if not messages:
        sys.exit('%s holds no SysEx messages' % args.file)

    with mido.open_output(matches[0]) as out:
        print('sending %d messages to %s' % (len(messages), matches[0]))
        for n, m in enumerate(messages):
            out.send(mido.Message('sysex', data=m))
            time.sleep(args.delay / 1000.0)
            if n % 50 == 0 or n == len(messages) - 1:
                print('\r  %d / %d' % (n + 1, len(messages)), end='',
                      flush=True)
        # The last message (store or save) writes the flash: give it time
        # before the port closes.
        time.sleep(1.0)
    print('\ndone')


if __name__ == '__main__':
    main()
