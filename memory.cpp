#include "memory.h"
#include <assert.h>
#include <stdarg.h>

#include "tlsf/tlsf.c"

namespace {

vdp1_vram_partitions *getVRAMPartitions() {
  static vdp1_vram_partitions partitions;
  static bool initialized = false;
  if (!initialized)
    vdp1_vram_partitions_get(&partitions);

  return &partitions;
}

} // namespace

StringStream &StringStream::operator<<(bool n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(unsigned char n) {
  addToBuffer(reinterpret_cast<const char *>(&n), 1);
  return *this;
}

StringStream &StringStream::operator<<(char n) {
  addToBuffer(reinterpret_cast<const char *>(&n), 1);
  return *this;
}

StringStream &StringStream::operator<<(unsigned short n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(short n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(unsigned int n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(int n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(long unsigned int n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(long int n) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "%d", n);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(const void *ptr) {
  const uint32_t chars = snprintf(mTmpBuffer, 64, "0x%X", ptr);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(nullptr_t) {
  addToBuffer("NULL", 4);

  return *this;
}

StringStream &StringStream::operator<<(yaul::fix16 n) {
  const uint32_t chars = fix16_str(n.value, mTmpBuffer, 3);
  addToBuffer(mTmpBuffer, chars);

  return *this;
}

StringStream &StringStream::operator<<(const char *str) {
  addToBuffer(str, strlen(str));
  return *this;
}

void StringStream::addToBuffer(const char *input, uint32_t len) {
  for (volatile uint32_t i = 0; i < len; ++i) {
    if (mSize >= (mMaxSize - 1))
      break;

    mBuffer[mSize] = input[i] ? input[i] : '0';
    ++mSize;
  }
}

const char *StringStream::buffer() const { return mBuffer; }

// Store debug messages
char DebugBuffer::msg[256];

void DebugBuffer::print() { print(msg); }

void DebugBuffer::printInLoop() { dbgio_printf(msg); }

void DebugBuffer::print(const char *newMsg) {
  dbgio_printf(newMsg);
  dbgio_flush();
}

void DebugBuffer::printv(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsprintf(msg, format, arguments);
  va_end(arguments);

  dbgio_printf(msg);
  dbgio_flush();
}

namespace Memory {

// Declare static data.
user_tlsf_t Low::tlsfHandle = nullptr;
uint32_t Low::usedMemory = 0;

uint8_t High::data[High::maxSize];
user_tlsf_t High::tlsfHandle = nullptr;
uint32_t High::usedMemory = 0;

user_tlsf_t DRAM::tlsfHandle = nullptr;
uint32_t DRAM::usedMemory = 0;
uint32_t DRAM::totalMemory = 0;

uint32_t getOffsetInCmdRAM(uint32_t offset) {
  static const uint32_t vram = reinterpret_cast<uint32_t>(getVRAMPartitions()->cmdt_base) - VDP1_VRAM(32);

  return vram + offset;
}

uint32_t getOffsetInTextureRAM(uint32_t offset) {
  static const uint32_t vram = reinterpret_cast<uint32_t>(getVRAMPartitions()->texture_base) - VDP1_VRAM(0);

  return vram + offset;
}

uint32_t getOffsetInGouraudRAM(uint32_t offset) {
  static const uint32_t vram = reinterpret_cast<uint32_t>(getVRAMPartitions()->gouraud_base) - VDP1_VRAM(0);

  return vram + offset;
}

void libAssert(const char *file, int line, const char *msg) {
#ifndef SATURN_SIMULATOR
  __asm__ volatile("mov #4294967295, r12\n"
                   "mov #4294967295, r13\n");
#endif

  if (msg != nullptr) {
    // Console::clear();
    dbgio_printf("\n\n%s\n\n", msg);
  } else {
    // Console::clear();
    dbgio_printf(DebugBuffer::buffer(), "Assertion failed at %s:%d\n\n", file, line);
  }

  // Keep program running.
  while (true) {
    dbgio_flush();
    vdp2_sync();
    vdp2_sync_wait();
  }
}

void runTests() { Memory::Low::runTests(); }

} // namespace Memory
