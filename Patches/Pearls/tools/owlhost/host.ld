ENTRY(Reset_Handler)
MEMORY {
  VECTORS (rx) : ORIGIN = 0x00000000, LENGTH = 1K
  CODE (rx)  : ORIGIN = 0x00200000, LENGTH = 1M
  RAM (rwx)  : ORIGIN = 0x20300000, LENGTH = 1M - 4K
}
SECTIONS {
  .vectors : { KEEP(*(.isr_vector)) } > VECTORS
  .text : {
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
}
