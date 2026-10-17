// Pearls for the Lich: system call stubs and the heap. newlib's abort() (pulled in by
// libgcc's unwinder) needs these; patches never exit.

void _exit(int status) {
  (void)status;
  for (;;) {
  }
}

int _kill(int pid, int sig) {
  (void)pid;
  (void)sig;
  return -1;
}

int _getpid(void) {
  return 1;
}

// malloc and free on the OWL heap (internal RAM first, then SDRAM), as
// operator new does in OwlProgram.
#include <stddef.h>
void* pvPortMalloc(size_t size);
void vPortFree(void* p);

void* malloc(size_t size) {
  return pvPortMalloc(size);
}

void free(void* p) {
  if (p) {
    vPortFree(p);
  }
}
