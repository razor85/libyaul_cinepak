#include "cd.h"
#include "decoder.h"
#include "pcmsys.h"

#ifndef MIN
#  define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#ifndef CLAMP
#  define CLAMP(V, X, Y) ((V) < (X) ? (X) : ((V) > (Y) ? (Y) : (V)))
#endif

#define CAST_DATA16(X) (volatile uint16_t *) (X)
#define CAST_DATA32(X) (volatile uint32_t *) (X)
#define VIDEO_WIDTH    320
#define VIDEO_HEIGHT   240
#define COLOR_DEPTH_15 15
#define COLOR_DEPTH_24 24

uint32_t *vdp2DestinationBuffer = (uint32_t *) VDP2_VRAM_ADDR(0, 0);
uint32_t lwrambuffer = 80;

// Audio data

// Timer
volatile uint32_t frtOverflowCount = 0;
const uint32_t frtTimerDiv = CPU_FRT_NTSC_320_128_COUNT_1MS;

uint16_t stream_read16(binary_stream_t *);
uint32_t stream_read32(binary_stream_t *);
uint32_t stream_pos(binary_stream_t *stream);
void stream_readbytes(binary_stream_t *, volatile uint16_t *, uint32_t);

void stream_skip(binary_stream_t *, uint32_t);

void frtOviHandler() { frtOverflowCount++; }

static void frtTimerStart(uint16_t count) {
  cpu_frt_count_set(count);
  frtOverflowCount = 0;
}

static uint32_t frtTimerEllapsed() {
  uint32_t ticks = (0xFFFF * frtOverflowCount) + cpu_frt_count_get();
  return ticks / frtTimerDiv;
}

inline void codebook_new(codebook_t *cb, binary_stream_t *stream) {
  readBytesFromRingBuff(stream, CAST_DATA16(cb), sizeof(codebook_t));
}

inline void codebookRGB_new(codebook_t *book, codebookRGB_t *cbrgb) {
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

    rgb888_t rgb = RGB888_INITIALIZER(1, nr, ng, nb);
    rgb1555_t rgb15 = RGB1555_INITIALIZER(1, nr >> 3, ng >> 3, nb >> 3);

    cbrgb->color24[i] = rgb.raw;
    cbrgb->color[i] = rgb15.raw;
  }
}

void stripdata_new(stripdata_t *data) {
  // memset(data->codebooks, 0, MAX_STRIPS * sizeof(strip_codebook_t));
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
  waitCopyingBlocks();

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src_mode = CPU_DMAC_SOURCE_INCREMENT,
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .stride = CPU_DMAC_STRIDE_16_BYTES,
    .bus_mode = CPU_DMAC_BUS_MODE_BURST,
    .src = CPU_CACHE_THROUGH | (uint32_t) &data->codebooks[data->strip - 1],
    .dst = CPU_CACHE_THROUGH | (uint32_t) &data->codebooks[data->strip],
    .len = sizeof(strip_codebook_t),
    .ihr = dmaCopyBlocksDone,
    .ihr_work = NULL,
  };

  copyingBlocks = true;

  cpu_dmac_channel_config_set(&cfg);
  cpu_dmac_channel_start(0);
}

inline static void waitCopyingVideoFrame(decode_work_t *work) {
  if (work->copyingVideoFrame != 0) {
    while (vdp_dma_count_get() > 0)
      cpu_instr_nop();

    work->copyingVideoFrame = 0;
  }
}

void copyVideoFrame(decode_work_t *work) {
  waitCopyingVideoFrame(work);

  if (work->filmHeader.fdsc.chroma_key == COLOR_DEPTH_15) {
    uint16_t *vdp2Image15Ptr = work->decodeParams->vramBuffAddr;
    vdp_dma_enqueue(vdp2DestinationBuffer + work->dma_delta,
      vdp2Image15Ptr + work->dma_delta, work->decodeParams->vramBuffSize);
  } else {
    uint32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;
    vdp_dma_enqueue(vdp2DestinationBuffer + work->dma_delta,
      vdp2ImagePtr + work->dma_delta, work->decodeParams->vramBuffSize);
  }

  work->copyingVideoFrame = vdp_dma_count_get();
}

void stripdata_copyLastCodebooks_nodma(stripdata_t *data) {
  memcpy(&data->codebooks[data->strip], &data->codebooks[data->strip - 1],
    sizeof(strip_codebook_t));
}

inline codebook_t *stripdata_getV1Codebook(stripdata_t *data) {
  return data->codebooks[data->strip].v1;
}

inline codebookRGB_t *stripdata_getV1CodebookRGB(stripdata_t *data) {
  return data->codebooks[data->strip].v1RGB;
}

inline codebook_t *stripdata_getV4Codebook(stripdata_t *data) {
  return data->codebooks[data->strip].v4;
}

inline codebookRGB_t *stripdata_getV4CodebookRGB(stripdata_t *data) {
  return data->codebooks[data->strip].v4RGB;
}

// stream must be exactly at the start of the sample descriptions
void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples) {
  // We only read a single sector, so check how many samples we have there.
  const uint32_t samplesInitialPos __unused = stream_pos(stream);

  if (totalNumSamples > stream->sampleCache.numSamples) {
    sprintf((char *) LWRAM(80),
      "Insufficient number of cached samples: expected %d (%d "
      "bytes), got %d (%d bytes)\n",
      totalNumSamples, totalNumSamples * sizeof(film_sample_t),
      stream->sampleCache.numSamples,
      stream->sampleCache.numSamples * sizeof(film_sample_t));
  }

  film_sample_t *outputSamples = stream->sampleCache.samples;

  stream->sampleCache.ringBuffStart = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));
  stream->sampleCache.readPos = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));
  stream->sampleCache.writePos  = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));

  for (volatile uint32_t i = 0; i < totalNumSamples; ++i) {
    volatile cd_film_sample_t sample = {0};

    stream_readbytes(stream, CAST_DATA32(&sample), sizeof(cd_film_sample_t));

    const bool isAudio = (sample.info1 == 0xFFFFFFFF);

    film_sample_t *newSample = &outputSamples[i];
    if (isAudio) {
      newSample->interval = 0xFFFFFFFF;
    } else {
      newSample->interval = sample.info2;
    }

    newSample->length = sample.length;
  }

  uint32_t ringBuffSize = stream->sampleCache.ringBuffEnd - stream->sampleCache.ringBuffStart;
  if (stream->sampleCache.samples[stream->sampleCache.currentBuffSample].length > ringBuffSize) {
    sprintf((char *) LWRAM(80),
      "Insufficient RingBuffer size: Allocated %d, required: %d\n",
      ringBuffSize,
      stream->sampleCache.samples[stream->sampleCache.currentBuffSample].length);
  } else {
    initRingBuffer(stream);
  }
}

// Return the current sample, don't do anything else.
inline film_sample_t film_sample_get_next_sample(film_sample_cache_t *cache) {
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
  stream->sampleCache.ringBuffEnd = (uint8_t *) sampleCache + sampleCacheSize;


  // Fetch first sectors
  const uint32_t sectorsReady =
    getSectorsReady(MIN(stream->remainingSectors, SECTORS_PREFETCH));

  int status __unused = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);

  waitUntilCdDataIsAvailable();

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void triggerDataRequest(binary_stream_t *stream) {
  // End previous transfers
  int status __unused = cd_block_cmd_data_transfer_end();

  const uint32_t sectorsReady =
    getSectorsReady(MIN(stream->remainingSectors, SECTORS_PREFETCH));

  while (true) {
    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
    if (status & CD_STATUS_WAIT) {
      for (volatile uint32_t i = 0; i < 4096; ++i) {
        cpu_instr_nop();
      }
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

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void readBytesFromRingBuff(
  binary_stream_t *stream, volatile uint16_t *destPtr, uint32_t len) {
    memcpy(destPtr, stream->sampleCache.readPos, len);
    stream->sampleCache.readPos += len;
}

void initRingBuffer(binary_stream_t* stream) {
    film_sample_t nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];

    uint32_t ringBuffSize = stream->sampleCache.ringBuffEnd - stream->sampleCache.ringBuffStart;
    while (ringBuffSize > 0) {
        if (stream->sampleCache.writePos + nextSample.length >
            stream->sampleCache.ringBuffEnd) {
          stream->sampleCache.writePos = stream->sampleCache.ringBuffStart;
            break;
        } else {
          stream_readbytes(stream, stream->sampleCache.writePos, nextSample.length);
            stream->sampleCache.samples[stream->sampleCache.currentBuffSample].offset = stream->sampleCache.writePos;

            stream->sampleCache.writePos += nextSample.length;
            stream->sampleCache.currentBuffSample++;
            nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];
        }
    }


}

void readBytesIntoRingBuff(binary_stream_t* stream) {

    film_sample_t nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];
    film_sample_t currentSample = stream->sampleCache.samples[stream->sampleCache.currentSample];

    uint8_t * stopPoint = currentSample.offset;
    if (currentSample.offset == 0) {
      currentSample =
        stream->sampleCache.samples[stream->sampleCache.currentSample - 1];
        stopPoint = stream->sampleCache.ringBuffEnd;
    }

    if (stream->sampleCache.writePos + nextSample.length >
        stream->sampleCache.ringBuffEnd) {
      stream->sampleCache.writePos = stream->sampleCache.ringBuffStart; 
    }

    if (stream->sampleCache.currentBuffSample <=
        stream->sampleCache.currentSample) {
        stream->sampleCache.writePos = stream->sampleCache.ringBuffStart; 
    }

    uint8_t iter = 0;
      while (iter < 1 && stream->sampleCache.writePos + nextSample.length < stopPoint) {
        stream_readbytes(
          stream, stream->sampleCache.writePos, nextSample.length);
        stream->sampleCache.samples[stream->sampleCache.currentBuffSample]
          .offset = stream->sampleCache.writePos;

        stream->sampleCache.writePos += nextSample.length;
        stream->sampleCache.currentBuffSample++;
        nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];
        iter++;
      }
}

void stream_readbytes(
  binary_stream_t *stream, volatile uint16_t *destPtr, uint32_t len) {
  static volatile uint16_t *cdData = (volatile uint16_t *) CD_BLOCK_DATA_2;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 1;

    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;

    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      *destPtr = *cdData;
      destPtr++;
    }
  }

  stream->offset += len;
}

void stream_skip(binary_stream_t *stream, uint32_t len) {
  static volatile uint16_t *cdData = (volatile uint16_t *) CD_BLOCK_DATA_2;

  uint32_t remainingBytes = len;
  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream);

    const uint32_t readSize = MIN(stream->dataAvailable, remainingBytes);
    const uint32_t readSizeLoop = readSize >> 1;

    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;

    volatile uint16_t nothing __unused;
    for (volatile uint32_t i = 0; i < readSizeLoop; ++i) {
      nothing = *cdData;
    }
  }

  stream->offset += len;
}

uint16_t stream_read16(binary_stream_t *stream) {
  volatile uint16_t data;
  readBytesFromRingBuff(stream, &data, 2);

  return data;
}

uint32_t stream_read32(binary_stream_t *stream) {
  volatile uint32_t data;
  readBytesFromRingBuff(stream, &data, 4);

  return data;
}

inline uint32_t stream_pos(binary_stream_t *stream) { return (uint32_t)stream->sampleCache.readPos; }

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

void renderPixel1(decode_work_t *work, uint8_t c0) {
  const codebookRGB_t *e0 = &stripdata_getV1CodebookRGB(&work->stripData)[c0];
  const uint32_t x = work->stripData.writeX;
  const uint32_t y = work->stripData.writeY;

  // VDP2 image has 512x256
  uint32_t imageIndex = ((work->videoStartY + y) * VIDEO_WIDTH) + x;

  // +----+----+  +---+  +---+
  // | y0 | y1 |  | u |  | v |
  // +----+----+  +---+  +---+
  // | y2 | y3 |
  // +----+----+

  uint16_t *vdp2Image15Ptr = work->decodeParams->vramBuffAddr;
  uint32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;

  if (y + 0 >= work->stripData.bottomY)
    return;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0->color24[0];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] =
      e0->color24[1];
  } else {
    vdp2Image15Ptr[imageIndex] = vdp2Image15Ptr[imageIndex + 1] = e0->color[0];
    vdp2Image15Ptr[imageIndex + 2] = vdp2Image15Ptr[imageIndex + 3] =
      e0->color[1];
  }

  if (y + 1 >= work->stripData.bottomY)
    return;

  imageIndex += VIDEO_WIDTH;

  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0->color24[0];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] =
      e0->color24[1];
  } else {
    vdp2Image15Ptr[imageIndex] = vdp2Image15Ptr[imageIndex + 1] = e0->color[0];
    vdp2Image15Ptr[imageIndex + 2] = vdp2Image15Ptr[imageIndex + 3] =
      e0->color[1];
  }

  if (y + 2 >= work->stripData.bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0->color24[2];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] =
      e0->color24[3];
  } else {
    vdp2Image15Ptr[imageIndex] = vdp2Image15Ptr[imageIndex + 1] = e0->color[2];
    vdp2Image15Ptr[imageIndex + 2] = vdp2Image15Ptr[imageIndex + 3] =
      e0->color[3];
  }

  if (y + 3 >= work->stripData.bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0->color24[2];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] =
      e0->color24[3];
  } else {
    vdp2Image15Ptr[imageIndex] = vdp2Image15Ptr[imageIndex + 1] = e0->color[2];
    vdp2Image15Ptr[imageIndex + 2] = vdp2Image15Ptr[imageIndex + 3] =
      e0->color[3];
  }
}

void renderPixel4(
  decode_work_t *work, uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3) {
  const codebookRGB_t *e0 = &stripdata_getV4CodebookRGB(&work->stripData)[c0];
  const codebookRGB_t *e1 = &stripdata_getV4CodebookRGB(&work->stripData)[c1];
  const codebookRGB_t *e2 = &stripdata_getV4CodebookRGB(&work->stripData)[c2];
  const codebookRGB_t *e3 = &stripdata_getV4CodebookRGB(&work->stripData)[c3];

  const uint32_t x = work->stripData.writeX;
  const uint32_t y = work->stripData.writeY;

  uint32_t imageIndex = ((work->videoStartY + y) * VIDEO_WIDTH) + x;

  // +------+------+------+------+
  // | e0y0 | e0y1 | e1y0 | e1y1 |
  // +------+------+------+------+
  // | e0y2 | e0y3 | e1y2 | e1y3 |
  // +------+------+------+------+
  // | e2y0 | e2y1 | e3y0 | e3y1 |
  // +------+------+------+------+
  // | e2y2 | e2y3 | e3y2 | e3y3 |
  // +------+------+------+------+

  if (y + 0 >= work->stripData.bottomY)
    return;

  uint16_t *vdp2Image15Ptr = work->decodeParams->vramBuffAddr;
  uint32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = e0->color24[0];
    vdp2ImagePtr[imageIndex + 1] = e0->color24[1];
    vdp2ImagePtr[imageIndex + 2] = e1->color24[0];
    vdp2ImagePtr[imageIndex + 3] = e1->color24[1];
  } else {
    vdp2Image15Ptr[imageIndex] = e0->color[0];
    vdp2Image15Ptr[imageIndex + 1] = e0->color[1];
    vdp2Image15Ptr[imageIndex + 2] = e1->color[0];
    vdp2Image15Ptr[imageIndex + 3] = e1->color[1];
  }
  if (y + 1 >= work->stripData.bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = e0->color24[2];
    vdp2ImagePtr[imageIndex + 1] = e0->color24[3];
    vdp2ImagePtr[imageIndex + 2] = e1->color24[2];
    vdp2ImagePtr[imageIndex + 3] = e1->color24[3];
  } else {
    vdp2Image15Ptr[imageIndex] = e0->color[2];
    vdp2Image15Ptr[imageIndex + 1] = e0->color[3];
    vdp2Image15Ptr[imageIndex + 2] = e1->color[2];
    vdp2Image15Ptr[imageIndex + 3] = e1->color[3];
  }
  if (y + 2 >= work->stripData.bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = e2->color24[0];
    vdp2ImagePtr[imageIndex + 1] = e2->color24[1];
    vdp2ImagePtr[imageIndex + 2] = e3->color24[0];
    vdp2ImagePtr[imageIndex + 3] = e3->color24[1];
  } else {
    vdp2Image15Ptr[imageIndex] = e2->color[0];
    vdp2Image15Ptr[imageIndex + 1] = e2->color[1];
    vdp2Image15Ptr[imageIndex + 2] = e3->color[0];
    vdp2Image15Ptr[imageIndex + 3] = e3->color[1];
  }
  if (y + 3 >= work->stripData.bottomY)
    return;

  imageIndex += VIDEO_WIDTH;
  if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_24) {
    vdp2ImagePtr[imageIndex] = e2->color24[2];
    vdp2ImagePtr[imageIndex + 1] = e2->color24[3];
    vdp2ImagePtr[imageIndex + 2] = e3->color24[2];
    vdp2ImagePtr[imageIndex + 3] = e3->color24[3];
  } else {
    vdp2Image15Ptr[imageIndex] = e2->color[2];
    vdp2Image15Ptr[imageIndex + 1] = e2->color[3];
    vdp2Image15Ptr[imageIndex + 2] = e3->color[2];
    vdp2Image15Ptr[imageIndex + 3] = e3->color[3];
  }
}

void readVectors(decode_work_t *work, uint16_t chunkDataLength) {
  uint32_t remainingSectionBytes = chunkDataLength;
    //stream_readbytes(&work->stream, tmpBuffer, chunkDataLength);
  waitCopyingVideoFrame(work);

 // uint8_t *tmpData = work->stream.sampleCache.readPos;
  uint32_t flags = 0;
  uint32_t shifts = 0;
  while (remainingSectionBytes &&
    work->stripData.writeY <= work->stripData.bottomY) {
    memcpy(&flags, work->stream.sampleCache.readPos, 4);
    work->stream.sampleCache.readPos += 4;
    remainingSectionBytes -= 4;

    for (uint32_t i = 0; i < 32; i++) {
      if (flags & 0x80000000) {
        // V4
        renderPixel4(work, work->stream.sampleCache.readPos[0],
          work->stream.sampleCache.readPos[1],
          work->stream.sampleCache.readPos[2],
          work->stream.sampleCache.readPos[3]);
        remainingSectionBytes -= 4;
        work->stream.sampleCache.readPos += 4;

      } else {
        // V1
        renderPixel1(work, work->stream.sampleCache.readPos[0]);
        remainingSectionBytes -= 1;
        work->stream.sampleCache.readPos += 1;
      }

      stripdata_skipBlock(&work->stripData);
      flags <<= 1;
    }
  }

  if (remainingSectionBytes) {
    work->stream.sampleCache.readPos += remainingSectionBytes;
    // stream_skip(&work->stream, remainingSectionBytes);
  }

}

void readVectorsInter(decode_work_t *work, uint16_t chunkDataLength) {
 // stream_readbytes(&work->stream, tmpBuffer, chunkDataLength);
  waitCopyingVideoFrame(work);

  uint32_t remainingSectionBytes = chunkDataLength - 4;

  // We keep reading flags as long as it is possible. We first read 4
  // bytes and then we start shifting them for the VLC. Once we reach
  // the end we try to read a new flag.
  uint32_t flags;
  memcpy(&flags, work->stream.sampleCache.readPos, 4);
  work->stream.sampleCache.readPos += 4;

  uint32_t shifts = 0;
  while (remainingSectionBytes) {
    // Running on VLC now, so we just keep consuming bytes (4 each time)
    // until there is nothing else. 0 => skip block, 1 => read next bit:
    // - next bit is 1 = V4
    // - next bit is 0 = V1
    if (flags & 0x80000000) {
      // We are at the last bit so we need to fetch the next flags and check
      // the first bit as if it was the next on this sequence.
      if (shifts == 31) {
        memcpy(&flags, work->stream.sampleCache.readPos, 4);
        work->stream.sampleCache.readPos += 4;
        remainingSectionBytes -= 4;

        shifts = 0;
      } else {
        flags <<= 1;
        shifts++;
      }

      if (flags & 0x80000000) {
        // V4
        renderPixel4(work, work->stream.sampleCache.readPos[0],
          work->stream.sampleCache.readPos[1],
          work->stream.sampleCache.readPos[2],
          work->stream.sampleCache.readPos[3]);
        work->stream.sampleCache.readPos += 4;
        remainingSectionBytes -= 4;
      } else {
        // V1
        renderPixel1(work, work->stream.sampleCache.readPos[0]);
        work->stream.sampleCache.readPos += 1;
        remainingSectionBytes -= 1;
      }
    }

    stripdata_skipBlock(&work->stripData);

    // If we read all the bits, we just fetch the next flags.
    if (shifts == 31) {
      if (remainingSectionBytes < 4)
        break;

      memcpy(&flags, work->stream.sampleCache.readPos, 4);
      work->stream.sampleCache.readPos += 4;
      remainingSectionBytes -= 4;

      shifts = 0;
    } else {
      flags <<= 1;
      shifts++;
    }
  }

 if (remainingSectionBytes) {
    work->stream.sampleCache.readPos += remainingSectionBytes;
    // stream_skip(&work->stream, remainingSectionBytes);
  }
}

void readV1VectorsInChunk(decode_work_t *work, uint16_t chunkDataLength) {
  uint32_t readBytes = 0;
  uint16_t originalWriteX = work->stripData.writeX;
  uint16_t originalWriteY = work->stripData.writeY;

  uint32_t remainingSectionBytes = chunkDataLength;
  while (remainingSectionBytes) {
    readBytes++;
    remainingSectionBytes -= 1;
    stripdata_skipBlock(&work->stripData);
  }

  work->stripData.writeX = originalWriteX;
  work->stripData.writeY = originalWriteY;

  waitCopyingVideoFrame(work);

   for (volatile uint32_t i = 0; i < readBytes; ++i) {
    const uint8_t c0 = work->stream.sampleCache.readPos[i];
    renderPixel1(work, c0);
    stripdata_skipBlock(&work->stripData);
  }
  work->stream.sampleCache.readPos += readBytes;


  if (remainingSectionBytes) {
    work->stream.sampleCache.readPos += remainingSectionBytes;
    // stream_skip(&work->stream, remainingSectionBytes);
  }

}

void readChunk(
  uint16_t chunkID, uint16_t chunkDataLength, decode_work_t *work) {
  // If we are still copying the blocks.
  waitCopyingBlocks();

  switch (chunkID) {
  // 12 bit V4 (0x2000) or V1(0x2200)
  case 0x2000:
  case 0x2200: {
    codebook_t *codebookPtr;
    codebookRGB_t *codebookRGBPtr;
    if (chunkID == 0x2000) {
      codebookPtr = stripdata_getV4Codebook(&work->stripData);
      codebookRGBPtr = stripdata_getV4CodebookRGB(&work->stripData);
    } else {
      codebookPtr = stripdata_getV1Codebook(&work->stripData);
      codebookRGBPtr = stripdata_getV1CodebookRGB(&work->stripData);
    }

    readBytesFromRingBuff(&work->stream, codebookPtr, chunkDataLength);
    uint32_t iter = 0;
    for (int32_t i = chunkDataLength; i >= 6; i -= 6) {
      codebookRGB_new(&codebookPtr[iter], &codebookRGBPtr[iter]);
      iter++;
    }

  } break;

  // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
  case 0x2100:
  case 0x2300: {
    codebook_t *codebookPtr;
    codebookRGB_t *codebookRGBPtr;
    if (chunkID == 0x2100) {
      codebookPtr = stripdata_getV4Codebook(&work->stripData);
      codebookRGBPtr = stripdata_getV4CodebookRGB(&work->stripData);
    } else {
      codebookPtr = stripdata_getV1Codebook(&work->stripData);
      codebookRGBPtr = stripdata_getV1CodebookRGB(&work->stripData);
    }

    uint32_t remainingSectionBytes = chunkDataLength;
    while (remainingSectionBytes >= 4) {
      uint32_t flags = stream_read32(&work->stream);
      remainingSectionBytes -= 4;

      for (uint32_t i = 0; i < 32; ++i) {
        if (flags & 0x80000000) {
          codebook_new(codebookPtr, &work->stream);
          codebookRGB_new(codebookPtr, codebookRGBPtr);
          remainingSectionBytes -= 6;
        }

        codebookPtr++;
        codebookRGBPtr++;
        flags <<= 1;
      }
    }

    if (remainingSectionBytes) {
      work->stream.sampleCache.readPos += remainingSectionBytes;   
      // stream_skip(&work->stream, remainingSectionBytes);
    }

  } break;

  // 8 bit V4
  case 0x2400:
    break;

  // 8 bit V1
  case 0x2600:
    break;

  // vectors
  case 0x3000:
    readVectors(work, chunkDataLength);
    // stream_skip(stream, chunkDataLength);
    break;

  // list of blocks from v1
  case 0x3100:
    readVectorsInter(work, chunkDataLength);
    break;

  case 0x3200:
    readV1VectorsInChunk(work, chunkDataLength);
    break;

  default: {
    const uint32_t chunkIdPos = work->stream.sampleCache.readPos - 4;

    uint16_t *vdp2Image15Ptr = work->decodeParams->vramBuffAddr;
    uint32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;
    if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_15) {
      memset(vdp2Image15Ptr, 0, VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint32_t));
    } else {
      memset(vdp2ImagePtr, 0, VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint32_t));
    }
    logMessage("Unknown chunk id 0x%X at offset 0x%X\n", chunkID, chunkIdPos);
    sprintf((char *) LWRAM(80), "Unknown chunk id 0x%X at offset 0x%X\n", chunkID,
      chunkIdPos);

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

void parseVideo(decode_work_t *work) {
  static_assert(sizeof(videoHeader) == 12);

  volatile videoHeader cvidHeader;
  readBytesFromRingBuff(
    &work->stream, CAST_DATA16(&cvidHeader), sizeof(videoHeader));

  const bool copyLastCodeBooks = !(cvidHeader.flagsAndCvidLength.b[0] & 0x1);

  uint16_t lastBottomY = 0;
  for (uint16_t stripId = 0; stripId < cvidHeader.numStrips; ++stripId) {
    work->stripData.strip = stripId;

    // flag bit 0 will tell if we need the contents of the previous strip
    if (stripId > 0 && copyLastCodeBooks) {
      stripdata_copyLastCodebooks_nodma(&work->stripData);
    }

    volatile uint16_t tmpStripBuffer[6];
    readBytesFromRingBuff(
      &work->stream, CAST_DATA32(tmpStripBuffer), 6 * sizeof(uint16_t));

    const uint16_t stripDataLength = tmpStripBuffer[1];

    work->stripData.topY = work->stripData.writeY = tmpStripBuffer[2];
    work->stripData.topX = work->stripData.writeX = tmpStripBuffer[3];
    work->stripData.bottomY = tmpStripBuffer[4];
    work->stripData.bottomX = tmpStripBuffer[5];

    if (stripId > 0 && work->stripData.topY == 0) {
      work->stripData.topY = work->stripData.writeY = lastBottomY;
      work->stripData.bottomY += lastBottomY;
    }

    // Read the strip chunks
    const uint32_t stripLimit = work->stream.sampleCache.readPos + stripDataLength - 12;
    lastBottomY = work->stripData.bottomY;

    while (work->stream.sampleCache.readPos < stripLimit) {
      const uint16_t cvidChunkID = stream_read16(&work->stream);
      const uint16_t cvidChunkDataLength = stream_read16(&work->stream) - 4;

      const size_t expectedEnd __unused =
        stream_pos(&work->stream) + cvidChunkDataLength;

      if (cvidChunkDataLength)
        readChunk(cvidChunkID, cvidChunkDataLength, work);

    } // Strip data
  }
}

void parseAudio(
  const film_sample_t *sample, binary_stream_t *stream, decode_work_t *work) {
  uint32_t length = sample->length;

  // TODO: Proper stereo
  if (work->filmHeader.fdsc.sound_channels == 2) {
    length >>= 1;
    work->stream.sampleCache.readPos += sample->length - length;

    //stream_skip(stream, sample->length - length);
  }

  uint32_t missingBytes = length;
  while (missingBytes > 0) {
    uint32_t readSize = film_audio_get_next_buffer_size();
    if (readSize > missingBytes) {
      readSize = missingBytes;
    }
    uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);

    readBytesFromRingBuff(stream, writeLocation, readSize);
    film_audio_notify_read_buffer_bytes(readSize);
    missingBytes -= readSize;
  }

    film_audio_play(0);

}

void initialize_film() {}
void init_film(cdfs_filelist_entry_t *fsEntry, decode_work_t *work,
  uint32_t vdp_height, uint32_t vdp_width) {
  queueDiskRead(fsEntry->starting_fad, fsEntry->size);
  stripdata_new(&work->stripData);

  stream_new(&work->stream, fsEntry, work->decodeParams->sampleBuffAddr,
    work->decodeParams->sampleBuffSize);
  stream_readbytes(&work->stream, &work->filmHeader, sizeof(film_header));

  film_audio_setup(work->filmHeader.fdsc.sample_rate >> 16,
    work->filmHeader.fdsc.sound_channels,
    work->filmHeader.fdsc.sound_resolution);

  work->stream.sampleCache.numSamples = work->filmHeader.stab.total_entries;

  // Must be called just before the sample list
  film_sample_cache_new(&work->stream, work->filmHeader.stab.total_entries);

  work->isDisplayReady = false;
  work->play_status = INIT;
  work->isDisplayReady = false;

  work->videoStartY = (vdp_height - work->filmHeader.fdsc.height) / 2;
  work->dma_delta = work->videoStartY * vdp_width;

  vdp2_sync();
  vdp2_sync_wait();
}

void cpk_play(decode_work_t *work) {
  // Ask the sound driver to stop the warm up sound and get ready to start
  // processing sounds
  film_audio_prepare_to_play();

  cpu_frt_ovi_set(frtOviHandler);
  frtTimerStart(0);

  work->frtOverflowCount = 0;
  work->ticksUntilNextFrame = 0;
  work->tickCount = 0;
  work->lastFrameTime = frtTimerEllapsed();
  work->copyingVideoFrame = 0;

  work->play_status = PLAY;
}

void cpk_task(decode_work_t *work) {
  switch (work->play_status) {
  case STOP:
    // TODO stop
    break;
  case PAUSE:
    // TODO Pause
    break;
  case PLAY:
    handle_play(work);
    break;
  case INIT:
    break;
  default:
    break;
  }
}

void handle_play(decode_work_t *work) {
  if (work->stream.sampleCache.currentSample <
    work->stream.sampleCache.numSamples) {
    work->nextSample = film_sample_get_next_sample(&work->stream.sampleCache);
    work->stream.sampleCache.readPos = work->nextSample.offset;
    bool isVideo = film_sample_is_video(&work->nextSample);

    work->timeEllapsed = frtTimerEllapsed();
    uint32_t deltaTime = work->timeEllapsed - work->lastFrameTime;
    if (deltaTime > 0) {
      work->lastFrameTime = work->timeEllapsed;
      work->tickCount += deltaTime;
    }

    if (work->timeEllapsed >= 1000) {
      frtTimerStart(work->timeEllapsed - 1000);
    }
    work->lastFrameTime = frtTimerEllapsed();

    if (isVideo) {
      parseVideo(work);
    } else {
      parseAudio(&work->nextSample, &work->stream, work);
    }
    readBytesIntoRingBuff(&work->stream);

    while (isVideo && work->tickCount < work->ticksUntilNextFrame) {
      /* work->nextSample =
        film_sample_get_next_sample(&work->stream.sampleCache);
      if (!film_sample_is_video(&work->nextSample)) {
        work->stream.sampleCache.currentSample--;
        parseAudio(&work->nextSample, &work->stream, work);
        readBytesIntoRingBuff(&work->stream);
      } else {
        work->stream.sampleCache.currentSample--;
      }*/
      work->timeEllapsed = frtTimerEllapsed();
      deltaTime = work->timeEllapsed - work->lastFrameTime;
      if (deltaTime > 0) {
        work->lastFrameTime = work->timeEllapsed;
        work->tickCount += deltaTime;
      }
    }
    if (isVideo) {
      work->isDisplayReady = true;
    }


  } else {
    work->play_status = END;
  }
}

bool cpk_display_ready(decode_work_t *work) { return work->isDisplayReady; }

void cpk_display_finished(decode_work_t *work) {
  work->ticksUntilNextFrame =
    (work->nextSample.interval * 1000) / work->filmHeader.stab.ticks_per_second;
  work->tickCount = 0;
  work->isDisplayReady = false;
}
