#!/usr/bin/env python3
"""fast_sections.py: the functions to place in the Lich's fast code region.

The Lich runs the patch from SRAM, where the CPU fetches instructions over
the System bus; through SRAM1's alias at 0x00000000 (SYSCFG remap) it
fetches them over the I-Code bus instead, faster. Only the patch's first
64 kB are in SRAM1 (0x2000c000 to 0x2001c000), so only the hot code goes
there, and called through the alias, it must only call code there too: a
relative branch from the alias lands at the alias of its target.

  fast_sections.py ld PATCH.elf ENTRY... > fast.ld
      the linker script lines that place every function the entries reach
      (by direct calls and branches), except those matching --exclude, in
      the fast region
  fast_sections.py check PATCH.elf ENTRY...
      fails if a function the entries reach is outside the fast region
      (excluded functions aside)

--exclude NAME: a regular expression on the demangled names of functions
that the entries reach but that never run through the alias (for instance
the processors of the other qualities: see engine/pearls_engine.cc).
"""

import argparse
import re
import subprocess
import sys

FAST_BEGIN = 0x2000c000
FAST_END = 0x2001c000


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


def data_references(elf, funcs, seen):
    """[(instruction address, target)] of the PC-relative address
    computations (a literal loaded in a register, then add rN, pc: how -fpic
    code reaches its data and GOT) in the functions seen, target None when
    it cannot be followed. Through the alias, the target is aliased too."""
    import tempfile
    with tempfile.NamedTemporaryFile() as f:
        subprocess.run([tool('objcopy'), '-O', 'binary', elf, f.name],
                       check=True)
        image = f.read()
    # The binary starts at the lowest loaded section.
    headers = subprocess.run([tool('objdump'), '-h', elf], capture_output=True,
                             text=True, check=True).stdout.splitlines()
    base = min(int(h.split()[3], 16) for h, flags in zip(headers, headers[1:])
               if re.match(r'\s+\d+ ', h) and 'LOAD' in flags)

    def word(a):
        return int.from_bytes(image[a - base:a - base + 4], 'little')

    literal = re.compile(r'\tldr(?:\.w)?\s+(r\d+|sb|sl|fp|ip|lr), '
                         r'\[pc, #-?\d+\]\s*; \(?([0-9a-f]+)')
    add_pc = re.compile(r'\tadd\s+(r\d+|sb|sl|fp|ip|lr), pc$')
    insn = re.compile(r'^\s*([0-9a-f]+):')
    result = []
    for a in sorted(seen):
        size = funcs[a][2]
        out = subprocess.run(
            [tool('objdump'), '-d', '--start-address=0x%x' % a,
             '--stop-address=0x%x' % (a + size), elf],
            capture_output=True, text=True, check=True).stdout
        loaded = {}
        for line in out.splitlines():
            m = insn.match(line)
            if not m:
                continue
            here = int(m.group(1), 16)
            m2 = literal.search(line)
            if m2:
                loaded[m2.group(1)] = word(int(m2.group(2), 16))
                continue
            m3 = add_pc.search(line)
            if m3:
                value = loaded.get(m3.group(1))
                result.append((here, None if value is None else
                               (value + here + 4) & 0xffffffff))
    return result


def reach(elf, entries, exclude):
    funcs = functions(elf)
    graph = calls(elf, funcs)
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
        name = funcs[a][1]
        if excluded and excluded.search(name):
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
        print('outside the fast region: %08x %s' % (a, funcs[a][1][:100]))
    refs = data_references(args.elf, funcs, seen)
    bad_refs = [(a, t) for a, t in refs
                if t is None or not FAST_BEGIN <= t < FAST_END]
    for a, t in bad_refs:
        print('data reference at %08x %s' % (a, 'not followed' if t is None
                                            else 'to %08x, outside' % t))
    if bad or bad_refs:
        sys.exit(1)
    print('fast region: %d functions and their %d data references, all in '
          '%08x-%08x (%d excluded callees)'
          % (len(seen), len(refs), FAST_BEGIN, FAST_END, len(skipped)))


if __name__ == '__main__':
    main()
