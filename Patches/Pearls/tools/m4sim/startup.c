#include <stdint.h>
#include <string.h>
extern int main(void);
extern void __libc_init_array(void);
extern void initialise_monitor_handles(void);
extern uint32_t __stack_top, __bss_start__, __bss_end__;
void Reset_Handler(void) {
  // Enable the FPU (CP10, CP11 full access).
  *(volatile uint32_t*)0xE000ED88 |= (0xFu << 20);
  __asm volatile("dsb; isb");
  memset(&__bss_start__, 0, (uint8_t*)&__bss_end__ - (uint8_t*)&__bss_start__);
  initialise_monitor_handles();
  __libc_init_array();
  extern void exit(int);
  exit(main());
}
static void Fault_Handler(void) {
  extern int printf(const char*, ...);
  printf("FAULT\n");
  extern void exit(int);
  exit(3);
}
__attribute__((section(".isr_vector"), used))
void* const vectors[16] = {
  &__stack_top, Reset_Handler, Fault_Handler, Fault_Handler, Fault_Handler,
  Fault_Handler, Fault_Handler, 0, 0, 0, 0, Fault_Handler, Fault_Handler, 0,
  Fault_Handler, Fault_Handler,
};
void _init(void){} void _fini(void){}
