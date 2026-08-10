# makefile to fail if any command in pipe is failed.
SHELL = /bin/bash -o pipefail

JOBS ?= $(shell nproc)
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

RUNTIME_SRC = runtime/mister_runtime.cpp runtime/mister_runtime_legacy.cpp
RUNTIME_OBJ = $(RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.o)
RUNTIME_DEP = $(RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.d)
RUNTIME_ARCHIVE = $(BUILDDIR)/libmister-runtime.a
RUNTIME_HEADERS = runtime/mister_runtime.h runtime/mister_runtime_internal.hpp
RUNTIME_C99_SMOKE_OBJECT = $(BUILDDIR)/runtime-smoke/mister_runtime_c99_smoke.o
RUNTIME_CPP14_SMOKE_OBJECT = $(BUILDDIR)/runtime-smoke/mister_runtime_cpp14_smoke.o
RUNTIME_C99_SMOKE = $(BUILDDIR)/runtime-smoke/mister_runtime_c99_smoke
RUNTIME_CPP14_SMOKE = $(BUILDDIR)/runtime-smoke/mister_runtime_cpp14_smoke
RUNTIME_PUBLIC_SYMBOLS = MisterRuntime_ABIVersion MisterRuntime_Create \
	MisterRuntime_Start MisterRuntime_Tick MisterRuntime_Load MisterRuntime_Status \
	MisterRuntime_Stop MisterRuntime_Destroy

CPP_SRC = $(filter-out $(RUNTIME_SRC),$(wildcard *.cpp)) \
          $(wildcard ./lib/serial_server/library/*.cpp) \
          $(wildcard ./support/*/*.cpp)

IMG =     $(wildcard *.png)

IMLIB2_LIB  = -Llib/imlib2 -lfreetype -lbz2 -lpng16 -lz -lImlib2

OBJ	= $(C_SRC:%.c=$(BUILDDIR)/%.c.o) $(CPP_SRC:%.cpp=$(BUILDDIR)/%.cpp.o) $(IMG:%.png=$(BUILDDIR)/%.png.o)
DEP	= $(C_SRC:%.c=$(BUILDDIR)/%.c.d) $(CPP_SRC:%.cpp=$(BUILDDIR)/%.cpp.d) \
	$(RUNTIME_DEP)

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

.PHONY: clean check-runtime-archive
clean:
	$(Q)rm -rf bin

check-runtime-archive: $(RUNTIME_ARCHIVE) $(RUNTIME_C99_SMOKE) \
	$(RUNTIME_CPP14_SMOKE)
	$(Q)expected_members='mister_runtime.cpp.o mister_runtime_legacy.cpp.o'; \
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

$(RUNTIME_C99_SMOKE): $(RUNTIME_C99_SMOKE_OBJECT) $(RUNTIME_ARCHIVE)
	$(Q)$(CXX) -o $@ $^

$(RUNTIME_CPP14_SMOKE): $(RUNTIME_CPP14_SMOKE_OBJECT) $(RUNTIME_ARCHIVE)
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

# Ensure correct time stamp
$(BUILDDIR)/main.cpp.o: $(RUNTIME_HEADERS) \
	$(filter-out $(BUILDDIR)/main.cpp.o, $(OBJ))
