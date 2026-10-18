# Pearls on the Befaco Lich

Pearls is an unofficial, non-commercial port of the **Mutable Instruments
Beads** granular processor, first made for the Qu-Bit Nebulae v2. This is
the same engine as an OWL patch for the **Befaco Lich** with the OWL2 board
(STM32F427 at 180 MHz). It comes with a paged control scheme for the Lich's
four knobs and two buttons, and most of the Pearls additions.

The audio code is Emilie Gillet's Beads DSP (MIT licence). The changes this
port needed are listed in `third_party/eurorack/UPSTREAM.md`. Following the
upstream README, derivative works don't use the original name.

> **Status: not yet tested on a Lich.** Everything builds into a patch that
> fits the Lich (142 kB of the 144 kB patch memory), and the tests pass on
> the desktop: the panel logic, the OWL patch against a stand-in for the OWL
> API, and the DSP against unmodified Beads. CPU use is estimated with an
> emulated Cortex-M4 (see [CPU](#cpu)). Please report the CPU percentage
> the OWL web interface shows, especially in delay mode.

## The big difference: CPU

Beads runs on a 400 MHz Cortex-M7. The Lich's Cortex-M4 at 180 MHz has
about a third of its throughput, and Beads with all 32 grains would need up
to twice the Lich's CPU. So on the Lich:

- **The number of grains follows the CPU load.** The patch measures how
  long each audio block takes and keeps the load around 70-85% by changing
  the number of grain voices, from 4 to 32. Light settings get all 32.
  Dense clouds of long grains get about 10, and the oldest grains fade out
  as on Beads when its 32 voices are used up.
- **The reverb runs at 24 kHz.** That halves its cost, and its memory then
  fits in the fast internal RAM. Compared with Beads' reverb, decay times
  are within 6% and levels within 1 dB, and the 8-12 kHz band is 10 dB
  lower.
- **The engine works in blocks of 16 frames** (Beads: 12). That divides the
  Lich's 32-frame audio blocks, so every block does the same work: with
  12, blocks alternated between three and two engine blocks. The patch
  works with any OpenWare block size and doesn't change it. A multiple of
  16 (32, 48, 64...) keeps the load even.
- **Everything else is Beads' DSP, with the same output.** The grains are
  rendered block by block, and silent taps, a silent reverb and repeated
  divisions are skipped. The tests check the output against unmodified
  Beads.

## Installing

The `prebuilt/` directory has everything as SysEx files. Send them to the
Lich over USB, one file at a time. Each file is a few hundred SysEx
messages, which must not be sent too fast:

- **`tools/send_sysex.py`** paces them (10 ms apart). It needs Python 3 and
  `pip install mido python-rtmidi`; then run
  `python3 tools/send_sysex.py prebuilt/pearls-lich.syx`. `--list` shows
  the MIDI ports, `--port` picks one (default: the first with "OWL" in its
  name), and `--delay 20` slows down further.
- **MIDI-OX** (Windows): "SysEx: no buffers available" means its output
  buffers are too few or too small. In View > SysEx, open Sysex >
  Configure: set the low level output buffers to 64 buffers of 512 bytes,
  and "Delay after F7" to 10 ms (20 if a transfer still fails). Close
  other programs using the Lich's MIDI port first.
- **Others:** SysEx Librarian (macOS) and `amidi -p hw:1,0,0 -s file.syx`
  (Linux) also work. Add a delay between messages if the transfer fails.

1. **`pearls-waves.syx`** (once). It stores the wavetables of the synth
   mode in the Lich's flash, as a 100 kB resource named `pearls-waves`.
   They don't fit in the patch. Without them, everything but the synth mode
   works.
2. **The patch.** `pearls-lich.syx` loads it and runs it from RAM, which is
   good for trying it out. `pearls-lich-slot1.syx` stores it in slot 1.
   For another slot, use
   `python3 tools/make_sysex.py prebuilt/pearls-lich.bin out.syx --store N`.
   Then choose the slot with the Lich's encoder and press it to load.
3. **Samples** (optional, see [Samples](#samples)).

## Panel

The Lich has four knobs, each with a CV input added to it (0-10 V), and two
buttons, each OR'd with a gate input by the OWL firmware. So a press and a
gate are the same thing to a patch. Pearls uses:

| Lich | Pearls |
|---|---|
| Knobs A-D + CV | the parameters of the current page (below) |
| Button 1 / gate 1 | **FREEZE** |
| Button 2 / gate 2 | **SEED** (button, clock input or tap tempo: setup page) |
| Gate out | grain gate (Beads' gate) |
| CV out 1 | a new random value with each grain, 0-10 V |
| CV out 2 | output level, 0-10 V |
| LED 1 | frozen |
| LED 2 | grain activity; dim while the generator is locked and idle |

### Pages

| Page | A | B | C | D |
|---|---|---|---|---|
| **1 Grains** | TIME | SIZE (fully clockwise: delay mode) | DENSITY (noon: no grains) | PITCH (1 V/oct) |
| **2 Mix** | SHAPE | FEEDBACK | DRY/WET | REVERB |
| **3 Random** | TIME randomness | SIZE randomness | SHAPE randomness | PITCH randomness |
| **4 Extras** | REVERSED GRAINS | STEREO SPREAD | SCALE | SAMPLE SLOT |
| **5 Setup** | QUALITY | INPUT GAIN | BUFFER | SEED MODE |

**Changing page: hold SEED, then press FREEZE.** The pages go round 1 to 5,
then back to 1. SEED must have been held for a quarter of a second, so a
trigger in the SEED gate input with a short pulse doesn't change the page.
The FREEZE press doesn't freeze. In button mode, the gesture also locks the
grain generator, as Beads' own SEED + FREEZE combination does.

- **The page is shown by both LEDs blinking its number** (1 to 5). Away
  from page 1, LED 2 pulses the page number every 2 s.
- **Pickup.** On a page you have just entered, a knob doesn't change its
  parameter until it reaches the parameter's value or passes it. Then it
  takes over. A parameter keeps its value while you are on other pages.
- **CVs follow the knobs.** On the Lich, a CV input is added to its knob in
  hardware, so CV A modulates whatever knob A controls on the current page.
  The parameters of the other pages keep their values. Stay on page 1 when
  you play with CVs on TIME, SIZE, DENSITY and PITCH.

**Page 1 Grains** are Beads' main controls.
- **PITCH:** 12 semitones per volt (1 V/oct on the CV), unison at noon,
  limited to ±48 semitones. The knob is coarse; the scales on page 4 help.
- **SIZE** fully clockwise switches to the delay mode, as on Beads.

**Page 2 Mix** is Beads' remaining knobs.

**Page 3 Random** is Beads' four attenuverters with nothing patched (that
is what they do on Beads then): noon = none; clockwise = random values for
each grain, more and more; counter-clockwise = occasional random jumps.

**Page 4 Extras** are Pearls additions; fully counter-clockwise is Beads.
- **Reversed grains:** grains play backwards (Beads only reverses grains
  with SIZE left of 11 o'clock); clockwise, more of the others do too.
- **Stereo spread:** grains spread further across the stereo field.
- **Scale** (8 zones): pitch quantisation of the grains, after their
  randomisation. The scales are off, chromatic, major, minor, major
  pentatonic, minor pentatonic, octaves and fifths, and octaves. Both LEDs
  blink the zone number when it changes. The root is the unison (the
  recorded pitch).
- **Sample slot** (9 zones): the first is none, then slots 1 to 8.
  Choosing a slot loads that sample into the buffer, frozen.

**Page 5 Setup:**
- **Quality** (4 zones, clockwise): hi-fi, cold digital, sunny tape,
  scorched cassette. Both LEDs blink 1 to 4. As on Beads, changing the
  quality clears the buffer, so it waits while the buffer is frozen.
- **Input gain:** fully counter-clockwise is Beads' automatic gain;
  clockwise is manual, from 1× to 40×.
- **Buffer** (3 zones): stereo, mono (left input only, twice the recording
  time), and Beads' wavetable synth mode. Synth mode needs the wavetables
  (install step 2); without them, the LEDs flicker 6 times.
- **SEED mode** (3 zones): button, clock, or tap (below).

### FREEZE (button 1, gate 1)

- **A press toggles freeze.** So does a trigger in gate 1.
- **Held longer than 0.6 s, it's momentary:** releasing it unfreezes. So a
  long gate freezes while it is high, as Beads' FREEZE gate does.
- After loading a sample, the buffer is frozen. One press releases it.

### SEED (button 2, gate 2)

The SEED mode (page 5, knob D) picks what button 2 and gate 2 do:

- **Button** (default): Beads' SEED button. A press makes grains and
  unlocks the generator, so grains then come only from presses. Hold it for
  4 s and release it to lock the generator again, or change page. A gate
  in gate 2 acts as presses.
- **Clock:** Beads' SEED input, for a clock or triggers in gate 2. It
  counts as patched while it receives edges (it times out after 4 s).
  Then, as on Beads, DENSITY divides the clock counter-clockwise of noon
  and sets the probability of a grain on each pulse clockwise. A 4 s
  press toggles the lock.
- **Tap:** Pearls' tap tempo. Tap two or more times to clock the grains (or
  the delay) at that tempo, as if a clock were in gate 2. A single tap
  later re-aligns the beat, and a 1 s press stops the clock. Changing page
  counts as a tap.

### LEDs

| LED 1 | LED 2 |
|---|---|
| on: frozen | flashes with the grains (Beads' SEED LED) |
| flickers: loading a sample | dim: generator locked, no grains |
| | page 2-5: pulses the page number every 2 s |

Both LEDs blink together to show a number: the page, the quality, the
scale, the sample slot, or the lock (1 = locked, 2 = unlocked, clock mode).
Six fast blinks mean an error, such as a missing sample or no wavetables.

## Samples

The extras page (knob D) loads one of 8 samples into the buffer, frozen, as
if FREEZE had been pressed. The samples are resources in the Lich's flash,
named `pearls-1.wav` to `pearls-8.wav`. Make the SysEx with:

    python3 tools/make_sample_syx.py my-sound.wav 3 pearls-3.syx
    python3 tools/make_sample_syx.py my-sound.wav 4 pearls-4.syx --mono --rate 24000

- The WAV files can be anywhere: give the tool their paths. The slot number
  (1 to 8) sets the name the sample is stored under, `pearls-N.wav`. Sending
  a file for a slot again replaces that sample.
- Only Python 3 is needed, no toolchain.
- The tool reads 8, 16, 24 and 32-bit and floating-point WAV files, and
  converts them to 16 bits.
- Send each `.syx` file to the Lich as for the patch. Then choose the slot
  on page 4, knob D.
- OpenWare resources are at most 1 MB, so the file is cut to fit: about 5 s
  of 48 kHz stereo, or 21 s of 24 kHz mono.
- The buffer itself holds 4 s in hi-fi stereo and up to 32 s in scorched
  cassette mono. The sunny tape and scorched cassette qualities record at
  24 kHz, which is what `--rate 24000` gives.
- The patch reads the file a few kB per audio block while LED 1 flickers,
  then copies it into the buffer, converted to the quality's sample rate.
  The output is muted meanwhile, for about a second.

**Not possible on the Lich:** saving the buffer, and keeping the quality or a
frozen buffer across power cycles (as Pearls does on the Nebulae).
OpenWare gives patches no way to write the flash.

## CPU

Estimated with `tools/m4sim`, a QEMU Cortex-M4 with the Lich's memory
timings, as a share of the 180 MHz CPU. These are estimates (within about
±25%), not measurements.

Each audio block must finish within its time, so the busiest block (the
peak) matters more than the average.

| Settings | Average | Busiest block | Governed |
|---|---|---|---|
| Granular, typical (density 3 o'clock, SIZE 0.6) | 45% (hi-fi) to 75% (cold digital) | 62-100% | same |
| Granular, dense long grains (SIZE 0.8-0.95), 32 grains | 127-187% | 160-240% | about 10 grains, under 85% |
| Delay mode, with reverb | 80% (sparse) to 100% (cold digital) | 93-118% | not governed |
| Wavetable synth | 60% | 79% | same |

The governor watches the busiest recent blocks, not the average.

- **Delay mode is the tight spot.** Its taps can't be reduced the way
  grains can, and its busiest blocks are estimated just over the budget.
  If the Lich glitches in delay mode, turn the reverb down
  (with nothing sent to it, the reverb stops using CPU once its tail has
  died away), or prefer the hi-fi and sunny tape qualities.
- **The OWL web interface shows the patch's CPU load.** Please report it,
  with the settings.

## Differences from Beads

- **Grain voices** follow the CPU load (4 to 32), and the **reverb** runs at
  24 kHz. See [The big difference](#the-big-difference-cpu).
- **Controls:** pages, pickup, and CVs that follow the knobs (see
  [Pages](#pages)). There are no separate attenuverters: page 3 sets the
  internal randomisation, which is what Beads' attenuverters do with no
  CV patched.
- **Buttons and gates are the same inputs.** FREEZE toggles on both, and is
  momentary when held. SEED is either Beads' button or Beads' input
  (setup page).
- **No jack detection:** mono and synth mode are chosen on the setup page.
  As on the Nebulae, the automatic gain treats a silent input (below
  -60 dBFS for 2 s) as unpatched.
- **Sample rate:** the codec stays at 48 kHz. Scorched cassette uses Beads'
  48 kHz + 2:1 resampling path. Cold digital runs at 32 kHz through a
  96-tap polyphase resampler (192 taps on the Nebulae).
- **Gate timing:** buttons and gates are read once per audio block (0.7 ms
  with OpenWare's default 32-frame blocks).
- **Control rate:** the engine works in 16-frame blocks (3 kHz) instead of
  Beads' 12 (4 kHz). Triggers and gates are still placed per sample.
- **No saving or memory** (see [Samples](#samples)).

## Building

    make test                    # desktop tests (g++)
    make syx OWLPROGRAM=<OwlProgram checkout> TOOLROOT=<arm gcc>/bin/
    make prebuilt ...            # the same into prebuilt/

- [OwlProgram](https://github.com/RebelTechnology/OwlProgram) builds the
  patch. Run `make libs` in it once, with the same toolchain.
- It needs arm-none-eabi-gcc 10 to 12 (its CI uses 10-2020-q4). GCC 13
  refuses the C++ headers under OwlProgram's `-ffreestanding`. The xPack
  build of GCC 10.3 works.
- OwlProgram compiles every file of a flat patch directory, so
  `owl/Makefile` copies the sources into `build/owl-src` first.
- `make syx` checks each SysEx file by decoding it with OpenWare's own
  `sysex.c` and `crc32.c`.

| Path | |
|---|---|
| `engine/` | the Pearls engine (as on the Nebulae, plus a few hooks) |
| `third_party/eurorack/` | the Beads and stmlib DSP (`UPSTREAM.md` lists the changes) |
| `lich/` | the Lich panel, CPU governor and WAV reader: host-independent |
| `owl/` | the OWL patch, its build options (`lich_config.h`) and Makefile |
| `tools/lich_test.cc` | tests of the panel logic |
| `tools/patch_test.cc`, `tools/owl_mock/` | tests of the OWL patch against a stand-in for OwlProgram |
| `tools/check_block_rendering.sh` | the DSP changes against unmodified Beads |
| `tools/m4sim/` | the Cortex-M4 CPU estimates (QEMU plugin and benchmark) |
| `tools/reverb_compare.*` | the 24 kHz reverb against Beads' |
| `tools/make_sysex.py`, `make_sample_syx.py`, `make_wavetable_resource.py`, `check_sysex.c`, `send_sysex.py` | SysEx and resources |
| `prebuilt/` | the SysEx files and `BUILD_INFO.txt` |
