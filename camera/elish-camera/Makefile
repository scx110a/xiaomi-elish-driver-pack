# elish-camera build
#
#   make            - build all modules, tools and the main binary
#   make tools      - build only the test tools
#   make clean
#
# Modules are plain C11.  Each owner keeps to their own files:
#   src/capture.c, src/main.c        (Lead)
#   src/gpu_debayer.c                (gpu-debayer)
#   src/venus_enc.c                  (venus-encoder)
#   src/loopback.c                   (loopback-interface)
#   tools/debayer_test.c             (gpu-debayer)
#   tools/enc_test.c                 (venus-encoder)
#   tools/loopback_test.c            (loopback-interface)

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11
CPPFLAGS += -Iinclude -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  += -lm -lpthread -lz

EGL_CFLAGS  := $(shell pkg-config --cflags egl glesv2 libdrm gbm 2>/dev/null)
PW_CFLAGS   := $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null)
PW_LIBS     := $(shell pkg-config --libs   libpipewire-0.3 2>/dev/null)
EGL_LIBS    := $(shell pkg-config --libs   egl glesv2 libdrm gbm 2>/dev/null)

BUILD := build

MODULES := $(BUILD)/capture.o $(BUILD)/gpu_debayer.o $(BUILD)/venus_enc.o $(BUILD)/loopback.o $(BUILD)/pwcam.o
MAINOBJ := $(BUILD)/main.o

TOOLS := $(BUILD)/debayer_test $(BUILD)/enc_test $(BUILD)/loopback_test $(BUILD)/capture_test

all: $(BUILD)/elish-camera $(TOOLS) tools/mux_mp4.sh

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/elish_camera.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(EGL_CFLAGS) $(PW_CFLAGS) -c $< -o $@

$(BUILD)/elish-camera: $(MAINOBJ) $(MODULES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(EGL_LIBS) $(PW_LIBS) $(LDLIBS)

# Each tool links the object files it needs.
$(BUILD)/debayer_test: tools/debayer_test.c $(BUILD)/gpu_debayer.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(EGL_CFLAGS) $^ -o $@ $(EGL_LIBS) $(LDLIBS)

$(BUILD)/enc_test: tools/enc_test.c $(BUILD)/venus_enc.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/loopback_test: tools/loopback_test.c $(BUILD)/loopback.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/capture_test: tools/capture_test.c $(BUILD)/capture.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS)

.PHONY: all tools clean
tools: $(TOOLS)
clean:
	rm -rf $(BUILD)
