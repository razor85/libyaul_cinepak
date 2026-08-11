#include "film_buff.h"
#include "cd.h"


#ifndef MIN
#  define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

static scu_dma_handle_t cd_dma = {.dnr = (uintptr_t) 0x25818000UL,
  .dnw = 0,
  .dnc = 0,
  .dnad = 0x00000002,
  .dnmd = 0x00000000};

// stream must be exactly at the start of the sample descriptions. 
void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples) {
  film_sample_t *outputSamples = stream->sampleCache.samples;

  stream->sampleCache.ringBuffStart = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));
  stream->sampleCache.readPos = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));
  stream->sampleCache.writePos  = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));

  for (uint32_t i = 0; i < totalNumSamples; i++) {
    film_sample_t sample = {0};

    stream_readbytes(stream, (uint16_t *) &sample, sizeof(film_sample_t));

    film_sample_t *newSample = &outputSamples[i];
    newSample->offset = 0;
    newSample->length = sample.length;
    newSample->time = sample.time;
    newSample->duration = sample.duration;
  }

  initRingBuffer(stream);
}

void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  film_sample_t *sampleCache, uint32_t sampleCacheSize) {
  stream->startFAD = entry->starting_fad;
  stream->size = entry->size;
  stream->remainingSectors = numSectorsForSize(entry->size);
  stream->dataAvailable = 0;
  stream->offset = 0;

  stream->sampleCache.samples = sampleCache;
  stream->sampleCache.numSamples = sampleCacheSize;
  stream->sampleCache.currentSample = 0;
  stream->sampleCache.currentBuffSample = 0;
  stream->eof = false;
  stream->sampleCache.ringBuffEnd = (uint8_t *) sampleCache + sampleCacheSize;

  int endStatus __unused = cd_block_cmd_data_transfer_end();

  const uint32_t sectorsReady =
    getSectorsReady(MIN(stream->remainingSectors, SECTORS_PREFETCH));

  int status __unused = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);

  waitUntilCdDataIsAvailable();

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void triggerDataRequest(binary_stream_t *stream, uint32_t sectors) {
  // End previous transfers
  int status __unused = cd_block_cmd_data_transfer_end();

  const uint32_t sectorsReady =
    getSectorsReady(MIN(stream->remainingSectors, sectors));

  while (true) {
    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
    if (status & CD_STATUS_WAIT) {

    } else {
      break;
    }
  }

  bool ready __unused = false;
  for (uint32_t i = 0; i < 240000; i++) {
    if (MEMORY_READ(32, CD_BLOCK(HIRQ)) & DRDY) {
      ready = true;
      break;
    }
  }

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void initRingBuffer(binary_stream_t *stream) {
  film_sample_t nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];

  uint32_t ringBuffSize = stream->sampleCache.ringBuffEnd - stream->sampleCache.ringBuffStart;
  while (ringBuffSize > 0) {
    if (stream->sampleCache.writePos + nextSample.length >
      stream->sampleCache.ringBuffEnd) {
      stream->sampleCache.writePos = stream->sampleCache.ringBuffStart;
      break;
    } else {
      stream_readbytes(stream, (uint16_t *) stream->sampleCache.writePos, nextSample.length);
      stream->sampleCache.samples[stream->sampleCache.currentBuffSample].offset = stream->sampleCache.writePos;

      stream->sampleCache.writePos += nextSample.length;
      stream->sampleCache.currentBuffSample++;
      nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];
    }
  }
}

void readBytesIntoRingBuff(binary_stream_t *stream) {
  uint8_t *writePtr = stream->sampleCache.writePos;
  film_sample_cache_t *cache = &stream->sampleCache;
  int32_t currIdx = cache->currentSample;
  int32_t buffIdx = cache->currentBuffSample;
  film_sample_t nextSample = cache->samples[buffIdx];
  film_sample_t currentSample = cache->samples[currIdx];
  int32_t nextLen = nextSample.length;
  uint8_t *stopPoint = currentSample.offset;

  if (currentSample.offset == 0) {
    currentSample = cache->samples[currIdx - 1];
    stopPoint = cache->ringBuffEnd;
  }

  if (writePtr + nextLen > cache->ringBuffEnd ||
    buffIdx <= currIdx) {
    writePtr = stream->sampleCache.ringBuffStart;
  }

  if (writePtr + nextLen < stopPoint &&
    buffIdx < cache->numSamples) {
    stream_readbytes(stream, (uint16_t *) writePtr, nextLen);
    cache->samples[buffIdx].offset = writePtr;

    writePtr += nextLen;
    buffIdx++;
  }
  stream->sampleCache.currentBuffSample = buffIdx;
  stream->sampleCache.writePos = writePtr;
}

void stream_readbytes(binary_stream_t *stream, uint16_t *destPtr, uint32_t len) {
  uint32_t remainingBytes = len;

  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream, SECTORS_PREFETCH);
    int32_t readSize = stream->dataAvailable;
    if (readSize > remainingBytes) {
      readSize = remainingBytes;
    }

    int32_t readSizeLoop = readSize >> 1;

    cd_dma.dnw = (uintptr_t) destPtr;
    cd_dma.dnc = readSize;

    scu_dma_config_set(0, SCU_DMA_START_FACTOR_ENABLE, &cd_dma, NULL);
    scu_dma_level_fast_start(0);
    cpu_cache_purge();
    destPtr += readSizeLoop;
    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;
  }

  stream->offset += len;
}

typedef struct {
  bool requestPending;
  uint32_t sectorsThisRequest;
  uint8_t *nextWritePtr;
  int32_t sampleBytesRemaining;
  uint8_t *sampleStartPtr;
  int32_t targetBuffIdx;
} async_cd_state_t;

static async_cd_state_t asyncCd = {0};
static uint32_t asyncBytesDelivered = 0;

uint32_t film_buff_async_bytes_delivered(void) {
  return asyncBytesDelivered;
}

void film_buff_credit_async_bytes_delivered(uint32_t bytes) {
  asyncBytesDelivered += bytes;
}

void film_buff_reset_async_state(void) {
  asyncCd = (async_cd_state_t) {0};
  asyncBytesDelivered = 0;
}
void asyncReadBytesIntoRingBuff(binary_stream_t *stream) {
  film_sample_cache_t *cache = &stream->sampleCache;

  if (asyncCd.sampleBytesRemaining <= 0 && !asyncCd.requestPending) {
    int32_t currIdx = cache->currentSample;
    int32_t buffIdx = cache->currentBuffSample;
    if (buffIdx >= cache->numSamples) {
      return; // nothing left to prefetch
    }

    film_sample_t nextSample = cache->samples[buffIdx];
    film_sample_t currentSample = cache->samples[currIdx];
    int32_t nextLen = nextSample.length;
    uint8_t *stopPoint = currentSample.offset;

    if (currentSample.offset == 0) {
      currentSample = cache->samples[currIdx - 1];
      stopPoint = cache->ringBuffEnd;
    }

    uint8_t *writePtr = cache->writePos;
    if (writePtr + nextLen > cache->ringBuffEnd || buffIdx <= currIdx) {
      writePtr = cache->ringBuffStart;
    }

    if (!(writePtr + nextLen < stopPoint) || buffIdx >= cache->numSamples) {
      return; // no room yet - try again next tick
    }

    asyncCd.sampleBytesRemaining = nextLen;
    asyncCd.sampleStartPtr = writePtr;
    asyncCd.nextWritePtr = writePtr;
    asyncCd.targetBuffIdx = buffIdx;
    cache->writePos = writePtr; // reserve the space now, same as the sync path
    return; // start the actual CD request on the next call
  }

  if (asyncCd.sampleBytesRemaining <= 0) {
    return;
  }

  if (!asyncCd.requestPending) {
    if (stream->dataAvailable == 0) {
      if (stream->remainingSectors == 0) {
        return; // EOF - nothing more to fetch
      }

      uint32_t wantSectors = MIN(stream->remainingSectors, SECTORS_PREFETCH);

      int32_t sectorsReady = cd_block_cmd_sector_number_get(0);
      if ((uint32_t) sectorsReady < wantSectors) {
        return;
      }

      int status = cd_block_cmd_data_transfer_end();
      if (status & CD_STATUS_WAIT) {
        return;
      }
      status = cd_block_cmd_sector_data_get_delete(0, 0, wantSectors);
      if (status & CD_STATUS_WAIT) {
        return; // command port busy - try again next tick
      }

      asyncCd.sectorsThisRequest = wantSectors;
      asyncCd.requestPending = true;
    }
  }

  if (asyncCd.requestPending) {
    if (!(MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY)) {
      return; // not ready yet - try again next tick
    }

    stream->dataAvailable += asyncCd.sectorsThisRequest * CDFS_SECTOR_SIZE;
    stream->remainingSectors -= asyncCd.sectorsThisRequest;
    asyncCd.requestPending = false;
  }

  if (stream->dataAvailable > 0) {
    int32_t readSize = stream->dataAvailable;
    if (readSize > asyncCd.sampleBytesRemaining) {
      readSize = asyncCd.sampleBytesRemaining;
    }

    cd_dma.dnw = (uintptr_t) asyncCd.nextWritePtr;
    cd_dma.dnc = readSize;
    scu_dma_config_set(0, SCU_DMA_START_FACTOR_ENABLE, &cd_dma, NULL);
    scu_dma_level_start(0);
    scu_dma_level_wait(0);
    cpu_cache_purge();

    asyncCd.nextWritePtr += readSize;
    asyncCd.sampleBytesRemaining -= readSize;
    stream->dataAvailable -= readSize;
    stream->offset += readSize;
    asyncBytesDelivered += readSize;

    if (asyncCd.sampleBytesRemaining <= 0) {
      cache->samples[asyncCd.targetBuffIdx].offset = asyncCd.sampleStartPtr;
      cache->currentBuffSample = asyncCd.targetBuffIdx + 1;
      cache->writePos = asyncCd.nextWritePtr;
      asyncCd.sampleBytesRemaining = 0;
    }
  }
}
