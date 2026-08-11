#ifndef DECODER_CD_OLD_H
#define DECODER_CD_OLD_H

// Drop-in alternate to cd.h/cd_sega.h: literal port of the CD block
// interface from the older build at
// D:\SaturnDocker\Yaul\libyaul_cinepak-old, which has tested stable on real
// hardware. Deliberately NOT merged with any of this session's later
// additions (filter/aperture setup, CD_STATUS_WAIT retry wrapping,
// waitCdCommandReady, the non-blocking cd_linear_pump_t) - this is meant to
// be a faithful reproduction of the old logic for an A/B test against it,
// not an "improved" hybrid. To use this instead of cd.h/cd_sega.h, swap the
// #include line for cd_old.h in main.c/film_lib_old.c/film_buff_old.c (and
// use film_buff_old.c/film_lib_old.c together - both together, don't mix
// with the newer film_buff.c/film_buff_sega.c/film_lib.c).

#include "base.h"

#define HIRQ 0x0008UL
#define DRDY 0x0002 /* Data transfer preparations complete */
#define EHST 0x0080 /* Host I/O processing complete */

// Read 2 bytes from register
#define CD_BLOCK_DATA_2 0x25890000UL

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

      if (status.cd_status == CD_STATUS_PAUSE ||
        status.cd_status == CD_STATUS_STANDBY) {
        break;
      }

    } else {
      logMessage("\nFailed to get cd block status: %d\n", lastStatus);
    }

    // Can't issue too many commands at once so...
    for (volatile uint32_t i = 0; i < 0xFFFF; ++i) {
      cpu_instr_nop();
    }
  }
}

static void queueDiskRead(uint32_t fad, uint32_t size) {
  DEBUG_REQUIRE_NE(size, 0);
  DEBUG_REQUIRE_EQ(size % 4, 0);
  waitForCD();

  int status __unused = cd_block_cmd_selector_reset(0, 0);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_cd_dev_connection_set(0);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_disk_play(0, fad, numSectorsForSize(size));
  DEBUG_REQUIRE_EQ(status, 0);
}

static uint32_t getSectorsReady(uint32_t wantSectors) {
  int32_t sectorsReady;
  while (true) {
    sectorsReady = cd_block_cmd_sector_number_get(0);
    if (sectorsReady >= wantSectors) {
      break;
    } else {
      for (volatile int32_t i = 0; i < 1024; ++i) { cpu_instr_nop(); }
    }
  }

  return sectorsReady;
}

// The old build had no equivalent helper - loadSoundDriver() (main.c) is
// the only caller, and it's a current-codebase addition. Kept as a plain,
// unwrapped passthrough (no CD_STATUS_WAIT retry loop) rather than pulling
// in the newer retry-wrapper version, to stay consistent with old cd.h's
// general style of not retry-wrapping commands - this exists here only so
// main.c compiles unchanged against this header.
static void cdCmdDataTransferEnd() {
  cd_block_cmd_data_transfer_end();
}

static void waitUntilCdDataIsAvailable() {
  // Wait until data is available
  bool ready __unused = false;
  for (volatile int32_t i = 0; i < 240000; ++i) {
    if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY) {
      ready = true;
      break;
    }
  }

  DEBUG_REQUIRE(ready);
}

#endif //DECODER_CD_OLD_H
