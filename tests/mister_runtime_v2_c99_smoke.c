/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime.h"

#include <stddef.h>
#include <string.h>

#define MISTER_C99_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]

MISTER_C99_ASSERT(v2_string_view_width, sizeof(MisterStringView) >= sizeof(char *) + 4);
MISTER_C99_ASSERT(v2_content_prefix, offsetof(MisterContentRefV2, abi_version) == 0);
MISTER_C99_ASSERT(v2_launch_prefix, offsetof(MisterLaunchV2, abi_version) == 0);
MISTER_C99_ASSERT(v2_observation_prefix, offsetof(MisterObservationV2, abi_version) == 0);
MISTER_C99_ASSERT(v2_recovery_prefix, offsetof(MisterRecoveryObservationV2, abi_version) == 0);
MISTER_C99_ASSERT(v2_platform_prefix, offsetof(MisterPlatformV2, abi_version) == 0);
MISTER_C99_ASSERT(v2_status_state_offset, offsetof(MisterStatusV2, state) == 8);
MISTER_C99_ASSERT(v2_status_ticks_offset, offsetof(MisterStatusV2, tick_count) == 24);
MISTER_C99_ASSERT(v2_result_representation, sizeof(MisterResult) == sizeof(int32_t));

#if defined(MISTER_RUNTIME_RAW_RESULT_FIXTURE)
MisterResult MisterRuntime_RawResultFixture(uint32_t raw)
{
	MisterResult result;
	memcpy(&result, &raw, sizeof(result));
	return result;
}
#elif defined(MISTER_RUNTIME_PRODUCTION_SANITIZE_FIXTURE)
static uint32_t raw_start;
static uint32_t raw_load;
static uint32_t raw_tick;
static uint32_t raw_observe;
static uint32_t raw_stop;
static uint32_t raw_recover;

static MisterResult raw_result(uint32_t raw)
{
	MisterResult result;
	memcpy(&result, &raw, sizeof(result));
	return result;
}

static MisterResult production_start(void *context, uint32_t deadline_ms)
{
	(void)context;
	(void)deadline_ms;
	return raw_result(raw_start);
}

static MisterResult production_load(void *context, const MisterLaunchV2 *launch,
	uint32_t deadline_ms)
{
	(void)context;
	(void)launch;
	(void)deadline_ms;
	return raw_result(raw_load);
}

static MisterResult production_tick(void *context, uint32_t deadline_ms)
{
	(void)context;
	(void)deadline_ms;
	return raw_result(raw_tick);
}

static MisterResult production_observe(void *context, MisterObservationV2 *observation,
	uint32_t deadline_ms)
{
	(void)context;
	(void)deadline_ms;
	if (raw_observe == 0) {
		observation->ready = 1;
		observation->resource_flags = MISTER_RESOURCE_V2_KNOWN;
	}
	return raw_result(raw_observe);
}

static MisterResult production_stop(void *context, uint32_t deadline_ms)
{
	(void)context;
	(void)deadline_ms;
	return raw_result(raw_stop);
}

static MisterResult production_recover(void *context, uint32_t resources,
	MisterRecoveryObservationV2 *observation, uint32_t deadline_ms)
{
	(void)context;
	(void)deadline_ms;
	if (raw_recover == 0) {
		observation->observed_resource_flags = 0;
		observation->neutral_resource_flags = resources;
	}
	return raw_result(raw_recover);
}

static MisterPlatformV2 production_platform(void)
{
	MisterPlatformV2 platform;
	memset(&platform, 0, sizeof(platform));
	platform.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	platform.struct_size = sizeof(platform);
	platform.capability_flags = MISTER_CAP_V2_KNOWN;
	platform.context = NULL;
	platform.start = production_start;
	platform.load = production_load;
	platform.tick = production_tick;
	platform.observe = production_observe;
	platform.stop = production_stop;
	platform.recover = production_recover;
	return platform;
}

static MisterLaunchV2 production_launch(void)
{
	static const char game[] = "sonic";
	static const char system[] = "megadrive";
	static const char core[] = "MegaDrive";
	static const char digest[] =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static const char extension[] = "md";
	MisterLaunchV2 launch;
	memset(&launch, 0, sizeof(launch));
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id.data = game;
	launch.game_id.length = sizeof(game) - 1;
	launch.system.data = system;
	launch.system.length = sizeof(system) - 1;
	launch.expected_core.data = core;
	launch.expected_core.length = sizeof(core) - 1;
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256.data = digest;
	launch.content.sha256.length = sizeof(digest) - 1;
	launch.content.size = 1;
	launch.content.extension.data = extension;
	launch.content.extension.length = sizeof(extension) - 1;
	return launch;
}

#define CHECK(expression) do { if (!(expression)) return 1; } while (0)

int main(void)
{
	static const uint32_t invalid_results[] = {8u, 99u, UINT32_MAX};
	unsigned index;
	for (index = 0; index < sizeof(invalid_results) / sizeof(invalid_results[0]); ++index) {
		MisterPlatformV2 platform = production_platform();
		MisterRuntime *runtime = NULL;
		MisterLaunchV2 launch = production_launch();
		MisterObservationV2 observation;
		MisterRecoveryObservationV2 recovery;
		uint32_t raw = invalid_results[index];

		raw_start = raw;
		raw_load = raw_tick = raw_observe = raw_stop = raw_recover = 0;
		CHECK(MisterRuntime_CreateV2(&platform, &runtime) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		raw_stop = 0;
		CHECK(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_OK);

		raw_start = 0;
		raw_load = raw;
		CHECK(MisterRuntime_CreateV2(&platform, &runtime) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_PLATFORM);
		CHECK(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_OK);

		raw_load = 0;
		raw_tick = raw;
		CHECK(MisterRuntime_CreateV2(&platform, &runtime) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_LoadV2(runtime, &launch, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_TickV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		CHECK(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_OK);

		raw_tick = 0;
		raw_observe = raw;
		memset(&observation, 0, sizeof(observation));
		observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		observation.struct_size = sizeof(observation);
		CHECK(MisterRuntime_CreateV2(&platform, &runtime) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_ObserveV2(runtime, &observation, 1) == MISTER_RESULT_PLATFORM);
		CHECK(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_OK);

		raw_observe = 0;
		raw_stop = raw;
		CHECK(MisterRuntime_CreateV2(&platform, &runtime) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_StartV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_PLATFORM);
		raw_stop = 0;
		CHECK(MisterRuntime_StopV2(runtime, 1) == MISTER_RESULT_OK);
		CHECK(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_OK);

		raw_recover = raw;
		memset(&recovery, 0, sizeof(recovery));
		recovery.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		recovery.struct_size = sizeof(recovery);
		CHECK(MisterRuntime_RecoverPlatformV2(&platform, MISTER_RESOURCE_FPGA,
			&recovery, 1) == MISTER_RESULT_PLATFORM);
		raw_recover = 0;
		memset(&recovery, 0, sizeof(recovery));
		recovery.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		recovery.struct_size = sizeof(recovery);
		CHECK(MisterRuntime_RecoverPlatformV2(&platform, MISTER_RESOURCE_FPGA,
			&recovery, 1) == MISTER_RESULT_OK);
	}
	return 0;
}
#else
int main(void)
{
	uint32_t (*abi_version)(void) = &MisterRuntime_ABIVersionV2;
	MisterResult (*create)(const MisterPlatformV2 *, MisterRuntime **) =
		&MisterRuntime_CreateV2;
	MisterResult (*start)(MisterRuntime *, uint32_t) = &MisterRuntime_StartV2;
	MisterResult (*load)(MisterRuntime *, const MisterLaunchV2 *, uint32_t) =
		&MisterRuntime_LoadV2;
	MisterResult (*tick)(MisterRuntime *, uint32_t) = &MisterRuntime_TickV2;
	MisterResult (*observe)(MisterRuntime *, MisterObservationV2 *, uint32_t) =
		&MisterRuntime_ObserveV2;
	MisterResult (*status)(const MisterRuntime *, MisterStatusV2 *) =
		&MisterRuntime_StatusV2;
	MisterResult (*stop)(MisterRuntime *, uint32_t) = &MisterRuntime_StopV2;
	MisterResult (*destroy)(MisterRuntime **) = &MisterRuntime_DestroyV2;
	MisterResult (*recover)(const MisterPlatformV2 *, uint32_t,
		MisterRecoveryObservationV2 *, uint32_t) = &MisterRuntime_RecoverPlatformV2;

	return abi_version != NULL && create != NULL && start != NULL && load != NULL &&
		tick != NULL && observe != NULL && status != NULL && stop != NULL &&
		destroy != NULL && recover != NULL ? 0 : 1;
}
#endif
