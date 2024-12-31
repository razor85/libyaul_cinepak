#include "timer.h"
#include "memory.h"


// Static variables declaration:
volatile uint32_t SystemTime::ms SKIP_CACHE;
volatile uint32_t SystemTime::accumulator;

#define TIMER_INTERVAL CPU_FRT_NTSC_320_8_COUNT_1MS

void oviHandler() {
  // TODO: decide what to do here.
}

void SystemTime::initialize() {
  Memory::uncached(ms) = 0;
  accumulator = 0;

  cpu_frt_init(CPU_FRT_CLOCK_DIV_8);
  cpu_frt_count_set(0);
  cpu_frt_ovi_set(oviHandler);
  cpu_frt_count_set(0);
  cpu_frt_oca_set(TIMER_INTERVAL, SystemTime::timerAutomaticUpdate);
  cpu_frt_interrupt_priority_set(8);
}

void SystemTime::timerAutomaticUpdate() {
  uint16_t frtCount{ cpu_frt_count_get() };
  while (frtCount > TIMER_INTERVAL) {
    ms++;
    frtCount -= TIMER_INTERVAL;
  }

  if (frtCount > 0) {
    accumulator += frtCount;
    while (accumulator >= TIMER_INTERVAL) {
      ms++;
      accumulator -= TIMER_INTERVAL;
    }
  
    DEBUG_REQUIRE_LT(accumulator, TIMER_INTERVAL);
  }
    
  cpu_frt_count_set(0);
}

MeasureTime::MeasureTime(uint32_t &inVar) : var(&inVar) {
  time = SystemTime::now();
}

MeasureTime::~MeasureTime() {
  *var += SystemTime::now() - time;
}

