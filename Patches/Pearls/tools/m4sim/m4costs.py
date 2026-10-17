#!/usr/bin/env python3
"""Cortex-M4 cycle cost per instruction address, for the m4cycles plugin.

Usage: m4costs.py guest.elf > costs.txt
Output lines: "<hex address> <cycles> <kind> <function index>", kind 0 =
other, 1 = load, 2 = store, 3 = load/store multiple; then lines
"F <index> <name>" naming the functions.

Costs follow the Cortex-M4 TRM: 1-cycle ALU and MUL/MLA/SMULL, 2-cycle loads
(1 when directly after another load/store, which the M4 pipelines), 1-cycle
stores, 1+N for LDM/STM/PUSH/POP and VLDM/VSTM/VPUSH/VPOP, 3-cycle VMLA/VFMA
and friends, 14-cycle VDIV/VSQRT, 2-12 cycles for SDIV/UDIV (7 assumed), and a
pipeline refill of 2 cycles on taken branches (conditional ones counted as
taken half the time).
"""

import re
import subprocess
import sys

LINE = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(\S+)\s*(.*)$')
CONDS = ('eq', 'ne', 'cs', 'cc', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi',
         'ls', 'ge', 'lt', 'gt', 'le', 'al')


def count_regs(ops):
    m = re.search(r'\{([^}]*)\}', ops)
    if not m:
        return 1
    n = 0
    for part in m.group(1).split(','):
        part = part.strip()
        r = re.match(r'([rsd])(\d+)-[rsd](\d+)', part)
        if r:
            n += int(r.group(3)) - int(r.group(2)) + 1
        elif part:
            n += 1
    # A double register moves two words.
    if re.search(r'\bd\d', m.group(1)):
        n *= 2
    return n


def strip_cond(mn, base):
    """True when mn is base, optionally followed by a condition code."""
    if mn == base:
        return True
    return mn.startswith(base) and mn[len(base):] in CONDS


def cost(mn, ops, prev_ldst):
    """Returns (cycles, kind)."""
    mn = mn.split('.')[0]
    if mn.startswith('v'):
        if mn.startswith(('vldm', 'vstm', 'vpush', 'vpop')):
            return 1 + count_regs(ops), 3
        if mn.startswith('vldr'):
            return (1 if prev_ldst else 2) + ('d' in ops.split(',')[0]), 1
        if mn.startswith('vstr'):
            return (1 if prev_ldst else 2) + ('d' in ops.split(',')[0]), 2
        if mn.startswith(('vdiv', 'vsqrt')):
            return 14, 0
        if mn.startswith(('vmla', 'vmls', 'vnmla', 'vnmls', 'vfma', 'vfms',
                          'vfnma', 'vfnms')):
            return 3, 0
        if mn.startswith('vmov') and ops.count(',') >= 2:
            return 2, 0
        return 1, 0
    if mn.startswith(('push', 'stm')):
        return 1 + count_regs(ops), 3
    if mn.startswith(('pop', 'ldm')):
        return 1 + count_regs(ops) + (2 if 'pc' in ops else 0), 3
    if mn.startswith(('ldrd', 'strd')):
        return 3, 1 if mn.startswith('ldrd') else 2
    if mn.startswith('ldr'):
        refill = 2 if ops.startswith('pc') else 0
        return (1 if prev_ldst else 2) + refill, 1
    if mn.startswith('str'):
        return 1, 2
    if mn.startswith(('sdiv', 'udiv')):
        return 7, 0
    if mn in ('b', 'bl', 'blx', 'bx'):
        return 3, 0
    if len(mn) == 3 and mn[0] == 'b' and mn[1:] in CONDS:
        return 2, 0
    if strip_cond(mn, 'bx') or strip_cond(mn, 'blx'):
        return 2, 0
    if mn.startswith(('cbz', 'cbnz')):
        return 2, 0
    if mn.startswith(('tbb', 'tbh')):
        return 4, 0
    if mn in ('nop',):
        return 1, 0
    return 1, 0


import os
PER_ADDRESS = os.environ.get('PER_ADDRESS') == '1'


def main():
    out = subprocess.run(['arm-none-eabi-objdump', '-d', '--no-show-raw-insn',
                          sys.argv[1]], capture_output=True, text=True,
                         check=True).stdout
    prev_ldst = False
    prev_addr = None
    lines = []
    func = re.compile(r'^[0-9a-f]+ <(.*)>:$')
    funcs = []
    simple = re.compile(r'^\s*([0-9a-f]+):\t(\S+)\s*(.*)$')
    for line in out.splitlines():
        f = func.match(line)
        if f:
            if not PER_ADDRESS:
                funcs.append(f.group(1))
            else:
                funcs.append('<' + f.group(1)[:40] + '>')
            continue
        m = simple.match(line)
        if not m:
            prev_ldst = False
            continue
        addr = int(m.group(1), 16)
        mn = m.group(2)
        ops = m.group(3).split(';')[0].strip()
        if mn.startswith('.'):
            prev_ldst = False
            continue
        c, kind = cost(mn, ops, prev_ldst)
        prev_ldst = kind in (1, 2)
        if PER_ADDRESS:
            funcs.append('%x:%s %s' % (addr, mn, ops.replace(' ', '')))
        lines.append('%x %d %d %d' % (addr, c, kind, len(funcs) - 1))
    lines += ['F %d %s' % (i, n) for i, n in enumerate(funcs)]
    sys.stdout.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
