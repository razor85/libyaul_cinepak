#include "cd.h"
#include "decoder.h"
#include "timer.h"

namespace {

struct Video {
  static constexpr uint32_t TargetWidth = 320;
  static constexpr uint32_t TargetHeight = 240;
  static constexpr uint32_t TargetSize = TargetWidth * TargetHeight;

  uint32_t height = TargetHeight;
  uint32_t startY = 0;
  uint16_t *vdp2DestinationBuffer = reinterpret_cast<uint16_t *>(VDP2_VRAM_ADDR(0, 0));

  // We write to a big temporary buffer and dma transfer to VRAM because one 320x240x3 won't fit in VRAM and we can't
  // double buffer there.
  uint16_t tmpBuffer[TargetSize];
  uint16_t *vdp2ImagePtr = tmpBuffer;
};

struct Audio {
  uint32_t numPlayedSamples = 0;
};

constexpr bool ShowStatistics = true;
Video video;
Audio audio;

// DMA
volatile bool copyingBlocks = false;
void dmaCopyBlocksDone(void *data __unused) {
  cpu_cache_purge();
  Memory::uncached(copyingBlocks) = false;
}

void waitCopyingBlocks() {
  while (Memory::uncached(copyingBlocks)) {
    cpu_instr_nop();
  }
}

volatile uint32_t copyingVideoFrame = 0;
static void waitCopyingVideoFrame() {
  if (copyingVideoFrame != 0) {
    while (vdp_dma_count_get() > 0) {
      cpu_instr_nop();
    }

    copyingVideoFrame = 0;
  }
}

void copyVideoFrame(uint32_t delta) {
  waitCopyingVideoFrame();

  constexpr uint32_t FixedFactor = Video::TargetWidth * sizeof(uint16_t);
  vdp_dma_enqueue(video.vdp2DestinationBuffer + delta, video.vdp2ImagePtr + delta, FixedFactor * video.height);

  copyingVideoFrame = vdp_dma_count_get();
}

void initializeFilm() {
  memset(video.vdp2ImagePtr, 0, Video::TargetSize * sizeof(uint16_t));
  vdp_dma_enqueue(video.vdp2DestinationBuffer, video.vdp2ImagePtr, Video::TargetSize * sizeof(uint16_t));

  vdp2_sync();
  vdp2_sync_wait();
}

uint32_t readU24(volatile uint8_t *bytes) { return bytes[2] | (bytes[1] << 8) | (bytes[0] << 16); }

} // namespace

void FilmStream::CodebookRGB::create(Codebook &book) {
  // | r |   | 1.0  0.0  2.0 | | y |
  // | g | = | 1.0 -0.5 -1.0 | | u |
  // | b |   | 1.0  2.0  0.0 | | v |
  const int16_t cr = (book.v << 1);
  const int16_t cg = -(book.u >> 1) - book.v;
  const int16_t cb = +(book.u << 1);

  for (uint32_t i = 0; i < 4; ++i) {
    const int16_t y = book.y[i];
    const int16_t r = y + cr;
    const int16_t g = y + cg;
    const int16_t b = y + cb;

    const auto nr = clamp<uint8_t>(r, 0, 255);
    const auto ng = clamp<uint8_t>(g, 0, 255);
    const auto nb = clamp<uint8_t>(b, 0, 255);

    const rgb1555_t tmpColor{
      1u,
      static_cast<unsigned int>(nb >> 3),
      static_cast<unsigned int>(ng >> 3),
      static_cast<unsigned int>(nr >> 3),
    };

    color[i] = tmpColor.raw;
  }
}

void FilmStream::StripData::copyLastCodebooks() {
  waitCopyingBlocks();

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src_mode = CPU_DMAC_SOURCE_INCREMENT,
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .stride = CPU_DMAC_STRIDE_16_BYTES,
    .request_mode = CPU_DMAC_REQUEST_MODE_AUTO,
    .dack_mode = CPU_DMAC_DACK_MODE_READ,
    .dack_level = CPU_DMAC_DACK_LEVEL_LOW,
    .detect_mode = CPU_DMAC_DETECT_MODE_LEVEL,
    .dreq_level = CPU_DMAC_DREQ_LEVEL_LOW,
    .bus_mode = CPU_DMAC_BUS_MODE_BURST,
    .resource_select = CPU_DMAC_RESOURCE_SELECT_DREQ,
    .non_default = 0,
    .src = CPU_CACHE_THROUGH | (uint32_t) lastCodebook,
    .dst = CPU_CACHE_THROUGH | (uint32_t) activeCodebook,
    .len = sizeof(StripCodebook),
    .ihr = dmaCopyBlocksDone,
    .ihr_work = NULL,
  };

  cpu_dmac_channel_config_set(&cfg);
  cpu_dmac_channel_start(0);

  copyingBlocks = true;
}

void FilmStream::StripData::copyLastCodebooksNoDMA() { memcpy(activeCodebook, lastCodebook, sizeof(StripCodebook)); }

void FilmStream::createCache() {
  // We only read a single sector, so check how many samples we have there.
  [[maybe_unused]] const uint32_t samplesInitialPos = getOffset();
  DEBUG_REQUIRE_EQ(FILM_SAMPLE_START_OFFSET, samplesInitialPos);

  m_sampleCacheIndex = 0;
  m_sampleCacheCount = min(m_stabChunk.numEntriesSampleTable, m_sampleCacheCapacity);

  for (uint32_t i = 0; i < m_sampleCacheCount; ++i) {
    Sample tmpSample;
    read(&tmpSample, sizeof(Sample));

    const_cast<CachedSample &>(m_sampleCache[i]) = CachedSample(tmpSample);
  }
}

FilmStream::Codebook FilmStream::readCodebook() {
  Codebook result;
  read(&result, sizeof(Codebook));

  return result;
}

void FilmStream::renderPixel1(uint8_t c0) {
  const CodebookRGB e0 = m_stripData.getV1Codebook()[c0];
  const uint32_t x = m_stripData.writeX;
  const uint32_t y = m_stripData.writeY;

  // VDP2 image has 512x256
  uint32_t imageIndex = ((video.startY + y) * Video::TargetWidth) + x;
  DEBUG_REQUIRE_LT(imageIndex, Video::TargetWidth * Video::TargetHeight);

  // +----+----+  +---+  +---+
  // | y0 | y1 |  | u |  | v |
  // +----+----+  +---+  +---+
  // | y2 | y3 |
  // +----+----+
  if (y + 0 >= m_stripData.bottomY)
    return;

  video.vdp2ImagePtr[imageIndex] = video.vdp2ImagePtr[imageIndex + 1] = e0.color[0];
  video.vdp2ImagePtr[imageIndex + 2] = video.vdp2ImagePtr[imageIndex + 3] = e0.color[1];

  if (y + 1 >= m_stripData.bottomY)
    return;

  imageIndex += Video::TargetWidth;
  video.vdp2ImagePtr[imageIndex] = video.vdp2ImagePtr[imageIndex + 1] = e0.color[0];
  video.vdp2ImagePtr[imageIndex + 2] = video.vdp2ImagePtr[imageIndex + 3] = e0.color[1];

  if (y + 2 >= m_stripData.bottomY)
    return;

  imageIndex += Video::TargetWidth;
  video.vdp2ImagePtr[imageIndex] = video.vdp2ImagePtr[imageIndex + 1] = e0.color[2];
  video.vdp2ImagePtr[imageIndex + 2] = video.vdp2ImagePtr[imageIndex + 3] = e0.color[3];

  if (y + 3 >= m_stripData.bottomY)
    return;

  imageIndex += Video::TargetWidth;
  video.vdp2ImagePtr[imageIndex] = video.vdp2ImagePtr[imageIndex + 1] = e0.color[2];
  video.vdp2ImagePtr[imageIndex + 2] = video.vdp2ImagePtr[imageIndex + 3] = e0.color[3];
}

void FilmStream::renderPixel4(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3) {
  const CodebookRGB *codebook = m_stripData.getV4Codebook();
  const CodebookRGB &e0 = codebook[c0];
  const CodebookRGB &e1 = codebook[c1];
  const CodebookRGB &e2 = codebook[c2];
  const CodebookRGB &e3 = codebook[c3];

  const uint32_t x = m_stripData.writeX;
  const uint32_t y = m_stripData.writeY;

  uint32_t imageIndex = ((video.startY + y) * Video::TargetWidth) + x;
  DEBUG_REQUIRE_LT(imageIndex, Video::TargetWidth * Video::TargetHeight);

  // +------+------+------+------+
  // | e0y0 | e0y1 | e1y0 | e1y1 |
  // +------+------+------+------+
  // | e0y2 | e0y3 | e1y2 | e1y3 |
  // +------+------+------+------+
  // | e2y0 | e2y1 | e3y0 | e3y1 |
  // +------+------+------+------+
  // | e2y2 | e2y3 | e3y2 | e3y3 |
  // +------+------+------+------+
  if (y + 0 >= m_stripData.bottomY)
    return;

  video.vdp2ImagePtr[imageIndex++] = e0.color[0];
  video.vdp2ImagePtr[imageIndex++] = e0.color[1];
  video.vdp2ImagePtr[imageIndex++] = e1.color[0];
  video.vdp2ImagePtr[imageIndex] = e1.color[1];

  if (y + 1 >= m_stripData.bottomY)
    return;

  imageIndex += Video::TargetWidth - 3;
  video.vdp2ImagePtr[imageIndex++] = e0.color[2];
  video.vdp2ImagePtr[imageIndex++] = e0.color[3];
  video.vdp2ImagePtr[imageIndex++] = e1.color[2];
  video.vdp2ImagePtr[imageIndex] = e1.color[3];

  if (y + 2 >= m_stripData.bottomY)
    return;

  imageIndex += Video::TargetWidth - 3;
  video.vdp2ImagePtr[imageIndex++] = e2.color[0];
  video.vdp2ImagePtr[imageIndex++] = e2.color[1];
  video.vdp2ImagePtr[imageIndex++] = e3.color[0];
  video.vdp2ImagePtr[imageIndex] = e3.color[1];

  if (y + 3 >= m_stripData.bottomY)
    return;

  imageIndex += Video::TargetWidth - 3;
  video.vdp2ImagePtr[imageIndex++] = e2.color[2];
  video.vdp2ImagePtr[imageIndex++] = e2.color[3];
  video.vdp2ImagePtr[imageIndex++] = e3.color[2];
  video.vdp2ImagePtr[imageIndex] = e3.color[3];
}

void FilmStream::readVectors(uint16_t chunkDataLength) {
  uint32_t flags = read32();
  uint32_t remainingSectionBytes = chunkDataLength - 4;

  DEBUG_REQUIRE_LT(remainingSectionBytes, m_tmpBufferSize);
  read(m_tmpBuffer, remainingSectionBytes);

  waitCopyingVideoFrame();

  uint8_t *tmpData = m_tmpBuffer;
  while (remainingSectionBytes) {
    if (m_stripData.writeY >= m_stripData.bottomY) {
      break;
    }

    for (volatile uint32_t i = 0; i < 32; ++i) {
      DEBUG_REQUIRE_LT(tmpData, m_tmpBuffer + m_tmpBufferSize);
      if (flags & 0x80000000) {
        if (remainingSectionBytes < 4) {
          break;
        }

        // V4
        renderPixel4(tmpData[0], tmpData[1], tmpData[2], tmpData[3]);
        remainingSectionBytes -= 4;
        tmpData += 4;

      } else {
        if (remainingSectionBytes < 1) {
          break;
        }

        // V1
        renderPixel1(tmpData[0]);
        remainingSectionBytes -= 1;
        tmpData += 1;
      }

      m_stripData.skipBlock();
      flags <<= 1;
    }

    if (remainingSectionBytes < 4) {
      break;
    }

    memcpy(&flags, tmpData, 4);
    tmpData += 4;
    remainingSectionBytes -= 4;
  }
}

void FilmStream::readVectorsInter(uint16_t chunkDataLength) {
  DEBUG_REQUIRE_LT(chunkDataLength, m_tmpBufferSize);

  read(m_tmpBuffer, chunkDataLength);
  waitCopyingVideoFrame();

  uint32_t remainingSectionBytes = chunkDataLength - 4;

  // We keep reading flags as long as it is possible. We first read 4
  // bytes and then we start shifting them for the VLC. Once we reach
  // the end we try to read a new flag.
  uint32_t flags = *reinterpret_cast<uint32_t *>(m_tmpBuffer);

  uint8_t *tmpData = m_tmpBuffer + 4;
  uint32_t shifts = 0;
  while (remainingSectionBytes) {
    DEBUG_REQUIRE_LE(shifts, 31);
    if (m_stripData.writeY >= m_stripData.bottomY) {
      break;
    }

    // Running on VLC now, so we just keep consuming bytes (4 each time)
    // until there is nothing else. 0 => skip block, 1 => read next bit:
    // - next bit is 1 = V4
    // - next bit is 0 = V1
    DEBUG_REQUIRE_LT(tmpData, m_tmpBuffer + m_tmpBufferSize);
    if (flags & 0x80000000) {
      // We are at the last bit so we need to fetch the next flags and check
      // the first bit as if it was the next on this sequence.
      if (shifts == 31) {
        if (remainingSectionBytes < 4) {
          break;
        }

        memcpy(&flags, tmpData, 4);
        tmpData += 4;
        remainingSectionBytes -= 4;

        shifts = 0;
      } else {
        flags <<= 1;
        shifts++;
      }

      if (flags & 0x80000000) {
        if (remainingSectionBytes < 4) {
          break;
        }

        // V4
        renderPixel4(tmpData[0], tmpData[1], tmpData[2], tmpData[3]);
        tmpData += 4;
        remainingSectionBytes -= 4;
      } else {
        if (remainingSectionBytes < 1) {
          break;
        }

        // V1
        renderPixel1(tmpData[0]);
        tmpData += 1;
        remainingSectionBytes -= 1;
      }
    }

    m_stripData.skipBlock();

    // If we read all the bits, we just fetch the next flags.
    if (shifts == 31) {
      DEBUG_REQUIRE_LT(tmpData, m_tmpBuffer + m_tmpBufferSize);
      if (remainingSectionBytes < 4) {
        break;
      }

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

void FilmStream::readV1VectorsInChunk(uint16_t chunkDataLength) {
  uint32_t readBytes = 0;
  uint16_t originalWriteX = m_stripData.writeX;
  uint16_t originalWriteY = m_stripData.writeY;

  uint32_t remainingSectionBytes = chunkDataLength;
  while (remainingSectionBytes) {
    if (m_stripData.writeY >= m_stripData.bottomY) {
      break;
    }

    ++readBytes;
    --remainingSectionBytes;
    m_stripData.skipBlock();
  }

  m_stripData.writeX = originalWriteX;
  m_stripData.writeY = originalWriteY;

  waitCopyingVideoFrame();

  while (readBytes > 0) {
    const uint32_t readNow = min(readBytes, m_tmpBufferSize);

    read(m_tmpBuffer, readNow);
    for (volatile uint32_t i = 0; i < readNow; ++i) {
      const uint8_t c0 = m_tmpBuffer[i];
      renderPixel1(c0);
      m_stripData.skipBlock();
    }

    readBytes -= readNow;
  }

  if (remainingSectionBytes) {
    skip(remainingSectionBytes);
  }
}

void FilmStream::readChunk(uint16_t chunkID, uint16_t chunkDataLength) {
  DEBUG_REQUIRE_EQ(chunkDataLength % 2, 0);

  switch (chunkID) {
  // 12 bit V4 (0x2000) or V1(0x2200)
  case 0x2000:
  case 0x2200: {
    // Start DIVU
    uint32_t remainingSectionBytes = chunkDataLength;
    cpu_divu_32_32_set(remainingSectionBytes, sizeof(Codebook));

    CodebookRGB *codebookPtr;
    if (chunkID == 0x2000) {
      codebookPtr = m_stripData.getV4Codebook();
    } else {
      codebookPtr = m_stripData.getV1Codebook();
    }

    DEBUG_REQUIRE_NE(codebookPtr, nullptr);

    const uint32_t numReads = cpu_divu_quotient_get();
    const uint32_t numReadBytes = numReads * sizeof(Codebook);
    DEBUG_REQUIRE_LE(numReadBytes, sizeof(Codebook) * 256);

    for (uint32_t i = 0; i < numReads; ++i) {
      FilmStream::Codebook tmpCodebook = readCodebook();
      codebookPtr[i].create(tmpCodebook);
    }

    remainingSectionBytes -= numReadBytes;

    // Deviant format shenanigans
    if (remainingSectionBytes) {
      skip(remainingSectionBytes);
    }
  } break;

  // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
  case 0x2100:
  case 0x2300: {
    CodebookRGB *codebookPtr;
    if (chunkID == 0x2100) {
      codebookPtr = m_stripData.getV4Codebook();
    } else {
      codebookPtr = m_stripData.getV1Codebook();
    }

    DEBUG_REQUIRE_NE(codebookPtr, nullptr);

    uint32_t remainingSectionBytes = chunkDataLength;
    while (remainingSectionBytes >= 4) {
      uint32_t flags = read32();
      remainingSectionBytes -= 4;

      for (uint32_t i = 0; i < 32; ++i) {
        if (flags & 0x80000000) {
          FilmStream::Codebook tmpCodebook = readCodebook();
          codebookPtr->create(tmpCodebook);

          remainingSectionBytes -= 6;
        }

        ++codebookPtr;
        flags <<= 1;
      }
    }

    if (remainingSectionBytes) {
      skip(remainingSectionBytes);
    }
  } break;

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
    readVectors(chunkDataLength);
    break;

  // list of blocks from v1
  case 0x3100:
    readVectorsInter(chunkDataLength);
    break;

  case 0x3200:
    readV1VectorsInChunk(chunkDataLength);
    break;

  default: {
    const uint32_t chunkIdPos = getOffset() - 4;

    memset(video.vdp2ImagePtr, 0, Video::TargetWidth * Video::TargetHeight * sizeof(uint16_t));
    Console::printf_flush("Unknown chunk id 0x%X at offset %d\n", chunkID, chunkIdPos);

    // DEBUG TO LWRAM
    sprintf((char *) LWRAM(80), "Unknown chunk id 0x%X at offset %d\n", chunkID, chunkIdPos);

    DEBUG_REQUIRE(false);
  } break;
  }
}

void FilmStream::parseVideo(const CachedSample &sample) {
  const uint32_t sampleLimit = getOffset() + sample.length;

  VideoHeader cvidHeader;
  read(&cvidHeader, sizeof(VideoHeader));

  const bool shouldCopyLastCodeBooks = !(cvidHeader.flags & 0x1);
  const uint32_t length = readU24(cvidHeader.length);

  // Used when stripId == 0
  union {
    struct {
      uint32_t u32;
      uint16_t u16;
    };
    uint8_t bytes[6];
  } auxData;

  bool auxDataUsed = false;

  if (!m_cvidHeaderPadding.hasValue()) {
    m_cvidHeaderPadding = uint32_t(0);

    // Execute division on DIV-U and then check for the mod operation if needed.
    uint32_t remainder = 0;
    if (length != m_stabChunk.length) {
      remainder = cpu_divu_remainder_get();
    }

    if (remainder != 0) {
      // If the encoded frame size differs from the frame size as indicated by the container file, this data likely
      // comes from a Sega FILM/CPK file. If the frame header is followed by the bytes FE 00 00 06 00 00 then this is
      // probably one of the two known files that have 6 extra bytes after the frame header. Else, assume 2 extra
      // bytes. The container size also cannot be a multiple of the encoded size. (FFMPEG DOCS)
      m_cvidHeaderPadding = 2;
      if (m_stabChunk.length >= 16) {
        auxDataUsed = true;
        read(auxData.bytes, 6);

        // Original:
        // if ((auxData.bytes[0] == 0xFE) && (auxData.bytes[1] == 0x00) && (auxData.bytes[2] == 0x00) &&
        // (auxData.bytes[3] == 0x06) && (auxData.bytes[4] == 0x00) && (auxData.bytes[5] == 0x00)) {
        // m_cvidHeaderPadding = 6;
        // }

        // Yep, this is UB on C++ but GCC should handle it just fine.
        if (auxData.u32 == 0xFE000006 && auxData.u16 == 0x0000) {
          m_cvidHeaderPadding = 6;
        }
      }
    }
  }

  DEBUG_REQUIRE(m_cvidHeaderPadding.hasValue());
  const uint32_t skipBytes = *m_cvidHeaderPadding;

  if (!auxDataUsed) {
    skip(skipBytes);
  }

  uint16_t lastBottomY = 0;
  for (uint16_t stripId = 0; stripId < cvidHeader.numCodedStrips; ++stripId) {
    // flag bit 0 will tell if we need the contents of the previous strip
    if (stripId > 0 && shouldCopyLastCodeBooks) {
      // For debugging use the No DMA version:
      // m_stripData.copyLastCodebooksNoDMA();
      m_stripData.copyLastCodebooks();
    }

    StripHeader stripHeader;
    if (stripId == 0 && auxDataUsed) {
      // We already had to read 6 extra bytes to judge if we are reading a deviant file so we must take
      // that into account when reading the next 12 bytes needed for the strip. If we read 6 bytes and
      // we must skip 2, we can only 8 now.
      const uint32_t validAuxBytes = 6 - skipBytes;
      if (validAuxBytes) {
        memcpy(&stripHeader, auxData.bytes + skipBytes, validAuxBytes);
      }

      read(reinterpret_cast<uint8_t *>(&stripHeader) + validAuxBytes, sizeof(StripHeader) - validAuxBytes);

    } else {
      // Read normally.
      read(&stripHeader, sizeof(StripHeader));
    }

    m_stripData.topY = m_stripData.writeY = stripHeader.topY;
    m_stripData.topX = m_stripData.writeX = stripHeader.topX;
    m_stripData.bottomY = stripHeader.bottomY;
    m_stripData.bottomX = stripHeader.bottomX;

    if (stripId > 0 && m_stripData.topY == 0) {
      m_stripData.topY = m_stripData.writeY = lastBottomY;
      m_stripData.bottomY += lastBottomY;
    }

    // Read the strip chunks
    const uint32_t stripLimit = getOffset() + stripHeader.dataSize - sizeof(StripHeader);
    lastBottomY = m_stripData.bottomY;

    while (getOffset() < stripLimit) {
      const uint16_t cvidChunkID = read16();
      const uint16_t cvidChunkDataLength = read16() - 4;

      [[maybe_unused]] const size_t expectedEnd = getOffset() + cvidChunkDataLength;
      if (cvidChunkDataLength) {
        readChunk(cvidChunkID, cvidChunkDataLength);
      }

      DEBUG_REQUIRE_EQ(getOffset(), expectedEnd);
    } // Strip data

    DEBUG_REQUIRE_EQ(getOffset(), stripLimit);

    // Keep the last strip without swapping because the next frame might need it.
    if (stripId != cvidHeader.numCodedStrips - 1) {
      m_stripData.swapCodebook();
    }
  }

  DEBUG_REQUIRE_EQ(getOffset(), sampleLimit);
}

void FilmStream::parseAudio(const CachedSample &sample) {
  uint32_t length = sample.length;

  // TODO: Mono at least
  skip(length);
  return;

  /*
  // TODO: Proper stereo
  if (m_fdscChunk.audioChannels == 2) {
    length >>= 1;
    DEBUG_REQUIRE_EQ(length % 2, 0);
    skip(sample.length - length);
  }

  uint32_t missingBytes = length;
  while (missingBytes > 0) {
    uint32_t readSize = min(film_audio_get_next_buffer_size(), missingBytes);
    uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);
    DEBUG_REQUIRE_NE(writeLocation, NULL);

    stream_readbytes(stream, writeLocation, readSize);
    film_audio_notify_read_buffer_bytes(readSize);
    missingBytes -= readSize;
  }

  film_audio_play(length);
  audioNumPlayedSamples++;
  */
}

void FilmStream::parseSample(const CachedSample &sample) {
  if (sample.isAudio()) {
    parseAudio(sample);
  } else {
    parseVideo(sample);
  }
}

void FilmStream::play(cdfs_filelist_entry_t *fileListEntry) {
  Console::clear();

  initialize(fileListEntry);
  initializeFilm();
  m_stripData.create();

  read(&m_header, sizeof(Header));
  DEBUG_REQUIRE_EQ(m_header.signature, ASCII_FILM);
  DEBUG_REQUIRE_EQ(m_header.version, ASCII_1d09);

  read(&m_fdscChunk, sizeof(FdscChunk));
  DEBUG_REQUIRE_EQ(m_fdscChunk.fdsc, ASCII_FDSC);
  DEBUG_REQUIRE_EQ(m_fdscChunk.fourcc, ASCII_CVID);

  video.height = m_fdscChunk.height;
  video.startY = (Video::TargetHeight - m_fdscChunk.height) / 2;
  audio.numPlayedSamples = 0;

  // TODO:
  // film_audio_setup(audioSamplingFrequencyHz, audioChannels, audioSamplingResolution);

  Console::printf("Found %dx%d@%dbpp video\n", m_fdscChunk.width, m_fdscChunk.height, m_fdscChunk.bpp);
  Console::printf("Centering at y = %d\n", video.startY);
  Console::printf_flush("Audio:\n"
                        " %d channels\n"
                        " %d bits (comp = %d)\n"
                        " %d Hz\n",
    m_fdscChunk.audioChannels, m_fdscChunk.audioResolution, m_fdscChunk.audioCompression, m_fdscChunk.frequencyHz);

  read(&m_stabChunk, sizeof(StabChunk));
  DEBUG_REQUIRE_EQ(m_stabChunk.stab, ASCII_STAB);

  const uint32_t framerateBaseFrequencyHz = m_stabChunk.frameRateBaseFrequencyHz;
  const uint32_t numSamples = m_stabChunk.numEntriesSampleTable;
  Console::printf_flush("FrameRateBaseFreq: %d Hz (%d samples)\n", framerateBaseFrequencyHz, numSamples);

  [[maybe_unused]] const uint32_t sampleDescriptionPos = getOffset();
  [[maybe_unused]] const uint32_t sampleDataPos = m_header.length;

  // Must be called just before the sample list
  createCache();
  Console::printf("SamplePos: %d\n"
                  "SampleDataPos: %d\n"
                  "Pos: %d\n",
    sampleDescriptionPos, sampleDataPos, getOffset());

  // Ask the sound driver to stop the warm up sound and get ready to start
  // processing sounds
  // film_audio_prepare_to_play();

  [[maybe_unused]] uint32_t samplesInSec = 0;
  [[maybe_unused]] uint32_t minSamplesInSec = 0xFFFFFFFF;
  [[maybe_unused]] uint32_t samplesInSecCount = 0;
  [[maybe_unused]] uint32_t bytesInSecCount = 0;
  [[maybe_unused]] uint32_t lastBytesInSecCount = 0;

  // FILM timing
  Timer frameTimer, totalTimer;
  for (uint32_t sampleId = 0; sampleId < numSamples; ++sampleId) {
    if (!m_loopCallback()) {
      break;
    }

    CachedSample sample = getNextSample();

    DEBUG_REQUIRE(sample.isAudio() || sample.info < (INT32_MAX / 1000));
    cpu_divu_32_32_set(1000 * sample.info, m_stabChunk.frameRateBaseFrequencyHz);

    const uint32_t timeToNextFrameMs = cpu_divu_quotient_get();
    parseSample(sample);

    if constexpr (ShowStatistics) {
      ++samplesInSecCount;
      bytesInSecCount += sample.length;
    }

    if (sample.isVideo()) {
      // Wait until we can proccess next frame due to pending interval (from previous frame).
      const uint32_t delta = video.startY * Video::TargetWidth;
      while (frameTimer.count() < timeToNextFrameMs) {
        cpu_instr_nop();
      }

      frameTimer.reset();
      copyVideoFrame(delta);
    }

    if constexpr (ShowStatistics) {
      Console::clear();
      Console::printf("Play Time %d ms\n"
                      "Frame %d\n"
                      "SamplesInSec: %d\n"
                      "BytesInSec: %d\n"
                      "LastBytesInSec: %d\n"
                      "Audio: %c / %d bits / %d Hz\n"
                      "NumPlayedSamples: %d\n"
                      "TickRate: %d Hz\n",
        totalTimer.count(), m_sampleCacheIndex, samplesInSec, bytesInSecCount, lastBytesInSecCount,
        m_fdscChunk.audioChannels == 1 ? 'M' : 'S', m_fdscChunk.audioResolution, m_fdscChunk.frequencyHz,
        audio.numPlayedSamples, framerateBaseFrequencyHz);

      Console::flush();
    }
  }

  [[maybe_unused]] const int status = cd_block_cmd_data_transfer_end();
  DEBUG_REQUIRE_EQ(status, 0);
}
