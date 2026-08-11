// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_RUNTIME_COORDINATOR_HPP
#define FOGCAST_RUNTIME_COORDINATOR_HPP

#include "fogcast/backend_fence.hpp"
#include "fogcast/runtime_store.hpp"
#include "runtime/mister_runtime.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace fogcast {

// Pure cleanup-deadline policy seam.  A zero resource mask has no applicable
// cleanup budget and therefore always returns zero.
uint32_t CleanupDeadlineBudget(uint32_t resource_mask, uint64_t global_deadline,
	uint64_t cleanup_started, uint64_t now);

// A durable owner names a generation; it is never an ABI handle.
struct LifecycleOwner {
	bool present;
	std::string session;
	uint64_t generation;
	LifecycleOwner() : present(false), generation(0) {}
};

// These are the exact admitted launch bytes.  cache_lease is coordinator-only
// durable metadata and is intentionally absent from the ABI launch structure.
struct LaunchMetadata {
	std::string game_id;
	std::string system;
	std::string expected_core;
	std::string sha256;
	uint64_t size;
	std::string extension;
	LaunchMetadata() : size(0) {}
};

// Do not collapse ABI lifecycle outcomes: cleanup policy depends on the exact
// state returned by the runtime.  `primary_result` is immutable once a launch
// call fails; later cleanup is recorded separately in `cleanup_result`.
struct LifecycleResult {
	MisterResult call_result;
	MisterResult primary_result;
	MisterResult cleanup_result;
	MisterStateV2 runtime_state;
	uint32_t active_mask;
	uint32_t observed_mask;
	uint32_t neutral_mask;
	bool ready;
	std::string observed_core;
	LifecycleResult(MisterResult call = MISTER_RESULT_OK,
		MisterResult primary = MISTER_RESULT_OK,
		MisterResult cleanup = MISTER_RESULT_OK,
		MisterStateV2 state = MISTER_STATE_CREATED)
		: call_result(call), primary_result(primary), cleanup_result(cleanup),
		  runtime_state(state), active_mask(0), observed_mask(0), neutral_mask(0), ready(false) {}
	bool is_ok() const { return call_result == MISTER_RESULT_OK; }
	static constexpr MisterResult ok = MISTER_RESULT_OK;
	static constexpr MisterResult invalid_argument = MISTER_RESULT_INVALID_ARGUMENT;
	static constexpr MisterResult invalid_state = MISTER_RESULT_INVALID_STATE;
	static constexpr MisterResult unsupported = MISTER_RESULT_UNSUPPORTED;
	static constexpr MisterResult deadline = MISTER_RESULT_DEADLINE;
	static constexpr MisterResult platform = MISTER_RESULT_PLATFORM;
	static constexpr MisterResult cleanup_incomplete = MISTER_RESULT_CLEANUP_INCOMPLETE;
	static constexpr MisterResult exit_required = MISTER_RESULT_EXIT_REQUIRED;
};

inline bool operator==(const LifecycleResult& result, MisterResult expected) {
	return result.call_result == expected;
}
inline bool operator!=(const LifecycleResult& result, MisterResult expected) { return !(result == expected); }
inline bool operator==(MisterResult expected, const LifecycleResult& result) { return result == expected; }
inline bool operator!=(MisterResult expected, const LifecycleResult& result) { return !(result == expected); }

// A durable owner is not evidence of a live ABI handle.  The lifecycle port
// returns both the current handle state and the owner embedded in that handle.
struct LiveHandleState {
	enum State { none, live, exit_required } state;
	LifecycleOwner owner;
	LiveHandleState(State value = none) : state(value) {}
	LiveHandleState(State value, const LifecycleOwner& value_owner)
		: state(value), owner(value_owner) {}
};
inline bool operator==(const LiveHandleState& value, LiveHandleState::State state) { return value.state == state; }
inline bool operator!=(const LiveHandleState& value, LiveHandleState::State state) { return !(value == state); }
inline bool operator==(LiveHandleState::State state, const LiveHandleState& value) { return value == state; }
inline bool operator!=(LiveHandleState::State state, const LiveHandleState& value) { return !(value == state); }

class MonotonicClock {
public:
	virtual ~MonotonicClock() {}
	virtual uint64_t NowMs() = 0;
};

// The coordinator does not know about Linux paths, handles, or ABI layout.
// This narrow port is implemented by the runtime/ABI adapter and is also the
// deterministic fake seam used by the coordinator tests.
class LifecyclePlatform {
public:
	virtual ~LifecyclePlatform() {}
	virtual LifecycleResult ProveNeutral(uint32_t resource_mask, uint32_t deadline_ms) = 0;
	// The coordinator calls this only after its durable transferred checkpoint.
	virtual LifecycleResult GrantTransfer(const LifecycleOwner& owner) = 0;
	virtual LifecycleResult Create(const LifecycleOwner& owner) = 0;
	virtual LifecycleResult Start(uint32_t deadline_ms) = 0;
	virtual LifecycleResult Load(const LaunchMetadata& launch, uint32_t deadline_ms) = 0;
	virtual LifecycleResult Observe(const std::string& expected_core, uint32_t deadline_ms) = 0;
	virtual LifecycleResult Stop(uint32_t deadline_ms) = 0;
	virtual LifecycleResult ObserveNeutral(uint32_t resource_mask, uint32_t deadline_ms) = 0;
	virtual LifecycleResult Destroy() = 0;
	virtual LifecycleResult RecoverStateless(uint32_t resource_mask, uint32_t deadline_ms) = 0;
	virtual LiveHandleState live_handle_state() const = 0;
	// Epoch adoption is safe only when the compatibility owner is independently
	// known absent.  The ABI does not expose process ownership, so production
	// composition must provide this observation through the platform boundary.
	virtual bool MainAbsent() = 0;
};

// The ABI-v2 adapter is the only production-facing lifecycle seam.  It keeps
// the coordinator portable while making deadlines and postconditions explicit.
class AbiV2LifecyclePlatform : public LifecyclePlatform {
public:
	explicit AbiV2LifecyclePlatform(const MisterPlatformV2* platform);
	LifecycleResult ProveNeutral(uint32_t resource_mask, uint32_t deadline_ms) override;
	LifecycleResult GrantTransfer(const LifecycleOwner& owner) override;
	LifecycleResult Create(const LifecycleOwner& owner) override;
	LifecycleResult Start(uint32_t deadline_ms) override;
	LifecycleResult Load(const LaunchMetadata& launch, uint32_t deadline_ms) override;
	LifecycleResult Observe(const std::string& expected_core, uint32_t deadline_ms) override;
	LifecycleResult Stop(uint32_t deadline_ms) override;
	LifecycleResult ObserveNeutral(uint32_t resource_mask, uint32_t deadline_ms) override;
	LifecycleResult Destroy() override;
	LifecycleResult RecoverStateless(uint32_t resource_mask, uint32_t deadline_ms) override;
	LiveHandleState live_handle_state() const override;
	bool MainAbsent() override;

private:
	LifecycleResult Recover(uint32_t resource_mask, uint32_t deadline_ms);
	LifecycleResult FinishRuntimeCall(MisterResult call, bool primary_call, bool cleanup_call);
	bool SynchronizeRuntimeState();
	const MisterPlatformV2* platform_;
	MisterRuntime* runtime_;
	LifecycleOwner live_owner_;
	LifecycleOwner granted_owner_;
	LiveHandleState::State live_state_;
	MisterStateV2 runtime_state_;
	MisterResult primary_result_;
	MisterResult cleanup_result_;
};

// The coordinator asks this seam only after a durable state-store commit.
// Production passes null; deterministic tests use it to model process death
// without weakening the persisted transition ordering.
class CommitCrashInjector {
public:
	virtual ~CommitCrashInjector() {}
	virtual bool AfterCommit(const std::string& phase) = 0;
};

// These codes are the portable coordinator-to-transport contract.  They are
// deliberately independent of wire parsing and platform/ABI result enums.
enum class CoordinatorCode {
	none,
	invalid_request,
	busy,
	not_ready,
	stale_admission,
	stale_generation,
	in_progress,
	operation_replay,
	operation_unknown,
	interrupted,
	deadline,
	recovery_required,
	unsupported_mode,
	owner_not_found,
	internal,
};

// This is the exact canonical wire Snapshot object, never a durable state
// record.  A false value means no checksum-valid snapshot is available.
struct CoordinatorSnapshot {
	bool valid;
	std::string canonical;
	CoordinatorSnapshot() : valid(false) {}
};

struct MutationReply {
	enum class Kind { terminal, in_progress, rejected } kind;
	CoordinatorCode code;
	std::string operation_id;
	std::string request_digest;
	bool ok;
	CoordinatorSnapshot snapshot;
	uint64_t resulting_sequence;
	MutationReply()
		: kind(Kind::rejected), code(CoordinatorCode::internal), ok(false),
		  resulting_sequence(0) {}
};

class Coordinator {
public:
	struct OperationStatus {
		enum Kind { unknown, in_flight, completed } kind;
		CoordinatorCode code;
		bool ok;
		std::string request_digest;
		std::string error;
		std::string snapshot;
		uint64_t resulting_sequence;
		OperationStatus() : kind(unknown), code(CoordinatorCode::operation_unknown),
			ok(false), resulting_sequence(0) {}
	};
	typedef LifecycleOwner Owner;
	struct Terminal {
		std::string operation_id;
		std::string digest;
		bool ok;
		std::string error;
		std::string snapshot;
		uint64_t sequence;
	};
	struct Admission {
		enum Kind { newly_recorded, terminal_replay, in_progress_replay, rejected } kind;
		CoordinatorCode code;
		Admission() : kind(rejected), code(CoordinatorCode::internal) {}
	};
	Coordinator();
	Coordinator(const std::string& state_directory, const BackendFence* fence,
		LifecyclePlatform* platform = 0, CommitCrashInjector* crash_injector = 0,
		MonotonicClock* clock = 0);
	bool ready() const;
	ErrorClass Initialize(uint64_t backend_epoch);
	ErrorClass Admit(const std::string& request);
	MutationReply ExecuteMutation(const std::string& request);
	ErrorClass Execute(const std::string& request);
	CoordinatorSnapshot snapshot() const;
	static const char* ProtocolCode(CoordinatorCode code);
	OperationStatus operation_status(const std::string& operation_id) const;
	uint64_t sequence() const;
	const std::string& canonical_state() const;
	const std::string& phase() const;

private:
	ErrorClass Commit(const std::string& phase, const Owner& owner,
		const Owner& release_owner, const Owner& candidate,
		const std::string& expected_core, const std::string& content_lease,
		const std::string& in_flight, const std::string& last_error,
		const Terminal* terminal);
	ErrorClass Release(const Owner& old_owner, const Owner& candidate,
		const std::string& operation_id, const std::string& digest,
		const LaunchMetadata& launch, const std::string& content_lease);
	ErrorClass Unwind(const Owner& candidate, const std::string& operation_id,
		const std::string& digest, const std::string& primary_error);
	bool FenceAllows(const std::string& operation) const;
	bool PhaseAllows(const std::string& operation) const;
	ErrorClass Restore(uint64_t backend_epoch, bool already_neutral = false);
	ErrorClass AdoptEpoch(uint64_t backend_epoch);
	bool ParseMutation(const std::string& request, std::string* operation,
		std::string* operation_id, std::string* digest, Owner* candidate,
		Owner* requested_owner, uint64_t* expected_sequence,
		LaunchMetadata* launch, std::string* content_lease) const;
	Admission AdmitParsed(const std::string& operation, const std::string& operation_id,
		const std::string& digest, const Owner& candidate, const Owner& requested_owner,
		uint64_t expected_sequence, const std::string& content_lease);
	ErrorClass ExecuteAdmitted(const std::string& operation, const std::string& operation_id,
		const std::string& digest, const Owner& candidate, const Owner& requested_owner,
		const LaunchMetadata& launch, const std::string& content_lease);
	uint32_t Remaining(uint64_t deadline_at) const;
	LifecycleResult NeutralLive(uint32_t resource_mask, uint64_t deadline_at,
		uint64_t cleanup_started);
	LifecycleResult NeutralStateless(uint32_t resource_mask, uint64_t deadline_at);
	LifecycleResult CleanupLive(const Owner& owner, uint32_t resource_mask, uint64_t deadline_at);
	LifecycleResult CleanupStateless(uint32_t resource_mask, uint64_t deadline_at);
	StateStore store_;
	const BackendFence* fence_;
	LifecyclePlatform* platform_;
	CommitCrashInjector* crash_injector_;
	MonotonicClock* clock_;
	StateRecord state_;
	Owner owner_;
	Owner release_owner_;
	Owner candidate_;
	std::string expected_core_;
	std::string content_lease_;
	std::string in_flight_id_;
	std::string in_flight_digest_;
	std::string in_flight_stage_;
	std::vector<Terminal> ledger_;
	uint64_t operation_deadline_;
	bool ready_;
	bool shutdown_;
};

}  // namespace fogcast

#endif
