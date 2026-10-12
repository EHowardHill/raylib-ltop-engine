# KallistiOS makefile for the engine. Run from build/dreamcast inside the
# dc-kos container:
#   kos-c++ compiles, romdisk packs the assets, output is ltop.elf
TARGET := ltop.elf

SRCS := $(wildcard ../../src/*.cpp)
AUTOSRCS := $(wildcard ../../src_auto/*.cpp)
OBJS := $(patsubst %.cpp,%.o,$(notdir $(SRCS))) $(patsubst %.cpp,%.o,$(notdir $(AUTOSRCS))) dreamcast_shim.o romdisk.o
vpath %.cpp ../../src ../../src_auto
vpath %.c ../../scripts/console

KOS_ROMDISK_DIR := romdisk

KOS_CFLAGS += -I$(KOS_PORTS)/raylib4dc/inst/include -I../../include -I../../include_auto -DPLATFORM_DREAMCAST
KOS_CXXFLAGS += -I$(KOS_PORTS)/raylib4dc/inst/include -I../../include -I../../include_auto -DPLATFORM_DREAMCAST

all: $(TARGET)

$(TARGET): $(OBJS)
	kos-c++ -o $(TARGET) $(OBJS) -lraylib -lGL -lm -lkosutils -lpthread

clean:
	rm -f $(OBJS) $(TARGET) romdisk.*

include $(KOS_BASE)/Makefile.rules
