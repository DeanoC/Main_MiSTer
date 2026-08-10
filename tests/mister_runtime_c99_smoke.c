/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime.h"

#include <stddef.h>

#define MISTER_C99_ASSERT(name, condition) \
	typedef char name[(condition) ? 1 : -1]

MISTER_C99_ASSERT(mister_uint32_size, sizeof(uint32_t) == 4);
MISTER_C99_ASSERT(mister_uint64_size, sizeof(uint64_t) == 8);
MISTER_C99_ASSERT(mister_status_size, sizeof(MisterStatus) == 48);
MISTER_C99_ASSERT(mister_abi_version_offset,
	offsetof(MisterStatus, abi_version) == 0);
MISTER_C99_ASSERT(mister_struct_size_offset,
	offsetof(MisterStatus, struct_size) == 4);
MISTER_C99_ASSERT(mister_state_offset, offsetof(MisterStatus, state) == 8);
MISTER_C99_ASSERT(mister_last_error_offset,
	offsetof(MisterStatus, last_error) == 12);
MISTER_C99_ASSERT(mister_primary_error_offset,
	offsetof(MisterStatus, primary_error) == 16);
MISTER_C99_ASSERT(mister_cleanup_error_offset,
	offsetof(MisterStatus, cleanup_error) == 20);
MISTER_C99_ASSERT(mister_tick_count_offset,
	offsetof(MisterStatus, tick_count) == 24);
MISTER_C99_ASSERT(mister_capability_flags_offset,
	offsetof(MisterStatus, capability_flags) == 32);
MISTER_C99_ASSERT(mister_reserved_offset, offsetof(MisterStatus, reserved) == 36);

int main(void)
{
	MisterStatus status = MisterRuntime_Status(NULL);
	MisterRuntime *runtime = NULL;
	MisterLaunch launch = {0};

	MisterRuntime_Tick(NULL);
	MisterRuntime_Stop(NULL);

	return MisterRuntime_ABIVersion() == MISTER_RUNTIME_ABI_VERSION &&
		!MisterRuntime_Start(NULL) && !MisterRuntime_Load(NULL, &launch) &&
		!MisterRuntime_Destroy(NULL) && !MisterRuntime_Destroy(&runtime) &&
		status.abi_version == MISTER_RUNTIME_ABI_VERSION &&
		status.struct_size == sizeof(MisterStatus) &&
		status.state == MISTER_RUNTIME_FAILED &&
		status.last_error == MISTER_RUNTIME_ERROR_INVALID_ARGUMENT &&
		status.primary_error == MISTER_RUNTIME_ERROR_INVALID_ARGUMENT &&
		status.cleanup_error == MISTER_RUNTIME_ERROR_NONE &&
		status.tick_count == 0 && status.capability_flags == 0 &&
		status.reserved[0] == 0 && status.reserved[1] == 0 &&
		status.reserved[2] == 0 ? 0 : 1;
}
