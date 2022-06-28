#include "decoder.h"
#include "md5.h"

#ifndef MIN
#define MIN(X, Y) (X) < (Y) ? (X) : (Y)
#endif

#ifndef MAX
#define MAX(X, Y) (X) > (Y) ? (X) : (Y)
#endif

#ifndef CLAMP
#define CLAMP(V, X, Y) ((V) < (X) ? (X) : ((V) > (Y) ? (Y) : (V)))
#endif

// #define LOGGER(...) dbgio_printf(__VA_ARGS__)
// #define LOGGER_FLUSH() dbgio_flush()
#define LOGGER(...) do {} while (false)
#define LOGGER_FLUSH() do {} while (false)

// Globals
stripdata_t stripData;

#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
#define VDP2_WIDTH 512
#define VDP2_HEIGHT 256
uint32_t videoWidth = VIDEO_WIDTH;
uint32_t videoHeight = VIDEO_HEIGHT;
uint32_t videoStartY = 0;

typedef struct {
  void *addr;
  uint32_t len;
} cache_purge_area_t;

// Dma operations
volatile bool dmaTransfering[2] = { false, false };
volatile bool dmaPendingCdDelete[2] = { false, false };
volatile cache_purge_area_t dmaPendingCachePurge[2];

// Timer
uint16_t frtOverflowCount = 0;
const uint32_t frtTimerDiv = CPU_FRT_NTSC_320_128_COUNT_1MS;
  
// For saving reads
#define TMP_BUFFER_SIZE 128
uint8_t tmpBuffer[TMP_BUFFER_SIZE];

#define HIRQ 0x0008UL
#define DRDY 0x0002 /* Data transfer preparations complete */
#define EHST 0x0080 /* Host I/O processing complete */
#define DTR 0x0000UL
#define CD_STATUS_TIMEOUT 0xAA

void dmacDone(void *cdBufferPtr) {
  const uint32_t cdBuffer = (uint32_t)cdBufferPtr;
  if (dmaPendingCdDelete[cdBuffer]) {
    const int status = cd_block_cmd_data_transfer_end();
    DEBUG_REQUIRE_EQ(status, 0);

    dmaPendingCdDelete[cdBuffer] = false;
  }
  
  dmaTransfering[cdBuffer] = false;
  cpu_cache_area_purge(dmaPendingCachePurge[cdBuffer].addr, 
    dmaPendingCachePurge[cdBuffer].len);
}

int hirqFlagWait(uint16_t flag) {
  volatile uint32_t i;

  for (i = 0; i < 0x240000; ++i) {
    if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & flag) {
      return 0;
    }
  }

  return -1;
}

bool cdBlockTransferData(uint8_t bufferNumber, uint8_t *outputBuffer,
  uint32_t length, bool waitChannel) {

  DEBUG_REQUIRE(outputBuffer != NULL);
  DEBUG_REQUIRE(length > 0);
  const uint32_t sectors = (length + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
  
  cpu_dmac_channel_wait(0);

  /* Start transfer */
  int status = cd_block_cmd_sector_data_get_delete(0, bufferNumber, sectors);
  DEBUG_REQUIRE_EQ(status, 0);

  // If waiting expire, generate a timeout
  if ((hirqFlagWait(DRDY | EHST)) != 0)
    return false;

  /* Transfer from register to user space */
  const uint32_t ihrParam = bufferNumber;
  const cpu_dmac_bus_mode_t busMode = waitChannel ?
    CPU_DMAC_BUS_MODE_BURST :
    CPU_DMAC_BUS_MODE_CYCLE_STEAL;

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src_mode = CPU_DMAC_SOURCE_FIXED,
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .stride = CPU_DMAC_STRIDE_2_BYTES,
    .bus_mode = busMode,
    .src = CD_BLOCK(DTR),
    .dst = (uint32_t)outputBuffer,
    .len = length,
    .ihr = dmacDone,
    .ihr_work = (void *)ihrParam,
  };

  dmaTransfering[bufferNumber] = true;
  dmaPendingCdDelete[bufferNumber] = true;
  dmaPendingCachePurge[bufferNumber].addr = outputBuffer;
  dmaPendingCachePurge[bufferNumber].len = length;

  cpu_dmac_channel_config_set(&cfg);
  cpu_dmac_channel_start(0);

  if (waitChannel) {
    while (dmaTransfering[bufferNumber])
      cpu_instr_nop();
  }

  return true;
}

uint8_t stream_read8(binary_stream_t *);
uint16_t stream_read16(binary_stream_t *);
uint32_t stream_read24(binary_stream_t *);
uint32_t stream_read32(binary_stream_t *);
void stream_readbytes(binary_stream_t *, void *, uint32_t);
void stream_skip(binary_stream_t *, uint32_t);
uint32_t stream_pos(binary_stream_t *stream);
uint32_t stream_relPos(binary_stream_t *stream);
uint32_t stream_cache_size(binary_stream_t *stream);

void frtOviHandler() { frtOverflowCount++; }

static void frtTimerStart(uint16_t count) {
  cpu_frt_count_set(count);
  frtOverflowCount = 0;
}

static uint32_t frtTimerEllapsed() {
  uint32_t ticks = (0xFFFF * frtOverflowCount) + cpu_frt_count_get();
  return ticks / frtTimerDiv;
}

inline void clearConsole() { dbgio_printf("[H[2J"); }

inline uint32_t numSectorsForSize(uint32_t size) {
  // Past size so we get the complete number of sectors for the whole data.
  return (size + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
}

inline uint32_t numSectorsForIndex(uint32_t size) {
  // We get the number of sectors to reach the data.
  return size / CDFS_SECTOR_SIZE;
}

int waitForCdData() {
  for (volatile uint32_t i = 0; i < 0x240000; ++i) {
    if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & (DRDY | EHST))
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

void queueDiskRead(uint8_t selector, uint32_t fad, uint32_t size) {
  DEBUG_REQUIRE_NE(size, 0);
  waitForCD();

  int status = cd_block_cmd_selector_reset(0, selector);
  DEBUG_REQUIRE_EQ(status, 0);

  const uint32_t numSectors = numSectorsForSize(size);
  status = cd_block_cmd_filter_range_set(selector, fad, numSectors);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_filter_connection_set(selector, 0, selector, 0xFF);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_cd_dev_connection_set(selector);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_disk_play(0, fad, numSectors);
  DEBUG_REQUIRE_EQ(status, 0);
}

void readQueuedCopy(uint8_t cdBuffer, void *destination, uint32_t size,
  bool waitCopy) {

  uint8_t *outputBuffer = (uint8_t *) destination;

  DEBUG_REQUIRE_EQ(dmaTransfering[cdBuffer], false);
  DEBUG_REQUIRE_EQ(dmaPendingCdDelete[cdBuffer], false);
  
  uint32_t bytesMissing = size;
  while (bytesMissing) {
    /* Wait until there's data ready */
    uint32_t sectorsReady;
    do {
      sectorsReady = cd_block_cmd_sector_number_get(cdBuffer);
    } while (sectorsReady == 0);

    const uint32_t toRead = MIN(sectorsReady * CDFS_SECTOR_SIZE, bytesMissing);
    if (cdBlockTransferData(cdBuffer, outputBuffer, toRead, waitCopy)) {
      outputBuffer += toRead;
      bytesMissing -= toRead;
    }
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
  
  memset(data->codebooks, 0, MAX_STRIPS * sizeof(strip_codebook_t));
}

volatile bool copyingBlocks = false;
void dmaCopyBlocksDone(void* data __unused) {
  copyingBlocks = false;
}

inline static void waitCopyingBlocks() {
  while (copyingBlocks)
    cpu_instr_nop();
}

void stripdata_copyLastCodebooks(stripdata_t *data) {
  DEBUG_REQUIRE_GT(data->strip, 0);
  DEBUG_REQUIRE_LT(data->strip, MAX_STRIPS);
  
  cpu_dmac_channel_wait(0);

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src_mode = CPU_DMAC_SOURCE_INCREMENT,
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .stride = CPU_DMAC_STRIDE_16_BYTES,
    .bus_mode = CPU_DMAC_BUS_MODE_BURST,
    .src = (uint32_t) &data->codebooks[data->strip - 1],
    .dst = (uint32_t) &data->codebooks[data->strip],
    .len = sizeof(strip_codebook_t),
    .ihr = dmaCopyBlocksDone,
    .ihr_work = NULL,
  };

  copyingBlocks = true;
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

  // We only read a single sector, so check how many samples we have there.
  const uint32_t samplesInitialPos = stream_pos(stream);
  const uint32_t initialCacheSamples = (CDFS_SECTOR_SIZE - samplesInitialPos) /
    sizeof(film_sample_t);

  cache->frontIndex = 0;
  cache->currentSample = 0;
  cache->numCopySamples = 0;
  cache->numCacheSamples = MIN(initialCacheSamples, totalNumSamples);
  cache->numPendingSamples = totalNumSamples - cache->numCacheSamples;

  dbgio_printf("Started with %d samples\n", cache->numCacheSamples);

  uint32_t samplesSize = cache->numCacheSamples * sizeof(film_sample_t);
  stream_readbytes(stream, cache->cache[0], samplesSize);

  if (cache->numPendingSamples) {
    cache->nextCacheSamplePos = stream_pos(stream);
    cache->numCopySamples = MIN(cache->numPendingSamples,
      FILM_SAMPLE_CACHE_COUNT);

    cache->numPendingSamples -= cache->numCopySamples;
    const uint32_t nextFAD = stream->startFAD +
      numSectorsForIndex(cache->nextCacheSamplePos);

    queueDiskRead(CDFS_SAMPLE_SELECTOR, nextFAD,
      cache->numCopySamples * sizeof(film_sample_t));
  }
}

// Returns NULL if there are no more samples
film_sample_t *film_sample_get_next_sample(binary_stream_t *stream) {
  // Time to swap?
  film_sample_cache_t *cache = &stream->sampleCache;
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
        copySize, true);

      cache->nextCacheSamplePos += copySize;
      cache->numCopySamples = 0;
    }

    // Get new samples from the disk
    if (cache->numPendingSamples > 0) {
      cache->numCopySamples = MIN(cache->numPendingSamples,
        FILM_SAMPLE_CACHE_COUNT);

      cache->numPendingSamples -= cache->numCopySamples;
      const uint32_t nextFAD = stream->startFAD +
        numSectorsForIndex(cache->nextCacheSamplePos);

      const uint32_t queuedSize = cache->numCopySamples * sizeof(film_sample_t);
      queueDiskRead(CDFS_SAMPLE_SELECTOR, nextFAD, queuedSize);
    }
  }
    
  return &cache->cache[cache->frontIndex][cache->currentSample++];
}

// Return the current sample, don't do anything else.
film_sample_t *film_sample_get_sample(film_sample_cache_t *cache) {
  return &cache->cache[cache->frontIndex][cache->currentSample];
}

void data_cache_fetch_first_sample_data(binary_stream_t *stream,
  const uint32_t sampleDataPos) {
    
  // First fetch is special because it blocks and completely discards the
  // content of the previous buffer.
  DEBUG_REQUIRE_GE(stream->size, sampleDataPos);
  const uint32_t numSectors = numSectorsForIndex(sampleDataPos);
  const uint32_t readFAD = stream->startFAD + numSectors;

  // Queue up copy
  uint32_t toRead = MIN(stream->size - sampleDataPos, DATA_CACHE_SIZE);
  queueDiskRead(CDFS_DATA_SELECTOR, readFAD, toRead);
  readQueuedCopy(CDFS_DATA_SELECTOR, stream->dataPtr, toRead, true);

  data_cache_t *cache = &stream->dataCaches[0];
  cache->pos = sampleDataPos;
  cache->relPos = sampleDataPos % CDFS_SECTOR_SIZE;
  cache->endPos = (numSectors * CDFS_SECTOR_SIZE) + toRead;
  cache->size = toRead;
  
  stream->remainingSize = stream->size - cache->endPos;
}

void data_cache_fetch_next_sample_data(binary_stream_t *stream) {
  if (!stream->remainingSize)
    return;

  data_cache_t *frontCache = &stream->dataCaches[stream->activeCacheIndex];
  data_cache_t *backCache = &stream->dataCaches[stream->activeCacheIndex ^ 1];

  const uint32_t numSectors = numSectorsForIndex(frontCache->endPos);
  const uint32_t startFAD = stream->startFAD + numSectors;

  uint32_t toRead = MIN(stream->size - frontCache->endPos, DATA_CACHE_SIZE);
  queueDiskRead(CDFS_DATA_SELECTOR, startFAD, toRead);
  
  backCache->pos = frontCache->endPos;
  backCache->relPos = frontCache->endPos % CDFS_SECTOR_SIZE;
  backCache->endPos = (numSectors * CDFS_SECTOR_SIZE) + toRead;
  backCache->size = toRead;
}

uint32_t stream_pos(binary_stream_t *stream) {
  return stream->dataCaches[stream->activeCacheIndex].pos;
}

uint32_t stream_relPos(binary_stream_t *stream) {
  return stream->dataCaches[stream->activeCacheIndex].relPos;
}

uint32_t stream_cache_size(binary_stream_t *stream) {
  return stream->dataCaches[stream->activeCacheIndex].size;
}

void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  void *dataCache0, void *dataCache1) {

  const uint32_t initialDataSize = MIN(entry->size,
    FILM_SAMPLE_CACHE_SIZE + FILM_SAMPLE_START_OFFSET);

  stream->startFAD = entry->starting_fad;
  stream->size = entry->size;

  stream->activeCacheIndex = 0;

  stream->dataPtr = (uint8_t*) dataCache0;

  stream->dataCaches[0].pos = 0;
  stream->dataCaches[0].relPos = 0;
  stream->dataCaches[0].size = initialDataSize;
  stream->dataCaches[0].data = (uint8_t*) dataCache0;

  stream->dataCaches[1].pos = 0;
  stream->dataCaches[1].relPos = 0;
  stream->dataCaches[1].size = 0;
  stream->dataCaches[1].data = (uint8_t*) dataCache1;

  memset(&stream->sampleCache, 0, sizeof(film_sample_cache_t));

  stream->remainingSize = stream->size;
  stream->eof = false;

  int status = cd_block_sectors_read(entry->starting_fad, dataCache0,
    initialDataSize);

  DEBUG_REQUIRE_EQ(status, 0);
}

void stream_readbytes(binary_stream_t *stream, void *tmpDst, uint32_t len) {
  DEBUG_REQUIRE_EQ(stream->eof, false);
  uint8_t *dst = (uint8_t *)tmpDst;

  // Swap buffers 
  data_cache_t *frontCache = &stream->dataCaches[stream->activeCacheIndex];
  while (frontCache->relPos + len >= frontCache->size) {
    const uint32_t skipBufferSize = (frontCache->size - frontCache->relPos);
    DEBUG_REQUIRE_GT(skipBufferSize, 0);

    if (dst != NULL) {
      memcpy(dst, &frontCache->data[frontCache->relPos], skipBufferSize);
      dst += skipBufferSize;
    }
    
    len -= skipBufferSize;
    frontCache->relPos += skipBufferSize;
    frontCache->pos += skipBufferSize;

    data_cache_t *backCache = &stream->dataCaches[!stream->activeCacheIndex];
    if (stream->remainingSize) {
      readQueuedCopy(CDFS_DATA_SELECTOR, backCache->data, backCache->size, true);
      stream->remainingSize -= backCache->size;
      
      // Swap buffers.
      stream->activeCacheIndex ^= 1;
      frontCache = &stream->dataCaches[stream->activeCacheIndex];
      stream->dataPtr = frontCache->data;

      // Read next stream if possible.
      data_cache_fetch_next_sample_data(stream);
    }

    if (!len)
      return;
  }

  DEBUG_REQUIRE_LE(frontCache->pos + len, stream->size);
  DEBUG_REQUIRE_LE(frontCache->relPos + len, frontCache->size);
  memcpy(dst, &frontCache->data[frontCache->relPos], len);

  frontCache->relPos += len;
  frontCache->pos += len;
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

uint16_t *vdp2ImagePtr = (uint16_t *)VDP2_VRAM_ADDR(0, 16 * VDP2_WIDTH);
inline void writeYUV(uint8_t cy, int16_t cr, int16_t cg, int16_t cb,
  uint32_t index) {

  DEBUG_REQUIRE_LT(index, VDP2_WIDTH * VDP2_HEIGHT);

  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int r = cy + cr;
  const int g = cy + cg;
  const int b = cy + cb;

  // TODO:
  const uint8_t nr = CLAMP(r, 0, 255);
  const uint8_t ng = CLAMP(g, 0, 255);
  const uint8_t nb = CLAMP(b, 0, 255);

  vdp2ImagePtr[index] = COLOR_RGB1888_RGB1555(1, nr, ng, nb).raw;
}

// Write the same value twice horizontally
inline void writeYUV2(uint8_t cy, int16_t cr, int16_t cg, int16_t cb,
  uint32_t index) {

  DEBUG_REQUIRE_LT(index, VDP2_WIDTH * VDP2_HEIGHT);

  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int r = cy + cr;
  const int g = cy + cg;
  const int b = cy + cb;

  // TODO:
  const uint8_t nr = CLAMP(r, 0, 255);
  const uint8_t ng = CLAMP(g, 0, 255);
  const uint8_t nb = CLAMP(b, 0, 255);

  const uint16_t color = COLOR_RGB1888_RGB1555(1, nr, ng, nb).raw;
  vdp2ImagePtr[index] = vdp2ImagePtr[index + 1] = color;
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
  const uint32_t x = data->writeX;
  const uint32_t y = data->writeY;

  // VDP2 image has 512x256
  uint32_t imageIndex = ((videoStartY + y) * VDP2_WIDTH) + x;
  DEBUG_REQUIRE_LT(imageIndex, VDP2_WIDTH * VDP2_HEIGHT);

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

  writeYUV2(e0.y[0], cr, cg, cb, imageIndex);
  writeYUV2(e0.y[1], cr, cg, cb, imageIndex + 2);
  
  if (y + 1 >= data->bottomY)
    return;

  imageIndex += VDP2_WIDTH;

  writeYUV2(e0.y[0], cr, cg, cb, imageIndex);
  writeYUV2(e0.y[1], cr, cg, cb, imageIndex + 2);
  
  if (y + 2 >= data->bottomY)
    return;
  
  imageIndex += VDP2_WIDTH;

  writeYUV2(e0.y[2], cr, cg, cb, imageIndex);
  writeYUV2(e0.y[3], cr, cg, cb, imageIndex + 2);
  
  if (y + 3 >= data->bottomY)
    return;
  
  imageIndex += VDP2_WIDTH;

  writeYUV2(e0.y[2], cr, cg, cb, imageIndex);
  writeYUV2(e0.y[3], cr, cg, cb, imageIndex + 2);
}

void renderPixel4(stripdata_t *data, uint8_t c0, uint8_t c1, uint8_t c2,
  uint8_t c3) {

  const codebook_t *codebook = stripdata_getV4Codebook(data);
  const codebook_t *e0 = &codebook[c0];
  const codebook_t *e1 = &codebook[c1];
  const codebook_t *e2 = &codebook[c2];
  const codebook_t *e3 = &codebook[c3];

  const uint32_t x = data->writeX;
  const uint32_t y = data->writeY;

  uint32_t imageIndex = ((videoStartY + y) * VDP2_WIDTH) + x;
  DEBUG_REQUIRE_LT(imageIndex, VDP2_WIDTH * VDP2_HEIGHT);
  
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
  writeYUV(e1->y[1], cr1, cg1, cb1, imageIndex);

  if (y + 1 >= data->bottomY)
    return;
  
  imageIndex += VDP2_WIDTH - 3;

  writeYUV(e0->y[2], cr0, cg0, cb0, imageIndex++);
  writeYUV(e0->y[3], cr0, cg0, cb0, imageIndex++);
  writeYUV(e1->y[2], cr1, cg1, cb1, imageIndex++);
  writeYUV(e1->y[3], cr1, cg1, cb1, imageIndex);

  if (y + 2 >= data->bottomY)
    return;
  
  imageIndex += VDP2_WIDTH - 3;

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
  writeYUV(e3->y[1], cr3, cg3, cb3, imageIndex);

  if (y + 3 >= data->bottomY)
    return;

  imageIndex += VDP2_WIDTH - 3;

  writeYUV(e2->y[2], cr2, cg2, cb2, imageIndex++);
  writeYUV(e2->y[3], cr2, cg2, cb2, imageIndex++);
  writeYUV(e3->y[2], cr3, cg3, cb3, imageIndex++);
  writeYUV(e3->y[3], cr3, cg3, cb3, imageIndex);
}
    
void readVectors(binary_stream_t *stream, stripdata_t *data,
  uint16_t chunkDataLength) {

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

void readVectorsInter(binary_stream_t *stream, stripdata_t *data,
  uint16_t chunkDataLength) {

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
    
void readV1VectorsInChunk(binary_stream_t *stream, stripdata_t *data,
  uint16_t chunkDataLength) {

  uint32_t remainingSectionBytes = chunkDataLength;

  uint32_t readBytes = 0;
  uint16_t originalWriteX = stripData.writeX;
  uint16_t originalWriteY = stripData.writeY;

  while (remainingSectionBytes) {
    if (data->writeY >= data->bottomY)
      break;

    readBytes++;
    remainingSectionBytes -= 1;
    stripdata_skipBlock(&stripData);
  }

  stripData.writeX = originalWriteX;
  stripData.writeY = originalWriteY;

  while (readBytes > 0) {
    const uint32_t readNow = MIN(readBytes, TMP_BUFFER_SIZE);

    stream_readbytes(stream, tmpBuffer, readNow);
    for (uint32_t i = 0; i < readNow; ++i) {
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

  if (chunkDataLength == 0)
    return;

  // If we are still copying the blocks.
  const uint32_t chunkIdPos = stream_pos(stream) - 4;
  waitCopyingBlocks();

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

      const uint32_t numReads = remainingSectionBytes / 6;
      const uint32_t numReadBytes = numReads * 6;

      stream_readbytes(stream, codebookPtr, numReadBytes);
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
      codebook_t *codebookPtr;
      if (chunkID == 0x2100)
        codebookPtr = stripdata_getV4Codebook(data);
      else
        codebookPtr = stripdata_getV1Codebook(data);

      uint32_t remainingSectionBytes = chunkDataLength;
      while (remainingSectionBytes >= 4) {
        uint32_t flags = stream_read32(stream);
        remainingSectionBytes -= 4;

        for (uint32_t i = 0; i < 32; ++i) {
          if (flags & 0x80000000) {
            codebook_new(codebookPtr, stream);
            remainingSectionBytes -= 6;
          }

          codebookPtr++;
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
    memset(vdp2ImagePtr, 0, VDP2_WIDTH * VDP2_HEIGHT * sizeof(uint16_t));
    dbgio_printf("Unknown chunk id 0x%X at offset %d, relpos %d\n", chunkID,
      chunkIdPos, stream_relPos(stream) - 4);

    sprintf((char *)LWRAM(256),
      "Unknown chunk id 0x%X at offset %d, relpos %d\n", chunkID,
      chunkIdPos, stream_relPos(stream) - 4);

    DEBUG_REQUIRE(false);
    break;
  }
}

void parseVideo(binary_stream_t *stream) {
  const uint8_t flags = stream_read8(stream);

#ifdef HAS_DEBUG_REQUIRE_FUNCTIONS
  // The deviant format length is 8 bytes shorter.
  // Those might be unused if LOGGER is disabled.
  const uint32_t cvidLength __unused = stream_read24(stream);
  const uint16_t width __unused = stream_read16(stream);
  const uint16_t height __unused = stream_read16(stream);
  const uint16_t numStrips = stream_read16(stream);
  LOGGER(
    "Reading video sample %dx%d with length = %d, flags = %d and %d strips\n",
    width, height, cvidLength, flags, numStrips);
#else
  stream_skip(stream, 7);
  const uint16_t numStrips = stream_read16(stream);
#endif

  // The deviant format as 2 extra bytes in the header.
  stream_skip(stream, 2);

  uint16_t lastBottomY = 0;
  for (uint16_t stripId = 0; stripId < numStrips; ++stripId) {
    stripData.strip = stripId;

    // flag bit 0 will tell if we need the contents of the previous strip
    if (stripId > 0 && (!(flags & 0x1)))
      stripdata_copyLastCodebooks(&stripData);

    const uint16_t cvidID __unused = stream_read16(stream);
    const uint16_t stripDataLength = stream_read16(stream);
    stripData.topY = stripData.writeY = stream_read16(stream);
    stripData.topX = stripData.writeX = stream_read16(stream);
    stripData.bottomY = stream_read16(stream);
    stripData.bottomX = stream_read16(stream);

    if (stripId > 0 && stripData.topY == 0) {
      stripData.topY = stripData.writeY = lastBottomY;
      stripData.bottomY += lastBottomY;
    }

    LOGGER("%u - %u Strip 0x%X, length = %d (%d,%d,%d,%d)\n",
      stream_pos(stream) - 12, stripId, cvidID, stripDataLength, stripData.topX,
      stripData.topY, stripData.bottomX, stripData.bottomY);
    
    // Read the strip chunks
    const uint32_t stripLimit = (uint32_t)(stream_pos(stream) +
      stripDataLength - 12);

    lastBottomY = stripData.bottomY;
    LOGGER("  Drawing to %dx%d\n", stripData.writeX, stripData.writeY);

    while (stream_pos(stream) < stripLimit) {
      const uint16_t cvidChunkID = stream_read16(stream);
      const uint16_t cvidChunkDataLength = stream_read16(stream) - 4;

      const size_t expectedEnd = stream_pos(stream) + cvidChunkDataLength;
      LOGGER("%u - Chunk ID 0x%X, length = %d (up to %u) to %d,%d\n",
        stream_pos(stream) - 4, cvidChunkID, cvidChunkDataLength, expectedEnd,
        stripData.writeX, stripData.writeY);
      
      readChunk(cvidChunkID, cvidChunkDataLength, stream, &stripData);
      LOGGER("%u - End chunk\n", stream_pos(stream));
      DEBUG_REQUIRE_EQ(stream_pos(stream), expectedEnd);

    } // Strip data

    DEBUG_REQUIRE_EQ(stream_pos(stream), stripLimit);
  }
}
    
void parseSample(const film_sample_t *sample, binary_stream_t *stream) {
  if (film_sample_is_audio(sample)) {
    stream_skip(stream, sample->length);
  } else {
    parseVideo(stream);
  }
}

void initialize_film() {
  cpu_dmac_memset(0, vdp2ImagePtr, 0,
    VDP2_WIDTH * VDP2_HEIGHT * sizeof(uint16_t));
  
  const int status = cd_block_cmd_sector_length_set(SECTOR_LENGTH_2048);
  DEBUG_REQUIRE_EQ(status, 0);
}

void play_film(cdfs_filelist_entry_t *entry, void *dataCache0,
  void *dataCache1) {

  clearConsole();
  stripdata_new(&stripData);

  DEBUG_REQUIRE(entry != NULL);
  dbgio_printf("%s (%d bytes), FAD: %d\n", entry->name, entry->size,
    entry->starting_fad);

  // Start reading the file
  binary_stream_t stream;
  stream_new(&stream, entry, dataCache0, dataCache1);

  const uint32_t asciiFilm = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiFilm, ASCII_FILM);
  const uint32_t filmHeaderLength = stream_read32(&stream);

  const uint32_t filmVersion __unused = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(filmVersion, ASCII_1d09);

  stream_skip(&stream, 4); // Unknown
  
  const uint32_t asciiFdsc = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiFdsc, ASCII_FDSC);

  // FDSC length.
  stream_skip(&stream, 4);

  const uint32_t asciiCvid = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiCvid, ASCII_CVID);

  videoHeight = stream_read32(&stream);
  videoWidth = stream_read32(&stream);
  videoStartY = (VIDEO_HEIGHT - videoHeight) / 2;

  const uint8_t videoBPP = stream_read8(&stream);
  const uint8_t audioChannels = stream_read8(&stream);
  const uint8_t audioSamplingResolution = stream_read8(&stream);
  const uint8_t audioCompression = stream_read8(&stream);
  const uint16_t audioSamplingFrequencyHz = stream_read16(&stream);

  dbgio_printf("Found %dx%d@%dbpp video\n", videoWidth, videoHeight, videoBPP);
  dbgio_printf("Centering at y = %d\n", videoStartY);
  dbgio_printf("Audio: %d channels, %d bits (comp = %d), %d Hz\n",
    audioChannels, audioSamplingResolution, audioCompression,
    audioSamplingFrequencyHz);
 
  // Unknown
  stream_skip(&stream, 6);

  const uint32_t asciiStab = stream_read32(&stream);
  DEBUG_REQUIRE_EQ(asciiStab, ASCII_STAB);

  // STAB length.
  stream_skip(&stream, 4);

  const uint32_t framerateBaseFrequencyHz = stream_read32(&stream);
  const uint32_t numSamples = stream_read32(&stream);

  dbgio_printf("Frame rate base frequency: %d Hz with %d samples\n",
    framerateBaseFrequencyHz, numSamples);
  
  const uint32_t sampleDescriptionPos = stream_pos(&stream);
  const uint32_t sampleDataPos = filmHeaderLength;

  // Must be called just before the sample list
  film_sample_cache_new(&stream.sampleCache, &stream, numSamples);
  
  // Queue up first data reading before we can start to process the frames.
  data_cache_fetch_first_sample_data(&stream, sampleDataPos);

  // Trigger next data copy.
  data_cache_fetch_next_sample_data(&stream);

  dbgio_printf("SamplePos: %d\nSampleDataPos: %d\nPos: %d\nRelPos: %d\n",
    sampleDescriptionPos, sampleDataPos, stream_pos(&stream),
    stream_relPos(&stream));

  dbgio_flush();
  cpu_frt_ovi_set(frtOviHandler);
  frtTimerStart(0);

  uint32_t samplesInSec = 0;
  uint32_t minSamplesInSec = 0xFFFFFFFF;
  uint32_t samplesInSecCount = 0;

  for (uint32_t sampleId = 0; sampleId < numSamples; ++sampleId) {
    uint32_t timeEllapsed = frtTimerEllapsed();
    if (timeEllapsed >= 1000) {
      samplesInSec = samplesInSecCount;
      if (samplesInSec < minSamplesInSec)
        minSamplesInSec = samplesInSec;

      samplesInSecCount = 0;
      frtTimerStart(timeEllapsed - 1000);
    }

    film_sample_t* sample = film_sample_get_next_sample(&stream);
    DEBUG_REQUIRE_NE(sample, NULL);

    parseSample(sample, &stream);
    samplesInSecCount++;

    clearConsole();
    dbgio_printf("Samples/s: %lu (%lu)\nMin: %lu\n", samplesInSec,
      samplesInSecCount, minSamplesInSec);
    dbgio_flush();

    LOGGER_FLUSH();
  }

  cpu_dmac_memset(0, vdp2ImagePtr, 0,
    VDP2_WIDTH * VDP2_HEIGHT * sizeof(uint16_t));
}

