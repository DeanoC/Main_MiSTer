/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime/mister_runtime_internal.hpp"

#include <exception>
#include <mutex>
#include <set>
#include <stdlib.h>

#if defined(MISTER_RUNTIME_TESTING)
static MisterRuntimeTest::Fault v2_test_fault = MisterRuntimeTest::FAULT_NONE;

namespace MisterRuntimeTest {
void SetFault(Fault fault) { v2_test_fault = fault; }
void ClearFault() { v2_test_fault = FAULT_NONE; }
}
#endif

namespace {

struct MisterRuntimeV2 {
	uint32_t generation;
	MisterPlatformV2 platform_v2;
	uint32_t v2_state;
	uint32_t v2_last_result;
	uint32_t v2_primary_result;
	uint32_t v2_cleanup_result;
	uint64_t v2_tick_count;
	uint32_t v2_capability_flags;
};

std::mutex v2_registry_mutex;
std::set<void *> v2_live_contexts;

enum RegistryResult {
	REGISTRY_OK,
	REGISTRY_BUSY,
	REGISTRY_FAILURE
};

#if defined(MISTER_RUNTIME_TESTING)
static bool test_fault(unsigned fault)
{
	return v2_test_fault == static_cast<MisterRuntimeTest::Fault>(fault) ||
		(v2_test_fault == MisterRuntimeTest::FAULT_RECOVER_DOUBLE_UNREGISTER &&
			(fault == MisterRuntimeTest::FAULT_RECOVER_UNREGISTER ||
			fault == MisterRuntimeTest::FAULT_RECOVER_DOUBLE_UNREGISTER));
}
#else
static bool test_fault(unsigned) { return false; }
#endif

static RegistryResult registry_register(void *context, unsigned fault)
{
	try {
		std::lock_guard<std::mutex> lock(v2_registry_mutex);
		if (test_fault(fault)) throw std::bad_alloc();
		if (v2_live_contexts.find(context) != v2_live_contexts.end()) {
			return REGISTRY_BUSY;
		}
		v2_live_contexts.insert(context);
		return REGISTRY_OK;
	} catch (...) {
		return REGISTRY_FAILURE;
	}
}

static bool registry_unregister(void *context, unsigned fault)
{
	try {
		std::lock_guard<std::mutex> lock(v2_registry_mutex);
		if (test_fault(fault)) throw std::bad_alloc();
		v2_live_contexts.erase(context);
		return true;
	} catch (...) {
		return false;
	}
}

static bool registry_force_unregister(void *context, unsigned fault)
{
	try {
		std::lock_guard<std::mutex> lock(v2_registry_mutex);
		if (test_fault(fault)) throw std::bad_alloc();
		v2_live_contexts.erase(context);
		return true;
	} catch (...) {
		return false;
	}
}

class RecoveryRegistration {
public:
	explicit RecoveryRegistration(void *context) : context_(context), held_(true) {}
	~RecoveryRegistration()
	{
		if (held_) registry_force_unregister(context_, 0);
	}
	bool release(unsigned fault)
	{
		if (!registry_unregister(context_, fault)) return false;
		held_ = false;
		return true;
	}
	bool force_release(unsigned fault)
	{
		if (registry_force_unregister(context_, fault) || registry_force_unregister(context_, 0)) {
			held_ = false;
			return true;
		}
		return false;
	}

private:
	void *context_;
	bool held_;
};

static bool all_zero(const uint32_t *values, unsigned count)
{
	for (unsigned index = 0; index < count; ++index) {
		if (values[index] != 0) return false;
	}
	return true;
}

static bool valid_result(MisterResult result)
{
	switch (result) {
	case MISTER_RESULT_OK:
	case MISTER_RESULT_INVALID_ARGUMENT:
	case MISTER_RESULT_INVALID_STATE:
	case MISTER_RESULT_UNSUPPORTED:
	case MISTER_RESULT_DEADLINE:
	case MISTER_RESULT_PLATFORM:
	case MISTER_RESULT_CLEANUP_INCOMPLETE:
	case MISTER_RESULT_EXIT_REQUIRED:
		return true;
	case MISTER_RESULT_REPRESENTATION_MIN:
	case MISTER_RESULT_REPRESENTATION_MAX:
	default:
		return false;
	}
}

static MisterResult normalize_result(MisterResult result)
{
	return valid_result(result) ? result : MISTER_RESULT_PLATFORM;
}

static bool valid_platform_prefix(const MisterPlatformV2 *platform)
{
	return platform != nullptr &&
		platform->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		platform->struct_size >= sizeof(*platform) &&
		platform->capability_flags == MISTER_CAP_V2_KNOWN &&
		all_zero(platform->reserved, 4);
}

static bool valid_platform(const MisterPlatformV2 *platform)
{
	return valid_platform_prefix(platform) && platform->start != nullptr &&
		platform->load != nullptr && platform->tick != nullptr &&
		platform->observe != nullptr && platform->stop != nullptr &&
		platform->recover != nullptr;
}

static bool valid_view(const MisterStringView view, uint32_t minimum,
	uint32_t maximum)
{
	return view.length >= minimum && view.length <= maximum &&
		(view.length == 0 || view.data != nullptr);
}

static bool lower_alnum(char value)
{
	return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

static bool lower_hex(char value)
{
	return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

static bool valid_game_id(MisterStringView view)
{
	if (!valid_view(view, 1, 128) || !lower_alnum(view.data[0]) ||
		!lower_alnum(view.data[view.length - 1])) return false;
	for (uint32_t index = 1; index + 1 < view.length; ++index) {
		if (!lower_alnum(view.data[index]) && view.data[index] != '-') return false;
	}
	return true;
}

static bool valid_system(MisterStringView view)
{
	if (!valid_view(view, 1, 32) || !lower_alnum(view.data[0])) return false;
	for (uint32_t index = 1; index < view.length; ++index) {
		char value = view.data[index];
		if (!lower_alnum(value) && value != '_' && value != '-') return false;
	}
	return true;
}

static bool valid_expected_core(MisterStringView view)
{
	if (!valid_view(view, 1, 64)) return false;
	char first = view.data[0];
	if (!((first >= 'A' && first <= 'Z') ||
		(first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) return false;
	for (uint32_t index = 1; index < view.length; ++index) {
		char value = view.data[index];
		bool valid = (value >= 'A' && value <= 'Z') ||
			(value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
			value == ' ' || value == '_' || value == '+' || value == '(' ||
			value == ')' || value == '.' || value == '-';
		if (!valid) return false;
	}
	return true;
}

static bool valid_sha256(MisterStringView view)
{
	if (!valid_view(view, 64, 64)) return false;
	for (uint32_t index = 0; index < view.length; ++index) {
		if (!lower_hex(view.data[index])) return false;
	}
	return true;
}

static bool valid_extension(MisterStringView view)
{
	if (!valid_view(view, 1, 16)) return false;
	for (uint32_t index = 0; index < view.length; ++index) {
		if (!lower_alnum(view.data[index])) return false;
	}
	return true;
}

static bool valid_launch(const MisterLaunchV2 *launch)
{
	return launch != nullptr &&
		launch->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		launch->struct_size >= sizeof(*launch) && all_zero(launch->reserved, 4) &&
		valid_game_id(launch->game_id) && valid_system(launch->system) &&
		valid_expected_core(launch->expected_core) &&
		launch->content.abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		launch->content.struct_size >= sizeof(launch->content) &&
		all_zero(launch->content.reserved, 4) &&
		valid_sha256(launch->content.sha256) &&
		launch->content.size >= 1 && launch->content.size <= 32u * 1024u * 1024u &&
		valid_extension(launch->content.extension);
}

static bool initialized_observation(const MisterObservationV2 *observation)
{
	return observation != nullptr &&
		observation->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		observation->struct_size == sizeof(*observation) &&
		observation->ready == 0 && observation->observed_core.data == nullptr &&
		observation->observed_core.length == 0 && observation->resource_flags == 0 &&
		all_zero(observation->reserved, 4);
}

static bool initialized_recovery_observation(
	const MisterRecoveryObservationV2 *observation)
{
	return observation != nullptr &&
		observation->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		observation->struct_size == sizeof(*observation) &&
		observation->observed_resource_flags == 0 &&
		observation->neutral_resource_flags == 0 && all_zero(observation->reserved, 4);
}

static bool initialized_status(const MisterStatusV2 *status)
{
	return status != nullptr && status->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		status->struct_size == sizeof(*status) && status->state == 0 &&
		status->last_result == 0 && status->primary_result == 0 &&
		status->cleanup_result == 0 && status->tick_count == 0 &&
		status->capability_flags == 0 && all_zero(status->reserved, 4);
}

static bool valid_observation_output(const MisterObservationV2 *observation)
{
	return observation->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		observation->struct_size == sizeof(*observation) && observation->ready <= 1 &&
		(observation->resource_flags & ~MISTER_RESOURCE_V2_KNOWN) == 0 &&
		(observation->observed_core.length == 0 ||
			valid_expected_core(observation->observed_core)) &&
		all_zero(observation->reserved, 4);
}

static bool valid_recovery_output(const MisterRecoveryObservationV2 *observation)
{
	return observation->abi_version == MISTER_RUNTIME_ABI_VERSION_V2 &&
		observation->struct_size == sizeof(*observation) &&
		(observation->observed_resource_flags & ~MISTER_RESOURCE_V2_KNOWN) == 0 &&
		(observation->neutral_resource_flags & ~MISTER_RESOURCE_V2_KNOWN) == 0 &&
		all_zero(observation->reserved, 4);
}

template <typename Callback>
static MisterResult call_platform(Callback callback)
{
	try {
		return normalize_result(callback());
	} catch (...) {
		return MISTER_RESULT_PLATFORM;
	}
}

static bool is_v2(const MisterRuntime *runtime)
{
	return MisterRuntime_ReadGeneration(runtime) == MISTER_RUNTIME_GENERATION_V2;
}

static MisterRuntimeV2 *as_v2(MisterRuntime *runtime)
{
	return reinterpret_cast<MisterRuntimeV2 *>(runtime);
}

static const MisterRuntimeV2 *as_v2(const MisterRuntime *runtime)
{
	return reinterpret_cast<const MisterRuntimeV2 *>(runtime);
}

static void set_primary(MisterRuntime *runtime, MisterResult result)
{
	if (as_v2(runtime)->v2_primary_result == MISTER_RESULT_OK) {
		as_v2(runtime)->v2_primary_result = static_cast<uint32_t>(result);
	}
}

static MisterResult fail_call(MisterRuntime *runtime, MisterResult result)
{
	as_v2(runtime)->v2_last_result = static_cast<uint32_t>(result);
	return result;
}

static MisterResult run_observe(MisterRuntime *runtime,
	MisterObservationV2 *observation, uint32_t deadline_ms)
{
	MisterResult result = call_platform([&]() {
		return as_v2(runtime)->platform_v2.observe(as_v2(runtime)->platform_v2.context, observation,
			deadline_ms);
	});
	if (result != MISTER_RESULT_OK) return result;
	return valid_observation_output(observation) ? MISTER_RESULT_OK : MISTER_RESULT_PLATFORM;
}

}  // namespace

#if defined(MISTER_RUNTIME_TESTING)
namespace MisterRuntimeTest {
MisterResult DiscardExitRequired(MisterRuntime **runtime)
{
	if (runtime == nullptr || *runtime == nullptr || !is_v2(*runtime) ||
		as_v2(*runtime)->v2_state != MISTER_STATE_EXIT_REQUIRED) {
		return MISTER_RESULT_INVALID_STATE;
	}
	void *context = as_v2(*runtime)->platform_v2.context;
	if (!registry_force_unregister(context, ~0u)) return MISTER_RESULT_PLATFORM;
	free(*runtime);
	*runtime = nullptr;
	return MISTER_RESULT_OK;
}
}
#endif

extern "C" uint32_t MisterRuntime_ABIVersionV2(void)
{
	return MISTER_RUNTIME_ABI_VERSION_V2;
}

extern "C" MisterResult MisterRuntime_CreateV2(const MisterPlatformV2 *platform,
	MisterRuntime **runtime)
{
	if (runtime == nullptr || *runtime != nullptr || !valid_platform(platform)) {
		return MISTER_RESULT_INVALID_ARGUMENT;
	}

	MisterRuntimeV2 *created = nullptr;
	try {
#if defined(MISTER_RUNTIME_TESTING)
		if (test_fault(MisterRuntimeTest::FAULT_CREATE_ALLOCATE)) throw std::bad_alloc();
#endif
		created = static_cast<MisterRuntimeV2 *>(calloc(1, sizeof(*created)));
		if (created == nullptr) return MISTER_RESULT_PLATFORM;
		created->generation = MISTER_RUNTIME_GENERATION_V2;
		created->platform_v2 = *platform;
		created->v2_state = MISTER_STATE_CREATED;
		created->v2_capability_flags = platform->capability_flags;
#if defined(MISTER_RUNTIME_TESTING)
		RegistryResult registered = registry_register(platform->context,
			MisterRuntimeTest::FAULT_CREATE_REGISTER);
#else
		RegistryResult registered = registry_register(platform->context, 0);
#endif
		if (registered != REGISTRY_OK) {
			free(created);
			return registered == REGISTRY_BUSY ? MISTER_RESULT_INVALID_STATE :
				MISTER_RESULT_PLATFORM;
		}
		*runtime = reinterpret_cast<MisterRuntime *>(created);
		return MISTER_RESULT_OK;
	} catch (...) {
		free(created);
		*runtime = nullptr;
		return MISTER_RESULT_PLATFORM;
	}
}

extern "C" MisterResult MisterRuntime_StartV2(MisterRuntime *runtime,
	uint32_t deadline_ms)
{
	if (!is_v2(runtime)) return MISTER_RESULT_INVALID_ARGUMENT;
	if (as_v2(runtime)->v2_state != MISTER_STATE_CREATED) return fail_call(runtime, MISTER_RESULT_INVALID_STATE);
	if (deadline_ms == 0) return fail_call(runtime, MISTER_RESULT_INVALID_ARGUMENT);
	MisterResult result = call_platform([&]() {
		return as_v2(runtime)->platform_v2.start(as_v2(runtime)->platform_v2.context, deadline_ms);
	});
	if (result == MISTER_RESULT_OK) {
		as_v2(runtime)->v2_state = MISTER_STATE_READY;
		as_v2(runtime)->v2_last_result = MISTER_RESULT_OK;
		return result;
	}
	as_v2(runtime)->v2_state = MISTER_STATE_FAILED;
	as_v2(runtime)->v2_last_result = result;
	set_primary(runtime, result);
	return result;
}

extern "C" MisterResult MisterRuntime_LoadV2(MisterRuntime *runtime,
	const MisterLaunchV2 *launch, uint32_t deadline_ms)
{
	if (!is_v2(runtime)) return MISTER_RESULT_INVALID_ARGUMENT;
	if (as_v2(runtime)->v2_state != MISTER_STATE_READY) return fail_call(runtime, MISTER_RESULT_INVALID_STATE);
	if (deadline_ms == 0 || !valid_launch(launch)) {
		return fail_call(runtime, MISTER_RESULT_INVALID_ARGUMENT);
	}
	MisterResult result = call_platform([&]() {
		return as_v2(runtime)->platform_v2.load(as_v2(runtime)->platform_v2.context, launch, deadline_ms);
	});
	if (result == MISTER_RESULT_OK) {
		MisterObservationV2 observation = {};
		observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		observation.struct_size = sizeof(observation);
		result = run_observe(runtime, &observation, deadline_ms);
		if (result == MISTER_RESULT_OK && observation.ready != 1) result = MISTER_RESULT_PLATFORM;
		if (result == MISTER_RESULT_OK &&
			(observation.resource_flags & MISTER_RESOURCE_V2_KNOWN) != MISTER_RESOURCE_V2_KNOWN) {
			result = MISTER_RESULT_PLATFORM;
		}
	}
	if (result == MISTER_RESULT_OK) {
		as_v2(runtime)->v2_state = MISTER_STATE_RUNNING;
		as_v2(runtime)->v2_last_result = MISTER_RESULT_OK;
		return result;
	}
	as_v2(runtime)->v2_state = MISTER_STATE_FAILED;
	as_v2(runtime)->v2_last_result = result;
	set_primary(runtime, result);
	return result;
}

extern "C" MisterResult MisterRuntime_TickV2(MisterRuntime *runtime,
	uint32_t deadline_ms)
{
	if (!is_v2(runtime)) return MISTER_RESULT_INVALID_ARGUMENT;
	if (as_v2(runtime)->v2_state != MISTER_STATE_RUNNING) return fail_call(runtime, MISTER_RESULT_INVALID_STATE);
	if (deadline_ms == 0) return fail_call(runtime, MISTER_RESULT_INVALID_ARGUMENT);
	MisterResult result = call_platform([&]() {
		return as_v2(runtime)->platform_v2.tick(as_v2(runtime)->platform_v2.context, deadline_ms);
	});
	if (result == MISTER_RESULT_OK) {
		++as_v2(runtime)->v2_tick_count;
		as_v2(runtime)->v2_last_result = MISTER_RESULT_OK;
		return result;
	}
	as_v2(runtime)->v2_state = MISTER_STATE_FAILED;
	as_v2(runtime)->v2_last_result = result;
	set_primary(runtime, result);
	return result;
}

extern "C" MisterResult MisterRuntime_ObserveV2(MisterRuntime *runtime,
	MisterObservationV2 *observation, uint32_t deadline_ms)
{
	if (!is_v2(runtime)) return MISTER_RESULT_INVALID_ARGUMENT;
	if (deadline_ms == 0 || !initialized_observation(observation)) {
		return fail_call(runtime, MISTER_RESULT_INVALID_ARGUMENT);
	}
	return run_observe(runtime, observation, deadline_ms);
}

extern "C" MisterResult MisterRuntime_StatusV2(const MisterRuntime *runtime,
	MisterStatusV2 *status)
{
	if (!is_v2(runtime) || !initialized_status(status)) return MISTER_RESULT_INVALID_ARGUMENT;
	status->state = as_v2(runtime)->v2_state;
	status->last_result = as_v2(runtime)->v2_last_result;
	status->primary_result = as_v2(runtime)->v2_primary_result;
	status->cleanup_result = as_v2(runtime)->v2_cleanup_result;
	status->tick_count = as_v2(runtime)->v2_tick_count;
	status->capability_flags = as_v2(runtime)->v2_capability_flags;
	return MISTER_RESULT_OK;
}

extern "C" MisterResult MisterRuntime_StopV2(MisterRuntime *runtime,
	uint32_t deadline_ms)
{
	if (!is_v2(runtime)) return MISTER_RESULT_INVALID_ARGUMENT;
	uint32_t state = as_v2(runtime)->v2_state;
	if (state != MISTER_STATE_CREATED && state != MISTER_STATE_READY &&
		state != MISTER_STATE_RUNNING && state != MISTER_STATE_FAILED &&
		state != MISTER_STATE_CLEANUP_INCOMPLETE && state != MISTER_STATE_STOPPED) {
		return fail_call(runtime, MISTER_RESULT_INVALID_STATE);
	}
	if (deadline_ms == 0) return fail_call(runtime, MISTER_RESULT_INVALID_ARGUMENT);
	if (state == MISTER_STATE_CREATED || state == MISTER_STATE_STOPPED) {
		as_v2(runtime)->v2_state = MISTER_STATE_STOPPED;
		as_v2(runtime)->v2_last_result = MISTER_RESULT_OK;
		as_v2(runtime)->v2_cleanup_result = MISTER_RESULT_OK;
		return MISTER_RESULT_OK;
	}
	MisterResult result = call_platform([&]() {
		return as_v2(runtime)->platform_v2.stop(as_v2(runtime)->platform_v2.context,
			deadline_ms);
	});
	if (result == MISTER_RESULT_OK) {
		as_v2(runtime)->v2_state = MISTER_STATE_STOPPED;
		as_v2(runtime)->v2_last_result = MISTER_RESULT_OK;
		as_v2(runtime)->v2_cleanup_result = MISTER_RESULT_OK;
		return MISTER_RESULT_OK;
	}
	if (result == MISTER_RESULT_EXIT_REQUIRED) {
		as_v2(runtime)->v2_state = MISTER_STATE_EXIT_REQUIRED;
		as_v2(runtime)->v2_last_result = result;
		as_v2(runtime)->v2_cleanup_result = result;
		return result;
	}
	as_v2(runtime)->v2_state = MISTER_STATE_CLEANUP_INCOMPLETE;
	as_v2(runtime)->v2_last_result = result;
	as_v2(runtime)->v2_cleanup_result = result;
	return result;
}

extern "C" MisterResult MisterRuntime_DestroyV2(MisterRuntime **runtime)
{
	if (runtime == nullptr || *runtime == nullptr || !is_v2(*runtime)) {
		return MISTER_RESULT_INVALID_ARGUMENT;
	}
	if (as_v2(*runtime)->v2_state != MISTER_STATE_CREATED &&
		as_v2(*runtime)->v2_state != MISTER_STATE_STOPPED) {
		return fail_call(*runtime, MISTER_RESULT_INVALID_STATE);
	}
	void *context = as_v2(*runtime)->platform_v2.context;
#if defined(MISTER_RUNTIME_TESTING)
	if (!registry_unregister(context, MisterRuntimeTest::FAULT_DESTROY_UNREGISTER)) {
#else
	if (!registry_unregister(context, 0)) {
#endif
		return MISTER_RESULT_PLATFORM;
	}
	free(*runtime);
	*runtime = nullptr;
	return MISTER_RESULT_OK;
}

extern "C" MisterResult MisterRuntime_RecoverPlatformV2(const MisterPlatformV2 *platform,
	uint32_t required_resource_flags, MisterRecoveryObservationV2 *observation,
	uint32_t deadline_ms)
{
	if (!valid_platform_prefix(platform) || platform->recover == nullptr ||
		required_resource_flags == 0 ||
		(required_resource_flags & ~MISTER_RESOURCE_V2_KNOWN) != 0 || deadline_ms == 0 ||
		!initialized_recovery_observation(observation)) {
		return MISTER_RESULT_INVALID_ARGUMENT;
	}
	RegistryResult registered = REGISTRY_FAILURE;
#if defined(MISTER_RUNTIME_TESTING)
	registered = registry_register(platform->context, MisterRuntimeTest::FAULT_RECOVER_REGISTER);
#else
	registered = registry_register(platform->context, 0);
#endif
	if (registered != REGISTRY_OK) {
		return registered == REGISTRY_BUSY ? MISTER_RESULT_INVALID_STATE :
			MISTER_RESULT_PLATFORM;
	}
	RecoveryRegistration registration(platform->context);
	MisterResult result = call_platform([&]() {
		return platform->recover(platform->context, required_resource_flags, observation,
			deadline_ms);
	});
#if defined(MISTER_RUNTIME_TESTING)
	bool released = registration.release(MisterRuntimeTest::FAULT_RECOVER_UNREGISTER);
#else
	bool released = registration.release(0);
#endif
	if (!released) {
		if (!registration.force_release(
#if defined(MISTER_RUNTIME_TESTING)
			MisterRuntimeTest::FAULT_RECOVER_DOUBLE_UNREGISTER
#else
			0
#endif
		)) return MISTER_RESULT_PLATFORM;
		return MISTER_RESULT_PLATFORM;
	}
	if (!valid_recovery_output(observation)) return MISTER_RESULT_PLATFORM;
	if (result != MISTER_RESULT_OK) return result;
	if ((observation->neutral_resource_flags & required_resource_flags) !=
		required_resource_flags ||
		(observation->observed_resource_flags & required_resource_flags) != 0) {
		return MISTER_RESULT_PLATFORM;
	}
	return MISTER_RESULT_OK;
}
