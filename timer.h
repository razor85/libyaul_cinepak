#pragma once

#include "base.h"
#include "memory.h"


class SystemTime {
public:
  static void initialize();
  static void timerAutomaticUpdate();

#ifdef SATURN_SIMULATOR
  static FORCE_INLINE uint32_t now() { return SDL_GetTicks(); }
#else
  static FORCE_INLINE uint32_t now() { return Memory::uncached(ms); }
#endif

private:
  static volatile uint32_t ms SKIP_CACHE;
  static volatile uint32_t accumulator;
};


class Timer {
public:
  Timer() { reset(); }

  void reset() {
    startTime = SystemTime::now();
    isRunning = true;
  }

  void end() {
    const uint32_t timeDiff{ SystemTime::now() - startTime };
    startTime = timeDiff;
    isRunning = false;
  }

  uint32_t count() {
    if (isRunning) {
      return SystemTime::now() - startTime;
    } else {
      return startTime;
    }
  }

private:
  uint32_t startTime{ 0 };
  bool isRunning{ true };
};

class MeasureTime {
public:
  MeasureTime(uint32_t& var);
  ~MeasureTime();

private:
  volatile uint32_t time;
  uint32_t* var;
};

