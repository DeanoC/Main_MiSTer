// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_coordinator.hpp"

#include <time.h>

namespace fogcast {
namespace {

const uint32_t kAllResources = 255u;
const uint32_t kFpgaCleanupDeadline = 5000u;
const uint32_t kOtherCleanupDeadline = 2000u;
const uint32_t kLaunchDeadline = 15000u;
const uint32_t kStopDeadline = 10000u;
const uint32_t kShutdownDeadline = 10000u;
const uint32_t kRecoverDeadline = 30000u;
const uint32_t kStartupDeadline = 30000u;
const uint32_t kMaxCleanupAttempts = 64u;
const uint32_t kFpgaResources = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
	MISTER_RESOURCE_CORE_PROTOCOL;
const uint32_t kOtherResources = MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_NATIVE_AUDIO |
	MISTER_RESOURCE_CORE_INPUT | MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT;

uint64_t AddDeadline(uint64_t start, uint32_t duration) {
	const uint64_t maximum = ~static_cast<uint64_t>(0);
	return start > maximum - duration ? maximum : start + duration;
}

uint64_t MinimumDeadline(uint64_t left, uint64_t right) { return left < right ? left : right; }

struct CleanupDeadlines {
	uint64_t global;
	uint64_t fpga;
	uint64_t other;
	CleanupDeadlines(uint64_t global_deadline, uint64_t cleanup_started)
		: global(global_deadline), fpga(MinimumDeadline(global_deadline,
			AddDeadline(cleanup_started, kFpgaCleanupDeadline))), other(MinimumDeadline(global_deadline,
			AddDeadline(cleanup_started, kOtherCleanupDeadline))) {}
	uint64_t ForMask(uint32_t resource_mask) const {
		if (resource_mask == 0) return 0;
		uint64_t deadline = global;
		if ((resource_mask & kFpgaResources) != 0) deadline = MinimumDeadline(deadline, fpga);
		if ((resource_mask & kOtherResources) != 0) deadline = MinimumDeadline(deadline, other);
		return deadline;
	}
};

bool RetryableCleanup(const LifecycleResult& result) {
	return result.call_result == MISTER_RESULT_CLEANUP_INCOMPLETE ||
		result.runtime_state == MISTER_STATE_CLEANUP_INCOMPLETE;
}

std::string PrimaryError(const LifecycleResult& result) {
	return result.call_result == MISTER_RESULT_DEADLINE ? "DEADLINE" : "INTERNAL";
}

MisterResult Normalize(MisterResult result) {
	switch (result) {
	case MISTER_RESULT_OK:
	case MISTER_RESULT_INVALID_ARGUMENT:
	case MISTER_RESULT_INVALID_STATE:
	case MISTER_RESULT_UNSUPPORTED:
	case MISTER_RESULT_DEADLINE:
	case MISTER_RESULT_PLATFORM:
	case MISTER_RESULT_CLEANUP_INCOMPLETE:
	case MISTER_RESULT_EXIT_REQUIRED: return result;
	default: return MISTER_RESULT_PLATFORM;
	}
}

LifecycleResult Result(MisterResult result, MisterStateV2 state = MISTER_STATE_CREATED,
	MisterResult primary = MISTER_RESULT_OK, MisterResult cleanup = MISTER_RESULT_OK) {
	const MisterResult call = Normalize(result);
	LifecycleResult value(call, primary, cleanup, state);
	if (call != MISTER_RESULT_OK && primary == MISTER_RESULT_OK) value.primary_result = call;
	return value;
}

class SystemMonotonicClock : public MonotonicClock {
public:
	uint64_t NowMs() override {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		return static_cast<uint64_t>(now.tv_sec) * 1000u +
			static_cast<uint64_t>(now.tv_nsec / 1000000u);
	}
};

SystemMonotonicClock& DefaultClock() {
	static SystemMonotonicClock clock;
	return clock;
}

std::string OwnerJson(const Coordinator::Owner& owner) {
	if (!owner.present) return "null";
	return std::string("{\"session\":\"") + owner.session + "\",\"generation\":" +
		std::to_string(owner.generation) + ",\"mode\":\"fpga_native\"}";
}

bool SameOwner(const Coordinator::Owner& left, const Coordinator::Owner& right) {
	return left.present == right.present && (!left.present ||
		(left.session == right.session && left.generation == right.generation));
}

std::string LeasesJson(const Coordinator::Owner& owner) {
	if (!owner.present) return "[]";
	static const char* const resources[] = {"fpga", "bridges", "core_protocol", "native_video",
		"native_audio", "core_input", "saves", "content"};
	std::string value("[");
	for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
		if (i) value += ',';
		value += std::string("{\"resource\":\"") + resources[i] + "\",\"session\":\"" +
			owner.session + "\",\"generation\":" + std::to_string(owner.generation) + "}";
	}
	return value + ']';
}

std::string ErrorJson(const std::string& code) {
	return code.empty() ? "null" : std::string("{\"code\":\"") + code +
		"\",\"message\":\"lifecycle failed\"}";
}

StateRecord Idle(uint64_t sequence, uint64_t epoch) {
	StateRecord record;
	record.sequence = sequence;
	record.backend_epoch = epoch;
	record.phase = "idle";
	record.canonical = std::string("{\"record_version\":1,\"protocol\":1,\"abi_version\":2,\"backend_epoch\":") +
		std::to_string(epoch) + ",\"sequence\":" + std::to_string(sequence) +
		",\"phase\":\"idle\",\"mode\":\"idle\",\"owner\":null,\"release_owner\":null,\"candidate\":null,\"leases\":[],\"content_lease\":null,\"observed_core\":null,\"capabilities\":31,\"last_error\":null,\"in_flight\":null,\"ledger\":[]}";
	record.digest = Sha256Hex(record.canonical);
	return record;
}

bool Text(const detail::Token* token, std::string* output) {
	if (!token || token->kind != detail::Token::Kind::string) return false;
	*output = token->text;
	return true;
}

bool ParseOwner(const detail::Token* token, Coordinator::Owner* output) {
	*output = Coordinator::Owner();
	if (!token || token->kind == detail::Token::Kind::null_value) return token != 0;
	if (token->kind != detail::Token::Kind::object) return false;
	const detail::Token* session = detail::Member(*token, "session");
	const detail::Token* generation = detail::Member(*token, "generation");
	if (!Text(session, &output->session) || !detail::PositiveUint63(generation, &output->generation)) return false;
	output->present = true;
	return true;
}

bool Bool(const detail::Token* token, bool* output) {
	if (!token || token->kind != detail::Token::Kind::boolean) return false;
	*output = token->text == "true";
	return true;
}

bool Null(const detail::Token* token) {
	return token && token->kind == detail::Token::Kind::null_value;
}

bool EmptyArray(const detail::Token* token) {
	return token && token->kind == detail::Token::Kind::array && token->array.empty();
}

bool ParseTerminal(const detail::Token& token, Coordinator::Terminal* output) {
	if (token.kind != detail::Token::Kind::object || !Text(detail::Member(token, "operation_id"),
		&output->operation_id) || !Text(detail::Member(token, "request_digest"), &output->digest) ||
		!Bool(detail::Member(token, "ok"), &output->ok) ||
		!detail::PositiveUint63(detail::Member(token, "resulting_sequence"), &output->sequence)) return false;
	const detail::Token* error = detail::Member(token, "error");
	const detail::Token* snapshot = detail::Member(token, "snapshot");
	if (!snapshot || snapshot->kind != detail::Token::Kind::object) return false;
	output->snapshot = detail::Encode(*snapshot);
	output->error.clear();
	if (error && error->kind != detail::Token::Kind::null_value &&
		!Text(detail::Member(*error, "code"), &output->error)) return false;
	return true;
}

std::string ErrorCode(const detail::Token* token) {
	if (!token || token->kind == detail::Token::Kind::null_value) return std::string();
	std::string result;
	return Text(detail::Member(*token, "code"), &result) ? result : std::string();
}

CoordinatorCode CoordinatorCodeFromTerminal(const std::string& value) {
	if (value.empty()) return CoordinatorCode::none;
	if (value == "INTERRUPTED") return CoordinatorCode::interrupted;
	if (value == "DEADLINE") return CoordinatorCode::deadline;
	if (value == "RECOVERY_REQUIRED") return CoordinatorCode::recovery_required;
	if (value == "UNSUPPORTED_MODE") return CoordinatorCode::unsupported_mode;
	if (value == "OWNER_NOT_FOUND") return CoordinatorCode::owner_not_found;
	if (value == "STALE_GENERATION") return CoordinatorCode::stale_generation;
	if (value == "STALE_ADMISSION") return CoordinatorCode::stale_admission;
	return CoordinatorCode::internal;
}

CoordinatorSnapshot SnapshotFromCanonical(const std::string& record) {
	CoordinatorSnapshot result;
	detail::Token root;
	if (record.empty() || detail::ScanV1Json(record, 256 * 1024, &root) != ErrorClass::ok ||
		root.kind != detail::Token::Kind::object) return result;
	static const char* const fields[] = {"sequence", "backend_epoch", "phase", "mode", "owner",
		"release_owner", "candidate", "leases", "content_lease", "observed_core", "capabilities",
		"last_error"};
	std::string snapshot("{");
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		const detail::Token* field = detail::Member(root, fields[i]);
		if (!field) return CoordinatorSnapshot();
		if (i) snapshot += ',';
		snapshot += std::string("\"") + fields[i] + "\":" + detail::Encode(*field);
	}
	result.valid = true;
	result.canonical = snapshot + '}';
	return result;
}

}  // namespace

uint32_t CleanupDeadlineBudget(uint32_t resource_mask, uint64_t global_deadline,
	uint64_t cleanup_started, uint64_t now) {
	const uint64_t deadline = CleanupDeadlines(global_deadline, cleanup_started).ForMask(resource_mask);
	if (deadline == 0 || now >= deadline) return 0;
	const uint64_t remaining = deadline - now;
	return remaining > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(remaining);
}

AbiV2LifecyclePlatform::AbiV2LifecyclePlatform(const MisterPlatformV2* platform)
	: platform_(platform), runtime_(0), live_state_(LiveHandleState::none),
	  runtime_state_(MISTER_STATE_CREATED), primary_result_(MISTER_RESULT_OK),
	  cleanup_result_(MISTER_RESULT_OK) {}

bool AbiV2LifecyclePlatform::SynchronizeRuntimeState() {
	if (!runtime_) return false;
	MisterStatusV2 status = {};
	status.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	status.struct_size = sizeof(status);
	if (MisterRuntime_StatusV2(runtime_, &status) != MISTER_RESULT_OK ||
		status.state > MISTER_STATE_STOPPED) {
		runtime_state_ = MISTER_STATE_FAILED;
		return false;
	}
	runtime_state_ = static_cast<MisterStateV2>(status.state);
	return true;
}

LifecycleResult AbiV2LifecyclePlatform::FinishRuntimeCall(MisterResult call,
	bool primary_call, bool cleanup_call) {
	call = Normalize(call);
	if (primary_call && call != MISTER_RESULT_OK && primary_result_ == MISTER_RESULT_OK)
		primary_result_ = call;
	if (cleanup_call) cleanup_result_ = call;
	if (!SynchronizeRuntimeState()) {
		call = MISTER_RESULT_PLATFORM;
		if (primary_call && primary_result_ == MISTER_RESULT_OK) primary_result_ = call;
		if (cleanup_call) cleanup_result_ = call;
	}
	return LifecycleResult(call, primary_result_, cleanup_result_, runtime_state_);
}

LifecycleResult AbiV2LifecyclePlatform::GrantTransfer(const LifecycleOwner& owner) {
	if (!owner.present || runtime_ || live_state_ != LiveHandleState::none) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	granted_owner_ = owner;
	return Result(MISTER_RESULT_OK, runtime_state_, primary_result_, cleanup_result_);
}

LifecycleResult AbiV2LifecyclePlatform::Create(const LifecycleOwner& owner) {
	if (runtime_ || live_state_ != LiveHandleState::none || !platform_ || !SameOwner(owner, granted_owner_))
		return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	const LifecycleResult result = Result(MisterRuntime_CreateV2(platform_, &runtime_), runtime_state_, primary_result_, cleanup_result_);
	if (result == LifecycleResult::ok && runtime_) {
		live_owner_ = owner;
		live_state_ = LiveHandleState::live;
		runtime_state_ = MISTER_STATE_CREATED;
		granted_owner_ = LifecycleOwner();
	} else if (!runtime_) {
		live_owner_ = LifecycleOwner();
		live_state_ = LiveHandleState::none;
	}
	return result;
}

LifecycleResult AbiV2LifecyclePlatform::Start(uint32_t deadline_ms) {
	if (!runtime_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	return FinishRuntimeCall(MisterRuntime_StartV2(runtime_, deadline_ms), true, false);
}

LifecycleResult AbiV2LifecyclePlatform::Load(const LaunchMetadata& metadata, uint32_t deadline_ms) {
	MisterLaunchV2 launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id = {metadata.game_id.data(), static_cast<uint32_t>(metadata.game_id.size())};
	launch.system = {metadata.system.data(), static_cast<uint32_t>(metadata.system.size())};
	launch.expected_core = {metadata.expected_core.data(), static_cast<uint32_t>(metadata.expected_core.size())};
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256 = {metadata.sha256.data(), static_cast<uint32_t>(metadata.sha256.size())};
	launch.content.size = metadata.size;
	launch.content.extension = {metadata.extension.data(), static_cast<uint32_t>(metadata.extension.size())};
	if (!runtime_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	return FinishRuntimeCall(MisterRuntime_LoadV2(runtime_, &launch, deadline_ms), true, false);
}

LifecycleResult AbiV2LifecyclePlatform::Observe(const std::string& expected_core, uint32_t deadline_ms) {
	MisterObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	if (!runtime_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	LifecycleResult result = FinishRuntimeCall(MisterRuntime_ObserveV2(runtime_, &observation, deadline_ms), true, false);
	if (result != MISTER_RESULT_OK) {
		return result;
	}
	result.ready = observation.ready == 1;
	result.active_mask = observation.resource_flags;
	result.observed_mask = observation.resource_flags;
	if (!result.ready || observation.resource_flags != MISTER_RESOURCE_V2_KNOWN ||
		observation.observed_core.length != expected_core.size() || expected_core.compare(0,
		expected_core.size(), observation.observed_core.data, observation.observed_core.length) != 0) {
		if (primary_result_ == MISTER_RESULT_OK) primary_result_ = MISTER_RESULT_PLATFORM;
		return LifecycleResult(MISTER_RESULT_PLATFORM, primary_result_, cleanup_result_, runtime_state_);
	}
	result.observed_core.assign(observation.observed_core.data, observation.observed_core.length);
	return result;
}

LifecycleResult AbiV2LifecyclePlatform::Stop(uint32_t deadline_ms) {
	if (!runtime_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	LifecycleResult result = FinishRuntimeCall(MisterRuntime_StopV2(runtime_, deadline_ms), false, true);
	if (result == MISTER_RESULT_EXIT_REQUIRED) live_state_ = LiveHandleState::exit_required;
	return result;
}

LifecycleResult AbiV2LifecyclePlatform::ObserveNeutral(uint32_t resource_mask, uint32_t deadline_ms) {
	MisterObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	if (!runtime_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	LifecycleResult result = FinishRuntimeCall(MisterRuntime_ObserveV2(runtime_, &observation, deadline_ms), false, true);
	if (result != MISTER_RESULT_OK) return result;
	result.active_mask = observation.resource_flags; result.observed_mask = observation.resource_flags;
	result.neutral_mask = resource_mask & ~observation.resource_flags;
	if ((observation.resource_flags & resource_mask) == 0) return result;
	cleanup_result_ = MISTER_RESULT_CLEANUP_INCOMPLETE;
	result.call_result = MISTER_RESULT_CLEANUP_INCOMPLETE;
	result.cleanup_result = cleanup_result_;
	return result;
}

LifecycleResult AbiV2LifecyclePlatform::Destroy() {
	if (!runtime_ || live_state_ != LiveHandleState::live || runtime_state_ != MISTER_STATE_STOPPED) {
		cleanup_result_ = MISTER_RESULT_INVALID_STATE;
		return LifecycleResult(MISTER_RESULT_INVALID_STATE, primary_result_, cleanup_result_, runtime_state_);
	}
	const MisterResult destroyed = Normalize(MisterRuntime_DestroyV2(&runtime_));
	cleanup_result_ = destroyed;
	LifecycleResult result(destroyed, primary_result_, cleanup_result_, runtime_state_);
	if (result == LifecycleResult::ok && !runtime_) {
		live_owner_ = LifecycleOwner();
		live_state_ = LiveHandleState::none;
		runtime_state_ = MISTER_STATE_CREATED;
		result.runtime_state = runtime_state_;
	} else if (runtime_ && !SynchronizeRuntimeState()) {
		result.call_result = MISTER_RESULT_PLATFORM;
		result.cleanup_result = MISTER_RESULT_PLATFORM;
		cleanup_result_ = MISTER_RESULT_PLATFORM;
		result.runtime_state = runtime_state_;
	}
	return result;
}

LifecycleResult AbiV2LifecyclePlatform::Recover(uint32_t resource_mask, uint32_t deadline_ms) {
	if (live_state_ != LiveHandleState::none || runtime_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	MisterRecoveryObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	if (!platform_) return Result(MISTER_RESULT_INVALID_STATE, runtime_state_, primary_result_, cleanup_result_);
	MisterResult call = Normalize(MisterRuntime_RecoverPlatformV2(platform_, resource_mask, &observation, deadline_ms));
	cleanup_result_ = call;
	LifecycleResult result(call, primary_result_, cleanup_result_, runtime_state_);
	result.active_mask = observation.observed_resource_flags;
	result.observed_mask = observation.observed_resource_flags;
	result.neutral_mask = observation.neutral_resource_flags;
	return result == MISTER_RESULT_OK && (observation.neutral_resource_flags & resource_mask) == resource_mask &&
		(observation.observed_resource_flags & resource_mask) == 0 ? result :
		(result == MISTER_RESULT_OK ? LifecycleResult(MISTER_RESULT_CLEANUP_INCOMPLETE, primary_result_,
			MISTER_RESULT_CLEANUP_INCOMPLETE, runtime_state_) : result);
}

LifecycleResult AbiV2LifecyclePlatform::ProveNeutral(uint32_t resource_mask, uint32_t deadline_ms) { return Recover(resource_mask, deadline_ms); }
LifecycleResult AbiV2LifecyclePlatform::RecoverStateless(uint32_t resource_mask, uint32_t deadline_ms) { return Recover(resource_mask, deadline_ms); }
LiveHandleState AbiV2LifecyclePlatform::live_handle_state() const { return LiveHandleState(live_state_, live_owner_); }
bool AbiV2LifecyclePlatform::MainAbsent() { return false; }

Coordinator::Coordinator() : store_(std::string()), fence_(0), platform_(0), crash_injector_(0),
	clock_(&DefaultClock()), operation_deadline_(0), ready_(false), shutdown_(false) {}

Coordinator::Coordinator(const std::string& state_directory, const BackendFence* fence,
	LifecyclePlatform* platform, CommitCrashInjector* crash_injector, MonotonicClock* clock)
	: store_(state_directory), fence_(fence), platform_(platform), crash_injector_(crash_injector),
	clock_(clock ? clock : &DefaultClock()), operation_deadline_(0), ready_(false), shutdown_(false) {}

bool Coordinator::ready() const { return ready_; }

uint32_t Coordinator::Remaining(uint64_t deadline_at) const {
	if (!clock_ || deadline_at == 0) return 0;
	const uint64_t now = clock_->NowMs();
	if (now >= deadline_at) return 0;
	const uint64_t remaining = deadline_at - now;
	return remaining > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(remaining);
}

LifecycleResult Coordinator::NeutralStateless(uint32_t resource_mask, uint64_t deadline_at) {
	if (!platform_) return LifecycleResult::invalid_state;
	const uint32_t remaining = Remaining(deadline_at);
	if (remaining == 0) return LifecycleResult::deadline;
	LifecycleResult result = platform_->RecoverStateless(resource_mask, remaining);
	if (result != LifecycleResult::ok) return result;
	if ((result.neutral_mask & resource_mask) != resource_mask ||
		(result.observed_mask & resource_mask) != 0) return LifecycleResult::cleanup_incomplete;
	return result;
}

LifecycleResult Coordinator::NeutralLive(uint32_t resource_mask, uint64_t deadline_at,
	uint64_t cleanup_started) {
	if (!platform_) return LifecycleResult::invalid_state;
	// Prove the shorter group first.  This is not a performance preference: a
	// proof of video/audio/input/save/content after its fixed group deadline is
	// invalid even when FPGA's longer group still has budget.
	static const uint32_t order[] = {
		MISTER_RESOURCE_NATIVE_VIDEO, MISTER_RESOURCE_NATIVE_AUDIO, MISTER_RESOURCE_CORE_INPUT,
		MISTER_RESOURCE_SAVES, MISTER_RESOURCE_CONTENT, MISTER_RESOURCE_FPGA,
		MISTER_RESOURCE_BRIDGES, MISTER_RESOURCE_CORE_PROTOCOL
	};
	for (size_t index = 0; index < sizeof(order) / sizeof(order[0]); ++index) {
		const uint32_t bit = order[index];
		if ((resource_mask & bit) == 0) continue;
		const uint32_t remaining = CleanupDeadlineBudget(bit, deadline_at,
			cleanup_started, clock_->NowMs());
		if (remaining == 0) return LifecycleResult::deadline;
		LifecycleResult result = platform_->ObserveNeutral(bit, remaining);
		if (CleanupDeadlineBudget(bit, deadline_at, cleanup_started,
			clock_->NowMs()) == 0) return LifecycleResult::deadline;
		if (result != LifecycleResult::ok || (result.neutral_mask & bit) != bit ||
			(result.observed_mask & bit) != 0) return LifecycleResult::cleanup_incomplete;
	}
	return LifecycleResult::ok;
}

LifecycleResult Coordinator::CleanupLive(const Owner& owner, uint32_t resource_mask, uint64_t deadline_at) {
	if (!platform_) return LifecycleResult::invalid_state;
	const LiveHandleState handle = platform_->live_handle_state();
	if (handle != LiveHandleState::live || !SameOwner(handle.owner, owner)) return LifecycleResult::invalid_state;
	const uint64_t cleanup_started = clock_->NowMs();
	uint32_t proven = 0;
	uint32_t retained_deadline_mask = resource_mask;
	for (uint32_t attempt = 0; attempt < kMaxCleanupAttempts; ++attempt) {
		const uint32_t outstanding = resource_mask & ~proven;
		if (outstanding != 0) retained_deadline_mask = outstanding;
		const uint32_t observation_mask = outstanding != 0 ? outstanding : resource_mask;
		const uint32_t remaining = CleanupDeadlineBudget(retained_deadline_mask,
			deadline_at, cleanup_started, clock_->NowMs());
		if (remaining == 0) return LifecycleResult::deadline;
		LifecycleResult stopped = platform_->Stop(remaining);
		const bool stop_retryable = RetryableCleanup(stopped);
		if (stopped != LifecycleResult::ok && !stop_retryable) return stopped;
		const uint32_t observe_remaining = CleanupDeadlineBudget(retained_deadline_mask,
			deadline_at, cleanup_started, clock_->NowMs());
		if (observe_remaining == 0) return LifecycleResult::deadline;
		LifecycleResult observed = platform_->ObserveNeutral(observation_mask, observe_remaining);
		if (CleanupDeadlineBudget(retained_deadline_mask, deadline_at, cleanup_started,
			clock_->NowMs()) == 0) return LifecycleResult::deadline;
		// A full observation may not reassert a resource which was already
		// proven neutral, even though the next call asks only for the remainder.
		if ((observed.observed_mask & proven) != 0 ||
			(observed.neutral_mask & ~resource_mask) != 0) return LifecycleResult::cleanup_incomplete;
		const uint32_t newly_proven = observed.neutral_mask & observation_mask;
		if ((observed.observed_mask & newly_proven) != 0) return LifecycleResult::cleanup_incomplete;
		proven |= newly_proven;
		const bool observe_retryable = RetryableCleanup(observed);
		if (observed != LifecycleResult::ok && !observe_retryable) return observed;
		if (stop_retryable || observe_retryable) continue;
		if (stopped.runtime_state != MISTER_STATE_STOPPED ||
			(proven & resource_mask) != resource_mask) return LifecycleResult::cleanup_incomplete;
		// Final proof covers the whole original mask but consumes only the tail
		// group's remaining absolute deadline.
		const uint32_t proof_remaining = CleanupDeadlineBudget(retained_deadline_mask,
			deadline_at, cleanup_started, clock_->NowMs());
		if (proof_remaining == 0) return LifecycleResult::deadline;
		LifecycleResult proof = platform_->ObserveNeutral(resource_mask, proof_remaining);
		if (CleanupDeadlineBudget(retained_deadline_mask, deadline_at, cleanup_started,
			clock_->NowMs()) == 0) return LifecycleResult::deadline;
		if (proof != LifecycleResult::ok || (proof.neutral_mask & resource_mask) != resource_mask ||
			(proof.observed_mask & resource_mask) != 0) return LifecycleResult::cleanup_incomplete;
		return platform_->Destroy();
	}
	return LifecycleResult::cleanup_incomplete;
}

LifecycleResult Coordinator::CleanupStateless(uint32_t resource_mask, uint64_t deadline_at) {
	if (!platform_) return LifecycleResult::invalid_state;
	if (platform_->live_handle_state() != LiveHandleState::none) return LifecycleResult::invalid_state;
	const uint64_t cleanup_started = clock_->NowMs();
	uint32_t proven = 0;
	uint32_t retained_deadline_mask = resource_mask;
	for (uint32_t attempt = 0; (proven & resource_mask) != resource_mask && attempt < kMaxCleanupAttempts; ++attempt) {
		const uint32_t outstanding = resource_mask & ~proven;
		if (outstanding != 0) retained_deadline_mask = outstanding;
		const uint32_t remaining = CleanupDeadlineBudget(retained_deadline_mask,
			deadline_at, cleanup_started, clock_->NowMs());
		if (remaining == 0) return LifecycleResult::deadline;
		LifecycleResult result = platform_->RecoverStateless(outstanding, remaining);
		if (CleanupDeadlineBudget(retained_deadline_mask, deadline_at, cleanup_started,
			clock_->NowMs()) == 0) return LifecycleResult::deadline;
		if ((result.observed_mask & proven) != 0 || (result.neutral_mask & ~resource_mask) != 0)
			return LifecycleResult::cleanup_incomplete;
		const uint32_t newly_proven = result.neutral_mask & outstanding;
		if ((result.observed_mask & newly_proven) != 0)
			return LifecycleResult::cleanup_incomplete;
		proven |= newly_proven;
		if (RetryableCleanup(result)) continue;
		if (result != LifecycleResult::ok) return result;
		if (newly_proven == 0) return LifecycleResult::cleanup_incomplete;
	}
	if ((proven & resource_mask) != resource_mask) return LifecycleResult::cleanup_incomplete;
	const uint32_t remaining = CleanupDeadlineBudget(retained_deadline_mask,
		deadline_at, cleanup_started, clock_->NowMs());
	if (remaining == 0) return LifecycleResult::deadline;
	LifecycleResult proof = platform_->ProveNeutral(resource_mask, remaining);
	if (CleanupDeadlineBudget(retained_deadline_mask, deadline_at, cleanup_started,
		clock_->NowMs()) == 0) return LifecycleResult::deadline;
	if (proof != LifecycleResult::ok || (proof.neutral_mask & resource_mask) != resource_mask ||
		(proof.observed_mask & resource_mask) != 0) return LifecycleResult::cleanup_incomplete;
	return proof;
}

ErrorClass Coordinator::Initialize(uint64_t backend_epoch) {
	ready_ = false;
	operation_deadline_ = clock_->NowMs() + kStartupDeadline;
	if (!fence_ || backend_epoch == 0) return ErrorClass::schema;
	BackendFenceRecord fence;
	if (fence_->Load(&fence) != ErrorClass::ok || fence.authority_epoch != backend_epoch ||
		(fence.state != "native" && fence.state != "native_quiescing")) return ErrorClass::transition;
	const StateLoadStatus loaded = store_.Load(&state_);
	if (loaded == StateLoadStatus::loaded) {
		if (state_.backend_epoch == backend_epoch) return Restore(backend_epoch);
		if (fence.state != "native" || state_.backend_epoch + 1 != backend_epoch) return ErrorClass::transition;
		const ErrorClass adopted = AdoptEpoch(backend_epoch);
		return adopted == ErrorClass::ok ? Restore(backend_epoch, true) : adopted;
	}
	if (loaded == StateLoadStatus::invalid) return ErrorClass::schema;
	if (fence.state != "native") return ErrorClass::transition;
	if (!platform_ || platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok)
		return ErrorClass::transition;
	state_ = Idle(1, backend_epoch);
	if (store_.Commit(state_) != ErrorClass::ok) return ErrorClass::schema;
	ready_ = true;
	return ErrorClass::ok;
}

ErrorClass Coordinator::Restore(uint64_t backend_epoch, bool already_neutral) {
	if (state_.backend_epoch != backend_epoch) return ErrorClass::transition;
	detail::Token root;
	if (detail::ScanV1Json(state_.canonical, 256 * 1024, &root) != ErrorClass::ok ||
		!ParseOwner(detail::Member(root, "owner"), &owner_)) return ErrorClass::schema;
	Owner release_owner;
	Owner candidate;
	if (!ParseOwner(detail::Member(root, "release_owner"), &release_owner) ||
		!ParseOwner(detail::Member(root, "candidate"), &candidate)) return ErrorClass::schema;
	release_owner_ = release_owner;
	candidate_ = candidate;
	const detail::Token* core = detail::Member(root, "observed_core");
	expected_core_ = core && core->kind == detail::Token::Kind::string ? core->text : std::string();
	const detail::Token* content = detail::Member(root, "content_lease");
	content_lease_ = content && content->kind != detail::Token::Kind::null_value ?
		detail::Encode(*content) : std::string();
	in_flight_id_.clear();
	in_flight_digest_.clear();
	in_flight_stage_.clear();
	const detail::Token* in_flight = detail::Member(root, "in_flight");
	if (in_flight && in_flight->kind == detail::Token::Kind::object &&
		(!Text(detail::Member(*in_flight, "operation_id"), &in_flight_id_) ||
		 !Text(detail::Member(*in_flight, "request_digest"), &in_flight_digest_) ||
		 !Text(detail::Member(*in_flight, "stage"), &in_flight_stage_))) return ErrorClass::schema;
	ledger_.clear();
	const detail::Token* ledger = detail::Member(root, "ledger");
	if (!ledger || ledger->kind != detail::Token::Kind::array) return ErrorClass::schema;
	for (size_t i = 0; i < ledger->array.size(); ++i) {
		Terminal terminal;
		if (!ParseTerminal(ledger->array[i], &terminal)) return ErrorClass::schema;
		ledger_.push_back(terminal);
	}
	const std::string error = ErrorCode(detail::Member(root, "last_error"));
	const bool began_no_owner = state_.phase == "no_owner";
	const Owner none;
	Terminal interrupted;
	bool has_inflight = !in_flight_id_.empty();
	if (has_inflight) {
		interrupted.operation_id = in_flight_id_;
		interrupted.digest = in_flight_digest_;
		interrupted.ok = false;
		interrupted.error = "INTERRUPTED";
	}
	if (state_.phase == "failed") {
		if (has_inflight && Commit("failed", owner_, release_owner, candidate, expected_core_,
			content_lease_, std::string(), error, &interrupted) != ErrorClass::ok) return ErrorClass::schema;
		ready_ = true;
		return ErrorClass::ok;
	}
	if (state_.phase == "idle" && !has_inflight) {
		if (!already_neutral && (!platform_ || platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok)) return ErrorClass::transition;
		ready_ = true;
		return ErrorClass::ok;
	}
	if (state_.phase == "idle") {
		if (!platform_ || platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok ||
			Commit("idle", none, none, none, std::string(), std::string(), std::string(),
				"INTERRUPTED", &interrupted) != ErrorClass::ok) return ErrorClass::transition;
		ready_ = true;
		return ErrorClass::ok;
	}
	// ABI v2 cannot attach after a daemon crash.  An intent/releasing candidate
	// has no lease, so it must never be reset as though it owned hardware.  Only
	// the recorded old owner is release-only at that point.  Transferred,
	// unwinding, and active records instead name the owner that must be reset.
	if (state_.phase == "intent" || state_.phase == "releasing") {
		const std::string inflight = has_inflight ? "transitioning" : std::string();
		if (owner_.present) {
			if (Commit("releasing", owner_, owner_, candidate, expected_core_, content_lease_,
				inflight, error.empty() ? "INTERRUPTED" : error, 0) != ErrorClass::ok ||
				CleanupStateless(kAllResources, operation_deadline_) != LifecycleResult::ok) return ErrorClass::transition;
		} else if (!platform_ || platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok) {
			return ErrorClass::transition;
		}
		if (Commit("no_owner", Owner(), Owner(), Owner(), std::string(), std::string(),
			inflight, error.empty() ? "INTERRUPTED" : error, 0) != ErrorClass::ok)
			return ErrorClass::transition;
	} else if (state_.phase == "active" || state_.phase == "transferred" ||
		state_.phase == "unwinding") {
		const Owner reset_owner = owner_.present ? owner_ : candidate;
		const std::string inflight = has_inflight ? "transitioning" : std::string();
		if (Commit("releasing", reset_owner, reset_owner, none, expected_core_, content_lease_,
			inflight, error.empty() ? "INTERRUPTED" : error, 0) != ErrorClass::ok ||
			CleanupStateless(kAllResources, operation_deadline_) != LifecycleResult::ok ||
			Commit("no_owner", none, none, none, std::string(), std::string(), inflight,
				error.empty() ? "INTERRUPTED" : error, 0) != ErrorClass::ok) return ErrorClass::transition;
	}
	if (began_no_owner && (!platform_ || platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok)) return ErrorClass::transition;
	if (has_inflight) {
		if (Commit("idle", none, none, none, std::string(), std::string(), std::string(),
			error.empty() ? "INTERRUPTED" : error, &interrupted) != ErrorClass::ok) return ErrorClass::schema;
	} else if (Commit("idle", none, none, none, std::string(), std::string(), std::string(),
		"INTERRUPTED", 0) != ErrorClass::ok) return ErrorClass::schema;
	ready_ = true;
	return ErrorClass::ok;
}

ErrorClass Coordinator::AdoptEpoch(uint64_t backend_epoch) {
	// The store already authenticated this record.  Adoption accepts only a true
	// no-owner checkpoint and changes exactly the two metadata counters.
	detail::Token root;
	if (detail::ScanV1Json(state_.canonical, 256 * 1024, &root) != ErrorClass::ok ||
		(state_.phase != "idle" && state_.phase != "no_owner") ||
		!Null(detail::Member(root, "owner")) || !Null(detail::Member(root, "release_owner")) ||
		!Null(detail::Member(root, "candidate")) || !EmptyArray(detail::Member(root, "leases")) ||
		!Null(detail::Member(root, "content_lease")) || !Null(detail::Member(root, "in_flight")) ||
		!platform_ || !platform_->MainAbsent() ||
		platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok)
		return ErrorClass::transition;
	const std::string source = state_.canonical.substr(0, state_.canonical.size() - 1) +
		",\"sha256\":\"" + state_.digest + "\"}";
	StateRecord adopted;
	if (ReencodeStateIdentity(source, state_.sequence + 1, backend_epoch, &adopted) !=
		ErrorClass::ok) return ErrorClass::schema;
	if (store_.Commit(adopted) != ErrorClass::ok) return ErrorClass::schema;
	state_ = adopted;
	return ErrorClass::ok;
}

bool Coordinator::FenceAllows(const std::string& operation) const {
	BackendFenceRecord fence;
	if (!fence_ || fence_->Load(&fence) != ErrorClass::ok ||
		fence.authority_epoch != state_.backend_epoch) return false;
	if (fence.state == "native") return true;
	if (fence.state != "native_quiescing") return false;
	if (operation == "recover") return true;
	if (operation == "stop") return owner_.present;
	return operation == "shutdown" && !owner_.present && state_.phase == "idle";
}

bool Coordinator::PhaseAllows(const std::string& operation) const {
	if (state_.phase == "idle")
		return operation == "launch" || operation == "recover" || operation == "shutdown";
	if (state_.phase == "active")
		return operation == "launch" || operation == "stop" || operation == "recover";
	return state_.phase == "failed" && operation == "recover";
}

bool Coordinator::ParseMutation(const std::string& request, std::string* operation,
	std::string* operation_id, std::string* digest, Owner* candidate,
	Owner* requested_owner, uint64_t* expected_sequence, LaunchMetadata* launch,
	std::string* content_lease) const {
	std::string canonical;
	if (ParseWire(request, &canonical, digest) != ErrorClass::ok) return false;
	detail::Token root;
	if (detail::ScanV1Json(canonical, 65536, &root) != ErrorClass::ok ||
		!Text(detail::Member(root, "operation"), operation) ||
		!Text(detail::Member(root, "operation_id"), operation_id)) return false;
	if (*operation != "launch" && *operation != "stop" && *operation != "recover" && *operation != "shutdown") return false;
	const detail::Token* precondition = detail::Member(root, "precondition");
	if (!precondition || precondition->kind != detail::Token::Kind::object ||
		!detail::PositiveUint63(detail::Member(*precondition, "sequence"), expected_sequence) ||
		!ParseOwner(detail::Member(*precondition, "owner"), requested_owner)) return false;
	*candidate = Owner();
	*launch = LaunchMetadata();
	*content_lease = std::string();
	if (*operation == "launch") {
		if (!ParseOwner(detail::Member(root, "candidate"), candidate) || !candidate->present) return false;
		const detail::Token* body = detail::Member(root, "body");
		const detail::Token* lease_id = body ? detail::Member(*body, "cache_lease_id") : 0;
		const detail::Token* content = body ? detail::Member(*body, "content") : 0;
		const detail::Token* hash = content ? detail::Member(*content, "sha256") : 0;
		const detail::Token* size = content ? detail::Member(*content, "size") : 0;
		const detail::Token* extension = content ? detail::Member(*content, "extension") : 0;
		if (!Text(body ? detail::Member(*body, "game_id") : 0, &launch->game_id) ||
			!Text(body ? detail::Member(*body, "system") : 0, &launch->system) ||
			!Text(body ? detail::Member(*body, "expected_core") : 0, &launch->expected_core) ||
			!Text(hash, &launch->sha256) || !detail::PositiveUint63(size, &launch->size) ||
			!Text(extension, &launch->extension) || !lease_id ||
			lease_id->kind != detail::Token::Kind::string || !content) return false;
		*content_lease = std::string("{\"lease_id\":\"") + lease_id->text + "\",\"content\":" +
			detail::Encode(*content) + '}';
	} else if (*operation == "stop" || *operation == "recover") {
		Owner top_owner;
		if (!ParseOwner(detail::Member(root, "owner"), &top_owner) || !SameOwner(top_owner, *requested_owner)) return false;
	}
	return true;
}

ErrorClass Coordinator::Commit(const std::string& phase, const Owner& owner,
	const Owner& release_owner, const Owner& candidate, const std::string& expected_core,
	const std::string& content_lease, const std::string& in_flight,
	const std::string& last_error, const Terminal* terminal) {
	const uint64_t next = state_.sequence + 1;
	const bool leased = phase == "active" || phase == "transferred" || phase == "unwinding" ||
		(phase == "failed" && owner.present) ||
		((phase == "intent" || phase == "releasing") && owner.present);
	const std::string leases = leased ? LeasesJson(owner) : "[]";
	const std::string content = leased ? content_lease : "null";
	const std::string observed = (phase == "active" && !expected_core.empty()) ?
		std::string("\"") + expected_core + "\"" : "null";
	const std::string mode = phase == "active" ? "fpga_native" : (phase == "failed" ? "failed" :
		(phase == "idle" ? "idle" : "recovering"));
	std::string snapshot = std::string("{\"sequence\":") + std::to_string(next) +
		",\"backend_epoch\":" + std::to_string(state_.backend_epoch) + ",\"phase\":\"" + phase +
		"\",\"mode\":\"" + mode + "\",\"owner\":" + OwnerJson(owner) +
		",\"release_owner\":" + OwnerJson(release_owner) + ",\"candidate\":" + OwnerJson(candidate) +
		",\"leases\":" + leases + ",\"content_lease\":" + content +
		",\"observed_core\":" + observed + ",\"capabilities\":31,\"last_error\":" + ErrorJson(last_error) + '}';
	std::vector<Terminal> pending = ledger_;
	if (terminal) {
		Terminal appended = *terminal;
		appended.snapshot = snapshot;
		appended.sequence = next;
		pending.push_back(appended);
		if (pending.size() > 64) pending.erase(pending.begin());
	}
	std::string ledger("[");
	for (size_t i = 0; i < pending.size(); ++i) {
		if (i) ledger += ',';
		ledger += std::string("{\"operation_id\":\"") + pending[i].operation_id +
			"\",\"request_digest\":\"" + pending[i].digest + "\",\"ok\":" +
			(pending[i].ok ? "true" : "false") + ",\"error\":" + ErrorJson(pending[i].error) +
			",\"snapshot\":" + pending[i].snapshot + ",\"resulting_sequence\":" +
			std::to_string(pending[i].sequence) + '}';
	}
	ledger += ']';
	const std::string inflight_json = in_flight.empty() ? "null" : std::string("{\"operation_id\":\"") +
		in_flight_id_ + "\",\"request_digest\":\"" + in_flight_digest_ + "\",\"stage\":\"" + in_flight + "\"}";
	StateRecord record;
	record.sequence = next;
	record.backend_epoch = state_.backend_epoch;
	record.phase = phase;
	record.canonical = std::string("{\"record_version\":1,\"protocol\":1,\"abi_version\":2,\"backend_epoch\":") +
		std::to_string(record.backend_epoch) + ",\"sequence\":" + std::to_string(next) +
		",\"phase\":\"" + phase + "\",\"mode\":\"" + mode + "\",\"owner\":" + OwnerJson(owner) +
		",\"release_owner\":" + OwnerJson(release_owner) + ",\"candidate\":" + OwnerJson(candidate) +
		",\"leases\":" + leases + ",\"content_lease\":" + content + ",\"observed_core\":" + observed +
		",\"capabilities\":31,\"last_error\":" + ErrorJson(last_error) + ",\"in_flight\":" +
		inflight_json + ",\"ledger\":" + ledger + '}';
	record.digest = Sha256Hex(record.canonical);
	if (store_.Commit(record) != ErrorClass::ok) return ErrorClass::schema;
	state_ = record;
	owner_ = owner;
	release_owner_ = release_owner;
	candidate_ = candidate;
	expected_core_ = expected_core;
	content_lease_ = content_lease;
	if (terminal) {
		ledger_ = pending;
		in_flight_id_.clear();
		in_flight_digest_.clear();
		in_flight_stage_.clear();
	}
	const std::string crash_phase = in_flight == "recorded" ? "recorded" : phase;
	if (crash_injector_ && (crash_injector_->AfterCommit(crash_phase) ||
		(terminal && crash_phase != "terminal" && crash_injector_->AfterCommit("terminal"))))
		return ErrorClass::transition;
	return ErrorClass::ok;
}

ErrorClass Coordinator::Unwind(const Owner& candidate, const std::string& operation_id,
	const std::string& digest, const std::string& primary_error) {
	ErrorClass result = Commit("unwinding", candidate, candidate, candidate, expected_core_, content_lease_, "transitioning", primary_error, 0);
	if (result != ErrorClass::ok) return result;
	LifecycleResult cleanup = LifecycleResult::invalid_state;
	if (platform_) {
		const LiveHandleState handle = platform_->live_handle_state();
		if (handle == LiveHandleState::live) cleanup = SameOwner(handle.owner, candidate) ?
			CleanupLive(candidate, kAllResources, operation_deadline_) : LifecycleResult::invalid_state;
		else if (handle == LiveHandleState::none) cleanup = CleanupStateless(kAllResources, operation_deadline_);
		else cleanup = LifecycleResult::exit_required;
	}
	if (cleanup != LifecycleResult::ok) {
		Terminal failure = {operation_id, digest, false, primary_error, std::string(), 0};
		return Commit("failed", candidate, candidate, candidate, expected_core_, content_lease_, std::string(), primary_error, &failure);
	}
	const Owner none;
	result = Commit("no_owner", none, none, none, std::string(), std::string(), "transitioning", primary_error, 0);
	if (result != ErrorClass::ok) return result;
	Terminal failure = {operation_id, digest, false, primary_error, std::string(), 0};
	return Commit("idle", none, none, none, std::string(), std::string(), std::string(), primary_error, &failure);
}

ErrorClass Coordinator::Release(const Owner& old_owner, const Owner& candidate,
	const std::string& operation_id, const std::string& digest, const LaunchMetadata& launch,
	const std::string& content_lease) {
	const Owner none;
	ErrorClass result = Commit("intent", old_owner, old_owner, candidate, launch.expected_core, content_lease, "transitioning", std::string(), 0);
	if (result != ErrorClass::ok) return result;
	result = Commit("releasing", old_owner, old_owner, candidate, launch.expected_core, content_lease, "transitioning", std::string(), 0);
	if (result != ErrorClass::ok) return result;
	if (!platform_) return Unwind(candidate, operation_id, digest, "RECOVERY_REQUIRED");
	if (old_owner.present) {
		const LiveHandleState handle = platform_->live_handle_state();
		const LifecycleResult cleanup = handle == LiveHandleState::live ?
			(SameOwner(handle.owner, old_owner) ? CleanupLive(old_owner, kAllResources, operation_deadline_) : LifecycleResult::invalid_state) :
			(handle == LiveHandleState::none ? CleanupStateless(kAllResources, operation_deadline_) : LifecycleResult::exit_required);
		if (cleanup != LifecycleResult::ok) {
			Terminal failure = {operation_id, digest, false, "RECOVERY_REQUIRED", std::string(), 0};
			return Commit("failed", old_owner, old_owner, candidate, launch.expected_core, content_lease, std::string(), "RECOVERY_REQUIRED", &failure);
		}
	} else if (Remaining(operation_deadline_) == 0) {
		return Unwind(candidate, operation_id, digest, "DEADLINE");
	} else if (platform_->ProveNeutral(kAllResources, Remaining(operation_deadline_)) != LifecycleResult::ok) {
		return Unwind(candidate, operation_id, digest, "RECOVERY_REQUIRED");
	}
	result = Commit("no_owner", none, none, none, std::string(), std::string(), "transitioning", std::string(), 0);
	if (result != ErrorClass::ok) return result;
	result = Commit("transferred", candidate, none, candidate, launch.expected_core, content_lease, "transitioning", std::string(), 0);
	if (result != ErrorClass::ok) return result;
	if (Remaining(operation_deadline_) == 0) return Unwind(candidate, operation_id, digest, "DEADLINE");
	LifecycleResult activation = platform_->GrantTransfer(candidate);
	if (activation != LifecycleResult::ok) return Unwind(candidate, operation_id, digest, PrimaryError(activation));
	if (Remaining(operation_deadline_) == 0) return Unwind(candidate, operation_id, digest, "DEADLINE");
	activation = platform_->Create(candidate);
	if (activation != LifecycleResult::ok) return Unwind(candidate, operation_id, digest, PrimaryError(activation));
	uint32_t remaining = Remaining(operation_deadline_);
	if (remaining == 0) return Unwind(candidate, operation_id, digest, "DEADLINE");
	activation = platform_->Start(remaining);
	if (activation != LifecycleResult::ok) return Unwind(candidate, operation_id, digest, PrimaryError(activation));
	remaining = Remaining(operation_deadline_);
	if (remaining == 0) return Unwind(candidate, operation_id, digest, "DEADLINE");
	activation = platform_->Load(launch, remaining);
	if (activation != LifecycleResult::ok) return Unwind(candidate, operation_id, digest, PrimaryError(activation));
	remaining = Remaining(operation_deadline_);
	if (remaining == 0) return Unwind(candidate, operation_id, digest, "DEADLINE");
	activation = platform_->Observe(launch.expected_core, remaining);
	if (activation != LifecycleResult::ok) return Unwind(candidate, operation_id, digest, PrimaryError(activation));
	Terminal success = {operation_id, digest, true, std::string(), std::string(), 0};
	return Commit("active", candidate, none, none, launch.expected_core, content_lease, std::string(), std::string(), &success);
}

ErrorClass Coordinator::Admit(const std::string& request) {
	std::string operation, operation_id, digest, content_lease;
	LaunchMetadata launch;
	Owner candidate, requested;
	uint64_t expected_sequence = 0;
	if (!ParseMutation(request, &operation, &operation_id, &digest, &candidate, &requested,
		&expected_sequence, &launch, &content_lease)) return ErrorClass::schema;
	const Admission admission = AdmitParsed(operation, operation_id, digest, candidate, requested,
		expected_sequence, content_lease);
	return admission.kind == Admission::newly_recorded || admission.kind == Admission::terminal_replay ?
		ErrorClass::ok : ErrorClass::transition;
}

Coordinator::Admission Coordinator::AdmitParsed(const std::string& operation,
	const std::string& operation_id, const std::string& digest, const Owner& candidate,
	const Owner& requested, uint64_t expected_sequence, const std::string& content_lease) {
	Admission admission;
	(void)content_lease;
	if (!ready_ || shutdown_ || !FenceAllows(operation)) { admission.code = CoordinatorCode::not_ready; return admission; }
	for (size_t i = 0; i < ledger_.size(); ++i) if (ledger_[i].operation_id == operation_id) {
		admission.kind = ledger_[i].digest == digest ? Admission::terminal_replay : Admission::rejected;
		admission.code = ledger_[i].digest == digest ? CoordinatorCode::none : CoordinatorCode::operation_replay;
		return admission;
	}
	if (!in_flight_id_.empty()) {
		admission.kind = in_flight_id_ == operation_id && in_flight_digest_ == digest ?
			Admission::in_progress_replay : Admission::rejected;
		admission.code = admission.kind == Admission::in_progress_replay ? CoordinatorCode::in_progress :
			(in_flight_id_ == operation_id ? CoordinatorCode::operation_replay : CoordinatorCode::busy);
		return admission;
	}
	if (expected_sequence != state_.sequence || !SameOwner(requested, owner_)) { admission.code = CoordinatorCode::stale_admission; return admission; }
	if (operation == "launch" && SameOwner(candidate, owner_)) { admission.code = CoordinatorCode::stale_generation; return admission; }
	if (operation == "stop" && !owner_.present) { admission.code = CoordinatorCode::owner_not_found; return admission; }
	if (operation == "shutdown" && (owner_.present || state_.phase != "idle")) { admission.code = CoordinatorCode::stale_admission; return admission; }
	if (state_.phase == "failed" && (operation == "launch" || operation == "stop")) { admission.code = CoordinatorCode::recovery_required; return admission; }
	if (state_.phase != "idle" && state_.phase != "active" && state_.phase != "failed") { admission.code = CoordinatorCode::busy; return admission; }
	if (!PhaseAllows(operation)) { admission.code = CoordinatorCode::stale_admission; return admission; }
	// The recorded commit is itself inside the operation's absolute budget: a
	// crash hook or durable store delay cannot manufacture a fresh lifecycle
	// window after admission.
	const uint32_t budget = operation == "launch" ? kLaunchDeadline :
		(operation == "recover" ? kRecoverDeadline :
		(operation == "shutdown" ? kShutdownDeadline : kStopDeadline));
	operation_deadline_ = clock_->NowMs() + budget;
	in_flight_id_ = operation_id;
	in_flight_digest_ = digest;
	// Recording admission is an in-flight overlay, not a state repair.  In
	// particular a failed record's durable diagnostic must survive a crash at
	// this exact boundary.
	std::string retained_error;
	detail::Token root;
	if (detail::ScanV1Json(state_.canonical, 256 * 1024, &root) != ErrorClass::ok) return admission;
	retained_error = ErrorCode(detail::Member(root, "last_error"));
	if (Commit(state_.phase, owner_, release_owner_, candidate_, expected_core_, content_lease_,
		"recorded", retained_error, 0) != ErrorClass::ok) return admission;
	admission.kind = Admission::newly_recorded;
	admission.code = CoordinatorCode::none;
	return admission;
}

ErrorClass Coordinator::ExecuteAdmitted(const std::string& operation,
	const std::string& operation_id, const std::string& digest, const Owner& candidate,
	const Owner& requested, const LaunchMetadata& launch, const std::string& content_lease) {
	(void)requested;
	if (operation == "launch") return Release(owner_, candidate, operation_id, digest, launch, content_lease);
	const Owner none;
	if (operation == "shutdown") {
		if (owner_.present || state_.phase != "idle")
			return ErrorClass::transition;
		std::string terminal_error;
		if (!platform_ || Remaining(operation_deadline_) == 0) {
			terminal_error = Remaining(operation_deadline_) == 0 ? "DEADLINE" : "RECOVERY_REQUIRED";
		} else {
			const LifecycleResult neutral = platform_->ProveNeutral(kAllResources,
				Remaining(operation_deadline_));
			if (Remaining(operation_deadline_) == 0) terminal_error = "DEADLINE";
			else if (neutral != LifecycleResult::ok) terminal_error = "RECOVERY_REQUIRED";
		}
		if (!terminal_error.empty()) {
			Terminal failure = {operation_id, digest, false, terminal_error, std::string(), 0};
			return Commit("failed", none, none, none, std::string(), std::string(), std::string(),
				terminal_error, &failure);
		}
		Terminal success = {operation_id, digest, true, std::string(), std::string(), 0};
		const ErrorClass result = Commit("idle", none, none, none, std::string(), std::string(), std::string(), std::string(), &success);
		if (result == ErrorClass::ok) shutdown_ = true;
		return result;
	}
	const ErrorClass releasing = Commit("releasing", owner_, owner_, none, expected_core_, content_lease_, "transitioning", std::string(), 0);
	if (releasing != ErrorClass::ok) return releasing;
	bool neutral = false;
	if (owner_.present && platform_) {
		const LiveHandleState handle = platform_->live_handle_state();
		neutral = handle == LiveHandleState::live ?
			(SameOwner(handle.owner, owner_) && CleanupLive(owner_, kAllResources, operation_deadline_) == LifecycleResult::ok) :
			(handle == LiveHandleState::none && CleanupStateless(kAllResources, operation_deadline_) == LifecycleResult::ok);
	} else neutral = platform_ && CleanupStateless(kAllResources, operation_deadline_) == LifecycleResult::ok;
	if (!neutral) {
		Terminal failure = {operation_id, digest, false, "RECOVERY_REQUIRED", std::string(), 0};
		return Commit("failed", owner_, owner_, none, expected_core_, content_lease_, std::string(), "RECOVERY_REQUIRED", &failure);
	}
	const ErrorClass no_owner = Commit("no_owner", none, none, none, std::string(), std::string(), "transitioning", std::string(), 0);
	if (no_owner != ErrorClass::ok) return no_owner;
	Terminal success = {operation_id, digest, true, std::string(), std::string(), 0};
	return Commit("idle", none, none, none, std::string(), std::string(), std::string(), std::string(), &success);
}

CoordinatorSnapshot Coordinator::snapshot() const { return SnapshotFromCanonical(state_.canonical); }

const char* Coordinator::ProtocolCode(CoordinatorCode code) {
	switch (code) {
	case CoordinatorCode::none: return 0;
	case CoordinatorCode::invalid_request: return "INVALID_REQUEST";
	case CoordinatorCode::busy: return "BUSY";
	case CoordinatorCode::not_ready: return "NOT_READY";
	case CoordinatorCode::stale_admission: return "STALE_ADMISSION";
	case CoordinatorCode::stale_generation: return "STALE_GENERATION";
	case CoordinatorCode::in_progress: return "IN_PROGRESS";
	case CoordinatorCode::operation_replay: return "OPERATION_REPLAY";
	case CoordinatorCode::operation_unknown: return "OPERATION_UNKNOWN";
	case CoordinatorCode::interrupted: return "INTERRUPTED";
	case CoordinatorCode::deadline: return "DEADLINE";
	case CoordinatorCode::recovery_required: return "RECOVERY_REQUIRED";
	case CoordinatorCode::unsupported_mode: return "UNSUPPORTED_MODE";
	case CoordinatorCode::owner_not_found: return "OWNER_NOT_FOUND";
	case CoordinatorCode::internal: return "INTERNAL";
	}
	return "INTERNAL";
}

MutationReply Coordinator::ExecuteMutation(const std::string& request) {
	MutationReply reply;
	std::string operation, operation_id, digest, content_lease;
	LaunchMetadata launch;
	Owner candidate, requested;
	uint64_t expected_sequence = 0;
	if (!ParseMutation(request, &operation, &operation_id, &digest, &candidate, &requested,
		&expected_sequence, &launch, &content_lease)) {
		reply.code = CoordinatorCode::invalid_request;
		return reply;
	}
	reply.operation_id = operation_id;
	reply.request_digest = digest;
	reply.snapshot = snapshot();
	const Admission admission = AdmitParsed(operation, operation_id, digest, candidate, requested,
		expected_sequence, content_lease);
	if (admission.kind == Admission::terminal_replay) {
		for (size_t i = 0; i < ledger_.size(); ++i) if (ledger_[i].operation_id == operation_id) {
			const Terminal& terminal = ledger_[i];
		reply.kind = MutationReply::Kind::terminal;
		reply.ok = terminal.ok;
		reply.code = terminal.ok ? CoordinatorCode::none : CoordinatorCodeFromTerminal(terminal.error);
		reply.snapshot.valid = !terminal.snapshot.empty();
		reply.snapshot.canonical = terminal.snapshot;
		reply.resulting_sequence = terminal.sequence;
		return reply;
	}
	}
	if (admission.kind == Admission::in_progress_replay) { reply.kind = MutationReply::Kind::in_progress; reply.code = admission.code; return reply; }
	if (admission.kind != Admission::newly_recorded) { reply.code = admission.code; reply.snapshot = snapshot(); return reply; }
	const ErrorClass result = ExecuteAdmitted(operation, operation_id, digest, candidate, requested,
		launch, content_lease);
	for (size_t i = 0; i < ledger_.size(); ++i) {
		const Terminal& terminal = ledger_[i];
		if (terminal.operation_id != operation_id) continue;
		reply.kind = MutationReply::Kind::terminal;
		reply.ok = terminal.ok;
		reply.code = terminal.ok ? CoordinatorCode::none : CoordinatorCodeFromTerminal(terminal.error);
		reply.snapshot.valid = !terminal.snapshot.empty();
		reply.snapshot.canonical = terminal.snapshot;
		reply.resulting_sequence = terminal.sequence;
		return reply;
	}
	(void)result;
	reply.code = CoordinatorCode::internal;
	reply.snapshot = snapshot();
	return reply;
}

ErrorClass Coordinator::Execute(const std::string& request) {
	const MutationReply reply = ExecuteMutation(request);
	if (reply.kind == MutationReply::Kind::terminal) return ErrorClass::ok;
	return reply.code == CoordinatorCode::invalid_request ? ErrorClass::schema : ErrorClass::transition;
}

Coordinator::OperationStatus Coordinator::operation_status(const std::string& operation_id) const {
	OperationStatus status;
	if (!in_flight_id_.empty() && in_flight_id_ == operation_id) {
		status.kind = OperationStatus::in_flight;
		status.code = CoordinatorCode::in_progress;
		status.request_digest = in_flight_digest_;
		return status;
	}
	for (size_t i = 0; i < ledger_.size(); ++i) {
		if (ledger_[i].operation_id != operation_id) continue;
		status.kind = OperationStatus::completed;
		status.ok = ledger_[i].ok;
		status.code = ledger_[i].ok ? CoordinatorCode::none : CoordinatorCodeFromTerminal(ledger_[i].error);
		status.request_digest = ledger_[i].digest;
		status.error = ledger_[i].error;
		status.snapshot = ledger_[i].snapshot;
		status.resulting_sequence = ledger_[i].sequence;
		return status;
	}
	return status;
}

uint64_t Coordinator::sequence() const { return state_.sequence; }
const std::string& Coordinator::canonical_state() const { return state_.canonical; }
const std::string& Coordinator::phase() const { return state_.phase; }

}  // namespace fogcast
