#include "timer.h"
#include "memory.h"

// Static variables declaration:
volatile uint32_t SystemTime::ms SKIP_CACHE;
volatile uint32_t SystemTime::accumulator;

enum class TimerIntervalResolution {
  Interval8 = 0,
  Interval32,
  Interval128,
};

constexpr TimerIntervalResolution TimerResolution{TimerIntervalResolution::Interval8};
constexpr uint32_t getFrtInitValue() {
  if constexpr (TimerResolution == TimerIntervalResolution::Interval8) {
    return CPU_FRT_CLOCK_DIV_8;
  } else if constexpr (TimerResolution == TimerIntervalResolution::Interval32) {
    return CPU_FRT_CLOCK_DIV_32;
  } else if constexpr (TimerResolution == TimerIntervalResolution::Interval128) {
    return CPU_FRT_CLOCK_DIV_128;
  }
}

constexpr uint32_t getTimerInterval() {
  if constexpr (TimerResolution == TimerIntervalResolution::Interval8) {
    return CPU_FRT_NTSC_320_8_COUNT_1MS;
  } else if constexpr (TimerResolution == TimerIntervalResolution::Interval32) {
    return CPU_FRT_NTSC_320_32_COUNT_1MS;
  } else if constexpr (TimerResolution == TimerIntervalResolution::Interval128) {
    return CPU_FRT_NTSC_320_128_COUNT_1MS;
  }
}

void SystemTime::initialize() {
  Memory::uncached(ms) = 0;
  accumulator = 0;

  cpu_frt_init(getFrtInitValue());
  cpu_frt_count_set(0);
  cpu_frt_ovi_set(nullptr);
  cpu_frt_count_set(0);
  cpu_frt_oca_set(getTimerInterval(), SystemTime::timerAutomaticUpdate);
  cpu_frt_interrupt_priority_set(15);
}

void SystemTime::timerAutomaticUpdate() {
  accumulator += cpu_frt_count_get();
  while (accumulator > getTimerInterval()) {
    ++ms;
    accumulator -= getTimerInterval();
  }

  cpu_frt_count_set(0);
}

MeasureTime::MeasureTime(uint32_t &inVar)
    : var(&inVar) {
  time = SystemTime::now();
}

MeasureTime::~MeasureTime() { *var += SystemTime::now() - time; }
