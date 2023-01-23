#ifndef __PCMSYS_H__
#define __PCMSYS_H__

#include <stdbool.h>

// Likely duplicates from other libraries (in this case, taken from iapetus)

#define SMPC_REG_IREG(i) *((volatile unsigned char *) 0x20100001 + ((i) *2))
#define SMPC_REG_COMREG  *((volatile unsigned char *) 0x2010001F)
#define SMPC_REG_OREG(o) *((volatile unsigned char *) 0x20100021 + ((o) *2))
#define SMPC_REG_SR      *((volatile unsigned char *) 0x20100061)
#define SMPC_REG_SF      *((volatile unsigned char *) 0x20100063)
#define SMPC_REG_PDR1    *((volatile unsigned char *) 0x20100075)
#define SMPC_REG_PDR2    *((volatile unsigned char *) 0x20100077)
#define SMPC_REG_DDR1    *((volatile unsigned char *) 0x20100079)
#define SMPC_REG_DDR2    *((volatile unsigned char *) 0x2010007B)
#define SMPC_REG_IOSEL   *((volatile unsigned char *) 0x2010007D)
#define SMPC_REG_EXLE    *((volatile unsigned char *) 0x2010007F)

#define SMPC_CMD_MSHON    0x00
#define SMPC_CMD_SSHON    0x02
#define SMPC_CMD_SSHOFF   0x03
#define SMPC_CMD_SNDON    0x06
#define SMPC_CMD_SNDOFF   0x07
#define SMPC_CMD_CDON     0x08
#define SMPC_CMD_CDOFF    0x09
#define SMPC_CMD_CARTON   0x0A
#define SMPC_CMD_CARTOFF  0x0B
#define SMPC_CMD_SYSRES   0x0D
#define SMPC_CMD_CKCHG352 0x0E
#define SMPC_CMD_CKCHG320 0x0F
#define SMPC_CMD_INTBACK  0x10
#define SMPC_CMD_SETTIM   0x16
#define SMPC_CMD_SETSM    0x17
#define SMPC_CMD_NMIREQ   0x18
#define SMPC_CMD_RESENA   0x19
#define SMPC_CMD_RESDIS   0x1A

// Address mapping shorthand
#define MAP_TO_SCSP(sh2map_snd_adr) ((sh2map_snd_adr - SNDRAM))

// No Touchy Sound Ram Start!
#define SNDRAM (0x25a00000)

// 1KB here is reserved for interrupts
#define SNDPRG (SNDRAM + 0x408)

// Also the end of sound RAM
#define PCMEND (SNDRAM + 0x7F000)

#define DRV_SYS_END \
  (47 * 1024) // System defined safe end of driver's address space

#define PCM_CTRL_MAX (64)


#define PCM_NO_LOOP   (0)
#define PCM_ALT_LOOP  (3)
#define PCM_RVS_LOOP  (2)
#define PCM_FWD_LOOP  (1)
#define PCM_VOLATILE  (0)
#define PCM_PROTECTED (-1)
#define PCM_SEMI      (-2)
#define ADX_STREAM    (-3)

#define PCM_MAX_VOLUME (7)

#define PCM_TYPE_ADX   (2) // 4-bit (compressed audio)
#define PCM_TYPE_8BIT  (1) // 8-bit
#define PCM_TYPE_16BIT (0) // 16-bit

#define PCM_SYS_REGION (0) // 0 for NTSC, 1 for PAL

#define PCM_PAN_LEFT  (1 << 4)
#define PCM_PAN_RIGHT (0)

#define ADX_MASTER_768  (0)
#define ADX_MASTER_1152 (1)
#define ADX_MASTER_1536 (2)
#define ADX_MASTER_2304 (3)

/* 7.68 Data */
#define ADX_768_COEF_1 (4401)
#define ADX_768_COEF_2 (-1183)

/* 11.52 data */
#define ADX_1152_COEF_1 (5386)
#define ADX_1152_COEF_2 (-1771)

/* 15.36 data */
#define ADX_1536_COEF_1 (5972)
#define ADX_1536_COEF_2 (-2187)

/* 23.04 data */
#define ADX_2304_COEF_1 (6631)
#define ADX_2304_COEF_2 (-2685)

typedef struct {
  // [0,1,2,3] No loop, normal loop, reverse loop, alternating loop
  char loopType;

  // 0 or 1, boolean
  unsigned char bitDepth;

  // bits 19-16 of...
  unsigned short hiAddrBits;

  // Two 16-bit chunks that when combined, form the
  // start address of the sound.
  unsigned short loAddrBits;

  // The # of samples forward from the start address to
  // return to after loop.
  unsigned short LSA;

  // The # of samples to play before the sound shall loop.
  // **Otherwise used as the length of the sound.** Do not leave at
  // 0! 8 bit PCM is 1 byte per sample. 16 bit PCM is 2 bytes per
  // sample. Therefore an 8bit PCM is a maximum of 64KB, and 16bit
  // is 128KB.
  unsigned short playsize;

  // the OCT & FNS word to use in the ICSR, verbatim.
  unsigned short pitchword;

  // Direct pan setting
  unsigned char pan;

  // Direct volume setting
  unsigned char volume;

  // Bytes the PCM will play every time the
  // driver is run (vblank)
  unsigned short bytes_per_blank;

  // Size of the buffer used for an ADX sound effect.
  // Specifically sized by Master SH2.
  unsigned short decompression_size;

  // Does the SH2 permit this command? If TRUE, run
  // the command. If FALSE, key its ICSR OFF.
  unsigned char sh2_permit;

  // Which explicit ICSR is this to land in? Can be controlled
  // by SH2 or by driver.
  char icsr_target;

} _PCM_CTRL; // Driver Local Command Struct

typedef struct {
  // Length of the ADX stream (in ADX frames)
  volatile unsigned int adx_stream_length;

  // System Start Boolean
  volatile unsigned short start;

  // Booleans
  volatile char adx_buffer_pass[2];

  // The (signed!) coefficient 1 the driver will
  // use to build ADX multiplication tables.
  volatile short drv_adx_coef_1;

  // The (signed!) coefficient 2 the driver will
  // use to build ADX multiplication tables.
  volatile short drv_adx_coef_2;
  volatile _PCM_CTRL *pcmCtrl;

  // Redbook left channel volume & pan.
  volatile unsigned char cdda_left_channel_vol_pan;

  // Redbook right channel volume & pan.
  volatile unsigned char cdda_right_channel_vol_pan;
} sysComPara;

typedef struct {
  // [this is 32768 or 0x8000]
  unsigned short one_half;
  short offset2data;

  // [this is 3]
  unsigned char format;

  // [this is 18]
  unsigned char block_size;

  // [this is 4]
  unsigned char bit_depth;

  // [this should be 1]
  unsigned char channels;
  unsigned int sample_rate;
  unsigned int sample_ct;

  // [this should be 500]
  unsigned short hp_cutoff;
  unsigned char loop;

  // [boolean, 0 for false, 1 for true]
  unsigned char illegal;
} adx_header;

extern sysComPara *m68k_com;
extern unsigned int *scsp_load;
extern unsigned short *master_volume;
extern short numberPCMs;

// System functions shared for pcmstm.c/h

short convert_bitrate_to_pitchword(short sampleRate);
short calculate_bytes_per_blank(int sampleRate, bool is8Bit, bool isPAL);
short lcm(short a, short b);

// These are likely to be duplicate commands from other libraries.

void smpc_wait_till_ready(void);
void smpc_issue_command(unsigned char cmd);

uint8_t *getSlotAddress(uint32_t slot);
uint32_t getSlotSize();
uint32_t pcmStreamBufferSize(uint8_t bits, uint32_t frequency);

void pcmsys_load_16bit_pcm_slot(uint32_t length, int sample_rate,
  uint32_t slot, int8_t loopType);

void pcmsys_load_8bit_pcm_slot(uint32_t length, int sample_rate, uint32_t slot,
  int8_t loopType);

void pcmsys_load_driver(void *buffer, uint32_t length);

void pcm_play(short pcmNumber, char ctrlType, char volume);
void pcm_parameter_change(short pcmNumber, char volume, char pan);
void pcm_cease(short pcmNumber);

void sound_notify_driver(void);

void pcmStreamInitialize();
void pcmStreamConfigure(uint8_t channels, uint8_t bits, uint32_t frequency);
void pcmStreamWarmUp();
void pcmStreamWarmUpStop();
bool pcmStreamPlay(uint8_t volume);
bool pcmStreamStop();

#endif
