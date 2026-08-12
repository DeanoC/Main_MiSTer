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
FOGCAST_PROTOCOL_TEST_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_protocol_test.o
FOGCAST_STATE_TEST_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_state_test.o
FOGCAST_PROTOCOL_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_protocol.o
FOGCAST_STATE_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_state.o
FOGCAST_STORE_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_store_linux.o
FOGCAST_COORDINATOR_TEST_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_coordinator_test.o
FOGCAST_FENCE_TEST_OBJECT = $(HOST_TEST_DIR)/fogcast_backend_fence_test.o
FOGCAST_COORDINATOR_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_coordinator.o
FOGCAST_FENCE_OBJECT = $(HOST_TEST_DIR)/fogcast_backend_fence_linux.o
FOGCAST_SERVER_TEST_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_server_test.o
FOGCAST_SERVER_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_server_linux.o
FOGCAST_MAIN_TEST_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_main_test.o
FOGCAST_MAIN_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_main.o
FOGCAST_PLATFORM_FAKE_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_platform_fake.o
FOGCAST_PLATFORM_UNAVAILABLE_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_platform_unavailable.o
RUNTIME_LINUX_V2_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_linux_v2_test.o
RUNTIME_RESOURCE_TEARDOWN_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_resource_teardown_test.o
RUNTIME_LINUX_V2_OBJECT = $(HOST_TEST_DIR)/mister_runtime_linux_v2.o
FOGCAST_STORAGE_PROBE_OBJECT = $(HOST_TEST_DIR)/fogcast_runtime_storage_probe.o
HOST_TEST_INCLUDE = $(if $(filter Darwin,$(shell uname -s)),-Itests/include)
HOST_EXIT_COMPAT = $(if $(filter Darwin,$(shell uname -s)),,-D_Exit=_exit)
SCHEDULER_HOST_CXX_FLAGS = $(HOST_CXX_FLAGS) $(HOST_TEST_INCLUDE)
LEGACY_HOST_CXX_FLAGS = $(HOST_CXX_FLAGS) -D_GNU_SOURCE $(HOST_TEST_INCLUDE)

# libco's portable ucontext implementation uses C++ thread-local storage.
# macOS also requires the X/Open feature declaration for ucontext symbols.
HOST_LIBCO_FLAGS = $(HOST_CXX_FLAGS) -x c++ -D_XOPEN_SOURCE \
	-Wno-deprecated-declarations -Wno-cpp

.PHONY: clean stage-c0-software-test test-runtime-smoke test-runtime test-scheduler test-runtime-legacy test-runtime-v2 test-runtime-v2-sanitize test-runtime-v2-production-sanitize test-runtime-linux-v2 test-runtime-resource-teardown test-runtime-linux-v2-sanitize test-runtime-resource-teardown-sanitize fogcast-runtime-storage-probe test-fogcast-protocol test-fogcast-state test-fogcast-protocol-sanitize test-fogcast-state-sanitize test-fogcast-coordinator test-fogcast-fence test-fogcast-coordinator-sanitize test-fogcast-server test-fogcast-server-sanitize test-fogcast-main test-fogcast-main-sanitize

stage-c0-software-test: test-runtime-v2 test-fogcast-protocol test-fogcast-state \
	test-fogcast-coordinator test-fogcast-fence test-fogcast-server \
	test-runtime-linux-v2 test-runtime-resource-teardown \
	test-fogcast-protocol-sanitize test-fogcast-state-sanitize \
	test-fogcast-coordinator-sanitize test-fogcast-server-sanitize \
	test-runtime-linux-v2-sanitize test-runtime-resource-teardown-sanitize \
	fogcast-runtime-storage-probe $(HOST_TEST_DIR)/fogcast-runtime

test-fogcast-protocol: $(HOST_TEST_DIR)/fogcast_runtime_protocol_test
	$(HOST_TEST_DIR)/fogcast_runtime_protocol_test

test-fogcast-state: $(HOST_TEST_DIR)/fogcast_runtime_state_test
	$(HOST_TEST_DIR)/fogcast_runtime_state_test

test-fogcast-protocol-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/protocol-sanitize HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-fogcast-protocol

test-fogcast-state-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/state-sanitize HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-fogcast-state

test-fogcast-coordinator: $(HOST_TEST_DIR)/fogcast_runtime_coordinator_test
	$(HOST_TEST_DIR)/fogcast_runtime_coordinator_test

test-fogcast-fence: $(HOST_TEST_DIR)/fogcast_backend_fence_test
	$(HOST_TEST_DIR)/fogcast_backend_fence_test

test-fogcast-coordinator-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/coordinator-sanitize HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-fogcast-coordinator

test-fogcast-server: $(HOST_TEST_DIR)/fogcast_runtime_server_test
	perl -e 'alarm 20; exec @ARGV' $(HOST_TEST_DIR)/fogcast_runtime_server_test

test-fogcast-server-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/server-sanitize HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-fogcast-server

test-fogcast-main: $(HOST_TEST_DIR)/fogcast_runtime_main_test \
	$(HOST_TEST_DIR)/fogcast-runtime-host-test $(HOST_TEST_DIR)/fogcast-runtime-unavailable-test
	perl -e 'alarm 90; exec @ARGV' $(HOST_TEST_DIR)/fogcast_runtime_main_test \
		$(abspath $(HOST_TEST_DIR)/fogcast-runtime-host-test) \
		$(abspath $(HOST_TEST_DIR)/fogcast-runtime-unavailable-test)

test-fogcast-main-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/main-sanitize HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-fogcast-main

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

test-runtime-linux-v2: $(HOST_TEST_DIR)/mister_runtime_linux_v2_test \
	$(HOST_TEST_DIR)/fogcast-runtime-storage-probe
	$(HOST_TEST_DIR)/mister_runtime_linux_v2_test \
		$(abspath $(HOST_TEST_DIR)/fogcast-runtime-storage-probe)

test-runtime-resource-teardown: $(HOST_TEST_DIR)/mister_runtime_resource_teardown_test
	$(HOST_TEST_DIR)/mister_runtime_resource_teardown_test

test-runtime-linux-v2-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/linux-v2-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-runtime-linux-v2

test-runtime-resource-teardown-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/resource-teardown-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-runtime-resource-teardown

fogcast-runtime-storage-probe: $(HOST_TEST_DIR)/fogcast-runtime-storage-probe

$(HOST_TEST_DIR):
	mkdir -p $@

$(FOGCAST_PROTOCOL_TEST_OBJECT): tests/fogcast_runtime_protocol_test.cpp fogcast/runtime_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_STATE_TEST_OBJECT): tests/fogcast_runtime_state_test.cpp fogcast/runtime_state.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_PROTOCOL_OBJECT): fogcast/runtime_protocol.cpp fogcast/runtime_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_STATE_OBJECT): fogcast/runtime_state.cpp fogcast/runtime_state.hpp fogcast/runtime_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_STORE_OBJECT): fogcast/runtime_store_linux.cpp fogcast/runtime_store.hpp fogcast/runtime_state.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_COORDINATOR_TEST_OBJECT): tests/fogcast_runtime_coordinator_test.cpp fogcast/runtime_coordinator.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_FENCE_TEST_OBJECT): tests/fogcast_backend_fence_test.cpp fogcast/backend_fence.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_SERVER_TEST_OBJECT): tests/fogcast_runtime_server_test.cpp fogcast/runtime_server.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_COORDINATOR_OBJECT): fogcast/runtime_coordinator.cpp fogcast/runtime_coordinator.hpp fogcast/backend_fence.hpp fogcast/runtime_store.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_FENCE_OBJECT): fogcast/backend_fence_linux.cpp fogcast/backend_fence.hpp fogcast/runtime_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_SERVER_OBJECT): fogcast/runtime_server_linux.cpp fogcast/runtime_server.hpp fogcast/runtime_coordinator.hpp fogcast/runtime_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_MAIN_TEST_OBJECT): tests/fogcast_runtime_main_test.cpp fogcast/backend_fence.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_MAIN_OBJECT): fogcast/runtime_main.cpp fogcast/runtime_platform_factory.hpp fogcast/runtime_server.hpp fogcast/runtime_coordinator.hpp fogcast/backend_fence.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DFOGCAST_RUNTIME_HOST_TEST -I. -c $< -o $@

$(FOGCAST_PLATFORM_FAKE_OBJECT): fogcast/runtime_platform_fake.cpp fogcast/runtime_platform_factory.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DFOGCAST_RUNTIME_HOST_TEST -I. -c $< -o $@

$(FOGCAST_PLATFORM_UNAVAILABLE_OBJECT): fogcast/runtime_platform_unavailable.cpp fogcast/runtime_platform_factory.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(RUNTIME_LINUX_V2_TEST_OBJECT): tests/mister_runtime_linux_v2_test.cpp runtime/mister_runtime_linux_v2.hpp runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(RUNTIME_RESOURCE_TEARDOWN_TEST_OBJECT): tests/mister_runtime_resource_teardown_test.cpp runtime/mister_runtime_linux_v2.hpp runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(RUNTIME_LINUX_V2_OBJECT): runtime/mister_runtime_linux_v2.cpp runtime/mister_runtime_linux_v2.hpp runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(FOGCAST_STORAGE_PROBE_OBJECT): tests/fogcast_runtime_storage_probe.cpp runtime/mister_runtime_linux_v2.hpp runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(HOST_TEST_DIR)/fogcast_runtime_coordinator_test: $(FOGCAST_COORDINATOR_TEST_OBJECT) $(FOGCAST_COORDINATOR_OBJECT) $(FOGCAST_FENCE_OBJECT) $(FOGCAST_PROTOCOL_OBJECT) $(FOGCAST_STATE_OBJECT) $(FOGCAST_STORE_OBJECT) $(RUNTIME_OBJECT) $(RUNTIME_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast_backend_fence_test: $(FOGCAST_FENCE_TEST_OBJECT) $(FOGCAST_FENCE_OBJECT) $(FOGCAST_PROTOCOL_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast_runtime_protocol_test: $(FOGCAST_PROTOCOL_TEST_OBJECT) $(FOGCAST_PROTOCOL_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast_runtime_state_test: $(FOGCAST_STATE_TEST_OBJECT) $(FOGCAST_STATE_OBJECT) $(FOGCAST_PROTOCOL_OBJECT) $(FOGCAST_STORE_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast_runtime_server_test: $(FOGCAST_SERVER_TEST_OBJECT) $(FOGCAST_SERVER_OBJECT) $(FOGCAST_COORDINATOR_OBJECT) $(FOGCAST_FENCE_OBJECT) $(FOGCAST_PROTOCOL_OBJECT) $(FOGCAST_STATE_OBJECT) $(FOGCAST_STORE_OBJECT) $(RUNTIME_OBJECT) $(RUNTIME_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

FOGCAST_MAIN_COMMON_OBJECTS = $(FOGCAST_MAIN_OBJECT) $(FOGCAST_SERVER_OBJECT) \
	$(FOGCAST_COORDINATOR_OBJECT) $(FOGCAST_FENCE_OBJECT) $(FOGCAST_PROTOCOL_OBJECT) \
	$(FOGCAST_STATE_OBJECT) $(FOGCAST_STORE_OBJECT) $(RUNTIME_OBJECT) $(RUNTIME_V2_OBJECT)

$(HOST_TEST_DIR)/fogcast-runtime-host-test: $(FOGCAST_MAIN_COMMON_OBJECTS) $(FOGCAST_PLATFORM_FAKE_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast-runtime-unavailable-test: $(FOGCAST_MAIN_COMMON_OBJECTS) $(FOGCAST_PLATFORM_UNAVAILABLE_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast-runtime: $(FOGCAST_MAIN_COMMON_OBJECTS) $(FOGCAST_PLATFORM_UNAVAILABLE_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast_runtime_main_test: $(FOGCAST_MAIN_TEST_OBJECT) $(FOGCAST_FENCE_OBJECT) $(FOGCAST_PROTOCOL_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

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

$(HOST_TEST_DIR)/mister_runtime_linux_v2_test: $(RUNTIME_LINUX_V2_TEST_OBJECT) $(RUNTIME_LINUX_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_resource_teardown_test: $(RUNTIME_RESOURCE_TEARDOWN_TEST_OBJECT) $(RUNTIME_LINUX_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/fogcast-runtime-storage-probe: $(FOGCAST_STORAGE_PROBE_OBJECT) $(RUNTIME_LINUX_V2_OBJECT)
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
