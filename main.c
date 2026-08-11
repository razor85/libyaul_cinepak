#include "adx_dsp_sega.h"
#include "base.h"
#include "cd.h"
#include "film_lib.h"
#include "film_snd.h"

#include "pcmsys.h"

static void _vblank_in_handler(void *work __unused);

static void _vblank_out_handler(void *work __unused);

static void _scu_timer_0_handler(void);

static smpc_peripheral_digital_t pad0;

static cdfs_filelist_t filelist;

volatile bool g_vbl_in = false;

volatile bool g_time_on = false;

volatile bool canPress = true;

uint32_t work_area[sizeof(decode_work_t)];

uint32_t *vdp2DestinationBuffer = (uint32_t *) VDP2_VRAM_ADDR(0, 0);

#define MOVIE_LIST_ENTRIES 20

#define VIDEO_WIDTH       320
#define VIDEO_HEIGHT      240
#define SAMPLE_CACHE_SIZE 24000

film_sample_t sampleCache[SAMPLE_CACHE_SIZE] __aligned(4);

uint32_t decode_buffer[VIDEO_WIDTH * VIDEO_HEIGHT] __aligned(4);

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

void loadSoundDriver() {
  queueDiskRead(soundDriverEntry->starting_fad, soundDriverEntry->size);

  const uint32_t sectorsReady =
    getSectorsReady(numSectorsForSize(soundDriverEntry->size));

  // Same unchecked-result gap already found and fixed in triggerDataRequest()/
  // stream_new() (film_buff.c) - retry on CD_STATUS_WAIT instead of ignoring
  // the result, since this runs on every boot right after queueDiskRead()'s
  // fresh disk_play, the same high-contention moment.
  int status;
  do {
    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
  } while (status & CD_STATUS_WAIT);

  waitUntilCdDataIsAvailable();

  volatile uint16_t *cdData = (volatile uint16_t *) CD_BLOCK_DATA_2;

  const uint32_t dataForReading = soundDriverEntry->size >> 1;
  for (volatile uint32_t i = 0; i < dataForReading; ++i) {
    soundDriverData[i] = *cdData;
  }

  cdCmdDataTransferEnd();

  pcmsys_load_driver((void *) soundDriverData, soundDriverEntry->size);
  pcmStreamInitialize();
}


/*  RAM cycle pattern (bank A0) register */
#define CYCLE_A_REG 0x25f80010
#define CYCLE_A1_REG 0x25f80014

/*  VRAM cycle pattern (bank B0) register */
#define CYCLE_B_REG 0x25f80018
#define CYCLE_B1_REG 0x25f8001c

/* Turn on CPU read/write mode */
#define CYCLE_CPU_WRITE(reg_addr) \
  { *((uint32_t *) (reg_addr)) = 0xeeeeeeee; }

#define CYCLE_CPU_NOACCESS(reg_addr) \
  { *((uint32_t *) (reg_addr)) = 0xffffffff; }

/* Set to character pattern data read mode */
#define CYCLE_VDP_READ(reg_addr) \
  { *((uint32_t *) (reg_addr)) = 0x44444444; }


/* 1/2 */
#define SMP_DIV2(a) ((a) >> 1)
#define SCL_MAXLINE         512
#define SMPCPK_VBL_COPY_MAX     (320*120)
#define SMPCPK_VBL_COPY_MAX_QTR ((256 * 1024) / 4)
void copyVideoFrame2(uint32_t movie_x, uint32_t movie_y, uint32_t *src, uint32_t *dst, uint8_t color_depth) {

  int32_t copy_size;
  int32_t *src_stop1 = src + movie_x * SMP_DIV2(movie_y);
  int32_t *src_stop2 = src + movie_x * movie_y;
  
  if (color_depth == COLOR_DEPTH_15) {
    copy_size = 2 * movie_x;
  } else {
    copy_size = 4 * movie_x;
  }

	g_time_on = false;
	while (g_time_on == false);
	g_vbl_in = false;
    /* Set bank A0 to CPU write mode */
    CYCLE_CPU_WRITE(CYCLE_A_REG);
    while (src < src_stop1) {
      DMA_ScuMemCopy(dst, src, copy_size);
      src += movie_x;
      if (color_depth == COLOR_DEPTH_15) {
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
      if (color_depth == COLOR_DEPTH_15) {
        dst += SMP_DIV2(SCL_MAXLINE);
      } else {
        dst += SCL_MAXLINE;
      }
    }

	/* Make bank B0 a character pattern data read. */
    CYCLE_VDP_READ(CYCLE_B_REG);
  
}

void copyVideoFrame1(uint32_t movie_x, uint32_t movie_y, uint32_t *src, uint32_t *dst, uint8_t color_depth) {
  int32_t copy_size;

  if (color_depth == COLOR_DEPTH_15) {
    copy_size = 2 * movie_x;
  } else {
    copy_size = 4 * movie_x;
  }

  uint32_t *src_stop1 = src + movie_x * SMP_DIV2(movie_y);
  uint32_t *src_stop2 = src + movie_x * movie_y;

  g_time_on = false;
  while (g_time_on == false)
    ;
  g_vbl_in = false;

  /* Set bank A0 to CPU write mode */
  CYCLE_CPU_WRITE(CYCLE_A_REG);

  while (src < src_stop1) {
    DMA_ScuMemCopy(dst, src, copy_size);
    if (color_depth == COLOR_DEPTH_15) {
      src += SMP_DIV2(movie_x);
      dst += SMP_DIV2(SCL_MAXLINE);
    } else {
      src += movie_x;
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
    // src += movie_x;
    if (color_depth == COLOR_DEPTH_15) {
      src += SMP_DIV2(movie_x);
      dst += SMP_DIV2(SCL_MAXLINE);
    } else {
      src += movie_x;
      dst += SCL_MAXLINE;
    }
  }
  /* Make bank B0 a character pattern data read. */
  CYCLE_VDP_READ(CYCLE_B_REG);

}

void copyVideoFrame(uint32_t movie_x, uint32_t movie_y, uint32_t *src, uint32_t *dst, uint8_t color_depth) {

    if ((movie_x * movie_y) * 4  <= SMPCPK_VBL_COPY_MAX || color_depth == COLOR_DEPTH_15) {
        copyVideoFrame1(movie_x, movie_y, src, dst, color_depth);
    } else {
        copyVideoFrame2(movie_x, movie_y, src, dst, color_depth);
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
   //  cpu_cache_purge();

}

int main() {
  cdfs_init();
  cdfs_filelist_entry_t *const filelist_entries = cdfs_entries_alloc(-1);

  cdfs_filelist_init(&filelist, filelist_entries, -1);
  cdfs_filelist_root_read(&filelist);

  memset(sampleCache, 0, sizeof(film_sample_t *) * SAMPLE_CACHE_SIZE);
  cdfs_filelist_entry_t *movieEntries[MOVIE_LIST_ENTRIES];
  memset(movieEntries, 0, sizeof(cdfs_filelist_entry_t *) * MOVIE_LIST_ENTRIES);

  memset(vdp2DestinationBuffer, 0, (512 * 256) * 4);
 // user_init();
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
  scu_timer_t0_value_set((256 / 2) + 2);
  scu_timer_t0_set(_scu_timer_0_handler);
  scu_timer_line_enable();
  uint32_t movie_lx, movie_ly;
  uint32_t movie_x;
  uint32_t movie_y;
  uint32_t *vram_addr;

  g_vbl_in = false;
  g_time_on = false;

  movieSelected = true;
  restart = true;

  while (true) {
    smpc_peripheral_process();
    smpc_peripheral_digital_port(1, &pad0);

      if (restart == true) {
       // memset(vdp2DestinationBuffer, 0, (512 * 256) * 4);
        sprintf((char *) LWRAM(80), "FILM INIT START");
        memset(&work_area, 0, sizeof(decode_work_t));
        memset(&sampleCache, 0, SAMPLE_CACHE_SIZE * sizeof(film_sample_t));
        memset(&cpk->filmHeader, 0, sizeof(film_header));

        params.sampleBuffAddr = &sampleCache;
        params.sampleBuffSize = SAMPLE_CACHE_SIZE * sizeof(film_sample_t);
        params.vramBuffAddr = &decode_buffer;
        params.vramWritePos = &decode_buffer;
        params.vramBufferWidth = VIDEO_WIDTH;
        params.vramBufferHeight = VIDEO_HEIGHT;
        params.vramDelta = VIDEO_WIDTH * 3;
        params.vramBuffSize = (VIDEO_WIDTH * VIDEO_HEIGHT) * 4;
        params.decodeColorDepth = COLOR_DEPTH_24;
        //Currently not used, eventually should use these and set them to what's in the FILM Header. 
        params.pcmVolume = 7;
        params.pcmChannels = 2;
        params.pcmPan = 0;
        params.pcmTransferMode = PCM_XFER_SH2_DMA;
        // Currently not used, should be used eventually to allow the user to define what slots and addresses to use for audio.
        params.audioBufferAddr = getSlotAddress(0);
        params.audioBufferSize = getSlotSize();

        cpk->decodeParams = &params;
        cpk->play_status = 0;

        init_film_start(movieEntries[menuSelection], cpk, 240, 320);
        cpk->stream.sampleCache.pcmPlayPosition = getSlotAddress(0);      

        
        vdp2_sync();
        vdp2_sync_wait();
        restart = false;
      }      

      playback_status_t prevStatus = cpk->play_status;
      cpk_task(cpk);
      if (prevStatus == INIT && cpk->play_status == PLAY) {
        movie_x = cpk->filmHeader.fdsc.width;
        movie_y = cpk->filmHeader.fdsc.height;

        movie_lx = (VIDEO_WIDTH - movie_x) / 2;
        movie_ly = (VIDEO_HEIGHT - movie_y) / 2;
        vram_addr = (uint32_t *) (VDP2_VRAM_ADDR(0, 0) +
          4 * (SCL_MAXLINE * movie_ly + movie_lx));
      }

      if (cpk->isDisplayReady == true) {
        uint8_t colorDepth = cpk->decodeParams->decodeColorDepth;

        copyVideoFrame(movie_x, movie_y, decode_buffer, vram_addr, colorDepth);
        cpk_display_finished(cpk);
      }

      if (cpk->play_status == END) {
        menuSelection++;
        if (menuSelection > 2) {
          menuSelection = 0;
          memset(vdp2DestinationBuffer, 0, (512 * 256) * 4);
        }
        restart = true;

        pcmStreamStop();
        adx_dsp_stop();
        sound_notify_driver();

        film_audio_reset();
         vdp2_sync();
         vdp2_sync_wait();
      }
    

  }
}

void setVramSettingsText() {
  vdp2_vram_ctl_t vramCtl = {.vram_mode = VDP2_VRAM_CTL_MODE_NO_PART_BANK_A,
    .coeff_table = VDP2_VRAM_CTL_COEFF_TABLE_VRAM};

  vdp2_vram_control_set(&vramCtl);

  const vdp2_vram_cycp_t vram_cycp = {.pt[0].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t4 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t5 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t6 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[0].t7 = VDP2_VRAM_CYCP_CHPNDR_NBG0,

    .pt[1].t0 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t1 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t2 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t3 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[1].t7 = VDP2_VRAM_CYCP_NO_ACCESS,

    .pt[2].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t1 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t2 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t3 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t4 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t5 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t6 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
    .pt[2].t7 = VDP2_VRAM_CYCP_CHPNDR_NBG0,

    .pt[3].t0 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t1 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t2 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t3 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t7 = VDP2_VRAM_CYCP_NO_ACCESS};

  
    vdp2_vram_cycp_set(&vram_cycp);
}

void user_init(void) {
  const vdp2_scrn_bitmap_format_t format = {.scroll_screen = VDP2_SCRN_NBG0,
    // .ccc = VDP2_SCRN_CCC_RGB_32768,
    .ccc = VDP2_SCRN_CCC_RGB_16770000,
    .bitmap_size = VDP2_SCRN_BITMAP_SIZE_512X256,
    .palette_base = 0x00000000,
    .bitmap_base = VDP2_VRAM_ADDR(0, 0x00000)};

  vdp2_scrn_sf_set(
    VDP2_SCRN_NBG0, 0, VDP2_SCRN_SF_TYPE_NONE, VDP2_SCRN_SF_CODE_A);

  vdp2_scrn_bitmap_format_set(&format);
  vdp2_scrn_priority_set(VDP2_SCRN_NBG0, 3);
  vdp2_scrn_display_set(VDP2_SCRN_DISPTP_NBG0);

  setVramSettingsText();
   


#define NBG0_LINE_SCROLL VDP2_VRAM_ADDR(3, 0x10000)
#define NBG0_VCS         VDP2_VRAM_ADDR(3, 0x12000)

  const vdp2_scrn_ls_format_t ls_format = {.scroll_screen = VDP2_SCRN_NBG0,
    .table_base = NBG0_LINE_SCROLL,
    .interval = 0,
    .type = VDP2_SCRN_LS_TYPE_HORZ | VDP2_SCRN_LS_TYPE_VERT};

 // vdp2_scrn_ls_set(&ls_format);

  const vdp2_scrn_vcs_format_t vcs_format = {.scroll_screen = VDP2_SCRN_NBG0,
    .table_base = NBG0_VCS};

  //Undid the line scroll stuff for now to make the DMA easier to figure out.

 // vdp2_scrn_vcs_set(&vcs_format);

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

  vdp_sync_vblank_in_set(_vblank_in_handler, NULL);

  vdp_sync_vblank_out_set(_vblank_out_handler, NULL);

  cpu_frt_init(CPU_FRT_CLOCK_DIV_128);
  cpu_frt_interrupt_priority_set(15);

  vdp2_tvmd_display_set();

  smpc_peripheral_init();

  // Improve performance by ignoring HBLANK IN
  scu_ic_mask_chg(SCU_IC_MASK_ALL, SCU_IC_MASK_HBLANK_IN);
}

static void _vblank_in_handler(void *work __unused) { g_vbl_in = true;}

static void _vblank_out_handler(void *work __unused) {
  smpc_peripheral_intback_issue();
  canPress = true;
}

_scu_timer_0_handler(void) { g_time_on = true; }
