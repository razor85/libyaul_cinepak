#ifndef DECODER_H
#define DECODER_H

#include "base.h"

#define FILM_SAMPLE_START_OFFSET 64
#define SECTORS_PREFETCH         8

// 0 if video, 1 if audio.
#define FILM_SAMPLE_CHECK_BIT 0x80000000

#define ASCII_FILM 1179208781 // 'FILM'
#define ASCII_1d09 825110585  // '1.09'
#define ASCII_FDSC 1178882883 // 'FDSC'
#define ASCII_CVID 1668704612 // 'cvid'
#define ASCII_STAB 1398030658 // 'STAB'

#define MAX_STRIPS 2

typedef enum {
  ERROR = -1,
  STOP = 0,
  PAUSE = 1,
  PLAY = 2,
  END = 3,
  INIT = 4
} playback_status_t;

typedef struct {
  uint8_t y[4];
  int8_t u;
  int8_t v;
} codebook_t;

typedef struct {
  uint16_t color[4];
  uint32_t color24[4];
} codebookRGB_t;

typedef struct {
  codebook_t v1[256];
  codebook_t v4[256];
  codebookRGB_t v1RGB[256];
  codebookRGB_t v4RGB[256];
} __aligned(16) strip_codebook_t;

// Sample data can be stored as a single uint32_t with the MSB telling us if its
// a video (0) or audio sample (1). If it is a video sample, the rest of the
// data holds the 'info2' field from the STAB data and if its audio, it holds
// the sample length.
typedef struct {
  uint8_t *offset;
  uint32_t interval; // 0xFFFFFFFF if audio
  uint32_t length;
  uint32_t padding;
} __packed __aligned(4) film_sample_t;

// A sample stored in the STAB table is:
typedef struct {
  uint32_t offset;
  uint32_t length;
  uint32_t info1;
  uint32_t info2;
} __packed __aligned(4) cd_film_sample_t;

typedef struct {
  uint32_t numSamples;
  uint32_t currentSample;
  uint32_t currentBuffSample;
  uint8_t *ringBuffStart;
  uint8_t *ringBuffEnd;
  uint8_t *writePos;
  uint8_t *readPos;
  film_sample_t *samples;
} film_sample_cache_t;

typedef struct {
  uint32_t startFAD;
  uint32_t size;
  uint32_t remainingSectors;

  uint32_t dataAvailable;
  uint32_t offset;

  film_sample_cache_t sampleCache;

  bool eof;

} binary_stream_t;

typedef struct {
  strip_codebook_t codebooks[MAX_STRIPS];
  uint32_t strip;

  uint32_t topX;
  uint32_t writeX;
  uint32_t bottomX;

  uint32_t topY;
  uint32_t writeY;
  uint32_t bottomY;

} __packed __aligned(4) stripdata_t;

typedef struct {
  uint32_t offset;
  int32_t size;
  int32_t play_tick;
  int32_t duration_ticks;
} __packed __aligned(
  4) stab_entry; // same as cd_film_sample_t, but this is what the actual
                 // variables are referred to in SBL.

typedef struct {
  char stab_str[4];
  uint32_t stab_size;
  uint32_t ticks_per_second;
  uint32_t total_entries;
  cd_film_sample_t entries[0];
} __packed __aligned(4) stab_table;

typedef struct {
  char fdsc_str[4];
  uint32_t fdsc_size;
  char fourcc[4];
  uint32_t height;
  uint32_t width;
  uint8_t color_depth;      // Usually 24
  uint8_t sound_channels;   // 1 or 2
  uint8_t sound_resolution; // 8 bit or 16 bit
  uint8_t sound_codec;      // 0 = PCM, 1 = Sega ADPCM, 2= ADX
  uint32_t sample_rate;
  uint32_t chroma_key;
} __packed __aligned(4) frame_description;

typedef struct {
  char film_str[4];
  uint32_t header_size;
  uint32_t version;
  uint32_t reserved;
  frame_description fdsc;
  stab_table stab;
} __packed __aligned(4) film_header;

typedef struct {
  uint32_t *sampleBuffAddr;
  uint32_t sampleBuffSize;
  uint32_t *vramBuffAddr;
  uint32_t vramBuffSize;
  uint16_t vramBufferWidth;
  uint32_t audioBufferAddr;
  uint32_t audioBufferSize;
  bool audioEnable;
  uint8_t pcmChannels;
  uint8_t pcmVolume;
  uint8_t pcmPan;
} __aligned(4) decode_param_t;

typedef struct {
  decode_param_t *decodeParams;
  playback_status_t play_status;
  bool isDisplayReady;
  uint32_t timeEllapsed;
  uint32_t frtOverflowCount;
  uint32_t ticksUntilNextFrame;
  uint32_t tickCount;
  uint32_t lastFrameTime;
  uint32_t copyingVideoFrame;
  uint32_t videoStartY;
  uint32_t dma_delta;
  film_header filmHeader;
  binary_stream_t stream;
  film_sample_t nextSample;
  stripdata_t stripData;
} __aligned(4) decode_work_t;

extern void initialize_film();

extern uint32_t film_audio_get_next_buffer_size();

extern uint16_t *film_audio_get_next_buffer_ptr(uint8_t slot);

extern void film_audio_notify_read_buffer_bytes(uint32_t length);

extern void film_audio_play(uint32_t bufferLength);

extern void film_audio_setup(
  uint16_t frequency, uint32_t numChannels, uint32_t sampleResolution);

extern void film_audio_prepare_to_play();

// Return 0 to stop film playback
extern int film_loop_handler();

extern void init_film(cdfs_filelist_entry_t *fsEntry, decode_work_t *work,
  uint32_t vdp_height, uint32_t vdp_width);

extern void cpk_play(decode_work_t *work);

extern void cpk_task(decode_work_t *work);

extern bool cpk_display_ready(decode_work_t *work);

extern void cpk_display_finished(decode_work_t *work);

extern void copyVideoFrame(decode_work_t *work);

#endif // DECODER_H
