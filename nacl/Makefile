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

# AirPlay sends audio as AAC-ELD.  The module decodes it itself and hands PCM to
# Pepper's audio output, which needs FDK-AAC.  FDK is an external dependency: it
# is not bundled here and is not covered by this project's GPL-3.0 license (see
# nacl/THIRD-PARTY.md).  scripts/fetch-fdk-aac.sh fetches FDK 2.0.3 and builds
# it for the toolchain above into .build/; set FDK_DIR to an existing FDK 2.0.3
# checkout instead if you already have one.
BUILD_DIR   := $(abspath $(CURDIR)/../.build)
FDK_DIR     ?= $(BUILD_DIR)/fdk-aac
FDK_LIB     ?= $(BUILD_DIR)/fdk-nacl/libfdk-aac.a
FDK_INC     := $(shell python3 -c "import os,sys; print(' '.join('-I'+os.path.relpath(p) for p in sys.argv[1:]))" \
                 '$(FDK_DIR)/libAACdec/include' '$(FDK_DIR)/libSYS/include' '$(FDK_DIR)/libFDK/include')
FDK_LIB_REL := $(shell python3 -c "import os; print(os.path.relpath('$(FDK_LIB)'))")

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

CXXFLAGS := $(FDK_INC) -O2 -std=gnu++11 -I$(INC_DIR) -Wall -Wno-unused-parameter
# -w for the vendored playfair tree only: it is third-party, prints on every
# call, and its warnings would bury ours.
CFLAGS   := -O2 -Wall -Wno-unused-parameter
VENDOR_W := -w
LDFLAGS  := -L$(LIB_DIR)
LIBS     := -lppapi_cpp -lppapi
# The static FDK archive must come before -lm; modified_md5.c calls fabs/sin to
# build the MD5 round constants, and FDK itself needs libm.
LDLIBS   := $(FDK_LIB_REL) -lm

.PHONY: all clean
all: $(TARGET)-arm.nexe

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

playfair/%.o: playfair/%.c
	$(CC) $(CFLAGS) $(VENDOR_W) -c $< -o $@

$(TARGET).pexe: $(OBJS) $(FDK_LIB)
	$(CXX) $(CXXFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) $(LDLIBS) -o $@

$(TARGET).final.pexe: $(TARGET).pexe
	$(FINALIZE) $< -o $@

# -O2 on translate: arm translation is the last place optimisation is applied.
$(TARGET)-arm.nexe: $(TARGET).final.pexe
	$(TRANSLATE) -arch arm -O2 $< -o $@

# Named explicitly so a missing FDK build explains itself instead of surfacing
# as an "undefined reference to aacDecoder_Open" at link time.
$(FDK_LIB):
	@echo "FDK-AAC is not built: $(FDK_LIB) does not exist."
	@echo "Run ./scripts/fetch-fdk-aac.sh (with PEPPER_DIR set) to fetch and build it,"
	@echo "or set FDK_DIR to a checkout of FDK 2.0.3 you have already built."
	@exit 1

# Only generated build intermediates are removed.
clean:
	rm -f $(OBJS) $(TARGET).pexe $(TARGET).final.pexe $(TARGET)-arm.nexe

airplay_module.o: video_sink.h native_video_sink.h pcm_queue.h rtp_queue.h url_video.h h264_size.h audio_sink.h aes.h bplist.h
