#!/bin/sh
# run.sh <build dir> [plugin args]: runs bench.elf under QEMU with the
# m4cycles plugin and prints the cycles per second of audio.
B=$1; shift
python3 m4costs.py $B/bench.elf > $B/costs.txt
ARGS="table=$B/costs.txt"
for a in "$@"; do ARGS="$ARGS,$a"; done
M4CYCLES_OUT=$B/phases.txt timeout 3000 qemu-system-arm -M mps2-an386 -cpu cortex-m4 \
  -nographic -monitor none -serial none -semihosting-config enable=on,target=native \
  -kernel $B/bench.elf -plugin $B/m4cycles.so,$ARGS > $B/out.txt 2>&1
cat $B/out.txt
python3 - "$B" <<'PY'
import re, sys
b = sys.argv[1]
names = {}
for l in open(b + '/out.txt'):
    m = re.match(r'config (\d+): (.{28})', l)
    if m: names[int(m.group(1))] = m.group(2).strip()
print('%-28s %8s %8s %5s %9s' % ('config', 'Mcyc/s', 'Minsn/s', 'CPI', '% 180 MHz'))
funcs = []
for l in open(b + '/phases.txt'):
    m = re.match(r'phase (\d+): (\d+) (\d+) (\d+) (\d+)', l)
    if m and m.group(1) != '0':
        k = int(m.group(1)); c = int(m.group(2)); i = int(m.group(3))
        print('%-28s %8.1f %8.1f %5.2f %8.0f%%' % (names.get(k, k), c/1e6, i/1e6, c/i, 100*c/180e6))
    f = re.match(r'func (\d+) (.*)', l)
    if f: funcs.append((int(f.group(1)), f.group(2)))
if funcs:
    print('profile (all configurations):')
    for c, n in sorted(funcs, reverse=True)[:15]:
        print('  %8.1f M  %s' % (c/1e6, n[:100]))
PY
