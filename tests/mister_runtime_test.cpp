/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <assert.h>
#include <stddef.h>
#include <string.h>

struct FakePlatform {
	MisterPlatform platform;
	bool start_result;
	bool load_result;
	bool tick_result;
	MisterPlatformStopResult stop_results[4];
	unsigned stop_result_count;
	unsigned stop_result_index;
	unsigned start_calls;
	unsigned load_calls;
	unsigned tick_calls;
	unsigned stop_calls;
	const char *events[8];
	unsigned event_count;
};

static void record(FakePlatform *fake, const char *event)
{
	assert(fake->event_count < sizeof(fake->events) / sizeof(fake->events[0]));
	fake->events[fake->event_count++] = event;
}

static bool fake_start(void *context)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->start_calls;
	record(fake, "start");
	return fake->start_result;
}

static bool fake_load(void *context, const MisterLaunch *)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->load_calls;
	record(fake, "load");
	return fake->load_result;
}

static bool fake_tick(void *context)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->tick_calls;
	record(fake, "tick");
	return fake->tick_result;
}

static MisterPlatformStopResult fake_stop(void *context)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->stop_calls;
	record(fake, "stop");
	assert(fake->stop_result_index < fake->stop_result_count);
	return fake->stop_results[fake->stop_result_index++];
}

static FakePlatform make_fake(uint32_t capability_flags = 0)
{
	FakePlatform fake = {};
	fake.platform.abi_version = MISTER_RUNTIME_ABI_VERSION;
	fake.platform.struct_size = sizeof(fake.platform);
	fake.platform.capability_flags = capability_flags;
	fake.platform.context = &fake;
	fake.platform.start = fake_start;
	fake.platform.load = fake_load;
	fake.platform.tick = fake_tick;
	fake.platform.stop = fake_stop;
	fake.start_result = true;
	fake.load_result = true;
	fake.tick_result = true;
	fake.stop_results[0] = MISTER_PLATFORM_RELEASED;
	fake.stop_result_count = 1;
	return fake;
}

static MisterRuntime *create_runtime(FakePlatform *fake)
{
	fake->platform.context = fake;
	return MisterRuntime_Create(&fake->platform);
}

static MisterLaunch valid_launch()
{
	MisterLaunch launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION;
	launch.struct_size = sizeof(launch);
	launch.core_path = "core.rbf";
	launch.xml_path = "core.xml";
	return launch;
}

static uint32_t primary_error_for_state(uint32_t state)
{
	return state == MISTER_RUNTIME_FAILED ||
		state == MISTER_RUNTIME_CLEANUP_FAILED ||
		state == MISTER_RUNTIME_EXIT_REQUIRED ?
		MISTER_RUNTIME_ERROR_PLATFORM_START : MISTER_RUNTIME_ERROR_NONE;
}

static uint32_t cleanup_error_for_state(uint32_t state)
{
	return state == MISTER_RUNTIME_CLEANUP_FAILED ?
		MISTER_RUNTIME_ERROR_PLATFORM_STOP : MISTER_RUNTIME_ERROR_NONE;
}

static MisterRuntime *create_runtime_in_state(FakePlatform *fake, uint32_t state)
{
	MisterRuntime *runtime = create_runtime(fake);
	MisterLaunch launch = valid_launch();
	assert(runtime != nullptr);

	switch (state) {
	case MISTER_RUNTIME_READY:
		assert(MisterRuntime_Start(runtime));
		break;
	case MISTER_RUNTIME_RUNNING:
		assert(MisterRuntime_Start(runtime));
		assert(MisterRuntime_Load(runtime, &launch));
		break;
	case MISTER_RUNTIME_FAILED:
		fake->start_result = false;
		assert(!MisterRuntime_Start(runtime));
		break;
	case MISTER_RUNTIME_CLEANUP_FAILED:
		fake->start_result = false;
		fake->stop_results[0] = MISTER_PLATFORM_STOP_FAILED;
		assert(!MisterRuntime_Start(runtime));
		MisterRuntime_Stop(runtime);
		break;
	case MISTER_RUNTIME_EXIT_REQUIRED:
		fake->start_result = false;
		fake->stop_results[0] = MISTER_PLATFORM_EXIT_REQUIRED;
		assert(!MisterRuntime_Start(runtime));
		MisterRuntime_Stop(runtime);
		break;
	case MISTER_RUNTIME_STOPPED:
		MisterRuntime_Stop(runtime);
		break;
	default:
		assert(false);
	}

	return runtime;
}

static void expect_status(const MisterRuntime *runtime, uint32_t state,
	uint32_t last_error, uint32_t primary_error, uint32_t cleanup_error,
	uint64_t tick_count, uint32_t capability_flags)
{
	MisterStatus status = MisterRuntime_Status(runtime);
	assert(status.abi_version == MISTER_RUNTIME_ABI_VERSION);
	assert(status.struct_size == sizeof(MisterStatus));
	assert(status.state == state);
	assert(status.last_error == last_error);
	assert(status.primary_error == primary_error);
	assert(status.cleanup_error == cleanup_error);
	assert(status.tick_count == tick_count);
	assert(status.capability_flags == capability_flags);
	assert(status.reserved[0] == 0);
	assert(status.reserved[1] == 0);
	assert(status.reserved[2] == 0);
}

static void test_null_contract()
{
	MisterRuntime *runtime = nullptr;
	MisterLaunch launch = valid_launch();

	assert(MisterRuntime_ABIVersion() == MISTER_RUNTIME_ABI_VERSION);
	expect_status(nullptr, MISTER_RUNTIME_FAILED,
		MISTER_RUNTIME_ERROR_INVALID_ARGUMENT,
		MISTER_RUNTIME_ERROR_INVALID_ARGUMENT, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(!MisterRuntime_Start(nullptr));
	assert(!MisterRuntime_Load(nullptr, &launch));
	MisterRuntime_Tick(nullptr);
	MisterRuntime_Stop(nullptr);
	assert(!MisterRuntime_Destroy(nullptr));
	assert(!MisterRuntime_Destroy(&runtime));
}

static void test_create_rejects_invalid_platforms()
{
	FakePlatform fake = make_fake();
	assert(MisterRuntime_Create(nullptr) == nullptr);

	fake.platform.abi_version = MISTER_RUNTIME_ABI_VERSION + 1;
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.abi_version = MISTER_RUNTIME_ABI_VERSION;

	fake.platform.struct_size = sizeof(fake.platform) - 1;
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.struct_size = sizeof(fake.platform);

	fake.platform.capability_flags = MISTER_PLATFORM_CAP_CLEAN_STOP |
		MISTER_PLATFORM_CAP_PROCESS_CONTROL_ESCAPE | (1u << 31);
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.capability_flags = 0;

	bool (*start)(void *) = fake.platform.start;
	fake.platform.start = nullptr;
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.start = start;

	bool (*load)(void *, const MisterLaunch *) = fake.platform.load;
	fake.platform.load = nullptr;
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.load = load;

	bool (*tick)(void *) = fake.platform.tick;
	fake.platform.tick = nullptr;
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.tick = tick;

	MisterPlatformStopResult (*stop)(void *) = fake.platform.stop;
	fake.platform.stop = nullptr;
	assert(MisterRuntime_Create(&fake.platform) == nullptr);
	fake.platform.stop = stop;

	assert(fake.start_calls == 0 && fake.load_calls == 0 &&
		fake.tick_calls == 0 && fake.stop_calls == 0);

	fake.platform.struct_size = sizeof(fake.platform) + 16;
	MisterRuntime *runtime = create_runtime(&fake);
	assert(runtime != nullptr);
	assert(MisterRuntime_Destroy(&runtime));
}

static void test_created_stop_and_destroy()
{
	FakePlatform fake = make_fake(MISTER_PLATFORM_CAP_CLEAN_STOP |
		MISTER_PLATFORM_CAP_PROCESS_CONTROL_ESCAPE);
	MisterRuntime *runtime = create_runtime(&fake);
	assert(runtime != nullptr);
	expect_status(runtime, MISTER_RUNTIME_CREATED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0,
		MISTER_RUNTIME_CAP_CLEAN_STOP);
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_STOPPED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0,
		MISTER_RUNTIME_CAP_CLEAN_STOP);
	assert(fake.stop_calls == 0);
	assert(MisterRuntime_Destroy(&runtime));
	assert(runtime == nullptr);

	runtime = create_runtime(&fake);
	assert(runtime != nullptr);
	assert(MisterRuntime_Destroy(&runtime));
	assert(runtime == nullptr);
}

static void test_successful_lifecycle_and_copied_platform()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = create_runtime(&fake);
	MisterLaunch launch = valid_launch();
	assert(runtime != nullptr);

	fake.platform.start = nullptr;
	assert(MisterRuntime_Start(runtime));
	assert(MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Tick(runtime);
	MisterRuntime_Tick(runtime);
	expect_status(runtime, MISTER_RUNTIME_RUNNING, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 2, 0);
	assert(fake.event_count == 4);
	assert(strcmp(fake.events[0], "start") == 0);
	assert(strcmp(fake.events[1], "load") == 0);
	assert(strcmp(fake.events[2], "tick") == 0);
	assert(strcmp(fake.events[3], "tick") == 0);
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_STOPPED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 2, 0);
	assert(fake.stop_calls == 1);
	assert(fake.event_count == 5);
	assert(strcmp(fake.events[4], "stop") == 0);
	assert(MisterRuntime_Destroy(&runtime));
}

static void test_invalid_calls_preserve_state_and_primary_error()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = create_runtime(&fake);
	MisterLaunch launch = valid_launch();
	assert(runtime != nullptr);
	assert(!MisterRuntime_Load(runtime, &launch));
	expect_status(runtime, MISTER_RUNTIME_CREATED, MISTER_RUNTIME_ERROR_INVALID_STATE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	MisterRuntime_Tick(runtime);
	MisterRuntime_Stop(runtime);
	MisterRuntime_Stop(runtime);
	assert(!MisterRuntime_Start(runtime));
	assert(!MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Tick(runtime);
	expect_status(runtime, MISTER_RUNTIME_STOPPED, MISTER_RUNTIME_ERROR_INVALID_STATE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(fake.event_count == 0);
	assert(MisterRuntime_Destroy(&runtime));
}

static void test_launch_validation()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = create_runtime(&fake);
	MisterLaunch launch = valid_launch();
	assert(runtime != nullptr);
	assert(MisterRuntime_Start(runtime));

	assert(!MisterRuntime_Load(runtime, nullptr));
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION + 1;
	assert(!MisterRuntime_Load(runtime, &launch));
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION;
	launch.struct_size = sizeof(launch) - 1;
	assert(!MisterRuntime_Load(runtime, &launch));
	launch.struct_size = sizeof(launch) + 8;
	launch.core_path = nullptr;
	assert(!MisterRuntime_Load(runtime, &launch));
	expect_status(runtime, MISTER_RUNTIME_READY, MISTER_RUNTIME_ERROR_INVALID_ARGUMENT,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(fake.load_calls == 0);
	MisterRuntime_Stop(runtime);
	assert(MisterRuntime_Destroy(&runtime));

	FakePlatform oversized_fake = make_fake();
	runtime = create_runtime(&oversized_fake);
	launch = valid_launch();
	launch.struct_size = sizeof(launch) + 16;
	assert(MisterRuntime_Start(runtime));
	assert(MisterRuntime_Load(runtime, &launch));
	assert(oversized_fake.load_calls == 1);
	MisterRuntime_Stop(runtime);
	assert(MisterRuntime_Destroy(&runtime));
}

static void test_start_load_and_tick_failures()
{
	MisterLaunch launch = valid_launch();

	FakePlatform start_fake = make_fake();
	start_fake.start_result = false;
	MisterRuntime *runtime = create_runtime(&start_fake);
	assert(!MisterRuntime_Start(runtime));
	expect_status(runtime, MISTER_RUNTIME_FAILED, MISTER_RUNTIME_ERROR_PLATFORM_START,
		MISTER_RUNTIME_ERROR_PLATFORM_START, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	MisterRuntime_Stop(runtime);
	assert(MisterRuntime_Destroy(&runtime));

	FakePlatform load_fake = make_fake();
	load_fake.load_result = false;
	runtime = create_runtime(&load_fake);
	assert(MisterRuntime_Start(runtime));
	assert(!MisterRuntime_Load(runtime, &launch));
	expect_status(runtime, MISTER_RUNTIME_FAILED, MISTER_RUNTIME_ERROR_PLATFORM_LOAD,
		MISTER_RUNTIME_ERROR_PLATFORM_LOAD, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	MisterRuntime_Stop(runtime);
	assert(MisterRuntime_Destroy(&runtime));

	FakePlatform tick_fake = make_fake();
	tick_fake.tick_result = false;
	runtime = create_runtime(&tick_fake);
	assert(MisterRuntime_Start(runtime));
	assert(MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Tick(runtime);
	expect_status(runtime, MISTER_RUNTIME_FAILED, MISTER_RUNTIME_ERROR_PLATFORM_TICK,
		MISTER_RUNTIME_ERROR_PLATFORM_TICK, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	MisterRuntime_Tick(runtime);
	expect_status(runtime, MISTER_RUNTIME_FAILED, MISTER_RUNTIME_ERROR_INVALID_STATE,
		MISTER_RUNTIME_ERROR_PLATFORM_TICK, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(tick_fake.tick_calls == 1);
	MisterRuntime_Stop(runtime);
	assert(MisterRuntime_Destroy(&runtime));
}

static void test_stop_failure_retry_and_exit_required()
{
	MisterLaunch launch = valid_launch();
	FakePlatform retry_fake = make_fake();
	retry_fake.stop_results[0] = MISTER_PLATFORM_STOP_FAILED;
	retry_fake.stop_results[1] = MISTER_PLATFORM_RELEASED;
	retry_fake.stop_result_count = 2;
	MisterRuntime *runtime = create_runtime(&retry_fake);
	assert(MisterRuntime_Start(runtime));
	assert(MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_CLEANUP_FAILED,
		MISTER_RUNTIME_ERROR_PLATFORM_STOP, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_PLATFORM_STOP, 0, 0);
	assert(!MisterRuntime_Destroy(&runtime));
	assert(runtime != nullptr);
	expect_status(runtime, MISTER_RUNTIME_CLEANUP_FAILED,
		MISTER_RUNTIME_ERROR_INVALID_STATE, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_PLATFORM_STOP, 0, 0);
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_STOPPED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(retry_fake.stop_calls == 2);
	assert(retry_fake.event_count == 4);
	assert(strcmp(retry_fake.events[0], "start") == 0);
	assert(strcmp(retry_fake.events[1], "load") == 0);
	assert(strcmp(retry_fake.events[2], "stop") == 0);
	assert(strcmp(retry_fake.events[3], "stop") == 0);
	assert(MisterRuntime_Destroy(&runtime));

	FakePlatform exit_fake = make_fake();
	exit_fake.stop_results[0] = MISTER_PLATFORM_EXIT_REQUIRED;
	runtime = create_runtime(&exit_fake);
	assert(runtime != nullptr);
	assert(MisterRuntime_Start(runtime));
	assert(MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Tick(runtime);
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_EXIT_REQUIRED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 1, 0);
	assert(!MisterRuntime_Destroy(&runtime));
	assert(runtime != nullptr);
	MisterRuntime_Stop(runtime);
	assert(exit_fake.stop_calls == 1);
	expect_status(runtime, MISTER_RUNTIME_EXIT_REQUIRED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 1, 0);
	assert(!MisterRuntime_Start(runtime));
	assert(!MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Tick(runtime);
	expect_status(runtime, MISTER_RUNTIME_EXIT_REQUIRED,
		MISTER_RUNTIME_ERROR_INVALID_STATE, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, 1, 0);
}

static void test_idempotent_stop_clears_intervening_errors()
{
	MisterLaunch launch = valid_launch();
	FakePlatform exit_fake = make_fake();
	exit_fake.stop_results[0] = MISTER_PLATFORM_EXIT_REQUIRED;
	MisterRuntime *runtime = create_runtime(&exit_fake);
	assert(MisterRuntime_Start(runtime));
	assert(MisterRuntime_Load(runtime, &launch));
	MisterRuntime_Stop(runtime);
	assert(!MisterRuntime_Destroy(&runtime));
	expect_status(runtime, MISTER_RUNTIME_EXIT_REQUIRED,
		MISTER_RUNTIME_ERROR_INVALID_STATE, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, 0, 0);
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_EXIT_REQUIRED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(exit_fake.stop_calls == 1);

	FakePlatform stopped_fake = make_fake();
	runtime = create_runtime(&stopped_fake);
	MisterRuntime_Stop(runtime);
	assert(!MisterRuntime_Start(runtime));
	expect_status(runtime, MISTER_RUNTIME_STOPPED,
		MISTER_RUNTIME_ERROR_INVALID_STATE, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, 0, 0);
	MisterRuntime_Stop(runtime);
	expect_status(runtime, MISTER_RUNTIME_STOPPED, MISTER_RUNTIME_ERROR_NONE,
		MISTER_RUNTIME_ERROR_NONE, MISTER_RUNTIME_ERROR_NONE, 0, 0);
	assert(stopped_fake.stop_calls == 0);
	assert(MisterRuntime_Destroy(&runtime));
}

enum MatrixApi {
	MATRIX_START,
	MATRIX_LOAD,
	MATRIX_TICK,
	MATRIX_DESTROY
};

static void release_matrix_runtime(FakePlatform *fake, MisterRuntime **runtime)
{
	if (*runtime == nullptr) {
		return;
	}
	uint32_t state = MisterRuntime_Status(*runtime).state;
	if (state == MISTER_RUNTIME_EXIT_REQUIRED) {
		return;
	}
	if (state == MISTER_RUNTIME_CLEANUP_FAILED) {
		fake->stop_results[fake->stop_result_count++] = MISTER_PLATFORM_RELEASED;
	}
	MisterRuntime_Stop(*runtime);
	assert(MisterRuntime_Status(*runtime).state == MISTER_RUNTIME_STOPPED);
	assert(MisterRuntime_Destroy(runtime));
}

static void test_exhaustive_api_state_matrix()
{
	static const uint32_t states[] = {
		MISTER_RUNTIME_READY,
		MISTER_RUNTIME_RUNNING,
		MISTER_RUNTIME_FAILED,
		MISTER_RUNTIME_CLEANUP_FAILED,
		MISTER_RUNTIME_EXIT_REQUIRED,
		MISTER_RUNTIME_STOPPED
	};
	static const MatrixApi apis[] = {
		MATRIX_START,
		MATRIX_LOAD,
		MATRIX_TICK,
		MATRIX_DESTROY
	};
	MisterLaunch launch = valid_launch();

	for (unsigned state_index = 0;
		state_index < sizeof(states) / sizeof(states[0]); ++state_index) {
		for (unsigned api_index = 0;
			api_index < sizeof(apis) / sizeof(apis[0]); ++api_index) {
			FakePlatform fake = make_fake();
			uint32_t initial_state = states[state_index];
			MisterRuntime *runtime = create_runtime_in_state(&fake, initial_state);
			unsigned start_calls = fake.start_calls;
			unsigned load_calls = fake.load_calls;
			unsigned tick_calls = fake.tick_calls;
			unsigned stop_calls = fake.stop_calls;
			unsigned event_count = fake.event_count;
			bool valid = false;

			switch (apis[api_index]) {
			case MATRIX_START:
				valid = MisterRuntime_Start(runtime);
				break;
			case MATRIX_LOAD:
				valid = MisterRuntime_Load(runtime, &launch);
				break;
			case MATRIX_TICK:
				MisterRuntime_Tick(runtime);
				valid = initial_state == MISTER_RUNTIME_RUNNING;
				break;
			case MATRIX_DESTROY:
				valid = MisterRuntime_Destroy(&runtime);
				break;
			}

			bool expected_valid =
				(apis[api_index] == MATRIX_LOAD &&
					initial_state == MISTER_RUNTIME_READY) ||
				(apis[api_index] == MATRIX_TICK &&
					initial_state == MISTER_RUNTIME_RUNNING) ||
				(apis[api_index] == MATRIX_DESTROY &&
					initial_state == MISTER_RUNTIME_STOPPED);
			assert(valid == expected_valid);

			if (apis[api_index] == MATRIX_DESTROY && expected_valid) {
				assert(runtime == nullptr);
				continue;
			}

			uint32_t expected_state = initial_state;
			uint32_t expected_error = MISTER_RUNTIME_ERROR_INVALID_STATE;
			uint64_t expected_ticks = 0;
			if (apis[api_index] == MATRIX_LOAD && expected_valid) {
				expected_state = MISTER_RUNTIME_RUNNING;
				expected_error = MISTER_RUNTIME_ERROR_NONE;
			}
			if (apis[api_index] == MATRIX_TICK && expected_valid) {
				expected_error = MISTER_RUNTIME_ERROR_NONE;
				expected_ticks = 1;
			}
			expect_status(runtime, expected_state, expected_error,
				primary_error_for_state(initial_state),
				cleanup_error_for_state(initial_state), expected_ticks, 0);

			assert(fake.start_calls == start_calls);
			assert(fake.stop_calls == stop_calls);
			assert(fake.load_calls == load_calls +
				(apis[api_index] == MATRIX_LOAD && expected_valid ? 1u : 0u));
			assert(fake.tick_calls == tick_calls +
				(apis[api_index] == MATRIX_TICK && expected_valid ? 1u : 0u));
			assert(fake.event_count == event_count + (expected_valid &&
				apis[api_index] != MATRIX_DESTROY ? 1u : 0u));

			release_matrix_runtime(&fake, &runtime);
		}
	}
}

int main()
{
	test_null_contract();
	test_create_rejects_invalid_platforms();
	test_created_stop_and_destroy();
	test_successful_lifecycle_and_copied_platform();
	test_invalid_calls_preserve_state_and_primary_error();
	test_launch_validation();
	test_start_load_and_tick_failures();
	test_stop_failure_retry_and_exit_required();
	test_idempotent_stop_clears_intervening_errors();
	test_exhaustive_api_state_matrix();
	return 0;
}
