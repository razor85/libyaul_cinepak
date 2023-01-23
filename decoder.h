#ifndef DECODER_H
#define DECODER_H

#include "base.h"

#define FILM_SAMPLE_START_OFFSET 64
#define SECTORS_PREFETCH 8

// 0 if video, 1 if audio.
#define FILM_SAMPLE_CHECK_BIT 0x80000000

#define ASCII_FILM 1179208781 // 'FILM'
#define ASCII_1d09 825110585  // '1.09'
#define ASCII_FDSC 1178882883 // 'FDSC'
#define ASCII_CVID 1668704612 // 'cvid'
#define ASCII_STAB 1398030658 // 'STAB'

// Sample data can be stored as a single uint32_t with the MSB telling us if its
// a video (0) or audio sample (1). If it is a video sample, the rest of the
// data holds the 'info2' field from the STAB data and if its audio, it holds
// the sample length.
typedef struct {
  uint32_t interval; // 0xFFFFFFFF if audio
  uint32_t length;
} __packed __aligned(4) film_sample_t;

// A sample stored in the STAB table is:
typedef struct {
  uint32_t offset;
  uint32_t length;
  uint32_t info1;
  uint32_t info2;
} __packed __aligned(4) cd_film_sample_t;

typedef struct {
  film_sample_t *samples;
  uint32_t numSamples;
  uint32_t currentSample;
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

#define MAX_STRIPS 16

typedef struct {
  uint8_t y[4];
  int8_t u;
  int8_t v;
} codebook_t;

typedef struct {
  uint16_t color[4];
} codebook555_t;

typedef struct {
  codebook_t v1[256];
  codebook_t v4[256];
  codebook555_t v1RGB[256];
  codebook555_t v4RGB[256];
} __aligned(16) strip_codebook_t;

typedef struct {
  strip_codebook_t codebooks[MAX_STRIPS];
  uint32_t strip;

  uint32_t topX;
  uint32_t writeX;
  uint32_t bottomX;

  uint32_t topY;
  uint32_t writeY;
  uint32_t bottomY;

} stripdata_t;

extern void initialize_film();

extern uint32_t film_audio_get_next_buffer_size();

extern uint16_t* film_audio_get_next_buffer_ptr(uint8_t slot);

extern void film_audio_notify_read_buffer_bytes(uint32_t length);

extern void film_audio_play(uint32_t bufferLength);

extern void film_audio_setup(uint32_t frequency, uint32_t numChannels, uint32_t sampleResolution);

extern void film_audio_prepare_to_play();

// Return 0 to stop film playback
extern int film_loop_handler();

extern void play_film(cdfs_filelist_entry_t *fsEntry, film_sample_t *sampleCache,
  uint32_t sampleCacheSize);

#endif // DECODER_H
