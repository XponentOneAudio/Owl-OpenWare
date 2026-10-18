#!/usr/bin/env python3
"""fast_sections.py: the functions to place in the Lich's fast code region.

The Lich runs the patch from SRAM, where the CPU fetches instructions over
the System bus, with the data; through SRAM1's alias at 0x00000000 (SYSCFG
remap) it fetches them over the I-Code bus instead, much faster. Only the
patch's first 64 kB are in SRAM1 (0x2000c000 to 0x2001c000), so only the hot
code goes there: owl/owl2-pearls.ld links it at its alias (.fast), and the
linker adds veneers for the calls between it and the rest.

  fast_sections.py ld PATCH.elf ENTRY... > fast.ld
      the linker script lines that place every function the entries reach
      (by direct calls and branches), except those matching --exclude, in
      .fast
  fast_sections.py check PATCH.elf ENTRY...
      fails if one of these functions is not in .fast

--exclude NAME: a regular expression on the demangled names (without
parameters) of functions that the entries reach but that are not hot (once
per grain or less), to leave room, or that are not in a section of their own
(link time optimised).
"""

import argparse
import re
import subprocess
import sys

FAST_BEGIN = 0x0000c000  # the alias of 0x2000c000
FAST_END = 0x0001c000


def tool(name):
    import os
    return os.environ.get('TOOLROOT', '') + 'arm-none-eabi-' + name


def functions(elf):
    """{address: (mangled, demangled, size)} of the code symbols."""
    out = subprocess.run([tool('nm'), '--print-size', elf], capture_output=True,
                         text=True, check=True).stdout
    dem = subprocess.run([tool('nm'), '--print-size', '-C', elf],
                         capture_output=True, text=True, check=True).stdout
    result = {}
    for raw, pretty in zip(out.splitlines(), dem.splitlines()):
        a = raw.split()
        b = pretty.split(None, 3)
        if len(a) == 4 and a[2] in 'tTwW':
            result[int(a[0], 16) & ~1] = (a[3], b[3], int(a[1], 16))
    return result


def calls(elf, funcs):
    """{function address: set of called function addresses}."""
    out = subprocess.run([tool('objdump'), '-d', elf], capture_output=True,
                         text=True, check=True).stdout
    starts = sorted(funcs)
    graph = {}
    current = None
    head = re.compile(r'^([0-9a-f]+) <(.*)>:$')
    branch = re.compile(
        r'\t(blx?|b(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?'
        r'(?:\.[nw])?|cbn?z\s+r\d+,)\s+([0-9a-f]+) <')
    for line in out.splitlines():
        m = head.match(line)
        if m:
            current = int(m.group(1), 16)
            graph.setdefault(current, set())
            continue
        m = branch.search(line)
        if m and current is not None:
            target = int(m.group(2), 16)
            if target in funcs and target != current:
                graph[current].add(target)
    return graph


def name_only(pretty):
    """A demangled name without its parameters."""
    depth = 0
    for i, c in enumerate(pretty):
        if c in '<':
            depth += 1
        elif c in '>':
            depth -= 1
        elif c == '(' and depth == 0 and i > 0:
            return pretty[:i]
    return pretty


def reach(elf, entries, exclude):
    funcs = functions(elf)
    graph = calls(elf, funcs)
    # A veneer (the linker's, for a call between .fast and the rest)
    # leads to its function.
    by_mangled = {f[0]: a for a, f in funcs.items()}
    for addr, (mangled, pretty, size) in funcs.items():
        m = re.match(r'__(.*)_veneer$', mangled)
        if m and m.group(1) in by_mangled:
            graph.setdefault(addr, set()).add(by_mangled[m.group(1)])
    by_name = {}
    for addr, (mangled, pretty, size) in funcs.items():
        by_name.setdefault(mangled, addr)
        by_name.setdefault(pretty, addr)
    todo = []
    for e in entries:
        matches = [a for a, f in funcs.items() if f[0] == e or f[1] == e or
                   f[1].startswith(e + '(')]
        if not matches:
            sys.exit('fast_sections.py: no function %s' % e)
        todo += matches
    excluded = re.compile(exclude) if exclude else None
    seen = set()
    skipped = set()
    while todo:
        a = todo.pop()
        if a in seen:
            continue
        name = name_only(funcs[a][1])
        if excluded and excluded.search(name) or name.endswith('_veneer'):
            skipped.add(a)
            continue
        seen.add(a)
        todo += graph.get(a, ())
    return funcs, seen, skipped


def main():
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['ld', 'check'])
    p.add_argument('elf')
    p.add_argument('entries', nargs='+')
    p.add_argument('--exclude', default='')
    args = p.parse_args()
    funcs, seen, skipped = reach(args.elf, args.entries, args.exclude)
    if args.mode == 'ld':
        total = 0
        for a in sorted(seen, key=lambda a: funcs[a][0]):
            mangled, pretty, size = funcs[a]
            total += size
            print('    *(.text.%s)  /* %s */' % (mangled,
                                                 pretty[:60].replace('*/', '')))
        sys.stderr.write('fast_sections.py: %d functions, %d bytes\n'
                         % (len(seen), total))
        return
    bad = [a for a in seen if not FAST_BEGIN <= a < FAST_END]
    for a in sorted(bad):
        print('not in .fast: %08x %s' % (a, funcs[a][1][:100]))
    if bad:
        sys.exit(1)
    print('.fast: %d functions, %d bytes, all at %08x-%08x (%d excluded '
          'callees)' % (len(seen), sum(funcs[a][2] for a in seen),
                        FAST_BEGIN, FAST_END, len(skipped)))


if __name__ == '__main__':
    main()
