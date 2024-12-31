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

  struct StabChunk {
    uint32_t stab;
    uint32_t length;
    uint32_t frameRateBaseFrequencyHz;
    uint32_t numEntriesSampleTable;
  } __packed;

  // A sample stored in the STAB table is:
  struct Sample {
    uint32_t offset;
    uint32_t length;
    uint32_t info1;
    uint32_t info2;
    [[nodiscard]] bool isAudio() const { return info1 == 0xFFFFFFFF; }
    [[nodiscard]] bool isVideo() const { return !isAudio(); }
  } __packed;

  struct StripHeader {
    uint16_t cvidId;
    uint16_t dataSize;
    uint16_t topY;
    uint16_t topX;
    uint16_t bottomY;
    uint16_t bottomX;
  } __packed;

  static_assert(sizeof(StripHeader) == 12, "sizeof(StripHeader) != 12");

  struct Codebook {
    uint8_t y[4];
    int8_t u;
    int8_t v;

    void create(FilmStream &file);
  } __packed;

  static_assert(sizoef(Codebook) == 6, "sizeof(Codebook) != 6");

  struct Codebook555 {
    uint16_t color[4];

    void create(Codebook &book);
  } __packed;

  struct StripCodebook {
    Codebook v1[256];
    Codebook v4[256];
    Codebook555 v1RGB[256];
    Codebook555 v4RGB[256];
  };

  struct StripData {
    static constexpr uint32_t MaxStrips = 16;
    StripCodebook codebooks[MaxStrips];
    uint32_t strip;

    uint32_t topX;
    uint32_t writeX;
    uint32_t bottomX;

    uint32_t topY;
    uint32_t writeY;
    uint32_t bottomY;

    void create() {
      memset(codebooks, 0, MaxStrips * sizeof(StripCodebook));
      strip = 0;
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
      }
    }

    [[nodiscard]] Codebook *getV1Codebook() { return codebooks[strip].v1; }

    [[nodiscard]] Codebook555 *getV1Codebook555() { return codebooks[strip].v1RGB; }

    [[nodiscard]] Codebook *getV4Codebook() { return codebooks[strip].v4; }

    [[nodiscard]] Codebook555 *getV4Codebook555() { return codebooks[strip].v4RGB; }
  } __packed;

  struct VideoHeader {
    uint8_t flags;
    uint8_t length[3];
    uint16_t width;
    uint16_t height;
    uint16_t numCodedStrips;
  } __packed;

private:
  void createCache();

  // Return the current sample, don't do anything else.
  // TODO: Check if returning a reference is actually faster.
  [[nodiscard]] Sample getNextSample() {
    DEBUG_REQUIRE_LT(m_sampleCacheIndex, m_sampleCacheCount);
    return m_sampleCache[m_sampleCacheIndex++];
  }

  void renderPixel1(StripData &data, uint8_t c0);
  void renderPixel4(StripData &data, uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3);
  void readVectors(StripData &data, uint16_t chunkDataLength);
  void readVectorsInter(StripData &data, uint16_t chunkDataLength);
  void readV1VectorsInChunk(StripData &data, uint16_t chunkDataLength);
  void readChunk(uint16_t chunkID, uint16_t chunkDataLength, StripData &data);
  void parseVideo(const Sample &sample);
  void parseAudio(const Sample &sample);
  void parseSample(const Sample &sample);

public:
  FilmStream(cdfs_filelist_entry_t *fileListEntry, uint8_t *tmpBuffer, uint32_t tmpBufferSize, Sample *sampleCache,
    uint32_t sampleCacheCapacity, InitializeCallback initializeCallback = &FilmStream::EmptyInitializeCallback,
    LoopCallback loopCallback = &FilmStream::EmptyLoopCallback)
      : StreamFile(fileListEntry)
      , m_initializeCallback(initializeCallback)
      , m_loopCallback(loopCallback)
      , m_tmpBuffer(tmpBuffer)
      , m_tmpBufferSize(tmpBufferSize)
      , m_sampleCache(sampleCache)
      , m_sampleCacheCapacity(sampleCacheCapacity) {}

  void play();

private:
  InitializeCallback m_initializeCallback{nullptr};
  LoopCallback m_loopCallback{nullptr};

  uint8_t *m_tmpBuffer{nullptr};
  const uint32_t m_tmpBufferSize{0};

  Optional<uint32_t> m_cvidHeaderPadding;

  Header m_header{};
  FdscChunk m_fdscChunk{};
  StabChunk m_stabChunk{};

  // TODO: FIX THIS
  const Sample *m_sampleCache{nullptr};
  const uint32_t m_sampleCacheCapacity{0};

  uint32_t m_sampleCacheCount{0};
  uint32_t m_sampleCacheIndex{0};
};
