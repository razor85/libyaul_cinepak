#include "base.h"
#include "cd.h"
#include "decoder.h"

#include "pcmsys.h"

static void _vblank_in_handler(void *work __unused);

static void _vblank_out_handler(void *work __unused);

static void _scu_timer_0_handler(void);

static smpc_peripheral_digital_t pad0;

static cdfs_filelist_t filelist;

volatile bool g_vbl_in = false;

volatile bool g_time_on = false;

uint32_t work_area[sizeof(decode_work_t)];

uint32_t *vdp2DestinationBuffer = (uint32_t *) VDP2_VRAM_ADDR(0, 0);

#define MOVIE_LIST_ENTRIES 40

#define VIDEO_WIDTH       320
#define VIDEO_HEIGHT      240
#define SAMPLE_CACHE_SIZE 14000

film_sample_t sampleCache[SAMPLE_CACHE_SIZE];

uint32_t decode_buffer[VIDEO_WIDTH * VIDEO_HEIGHT];

int film_loop_handler() {
  smpc_peripheral_process();
  smpc_peripheral_digital_port(1, &pad0);
  if (pad0.released.button.b) {
    return false;
  } else {
    return true;
  }
}

cdfs_filelist_entry_t *soundDriverEntry = NULL;
const char *soundDriverName = "SNDDRV.BIN";
volatile uint16_t soundDriverData[8192] = {0};

uint8_t *baseSoundMemory = NULL;
uint8_t *soundMemory = NULL;
uint8_t *soundMemoryLimit = NULL;

uint32_t film_audio_get_next_buffer_size() {
  uint32_t size = (uint32_t)(soundMemoryLimit - soundMemory);
  if (size == 0) {
    soundMemory = baseSoundMemory;
    size = (uint32_t)(soundMemoryLimit - soundMemory);
  }
  return size;
}

uint16_t *film_audio_get_next_buffer_ptr(uint8_t slot) {
  
  return (uint16_t *) soundMemory + (slot * getSlotSize());
}

void film_audio_notify_read_buffer_bytes(uint32_t length) {
  soundMemory += length;

  if (soundMemory >= soundMemoryLimit) {
    soundMemory = baseSoundMemory;
  }
}

void film_audio_play(uint32_t bufferLength __unused) {
  pcmStreamPlay(7);
  sound_notify_driver();
}

void film_audio_setup(decode_work_t *work, uint16_t frequency, uint32_t channels, uint32_t numBits) {
  // TODO: Proper stereo
  pcmStreamConfigure(channels == 2 ? 1 : 1, numBits, frequency);
  baseSoundMemory = getSlotAddress(0);
  soundMemory = baseSoundMemory;
  soundMemoryLimit = baseSoundMemory + getSlotSize();
  work->decodeParams->audioBufferAddr = getSlotAddress(0);
  work->decodeParams->audioBufferSize = getSlotSize();
  work->stream.sampleCache.pcmPlayPosition = getSlotAddress(0);
}

void film_audio_prepare_to_play() {}

void film_audio_reset() {
  baseSoundMemory = NULL;
  soundMemory = NULL;
  soundMemoryLimit = NULL;
}

void loadSoundDriver() {
  queueDiskRead(soundDriverEntry->starting_fad, soundDriverEntry->size);

  const uint32_t sectorsReady =
    getSectorsReady(numSectorsForSize(soundDriverEntry->size));

  int status __unused = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);

  waitUntilCdDataIsAvailable();

  volatile uint16_t *cdData = (volatile uint16_t *) CD_BLOCK_DATA_2;

  const uint32_t dataForReading = soundDriverEntry->size >> 1;
  for (volatile uint32_t i = 0; i < dataForReading; ++i) {
    soundDriverData[i] = *cdData;
  }

  status = cd_block_cmd_data_transfer_end();

  pcmsys_load_driver((void *) soundDriverData, soundDriverEntry->size);
  pcmStreamInitialize();
}


#define SMPCPK_VBL_COPY_MAX (352 * 120)
/*  RAM cycle pattern (bank A0) register */
#define CYCLE_A_REG 0x25f80010

/*  VRAM cycle pattern (bank B0) register */
#define CYCLE_B_REG 0x25f80018

/* Turn on CPU read/write mode */
#define CYCLE_CPU_WRITE(reg_addr) \
  { *((uint32_t *) (reg_addr)) = 0xeeeeeeee; }

/* Set to character pattern data read mode */
#define CYCLE_VDP_READ(reg_addr) \
  { *((uint32_t *) (reg_addr)) = 0x44444444; }

/* 1/2 */
#define SMP_DIV2(a) ((a) >> 1)
#define SCL_MAXLINE 512
void copyVideoFrame2(decode_work_t *work) {

  uint32_t movie_x = work->filmHeader.fdsc.width;
  uint32_t movie_y = work->filmHeader.fdsc.height;
  uint32_t *src = work->decodeParams->vramBuffAddr;
  uint32_t *dst = vdp2DestinationBuffer;
  int32_t copy_size;
  
  if (work->decodeParams->decodeColorDepth == COLOR_DEPTH_15) {
    copy_size = 2 * movie_x;
  } else {
    copy_size = 4 * movie_x;
  }

    uint32_t *src_stop1 = src + movie_x * SMP_DIV2(movie_y);
    uint32_t *src_stop2 = src + movie_x * movie_y;

	g_time_on = false;
	while (g_time_on == false) ;
	g_vbl_in = false;

    /* Set bank A0 to CPU write mode */
	CYCLE_CPU_WRITE(CYCLE_A_REG);

	while (src < src_stop1) {
    DMA_ScuMemCopy(dst, src, copy_size);
		src += movie_x;
    if (work->decodeParams->decodeColorDepth == COLOR_DEPTH_15) {
			dst += SMP_DIV2(SCL_MAXLINE);
		} else {
			dst += SCL_MAXLINE;
        }
	}

	/* Make bank A0 a character pattern data read. */
	CYCLE_VDP_READ(CYCLE_A_REG);

	/* Bottom half transfer  */
	while (g_vbl_in == false);

	/* Set bank B0 to CPU write mode */
	CYCLE_CPU_WRITE(CYCLE_B_REG);

	while (src < src_stop2) {
		DMA_ScuMemCopy(dst, src, copy_size);
		src += movie_x;
    if (work->decodeParams->decodeColorDepth == COLOR_DEPTH_15) {
			dst += SMP_DIV2(SCL_MAXLINE);
		} else {
			dst += SCL_MAXLINE;
        }
		
	}
	/* Make bank B0 a character pattern data read. */
	CYCLE_VDP_READ(CYCLE_B_REG);
  
}

void copyVideoFrame1(decode_work_t *work) {
  uint32_t movie_x = work->filmHeader.fdsc.width;
  uint32_t movie_y = work->filmHeader.fdsc.height;
  uint32_t *src = work->decodeParams->vramBuffAddr;
  uint32_t *dst = vdp2DestinationBuffer;
  int32_t copy_size;

  if (work->decodeParams->decodeColorDepth == COLOR_DEPTH_15) {
    copy_size = 2 * movie_x;
  } else {
    copy_size = 4 * movie_x;
  }

  uint32_t *src_stop1 = src + movie_x * SMP_DIV2(movie_y);
  uint32_t *src_stop2 = src + movie_x * movie_y;

  g_time_on = false;
  while (g_time_on == false);
  g_vbl_in = false;

  /* Set bank A0 to CPU write mode */
  CYCLE_CPU_WRITE(CYCLE_A_REG);

  while (src < src_stop1) {
    DMA_ScuMemCopy(dst, src, copy_size);
    src += movie_x;
    if (work->decodeParams->decodeColorDepth == COLOR_DEPTH_15) {
      dst += SMP_DIV2(SCL_MAXLINE);
    } else {
      dst += SCL_MAXLINE;
    }
  }

  /* Make bank A0 a character pattern data read. */
  CYCLE_VDP_READ(CYCLE_A_REG);
  
	/* Bottom half transfer */
    /*Don't wait, transfer all at once during Vbl. */
    /* while (g_vbl_in == FALSE); */

  /* Set bank B0 to CPU write mode */
  CYCLE_CPU_WRITE(CYCLE_B_REG);

  while (src < src_stop2) {
    DMA_ScuMemCopy(dst, src, copy_size);
    src += movie_x;
    if (work->decodeParams->decodeColorDepth == COLOR_DEPTH_15) {
      dst += SMP_DIV2(SCL_MAXLINE);
    } else {
      dst += SCL_MAXLINE;
    }
  }
  /* Make bank B0 a character pattern data read. */
  CYCLE_VDP_READ(CYCLE_B_REG);
}

#define SMPCPK_VBL_COPY_MAX (352 * 120)
void copyVideoFrame(decode_work_t *work) {

    if (work->filmHeader.fdsc.width * work->filmHeader.fdsc.height <= SMPCPK_VBL_COPY_MAX) {
        copyVideoFrame1(work);
    } else {
        //This should need to be split between blanks, but it seems to work without doing this.
        //copyVideoFrame2(work);
        copyVideoFrame1(work);
    }

}

void DMA_ScuMemCopy(void *dst, void *src, uint32_t cnt) {

     const scu_dma_handle_t dma_handle = {.dnr =
                                         CPU_CACHE_THROUGH | (uintptr_t) src,
    .dnw = CPU_CACHE_THROUGH | (uintptr_t) dst,
    .dnc = cnt,
    .dnad = 0x00000101,
    .dnmd = 0x00000000};

     scu_dma_config_set(0, SCU_DMA_START_FACTOR_ENABLE, &dma_handle, NULL);
     scu_dma_level_fast_start(0);
     cpu_cache_purge();

}

int main() {
  cdfs_init();
  cdfs_filelist_entry_t *const filelist_entries = cdfs_entries_alloc(-1);

  cdfs_filelist_init(&filelist, filelist_entries, -1);
  cdfs_filelist_root_read(&filelist);

  cdfs_filelist_entry_t *movieEntries[MOVIE_LIST_ENTRIES];
  memset(movieEntries, 0, sizeof(cdfs_filelist_entry_t *) * MOVIE_LIST_ENTRIES);

  const uint32_t soundDriverNameLen = strlen(soundDriverName);

  uint32_t numMovieEntries = 0;
  for (uint32_t i = 0; i < filelist.entries_count; ++i) {
    const char *name = filelist.entries[i].name;
    uint32_t nameLen = strlen(name);

    if (soundDriverEntry == NULL &&
      strncmp(soundDriverName, name, soundDriverNameLen) == 0) {
      soundDriverEntry = &filelist.entries[i];
    } else if (strcmp(".CPK", &name[nameLen - 4]) == 0) {
      movieEntries[numMovieEntries++] = &filelist.entries[i];
    }
  }

  loadSoundDriver();

  uint32_t menuSelection = 0;
  bool movieSelected = false;
  bool restart = false;

  decode_work_t *cpk = &work_area;
  decode_param_t params;
  scu_timer_t0_value_set(122);
  scu_timer_t0_set(_scu_timer_0_handler);
  scu_timer_enable();

  vdp2_sync();
  while (true) {
    smpc_peripheral_process();
    smpc_peripheral_digital_port(1, &pad0);

    if (movieSelected == false) {
      for (uint32_t i = 0; i < numMovieEntries; ++i) {
        if (menuSelection == i)
          dbgio_printf(" > ");
        else
          dbgio_printf("   ");

        dbgio_printf("%s\n", movieEntries[i]->name);
      }

      if (pad0.released.button.down) {
        menuSelection++;
        if (menuSelection >= numMovieEntries)
          menuSelection = 0;
      } else if (pad0.released.button.up) {
        menuSelection--;
        if (menuSelection == 0xFFFFFFFF)
          menuSelection = numMovieEntries - 1;
      } else if (pad0.released.button.a /* || pad0.released.button.start*/) {
        movieSelected = true;
        restart = true;
      }

      dbgio_flush();
      clearLog();
      vdp2_sync();
      vdp2_sync_wait();
    } else {

      if (restart == true) {
        dbgio_flush();
        vdp2_sync();
        vdp2_sync_wait();
        sprintf((char *) LWRAM(80), "FILM INIT START");

        memset(&sampleCache, 0, SAMPLE_CACHE_SIZE * sizeof(film_sample_t));
        memset(&cpk->filmHeader, 0, sizeof(film_header));

        params.sampleBuffAddr = &sampleCache;
        params.sampleBuffSize = SAMPLE_CACHE_SIZE * sizeof(film_sample_t);
        params.vramBuffAddr = &decode_buffer;
        params.vramBufferWidth = VIDEO_WIDTH;
        params.vramBuffSize = (VIDEO_WIDTH * VIDEO_HEIGHT) * 4;
        params.decodeColorDepth = COLOR_DEPTH_24;
        //Currently not used, eventually should use these and set them to what's in the FILM Header. 
        //params.pcmVolume = 7;
        //params.pcmChannels = 2;
        //params.pcmPan = 16;
        // Currently not used, should be used eventually to allow the user to define what slots and addresses to use for audio.
        //params.audioBufferAddr = getSlotAddress(0);
        //params.audioBufferSize = pcmStreamBufferSize(16, 22050);

        cpk->decodeParams = &params;
        cpk->play_status = 0;

        init_film(movieEntries[menuSelection], cpk, 240, 320);

        cpk_play(cpk);

        restart = false;
      }

      cpk_task(cpk);

      if (cpk_display_ready(cpk) == true) {
        copyVideoFrame(cpk);
        cpk_display_finished(cpk);
      }

      if (cpk->play_status == END) {
        movieSelected = false;

        pcmStreamStop();
        sound_notify_driver();

        film_audio_reset();

        dbgio_dev_font_load();
      }
    }

  }
}

void user_init(void) {
  const vdp2_scrn_bitmap_format_t format = {.scroll_screen = VDP2_SCRN_NBG0,
    //.ccc = VDP2_SCRN_CCC_RGB_32768,
    .ccc = VDP2_SCRN_CCC_RGB_16770000,
    .bitmap_size = VDP2_SCRN_BITMAP_SIZE_512X256,
    .palette_base = 0x00000000,
    .bitmap_base = VDP2_VRAM_ADDR(0, 0x00000)};

  vdp2_scrn_sf_set(
    VDP2_SCRN_NBG0, 0, VDP2_SCRN_SF_TYPE_NONE, VDP2_SCRN_SF_CODE_A);

  vdp2_scrn_bitmap_format_set(&format);
  vdp2_scrn_priority_set(VDP2_SCRN_NBG0, 3);
  vdp2_scrn_display_set(VDP2_SCRN_DISPTP_NBG0);

  const vdp2_vram_cycp_t vram_cycp = {.pt[0].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[0].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[0].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[0].t7 = VDP2_VRAM_CYCP_NO_ACCESS,

    .pt[1].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[1].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[1].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[1].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[1].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t7 = VDP2_VRAM_CYCP_NO_ACCESS,

    .pt[2].t0 = VDP2_VRAM_CYCP_VCSTDR_NBG0,
    .pt[2].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t7 = VDP2_VRAM_CYCP_NO_ACCESS,

    .pt[3].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[3].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[3].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[3].t3 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t7 = VDP2_VRAM_CYCP_NO_ACCESS};

  vdp2_vram_cycp_set(&vram_cycp);

#define NBG0_LINE_SCROLL VDP2_VRAM_ADDR(2, 0x20000)
#define NBG0_VCS         VDP2_VRAM_ADDR(2, 0x10000)

  const vdp2_scrn_ls_format_t ls_format = {.scroll_screen = VDP2_SCRN_NBG0,
    .table_base = NBG0_LINE_SCROLL,
    .interval = 0,
    .type = VDP2_SCRN_LS_TYPE_HORZ | VDP2_SCRN_LS_TYPE_VERT};

  //vdp2_scrn_ls_set(&ls_format);

  const vdp2_scrn_vcs_format_t vcs_format = {.scroll_screen = VDP2_SCRN_NBG0,
    .table_base = NBG0_VCS};

  //Undid the line scroll stuff for now to make the DMA easier to figure out.

  //vdp2_scrn_vcs_set(&vcs_format);

  /* volatile uint32_t *horizontalCoordinates =
    (volatile uint32_t *) NBG0_LINE_SCROLL;
  const uint32_t scrollMask = 0x01ff0000; // Integer part
  for (volatile uint32_t n = 0; n < 240; ++n) {
    // Horizontal screen scroll value. This will be (value % 320) * 0xFFFF
    const uint32_t horizontalSize = 320;
    horizontalCoordinates[n * 2 + 0] =
      (65536 * horizontalSize * n) & scrollMask;

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
  }*/

  vdp2_tvmd_display_res_set(
    VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A, VDP2_TVMD_VERT_240);

  vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), RGB1555(1, 0, 0, 0));

  vdp_sync_vblank_in_set(_vblank_in_handler, NULL);

  vdp_sync_vblank_out_set(_vblank_out_handler, NULL);

  cpu_frt_init(CPU_FRT_CLOCK_DIV_128);
  cpu_frt_interrupt_priority_set(15);

  dbgio_init();
  dbgio_dev_default_init(DBGIO_DEV_VDP2);
  dbgio_dev_font_load();

  vdp2_tvmd_display_set();

  smpc_peripheral_init();

  // Improve performance by ignoring HBLANK IN
  scu_ic_mask_chg(SCU_IC_MASK_ALL, SCU_IC_MASK_HBLANK_IN);
}

static void _vblank_in_handler(void *work __unused) { g_vbl_in = true;}

static void _vblank_out_handler(void *work __unused) {
  smpc_peripheral_intback_issue();
}

_scu_timer_0_handler(void) { g_time_on = true; }
