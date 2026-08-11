ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

SH_PROGRAM:= cinepak_player
SH_SRCS :=       \
  adx_dsp_sega.c \
  base.c         \
  film_buff.c    \
  film_cvid.c    \
  film_lib.c     \
  film_snd.c     \
  main.c         \
  pcmsys.c       \
  scsp_dsp.c     \
  snd_adx.c

SH_LIBRARIES:=
SH_CFLAGS+= -I. -O2 -g -fno-lto -Wall
IP_VERSION:= V1.000
IP_RELEASE_DATE:= 20220720
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= cinepak_player
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0


include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk

