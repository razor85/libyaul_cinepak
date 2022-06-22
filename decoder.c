#include "decoder.h"
#include "md5.h"

#ifndef MIN
#define MIN(X, Y) (X < Y ? X : Y)
#endif

#ifndef MAX
#define MAX(X, Y) (X > Y ? X : Y)
#endif

#define COPY_NOT_WAITING 0xFF
int consolePrints = 0;

inline uint32_t numSectorsForSize(uint32_t size) {
  return (size + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
}

int queueDiskRead(uint8_t selector, uint32_t fad, uint32_t size,
  uint8_t *queuedDestinationBuffer) {

  int status;
  if ((status = cd_block_cmd_sector_length_set(SECTOR_LENGTH_2048)) != 0) {
    return status;
  }

  if ((status = cd_block_cmd_selector_reset(0, selector)) != 0) {
    return status;
  }

  if ((status = cd_block_cmd_cd_dev_connection_set(0)) != 0) {
    return status;
  }

  const uint32_t numSectors = numSectorsForSize(size);
  if ((status = cd_block_cmd_disk_play(0, fad, numSectors)) != 0) {
    return status;
  }

  uint8_t cdStatus;
  if ((status = cd_block_cmd_last_buffer_destination_get(&cdStatus,
         queuedDestinationBuffer)) != 0) {

    return status;
  }

  return 0;
}

void readQueuedCopy(uint8_t cdBuffer, void *destination, uint32_t size) {
  uint8_t *outputBuffer = (uint8_t *) destination;

  uint32_t bytes_missing = size;
  while (bytes_missing) {
    /* Wait until there's data ready */
    uint32_t sectors_ready;
    do {
      sectors_ready = cd_block_cmd_sector_number_get(cdBuffer);
    } while (sectors_ready == 0);

    uint32_t bytes_to_read;
    if ((sectors_ready * CDFS_SECTOR_SIZE) > bytes_missing) {
      bytes_to_read = bytes_missing;
    } else {
      bytes_to_read = sectors_ready * CDFS_SECTOR_SIZE;
    }

    int status = cd_block_transfer_data(0, cdBuffer, outputBuffer,
      bytes_to_read);

    DEBUG_REQUIRE_EQ(status, 0);
    outputBuffer += bytes_to_read;
    bytes_missing -= bytes_to_read;
  }
}

// stream must be exactly at the start of the sample descriptions
void film_sample_cache_new(film_sample_cache_t *cache,
  binary_stream_t *stream, uint32_t totalNumSamples) {

  cache->frontIndex = 0;
  cache->currentSample = 0;
  cache->numCopySamples = 0;
  cache->numCacheSamples = (CDFS_SECTOR_SIZE - stream->pos) / sizeof(film_sample_t);
  if (cache->numCacheSamples > totalNumSamples)
    cache->numCacheSamples = totalNumSamples;

  cache->numPendingSamples = totalNumSamples - cache->numCacheSamples;

  dbgio_printf("Started with %d samples\n", cache->numCacheSamples);
  stream_readbytes(stream, cache->cache[0],
    cache->numCacheSamples * sizeof(film_sample_t));

  if (cache->numPendingSamples) {
    cache->nextCacheSampleFAD = stream->fs->starting_fad + 1;
    cache->numCopySamples = MIN(cache->numPendingSamples,
      FILM_SAMPLE_CACHE_COUNT);

    cache->numPendingSamples -= cache->numCopySamples;
    int status = queueDiskRead(CDFS_SAMPLE_SELECTOR, cache->nextCacheSampleFAD,
      cache->numCopySamples * sizeof(film_sample_t), &cache->cdCopyBuffer);

    /*
    if (consolePrints++ > 20)
      dbgio_printf("[H[2J");

    dbgio_printf("Queue reading %d samples, still %d pending\n",
      cache->numCopySamples, cache->numPendingSamples);
    */

    DEBUG_REQUIRE_EQ(status, 0);
  }
}

// Returns NULL if there are no more samples
film_sample_t *film_sample_get_next_sample(film_sample_cache_t *cache) {
  // Time to swap?
  if (cache->currentSample >= cache->numCacheSamples) {
    // If there is nothing else to read and no data available on the back
    // buffer, return NULL.
    if (!cache->numPendingSamples && !cache->numCopySamples)
      return NULL;

    // Swap and schedule the new transfer.
    cache->frontIndex ^= 1;
    cache->currentSample = 0;
    cache->numCacheSamples = cache->numCopySamples;

    // TODO: replace by DMA
    if (cache->numCopySamples) {
      // dbgio_printf("Copying %d to buffer %d...", cache->numCopySamples,
      //   (int)cache->frontIndex);
      // dbgio_flush();

      const uint32_t copySize = cache->numCopySamples * sizeof(film_sample_t);
      readQueuedCopy(cache->cdCopyBuffer, &cache->cache[cache->frontIndex],
        copySize);

      cache->nextCacheSampleFAD += numSectorsForSize(copySize);
      cache->numCopySamples = 0;
      // dbgio_printf("Done\n");
      // dbgio_flush();
    }

    // Get new samples from the disk
    if (cache->numPendingSamples > 0) {
      cache->numCopySamples = MIN(cache->numPendingSamples,
        FILM_SAMPLE_CACHE_COUNT);

      cache->numPendingSamples -= cache->numCopySamples;

      const uint32_t queuedSize = cache->numCopySamples * sizeof(film_sample_t);
      const int status = queueDiskRead(CDFS_SAMPLE_SELECTOR,
        cache->nextCacheSampleFAD, queuedSize, &cache->cdCopyBuffer);
    
      /*
      if (consolePrints++ > 20)
        dbgio_printf("[H[2J");

      dbgio_printf("Queue reading %d samples, still %d pending\n",
        cache->numCopySamples, cache->numPendingSamples);
      */

      DEBUG_REQUIRE_EQ(status, 0);
    }
  }
    
  return &cache->cache[cache->frontIndex][cache->currentSample++];
}

// Return the current sample, don't do anything else.
film_sample_t *film_sample_get_sample(film_sample_cache_t *cache) {
  return &cache->cache[cache->frontIndex][cache->currentSample];
}

void data_cache_fetch_next_sample_data(binary_stream_t *stream, int firstFetch,
  uint32_t sampleDataPos) {

  data_cache_t* cache = &stream->dataCache;

  // First fetch is special because it blocks and completely discards the
  // content of the previous buffer.
  if (firstFetch) {
    const uint32_t startFAD = stream->fs->starting_fad +
      (sampleDataPos / CDFS_SECTOR_SIZE);
    
    DEBUG_REQUIRE_GE(stream->fs->size, sampleDataPos);

    uint32_t totalReadBytes = stream->fs->size - sampleDataPos;
    if (totalReadBytes > DATA_CACHE_SIZE)
      totalReadBytes = DATA_CACHE_SIZE;

    // Queue up copy
    int status = queueDiskRead(CDFS_DATA_SELECTOR, startFAD, totalReadBytes,
      &cache->cdCopyBuffer);

    DEBUG_REQUIRE_EQ(status, 0);

    readQueuedCopy(cache->cdCopyBuffer, cache->cache[0], totalReadBytes);
    stream->pos = sampleDataPos;
    stream->relPos = sampleDataPos % CDFS_SECTOR_SIZE;
    stream->dataPtr = cache->cache[0];
    stream->remainingSize = stream->fs->size - totalReadBytes;
    stream->nextDataPos = sampleDataPos + totalReadBytes;

  } else if (stream->remainingSize > 0) {
    const uint32_t startFAD = stream->fs->starting_fad +
      (stream->nextDataPos / CDFS_SECTOR_SIZE);

    DEBUG_REQUIRE_GE(stream->fs->size, stream->nextDataPos);

    uint32_t totalReadBytes = stream->fs->size - stream->nextDataPos;
    if (totalReadBytes > DATA_CACHE_SIZE)
      totalReadBytes = DATA_CACHE_SIZE;

    // Queue up copy
    int status = queueDiskRead(CDFS_DATA_SELECTOR, startFAD, totalReadBytes,
      &cache->cdCopyBuffer);

    DEBUG_REQUIRE_EQ(status, 0);
    stream->remainingSize -= totalReadBytes;
    stream->nextDataPos += totalReadBytes;
  }
}

void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  void *dataCache0, void *dataCache1) {

  const uint32_t initialDataSize = MIN(entry->size, CDFS_SECTOR_SIZE);

  stream->fs = entry;
  stream->pos = 0;
  stream->relPos = 0;
  stream->remainingSize = stream->fs->size;
  stream->remainingBytesToNextSection = 0;

  stream->dataCache.frontIndex = 0;
  stream->dataCache.cache[0] = (uint8_t *)dataCache0;
  stream->dataCache.cacheSize[0] = initialDataSize;
  stream->dataCache.cache[1] = (uint8_t *)dataCache1;

  stream->dataPtr = stream->dataCache.cache[stream->dataCache.frontIndex];

  dbgio_printf("Starting with %d bytes...\n", initialDataSize);

  int status = cd_block_sectors_read(entry->starting_fad, dataCache0,
    initialDataSize);

  DEBUG_REQUIRE_EQ(status, 0);
}

uint8_t stream_read8(binary_stream_t *stream) {
  // TODO: romulussss
  DEBUG_REQUIRE_LT(stream->relPos + 1, stream->fs->size);
  const uint8_t data = stream->dataPtr[stream->relPos];
  stream->relPos++;
  stream->pos++;
  stream->remainingBytesToNextSection--;
  return data;
}

uint16_t stream_read16(binary_stream_t *stream) {
  DEBUG_REQUIRE_LT(stream->relPos + 2, stream->fs->size);
  const uint16_t data = *((uint16_t *)&stream->dataPtr[stream->relPos]);
  stream->relPos += 2;
  stream->pos += 2;
  stream->remainingBytesToNextSection -= 2;
  return data;
}

uint32_t stream_read24(binary_stream_t *stream) {
  DEBUG_REQUIRE_LT(stream->relPos + 3, stream->fs->size);
  uint32_t data = 0;
  data |= stream->dataPtr[stream->relPos++];
  data |= stream->dataPtr[stream->relPos++] << 8;
  data |= stream->dataPtr[stream->relPos++] << 16;

  stream->pos += 3;
  stream->remainingBytesToNextSection -= 3;
  return data;
}

uint32_t stream_read32(binary_stream_t *stream) {
  DEBUG_REQUIRE_LT(stream->relPos + 4, stream->fs->size);
  const uint32_t data = *((uint32_t *)&stream->dataPtr[stream->relPos]);
  stream->relPos += 4;
  stream->pos += 4;
  stream->remainingBytesToNextSection -= 4;
  return data;
}

void stream_readbytes(binary_stream_t *stream, void *dst, uint32_t len) {
  DEBUG_REQUIRE_LT(stream->relPos + len, stream->fs->size);
  memcpy(dst, &stream->dataPtr[stream->relPos], len);
  stream->relPos += len;
  stream->pos += len;
  stream->remainingBytesToNextSection -= len;
}

void stream_skip(binary_stream_t *stream, uint32_t len) {
  DEBUG_REQUIRE_LT(stream->relPos + len, stream->fs->size);
  stream->relPos += len;
  stream->pos += len;
  stream->remainingBytesToNextSection -= len;
}

int film_sample_is_video(film_sample_t *sample) {
  return sample->info1 != 0xFFFFFFFF;
}

int film_sample_is_audio(film_sample_t *sample) {
  return !film_sample_is_video(sample);
}

int film_sample_is_keyFrame(film_sample_t *sample) {
  return film_sample_is_video(sample) && (!(sample->info1 & 0x1));
}

void film_read_sample(film_sample_t *sample) {
  dbgio_printf("Found %s sample at %d offset and %d length (infos: 0x%X [%d] "
               "/ 0x%X [%d])\n",
    film_sample_is_audio(sample) ? "audio" : "video", sample->offset,
    sample->length, sample->info1, sample->info1, sample->info2, sample->info2);

  if (film_sample_is_video(sample))
    dbgio_printf("  KeyFrame = %d\n", film_sample_is_keyFrame(sample));
}

void play_film(cdfs_filelist_entry_t *entry, void *dataCache0,
  void *dataCache1) {

  DEBUG_REQUIRE(entry != NULL);
  dbgio_printf("%s\n", entry->name);

  // Start reading the file
  binary_stream_t stream;
  stream_new(&stream, entry, dataCache0, dataCache1);

  DEBUG_REQUIRE_EQ(stream_read32(&stream), ASCII_FILM);
  const uint32_t filmHeaderLength = stream_read32(&stream);

  const uint32_t filmVersion = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(filmVersion, ASCII_1d09);

  stream_skip(&stream, 4); // Unknown
  
  DEBUG_REQUIRE_EQ(stream_read32(&stream), ASCII_FDSC);
  const uint32_t fdscLength = stream_read32(&stream);
  
  DEBUG_REQUIRE_EQ(stream_read32(&stream), ASCII_CVID);
  const uint32_t videoHeight = stream_read32(&stream);
  const uint32_t videoWidth = stream_read32(&stream);

  const uint8_t videoBPP = stream_read8(&stream);
  const uint8_t audioChannels = stream_read8(&stream);
  const uint8_t audioSamplingResolution = stream_read8(&stream);
  const uint8_t audioCompression = stream_read8(&stream);
  const uint16_t audioSamplingFrequencyHz = stream_read16(&stream);

  dbgio_printf("Found %dx%d@%dbpp video\n", videoWidth, videoHeight, videoBPP);
  dbgio_printf("Audio: %d channels, %d bits (comp = %d), %d Hz\n",
    audioChannels, audioSamplingResolution, audioCompression,
    audioSamplingFrequencyHz);
  
  stream_skip(&stream, 6); // Unknown

  DEBUG_REQUIRE_EQ(stream_read32(&stream), ASCII_STAB);
  const uint32_t stabLength = stream_read32(&stream);
  const uint32_t framerateBaseFrequencyHz = stream_read32(&stream);
  const uint32_t numSamples = stream_read32(&stream);

  dbgio_printf("Frame rate base frequency: %d Hz with %d samples\n",
    framerateBaseFrequencyHz, numSamples);
  
  const uint32_t sampleDescriptionPos = stream.pos;
  const uint32_t sampleDataPos = filmHeaderLength;

  // Must be called just before the sample list
  film_sample_cache_new(&stream.sampleCache, &stream, numSamples);
  
  // Queue up first data reading before we can start to process the frames
  // data_cache_fetch_next_sample_data(&stream, 1, sampleDataPos);
  dbgio_printf("SamplePos: %d\nSampleDataPos: %d\nPos: %d\nRelPos: %d\n",
    sampleDescriptionPos, sampleDataPos, stream.pos, stream.relPos);
    
  dbgio_flush();
  vdp2_sync();
  vdp2_sync_wait();

  // Calculate MD5 of frame description data
  /*
  MD5_CTX md5;
  MD5_Init(&md5);

  for (uint32_t sampleId = 0; sampleId < numSamples; ++sampleId) {
    film_sample_t* sample = film_sample_get_next_sample(&stream.sampleCache);
    DEBUG_REQUIRE_NE(sample, NULL);

    MD5_Update(&md5, (void*) sample, sizeof(film_sample_t));
    if (sampleId % 1024 == 0)
      dbgio_printf("Updating...\n");

    dbgio_flush();
    vdp2_sync();
    vdp2_sync_wait();
  }

  unsigned char md5Result[2048];
  MD5_Final(md5Result, &md5);

  dbgio_printf("MD5 is\n");
  for (uint32_t i = 0; i < MD5_DIGEST_LENGTH; i++)
    dbgio_printf("%02x", md5Result[i]);

  VDP_INFLOOP();
  */

  uint32_t videoFrame = 0;
  for (uint32_t sampleId = 0; sampleId < numSamples; ++sampleId) {
    film_sample_t* sample = film_sample_get_next_sample(&stream.sampleCache);
    DEBUG_REQUIRE_NE(sample, NULL);

    film_read_sample(sample);

    // file.seek(sampleDataPos + sample.offset);
    // parseSample(sample, file);

    VDP_INFLOOP();
    dbgio_flush();
    vdp2_sync();
    vdp2_sync_wait();
  }

  VDP_INFLOOP();
}

