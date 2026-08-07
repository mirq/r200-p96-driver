CROSS ?= /opt/amiga/bin/m68k-amigaos-
DEBUG ?= 0
CC := $(CROSS)gcc
STRIP := $(CROSS)strip

TARGET := Radeon9200.card
BUILD_DIR := build
P96_SCREEN_TEST := $(BUILD_DIR)/p96screen

P96_DIR := Picasso96Develop
OPENPCI_DIR := OpenPci2.1-SDK290208

SOURCES := \
	src/startup.c \
	src/library.c \
	src/radeon9200.c \
	src/dma.c \
	src/radeon_accel.c \
	src/radeon_bios.c \
	src/radeon_mode.c
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

CPPFLAGS := \
	-DOPENPCI_SWAP \
	-Iinclude \
	-I$(P96_DIR)/PrivateInclude \
	-I$(OPENPCI_DIR)/Include/gcc \
	-I$(OPENPCI_DIR)/Include

ifeq ($(DEBUG),1)
CPPFLAGS += -DDEBUG
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

ifeq ($(DEBUG),1)
LDLIBS := -ldebug -lc $(LDLIBS)
endif

.PHONY: all clean tools

all: $(TARGET)

tools: $(P96_SCREEN_TEST)

$(P96_SCREEN_TEST): tools/p96screen.c
	mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -m68020-60 -noixemul \
		-Iinclude $< -lamiga -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $(BUILD_DIR)/$@
	$(STRIP) --strip-unneeded $(BUILD_DIR)/$@ -o $@

$(BUILD_DIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	rm -rf build build-debug $(TARGET)
