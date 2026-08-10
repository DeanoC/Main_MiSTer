/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime.h"

#include <cstddef>
#include <cstdint>

static_assert(sizeof(uint32_t) == 4, "V1 uint32_t size");
static_assert(sizeof(uint64_t) == 8, "V1 uint64_t size");
static_assert(sizeof(MisterStatus) == 48, "V1 status size");
static_assert(offsetof(MisterStatus, abi_version) == 0, "V1 abi_version offset");
static_assert(offsetof(MisterStatus, struct_size) == 4, "V1 struct_size offset");
static_assert(offsetof(MisterStatus, state) == 8, "V1 state offset");
static_assert(offsetof(MisterStatus, last_error) == 12, "V1 last_error offset");
static_assert(offsetof(MisterStatus, primary_error) == 16, "V1 primary_error offset");
static_assert(offsetof(MisterStatus, cleanup_error) == 20, "V1 cleanup_error offset");
static_assert(offsetof(MisterStatus, tick_count) == 24, "V1 tick_count offset");
static_assert(offsetof(MisterStatus, capability_flags) == 32, "V1 capability_flags offset");
static_assert(offsetof(MisterStatus, reserved) == 36, "V1 reserved offset");

int main()
{
	uint32_t (*abi_version)(void) = &MisterRuntime_ABIVersion;
	MisterRuntime *(*create)(const MisterPlatform *) = &MisterRuntime_Create;
	bool (*start)(MisterRuntime *) = &MisterRuntime_Start;
	void (*tick)(MisterRuntime *) = &MisterRuntime_Tick;
	bool (*load)(MisterRuntime *, const MisterLaunch *) = &MisterRuntime_Load;
	MisterStatus (*status)(const MisterRuntime *) = &MisterRuntime_Status;
	void (*stop)(MisterRuntime *) = &MisterRuntime_Stop;
	bool (*destroy)(MisterRuntime **) = &MisterRuntime_Destroy;

	return abi_version != nullptr && create != nullptr && start != nullptr &&
		tick != nullptr && load != nullptr && status != nullptr &&
		stop != nullptr && destroy != nullptr ? 0 : 1;
}
