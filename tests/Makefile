# Copyright 2026 FogCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

HOST_CC ?= cc
HOST_CXX ?= c++

HOST_FLAGS = -Wall -Wextra -Werror
HOST_C_FLAGS = -std=c99 $(HOST_FLAGS)
HOST_CXX_FLAGS = -std=c++14 $(HOST_FLAGS) -fno-exceptions -fno-rtti -DMISTER_NATIVE_PROFILE_TESTING
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
RUNTIME_OFFLOAD_LIFECYCLE_TEST_OBJECT = $(HOST_TEST_DIR)/mister_runtime_offload_lifecycle_test.o
OFFLOAD_TEST_OBJECT = $(HOST_TEST_DIR)/offload_test.o
NATIVE_HARDWARE_BROKER_TEST_OBJECT = $(HOST_TEST_DIR)/native_hardware_broker_test.o
NATIVE_HARDWARE_BROKER_OBJECT = $(HOST_TEST_DIR)/hardware_broker.o
NATIVE_PERIPHERAL_SESSION_OBJECT = $(HOST_TEST_DIR)/native_peripheral_session.o
NATIVE_SPI_BUS_TEST_OBJECT = $(HOST_TEST_DIR)/native_spi_bus_test.o
NATIVE_SPI_BUS_OBJECT = $(HOST_TEST_DIR)/native_spi_bus.o
NATIVE_SPI_BUS_TEST_IMPLEMENTATION_OBJECT = \
	$(HOST_TEST_DIR)/native_spi_bus_test_implementation.o
NATIVE_CORE_PROFILE_TEST_OBJECT = $(HOST_TEST_DIR)/native_core_profile_test.o
NATIVE_CORE_PROFILE_OBJECT = $(HOST_TEST_DIR)/native_core_profile.o
NATIVE_SNES_CONTENT_TEST_OBJECT = $(HOST_TEST_DIR)/native_snes_content_test.o
NATIVE_SNES_CONTENT_OBJECT = $(HOST_TEST_DIR)/native_snes_content.o
NATIVE_CORE_PROTOCOL_TEST_OBJECT = $(HOST_TEST_DIR)/native_core_protocol_test.o
NATIVE_CORE_PROTOCOL_OBJECT = $(HOST_TEST_DIR)/native_core_protocol.o
NATIVE_CORE_PROTOCOL_REACHABILITY_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_core_protocol_reachability_test.o
NATIVE_INPUT_TEST_OBJECT = $(HOST_TEST_DIR)/native_input_test.o
NATIVE_INPUT_OBJECT = $(HOST_TEST_DIR)/native_input.o
NATIVE_LIFECYCLE_TEST_OBJECT = $(HOST_TEST_DIR)/native_lifecycle_test.o
NATIVE_LIFECYCLE_OBJECT = $(HOST_TEST_DIR)/native_lifecycle.o
NATIVE_CONTAINMENT_TEST_OBJECT = $(HOST_TEST_DIR)/native_containment_test.o
NATIVE_CONTAINMENT_OBJECT = $(HOST_TEST_DIR)/native_containment.o
NATIVE_RECOVERY_TEST_OBJECT = $(HOST_TEST_DIR)/native_recovery_test.o
NATIVE_RECOVERY_OBJECT = $(HOST_TEST_DIR)/native_recovery.o
NATIVE_AUDIO_VIDEO_ADAPTER_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_audio_video_adapter_test.o
NATIVE_AUDIO_VIDEO_ADAPTER_OBJECTS = \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(HOST_TEST_DIR)/native_av_io_adapter.o \
	$(HOST_TEST_DIR)/native_audio_adapter.o \
	$(HOST_TEST_DIR)/native_video_adapter.o
NATIVE_SAVE_ADAPTER_TEST_OBJECT = $(HOST_TEST_DIR)/native_save_adapter_test.o
NATIVE_SAVE_ADAPTER_OBJECTS = \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(HOST_TEST_DIR)/native_save_key.o \
	$(HOST_TEST_DIR)/native_save_adapter.o
NATIVE_LINUX_ARTIFACT_PROGRAMMER_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_linux_artifact_programmer_test.o
NATIVE_LINUX_ARTIFACT_PROGRAMMER_OBJECTS = \
	$(HOST_TEST_DIR)/native_core_artifact_adapter.o \
	$(HOST_TEST_DIR)/native_content_adapter.o \
	$(HOST_TEST_DIR)/native_fpga_programmer.o
NATIVE_LINUX_MMIO_CONTAINMENT_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_linux_mmio_containment_test.o
NATIVE_LINUX_MMIO_CONTAINMENT_OBJECT = \
	$(HOST_TEST_DIR)/native_mmio_adapter.o
NATIVE_LINUX_MMIO_PRODUCTION_OBJECT = \
	$(HOST_TEST_DIR)/native_mmio_adapter_production.o
NATIVE_LINUX_INPUT_EXECUTION_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_linux_input_execution_test.o
NATIVE_LINUX_INPUT_EXECUTION_OBJECTS = \
	$(HOST_TEST_DIR)/native_input_adapter.o \
	$(HOST_TEST_DIR)/native_scheduler_adapter.o \
	$(HOST_TEST_DIR)/native_offload_adapter.o
NATIVE_LINUX_INPUT_EXECUTION_PRODUCTION_OBJECTS = \
	$(HOST_TEST_DIR)/native_input_adapter_production.o \
	$(HOST_TEST_DIR)/native_scheduler_adapter_production.o \
	$(HOST_TEST_DIR)/native_offload_adapter_production.o
NATIVE_LINUX_INPUT_EXECUTION_HEADERS = \
	runtime/native/linux/native_input_adapter.hpp \
	runtime/native/linux/native_scheduler_adapter.hpp \
	runtime/native/linux/native_offload_adapter.hpp \
	runtime/native/native_input.hpp runtime/native/native_resources.hpp \
	runtime/native/native_spi_bus.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp runtime/mister_runtime.h
NATIVE_LINUX_CORE_PROTOCOL_IO_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_linux_core_protocol_io_test.o
NATIVE_LINUX_CORE_PROTOCOL_IO_OBJECT = \
	$(HOST_TEST_DIR)/native_core_protocol_io_adapter.o
NATIVE_LINUX_CORE_PROTOCOL_IO_PRODUCTION_OBJECT = \
	$(HOST_TEST_DIR)/native_core_protocol_io_adapter_production.o
NATIVE_CORE_PROTOCOL_CAPABILITY_REACHABILITY_TEST_OBJECT = \
	$(HOST_TEST_DIR)/native_core_protocol_capability_reachability_test.o
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

.PHONY: clean stage-c0-software-test test-runtime-smoke test-runtime test-scheduler test-scheduler-sanitize test-runtime-offload-lifecycle test-runtime-offload-lifecycle-sanitize test-native-hardware-broker test-native-hardware-broker-sanitize test-native-spi-bus test-native-spi-bus-sanitize test-native-core-profile test-native-core-profile-sanitize test-native-snes-content test-native-snes-content-sanitize test-native-core-protocol test-native-core-protocol-sanitize test-native-core-protocol-reachability test-native-core-protocol-capability-reachability test-native-core-protocol-dependency-invalidation test-native-save-dependency-invalidation test-native-linux-hardware-dependency-invalidation test-native-input test-native-input-sanitize test-native-lifecycle test-native-lifecycle-sanitize test-native-containment test-native-containment-sanitize test-native-recovery test-native-recovery-sanitize test-native-audio-video-adapter test-native-audio-video-adapter-sanitize test-native-audio-video-adapter-tsan test-native-audio-video-adapter-production test-native-save-adapter test-native-save-adapter-sanitize test-native-save-adapter-tsan test-native-save-adapter-production test-native-save-adapter-reachability test-native-linux-artifact-programmer test-native-linux-artifact-programmer-sanitize test-native-linux-mmio-containment test-native-linux-mmio-containment-sanitize test-native-linux-mmio-reachability test-native-linux-input-execution test-native-linux-input-execution-sanitize test-native-linux-input-execution-tsan test-native-linux-input-execution-reachability test-native-linux-core-protocol-io test-native-linux-core-protocol-io-sanitize test-native-linux-core-protocol-io-tsan test-native-linux-core-protocol-io-reachability test-runtime-legacy test-runtime-v2 test-runtime-v2-sanitize test-runtime-v2-production-sanitize test-runtime-linux-v2 test-runtime-resource-teardown test-runtime-resource-teardown-sanitize fogcast-runtime-storage-probe test-fogcast-protocol test-fogcast-state test-fogcast-protocol-sanitize test-fogcast-state-sanitize test-fogcast-coordinator test-fogcast-fence test-fogcast-coordinator-sanitize test-fogcast-server test-fogcast-server-sanitize test-fogcast-main test-fogcast-main-sanitize

test-native-linux-core-protocol-io: \
	$(HOST_TEST_DIR)/native_linux_core_protocol_io_test \
	test-native-linux-core-protocol-io-reachability \
	test-native-core-protocol-capability-reachability
	$(HOST_TEST_DIR)/native_linux_core_protocol_io_test

test-native-linux-core-protocol-io-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-core-protocol-io-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-linux-core-protocol-io

test-native-linux-core-protocol-io-tsan:
	TSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-core-protocol-io-tsan-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' test-native-linux-core-protocol-io

test-native-audio-video-adapter: $(HOST_TEST_DIR)/native_audio_video_adapter_test
	$(HOST_TEST_DIR)/native_audio_video_adapter_test

test-native-audio-video-adapter-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-audio-video-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-audio-video-adapter

test-native-audio-video-adapter-tsan:
	TSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-audio-video-tsan-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' test-native-audio-video-adapter

test-native-audio-video-adapter-production:
	mkdir -p $(HOST_TEST_DIR)/native-audio-video-production
	for f in runtime/native/hardware_broker.cpp runtime/native/native_peripheral_session.cpp runtime/native/native_core_profile.cpp runtime/native/native_lifecycle.cpp runtime/native/native_recovery.cpp runtime/native/linux/native_av_io_adapter.cpp runtime/native/linux/native_audio_adapter.cpp runtime/native/linux/native_video_adapter.cpp; do $(HOST_CXX) -std=c++14 $(HOST_FLAGS) -fno-exceptions -fno-rtti -I. -pthread -c $$f -o $(HOST_TEST_DIR)/native-audio-video-production/$$(basename $$f).o || exit 1; done

test-native-save-adapter: $(HOST_TEST_DIR)/native_save_adapter_test
	@privacy_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-save.XXXXXX"); \
		trap 'rm -r "$$privacy_dir"' EXIT; \
		if ! $(HOST_TEST_DIR)/native_save_adapter_test \
			>"$$privacy_dir/stdout" 2>"$$privacy_dir/stderr"; then \
			echo "native save adapter test failed; captured output withheld" >&2; \
			exit 1; \
		fi; \
		if test -s "$$privacy_dir/stdout" || test -s "$$privacy_dir/stderr"; then \
			echo "native save adapter test emitted private output" >&2; \
			exit 1; \
		fi; \
		for sentinel in \
			'/task6f-root-sentinel-2f5a' \
			'task6f-system-sentinel-6b91' \
			'c0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0de' \
			'task6f-basename-sentinel-1d6f' \
			'/task6f-device-sentinel-73bc' \
			'61951' \
			'task6f-profile-sentinel-4cd3' \
			'task6f-core-sentinel-8e20' \
			'task6f-game-sentinel-9a47' \
			'456192'; do \
			if grep -F -q "$$sentinel" "$$privacy_dir/stdout" "$$privacy_dir/stderr"; then \
				echo "native save adapter test leaked a seeded privacy sentinel" >&2; \
				exit 1; \
			fi; \
		done

test-native-save-adapter-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-save-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-save-adapter

test-native-save-adapter-tsan:
	TSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-save-tsan-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' test-native-save-adapter

test-native-save-adapter-production: test-native-save-adapter-reachability
	mkdir -p $(HOST_TEST_DIR)/native-save-production
	for f in runtime/native/hardware_broker.cpp runtime/native/native_core_profile.cpp runtime/native/native_save_key.cpp runtime/native/native_lifecycle.cpp runtime/native/native_recovery.cpp runtime/native/linux/native_save_adapter.cpp; do $(HOST_CXX) -std=c++14 $(HOST_FLAGS) -fno-exceptions -fno-rtti -I. -pthread -c $$f -o $(HOST_TEST_DIR)/native-save-production/$$(basename $$f).o || exit 1; done
	@if nm -g $(HOST_TEST_DIR)/native-save-production/native_save_adapter.cpp.o | \
		grep -E 'NativeSave(FileSystem|OpenResult)|HardwareLeaseView|NativeAvIo' >/dev/null; then \
		echo "production save adapter exports fixture or hardware authority" >&2; \
		exit 1; \
	fi

test-native-save-adapter-reachability:
	@probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-save-probe.XXXXXX"); \
		trap 'rm -r "$$probe_dir"' EXIT; \
		if $(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_SAVE_TESTING \
			-DMISTER_NATIVE_SAVE_REACHABILITY_PROBE -I. -fsyntax-only \
			tests/native_save_adapter_test.cpp >"$$probe_dir/out" 2>"$$probe_dir/err"; then \
			echo "production save header exposes fixture filesystem authority" >&2; \
			exit 1; \
		fi; \
		if ! grep 'NativeSaveFileSystem' "$$probe_dir/err" >/dev/null; then \
			echo "save fixture-authority probe failed for an unrelated reason" >&2; \
			exit 1; \
		fi

test-native-core-protocol-capability-reachability: \
	$(HOST_TEST_DIR)/native_core_protocol_capability_reachability_test
	$(HOST_TEST_DIR)/native_core_protocol_capability_reachability_test
	@if $(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_PROFILE_TESTING \
		-DMISTER_NATIVE_CORE_PROTOCOL_FIXTURE_CONSTRUCTOR_REACHABILITY_PROBE \
		-I. -fsyntax-only tests/native_core_protocol_capability_reachability_test.cpp \
		>/dev/null 2>&1; then \
		echo "production protocol exposes the fixture-only active constructor" >&2; exit 1; \
	fi

test-native-linux-core-protocol-io-reachability: \
	$(NATIVE_LINUX_CORE_PROTOCOL_IO_PRODUCTION_OBJECT)
	@if $(HOST_CXX) $(HOST_CXX_FLAGS) \
		-UMISTER_NATIVE_CORE_PROTOCOL_IO_TESTING \
		-DMISTER_NATIVE_CORE_PROTOCOL_IO_REACHABILITY_PROBE -I. -fsyntax-only \
		tests/native_linux_core_protocol_io_test.cpp >/dev/null 2>&1; then \
		echo "production header exposes raw core-protocol I/O authority" >&2; exit 1; \
	fi
	@if nm -g $(NATIVE_LINUX_CORE_PROTOCOL_IO_PRODUCTION_OBJECT) | \
		grep -E 'NativeCoreProtocolIoTest|LinuxOperations|PosixOperations|ff706' >/dev/null; then \
		echo "core-protocol I/O adapter exports raw authority" >&2; exit 1; \
	fi

test-native-linux-input-execution: \
	$(HOST_TEST_DIR)/native_linux_input_execution_test \
	test-native-linux-input-execution-reachability
	@privacy_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-input-exec.XXXXXX"); \
		trap 'rm -r "$$privacy_dir"' EXIT; \
		if ! $(HOST_TEST_DIR)/native_linux_input_execution_test \
			>"$$privacy_dir/stdout" 2>"$$privacy_dir/stderr"; then \
			echo "native input/execution test failed; captured output withheld" >&2; \
			exit 1; \
		fi; \
		if test -s "$$privacy_dir/stdout" || test -s "$$privacy_dir/stderr"; then \
			echo "native input/execution test emitted private output" >&2; \
			exit 1; \
		fi

test-native-linux-input-execution-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-linux-input-exec-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-linux-input-execution

test-native-linux-input-execution-tsan:
	$(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-linux-input-exec-tsan-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=thread -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' test-native-linux-input-execution

test-native-linux-input-execution-reachability: \
	$(NATIVE_LINUX_INPUT_EXECUTION_PRODUCTION_OBJECTS)
	@probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-exec-probes.XXXXXX"); \
		trap 'rm -r "$$probe_dir"' EXIT; \
		for probe in \
			MISTER_NATIVE_INPUT_OPERATIONS_REACHABILITY_PROBE \
			MISTER_NATIVE_EXECUTION_OPERATIONS_REACHABILITY_PROBE \
			MISTER_NATIVE_HARDWARE_VIEW_REACHABILITY_PROBE \
			MISTER_NATIVE_PROCESS_GUARD_REACHABILITY_PROBE; do \
			if $(HOST_CXX) $(HOST_CXX_FLAGS) \
				-UMISTER_NATIVE_LINUX_EXECUTION_TESTING -D$$probe \
				-I. -fsyntax-only tests/native_linux_input_execution_test.cpp \
				>"$$probe_dir/$$probe.out" 2>"$$probe_dir/$$probe.err"; then \
				echo "production header exposes forbidden route: $$probe" >&2; \
				exit 1; \
			fi; \
			case $$probe in \
			*MISTER_NATIVE_INPUT*) symbol=NativeLinuxInputOperations ;; \
			*MISTER_NATIVE_EXECUTION*) symbol=NativeLinuxExecutionOperations ;; \
			*MISTER_NATIVE_HARDWARE*) symbol=AcquireHardwareLeaseView ;; \
			*) symbol=AcquireProcessOperationGuard ;; \
			esac; \
			if ! grep "$$symbol" "$$probe_dir/$$probe.err" >/dev/null; then \
				echo "forbidden-route probe failed for an unrelated reason: $$probe" >&2; \
				exit 1; \
			fi; \
		done
	@if nm -g $(NATIVE_LINUX_INPUT_EXECUTION_PRODUCTION_OBJECTS) | \
		grep -E 'NativeLinux(Input|Execution)Operations|HardwareLeaseView|Main|user_io|scheduler.cpp|offload.cpp|input.cpp' >/dev/null; then \
		echo "native execution adapters expose raw or compatibility authority" >&2; exit 1; \
	fi

test-native-linux-artifact-programmer: \
	$(HOST_TEST_DIR)/native_linux_artifact_programmer_test
	$(HOST_TEST_DIR)/native_linux_artifact_programmer_test

test-native-linux-artifact-programmer-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-linux-artifact-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-linux-artifact-programmer

test-native-linux-mmio-containment: \
	$(HOST_TEST_DIR)/native_linux_mmio_containment_test \
	test-native-linux-mmio-reachability
	$(HOST_TEST_DIR)/native_linux_mmio_containment_test

test-native-linux-mmio-containment-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-linux-mmio-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-linux-mmio-containment

test-native-linux-mmio-reachability: $(NATIVE_LINUX_MMIO_PRODUCTION_OBJECT)
	@for probe in MISTER_NATIVE_BRIDGE_TOKEN_CONSTRUCT_REACHABILITY_PROBE \
		MISTER_NATIVE_BRIDGE_TOKEN_COPY_REACHABILITY_PROBE \
		MISTER_NATIVE_BRIDGE_TOKEN_MOVE_REACHABILITY_PROBE \
		MISTER_NATIVE_BRIDGE_TOKEN_MINT_REACHABILITY_PROBE \
		MISTER_NATIVE_BRIDGE_TOKEN_INSTALL_REACHABILITY_PROBE \
		MISTER_NATIVE_BRIDGE_TOKEN_VALIDATE_REACHABILITY_PROBE \
		MISTER_NATIVE_USER_IO_RAW_SELECT_REACHABILITY_PROBE \
		MISTER_NATIVE_USER_IO_RAW_CAST_REACHABILITY_PROBE \
		MISTER_NATIVE_CLEANUP_INPUT_VIEW_CONSTRUCT_REACHABILITY_PROBE \
		MISTER_NATIVE_CLEANUP_INPUT_VIEW_MINT_REACHABILITY_PROBE \
		MISTER_NATIVE_CLEANUP_INPUT_RAW_ROUTE_REACHABILITY_PROBE; do \
		if $(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_MMIO_TESTING \
			-D$$probe -I. -fsyntax-only \
			tests/native_linux_mmio_containment_test.cpp >/dev/null 2>&1; then \
			echo "production header exposes bridge activation authority: $$probe" >&2; \
			exit 1; \
		fi; \
	done
	@if $(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_MMIO_TESTING \
		-DMISTER_NATIVE_MMIO_REACHABILITY_PROBE -I. -fsyntax-only \
		tests/native_linux_mmio_containment_test.cpp >/dev/null 2>&1; then \
		echo "production header exposes raw MMIO authority" >&2; exit 1; \
	fi
	@if nm -g $(NATIVE_LINUX_MMIO_PRODUCTION_OBJECT) | \
		grep -E 'NativeMmio(Test|LinuxOperations)|PosixOperations' >/dev/null; then \
		echo "production object exports raw MMIO symbols" >&2; exit 1; \
	fi

stage-c0-software-test: test-runtime-v2 test-fogcast-protocol test-fogcast-state \
	test-fogcast-coordinator test-fogcast-fence test-fogcast-server \
	test-runtime-linux-v2 test-runtime-resource-teardown test-scheduler \
	test-runtime-offload-lifecycle \
	test-fogcast-protocol-sanitize test-fogcast-state-sanitize \
	test-fogcast-coordinator-sanitize test-fogcast-server-sanitize \
	test-runtime-linux-v2-sanitize test-runtime-resource-teardown-sanitize \
	test-scheduler-sanitize test-runtime-offload-lifecycle-sanitize \
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

test-scheduler-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/scheduler-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-scheduler

test-runtime-offload-lifecycle: $(HOST_TEST_DIR)/mister_runtime_offload_lifecycle_test
	$(HOST_TEST_DIR)/mister_runtime_offload_lifecycle_test

test-runtime-offload-lifecycle-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/offload-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-runtime-offload-lifecycle

test-native-hardware-broker: $(HOST_TEST_DIR)/native_hardware_broker_test
	$(HOST_TEST_DIR)/native_hardware_broker_test

test-native-hardware-broker-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-broker-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-hardware-broker

test-native-spi-bus: $(HOST_TEST_DIR)/native_spi_bus_test
	$(HOST_TEST_DIR)/native_spi_bus_test

test-native-spi-bus-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-spi-sanitize \
		HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
		HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-spi-bus

test-native-core-profile: $(HOST_TEST_DIR)/native_core_profile_test
	$(HOST_TEST_DIR)/native_core_profile_test

test-native-core-profile-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-profile-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-core-profile

test-native-snes-content: $(HOST_TEST_DIR)/native_snes_content_test
	$(HOST_TEST_DIR)/native_snes_content_test

test-native-snes-content-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-snes-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-snes-content

test-native-core-protocol: $(HOST_TEST_DIR)/native_core_protocol_test
	$(HOST_TEST_DIR)/native_core_protocol_test

test-native-core-protocol-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-core-protocol-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-core-protocol

test-native-core-protocol-reachability: \
	$(HOST_TEST_DIR)/native_core_protocol_reachability_test
	$(HOST_TEST_DIR)/native_core_protocol_reachability_test
	@probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-core-protocol-probes.XXXXXX"); \
		trap 'rm -r "$$probe_dir"' EXIT; \
		for probe in MISTER_NATIVE_INPUT_FILE_IO_REACHABILITY_PROBE MISTER_NATIVE_INPUT_GENERIC_SPI_REACHABILITY_PROBE; do \
			if $(HOST_CXX) $(HOST_CXX_FLAGS) -I. -D$$probe -fsyntax-only \
				tests/native_core_protocol_reachability_test.cpp \
				>"$$probe_dir/$$probe.out" 2>"$$probe_dir/$$probe.err"; then \
				echo "NativeInput exposes a forbidden generic SPI route: $$probe" >&2; \
				exit 1; \
			fi; \
			if ! grep 'bus_' "$$probe_dir/$$probe.err" >/dev/null; then \
				echo "forbidden NativeInput SPI probe failed for an unrelated reason: $$probe" >&2; \
				exit 1; \
			fi; \
		done

test-native-core-protocol-dependency-invalidation:
	@probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-core-protocol-deps.XXXXXX"); \
		trap 'rm -r "$$probe_dir"' EXIT; \
		for check in \
			'runtime/native/native_hardware_io.hpp:test-native-core-protocol:-c runtime/native/native_core_protocol.cpp' \
			'runtime/native/native_clock.hpp:test-native-core-protocol:-c runtime/native/native_core_protocol.cpp' \
			'runtime/native/native_snes_content.hpp:test-native-core-protocol:-c runtime/native/native_core_protocol.cpp' \
			'runtime/native/native_core_protocol_session_state.hpp:test-native-core-protocol:-c runtime/native/native_core_protocol.cpp' \
			'runtime/native/native_hardware_io.hpp:test-native-core-protocol-reachability:-c tests/native_core_protocol_reachability_test.cpp' \
			'runtime/native/native_clock.hpp:test-native-core-protocol-reachability:-c tests/native_core_protocol_reachability_test.cpp'; do \
			header=$${check%%:*}; remainder=$${check#*:}; \
			target=$${remainder%%:*}; expected=$${remainder#*:}; \
			$(MAKE) -f tests/Makefile HOST_TEST_DIR="$$probe_dir" "$$target" >/dev/null || exit 1; \
			if ! $(MAKE) -f tests/Makefile HOST_TEST_DIR="$$probe_dir" -W "$$header" -n "$$target" | grep -F -- "$$expected" >/dev/null; then \
				echo "missing timestamp invalidation: $$header -> $$target" >&2; \
				exit 1; \
		fi; \
	done

test-native-save-dependency-invalidation:
	@probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-save-deps.XXXXXX"); \
		trap 'rm -r "$$probe_dir"' EXIT; \
		for check in \
			'native_lifecycle.o:runtime/native/native_lifecycle.cpp' \
			'native_recovery.o:runtime/native/native_recovery.cpp' \
			'native_content_adapter.o:runtime/native/linux/native_content_adapter.cpp' \
			'native_linux_artifact_programmer_test.o:tests/native_linux_artifact_programmer_test.cpp' \
			'native_save_adapter_test.o:tests/native_save_adapter_test.cpp'; do \
			object=$${check%%:*}; source=$${check#*:}; target="$$probe_dir/$$object"; \
			$(MAKE) -f tests/Makefile HOST_TEST_DIR="$$probe_dir" "$$target" >/dev/null || exit 1; \
			for header in runtime/native/native_save_key.hpp runtime/native/native_resources.hpp; do \
				if ! $(MAKE) -f tests/Makefile HOST_TEST_DIR="$$probe_dir" -W "$$header" -n "$$target" | \
					grep -F -- "-c $$source" >/dev/null; then \
					echo "save dependency invalidation missing: $$object <- $$header" >&2; \
					exit 1; \
				fi; \
			done; \
		done

test-native-linux-hardware-dependency-invalidation:
	@probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/native-linux-hardware-deps.XXXXXX"); \
		trap 'rm -r "$$probe_dir"' EXIT; \
		for check in \
			'runtime/native/linux/native_core_artifact_adapter.hpp:test-native-linux-artifact-programmer:-c runtime/native/linux/native_core_artifact_adapter.cpp' \
			'runtime/native/linux/native_core_artifact_adapter.hpp:test-native-linux-artifact-programmer:-c runtime/native/linux/native_fpga_programmer.cpp' \
			'runtime/native/linux/native_fpga_programmer.hpp:test-native-linux-artifact-programmer:-c tests/native_linux_artifact_programmer_test.cpp' \
			'runtime/native/linux/native_fpga_programmer.hpp:test-native-linux-mmio-containment:-c runtime/native/linux/native_mmio_adapter.cpp' \
			'runtime/native/linux/native_mmio_adapter.hpp:test-native-linux-mmio-containment:-c tests/native_linux_mmio_containment_test.cpp' \
			'runtime/native/linux/native_mmio_adapter.hpp:test-native-linux-mmio-containment:-c runtime/native/linux/native_mmio_adapter.cpp' \
			'runtime/native/native_containment.hpp:test-native-containment:-c runtime/native/native_containment.cpp' \
			'runtime/native/native_containment.hpp:test-native-containment:-c tests/native_containment_test.cpp' \
			'runtime/native/native_containment.hpp:test-native-linux-mmio-containment:-c runtime/native/linux/native_mmio_adapter.cpp' \
			'runtime/native/native_containment.hpp:test-native-linux-mmio-containment:-c tests/native_linux_mmio_containment_test.cpp' \
			'runtime/native/hardware_broker.hpp:test-native-hardware-broker:-c runtime/native/hardware_broker.cpp' \
			'runtime/native/hardware_broker.hpp:test-native-hardware-broker:-c tests/native_hardware_broker_test.cpp' \
			'runtime/native/hardware_broker.hpp:test-native-containment:-c runtime/native/native_containment.cpp' \
			'runtime/native/hardware_broker.hpp:test-native-linux-mmio-containment:-c runtime/native/linux/native_mmio_adapter.cpp' \
			'runtime/native/native_hardware_io.hpp:test-native-linux-mmio-containment:-c runtime/native/linux/native_mmio_adapter.cpp' \
			'runtime/native/native_spi_bus.hpp:test-native-linux-mmio-containment:-c tests/native_linux_mmio_containment_test.cpp' \
			'runtime/native/native_input.hpp:test-native-spi-bus:-c runtime/native/native_spi_bus.cpp' \
			'runtime/native/native_input.hpp:test-native-linux-mmio-containment:-c tests/native_linux_mmio_containment_test.cpp' \
			'runtime/native/native_resources.hpp:test-native-input:-c runtime/native/native_input.cpp' \
			'runtime/native/native_clock.hpp:test-native-linux-artifact-programmer:-c runtime/native/linux/native_fpga_programmer.cpp' \
			'runtime/native/native_core_profile.hpp:test-native-linux-artifact-programmer:-c runtime/native/linux/native_core_artifact_adapter.cpp' \
			'runtime/native/native_resources.hpp:test-native-linux-artifact-programmer:-c tests/native_linux_artifact_programmer_test.cpp' \
			'runtime/mister_runtime.h:test-native-linux-mmio-containment:-c runtime/native/linux/native_mmio_adapter.cpp'; do \
			header=$${check%%:*}; remainder=$${check#*:}; \
			target=$${remainder%%:*}; expected=$${remainder#*:}; \
			$(MAKE) -f tests/Makefile HOST_TEST_DIR="$$probe_dir" "$$target" >/dev/null || exit 1; \
			if ! $(MAKE) -f tests/Makefile HOST_TEST_DIR="$$probe_dir" -W "$$header" -n "$$target" | \
				grep -F -- "$$expected" >/dev/null; then \
				echo "native Linux hardware dependency invalidation missing: $$header -> $$expected" >&2; \
				exit 1; \
			fi; \
		done

test-native-input: $(HOST_TEST_DIR)/native_input_test
	$(HOST_TEST_DIR)/native_input_test

test-native-input-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-input-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-input

test-native-lifecycle: $(HOST_TEST_DIR)/native_lifecycle_test
	$(HOST_TEST_DIR)/native_lifecycle_test

test-native-lifecycle-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-lifecycle-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-lifecycle

test-native-containment: $(HOST_TEST_DIR)/native_containment_test
	$(HOST_TEST_DIR)/native_containment_test

test-native-containment-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-containment-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-containment

test-native-recovery: $(HOST_TEST_DIR)/native_recovery_test
	$(HOST_TEST_DIR)/native_recovery_test

test-native-recovery-sanitize:
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f tests/Makefile HOST_TEST_DIR=$(HOST_TEST_DIR)/native-recovery-sanitize-inner HOST_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' HOST_LINK_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' test-native-recovery

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

$(RUNTIME_OFFLOAD_LIFECYCLE_TEST_OBJECT): tests/mister_runtime_offload_lifecycle_test.cpp offload.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DOFFLOAD_TESTING -I. -c $< -o $@

$(OFFLOAD_TEST_OBJECT): offload.cpp offload.h profiling.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DOFFLOAD_TESTING -I. -c $< -o $@

$(NATIVE_HARDWARE_BROKER_TEST_OBJECT): tests/native_hardware_broker_test.cpp \
	tests/native_core_protocol_authority_test_peer.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp \
	runtime/native/native_core_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_HARDWARE_BROKER_OBJECT): runtime/native/hardware_broker.cpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_peripheral_session.hpp \
	runtime/native/native_core_profile.hpp runtime/native/native_recovery.hpp \
	runtime/native/native_core_protocol.hpp \
	runtime/native/native_core_protocol_session_state.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_SPI_BUS_TEST_OBJECT): tests/native_spi_bus_test.cpp \
	tests/native_core_protocol_authority_test_peer.hpp \
	runtime/native/native_input.hpp runtime/native/native_resources.hpp \
	runtime/native/native_spi_bus.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp \
	runtime/native/native_core_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SPI_TESTING -I. -pthread -c $< -o $@

$(NATIVE_SPI_BUS_OBJECT): runtime/native/native_spi_bus.cpp \
	runtime/native/native_spi_bus.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/native_input.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_SPI_BUS_TEST_IMPLEMENTATION_OBJECT): runtime/native/native_spi_bus.cpp \
	runtime/native/native_spi_bus.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/native_input.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SPI_TESTING -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_spi_bus_test: $(NATIVE_SPI_BUS_TEST_OBJECT) \
	$(NATIVE_SPI_BUS_TEST_IMPLEMENTATION_OBJECT) $(NATIVE_INPUT_OBJECT) \
	$(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(NATIVE_CORE_PROFILE_TEST_OBJECT): tests/native_core_profile_test.cpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(NATIVE_CORE_PROFILE_OBJECT): runtime/native/native_core_profile.cpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(HOST_TEST_DIR)/native_core_profile_test: $(NATIVE_CORE_PROFILE_TEST_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(NATIVE_SNES_CONTENT_TEST_OBJECT): tests/native_snes_content_test.cpp \
	runtime/native/native_snes_content.hpp runtime/native/native_clock.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(NATIVE_SNES_CONTENT_OBJECT): runtime/native/native_snes_content.cpp \
	runtime/native/native_snes_content.hpp runtime/native/native_clock.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(HOST_TEST_DIR)/native_snes_content_test: $(NATIVE_SNES_CONTENT_TEST_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(NATIVE_CORE_PROTOCOL_TEST_OBJECT): tests/native_core_protocol_test.cpp \
	runtime/native/native_core_protocol.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/native_snes_content.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(NATIVE_CORE_PROTOCOL_OBJECT): runtime/native/native_core_protocol.cpp \
	runtime/native/native_core_protocol.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/native_snes_content.hpp \
	runtime/native/native_core_protocol_session_state.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(HOST_TEST_DIR)/native_core_protocol_test: \
	$(NATIVE_CORE_PROTOCOL_TEST_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(NATIVE_LINUX_CORE_PROTOCOL_IO_TEST_OBJECT): \
	tests/native_linux_core_protocol_io_test.cpp \
	runtime/native/linux/native_core_protocol_io_adapter.hpp \
	runtime/native/native_core_protocol.hpp runtime/native/native_resources.hpp \
	runtime/native/native_recovery.hpp runtime/native/hardware_broker.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_CORE_PROTOCOL_IO_TESTING \
		-I. -pthread -c $< -o $@

$(NATIVE_LINUX_CORE_PROTOCOL_IO_OBJECT): \
	runtime/native/linux/native_core_protocol_io_adapter.cpp \
	runtime/native/linux/native_core_protocol_io_adapter.hpp \
	runtime/native/native_core_protocol_session_state.hpp \
	runtime/native/hardware_broker.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_CORE_PROTOCOL_IO_TESTING \
		-I. -pthread -c $< -o $@

$(NATIVE_LINUX_CORE_PROTOCOL_IO_PRODUCTION_OBJECT): \
	runtime/native/linux/native_core_protocol_io_adapter.cpp \
	runtime/native/linux/native_core_protocol_io_adapter.hpp \
	runtime/native/native_core_protocol_session_state.hpp \
	runtime/native/hardware_broker.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_CORE_PROTOCOL_IO_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_linux_core_protocol_io_test: \
	$(NATIVE_LINUX_CORE_PROTOCOL_IO_TEST_OBJECT) \
	$(NATIVE_LINUX_CORE_PROTOCOL_IO_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_RECOVERY_OBJECT) $(NATIVE_CONTAINMENT_OBJECT) \
	$(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(NATIVE_CORE_PROTOCOL_REACHABILITY_TEST_OBJECT): \
	tests/native_core_protocol_reachability_test.cpp \
	runtime/native/native_core_protocol.hpp runtime/native/native_input.hpp \
	runtime/native/native_core_profile.hpp runtime/native/native_spi_bus.hpp \
	runtime/native/native_hardware_io.hpp runtime/native/hardware_broker.hpp \
	runtime/native/native_clock.hpp runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(NATIVE_CORE_PROTOCOL_CAPABILITY_REACHABILITY_TEST_OBJECT): \
	tests/native_core_protocol_capability_reachability_test.cpp \
	runtime/native/native_core_protocol.hpp \
	runtime/native/linux/native_core_protocol_io_adapter.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -c $< -o $@

$(HOST_TEST_DIR)/native_core_protocol_capability_reachability_test: \
	$(NATIVE_CORE_PROTOCOL_CAPABILITY_REACHABILITY_TEST_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/native_core_protocol_reachability_test: \
	$(NATIVE_CORE_PROTOCOL_REACHABILITY_TEST_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(NATIVE_INPUT_TEST_OBJECT): tests/native_input_test.cpp \
	runtime/native/native_input.hpp runtime/native/native_core_profile.hpp \
	runtime/native/native_resources.hpp \
	runtime/native/native_spi_bus.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_INPUT_OBJECT): runtime/native/native_input.cpp \
	runtime/native/native_input.hpp runtime/native/native_core_profile.hpp \
	runtime/native/native_resources.hpp \
	runtime/native/native_spi_bus.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_LIFECYCLE_TEST_OBJECT): tests/native_lifecycle_test.cpp \
	tests/native_core_protocol_authority_test_peer.hpp \
	runtime/native/native_lifecycle.hpp runtime/native/native_resources.hpp \
	runtime/native/native_save_key.hpp runtime/native/linux/native_save_adapter.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp \
	runtime/native/native_core_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SAVE_TESTING -I. -pthread -c $< -o $@

$(NATIVE_LIFECYCLE_OBJECT): runtime/native/native_lifecycle.cpp \
	runtime/native/native_lifecycle.hpp runtime/native/native_resources.hpp \
	runtime/native/native_save_key.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_CONTAINMENT_TEST_OBJECT): tests/native_containment_test.cpp \
	runtime/native/native_containment.hpp runtime/native/hardware_broker.hpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_CONTAINMENT_OBJECT): runtime/native/native_containment.cpp \
	runtime/native/native_containment.hpp runtime/native/hardware_broker.hpp \
	runtime/native/native_clock.hpp runtime/native/native_core_profile.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_RECOVERY_TEST_OBJECT): tests/native_recovery_test.cpp \
	tests/native_core_protocol_authority_test_peer.hpp \
	runtime/native/native_recovery.hpp runtime/native/native_containment.hpp \
	runtime/native/linux/native_save_adapter.hpp runtime/native/native_save_key.hpp \
	runtime/native/native_resources.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_core_profile.hpp \
	runtime/native/native_core_protocol.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SAVE_TESTING -I. -pthread -c $< -o $@

$(NATIVE_RECOVERY_OBJECT): runtime/native/native_recovery.cpp \
	runtime/native/native_recovery.hpp runtime/native/hardware_broker.hpp \
	runtime/native/native_resources.hpp runtime/native/native_save_key.hpp \
	runtime/native/native_clock.hpp runtime/native/native_core_profile.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_input_test: $(NATIVE_INPUT_TEST_OBJECT) \
	$(NATIVE_INPUT_OBJECT) $(NATIVE_CORE_PROFILE_OBJECT) \
	$(NATIVE_SPI_BUS_OBJECT) \
	$(NATIVE_HARDWARE_BROKER_OBJECT) $(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/native_lifecycle_test: $(NATIVE_LIFECYCLE_TEST_OBJECT) \
	$(NATIVE_LIFECYCLE_OBJECT) $(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) $(NATIVE_SAVE_ADAPTER_OBJECTS) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CONTAINMENT_OBJECT) \
	$(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/native_containment_test: $(NATIVE_CONTAINMENT_TEST_OBJECT) \
	$(NATIVE_CONTAINMENT_OBJECT) $(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/native_recovery_test: $(NATIVE_RECOVERY_TEST_OBJECT) \
	$(NATIVE_RECOVERY_OBJECT) $(NATIVE_CONTAINMENT_OBJECT) \
	$(NATIVE_HARDWARE_BROKER_OBJECT) $(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_SAVE_ADAPTER_OBJECTS) \
	$(NATIVE_CORE_PROTOCOL_OBJECT) $(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(NATIVE_AUDIO_VIDEO_ADAPTER_TEST_OBJECT): \
	tests/native_audio_video_adapter_test.cpp \
	runtime/native/native_peripheral_session.hpp \
	runtime/native/linux/native_av_io_adapter.hpp \
	runtime/native/linux/native_audio_adapter.hpp \
	runtime/native/linux/native_video_adapter.hpp \
	runtime/native/native_core_profile.hpp runtime/native/hardware_broker.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_AUDIO_VIDEO_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_peripheral_session.o: \
	runtime/native/native_peripheral_session.cpp \
	runtime/native/native_peripheral_session.hpp \
	runtime/native/hardware_broker.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_AUDIO_VIDEO_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_av_io_adapter.o: \
	runtime/native/linux/native_av_io_adapter.cpp \
	runtime/native/linux/native_av_io_adapter.hpp \
	runtime/native/native_peripheral_session.hpp \
	runtime/native/native_core_profile.hpp runtime/native/native_clock.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_AUDIO_VIDEO_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_audio_adapter.o: \
	runtime/native/linux/native_audio_adapter.cpp \
	runtime/native/linux/native_audio_adapter.hpp \
	runtime/native/linux/native_av_io_adapter.hpp \
	runtime/native/native_peripheral_session.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_resources.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_AUDIO_VIDEO_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_video_adapter.o: \
	runtime/native/linux/native_video_adapter.cpp \
	runtime/native/linux/native_video_adapter.hpp \
	runtime/native/linux/native_av_io_adapter.hpp \
	runtime/native/native_peripheral_session.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_resources.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_AUDIO_VIDEO_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_audio_video_adapter_test: \
	$(NATIVE_AUDIO_VIDEO_ADAPTER_TEST_OBJECT) \
	$(NATIVE_AUDIO_VIDEO_ADAPTER_OBJECTS) $(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(NATIVE_SAVE_ADAPTER_TEST_OBJECT): tests/native_save_adapter_test.cpp \
	runtime/native/native_save_key.hpp runtime/native/native_core_profile.hpp \
	runtime/native/linux/native_save_adapter.hpp runtime/native/hardware_broker.hpp \
	runtime/native/native_resources.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SAVE_TESTING -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_save_key.o: runtime/native/native_save_key.cpp \
	runtime/native/native_save_key.hpp runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SAVE_TESTING -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_save_adapter.o: runtime/native/linux/native_save_adapter.cpp \
	runtime/native/linux/native_save_adapter.hpp runtime/native/native_save_key.hpp \
	runtime/native/native_resources.hpp runtime/native/hardware_broker.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_SAVE_TESTING -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_save_adapter_test: $(NATIVE_SAVE_ADAPTER_TEST_OBJECT) \
	$(NATIVE_SAVE_ADAPTER_OBJECTS) $(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(NATIVE_LINUX_ARTIFACT_PROGRAMMER_TEST_OBJECT): \
	tests/native_linux_artifact_programmer_test.cpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/linux/native_content_adapter.hpp \
	runtime/native/linux/native_fpga_programmer.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_core_profile.hpp \
	runtime/native/native_clock.hpp runtime/native/native_resources.hpp \
	runtime/native/native_save_key.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_linux_artifact_programmer_test: \
	$(NATIVE_LINUX_ARTIFACT_PROGRAMMER_TEST_OBJECT) \
	$(NATIVE_LINUX_ARTIFACT_PROGRAMMER_OBJECTS) \
	$(NATIVE_HARDWARE_BROKER_OBJECT) $(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(HOST_TEST_DIR)/native_save_key.o \
	$(NATIVE_CORE_PROTOCOL_OBJECT) $(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/native_fpga_programmer.o: runtime/native/linux/native_fpga_programmer.cpp \
	runtime/native/linux/native_fpga_programmer.hpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/hardware_broker.hpp runtime/native/native_clock.hpp \
	runtime/native/native_core_profile.hpp runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_core_artifact_adapter.o: runtime/native/linux/native_core_artifact_adapter.cpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/native_core_profile.hpp | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_content_adapter.o: runtime/native/linux/native_content_adapter.cpp \
	runtime/native/linux/native_content_adapter.hpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/native_resources.hpp runtime/native/hardware_broker.hpp \
	runtime/native/native_clock.hpp runtime/native/native_core_profile.hpp \
	runtime/native/native_save_key.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -I. -pthread -c $< -o $@

$(NATIVE_LINUX_MMIO_CONTAINMENT_TEST_OBJECT): \
	tests/native_linux_mmio_containment_test.cpp \
	runtime/native/linux/native_mmio_adapter.hpp \
	runtime/native/linux/native_fpga_programmer.hpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/native_containment.hpp runtime/native/native_spi_bus.hpp \
	runtime/native/native_input.hpp runtime/native/native_resources.hpp \
	runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp \
	runtime/native/native_clock.hpp runtime/native/native_core_profile.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_MMIO_TESTING \
		-DMISTER_NATIVE_SPI_TESTING \
		-I. -pthread -c $< -o $@

$(NATIVE_LINUX_MMIO_CONTAINMENT_OBJECT): \
	runtime/native/linux/native_mmio_adapter.cpp \
	runtime/native/linux/native_mmio_adapter.hpp \
	runtime/native/linux/native_fpga_programmer.hpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/native_containment.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp \
	runtime/native/native_clock.hpp runtime/native/native_core_profile.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_MMIO_TESTING \
		-I. -pthread -c $< -o $@

$(NATIVE_LINUX_MMIO_PRODUCTION_OBJECT): \
	runtime/native/linux/native_mmio_adapter.cpp \
	runtime/native/linux/native_mmio_adapter.hpp \
	runtime/native/linux/native_fpga_programmer.hpp \
	runtime/native/linux/native_core_artifact_adapter.hpp \
	runtime/native/native_containment.hpp runtime/native/native_hardware_io.hpp \
	runtime/native/hardware_broker.hpp \
	runtime/native/native_clock.hpp runtime/native/native_core_profile.hpp \
	runtime/mister_runtime.h | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_MMIO_TESTING \
		-I. -pthread -c $< -o $@

$(NATIVE_LINUX_INPUT_EXECUTION_TEST_OBJECT): \
	tests/native_linux_input_execution_test.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_input_adapter.o: runtime/native/linux/native_input_adapter.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_scheduler_adapter.o: runtime/native/linux/native_scheduler_adapter.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_offload_adapter.o: runtime/native/linux/native_offload_adapter.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -DMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_input_adapter_production.o: runtime/native/linux/native_input_adapter.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_scheduler_adapter_production.o: runtime/native/linux/native_scheduler_adapter.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_offload_adapter_production.o: runtime/native/linux/native_offload_adapter.cpp \
	$(NATIVE_LINUX_INPUT_EXECUTION_HEADERS) | $(HOST_TEST_DIR)
	$(HOST_CXX) $(HOST_CXX_FLAGS) -UMISTER_NATIVE_LINUX_EXECUTION_TESTING \
		-I. -pthread -c $< -o $@

$(HOST_TEST_DIR)/native_linux_input_execution_test: \
	$(NATIVE_LINUX_INPUT_EXECUTION_TEST_OBJECT) \
	$(NATIVE_LINUX_INPUT_EXECUTION_OBJECTS) $(NATIVE_INPUT_OBJECT) \
	$(NATIVE_SPI_BUS_OBJECT) $(NATIVE_HARDWARE_BROKER_OBJECT) \
	$(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) \
	$(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/native_linux_mmio_containment_test: \
	$(NATIVE_LINUX_MMIO_CONTAINMENT_TEST_OBJECT) \
	$(NATIVE_LINUX_MMIO_CONTAINMENT_OBJECT) \
	$(HOST_TEST_DIR)/native_core_artifact_adapter.o \
	$(HOST_TEST_DIR)/native_fpga_programmer.o $(NATIVE_CONTAINMENT_OBJECT) \
	$(NATIVE_SPI_BUS_TEST_IMPLEMENTATION_OBJECT) $(NATIVE_INPUT_OBJECT) \
	$(NATIVE_HARDWARE_BROKER_OBJECT) $(NATIVE_PERIPHERAL_SESSION_OBJECT) \
	$(NATIVE_CORE_PROFILE_OBJECT) \
	$(NATIVE_CORE_PROTOCOL_OBJECT) $(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

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

$(HOST_TEST_DIR)/mister_runtime_offload_lifecycle_test: $(RUNTIME_OFFLOAD_LIFECYCLE_TEST_OBJECT) $(OFFLOAD_TEST_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/native_hardware_broker_test: $(NATIVE_HARDWARE_BROKER_TEST_OBJECT) $(NATIVE_HARDWARE_BROKER_OBJECT) $(NATIVE_PERIPHERAL_SESSION_OBJECT) $(NATIVE_CORE_PROFILE_OBJECT) $(NATIVE_CORE_PROTOCOL_OBJECT) $(NATIVE_SNES_CONTENT_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -pthread -o $@

$(HOST_TEST_DIR)/fogcast-runtime-storage-probe: $(FOGCAST_STORAGE_PROBE_OBJECT) $(RUNTIME_LINUX_V2_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_wrapper_fixture: $(WRAPPER_FIXTURE_OBJECT) \
	$(WRAPPER_MAIN_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/mister_runtime_wrapper_test: $(WRAPPER_TEST_OBJECT)
	$(HOST_CXX) $^ -o $@

$(HOST_TEST_DIR)/scheduler_step_test: $(SCHEDULER_TEST_OBJECT) $(SCHEDULER_OBJECT) \
	$(HOST_LIBCO_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

$(HOST_TEST_DIR)/scheduler_init_failure_test: $(SCHEDULER_FAILURE_TEST_OBJECT) \
	$(SCHEDULER_OBJECT)
	$(HOST_CXX) $(HOST_LINK_FLAGS) $^ -o $@

clean:
	rm -rf $(HOST_TEST_DIR)
