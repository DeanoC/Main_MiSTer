/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MISTER_RUNTIME_H
#define MISTER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MISTER_RUNTIME_ABI_VERSION 1u

typedef struct MisterRuntime MisterRuntime;
typedef struct MisterPlatform MisterPlatform;

#define MISTER_RUNTIME_CREATED         0u
#define MISTER_RUNTIME_READY           1u
#define MISTER_RUNTIME_RUNNING         2u
#define MISTER_RUNTIME_FAILED          3u
#define MISTER_RUNTIME_CLEANUP_FAILED  4u
#define MISTER_RUNTIME_EXIT_REQUIRED   5u
#define MISTER_RUNTIME_STOPPED         6u

#define MISTER_RUNTIME_ERROR_NONE              0u
#define MISTER_RUNTIME_ERROR_INVALID_ARGUMENT  1u
#define MISTER_RUNTIME_ERROR_INVALID_STATE     2u
#define MISTER_RUNTIME_ERROR_PLATFORM_START    3u
#define MISTER_RUNTIME_ERROR_PLATFORM_LOAD     4u
#define MISTER_RUNTIME_ERROR_PLATFORM_TICK     5u
#define MISTER_RUNTIME_ERROR_PLATFORM_STOP     6u

#define MISTER_RUNTIME_CAP_CLEAN_STOP (1u << 0)

typedef struct MisterLaunch {
	uint32_t abi_version;
	uint32_t struct_size;
	const char *core_path;
	const char *xml_path;
} MisterLaunch;

typedef struct MisterStatus {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t state;
	uint32_t last_error;
	uint32_t primary_error;
	uint32_t cleanup_error;
	uint64_t tick_count;
	uint32_t capability_flags;
	uint32_t reserved[3];
} MisterStatus;

uint32_t MisterRuntime_ABIVersion(void);
MisterRuntime *MisterRuntime_Create(const MisterPlatform *platform);
bool MisterRuntime_Start(MisterRuntime *runtime);
void MisterRuntime_Tick(MisterRuntime *runtime);
bool MisterRuntime_Load(MisterRuntime *runtime, const MisterLaunch *launch);
MisterStatus MisterRuntime_Status(const MisterRuntime *runtime);
void MisterRuntime_Stop(MisterRuntime *runtime);
bool MisterRuntime_Destroy(MisterRuntime **runtime);

#ifdef __cplusplus
}
#endif

#endif
