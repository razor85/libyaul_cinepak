#include "decoder.h"
#include "md5.h"

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#ifndef MAX
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))
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
uint16_t *vdp2ImagePtr = (uint16_t *)VDP2_VRAM_ADDR(0, 16 * VDP2_WIDTH);

typedef struct {
  void *addr;
  uint32_t len;
} cache_purge_area_t;

// Timer
uint16_t frtOverflowCount = 0;
const uint32_t frtTimerDiv = CPU_FRT_NTSC_320_128_COUNT_1MS;
  
// For saving reads
#define TMP_BUFFER_SIZE 65536
__aligned(4) uint8_t tmpBuffer[TMP_BUFFER_SIZE];

// Keep track when we asked for updates
// TODO: Debugging only
uint32_t currentSampleId = 0;
uint32_t lastUpdateSampleId = 0;

#define HIRQ 0x0008UL
#define DRDY 0x0002 /* Data transfer preparations complete */
#define EHST 0x0080 /* Host I/O processing complete */

// Read 4 bytes from register
#define CD_BLOCK_DATA_4 0x25818000UL

// Read 2 bytes from register
#define CD_BLOCK_DATA_2 0x25890000UL

uint16_t stream_read16(binary_stream_t *);
uint32_t stream_read32(binary_stream_t *);
uint32_t stream_pos(binary_stream_t *stream);
void stream_readbytes(binary_stream_t *, void *, uint32_t);
void stream_skip(binary_stream_t *, uint32_t);

volatile bool dmaTransfering = false;
void waitDmac() {
  while (dmaTransfering)
    cpu_instr_nop();
}

void frtOviHandler() { frtOverflowCount++; }

static void frtTimerStart(uint16_t count) {
  cpu_frt_count_set(count);
  frtOverflowCount = 0;
}

static uint32_t frtTimerEllapsed() {
  uint32_t ticks = (0xFFFF * frtOverflowCount) + cpu_frt_count_get();
  return ticks / frtTimerDiv;
}

void md5PrintResult(MD5_CTX *md5) {
  unsigned char md5Result[2048];
  memset(md5Result, 0, 2048);
  MD5_Final(md5Result, md5);

  dbgio_printf("MD5\n");
  for (uint32_t i = 0; i < MD5_DIGEST_LENGTH; i++)
    dbgio_printf("%02x", md5Result[i]);
}

inline void clearConsole() { dbgio_printf("[H[2J"); }

void waitForCD() {
  cd_block_status_t status;
  while (true) {
    if (!cd_block_cmd_status_get(&status)) {
      DEBUG_REQUIRE_NE(status.cd_status, CD_STATUS_ERROR);
      DEBUG_REQUIRE_NE(status.cd_status, CD_STATUS_FATAL);

      if (status.cd_status == CD_STATUS_PAUSE)
        break;
    }
  }
}

inline uint32_t numSectorsForSize(uint32_t size) {
  // Past size so we get the complete number of sectors for the whole data.
  return (size + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
}

void queueDiskRead(uint32_t fad, uint32_t size) {
  DEBUG_REQUIRE_NE(size, 0);
  DEBUG_REQUIRE_EQ(size % 4, 0);
  waitForCD();

  int status __unused = cd_block_cmd_selector_reset(0, 0);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_cd_dev_connection_set(0);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_disk_play(0, fad, numSectorsForSize(size));
  DEBUG_REQUIRE_EQ(status, 0);
}

static inline uint32_t getSectorsReady(uint32_t wantSectors) {
  uint32_t sectorsReady;
  do {
    sectorsReady = cd_block_cmd_sector_number_get(0);
  } while (sectorsReady < wantSectors);

  return sectorsReady;
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
void dmaCopyBlocksDone(void *data __unused) {
  cpu_cache_purge();
  dmaTransfering = false;
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
  dmaTransfering = true;

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
void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples) {
  // We only read a single sector, so check how many samples we have there.
  const uint32_t samplesInitialPos __unused = stream_pos(stream);
  DEBUG_REQUIRE_EQ(FILM_SAMPLE_START_OFFSET, samplesInitialPos);

  if (totalNumSamples > stream->sampleCache.numSamples) {
    clearConsole();
    dbgio_printf("Insufficient number of cached samples: expected %d (%d "
                 "bytes), got %d (%d bytes)\n",
      totalNumSamples, totalNumSamples * sizeof(film_sample_t),
      stream->sampleCache.numSamples,
      stream->sampleCache.numSamples * sizeof(film_sample_t));

    dbgio_flush();
    vdp2_sync();

    VDP_INFLOOP();
  }

  uint32_t missingSamples = totalNumSamples;
  film_sample_t *outputSamples = stream->sampleCache.samples;

  while (missingSamples) {
    cd_film_sample_t sample;
    stream_readbytes(stream, &sample, sizeof(cd_film_sample_t));
    
    const bool isAudio = (sample.info1 == 0xFFFFFFFF);
    DEBUG_REQUIRE_EQ(sample.length % 4, 0);

    film_sample_t *newSample = outputSamples;
    if (isAudio) {
      newSample->interval = 0xFFFFFFFF;
    } else {
      newSample->interval = sample.info2;
    }

    newSample->length = sample.length;
    outputSamples++;
    missingSamples--;
  }
}

// Return the current sample, don't do anything else.
inline film_sample_t film_sample_get_next_sample(film_sample_cache_t *cache) {
  DEBUG_REQUIRE_LT(cache->currentSample, cache->numSamples);
  return cache->samples[cache->currentSample++];
}

void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  void *dataCache0, void *sampleCache, uint32_t sampleCacheSize) {

  stream->startFAD = entry->starting_fad;
  stream->size = entry->size;
  stream->remainingSectors = numSectorsForSize(entry->size);
  stream->dataAvailable = 0;
  stream->offset = 0;

  stream->sampleCache.samples = (film_sample_t *)sampleCache;
  stream->sampleCache.numSamples = sampleCacheSize / sizeof(film_sample_t);
  stream->sampleCache.currentSample = 0;
  stream->eof = false;

  // Fetch first sectors
  const uint32_t sectorsReady = getSectorsReady(MIN(stream->remainingSectors, 200));
  DEBUG_REQUIRE_GT(sectorsReady, 0);

  int status __unused = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
  DEBUG_REQUIRE_EQ(status, 0);

  // Wait until data is available
  while (!(MEMORY_READ(16, CD_BLOCK(HIRQ)) & (DRDY | EHST)))
    cpu_instr_nop();

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void triggerDataRequest(binary_stream_t *stream) {
  DEBUG_REQUIRE_NE(stream->remainingSectors, 0);
      
  // End previous transfers
  int status __unused = cd_block_cmd_data_transfer_end();
  DEBUG_REQUIRE_EQ(status, 0);

  const uint32_t sectorsReady = getSectorsReady(MIN(stream->remainingSectors, 16));
  DEBUG_REQUIRE_GT(sectorsReady, 0);

  status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
  DEBUG_REQUIRE_EQ(status, 0);

  // Wait until data is available
  while (!(MEMORY_READ(16, CD_BLOCK(HIRQ)) & (DRDY | EHST)))
    cpu_instr_nop();

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
  lastUpdateSampleId = currentSampleId;
}
  
void stream_readbytes(binary_stream_t *stream, void *tmpDst, uint32_t len) {
  DEBUG_REQUIRE_EQ(len % 2, 0);
  DEBUG_REQUIRE_NE(tmpDst, NULL);
  DEBUG_REQUIRE_LE(stream->offset + len, stream->size);

  volatile uint16_t *destPtr = (volatile uint16_t *)tmpDst;
  static volatile uint16_t *cdData = (volatile uint16_t *)CD_BLOCK_DATA_2;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 1;
    
    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      *destPtr++ = *cdData;
    }

    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;
  }

  stream->offset += len;
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

    volatile uint16_t nothing __unused;
    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      nothing = *cdData;
    }

    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;
  }

  stream->offset += len;
}

uint16_t stream_read16(binary_stream_t *stream) {
  uint16_t data;
  stream_readbytes(stream, &data, 2);

  return data;
}

uint32_t stream_read32(binary_stream_t *stream) {
  uint32_t data;
  stream_readbytes(stream, &data, 4);

  return data;
}

inline uint32_t stream_pos(binary_stream_t *stream) {
  return stream->offset;
}

inline bool film_sample_is_video(const film_sample_t* sample) {
  return sample->interval != 0xFFFFFFFF;
}

inline bool film_sample_is_audio(const film_sample_t* sample) {
  return sample->interval == 0xFFFFFFFF;
}

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

  const rgb1555_t color = RGB1555_INITIALIZER(1, nb >> 3, ng >> 3, nr >> 3);
  vdp2ImagePtr[index] = color.raw;
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

  const rgb1555_t color = RGB1555_INITIALIZER(1, nb >> 3, ng >> 3, nr >> 3);
  vdp2ImagePtr[index] = vdp2ImagePtr[index + 1] = color.raw;
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

  uint32_t flags = stream_read32(stream);
  uint32_t remainingSectionBytes = chunkDataLength - 4;

  DEBUG_REQUIRE_LT(remainingSectionBytes, TMP_BUFFER_SIZE);
  stream_readbytes(stream, tmpBuffer, remainingSectionBytes);

  uint8_t *tmpData = tmpBuffer;
  while (remainingSectionBytes) {
    if (data->writeY >= data->bottomY)
      break;

    for (uint32_t i = 0; i < 32; ++i) {
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

  DEBUG_REQUIRE_LT(chunkDataLength, TMP_BUFFER_SIZE);
  stream_readbytes(stream, tmpBuffer, chunkDataLength);
  
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

  while (readBytes > 0) {
    const uint32_t readNow = MIN(readBytes, TMP_BUFFER_SIZE);

    stream_readbytes(stream, tmpBuffer, readNow);
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
    {
      const uint32_t chunkIdPos = stream_pos(stream) - 4;

      memset(vdp2ImagePtr, 0, VDP2_WIDTH * VDP2_HEIGHT * sizeof(uint16_t));
      dbgio_printf("Unknown chunk id 0x%X at offset %d\n", chunkID, chunkIdPos);
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
  videoHeader cvidHeader;
  stream_readbytes(stream, &cvidHeader, sizeof(videoHeader));

  const bool copyLastCodeBooks = !(cvidHeader.flagsAndCvidLength.b[0] & 0x1);

  uint16_t lastBottomY = 0;
  for (uint16_t stripId = 0; stripId < cvidHeader.numStrips; ++stripId) {
    stripData.strip = stripId;

    // flag bit 0 will tell if we need the contents of the previous strip
    if (stripId > 0 && copyLastCodeBooks)
      stripdata_copyLastCodebooks(&stripData);

    uint16_t tmpStripBuffer[6];
    stream_readbytes(stream, tmpStripBuffer, 6 * sizeof(uint16_t));
    
    const uint16_t stripDataLength = tmpStripBuffer[1];

    stripData.topY = stripData.writeY = tmpStripBuffer[2];
    stripData.topX = stripData.writeX = tmpStripBuffer[3];
    stripData.bottomY = tmpStripBuffer[4];
    stripData.bottomX = tmpStripBuffer[5];

    if (stripId > 0 && stripData.topY == 0) {
      stripData.topY = stripData.writeY = lastBottomY;
      stripData.bottomY += lastBottomY;
    }

    LOGGER("%u - %u Strip 0x%X, length = %d (%d,%d,%d,%d)\n",
      stream_pos(stream) - 12, stripId, tmpStripBuffer[0], stripDataLength,
      stripData.topX, stripData.topY, stripData.bottomX, stripData.bottomY);

    // Read the strip chunks
    const uint32_t stripLimit = stream_pos(stream) + stripDataLength - 12;

    lastBottomY = stripData.bottomY;
    LOGGER("  Drawing to %dx%d\n", stripData.writeX, stripData.writeY);

    while (stream_pos(stream) < stripLimit) {
      const uint16_t cvidChunkID = stream_read16(stream);
      const uint16_t cvidChunkDataLength = stream_read16(stream) - 4;

      const size_t expectedEnd __unused = stream_pos(stream) + cvidChunkDataLength;
      LOGGER("%u - Chunk ID 0x%X, length = %d (up to %u) to %d,%d\n",
        stream_pos(stream) - 4, cvidChunkID, cvidChunkDataLength, expectedEnd,
        stripData.writeX, stripData.writeY);

      if (cvidChunkDataLength)
        readChunk(cvidChunkID, cvidChunkDataLength, stream, &stripData);

      DEBUG_REQUIRE_EQ(stream_pos(stream), expectedEnd);
    } // Strip data

    DEBUG_REQUIRE_EQ(stream_pos(stream), stripLimit);
  }
}
    
inline void parseSample(const film_sample_t *sample, binary_stream_t *stream) {
  if (film_sample_is_audio(sample)) {
    stream_skip(stream, sample->length);
  } else {
    parseVideo(stream);
  }
}

void initialize_film() {
  memset(vdp2ImagePtr, 0, VDP2_WIDTH * VDP2_HEIGHT * sizeof(uint16_t));
  
  const int status __unused = cd_block_cmd_sector_length_set(SECTOR_LENGTH_2048);
  DEBUG_REQUIRE_EQ(status, 0);
}

void film_vblank() {
}

void play_film(cdfs_filelist_entry_t *entry, void *dataCache0,
  void *sampleCache, uint32_t sampleCacheSize) {

  queueDiskRead(entry->starting_fad, entry->size);
  clearConsole();

  stripdata_new(&stripData);

  DEBUG_REQUIRE(entry != NULL);
  dbgio_printf("%s (%d bytes), FAD: %d\n", entry->name, entry->size,
    entry->starting_fad);

  // Start reading the file
  binary_stream_t stream;
  stream_new(&stream, entry, dataCache0, sampleCache, sampleCacheSize);

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
  const uint8_t videoBPP = (ABCD >> 24) & 0xFF;
  const uint8_t audioChannels = (ABCD >> 16) & 0xFF;
  const uint8_t audioSamplingResolution = (ABCD >> 8) & 0xFF;
  const uint8_t audioCompression = ABCD & 0xFF;
  const uint16_t audioSamplingFrequencyHz = stream_read16(&stream);

  dbgio_printf("Found %dx%d@%dbpp video\n", videoWidth, videoHeight, videoBPP);
  dbgio_printf("Centering at y = %d\n", videoStartY);
  dbgio_printf("Audio: %d channels, %d bits (comp = %d), %d Hz\n",
    audioChannels, audioSamplingResolution, audioCompression,
    audioSamplingFrequencyHz);
 
  // Unknown
  stream_skip(&stream, 6);

  const uint32_t asciiStab __unused = stream_read32(&stream);
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
  film_sample_cache_new(&stream, numSamples);
  
  dbgio_printf("SamplePos: %d\nSampleDataPos: %d\nPos: %d\n",
    sampleDescriptionPos, sampleDataPos, stream_pos(&stream));

  dbgio_flush();
  vdp2_sync();

  cpu_frt_ovi_set(frtOviHandler);
  frtTimerStart(0);

  uint32_t samplesInSec = 0;
  uint32_t minSamplesInSec = 0xFFFFFFFF;
  uint32_t samplesInSecCount = 0;

  uint32_t *debugData = (uint32_t*) LWRAM(0);
  
  clearConsole();
  dbgio_flush();
  vdp2_sync();

  for (uint32_t sampleId = 0; sampleId < 0xFFFFFFFF; ++sampleId) {
    currentSampleId = sampleId;

    uint32_t timeEllapsed = frtTimerEllapsed();
    if (timeEllapsed >= 1000) {
      samplesInSec = samplesInSecCount;
      if (samplesInSec < minSamplesInSec)
        minSamplesInSec = samplesInSec;

      samplesInSecCount = 0;
      frtTimerStart(timeEllapsed - 1000);
    }

    film_sample_t sample = film_sample_get_next_sample(&stream.sampleCache);
    parseSample(&sample, &stream);
    samplesInSecCount++;

    // Samples, samplesInSecCount, minSamplesInSec, isUpdating.
    debugData[0] = samplesInSec;
    debugData[1] = samplesInSecCount;
    debugData[2] = minSamplesInSec;
    debugData[3] = currentSampleId == lastUpdateSampleId;

    if (film_sample_is_video(&sample))
      vdp2_sync();
  }
    
  const int status __unused = cd_block_cmd_data_transfer_end();
  DEBUG_REQUIRE_EQ(status, 0);

  memset(vdp2ImagePtr, 0, VDP2_WIDTH * VDP2_HEIGHT * sizeof(uint16_t));
}

