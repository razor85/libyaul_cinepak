#include "cd.h"
#include "decoder.h"
#include "pcmsys.h"

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#ifndef CLAMP
#define CLAMP(V, X, Y) ((V) < (X) ? (X) : ((V) > (Y) ? (Y) : (V)))
#endif

#define CAST_DATA16(X) (volatile uint16_t*)(X)
#define CAST_DATA32(X) (volatile uint32_t*)(X)

// Globals
stripdata_t stripData;

#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
uint32_t videoWidth = VIDEO_WIDTH;
uint32_t videoHeight = VIDEO_HEIGHT;
uint32_t videoStartY = 0;
uint16_t *vdp2DestinationBuffer = (uint16_t *)VDP2_VRAM_ADDR(0, 0);
uint16_t videoTmpBuffer[VIDEO_WIDTH * VIDEO_HEIGHT];
uint16_t *vdp2ImagePtr = videoTmpBuffer;
  
// Audio data
uint8_t audioChannels = 0;
uint8_t audioSamplingResolution = 0;
uint8_t audioCompression = 0;
uint16_t audioSamplingFrequencyHz = 0;
uint32_t audioNumPlayedSamples = 0;
uint32_t audioSize = 0;

// Timer
volatile uint32_t frtOverflowCount = 0;
const uint32_t frtTimerDiv = CPU_FRT_NTSC_320_128_COUNT_1MS;
  
// For saving reads
#define TMP_BUFFER_SIZE 0x10000
uint8_t tmpBuffer[TMP_BUFFER_SIZE];

uint16_t stream_read16(binary_stream_t *);
uint32_t stream_read32(binary_stream_t *);
uint32_t stream_pos(binary_stream_t *stream);
void stream_readbytes(binary_stream_t *, volatile uint16_t*, uint32_t);
void stream_readbytes4(binary_stream_t *, volatile uint32_t*, uint32_t);
void stream_readbytes_generic(binary_stream_t *, volatile void*, uint32_t);

void stream_skip(binary_stream_t *, uint32_t);
void stream_skip4(binary_stream_t *, uint32_t);
void stream_skip_generic(binary_stream_t *, uint32_t);

void frtOviHandler() { frtOverflowCount++; }

static void frtTimerStart(uint16_t count) {
  cpu_frt_count_set(count);
  frtOverflowCount = 0;
}

static uint32_t frtTimerEllapsed() {
  uint32_t ticks = (0xFFFF * frtOverflowCount) + cpu_frt_count_get();
  return ticks / frtTimerDiv;
}

static inline fix16_t fix16_div(fix16_t dividend, fix16_t divisor) {
  cpu_divu_fix16_set(dividend, divisor);
  return cpu_divu_quotient_get();
}

inline void codebook_new(codebook_t *cb, binary_stream_t *stream) {
  stream_readbytes(stream, CAST_DATA16(cb), sizeof(codebook_t));
}

inline void codebook555_new(codebook_t *book, codebook555_t *cb555) {
  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int16_t cr = (book->v << 1);
  const int16_t cg = -(book->u >> 1) - book->v;
  const int16_t cb = +(book->u << 1);

  for (uint32_t i = 0; i < 4; ++i) {
    const int16_t r = book->y[i] + cr;
    const int16_t g = book->y[i] + cg;
    const int16_t b = book->y[i] + cb;

    const uint8_t nr = CLAMP(r, 0, 255);
    const uint8_t ng = CLAMP(g, 0, 255);
    const uint8_t nb = CLAMP(b, 0, 255);

    const rgb1555_t color = RGB1555_INITIALIZER(1, nr >> 3, ng >> 3, nb >> 3);
    cb555->color[i] = color.raw;
  }
}

void stripdata_new(stripdata_t *data) {
  memset(data->codebooks, 0, MAX_STRIPS * sizeof(strip_codebook_t));
  data->strip = 0;
  data->writeX = data->topX = 0;
  data->bottomX = 320;
  data->writeY = data->topY = 0;
  data->bottomY = 240;
}

volatile bool copyingBlocks = false;
void dmaCopyBlocksDone(void *data __unused) {
  cpu_cache_purge();
  copyingBlocks = false;
}

inline static void waitCopyingBlocks() {
  while (copyingBlocks)
    cpu_instr_nop();
}

void stripdata_copyLastCodebooks(stripdata_t *data) {
  DEBUG_REQUIRE_GT(data->strip, 0);
  DEBUG_REQUIRE_LT(data->strip, MAX_STRIPS);

  waitCopyingBlocks();

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src_mode = CPU_DMAC_SOURCE_INCREMENT,
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .stride = CPU_DMAC_STRIDE_16_BYTES,
    .bus_mode = CPU_DMAC_BUS_MODE_BURST,
    .src = CPU_CACHE_THROUGH | (uint32_t)&data->codebooks[data->strip - 1],
    .dst = CPU_CACHE_THROUGH | (uint32_t)&data->codebooks[data->strip],
    .len = sizeof(strip_codebook_t),
    .ihr = dmaCopyBlocksDone,
    .ihr_work = NULL,
  };

  copyingBlocks = true;

  cpu_dmac_channel_config_set(&cfg);
  cpu_dmac_channel_start(0);
}

volatile uint32_t copyingVideoFrame = 0;
inline static void waitCopyingVideoFrame() {
  if (copyingVideoFrame != 0) {
    while (vdp_dma_count_get() >= copyingVideoFrame)
      cpu_instr_nop();

    copyingVideoFrame = 0;
  }
}

void copyVideoFrame(uint32_t delta) {
  waitCopyingVideoFrame();

  vdp_dma_enqueue(vdp2DestinationBuffer + delta, vdp2ImagePtr + delta,
    VIDEO_WIDTH * videoHeight * sizeof(uint16_t));

  copyingVideoFrame = vdp_dma_count_get();
}

void stripdata_copyLastCodebooks_nodma(stripdata_t *data) {
  DEBUG_REQUIRE_GT(data->strip, 0);
  DEBUG_REQUIRE_LT(data->strip, MAX_STRIPS);
  memcpy(&data->codebooks[data->strip], &data->codebooks[data->strip - 1], sizeof(strip_codebook_t));
}

inline codebook_t *stripdata_getV1Codebook(stripdata_t *data) {
  return data->codebooks[data->strip].v1;
}

inline codebook555_t *stripdata_getV1Codebook555(stripdata_t *data) {
  return data->codebooks[data->strip].v1RGB;
}

inline codebook_t *stripdata_getV4Codebook(stripdata_t *data) {
  return data->codebooks[data->strip].v4;
}

inline codebook555_t *stripdata_getV4Codebook555(stripdata_t *data) {
  return data->codebooks[data->strip].v4RGB;
}

// stream must be exactly at the start of the sample descriptions
void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples) {
  // We only read a single sector, so check how many samples we have there.
  const uint32_t samplesInitialPos __unused = stream_pos(stream);
  DEBUG_REQUIRE_EQ(FILM_SAMPLE_START_OFFSET, samplesInitialPos);

  if (totalNumSamples > stream->sampleCache.numSamples) {
    logError("Insufficient number of cached samples: expected %d (%d "
             "bytes), got %d (%d bytes)\n",
      totalNumSamples, totalNumSamples * sizeof(film_sample_t),
      stream->sampleCache.numSamples,
      stream->sampleCache.numSamples * sizeof(film_sample_t));
  }

  film_sample_t *outputSamples = stream->sampleCache.samples;
  for (volatile uint32_t i = 0; i < totalNumSamples; ++i) {
    volatile cd_film_sample_t sample = { 0 };
    stream_readbytes4(stream, CAST_DATA32(&sample), sizeof(cd_film_sample_t));
    
    const bool isAudio = (sample.info1 == 0xFFFFFFFF);
    DEBUG_REQUIRE_NE(sample.length, 0);
    DEBUG_REQUIRE_EQ(sample.length % 4, 0);

    film_sample_t *newSample = &outputSamples[i];
    if (isAudio) {
      newSample->interval = 0xFFFFFFFF;
    } else {
      newSample->interval = sample.info2;
    }

    newSample->length = sample.length;
  }
}

// Return the current sample, don't do anything else.
inline film_sample_t film_sample_get_next_sample(film_sample_cache_t *cache) {
  DEBUG_REQUIRE_LT(cache->currentSample, cache->numSamples);
  return cache->samples[cache->currentSample++];
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
  stream->eof = false;

  // Fetch first sectors
  const uint32_t sectorsReady = getSectorsReady(
    MIN(stream->remainingSectors, SECTORS_PREFETCH));

  DEBUG_REQUIRE_GT(sectorsReady, 0);

  int status __unused = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
  DEBUG_REQUIRE_EQ(status, 0);

  waitUntilCdDataIsAvailable();

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void triggerDataRequest(binary_stream_t *stream) {
  DEBUG_REQUIRE_NE(stream->remainingSectors, 0);

  // End previous transfers
  int status __unused = cd_block_cmd_data_transfer_end();
  DEBUG_REQUIRE_EQ(status, 0);

  const uint32_t sectorsReady = getSectorsReady(
    MIN(stream->remainingSectors, SECTORS_PREFETCH));

  DEBUG_REQUIRE_GT(sectorsReady, 0);

  while (true) {
    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
    if (status & CD_STATUS_WAIT) {
      for (volatile uint32_t i = 0; i < 4096; ++i) { cpu_instr_nop(); }
    } else {
      break;
    }
  }

  // Wait until data is available
  bool ready __unused = false;
  for (volatile uint32_t i = 0; i < 240000; ++i) {
    if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY) {
      ready = true;
      break;
    }
  }

  DEBUG_REQUIRE(ready);

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void stream_readbytes(binary_stream_t *stream, volatile uint16_t *destPtr,
  uint32_t len) {

  DEBUG_REQUIRE_EQ(len % 2, 0);
  DEBUG_REQUIRE_NE(destPtr, NULL);
  DEBUG_REQUIRE_LE(stream->offset + len, stream->size);

  static volatile uint16_t *cdData = (volatile uint16_t *)CD_BLOCK_DATA_2;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 1;
    DEBUG_REQUIRE_EQ(readSize % 2, 0);
    
    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;

    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      *destPtr = *cdData;
      destPtr++;
    }
  }

  stream->offset += len;
}

void stream_readbytes4(binary_stream_t *stream, volatile uint32_t *destPtr,
  uint32_t len) {

  DEBUG_REQUIRE_EQ(len % 4, 0);
  DEBUG_REQUIRE_NE(destPtr, NULL);
  DEBUG_REQUIRE_LE(stream->offset + len, stream->size);

  static volatile uint32_t *cdData = (volatile uint32_t *)CD_BLOCK_DATA_4;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 2;
    DEBUG_REQUIRE_EQ(readSize % 2, 0);
    
    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;

    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      *destPtr = *cdData;
      destPtr++;
    }
  }

  stream->offset += len;
}

void stream_readbytes_generic(binary_stream_t *stream, volatile void *destPtr,
  uint32_t len) {

  if (len % 4 == 0)
    stream_readbytes4(stream, CAST_DATA32(destPtr), len);
  else
    stream_readbytes(stream, CAST_DATA16(destPtr), len);
}

void stream_skip(binary_stream_t *stream, uint32_t len) {
  DEBUG_REQUIRE_EQ(len % 2, 0);
  DEBUG_REQUIRE_LE(stream->offset + len, stream->size);

  static volatile uint16_t *cdData = (volatile uint16_t *)CD_BLOCK_DATA_2;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 1;
    DEBUG_REQUIRE_EQ(readSize % 2, 0);
    
    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;

    volatile uint16_t nothing __unused;
    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      nothing = *cdData;
    }
  }

  stream->offset += len;
}

void stream_skip4(binary_stream_t *stream, uint32_t len) {
  DEBUG_REQUIRE_EQ(len % 4, 0);
  DEBUG_REQUIRE_LE(stream->offset + len, stream->size);

  static volatile uint32_t *cdData = (volatile uint32_t *)CD_BLOCK_DATA_4;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 2;
    DEBUG_REQUIRE_EQ(readSize % 4, 0);
    
    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;

    volatile uint16_t nothing __unused;
    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      nothing = *cdData;
    }
  }

  stream->offset += len;
}

void stream_skip_generic(binary_stream_t *stream, uint32_t len) {
  if (len % 4 == 0)
    stream_skip4(stream, len);
  else
    stream_skip(stream, len);
}

uint16_t stream_read16(binary_stream_t *stream) {
  volatile uint16_t data;
  stream_readbytes(stream, &data, 2);

  return data;
}

uint32_t stream_read32(binary_stream_t *stream) {
  volatile uint32_t data;
  stream_readbytes4(stream, &data, 4);

  return data;
}

inline uint32_t stream_pos(binary_stream_t *stream) { return stream->offset; }

inline bool film_sample_is_video(const film_sample_t *sample) {
  return sample->interval != 0xFFFFFFFF;
}

inline bool film_sample_is_audio(const film_sample_t *sample) {
  return sample->interval == 0xFFFFFFFF;
}

inline void stripdata_skipBlock(stripdata_t *data) {
  data->writeX += 4;
  if (data->writeX >= data->bottomX) {
    data->writeX = data->topX;
    data->writeY += 4;
  }
}

void renderPixel1(stripdata_t *data, uint8_t c0) {
  const codebook555_t e0 = stripdata_getV1Codebook555(data)[c0];
  const uint32_t x = data->writeX;
  const uint32_t y = data->writeY;

  // VDP2 image has 512x256
  uint32_t imageIndex = ((videoStartY + y) * VIDEO_WIDTH) + x;
  DEBUG_REQUIRE_LT(imageIndex, VIDEO_WIDTH * VIDEO_HEIGHT);

  // +----+----+  +---+  +---+
  // | y0 | y1 |  | u |  | v |
  // +----+----+  +---+  +---+
  // | y2 | y3 |
  // +----+----+

  if (y + 0 >= data->bottomY)
    return;

  vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[0];
  vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[1];

  if (y + 1 >= data->bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[0];
  vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[1];

  if (y + 2 >= data->bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[2];
  vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[3];

  if (y + 3 >= data->bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[2];
  vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[3];
}

void renderPixel4(stripdata_t *data, uint8_t c0, uint8_t c1, uint8_t c2,
  uint8_t c3) {

  const codebook555_t *codebook = stripdata_getV4Codebook555(data);
  const codebook555_t *e0 = &codebook[c0];
  const codebook555_t *e1 = &codebook[c1];
  const codebook555_t *e2 = &codebook[c2];
  const codebook555_t *e3 = &codebook[c3];

  const uint32_t x = data->writeX;
  const uint32_t y = data->writeY;

  uint32_t imageIndex = ((videoStartY + y) * VIDEO_WIDTH) + x;
  DEBUG_REQUIRE_LT(imageIndex, VIDEO_WIDTH * VIDEO_HEIGHT);

  // +------+------+------+------+
  // | e0y0 | e0y1 | e1y0 | e1y1 |
  // +------+------+------+------+
  // | e0y2 | e0y3 | e1y2 | e1y3 |
  // +------+------+------+------+
  // | e2y0 | e2y1 | e3y0 | e3y1 |
  // +------+------+------+------+
  // | e2y2 | e2y3 | e3y2 | e3y3 |
  // +------+------+------+------+
  if (y + 0 >= data->bottomY)
    return;

  vdp2ImagePtr[imageIndex++] = e0->color[0];
  vdp2ImagePtr[imageIndex++] = e0->color[1];
  vdp2ImagePtr[imageIndex++] = e1->color[0];
  vdp2ImagePtr[imageIndex] = e1->color[1];

  if (y + 1 >= data->bottomY)
    return;

  imageIndex += VIDEO_WIDTH - 3;
  vdp2ImagePtr[imageIndex++] = e0->color[2];
  vdp2ImagePtr[imageIndex++] = e0->color[3];
  vdp2ImagePtr[imageIndex++] = e1->color[2];
  vdp2ImagePtr[imageIndex] = e1->color[3];

  if (y + 2 >= data->bottomY)
    return;

  imageIndex += VIDEO_WIDTH - 3;
  vdp2ImagePtr[imageIndex++] = e2->color[0];
  vdp2ImagePtr[imageIndex++] = e2->color[1];
  vdp2ImagePtr[imageIndex++] = e3->color[0];
  vdp2ImagePtr[imageIndex] = e3->color[1];

  if (y + 3 >= data->bottomY)
    return;

  imageIndex += VIDEO_WIDTH - 3;
  vdp2ImagePtr[imageIndex++] = e2->color[2];
  vdp2ImagePtr[imageIndex++] = e2->color[3];
  vdp2ImagePtr[imageIndex++] = e3->color[2];
  vdp2ImagePtr[imageIndex] = e3->color[3];
}

void readVectors(binary_stream_t *stream, stripdata_t *data,
  uint16_t chunkDataLength) {

  uint32_t flags = stream_read32(stream);
  uint32_t remainingSectionBytes = chunkDataLength - 4;

  DEBUG_REQUIRE_LT(remainingSectionBytes, TMP_BUFFER_SIZE);
  stream_readbytes_generic(stream, tmpBuffer, remainingSectionBytes);
  
  waitCopyingVideoFrame();

  uint8_t *tmpData = tmpBuffer;
  while (remainingSectionBytes) {
    if (data->writeY >= data->bottomY)
      break;

    for (uint32_t i = 0; i < 32; ++i) {
      DEBUG_REQUIRE_LT(tmpData, tmpBuffer + TMP_BUFFER_SIZE);
      if (flags & 0x80000000) {
        if (remainingSectionBytes < 4)
          break;

        // V4
        renderPixel4(data, tmpData[0], tmpData[1], tmpData[2], tmpData[3]);
        remainingSectionBytes -= 4;
        tmpData += 4;

      } else {
        if (remainingSectionBytes < 1)
          break;

        // V1
        renderPixel1(data, tmpData[0]);
        remainingSectionBytes -= 1;
        tmpData += 1;
      }

      stripdata_skipBlock(&stripData);
      flags <<= 1;
    }

    if (remainingSectionBytes < 4)
      break;

    memcpy(&flags, tmpData, 4);
    tmpData += 4;
    remainingSectionBytes -= 4;
  }
}

void readVectorsInter(binary_stream_t *stream, stripdata_t *data,
  uint16_t chunkDataLength) {

  stream_readbytes_generic(stream, tmpBuffer, chunkDataLength);
  waitCopyingVideoFrame();

  uint32_t remainingSectionBytes = chunkDataLength - 4;

  // We keep reading flags as long as it is possible. We first read 4
  // bytes and then we start shifting them for the VLC. Once we reach
  // the end we try to read a new flag.
  uint32_t flags = *((uint32_t*)tmpBuffer);
  
  uint8_t *tmpData = tmpBuffer + 4;
  uint32_t shifts = 0;
  while (remainingSectionBytes) {
    DEBUG_REQUIRE_LE(shifts, 31);
    if (data->writeY >= data->bottomY)
      break;

    // Running on VLC now, so we just keep consuming bytes (4 each time)
    // until there is nothing else. 0 => skip block, 1 => read next bit:
    // - next bit is 1 = V4
    // - next bit is 0 = V1
    DEBUG_REQUIRE_LT(tmpData, tmpBuffer + TMP_BUFFER_SIZE);
    if (flags & 0x80000000) {
      // We are at the last bit so we need to fetch the next flags and check
      // the first bit as if it was the next on this sequence.
      if (shifts == 31) {
        if (remainingSectionBytes < 4)
          break;

        memcpy(&flags, tmpData, 4);
        tmpData += 4;
        remainingSectionBytes -= 4;

        shifts = 0;
      } else {
        flags <<= 1;
        shifts++;
      }

      if (flags & 0x80000000) {
        if (remainingSectionBytes < 4)
          break;

        // V4
        renderPixel4(data, tmpData[0], tmpData[1], tmpData[2], tmpData[3]);
        tmpData += 4;
        remainingSectionBytes -= 4;
      } else {
        if (remainingSectionBytes < 1)
          break;

        // V1
        renderPixel1(data, tmpData[0]);
        tmpData += 1;
        remainingSectionBytes -= 1;
      }
    }

    stripdata_skipBlock(&stripData);

    // If we read all the bits, we just fetch the next flags.
    if (shifts == 31) {
      DEBUG_REQUIRE_LT(tmpData, tmpBuffer + TMP_BUFFER_SIZE);
      if (remainingSectionBytes < 4)
        break;

      memcpy(&flags, tmpData, 4);
      tmpData += 4;
      remainingSectionBytes -= 4;

      shifts = 0;
    } else {
      flags <<= 1;
      shifts++;
    }
  }
}

void readV1VectorsInChunk(binary_stream_t *stream, stripdata_t *data,
  uint16_t chunkDataLength) {

  uint32_t readBytes = 0;
  uint16_t originalWriteX = stripData.writeX;
  uint16_t originalWriteY = stripData.writeY;

  uint32_t remainingSectionBytes = chunkDataLength;
  while (remainingSectionBytes) {
    if (data->writeY >= data->bottomY)
      break;

    readBytes++;
    remainingSectionBytes -= 1;
    stripdata_skipBlock(&stripData);
  }

  stripData.writeX = originalWriteX;
  stripData.writeY = originalWriteY;

  waitCopyingVideoFrame();

  while (readBytes > 0) {
    const uint32_t readNow = MIN(readBytes, TMP_BUFFER_SIZE);

    stream_readbytes_generic(stream, tmpBuffer, readNow);
    for (volatile uint32_t i = 0; i < readNow; ++i) {
      const uint8_t c0 = tmpBuffer[i];
      renderPixel1(data, c0);
      stripdata_skipBlock(&stripData);
    }

    readBytes -= readNow;
  }

  if (remainingSectionBytes)
    stream_skip(stream, remainingSectionBytes);
}

void readChunk(uint16_t chunkID, uint16_t chunkDataLength,
  binary_stream_t *stream, stripdata_t *data) {

  DEBUG_REQUIRE_EQ(chunkDataLength % 2, 0);

  // If we are still copying the blocks.
  waitCopyingBlocks();

  switch (chunkID) {
  // 12 bit V4 (0x2000) or V1(0x2200)
  case 0x2000:
  case 0x2200:
    {
      DEBUG_REQUIRE_LT(data->strip, MAX_STRIPS);

      codebook_t *codebookPtr;
      codebook555_t *codebook555Ptr;
      if (chunkID == 0x2000) {
        codebookPtr = stripdata_getV4Codebook(data);
        codebook555Ptr = stripdata_getV4Codebook555(data);
      } else {
        codebookPtr = stripdata_getV1Codebook(data);
        codebook555Ptr = stripdata_getV1Codebook555(data);
      }

      uint32_t remainingSectionBytes = chunkDataLength;

      const uint32_t numReads = remainingSectionBytes / 6;
      const uint32_t numReadBytes = numReads * 6;
      DEBUG_REQUIRE_LE(numReadBytes, sizeof(codebook_t) * 256);

      stream_readbytes_generic(stream, codebookPtr, numReadBytes);
      for (uint32_t i = 0; i < numReads; ++i) {
        codebook555_new(&codebookPtr[i], &codebook555Ptr[i]);
      }

      remainingSectionBytes -= numReadBytes;

      // Deviant format shenanigans
      if (remainingSectionBytes)
        stream_skip(stream, remainingSectionBytes);
    }
    break;

  // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
  case 0x2100:
  case 0x2300:
    {
      DEBUG_REQUIRE_LT(data->strip, MAX_STRIPS);

      codebook_t *codebookPtr;
      codebook555_t *codebook555Ptr;
      if (chunkID == 0x2100) {
        codebookPtr = stripdata_getV4Codebook(data);
        codebook555Ptr = stripdata_getV4Codebook555(data);
      } else {
        codebookPtr = stripdata_getV1Codebook(data);
        codebook555Ptr = stripdata_getV1Codebook555(data);
      }

      uint32_t remainingSectionBytes = chunkDataLength;
      while (remainingSectionBytes >= 4) {
        uint32_t flags = stream_read32(stream);
        remainingSectionBytes -= 4;

        for (uint32_t i = 0; i < 32; ++i) {
          if (flags & 0x80000000) {
            codebook_new(codebookPtr, stream);
            codebook555_new(codebookPtr, codebook555Ptr);
            remainingSectionBytes -= 6;
          }

          codebookPtr++;
          codebook555Ptr++;
          flags <<= 1;
        }
      }

      if (remainingSectionBytes)
        stream_skip(stream, remainingSectionBytes);
    }
    break;

  // 8 bit V4
  case 0x2400:
    DEBUG_REQUIRE(false);
    break;

  // 8 bit V1
  case 0x2600:
    DEBUG_REQUIRE(false);
    break;

  // vectors
  case 0x3000:
    readVectors(stream, data, chunkDataLength);
    break;

  // list of blocks from v1
  case 0x3100:
    readVectorsInter(stream, data, chunkDataLength);
    break;

  case 0x3200:
    readV1VectorsInChunk(stream, data, chunkDataLength);
    break;

  default:
    {
      const uint32_t chunkIdPos = stream_pos(stream) - 4;

      memset(vdp2ImagePtr, 0, VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));
      logMessage("Unknown chunk id 0x%X at offset %d\n", chunkID, chunkIdPos);
      sprintf((char *)LWRAM(80), "Unknown chunk id 0x%X at offset %d\n",
        chunkID, chunkIdPos);

      DEBUG_REQUIRE(false);
      break;
    }
  }
}

typedef struct {
  union {
    uint32_t l;
    uint8_t b[4];
  } flagsAndCvidLength;

  uint16_t width;
  uint16_t height;
  uint16_t numStrips;
  uint16_t padding;
} videoHeader;

void parseVideo(binary_stream_t *stream) {
  static_assert(sizeof(videoHeader) == 12);

  volatile videoHeader cvidHeader;
  stream_readbytes(stream, CAST_DATA16(&cvidHeader), sizeof(videoHeader));

  const bool copyLastCodeBooks = !(cvidHeader.flagsAndCvidLength.b[0] & 0x1);

  uint16_t lastBottomY = 0;
  for (uint16_t stripId = 0; stripId < cvidHeader.numStrips; ++stripId) {
    stripData.strip = stripId;

    // flag bit 0 will tell if we need the contents of the previous strip
    if (stripId > 0 && copyLastCodeBooks) {
      stripdata_copyLastCodebooks_nodma(&stripData);
    }

    volatile uint16_t tmpStripBuffer[6];
    stream_readbytes4(stream, CAST_DATA32(tmpStripBuffer), 6 * sizeof(uint16_t));

    const uint16_t stripDataLength = tmpStripBuffer[1];

    stripData.topY = stripData.writeY = tmpStripBuffer[2];
    stripData.topX = stripData.writeX = tmpStripBuffer[3];
    stripData.bottomY = tmpStripBuffer[4];
    stripData.bottomX = tmpStripBuffer[5];

    if (stripId > 0 && stripData.topY == 0) {
      stripData.topY = stripData.writeY = lastBottomY;
      stripData.bottomY += lastBottomY;
    }

#ifdef DEBUG_DECODER
    logMessage("%u - %u Strip 0x%X, length = %d (%d,%d,%d,%d)\n", stream_pos(stream) - 12, stripId,
      tmpStripBuffer[0], stripDataLength, stripData.topX, stripData.topY, stripData.bottomX,
      stripData.bottomY);
#endif

    // Read the strip chunks
    const uint32_t stripLimit = stream_pos(stream) + stripDataLength - 12;
    lastBottomY = stripData.bottomY;

#ifdef DEBUG_DECODER
    logMessage("  Drawing to %dx%d\n", stripData.writeX, stripData.writeY);
#endif

    while (stream_pos(stream) < stripLimit) {
      const uint16_t cvidChunkID = stream_read16(stream);
      const uint16_t cvidChunkDataLength = stream_read16(stream) - 4;

      const size_t expectedEnd __unused = stream_pos(stream) +
        cvidChunkDataLength;

#ifdef DEBUG_DECODER
      logMessage("%u - Chunk ID 0x%X, length = %d (up to %u) to %d,%d\n", stream_pos(stream) - 4,
        cvidChunkID, cvidChunkDataLength, expectedEnd, stripData.writeX, stripData.writeY);
#endif

      if (cvidChunkDataLength)
        readChunk(cvidChunkID, cvidChunkDataLength, stream, &stripData);

      DEBUG_REQUIRE_EQ(stream_pos(stream), expectedEnd);
    } // Strip data

    DEBUG_REQUIRE_EQ(stream_pos(stream), stripLimit);
  }
}

void parseAudio(const film_sample_t *sample, binary_stream_t *stream) {
  uint32_t length = sample->length;

  // TODO: Proper stereo
  if (audioChannels == 2) {
    length >>= 1;
    DEBUG_REQUIRE_EQ(length % 2, 0);
  }

  uint32_t missingBytes = length;
  bool hasReadBytes = false;
    
  uint32_t *debug = (uint32_t *) LWRAM(0);

  audioSize = length;
  while (missingBytes > 0) {
    uint32_t readSize = MIN(film_audio_get_next_buffer_size(), missingBytes);
    uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);
    debug[0] = (uint32_t) writeLocation;

    if (writeLocation != NULL) {
      stream_readbytes_generic(stream, writeLocation, readSize);
      film_audio_notify_read_buffer_bytes(readSize);
      hasReadBytes = true;
    } else {
      stream_skip_generic(stream, readSize);
    }

    missingBytes -= readSize;
  }

  if (audioChannels == 2) {
    stream_skip_generic(stream, sample->length - length);
  }

  if (hasReadBytes) {
    film_audio_play(length);
    audioNumPlayedSamples++;
  }
}

inline void parseSample(const film_sample_t *sample, binary_stream_t *stream) {
  if (film_sample_is_audio(sample)) {
    parseAudio(sample, stream);
  } else {
    parseVideo(stream);
  }
}

void initialize_film() {
  memset(vdp2ImagePtr, 0, VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));
  vdp_dma_enqueue(vdp2DestinationBuffer, vdp2ImagePtr,
    VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));

  vdp2_sync();
  vdp2_sync_wait();
}

void play_film(cdfs_filelist_entry_t *entry, film_sample_t *sampleCache,
  uint32_t sampleCacheSize) {

  queueDiskRead(entry->starting_fad, entry->size);
  clearLog();

  stripdata_new(&stripData);

  DEBUG_REQUIRE(entry != NULL);
  logMessage("%s (%d bytes), FAD: %d\n", entry->name, entry->size, entry->starting_fad);

  // Start reading the file
  binary_stream_t stream;
  stream_new(&stream, entry, sampleCache, sampleCacheSize);

  const uint32_t asciiFilm __unused = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiFilm, ASCII_FILM);
  const uint32_t filmHeaderLength = stream_read32(&stream);

  const uint32_t filmVersion __unused = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(filmVersion, ASCII_1d09);

  stream_skip(&stream, 4); // Unknown

  const uint32_t asciiFdsc __unused = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiFdsc, ASCII_FDSC);

  // FDSC length.
  stream_skip(&stream, 4);

  const uint32_t asciiCvid __unused = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiCvid, ASCII_CVID);

  videoHeight = stream_read32(&stream);
  videoWidth = stream_read32(&stream);
  videoStartY = (VIDEO_HEIGHT - videoHeight) / 2;

  // A = video BPP
  // B = number of audio channels
  // C = audio sampling resolution
  // D = audio compression
  const uint32_t ABCD = stream_read32(&stream);
  const uint8_t videoBPP __unused = (ABCD >> 24) & 0xFF;
  audioChannels = (ABCD >> 16) & 0xFF;
  audioSamplingResolution = (ABCD >> 8) & 0xFF;
  audioCompression = ABCD & 0xFF;
  audioSamplingFrequencyHz = stream_read16(&stream);
  audioNumPlayedSamples = 0;

  film_audio_setup(audioSamplingFrequencyHz, audioChannels,
    audioSamplingResolution);

  logMessage("Found %dx%d@%dbpp video\n", videoWidth, videoHeight, videoBPP);
  logMessage("Centering at y = %d\n", videoStartY);
  logMessage("Audio: %d channels, %d bits (comp = %d), %d Hz\n", audioChannels,
    audioSamplingResolution, audioCompression, audioSamplingFrequencyHz);

  // Unknown
  stream_skip(&stream, 6);

  const uint32_t asciiStab __unused = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiStab, ASCII_STAB);

  // STAB length.
  stream_skip(&stream, 4);

  const uint32_t framerateBaseFrequencyHz = stream_read32(&stream);
  const uint32_t numSamples = stream_read32(&stream);
  stream.sampleCache.numSamples = numSamples;

  logMessage("Frame rate base frequency: %d Hz with %d samples\n", framerateBaseFrequencyHz,
    numSamples);

  const uint32_t sampleDescriptionPos __unused = stream_pos(&stream);
  const uint32_t sampleDataPos __unused = filmHeaderLength;

  // Must be called just before the sample list
  film_sample_cache_new(&stream, numSamples);

  logMessage("SamplePos: %d\nSampleDataPos: %d\nPos: %d\n", sampleDescriptionPos, sampleDataPos,
    stream_pos(&stream));
  
  // Ask the sound driver to stop the warm up sound and get ready to start processing sounds
  film_audio_prepare_to_play();

  cpu_frt_ovi_set(frtOviHandler);
  frtTimerStart(0);
  
  // Statistics
  uint32_t samplesInSec __unused = 0;
  uint32_t minSamplesInSec __unused = 0xFFFFFFFF;
  uint32_t samplesInSecCount __unused = 0;
  uint32_t bytesInSecCount __unused = 0;
  uint32_t lastBytesInSecCount __unused = 0;
  uint32_t playTime __unused = 0;

  // FILM timing
  uint32_t ticksUntilNextFrame = 0;
  uint32_t tickCount = 0;
  uint32_t lastFrameTime = frtTimerEllapsed();

  for (uint32_t sampleId = 0; sampleId < numSamples; ++sampleId) {
    if (!film_loop_handler()) {
      break;
    }

    film_sample_t sample = film_sample_get_next_sample(&stream.sampleCache);

    // Wait until we can proccess next frame due to pending interval
    const bool isVideo = film_sample_is_video(&sample);
    uint32_t timeEllapsed;

    timeEllapsed = frtTimerEllapsed();
    const uint32_t deltaTime = timeEllapsed - lastFrameTime;
    if (deltaTime > 0) {
      lastFrameTime = timeEllapsed;
      tickCount += deltaTime;
    }

    if (timeEllapsed >= 1000) {
      samplesInSec = samplesInSecCount;
      if (samplesInSec < minSamplesInSec) {
        minSamplesInSec = samplesInSec;
      }

      playTime += 1;
      samplesInSecCount = 0;
      lastBytesInSecCount = bytesInSecCount;
      bytesInSecCount = 0;
      frtTimerStart(timeEllapsed - 1000);
    }
      
    lastFrameTime = frtTimerEllapsed();

    parseSample(&sample, &stream);
    samplesInSecCount++;
    bytesInSecCount += sample.length;
    
    while (isVideo && (tickCount < ticksUntilNextFrame)) {
      timeEllapsed = frtTimerEllapsed();
      const uint32_t deltaTime = timeEllapsed - lastFrameTime;
      if (deltaTime > 0) {
        lastFrameTime = timeEllapsed;
        tickCount += deltaTime;
      }
    }

    if (isVideo) {
      const uint32_t delta = videoStartY * VIDEO_WIDTH;
      copyVideoFrame(delta);

      ticksUntilNextFrame = (sample.interval * 1000) / framerateBaseFrequencyHz;
      tickCount = 0;
    }

    clearLog();
    logMessage(
      "\nPlay time: %ds\nFrame %d\nSamplesInSec: %d\nBytesInSec: "
      "%d\nLastBytesInSec: %d\nAudio: %c / %d bits / %d Hz\nNumPlayedSamples: %d\nTickRate: %d Hz",
      playTime, sampleId, samplesInSec, bytesInSecCount, lastBytesInSecCount,
      audioChannels == 1 ? 'M' : 'S', audioSamplingResolution,
      audioSamplingFrequencyHz, audioNumPlayedSamples,
      framerateBaseFrequencyHz);
  }

  const int status __unused = cd_block_cmd_data_transfer_end();
  DEBUG_REQUIRE_EQ(status, 0);
}
