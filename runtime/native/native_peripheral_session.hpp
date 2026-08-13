// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_PERIPHERAL_SESSION_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_PERIPHERAL_SESSION_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_core_profile.hpp"

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <utility>

namespace mister {
namespace native {

class HardwareBroker;
class HardwareLeaseView;
struct OperationRegistration;
class PeripheralAuthorityTestPeer;

namespace linux_native {
class NativeAudioAdapter;
class NativeVideoAdapter;
class NativeAvIoAdapter;
}

enum class PeripheralBrokerDisposition : uint8_t {
	no_session,
	live,
	abandoned,
	success_completed,
	failure_completed
};

enum class PeripheralSessionPhase : uint8_t {
	live,
	abandoned,
	finalized
};

enum class PeripheralSessionKind : uint8_t {
	audio,
	video,
	audio_video
};

// The broker owns this action ledger for the exact registration.  Its entries
// are intentionally recipe-level rather than raw-word authority: fixture I/O
// may resume only the acknowledged suffix of the one action it admitted.
enum class PeripheralSessionAction : uint8_t {
	none,
	audio_attenuation,
	video_activation,
	video_teardown,
	coupled_transmitter
};

// This identity is constructed only by the broker for an adapter instance.
// It binds a typed session to that instance so no other backend can adopt it.
class PeripheralBackendIdentity final {
public:
	PeripheralBackendIdentity(const PeripheralBackendIdentity &) = default;
	PeripheralBackendIdentity &operator=(const PeripheralBackendIdentity &) =
		default;
	bool Matches(const PeripheralBackendIdentity &other) const
	{
		return adapter_instance_ == other.adapter_instance_ &&
			construction_nonce_ == other.construction_nonce_;
	}

private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeVideoAdapter;
	PeripheralBackendIdentity(const void *adapter_instance, uint64_t nonce)
		: adapter_instance_(adapter_instance), construction_nonce_(nonce) {}
	const void *adapter_instance_;
	uint64_t construction_nonce_;
};

struct PeripheralCompletionReceipt {
	MisterResult result;
	bool transaction_closed;
	bool mapping_absent;
	bool descriptor_absent;
	bool local_resources_absent;
	bool closure_unknown;
	uint64_t mutation_sequence;
	// The final accepted acknowledgement is residue, never a neutrality proof.
	uint16_t transaction_residue;
};

struct PeripheralFailureReceipt {
	MisterResult primary_result;
	PeripheralCompletionReceipt release;
};

struct CoupledAcquisitionReceipt {
	MisterResult result;
	uint32_t affected_flags;
	bool transaction_closed;
	bool local_resources_absent;
	bool closure_unknown;
	uint64_t mutation_sequence;
	uint16_t transaction_residue;
};

struct CoupledFailureReceipt {
	MisterResult primary_result;
	CoupledAcquisitionReceipt release;
};

struct CoupledCompletionReceipt {
	MisterResult result;
	uint32_t affected_flags;
	uint32_t observed_flags;
	uint32_t neutral_flags;
	bool local_resources_absent;
	bool closure_unknown;
	uint64_t mutation_sequence;
	bool transaction_closed;
	uint16_t transaction_residue;
};

typedef CoupledCompletionReceipt CoupledRecoveryReceipt;

struct PeripheralSessionState {
	PeripheralSessionState(PeripheralSessionKind session_kind,
		const PeripheralBackendIdentity &backend_identity);
	~PeripheralSessionState();

	std::unique_ptr<HardwareLeaseView> view;
	std::weak_ptr<OperationRegistration> owner_registration;
	const NativeCoreProfile *profile;
	const SafePeripheralRecoveryRecord *recovery_record;
	PeripheralBackendIdentity backend;
	PeripheralSessionKind kind;
	uint64_t initial_mutation_sequence;
	uint64_t absolute_deadline_ms;
	const void *action_profile_identity;
	PeripheralSessionAction action;
	uint8_t action_word_count;
	uint8_t action_next_word_index;
	bool action_transaction_closed;
	uint64_t last_mutation_sequence;
	// If an accepted acknowledgement or close could not be committed to this
	// registration-owned ledger, never hand the state back for a raw retry.
	bool action_progress_unknown;
	// A closure-unknown abandonment is containment-only: the broker must not
	// hand that exact registration back to raw A/V I/O for another attempt.
	bool recheckout_allowed;
	std::atomic<PeripheralSessionPhase> phase;
};

// Dropping the sole typed wrapper never manufactures a successful receipt or
// performs I/O.  It only returns the exact registration-owned state to the
// broker's retryable abandoned phase.  Finalized states stay finalized.
inline void MarkPeripheralSessionAbandoned(
	const std::shared_ptr<PeripheralSessionState> &state)
{
	if (!state) return;
	PeripheralSessionPhase expected = PeripheralSessionPhase::live;
	state->phase.compare_exchange_strong(expected,
		PeripheralSessionPhase::abandoned);
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
// Host-only deterministic wrapper-allocation seam.  Production builds neither
// expose nor execute this control.
void SetPeripheralSessionAllocationFailureForTest(size_t allocation_index);
void ClearPeripheralSessionAllocationFailureForTest();
bool PeripheralSessionAllocationAllowedForTest();
#endif

class ActiveAudioSession final {
public:
	~ActiveAudioSession();
	ActiveAudioSession(const ActiveAudioSession &) = delete;
	ActiveAudioSession &operator=(const ActiveAudioSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeAvIoAdapter;
	ActiveAudioSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class CleanupAudioSession final {
public:
	~CleanupAudioSession();
	CleanupAudioSession(const CleanupAudioSession &) = delete;
	CleanupAudioSession &operator=(const CleanupAudioSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeAvIoAdapter;
	CleanupAudioSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class RecoveryAudioSession final {
public:
	~RecoveryAudioSession();
	RecoveryAudioSession(const RecoveryAudioSession &) = delete;
	RecoveryAudioSession &operator=(const RecoveryAudioSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeAvIoAdapter;
	RecoveryAudioSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class ActiveVideoSession final {
public:
	~ActiveVideoSession();
	ActiveVideoSession(const ActiveVideoSession &) = delete;
	ActiveVideoSession &operator=(const ActiveVideoSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeVideoAdapter;
	friend class linux_native::NativeAvIoAdapter;
	ActiveVideoSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class CleanupVideoSession final {
public:
	~CleanupVideoSession();
	CleanupVideoSession(const CleanupVideoSession &) = delete;
	CleanupVideoSession &operator=(const CleanupVideoSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeVideoAdapter;
	friend class linux_native::NativeAvIoAdapter;
	CleanupVideoSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class RecoveryVideoSession final {
public:
	~RecoveryVideoSession();
	RecoveryVideoSession(const RecoveryVideoSession &) = delete;
	RecoveryVideoSession &operator=(const RecoveryVideoSession &) = delete;
private:
	friend class HardwareBroker;
	friend class linux_native::NativeVideoAdapter;
	friend class linux_native::NativeAvIoAdapter;
	RecoveryVideoSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class ActiveAudioVideoSession final {
public:
	~ActiveAudioVideoSession();
	ActiveAudioVideoSession(const ActiveAudioVideoSession &) = delete;
	ActiveAudioVideoSession &operator=(const ActiveAudioVideoSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAvIoAdapter;
	friend class linux_native::NativeVideoAdapter;
	ActiveAudioVideoSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class CleanupAudioVideoSession final {
public:
	~CleanupAudioVideoSession();
	CleanupAudioVideoSession(const CleanupAudioVideoSession &) = delete;
	CleanupAudioVideoSession &operator=(const CleanupAudioVideoSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAvIoAdapter;
	friend class linux_native::NativeVideoAdapter;
	CleanupAudioVideoSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

class RecoveryAudioVideoSession final {
public:
	~RecoveryAudioVideoSession();
	RecoveryAudioVideoSession(const RecoveryAudioVideoSession &) = delete;
	RecoveryAudioVideoSession &operator=(const RecoveryAudioVideoSession &) = delete;
private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAvIoAdapter;
	friend class linux_native::NativeVideoAdapter;
	RecoveryAudioVideoSession();
	std::shared_ptr<PeripheralSessionState> state_;
};

// A typed wrapper is meaningful only to the backend identity for which the
// broker minted it.  This private, move-only bundle prevents lifecycle or a
// test double from separating those capabilities and handing the session to a
// same-kind foreign consumer.
template <typename Session>
class PeripheralSessionBundle final {
public:
	~PeripheralSessionBundle() = default;
	PeripheralSessionBundle(const PeripheralSessionBundle &) = delete;
	PeripheralSessionBundle &operator=(const PeripheralSessionBundle &) = delete;
	PeripheralSessionBundle(PeripheralSessionBundle &&) = delete;
	PeripheralSessionBundle &operator=(PeripheralSessionBundle &&) = delete;

private:
	friend class HardwareBroker;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeVideoAdapter;
	PeripheralSessionBundle(std::unique_ptr<Session> &&session,
		const PeripheralBackendIdentity &backend)
		: session_(std::move(session)), backend_(backend) {}

	std::unique_ptr<Session> session_;
	PeripheralBackendIdentity backend_;
};

using ActiveAudioSessionBundle = PeripheralSessionBundle<ActiveAudioSession>;
using CleanupAudioSessionBundle = PeripheralSessionBundle<CleanupAudioSession>;
using RecoveryAudioSessionBundle = PeripheralSessionBundle<RecoveryAudioSession>;
using ActiveVideoSessionBundle = PeripheralSessionBundle<ActiveVideoSession>;
using CleanupVideoSessionBundle = PeripheralSessionBundle<CleanupVideoSession>;
using RecoveryVideoSessionBundle = PeripheralSessionBundle<RecoveryVideoSession>;
using ActiveAudioVideoSessionBundle =
	PeripheralSessionBundle<ActiveAudioVideoSession>;
using CleanupAudioVideoSessionBundle =
	PeripheralSessionBundle<CleanupAudioVideoSession>;
using RecoveryAudioVideoSessionBundle =
	PeripheralSessionBundle<RecoveryAudioVideoSession>;

} // namespace native
} // namespace mister

#endif
