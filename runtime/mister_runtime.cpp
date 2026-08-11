/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <stdlib.h>

struct MisterRuntime {
	uint32_t generation;
	MisterPlatform platform;
	uint32_t state;
	uint32_t last_error;
	uint32_t primary_error;
	uint32_t cleanup_error;
	uint64_t tick_count;
	uint32_t capability_flags;
};

static MisterStatus mister_runtime_null_status(void)
{
	MisterStatus status = {};
	status.abi_version = MISTER_RUNTIME_ABI_VERSION;
	status.struct_size = sizeof(status);
	status.state = MISTER_RUNTIME_FAILED;
	status.last_error = MISTER_RUNTIME_ERROR_INVALID_ARGUMENT;
	status.primary_error = MISTER_RUNTIME_ERROR_INVALID_ARGUMENT;
	return status;
}

static bool mister_runtime_valid_platform(const MisterPlatform *platform)
{
	return platform != nullptr &&
		platform->abi_version == MISTER_RUNTIME_ABI_VERSION &&
		platform->struct_size >= sizeof(MisterPlatform) &&
		(platform->capability_flags & ~MISTER_PLATFORM_CAP_KNOWN) == 0 &&
		platform->start != nullptr && platform->load != nullptr &&
		platform->tick != nullptr && platform->stop != nullptr;
}

static bool mister_runtime_valid_launch(const MisterLaunch *launch)
{
	return launch != nullptr &&
		launch->abi_version == MISTER_RUNTIME_ABI_VERSION &&
		launch->struct_size >= sizeof(MisterLaunch) &&
		launch->core_path != nullptr;
}

static void mister_runtime_set_primary_error(MisterRuntime *runtime,
	uint32_t error)
{
	if (runtime->primary_error == MISTER_RUNTIME_ERROR_NONE) {
		runtime->primary_error = error;
	}
}

extern "C" uint32_t MisterRuntime_ABIVersion(void)
{
	return MISTER_RUNTIME_ABI_VERSION;
}

extern "C" MisterRuntime *MisterRuntime_Create(const MisterPlatform *platform)
{
	if (!mister_runtime_valid_platform(platform)) {
		return nullptr;
	}

	MisterRuntime *runtime = static_cast<MisterRuntime *>(calloc(1, sizeof(*runtime)));
	if (runtime == nullptr) {
		return nullptr;
	}

	runtime->generation = MISTER_RUNTIME_GENERATION_V1;
	runtime->platform = *platform;
	runtime->state = MISTER_RUNTIME_CREATED;
	runtime->capability_flags = platform->capability_flags &
		MISTER_PLATFORM_CAP_CLEAN_STOP ? MISTER_RUNTIME_CAP_CLEAN_STOP : 0;
	return runtime;
}

extern "C" bool MisterRuntime_Start(MisterRuntime *runtime)
{
	if (MisterRuntime_ReadGeneration(runtime) != MISTER_RUNTIME_GENERATION_V1) {
		return false;
	}
	if (runtime->state != MISTER_RUNTIME_CREATED) {
		runtime->last_error = MISTER_RUNTIME_ERROR_INVALID_STATE;
		return false;
	}

	if (!runtime->platform.start(runtime->platform.context)) {
		runtime->state = MISTER_RUNTIME_FAILED;
		runtime->last_error = MISTER_RUNTIME_ERROR_PLATFORM_START;
		mister_runtime_set_primary_error(runtime, runtime->last_error);
		return false;
	}

	runtime->state = MISTER_RUNTIME_READY;
	runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
	return true;
}

extern "C" void MisterRuntime_Tick(MisterRuntime *runtime)
{
	if (MisterRuntime_ReadGeneration(runtime) != MISTER_RUNTIME_GENERATION_V1) {
		return;
	}
	if (runtime->state != MISTER_RUNTIME_RUNNING) {
		runtime->last_error = MISTER_RUNTIME_ERROR_INVALID_STATE;
		return;
	}

	if (!runtime->platform.tick(runtime->platform.context)) {
		runtime->state = MISTER_RUNTIME_FAILED;
		runtime->last_error = MISTER_RUNTIME_ERROR_PLATFORM_TICK;
		mister_runtime_set_primary_error(runtime, runtime->last_error);
		return;
	}

	++runtime->tick_count;
	runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
}

extern "C" bool MisterRuntime_Load(MisterRuntime *runtime,
	const MisterLaunch *launch)
{
	if (MisterRuntime_ReadGeneration(runtime) != MISTER_RUNTIME_GENERATION_V1) {
		return false;
	}
	if (runtime->state != MISTER_RUNTIME_READY) {
		runtime->last_error = MISTER_RUNTIME_ERROR_INVALID_STATE;
		return false;
	}
	if (!mister_runtime_valid_launch(launch)) {
		runtime->last_error = MISTER_RUNTIME_ERROR_INVALID_ARGUMENT;
		return false;
	}

	if (!runtime->platform.load(runtime->platform.context, launch)) {
		runtime->state = MISTER_RUNTIME_FAILED;
		runtime->last_error = MISTER_RUNTIME_ERROR_PLATFORM_LOAD;
		mister_runtime_set_primary_error(runtime, runtime->last_error);
		return false;
	}

	runtime->state = MISTER_RUNTIME_RUNNING;
	runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
	return true;
}

extern "C" MisterStatus MisterRuntime_Status(const MisterRuntime *runtime)
{
	if (MisterRuntime_ReadGeneration(runtime) != MISTER_RUNTIME_GENERATION_V1) {
		return mister_runtime_null_status();
	}

	MisterStatus status = {};
	status.abi_version = MISTER_RUNTIME_ABI_VERSION;
	status.struct_size = sizeof(status);
	status.state = runtime->state;
	status.last_error = runtime->last_error;
	status.primary_error = runtime->primary_error;
	status.cleanup_error = runtime->cleanup_error;
	status.tick_count = runtime->tick_count;
	status.capability_flags = runtime->capability_flags;
	return status;
}

extern "C" void MisterRuntime_Stop(MisterRuntime *runtime)
{
	if (MisterRuntime_ReadGeneration(runtime) != MISTER_RUNTIME_GENERATION_V1) {
		return;
	}

	if (runtime->state == MISTER_RUNTIME_CREATED) {
		runtime->state = MISTER_RUNTIME_STOPPED;
		runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
		return;
	}
	if (runtime->state == MISTER_RUNTIME_EXIT_REQUIRED ||
		runtime->state == MISTER_RUNTIME_STOPPED) {
		runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
		return;
	}
	if (runtime->state != MISTER_RUNTIME_READY &&
		runtime->state != MISTER_RUNTIME_RUNNING &&
		runtime->state != MISTER_RUNTIME_FAILED &&
		runtime->state != MISTER_RUNTIME_CLEANUP_FAILED) {
		runtime->last_error = MISTER_RUNTIME_ERROR_INVALID_STATE;
		return;
	}

	switch (runtime->platform.stop(runtime->platform.context)) {
	case MISTER_PLATFORM_RELEASED:
		runtime->state = MISTER_RUNTIME_STOPPED;
		runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
		runtime->cleanup_error = MISTER_RUNTIME_ERROR_NONE;
		return;
	case MISTER_PLATFORM_EXIT_REQUIRED:
		runtime->state = MISTER_RUNTIME_EXIT_REQUIRED;
		runtime->last_error = MISTER_RUNTIME_ERROR_NONE;
		return;
	case MISTER_PLATFORM_STOP_FAILED:
	default:
		runtime->state = MISTER_RUNTIME_CLEANUP_FAILED;
		runtime->last_error = MISTER_RUNTIME_ERROR_PLATFORM_STOP;
		runtime->cleanup_error = MISTER_RUNTIME_ERROR_PLATFORM_STOP;
		return;
	}
}

extern "C" bool MisterRuntime_Destroy(MisterRuntime **runtime)
{
	if (runtime == nullptr || *runtime == nullptr ||
		MisterRuntime_ReadGeneration(*runtime) != MISTER_RUNTIME_GENERATION_V1) {
		return false;
	}
	if ((*runtime)->state != MISTER_RUNTIME_CREATED &&
		(*runtime)->state != MISTER_RUNTIME_STOPPED) {
		(*runtime)->last_error = MISTER_RUNTIME_ERROR_INVALID_STATE;
		return false;
	}

	free(*runtime);
	*runtime = nullptr;
	return true;
}
