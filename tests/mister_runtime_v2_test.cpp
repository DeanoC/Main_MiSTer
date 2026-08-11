/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <assert.h>
#include <string.h>

extern "C" MisterResult MisterRuntime_RawResultFixture(uint32_t raw);

struct FakePlatform {
	MisterPlatformV2 platform;
	MisterResult start_result;
	MisterResult load_result;
	MisterResult tick_result;
	MisterResult observe_result;
	MisterResult stop_result;
	MisterResult recover_result;
	uint32_t observed_resources;
	uint32_t neutral_resources;
	uint32_t observed_ready;
	unsigned start_calls;
	unsigned load_calls;
	unsigned tick_calls;
	unsigned observe_calls;
	unsigned stop_calls;
	unsigned recover_calls;
	uint32_t last_deadline;
	bool try_create_during_recover;
	unsigned throw_callback;
	bool malformed_observation;
	bool malformed_recovery;
	unsigned observation_output_mutation;
	unsigned recovery_output_mutation;
};

static unsigned callback_count(const FakePlatform &fake);
static MisterRuntime *create_in_state(FakePlatform *fake, uint32_t state);

static MisterResult fake_start(void *context, uint32_t deadline_ms)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->start_calls;
	fake->last_deadline = deadline_ms;
	if (fake->throw_callback == 1) throw 1;
	return fake->start_result;
}

static MisterResult fake_load(void *context, const MisterLaunchV2 *, uint32_t deadline_ms)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->load_calls;
	fake->last_deadline = deadline_ms;
	if (fake->throw_callback == 2) throw 1;
	return fake->load_result;
}

static MisterResult fake_tick(void *context, uint32_t deadline_ms)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->tick_calls;
	fake->last_deadline = deadline_ms;
	if (fake->throw_callback == 3) throw 1;
	return fake->tick_result;
}

static MisterResult fake_observe(void *context, MisterObservationV2 *observation,
	uint32_t deadline_ms)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->observe_calls;
	fake->last_deadline = deadline_ms;
	if (fake->throw_callback == 4) throw 1;
	if (fake->observe_result != MISTER_RESULT_OK) return fake->observe_result;
	if (fake->malformed_observation) observation->abi_version = 1;
	observation->ready = fake->observed_ready;
	observation->resource_flags = fake->observed_resources;
	static const char long_core[] =
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	switch (fake->observation_output_mutation) {
	case 1: observation->abi_version = 1; break;
	case 2: observation->struct_size--; break;
	case 3: observation->struct_size++; break;
	case 4: observation->ready = 2; break;
	case 5: observation->resource_flags = 1u << 16; break;
	case 6: observation->observed_core = {"?", 1}; break;
	case 7: observation->observed_core = {long_core, static_cast<uint32_t>(sizeof(long_core) - 1)}; break;
	case 8: case 9: case 10: case 11: observation->reserved[fake->observation_output_mutation - 8] = 1; break;
	default: break;
	}
	return MISTER_RESULT_OK;
}

static MisterResult fake_stop(void *context, uint32_t deadline_ms)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->stop_calls;
	fake->last_deadline = deadline_ms;
	if (fake->throw_callback == 5) throw 1;
	return fake->stop_result;
}

static MisterResult fake_observe_as_stop(void *context, MisterObservationV2 *,
	uint32_t deadline_ms)
{
	return fake_stop(context, deadline_ms);
}

static MisterResult fake_recover(void *context, uint32_t resources,
	MisterRecoveryObservationV2 *observation, uint32_t deadline_ms)
{
	FakePlatform *fake = static_cast<FakePlatform *>(context);
	++fake->recover_calls;
	fake->last_deadline = deadline_ms;
	if (fake->throw_callback == 6) throw 1;
	if (fake->try_create_during_recover) {
		MisterRuntime *runtime = nullptr;
		assert(MisterRuntime_CreateV2(&fake->platform, &runtime) ==
			MISTER_RESULT_INVALID_STATE);
		assert(runtime == nullptr);
	}
	if (fake->malformed_recovery) observation->struct_size = 0;
	observation->observed_resource_flags = fake->observed_resources & resources;
	observation->neutral_resource_flags = fake->neutral_resources & resources;
	switch (fake->recovery_output_mutation) {
	case 1: observation->abi_version = 1; break;
	case 2: observation->struct_size--; break;
	case 3: observation->struct_size++; break;
	case 4: observation->observed_resource_flags = 1u << 16; break;
	case 5: observation->neutral_resource_flags = resources | (1u << 16); break;
	case 6: observation->observed_resource_flags = resources; break;
	case 7: case 8: case 9: case 10: observation->reserved[fake->recovery_output_mutation - 7] = 1; break;
	default: break;
	}
	return fake->recover_result;
}

static FakePlatform make_fake()
{
	FakePlatform fake = {};
	fake.platform.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	fake.platform.struct_size = sizeof(fake.platform);
	fake.platform.capability_flags = MISTER_CAP_V2_KNOWN;
	fake.platform.context = &fake;
	fake.platform.start = fake_start;
	fake.platform.load = fake_load;
	fake.platform.tick = fake_tick;
	fake.platform.observe = fake_observe;
	fake.platform.stop = fake_stop;
	fake.platform.recover = fake_recover;
	fake.start_result = MISTER_RESULT_OK;
	fake.load_result = MISTER_RESULT_OK;
	fake.tick_result = MISTER_RESULT_OK;
	fake.observe_result = MISTER_RESULT_OK;
	fake.stop_result = MISTER_RESULT_OK;
	fake.recover_result = MISTER_RESULT_OK;
	fake.observed_resources = MISTER_RESOURCE_V2_KNOWN;
	fake.neutral_resources = MISTER_RESOURCE_V2_KNOWN;
	fake.observed_ready = 1;
	return fake;
}

static MisterLaunchV2 valid_launch()
{
	static const char game[] = "sonic-the-hedgehog";
	static const char system[] = "megadrive";
	static const char core[] = "MegaDrive";
	static const char digest[] =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static const char extension[] = "md";
	MisterLaunchV2 launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id = {game, static_cast<uint32_t>(sizeof(game) - 1)};
	launch.system = {system, static_cast<uint32_t>(sizeof(system) - 1)};
	launch.expected_core = {core, static_cast<uint32_t>(sizeof(core) - 1)};
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256 = {digest, static_cast<uint32_t>(sizeof(digest) - 1)};
	launch.content.size = 1048576;
	launch.content.extension = {extension, static_cast<uint32_t>(sizeof(extension) - 1)};
	return launch;
}

static MisterObservationV2 initialized_observation()
{
	MisterObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

static MisterRecoveryObservationV2 initialized_recovery()
{
	MisterRecoveryObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

static MisterStatusV2 initialized_status()
{
	MisterStatusV2 status = {};
	status.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	status.struct_size = sizeof(status);
	return status;
}

static void expect_status(MisterRuntime *runtime, uint32_t state,
	MisterResult last, MisterResult primary, MisterResult cleanup, uint64_t ticks)
{
	MisterStatusV2 status = initialized_status();
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
	assert(status.state == state);
	assert(status.last_result == static_cast<uint32_t>(last));
	assert(status.primary_result == static_cast<uint32_t>(primary));
	assert(status.cleanup_result == static_cast<uint32_t>(cleanup));
	assert(status.tick_count == ticks);
	assert(status.capability_flags == MISTER_CAP_V2_KNOWN);
	for (unsigned index = 0; index < 4; ++index) assert(status.reserved[index] == 0);
}

static MisterRuntime *create(FakePlatform *fake)
{
	fake->platform.context = fake;
	MisterRuntime *runtime = nullptr;
	assert(MisterRuntime_CreateV2(&fake->platform, &runtime) == MISTER_RESULT_OK);
	assert(runtime != nullptr);
	return runtime;
}

static void destroy_stopped(MisterRuntime **runtime)
{
	assert(MisterRuntime_DestroyV2(runtime) == MISTER_RESULT_OK);
	assert(*runtime == nullptr);
}

static void test_create_validation_and_cross_generation_rejection()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = nullptr;
	assert(MisterRuntime_ABIVersionV2() == MISTER_RUNTIME_ABI_VERSION_V2);
	assert(MisterRuntime_CreateV2(nullptr, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_CreateV2(&fake.platform, nullptr) == MISTER_RESULT_INVALID_ARGUMENT);
	runtime = reinterpret_cast<MisterRuntime *>(1);
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	runtime = nullptr;

	fake.platform.abi_version = 1;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	fake.platform.struct_size = sizeof(fake.platform) - 1;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.struct_size = sizeof(fake.platform) + 16;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
	fake.platform.struct_size = sizeof(fake.platform);
	fake.platform.reserved[2] = 1;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.reserved[2] = 0;
	fake.platform.capability_flags = MISTER_CAP_V2_KNOWN | (1u << 16);
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.capability_flags = MISTER_CAP_CLEAN_STOP;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.capability_flags = MISTER_CAP_V2_KNOWN;
	MisterResult (*callbacks[])(void *, uint32_t) = {fake.platform.start,
		fake.platform.tick, fake.platform.stop};
	for (unsigned index = 0; index < sizeof(callbacks) / sizeof(callbacks[0]); ++index) {
		MisterResult (*saved)(void *, uint32_t) = callbacks[index];
		if (index == 0) fake.platform.start = nullptr;
		if (index == 1) fake.platform.tick = nullptr;
		if (index == 2) fake.platform.stop = nullptr;
		assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
		if (index == 0) fake.platform.start = saved;
		if (index == 1) fake.platform.tick = saved;
		if (index == 2) fake.platform.stop = saved;
	}
	MisterResult (*saved_load)(void *, const MisterLaunchV2 *, uint32_t) = fake.platform.load;
	fake.platform.load = nullptr;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.load = saved_load;
	MisterResult (*saved_observe)(void *, MisterObservationV2 *, uint32_t) =
		fake.platform.observe;
	fake.platform.observe = nullptr;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.observe = saved_observe;
	MisterResult (*saved_recover)(void *, uint32_t, MisterRecoveryObservationV2 *, uint32_t) =
		fake.platform.recover;
	fake.platform.recover = nullptr;
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
	fake.platform.recover = saved_recover;
	MisterRuntime *v2 = create(&fake);
	assert(MisterRuntime_StartV2(v2, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	expect_status(v2, MISTER_STATE_CREATED, MISTER_RESULT_INVALID_ARGUMENT,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_StopV2(v2, 1) == MISTER_RESULT_OK);
	destroy_stopped(&v2);
}

static bool v1_start(void *) { return true; }
static bool v1_load(void *, const MisterLaunch *) { return true; }
static bool v1_tick(void *) { return true; }
static MisterPlatformStopResult v1_stop(void *) { return MISTER_PLATFORM_RELEASED; }

static void test_cross_generation_handles_are_rejected()
{
	MisterPlatform v1_platform = {
		MISTER_RUNTIME_ABI_VERSION, sizeof(MisterPlatform), 0, nullptr,
		v1_start, v1_load, v1_tick, v1_stop
	};
	MisterRuntime *v1 = MisterRuntime_Create(&v1_platform);
	assert(v1 != nullptr);
	assert(MisterRuntime_StartV2(v1, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	MisterLaunchV2 launch = valid_launch();
	MisterObservationV2 observation = initialized_observation();
	MisterStatusV2 status = initialized_status();
	assert(MisterRuntime_LoadV2(v1, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_TickV2(v1, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_ObserveV2(v1, &observation, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_StatusV2(v1, &status) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_StopV2(v1, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_DestroyV2(&v1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_Destroy(&v1));

	MisterLaunch v1_launch = {MISTER_RUNTIME_ABI_VERSION, sizeof(MisterLaunch), "core", nullptr};
	for (unsigned call = 0; call < 5; ++call) {
		FakePlatform fake = make_fake();
		uint32_t v1_state = call == 0 ? MISTER_RUNTIME_CREATED :
			(call == 1 ? MISTER_RUNTIME_RUNNING : MISTER_RUNTIME_READY);
		fake.platform.stop = reinterpret_cast<MisterResult (*)(void *, uint32_t)>(
			static_cast<uintptr_t>(0x100000000ULL + v1_state));
		if (call == 4) {
			fake.platform.observe = fake_observe_as_stop;
		}
		MisterRuntime *v2 = create(&fake);
		switch (call) {
		case 0: assert(!MisterRuntime_Start(v2)); break;
		case 1: MisterRuntime_Tick(v2); break;
		case 2: assert(!MisterRuntime_Load(v2, &v1_launch)); break;
		case 3: {
			MisterStatus status = MisterRuntime_Status(v2);
			assert(status.state == MISTER_RUNTIME_FAILED);
			assert(status.last_error == MISTER_RUNTIME_ERROR_INVALID_ARGUMENT);
			break;
		}
		case 4: MisterRuntime_Stop(v2); break;
		default: assert(false);
		}
		assert(callback_count(fake) == 0);
		expect_status(v2, MISTER_STATE_CREATED, MISTER_RESULT_OK,
			MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
		destroy_stopped(&v2);
	}

	static FakePlatform destroy_guard_fake;
	destroy_guard_fake = make_fake();
	destroy_guard_fake.platform.stop = reinterpret_cast<MisterResult (*)(void *, uint32_t)>(
		static_cast<uintptr_t>(MISTER_RUNTIME_STOPPED));
	MisterRuntime *v2 = create(&destroy_guard_fake);
	assert(!MisterRuntime_Destroy(&v2));
	assert(v2 != nullptr && callback_count(destroy_guard_fake) == 0);
	expect_status(v2, MISTER_STATE_CREATED, MISTER_RESULT_OK,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	destroy_stopped(&v2);
}

static void test_validation_and_successful_lifecycle()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = create(&fake);
	MisterLaunchV2 launch = valid_launch();
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_STATE);
	assert(MisterRuntime_StartV2(runtime, 17) == MISTER_RESULT_OK);
	assert(fake.last_deadline == 17 && fake.start_calls == 1);
	launch.game_id.data = nullptr;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = valid_launch();
	launch.game_id.data = "Bad";
	launch.game_id.length = 3;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = valid_launch();
	launch.content.sha256.data = "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = valid_launch();
	launch.content.size = 0;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch.content.size = 32u * 1024u * 1024u + 1;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = valid_launch();
	launch.content.extension.data = "7z";
	launch.content.extension.length = 2;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform mixed_extension_fake = make_fake();
	runtime = create(&mixed_extension_fake);
	launch = valid_launch();
	assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
	launch.content.extension.data = "m2";
	launch.content.extension.length = 2;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform invalid_extension_fake = make_fake();
	runtime = create(&invalid_extension_fake);
	launch = valid_launch();
	assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
	launch.content.extension.data = "M2";
	launch.content.extension.length = 2;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform rest_fake = make_fake();
	runtime = create(&rest_fake);
	launch = valid_launch();
	assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
	launch = valid_launch();
	launch.content.reserved[0] = 1;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = valid_launch();
	launch.struct_size = sizeof(launch) - 1;
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = valid_launch();
	launch.struct_size = sizeof(launch) + 8;
	assert(MisterRuntime_LoadV2(runtime, &launch, 19) == MISTER_RESULT_OK);
	assert(rest_fake.load_calls == 1 && rest_fake.observe_calls == 1 && rest_fake.last_deadline == 19);
	expect_status(runtime, MISTER_STATE_RUNNING, MISTER_RESULT_OK,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_TickV2(runtime, 23) == MISTER_RESULT_OK);
	assert(rest_fake.tick_calls == 1 && rest_fake.last_deadline == 23);
	MisterObservationV2 observation = initialized_observation();
	assert(MisterRuntime_ObserveV2(runtime, &observation, 29) == MISTER_RESULT_OK);
	assert(observation.ready == 1 && observation.resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(MisterRuntime_StopV2(runtime, 31) == MISTER_RESULT_OK);
	expect_status(runtime, MISTER_STATE_STOPPED, MISTER_RESULT_OK,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 1);
	destroy_stopped(&runtime);
}

static void test_registry_failure_paths_and_raw_invalid_results()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = nullptr;
	MisterRuntimeTest::SetFault(MisterRuntimeTest::FAULT_CREATE_ALLOCATE);
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_PLATFORM);
	assert(runtime == nullptr);
	MisterRuntimeTest::SetFault(MisterRuntimeTest::FAULT_CREATE_REGISTER);
	assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_PLATFORM);
	assert(runtime == nullptr);
	MisterRuntimeTest::ClearFault();
	runtime = create(&fake);
	MisterRuntimeTest::SetFault(MisterRuntimeTest::FAULT_DESTROY_UNREGISTER);
	assert(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_PLATFORM);
	assert(runtime != nullptr);
	MisterRuntimeTest::ClearFault();
	destroy_stopped(&runtime);

	MisterRecoveryObservationV2 recovery = initialized_recovery();
	fake.platform.context = &fake;
	fake.observed_resources = 0;
	fake.neutral_resources = MISTER_RESOURCE_FPGA;
	MisterRuntimeTest::SetFault(MisterRuntimeTest::FAULT_RECOVER_REGISTER);
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&recovery, 1) == MISTER_RESULT_PLATFORM);
	assert(fake.recover_calls == 0);
	MisterRuntimeTest::SetFault(MisterRuntimeTest::FAULT_RECOVER_UNREGISTER);
	recovery = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&recovery, 1) == MISTER_RESULT_PLATFORM);
	MisterRuntimeTest::ClearFault();
	recovery = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&recovery, 1) == MISTER_RESULT_OK);
	MisterRuntimeTest::SetFault(MisterRuntimeTest::FAULT_RECOVER_DOUBLE_UNREGISTER);
	recovery = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&recovery, 1) == MISTER_RESULT_PLATFORM);
	MisterRuntimeTest::ClearFault();
	recovery = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&recovery, 1) == MISTER_RESULT_OK);
	runtime = create(&fake);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	static const uint32_t invalid_results[] = {8u, 99u, UINT32_MAX};
	for (unsigned index = 0; index < sizeof(invalid_results) / sizeof(invalid_results[0]); ++index) {
		MisterResult raw = MisterRuntime_RawResultFixture(invalid_results[index]);

		FakePlatform start_fake = make_fake();
		start_fake.start_result = raw;
		runtime = create(&start_fake);
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);

		FakePlatform load_fake = make_fake();
		load_fake.load_result = raw;
		runtime = create(&load_fake);
		MisterLaunchV2 launch = valid_launch();
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_PLATFORM);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);

		FakePlatform tick_fake = make_fake();
		tick_fake.tick_result = raw;
		runtime = create(&tick_fake);
		launch = valid_launch();
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
		assert(MisterRuntime_TickV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);

		FakePlatform observe_fake = make_fake();
		observe_fake.observe_result = raw;
		runtime = create(&observe_fake);
		MisterObservationV2 observed = initialized_observation();
		assert(MisterRuntime_ObserveV2(runtime, &observed, 1) == MISTER_RESULT_PLATFORM);
		expect_status(runtime, MISTER_STATE_CREATED, MISTER_RESULT_OK,
			MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);

		FakePlatform stop_fake = make_fake();
		stop_fake.stop_result = raw;
		runtime = create(&stop_fake);
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		expect_status(runtime, MISTER_STATE_CLEANUP_INCOMPLETE, MISTER_RESULT_PLATFORM,
			MISTER_RESULT_OK, MISTER_RESULT_PLATFORM, 0);
		stop_fake.stop_result = MISTER_RESULT_OK;
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);

		FakePlatform recover_fake = make_fake();
		recover_fake.platform.context = &recover_fake;
		recover_fake.observed_resources = 0;
		recover_fake.neutral_resources = MISTER_RESOURCE_FPGA;
		recover_fake.recover_result = raw;
		recovery = initialized_recovery();
		assert(MisterRuntime_RecoverPlatformV2(&recover_fake.platform,
			MISTER_RESOURCE_FPGA, &recovery, 1) == MISTER_RESULT_PLATFORM);
		recover_fake.recover_result = MISTER_RESULT_OK;
		recovery = initialized_recovery();
		assert(MisterRuntime_RecoverPlatformV2(&recover_fake.platform,
			MISTER_RESOURCE_FPGA, &recovery, 1) == MISTER_RESULT_OK);
	}
}

static void test_observe_and_status_initialization()
{
	FakePlatform fake = make_fake();
	MisterRuntime *runtime = create(&fake);
	MisterObservationV2 observation = initialized_observation();
	observation.ready = 1;
	assert(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_ObserveV2(runtime, nullptr, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	observation = initialized_observation();
	assert(MisterRuntime_ObserveV2(runtime, &observation, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	MisterStatusV2 status = initialized_status();
	status.tick_count = 1;
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_INVALID_ARGUMENT);
	status = initialized_status();
	status.struct_size--;
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_INVALID_ARGUMENT);
	status = initialized_status();
	status.abi_version--;
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_INVALID_ARGUMENT);
	status = initialized_status();
	status.reserved[0] = 1;
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_INVALID_ARGUMENT);
	status = initialized_status();
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
}

static void test_each_v2_structure_guard_rejects_one_mutation()
{
	for (unsigned mutation = 0; mutation < 24; ++mutation) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create(&fake);
		MisterLaunchV2 launch = valid_launch();
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		switch (mutation) {
		case 0: launch.abi_version = 1; break;
		case 1: launch.struct_size = sizeof(launch) - 1; break;
		case 2: case 3: case 4: case 5: launch.reserved[mutation - 2] = 1; break;
		case 6: launch.game_id.data = nullptr; break;
		case 7: launch.game_id.length = 0; break;
		case 8: launch.system.data = nullptr; break;
		case 9: launch.system.length = 0; break;
		case 10: launch.expected_core.data = nullptr; break;
		case 11: launch.expected_core.length = 0; break;
		case 12: launch.content.abi_version = 1; break;
		case 13: launch.content.struct_size = sizeof(launch.content) - 1; break;
		case 14: case 15: case 16: case 17: launch.content.reserved[mutation - 14] = 1; break;
		case 18: launch.content.sha256.data = nullptr; break;
		case 19: launch.content.sha256.length = 63; break;
		case 20: launch.content.size = 0; break;
		case 21: launch.content.extension.data = nullptr; break;
		case 22: launch.content.extension.length = 0; break;
		case 23: launch.content.size = 32u * 1024u * 1024u + 1; break;
		default: assert(false);
		}
		assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
		assert(fake.load_calls == 0 && fake.observe_calls == 0);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}

	for (unsigned mutation = 0; mutation < 11; ++mutation) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create(&fake);
		MisterObservationV2 observation = initialized_observation();
		switch (mutation) {
		case 0: observation.abi_version = 1; break;
		case 1: observation.struct_size--; break;
		case 2: observation.struct_size++; break;
		case 3: observation.ready = 1; break;
		case 4: observation.observed_core.data = "x"; break;
		case 5: observation.observed_core.length = 1; break;
		case 6: observation.resource_flags = MISTER_RESOURCE_FPGA; break;
		case 7: case 8: case 9: case 10: observation.reserved[mutation - 7] = 1; break;
		default: assert(false);
		}
		assert(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_INVALID_ARGUMENT);
		assert(fake.observe_calls == 0);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}

	for (unsigned mutation = 0; mutation < 9; ++mutation) {
		FakePlatform fake = make_fake();
		MisterRecoveryObservationV2 recovery = initialized_recovery();
		switch (mutation) {
		case 0: recovery.abi_version = 1; break;
		case 1: recovery.struct_size--; break;
		case 2: recovery.struct_size++; break;
		case 3: recovery.observed_resource_flags = MISTER_RESOURCE_FPGA; break;
		case 4: recovery.neutral_resource_flags = MISTER_RESOURCE_FPGA; break;
		case 5: case 6: case 7: case 8: recovery.reserved[mutation - 5] = 1; break;
		default: assert(false);
		}
		assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
			&recovery, 1) == MISTER_RESULT_INVALID_ARGUMENT);
		assert(fake.recover_calls == 0);
	}

	for (unsigned mutation = 0; mutation < 13; ++mutation) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create(&fake);
		MisterStatusV2 status = initialized_status();
		switch (mutation) {
		case 0: status.abi_version = 1; break;
		case 1: status.struct_size--; break;
		case 2: status.struct_size++; break;
		case 3: status.state = 1; break;
		case 4: status.last_result = 1; break;
		case 5: status.primary_result = 1; break;
		case 6: status.cleanup_result = 1; break;
		case 7: status.tick_count = 1; break;
		case 8: status.capability_flags = 1; break;
		case 9: case 10: case 11: case 12: status.reserved[mutation - 9] = 1; break;
		default: assert(false);
		}
		assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_INVALID_ARGUMENT);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}

	for (unsigned mutation = 0; mutation < 7; ++mutation) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = nullptr;
		switch (mutation) {
		case 0: fake.platform.abi_version = 1; break;
		case 1: fake.platform.struct_size--; break;
		case 2: case 3: case 4: case 5: fake.platform.reserved[mutation - 2] = 1; break;
		case 6: fake.platform.capability_flags = MISTER_CAP_CLEAN_STOP; break;
		default: assert(false);
		}
		assert(MisterRuntime_CreateV2(&fake.platform, &runtime) == MISTER_RESULT_INVALID_ARGUMENT);
		assert(runtime == nullptr);
	}
}

static void test_string_and_null_input_guards()
{
	FakePlatform null_fake = make_fake();
	MisterRuntime *runtime = create_in_state(&null_fake, MISTER_STATE_READY);
	assert(MisterRuntime_LoadV2(runtime, nullptr, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(null_fake.load_calls == 0);
	MisterStatusV2 status = initialized_status();
	assert(MisterRuntime_StatusV2(runtime, nullptr) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	char game[130]; char system[34]; char core[66]; char extension[18];
	memset(game, 'a', sizeof(game)); memset(system, 'a', sizeof(system));
	memset(core, 'a', sizeof(core)); memset(extension, '1', sizeof(extension));
	for (unsigned mutation = 0; mutation < 10; ++mutation) {
		FakePlatform fake = make_fake();
		runtime = create_in_state(&fake, MISTER_STATE_READY);
		MisterLaunchV2 launch = valid_launch();
		switch (mutation) {
		case 0: launch.game_id = {"-sonic", 6}; break;
		case 1: launch.game_id = {"sonic-", 6}; break;
		case 2: launch.game_id = {"so?nic", 6}; break;
		case 3: launch.game_id = {game, 129}; break;
		case 4: launch.system = {"mega?drive", 10}; break;
		case 5: launch.system = {system, 33}; break;
		case 6: launch.expected_core = {"Mega?Drive", 10}; break;
		case 7: launch.expected_core = {core, 65}; break;
		case 8: launch.content.extension = {extension, 17}; break;
		case 9: launch.content.extension = {"?", 1}; break;
		default: assert(false);
		}
		assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
		assert(fake.load_calls == 0 && fake.observe_calls == 0);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}
	for (unsigned kind = 0; kind < 4; ++kind) {
		FakePlatform fake = make_fake();
		runtime = create_in_state(&fake, MISTER_STATE_READY);
		MisterLaunchV2 launch = valid_launch();
		if (kind == 0) launch.game_id = {game, 128};
		if (kind == 1) launch.system = {system, 32};
		if (kind == 2) launch.expected_core = {core, 64};
		if (kind == 3) launch.content.extension = {extension, 16};
		assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
		assert(fake.load_calls == 1 && fake.observe_calls == 1);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}
	FakePlatform numeric_fake = make_fake();
	runtime = create_in_state(&numeric_fake, MISTER_STATE_READY);
	MisterLaunchV2 numeric_launch = valid_launch();
	numeric_launch.content.extension = {"7z", 2};
	assert(MisterRuntime_LoadV2(runtime, &numeric_launch, 1) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
}

static void test_platform_output_guards_individually()
{
	for (unsigned mutation = 1; mutation <= 11; ++mutation) {
		FakePlatform fake = make_fake();
		fake.observation_output_mutation = mutation;
		MisterRuntime *runtime = create(&fake);
		MisterObservationV2 observation = initialized_observation();
		assert(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_PLATFORM);
		assert(fake.observe_calls == 1);
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}
	for (unsigned mutation = 1; mutation <= 10; ++mutation) {
		FakePlatform fake = make_fake();
		fake.observed_resources = 0;
		fake.neutral_resources = MISTER_RESOURCE_FPGA;
		fake.recovery_output_mutation = mutation;
		MisterRecoveryObservationV2 recovery = initialized_recovery();
		assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
			&recovery, 1) == MISTER_RESULT_PLATFORM);
		assert(fake.recover_calls == 1);
	}
}

static void test_load_observation_and_primary_result()
{
	FakePlatform fake = make_fake();
	fake.observed_resources = MISTER_RESOURCE_FPGA;
	MisterRuntime *runtime = create(&fake);
	MisterLaunchV2 launch = valid_launch();
	assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
	assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_PLATFORM);
	expect_status(runtime, MISTER_STATE_FAILED, MISTER_RESULT_PLATFORM,
		MISTER_RESULT_PLATFORM, MISTER_RESULT_OK, 0);
	fake.stop_result = MISTER_RESULT_DEADLINE;
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_DEADLINE);
	expect_status(runtime, MISTER_STATE_CLEANUP_INCOMPLETE, MISTER_RESULT_DEADLINE,
		MISTER_RESULT_PLATFORM, MISTER_RESULT_DEADLINE, 0);
	fake.stop_result = MISTER_RESULT_OK;
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
}

static void test_callback_exception_and_malformed_outputs_are_contained()
{
	FakePlatform throwing_fake = make_fake();
	throwing_fake.throw_callback = 1;
	MisterRuntime *runtime = create(&throwing_fake);
	assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_PLATFORM);
	expect_status(runtime, MISTER_STATE_FAILED, MISTER_RESULT_PLATFORM,
		MISTER_RESULT_PLATFORM, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform observation_fake = make_fake();
	observation_fake.malformed_observation = true;
	runtime = create(&observation_fake);
	MisterObservationV2 observation = initialized_observation();
	assert(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_PLATFORM);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform recovery_fake = make_fake();
	recovery_fake.malformed_recovery = true;
	MisterRecoveryObservationV2 recovery = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&recovery_fake.platform,
		MISTER_RESOURCE_FPGA, &recovery, 1) == MISTER_RESULT_PLATFORM);
}

static void test_every_callback_exception_is_contained()
{
	MisterLaunchV2 launch = valid_launch();
	for (unsigned callback = 2; callback <= 6; ++callback) {
		FakePlatform fake = make_fake();
		fake.throw_callback = callback;
		MisterRuntime *runtime = nullptr;
		if (callback == 6) {
			fake.platform.context = &fake;
			fake.observed_resources = 0;
			fake.neutral_resources = MISTER_RESOURCE_FPGA;
			MisterRecoveryObservationV2 recovery = initialized_recovery();
			assert(MisterRuntime_RecoverPlatformV2(&fake.platform,
				MISTER_RESOURCE_FPGA, &recovery, 1) == MISTER_RESULT_PLATFORM);
			fake.throw_callback = 0;
			recovery = initialized_recovery();
			assert(MisterRuntime_RecoverPlatformV2(&fake.platform,
				MISTER_RESOURCE_FPGA, &recovery, 1) == MISTER_RESULT_OK);
			continue;
		}
		runtime = create(&fake);
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		if (callback == 2) {
			assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_PLATFORM);
		} else if (callback == 3) {
			fake.throw_callback = 0;
			assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
			fake.throw_callback = callback;
			assert(MisterRuntime_TickV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		} else if (callback == 4) {
			MisterObservationV2 observation = initialized_observation();
			assert(MisterRuntime_ObserveV2(runtime, &observation, 1) ==
				MISTER_RESULT_PLATFORM);
			MisterStatusV2 status = initialized_status();
			assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
			assert(status.state == MISTER_STATE_READY);
		} else {
			assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_PLATFORM);
			expect_status(runtime, MISTER_STATE_CLEANUP_INCOMPLETE,
				MISTER_RESULT_PLATFORM, MISTER_RESULT_OK, MISTER_RESULT_PLATFORM, 0);
		}
		fake.throw_callback = 0;
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}
}

static void test_stop_result_mapping_and_destroy_retention()
{
	static const MisterResult results[] = {
		MISTER_RESULT_OK, MISTER_RESULT_INVALID_ARGUMENT, MISTER_RESULT_INVALID_STATE,
		MISTER_RESULT_UNSUPPORTED, MISTER_RESULT_DEADLINE, MISTER_RESULT_PLATFORM,
		MISTER_RESULT_CLEANUP_INCOMPLETE
	};
	for (unsigned index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
		FakePlatform fake = make_fake();
		fake.stop_result = results[index];
		MisterRuntime *runtime = create(&fake);
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		assert(MisterRuntime_StopV2(runtime, 1) == results[index]);
		if (results[index] == MISTER_RESULT_OK) {
			expect_status(runtime, MISTER_STATE_STOPPED, MISTER_RESULT_OK,
				MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
			destroy_stopped(&runtime);
		} else {
			expect_status(runtime, MISTER_STATE_CLEANUP_INCOMPLETE, results[index],
				MISTER_RESULT_OK, results[index], 0);
			fake.stop_result = MISTER_RESULT_OK;
			assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
			destroy_stopped(&runtime);
		}
	}
	static FakePlatform exit_fake;
	exit_fake = make_fake();
	exit_fake.stop_result = MISTER_RESULT_EXIT_REQUIRED;
	MisterRuntime *runtime = create(&exit_fake);
	assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_EXIT_REQUIRED);
	expect_status(runtime, MISTER_STATE_EXIT_REQUIRED, MISTER_RESULT_EXIT_REQUIRED,
		MISTER_RESULT_OK, MISTER_RESULT_EXIT_REQUIRED, 0);
	assert(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_INVALID_STATE);
	assert(runtime != nullptr);
}

static MisterRuntime *create_in_state(FakePlatform *fake, uint32_t state)
{
	MisterRuntime *runtime = create(fake);
	MisterLaunchV2 launch = valid_launch();
	switch (state) {
	case MISTER_STATE_CREATED:
		break;
	case MISTER_STATE_READY:
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		break;
	case MISTER_STATE_RUNNING:
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		assert(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
		break;
	case MISTER_STATE_FAILED:
		fake->start_result = MISTER_RESULT_PLATFORM;
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		break;
	case MISTER_STATE_CLEANUP_INCOMPLETE:
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		fake->stop_result = MISTER_RESULT_DEADLINE;
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_DEADLINE);
		break;
	case MISTER_STATE_EXIT_REQUIRED:
		assert(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		fake->stop_result = MISTER_RESULT_EXIT_REQUIRED;
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_EXIT_REQUIRED);
		break;
	case MISTER_STATE_STOPPED:
		assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		break;
	default:
		assert(false);
	}
	return runtime;
}

static void release_matrix_runtime(FakePlatform *fake, MisterRuntime **runtime)
{
	if (*runtime == nullptr) return;
	MisterStatusV2 status = initialized_status();
	assert(MisterRuntime_StatusV2(*runtime, &status) == MISTER_RESULT_OK);
	if (status.state == MISTER_STATE_EXIT_REQUIRED) return;
	if (status.state == MISTER_STATE_CLEANUP_INCOMPLETE) fake->stop_result = MISTER_RESULT_OK;
	if (status.state != MISTER_STATE_CREATED && status.state != MISTER_STATE_STOPPED) {
		assert(MisterRuntime_StopV2(*runtime, 1) == MISTER_RESULT_OK);
	}
	destroy_stopped(runtime);
}

static void test_legal_and_illegal_state_pairs()
{
	static const uint32_t states[] = {
		MISTER_STATE_CREATED, MISTER_STATE_READY, MISTER_STATE_RUNNING,
		MISTER_STATE_FAILED, MISTER_STATE_CLEANUP_INCOMPLETE, MISTER_STATE_STOPPED
	};
	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create_in_state(&fake, states[index]);
		unsigned callback_count = fake.start_calls + fake.load_calls + fake.tick_calls +
			fake.observe_calls + fake.stop_calls;
		MisterResult result = MisterRuntime_StartV2(runtime, 1);
		assert(result == (states[index] == MISTER_STATE_CREATED ?
			MISTER_RESULT_OK : MISTER_RESULT_INVALID_STATE));
		if (states[index] != MISTER_STATE_CREATED) {
			assert(fake.start_calls + fake.load_calls + fake.tick_calls + fake.observe_calls +
				fake.stop_calls == callback_count);
		}
		release_matrix_runtime(&fake, &runtime);
	}

	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create_in_state(&fake, states[index]);
		MisterLaunchV2 launch = valid_launch();
		MisterResult result = MisterRuntime_LoadV2(runtime, &launch, 1);
		assert(result == (states[index] == MISTER_STATE_READY ?
			MISTER_RESULT_OK : MISTER_RESULT_INVALID_STATE));
		release_matrix_runtime(&fake, &runtime);
	}

	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create_in_state(&fake, states[index]);
		MisterResult result = MisterRuntime_TickV2(runtime, 1);
		assert(result == (states[index] == MISTER_STATE_RUNNING ?
			MISTER_RESULT_OK : MISTER_RESULT_INVALID_STATE));
		release_matrix_runtime(&fake, &runtime);
	}

	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create_in_state(&fake, states[index]);
		MisterObservationV2 observation = initialized_observation();
		assert(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_OK);
		MisterStatusV2 status = initialized_status();
		assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
		release_matrix_runtime(&fake, &runtime);
	}

	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create_in_state(&fake, states[index]);
		if (states[index] == MISTER_STATE_CLEANUP_INCOMPLETE) {
			fake.stop_result = MISTER_RESULT_OK;
		}
		MisterResult result = MisterRuntime_StopV2(runtime, 1);
		assert(result == MISTER_RESULT_OK);
		destroy_stopped(&runtime);
	}

	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		FakePlatform fake = make_fake();
		MisterRuntime *runtime = create_in_state(&fake, states[index]);
		MisterResult result = MisterRuntime_DestroyV2(&runtime);
		bool legal = states[index] == MISTER_STATE_CREATED || states[index] == MISTER_STATE_STOPPED;
		assert(result == (legal ? MISTER_RESULT_OK : MISTER_RESULT_INVALID_STATE));
		if (!legal) release_matrix_runtime(&fake, &runtime);
	}

	static FakePlatform exit_fake;
	exit_fake = make_fake();
	MisterRuntime *exit_runtime = create_in_state(&exit_fake, MISTER_STATE_EXIT_REQUIRED);
	MisterLaunchV2 launch = valid_launch();
	assert(MisterRuntime_StartV2(exit_runtime, 1) == MISTER_RESULT_INVALID_STATE);
	assert(MisterRuntime_LoadV2(exit_runtime, &launch, 1) == MISTER_RESULT_INVALID_STATE);
	assert(MisterRuntime_TickV2(exit_runtime, 1) == MISTER_RESULT_INVALID_STATE);
	MisterObservationV2 observation = initialized_observation();
	assert(MisterRuntime_ObserveV2(exit_runtime, &observation, 1) == MISTER_RESULT_OK);
	MisterStatusV2 status = initialized_status();
	assert(MisterRuntime_StatusV2(exit_runtime, &status) == MISTER_RESULT_OK);
	assert(MisterRuntime_StopV2(exit_runtime, 1) == MISTER_RESULT_INVALID_STATE);
	assert(MisterRuntime_DestroyV2(&exit_runtime) == MISTER_RESULT_INVALID_STATE);
}

static void test_invalid_state_precedes_deadline_and_input_validation()
{
	FakePlatform stopped_fake = make_fake();
	MisterRuntime *runtime = create_in_state(&stopped_fake, MISTER_STATE_STOPPED);
	assert(MisterRuntime_StartV2(runtime, 0) == MISTER_RESULT_INVALID_STATE);
	assert(stopped_fake.start_calls == 0);
	expect_status(runtime, MISTER_STATE_STOPPED, MISTER_RESULT_INVALID_STATE,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	destroy_stopped(&runtime);

	FakePlatform created_fake = make_fake();
	runtime = create_in_state(&created_fake, MISTER_STATE_CREATED);
	assert(MisterRuntime_LoadV2(runtime, nullptr, 0) == MISTER_RESULT_INVALID_STATE);
	assert(created_fake.load_calls == 0);
	expect_status(runtime, MISTER_STATE_CREATED, MISTER_RESULT_INVALID_STATE,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	destroy_stopped(&runtime);

	FakePlatform ready_fake = make_fake();
	runtime = create_in_state(&ready_fake, MISTER_STATE_READY);
	assert(MisterRuntime_TickV2(runtime, 0) == MISTER_RESULT_INVALID_STATE);
	assert(ready_fake.tick_calls == 0);
	expect_status(runtime, MISTER_STATE_READY, MISTER_RESULT_INVALID_STATE,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	static FakePlatform exit_fake;
	exit_fake = make_fake();
	runtime = create_in_state(&exit_fake, MISTER_STATE_EXIT_REQUIRED);
	assert(MisterRuntime_StopV2(runtime, 0) == MISTER_RESULT_INVALID_STATE);
	assert(exit_fake.stop_calls == 1);
	expect_status(runtime, MISTER_STATE_EXIT_REQUIRED, MISTER_RESULT_INVALID_STATE,
		MISTER_RESULT_OK, MISTER_RESULT_EXIT_REQUIRED, 0);
	assert(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_INVALID_STATE);
}

static void test_legal_state_zero_deadline_rejects_without_dispatch()
{
	FakePlatform load_fake = make_fake();
	MisterRuntime *runtime = create_in_state(&load_fake, MISTER_STATE_READY);
	MisterLaunchV2 launch = valid_launch();
	assert(MisterRuntime_LoadV2(runtime, &launch, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(load_fake.load_calls == 0 && load_fake.observe_calls == 0);
	expect_status(runtime, MISTER_STATE_READY, MISTER_RESULT_INVALID_ARGUMENT,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform tick_fake = make_fake();
	runtime = create_in_state(&tick_fake, MISTER_STATE_RUNNING);
	assert(MisterRuntime_TickV2(runtime, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(tick_fake.tick_calls == 0);
	expect_status(runtime, MISTER_STATE_RUNNING, MISTER_RESULT_INVALID_ARGUMENT,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);

	FakePlatform stop_fake = make_fake();
	runtime = create_in_state(&stop_fake, MISTER_STATE_READY);
	assert(MisterRuntime_StopV2(runtime, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(stop_fake.stop_calls == 0);
	expect_status(runtime, MISTER_STATE_READY, MISTER_RESULT_INVALID_ARGUMENT,
		MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
}

static unsigned callback_count(const FakePlatform &fake)
{
	return fake.start_calls + fake.load_calls + fake.tick_calls + fake.observe_calls +
		fake.stop_calls + fake.recover_calls;
}

static void expect_callback_delta(const FakePlatform &before, const FakePlatform &after,
	unsigned start, unsigned load, unsigned tick, unsigned observe, unsigned stop)
{
	assert(after.start_calls == before.start_calls + start);
	assert(after.load_calls == before.load_calls + load);
	assert(after.tick_calls == before.tick_calls + tick);
	assert(after.observe_calls == before.observe_calls + observe);
	assert(after.stop_calls == before.stop_calls + stop);
	assert(after.recover_calls == before.recover_calls);
}

static MisterStatusV2 status_of(MisterRuntime *runtime)
{
	MisterStatusV2 status = initialized_status();
	assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
	return status;
}

static void expect_illegal_state(MisterRuntime *runtime, const MisterStatusV2 &before)
{
	MisterStatusV2 after = status_of(runtime);
	assert(after.state == before.state);
	assert(after.last_result == MISTER_RESULT_INVALID_STATE);
	assert(after.primary_result == before.primary_result);
	assert(after.cleanup_result == before.cleanup_result);
	assert(after.tick_count == before.tick_count);
}

static void test_full_v2_call_state_matrix()
{
	static FakePlatform start_fakes[7];
	static FakePlatform load_fakes[7];
	static FakePlatform tick_fakes[7];
	static FakePlatform observe_fakes[7];
	static FakePlatform status_fakes[7];
	static FakePlatform stop_fakes[7];
	static FakePlatform destroy_fakes[7];
	static const uint32_t states[] = {
		MISTER_STATE_CREATED, MISTER_STATE_READY, MISTER_STATE_RUNNING,
		MISTER_STATE_FAILED, MISTER_STATE_CLEANUP_INCOMPLETE,
		MISTER_STATE_EXIT_REQUIRED, MISTER_STATE_STOPPED
	};
	for (unsigned index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
		const uint32_t state = states[index];
		MisterLaunchV2 launch = valid_launch();

		FakePlatform &start_fake = start_fakes[index];
		start_fake = make_fake();
		MisterRuntime *runtime = create_in_state(&start_fake, state);
		MisterStatusV2 before = status_of(runtime);
		unsigned calls = callback_count(start_fake);
		FakePlatform before_callbacks = start_fake;
		MisterResult result = MisterRuntime_StartV2(runtime, 1);
		if (state == MISTER_STATE_CREATED) {
			assert(result == MISTER_RESULT_OK && start_fake.start_calls == 1);
			expect_callback_delta(before_callbacks, start_fake, 1, 0, 0, 0, 0);
			expect_status(runtime, MISTER_STATE_READY, MISTER_RESULT_OK,
				MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
		} else {
			assert(result == MISTER_RESULT_INVALID_STATE && callback_count(start_fake) == calls);
			expect_illegal_state(runtime, before);
		}
		release_matrix_runtime(&start_fake, &runtime);

		FakePlatform &load_fake = load_fakes[index];
		load_fake = make_fake();
		runtime = create_in_state(&load_fake, state);
		before = status_of(runtime);
		calls = callback_count(load_fake);
		before_callbacks = load_fake;
		result = MisterRuntime_LoadV2(runtime, &launch, 1);
		if (state == MISTER_STATE_READY) {
			assert(result == MISTER_RESULT_OK && load_fake.load_calls == 1 &&
				load_fake.observe_calls == 1);
			expect_callback_delta(before_callbacks, load_fake, 0, 1, 0, 1, 0);
			expect_status(runtime, MISTER_STATE_RUNNING, MISTER_RESULT_OK,
				MISTER_RESULT_OK, MISTER_RESULT_OK, 0);
		} else {
			assert(result == MISTER_RESULT_INVALID_STATE && callback_count(load_fake) == calls);
			expect_illegal_state(runtime, before);
		}
		release_matrix_runtime(&load_fake, &runtime);

		FakePlatform &tick_fake = tick_fakes[index];
		tick_fake = make_fake();
		runtime = create_in_state(&tick_fake, state);
		before = status_of(runtime);
		calls = callback_count(tick_fake);
		before_callbacks = tick_fake;
		result = MisterRuntime_TickV2(runtime, 1);
		if (state == MISTER_STATE_RUNNING) {
			assert(result == MISTER_RESULT_OK && tick_fake.tick_calls == 1);
			expect_callback_delta(before_callbacks, tick_fake, 0, 0, 1, 0, 0);
			expect_status(runtime, MISTER_STATE_RUNNING, MISTER_RESULT_OK,
				MISTER_RESULT_OK, MISTER_RESULT_OK, 1);
		} else {
			assert(result == MISTER_RESULT_INVALID_STATE && callback_count(tick_fake) == calls);
			expect_illegal_state(runtime, before);
		}
		release_matrix_runtime(&tick_fake, &runtime);

		FakePlatform &observe_fake = observe_fakes[index];
		observe_fake = make_fake();
		runtime = create_in_state(&observe_fake, state);
		before = status_of(runtime);
		calls = callback_count(observe_fake);
		before_callbacks = observe_fake;
		MisterObservationV2 observation = initialized_observation();
		assert(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_OK);
		assert(callback_count(observe_fake) == calls + 1);
		expect_callback_delta(before_callbacks, observe_fake, 0, 0, 0, 1, 0);
		MisterStatusV2 after_observe = status_of(runtime);
		assert(after_observe.state == before.state && after_observe.last_result == before.last_result &&
			after_observe.primary_result == before.primary_result &&
			after_observe.cleanup_result == before.cleanup_result &&
			after_observe.tick_count == before.tick_count);
		release_matrix_runtime(&observe_fake, &runtime);

		FakePlatform &status_fake = status_fakes[index];
		status_fake = make_fake();
		runtime = create_in_state(&status_fake, state);
		before = status_of(runtime);
		calls = callback_count(status_fake);
		before_callbacks = status_fake;
		MisterStatusV2 status = initialized_status();
		assert(MisterRuntime_StatusV2(runtime, &status) == MISTER_RESULT_OK);
		assert(status.state == before.state && status.last_result == before.last_result &&
			status.primary_result == before.primary_result &&
			status.cleanup_result == before.cleanup_result && status.tick_count == before.tick_count);
		assert(callback_count(status_fake) == calls);
		expect_callback_delta(before_callbacks, status_fake, 0, 0, 0, 0, 0);
		release_matrix_runtime(&status_fake, &runtime);

		FakePlatform &stop_fake = stop_fakes[index];
		stop_fake = make_fake();
		runtime = create_in_state(&stop_fake, state);
		if (state == MISTER_STATE_CLEANUP_INCOMPLETE) stop_fake.stop_result = MISTER_RESULT_OK;
		before = status_of(runtime);
		calls = callback_count(stop_fake);
		before_callbacks = stop_fake;
		result = MisterRuntime_StopV2(runtime, 1);
		if (state == MISTER_STATE_EXIT_REQUIRED) {
			assert(result == MISTER_RESULT_INVALID_STATE && callback_count(stop_fake) == calls);
			expect_illegal_state(runtime, before);
		} else {
			assert(result == MISTER_RESULT_OK);
			expect_callback_delta(before_callbacks, stop_fake, 0, 0, 0, 0,
				(state == MISTER_STATE_CREATED || state == MISTER_STATE_STOPPED) ? 0 : 1);
			expect_status(runtime, MISTER_STATE_STOPPED, MISTER_RESULT_OK,
				static_cast<MisterResult>(before.primary_result), MISTER_RESULT_OK,
				before.tick_count);
			destroy_stopped(&runtime);
		}

		FakePlatform &destroy_fake = destroy_fakes[index];
		destroy_fake = make_fake();
		runtime = create_in_state(&destroy_fake, state);
		before = status_of(runtime);
		calls = callback_count(destroy_fake);
		before_callbacks = destroy_fake;
		result = MisterRuntime_DestroyV2(&runtime);
		if (state == MISTER_STATE_CREATED || state == MISTER_STATE_STOPPED) {
			assert(result == MISTER_RESULT_OK && runtime == nullptr);
			expect_callback_delta(before_callbacks, destroy_fake, 0, 0, 0, 0, 0);
		} else {
			assert(result == MISTER_RESULT_INVALID_STATE && runtime != nullptr &&
				callback_count(destroy_fake) == calls);
			expect_illegal_state(runtime, before);
			release_matrix_runtime(&destroy_fake, &runtime);
		}
	}
}

static void test_recovery_is_stateless_and_neutral()
{
	FakePlatform fake = make_fake();
	fake.platform.context = &fake;
	MisterRecoveryObservationV2 observation = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(nullptr, MISTER_RESOURCE_FPGA, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA, nullptr, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	observation.abi_version--;
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	observation = initialized_recovery();
	observation.reserved[0] = 1;
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	observation = initialized_recovery();
	observation.struct_size--;
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	observation = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, 0, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform,
		MISTER_RESOURCE_FPGA | (1u << 16), &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&observation, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	observation = initialized_recovery();
	fake.observed_resources = MISTER_RESOURCE_FPGA;
	fake.neutral_resources = MISTER_RESOURCE_FPGA;
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&observation, 3) == MISTER_RESULT_PLATFORM);
	assert(fake.recover_calls == 1 && fake.last_deadline == 3);
	observation = initialized_recovery();
	fake.observed_resources = 0;
	fake.neutral_resources = MISTER_RESOURCE_FPGA;
	fake.try_create_during_recover = true;
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&observation, 5) == MISTER_RESULT_OK);
	assert(fake.recover_calls == 2);
	observation = initialized_recovery();
	fake.recover_result = MisterRuntime_RawResultFixture(99);
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&observation, 1) == MISTER_RESULT_PLATFORM);
	observation = initialized_recovery();
	fake.recover_result = MISTER_RESULT_DEADLINE;
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&observation, 1) == MISTER_RESULT_DEADLINE);
	fake.recover_result = MISTER_RESULT_OK;
	MisterRuntime *runtime = create(&fake);
	observation = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, MISTER_RESOURCE_FPGA,
		&observation, 1) == MISTER_RESULT_INVALID_STATE);
	assert(fake.recover_calls == 4);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
}

static void test_recovery_partial_progress_is_monotonic_across_non_ok_results()
{
	FakePlatform fake = make_fake();
	const uint32_t required = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	fake.observed_resources = 0;
	fake.neutral_resources = MISTER_RESOURCE_FPGA;
	fake.recover_result = MISTER_RESULT_DEADLINE;
	MisterRecoveryObservationV2 first = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, required, &first, 1) ==
		MISTER_RESULT_DEADLINE);
	assert(first.observed_resource_flags == 0);
	assert(first.neutral_resource_flags == MISTER_RESOURCE_FPGA);

	fake.neutral_resources = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES;
	fake.recover_result = MISTER_RESULT_PLATFORM;
	MisterRecoveryObservationV2 second = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, required, &second, 1) ==
		MISTER_RESULT_PLATFORM);
	assert(second.observed_resource_flags == 0);
	assert((second.neutral_resource_flags & first.neutral_resource_flags) ==
		first.neutral_resource_flags);
	assert(second.neutral_resource_flags ==
		(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES));

	fake.recover_result = MISTER_RESULT_OK;
	fake.neutral_resources = required;
	MisterRecoveryObservationV2 final = initialized_recovery();
	assert(MisterRuntime_RecoverPlatformV2(&fake.platform, required, &final, 1) ==
		MISTER_RESULT_OK);
	assert((final.neutral_resource_flags & second.neutral_resource_flags) ==
		second.neutral_resource_flags);
	assert(final.neutral_resource_flags == required && final.observed_resource_flags == 0);
	MisterRuntime *runtime = create(&fake);
	assert(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
	destroy_stopped(&runtime);
}

int main()
{
	test_create_validation_and_cross_generation_rejection();
	test_cross_generation_handles_are_rejected();
	test_validation_and_successful_lifecycle();
	test_registry_failure_paths_and_raw_invalid_results();
	test_observe_and_status_initialization();
	test_each_v2_structure_guard_rejects_one_mutation();
	test_string_and_null_input_guards();
	test_platform_output_guards_individually();
	test_load_observation_and_primary_result();
	test_callback_exception_and_malformed_outputs_are_contained();
	test_every_callback_exception_is_contained();
	test_stop_result_mapping_and_destroy_retention();
	test_legal_and_illegal_state_pairs();
	test_invalid_state_precedes_deadline_and_input_validation();
	test_legal_state_zero_deadline_rejects_without_dispatch();
	test_full_v2_call_state_matrix();
	test_recovery_is_stateless_and_neutral();
	test_recovery_partial_progress_is_monotonic_across_non_ok_results();
	return 0;
}
