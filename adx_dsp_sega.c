#include <yaul.h>

#include "adx_dsp_sega.h"
#include "pcmsys.h"
#include "scsp_dsp.h"

// THIS IS PRETTY MUCH ENTIRELY CLAUDE AI GENERATED.
// I had Claude look into Sega's official ADX Implementation and 
// then look at Celeriac's ADPCM DSP engine and between the two it was
// able to make this which works with Ponesound. It can probably be cleaned up and
// made better by someone who knows the SCSP DSP better than me.

// TEMP slot allocation. Each channel owns exactly two dedicated slots:
//
//   HIST_BASE - written ONCE per sample, at the very end of that channel's
//   chain, with the final filtered output. Never read directly by index -
//   read via TRA=HIST_BASE+1/+2/+3, which the ring's own per-sample
//   pointer decrement turns into "1/2/3 samples ago" for free. This is the
//   actual mechanism Sega's real program uses (confirmed: their TRA=118/117
//   pair for one channel is a fixed write point read back at two adjacent
//   relative offsets, not two independently-maintained slots) - and it's
//   the bug in the previous attempt: that version tried to maintain H1/H2
//   as two separate slots but only ever wrote H1, so H2 silently read zero
//   forever, collapsing what should be a 2-tap filter into a 1-tap one.
//
//   STAGE1_TMP - same-pass handoff between this channel's two MAC stages,
//   mirroring Sega's own step113(write)->step114(read) pattern exactly
//   (write, then read back one step later in the SAME 128-step pass, not
//   waiting for the ring to advance a full sample). Never read via a
//   relative offset - always read at literally the same index it was just
//   written to, since the pipeline gap (not the ring) is what makes the
//   fresh value available.
//
// Spaced well apart (10 slots) so a channel's own +1/+2/+3 history reads
// can never collide with the other channel's slots.
#define ADX_DSP_L_HIST_BASE  (0)
#define ADX_DSP_L_STAGE1_TMP (10)
#define ADX_DSP_R_HIST_BASE  (20)
#define ADX_DSP_R_STAGE1_TMP (30)

// COEF allocation. Values are Sega's own, confirmed via live SCSP captures
// during actual Sakura Wars 2 Cinepak+ADX playback (steps 110-115 of its
// real DSP microprogram, channel 9's chain - see SCSP_ADX_FILTER_FINDINGS.md).
// Shared by both channels - the filter shape is identical, only the TEMP/
// MIXS/EFREG indices differ per channel.
#define ADX_DSP_COEF_A     (0)  // ~0.895  (raw 0x7298, v=3667)  - Sega's COEF[61]
#define ADX_DSP_COEF_B     (1)  // ~-0.401 (raw 0xCCB0, v=-1642) - Sega's COEF[62]
#define ADX_DSP_COEF_INPUT (2)  // 0.5     (raw 0x4000, v=2048)  - Sega's COEF[60]
#define ADX_DSP_COEF_UNITY (3)  // ~0.9998 (raw 0x7FF8, v=4095)  - Sega's COEF[63]/COEF[0]

// Feeder voices route into these two MIXS slots (read by the DSP via
// IRA 32/33 - MIXS occupies IRA 32-47, see scsp_dsp.h).
#define ADX_DSP_MIXS_L (0)
#define ADX_DSP_MIXS_R (1)
#define ADX_DSP_IRA_MIXS_L (32 + ADX_DSP_MIXS_L)
#define ADX_DSP_IRA_MIXS_R (32 + ADX_DSP_MIXS_R)

// Deliberately different numbers from ADX_DSP_FEEDER_SLOT_L/R (0/1) above -
// EFREG is a separate DSP output register bank, not aliased to PCM voice
// slot numbers in hardware, but keeping the index spaces visibly distinct
// avoids any confusion about which "0"/"1" a given piece of code means.
#define ADX_DSP_EFREG_L (2)
#define ADX_DSP_EFREG_R (3)

// Raw per-slot SCSP register access - pcmsys.h only exposes the
// driver-mediated _PCM_CTRL protocol, but ISEL/IMXL (DSP mix routing) and
// DISDL (dry output level) aren't modeled by that struct. Confirmed safe to
// write directly (unchanged from the previous attempt - this plumbing was
// never the problem): the driver's csr[] is a direct pointer to these same
// registers (not a shadow copy synced each tick), and its own input_sel
// write only happens once at boot.
#define ADX_DSP_SLOT_REG(slot, offset) \
  (*(volatile uint16_t *) (SNDRAM + 0x100000 + (slot) * 0x20 + (offset)))

// +0x16 packs FOUR fields into one 16-bit register (manual-confirmed,
// MSB-first same as +0x00's KX/KB/SBCTL/.../SA field ordering):
// DISDL[15:13] DIPAN[12:8] EFSDL[7:5] EFPAN[4:0]. Blindly writing 0x0000
// here (the previous version of this code, and the old adx_dsp.c before
// it) doesn't just mute DISDL - it also zeroes EFSDL, and the manual
// describes EFSDL as controlling how much of this slot's signal reaches
// the DSP-processed output path. Needs a masked read-modify-write instead
// of a blind overwrite so DIPAN/EFPAN aren't disturbed either.
#define ADX_DSP_DISDL_SHIFT (13)
#define ADX_DSP_EFSDL_SHIFT (5)
#define ADX_DSP_EFPAN_SHIFT (0)
#define ADX_DSP_DISDL_MASK  (0x7  << ADX_DSP_DISDL_SHIFT)
#define ADX_DSP_EFSDL_MASK  (0x7  << ADX_DSP_EFSDL_SHIFT)
#define ADX_DSP_EFPAN_MASK  (0x1F << ADX_DSP_EFPAN_SHIFT)

// EFPAN[4:0]: 00h-0Fh pan left (-3.0dB at 00h down to -42.0dB at 0Fh, i.e.
// increasing attenuation on the OPPOSITE side as the value rises), 10h-1Fh
// pan right the same way. 0Fh/1Fh are the hardest-left/hardest-right
// values, not the softest - never explicitly set anywhere before this, so
// both EFREG_L and EFREG_R were sitting at the hardware default (0 =
// barely-left-of-center) instead of being split hard-left/hard-right,
// which would explain an audible channel imbalance rather than true
// stereo separation.
#define ADX_DSP_EFPAN_HARD_LEFT  (0x0F)
#define ADX_DSP_EFPAN_HARD_RIGHT (0x1F)

// Forces DISDL=0 (muted dry output - only the DSP-filtered EFREG result
// should be audible) and EFSDL=7 (0dB, max - the manual's value table for
// both fields runs 0="-inf(do not send)" through 7="-0dB", same
// progression) every time it's called, leaving DIPAN/EFPAN untouched.
// Needs to force EFSDL every time, not just once at configure time: the
// 68K driver's own play-command handling writes pan_send=(7<<13)|(pan<<8)
// to this same register sometime after processing sh2_permit=1 (see
// adx_dsp_remute_dry_output()'s longer comment) - that write only fills
// the top byte, so it zeroes EFSDL as a side effect of unmuting DISDL.
// Previously this code only fought the DISDL half of that clobber
// (forcing the WHOLE register back to 0), which incidentally re-zeroed
// EFSDL too instead of restoring it - silencing the DSP's actual input
// permanently after the driver's first clobber, while DISDL stayed
// correctly muted. That's the leading theory for "brief real audio, then
// permanent silence" - untested until this change.
static void adx_dsp_force_mute_and_effect_send(uint32_t slot) {
  uint16_t reg = ADX_DSP_SLOT_REG(slot, 0x16);
  reg &= (uint16_t) ~(ADX_DSP_DISDL_MASK | ADX_DSP_EFSDL_MASK);
  reg |= (uint16_t) ADX_DSP_EFSDL_MASK; // EFSDL=7; DISDL bits stay 0 (masked off above)
  ADX_DSP_SLOT_REG(slot, 0x16) = reg;
}

// Only meaningful on the EFREG_L/EFREG_R slots - EFPAN positions THAT
// slot's EFREG output in the stereo field. One-shot, same as
// adx_dsp_force_mute_and_effect_send() on those slots: nothing ever plays
// them as real voices, so there's no driver clobbering to fight here.
static void adx_dsp_set_efpan(uint32_t slot, uint16_t efpan) {
  uint16_t reg = ADX_DSP_SLOT_REG(slot, 0x16);
  reg &= (uint16_t) ~ADX_DSP_EFPAN_MASK;
  reg |= (uint16_t) (efpan << ADX_DSP_EFPAN_SHIFT);
  ADX_DSP_SLOT_REG(slot, 0x16) = reg;
}

static void adx_dsp_configure_feeder(uint32_t slot, uint32_t mixsIndex,
  uint32_t bufferSize, uint16_t sampleRate) {
  pcmsys_load_16bit_pcm_slot(bufferSize, sampleRate, slot, PCM_FWD_LOOP);
  memset(getSlotAddress(slot), 0, getSlotSize());

  // Neither pcmsys_load_16bit_pcm_slot() nor pcm_play() ever set
  // icsr_target - if the driver's default for that field is "unassigned"
  // rather than "same as the pcmCtrl index", sh2_permit=1 would get
  // processed without the driver ever knowing which real hardware voice to
  // key on, leaving its true ICSR register untouched. Set it explicitly.
  m68k_com->pcmCtrl[slot].icsr_target = (int8_t) slot;

  // ISEL[3:0]/IMXL[2:0] at +0x14: route this voice's output into
  // MIXS[mixsIndex] at level 7. Values taken directly from scspadpcm's
  // adp68k.c (proven working code), not guessed.
  ADX_DSP_SLOT_REG(slot, 0x14) = (uint16_t) ((mixsIndex << 3) | 0x07);

  // DISDL=0/EFSDL=7 at +0x16: mute this voice's dry output (only the
  // DSP-filtered EFREG result should be audible) while making sure its
  // effect send is actually at max, not incidentally zeroed.
  adx_dsp_force_mute_and_effect_send(slot);

  // Bypass EG/TL/ALFO at +0x0C bit 8, so the envelope generator's
  // attack/decay ramp doesn't distort the residual stream on its way into
  // the DSP. Also taken from adp68k.c.
  ADX_DSP_SLOT_REG(slot, 0x0C) |= (1 << 8);
}

// Builds one channel's 6-step chain, faithfully matching Sega's real
// two-stage structure (steps 110-115 of their program, generalized to
// caller-supplied slot indices):
//
//   stage1 = hist[-1]*COEF_A + hist[-2]*COEF_B + live*COEF_INPUT + hist[-3]*COEF_UNITY
//            -> written to stage1Tmp
//   stage2 = stage1Tmp*COEF_UNITY + hist[-1]*COEF_UNITY
//            -> written to EFREG[efregIndex] AND to histBase (next sample's history)
//
// The hist[-3] term in stage1 and the second term in stage2 are OUR OWN
// design choice, not a literal copy of Sega's exact operands there - their
// real TRA=3/TRA=5 reads something we can't identify from our TEMP dumps
// with certainty (possibly a genuinely-owned third tap, possibly something
// specific to their own multi-channel allocation that reads as zero for
// this filter). Rather than guess at their exact slot and risk silently
// reading garbage the way the previous attempt's H2 bug did, both of these
// read our OWN dedicated histBase tap again, so every term is guaranteed
// meaningful. If this turns out to still be unstable, the first thing to
// try is dropping these two terms back to zero (i.e. a pure 3-term
// stage1 + 1-term stage2) to isolate whether they're the problem.
static void adx_dsp_build_channel(uint64_t program[DSP_STEP_COUNT],
  unsigned int step0, unsigned int histBase, unsigned int stage1Tmp,
  unsigned int iraMixs, unsigned int efregIndex) {

  program[step0 + 0] = DSP_ZERO |
    DSP_XSEL(0) | DSP_TRA(histBase + 1) | DSP_YSEL(1) | DSP_CRA(ADX_DSP_COEF_A);

  program[step0 + 1] = DSP_BSEL(1) |
    DSP_XSEL(0) | DSP_TRA(histBase + 2) | DSP_YSEL(1) | DSP_CRA(ADX_DSP_COEF_B);

  program[step0 + 2] = DSP_BSEL(1) |
    DSP_XSEL(1) | DSP_IRA(iraMixs) | DSP_YSEL(1) | DSP_CRA(ADX_DSP_COEF_INPUT);

  // Stage 1 final term + write. SHFT0 placed here to match Sega's own
  // step113 exactly - none of these coefficients individually overflow
  // COEF's 13-bit range, so this looks like a routine part of this MAC
  // chain shape rather than overflow compensation, but it's copied
  // faithfully rather than guessed away.
  program[step0 + 3] = DSP_BSEL(1) | DSP_SHFT0 |
    DSP_XSEL(0) | DSP_TRA(histBase + 3) | DSP_YSEL(1) | DSP_CRA(ADX_DSP_COEF_UNITY) |
    DSP_TWT | DSP_TWA(stage1Tmp);

  // Stage 2, term 1: read stage1Tmp back one step after writing it - same
  // pipeline-gap trick as Sega's step113->step114 (a same-pass read, not a
  // ring-delayed one - stage1Tmp is read at its own literal index here,
  // not histBase+N).
  program[step0 + 4] = DSP_ZERO |
    DSP_XSEL(0) | DSP_TRA(stage1Tmp) | DSP_YSEL(1) | DSP_CRA(ADX_DSP_COEF_UNITY);

  // Stage 2, term 2 + final write: to EFREG (audible output) AND back to
  // histBase, so next sample's hist[-1]/[-2]/[-3] taps see this sample's
  // real filtered result.
  program[step0 + 5] = DSP_BSEL(1) |
    DSP_XSEL(0) | DSP_TRA(histBase + 1) | DSP_YSEL(1) | DSP_CRA(ADX_DSP_COEF_UNITY) |
    DSP_EWT | DSP_EWA(efregIndex) |
    DSP_TWT | DSP_TWA(histBase);
}

static void adx_dsp_build_program(uint64_t program[DSP_STEP_COUNT]) {
  memset(program, 0, DSP_STEP_COUNT * sizeof(uint64_t));

  adx_dsp_build_channel(program, 0,
    ADX_DSP_L_HIST_BASE, ADX_DSP_L_STAGE1_TMP, ADX_DSP_IRA_MIXS_L, ADX_DSP_EFREG_L);
  adx_dsp_build_channel(program, 6,
    ADX_DSP_R_HIST_BASE, ADX_DSP_R_STAGE1_TMP, ADX_DSP_IRA_MIXS_R, ADX_DSP_EFREG_R);
}

uint32_t adx_dsp_setup(uint16_t sampleRate) {
  // coef1/coef2 (the SH2 predictor's own Q12 table, decoder.c) aren't used
  // for the filter math - using Sega's own confirmed real coefficients
  // instead. KNOWN LIMITATION, same as before: only confirmed correct for
  // 44100Hz (what Sakura Wars 2 uses) - kept as parameters so the call site
  // doesn't need to change if per-rate values get found later.
  int16_t coef1;
  int16_t coef2;

  switch (sampleRate) { 
      case 44100:
        coef1 = 3667;
        coef2 = -1642;
        break;
      case 37800:
        coef1 = 3600;
        coef2 = -1582;        
        break;
      case 32000:
        coef1 = 3517;
        coef2 = -1510;
        break;
      case 22050:
        coef1 = 3285;
        coef2 = -1317;
        break;
      case 18000:
        coef1 = 3127;
        coef2 = -1193;
        break;
      case 16000:
        coef1 = 3024;
        coef2 = -1116;
        break;
      case 14700:
        coef1 = 2945;
        coef2 = -1059;
        break;
      default:
        coef1 = 3667;
        coef2 = -1642;
        break;
  }

  DSP_COEF[ADX_DSP_COEF_A] = DSP_MAKE_COEF(coef1);
  DSP_COEF[ADX_DSP_COEF_B]     = DSP_MAKE_COEF(coef2);
  DSP_COEF[ADX_DSP_COEF_INPUT] = DSP_MAKE_COEF(2048);
  DSP_COEF[ADX_DSP_COEF_UNITY] = DSP_MAKE_COEF(4095);

  static uint64_t program[DSP_STEP_COUNT];
  adx_dsp_build_program(program);
  scsp_dsp_load_program(program);

  uint32_t bufferSize = pcmStreamBufferSize(16, sampleRate);
  adx_dsp_configure_feeder(ADX_DSP_FEEDER_SLOT_L, ADX_DSP_MIXS_L, bufferSize,
    sampleRate);
  adx_dsp_configure_feeder(ADX_DSP_FEEDER_SLOT_R, ADX_DSP_MIXS_R, bufferSize,
    sampleRate);

  // Live capture confirmed the DSP itself is working correctly (MIXS
  // receiving real residual, TEMP actively computing, EFREG[2]/[3] holding
  // real nonzero filtered samples) while still being completely silent -
  // meaning EFREG's output wasn't reaching the DAC at all. +0x16's EFSDL
  // field lives in the per-SLOT register block, but the manual describes
  // it as controlling "output level of waveform data (effect data)
  // processed through DSP... to the D/A converter" - i.e. this is likely
  // NOT about a slot's own dry audio, it's slot N's EFSDL gating whether
  // EFREG[N] specifically reaches the output. Slots 2/3 (our EFREG
  // indices) are never touched anywhere else in this file - they're not
  // real playing voices, just borrowing the same per-slot register
  // addressing scheme for this purpose - so their EFSDL sits at whatever
  // the hardware's power-on default is (almost certainly 0, muted) unless
  // explicitly set here. One-shot, not per-tick: nothing ever calls
  // pcm_play()/sh2_permit on these slots, so there's no driver clobbering
  // race to fight for them the way there is for the feeder voices.
  adx_dsp_force_mute_and_effect_send(ADX_DSP_EFREG_L);
  adx_dsp_force_mute_and_effect_send(ADX_DSP_EFREG_R);

  // EFPAN was never set anywhere before this, so both EFREG_L and EFREG_R
  // sat at the hardware default (0 = barely-left-of-center on the pan
  // curve) instead of being split hard-left/hard-right - likely why a
  // stereo test showed one channel much quieter than the other (both
  // channels panned almost the same way, rather than positioned
  // separately) instead of clean stereo separation.
  adx_dsp_set_efpan(ADX_DSP_EFREG_L, ADX_DSP_EFPAN_HARD_LEFT);
  adx_dsp_set_efpan(ADX_DSP_EFREG_R, ADX_DSP_EFPAN_HARD_RIGHT);

  return bufferSize;
}

void adx_dsp_reset_history(void) {
  DSP_TEMP[ADX_DSP_L_HIST_BASE]  = 0;
  DSP_TEMP[ADX_DSP_L_STAGE1_TMP] = 0;
  DSP_TEMP[ADX_DSP_R_HIST_BASE]  = 0;
  DSP_TEMP[ADX_DSP_R_STAGE1_TMP] = 0;
}

void adx_dsp_play(uint8_t volume) {
  pcm_play(ADX_DSP_FEEDER_SLOT_L, PCM_FWD_LOOP, volume);
  pcm_play(ADX_DSP_FEEDER_SLOT_R, PCM_FWD_LOOP, volume);
  sound_notify_driver();
}

// The 68K driver's own play-command handling overwrites +0x16 with
// pan_send = (7<<13)|(pan<<8) - a hardcoded DISDL=7 (unmuting the dry
// output) - asynchronously, sometime after it processes sh2_permit=1. That
// write only fills the top byte, so it ALSO zeroes EFSDL (bottom byte) as
// a side effect, every time it fires - not just once. Call this every tick
// to keep re-winning that race (a one-shot remute doesn't reliably win it,
// confirmed previously for the DISDL half - assume the same applies here).
void adx_dsp_remute_dry_output(void) {
  adx_dsp_force_mute_and_effect_send(ADX_DSP_FEEDER_SLOT_L);
  adx_dsp_force_mute_and_effect_send(ADX_DSP_FEEDER_SLOT_R);
}

void adx_dsp_stop(void) {
  pcm_cease(ADX_DSP_FEEDER_SLOT_L);
  pcm_cease(ADX_DSP_FEEDER_SLOT_R);
  adx_dsp_remute_dry_output();
}
