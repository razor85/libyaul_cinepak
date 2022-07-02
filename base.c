#include "base.h"

void satAssert(const char *filename, int line, const char *msg) {
  const uint32_t debug1 = 0xDEADC0DE;
  const uint32_t debug2 = 0x8BADF00D;
  __asm__ volatile (
    "mov %[debug1], r12\n"
    "mov %[debug2], r13\n"
    :
    : [debug1] "r" (debug1),
      [debug2] "r" (debug2)
  );

  char *lwRam = (char *)LWRAM(8);
  
  if (msg != NULL) {
    sprintf(lwRam, "%s\n", msg);
    dbgio_printf("%s\n\n", msg);
  } else {
    sprintf(lwRam, "Assertion failed at %s:%d\n\n", filename, line);
    dbgio_printf("Assertion failed at %s:%d\n\n", filename, line);
  }
    
  dbgio_flush();

  while (1) {
    vdp2_sync();
    vdp2_sync_wait();
  }
}