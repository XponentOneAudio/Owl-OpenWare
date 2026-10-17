#!/bin/sh
# Compares the Lich block rendering of the grains with the upstream
# per-sample rendering over the scenarios in scenario_render.cc.
# With PRISTINE=<directory of an unmodified Pearls beads_port tree>, also
# compares with that tree (no Lich changes at all).
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/check}
mkdir -p $OUT
SRC="third_party/eurorack/beads/dsp/processor.cc third_party/eurorack/beads/dsp/synth.cc \
 third_party/eurorack/beads/dsp/mu_law.cc third_party/eurorack/beads/dsp/delay_clock.cc \
 third_party/eurorack/beads/dsp/delay_controller.cc third_party/eurorack/beads/dsp/trigger_generator.cc \
 third_party/eurorack/beads/resources.cc third_party/eurorack/stmlib/utils/random.cc \
 third_party/eurorack/stmlib/dsp/units.cc engine/pearls_engine.cc engine/gain_control.cc \
 tools/scenario_render.cc"
FLAGS="-O2 -DTEST -DBEADS_NO_HARDWARE_24K -include cstdio -I. -Ithird_party/eurorack -w ${EXTRA_FLAGS}"
${CXX:-g++} $FLAGS -DBEADS_PER_SAMPLE_GRAINS -o $OUT/render_upstream $SRC
${CXX:-g++} $FLAGS -o $OUT/render_block $SRC
$OUT/render_upstream > $OUT/upstream.raw
$OUT/render_block > $OUT/block.raw
python3 tools/compare_raw.py $OUT/upstream.raw $OUT/block.raw
if [ -n "$PRISTINE" ]; then
  P=$(cd "$PRISTINE" && pwd)
  PSRC=$(echo $SRC | sed "s#tools/scenario_render.cc##; s#\([a-z_]*/[^ ]*\)#$P/\1#g")
  ${CXX:-g++} -O2 -DTEST -DBEADS_NO_HARDWARE_24K -include cstdio -I$P -I$P/third_party/eurorack -w \
    -o $OUT/render_pristine $PSRC tools/scenario_render.cc
  $OUT/render_pristine > $OUT/pristine.raw
  echo "--- pristine Pearls vs Lich block rendering:"
  python3 tools/compare_raw.py $OUT/pristine.raw $OUT/block.raw
fi
