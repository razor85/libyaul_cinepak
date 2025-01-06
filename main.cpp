#include "base.h"
#include "cd.h"
#include "decoder.h"
#include "timer.h"

namespace {

// Forward Declaration
void filmInitCallback();
bool filmLoopCallback();
void vblankInHandler(void *);
void vblankOutHandler(void *);

smpc_peripheral_digital_t pad0;
cdfs_filelist_t cdFiles;

constexpr uint32_t TmpBufferSize = 0x10000;
uint8_t tmpBuffer[TmpBufferSize];

constexpr uint32_t SampleCacheNumSamples{20000};
FilmStream::CachedSample sampleCache[SampleCacheNumSamples];

FilmStream film(tmpBuffer, TmpBufferSize, sampleCache, SampleCacheNumSamples, &filmInitCallback, &filmLoopCallback);

void filmInitCallback() {}

bool filmLoopCallback() {
  smpc_peripheral_process();
  smpc_peripheral_digital_port(1, &pad0);
  // if (pad0.released.button.b) {
  //   return false;
  // } else {
  //   return true;
  // }

  return true;
}

void vblankInHandler(void *) {}

void vblankOutHandler(void *) { smpc_peripheral_intback_issue(); }

} // namespace

/*
uint32_t film_audio_get_next_buffer_size() {
  return (uint32_t) (soundMemoryLimit - soundMemory);
}

uint16_t *film_audio_get_next_buffer_ptr(uint8_t slot) {
  return (uint16_t *) soundMemory + (slot * getSlotSize());
}

void film_audio_notify_read_buffer_bytes(uint32_t length) {
  soundMemory += length;
  DEBUG_REQUIRE_LE(soundMemory, soundMemoryLimit);

  if (soundMemory == soundMemoryLimit) {
    soundMemory = baseSoundMemory;
  }
}

void film_audio_play(uint32_t bufferLength __unused) {
  pcmStreamPlay(7);
  sound_notify_driver();
}

void film_audio_setup(uint16_t frequency, uint32_t channels, uint32_t numBits) {
  DEBUG_REQUIRE_EQ(baseSoundMemory, NULL);
  DEBUG_REQUIRE_EQ(soundMemory, NULL);
  DEBUG_REQUIRE_EQ(soundMemoryLimit, NULL);

  // TODO: Proper stereo
  pcmStreamConfigure(channels == 2 ? 1 : 1, numBits, frequency);
  baseSoundMemory = getSlotAddress(0);
  soundMemory = baseSoundMemory;
  soundMemoryLimit = baseSoundMemory + pcmStreamBufferSize(numBits, frequency);
}

void film_audio_prepare_to_play() {}

void film_audio_reset() {
  baseSoundMemory = NULL;
  soundMemory = NULL;
  soundMemoryLimit = NULL;
}

void loadSoundDriver() {
  DEBUG_REQUIRE_NE(soundDriverEntry, NULL);
  queueDiskRead(soundDriverEntry->starting_fad, soundDriverEntry->size);

  const uint32_t sectorsReady =
    getSectorsReady(numSectorsForSize(soundDriverEntry->size));

  DEBUG_REQUIRE_GT(sectorsReady, 0);

  int status __unused = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
  DEBUG_REQUIRE_EQ(status, 0);

  waitUntilCdDataIsAvailable();

  volatile uint16_t *cdData = (volatile uint16_t *) CD_BLOCK_DATA_REGISTER;

  const uint32_t dataForReading = soundDriverEntry->size >> 1;
  for (volatile uint32_t i = 0; i < dataForReading; ++i) {
    soundDriverData[i] = *cdData;
  }

  status = cd_block_cmd_data_transfer_end();
  DEBUG_REQUIRE_EQ(status, 0);

  pcmsys_load_driver((void *) soundDriverData, soundDriverEntry->size);
  pcmStreamInitialize();
}
*/

int main() {
  cdfs_init();

  cdfs_filelist_entry_t *const filelistEntries = cdfs_entries_alloc(-1);
  DEBUG_REQUIRE(filelistEntries != NULL);

  cdfs_filelist_init(&cdFiles, filelistEntries, -1);
  cdfs_filelist_root_read(&cdFiles);

  constexpr uint32_t MaxListEntries = 30;
  cdfs_filelist_entry_t *movieEntries[MaxListEntries];
  memset(movieEntries, 0, sizeof(cdfs_filelist_entry_t) * MaxListEntries);

  // const uint32_t soundDriverNameLen = strlen(soundDriverName);

  uint32_t numMovieEntries = 0;
  for (uint32_t i = 0; i < cdFiles.entries_count; ++i) {
    const char *name = cdFiles.entries[i].name;
    const uint32_t nameLen = strlen(name);

    // if (soundDriverEntry == NULL && strncmp(soundDriverName, name, soundDriverNameLen) == 0) {
    //   soundDriverEntry = &cdFiles.entries[i];
    // }

    if (nameLen >= 4 && strcmp(".CPK", &name[nameLen - 4]) == 0) {
      movieEntries[numMovieEntries++] = &cdFiles.entries[i];
    }
  }

  // loadSoundDriver();

  uint32_t menuSelection = 0;
  bool movieSelected = false;

  while (true) {
    smpc_peripheral_process();
    smpc_peripheral_digital_port(1, &pad0);

    Console::clear();

    if (!movieSelected) {
      for (uint32_t i = 0; i < numMovieEntries; ++i) {
        if (menuSelection == i)
          Console::printf(" > ");
        else
          Console::printf("   ");

        Console::printf("%s\n", movieEntries[i]->name);
      }

      if (pad0.released.button.down) {
        menuSelection++;
        if (menuSelection >= numMovieEntries)
          menuSelection = 0;
      } else if (pad0.released.button.up) {
        menuSelection--;
        if (menuSelection == 0xFFFFFFFF)
          menuSelection = numMovieEntries - 1;
      } else if (pad0.released.button.a || pad0.released.button.start) {
        movieSelected = true;
      }

    } else {
      Console::clear();
      Console::flush();

      film.play(movieEntries[menuSelection]);

      movieSelected = false;

      // pcmStreamStop();
      // sound_notify_driver();
      // film_audio_reset();
    }

    Console::flush();
    vdp2_sync();
    vdp2_sync_wait();
  }
}

void user_init(void) {
  const vdp2_scrn_bitmap_format_t format = {
    .scroll_screen = VDP2_SCRN_NBG0,
    .ccc = VDP2_SCRN_CCC_RGB_32768,
    .bitmap_size = VDP2_SCRN_BITMAP_SIZE_512X256,
    .palette_base = 0x00000000,
    .bitmap_base = VDP2_VRAM_ADDR(0, 0x00000),
  };

  vdp2_scrn_sf_set(VDP2_SCRN_NBG0, 0, VDP2_SCRN_SF_TYPE_NONE, VDP2_SCRN_SF_CODE_A);

  vdp2_scrn_bitmap_format_set(&format);
  vdp2_scrn_priority_set(VDP2_SCRN_NBG0, 3);
  vdp2_scrn_display_set(VDP2_SCRN_DISPTP_NBG0);

  vdp2_vram_cycp_t vram_cycp{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  vram_cycp.pt[0].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[0].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[0].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[0].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0;

  vram_cycp.pt[1].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[1].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[1].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[1].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0;

  vram_cycp.pt[2].t0 = VDP2_VRAM_CYCP_VCSTDR_NBG0;
  vram_cycp.pt[2].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[2].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[2].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0;

  vram_cycp.pt[3].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[3].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0;
  vram_cycp.pt[3].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0;

  vdp2_vram_cycp_set(&vram_cycp);

  constexpr uint32_t NBG0_LINE_SCROLL = VDP2_VRAM_ADDR(2, 0x00000);

  constexpr uint32_t NBG0_VCS = VDP2_VRAM_ADDR(2, 0x10000);

  const vdp2_scrn_ls_format_t ls_format = {
    .scroll_screen = VDP2_SCRN_NBG0,
    .table_base = NBG0_LINE_SCROLL,
    .interval = 0,
    .type = static_cast<vdp2_scrn_ls_type>(VDP2_SCRN_LS_TYPE_HORZ | VDP2_SCRN_LS_TYPE_VERT),
  };

  vdp2_scrn_ls_set(&ls_format);

  const vdp2_scrn_vcs_format_t vcs_format = {
    .scroll_screen = VDP2_SCRN_NBG0,
    .table_base = NBG0_VCS,
  };

  vdp2_scrn_vcs_set(&vcs_format);

  volatile uint32_t *horizontalCoordinates = (volatile uint32_t *) NBG0_LINE_SCROLL;
  const uint32_t scrollMask = 0x01ff0000; // Integer part
  for (volatile uint32_t n = 0; n < 240; ++n) {
    // Horizontal screen scroll value. This will be (value % 320) * 0xFFFF
    const uint32_t horizontalSize = 320;
    horizontalCoordinates[n * 2 + 0] = (65536 * horizontalSize * n) & scrollMask;

    // Vertical screen scroll value
    uint32_t iPortion = (128 * horizontalSize * n) & scrollMask;
    uint32_t aN = 64 * ((iPortion / 65536) + 1);
    uint32_t bN = (horizontalSize / 8) * (n + 1);
    uint32_t bNN = (horizontalSize / 8) * n;
    uint32_t m = aN >= bN ? 64 : aN - bNN;
    horizontalCoordinates[n * 2 + 1] = iPortion + (1024 * (64 - m));
  }

  volatile uint32_t *verticalCoordinates = (volatile uint32_t *) NBG0_VCS;
  for (volatile uint32_t n = 0; n < 256; ++n) {
    // cell is 8x8 so in 320 we have 40 cells
    const uint32_t numCells = 64;
    for (volatile uint32_t cellW = 0; cellW < numCells; ++cellW) {
      const uint32_t cellScrollValue = 1024 * cellW;
      verticalCoordinates[n * numCells + cellW] = cellScrollValue;
    }
  }

  vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A, VDP2_TVMD_VERT_240);
  vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), RGB1555(1, 0, 0, 0));
  vdp_sync_vblank_in_set(vblankInHandler, NULL);
  vdp_sync_vblank_out_set(vblankOutHandler, NULL);

  smpc_peripheral_init();

  // Console::initialize();
  SystemTime::initialize();

  vdp2_tvmd_display_set();

  // TODO: Improve performance by ignoring HBLANK IN
  scu_ic_mask_chg(SCU_IC_MASK_ALL, SCU_IC_MASK_HBLANK_IN);
}
