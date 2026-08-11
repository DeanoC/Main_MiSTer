/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime.h"

#include <cstddef>
#include <cstdint>

static_assert(sizeof(MisterStringView) >= sizeof(char *) + 4, "v2 string view width");
static_assert(offsetof(MisterContentRefV2, abi_version) == 0, "v2 content prefix");
static_assert(offsetof(MisterLaunchV2, abi_version) == 0, "v2 launch prefix");
static_assert(offsetof(MisterObservationV2, abi_version) == 0, "v2 observation prefix");
static_assert(offsetof(MisterRecoveryObservationV2, abi_version) == 0, "v2 recovery prefix");
static_assert(offsetof(MisterPlatformV2, abi_version) == 0, "v2 platform prefix");
static_assert(offsetof(MisterStatusV2, tick_count) == 24, "v2 tick offset");
static_assert(sizeof(MisterResult) == sizeof(int32_t), "v2 result representation");

int main()
{
	auto abi_version = &MisterRuntime_ABIVersionV2;
	auto create = &MisterRuntime_CreateV2;
	auto start = &MisterRuntime_StartV2;
	auto load = &MisterRuntime_LoadV2;
	auto tick = &MisterRuntime_TickV2;
	auto observe = &MisterRuntime_ObserveV2;
	auto status = &MisterRuntime_StatusV2;
	auto stop = &MisterRuntime_StopV2;
	auto destroy = &MisterRuntime_DestroyV2;
	auto recover = &MisterRuntime_RecoverPlatformV2;
	return abi_version != nullptr && create != nullptr && start != nullptr &&
		load != nullptr && tick != nullptr && observe != nullptr && status != nullptr &&
		stop != nullptr && destroy != nullptr && recover != nullptr ? 0 : 1;
}
