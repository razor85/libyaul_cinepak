#pragma once

#include "base.h"
#include "cd.h"

#define FILM_SAMPLE_START_OFFSET 64
#define SECTORS_PREFETCH         8

#define ASCII_FILM 1179208781 // 'FILM'
#define ASCII_1d09 825110585  // '1.09'
#define ASCII_FDSC 1178882883 // 'FDSC'
#define ASCII_CVID 1668704612 // 'cvid'
#define ASCII_STAB 1398030658 // 'STAB'

class GenericBuffer {
public:
  GenericBuffer(void *buffer, uint32_t size)
      : m_buffer(static_cast<uint8_t *>(buffer))
      , m_end(m_buffer + size) {}

  uint8_t read8() {
    DEBUG_REQUIRE_LE(m_buffer + 1, m_end);
    uint8_t value = *m_buffer;
    ++m_buffer;
    return value;
  }

  uint16_t read16() {
    DEBUG_REQUIRE_LE(m_buffer + 2, m_end);
    uint16_t value = (m_buffer[1] | (m_buffer[0] << 8));
    m_buffer += 2;
    return value;
  }

  uint32_t read24() {
    DEBUG_REQUIRE_LE(m_buffer + 3, m_end);
    uint32_t value = (m_buffer[2] | (m_buffer[1] << 8) | (m_buffer[0] << 16));
    m_buffer += 3;
    return value;
  }

  uint32_t read32() {
    DEBUG_REQUIRE_LE(m_buffer + 4, m_end);
    uint32_t value = (m_buffer[3] | (m_buffer[2] << 8) | (m_buffer[1] << 16) | (m_buffer[0] << 24));
    m_buffer += 4;
    return value;
  }

  void read(uint8_t *target, uint32_t size) {
    DEBUG_REQUIRE_LE(m_buffer + size, m_end);
    memcpy(target, m_buffer, size);
    m_buffer += size;
  }

  uint8_t *m_buffer;
  const uint8_t *m_end;
};

class FilmStream : public StreamFile {
public:
  // Will be called before stream begins
  typedef void (*InitializeCallback)();
  static void EmptyInitializeCallback() {}

  // Return false to stop film playback
  typedef bool (*LoopCallback)();
  static bool EmptyLoopCallback() { return true; }

  struct Header {
    uint32_t signature;
    uint32_t length;
    uint32_t version;
    uint32_t unknown;
  } __packed;

  static_assert(sizeof(Header) == 16);

  struct FdscChunk {
    uint32_t fdsc;
    uint32_t length;
    uint32_t fourcc;
    uint32_t height;
    uint32_t width;
    uint8_t bpp;
    uint8_t audioChannels;
    uint8_t audioResolution; // 8 or 16
    uint8_t audioCompression;
    uint16_t frequencyHz;
    uint8_t padding1[6]; // Guess
  } __packed;

  static_assert(sizeof(FdscChunk) == 32);

  struct StabChunk {
    uint32_t stab;
    uint32_t length;
    uint32_t frameRateBaseFrequencyHz;
    uint32_t numEntriesSampleTable;
  } __packed;

  static_assert(sizeof(StabChunk) == 16);

  // A sample stored in the STAB table is:
  struct Sample {
    uint32_t offset;
    uint32_t length;
    uint32_t info1;
    uint32_t info2;
    [[nodiscard]] bool isAudio() const { return info1 == 0xFFFFFFFF; }
    [[nodiscard]] bool isVideo() const { return !isAudio(); }
  } __packed;

  static_assert(sizeof(Sample) == 16);

  struct CachedSample {
    uint32_t length;
    uint32_t info;

    CachedSample() = default;

    CachedSample(const Sample &sample) {
      length = sample.length;
      if (sample.isAudio()) {
        info = 0xFFFFFFFF;
      } else {
        DEBUG_REQUIRE_NE(sample.info2, 0xFFFFFFFF);
        info = sample.info2;
      }
    }

    [[nodiscard]] bool isAudio() const { return info == 0xFFFFFFFF; }

    [[nodiscard]] bool isVideo() const { return !isAudio(); }

  } __packed;

  static_assert(sizeof(CachedSample) == 8);

  struct StripHeader {
    uint16_t cvidId;
    uint16_t dataSize;
    uint16_t topY;
    uint16_t topX;
    uint16_t bottomY;
    uint16_t bottomX;
  } __packed;

  static_assert(sizeof(StripHeader) == 12);

  struct Codebook {
    uint8_t y[4];
    int8_t u;
    int8_t v;
  } __packed;

  static_assert(sizeof(Codebook) == 6);

  struct CodebookRGB {
    uint16_t color[4];

    void create(Codebook &book);
  } __packed;

  static_assert(sizeof(CodebookRGB) == 8);

  struct StripCodebook {
    CodebookRGB v1[256];
    CodebookRGB v4[256];
  };

  static_assert(sizeof(StripCodebook) == (256 * 2 * sizeof(CodebookRGB)));

  struct StripData {
    StripCodebook codebooks[2];
    StripCodebook *lastCodebook{nullptr};
    StripCodebook *activeCodebook{nullptr};

    uint32_t topX;
    uint32_t writeX;
    uint32_t bottomX;

    uint32_t topY;
    uint32_t writeY;
    uint32_t bottomY;

    void create() {
      memset(codebooks, 0, 2 * sizeof(StripCodebook));
      lastCodebook = &codebooks[0];
      activeCodebook = &codebooks[1];

      writeX = topX = 0;
      bottomX = 320;
      writeY = topY = 0;
      bottomY = 240;
    }

    void copyLastCodebooks();

    void copyLastCodebooksNoDMA();

    void skipBlock() {
      writeX += 4;
      if (writeX >= bottomX) {
        writeX = topX;
        writeY += 4;
        DEBUG_REQUIRE_LE(writeY, bottomY);
      }
    }

    void swapCodebook() {
      StripCodebook *tmp = activeCodebook;
      activeCodebook = lastCodebook;
      lastCodebook = tmp;
    }

    [[nodiscard]] CodebookRGB *getV1Codebook() { return activeCodebook->v1; }

    [[nodiscard]] CodebookRGB *getV4Codebook() { return activeCodebook->v4; }

  } __packed;

  struct VideoHeader {
    uint8_t flags;
    uint8_t length[3];
    uint16_t width;
    uint16_t height;
    uint16_t numCodedStrips;
  } __packed;

  static_assert(sizeof(VideoHeader) == 10);

private:
  void createCache();

  // Return the current sample, don't do anything else.
  // TODO: Check if returning a reference is actually faster.
  [[nodiscard]] CachedSample getNextSample() {
    DEBUG_REQUIRE_LT(m_sampleCacheIndex, m_sampleCacheCount);
    return m_sampleCache[m_sampleCacheIndex++];
  }

  [[nodiscard]] Codebook readCodebook();

  void renderPixel1(uint8_t c0);
  void renderPixel4(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3);
  void readVectors(uint16_t chunkDataLength);
  void readVectorsInter(uint16_t chunkDataLength);
  void readV1VectorsInChunk(uint16_t chunkDataLength);
  void readChunk(uint16_t chunkID, uint16_t chunkDataLength);
  void parseVideo(const CachedSample &sample);
  void parseAudio(const CachedSample &sample);
  void parseSample(const CachedSample &sample);

  // Cutting dbgio save us 300ms
  bool IsLogEnabled = true;

  void clearLog() {
    if (IsLogEnabled) {
      Console::clear();
    }
  }

  template <typename... Args>
  void printf(const char *msg, const Args &...args) {
    if (IsLogEnabled) {
      Console::printf(msg, args...);
    }
  }

  template <typename... Args>
  void printf_flush(const char *msg, const Args &...args) {
    if (IsLogEnabled) {
      Console::printf_flush(msg, args...);
    }
  }

public:
  FilmStream(uint8_t *tmpBuffer, uint32_t tmpBufferSize, CachedSample *sampleCache, uint32_t sampleCacheCapacity,
    InitializeCallback initializeCallback = &FilmStream::EmptyInitializeCallback,
    LoopCallback loopCallback = &FilmStream::EmptyLoopCallback)
      : m_initializeCallback(initializeCallback)
      , m_loopCallback(loopCallback)
      , m_tmpBuffer(tmpBuffer)
      , m_tmpBufferSize(tmpBufferSize)
      , m_sampleCache(sampleCache)
      , m_sampleCacheCapacity(sampleCacheCapacity) {}

  void play(cdfs_filelist_entry_t *fileListEntry);

private:
  InitializeCallback m_initializeCallback{nullptr};
  LoopCallback m_loopCallback{nullptr};

  uint8_t *m_tmpBuffer{nullptr};
  const uint32_t m_tmpBufferSize{0};

  Optional<uint32_t> m_cvidHeaderPadding;
  StripData m_stripData;

  Header m_header{};
  FdscChunk m_fdscChunk{};
  StabChunk m_stabChunk{};

  // TODO: FIX THIS
  const CachedSample *m_sampleCache{nullptr};
  const uint32_t m_sampleCacheCapacity{0};

  uint32_t m_sampleCacheCount{0};
  uint32_t m_sampleCacheIndex{0};
};
