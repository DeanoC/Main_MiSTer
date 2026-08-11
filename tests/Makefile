# Copyright 2026 FogCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

HOST_CC ?= cc
HOST_CXX ?= c++

HOST_FLAGS = -Wall -Wextra -Werror
HOST_C_FLAGS = -std=c99 $(HOST_FLAGS)
HOST_CXX_FLAGS = -std=c++14 $(HOST_FLAGS) -fno-exceptions -fno-rtti
HOST_TEST_DIR = bin/host-tests
RUNTIME_OBJECT = $(HOST_TEST_DIR)/mister_runtime.o
RUNTIME_V2_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2.o
RUNTIME_PRODUCTION_OBJECT = $(HOST_TEST_DIR)/mister_runtime_production.o
RUNTIME_V2_PRODUCTION_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2_production.o
C99_SMOKE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_c99_smoke.o
CPP14_SMOKE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_cpp14_smoke.o
C99_V2_SMOKE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2_c99_smoke.o
C99_V2_RAW_FIXTURE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2_raw_fixture.o
C99_V2_PRODUCTION_SANITIZE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2_production_sanitize.o
CPP14_V2_SMOKE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2_cpp14_smoke.o
LIFECYCLE_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_test.o
LEGACY_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_legacy_test.o
V2_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_v2_test.o
LEGACY_OBJECT = $(HOST_TEST_DIR)/mister_runtime_legacy.o
WRAPPER_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_wrapper_test.o
WRAPPER_FIXTURE_OBJECT = $(HOST_TEST_DIR)/mister_runtime_wrapper_fixture.o
WRAPPER_MAIN_OBJECT = $(HOST_TEST_DIR)/mister_runtime_wrapper_main.o
SCHEDULER_TEST_OBJECT = $(HOST_TEST_DIR)/scheduler_step_test.o
SCHEDULER_FAILURE_TEST_OBJECT = $(HOST_TEST_DIR)/scheduler_init_failure_test.o
SCHEDULER_OBJECT = $(HOST_TEST_DIR)/scheduler.o
HOST_LIBCO_OBJECT = $(HOST_TEST_DIR)/host_libco.o
HOST_TEST_INCLUDE = $(if $(filter Darwin,$(shell uname -s)),-Itests/include)
HOST_EXIT_COMPAT = $(if $(filter Darwin,$(shell uname -s)),,-D_Exit=_exit)
SCHEDULER_HOST_CXX_FLAGS = $(HOST_CXX_FLAGS) $(HOST_TEST_INCLUDE)
LEGACY_HOST_CXX_FLAGS = $(HOST_CXX_FLAGS) -D_GNU_SOURCE $(HOST_TEST_INCLUDE)

# libco's portable ucontext implementation uses C++ thread-local storage.
# macOS also requires the X/Open feature declaration for ucontext symbols.
HOST_LIBCO_FLAGS = $(HOST_CXX_FLAGS) -x c++ -D_XOPEN_SOURCE \
	-Wno-deprecated-declarations -Wno-cpp

.PHONY: clean test-runtime-smoke test-runtime test-scheduler test-runtime-legacy test-runtime-v2 test-runtime-v2-sanitize test-runtime-v2-production-sanitize

test-runtime-smoke: $(HOST_TEST_DIR)/mister_runtime_c99_smoke \
	$(HOST_TEST_DIR)/mister_runtime_cpp14_smoke
	$(HOST_TEST_DIR)/mister_runtime_c99_smoke
	$(HOST_TEST_DIR)/mister_runtime_cpp14_smoke

test-runtime: $(HOST_TEST_DIR)/mister_runtime_test
	$(HOST_TEST_DIR)/mister_runtime_test

test-scheduler: $(HOST_TEST_DIR)/scheduler_step_test \
	$(HOST_TEST_DIR)/scheduler_init_failure_test
	$(HOST_TEST_DIR)/scheduler_step_test
	$(HOST_TEST_DIR)/scheduler_init_failure_test

test-runtime-legacy: $(HOST_TEST_DIR)/mister_runtime_legacy_test \
	$(HOST_TEST_DIR)/mister_runtime_wrapper_fixture \
	$(HOST_TEST_DIR)/mister_runtime_wrapper_test
	$(HOST_TEST_DIR)/mister_runtime_legacy_test
	$(HOST_TEST_DIR)/mister_runtime_wrapper_test \
		$(HOST_TEST_DIR)/mister_runtime_wrapper_fixture

test-runtime-v2: $(HOST_TEST_DIR)/mister_runtime_v2_test \
	$(HOST_TEST_DIR)/mister_runtime_v2_c99_smoke \
	$(HOST_TEST_DIR)/mister_runtime_v2_cpp14_smoke
	$(HOST_TEST_DIR)/mister_runtime_v2_test
	$(HOST_TEST_DIR)/mister_runtime_v2_c99_smoke
	$(HOST_TEST_DIR)/mister_runtime_v2_cpp14_smoke

test-runtime-v2-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-runtime-v2

test-runtime-v2-production-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/production-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
		$(HOST_TEST_DIR)/production-sanitize/mister_runtime_v2_production_sanitize
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(HOST_TEST_DIR)/production-sanitize/mister_runtime_v2_production_sanitize

$(HOST_TEST_DIR):
	mkdir -p $@

$(RUNTIME_OBJECT): runtime/mister_runtime.cpp runtime/mister_runtime.h \
	runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(RUNTIME_V2_OBJECT): runtime/mister_runtime_v2.cpp runtime/mister_runtime.h \
	runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) -std=c++14 $(HOST_FLAGS) -fno-rtti -DMISTER_RUNTIME_TESTING -I. -c $< -o $@

$(RUNTIME_PRODUCTION_OBJECT): runtime/mister_runtime.cpp runtime/mister_runtime.h \
	runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(RUNTIME_V2_PRODUCTION_OBJECT): runtime/mister_runtime_v2.cpp runtime/mister_runtime.h \
	runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) -std=c++14 $(HOST_FLAGS) -fno-rtti -I. -c $< -o $@

$(C99_SMOKE_OBJECT): tests/mister_runtime_c99_smoke.c \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_C_FLAGS) -I. -c $< -o $@

$(CPP14_SMOKE_OBJECT): tests/mister_runtime_cpp14_smoke.cpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(C99_V2_SMOKE_OBJECT): tests/mister_runtime_v2_c99_smoke.c \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_C_FLAGS) -I. -c $< -o $@

$(C99_V2_RAW_FIXTURE_OBJECT): tests/mister_runtime_v2_c99_smoke.c \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_C_FLAGS) -DMISTER_RUNTIME_RAW_RESULT_FIXTURE -I. -c $< -o $@

$(C99_V2_PRODUCTION_SANITIZE_OBJECT): tests/mister_runtime_v2_c99_smoke.c \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CC) $(HOST_C_FLAGS) -DMISTER_RUNTIME_PRODUCTION_SANITIZE_FIXTURE -I. -c $< -o $@

$(CPP14_V2_SMOKE_OBJECT): tests/mister_runtime_v2_cpp14_smoke.cpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(LIFECYCLE_TEST_OBJECT): tests/mister_runtime_test.cpp \
	runtime/mister_runtime.h runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(LEGACY_TEST_OBJECT): tests/mister_runtime_legacy_test.cpp \
	runtime/mister_runtime_internal.hpp scheduler.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(LEGACY_HOST_CXX_FLAGS) -I. -c $< -o $@

$(V2_TEST_OBJECT): tests/mister_runtime_v2_test.cpp runtime/mister_runtime.h \
	runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) -std=c++14 $(HOST_FLAGS) -fno-rtti -DMISTER_RUNTIME_TESTING -I. -c $< -o $@

$(LEGACY_OBJECT): runtime/mister_runtime_legacy.cpp runtime/mister_runtime_internal.hpp \
	scheduler.h offload.h fpga_io.h file_io.h user_io.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(LEGACY_HOST_CXX_FLAGS) -I. -c $< -o $@

$(WRAPPER_TEST_OBJECT): tests/mister_runtime_wrapper_test.cpp \
	runtime/mister_runtime.h runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(WRAPPER_FIXTURE_OBJECT): tests/mister_runtime_wrapper_test.cpp \
	runtime/mister_runtime.h runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_RUNTIME_WRAPPER_FIXTURE -I. -c $< -o $@

$(WRAPPER_MAIN_OBJECT): main.cpp runtime/mister_runtime.h \
	runtime/mister_runtime_internal.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DVDATE=\"260810\" -I. -c $< -o $@

$(SCHEDULER_TEST_OBJECT): tests/scheduler_step_test.cpp scheduler.h \
	lib/libco/libco.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(SCHEDULER_HOST_CXX_FLAGS) -I. -Ilib/libco -c $< -o $@

$(SCHEDULER_FAILURE_TEST_OBJECT): tests/scheduler_init_failure_test.cpp \
	scheduler.h lib/libco/libco.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(SCHEDULER_HOST_CXX_FLAGS) $(HOST_EXIT_COMPAT) -I. -Ilib/libco -c $< -o $@

$(SCHEDULER_OBJECT): scheduler.cpp scheduler.h lib/libco/libco.h menu.h \
	user_io.h input.h frame_timer.h fpga_io.h osd.h profiling.h video.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(SCHEDULER_HOST_CXX_FLAGS) -I. -Ilib/libco -c $< -o $@

$(HOST_LIBCO_OBJECT): lib/libco/ucontext.c lib/libco/libco.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_LIBCO_FLAGS) -Ilib/libco -c $< -o $@

$(HOST_TEST_DIR)/mister_runtime_c99_smoke: $(C99_SMOKE_OBJECT) $(RUNTIME_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_cpp14_smoke: $(CPP14_SMOKE_OBJECT) $(RUNTIME_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_v2_c99_smoke: $(C99_V2_SMOKE_OBJECT) \
	$(RUNTIME_OBJECT) $(RUNTIME_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_v2_cpp14_smoke: $(CPP14_V2_SMOKE_OBJECT) \
	$(RUNTIME_OBJECT) $(RUNTIME_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_test: $(LIFECYCLE_TEST_OBJECT) $(RUNTIME_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_legacy_test: $(LEGACY_TEST_OBJECT) $(LEGACY_OBJECT) \
	$(RUNTIME_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_v2_test: $(V2_TEST_OBJECT) $(RUNTIME_OBJECT) \
	$(RUNTIME_V2_OBJECT) $(C99_V2_RAW_FIXTURE_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_v2_production_sanitize: $(C99_V2_PRODUCTION_SANITIZE_OBJECT) \
	$(RUNTIME_PRODUCTION_OBJECT) $(RUNTIME_V2_PRODUCTION_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_wrapper_fixture: $(WRAPPER_FIXTURE_OBJECT) \
	$(WRAPPER_MAIN_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_wrapper_test: $(WRAPPER_TEST_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/scheduler_step_test: $(SCHEDULER_TEST_OBJECT) $(SCHEDULER_OBJECT) \
	$(HOST_LIBCO_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/scheduler_init_failure_test: $(SCHEDULER_FAILURE_TEST_OBJECT) \
	$(SCHEDULER_OBJECT)
	$(HOST_CXX) $^ -o $@

clean:
	rm -rf $(HOST_TEST_DIR)
