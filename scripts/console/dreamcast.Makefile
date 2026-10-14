# KallistiOS makefile for the engine. Run from build/dreamcast inside the
# dc-kos container:
#   kos-c++ compiles; output is ltop.elf (assets live on the disc's data
#   track, added by mkdcdisc in build-dreamcast.sh — read at /cd at runtime)
TARGET := ltop.elf

SRCS := $(wildcard ../../src/*.cpp)
AUTOSRCS := $(wildcard ../../src_auto/*.cpp)
# Game-local C++ (resources/code/*.cpp) compiles into the binary with the
# engine — the layer-4 seam (see NOTES.md "Custom behaviors").
GAMESRCS := $(wildcard ../../resources/code/*.cpp)
OBJS := $(patsubst %.cpp,%.o,$(notdir $(SRCS))) $(patsubst %.cpp,%.o,$(notdir $(AUTOSRCS))) $(patsubst %.cpp,%.o,$(notdir $(GAMESRCS))) dreamcast_shim.o
vpath %.cpp ../../src ../../src_auto ../../resources/code
vpath %.c ../../scripts/console

KOS_CFLAGS += -I$(KOS_PORTS)/raylib4dc/inst/include -I../../include -I../../include_auto -DPLATFORM_DREAMCAST
KOS_CXXFLAGS += -I$(KOS_PORTS)/raylib4dc/inst/include -I../../include -I../../include_auto -DPLATFORM_DREAMCAST

all: $(TARGET)

$(TARGET): $(OBJS)
	kos-c++ -o $(TARGET) $(OBJS) -lraylib -lGL -lm -lkosutils -lpthread

clean:
	rm -f $(OBJS) $(TARGET)

include $(KOS_BASE)/Makefile.rules
