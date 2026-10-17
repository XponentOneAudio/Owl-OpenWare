/* Bare-metal layout for QEMU mps2-an386 (Cortex-M4F) standing in for the
   OWL2: code in SSRAM1, internal data in SSRAM2, "SDRAM" pool in PSRAM. */
ENTRY(Reset_Handler)
MEMORY {
  CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 1M
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 4M - 4K
  EXT (rw)   : ORIGIN = 0x00100000, LENGTH = 3M
}
SECTIONS {
  /* The wavetables live in SDRAM on the Lich (loaded from a resource). */
  .extrodata : { *(.rodata.*wav_integrated_waves*) } > EXT
  .text : {
    KEEP(*(.isr_vector))
    *(.text*) *(.rodata*)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
    __preinit_array_start = .; KEEP(*(.preinit_array*)) __preinit_array_end = .;
    __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) __init_array_end = .;
    __fini_array_start = .; KEEP(*(.fini_array*)) __fini_array_end = .;
  } > CODE
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > CODE
  .data : { *(.data*) } > RAM
  .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end__ = .; } > RAM
  . = ALIGN(8);
  end = .; _end = .;
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  __ext_start = ORIGIN(EXT) + SIZEOF(.extrodata);
}
