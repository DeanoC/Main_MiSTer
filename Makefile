# makefile to fail if any command in pipe is failed.
SHELL = /bin/bash -o pipefail

JOBS ?= $(shell command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 1)
MAKEFLAGS += -j $(JOBS)

# using gcc version 10.2.1
BASE    = arm-none-linux-gnueabihf

CC      := $(BASE)-gcc
CXX     := $(BASE)-g++
AR      := $(BASE)-ar
NM      := $(BASE)-nm
LD      = $(BASE)-ld
STRIP   = $(BASE)-strip

ifeq ($(V),1)
	Q :=
else
	Q := @
endif

INCLUDE	= -I./
INCLUDE	+= -I./lib/libco
INCLUDE	+= -I./lib/miniz
INCLUDE	+= -I./lib/md5
INCLUDE += -I./lib/lzma
INCLUDE += -I./lib/zstd/lib
INCLUDE += -I./lib/libchdr/include
INCLUDE += -I./lib/bluetooth
INCLUDE += -I./lib/serial_server/library

BUILDDIR = bin

PRJ = MiSTer
C_SRC =   $(wildcard *.c) \
          $(wildcard ./lib/miniz/*.c) \
          $(wildcard ./lib/md5/*.c) \
          $(wildcard ./lib/lzma/*.c) \
					$(wildcard ./lib/zstd/lib/common/*.c) \
					$(wildcard ./lib/zstd/lib/decompress/*.c) \
          $(wildcard ./lib/libchdr/*.c) \
          lib/libco/arm.c

RUNTIME_SRC = runtime/mister_runtime.cpp runtime/mister_runtime_v2.cpp runtime/mister_runtime_legacy.cpp
RUNTIME_OBJ = $(RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.o)
RUNTIME_DEP = $(RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.d)
RUNTIME_ARCHIVE = $(BUILDDIR)/libmister-runtime.a
RUNTIME_HEADERS = runtime/mister_runtime.h runtime/mister_runtime_internal.hpp
RUNTIME_C99_SMOKE_OBJECT = $(BUILDDIR)/runtime-smoke/mister_runtime_c99_smoke.o
RUNTIME_CPP14_SMOKE_OBJECT = $(BUILDDIR)/runtime-smoke/mister_runtime_cpp14_smoke.o
RUNTIME_V2_C99_SMOKE_OBJECT = $(BUILDDIR)/runtime-smoke/mister_runtime_v2_c99_smoke.o
RUNTIME_V2_CPP14_SMOKE_OBJECT = $(BUILDDIR)/runtime-smoke/mister_runtime_v2_cpp14_smoke.o
RUNTIME_C99_SMOKE = $(BUILDDIR)/runtime-smoke/mister_runtime_c99_smoke
RUNTIME_CPP14_SMOKE = $(BUILDDIR)/runtime-smoke/mister_runtime_cpp14_smoke
RUNTIME_V2_C99_SMOKE = $(BUILDDIR)/runtime-smoke/mister_runtime_v2_c99_smoke
RUNTIME_V2_CPP14_SMOKE = $(BUILDDIR)/runtime-smoke/mister_runtime_v2_cpp14_smoke
RUNTIME_PUBLIC_SYMBOLS = MisterRuntime_ABIVersion MisterRuntime_Create \
	MisterRuntime_Start MisterRuntime_Tick MisterRuntime_Load MisterRuntime_Status \
	MisterRuntime_Stop MisterRuntime_Destroy MisterRuntime_ABIVersionV2 \
	MisterRuntime_CreateV2 MisterRuntime_StartV2 MisterRuntime_LoadV2 \
	MisterRuntime_TickV2 MisterRuntime_ObserveV2 MisterRuntime_StatusV2 \
	MisterRuntime_StopV2 MisterRuntime_DestroyV2 MisterRuntime_RecoverPlatformV2

FOGCAST_RUNTIME_SRC = fogcast/runtime_main.cpp fogcast/runtime_server_linux.cpp \
	fogcast/runtime_coordinator.cpp fogcast/backend_fence_linux.cpp \
	fogcast/runtime_protocol.cpp fogcast/runtime_state.cpp \
	fogcast/runtime_store_linux.cpp fogcast/runtime_platform_unavailable.cpp
FOGCAST_RUNTIME_OBJ = $(FOGCAST_RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.o)
FOGCAST_RUNTIME_DEP = $(FOGCAST_RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.d)

CPP_SRC = $(filter-out $(RUNTIME_SRC),$(wildcard *.cpp)) \
          $(wildcard ./lib/serial_server/library/*.cpp) \
          $(wildcard ./support/*/*.cpp)

IMG =     $(wildcard *.png)

IMLIB2_LIB  = -Llib/imlib2 -lfreetype -lbz2 -lpng16 -lz -lImlib2

OBJ	= $(C_SRC:%.c=$(BUILDDIR)/%.c.o) $(CPP_SRC:%.cpp=$(BUILDDIR)/%.cpp.o) $(IMG:%.png=$(BUILDDIR)/%.png.o)
DEP	= $(C_SRC:%.c=$(BUILDDIR)/%.c.d) $(CPP_SRC:%.cpp=$(BUILDDIR)/%.cpp.d) \
	$(RUNTIME_DEP) $(FOGCAST_RUNTIME_DEP)

ifneq ($(origin VDATE),command line)
$(error VDATE must be supplied as six ASCII YYMMDD digits)
endif
override stage_a0_shell_quote = '$(subst ','"'"',$(1))'
override STAGE_A0_VDATE := $(value VDATE)
override VDATE_VALID := $(shell LC_ALL=C; export LC_ALL; VDATE=$(call stage_a0_shell_quote,$(STAGE_A0_VDATE)); export VDATE; [[ "$$VDATE" =~ ^[0-9]{6}$$ ]] && printf valid)
ifneq ($(VDATE_VALID),valid)
$(error VDATE must be supplied as six ASCII YYMMDD digits)
endif
override VDATE := $(STAGE_A0_VDATE)
DFLAGS	= $(INCLUDE) -D_7ZIP_ST -DPACKAGE_VERSION=\"1.3.3\" -DHAVE_LROUND -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_SYS_PARAM_H -DENABLE_64_BIT_WORDS=0 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -DVDATE=\"$(VDATE)\"
CFLAGS	= $(DFLAGS) -Wall -Wextra -Wno-strict-aliasing -Wno-stringop-overflow -Wno-stringop-truncation -Wno-format-truncation -Wno-psabi -Wno-restrict -c
LFLAGS	= -lc -lstdc++ -lm -lrt $(IMLIB2_LIB) -Llib/bluetooth -lbluetooth -lpthread

OUTPUT_FILTER = sed -e 's/\(.[a-zA-Z]\+\):\([0-9]\+\):\([0-9]\+\):/\1(\2,\ \3):/g'

ifneq ($(DEBUG),1)
	CFLAGS += -O3
else
	CFLAGS += -O0 -g -fomit-frame-pointer
endif

ifeq ($(PROFILING),1)
	DFLAGS += -DPROFILING
endif

$(BUILDDIR)/$(PRJ): $(OBJ) $(RUNTIME_ARCHIVE)
	$(Q)$(info $@)
	$(Q)$(CXX) -o $@ $+ $(LFLAGS)
	$(Q)cp $@ $@.elf
ifneq ($(DEBUG),1)
	$(Q)$(STRIP) $@
endif

.PHONY: clean check-runtime-archive fogcast-runtime fogcast-runtime-host-test fogcast-runtime-dep-test
clean:
	$(Q)rm -rf bin

fogcast-runtime: $(BUILDDIR)/fogcast-runtime

fogcast-runtime-host-test:
	$(Q)$(MAKE) -f tests/Makefile HOST_TEST_DIR=bin/fogcast-runtime-host-tests test-fogcast-main

fogcast-runtime-dep-test:
	$(Q)set -e; \
	dep_root="$$(mktemp -d "$${TMPDIR:-/tmp}/fogcast-runtime-deps.XXXXXX")"; \
	case "$${dep_root}" in */fogcast-runtime-deps.*) ;; *) exit 1 ;; esac; \
	trap 'rm -rf -- "$${dep_root}"' EXIT; \
	cp Makefile "$${dep_root}/"; cp -R fogcast runtime tests "$${dep_root}/"; \
	cd "$${dep_root}"; \
	$(MAKE) -s VDATE=$(VDATE) CXX=c++ CC=cc BUILDDIR=dep-bin RUNTIME_DEP= \
		dep-bin/fogcast/runtime_main.cpp.d \
		dep-bin/fogcast/runtime_server_linux.cpp.d \
		dep-bin/fogcast/runtime_coordinator.cpp.d \
		dep-bin/fogcast/runtime_platform_unavailable.cpp.d; \
	check_rebuild() { \
		object="$$1"; header="$$2"; touch "$${object}"; sleep 1; touch "$${header}"; \
		code=0; $(MAKE) -s VDATE=$(VDATE) CXX=c++ CC=cc BUILDDIR=dep-bin RUNTIME_DEP= -q "$${object}" || code=$$?; \
		test "$${code}" -eq 1 || { echo "dependency rebuild check failed: $${object} ($${code})" >&2; exit 1; }; \
	}; \
	check_rebuild dep-bin/fogcast/runtime_platform_unavailable.cpp.o fogcast/runtime_platform_factory.hpp; \
	check_rebuild dep-bin/fogcast/runtime_server_linux.cpp.o fogcast/runtime_server.hpp; \
	check_rebuild dep-bin/fogcast/runtime_coordinator.cpp.o fogcast/runtime_coordinator.hpp

$(BUILDDIR)/fogcast-runtime: $(FOGCAST_RUNTIME_OBJ) $(RUNTIME_ARCHIVE)
	$(Q)$(info $@)
	$(Q)$(CXX) -o $@ $^ -lpthread -lrt

check-runtime-archive: $(RUNTIME_ARCHIVE) $(RUNTIME_C99_SMOKE) \
	$(RUNTIME_CPP14_SMOKE) $(RUNTIME_V2_C99_SMOKE) $(RUNTIME_V2_CPP14_SMOKE)
	$(Q)expected_members='mister_runtime.cpp.o mister_runtime_v2.cpp.o mister_runtime_legacy.cpp.o'; \
	actual_members="$$($(AR) t $(RUNTIME_ARCHIVE) | tr '\n' ' ' | sed 's/ $$//')"; \
	test "$$actual_members" = "$$expected_members"
	$(Q)for symbol in $(RUNTIME_PUBLIC_SYMBOLS); do \
		count="$$($(NM) -g --defined-only $(RUNTIME_ARCHIVE) | awk -v symbol="$$symbol" '$$3 == symbol { count++ } END { print count + 0 }')"; \
		test "$$count" = 1 || { echo "runtime symbol $$symbol count $$count" >&2; exit 1; }; \
	done

$(BUILDDIR)/%.c.o: %.c
	$(Q)$(info $<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -std=gnu99 -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/%.cpp.o: %.cpp
	$(Q)$(info $<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CFLAGS) -std=gnu++14 -Wno-class-memaccess -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(RUNTIME_OBJ): $(RUNTIME_HEADERS)

$(BUILDDIR)/runtime/%.cpp.o: runtime/%.cpp
	$(Q)$(info $<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CFLAGS) -std=gnu++14 -fno-exceptions -fno-rtti -Wno-class-memaccess -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/runtime/mister_runtime_v2.cpp.o: runtime/mister_runtime_v2.cpp
	$(Q)$(info $<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CFLAGS) -std=gnu++14 -fno-rtti -Wno-class-memaccess -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(RUNTIME_ARCHIVE): $(RUNTIME_OBJ)
	$(Q)$(info $@)
	$(Q)$(AR) rcsD $@ $^

$(RUNTIME_C99_SMOKE_OBJECT): tests/mister_runtime_c99_smoke.c \
	runtime/mister_runtime.h | $(BUILDDIR)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -std=c99 -pedantic-errors -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(RUNTIME_CPP14_SMOKE_OBJECT): tests/mister_runtime_cpp14_smoke.cpp \
	runtime/mister_runtime.h | $(BUILDDIR)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CFLAGS) -std=gnu++14 -fno-exceptions -fno-rtti \
		-Wno-class-memaccess -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(RUNTIME_V2_C99_SMOKE_OBJECT): tests/mister_runtime_v2_c99_smoke.c \
	runtime/mister_runtime.h | $(BUILDDIR)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -std=c99 -pedantic-errors -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(RUNTIME_V2_CPP14_SMOKE_OBJECT): tests/mister_runtime_v2_cpp14_smoke.cpp \
	runtime/mister_runtime.h | $(BUILDDIR)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CFLAGS) -std=gnu++14 -fno-exceptions -fno-rtti \
		-Wno-class-memaccess -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(RUNTIME_C99_SMOKE): $(RUNTIME_C99_SMOKE_OBJECT) $(RUNTIME_ARCHIVE)
	$(Q)$(CXX) -o $@ $^

$(RUNTIME_CPP14_SMOKE): $(RUNTIME_CPP14_SMOKE_OBJECT) $(RUNTIME_ARCHIVE)
	$(Q)$(CXX) -o $@ $^

$(RUNTIME_V2_C99_SMOKE): $(RUNTIME_V2_C99_SMOKE_OBJECT) $(RUNTIME_ARCHIVE)
	$(Q)$(CXX) -o $@ $^

$(RUNTIME_V2_CPP14_SMOKE): $(RUNTIME_V2_CPP14_SMOKE_OBJECT) $(RUNTIME_ARCHIVE)
	$(Q)$(CXX) -o $@ $^

$(BUILDDIR)/%.png.o: %.png
	$(Q)$(info $<)
	$(Q)$(LD) -r -b binary -o $@ $< 2>&1 | $(OUTPUT_FILTER)

ifneq ($(MAKECMDGOALS), clean)
-include $(DEP)
endif
$(BUILDDIR)/%.c.d: %.c
	@mkdir -p $(dir $(BUILDDIR)/$*)
	$(Q)$(info $< >> $@)
	$(Q)$(CC) $(DFLAGS) -MM $< -MT $@ -MT $*.c.o -MF $@ 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/%.cpp.d: %.cpp
	@mkdir -p $(dir $(BUILDDIR)/$*)
	$(Q)$(info $< >> $@)
	$(Q)$(CC) $(DFLAGS) -MM $< -MT $@ -MT $*.cpp.o -MF $@ 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/runtime/%.cpp.d: runtime/%.cpp
	@mkdir -p $(dir $@)
	$(Q)$(CXX) $(DFLAGS) -MM $< -MT $@ -MT $(BUILDDIR)/runtime/$*.cpp.o \
		-MF $@ 2>&1 | $(OUTPUT_FILTER)

$(FOGCAST_RUNTIME_DEP): $(BUILDDIR)/fogcast/%.cpp.d: fogcast/%.cpp
	@mkdir -p $(dir $@)
	$(Q)$(CXX) $(DFLAGS) -std=gnu++14 -MM $< -MT $@ \
		-MT $(BUILDDIR)/fogcast/$*.cpp.o -MF $@ 2>&1 | $(OUTPUT_FILTER)

# Ensure correct time stamp
$(BUILDDIR)/main.cpp.o: $(RUNTIME_HEADERS) \
	$(filter-out $(BUILDDIR)/main.cpp.o, $(OBJ))
