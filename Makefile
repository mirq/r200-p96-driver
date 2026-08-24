CROSS ?= /opt/amiga/bin/m68k-amigaos-
DEBUG ?= 0
FASTWAIT ?= 0
CC := $(CROSS)gcc
STRIP := $(CROSS)strip

ifeq ($(FASTWAIT),1)
ifeq ($(DEBUG),1)
TARGET ?= Radeon9200-debug-fastwait.chip
CARD_TARGET ?= Prometheus-debug.card
BUILD_DIR ?= build-debug-fastwait
else
TARGET ?= Radeon9200-fastwait.chip
CARD_TARGET ?= Prometheus.card
BUILD_DIR ?= build-fastwait
endif
else ifeq ($(DEBUG),1)
TARGET ?= Radeon9200-debug.chip
CARD_TARGET ?= Prometheus-debug.card
BUILD_DIR ?= build-debug
else
TARGET ?= Radeon9200.chip
CARD_TARGET ?= Prometheus.card
BUILD_DIR ?= build
endif
CARD_BUILD_DIR := $(BUILD_DIR)/prometheus-card
P96_SCREEN_TEST := $(BUILD_DIR)/p96screen
P96_OVERLAP_TEST := $(BUILD_DIR)/p96overlap
P96_WINDOWMOVE_TEST := $(BUILD_DIR)/p96windowmove

P96_DIR := Picasso96Develop
BYTESWAP_DIR := OpenPci2.1-SDK290208/Include

SOURCES := \
	src/startup.c \
	src/library.c \
	src/radeon9200.c \
	src/dma.c \
	src/r200_microcode.c \
	src/radeon_cp.c \
	src/radeon3d_service.c \
	src/radeon_debug.c \
	src/radeon_cursor.c \
	src/radeon_accel.c \
	src/radeon_bios.c \
	src/radeon_mode.c \
	src/runtime_shim.c
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

CARD_DIR := Prometheus/PrometheusCard
CARD_SOURCES := \
	$(CARD_DIR)/vbcc_libinit.c \
	$(CARD_DIR)/dma.c \
	$(CARD_DIR)/card_radeon9200.c \
	$(CARD_DIR)/card_s3virge.c \
	$(CARD_DIR)/card_3dlabspermedia2.c \
	$(CARD_DIR)/card_3dfxvoodoo.c \
	$(CARD_DIR)/card.c
CARD_OBJECTS := $(patsubst $(CARD_DIR)/%.c,$(CARD_BUILD_DIR)/%.o,$(CARD_SOURCES))

CPPFLAGS := \
	-Iinclude \
	-IPrometheus/PromLib \
	-I$(P96_DIR)/PrivateInclude \
	-I$(BYTESWAP_DIR)

ifeq ($(DEBUG),1)
CPPFLAGS += -DDEBUG
endif
ifeq ($(FASTWAIT),1)
CPPFLAGS += -DRADEON_FAST_WAIT
endif

CFLAGS := \
	-std=gnu99 \
	-O2 \
	-Wall \
	-Wextra \
	-Werror \
	-Wmissing-prototypes \
	-Wstrict-prototypes \
	-m68020-60 \
	-mregparm=4 \
	-msmall-code \
	-noixemul \
	-ffreestanding \
	-fno-builtin

LDFLAGS := \
	-m68020-60 \
	-mregparm=4 \
	-msmall-code \
	-noixemul \
	-ramiga-lib \
	-nostartfiles \
	-nodefaultlibs

LDLIBS := -lamiga -lgcc

CARD_CPPFLAGS := \
	-I$(CARD_DIR) \
	-IPrometheus/PromLib \
	-IPrometheus/PromLib/include \
	-I$(CARD_DIR)/proto \
	-I$(CARD_DIR)/clib \
	-I$(CARD_DIR)/inline
ifeq ($(DEBUG),1)
CARD_CPPFLAGS += -DRADEON3D_DEBUG_CHIP
endif
CARD_CFLAGS := \
	-std=gnu99 \
	-O2 \
	-Wall \
	-m68020-60 \
	-mregparm=4 \
	-noixemul \
	-ffreestanding \
	-fno-builtin
CARD_LDFLAGS := \
	-m68020-60 \
	-mregparm=4 \
	-noixemul \
	-ramiga-lib \
	-nostartfiles \
	-nodefaultlibs

ifeq ($(DEBUG),1)
LDLIBS := -ldebug -lc $(LDLIBS)
endif

VBCC_ROOT ?= /home/mirek/vbcc
VBCC_NDK ?= /opt/amiga/m68k-amigaos/ndk-include
VBCC_INCLUDE := $(VBCC_ROOT)/build/targets/m68k-amigaos/include
ABI_CHECK_GCC := $(BUILD_DIR)/abi/radeon3d-gcc.o
ABI_CHECK_VBCC := $(BUILD_DIR)/abi/radeon3d-vbcc.o
R3D_INFO_TEST := $(BUILD_DIR)/radeon3dinfo
R3D_SESSIONS_TEST := $(BUILD_DIR)/radeon3dsessions
R3D_PHASE1_TEST := $(BUILD_DIR)/radeon3dphase1
R3D_FORMATS_TEST := $(BUILD_DIR)/radeon3dformats
R3D_STREAM_TEST := $(BUILD_DIR)/radeon3dstream
VRAM_STREAM_TEST := $(BUILD_DIR)/vramstream

.PHONY: all abi-check clean r3d-tools tools vramstream r3dstream

all: $(TARGET) $(CARD_TARGET)

tools: $(P96_SCREEN_TEST) $(P96_OVERLAP_TEST) $(P96_WINDOWMOVE_TEST)

vramstream: $(VRAM_STREAM_TEST)

r3dstream: $(R3D_STREAM_TEST)

$(VRAM_STREAM_TEST): tools/vramstream.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -m68020-60 -noixemul \
		-Iinclude $< -lamiga -o $@

$(R3D_STREAM_TEST): tools/radeon3dstream.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d_protos.h
	mkdir -p $(dir $@)
	VBCC=$(VBCC_ROOT)/build PATH="$(VBCC_ROOT)/build/bin:$$PATH" \
		vc +aos68k -c99 -O=1 -Iinclude -I$(P96_DIR)/PrivateInclude \
		-I$(VBCC_INCLUDE) -I$(VBCC_NDK) $< -o $@

abi-check: $(ABI_CHECK_GCC) $(ABI_CHECK_VBCC)

r3d-tools: $(R3D_INFO_TEST) $(R3D_SESSIONS_TEST) $(R3D_PHASE1_TEST) $(R3D_FORMATS_TEST)

$(ABI_CHECK_GCC): tools/radeon3d_abi_check.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d.h
	mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -m68020-60 -mregparm=4 \
		-noixemul -Iinclude -I$(P96_DIR)/PrivateInclude -c $< -o $@

$(ABI_CHECK_VBCC): tools/radeon3d_abi_check.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d_protos.h
	mkdir -p $(dir $@)
	VBCC=$(VBCC_ROOT)/build PATH="$(VBCC_ROOT)/build/bin:$$PATH" \
		vc +aos68k -c99 -O=1 -Iinclude -I$(P96_DIR)/PrivateInclude \
		-I$(VBCC_INCLUDE) -I$(VBCC_NDK) -c $< -o $@

$(R3D_INFO_TEST): tools/radeon3dinfo.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d_protos.h
	mkdir -p $(dir $@)
	VBCC=$(VBCC_ROOT)/build PATH="$(VBCC_ROOT)/build/bin:$$PATH" \
		vc +aos68k -c99 -O=1 -Iinclude -I$(P96_DIR)/PrivateInclude \
		-I$(VBCC_INCLUDE) -I$(VBCC_NDK) $< -o $@

$(R3D_SESSIONS_TEST): tools/radeon3dsessions.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d_protos.h
	mkdir -p $(dir $@)
	VBCC=$(VBCC_ROOT)/build PATH="$(VBCC_ROOT)/build/bin:$$PATH" \
		vc +aos68k -c99 -O=1 -Iinclude -I$(P96_DIR)/PrivateInclude \
		-I$(VBCC_INCLUDE) -I$(VBCC_NDK) $< -o $@

$(R3D_PHASE1_TEST): tools/radeon3dphase1.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d_protos.h
	mkdir -p $(dir $@)
	VBCC=$(VBCC_ROOT)/build PATH="$(VBCC_ROOT)/build/bin:$$PATH" \
		vc +aos68k -c99 -O=1 -Iinclude -I$(P96_DIR)/PrivateInclude \
		-I$(VBCC_INCLUDE) -I$(VBCC_NDK) $< -o $@

$(R3D_FORMATS_TEST): tools/radeon3dformats.c include/radeon3d.h \
		include/proto/radeon3d.h include/clib/radeon3d_protos.h \
		include/inline/radeon3d_protos.h
	mkdir -p $(dir $@)
	VBCC=$(VBCC_ROOT)/build PATH="$(VBCC_ROOT)/build/bin:$$PATH" \
		vc +aos68k -c99 -O=1 -Iinclude -I$(P96_DIR)/PrivateInclude \
		-I$(VBCC_INCLUDE) -I$(VBCC_NDK) $< -o $@

$(P96_SCREEN_TEST): tools/p96screen.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -m68020-60 -noixemul \
		-Iinclude $< -lamiga -o $@

$(P96_OVERLAP_TEST): tools/p96overlap.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -m68020-60 -noixemul \
		-Iinclude $< -lamiga -o $@

$(P96_WINDOWMOVE_TEST): tools/p96windowmove.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -m68020-60 -noixemul \
		-Iinclude $< -lamiga -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $(BUILD_DIR)/$@
	$(STRIP) --strip-unneeded $(BUILD_DIR)/$@ -o $@

$(CARD_TARGET): $(CARD_OBJECTS)
	$(CC) $(CARD_LDFLAGS) $^ -lamiga -lgcc -o $(CARD_BUILD_DIR)/$@
	$(STRIP) --strip-unneeded $(CARD_BUILD_DIR)/$@ -o $@

$(BUILD_DIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(CARD_BUILD_DIR)/%.o: $(CARD_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CARD_CPPFLAGS) $(CARD_CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d) $(CARD_OBJECTS:.o=.d)

clean:
	rm -rf build build-debug build-fastwait build-debug-fastwait \
		Radeon9200.chip Radeon9200-debug.chip \
		Radeon9200-fastwait.chip Radeon9200-debug-fastwait.chip \
		Radeon9200.card Radeon9200-debug.card \
		Prometheus.card Prometheus-debug.card
