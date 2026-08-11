#ifndef FILM_BUFF_OLD_H
#define FILM_BUFF_OLD_H

#include "film_lib.h"
extern void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples);

extern void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  film_sample_t *sampleCache, uint32_t sampleCacheSize);

extern void triggerDataRequest(binary_stream_t *stream, uint32_t sectors);

extern void stream_readbytes(binary_stream_t *stream, uint16_t *destPtr, uint32_t len);

extern void initRingBuffer(binary_stream_t *stream);

extern void readBytesIntoRingBuff(binary_stream_t *stream);

extern void asyncReadBytesIntoRingBuff(binary_stream_t *stream);
extern void film_buff_reset_async_state(void);

extern uint32_t film_buff_async_bytes_delivered(void);
extern void film_buff_credit_async_bytes_delivered(uint32_t bytes);

#endif // FILM_BUFF_OLD_H
