/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MISTER_RUNTIME_H
#define MISTER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

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

/*
 * ABI v2 is intentionally additive.  ABI v1 above is retained verbatim for
 * compatibility Main; v2 callers must use only the versioned declarations
 * below.
 */
#define MISTER_RUNTIME_ABI_VERSION_V2 2u

typedef struct MisterStringView {
	const char *data;
	uint32_t length;
} MisterStringView;

typedef enum MisterResult {
	MISTER_RESULT_REPRESENTATION_MIN = INT32_MIN,
	MISTER_RESULT_OK = 0,
	MISTER_RESULT_INVALID_ARGUMENT = 1,
	MISTER_RESULT_INVALID_STATE = 2,
	MISTER_RESULT_UNSUPPORTED = 3,
	MISTER_RESULT_DEADLINE = 4,
	MISTER_RESULT_PLATFORM = 5,
	MISTER_RESULT_CLEANUP_INCOMPLETE = 6,
	MISTER_RESULT_EXIT_REQUIRED = 7,
	MISTER_RESULT_REPRESENTATION_MAX = INT32_MAX
} MisterResult;

typedef enum MisterStateV2 {
	MISTER_STATE_CREATED = 0,
	MISTER_STATE_READY = 1,
	MISTER_STATE_RUNNING = 2,
	MISTER_STATE_FAILED = 3,
	MISTER_STATE_CLEANUP_INCOMPLETE = 4,
	MISTER_STATE_EXIT_REQUIRED = 5,
	MISTER_STATE_STOPPED = 6
} MisterStateV2;

typedef enum MisterCapabilityV2 {
	MISTER_CAP_CLEAN_STOP = 1u << 0,
	MISTER_CAP_OBSERVE = 1u << 1,
	MISTER_CAP_CONTENT_REF = 1u << 2,
	MISTER_CAP_STATELESS_RECOVERY = 1u << 3
} MisterCapabilityV2;

typedef enum MisterResourceV2 {
	MISTER_RESOURCE_FPGA = 1u << 0,
	MISTER_RESOURCE_BRIDGES = 1u << 1,
	MISTER_RESOURCE_CORE_PROTOCOL = 1u << 2,
	MISTER_RESOURCE_NATIVE_VIDEO = 1u << 3,
	MISTER_RESOURCE_NATIVE_AUDIO = 1u << 4,
	MISTER_RESOURCE_CORE_INPUT = 1u << 5,
	MISTER_RESOURCE_SAVES = 1u << 6,
	MISTER_RESOURCE_CONTENT = 1u << 7
} MisterResourceV2;

#define MISTER_CAP_V2_KNOWN (MISTER_CAP_CLEAN_STOP | MISTER_CAP_OBSERVE | \
	MISTER_CAP_CONTENT_REF | MISTER_CAP_STATELESS_RECOVERY)
#define MISTER_RESOURCE_V2_KNOWN (MISTER_RESOURCE_FPGA | \
	MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL | \
	MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_NATIVE_AUDIO | \
	MISTER_RESOURCE_CORE_INPUT | MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT)

typedef struct MisterContentRefV2 {
	uint32_t abi_version;
	uint32_t struct_size;
	MisterStringView sha256;
	uint64_t size;
	MisterStringView extension;
	uint32_t reserved[4];
} MisterContentRefV2;

typedef struct MisterLaunchV2 {
	uint32_t abi_version;
	uint32_t struct_size;
	MisterStringView game_id;
	MisterStringView system;
	MisterStringView expected_core;
	MisterContentRefV2 content;
	uint32_t reserved[4];
} MisterLaunchV2;

typedef struct MisterObservationV2 {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t ready;
	MisterStringView observed_core;
	uint32_t resource_flags;
	uint32_t reserved[4];
} MisterObservationV2;

typedef struct MisterRecoveryObservationV2 {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t observed_resource_flags;
	uint32_t neutral_resource_flags;
	uint32_t reserved[4];
} MisterRecoveryObservationV2;

typedef struct MisterStatusV2 {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t state;
	uint32_t last_result;
	uint32_t primary_result;
	uint32_t cleanup_result;
	uint64_t tick_count;
	uint32_t capability_flags;
	uint32_t reserved[4];
} MisterStatusV2;

typedef struct MisterPlatformV2 {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t capability_flags;
	void *context;
	MisterResult (*start)(void *context, uint32_t deadline_ms);
	MisterResult (*load)(void *context, const MisterLaunchV2 *launch,
		uint32_t deadline_ms);
	MisterResult (*tick)(void *context, uint32_t deadline_ms);
	MisterResult (*observe)(void *context, MisterObservationV2 *observation,
		uint32_t deadline_ms);
	MisterResult (*stop)(void *context, uint32_t deadline_ms);
	MisterResult (*recover)(void *context, uint32_t required_resource_flags,
		MisterRecoveryObservationV2 *observation, uint32_t deadline_ms);
	uint32_t reserved[4];
} MisterPlatformV2;

uint32_t MisterRuntime_ABIVersionV2(void);
MisterResult MisterRuntime_CreateV2(const MisterPlatformV2 *platform,
	MisterRuntime **runtime);
MisterResult MisterRuntime_StartV2(MisterRuntime *runtime,
	uint32_t deadline_ms);
MisterResult MisterRuntime_LoadV2(MisterRuntime *runtime,
	const MisterLaunchV2 *launch, uint32_t deadline_ms);
MisterResult MisterRuntime_TickV2(MisterRuntime *runtime,
	uint32_t deadline_ms);
MisterResult MisterRuntime_ObserveV2(MisterRuntime *runtime,
	MisterObservationV2 *observation, uint32_t deadline_ms);
MisterResult MisterRuntime_StatusV2(const MisterRuntime *runtime,
	MisterStatusV2 *status);
MisterResult MisterRuntime_StopV2(MisterRuntime *runtime,
	uint32_t deadline_ms);
MisterResult MisterRuntime_DestroyV2(MisterRuntime **runtime);
MisterResult MisterRuntime_RecoverPlatformV2(const MisterPlatformV2 *platform,
	uint32_t required_resource_flags, MisterRecoveryObservationV2 *observation,
	uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
