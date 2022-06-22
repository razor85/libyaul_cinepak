#include "base.h"

void satAssert(const char *filename, int line, const char *msg) {
  __asm__ volatile (
    "mov #4294967295, r12\n"
    "mov #4294967295, r13\n"
  );

  if (msg != NULL) {
    dbgio_printf("%s\n\n", msg);
  } else {
    dbgio_printf("Assertion failed at %s:%d\n\n", filename, line);
  }

  while (1) {
    dbgio_flush();
    vdp2_sync();
    vdp2_sync_wait();
  }
}