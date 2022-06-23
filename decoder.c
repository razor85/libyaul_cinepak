#include "decoder.h"
#include "md5.h"

#ifndef MIN
#define MIN(X, Y) (X < Y ? X : Y)
#endif

#ifndef MAX
#define MAX(X, Y) (X > Y ? X : Y)
#endif

// #define LOGGER(...) dbgio_printf(__VA_ARGS__)
// #define LOGGER_FLUSH() dbgio_flush()
#define LOGGER(...) 
#define LOGGER_FLUSH()

// TODO:
#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
const int vdp2Width = 512;
const int vdp2Height = 256;
const int videoWidth = VIDEO_WIDTH;
const int videoHeight = VIDEO_HEIGHT;

// Globals
int consolePrints = 0;
stripdata_t stripData;

uint8_t stream_read8(binary_stream_t *);
uint16_t stream_read16(binary_stream_t *);
uint32_t stream_read24(binary_stream_t *);
uint32_t stream_read32(binary_stream_t *);
void stream_readbytes(binary_stream_t *, void *, uint32_t);
void stream_skip(binary_stream_t *, uint32_t);

inline void clearConsole() { LOGGER("[H[2J"); }

inline uint32_t numSectorsForSize(uint32_t size) {
  // Past size so we get the complete number of sectors for the whole data.
  return (size + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
}

inline uint32_t numSectorsForIndex(uint32_t size) {
  // We get the number of sectors to reach the data.
  return size / CDFS_SECTOR_SIZE;
}

int waitForCdData() {
  const uint16_t flag = 0x0002 | 0x0080; /* DRDY | EHST*/
  const uint16_t hirq = 0x0008UL;
  for (volatile uint32_t i = 0; i < 0x240000; ++i) {
    if (MEMORY_READ(16, CD_BLOCK(hirq)) & flag)
      return 0;
  }

  return -1;
}

void waitForCD() {
  cd_block_status_t status;
  while (true) {
    if (cd_block_cmd_status_get(&status) != 0)
      continue;

    DEBUG_REQUIRE_NE(status.cd_status, CD_STATUS_ERROR);
    DEBUG_REQUIRE_NE(status.cd_status, CD_STATUS_FATAL);

    if (status.cd_status == CD_STATUS_PAUSE)
      break;
  }
}

int queueDiskRead(uint8_t selector, uint32_t fad, uint32_t size) {
  int status;
  waitForCD();

  if ((status = cd_block_cmd_sector_length_set(SECTOR_LENGTH_2048)) != 0) {
    return status;
  }

  if ((status = cd_block_cmd_selector_reset(0, selector)) != 0) {
    return status;
  }

  const uint32_t numSectors = numSectorsForSize(size);
  if ((status = cd_block_cmd_filter_range_set(selector, fad, numSectors)) !=
    0) {
    return status;
  }

  if ((status = cd_block_cmd_filter_connection_set(selector, 0, selector,
         0xFF)) != 0) {

    return status;
  }

  if ((status = cd_block_cmd_cd_dev_connection_set(selector)) != 0) {
    return status;
  }

  if ((status = cd_block_cmd_disk_play(0, fad, numSectors)) != 0) {
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

    uint32_t bytesToRead;
    if ((sectors_ready * CDFS_SECTOR_SIZE) > bytes_missing) {
      bytesToRead = bytes_missing;
    } else {
      bytesToRead = sectors_ready * CDFS_SECTOR_SIZE;
    }

    int status = cd_block_transfer_data_dmac(0, cdBuffer, outputBuffer,
      bytesToRead, 0);

    DEBUG_REQUIRE_EQ(status, 0);

    outputBuffer += bytesToRead;
    bytes_missing -= bytesToRead;
  }
}

inline void codebook_new(codebook_t *cb, binary_stream_t *stream) {
  stream_readbytes(stream, cb, sizeof(codebook_t));
}
  
void stripdata_new(stripdata_t *data) {
  data->strip = 0;
  data->writeX = data->topX = 0;
  data->bottomX = 320;
  data->writeY = data->topY = 0;
  data->bottomY = 240;
  
  memset(data->codebooks, 0, MAX_STRIPS * 512 * sizeof(codebook_t));
}

volatile bool cpuDmaActive = false;
void cpuDmaInterruptHandler(void* data __unused) {
  cpuDmaActive = false;
}

inline static void waitCpuDma() {
  while (cpuDmaActive)
    cpu_instr_nop();
}

void stripdata_copyLastCodebooks(stripdata_t *data) {
  waitCpuDma();

  DEBUG_REQUIRE_GT(data->strip, 0);
  DEBUG_REQUIRE_LT(data->strip, MAX_STRIPS);

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src = (uint32_t) &data->codebooks[data->strip - 1],
    .src_mode = CPU_DMAC_SOURCE_INCREMENT,
    .dst = (uint32_t) &data->codebooks[data->strip],
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .len = sizeof(codebook_t) * 512,
    .stride = CPU_DMAC_STRIDE_4_BYTES,
    .bus_mode = CPU_DMAC_BUS_MODE_BURST,
    .ihr = cpuDmaInterruptHandler,
    .ihr_work = NULL
  };

  cpuDmaActive = true;
  cpu_dmac_channel_config_set(&cfg);
  cpu_dmac_channel_start(0);
}

inline codebook_t* stripdata_getV1Codebook(stripdata_t *data) {
  return data->codebooks[data->strip].v1;
}

inline codebook_t* stripdata_getV4Codebook(stripdata_t *data) {
  return data->codebooks[data->strip].v4;
}

// stream must be exactly at the start of the sample descriptions
void film_sample_cache_new(film_sample_cache_t *cache, binary_stream_t *stream,
  uint32_t totalNumSamples) {

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
      cache->numCopySamples * sizeof(film_sample_t));

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
      const uint32_t copySize = cache->numCopySamples * sizeof(film_sample_t);
      readQueuedCopy(CDFS_SAMPLE_SELECTOR, &cache->cache[cache->frontIndex],
        copySize);

      cache->nextCacheSampleFAD += numSectorsForIndex(copySize);
      cache->numCopySamples = 0;
    }

    // Get new samples from the disk
    if (cache->numPendingSamples > 0) {
      cache->numCopySamples = MIN(cache->numPendingSamples,
        FILM_SAMPLE_CACHE_COUNT);

      cache->numPendingSamples -= cache->numCopySamples;

      const uint32_t queuedSize = cache->numCopySamples * sizeof(film_sample_t);
      const int status = queueDiskRead(CDFS_SAMPLE_SELECTOR,
        cache->nextCacheSampleFAD, queuedSize);
    
      DEBUG_REQUIRE_EQ(status, 0);
    }
  }
    
  return &cache->cache[cache->frontIndex][cache->currentSample++];
}

// Return the current sample, don't do anything else.
film_sample_t *film_sample_get_sample(film_sample_cache_t *cache) {
  return &cache->cache[cache->frontIndex][cache->currentSample];
}

void data_cache_fetch_next_sample_data(binary_stream_t *stream, bool firstFetch,
  uint32_t sampleDataPos) {

  data_cache_t* cache = &stream->dataCache;

  // First fetch is special because it blocks and completely discards the
  // content of the previous buffer.
  if (firstFetch) {
    const uint32_t startFAD = stream->fs->starting_fad +
      numSectorsForIndex(sampleDataPos);
    
    DEBUG_REQUIRE_GE(stream->fs->size, sampleDataPos);

    uint32_t totalReadBytes = stream->fs->size - sampleDataPos;
    if (totalReadBytes > DATA_CACHE_SIZE)
      totalReadBytes = DATA_CACHE_SIZE;

    // Queue up copy
    int status = queueDiskRead(CDFS_DATA_SELECTOR, startFAD, totalReadBytes);
    DEBUG_REQUIRE_EQ(status, 0);

    readQueuedCopy(CDFS_DATA_SELECTOR, cache->cache[0], totalReadBytes);
    stream->pos = sampleDataPos;
    stream->relPos = sampleDataPos % CDFS_SECTOR_SIZE;
    stream->dataPtr = cache->cache[0];
    stream->remainingSize = stream->fs->size - totalReadBytes - sampleDataPos;
    stream->nextDataPos = sampleDataPos + totalReadBytes;
    cache->queuedDataSize = 0;

  } else if (stream->remainingSize > 0) {
    const uint32_t startFAD = stream->fs->starting_fad +
      numSectorsForIndex(stream->nextDataPos);

    DEBUG_REQUIRE_GE(stream->fs->size, stream->nextDataPos);

    uint32_t totalReadBytes = stream->fs->size - stream->nextDataPos;
    if (totalReadBytes > DATA_CACHE_SIZE)
      totalReadBytes = DATA_CACHE_SIZE;

    // Queue up copy
    int status = queueDiskRead(CDFS_DATA_SELECTOR, startFAD, totalReadBytes);

    DEBUG_REQUIRE_EQ(status, 0);
    stream->remainingSize -= totalReadBytes;
    stream->nextDataPos += totalReadBytes;
    cache->queuedDataSize = totalReadBytes;
  }
}

void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  void *dataCache0, void *dataCache1) {

  const uint32_t initialDataSize = MIN(entry->size, CDFS_SECTOR_SIZE);

  stream->fs = entry;
  stream->pos = stream->relPos = 0;
  stream->remainingSize = stream->fs->size;

  stream->dataCache.frontIndex = 0;
  stream->dataCache.cache[0] = (uint8_t *)dataCache0;
  stream->dataCache.cacheSize[0] = initialDataSize;
  stream->dataCache.cache[1] = (uint8_t *)dataCache1;
  stream->dataCache.cacheSize[1] = 0xFFFFFFFF;
  stream->dataPtr = stream->dataCache.cache[stream->dataCache.frontIndex];

  int status = cd_block_sectors_read(entry->starting_fad, dataCache0,
    initialDataSize);

  DEBUG_REQUIRE_EQ(status, 0);
}

void stream_readbytes(binary_stream_t *stream, void *tmpDst, uint32_t len) {
  uint8_t *dst = (uint8_t *)tmpDst;

  // Swap buffers 
  if (stream->relPos + len >= DATA_CACHE_SIZE) {
    const uint32_t skipBufferSize = (DATA_CACHE_SIZE - stream->relPos);
    if (dst != NULL) {
      memcpy(dst, &stream->dataPtr[stream->relPos], skipBufferSize);
      dst += skipBufferSize;
    }

    data_cache_t *cache = &stream->dataCache;
    cache->frontIndex ^= 1;
    stream->dataPtr = cache->cache[cache->frontIndex];

    DEBUG_REQUIRE_NE(cache->queuedDataSize, 0);
    readQueuedCopy(CDFS_DATA_SELECTOR, stream->dataPtr, cache->queuedDataSize);

    data_cache_fetch_next_sample_data(stream, false, 0);
    stream->relPos = 0;
    stream->pos += skipBufferSize;
    len -= skipBufferSize;
  }

  DEBUG_REQUIRE_LE(stream->pos + len, stream->fs->size);
  DEBUG_REQUIRE_LE(stream->relPos + len, DATA_CACHE_SIZE);
  memcpy(dst, &stream->dataPtr[stream->relPos], len);

  stream->relPos += len;
  stream->pos += len;
}

inline uint8_t stream_read8(binary_stream_t *stream) {
  uint8_t data;
  stream_readbytes(stream, &data, 1);

  return data;
}

inline uint16_t stream_read16(binary_stream_t *stream) {
  uint16_t data;
  stream_readbytes(stream, &data, 2);

  return data;
}

inline uint32_t stream_read24(binary_stream_t *stream) {
  uint32_t data;
  stream_readbytes(stream, &data, 3);
  data >>= 8;

  return data;
}

inline uint32_t stream_read32(binary_stream_t *stream) {
  uint32_t data;
  stream_readbytes(stream, &data, 4);

  return data;
}

inline void stream_skip(binary_stream_t *stream, uint32_t len) {
  stream_readbytes(stream, NULL, len);
}

inline bool film_sample_is_video(const film_sample_t *sample) {
  return sample->info1 != 0xFFFFFFFF;
}

inline bool film_sample_is_audio(const film_sample_t *sample) {
  return !film_sample_is_video(sample);
}

inline bool film_sample_is_keyFrame(const film_sample_t *sample) {
  return film_sample_is_video(sample) && (!(sample->info1 & 0x1));
}

void film_read_sample(film_sample_t *sample) {
  LOGGER("Found %s sample at %d offset and %d length (infos: 0x%X [%d] "
         "/ 0x%X [%d])\n",
    film_sample_is_audio(sample) ? "audio" : "video", sample->offset,
    sample->length, sample->info1, sample->info1, sample->info2, sample->info2);

  if (film_sample_is_video(sample))
    LOGGER("  KeyFrame = %d\n", film_sample_is_keyFrame(sample));
}

uint16_t *vdp2ImagePtr = (uint16_t *)VDP2_VRAM_ADDR(0, 0);
inline void writeYUV(uint8_t cy, int16_t cr, int16_t cg, int16_t cb,
  uint32_t index) {

  DEBUG_REQUIRE_LT(index, vdp2Width * vdp2Height);

  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int r = cy + cr;
  const int g = cy + cg;
  const int b = cy + cb;

  // TODO:
  // uint8_t nr = r < 0 ? 0 : r > 255 ? 255 : r;
  // uint8_t ng = g < 0 ? 0 : g > 255 ? 255 : g;
  // uint8_t nb = b < 0 ? 0 : b > 255 ? 255 : b;
  uint8_t nr = r;
  uint8_t ng = g;
  uint8_t nb = b;

  vdp2ImagePtr[index] = COLOR_RGB1888_RGB1555(1, nr, ng, nb).raw;
}

inline void stripdata_skipBlock(stripdata_t *data) {
  data->writeX += 4;
  if (data->writeX >= data->bottomX) {
    data->writeX = data->topX;
    data->writeY += 4;
  }
}

void renderPixel1(stripdata_t *data, uint8_t c0) {
  const codebook_t e0 = stripdata_getV1Codebook(data)[c0];
  const uint16_t x = data->writeX;
  const uint16_t y = data->writeY;

  // VDP2 image has 512x256
  uint32_t imageIndex = y * vdp2Width + x;

  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int16_t cr = (e0.v << 1);
  const int16_t cg = - (e0.u >> 1) - e0.v;
  const int16_t cb = + (e0.u << 1);

  // +----+----+  +---+  +---+
  // | y0 | y1 |  | u |  | v |
  // +----+----+  +---+  +---+
  // | y2 | y3 |
  // +----+----+

  if (y + 0 >= data->bottomY)
    return;

  writeYUV(e0.y[0], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[0], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[1], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[1], cr, cg, cb, imageIndex++);
  
  if (y + 1 >= data->bottomY)
    return;

  imageIndex += vdp2Width - 4;

  writeYUV(e0.y[0], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[0], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[1], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[1], cr, cg, cb, imageIndex++);
  
  if (y + 2 >= data->bottomY)
    return;
  
  imageIndex += vdp2Width - 4;

  writeYUV(e0.y[2], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[2], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[3], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[3], cr, cg, cb, imageIndex++);
  
  if (y + 3 >= data->bottomY)
    return;
  
  imageIndex += vdp2Width - 4;

  writeYUV(e0.y[2], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[2], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[3], cr, cg, cb, imageIndex++);
  writeYUV(e0.y[3], cr, cg, cb, imageIndex++);
}

void renderPixel4(stripdata_t *data, uint8_t c0, uint8_t c1, uint8_t c2,
  uint8_t c3) {

  const codebook_t *codebook = stripdata_getV4Codebook(data);
  const codebook_t *e0 = &codebook[c0];
  const codebook_t *e1 = &codebook[c1];
  const codebook_t *e2 = &codebook[c2];
  const codebook_t *e3 = &codebook[c3];

  const uint16_t x = data->writeX;
  const uint16_t y = data->writeY;
  
  // VDP2 image has 512x256
  uint32_t imageIndex = y * vdp2Width + x;
  
  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int16_t cr0 = (e0->v << 1);
  const int16_t cg0 = - (e0->u >> 1) - e0->v;
  const int16_t cb0 = + (e0->u << 1);

  const int16_t cr1 = (e1->v << 1);
  const int16_t cg1 = - (e1->u >> 1) - e1->v;
  const int16_t cb1 = + (e1->u << 1);

  // +------+------+------+------+  +-----+-----+  +-----+-----+
  // | e0y0 | e0y1 | e1y0 | e1y1 |  | e0u | e1u |  | e0v | e1v |
  // +------+------+------+------+  +-----+-----+  +-----+-----+
  // | e0y2 | e0y3 | e1y2 | e1y3 |  | e2u | e3u |  | e2v | e3v |
  // +------+------+------+------+  +-----+-----+  +-----+-----+
  // | e2y0 | e2y1 | e3y0 | e3y1 |
  // +------+------+------+------+
  // | e2y2 | e2y3 | e3y2 | e3y3 |
  // +------+------+------+------+
  if (y + 0 >= data->bottomY)
    return;

  writeYUV(e0->y[0], cr0, cg0, cb0, imageIndex++);
  writeYUV(e0->y[1], cr0, cg0, cb0, imageIndex++);
  writeYUV(e1->y[0], cr1, cg1, cb1, imageIndex++);
  writeYUV(e1->y[1], cr1, cg1, cb1, imageIndex++);

  if (y + 1 >= data->bottomY)
    return;
  
  imageIndex += vdp2Width - 4;

  writeYUV(e0->y[2], cr0, cg0, cb0, imageIndex++);
  writeYUV(e0->y[3], cr0, cg0, cb0, imageIndex++);
  writeYUV(e1->y[2], cr1, cg1, cb1, imageIndex++);
  writeYUV(e1->y[3], cr1, cg1, cb1, imageIndex++);

  if (y + 2 >= data->bottomY)
    return;
  
  imageIndex += vdp2Width - 4;

  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int16_t cr2 = (e2->v << 1);
  const int16_t cg2 = - (e2->u >> 1) - e2->v;
  const int16_t cb2 = + (e2->u << 1);

  const int16_t cr3 = (e3->v << 1);
  const int16_t cg3 = - (e3->u >> 1) - e3->v;
  const int16_t cb3 = + (e3->u << 1);

  writeYUV(e2->y[0], cr2, cg2, cb2, imageIndex++);
  writeYUV(e2->y[1], cr2, cg2, cb2, imageIndex++);
  writeYUV(e3->y[0], cr3, cg3, cb3, imageIndex++);
  writeYUV(e3->y[1], cr3, cg3, cb3, imageIndex++);

  if (y + 3 >= data->bottomY)
    return;

  imageIndex += vdp2Width - 4;

  writeYUV(e2->y[2], cr2, cg2, cb2, imageIndex++);
  writeYUV(e2->y[3], cr2, cg2, cb2, imageIndex++);
  writeYUV(e3->y[2], cr3, cg3, cb3, imageIndex++);
  writeYUV(e3->y[3], cr3, cg3, cb3, imageIndex++);
}
    
void readVectors(const film_sample_t *sample, binary_stream_t *stream,
  stripdata_t *data, uint16_t chunkDataLength) {

  uint32_t remainingSectionBytes = chunkDataLength;
  uint32_t flags = stream_read32(stream);
  remainingSectionBytes -= 4;

  while (remainingSectionBytes) {
    if (data->writeY >= data->bottomY)
      break;

    for (uint32_t i = 0; i < 32; ++i) {
      if (flags & 0x80000000) {
        if (remainingSectionBytes < 4)
          break;

        // V4
        uint8_t c[4];
        stream_readbytes(stream, c, 4);

        remainingSectionBytes -= 4;
        renderPixel4(data, c[0], c[1], c[2], c[3]);
      } else {
        if (remainingSectionBytes < 1)
          break;

        // V1
        const uint8_t c0 = stream_read8(stream);
        remainingSectionBytes -= 1;
        renderPixel1(data, c0);
      }

      stripdata_skipBlock(&stripData);
      flags <<= 1;
    }

    if (remainingSectionBytes < 4)
      break;

    flags = stream_read32(stream);
    remainingSectionBytes -= 4;
  }

  if (remainingSectionBytes)
    stream_skip(stream, remainingSectionBytes);
}

void readVectorsInter(const film_sample_t *sample, binary_stream_t *stream,
  stripdata_t *data, uint16_t chunkDataLength) {

  uint32_t remainingSectionBytes = chunkDataLength;

  // We keep reading flags as long as it is possible. We first read 4
  // bytes and then we start shifting them for the VLC. Once we reach
  // the end we try to read a new flag.
  uint32_t flags = stream_read32(stream);
  remainingSectionBytes -= 4;

  uint32_t shifts = 0;
  while (remainingSectionBytes) {
    DEBUG_REQUIRE_LE(shifts, 31);
    if (data->writeY >= data->bottomY)
      break;

    // Running on VLC now, so we just keep consuming bytes (4 each time)
    // until there is nothing else. 0 => skip block, 1 => read next bit:
    // - next bit is 1 = V4
    // - next bit is 0 = V1
    if (flags & 0x80000000) {
      // We are at the last bit so we need to fetch the next flags and check
      // the first bit as if it was the next on this sequence.
      if (shifts == 31) {
        if (remainingSectionBytes < 4)
          break;

        flags = stream_read32(stream);
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
        uint8_t c[4];
        stream_readbytes(stream, c, 4);

        remainingSectionBytes -= 4;
        renderPixel4(data, c[0], c[1], c[2], c[3]);
      } else {
        if (remainingSectionBytes < 1)
          break;
        
        // V1
        const uint8_t c0 = stream_read8(stream);
        remainingSectionBytes -= 1;
        renderPixel1(data, c0);
      }
    }

    stripdata_skipBlock(&stripData);
      
    // If we read all the bits, we just fetch the next flags.
    if (shifts == 31) {
      if (remainingSectionBytes < 4)
        break;

      flags = stream_read32(stream);
      remainingSectionBytes -= 4;
      shifts = 0;
    } else {
      flags <<= 1;
      shifts++;
    }
  }
      
  if (remainingSectionBytes)
    stream_skip(stream, remainingSectionBytes);
}
    
void readV1VectorsInChunk(const film_sample_t *sample, binary_stream_t *stream,
  stripdata_t *data, uint16_t chunkDataLength) {

  uint32_t remainingSectionBytes = chunkDataLength;
  while (remainingSectionBytes) {
    if (data->writeY >= data->bottomY)
      break;

    const uint8_t c0 = stream_read8(stream);
    remainingSectionBytes -= 1;
    renderPixel1(data, c0);
    stripdata_skipBlock(&stripData);
  }
  
  if (remainingSectionBytes)
    stream_skip(stream, remainingSectionBytes);
}

void readChunk(const film_sample_t *sample, uint16_t chunkID,
  uint16_t chunkDataLength, binary_stream_t *stream, stripdata_t *data) {

  if (chunkDataLength == 0)
    return;

  // If we are still copying the blocks.
  waitCpuDma();

  switch (chunkID) {
  // 12 bit V4 (0x2000) or V1(0x2200)
  case 0x2000:
  case 0x2200:
    {
      codebook_t *codebookPtr;
      if (chunkID == 0x2000)
        codebookPtr = stripdata_getV4Codebook(data);
      else
        codebookPtr = stripdata_getV1Codebook(data);
  
      uint32_t remainingSectionBytes = chunkDataLength;
      while (remainingSectionBytes >= 6) {
        codebook_new(codebookPtr++, stream);
        remainingSectionBytes -= 6;
      }
        
      // Deviant format shenanigans
      if (remainingSectionBytes)
        stream_skip(stream, remainingSectionBytes);
    }
    break;

  // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
  case 0x2100:
  case 0x2300:
    {
      codebook_t *codebookPtr;
      if (chunkID == 0x2100)
        codebookPtr = stripdata_getV4Codebook(data);
      else
        codebookPtr = stripdata_getV1Codebook(data);

      uint32_t remainingSectionBytes = chunkDataLength;
      uint32_t replacementID = 0;
      while (remainingSectionBytes >= 4) {
        uint32_t flags = stream_read32(stream);
        remainingSectionBytes -= 4;

        for (uint32_t i = 0; i < 32; ++i) {
          if (flags & 0x80000000) {
            DEBUG_REQUIRE_LT(replacementID, 256);
            codebook_new(&codebookPtr[replacementID], stream);
            remainingSectionBytes -= 6;
          }

          replacementID++;
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
    readVectors(sample, stream, data, chunkDataLength);
    break;

  // list of blocks from v1
  case 0x3100:
    readVectorsInter(sample, stream, data, chunkDataLength);
    break;

  case 0x3200:
    readV1VectorsInChunk(sample, stream, data, chunkDataLength);
    break;

  default:
    printf("Unknown chunk id %d (0x%X)\n", chunkID, chunkID);
    DEBUG_REQUIRE(false);
    break;
  }
}

void parseVideo(const film_sample_t *sample, binary_stream_t *stream) {
  const uint8_t flags = stream_read8(stream);

  // The deviant format length is 8 bytes shorter
#ifdef HAS_DEBUG_REQUIRE_FUNCTIONS
  const uint32_t cvidLength = stream_read24(stream);
  const uint16_t width = stream_read16(stream);
  const uint16_t height = stream_read16(stream);
#else
  stream_skip(stream, 7);
#endif

  const uint16_t numStrips = stream_read16(stream);

  // The deviant format as 2 extra bytes in the header.
  stream_skip(stream, 2);

  LOGGER(
    "Reading video sample %dx%d with length = %d, flags = %d and %d strips\n",
    width, height, cvidLength, flags, numStrips);

  uint16_t lastBottomY = 0;
  for (uint16_t stripId = 0; stripId < numStrips; ++stripId) {
    const uint16_t cvidID = stream_read16(stream);
    const uint16_t stripDataLength = stream_read16(stream);
    stripData.strip = stripId;
    stripData.topY = stripData.writeY = stream_read16(stream);
    stripData.topX = stripData.writeX = stream_read16(stream);
    stripData.bottomY = stream_read16(stream);
    stripData.bottomX = stream_read16(stream);

    if (stripId > 0 && stripData.topY == 0) {
      stripData.topY = stripData.writeY = lastBottomY;
      stripData.bottomY += lastBottomY;
    }

    const bool isIntraCoded = (cvidID == 0x1000); // Keyframe
    const bool isInterCoded = !isIntraCoded;

    LOGGER("%u - %u Strip 0x%X, length = %d (%d,%d,%d,%d)\n",
      stream->pos - 12, stripId, cvidID, stripDataLength, stripData.topX,
      stripData.topY, stripData.bottomX, stripData.bottomY);
    
    // Read the strip chunks
    const uint32_t stripLimit = (uint32_t)(stream->pos + stripDataLength - 12);

    // flag bit 0 will tell if we need the contents of the previous strip
    if (stripId > 0 && (!(flags & 0x1)))
      stripdata_copyLastCodebooks(&stripData);

    lastBottomY = stripData.bottomY;
    LOGGER("  Drawing to %dx%d\n", stripData.writeX, stripData.writeY);

    while (stream->pos < stripLimit) {
      const uint16_t cvidChunkID = stream_read16(stream);
      const uint16_t cvidChunkDataLength = stream_read16(stream) - 4;

      const size_t expectedEnd = stream->pos + cvidChunkDataLength;
      LOGGER("%u - Chunk ID 0x%X, length = %d (up to %u) to %d,%d\n",
        stream->pos - 4, cvidChunkID, cvidChunkDataLength, expectedEnd,
        stripData.writeX, stripData.writeY);
      
      readChunk(sample, cvidChunkID, cvidChunkDataLength, stream, &stripData);
      LOGGER("%u - End chunk\n", stream->pos);
      DEBUG_REQUIRE_EQ(stream->pos, expectedEnd);

    } // Strip data
  }
}
    
void parseSample(const film_sample_t *sample, binary_stream_t *stream) {
  if (film_sample_is_audio(sample)) {
    stream_skip(stream, sample->length);
  } else {
    parseVideo(sample, stream);
  }
}

void play_film(cdfs_filelist_entry_t *entry, void *dataCache0,
  void *dataCache1) {

  stripdata_new(&stripData);

  DEBUG_REQUIRE(entry != NULL);
  dbgio_printf("%s\n", entry->name);

  // Start reading the file
  binary_stream_t stream;
  stream_new(&stream, entry, dataCache0, dataCache1);

  const uint32_t asciiFilm = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiFilm, ASCII_FILM);
  const uint32_t filmHeaderLength = stream_read32(&stream);

  const uint32_t filmVersion = stream_read32(&stream);
  dbgio_printf("Version %X\n", filmVersion);
  DEBUG_REQUIRE_EQ(filmVersion, ASCII_1d09);

  stream_skip(&stream, 4); // Unknown
  
  const uint32_t asciiFdsc = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiFdsc, ASCII_FDSC);
  const uint32_t fdscLength = stream_read32(&stream);

  const uint32_t asciiCvid = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiCvid, ASCII_CVID);

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

  const uint32_t asciiStab = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiStab, ASCII_STAB);

  const uint32_t stabLength = stream_read32(&stream);
  const uint32_t framerateBaseFrequencyHz = stream_read32(&stream);
  const uint32_t numSamples = stream_read32(&stream);

  dbgio_printf("Frame rate base frequency: %d Hz with %d samples\n",
    framerateBaseFrequencyHz, numSamples);
  
  const uint32_t sampleDescriptionPos = stream.pos;
  const uint32_t sampleDataPos = filmHeaderLength;

  // Must be called just before the sample list
  film_sample_cache_new(&stream.sampleCache, &stream, numSamples);
  
  // Queue up first data reading before we can start to process the frames.
  data_cache_fetch_next_sample_data(&stream, 1, sampleDataPos);

  // Trigger next data copy.
  data_cache_fetch_next_sample_data(&stream, 0, 0);

  dbgio_printf("SamplePos: %d\nSampleDataPos: %d\nPos: %d\nRelPos: %d\n",
    sampleDescriptionPos, sampleDataPos, stream.pos, stream.relPos);

  for (uint32_t sampleId = 0; sampleId < numSamples; ++sampleId) {
    clearConsole();

    film_sample_t* sample = film_sample_get_next_sample(&stream.sampleCache);
    DEBUG_REQUIRE_NE(sample, NULL);

    film_read_sample(sample);
    
    vdp2_sync_wait();
    parseSample(sample, &stream);

    LOGGER_FLUSH();
    vdp2_sync();
  }

  VDP_INFLOOP();
}

