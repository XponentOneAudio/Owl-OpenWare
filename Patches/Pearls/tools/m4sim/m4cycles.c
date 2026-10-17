// QEMU TCG plugin: approximate Cortex-M4 cycle counts, with a model of the
// OWL2 (STM32F427) external SDRAM.
//
// Instruction costs are computed ahead of time by m4costs.py from an objdump
// of the guest ELF (this QEMU build has no ARM disassembler). They follow
// the Cortex-M4 TRM (r0p1, section 3.3 and the FPU chapter):
// single-cycle ALU and MUL, 2-cycle loads (1 when pipelined after another
// load/store), 1-cycle stores, 1+N load/store multiple, 3-cycle VMLA/VFMA,
// 14-cycle VDIV/VSQRT, and a 2-cycle average pipeline refill on branches.
//
// Data accesses inside [SDRAM_BASE, SDRAM_BASE + SDRAM_SIZE) get the extra
// latency of the FMC SDRAM controller as configured on the Lich: 16-bit bus,
// SDCLK = HCLK / 2 = 90 MHz, CAS 3, tRP = tRCD = 2, 4 banks, 8 column bits
// (512-byte rows), read burst on. A row hit costs CAS + 2 data beats, a row
// miss adds precharge and activate; reads served from the read FIFO (same
// 16-byte line as the previous SDRAM read) are nearly free. Writes go through
// the FMC write FIFO.
//
// Phases: a guest store to MARK_BASE + 4 * k starts phase k. Totals per phase
// are printed at exit as "phase k: cycles insns sdram_reads sdram_writes".

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu-plugin.h"  // fetched by the Makefile

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint32_t SDRAM_BASE = 0x00100000u;
#define SDRAM_SIZE 0x00300000u
#define MARK_BASE 0x203ff000u
#define NUM_PHASES 64

// SDRAM penalties in HCLK cycles, on top of the 2-cycle load already counted.
static int kReadHit = 9;     // 3 CAS + 2 beats = 5 SDCLK = 10 HCLK, +1 AHB
static int kReadMiss = 17;   // + tRP 2 + tRCD 2 SDCLK
static int kReadFifo = 1;
static int kWriteHit = 2;
static int kWriteMiss = 6;

static uint64_t cycles;
static uint64_t insns;
static uint64_t sdram_reads;
static uint64_t sdram_writes;

static uint64_t phase_cycles[NUM_PHASES];
static uint64_t phase_insns[NUM_PHASES];
static uint64_t phase_reads[NUM_PHASES];
static uint64_t phase_writes[NUM_PHASES];
static int phase = 0;
static uint64_t phase_start_cycles, phase_start_insns;
static uint64_t phase_start_reads, phase_start_writes;

static uint32_t open_row[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
static uint32_t last_read_line = 0xffffffff;

enum Kind { K_OTHER = 0, K_LOAD = 1, K_STORE = 2, K_MULTI = 3 };

// Per-address costs from tools/m4costs.py (objdump of the guest ELF):
// lines "address cost kind" (hex address).
static GHashTable* costs;
#define MAX_FUNCS 0x7ffff
static char* func_names[MAX_FUNCS];
static uint64_t func_cycles[MAX_FUNCS];

static void load_costs(const char* path) {
  costs = g_hash_table_new(g_direct_hash, g_direct_equal);
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "m4cycles: cannot open %s\n", path);
    exit(1);
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    unsigned long addr;
    int cost, kind, fn;
    char name[480];
    if (sscanf(line, "F %d %479s", &fn, name) == 2) {
      if (fn >= 0 && fn < MAX_FUNCS) func_names[fn] = g_strdup(name);
    } else if (sscanf(line, "%lx %d %d %d", &addr, &cost, &kind, &fn) == 4) {
      if (fn < 0 || fn >= MAX_FUNCS) fn = MAX_FUNCS - 1;
      g_hash_table_insert(costs, GUINT_TO_POINTER(addr),
                          GUINT_TO_POINTER(((uint32_t)fn << 12) | (cost << 4) |
                                           kind | 0x80000000u));
    }
  }
  fclose(f);
}

static uint64_t unknown_insns;

static void mem_cb(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void* udata) {
  uint32_t a = (uint32_t)vaddr;
  bool store = qemu_plugin_mem_is_store(info);
  if (store && a >= MARK_BASE && a < MARK_BASE + 4 * NUM_PHASES) {
    phase_cycles[phase] += cycles - phase_start_cycles;
    phase_insns[phase] += insns - phase_start_insns;
    phase_reads[phase] += sdram_reads - phase_start_reads;
    phase_writes[phase] += sdram_writes - phase_start_writes;
    phase = (a - MARK_BASE) / 4;
    phase_start_cycles = cycles;
    phase_start_insns = insns;
    phase_start_reads = sdram_reads;
    phase_start_writes = sdram_writes;
    return;
  }
  if (a - SDRAM_BASE >= SDRAM_SIZE) return;
  uint32_t off = a - SDRAM_BASE;
  uint32_t bank = (off >> 9) & 3;
  uint32_t row = off >> 11;
  bool hit = open_row[bank] == row;
  open_row[bank] = row;
  int penalty;
  if (store) {
    ++sdram_writes;
    penalty = hit ? kWriteHit : kWriteMiss;
  } else {
    ++sdram_reads;
    uint32_t line = off >> 4;
    penalty = line == last_read_line ? kReadFifo : hit ? kReadHit : kReadMiss;
    last_read_line = line;
  }
  cycles += penalty;
  if (phase != 0) func_cycles[GPOINTER_TO_UINT(udata)] += penalty;
}

static bool profile;

static void func_cb(unsigned int vcpu, void* udata) {
  uint32_t v = GPOINTER_TO_UINT(udata);
  if (phase != 0) func_cycles[v >> 8] += v & 0xff;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
  size_t n = qemu_plugin_tb_n_insns(tb);
  for (size_t i = 0; i < n; ++i) {
    struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
    uint32_t v = GPOINTER_TO_UINT(g_hash_table_lookup(
        costs, GUINT_TO_POINTER((unsigned long)qemu_plugin_insn_vaddr(insn))));
    int cost = 1;
    int kind = K_STORE;  // unknown code: watch its memory accesses anyway
    uint32_t fn = MAX_FUNCS - 1;
    if (v) {
      cost = (v >> 4) & 0xff;
      kind = v & 0xf;
      fn = (v & 0x7fffffffu) >> 12;
    } else {
      qemu_plugin_register_vcpu_insn_exec_inline(
          insn, QEMU_PLUGIN_INLINE_ADD_U64, &unknown_insns, 1);
    }
    qemu_plugin_register_vcpu_insn_exec_inline(
        insn, QEMU_PLUGIN_INLINE_ADD_U64, &cycles, cost);
    qemu_plugin_register_vcpu_insn_exec_inline(
        insn, QEMU_PLUGIN_INLINE_ADD_U64, &insns, 1);
    // Function profile (attributed to all non-idle phases together).
    if (profile) qemu_plugin_register_vcpu_insn_exec_cb(insn, func_cb, QEMU_PLUGIN_CB_NO_REGS,
                                           GUINT_TO_POINTER((fn << 8) | cost));
    if (kind != K_OTHER) {
      qemu_plugin_register_vcpu_mem_cb(insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS,
                                       QEMU_PLUGIN_MEM_RW, GUINT_TO_POINTER(fn));
    }
  }
}

static void plugin_exit(qemu_plugin_id_t id, void* p) {
  phase_cycles[phase] += cycles - phase_start_cycles;
  phase_insns[phase] += insns - phase_start_insns;
  phase_reads[phase] += sdram_reads - phase_start_reads;
  phase_writes[phase] += sdram_writes - phase_start_writes;
  FILE* f = stderr;
  const char* path = getenv("M4CYCLES_OUT");
  if (path) f = fopen(path, "w");
  for (int i = 0; i < NUM_PHASES; ++i) {
    if (phase_insns[i]) {
      fprintf(f, "phase %d: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
              i, phase_cycles[i], phase_insns[i], phase_reads[i],
              phase_writes[i]);
    }
  }
  fprintf(f, "unknown: %" PRIu64 "\n", unknown_insns);
  if (profile) {
    for (int i = 0; i < MAX_FUNCS; ++i) {
      if (func_cycles[i]) {
        fprintf(f, "func %" PRIu64 " %s\n", func_cycles[i],
                func_names[i] ? func_names[i] : "?");
      }
    }
  }
  if (f != stderr) fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t* info, int argc,
                                           char** argv) {
  for (int i = 0; i < argc; ++i) {
    // sdram=0 disables the SDRAM penalties (internal SRAM only).
    if (strcmp(argv[i], "profile=1") == 0) profile = true;
    if (strncmp(argv[i], "table=", 6) == 0) {
      load_costs(argv[i] + 6);
    }
    if (strncmp(argv[i], "base=", 5) == 0) {
      SDRAM_BASE = strtoul(argv[i] + 5, NULL, 0);
    }
    if (strcmp(argv[i], "sdram=0") == 0) {
      kReadHit = kReadMiss = kReadFifo = kWriteHit = kWriteMiss = 0;
    }
  }
  if (!costs) {
    fprintf(stderr, "m4cycles: table=<file> required\n");
    return 1;
  }
  qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
  qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
  return 0;
}
