#ifndef DECODER_CD_H
#define DECODER_CD_H

#include "base.h"
#include "memory.h"

// Read 2 bytes from register
#define CD_BLOCK_TRANSFER_REGISTER 0x25890000UL
#define CD_BLOCK_HIRQ              CD_BLOCK(0x0008UL)

enum class CdBlockFlagsIRQ : uint16_t {
  CMOK = 0x0001, /* Command dispatch possible */
  DRDY = 0x0002, /* Data transfer preparations complete */
  CSCT = 0x0004, /* Finished reading 1 sector */
  BFUL = 0x0008, /* CD buffer full */
  PEND = 0x0010, /* CD playback completed */
  DCHG = 0x0020, /* Disc change or tray open */
  ESEL = 0x0040, /* Selector settings processing complete */
  EHST = 0x0080, /* Host I/O processing complete */
  ECPY = 0x0100, /* Duplication/move processing complete */
  EFLS = 0x0200, /* File system processing complete */
  SCDQ = 0x0400, /* Subcode Q update completed */
  MPED = 0x0800, /* MPEG-related processing complete */
  MPCM = 0x1000, /* MPEG action uncertain */
  MPST = 0x2000, /* MPEG interrupt status report */
};

/**
 * Stream big files from the CD block
 */
class StreamFile {
private:
  static constexpr uint32_t SectorsToPreload = 8;

  bool m_initialized{false};
  uint32_t m_startFAD{0};
  uint32_t m_size{0};
  uint32_t m_remainingSectors{0};
  uint32_t m_dataAvailable{0};
  uint32_t m_offset{0};

  static uint16_t waitFlagIRQ(CdBlockFlagsIRQ flag) {
    volatile uint16_t lastRead = 0;
    do {
      lastRead = MEMORY_READ(16, CD_BLOCK_HIRQ);
    } while (!(lastRead & static_cast<uint16_t>(flag)));

    return lastRead;
  }

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
    }
  }

  static uint32_t getSectorsReady(uint32_t wantSectors) {
    volatile uint32_t sectorsReady;
    while (true) {
      sectorsReady = cd_block_cmd_sector_number_get(0);
      if (sectorsReady >= wantSectors) {
        break;
      } else {
        waitFlagIRQ(CdBlockFlagsIRQ::CSCT);
      }
    }

    return sectorsReady;
  }

  static void waitUntilCdDataIsAvailable() { waitFlagIRQ(CdBlockFlagsIRQ::DRDY); }

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

    waitFlagIRQ(CdBlockFlagsIRQ::CMOK);

    // End previous transfers
    [[maybe_unused]] int status = cd_block_cmd_data_transfer_end();
    DEBUG_REQUIRE_EQ(status, 0);

    const uint32_t sectorsReady = getSectorsReady(min<uint32_t>(m_remainingSectors, SectorsToPreload));
    DEBUG_REQUIRE_GT(sectorsReady, 0);

    waitFlagIRQ(CdBlockFlagsIRQ::CMOK);

    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
    DEBUG_REQUIRE_EQ(status, 0);

    // Set those anyway while we wait for the cdblock to respond
    m_dataAvailable += sectorsReady * CDFS_SECTOR_SIZE;
    m_remainingSectors -= sectorsReady;

    // Wait until data is available
    const volatile uint16_t hirq = waitFlagIRQ(CdBlockFlagsIRQ::DRDY);
    if (hirq & static_cast<uint16_t>(CdBlockFlagsIRQ::EHST)) {
      // Read failed :(
      assert(false);
    }
  }

public:
  StreamFile() = default;

  ~StreamFile() {
    if (m_initialized) {
      cd_block_cmd_data_transfer_end();
    }
  }

  void initialize(cdfs_filelist_entry_t *entry) {
    if (m_initialized) {
      cd_block_cmd_data_transfer_end();
    }

    m_initialized = true;
    queueDiskRead(entry->starting_fad, entry->size);

    m_startFAD = entry->starting_fad;
    m_size = entry->size;
    m_remainingSectors = numSectorsForSize(entry->size);
    m_dataAvailable = 0;
    m_offset = 0;

    // Fetch first sectors
    const uint32_t sectorsReady = getSectorsReady(min<uint32_t>(m_remainingSectors, SectorsToPreload));
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
      if (m_dataAvailable <= 1) {
        triggerDataRequest();
      }

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
      if (m_dataAvailable <= 1) {
        triggerDataRequest();
      }

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

    uint16_t data = 0;
    read(&data, 2);

    return data;
  }

  uint32_t read32() {
    DEBUG_REQUIRE(m_initialized);

    uint32_t data = 0;
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
