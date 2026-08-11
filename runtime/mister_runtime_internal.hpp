/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MISTER_RUNTIME_INTERNAL_HPP
#define MISTER_RUNTIME_INTERNAL_HPP

#include "runtime/mister_runtime.h"

#include <string.h>

/* Shared by both ABI implementations to reject cross-generation handles. */
#define MISTER_RUNTIME_GENERATION_V1 1u
#define MISTER_RUNTIME_GENERATION_V2 2u

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

static inline uint32_t MisterRuntime_ReadGeneration(const MisterRuntime *runtime)
{
	uint32_t generation = 0;
	if (runtime != nullptr) memcpy(&generation, runtime, sizeof(generation));
	return generation;
}

#if defined(MISTER_RUNTIME_TESTING)
namespace MisterRuntimeTest {
enum Fault {
	FAULT_NONE = 0,
	FAULT_CREATE_ALLOCATE = 1,
	FAULT_CREATE_REGISTER = 2,
	FAULT_DESTROY_UNREGISTER = 3,
	FAULT_RECOVER_REGISTER = 4,
	FAULT_RECOVER_UNREGISTER = 5,
	FAULT_RECOVER_DOUBLE_UNREGISTER = 6
};
void SetFault(Fault fault);
void ClearFault();
}
#endif

const MisterPlatform *MisterRuntime_LegacyPlatform(void);

#endif
