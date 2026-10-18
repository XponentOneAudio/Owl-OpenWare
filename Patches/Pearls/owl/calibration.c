// Pearls for the Lich: the calibration loops of PearlsPatch::Calibrate(), in
// assembly so that the compiler (with link time optimisation) neither
// changes nor moves them: in .text.fast, at the start of the patch, in
// SRAM1, so that they can also run through its alias at 0x00000000.

#ifdef __arm__
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
    "  .text\n");
#endif
