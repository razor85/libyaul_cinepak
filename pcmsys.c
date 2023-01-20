// pcm_sys.c
// this file is compiled separately
// hopefully somewhat portable
//
#include <yaul.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pcmsys.h"

// clang-format off
static const int logtbl[] = {
  /* 0   */ 0,
  /* 1   */ 1,
  /* 2   */ 2, 2,
  /* 4   */ 3, 3, 3, 3,
  /* 8   */ 4, 4, 4, 4, 4, 4, 4, 4,
  /* 16  */ 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
  /* 32  */ 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
            6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  /* 64  */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
  /* 128 */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
            8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
            8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
            8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
            8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
            8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
};
// clang-format on

#define PCM_MSK1(a)  ((a) &0x0001)
#define PCM_MSK3(a)  ((a) &0x0007)
#define PCM_MSK4(a)  ((a) &0x000F)
#define PCM_MSK5(a)  ((a) &0x001F)
#define PCM_MSK10(a) ((a) &0x03FF)

#define PCM_SCSP_FREQUENCY (44100L)

#define PCM_CALC_OCT(smpling_rate) \
  ((int) logtbl[PCM_SCSP_FREQUENCY / ((smpling_rate) + 1)])

#define PCM_CALC_SHIFT_FREQ(oct) (PCM_SCSP_FREQUENCY >> (oct))

#define PCM_CALC_FNS(smpling_rate, shift_freq) \
  ((((smpling_rate) - (shift_freq)) << 10) / (shift_freq))

#define PCM_SET_PITCH_WORD(oct, fns) \
  ((int) ((PCM_MSK4(-(oct)) << 11) | PCM_MSK10(fns)))

sysComPara *m68k_com = (sysComPara *) (SNDPRG + DRV_SYS_END);

// Local loading address for sound data, is DRV_SYS_END ahead of the SNDPRG, and
// ahead of the communication data
unsigned int *scsp_load_start_address =
  (unsigned int *) (0x408 + DRV_SYS_END + 0x20);

unsigned int *scsp_load = (unsigned int *) (0x408 + DRV_SYS_END + 0x20);
unsigned short *master_volume = (unsigned short *) (SNDRAM + 0x100400);
short numberPCMs = 0;

void pcm_play(short pcmNumber, char ctrlType, char volume) {
  m68k_com->pcmCtrl[pcmNumber].sh2_permit = 1;
  m68k_com->pcmCtrl[pcmNumber].volume = volume;
  m68k_com->pcmCtrl[pcmNumber].loopType = ctrlType;
}

void pcm_parameter_change(short pcmNumber, char volume, char pan) {
  m68k_com->pcmCtrl[pcmNumber].volume = volume;
  m68k_com->pcmCtrl[pcmNumber].pan = pan;
}

void pcm_cease(short pcmNumber) {
  // If it is a volatile or protected sound, the expected control method is
  // to mute the sound and let it end itself.
  if (m68k_com->pcmCtrl[pcmNumber].loopType <= 0) {
    // Protected sounds have a permission state of "until they end".
    m68k_com->pcmCtrl[pcmNumber].volume = 0;
  } else {
    // If it is a looping sound, the control method is to
    // command it to stop.
    m68k_com->pcmCtrl[pcmNumber].sh2_permit = 0;
  }
}

void smpc_wait_till_ready(void) {
  // Wait until SF register is cleared
  while (SMPC_REG_SF & 0x1) {}
}

void smpc_issue_command(unsigned char cmd) {
  // Set SF register so that no other command can be issued.
  SMPC_REG_SF = 1;

  // Writing COMREG starts execution
  SMPC_REG_COMREG = cmd;
}

void pcmsys_load_driver(void *buffer, uint32_t length) {
  // Make sure SCSP is set to 512k mode
  *(volatile uint8_t *) (0x25B00400) = 0x02;

  // Clear Sound RAM
  for (int i = 0; i < 0x80000; i += 4) {
    *(volatile uint32_t *) (SNDRAM + i) = 0x00000000;
  }

  // The immediacy of these commands is important.
  // As per SEGA technical bulletin 51, the Sound CPU is not to be turned off
  // for more than 0.5 seconds.

  // Turn off Sound CPU
  smpc_issue_command(SMPC_CMD_SNDOFF);
  smpc_wait_till_ready();

  // Set max master volume + 4mbit memory
  *master_volume = 0x20F;

  // Copy the driver binary (code) over to sound RAM. The binary includes the
  // vector table information.
  memcpy((void *) SNDRAM, buffer, length);

  // Set the ADX coefficients for the driver to use, if one was selected.
  const uint32_t master_adx_frequency = ADX_MASTER_768;
  if (master_adx_frequency == ADX_MASTER_768) {
    m68k_com->drv_adx_coef_1 = ADX_768_COEF_1;
    m68k_com->drv_adx_coef_2 = ADX_768_COEF_2;
  } else if (master_adx_frequency == ADX_MASTER_1152) {
    m68k_com->drv_adx_coef_1 = ADX_1152_COEF_1;
    m68k_com->drv_adx_coef_2 = ADX_1152_COEF_2;
  } else if (master_adx_frequency == ADX_MASTER_1536) {
    m68k_com->drv_adx_coef_1 = ADX_1536_COEF_1;
    m68k_com->drv_adx_coef_2 = ADX_1536_COEF_2;
  } else if (master_adx_frequency == ADX_MASTER_2304) {
    m68k_com->drv_adx_coef_1 = ADX_2304_COEF_1;
    m68k_com->drv_adx_coef_2 = ADX_2304_COEF_2;
  } else {
    m68k_com->drv_adx_coef_1 = 1;
    m68k_com->drv_adx_coef_2 = 1;
  }

  // Turn on Sound CPU again
  smpc_wait_till_ready();
  smpc_issue_command(SMPC_CMD_SNDON);

  m68k_com->start = 0xFFFF;
}

short calculate_bytes_per_blank(int sampleRate, bool is8Bit, bool isPAL) {
  int frameCount = (isPAL == true) ? 50 : 60;
  int sampleSize = (is8Bit == true) ? 8 : 16;
  return ((sampleRate * sampleSize) >> 3) / frameCount;
}

short convert_bitrate_to_pitchword(short sampleRate) {
  int octr;
  int shiftr;
  int fnsr;

  octr = PCM_CALC_OCT(sampleRate);
  shiftr = PCM_CALC_SHIFT_FREQ(octr);
  fnsr = PCM_CALC_FNS(sampleRate, shiftr);

  return PCM_SET_PITCH_WORD(octr, fnsr);
}

uint32_t getSlotAddressOffset(uint32_t slot) {
  // Start past 64kb of the start of sound memory (to allow room for driver +
  // needed data) and then use 64kb for each 'slot'
  return (uint32_t) (0x10000 + slot * getSlotSize());
}

uint8_t *getSlotAddress(uint32_t slot) {
  return (uint8_t *) (getSlotAddressOffset(slot) + SNDRAM);
}

uint32_t getSlotSize() { return (64 * 1024); }

uint32_t pcmStreamBufferSize(uint8_t bits) { return bits == 8 ? 44100 : 88200; }

void pcmsys_load_16bit_pcm_slot(uint32_t length, int sampleRate,
  uint32_t slot) {
  const uint32_t destinationAddress = getSlotAddressOffset(slot);

  // PCM size too large for general-purpose playback [could still work with
  // timed execution & offets]
  if (length > (128 * 1024))
    return;

  length += ((unsigned int) length & 1) ? 1 : 0;
  length += ((unsigned int) length & 3) ? 2 : 0;

  m68k_com->pcmCtrl[slot].hiAddrBits =
    (unsigned short) (destinationAddress >> 16);
  m68k_com->pcmCtrl[slot].loAddrBits =
    (unsigned short) (destinationAddress & 0xFFFF);

  m68k_com->pcmCtrl[slot].pitchword = convert_bitrate_to_pitchword(sampleRate);
  m68k_com->pcmCtrl[slot].playsize = (length >> 1);
  m68k_com->pcmCtrl[slot].bytes_per_blank =
    calculate_bytes_per_blank(sampleRate, false, PCM_SYS_REGION);
  m68k_com->pcmCtrl[slot].bitDepth = PCM_TYPE_16BIT;
  m68k_com->pcmCtrl[slot].loopType = PCM_NO_LOOP;
  m68k_com->pcmCtrl[slot].volume = PCM_MAX_VOLUME;
}

void pcmsys_load_8bit_pcm_slot(uint32_t length, int sampleRate, uint32_t slot) {
  const uint32_t destinationAddress = getSlotAddressOffset(slot);

  // PCM size too large for general-purpose playback [could still work with
  // timed execution & offets]
  if (length > (64 * 1024))
    return;

  length += ((unsigned int) length & 1) ? 1 : 0;
  length += ((unsigned int) length & 3) ? 2 : 0;

  m68k_com->pcmCtrl[slot].hiAddrBits =
    (unsigned short) (destinationAddress >> 16);
  m68k_com->pcmCtrl[slot].loAddrBits =
    (unsigned short) (destinationAddress & 0xFFFF);

  m68k_com->pcmCtrl[slot].pitchword = convert_bitrate_to_pitchword(sampleRate);
  m68k_com->pcmCtrl[slot].playsize = length;
  m68k_com->pcmCtrl[slot].bytes_per_blank =
    calculate_bytes_per_blank(sampleRate, true, PCM_SYS_REGION);
  m68k_com->pcmCtrl[slot].bitDepth = PCM_TYPE_8BIT;
  m68k_com->pcmCtrl[slot].loopType = PCM_NO_LOOP;
  m68k_com->pcmCtrl[slot].volume = PCM_MAX_VOLUME;
}

#define MAX_PCM_STREAM_BUFFERS 2

typedef struct {
  int16_t isUsed;
  int16_t pcmIndex;
  uint32_t size;
} pcm_stream_buffer;

typedef struct {
  pcm_stream_buffer buffers[MAX_PCM_STREAM_BUFFERS];
  uint8_t volume;
  int8_t controlType;
  bool isWarming;
  bool isPlaying;
} pcm_stream;

pcm_stream pcmStream;

void pcmStreamClear() {
  memset(pcmStream.buffers, 0,
    sizeof(pcm_stream_buffer) * MAX_PCM_STREAM_BUFFERS);
  pcmStream.volume = 0;
  pcmStream.controlType = PCM_FWD_LOOP;
  pcmStream.isWarming = false;
  pcmStream.isPlaying = false;
}

void pcmStreamInitialize() { pcmStreamClear(); }

void pcmStreamConfigure(uint8_t channels, uint8_t bits, uint32_t frequency) {
  uint32_t bufferSize = pcmStreamBufferSize(bits);
  for (uint32_t i = 0; i < channels; ++i) {
    pcm_stream_buffer *nextBuffer = &pcmStream.buffers[i];
    if (nextBuffer->isUsed) {
      return NULL;
    }

    nextBuffer->isUsed = true;
    nextBuffer->pcmIndex = i;

    if (bits == 8) {
      pcmsys_load_8bit_pcm_slot(bufferSize, frequency, nextBuffer->pcmIndex);
    } else {
      pcmsys_load_16bit_pcm_slot(bufferSize, frequency, nextBuffer->pcmIndex);
    }
      
    memset(getSlotAddress(i), 0, getSlotSize());
  }
}

void pcmStreamWarmUp() {
  if (pcmStream.isWarming || pcmStream.isPlaying) {
    return;
  }

  for (uint32_t i = 0; i < MAX_PCM_STREAM_BUFFERS; ++i) {
    pcm_play(i, pcmStream.controlType, 0);
  }
    
  pcmStream.isWarming = true;
}

bool pcmStreamPlay(uint8_t volume) {
  if (pcmStream.isPlaying) {
    return false;
  }

  if (pcmStream.isWarming) {
    pcmStream.isWarming = false;
    pcmStreamStop();
  }

  // Play all available channels at once
  for (uint32_t i = 0; i < MAX_PCM_STREAM_BUFFERS; ++i) {
    pcm_stream_buffer *nextBuffer = &pcmStream.buffers[i];
    if (nextBuffer->isUsed) {
      pcmStream.isPlaying = true;
      pcmStream.volume = volume;
      pcm_play(nextBuffer->pcmIndex, pcmStream.controlType, volume);
    }
  }

  sound_notify_driver();
  return true;
}

bool pcmStreamStop() {
  if (pcmStream.isPlaying) {
    for (uint32_t i = 0; i < MAX_PCM_STREAM_BUFFERS; ++i) {
      pcm_cease(i);
    }

    pcmStreamClear();
  }

  sound_notify_driver();
  return true;
}

void sound_notify_driver(void) { m68k_com->start = 1; }
