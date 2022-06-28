#ifndef DECODER_H
#define DECODER_H

#include "base.h"

// Maximum frame length is 65536 and we might have audio + video
#define DATA_CACHE_SIZE (CDFS_SECTOR_SIZE * 64)

// Fixed
#define FILM_SAMPLE_START_OFFSET 64

#define FILM_SAMPLE_CACHE_COUNT 128
#define FILM_SAMPLE_CACHE_SIZE (FILM_SAMPLE_CACHE_COUNT * sizeof(film_sample_t))

#define CDFS_DATA_SELECTOR 0
#define CDFS_SAMPLE_SELECTOR 1

#define ASCII_FILM 1179208781 // 'FILM'
#define ASCII_1d09 825110585  // '1.09'
#define ASCII_FDSC 1178882883 // 'FDSC'
#define ASCII_CVID 1668704612 // 'cvid'
#define ASCII_STAB 1398030658 // 'STAB'

typedef struct {
  uint32_t offset;
  uint32_t length;
  uint32_t info1;
  uint32_t info2;
} __packed __aligned(4) film_sample_t;

typedef struct {
  // Front and back cache of film samples, front is always used and back
  // is reserved for transfers. When front is completely read we swap.
  film_sample_t cache[2][FILM_SAMPLE_CACHE_COUNT];
  uint32_t numCacheSamples;

  // 0 or 1
  uint32_t frontIndex;

  // Index of the current sample being read.
  uint32_t currentSample;

  // Number of samples prepared to be copied from cd.
  uint32_t numCopySamples;

  // Index to the sector containing the next set of sample descriptions.
  uint32_t nextCacheSamplePos;

  // Number of samples we still need to read to the cache.
  uint32_t numPendingSamples;

} __packed __aligned(4) film_sample_cache_t;

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
  uint16_t strip;

  uint16_t topY;
  uint16_t writeY;

  uint16_t topX;
  uint16_t writeX;

  uint16_t bottomY;
  uint16_t bottomX;
} stripdata_t;

extern void initialize_film();

extern void play_film(cdfs_filelist_entry_t *fsEntry, void *dataCache0,
  void *dataCache1);

#endif // DECODER_H
