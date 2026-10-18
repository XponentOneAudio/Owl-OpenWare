# Vendored upstream sources

These files are copied from Emilie Gillet's open-source Eurorack firmware.
They are MIT-licensed; the licence header in each file applies.

| Source | Repository | Commit |
|---|---|---|
| `beads/` | https://github.com/pichenettes/eurorack | `f4cfa7697095a28d292bff758beb51deb49b5a86` |
| `stmlib/` | https://github.com/pichenettes/stmlib | `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` |

Only the DSP code is included (no drivers, UI or bootloader), and only the
files the build actually uses.

## Local modifications

First change:

- `beads/dsp/processor.h`: `#define ENABLE_HARDWARE_24K` is wrapped in
  `#ifndef BEADS_NO_HARDWARE_24K`. The Nebulae codec cannot run at 24 kHz,
  so the port defines that macro, and "dirty tape" then uses the
  upstream 48 kHz + 2:1 resampling path.

Build flags `-DTEST -include cstdio` select stmlib's non-embedded code paths
and supply a missing `<cstdio>` include. No source edits are needed for them.

## Trademark note

From the upstream README: "Mutable Instruments" is a registered trademark,
and derivative works should not use that name or, ideally, the original
module name. The Nebulae instrument built from this code is therefore
called **Pearls**. It is an unofficial, non-commercial port of the Beads DSP.

Second change, also small:

- `beads/dsp/processor.{h,cc}` and `beads/dsp/synth.{h,cc}`: an optional
  `set_gate_output(bool*)` sink that receives the per-sample grain/tap gate.
  Upstream can only output the gate by replacing the right audio channel.
  Nebulae has a dedicated gate output jack (EOL), so the port keeps stereo
  audio and sends the gate there. Nothing changes when no sink is set.

Third change, for the Pearls additions (pearls5). Every addition defaults to
the upstream behaviour:

- `beads/dsp/granular_player.h`: `Parameters` gains `reverse_probability` and
  `stereo_spread`. With a spread above 0, each grain's pan moves towards a
  uniformly random position.
- `beads/dsp/delay_granular_player.h`: with a reverse probability above 0, a
  grain also plays backwards with that probability, in addition to Beads'
  short-grain reversal.
- `beads/dsp/granular_processor.h` and `beads/dsp/processor.h`: accessors to
  read, write and trim the recording buffer at its own rate. These are used
  to load samples and to save the buffer. `WasteCPUCycles` initialises the
  two new parameters.

Fourth change, pitch quantisation (pearls7):

- `beads/dsp/randomized_parameter.h`: `QuantizeSemitones()` and a
  `scale_mask` on `RandomizedParameter`. `pitch_value()`, which every grain
  calls when it starts, snaps the randomised pitch to the scale. A mask of 0
  (the default; also set in `granular_processor.h`'s `WasteCPUCycles`) is
  the upstream behaviour.

`git diff --no-index` against the upstream commit shows exactly these edits.

## Lich port changes

Copied from the Nebulae port (QB_Nebulae_V2 branch `ccr-c1f1464e-wcl7nr`,
commit `cbb6412`), then changed for the Befaco Lich (STM32F427 at 168 MHz,
against Beads' 400 MHz STM32H7). Unless a macro says otherwise, each change
gives the same output; `tools/check_block_rendering.sh` checks them against
an unmodified tree.

Same output:

- `delay_grain.h`, `delay_granular_player.h`, `granular_processor.h`,
  `delay_line.h`: grains are rendered over a block before it is written
  (`RenderGrainsBlock`, `RenderSegment`, `DelayLine::ReadAt`, read state
  accessors). Exact except for a grain fading out within the last frames
  of the buffer (gain under 1%, differences under 1e-5 in the tests).
  `-DBEADS_PER_SAMPLE_GRAINS` restores the per-sample rendering.
- `granular_player.h`: `set_max_grains()`, a runtime voice limit (32 =
  Beads). Above a lowered limit, grains fade out like stolen grains.
- `tap.h`: delay taps with zero gain skip their reads.
- `delay_controller.{h,cc}`: the tap envelope and play head triangle slopes
  are computed once per sample instead of once per tap.
- `fx/reverb.h`: the reverb is skipped while nothing is sent to it and its
  tail has been under -90 dB for a second (`-DBEADS_REVERB_ALWAYS_ON`
  disables this; differences under 2e-7 in the tests).
- `resources.{h,cc}`: with `BEADS_EXTERNAL_WAVETABLES`, the wavetables are
  a pointer the host sets (the Lich loads them from a resource).

Different output, selected by the Lich build (`owl/lich_config.h`):

- `fx/reverb.h`: `ReverbT<divider>`; `BEADS_REVERB_DIVIDER 2` runs the
  reverb at 24 kHz with half the memory (same decay times within 6%,
  same levels within 1 dB, the 8-12 kHz band 10 dB lower).
- `BEADS_NO_WASTE_CPU` removes the dummy grain players that Beads uses to
  waste CPU cycles.
- `multitap_delay.h`, `tap.h`, `delay_line.h`, `granular_processor.h`: the
  delay mode's taps are rendered a block at a time before the block is
  written (as the grains), when no tap reads a frame the block writes;
  each tap's frames are copied from the buffer in one go (burst reads from
  the SDRAM instead of 4 scattered reads per sample and head). Otherwise
  (delays near the buffer's length), the same code renders one sample at
  a time, after each write. Same output.
- `delay_controller.{h,cc}`: a tap's phase increment (a division) is only
  computed again when its delay changes, and a silent tap's envelope only
  advances. Same output.
- Some functions called once per grain or per block are kept out of line
  (`noinline`), and the block code is shared by the 8 delay line types,
  for the patch to fit in the Lich's 144 kB.
