# Copyright 2026 FogCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

SHELL := /bin/bash

ROOT ?= ../..
BUILD_DIR ?= $(ROOT)/bin/native-personality
CXX ?= c++
AR ?= ar

SOURCES := \
	runtime/mister_runtime.cpp \
	runtime/mister_runtime_v2.cpp \
	runtime/native/hardware_broker.cpp \
	runtime/native/linux/native_audio_adapter.cpp \
	runtime/native/linux/native_av_io_adapter.cpp \
	runtime/native/linux/native_content_adapter.cpp \
	runtime/native/linux/native_core_artifact_adapter.cpp \
	runtime/native/linux/native_core_protocol_io_adapter.cpp \
	runtime/native/linux/native_fpga_programmer.cpp \
	runtime/native/linux/native_input_adapter.cpp \
	runtime/native/linux/native_linux_v2_context.cpp \
	runtime/native/linux/native_mmio_adapter.cpp \
	runtime/native/linux/native_offload_adapter.cpp \
	runtime/native/linux/native_save_adapter.cpp \
	runtime/native/linux/native_scheduler_adapter.cpp \
	runtime/native/linux/native_video_adapter.cpp \
	runtime/native/native_containment.cpp \
	runtime/native/native_core_profile.cpp \
	runtime/native/native_core_protocol.cpp \
	runtime/native/native_input.cpp \
	runtime/native/native_lifecycle.cpp \
	runtime/native/native_peripheral_session.cpp \
	runtime/native/native_recovery.cpp \
	runtime/native/native_save_key.cpp \
	runtime/native/native_snes_content.cpp \
	runtime/native/native_spi_bus.cpp

OBJECTS := $(addprefix $(BUILD_DIR)/objects/,$(SOURCES:.cpp=.o))
DEPENDENCIES := $(OBJECTS:.o=.d)
ARCHIVE := $(BUILD_DIR)/libfogcast-native-personality.a
RELOCATABLE := $(BUILD_DIR)/fogcast-native-personality.o
SOURCE_MANIFEST := $(BUILD_DIR)/native-personality.sources
OBJECT_MANIFEST := $(BUILD_DIR)/native-personality.objects
LINK_MAP := $(BUILD_DIR)/native-personality.map
DEPENDENCY_MANIFEST := $(BUILD_DIR)/native-personality.dependencies

CPPFLAGS := -I$(ROOT)
CXXFLAGS := -std=c++14 -Wall -Wextra -Werror -fno-rtti -pthread -MMD -MP
EXCEPTION_FLAGS := -fno-exceptions

ifeq ($(shell uname -s),Darwin)
LINK_MAP_FLAG = -Wl,-map,$(LINK_MAP)
ARFLAGS := rcs
else
LINK_MAP_FLAG = -Wl,-Map,$(LINK_MAP)
ARFLAGS := rcsD
endif

.PHONY: all clean

all: $(ARCHIVE) $(RELOCATABLE) $(SOURCE_MANIFEST) $(OBJECT_MANIFEST) \
	$(DEPENDENCY_MANIFEST)

$(BUILD_DIR)/objects/runtime/mister_runtime_v2.o: EXCEPTION_FLAGS :=

$(BUILD_DIR)/objects/%.o: $(ROOT)/%.cpp
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(EXCEPTION_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/objects/%.d: $(BUILD_DIR)/objects/%.o
	@test -f "$@"

$(ARCHIVE): $(OBJECTS)
	@mkdir -p "$(dir $@)"
	ZERO_AR_DATE=1 $(AR) $(ARFLAGS) "$@" $(OBJECTS)

$(RELOCATABLE): $(OBJECTS)
	@mkdir -p "$(dir $@)"
	$(CXX) -r $(LINK_MAP_FLAG) -o "$@" $(OBJECTS)

$(SOURCE_MANIFEST): $(ROOT)/runtime/native/Makefile
	@mkdir -p "$(dir $@)"
	@printf '%s\n' $(SOURCES) >"$@.tmp"
	@mv "$@.tmp" "$@"

$(OBJECT_MANIFEST): $(ROOT)/runtime/native/Makefile
	@mkdir -p "$(dir $@)"
	@printf '%s\n' $(patsubst $(BUILD_DIR)/%,%,$(OBJECTS)) >"$@.tmp"
	@mv "$@.tmp" "$@"

$(DEPENDENCY_MANIFEST): $(DEPENDENCIES)
	@mkdir -p "$(dir $@)"
	@LC_ALL=C sort $(DEPENDENCIES) >"$@.tmp"
	@mv "$@.tmp" "$@"

clean:
	rm -rf -- "$(BUILD_DIR)"

-include $(DEPENDENCIES)
