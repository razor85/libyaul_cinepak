#ifndef DECODER_H
#define DECODER_H

#include "base.h"

// Maximum frame length is 65536 and we might have audio + video
#define DATA_CACHE_SIZE (CDFS_SECTOR_SIZE * 64)

#define FILM_SAMPLE_START_OFFSET 64
#define FILM_SAMPLE_CHECK_BIT 0x80000000

#define CDFS_DATA_SELECTOR 0
#define CDFS_SAMPLE_SELECTOR 1

#define ASCII_FILM 1179208781 // 'FILM'
#define ASCII_1d09 825110585  // '1.09'
#define ASCII_FDSC 1178882883 // 'FDSC'
#define ASCII_CVID 1668704612 // 'cvid'
#define ASCII_STAB 1398030658 // 'STAB'

// Sample data can be stored as a single uint32_t with the MSB telling us if its
// a video (0) or audio sample (1). If it is a video sample, the rest of the
// data holds the 'info2' field from the STAB data and if its audio, it holds
// the sample length.
typedef uint32_t film_sample_t;

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
  uint32_t pos;
  uint32_t relPos;
  uint32_t endPos;
  uint32_t size;
  uint8_t *data;
} __packed __aligned(4) data_cache_t;

typedef struct {
  uint32_t startFAD;
  uint32_t size;
  
  // Active data cache.
  uint32_t activeCacheIndex;

  // Either point to dataCache[0]->data or dataCache[1]->data.
  uint8_t *dataPtr;
  data_cache_t dataCaches[2];

  // Sample descriptions.
  film_sample_cache_t sampleCache;
  
  // Will be != 0 if there is more data on the next buffer.
  uint32_t remainingSize;

  bool eof;

} binary_stream_t;

#define MAX_STRIPS 16

typedef struct {
  uint8_t y[4];
  int8_t u;
  int8_t v;
} codebook_t;

typedef struct {
  codebook_t v1[256];
  codebook_t v4[256];
} __aligned(16) strip_codebook_t;

typedef struct {
  strip_codebook_t codebooks[MAX_STRIPS];
  uint32_t strip;

  uint32_t topY;
  uint32_t writeY;

  uint32_t topX;
  uint32_t writeX;

  uint32_t bottomY;
  uint32_t bottomX;
} stripdata_t;

extern void initialize_film();

extern void play_film(cdfs_filelist_entry_t *fsEntry, void *dataCache0,
  void *dataCache1, void* sampleCache, uint32_t sampleCacheSize);

#endif // DECODER_H
