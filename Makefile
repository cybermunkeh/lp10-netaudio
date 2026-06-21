CC ?= gcc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS ?= -lasound

BUILD_DIR := build
TARGET := $(BUILD_DIR)/lp10-netaudio
SOURCE := src/lp10_netaudio.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
