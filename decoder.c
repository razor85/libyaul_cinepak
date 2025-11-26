#include "cd.h"
#include "decoder.h"
#include "pcmsys.h"

#ifndef MIN
#  define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#define CAST_DATA16(X) (volatile uint16_t *) (X)
#define CAST_DATA32(X) (volatile uint32_t *) (X)
#define VIDEO_HEIGHT   240

int32_t lwrambuffer = 80;

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


void pcm_MemcpyDword(int32_t *dst, int32_t *src, int32_t dwsize) {
  dst += dwsize;
  src += dwsize;
  while (--dwsize >= 0) {
    *--dst = *--src;
  }
}

#ifndef CLAMP
#  define CLAMP(V) ((V) > (255) ? (255) : (V) < (0) ? (0) : (V))
#endif

static uint8_t clampLUT24[512];

void initClampLUT24(void) {
  for (int i = -128; i < 384; i++) {
    if (i < 0) {
      clampLUT24[i + 128] = 0;
    } else if (i > 255) {
      clampLUT24[i + 128] = 255;
    } else {
      clampLUT24[i + 128] = (uint8_t) i;
    }
  }
}


void codebookRGB_24(decode_work_t *work, bool isV4, uint16_t chunkID, int32_t remaining) {
  uint32_t *codebookRGBPtr = isV4 ?
    work->stripData.codebooks[work->stripData.strip].v4RGB->color :
    work->stripData.codebooks[work->stripData.strip].v1RGB->color;

  int32_t iter = 0;
  if (chunkID != 0x2000 && chunkID != 0x2200) {
    uint8_t *readPtr = work->stream.sampleCache.readPos;
    do {
      int32_t flags = (readPtr[0] << 24) | (readPtr[1] << 16) |
        (readPtr[2] << 8) | (readPtr[3]);
      readPtr += 4;
      iter += 4;
      int8_t shifts = 32;
      do {
        if (flags & 0x80000000) {
          codebook_t *book = (codebook_t *) readPtr;
          // | r |   | 1.0  0.0  2.0 | | y |
          // | g | = | 1.0 -0.5 -1.0 | | u |
          // | b |   | 1.0  2.0  0.0 | | v |  

          int cr = (book->v << 1);
          int cg = -(book->u >> 1) - book->v;
          int cb = (book->u << 1);
          int y = book->y[0] + 128;

          int r = y + cr;
          int g = y + cg;
          int b = y + cb;

          uint8_t nr = clampLUT24[r];
          uint8_t ng = clampLUT24[g];
          uint8_t nb = clampLUT24[b];

          codebookRGBPtr[0] =nr | nb << 0x10 | ng << 8 | 0x80000000;

          y = book->y[1] + 128;
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampLUT24[r];
          ng = clampLUT24[g];
          nb = clampLUT24[b];
          codebookRGBPtr[1] = nr | nb << 0x10 | ng << 8 | 0x80000000;

          y = book->y[2] + 128;
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampLUT24[r];
          ng = clampLUT24[g];
          nb = clampLUT24[b];
          codebookRGBPtr[2] = nr | nb << 0x10 | ng << 8 | 0x80000000;

          y = book->y[3] + 128;
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = clampLUT24[r];
          ng = clampLUT24[g];
          nb = clampLUT24[b];
          codebookRGBPtr[3] = nr | nb << 0x10 | ng << 8 | 0x80000000;
                    
          readPtr += 6;
          iter += 6;
        }
        codebookRGBPtr+=4;
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

      int cr = (book->v << 1);
      int cg = -(book->u >> 1) - book->v;
      int cb = (book->u << 1);
      int y = book->y[0] + 128;

      int r = y + cr;
      int g = y + cg;
      int b = y + cb;

      uint8_t nr = clampLUT24[r];
      uint8_t ng = clampLUT24[g];
      uint8_t nb = clampLUT24[b];
      codebookRGBPtr[0] = nr | nb << 0x10 | ng << 8 | 0x80000000;

      y = book->y[1] + 128;
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampLUT24[r];
      ng = clampLUT24[g];
      nb = clampLUT24[b];
      codebookRGBPtr[1] = nr | nb << 0x10 | ng << 8 | 0x80000000;

      y = book->y[2] + 128;
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampLUT24[r];
      ng = clampLUT24[g];
      nb = clampLUT24[b];
      codebookRGBPtr[2] = nr | nb << 0x10 | ng << 8 | 0x80000000;

      y = book->y[3] + 128;
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = clampLUT24[r];
      ng = clampLUT24[g];
      nb = clampLUT24[b];
      codebookRGBPtr[3] = nr | nb << 0x10 | ng << 8 | 0x80000000;
      
      book++;
      codebookRGBPtr+=4;
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
      int32_t flags = (readPtr[0] << 24) | (readPtr[1] << 16) |
        (readPtr[2] << 8) | (readPtr[3]);
      readPtr += 4;
      iter += 4;
      int8_t shifts = 32;
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
          uint8_t nr = CLAMP(r) >> 3;
          uint8_t ng = CLAMP(g) >> 3;
          uint8_t nb = CLAMP(b) >> 3;
          ((uint16_t *) codebookRGBPtr->color)[0] = nr | nb << 10 | ng << 5 | 0x8000;

          y = book->y[1];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = CLAMP(r) >> 3;
          ng = CLAMP(g) >> 3;
          nb = CLAMP(b) >> 3;
         ((uint16_t *) codebookRGBPtr->color)[1] = nr | nb << 10 | ng << 5 | 0x8000;

          y = book->y[2];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = CLAMP(r) >> 3;
          ng = CLAMP(g) >> 3;
          nb = CLAMP(b) >> 3;
          ((uint16_t *) codebookRGBPtr->color)[2] = nr | nb << 10 | ng << 5 | 0x8000;

          y = book->y[3];
          r = y + cr;
          g = y + cg;
          b = y + cb;
          nr = CLAMP(r) >> 3;
          ng = CLAMP(g) >> 3;
          nb = CLAMP(b) >> 3;
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
      uint8_t nr = CLAMP(r) >> 3;
      uint8_t ng = CLAMP(g) >> 3;
      uint8_t nb = CLAMP(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[0] = nr | nb << 10 | ng << 5 | 0x8000;

      y = book->y[1];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = CLAMP(r) >> 3;
      ng = CLAMP(g) >> 3;
      nb = CLAMP(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[1] = nr | nb << 10 | ng << 5 | 0x8000;

      y = book->y[2];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = CLAMP(r) >> 3;
      ng = CLAMP(g) >> 3;
      nb = CLAMP(b) >> 3;
      ((uint16_t *) codebookRGBPtr->color)[2] = nr | nb << 10 | ng << 5 | 0x8000;

      y = book->y[3];
      r = y + cr;
      g = y + cg;
      b = y + cb;
      nr = CLAMP(r) >> 3;
      ng = CLAMP(g) >> 3;
      nb = CLAMP(b) >> 3;
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
    film_sample_t sample = {0};

    stream_readbytes(stream, CAST_DATA32(&sample), sizeof(film_sample_t));

    film_sample_t *newSample = &outputSamples[i];
    newSample->offset = 0;
    newSample->length = sample.length;
    newSample->time = sample.time;
    newSample->duration = sample.duration;
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
  pcm_cfg.src = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos;
  pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtr;
  pcm_cfg.len = len;

  cpu_dmac_channel_config_set(&pcm_cfg);
  cpu_dmac_channel_start(0);
  cpu_cache_purge();

 // pcm_MemcpyDword(destPtr, stream->sampleCache.readPos, len >> 2);
  destPtr += (len >> 2);
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
  int32_t currIdx = cache->currentSample;
  int32_t buffIdx = cache->currentBuffSample;
  film_sample_t nextSample = cache->samples[buffIdx];
  film_sample_t currentSample = cache->samples[currIdx];
  int32_t nextLen = nextSample.length;
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

static void decodeIntra15(decode_work_t *work, uint16_t chunkDataLength,
  bool skipStripData, uint32_t quarterY, uint32_t quarterX) {
  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  int32_t vramwidth = work->decodeParams->vramBufferWidth;

  uint16_t rgb1, rgb2, rgb3, rgb4 = 0;

  int32_t flags = 0;
  int32_t shifts = 0;
  int32_t vramDelta = vramwidth * 3;
  int32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint16_t *y0 = work->decodeParams->vramWritePos;
  uint16_t *y1 = y0 + vramwidth;
  uint16_t *y2 = y1 + vramwidth;
  uint16_t *y3 = y2 + vramwidth;
  int32_t isV4 = 0;
  
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

static void decodeInter15(decode_work_t *work, uint16_t chunkDataLength,
  int32_t quarterY, int32_t quarterX) {
  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  int32_t vramwidth = work->decodeParams->vramBufferWidth;

  int16_t rgb1, rgb2, rgb3, rgb4 = 0;

  int32_t flags = 0;
  int32_t shifts = 0;
  int32_t vramDelta = vramwidth * 3;

  int32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint16_t *y0 = work->decodeParams->vramWritePos;
  uint16_t *y1 = y0 + vramwidth;
  uint16_t *y2 = y1 + vramwidth;
  uint16_t *y3 = y2 + vramwidth;
  int32_t isV4 = 0;

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

void decodeInter24(decode_work_t *work, uint16_t chunkDataLength,
  uint32_t quarterY, uint32_t quarterX) {
    int32_t remainingSectionBytes = chunkDataLength - 4;
    uint8_t *src = work->stream.sampleCache.readPos;
    int32_t vramwidth = work->decodeParams->vramBufferWidth;
    int32_t rgb1, rgb2, rgb3, rgb4 = 0;
   
    int32_t flags = 0;
    int32_t shifts = 0;
    int32_t vramDelta = work->decodeParams->vramDelta;

    int32_t xPos = quarterX;
    int32_t *codebook_rgbval;
    int32_t *y0 = work->decodeParams->vramWritePos;
    int32_t *y1 = y0 + vramwidth;
    int32_t *y2 = y1 + vramwidth;
    int32_t *y3 = y2 + vramwidth;
    int32_t isV4 = 0;

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
                codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v1RGB[src[0]].color;
                rgb1 = codebook_rgbval[0];
                rgb2 = codebook_rgbval[1];
                y0[0] = rgb1;
                y0[1] = rgb1;
                y0[2] = rgb2;
                y0[3] = rgb2;
                y1[0] = rgb1;
                y1[1] = rgb1;
                y1[2] = rgb2;
                y1[3] = rgb2;
                rgb1 = codebook_rgbval[2];
                rgb2 = codebook_rgbval[3];
                y2[0] = rgb1;
                y2[1] = rgb1;
                y2[2] = rgb2;
                y2[3] = rgb2;
                y3[0] = rgb1;
                y3[1] = rgb1;
                y3[2] = rgb2;
                y3[3] = rgb2;
                src++;
            } else {
              codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[0]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
              y0[0] = rgb1;
              y0[1] = rgb2;
              y1[0] = rgb3;
              y1[1] = rgb4;
              
             codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[1]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
             y0[2] = rgb1;
             y0[3] = rgb2;
             y1[2] = rgb3;
             y1[3] = rgb4;
            
             codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[2]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
             y2[0] = rgb1;
             y2[1] = rgb2;
             y3[0] = rgb3;
             y3[1] = rgb4;

             codebook_rgbval = &work->stripData.codebooks[work->stripData.strip].v4RGB[src[3]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
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
  bool skipStripData, uint32_t quarterY, uint32_t quarterX) {

  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  int32_t vramwidth = work->decodeParams->vramBufferWidth;
  int32_t y = work->stripData.writeY;

  int32_t rgb1, rgb2, rgb3, rgb4 = 0;

  int32_t flags = 0;
  int32_t shifts = 0;
  int32_t vramDelta = work->decodeParams->vramDelta;

  int32_t xPos = quarterX;
  int32_t *codebook_rgbval;
  int32_t *y0 = work->decodeParams->vramWritePos;
  int32_t *y1 = y0 + vramwidth;
  int32_t *y2 = y1 + vramwidth;
  int32_t *y3 = y2 + vramwidth;
  int32_t isV4 = 0;

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
            &work->stripData.codebooks[work->stripData.strip].v1RGB[src[0]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            y0[0] = rgb1;
            y0[1] = rgb1;
            y0[2] = rgb2;
            y0[3] = rgb2;
            y1[0] = rgb1;
            y1[1] = rgb1;
            y1[2] = rgb2;
            y1[3] = rgb2;
            rgb1 = codebook_rgbval[2];
            rgb2 = codebook_rgbval[3];
            y2[0] = rgb1;
            y2[1] = rgb1;
            y2[2] = rgb2;
            y2[3] = rgb2;
            y3[0] = rgb1;
            y3[1] = rgb1;
            y3[2] = rgb2;
            y3[3] = rgb2;
            src++;
        } else {
            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[0]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y0[0] = rgb1;
            y0[1] = rgb2;
            y1[0] = rgb3;
            y1[1] = rgb4;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[1]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y0[2] = rgb1;
            y0[3] = rgb2;
            y1[2] = rgb3;
            y1[3] = rgb4;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[2]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y2[0] = rgb1;
            y2[1] = rgb2;
            y3[0] = rgb3;
            y3[1] = rgb4;

            codebook_rgbval =
              &work->stripData.codebooks[work->stripData.strip].v4RGB[src[3]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
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

  int16_t width;
  int16_t height;
  int16_t numStrips;
  int16_t padding;
} videoHeader;

static void parseVideo(decode_work_t *work) {

  videoHeader *cvidHeader;
  cvidHeader = work->stream.sampleCache.readPos;
  work->stream.sampleCache.readPos += sizeof(videoHeader);
  int16_t numStrips = cvidHeader->numStrips;
  int16_t stripNum = 0;
  const bool copyLastCodeBooks = !(cvidHeader->flagsAndCvidLength.b[0] & 0x1);
  int16_t lastBottomY = 0;

  int8_t colorDepth = work->decodeParams->decodeColorDepth;
  void (*decodeIntra)(decode_work_t *work, uint16_t chunkDataLength, bool skipStripData, uint32_t quarterY, uint32_t quarterX); 
  void (*decodeInter)(decode_work_t * work, uint16_t chunkDataLength, uint32_t quarterY, uint32_t quarterX);
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
            stripdata_copyLastCodebooks(&work->stripData);
          }

          uint16_t *tmpStripBuffer;
          tmpStripBuffer = work->stream.sampleCache.readPos;
          work->stream.sampleCache.readPos += 6 * 2;

          uint32_t stripDataLength = tmpStripBuffer[1];

          work->stripData.topY = work->stripData.writeY = tmpStripBuffer[2];
          work->stripData.topX = work->stripData.writeX = tmpStripBuffer[3];
          work->stripData.bottomY = tmpStripBuffer[4];
          work->stripData.bottomX = tmpStripBuffer[5];

          if (stripNum > 0 && work->stripData.topY == 0) {
            work->stripData.topY = work->stripData.writeY = lastBottomY;
            work->stripData.bottomY += lastBottomY;
          }

          int32_t quarterY = (work->stripData.bottomY - work->stripData.writeY) >> 2;
          int32_t quarterX = work->stripData.bottomX >> 2;

          // Read the strip chunks
          int32_t stripLimit =
            work->stream.sampleCache.readPos + stripDataLength - 12;
          lastBottomY = work->stripData.bottomY;
          if (work->stream.sampleCache.readPos < stripLimit) {
              do {
                  //uint8_t *readPtr = work->stream.sampleCache.readPos;
                int32_t chunkID =
                    (work->stream.sampleCache.readPos[0] << 8) |
                    work->stream.sampleCache.readPos[1];
                  work->stream.sampleCache.readPos += 2;
                  int32_t chunkDataLength =
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
                          decodeIntra(work, chunkDataLength, false, quarterY, quarterX);
                          break;
                        // list of blocks from v1
                        case 0x3100:
                            decodeInter(work, chunkDataLength, quarterY, quarterX);
                          break;
                        case 0x3200:
                            decodeIntra(work, chunkDataLength, true, quarterY, quarterX);
                          break;
                        default: {
                          const int32_t chunkIdPos =
                            work->stream.sampleCache.readPos - 4;

                          int16_t *vdp2Image15Ptr =
                            work->decodeParams->vramBuffAddr;
                          int32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;
                          if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_15) {
                            memset(vdp2Image15Ptr, 0,
                              work->decodeParams->vramBufferWidth * VIDEO_HEIGHT *
                                sizeof(int32_t));
                          } else {
                            memset(vdp2ImagePtr, 0,
                              work->decodeParams->vramBufferWidth * VIDEO_HEIGHT *
                                sizeof(int32_t));
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

uint8_t *baseSoundMemory = NULL;
uint8_t *soundMemory = NULL;
uint8_t *soundMemoryLimit = NULL;
int32_t soundBufferSize = NULL;

inline int32_t film_audio_get_next_buffer_size() {
  int32_t size = (int32_t) (soundMemoryLimit - soundMemory);
  if (size == 0) {
    soundMemory = baseSoundMemory;
    size = (int32_t) (soundMemoryLimit - soundMemory);
  }
  return size;
}

inline uint16_t *film_audio_get_next_buffer_ptr(uint8_t slot) {
  return (uint16_t *) soundMemory + (slot * soundBufferSize);
}

inline void film_audio_notify_read_buffer_bytes(int32_t length) {
  soundMemory += length;

  if (soundMemory >= soundMemoryLimit) {
    soundMemory = baseSoundMemory;
  }
}

inline void film_audio_play(int32_t bufferLength __unused) {
  pcmStreamPlay(7);
  sound_notify_driver();
}

void film_audio_setup(decode_work_t *work, int16_t frequency,
  int32_t channels, int32_t numBits) {
  // TODO: Proper stereo
  pcmStreamConfigure(channels == 2 ? 1 : 1, numBits, frequency);
  soundBufferSize = work->decodeParams->audioBufferSize;
  baseSoundMemory = work->decodeParams->audioBufferAddr;
  soundMemory = baseSoundMemory;
  soundMemoryLimit = baseSoundMemory + soundBufferSize;
}

void film_audio_prepare_to_play() {}

void film_audio_reset() {
  baseSoundMemory = NULL;
  soundMemory = NULL;
  soundMemoryLimit = NULL;
  soundBufferSize = NULL;
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
        film_audio_play(0);
      }
    }
        
  }

void init_film(cdfs_filelist_entry_t *fsEntry, decode_work_t *work,
  int32_t vdp_height, int32_t vdp_width) {
    initClampLUT24();
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

  cpu_divu_32_32_set((work->stream.sampleCache.samples[0].duration * 1000), work->filmHeader.stab.ticks_per_second);
   
  work->isDisplayReady = false;
  work->play_status = INIT;
  work->displayWaiting = false;

  work->videoStartY = 0;
  work->dma_delta = work->videoStartY * vdp_width;
  work->audioPlaying = false;
  readBytesIntoRingBuff(&work->stream);
  
}
// Timer
volatile int32_t frtOverflowCount = 0;
const int32_t frtTimerDiv = CPU_FRT_NTSC_320_128_COUNT_1MS;
const int32_t frtTimerMul = (1 << 16) / frtTimerDiv; // scale factor

inline static void frtTimerStart(uint16_t count) {
  cpu_frt_count_set(count);
  frtOverflowCount = 0;
}

void frtOviHandler() { frtOverflowCount++; }

inline static int32_t frtTimerEllapsed() {
  int32_t ticks = (0xFFFF * frtOverflowCount) + cpu_frt_count_get();
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
  readBytesIntoRingBuff(&work->stream);
  if (!work->displayWaiting) {
    if (work->stream.sampleCache.currentSample <
      work->stream.sampleCache.numSamples) {
      work->nextSample = work->stream.sampleCache.samples[work->stream.sampleCache.currentSample++];
      work->stream.sampleCache.readPos = work->nextSample.offset;
      if (work->nextSample.time == 0xFFFFFFFF) {
        parseAudio(work, work->nextSample.length);
        //readBytesIntoRingBuff(&work->stream);
      } else {
        cpu_divu_32_32_set((work->nextSample.duration * 1000), work->filmHeader.stab.ticks_per_second);
        parseVideo(work);
        work->ticksUntilNextFrame = cpu_divu_quotient_get();
        work->decodeParams->vramWritePos = work->decodeParams->vramBuffAddr;
        work->displayWaiting = true;
      }
    }
  } 

  if (work->displayWaiting) {
    if (work->tickCount < work->ticksUntilNextFrame) {
      int32_t ellapsed = frtTimerEllapsed();
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
      }
    }
  if (work->stream.sampleCache.currentSample >=
    work->stream.sampleCache.numSamples) {
    work->play_status = END;
  }

}

inline bool cpk_display_ready(decode_work_t *work) { return work->isDisplayReady; }

inline void cpk_display_finished(decode_work_t *work) {
  work->tickCount = 0;
  work->isDisplayReady = false;
  work->displayWaiting = false;
}
