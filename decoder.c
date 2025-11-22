#include "cd.h"
#include "decoder.h"
#include "pcmsys.h"

#ifndef MIN
#  define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#define CAST_DATA16(X) (volatile uint16_t *) (X)
#define CAST_DATA32(X) (volatile uint32_t *) (X)
#define VIDEO_HEIGHT   240

uint32_t lwrambuffer = 80;

static cpu_dmac_cfg_t pcm_cfg = {
  .channel = 0,
  .src_mode = CPU_DMAC_SOURCE_INCREMENT,
  .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
  .stride = CPU_DMAC_STRIDE_16_BYTES,
  .bus_mode = CPU_DMAC_BUS_MODE_BURST,
  .src = 0,
  .dst = 0,
  .len = 0,
  .ihr = NULL,
  .ihr_work = NULL,
};

static scu_dma_handle_t cd_dma = {.dnr = (uintptr_t) 0x25818000UL,
  .dnw = 0,
  .dnc = 0,
  .dnad = 0x00000002,
  .dnmd = 0x00000000};


static inline void stream_readbytes(binary_stream_t *, uint16_t *, uint32_t);


void pcm_MemcpyDword(uint32_t *dst, uint32_t *src, int32_t dwsize) {
  dst += dwsize;
  src += dwsize;
  while (--dwsize >= 0) {
    *--dst = *--src;
  }
}

static inline uint8_t clampRGB(int16_t x) {
  if (x < 0)
    return 0;
  if (x > 255)
    return 255;
  return (uint8_t) x;
}

void codebookRGB_24(decode_work_t *work, bool isV4, uint16_t chunkID, int32_t remaining) {
  codebookRGB_t *codebookRGBPtr = isV4 ?
    work->stripData.codebooks[work->stripData.strip].v4RGB :
    work->stripData.codebooks[work->stripData.strip].v1RGB;

  int32_t iter = 0;
  if (chunkID != 0x2000 && chunkID != 0x2200) {
    uint8_t *readPtr = work->stream.sampleCache.readPos;
    do {
      uint32_t flags = (readPtr[0] << 24) | (readPtr[1] << 16) |
        (readPtr[2] << 8) | (readPtr[3]);
      readPtr += 4;
      iter += 4;
      uint8_t shifts = 32;
      do {
        if (flags & 0x80000000) {
          codebook_t *book = (codebook_t *) readPtr;
          // | r |   | 1.0  0.0  2.0 | | y |
          // | g | = | 1.0 -0.5 -1.0 | | u |
          // | b |   | 1.0  2.0  0.0 | | v |

          int16_t cr = (book->v << 1);
          int16_t cg = -(book->u >> 1) - book->v;
          int16_t cb = +(book->u << 1);
          int16_t y = book->y[0];
          int16_t r = y + cr;
          int16_t g = y + cg;
          int16_t b = y + cb;
          uint8_t nr = clampRGB(r);
          uint8_t ng = clampRGB(g);
          uint8_t nb = clampRGB(b);
          codebookRGBPtr->color[0] = nr | nb << 0x10 | ng << 8 | 0x80000000;

          y = book->y[1];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampRGB(r);
          ng = clampRGB(g);
          nb = clampRGB(b);
          codebookRGBPtr->color[1] = nr | nb << 0x10 | ng << 8 | 0x80000000;

          y = book->y[2];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampRGB(r);
          ng = clampRGB(g);
          nb = clampRGB(b);
          codebookRGBPtr->color[2] = nr | nb << 0x10 | ng << 8 | 0x80000000;

          y = book->y[3];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampRGB(r);
          ng = clampRGB(g);
          nb = clampRGB(b);
          codebookRGBPtr->color[3] = nr | nb << 0x10 | ng << 8 | 0x80000000;
          
          readPtr += 6;
          iter += 6;
        }
        codebookRGBPtr++;
        if (remaining < iter) {
          return;
        }
        flags <<= 1;
        shifts--;
      } while (shifts != 0);
    } while (true);
  } else {
      codebook_t *book = (codebook_t *) work->stream.sampleCache.readPos;
    do {
      // | r |   | 1.0  0.0  2.0 | | y |
      // | g | = | 1.0 -0.5 -1.0 | | u |
      // | b |   | 1.0  2.0  0.0 | | v |
      int16_t cr = (book->v << 1);
      int16_t cg = -(book->u >> 1) - book->v;
      int16_t cb = +(book->u << 1);

      int16_t y = book->y[0];
      int16_t r = y + cr;
      int16_t g = y + cg;
      int16_t b = y + cb;
      uint8_t nr = clampRGB(r);
      uint8_t ng = clampRGB(g);
      uint8_t nb = clampRGB(b);
      codebookRGBPtr->color[0] = nr | nb << 0x10 | ng << 8 | 0x80000000;

      y = book->y[1];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampRGB(r);
      ng = clampRGB(g);
      nb = clampRGB(b);
      codebookRGBPtr->color[1] = nr | nb << 0x10 | ng << 8 | 0x80000000;

      y = book->y[2];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampRGB(r);
      ng = clampRGB(g);
      nb = clampRGB(b);
      codebookRGBPtr->color[2] = nr | nb << 0x10 | ng << 8 | 0x80000000;

      y = book->y[3];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampRGB(r);
      ng = clampRGB(g);
      nb = clampRGB(b);
      codebookRGBPtr->color[3] = nr | nb << 0x10 | ng << 8 | 0x80000000;
      
      book++;
      codebookRGBPtr++;
      iter += 6;
    } while (iter < remaining);
  }  
}

void codebookRGB_15(decode_work_t *work, bool isV4, uint16_t chunkID,
  int32_t remaining) {
  codebookRGB_t *codebookRGBPtr = isV4 ?
    work->stripData.codebooks[work->stripData.strip].v4RGB :
    work->stripData.codebooks[work->stripData.strip].v1RGB;

  int32_t iter = 0;
  if (chunkID != 0x2000 && chunkID != 0x2200) {
    uint8_t *readPtr = work->stream.sampleCache.readPos;
    do {
      uint32_t flags = (readPtr[0] << 24) | (readPtr[1] << 16) |
        (readPtr[2] << 8) | (readPtr[3]);
      readPtr += 4;
      iter += 4;
      uint8_t shifts = 32;
      do {
        if (flags & 0x80000000) {
          codebook_t *book = (codebook_t *) readPtr;
          // | r |   | 1.0  0.0  2.0 | | y |
          // | g | = | 1.0 -0.5 -1.0 | | u |
          // | b |   | 1.0  2.0  0.0 | | v |

          int16_t cr = (book->v << 1);
          int16_t cg = -(book->u >> 1) - book->v;
          int16_t cb = +(book->u << 1);

          int16_t y = book->y[0];
          int16_t r = y + cr;
          int16_t g = y + cg;
          int16_t b = y + cb;
          uint8_t nr = clampRGB(r) >> 3;
          uint8_t ng = clampRGB(g) >> 3;
          uint8_t nb = clampRGB(b) >> 3;
          ((uint16_t *) codebookRGBPtr->color)[0] = nr | nb << 10 | ng << 5 | 0x8000;

          y = book->y[1];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampRGB(r) >> 3;
          ng = clampRGB(g) >> 3;
          nb = clampRGB(b) >> 3;
         ((uint16_t *) codebookRGBPtr->color)[1] = nr | nb << 10 | ng << 5 | 0x8000;

          y = book->y[2];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampRGB(r) >> 3;
          ng = clampRGB(g) >> 3;
          nb = clampRGB(b) >> 3;
          ((uint16_t *) codebookRGBPtr->color)[2] = nr | nb << 10 | ng << 5 | 0x8000;

          y = book->y[3];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampRGB(r) >> 3;
          ng = clampRGB(g) >> 3;
          nb = clampRGB(b) >> 3;
          ((uint16_t *) codebookRGBPtr->color)[3] = nr | nb << 10 | ng << 5 | 0x8000;

          readPtr += 6;
          iter += 6;
        }
        codebookRGBPtr++;
        if (remaining < iter) {
          return;
        }
        flags <<= 1;
        shifts--;
      } while (shifts != 0);
    } while (true);
  } else {
    codebook_t *book = (codebook_t *) work->stream.sampleCache.readPos;
    do {
      // | r |   | 1.0  0.0  2.0 | | y |
      // | g | = | 1.0 -0.5 -1.0 | | u |
      // | b |   | 1.0  2.0  0.0 | | v |

      int16_t cr = (book->v << 1);
      int16_t cg = -(book->u >> 1) - book->v;
      int16_t cb = +(book->u << 1);

      int16_t y = book->y[0];
      int16_t r = y + cr;
      int16_t g = y + cg;
      int16_t b = y + cb;
      uint8_t nr = clampRGB(r) >> 3;
      uint8_t ng = clampRGB(g) >> 3;
      uint8_t nb = clampRGB(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[0] = nr | nb << 10 | ng << 5 | 0x8000;

      y = book->y[1];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampRGB(r) >> 3;
      ng = clampRGB(g) >> 3;
      nb = clampRGB(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[1] = nr | nb << 10 | ng << 5 | 0x8000;

      y = book->y[2];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampRGB(r) >> 3;
      ng = clampRGB(g) >> 3;
      nb = clampRGB(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[2] = nr | nb << 10 | ng << 5 | 0x8000;

      y = book->y[3];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampRGB(r) >> 3;
      ng = clampRGB(g) >> 3;
      nb = clampRGB(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[3] = nr | nb << 10 | ng << 5 | 0x8000;

      book++;
      codebookRGBPtr++;
      iter += 6;
    } while (iter <= remaining);
  }
}

void stripdata_new(stripdata_t *data, uint32_t height, uint32_t width) {
  data->strip = 0;
  data->writeX = data->topX = 0;
  data->bottomX = width;
  data->writeY = data->topY = 0;
  data->bottomY = height;
}

bool copyingBlocks = false;

void dmaCopyBlocksDone(void *data __unused) {
  cpu_cache_purge();
  copyingBlocks = false;
}

void stripdata_copyLastCodebooks(stripdata_t *data) {

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

void stripdata_copyLastCodebooks_nodma(stripdata_t *data) {
  memcpy(&data->codebooks[data->strip], &data->codebooks[data->strip - 1],
    sizeof(strip_codebook_t));
}

// stream must be exactly at the start of the sample descriptions
void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples) {
  // We only read a single sector, so check how many samples we have there.

  film_sample_t *outputSamples = stream->sampleCache.samples;

  stream->sampleCache.ringBuffStart = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));
  stream->sampleCache.readPos = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));
  stream->sampleCache.writePos  = (uint8_t *) outputSamples + (totalNumSamples * (sizeof(film_sample_t)));

  for (uint32_t i = 0; i < totalNumSamples; i++) {
    cd_film_sample_t sample = {0};

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
  

  // Fetch first sectors
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

  // Wait until data is available
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


void readStereoPcmBytesFromRingBuff(binary_stream_t *stream,
  uint16_t *destPtrL, uint16_t *destPtrR, int32_t len, int32_t channelOffset) {
  uint32_t *srcL = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos;
  uint32_t *srcR = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos + channelOffset;
  
  pcm_cfg.src = srcL;
  pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtrL;
  pcm_cfg.len = len;

  cpu_dmac_channel_config_set(&pcm_cfg);
  cpu_dmac_channel_start(0);
  cpu_cache_purge();

  //pcm_MemcpyDword(destPtrL, stream->sampleCache.readPos, len);
 // memcpy(destPtrL, stream->sampleCache.readPos, len >> 2);

 // destPtrL += (len >> 2);

  pcm_cfg.src = srcR;
  pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtrR;
  pcm_cfg.len = len;
//

  cpu_dmac_channel_config_set(&pcm_cfg);
  cpu_dmac_channel_start(0);
  cpu_cache_purge();

  //memcpy(destPtrR, stream->sampleCache.readPos + channelOffset, len >> 2);

  // pcm_MemcpyDword(destPtrR, stream->sampleCache.readPos, len);
 // destPtrR += (len >> 2);
  stream->sampleCache.readPos += len * 2;
}

void readPcmBytesFromRingBuff(
  binary_stream_t *stream, uint16_t *destPtr, int32_t len) {

   uint32_t *dest = destPtr;
 // pcm_cfg.src = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos;
 // pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtr;
  //pcm_cfg.len = len;

 // cpu_dmac_channel_config_set(&pcm_cfg);
 // cpu_dmac_channel_start(0);
 // cpu_cache_purge();

  pcm_MemcpyDword(destPtr, stream->sampleCache.readPos, len >> 2);
  //destPtr += (len >> 2);
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

static inline void readBytesIntoRingBuff(binary_stream_t *stream) {
  uint8_t *writePtr = stream->sampleCache.writePos;
  film_sample_cache_t *cache = &stream->sampleCache;
  uint32_t currIdx = cache->currentSample;
  uint32_t buffIdx = cache->currentBuffSample;
  film_sample_t nextSample = cache->samples[buffIdx];
  film_sample_t currentSample = cache->samples[currIdx];
  uint32_t nextLen = nextSample.length;
  uint8_t * stopPoint = currentSample.offset;

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
      stream_readbytes(stream, writePtr, nextLen);
        cache->samples[buffIdx].offset = writePtr;

        writePtr += nextLen;
        buffIdx++;
     }
    stream->sampleCache.currentBuffSample = buffIdx;
    stream->sampleCache.writePos = writePtr;
}

static inline void stream_readbytes(binary_stream_t *stream,
  uint16_t *destPtr, uint32_t len) {

  uint32_t remainingBytes = len;
  //uint32_t sectors = len >> 0xB;
  //if (sectors << 0xB != len) {
  //  sectors++;
  //}

  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream, SECTORS_PREFETCH);
    uint32_t readSize = stream->dataAvailable;
    if (readSize > remainingBytes) {
      readSize = remainingBytes;
    }

    uint32_t readSizeLoop = readSize >> 1;   
   
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

inline bool film_sample_is_video(const film_sample_t *sample) {
  return sample->interval != 0xFFFFFFFF;
}

inline bool film_sample_is_audio(const film_sample_t *sample) {
  return sample->interval == 0xFFFFFFFF;
}
static void decodeIntra15(decode_work_t *work, uint16_t chunkDataLength,
  bool skipStripData) {
  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  uint32_t vramwidth = work->decodeParams->vramBufferWidth;
  uint32_t y = work->stripData.writeY;

  uint16_t rgb1, rgb2, rgb3, rgb4 = 0;

  uint32_t flags = 0;
  uint32_t shifts = 0;
  uint32_t vramDelta = vramwidth * 3;

  uint32_t quarterY = (work->stripData.bottomY - y) >> 2;
  uint32_t quarterX = work->stripData.bottomX >> 2;
  uint32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint16_t *y0 = work->decodeParams->vramWritePos;
  uint16_t *y1 = y0 + vramwidth;
  uint16_t *y2 = y1 + vramwidth;
  uint16_t *y3 = y2 + vramwidth;
  uint32_t isV4 = 0;
  
  if ((quarterY != 0) && (quarterX != 0)) {
    shifts = 1;
    xPos = quarterX;
    if (skipStripData == true) {
      shifts = -1;
      flags = 0;
    }
    do {
      do {
        shifts--;
        if (shifts == 0) {
          flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
          src += 4;
          shifts = 32;
        }
        isV4 = flags & 0x80000000;
        flags <<= 1;
        if (isV4 == 0) {
          codebook_rgbval =
            &work->stripData.codebooks[work->stripData.strip].v1RGB[src[0]];
          codebook_rgbval->color[0];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          y0[0] = rgb2;
          y0[1] = rgb2;
          y1[0] = rgb2;
          y1[1] = rgb2;
          y0[2] = rgb1;
          y0[3] = rgb1;
          y1[2] = rgb1;
          y1[3] = rgb1;
          rgb1 = codebook_rgbval->color[1] & 0xFFFF;
          rgb2 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y2[0] = rgb2;
          y2[1] = rgb2;
          y3[0] = rgb2;
          y3[1] = rgb2;
          y2[2] = rgb1;
          y2[3] = rgb1;
          y3[2] = rgb1;
          y3[3] = rgb1;
          src++;
        } else {
          codebook_rgbval =
            &work->stripData.codebooks[work->stripData.strip].v4RGB[src[0]];

          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y0[0] = rgb2;
          y0[1] = rgb1;
          y1[0] = rgb4;
          y1[1] = rgb3;

          codebook_rgbval =
            &work->stripData.codebooks[work->stripData.strip].v4RGB[src[1]];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y0[2] = rgb2;
          y0[3] = rgb1;
          y1[2] = rgb4;
          y1[3] = rgb3;

          codebook_rgbval =
            &work->stripData.codebooks[work->stripData.strip].v4RGB[src[2]];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y2[0] = rgb2;
          y2[1] = rgb1;
          y3[0] = rgb4;
          y3[1] = rgb3;

          codebook_rgbval =
            &work->stripData.codebooks[work->stripData.strip].v4RGB[src[3]];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y2[2] = rgb2;
          y2[3] = rgb1;
          y3[2] = rgb4;
          y3[3] = rgb3;
          src += 4;
        }

        y0 += 4;
        y1 += 4;
        y2 += 4;
        y3 += 4;
        xPos--;
      } while (xPos != 0);
      y0 += vramDelta;
      y1 += vramDelta;
      y2 += vramDelta;
      y3 += vramDelta;
      quarterY--;
      xPos = quarterX;
    } while (quarterY != 0);
  }
  work->decodeParams->vramWritePos = y0;
  work->stream.sampleCache.readPos += chunkDataLength;
}

static void decodeInter15(decode_work_t *work, uint16_t chunkDataLength) {
  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  uint32_t vramwidth = work->decodeParams->vramBufferWidth;
  uint32_t y = work->stripData.writeY;

  uint16_t rgb1, rgb2, rgb3, rgb4 = 0;

  uint32_t flags = 0;
  uint32_t shifts = 0;
  uint32_t vramDelta = vramwidth * 3;

  uint32_t quarterY = (work->stripData.bottomY - y) >> 2;
  uint32_t quarterX = work->stripData.bottomX >> 2;
  uint32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint16_t *y0 = work->decodeParams->vramWritePos;
  uint16_t *y1 = y0 + vramwidth;
  uint16_t *y2 = y1 + vramwidth;
  uint16_t *y3 = y2 + vramwidth;
  uint32_t isV4 = 0;

  if ((quarterY != 0) && (quarterX != 0)) {
    shifts = 1;
    xPos = quarterX;
    do {
      do {
        shifts--;
        if (shifts == 0) {
          flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
          src += 4;
          shifts = 32;
        }
        isV4 = flags & 0x80000000;
        flags <<= 1;
        if (isV4 != 0) {
          shifts--;
          if (shifts == 0) {
            flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
            src += 4;
            shifts = 32;
          }
          isV4 = flags & 0x80000000;
          flags <<= 1;
          if (isV4 == 0) {
            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v1RGB[src[0]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            y0[0] = rgb2;
            y0[1] = rgb2;
            y1[0] = rgb2;
            y1[1] = rgb2;
            y0[2] = rgb1;
            y0[3] = rgb1;
            y1[2] = rgb1;
            y1[3] = rgb1;
            rgb1 = codebook_rgbval->color[1] & 0xFFFF;
            rgb2 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y2[0] = rgb2;
            y2[1] = rgb2;
            y3[0] = rgb2;
            y3[1] = rgb2;
            y2[2] = rgb1;
            y2[3] = rgb1;
            y3[2] = rgb1;
            y3[3] = rgb1;
            src++;
          } else {
            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[0]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y0[0] = rgb2;
            y0[1] = rgb1;
            y1[0] = rgb4;
            y1[1] = rgb3;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[1]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y0[2] = rgb2;
            y0[3] = rgb1;
            y1[2] = rgb4;
            y1[3] = rgb3;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[2]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y2[0] = rgb2;
            y2[1] = rgb1;
            y3[0] = rgb4;
            y3[1] = rgb3;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[3]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1]  >> 16 & 0xFFFF;
            y2[2] = rgb2;
            y2[3] = rgb1;
            y3[2] = rgb4;
            y3[3] = rgb3;
            src += 4;
          }
        }
        y0 += 4;
        y1 += 4;
        y2 += 4;
        y3 += 4;
        xPos--;
      } while (xPos != 0);
      y0 += vramDelta;
      y1 += vramDelta;
      y2 += vramDelta;
      y3 += vramDelta;
      quarterY--;
      xPos = quarterX;
    } while (quarterY != 0);
  }
  work->decodeParams->vramWritePos = y0;
  work->stream.sampleCache.readPos += chunkDataLength;
}

void decodeInter24(decode_work_t *work,  uint16_t chunkDataLength) {
    int32_t remainingSectionBytes = chunkDataLength - 4;
    uint8_t *src = work->stream.sampleCache.readPos;
    uint32_t vramwidth = work->decodeParams->vramBufferWidth;
    uint32_t y = work->stripData.writeY;
    uint32_t rgb1, rgb2, rgb3, rgb4 = 0;
   
    uint32_t flags = 0;
    uint32_t shifts = 0;
    uint32_t vramDelta = vramwidth * 3;

    uint32_t quarterY = (work->stripData.bottomY - y) >> 2;
    uint32_t quarterX = work->stripData.bottomX >> 2;
    uint32_t xPos = quarterX;
    codebookRGB_t *codebook_rgbval;
    uint32_t *y0 = work->decodeParams->vramWritePos;
    uint32_t *y1 = y0 + vramwidth;
    uint32_t *y2 = y1 + vramwidth;
    uint32_t *y3 = y2 + vramwidth;
    uint32_t isV4 = 0;

    if ((quarterY != 0) && (quarterX != 0)) {
      shifts = 1;
      xPos = quarterX;
      do {
          do {
          shifts--;
          if (shifts == 0) {
            flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
                src += 4;
                shifts = 32;
          }
          isV4 = flags & 0x80000000;
          flags <<= 1;
          if (isV4 != 0) {
            shifts--;
            if (shifts == 0) {
              flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
              src += 4;
              shifts = 32;
            }
            isV4 = flags & 0x80000000;
            flags <<= 1;
            if (isV4 == 0) {
                codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v1RGB[src[0]];
                rgb1 = codebook_rgbval->color[0];
                rgb2 = codebook_rgbval->color[1];
                y0[0] = rgb1;
                y0[1] = rgb1;
                y1[0] = rgb1;
                y1[1] = rgb1;
                y0[2] = rgb2;
                y0[3] = rgb2;
                y1[2] = rgb2;
                y1[3] = rgb2;
                rgb1 = codebook_rgbval->color[2];
                rgb2 = codebook_rgbval->color[3];
                y2[0] = rgb1;
                y2[1] = rgb1;
                y3[0] = rgb1;
                y3[1] = rgb1;
                y2[2] = rgb2;
                y2[3] = rgb2;
                y3[2] = rgb2;
                y3[3] = rgb2;
                src++;
            } else {
              codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[0]];
              rgb1 = codebook_rgbval->color[0];
              rgb2 = codebook_rgbval->color[1];
              rgb3 = codebook_rgbval->color[2];
              rgb4 = codebook_rgbval->color[3];
              y0[0] = rgb1;
              y0[1] = rgb2;
              y1[0] = rgb3;
              y1[1] = rgb4;
              
             codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[1]];
              rgb1 = codebook_rgbval->color[0];
              rgb2 = codebook_rgbval->color[1];
              rgb3 = codebook_rgbval->color[2];
              rgb4 = codebook_rgbval->color[3];
             y0[2] = rgb1;
             y0[3] = rgb2;
             y1[2] = rgb3;
             y1[3] = rgb4;
            
             codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[2]];
              rgb1 = codebook_rgbval->color[0];
              rgb2 = codebook_rgbval->color[1];
              rgb3 = codebook_rgbval->color[2];
              rgb4 = codebook_rgbval->color[3];
             y2[0] = rgb1;
             y2[1] = rgb2;
             y3[0] = rgb3;
             y3[1] = rgb4;

             codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[3]];
              rgb1 = codebook_rgbval->color[0];
              rgb2 = codebook_rgbval->color[1];
              rgb3 = codebook_rgbval->color[2];
              rgb4 = codebook_rgbval->color[3];
             y2[2] = rgb1;
             y2[3] = rgb2;
             y3[2] = rgb3;
             y3[3] = rgb4;
             src += 4;
            }
          }
          y0 += 4;
          y1 += 4;
          y2 += 4;
          y3 += 4;
          xPos--;
          } while (xPos != 0);
          y0 += vramDelta;
          y1 += vramDelta;
          y2 += vramDelta;
          y3 += vramDelta;
          quarterY--;
          xPos = quarterX;
      } while (quarterY != 0);
    }
    work->decodeParams->vramWritePos = y0;
    work->stream.sampleCache.readPos += chunkDataLength;
}

void decodeIntra24(decode_work_t *work, uint16_t chunkDataLength,
  bool skipStripData) {

  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  uint32_t vramwidth = work->decodeParams->vramBufferWidth;
  uint32_t y = work->stripData.writeY;

  uint32_t rgb1, rgb2, rgb3, rgb4 = 0;

  uint32_t flags = 0;
  uint32_t shifts = 0;
  uint32_t vramDelta = vramwidth * 3;

  uint32_t quarterY = (work->stripData.bottomY - y) >> 2;
  uint32_t quarterX = work->stripData.bottomX >> 2;
  uint32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint32_t *y0 = work->decodeParams->vramWritePos;
  uint32_t *y1 = y0 + vramwidth;
  uint32_t *y2 = y1 + vramwidth;
  uint32_t *y3 = y2 + vramwidth;
  uint32_t isV4 = 0;

  if ((quarterY != 0) && (quarterX != 0)) {
    shifts = 1;
    xPos = quarterX;
    if (skipStripData == true) {
      shifts = -1;
      flags = 0;
    }
    do {
      do {
        shifts--;
        if (shifts == 0) {
          flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
          src += 4;
          shifts = 32;
        }
        isV4 = flags & 0x80000000;
        flags <<= 1;
        if (isV4 == 0) {
            codebook_rgbval =
            &work->stripData.codebooks[work->stripData.strip].v1RGB[src[0]];
            rgb1 = codebook_rgbval->color[0];
            rgb2 = codebook_rgbval->color[1];
            y0[0] = rgb1;
            y0[1] = rgb1;
            y1[0] = rgb1;
            y1[1] = rgb1;
            y0[2] = rgb2;
            y0[3] = rgb2;
            y1[2] = rgb2;
            y1[3] = rgb2;
            rgb1 = codebook_rgbval->color[2];
            rgb2 = codebook_rgbval->color[3];
            y2[0] = rgb1;
            y2[1] = rgb1;
            y3[0] = rgb1;
            y3[1] = rgb1;
            y2[2] = rgb2;
            y2[3] = rgb2;
            y3[2] = rgb2;
            y3[3] = rgb2;
            src++;
        } else {
            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[0]];
            rgb1 = codebook_rgbval->color[0];
            rgb2 = codebook_rgbval->color[1];
            rgb3 = codebook_rgbval->color[2];
            rgb4 = codebook_rgbval->color[3];
            y0[0] = rgb1;
            y0[1] = rgb2;
            y1[0] = rgb3;
            y1[1] = rgb4;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[1]];
            rgb1 = codebook_rgbval->color[0];
            rgb2 = codebook_rgbval->color[1];
            rgb3 = codebook_rgbval->color[2];
            rgb4 = codebook_rgbval->color[3];
            y0[2] = rgb1;
            y0[3] = rgb2;
            y1[2] = rgb3;
            y1[3] = rgb4;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[2]];
            rgb1 = codebook_rgbval->color[0];
            rgb2 = codebook_rgbval->color[1];
            rgb3 = codebook_rgbval->color[2];
            rgb4 = codebook_rgbval->color[3];
            y2[0] = rgb1;
            y2[1] = rgb2;
            y3[0] = rgb3;
            y3[1] = rgb4;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[3]];
            rgb1 = codebook_rgbval->color[0];
            rgb2 = codebook_rgbval->color[1];
            rgb3 = codebook_rgbval->color[2];
            rgb4 = codebook_rgbval->color[3];
            y2[2] = rgb1;
            y2[3] = rgb2;
            y3[2] = rgb3;
            y3[3] = rgb4;
            src += 4;
        }
        
        y0 += 4;
        y1 += 4;
        y2 += 4;
        y3 += 4;
        xPos--;
      } while (xPos != 0);
      y0 += vramDelta;
      y1 += vramDelta;
      y2 += vramDelta;
      y3 += vramDelta;
      quarterY--;
      xPos = quarterX;
    } while (quarterY != 0);
  }
  work->decodeParams->vramWritePos = y0;
  work->stream.sampleCache.readPos += chunkDataLength;
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

static void parseVideo(decode_work_t *work) {

  videoHeader *cvidHeader;
  cvidHeader = work->stream.sampleCache.readPos;
  work->stream.sampleCache.readPos += sizeof(videoHeader);
  uint16_t numStrips = cvidHeader->numStrips;
  uint16_t stripNum = 0;
  const bool copyLastCodeBooks = !(cvidHeader->flagsAndCvidLength.b[0] & 0x1);
  uint16_t lastBottomY = 0;

  uint8_t colorDepth = work->decodeParams->decodeColorDepth;
  void (*decodeIntra)(decode_work_t *work, uint16_t chunkDataLength, bool skipStripData); 
  void (*decodeInter)(decode_work_t * work, uint16_t chunkDataLength);
  void (*codebookRGB_new)(decode_work_t * work, bool isV4, uint16_t chunkID, uint32_t remaining); 
  
  if (colorDepth == COLOR_DEPTH_24) {
    decodeIntra = &decodeIntra24;
    decodeInter = &decodeInter24;
    codebookRGB_new = &codebookRGB_24;
  } else {
    decodeIntra = &decodeIntra15;
    decodeInter = &decodeInter15;
    codebookRGB_new = &codebookRGB_15;
  }

  if (numStrips != 0) {
      do {
          work->stripData.strip = stripNum;

          // Is this even necessary?
          // flag bit 0 will tell if we need the contents of the previous strip
          if (stripNum > 0 && copyLastCodeBooks) {
            stripdata_copyLastCodebooks_nodma(&work->stripData);
          }

          uint16_t *tmpStripBuffer;
          tmpStripBuffer = work->stream.sampleCache.readPos;
          work->stream.sampleCache.readPos += 6 * 2;

          uint16_t stripDataLength = tmpStripBuffer[1];

          work->stripData.topY = work->stripData.writeY = tmpStripBuffer[2];
          work->stripData.topX = work->stripData.writeX = tmpStripBuffer[3];
          work->stripData.bottomY = tmpStripBuffer[4];
          work->stripData.bottomX = tmpStripBuffer[5];

          if (stripNum > 0 && work->stripData.topY == 0) {
            work->stripData.topY = work->stripData.writeY = lastBottomY;
            work->stripData.bottomY += lastBottomY;
          }

          // Read the strip chunks
          uint32_t stripLimit =
            work->stream.sampleCache.readPos + stripDataLength - 12;
          lastBottomY = work->stripData.bottomY;
          if (work->stream.sampleCache.readPos < stripLimit) {
              do {
                  //uint8_t *readPtr = work->stream.sampleCache.readPos;
                uint16_t chunkID =
                    (work->stream.sampleCache.readPos[0] << 8) |
                    work->stream.sampleCache.readPos[1];
                  work->stream.sampleCache.readPos += 2;
                  uint16_t chunkDataLength =
                    ((work->stream.sampleCache.readPos[0] << 8) |
                      work->stream.sampleCache.readPos[1]) - 4;

                  work->stream.sampleCache.readPos += 2;
                  //work->stream.sampleCache.readPos = readPtr;
                    bool isV4 = (chunkID == 0x2000 || chunkID == 0x2100);
                    switch (chunkID) {
                        // 12 bit V4 (0x2000) or V1(0x2200)
                        case 0x2000:
                        case 0x2200: {
                          codebookRGB_new(work, isV4, chunkID, chunkDataLength);
                          work->stream.sampleCache.readPos += chunkDataLength;
                        } break;

                        // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
                        case 0x2100:
                        case 0x2300: {
                          codebookRGB_new(work, isV4, chunkID, chunkDataLength);
                          work->stream.sampleCache.readPos += chunkDataLength;
                        } break;

                        // 8 bit V4
                        case 0x2400:
                          break;

                        // 8 bit V1
                        case 0x2600:
                          break;

                        // vectors
                        case 0x3000:
                          decodeIntra(work, chunkDataLength, false);
                          break;
                        // list of blocks from v1
                        case 0x3100:
                            decodeInter(work, chunkDataLength);
                          break;
                        case 0x3200:
                            decodeIntra(work, chunkDataLength, true);
                          break;
                        default: {
                          const uint32_t chunkIdPos =
                            work->stream.sampleCache.readPos - 4;

                          uint16_t *vdp2Image15Ptr =
                            work->decodeParams->vramBuffAddr;
                          uint32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;
                          if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_15) {
                            memset(vdp2Image15Ptr, 0,
                              work->decodeParams->vramBufferWidth * VIDEO_HEIGHT *
                                sizeof(uint32_t));
                          } else {
                            memset(vdp2ImagePtr, 0,
                              work->decodeParams->vramBufferWidth * VIDEO_HEIGHT *
                                sizeof(uint32_t));
                          }
                          logMessage("Unknown chunk id 0x%X at offset 0x%X\n",
                            chunkID, chunkIdPos);
                          sprintf((char *) LWRAM(80),
                            "Unknown chunk id 0x%X at offset 0x%X\n", chunkID,
                            chunkIdPos);

                          break;
                        }
                    }
                  
            } while (work->stream.sampleCache.readPos < stripLimit);
          }
          stripNum++;

      } while (stripNum < numStrips);
  }
}

void parseAudio(decode_work_t *work, int32_t length) {
 
  if (work->filmHeader.fdsc.sound_channels == 2) {
      if (work->filmHeader.fdsc.sound_resolution == 8) {
      pcm_cfg.stride = CPU_DMAC_STRIDE_1_BYTE;
    }
      /*
        int32_t missingBytes = length;
        int32_t offset = length >> 1;
        while (missingBytes > 0) {
          int32_t readSize = film_audio_get_next_buffer_size();
          if (readSize > missingBytes) {
            readSize = missingBytes >> 1;
          }
          uint16_t *writeLocation_L = film_audio_get_next_buffer_ptr(0);
          uint16_t *writeLocation_R = film_audio_get_next_buffer_ptr(1);
          readStereoPcmBytesFromRingBuff(&work->stream, writeLocation_L,
            writeLocation_R, readSize, offset);

         film_audio_notify_read_buffer_bytes(readSize);
         missingBytes -= readSize * 2;
        }

        if (work->audioPlaying == false) {
          film_audio_play(0);
          work->audioPlaying = true;
        } 
        */
      int32_t missingBytes = length >> 1;
        
      while (missingBytes > 0) {
        int32_t readSize = film_audio_get_next_buffer_size();
        if (readSize > missingBytes) {
          readSize = missingBytes;
        }
        uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);

        readPcmBytesFromRingBuff(&work->stream, writeLocation, readSize);
        film_audio_notify_read_buffer_bytes(readSize);
        missingBytes -= readSize;
      }
      work->stream.sampleCache.readPos += length >> 1;

      if (work->audioPlaying == false) {
      //  work->audioWaitingToStart = true;
      }
    }
    else {
      int32_t missingBytes = length;

      while (missingBytes > 0) {
        int32_t readSize = film_audio_get_next_buffer_size();
        if (readSize > missingBytes) {
          readSize = missingBytes;
        }
        uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);

        readPcmBytesFromRingBuff(&work->stream, writeLocation, readSize);
        film_audio_notify_read_buffer_bytes(readSize);
        missingBytes -= readSize;
      }

      if (work->audioPlaying == false) {
        work->audioWaitingToStart = true;
      }
    }
        
  }

void init_film(cdfs_filelist_entry_t *fsEntry, decode_work_t *work,
  uint32_t vdp_height, uint32_t vdp_width) {
  queueDiskRead(fsEntry->starting_fad, fsEntry->size);

  stream_new(&work->stream, fsEntry, work->decodeParams->sampleBuffAddr,
    work->decodeParams->sampleBuffSize);

  char *filmPtr = &work->filmHeader;
  //Will fail to play a 2nd file without this.
  while (filmPtr[0] != 'F') {
    stream_readbytes(&work->stream, &work->filmHeader, sizeof(film_header));
    scu_dma_level_wait(0);
  }

  stripdata_new(&work->stripData, work->filmHeader.fdsc.height, work->filmHeader.fdsc.width);

  film_audio_setup(work, work->filmHeader.fdsc.sample_rate >> 16,
    work->filmHeader.fdsc.sound_channels,
    work->filmHeader.fdsc.sound_resolution);

  work->stream.sampleCache.currentSample = 0;
  work->stream.sampleCache.currentBuffSample = 0;
  work->stream.sampleCache.numSamples = work->filmHeader.stab.total_entries;
  work->stream.sampleCache.remainingPcmBytes = 0;
  if (work->filmHeader.fdsc.sound_resolution == 8) {
    work->stream.sampleCache.pcmBytesPerBlank = calculate_bytes_per_blank(
      work->filmHeader.fdsc.sample_rate >> 16, true, false);
  } else {
    work->stream.sampleCache.pcmBytesPerBlank = calculate_bytes_per_blank(
      work->filmHeader.fdsc.sample_rate >> 16, false, false);
  }


  // Must be called just before the sample list
  film_sample_cache_new(&work->stream, work->filmHeader.stab.total_entries);
  
  work->isDisplayReady = false;
  work->play_status = INIT;
  work->displayWaiting = false;

  work->videoStartY = 0;
  //(vdp_height - work->filmHeader.fdsc.height) / 2;
  work->dma_delta = work->videoStartY * vdp_width;
  work->audioPlaying = false;
  readBytesIntoRingBuff(&work->stream);
  
}
// Timer
volatile uint32_t frtOverflowCount = 0;
const uint32_t frtTimerDiv = CPU_FRT_NTSC_320_128_COUNT_1MS;
const uint32_t frtTimerMul = (1 << 16) / frtTimerDiv; // scale factor

inline static void frtTimerStart(uint16_t count) {
  cpu_frt_count_set(count);
  frtOverflowCount = 0;
}

void frtOviHandler() { frtOverflowCount++; }

inline static uint32_t frtTimerEllapsed() {
  uint32_t ticks = (0xFFFF * frtOverflowCount) + cpu_frt_count_get();
  return (ticks * frtTimerMul) >> 16;
}

inline void codebook_new(codebook_t *cb, binary_stream_t *stream) {
  cb = stream->sampleCache.readPos;
  stream->sampleCache.readPos += 6;
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
  work->copyingVideoFrame = 0;
  work->audioWaitingToStart = false;
  work->isDisplayReady = false;
  work->displayWaiting = false;
  work->lastFrameTime = frtTimerEllapsed();
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
  if (!work->displayWaiting) {
    if (work->stream.sampleCache.currentSample <
      work->stream.sampleCache.numSamples) {
      readBytesIntoRingBuff(&work->stream);
      work->nextSample = work->stream.sampleCache.samples[work->stream.sampleCache.currentSample++];
      work->stream.sampleCache.readPos = work->nextSample.offset;
      bool isVideo = film_sample_is_video(&work->nextSample);
      uint32_t ellapsed = frtTimerEllapsed();
      work->timeEllapsed = ellapsed;
      uint32_t deltaTime = ellapsed - work->lastFrameTime;
      if (deltaTime > 0) {
        work->tickCount += deltaTime;
        work->lastFrameTime = ellapsed;
      }

      if (work->timeEllapsed >= 1000) {
        frtTimerStart(ellapsed - 1000);
        work->lastFrameTime = frtTimerEllapsed();
      }

      if (isVideo) {
        parseVideo(work);
        cpu_divu_32_32_set((work->nextSample.interval * 1000), work->filmHeader.stab.ticks_per_second);
        work->decodeParams->vramWritePos = work->decodeParams->vramBuffAddr;
        work->displayWaiting = true;
      } else {
        parseAudio(work, work->nextSample.length);
      }

    }
  } 
  

  if(work->displayWaiting) {
    if (work->tickCount < work->ticksUntilNextFrame) {
      uint32_t ellapsed = frtTimerEllapsed();
      work->timeEllapsed = ellapsed;
      int32_t deltaTime = ellapsed - work->lastFrameTime;
      if (deltaTime > 0) {
        work->lastFrameTime = ellapsed;
        work->tickCount += deltaTime;
      }

      if (work->timeEllapsed >= 1000) {
        frtTimerStart(ellapsed - 1000);
        work->lastFrameTime = frtTimerEllapsed();
      }
    } else {
        work->isDisplayReady = true;
          if (work->audioWaitingToStart) {
              work->audioWaitingToStart = false;
              film_audio_play(0);
          }
      }
    }
  

  if (work->stream.sampleCache.currentSample >=
    work->stream.sampleCache.numSamples) {
    work->play_status = END;
  }

}

inline bool cpk_display_ready(decode_work_t *work) { return work->isDisplayReady; }

inline void cpk_display_finished(decode_work_t *work) {
  work->ticksUntilNextFrame = cpu_divu_quotient_get(); 
  work->tickCount = 0;
  work->isDisplayReady = false;
  work->displayWaiting = false;
}
