#pragma once

#include <gamemath.h>
#include <stdint.h>
#include <yaul.h>

#ifndef SKIP_CACHE
#  define SKIP_CACHE __section(".uncached")
#endif

#ifndef FORCE_INLINE
#  define FORCE_INLINE __attribute__((always_inline)) inline
#endif

#ifndef NO_INLINE
#  define NO_INLINE __attribute__((noinline))
#endif

#define DEBUG_FUNCTIONS_ON

#ifdef min
#  undef min
#endif

#ifdef max
#  undef max
#endif

#ifdef abs
#  undef abs
#endif

enum TransferCommands { TC_REQUEST_FILE = 0, TC_REQUEST_FILE_SIZE, TC_MESSAGE, TC_INVALID = 0xFF };

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

class Console {
public:
  static constexpr bool HasUsbConnection{false};

  static void initialize() {
    dbgio_dev_default_init(DBGIO_DEV_VDP2);
    dbgio_dev_font_load();
  }

  static void clear() { dbgio_printf("[1;1H[2J"); }

  static void flush() { dbgio_flush(); }

  // Print to usb_dev_tool
  template <typename... Args>
  static void printToUsb(const char *msg, const Args &...args) {
    if constexpr (HasUsbConnection) {
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

  template <typename... Args>
  static void printf(const char *msg, const Args &...args) {
    dbgio_printf(msg, args...);
  }

  template <typename... Args>
  static void log(const char *msg, const Args &...args) {
    printToUsb(msg, args...);
    dbgio_printf(msg, args...);
  }

  template <typename... Args>
  static void printf_flush(const char *msg, const Args &...args) {
    dbgio_printf(msg, args...);
    dbgio_flush();
  }

  template <typename... Args>
  static void log_flush(const char *msg, const Args &...args) {
    printToUsb(msg, args...);
    dbgio_printf(msg, args...);
    dbgio_flush();
  }

  // Printf to mednafen.
  static void mPrint(const char *msg) {
#ifdef DEBUG_FUNCTIONS_ON
    volatile static char *debugAddress{reinterpret_cast<char *>(0x22100001)};

    const char *msgPtr{msg};
    while (*msgPtr != '\0') {
      *debugAddress = *msgPtr;
      msgPtr++;
    }
#endif
  }
};

template <typename T>
static void swap(T &a, T &b) noexcept {
  T temp = std::move(a);
  a = std::move(b);
  b = std::move(temp);
}

template <typename TA, typename TB>
struct Pair {
  TA _0;
  TB _1;
};

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

template <typename T>
class Optional {
private:
  void __destroy() {
    if (m_hasValue) {
      realValue.~T();
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