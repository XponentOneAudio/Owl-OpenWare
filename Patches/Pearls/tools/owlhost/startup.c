#include <stdint.h>
#include <string.h>
extern int main(void);
extern void __libc_init_array(void);
extern void initialise_monitor_handles(void);
extern uint32_t __stack_top, __bss_start__, __bss_end__;
void Reset_Handler(void) {
  *(volatile uint32_t*)0xE000ED88 |= (0xFu << 20);  /* FPU on */
  *(volatile uint32_t*)0xE000ED24 |= 7u << 16;  /* separate fault handlers */
  *(volatile uint32_t*)0xE000ED14 |= 1u << 4;  /* trap divide by zero */
  __asm volatile("dsb; isb");
  memset(&__bss_start__, 0, (uint8_t*)&__bss_end__ - (uint8_t*)&__bss_start__);
  initialise_monitor_handles();
  __libc_init_array();
  extern void exit(int);
  exit(main());
}
__attribute__((naked)) static void Fault_Handler(void) {
  __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n"
                 "mov r1, lr\n b fault_report\n");
}
__attribute__((section(".isr_vector"), used))
void* const vectors[16] = {
  &__stack_top, Reset_Handler, Fault_Handler, Fault_Handler, Fault_Handler,
  Fault_Handler, Fault_Handler, 0, 0, 0, 0, Fault_Handler, Fault_Handler, 0,
  Fault_Handler, Fault_Handler,
};
void _init(void){} void _fini(void){}
