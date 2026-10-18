// Pearls for the Lich: the patch's entry point, and the calibration loops of
// PearlsPatch::Calibrate(), in assembly so that the compiler (with link time
// optimisation) neither changes nor moves them.

#ifdef __arm__
// The entry point (OpenWare jumps to it; OwlProgram's, in startup.s, is
// weak). It maps SRAM1 at 0x00000000 (SYSCFG_MEMRMP) before anything else
// runs: the hot code is linked at its alias there (owl/owl2-pearls.ld), where
// the CPU fetches it over the I-Code bus instead of the System bus. The
// firmware's vectors are at 0x08010000 (VTOR), and a reset restores the
// mapping. Then as startup.s: .data is in place (_sidata = _sdata), the
// .bss zeroed, the static constructors, main().
__asm__(
    "  .section .text.Reset_Handler,\"ax\",%progbits\n"
    "  .syntax unified\n"
    "  .thumb\n"
    "  .p2align 2\n"
    "  .global Reset_Handler\n"
    "  .thumb_func\n"
    "  .type Reset_Handler, %function\n"
    "Reset_Handler:\n"
    "  ldr r0, =0x40023844\n"       // RCC_APB2ENR
    "  ldr r1, [r0]\n"
    "  orr r1, r1, #0x4000\n"       // SYSCFGEN (the firmware has it on)
    "  str r1, [r0]\n"
    "  ldr r1, [r0]\n"
    "  ldr r0, =0x40013800\n"       // SYSCFG_MEMRMP
    "  ldr r1, [r0]\n"
    "  bic r1, r1, #7\n"
    "  orr r1, r1, #3\n"            // MEM_MODE 3: SRAM1 at 0x00000000
    "  str r1, [r0]\n"
    "  dsb\n"
    "  isb\n"
    "  ldr r2, =_sbss\n"
    "  ldr r3, =_ebss\n"
    "  movs r1, #0\n"
    "1:\n"
    "  cmp r2, r3\n"
    "  bcs 2f\n"
    "  str r1, [r2], #4\n"
    "  b 1b\n"
    "2:\n"
    "  bl __libc_init_array\n"
    "  bl main\n"
    "3:\n"
    "  b 3b\n"
    "  .ltorg\n"
    "  .size Reset_Handler, .-Reset_Handler\n");

// The loops: in .text.fast, linked at the SRAM1 alias, so that they run
// through it, and at their own address 0x20000000 above.
__asm__(
    "  .section .text.fast,\"ax\",%progbits\n"
    "  .syntax unified\n"
    "  .thumb\n"
    "  .p2align 2\n"
    "  .global pearls_fma_loop\n"
    "  .thumb_func\n"
    "  .type pearls_fma_loop, %function\n"
    "pearls_fma_loop:\n"            // s0: a, r0: n
    "  vmov.f32 s1, #0.5\n"
    "1:\n"
    "  vfma.f32 s0, s0, s1\n"
    "  vfma.f32 s0, s0, s1\n"
    "  vfma.f32 s0, s0, s1\n"
    "  vfma.f32 s0, s0, s1\n"
    "  subs r0, r0, #1\n"
    "  bne 1b\n"
    "  bx lr\n"
    "  .p2align 2\n"
    "  .global pearls_fma_independent_loop\n"
    "  .thumb_func\n"
    "  .type pearls_fma_independent_loop, %function\n"
    "pearls_fma_independent_loop:\n"
    "  vmov.f32 s1, #0.5\n"
    "  vmov.f32 s2, s0\n"
    "  vmov.f32 s3, s0\n"
    "  vmov.f32 s4, s0\n"
    "1:\n"
    "  vfma.f32 s0, s0, s1\n"
    "  vfma.f32 s2, s2, s1\n"
    "  vfma.f32 s3, s3, s1\n"
    "  vfma.f32 s4, s4, s1\n"
    "  subs r0, r0, #1\n"
    "  bne 1b\n"
    "  vadd.f32 s0, s0, s2\n"
    "  bx lr\n"
    "  .p2align 2\n"
    "  .global pearls_int_loop\n"
    "  .thumb_func\n"
    "  .type pearls_int_loop, %function\n"
    "pearls_int_loop:\n"            // r0: x, r1: n
    "  movs r2, #3\n"
    "  movs r3, #1\n"
    "1:\n"
    "  mla r0, r0, r2, r3\n"
    "  mla r0, r0, r2, r3\n"
    "  mla r0, r0, r2, r3\n"
    "  mla r0, r0, r2, r3\n"
    "  subs r1, r1, #1\n"
    "  bne 1b\n"
    "  bx lr\n"
    // Like compiled DSP code: a constant from the literal pool, loads and
    // stores to the stack (CCM), a table read from SRAM, a multiply-add.
    "  .p2align 2\n"
    "  .global pearls_mix_loop\n"
    "  .thumb_func\n"
    "  .type pearls_mix_loop, %function\n"
    "pearls_mix_loop:\n"            // r0: x, r1: n, r2: SRAM table
    "  push {r4, r5}\n"
    "  sub sp, sp, #16\n"
    "  movs r3, #0\n"
    "  movs r5, #0\n"
    "  str r3, [sp]\n"
    "  str r3, [sp, #4]\n"
    "1:\n"
    "  vldr s0, [sp]\n"
    "  vldr s1, [sp, #4]\n"
    "  ldr r3, 2f\n"
    "  vmov s2, r3\n"
    "  vfma.f32 s0, s1, s2\n"
    "  vstr s0, [sp]\n"
    "  ldr r4, [r2, r5, lsl #2]\n"
    "  adds r5, r5, #1\n"
    "  ands r5, r5, #63\n"
    "  add r0, r0, r4\n"
    "  str r0, [sp, #8]\n"
    "  subs r1, r1, #1\n"
    "  bne 1b\n"
    "  add sp, sp, #16\n"
    "  pop {r4, r5}\n"
    "  bx lr\n"
    "  .p2align 2\n"
    "2:\n"
    "  .word 0x3f000000\n"
    "  .text\n");
#endif
