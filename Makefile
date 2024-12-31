ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

SH_PROGRAM:= cinepak_player
SH_SRCS :=     \
  base.cpp     \
  decoder.cpp  \
  main.cpp     \
  memory.cpp   \
  timer.cpp     

SH_LIBRARIES:=
PROJECT_COMMON_FLAGS:=  \
  -O2                   \
  -g                    \
  -std=c++17            \
  -Wall                 \
  -Wno-unused-function  \
  -Wno-register         \
  -fno-builtin          \
  -flto=auto

SH_CFLAGS += $(PROJECT_COMMON_FLAGS)
SH_CXXFLAGS += $(PROJECT_COMMON_FLAGS)
IP_VERSION := V1.000
IP_RELEASE_DATE:= 20241019
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= cinepak_player
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06002000
IP_1ST_READ_ADDR:= 0x06004000
IP_1ST_READ_SIZE:= 0


include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk
