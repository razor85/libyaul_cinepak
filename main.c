#include "base.h"
#include "decoder.h"

static void _vblank_out_handler(void *work __unused);

static void _frt_ovi_handler(void);

static smpc_peripheral_digital_t _digital;

static cdfs_filelist_t filelist;

static uint16_t _frt_overflow_count = 0;
  
static uint8_t dataCaches[DATA_CACHE_SIZE * 2];

void clearConsole() { dbgio_printf("[H[2J"); }

int main() {
  /* Load the maximum number. We have to free the allocated filelist
   * entries, but since we never exit, we don't have to */
  cdfs_filelist_entry_t *const filelist_entries = cdfs_entries_alloc(-1);
  DEBUG_REQUIRE(filelist_entries != NULL);

  cdfs_filelist_default_init(&filelist, filelist_entries, -1);

  cdfs_filelist_root_read(&filelist);

  const char* desiredFile = "APPLE.CPK";
  // const char* desiredFile = "SONIC.CPK";

  // Find test entry
  cdfs_filelist_entry_t* testEntry = NULL;
  for (uint32_t i = 0; i < filelist.entries_count; ++i) {
    if (strcmp(desiredFile, filelist.entries[i].name) == 0) {
      testEntry = &filelist.entries[i];
      break;
    }
  }

  while (true) {
    smpc_peripheral_process();
    smpc_peripheral_digital_port(1, &_digital);

    clearConsole();
    play_film(testEntry, dataCaches, &dataCaches[DATA_CACHE_SIZE]);

    dbgio_flush();
    vdp2_sync();
    vdp2_sync_wait();
  }
}

void user_init(void) {
  const vdp2_scrn_bitmap_format_t format = { .scroll_screen = VDP2_SCRN_NBG0,
    .cc_count = VDP2_SCRN_CCC_RGB_32768,
    .bitmap_size.width = 512,
    .bitmap_size.height = 256,
    .color_palette = 0x00000000,
    .bitmap_pattern = VDP2_VRAM_ADDR(0, 0x00000),
    .sf_type = VDP2_SCRN_SF_TYPE_NONE,
    .sf_code = VDP2_SCRN_SF_CODE_A,
    .sf_mode = 0 };

  vdp2_scrn_bitmap_format_set(&format);
  vdp2_scrn_priority_set(VDP2_SCRN_NBG0, 7);
  vdp2_scrn_display_set(VDP2_SCRN_NBG0_DISP);

  const vdp2_vram_cycp_t vram_cycp = { .pt[0].t0 = VDP2_VRAM_CYCP_CHPNDR_NBG0,
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

    .pt[2].t0 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t1 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t2 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t3 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[2].t7 = VDP2_VRAM_CYCP_NO_ACCESS,

    .pt[3].t0 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t1 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t2 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t3 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t4 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t5 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t6 = VDP2_VRAM_CYCP_NO_ACCESS,
    .pt[3].t7 = VDP2_VRAM_CYCP_NO_ACCESS };

  vdp2_vram_cycp_set(&vram_cycp);

  vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A,
    VDP2_TVMD_VERT_240);

  vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE),
    COLOR_RGB1555(1, 0, 3, 15));

  vdp_sync_vblank_out_set(_vblank_out_handler, NULL);

  cpu_frt_init(CPU_FRT_CLOCK_DIV_32);

  dbgio_init();
  dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
  dbgio_dev_font_load();

  vdp2_tvmd_display_set();

  cd_block_init();

  smpc_peripheral_init();
}

static void _vblank_out_handler(void *work __unused) {
  smpc_peripheral_intback_issue();
}

static void _frt_ovi_handler(void) { _frt_overflow_count++; }
