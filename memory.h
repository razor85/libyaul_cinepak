#pragma once

#include "assert.h"
#include "base.h"
#include "tlsf/tlsf.h"

#include <dbgio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <gamemath/fix16.h>

#ifdef DEBUG_FUNCTIONS_ON
#define HAS_DEBUG_REQUIRE_FUNCTIONS
#endif 

#ifdef HAS_DEBUG_REQUIRE_FUNCTIONS
#define __STRINGIFY(x) #x
#define __TOSTRING(x) __STRINGIFY(x)
#define REQUIRE_BODY_OP(A, B, _OP_)                                            \
  do {                                                                         \
    if (!((A)_OP_(B))) {                                                       \
      StringStream str(DebugBuffer::buffer(), 256);                            \
      str << "Assertion at " __FILE__ ":" __TOSTRING(__LINE__) " ";            \
      str << (A);                                                              \
      str << #_OP_;                                                            \
      str << (B);                                                              \
      str << " failed";                                                        \
      Memory::libAssert(__FILE__, __LINE__, str.buffer());                     \
    }                                                                          \
  } while (false)

#define DEBUG_REQUIRE_EQ(A, B) REQUIRE_BODY_OP(A, B, ==)
#define DEBUG_REQUIRE_NE(A, B) REQUIRE_BODY_OP(A, B, !=)
#define DEBUG_REQUIRE_LE(A, B) REQUIRE_BODY_OP(A, B, <=)
#define DEBUG_REQUIRE_LT(A, B) REQUIRE_BODY_OP(A, B, <)
#define DEBUG_REQUIRE_GE(A, B) REQUIRE_BODY_OP(A, B, >=)
#define DEBUG_REQUIRE_GT(A, B) REQUIRE_BODY_OP(A, B, >)
#define DEBUG_REQUIRE(A)                                                       \
  do {                                                                         \
    if (!(A)) {                                                                \
      Memory::libAssert(__FILE__, __LINE__,                                    \
        "Error (" #A " is false) at " __FILE__ ":" __TOSTRING(__LINE__));      \
    }                                                                          \
  } while (false)
#define DEBUG_ASSERT_FALSE() DEBUG_REQUIRE(false)

#else // HAS_DEBUG_REQUIRE_FUNCTIONS
#define DEBUG_REQUIRE_EQ(A, B)
#define DEBUG_REQUIRE_NE(A, B)
#define DEBUG_REQUIRE_LE(A, B)
#define DEBUG_REQUIRE_LT(A, B) 
#define DEBUG_REQUIRE_GE(A, B)
#define DEBUG_REQUIRE_GT(A, B)
#define DEBUG_REQUIRE(A) (A)
#define DEBUG_ASSERT_FALSE() assert(false)
#endif // HAS_DEBUG_REQUIRE_FUNCTIONS

#define VDP_INFLOOP()                                                          \
  do {                                                                         \
    dbgio_flush();                                                             \
    vdp2_sync();                                                               \
    vdp2_sync_wait();                                                          \
  } while (true)

class StringStream {
public:
  StringStream(char *buffer, uint32_t size)
      : mBuffer(buffer)
      , mSize(0)
      , mMaxSize(size) {}

  StringStream& operator<<(bool n);
  StringStream& operator<<(unsigned char n);
  StringStream& operator<<(char n);
  StringStream& operator<<(unsigned short n);
  StringStream& operator<<(short n);
  StringStream& operator<<(unsigned int n);
  StringStream& operator<<(int n);
  StringStream& operator<<(long unsigned int n);
  StringStream& operator<<(long int n);
  StringStream& operator<<(const void* ptr);
  StringStream& operator<<(nullptr_t ptr);
  StringStream& operator<<(yaul::fix16 n);
  StringStream& operator<<(const char* str);
  
  const char* buffer() const;

private:
  char mTmpBuffer[64];
  void addToBuffer(const char* input, uint32_t len);

  char* mBuffer{ nullptr };
  uint32_t mSize{ 0 };
  uint32_t mMaxSize{ 256 };
};

class DebugBuffer {
public:
  static char* buffer() { return msg;  }
  
  static bool isDebuggingModels() { return false; }
  static bool isDebuggingImages() { return false; }
  static bool isDebuggingMemory() { return false; }
  static bool isDebuggingFilesystem() { return false; }
  static bool isDebuggingCdBlock() { return false; }
  static bool isDebugging() { return false; }

  static void print();
  static void printInLoop();
  static void print(const char*);
  static void printv(const char *format, ...);

private:
  static char msg[ 256 ];
};


namespace Memory {


template <typename T>
T *uncachedPtr(volatile T *ptr) {
  constexpr volatile uint32_t skipCacheAddr = 0x20000000;
  volatile uint32_t ptrAddr = reinterpret_cast<uint32_t>(ptr);
  ptrAddr |= skipCacheAddr;

  return reinterpret_cast<T *>(ptrAddr);
}
  
template <typename T>
T &uncached(volatile T &ptr) {
  return *uncachedPtr<T>(&ptr);
}

template <typename T>
T uncachedPostIncrement(volatile T &ptr) {
  volatile T *uncachedValuePtr = uncachedPtr<T>(&ptr);
  volatile const T tmpValue = *uncachedValuePtr;

  // We need to update BOTH values to prevent the local copy for being outdated.
  *uncachedValuePtr = ptr = tmpValue + 1;
  return tmpValue;
}

template <typename T>
class Uncached {
public:
  T& value;

  Uncached(T& v) : value(v) {}
  ~Uncached() {}

  Uncached(const Uncached<T>& value) = delete;
  Uncached(Uncached<T>&& value) = delete;
  Uncached<T> &operator=(Uncached<T> &&) = delete;
  Uncached<T> &operator=(const Uncached<T>&) = delete;

  T &operator()() { return Memory::uncached(value); }
  const T &operator()() const { return Memory::uncached(value); }

  Uncached<T> &operator=(T &&other) {
    Memory::uncached(value) = other.value;
    value = other.value;
    return *this;
  }

  Uncached<T> &operator=(const T &other) {
    Memory::uncached(value) = other.value;
    value = other.value;
    return *this;
  }
};

template <typename T>
void swap(T& a, T& b) noexcept {
  T tmp = a;
  a = b;
  b = tmp;
}

extern uint32_t getOffsetInCmdRAM(uint32_t offset);
extern uint32_t getOffsetInTextureRAM(uint32_t offset);
extern uint32_t getOffsetInGouraudRAM(uint32_t offset);

template <typename T>
extern T* getPointerToTextureRAM(uint32_t offset) {
  return reinterpret_cast<T *>(VDP1_VRAM(getOffsetInTextureRAM(offset)));
}

template <typename T>
T *getPointerToDRAM(uint32_t offset) {
  uint8_t *dramAddress{ reinterpret_cast<uint8_t *>(0x22400000UL) };
  return reinterpret_cast<T *>(&dramAddress[offset]);
}

template <typename T>
T* getPointerToGouraudRAM(uint32_t offset) {
  return reinterpret_cast<T *>(VDP1_VRAM(getOffsetInGouraudRAM(offset)));
}

extern void libAssert(const char* file, int line, const char* msg = nullptr) __noreturn;

extern void runTests();

static void clear32(void* dst, uint32_t size) {
  volatile uint32_t* dstPtr = static_cast<uint32_t*>(dst);
  volatile const uint32_t size32 = size >> 2;
  for (volatile uint32_t i = 0; i < size32; ++i)
    dstPtr[ i ] = 0;
}

static void copy16(void* dst, const void* src, uint32_t size) {
  volatile uint16_t* dstPtr = static_cast<uint16_t*>(dst);
  volatile const uint16_t* srcPtr = static_cast<const uint16_t*>(src);

  const uint32_t size16{ size >> 1 };
  for (volatile uint32_t i = 0; i < size16; ++i)
    dstPtr[ i ] = srcPtr[ i ];
}

static void copy32(void* dst, const void* src, uint32_t size) {
  volatile uint32_t* dstPtr = static_cast<uint32_t*>(dst);
  volatile const uint32_t* srcPtr = static_cast<const uint32_t*>(src);

  const uint32_t size32{ size >> 2 };
  for (volatile uint32_t i = 0; i < size32; ++i)
    dstPtr[ i ] = srcPtr[ i ];
}

class Low {
public:
  static void initialize() {
    tlsfHandle = user_tlsf_create_with_pool(reinterpret_cast<void *>(LWRAM(0)),
      static_cast<size_t>(1024 * 1024));
  }

  static void runTests() {
    DEBUG_REQUIRE_EQ(user_tlsf_check(tlsfHandle), 0);
  }

  static void* alloc(uint32_t size) {
    return user_tlsf_malloc(tlsfHandle, size);
  }

  template <typename T>
  static T* alloc(uint32_t size) {
    return static_cast<T *>(alloc(size));
  }

  template <typename T>
  static void alloc(uint32_t size, T *&destination) {
    destination = static_cast<T *>(alloc(size));
  }
  
  static void* memalign(uint32_t align, uint32_t size) {
    return user_tlsf_memalign(tlsfHandle, align, size);
  }

  static void* realloc(void* ptr, uint32_t size) {
    return user_tlsf_realloc(tlsfHandle, ptr, size);
  }

  static void free(void* ptr) {
    return user_tlsf_free(tlsfHandle, ptr);
  }
  
  static uint32_t getUsedBytes(bool calculate = true) {
    if (calculate) {
      user_tlsf_walk_pool(tlsfHandle, memWalker, &usedMemory);
    }

    return usedMemory;
  }

  static uint32_t getFreeBytes(bool calculate = true) {
    return getTotalBytes() - getUsedBytes(calculate);
  }

  static uint32_t getTotalBytes() {
    return 1024 * 1024;
  }

private:
  static void memWalker(void *, size_t size, int used, void *userData) {
    uint32_t &count{ *static_cast<uint32_t *>(userData) };
    if (used) {
      count += size;
    }
  }

  static user_tlsf_t tlsfHandle;
  static uint32_t usedMemory;
};

class High {
public:
  High() = default;

  static void initialize() {
    tlsfHandle = user_tlsf_create_with_pool(data, maxSize);
  }

  static void *alloc(uint32_t size) {
    return user_tlsf_malloc(tlsfHandle, size);
  }

  static void free(void* ptr) {
    return user_tlsf_free(tlsfHandle, ptr);
  }

  static void *memalign(uint32_t align, uint32_t size) {
    return user_tlsf_memalign(tlsfHandle, align, size);
  }

  template <typename T>
  static T *alloc(uint32_t size) {
    return static_cast<T *>(alloc(size));
  }
  
  template <typename T>
  static T *alloc() {
    return static_cast<T *>(alloc(sizeof(T)));
  }

  template <typename T, uint32_t Number>
  static T *alloc() {
    return static_cast<T *>(alloc(sizeof(T) * Number));
  }

  template <typename T>
  static T *memalign(uint32_t align, uint32_t size) {
    return static_cast<T *>(memalign(align, size));
  }

  static void freeAll() {
    initialize();
  }

  static uint32_t getUsedBytes(bool calculate = true) {
    if (calculate) {
      user_tlsf_walk_pool(tlsfHandle, memWalker, &usedMemory);
    }

    return usedMemory;
  }

  static uint32_t getFreeBytes(bool calculate = true) {
    return getTotalBytes() - getUsedBytes(calculate);
  }

  static uint32_t getTotalBytes() {
    return maxSize;
  }

private:
  // constexpr static uint32_t maxSize{ 1024 * 530 };
  constexpr static uint32_t maxSize{ 1024 * 500 };
  static uint8_t data[maxSize];

  static void memWalker(void *, size_t size, int used, void *userData) {
    uint32_t &count{ *static_cast<uint32_t *>(userData) };
    if (used) {
      count += size;
    }
  }

  static user_tlsf_t tlsfHandle;
  static uint32_t usedMemory;
};

class DRAM {
public:
  static void initialize() {
    const dram_cart_id_t cartId{ dram_cart_id_get() };
    const uint32_t cartSize{ cartId == DRAM_CART_ID_INVALID ? 0UL :
      cartId == DRAM_CART_ID_1MIB ? 1UL : 4UL };

    if (cartSize > 0) {
      totalMemory = cartSize * 1024 * 1024;
      tlsfHandle = user_tlsf_create_with_pool(dram_cart_area_get(),
        totalMemory);
    } else {
      totalMemory = 0;
    }
  }

  static void runTests() {
    DEBUG_REQUIRE_EQ(user_tlsf_check(tlsfHandle), 0);
  }

  static void* alloc(uint32_t size) {
    return user_tlsf_malloc(tlsfHandle, size);
  }

  template <typename T>
  static T* alloc(uint32_t size) {
    return static_cast<T *>(alloc(size));
  }

  template <typename T>
  static void alloc(uint32_t size, T *&destination) {
    destination = static_cast<T *>(alloc(size));
  }
  
  template <typename T>
  static T *memalign(uint32_t align, uint32_t size) {
    return static_cast<T *>(memalign(align, size));
  }
  
  static void* memalign(uint32_t align, uint32_t size) {
    return user_tlsf_memalign(tlsfHandle, align, size);
  }

  static void* realloc(void* ptr, uint32_t size) {
    return user_tlsf_realloc(tlsfHandle, ptr, size);
  }

  static void free(void* ptr) {
    return user_tlsf_free(tlsfHandle, ptr);
  }
  
  static uint32_t getUsedBytes(bool calculate = true) {
    if (calculate) {
      user_tlsf_walk_pool(tlsfHandle, memWalker, &usedMemory);
    }

    return usedMemory;
  }

  static uint32_t getFreeBytes(bool calculate = true) {
    return getTotalBytes() - getUsedBytes(calculate);
  }

  static uint32_t getTotalBytes() {
    return totalMemory;
  }

private:
  static void memWalker(void *, size_t size, int used, void *userData) {
    uint32_t &count{ *static_cast<uint32_t *>(userData) };
    if (used) {
      count += size;
    }
  }

  static user_tlsf_t tlsfHandle;
  static uint32_t usedMemory;
  static uint32_t totalMemory;
};

inline void initializeAll() {
  Memory::Low::initialize();
  Memory::High::initialize();
  Memory::DRAM::initialize();
}

} // namespace Memory


/**
 * Pre-allocated (on HWRAM) arrays that can have elements inserted/removed.
 *
 * By default the number of elements in the array is the size of array, use
 * clear() to remove all elements.
 */
template <typename T, uint32_t SizeV>
class StaticArray {
public:
  StaticArray() = default;
  StaticArray(StaticArray&) = delete;
  StaticArray(StaticArray&&) = delete;
  StaticArray(const StaticArray&) = delete;
  StaticArray& operator=(StaticArray&) = delete;
  StaticArray& operator=(StaticArray&&) = delete;
  StaticArray& operator=(const StaticArray&) = delete;

  ~StaticArray() {}

  void clear() { count = 0; }

  void zeroData() { memset(dataPtr, 0, sizeof(T) * count); }

  void zeroAllData() { memset(dataPtr, 0, sizeof(T) * SizeV); }

  uint32_t num() const { return count; }
  
  uint32_t uncachedNum() const { return Memory::uncached(count); }
  
  uint32_t size() const { return SizeV; }

  uint32_t freeSpace() const { return SizeV - count; }

  T* lastPtr() const { return &dataPtr[count]; }

  T &add() {
    DEBUG_REQUIRE_LT(count + 1, SizeV);
    return dataPtr[count++];
  }

  T &uncachedAdd() {
    volatile const uint32_t posBeforeInc = Memory::uncachedPostIncrement(count);
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(posBeforeInc + 1, SizeV);
    return dataPtr[posBeforeInc];
  }

  Pair<T&, uint32_t> uncachedAddPair() {
    volatile const uint32_t posBeforeInc = Memory::uncachedPostIncrement(count);

    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(posBeforeInc + 1, SizeV);
    return { dataPtr[posBeforeInc], posBeforeInc };
  }

  void add(const T& v) { 
    DEBUG_REQUIRE_LT(count + 1, SizeV);
    dataPtr[count++] = v;
  }

  void add(T&& v) { 
    DEBUG_REQUIRE_LT(count + 1, SizeV);
    dataPtr[count++] = v;
  }
  
  T* addMany(uint32_t numItems) {
    DEBUG_REQUIRE_LT(count + numItems, SizeV);

    T *ptr{ &dataPtr[count] };
    count += numItems;
    return ptr;
  }
  
  T* uncachedAddMany(uint32_t numItems) {
    DEBUG_REQUIRE_LT(count + numItems, SizeV);
    volatile const uint32_t posBeforeInc = Memory::uncached(count);

    T *ptr{ &dataPtr[count] };
    count = posBeforeInc + numItems;
    return ptr;
  }
  
  Pair<T*, uint32_t> uncachedAddManyPair(uint32_t numItems) {
    DEBUG_REQUIRE_LT(count + numItems, SizeV);
    volatile const uint32_t posBeforeInc = Memory::uncached(count);
    
    T *ptr{ &dataPtr[posBeforeInc] };
    Memory::uncached(count) = posBeforeInc + numItems;

    return { ptr, posBeforeInc };
  }
  
  Pair<T*, uint32_t> addManyPair(uint32_t numItems) {
    DEBUG_REQUIRE_LT(count + numItems, SizeV);
    volatile const uint32_t posBeforeInc = Memory::uncached(count);

    T *ptr{ &dataPtr[posBeforeInc] };
    Memory::uncached(count) = posBeforeInc + numItems;

    return { ptr, posBeforeInc };
  }
  
  void setOffset(uint32_t newOffset) {
    DEBUG_REQUIRE_LT(newOffset, SizeV);
    count = newOffset;
  }

  T& at(uint32_t index) {
    DEBUG_REQUIRE_LT(index, count);
    return dataPtr[index];
  }

  const T& at(uint32_t index) const {
    DEBUG_REQUIRE_LT(index, count);
    return dataPtr[index];
  }
  
  void popItems(uint32_t numItems) {
    DEBUG_REQUIRE_GE(count, numItems);
    count -= numItems;
  }
  
  T* data() { return dataPtr; }
  const T* data() const { return dataPtr; }

  T& operator [](uint32_t index) { return dataPtr[index]; }
  const T& operator [](uint32_t index) const { return dataPtr[index]; }

private:
  T dataPtr[SizeV] __packed __aligned(alignof(T));
  volatile uint32_t count{ SizeV };
};

/**
 * Dynamic array allocated on HIGH RAM.
 */
template <typename T>
class Array {
public:
  Array() = default;
  Array(Array&) = delete;
  Array(Array&&) = delete;
  Array(const Array&) = delete;
  Array& operator=(Array&) = delete;
  Array& operator=(Array&&) = delete;
  Array &operator=(const Array &) = delete;

  ~Array() {
    DEBUG_ASSERT_FALSE();
    if (dataPtr != nullptr) {
      if (allocationArea == AllocationArea::HWRAM) {
        Memory::High::free(dataPtr);
      } else if (allocationArea == AllocationArea::DRAM) {
        Memory::DRAM::free(dataPtr);
      }
    }
  }
  
  explicit Array(T *ptr, uint32_t inNumItems, uint32_t inCapacity) {
    pointToPreallocatedArea(ptr, inNumItems, inCapacity);
  }

  void pointToPreallocatedArea(Array<T> &inArray, uint32_t inNumItems,
    bool construct = false) {

    DEBUG_REQUIRE_EQ(dataPtr, nullptr);

    T* ptr{ inArray.addMany(inNumItems) };
    DEBUG_REQUIRE_NE(ptr, nullptr);

    count = inNumItems;
    totalCapacity = inNumItems;

    if (construct) {
      dataPtr = new (ptr) T[totalCapacity];
    } else {
      dataPtr = reinterpret_cast<T *>(ptr);
    }

    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    allocationArea = AllocationArea::PRE_ALLOCATED;
  }

  void pointToPreallocatedArea(T *ptr, uint32_t inNumItems, uint32_t inCapacity,
    bool construct = false) {

    DEBUG_REQUIRE_EQ(dataPtr, nullptr);

    count = inNumItems;
    totalCapacity = inCapacity;

    if (construct) {
      dataPtr = new (ptr) T[totalCapacity];
    } else {
      dataPtr = reinterpret_cast<T *>(ptr);
    }

    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    allocationArea = AllocationArea::PRE_ALLOCATED;
  }

  void allocateHWRAM(uint32_t numItems, bool fillCapacity = false) {
    DEBUG_REQUIRE_EQ(dataPtr, nullptr);

    count = fillCapacity ? numItems : 0;
    totalCapacity = numItems;

    if (numItems > 0) {
      T *highAllocatedNewData{ Memory::High::memalign<T>(alignof(T),
        totalCapacity * sizeof(T)) };

      DEBUG_REQUIRE_NE(highAllocatedNewData, nullptr);
      dataPtr = highAllocatedNewData;
      allocationArea = AllocationArea::HWRAM;
    } else {
      dataPtr = nullptr;
    }
  }

  void allocateDRAM(uint32_t numItems, bool fillCapacity = false) {
    DEBUG_REQUIRE_EQ(dataPtr, nullptr);

    count = fillCapacity ? numItems : 0;
    totalCapacity = numItems;

    if (numItems > 0) {
      T *highAllocatedNewData{ Memory::DRAM::memalign<T>(alignof(T),
        totalCapacity * sizeof(T)) };

      DEBUG_REQUIRE_NE(highAllocatedNewData, nullptr);
      dataPtr = highAllocatedNewData;
      allocationArea = AllocationArea::DRAM;
    } else {
      dataPtr = nullptr;
    }
  }

  void clear() { 
    count = 0; 
  }
  
  void zeroData() {
    for (uint32_t i = 0; i < count; ++i) {
      dataPtr[i].~T();
    }

    memset(dataPtr, 0, sizeof(T) * count);
  }

  void zeroAllData() {
    for (uint32_t i = 0; i < count; ++i) {
      dataPtr[i].~T();
    }

    memset(dataPtr, 0, sizeof(T) * totalCapacity);
  }

  uint32_t num() const { return count; }
  
  uint32_t size() const { return totalCapacity; }
  
  uint32_t freeSpace() const { return totalCapacity - count; }
  
  T* lastPtr() const { return &dataPtr[count]; }

  T *addMany(uint32_t numItems) {
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(count + numItems, totalCapacity);

    T *ptr{ &dataPtr[count] };
    count += numItems;
    return ptr;
  }
  
  T& add() { 
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(count + 1, totalCapacity);

    T* newObj{ new (&dataPtr[count++]) T() };
    return *newObj;
  }

  T &uncachedAdd() {
    volatile const uint32_t posBeforeInc = Memory::uncachedPostIncrement(count);
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(posBeforeInc + 1, totalCapacity);
    
    T* newObj{ new (&dataPtr[posBeforeInc]) T() };
    return *newObj;
  }

  T& add(const T& v) { 
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(count + 1, totalCapacity);

    T* newObj{ new (&dataPtr[count++]) T(v) };
    return *newObj;
  }

  T& add(T&& v) { 
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(count + 1, totalCapacity);

    T* newObj{ new (&dataPtr[count++]) T(v) };
    return *newObj;
  }

  T& at(uint32_t index) {
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(index, count);
    return dataPtr[index];
  }

  const T& at(uint32_t index) const {
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_LT(index, count);
    return dataPtr[index];
  }

  void popItems(uint32_t numItems) {
    DEBUG_REQUIRE_NE(dataPtr, nullptr);
    DEBUG_REQUIRE_GE(count, numItems);
    count -= numItems;
  }
  
  T* data() { return dataPtr; }
  const T* data() const { return dataPtr; }

  T &operator[](uint32_t index) { return dataPtr[index]; }
  const T &operator[](uint32_t index) const { return dataPtr[index]; }

  /**
   * Wraps an interval.
   */
  class Span {
  public:
    Span() = default;
    Span(Array<T> &inArray, uint32_t inBegin, uint32_t inCount)
    {
      DEBUG_REQUIRE_LE(inBegin + inCount, inArray.num());
      dataPtr = &inArray.at(inBegin);
      totalCapacity = inCount;
    }

    T& at(uint32_t index) {
      DEBUG_REQUIRE_NE(dataPtr, nullptr);
      DEBUG_REQUIRE_LT(index, totalCapacity);
      return dataPtr[index];
    }

    const T& at(uint32_t index) const {
      DEBUG_REQUIRE_NE(dataPtr, nullptr);
      DEBUG_REQUIRE_LT(index, totalCapacity);
      return dataPtr[index];
    }
    
    T* data() { return dataPtr; }
    const T* data() const { return dataPtr; }
  
    T &operator[](uint32_t index) { return dataPtr[index]; }
    const T &operator[](uint32_t index) const { return dataPtr[index]; }
  
  private:
    T* dataPtr{ nullptr };
    volatile uint32_t totalCapacity{ 0 };
  };
  
  Span span(uint32_t inBegin) {
    return Span(*this, inBegin, count - inBegin);
  }

  Span span(uint32_t inBegin, uint32_t inCount) {
    return Span(*this, inBegin, inCount);
  }
  
  Span spanFromAddMany(uint32_t numItems) {
    const volatile uint32_t copyCount{ count };
    const T* ptr{ addMany(numItems) };
    DEBUG_REQUIRE_NE(ptr, nullptr);

    return Span(*this, copyCount, numItems);
  }

private:
  T *dataPtr __packed __aligned(alignof(T)) { nullptr };

  enum class AllocationArea : uint32_t {
    PRE_ALLOCATED = 0,
    HWRAM,
    DRAM,
  };

  volatile AllocationArea allocationArea{ AllocationArea::PRE_ALLOCATED };
  volatile uint32_t count{ 0 };
  volatile uint32_t totalCapacity{ 0 };
};

