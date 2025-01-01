#ifndef DECODER_CD_H
#define DECODER_CD_H

#include "base.h"
#include "memory.h"

#define HIRQ 0x0008UL
#define DRDY 0x0002 /* Data transfer preparations complete */

// Read 2 bytes from register
#define CD_BLOCK_TRANSFER_REGISTER 0x25890000UL

/**
 * Stream big files from the CD block
 */
class StreamFile {
private:
  bool m_initialized{false};
  uint32_t m_startFAD{0};
  uint32_t m_size{0};
  uint32_t m_remainingSectors{0};
  uint32_t m_dataAvailable{0};
  uint32_t m_offset{0};

  static uint32_t numSectorsForSize(uint32_t size) {
    // Past size so we get the complete number of sectors for the whole data.
    return (size + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
  }

  static void waitForCD() {
    cd_block_status_t status;
    while (true) {
      while (cd_block_busy()) {
        cpu_instr_nop();
      }

      const int lastStatus = cd_block_cmd_status_get(&status);
      if (lastStatus == 0) {
        DEBUG_REQUIRE_NE(status.cd_status, CD_STATUS_ERROR);
        DEBUG_REQUIRE_NE(status.cd_status, CD_STATUS_FATAL);

        if (status.cd_status == CD_STATUS_PAUSE || status.cd_status == CD_STATUS_STANDBY) {
          break;
        }
      } else {
        Console::printf("\nFailed to get cd block status: %d\n", lastStatus);
      }

      // Can't issue too many commands at once so...
      for (volatile uint32_t i = 0; i < 0xFFFF; ++i) {
        cpu_instr_nop();
      }
    }
  }

  static uint32_t getSectorsReady(uint32_t wantSectors) {
    uint32_t sectorsReady;
    while (true) {
      sectorsReady = cd_block_cmd_sector_number_get(0);
      if (sectorsReady >= wantSectors) {
        break;
      } else {
        for (volatile uint32_t i = 0; i < 1024; ++i) {
          cpu_instr_nop();
        }
      }
    }

    return sectorsReady;
  }

  static void waitUntilCdDataIsAvailable() {
    // Wait until data is available
    [[maybe_unused]] bool ready = false;
    for (volatile uint32_t i = 0; i < 240000; ++i) {
      if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY) {
        ready = true;
        break;
      }
    }

    DEBUG_REQUIRE(ready);
  }

  static void queueDiskRead(uint32_t fad, uint32_t size) {
    DEBUG_REQUIRE_NE(size, 0);
    DEBUG_REQUIRE_EQ(size % 2, 0);
    waitForCD();

    [[maybe_unused]] int status = cd_block_cmd_selector_reset(0, 0);
    DEBUG_REQUIRE_EQ(status, 0);

    status = cd_block_cmd_cd_dev_connection_set(0);
    DEBUG_REQUIRE_EQ(status, 0);

    status = cd_block_cmd_disk_play(0, fad, numSectorsForSize(size));
    DEBUG_REQUIRE_EQ(status, 0);
  }

  void triggerDataRequest() {
    DEBUG_REQUIRE(m_initialized);
    DEBUG_REQUIRE_NE(m_remainingSectors, 0);

    // End previous transfers
    [[maybe_unused]] int status = cd_block_cmd_data_transfer_end();
    DEBUG_REQUIRE_EQ(status, 0);

    const uint32_t sectorsReady = getSectorsReady(min<uint32_t>(m_remainingSectors, 8));
    DEBUG_REQUIRE_GT(sectorsReady, 0);

    while (true) {
      status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
      if (status & CD_STATUS_WAIT) {
        for (volatile uint32_t i = 0; i < 4096; ++i) {
          cpu_instr_nop();
        }
      } else {
        break;
      }
    }

    // Wait until data is available
    [[maybe_unused]] bool ready = false;
    for (volatile uint32_t i = 0; i < 240000; ++i) {
      if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY) {
        ready = true;
        break;
      }
    }

    DEBUG_REQUIRE(ready);

    m_dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
    m_remainingSectors -= sectorsReady;
  }

public:
  StreamFile() = default;

  ~StreamFile() {
    if (m_initialized) {
      cd_block_cmd_data_transfer_end();
    }
  }

  void initialize(cdfs_filelist_entry_t *entry) {
    DEBUG_REQUIRE(!m_initialized);

    Console::printf_flush("%s (%d bytes), FAD: %d\n", entry->name, entry->size, entry->starting_fad);

    m_initialized = true;
    queueDiskRead(entry->starting_fad, entry->size);

    m_startFAD = entry->starting_fad;
    m_size = entry->size;
    m_remainingSectors = numSectorsForSize(entry->size);
    m_dataAvailable = 0;
    m_offset = 0;

    // Fetch first sectors
    const uint32_t sectorsReady = getSectorsReady(min<uint32_t>(m_remainingSectors, 8));

    DEBUG_REQUIRE_GT(sectorsReady, 0);

    [[maybe_unused]] int status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);

    DEBUG_REQUIRE_EQ(status, 0);

    waitUntilCdDataIsAvailable();

    m_dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
    m_remainingSectors -= sectorsReady;
  }

  void read(volatile void *tmpDestPtr, uint32_t len) {
    DEBUG_REQUIRE(m_initialized);
    DEBUG_REQUIRE_NE(tmpDestPtr, 0);
    DEBUG_REQUIRE_EQ(len % 2, 0);
    DEBUG_REQUIRE_LE(m_offset + len, m_size);

    volatile uint16_t *destPtr = reinterpret_cast<volatile uint16_t *>(tmpDestPtr);

    uint32_t remainingBytes = len;
    while (remainingBytes) {
      if (!m_dataAvailable)
        triggerDataRequest();

      const uint32_t readSize = min(m_dataAvailable, remainingBytes);
      const uint32_t readSizeLoop = readSize >> 1;
      DEBUG_REQUIRE_EQ(readSize % 2, 0);

      m_dataAvailable -= readSize;
      remainingBytes -= readSize;
      for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
        *destPtr++ = MEMORY_READ(16, CD_BLOCK_TRANSFER_REGISTER);
      }
    }

    m_offset += len;
  }

  void skip(uint32_t len) {
    if (len == 0) {
      return;
    }

    DEBUG_REQUIRE(m_initialized);
    DEBUG_REQUIRE_EQ(len % 2, 0);
    DEBUG_REQUIRE_LE(m_offset + len, m_size);

    uint32_t remainingBytes = len;
    while (remainingBytes) {
      if (!m_dataAvailable)
        triggerDataRequest();

      const uint32_t readSize = min(m_dataAvailable, remainingBytes);
      const uint32_t readSizeLoop = readSize >> 1;
      DEBUG_REQUIRE_EQ(readSize % 2, 0);

      m_dataAvailable -= readSize;
      remainingBytes -= readSize;

      for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
        MEMORY_READ(16, CD_BLOCK_TRANSFER_REGISTER);
      }
    }

    m_offset += len;
  }

  uint16_t read16() {
    DEBUG_REQUIRE(m_initialized);

    uint16_t data;
    read(&data, 2);

    return data;
  }

  uint32_t read32() {
    DEBUG_REQUIRE(m_initialized);

    uint32_t data;
    read(&data, 4);

    return data;
  }

  [[nodiscard]] bool isInitialized() { return m_initialized; }

  [[nodiscard]] uint32_t getStartFAD() { return m_startFAD; }

  [[nodiscard]] uint32_t getSize() { return m_size; }

  [[nodiscard]] uint32_t getRemainingSectors() { return m_remainingSectors; }

  [[nodiscard]] uint32_t getDataAvailable() { return m_dataAvailable; }

  [[nodiscard]] uint32_t getOffset() { return m_offset; }

  [[nodiscard]] bool hasDataToRead() { return m_offset < m_size; }
};

#endif // DECODER_CD_H
