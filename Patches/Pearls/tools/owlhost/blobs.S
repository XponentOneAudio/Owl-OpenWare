  .section .rodata.blobs, "a"
  .balign 8
  .global patch_bin, patch_bin_end, waves_bin, waves_bin_end
patch_bin: .incbin PATCH_BIN
patch_bin_end:
  .balign 8
waves_bin: .incbin WAVES_BIN
waves_bin_end:
