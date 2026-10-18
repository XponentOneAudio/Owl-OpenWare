/* A stand-in for the OpenWare firmware, to run the real Pearls patch binary
   (pearls-lich.bin) under QEMU (mps2-an386, Cortex-M4F): the program
   vector as v22.5 fills it for the Lich, its heap segments, the 8 kB stack
   of the firmware's audio task, the resource service call. It reports
   faults with their address, the stack used, and the output level.

   Memory: patch RAM at 0x2000c000 (144 kB, as on the Lich), "CCM" at
   0x20200000 (64 kB), "SDRAM" at 0x21000000 (8 MB), this host's own RAM
   above 0x20300000. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef struct { uint8_t* location; uint32_t size; } MemorySegment;
typedef struct {
  uint8_t checksum;
  uint8_t hardware_version;
  int32_t* audio_input;
  int32_t* audio_output;
  uint8_t audio_format;
  uint16_t audio_blocksize;
  uint32_t audio_samplingrate;
  int16_t* parameters;
  uint8_t parameters_size;
  uint16_t buttons;
  int8_t error;
  void (*registerPatch)(const char*, uint8_t, uint8_t);
  void (*registerPatchParameter)(uint8_t, const char*);
  void (*programReady)(void);
  void (*programStatus)(int);
  int (*serviceCall)(int, void**, int);
  uint32_t cycles_per_block;
  uint32_t heap_bytes_used;
  char* message;
  void (*setButton)(uint8_t, uint16_t, uint16_t);
  void (*setPatchParameter)(uint8_t, int16_t);
  void (*buttonChangedCallback)(uint8_t, uint16_t, uint16_t);
  MemorySegment* heapSegments;
} ProgramVector;

typedef struct {
  uint32_t magic;
  uint32_t* linkAddress;
  uint32_t* endAddress;
  void (*jumpAddress)(void);
  uint32_t* stackBegin;
  uint32_t* stackEnd;
  ProgramVector* programVector;
  char programName[24];
} ProgramHeader;

extern const uint8_t patch_bin[], patch_bin_end[];
extern const uint8_t waves_bin[], waves_bin_end[];

#define PATCHRAM ((uint8_t*)0x2000c000)
#define PATCHRAM_SIZE (144 * 1024)
#define CCM ((uint8_t*)0x20200000)
#define CCM_SIZE (64 * 1024)
#define EXTRAM ((uint8_t*)0x21000000)
#define EXTRAM_SIZE (8 * 1024 * 1024)
#define BLOCK 32
#define STACK_SIZE 8192
#define NOF_PARAMETERS 40

static ProgramVector* pv;
static int32_t audio_in[BLOCK * 2], audio_out[BLOCK * 2];
static int16_t parameters[NOF_PARAMETERS];
static MemorySegment segments[4];
static uint32_t program_stack[STACK_SIZE / 4] __attribute__((aligned(8)));
static int blocks, seconds;
static int run_seconds = 4;
static double out_energy, out_peak;
static int out_nan;
static unsigned leds;

static void onRegisterPatch(const char* name, uint8_t in, uint8_t out) {
  printf("registerPatch \"%s\" %d in %d out\n", name, in, out);
}
static void onRegisterPatchParameter(uint8_t id, const char* name) {
  printf("parameter %d: %s\n", id, name);
}
static void onProgramStatus(int status) {
  printf("programStatus %d (error %d: %s)\n", status, pv->error,
         pv->message ? pv->message : "");
  exit(2);
}
static void onSetButton(uint8_t id, uint16_t state, uint16_t samples) {
  if (id < 16) leds = state ? leds | (1u << id) : leds & ~(1u << id);
}
static void onSetPatchParameter(uint8_t id, int16_t value) {}

static int onServiceCall(int service, void** params, int len) {
  if (service == 0x1001 && len == 4) {  /* OWL_SERVICE_LOAD_RESOURCE */
    const char* name = (const char*)params[0];
    uint8_t** buffer = (uint8_t**)params[1];
    uint32_t offset = *(uint32_t*)params[2];
    uint32_t* max_size = (uint32_t*)params[3];
    printf("load resource \"%s\" offset %lu buffer %p size %lu\n", name,
           (unsigned long)offset, (void*)*buffer, (unsigned long)*max_size);
    if (strcmp(name, "pearls-waves") != 0 || (offset & 3)) return -1;
    uint32_t size = waves_bin_end - waves_bin;
    if (*buffer == NULL) {
      *max_size = size - offset;  /* port mapped: no pointer */
    } else {
      uint32_t n = *max_size < size - offset ? *max_size : size - offset;
      memcpy(*buffer, waves_bin + offset, n);
      *max_size = n;
    }
    return 0;
  }
  return -1;  /* tables, MIDI callbacks: not provided */
}

static uint32_t stack_used(void) {
  uint32_t i = 0;
  while (i < STACK_SIZE / 4 && program_stack[i] == 0xa5a5a5a5u) ++i;
  return STACK_SIZE - 4 * i;
}

/* Called by the patch before each block: report on the last one, and
   give it the next. */
static void onProgramReady(void) {
  if (blocks > 0) {
    for (int i = 0; i < BLOCK * 2; ++i) {
      int32_t v = audio_out[i] << 8 >> 8;  /* 24 bits */
      double x = v / 8388608.0;
      out_energy += x * x;
      if (fabs(x) > out_peak) out_peak = fabs(x);
    }
  }
  if (blocks % (48000 / BLOCK) == 0) {
    if (blocks > 0) {
      printf("%d s: out rms %.4f peak %.4f, leds %x, stack %lu, heap %lu\n",
             seconds, sqrt(out_energy / (48000 * 2)), out_peak, leds,
             (unsigned long)stack_used(),
             (unsigned long)pv->heap_bytes_used);
      out_energy = out_peak = 0;
    }
    if (seconds == run_seconds) {
      printf("ok\n");
      exit(0);
    }
    ++seconds;
  }
  /* Input: a 220 Hz / 330 Hz chord at -12 dB. */
  for (int i = 0; i < BLOCK; ++i) {
    double t = (blocks * BLOCK + i) / 48000.0;
    audio_in[2 * i] = (int32_t)(0.25 * sin(2 * M_PI * 220 * t) * 8388607);
    audio_in[2 * i + 1] = (int32_t)(0.25 * sin(2 * M_PI * 330 * t) * 8388607);
  }
  ++blocks;
}

void fault_report(uint32_t* frame, uint32_t lr) {
  volatile uint32_t* scb = (uint32_t*)0xE000ED00;
  printf("FAULT: pc %08lx lr %08lx sp %08lx, CFSR %08lx HFSR %08lx "
         "MMFAR %08lx BFAR %08lx, block %d, stack %lu\n",
         (unsigned long)frame[6], (unsigned long)frame[5],
         (unsigned long)(uint32_t)frame, (unsigned long)scb[0x28 / 4],
         (unsigned long)scb[0x2c / 4], (unsigned long)scb[0x34 / 4],
         (unsigned long)scb[0x38 / 4], blocks, (unsigned long)stack_used());
  exit(3);
}

int main(void) {
  const ProgramHeader* header = (const ProgramHeader*)patch_bin;
  size_t size = patch_bin_end - patch_bin;
  printf("patch \"%s\": %u bytes, link %p end %p entry %p stack %p-%p pv %p\n",
         header->programName, (unsigned)size, (void*)header->linkAddress,
         (void*)header->endAddress, (void*)header->jumpAddress,
         (void*)header->stackBegin, (void*)header->stackEnd,
         (void*)header->programVector);
  if (header->magic != 0xDADAC0DE || (uint8_t*)header->linkAddress != PATCHRAM) {
    printf("not a patch for this address\n");
    return 1;
  }
  uint32_t program_size = (uint8_t*)header->endAddress - PATCHRAM;
  if (program_size > PATCHRAM_SIZE) {
    printf("too big\n");
    return 1;
  }
  /* As DynamicPatchDefinition: copy, blank the rest of the program. */
  memcpy(PATCHRAM, patch_bin, size);
  if (size < program_size) memset(PATCHRAM + size, 0, program_size - size);

  /* As updateProgramVector (v22.5, Lich). */
  pv = header->programVector;
  memset(pv, 0, sizeof(*pv));
  pv->hardware_version = 0x1e;  /* LICH_HARDWARE */
  pv->checksum = 0x51;
  pv->parameters_size = NOF_PARAMETERS;
  pv->parameters = parameters;
  pv->audio_samplingrate = 48000;
  pv->audio_blocksize = BLOCK;
  pv->registerPatch = onRegisterPatch;
  pv->registerPatchParameter = onRegisterPatchParameter;
  pv->programReady = onProgramReady;
  pv->programStatus = onProgramStatus;
  pv->serviceCall = onServiceCall;
  pv->setButton = onSetButton;
  pv->setPatchParameter = onSetPatchParameter;
  pv->audio_input = audio_in;
  pv->audio_output = audio_out;
  pv->audio_format = 0x20;  /* AUDIO_FORMAT_24B32 */
  uint8_t* end = (uint8_t*)header->stackBegin;
  uint32_t remain = PATCHRAM + PATCHRAM_SIZE - end;
  if (end < PATCHRAM || end + remain > PATCHRAM + PATCHRAM_SIZE) remain = 0;
  int n = 0;
  segments[n++] = (MemorySegment){CCM, CCM_SIZE};
  if (remain >= 32) segments[n++] = (MemorySegment){end, remain};
  segments[n++] = (MemorySegment){EXTRAM, EXTRAM_SIZE};
  segments[n] = (MemorySegment){NULL, 0};
  for (int i = 0; i < n; ++i) {
    printf("heap segment %p %lu\n", (void*)segments[i].location,
           (unsigned long)segments[i].size);
    memset(segments[i].location, 0, segments[i].size);
  }
  pv->heapSegments = segments;
  for (int i = 0; i < 4; ++i) parameters[i] = 2048;

  /* Run it on an 8 kB stack, as the firmware's audio task. */
  for (int i = 0; i < STACK_SIZE / 4; ++i) program_stack[i] = 0xa5a5a5a5u;
  void (*entry)(void) = header->jumpAddress;
  __asm volatile("mov sp, %0\n blx %1\n" ::
                 "r"(program_stack + STACK_SIZE / 4), "r"(entry) : "memory");
  printf("the patch returned\n");
  return 1;
}
