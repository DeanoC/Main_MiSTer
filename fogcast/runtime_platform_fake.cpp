// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_platform_factory.hpp"

#ifdef FOGCAST_RUNTIME_HOST_TEST
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace fogcast {
namespace {

const uint32_t kResources = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
	MISTER_RESOURCE_CORE_PROTOCOL | MISTER_RESOURCE_NATIVE_VIDEO |
	MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_CORE_INPUT |
	MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT;

struct FakeState { bool active; FakeState() : active(false) {} };
FakeState g_state;

MisterResult Start(void*, uint32_t) { return MISTER_RESULT_OK; }
MisterResult Load(void* context, const MisterLaunchV2*, uint32_t deadline_ms) {
	FakeState* state = static_cast<FakeState*>(context);
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
#ifdef FOGCAST_RUNTIME_HOST_TEST
	const char* marker = getenv("FOGCAST_TEST_LAUNCH_MARKER");
	const char* release = getenv("FOGCAST_TEST_LAUNCH_RELEASE");
	if ((marker && !release) || (!marker && release)) return MISTER_RESULT_PLATFORM;
	if (marker) {
		const int descriptor = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (descriptor < 0 || close(descriptor) != 0) return MISTER_RESULT_PLATFORM;
		struct timespec started;
		if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return MISTER_RESULT_PLATFORM;
		const uint64_t start_ms = static_cast<uint64_t>(started.tv_sec) * 1000u +
			static_cast<uint64_t>(started.tv_nsec / 1000000u);
		for (;;) {
			struct stat release_info;
			if (lstat(release, &release_info) == 0) {
				if (!S_ISREG(release_info.st_mode)) return MISTER_RESULT_PLATFORM;
				break;
			}
			if (errno != ENOENT) return MISTER_RESULT_PLATFORM;
			struct timespec now;
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return MISTER_RESULT_PLATFORM;
			const uint64_t now_ms = static_cast<uint64_t>(now.tv_sec) * 1000u +
				static_cast<uint64_t>(now.tv_nsec / 1000000u);
			if (deadline_ms == 0 || now_ms - start_ms >= deadline_ms) return MISTER_RESULT_DEADLINE;
			struct timespec pause = {0, 1000000};
			while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
		}
	}
#else
	(void)deadline_ms;
#endif
	state->active = true;
	return MISTER_RESULT_OK;
}
MisterResult Observe(void* context, MisterObservationV2* observation, uint32_t) {
	FakeState* state = static_cast<FakeState*>(context);
	if (!observation) return MISTER_RESULT_INVALID_ARGUMENT;
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	observation->ready = state->active ? 1u : 0u;
	observation->resource_flags = state->active ? kResources : 0u;
	static const char core[] = "MegaDrive";
	observation->observed_core.data = state->active ? core : 0;
	observation->observed_core.length = state->active ? sizeof(core) - 1 : 0;
	return MISTER_RESULT_OK;
}
MisterResult Stop(void* context, uint32_t) {
	FakeState* state = static_cast<FakeState*>(context);
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	state->active = false;
	return MISTER_RESULT_OK;
}
MisterResult Recover(void* context, uint32_t required, MisterRecoveryObservationV2* observation,
	uint32_t) {
	FakeState* state = static_cast<FakeState*>(context);
	if (!state || !observation) return MISTER_RESULT_INVALID_ARGUMENT;
	state->active = false;
	observation->observed_resource_flags = 0;
	observation->neutral_resource_flags = required;
	return MISTER_RESULT_OK;
}
bool MainAbsent(void*) { return true; }

const MisterPlatformV2 kPlatform = {
	MISTER_RUNTIME_ABI_VERSION_V2, sizeof(MisterPlatformV2),
	MISTER_CAP_CLEAN_STOP | MISTER_CAP_OBSERVE | MISTER_CAP_CONTENT_REF |
		MISTER_CAP_STATELESS_RECOVERY,
	&g_state, Start, Load, Start, Observe, Stop, Recover, {0, 0, 0, 0}
};
const RuntimePlatformBinding kBinding = {&kPlatform, 0, MainAbsent};
const RuntimePlatformBinding kNullAbiBinding = {0, 0, MainAbsent};
const RuntimePlatformBinding kNullCallbackBinding = {&kPlatform, 0, 0};

}  // namespace

const RuntimePlatformBinding* RuntimePlatform() {
#ifdef FOGCAST_RUNTIME_HOST_TEST
	const char* fault = getenv("FOGCAST_TEST_PLATFORM");
	if (fault && strcmp(fault, "binding") == 0) return 0;
	if (fault && strcmp(fault, "abi") == 0) return &kNullAbiBinding;
	if (fault && strcmp(fault, "callback") == 0) return &kNullCallbackBinding;
#endif
	return &kBinding;
}

}  // namespace fogcast
