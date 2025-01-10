#pragma once

#include <gamemath.h>
#include <stdint.h>
#include <yaul.h>

// Somehow this is undefined. I guess it had to do with stdbool on C vs. C++.
extern "C" {
extern void dbgio_display_set(bool);
}

// Standard stuff so we get std::move and others.
namespace std {

using nullptr_t = decltype(nullptr);

template <class T>
struct remove_reference;

template <class T>
using remove_reference_t = typename remove_reference<T>::type;

template <class T>
constexpr remove_reference_t<T> &&move(T &&t) noexcept;

template <class T>
struct is_trivially_destructible;

} // namespace std

#ifndef SKIP_CACHE
#  define SKIP_CACHE __section(".uncached")
#endif

#ifndef FORCE_INLINE
#  define FORCE_INLINE __attribute__((always_inline)) inline
#endif

#ifndef NO_INLINE
#  define NO_INLINE __attribute__((noinline))
#endif

// #define DEBUG_FUNCTIONS_ON

#ifdef min
#  undef min
#endif

#ifdef max
#  undef max
#endif

#ifdef abs
#  undef abs
#endif

enum TransferCommands {
  TC_REQUEST_FILE = 0,
  TC_REQUEST_FILE_SIZE,
  TC_MESSAGE,
  TC_INVALID = 0xFF,
};

#define KRONOS_DEBUG_32M_LOG_ADDRESS 0x24001000
#define KRONOS_DEBUG_32M_CMD_PUSH    0x24001010
#define KRONOS_DEBUG_32M_CMD_POP     0x24001014
#define KRONOS_DEBUG_32M_CMD_ADDRESS 0x24001020

class Kronos {
public:
  enum Commands : uint32_t {
    ProfilerEnable = 0x00000010,
    ProfilerDisable = 0x00000011,
  };

  template <typename T>
  static void write(T val) {
    *reinterpret_cast<volatile T *>(KRONOS_DEBUG_32M_CMD_ADDRESS) = val;
  }

  static void writeCommand(Commands command) { write<uint32_t>(static_cast<uint32_t>(command)); }

  template <typename... Args>
  static void logPrintf(const char *msg, const Args &...args) {
    char logBuffer[1024];
    snprintf(logBuffer, 1024, msg, args...);

    char *ptr = logBuffer;
    while (*ptr != 0) {
      *reinterpret_cast<volatile uint8_t *>(KRONOS_DEBUG_32M_LOG_ADDRESS) = *ptr++;
    }
  }
};

class Mednafen {
public:
  template <typename... Args>
  static void logPrintf(const char *msg, const Args &...args) {
    char logBuffer[1024];
    snprintf(logBuffer, 1024, msg, args...);

    char *ptr = logBuffer;
    while (*ptr != 0) {
      *reinterpret_cast<volatile uint8_t *>(0x22100001) = *ptr++;
    }
  }
};

class Console {
public:
  static constexpr bool HasUsbConnection{false};
  static bool Initialized;

  static void initialize() {
    dbgio_init();
    dbgio_dev_default_init(DBGIO_DEV_VDP2);
    dbgio_dev_font_load();
    Initialized = true;
  }

  static void reinitialize() {
    // dbgio_init();
    // dbgio_dev_default_init(DBGIO_DEV_VDP2);
    // dbgio_dev_font_load();
    Initialized = true;
  }

  static void deinitialize() {
    // TODO:
    // dbgio_dev_deinit();
    Initialized = false;
  }

  static void clear() {
    if (Initialized) {
      dbgio_printf("[1;1H[2J");
    }
  }

  static void flush() {
    if (Initialized) {
      dbgio_flush();
    }
  }

  // Print to usb_dev_tool
  template <typename... Args>
  static void printToUsb(const char *msg, const Args &...args) {
    if constexpr (HasUsbConnection) {
      if (Initialized) {
        char tmpBuffer[128] = {};
        ::snprintf(tmpBuffer, 128, msg, args...);

        const uint32_t msgLen = strlen(tmpBuffer);

        usb_cart_byte_send(static_cast<uint8_t>(TC_MESSAGE));
        usb_cart_long_send(msgLen);
        for (uint32_t i = 0; i < msgLen; ++i) {
          usb_cart_byte_send(tmpBuffer[i]);
        }
      }
    }
  }

  template <typename... Args>
  static void printf(const char *msg, const Args &...args) {
    if (Initialized) {
      dbgio_printf(msg, args...);
    }
  }

  template <typename... Args>
  static void log(const char *msg, const Args &...args) {
    if (Initialized) {
      printToUsb(msg, args...);
      dbgio_printf(msg, args...);
    }
  }

  template <typename... Args>
  static void printf_flush(const char *msg, const Args &...args) {
    if (Initialized) {
      dbgio_printf(msg, args...);
      dbgio_flush();
    }
  }

  template <typename... Args>
  static void log_flush(const char *msg, const Args &...args) {
    if (Initialized) {
      printToUsb(msg, args...);
      dbgio_printf(msg, args...);
      dbgio_flush();
    }
  }
};

template <typename T>
static void swap(T &a, T &b) noexcept {
  T temp = std::move(a);
  a = std::move(b);
  b = std::move(temp);
}

template <typename T, typename ValueT>
T clamp(ValueT v, T min, T max) {
  if (v < min) {
    return static_cast<T>(min);
  } else if (v > max) {
    return static_cast<T>(max);
  } else {
    return static_cast<T>(v);
  }
}

template <typename TA, typename TB>
struct Pair {
  TA _0;
  TB _1;
};

template <typename T, bool HasDestructor = true>
class alignas(alignof(T)) Optional {
private:
  void __destroy() {
    if constexpr (HasDestructor) {
      if (m_hasValue) {
        realValue.~T();
      }
    }
  }

public:
  Optional()
      : dummy(0)
      , m_hasValue(false) {}

  Optional(const T &value)
      : realValue(value)
      , m_hasValue(true) {}

  Optional(T &&value)
      : realValue(std::move(value))
      , m_hasValue(true) {}

  ~Optional() { __destroy(); }

  void reset() {
    __destroy();

    dummy = 0;
    m_hasValue = false;
  }

  Optional &operator=(std::nullptr_t) {
    __destroy();

    dummy = 0;
    m_hasValue = false;
    return *this;
  }

  Optional &operator=(const T &value) {
    __destroy();

    realValue = value;
    m_hasValue = true;
    return *this;
  }

  T &operator*() noexcept {
    assert(m_hasValue);
    return realValue;
  }

  const T &operator*() const noexcept {
    assert(m_hasValue);
    return realValue;
  }

  T &get() noexcept { return *this; }

  const T &get() const noexcept { return *this; }

  [[nodiscard]] bool hasValue() { return m_hasValue; }

  explicit operator bool() const noexcept { return m_hasValue; }

private:
  union {
    char dummy;
    T realValue;
  };

  bool m_hasValue{false};
};
