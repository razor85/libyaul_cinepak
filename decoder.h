#ifndef DECODER_H
#define DECODER_H

#include "base.h"

#define DATA_CACHE_SIZE (CDFS_SECTOR_SIZE * 16)

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
} film_sample_t;

#define FILM_SAMPLE_CACHE_COUNT 128
#define FILM_SAMPLE_CACHE_SIZE (FILM_SAMPLE_CACHE_COUNT * sizeof(film_sample_t))

typedef struct {
  // Front and back cache of film samples, front is always used and back
  // is reserved for transfers. When front is completely read we swap.
  film_sample_t cache[2][FILM_SAMPLE_CACHE_COUNT];
  uint8_t numCacheSamples;

  // 0 or 1
  uint8_t frontIndex;

  // Index of the current sample being read
  uint8_t currentSample;

  // If there is a copy in progress for the back buffer it will store
  // the buffer index, otherwise 0xFF
  uint8_t cdCopyBuffer;
  uint8_t numCopySamples;

  // Index to the sector containing the next set of sample descriptions.
  uint32_t nextCacheSampleFAD;

  // Number of samples we still need to read to the cache.
  uint32_t numPendingSamples;

} __packed __aligned(4) film_sample_cache_t;

typedef struct {
  // How many bytes we store in each pointer.
  uint32_t cacheSize[2];

  // Used like a double buffer, background reads will happen in the buffer
  // that is not being used.
  uint8_t *cache[2];

  // 0 or 1.
  uint8_t frontIndex;

  // Next time we queue up something to copy, we store the buffer that was used
  // [1, 199] (0 is reserved for samples) and how much data we queued.
  uint8_t cdCopyBuffer;
  uint8_t queuedSectors;

  // Next read for data.
  uint32_t nextCacheFAD;

} __packed __aligned(4) data_cache_t;

typedef struct {
  cdfs_filelist_entry_t *fs;

  // Pointer used to read the data, it will point either to dataCache[0] or
  // dataCache[1].
  uint8_t *dataPtr;

  data_cache_t dataCache;

  film_sample_cache_t sampleCache;
  
  // Position relative to the data cache
  uint32_t relPos;

  // Position relative to the beginning of the file
  uint32_t pos;

  // Will be != 0 if there is more data on the next buffer.
  uint32_t remainingSize;

  // Offset from the beginning of the file where the next data should be.
  uint32_t nextDataPos;

  // Used as a helper for section, might remove.
  int32_t remainingBytesToNextSection;
  
} binary_stream_t;

extern void play_film(cdfs_filelist_entry_t *fsEntry, void *dataCache0,
  void *dataCache1);

#endif // DECODER_H
