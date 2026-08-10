/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MISTER_RUNTIME_INTERNAL_HPP
#define MISTER_RUNTIME_INTERNAL_HPP

#include "runtime/mister_runtime.h"

enum MisterPlatformStopResult : uint32_t {
	MISTER_PLATFORM_RELEASED = 0,
	MISTER_PLATFORM_EXIT_REQUIRED = 1,
	MISTER_PLATFORM_STOP_FAILED = 2
};

#define MISTER_PLATFORM_CAP_CLEAN_STOP (1u << 0)
#define MISTER_PLATFORM_CAP_PROCESS_CONTROL_ESCAPE (1u << 1)
#define MISTER_PLATFORM_CAP_KNOWN \
	(MISTER_PLATFORM_CAP_CLEAN_STOP | MISTER_PLATFORM_CAP_PROCESS_CONTROL_ESCAPE)

struct MisterPlatform {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t capability_flags;
	void *context;
	bool (*start)(void *context);
	bool (*load)(void *context, const MisterLaunch *launch);
	bool (*tick)(void *context);
	MisterPlatformStopResult (*stop)(void *context);
};

const MisterPlatform *MisterRuntime_LegacyPlatform(void);

#endif
