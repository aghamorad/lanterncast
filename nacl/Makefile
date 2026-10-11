# PNaCl build for the AirPlay NaCl module (Pepper 47, Samsung TV).
#
# The Pepper 47 SDK's own tools/common.mk hard-depends on `python` (Python 2),
# which does not exist on this Mac; and its pydir has been ported to Python 3.
# We therefore drive the drivers directly and hand-write the .nmf.
#
# The link line matters: OBJECT FILES FIRST, then -L/-l.  Libraries listed
# before the objects they satisfy produce "undefined reference" errors.

ifndef PEPPER_DIR
$(error Set PEPPER_DIR to the Samsung Pepper 47 SDK directory)
endif
PNACL_HOST ?= mac
PNACL_BIN  := $(PEPPER_DIR)/toolchain/$(PNACL_HOST)_pnacl/bin
INC_DIR    := $(shell python3 -c "import os; print(os.path.relpath('$(PEPPER_DIR)/include'))")
LIB_DIR    := $(PEPPER_DIR)/lib/pnacl/Release

export PNACLPYTHON = python3
export PATH := $(PNACL_BIN):$(PATH)

CXX      := $(PNACL_BIN)/pnacl-clang++
CC       := $(PNACL_BIN)/pnacl-clang
FINALIZE := $(PNACL_BIN)/pnacl-finalize
TRANSLATE:= $(PNACL_BIN)/pnacl-translate

TARGET := airplay

# The FairPlay layer is C, and stays C.  Compiled as C++ the vendored playfair
# sources fail on implicit void* conversions and on casts between
# `unsigned char*` and `uint32_t*`; as C they are what their author wrote.
CXX_SRC := airplay_module.cc bplist.cc
C_SRC   := aes.c sha512.c fairplay.c \
           playfair/playfair.c \
           playfair/omg_hax.c \
           playfair/hand_garble.c \
           playfair/sap_hash.c \
           playfair/modified_md5.c
OBJS    := $(CXX_SRC:.cc=.o) $(C_SRC:.c=.o)

CXXFLAGS := -O2 -std=gnu++11 -I$(INC_DIR) -Wall -Wno-unused-parameter
# -w for the vendored playfair tree only: it is third-party, prints on every
# call, and its warnings would bury ours.
CFLAGS   := -O2 -Wall -Wno-unused-parameter
VENDOR_W := -w
LDFLAGS  := -L$(LIB_DIR)
LIBS     := -lppapi_cpp -lppapi
# modified_md5.c calls fabs/sin to build the MD5 round constants.
LDLIBS   := -lm

.PHONY: all clean
all: $(TARGET)-arm.nexe

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

playfair/%.o: playfair/%.c
	$(CC) $(CFLAGS) $(VENDOR_W) -c $< -o $@

$(TARGET).pexe: $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) $(LDLIBS) -o $@

$(TARGET).final.pexe: $(TARGET).pexe
	$(FINALIZE) $< -o $@

# -O2 on translate: arm translation is the last place optimisation is applied.
$(TARGET)-arm.nexe: $(TARGET).final.pexe
	$(TRANSLATE) -arch arm -O2 $< -o $@

# Only generated build intermediates are removed.
clean:
	rm -f $(OBJS) $(TARGET).pexe $(TARGET).final.pexe $(TARGET)-arm.nexe

airplay_module.o: video_sink.h audio_sink.h aes.h bplist.h
