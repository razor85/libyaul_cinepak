#include "base.h"

void satAssert(const char *filename, int line, const char *msg) {
  char *lwRam = (char *)LWRAM(120);

  cpu_intc_mask_set(15);
  
  if (msg != NULL) {
    sprintf(lwRam, "%s\n", msg);
    dbgio_printf("%s\n\n", msg);
  } else {
    sprintf(lwRam, "Assertion failed at %s:%d\n\n", filename, line);
    dbgio_printf("Assertion failed at %s:%d\n\n", filename, line);
  }

  dbgio_flush();
  vdp2_sync();
  vdp2_sync_wait();

  __asm__ volatile("sleep\n");

  while (true) {
  }
}