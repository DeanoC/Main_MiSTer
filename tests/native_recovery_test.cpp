// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_recovery.hpp"
#include "runtime/native/linux/native_save_adapter.hpp"
#include "tests/native_core_protocol_authority_test_peer.hpp"
#include "tests/native_peripheral_authority_test_peer.hpp"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace mister {
namespace native {
namespace {

static_assert(std::is_trivially_copyable<NativeRetainedOperationSnapshot>::value,
	"retained operation snapshots must remain observational values");

void AssertZeroSnapshot(const NativeRetainedOperationSnapshot &snapshot)
{
	const NativeRetainedOperationSnapshot zero = {};
	assert(memcmp(&snapshot, &zero, sizeof(snapshot)) == 0);
}

void AssertSameSnapshot(const NativeRetainedOperationSnapshot &left,
	const NativeRetainedOperationSnapshot &right)
{
	assert(memcmp(&left, &right, sizeof(left)) == 0);
}

void AssertOnlyRecoveryResultChanged(
	const NativeRetainedOperationSnapshot &before,
	const NativeRetainedOperationSnapshot &after, Result expected_result)
{
	assert(after.recovery_result == expected_result);
	NativeRetainedOperationSnapshot normalized_before = before;
	NativeRetainedOperationSnapshot normalized_after = after;
	normalized_before.recovery_result = MISTER_RESULT_OK;
	normalized_after.recovery_result = MISTER_RESULT_OK;
	AssertSameSnapshot(normalized_before, normalized_after);
}

void AssertExactIdleSnapshot(const NativeRetainedOperationSnapshot &snapshot)
{
	NativeRetainedOperationSnapshot expected = {};
	expected.query_valid = true;
	expected.broker_idle = true;
	AssertSameSnapshot(snapshot, expected);
}

void NormalizeRetainedRebindFields(NativeRetainedOperationSnapshot *snapshot)
{
	snapshot->registration_effective_deadline_ms = 0;
	snapshot->invocation_identity = 0;
	snapshot->invocation_callback_deadline_ms = 0;
	snapshot->peripheral_phase = PeripheralSessionPhase::live;
	snapshot->protocol_phase = ProtocolSessionHandleState::live;
	snapshot->core_disposition = CoreProtocolBrokerDisposition::no_session;
	snapshot->peripheral_disposition = PeripheralBrokerDisposition::no_session;
	snapshot->invocation_registered = false;
	snapshot->invocation_outcome_missing = false;
	snapshot->registration_is_invoked = false;
	snapshot->registration_is_suspended = false;
	snapshot->registration_outcome_recorded = false;
	snapshot->process_guard_active = false;
}

// Compares every byte after erasing only the fields which the broker contract
// permits a fresh invocation bind/session checkout to change.  Action suffix,
// resource partition, all identities, deadlines, and mutation sequences remain
// part of the byte-for-byte comparison.
void AssertOnlyRetainedRebindFieldsChanged(
	const NativeRetainedOperationSnapshot &before,
	const NativeRetainedOperationSnapshot &after)
{
	NativeRetainedOperationSnapshot normalized_before = before;
	NativeRetainedOperationSnapshot normalized_after = after;
	NormalizeRetainedRebindFields(&normalized_before);
	NormalizeRetainedRebindFields(&normalized_after);
	AssertSameSnapshot(normalized_before, normalized_after);
}

// FinishInvocation may only suspend the already-recorded registration.  This
// whole-struct comparator makes that allowed-delta list explicit.
void AssertOnlyInvocationSuspensionChanged(
	const NativeRetainedOperationSnapshot &invoked,
	const NativeRetainedOperationSnapshot &suspended)
{
	assert(invoked.registration_effective_deadline_ms != 0);
	assert(invoked.invocation_identity != 0);
	assert(invoked.invocation_callback_deadline_ms != 0);
	assert(invoked.invocation_registered && invoked.registration_is_invoked &&
		!invoked.registration_is_suspended);
	assert(suspended.registration_effective_deadline_ms == 0);
	assert(suspended.invocation_identity == 0);
	assert(suspended.invocation_callback_deadline_ms == 0);
	assert(!suspended.invocation_registered && !suspended.registration_is_invoked &&
		suspended.registration_is_suspended);
	NativeRetainedOperationSnapshot normalized_invoked = invoked;
	NativeRetainedOperationSnapshot normalized_suspended = suspended;
	normalized_invoked.registration_effective_deadline_ms = 0;
	normalized_suspended.registration_effective_deadline_ms = 0;
	normalized_invoked.invocation_identity = 0;
	normalized_suspended.invocation_identity = 0;
	normalized_invoked.invocation_callback_deadline_ms = 0;
	normalized_suspended.invocation_callback_deadline_ms = 0;
	normalized_invoked.invocation_registered = false;
	normalized_suspended.invocation_registered = false;
	normalized_invoked.registration_is_invoked = false;
	normalized_suspended.registration_is_invoked = false;
	normalized_invoked.registration_is_suspended = false;
	normalized_suspended.registration_is_suspended = false;
	AssertSameSnapshot(normalized_invoked, normalized_suspended);
}

struct PeripheralResourceEvidence {
	size_t callback_count;
	size_t mutation_count;
	size_t final_ack_count;
	uint16_t final_ack;
	uint64_t accepted_mutation_sequence;
	bool transaction_closed;
	bool local_resources_absent;
	bool closure_unknown;
};

struct SaveResourceEvidence {
	size_t callback_count;
	size_t fdatasync_attempts;
	size_t file_fsync_successes;
	size_t directory_fsync_successes;
	size_t descriptor_close_successes;
	size_t open_descriptor_count;
	bool file_synced;
	bool directory_synced;
	bool descriptors_closed;
	bool closure_unknown;
};

struct DeadlineResourceEvidence {
	size_t callback_count;
	size_t mutation_count;
	size_t final_ack_count;
	uint16_t final_ack;
	uint64_t accepted_mutation_sequence;
	bool transaction_closed;
	bool local_resources_absent;
	bool closure_unknown;
	size_t core_mapping_count;
	size_t core_descriptor_count;
	size_t core_release_attempts;
	size_t core_release_successes;
	bool core_selected_transaction_closed;
};

void AssertSameDeadlineResourceEvidence(const DeadlineResourceEvidence &left,
	const DeadlineResourceEvidence &right)
{
	assert(left.callback_count == right.callback_count);
	assert(left.mutation_count == right.mutation_count);
	assert(left.final_ack_count == right.final_ack_count);
	assert(left.final_ack == right.final_ack);
	assert(left.accepted_mutation_sequence == right.accepted_mutation_sequence);
	assert(left.transaction_closed == right.transaction_closed);
	assert(left.local_resources_absent == right.local_resources_absent);
	assert(left.closure_unknown == right.closure_unknown);
	assert(left.core_mapping_count == right.core_mapping_count);
	assert(left.core_descriptor_count == right.core_descriptor_count);
	assert(left.core_release_attempts == right.core_release_attempts);
	assert(left.core_release_successes == right.core_release_successes);
	assert(left.core_selected_transaction_closed ==
		right.core_selected_transaction_closed);
}

void AssertSamePeripheralEvidence(const PeripheralResourceEvidence &left,
	const PeripheralResourceEvidence &right)
{
	assert(left.callback_count == right.callback_count);
	assert(left.mutation_count == right.mutation_count);
	assert(left.final_ack_count == right.final_ack_count);
	assert(left.final_ack == right.final_ack);
	assert(left.accepted_mutation_sequence == right.accepted_mutation_sequence);
	assert(left.transaction_closed == right.transaction_closed);
	assert(left.local_resources_absent == right.local_resources_absent);
	assert(left.closure_unknown == right.closure_unknown);
}

void AssertPeripheralSnapshotPrefixMatchesEvidence(
	const NativeRetainedOperationSnapshot &snapshot,
	const PeripheralResourceEvidence &evidence)
{
	assert(snapshot.action_applicable);
	assert(snapshot.peripheral_action == PeripheralSessionAction::none);
	assert(snapshot.action_word_count == 0);
	assert(snapshot.action_next_word_index == 0);
	assert(snapshot.action_transaction_closed);
	assert(!snapshot.action_progress_unknown);
	assert(snapshot.recheckout_allowed);
	assert(snapshot.session_initial_mutation_sequence == 0);
	assert(snapshot.session_last_mutation_sequence ==
		evidence.accepted_mutation_sequence);
	assert(snapshot.broker_mutation_sequence ==
		evidence.accepted_mutation_sequence);
}

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t) override { return false; }
	void SetNow(uint64_t now_ms) { now_ms_ = now_ms; }

private:
	uint64_t now_ms_;
};

size_t KindIndex(OperationKind kind)
{
	return static_cast<size_t>(kind);
}

void AssertSameOwnerDestructionSnapshot(
	const HardwareBroker::OwnerDestructionSnapshot &left,
	const HardwareBroker::OwnerDestructionSnapshot &right)
{
	assert(left.generation == right.generation);
	assert(left.cleanup_identity == right.cleanup_identity);
	assert(left.recovery_identity == right.recovery_identity);
	assert(left.registration_identity == right.registration_identity);
	assert(left.session_identity == right.session_identity);
	assert(left.session_initial_mutation_sequence == right.session_initial_mutation_sequence);
	assert(left.session_last_mutation_sequence == right.session_last_mutation_sequence);
	assert(left.effective_deadline_ms == right.effective_deadline_ms);
	assert(left.active_lease_count == right.active_lease_count);
	assert(left.terminal_lease_count == right.terminal_lease_count);
	assert(left.operation_kind == right.operation_kind);
	assert(left.peripheral_kind == right.peripheral_kind);
	assert(left.peripheral_phase == right.peripheral_phase);
	assert(left.peripheral_action == right.peripheral_action);
	assert(left.core_handle_state == right.core_handle_state);
	assert(left.invocation_registered == right.invocation_registered);
	assert(left.suspended_registration_present == right.suspended_registration_present);
	assert(left.core_session_present == right.core_session_present);
	assert(left.peripheral_session_present == right.peripheral_session_present);
	assert(left.session_view_names_registration == right.session_view_names_registration);
	assert(left.session_progress_unknown == right.session_progress_unknown);
	assert(left.session_recheckout_allowed == right.session_recheckout_allowed);
	assert(left.hardware_transaction_active == right.hardware_transaction_active);
	assert(left.mutation_sequence == right.mutation_sequence);
	assert(left.destruction_failure_count == right.destruction_failure_count);
}

class FakeRecoveryIo final : public NativeRecoveryIo {
public:
	explicit FakeRecoveryIo(HardwareBroker &broker)
		: broker_(broker), calls(0), core_protocol_session_calls(0),
		  last_deadline(0), save_record(FixtureSafeSaveRecoveryRecordForTest(
			NativeSystem::snes))
	{
		for (size_t index = 0; index != 11; ++index) {
			states[index] = RecoveryResourceState::unknown;
			results[index] = MISTER_RESULT_OK;
		}
	}

	Result Apply(const OperationLease &lease, OperationKind kind,
		RecoveryResourceState *state)
	{
		return ApplyDeadline(lease.absolute_deadline_ms(), kind, state);
	}
	Result ApplyDeadline(uint64_t absolute_deadline_ms, OperationKind kind,
		RecoveryResourceState *state)
	{
		++calls;
		last_deadline = absolute_deadline_ms;
		const size_t index = KindIndex(kind);
		*state = states[index];
		return results[index];
	}
	Result CloseInputDescriptors(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::input_descriptors, state);
	}
	Result MuteAudio(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::audio, state);
	}
	Result PowerDownVideo(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::video, state);
	}
	Result CloseContent(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::content, state);
	}
	Result DisableCoreProtocol(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		std::unique_ptr<RecoveryCoreProtocolSession> session;
		const Result acquire = CoreProtocolAuthorityTestPeer::AcquireRecovery(
			lease, broker_, &session);
		if (acquire != MISTER_RESULT_OK) return acquire;
		if (core_mapping_count == 0) core_mapping_count = 1;
		if (core_descriptor_count == 0) core_descriptor_count = 2;
		core_selected_transaction_closed = false;
		if (snapshot_recovery != nullptr && snapshot_epoch != nullptr) {
			assert(snapshot_recovery->retained_snapshot_for_test(*snapshot_epoch,
				OperationKind::core_protocol, &live_snapshot) == MISTER_RESULT_OK);
			live_snapshot_captured = true;
		}
		++core_protocol_session_calls;
		if (deadline_core_once && deadline_clock != nullptr) {
			deadline_core_once = false;
			last_deadline = lease.absolute_deadline_ms();
			*state = RecoveryResourceState::unknown;
			deadline_clock->SetNow(last_deadline);
			return MISTER_RESULT_DEADLINE;
		}
		const Result primary = ApplyDeadline(lease.absolute_deadline_ms(),
			OperationKind::core_protocol, state);
		if (abandon_core_protocol_release_once) {
			abandon_core_protocol_release_once = false;
			++core_release_attempts;
			return MISTER_RESULT_PLATFORM;
		}
		++core_release_attempts;
		const ProtocolMappingReleaseReceipt release = {
			MISTER_RESULT_OK, true, true, true, true, true,
			broker_.mutation_sequence_for_test()};
		const Result completed = CoreProtocolAuthorityTestPeer::CompleteRecovery(
			lease, broker_, std::move(session), release);
		if (completed == MISTER_RESULT_OK) {
			core_mapping_count = 0;
			core_descriptor_count = 0;
			++core_release_successes;
			core_selected_transaction_closed = true;
		}
		return completed == MISTER_RESULT_OK ? primary : completed;
	}
	const SafeAudioRecoveryRecord *SafeAudioRecord() const override
	{
		return FixtureSafeAudioRecoveryRecordForTest(NativeSystem::snes);
	}
	const SafeVideoRecoveryRecord *SafeVideoRecord() const override
	{
		return FixtureSafeVideoRecoveryRecordForTest(NativeSystem::snes);
	}
	const SafeAudioVideoRecoveryRecord *SafeAudioVideoRecord() const override
	{
		return FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::snes);
	}
	const SafeSaveRecoveryRecord *SafeSaveRecord() const override
	{
		return save_record;
	}

	HardwareBroker &broker_;
	RecoveryResourceState states[11];
	Result results[11];
	int calls;
	int core_protocol_session_calls;
	uint64_t last_deadline;
	bool abandon_core_protocol_release_once = false;
	FakeClock *deadline_clock = nullptr;
	bool deadline_core_once = false;
	size_t core_mapping_count = 0;
	size_t core_descriptor_count = 0;
	size_t core_release_attempts = 0;
	size_t core_release_successes = 0;
	bool core_selected_transaction_closed = true;
	const SafeSaveRecoveryRecord *save_record;
	NativeRecovery *snapshot_recovery = nullptr;
	const RecoveryEpoch *snapshot_epoch = nullptr;
	NativeRetainedOperationSnapshot live_snapshot = {};
	bool live_snapshot_captured = false;
};

class FakeTypedRecoveryResources final : public NativeAudioResource,
	public NativeVideoResource, public NativeAudioVideoResource {
public:
	explicit FakeTypedRecoveryResources(HardwareBroker &broker)
		: broker_(broker), backend_(PeripheralAuthorityTestPeer::Backend(broker,
			this)), audio_calls(0), video_calls(0), coupled_calls(0), last_deadline(0),
		  abandon_once(false), closure_unknown_once(false), deadline_clock(nullptr),
		  deadline_once_kind(OperationKind::program_fpga), deadline_to_expire(1500),
		  coupled_all_neutral(false), evidence_() {}

	PeripheralBackendIdentity BackendIdentity() const override { return backend_; }
	NativePeripheralAcquisitionOutcome StartAudio(
		std::unique_ptr<ActiveAudioSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false}; }
	NativePeripheralReleaseOutcome StopAudio(
		std::unique_ptr<CleanupAudioSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false, false, false, false}; }
	NativePeripheralReleaseOutcome RecoverAudio(
		std::unique_ptr<RecoveryAudioSessionBundle> &&session) override
	{
		++audio_calls;
		PeripheralResourceEvidence &evidence =
			evidence_[KindIndex(OperationKind::audio)];
		++evidence.callback_count;
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false, false,
			false, false};
		last_deadline = PeripheralAuthorityTestPeer::Deadline(*session);
		CaptureLive(OperationKind::audio);
		if (deadline_once_kind == OperationKind::audio && deadline_clock != nullptr) {
			deadline_once_kind = OperationKind::program_fpga;
			deadline_clock->SetNow(last_deadline);
			evidence.transaction_closed = true;
			evidence.local_resources_absent = true;
			session.reset();
			return {MISTER_RESULT_DEADLINE, false, false, false, false};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK)
			return {recorded, false, false, false, false};
		++evidence.mutation_count;
		evidence.accepted_mutation_sequence = sequence;
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, sequence, 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteAudio(
			broker_, std::move(session), receipt);
		if (completed == MISTER_RESULT_OK) {
			++evidence.final_ack_count;
			evidence.final_ack = 0xa55a;
			evidence.transaction_closed = true;
			evidence.local_resources_absent = true;
			evidence.closure_unknown = false;
		}
		return {completed, completed == MISTER_RESULT_OK, false,
			completed == MISTER_RESULT_OK, false};
	}
	void CloseAudioForProcessExit() override {}
	NativePeripheralAcquisitionOutcome StartVideo(
		std::unique_ptr<ActiveVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false}; }
	NativePeripheralReleaseOutcome StopVideo(
		std::unique_ptr<CleanupVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false, false, false, false}; }
	NativePeripheralReleaseOutcome RecoverVideo(
		std::unique_ptr<RecoveryVideoSessionBundle> &&session) override
	{
		++video_calls;
		PeripheralResourceEvidence &evidence =
			evidence_[KindIndex(OperationKind::video)];
		++evidence.callback_count;
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false, false,
			false, false};
		CaptureLive(OperationKind::video);
		if (deadline_once_kind == OperationKind::video && deadline_clock != nullptr) {
			last_deadline = deadline_to_expire;
			deadline_once_kind = OperationKind::program_fpga;
			deadline_clock->SetNow(deadline_to_expire);
			evidence.transaction_closed = true;
			evidence.local_resources_absent = true;
			session.reset();
			return {MISTER_RESULT_DEADLINE, false, false, false, false};
		}
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, broker_.mutation_sequence_for_test(), 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteVideo(
			broker_, std::move(session), receipt);
		if (completed == MISTER_RESULT_OK) {
			++evidence.final_ack_count;
			evidence.final_ack = 0xa55a;
			evidence.accepted_mutation_sequence = receipt.mutation_sequence;
			evidence.transaction_closed = true;
			evidence.local_resources_absent = true;
			evidence.closure_unknown = false;
		}
		return {completed, completed == MISTER_RESULT_OK,
			completed == MISTER_RESULT_OK,
			completed == MISTER_RESULT_OK, false};
	}
	void CloseVideoForProcessExit() override {}
	NativeCoupledAcquisitionOutcome StartAudioVideo(
		std::unique_ptr<ActiveAudioVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, 0, false}; }
	NativeCoupledReleaseOutcome StopAudioVideo(
		std::unique_ptr<CleanupAudioVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, 0, 0, 0, false, false, 0}; }
	NativeCoupledReleaseOutcome RecoverAudioVideo(
		std::unique_ptr<RecoveryAudioVideoSessionBundle> &&session) override
	{
		++coupled_calls;
		PeripheralResourceEvidence &evidence =
			evidence_[KindIndex(OperationKind::audio_video)];
		++evidence.callback_count;
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false,
			false, 0};
		last_deadline = PeripheralAuthorityTestPeer::Deadline(*session);
		CaptureLive(OperationKind::audio_video);
		const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		if (deadline_once_kind == OperationKind::audio_video &&
			deadline_clock != nullptr) {
			deadline_once_kind = OperationKind::program_fpga;
			deadline_clock->SetNow(last_deadline);
			const CoupledFailureReceipt failure = {MISTER_RESULT_DEADLINE,
				{MISTER_RESULT_DEADLINE, affected, false, false, false,
					broker_.mutation_sequence_for_test(), 0xa55a}};
			const Result abandoned = PeripheralAuthorityTestPeer::AbandonAudioVideo(
				broker_, std::move(session), failure);
			evidence.final_ack = 0xa55a;
			evidence.transaction_closed = false;
			evidence.local_resources_absent = false;
			evidence.closure_unknown = false;
			return {abandoned == MISTER_RESULT_OK ? MISTER_RESULT_DEADLINE :
				abandoned, affected, 0, 0, false, false,
				broker_.mutation_sequence_for_test()};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK)
			return {recorded, 0, 0, 0, false, false, 0};
		++evidence.mutation_count;
		evidence.accepted_mutation_sequence = sequence;
		if (abandon_once) {
			abandon_once = false;
			const bool closure_unknown = closure_unknown_once;
			closure_unknown_once = false;
			const CoupledFailureReceipt failure = {MISTER_RESULT_PLATFORM,
				{MISTER_RESULT_PLATFORM, affected, false, false, closure_unknown,
					sequence, 0xa55a}};
			const Result abandoned = PeripheralAuthorityTestPeer::AbandonAudioVideo(
				broker_, std::move(session), failure);
			evidence.final_ack = 0xa55a;
			evidence.transaction_closed = false;
			evidence.local_resources_absent = false;
			evidence.closure_unknown = closure_unknown;
			return {abandoned == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM :
				abandoned, affected, 0, 0, false, closure_unknown, sequence};
		}
		const uint32_t neutral = coupled_all_neutral ? affected :
			static_cast<uint32_t>(MISTER_RESOURCE_NATIVE_VIDEO);
		const CoupledRecoveryReceipt receipt = {MISTER_RESULT_OK, affected, 0,
			neutral, true, false, sequence, true, 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteAudioVideo(
			broker_, std::move(session), receipt);
		if (completed == MISTER_RESULT_OK) {
			++evidence.final_ack_count;
			evidence.final_ack = 0xa55a;
			evidence.transaction_closed = true;
			evidence.local_resources_absent = true;
			evidence.closure_unknown = false;
		}
		return {completed, affected, 0, neutral, true,
			false, sequence};
	}
	void CloseAudioVideoForProcessExit() override {}
	void CaptureLive(OperationKind kind)
	{
		if (snapshot_recovery == nullptr || snapshot_epoch == nullptr) return;
		assert(snapshot_recovery->retained_snapshot_for_test(*snapshot_epoch, kind,
			&live_snapshot) == MISTER_RESULT_OK);
		live_snapshot_captured = true;
	}
	PeripheralResourceEvidence Evidence(OperationKind kind) const
	{
		return evidence_[KindIndex(kind)];
	}

	HardwareBroker &broker_;
	PeripheralBackendIdentity backend_;
	int audio_calls;
	int video_calls;
	int coupled_calls;
	uint64_t last_deadline;
	bool abandon_once;
	bool closure_unknown_once;
	FakeClock *deadline_clock;
	OperationKind deadline_once_kind;
	uint64_t deadline_to_expire;
	bool coupled_all_neutral;
	NativeRecovery *snapshot_recovery = nullptr;
	const RecoveryEpoch *snapshot_epoch = nullptr;
	NativeRetainedOperationSnapshot live_snapshot = {};
	bool live_snapshot_captured = false;
	PeripheralResourceEvidence evidence_[11];
};

struct RecoveryResourceTrace {
	std::array<RecoveryResourceState, 11> io_states;
	std::array<Result, 11> io_results;
	int io_calls;
	int core_protocol_session_calls;
	uint64_t io_last_deadline;
	bool abandon_core_protocol_release_once;
	FakeClock *io_deadline_clock;
	bool deadline_core_once;
	const SafeSaveRecoveryRecord *save_record;
	int audio_calls;
	int video_calls;
	int coupled_calls;
	uint64_t resource_last_deadline;
	bool abandon_once;
	bool closure_unknown_once;
	FakeClock *resource_deadline_clock;
	OperationKind deadline_once_kind;
	uint64_t deadline_to_expire;
	bool coupled_all_neutral;
};

RecoveryResourceTrace CaptureRecoveryResourceTrace(const FakeRecoveryIo &io,
	const FakeTypedRecoveryResources &resources)
{
	RecoveryResourceTrace trace = {};
	for (size_t index = 0; index != trace.io_states.size(); ++index) {
		trace.io_states[index] = io.states[index];
		trace.io_results[index] = io.results[index];
	}
	trace.io_calls = io.calls;
	trace.core_protocol_session_calls = io.core_protocol_session_calls;
	trace.io_last_deadline = io.last_deadline;
	trace.abandon_core_protocol_release_once =
		io.abandon_core_protocol_release_once;
	trace.io_deadline_clock = io.deadline_clock;
	trace.deadline_core_once = io.deadline_core_once;
	trace.save_record = io.save_record;
	trace.audio_calls = resources.audio_calls;
	trace.video_calls = resources.video_calls;
	trace.coupled_calls = resources.coupled_calls;
	trace.resource_last_deadline = resources.last_deadline;
	trace.abandon_once = resources.abandon_once;
	trace.closure_unknown_once = resources.closure_unknown_once;
	trace.resource_deadline_clock = resources.deadline_clock;
	trace.deadline_once_kind = resources.deadline_once_kind;
	trace.deadline_to_expire = resources.deadline_to_expire;
	trace.coupled_all_neutral = resources.coupled_all_neutral;
	return trace;
}

void AssertSameRecoveryResourceTrace(const RecoveryResourceTrace &left,
	const RecoveryResourceTrace &right)
{
	for (size_t index = 0; index != left.io_states.size(); ++index) {
		assert(left.io_states[index] == right.io_states[index]);
		assert(left.io_results[index] == right.io_results[index]);
	}
	assert(left.io_calls == right.io_calls);
	assert(left.core_protocol_session_calls ==
		right.core_protocol_session_calls);
	assert(left.io_last_deadline == right.io_last_deadline);
	assert(left.abandon_core_protocol_release_once ==
		right.abandon_core_protocol_release_once);
	assert(left.io_deadline_clock == right.io_deadline_clock);
	assert(left.deadline_core_once == right.deadline_core_once);
	assert(left.save_record == right.save_record);
	assert(left.audio_calls == right.audio_calls);
	assert(left.video_calls == right.video_calls);
	assert(left.coupled_calls == right.coupled_calls);
	assert(left.resource_last_deadline == right.resource_last_deadline);
	assert(left.abandon_once == right.abandon_once);
	assert(left.closure_unknown_once == right.closure_unknown_once);
	assert(left.resource_deadline_clock == right.resource_deadline_clock);
	assert(left.deadline_once_kind == right.deadline_once_kind);
	assert(left.deadline_to_expire == right.deadline_to_expire);
	assert(left.coupled_all_neutral == right.coupled_all_neutral);
}

class FakeTypedSaveRecoveryResource final : public NativeSaveResource {
public:
	NativeSaveOpenOutcome OpenSave(const OperationLease &, const NativeCoreProfile &,
		const NativeSaveKey &) override
	{
		return {MISTER_RESULT_UNSUPPORTED, false};
	}
	NativeSaveCloseOutcome FlushAndCloseSave(const OperationLease &) override
	{
		return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
	}
	NativeSaveCloseOutcome RecoverSave(const OperationLease &lease,
		const SafeSaveRecoveryRecord &) override
	{
		++calls;
		++evidence.callback_count;
		last_deadline = lease.absolute_deadline_ms();
		if (snapshot_recovery != nullptr && snapshot_epoch != nullptr) {
			assert(snapshot_recovery->retained_snapshot_for_test(*snapshot_epoch,
				OperationKind::save, &live_snapshot) == MISTER_RESULT_OK);
			live_snapshot_captured = true;
		}
		if (deadline_once && deadline_clock != nullptr) {
			deadline_once = false;
			deadline_clock->SetNow(last_deadline);
			return {MISTER_RESULT_DEADLINE, false, false, false, false};
		}
		if (fail_once) {
			fail_once = false;
			return {MISTER_RESULT_PLATFORM, false, false, false, false};
		}
		evidence.file_synced = true;
		evidence.directory_synced = true;
		evidence.descriptors_closed = true;
		evidence.closure_unknown = false;
		if (complete_at_deadline && deadline_clock != nullptr) {
			complete_at_deadline = false;
			deadline_clock->SetNow(last_deadline);
		}
		return {MISTER_RESULT_OK, true, true, true, false};
	}
	void CloseSaveForProcessExit() override {}

	int calls = 0;
	uint64_t last_deadline = 0;
	bool fail_once = false;
	FakeClock *deadline_clock = nullptr;
	bool deadline_once = false;
	bool complete_at_deadline = false;
	NativeRecovery *snapshot_recovery = nullptr;
	const RecoveryEpoch *snapshot_epoch = nullptr;
	NativeRetainedOperationSnapshot live_snapshot = {};
	bool live_snapshot_captured = false;
	SaveResourceEvidence evidence = {};
};

class RecoverySaveFileSystem final : public linux_native::NativeSaveFileSystem {
public:
	RecoverySaveFileSystem()
		: fdatasync_fail_once_(false), calls_(0), evidence_(), trace_(),
		  snapshot_recovery_(nullptr), snapshot_epoch_(nullptr),
		  callback_snapshots_()
	{
		Initialize(&root_, 10, 1, S_IFDIR | 0755, 0, 0);
		Initialize(&parent_, 11, 2, S_IFDIR | 0755, 0, 0);
		Initialize(&save_root_, 12, 3, S_IFDIR | 0700, 1000, 1000);
		Initialize(&system_, 13, 4, S_IFDIR | 0700, 1000, 1000);
		Initialize(&file_, 14, 5, S_IFREG | 0600, 1000, 1000);
	}

	uint64_t NowMs() const override { return 1000; }
	linux_native::NativeSaveOpenResult OpenAt(int parent, const char *name,
		int, mode_t) override
	{
		++calls_;
		int descriptor = -1;
		if (parent == AT_FDCWD && strcmp(name, "/") == 0) descriptor = 10;
		else if (parent == 10 && strcmp(name, "fogcast-fixture") == 0) descriptor = 11;
		else if (parent == 11 && strcmp(name, "saves") == 0) descriptor = 12;
		else if (parent == 12 && strcmp(name, "snes") == 0) descriptor = 13;
		else if (parent == 13 && strstr(name, ".sav") != nullptr) descriptor = 14;
		if (descriptor >= 0) {
			Node *const node = NodeFor(descriptor);
			assert(node != nullptr);
			if (!node->open) {
				node->open = true;
				++evidence_.open_descriptor_count;
			}
			return {descriptor, 0};
		}
		return {-1, ENOENT};
	}
	int Stat(int descriptor, struct stat *info) override
	{ return Copy(NodeFor(descriptor), info); }
	int StatAt(int parent, const char *name, struct stat *info, int) override
	{
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0)
			return Copy(&parent_, info);
		if (parent == 11 && strcmp(name, "saves") == 0) return Copy(&save_root_, info);
		if (parent == 12 && strcmp(name, "snes") == 0) return Copy(&system_, info);
		if (parent == 13 && strstr(name, ".sav") != nullptr) return Copy(&file_, info);
		return -1;
	}
	Result MountId(int descriptor, uint64_t *mount_id) override
	{
		if (NodeFor(descriptor) == nullptr || mount_id == nullptr)
			return MISTER_RESULT_PLATFORM;
		*mount_id = 1;
		return MISTER_RESULT_OK;
	}
	int Fdatasync(int descriptor) override
	{
		if (descriptor != 14) return -1;
		CaptureCallbackSnapshot();
		++evidence_.fdatasync_attempts;
		trace_.push_back('d');
		if (fdatasync_fail_once_) {
			fdatasync_fail_once_ = false;
			return -1;
		}
		evidence_.file_synced = true;
		return 0;
	}
	int Fsync(int descriptor) override
	{
		if (descriptor == 14) {
			++evidence_.file_fsync_successes;
			trace_.push_back('f');
			return 0;
		}
		if (descriptor == 13) {
			++evidence_.directory_fsync_successes;
			evidence_.directory_synced = true;
			trace_.push_back('s');
			return 0;
		}
		return -1;
	}
	int Close(int descriptor) override
	{
		Node *const node = NodeFor(descriptor);
		if (node == nullptr || !node->open) return -1;
		node->open = false;
		--evidence_.open_descriptor_count;
		++evidence_.descriptor_close_successes;
		evidence_.descriptors_closed = evidence_.open_descriptor_count == 0;
		if (evidence_.descriptors_closed) evidence_.directory_synced = true;
		trace_.push_back(static_cast<char>('0' + descriptor - 10));
		return 0;
	}
	void FailFdatasyncOnce() { fdatasync_fail_once_ = true; }
	int calls() const { return calls_; }
	const SaveResourceEvidence &evidence() const { return evidence_; }
	const std::vector<char> &trace() const { return trace_; }
	const std::vector<NativeRetainedOperationSnapshot> &callback_snapshots() const
	{ return callback_snapshots_; }
	void ObserveSnapshots(NativeRecovery *recovery, const RecoveryEpoch *epoch)
	{
		snapshot_recovery_ = recovery;
		snapshot_epoch_ = epoch;
	}

private:
	struct Node { int descriptor; struct stat identity; bool open; };
	static void Initialize(Node *node, int descriptor, ino_t inode, mode_t mode,
		uid_t uid, gid_t gid)
	{
		node->descriptor = descriptor;
		node->open = false;
		memset(&node->identity, 0, sizeof(node->identity));
		node->identity.st_dev = 1;
		node->identity.st_ino = inode;
		node->identity.st_mode = mode;
		node->identity.st_nlink = 1;
		node->identity.st_uid = uid;
		node->identity.st_gid = gid;
	}
	Node *NodeFor(int descriptor)
	{
		Node *nodes[] = {&root_, &parent_, &save_root_, &system_, &file_};
		for (size_t index = 0; index != sizeof(nodes) / sizeof(nodes[0]); ++index)
			if (nodes[index]->descriptor == descriptor) return nodes[index];
		return nullptr;
	}
	static int Copy(const Node *node, struct stat *info)
	{
		if (node == nullptr || info == nullptr) return -1;
		*info = node->identity;
		return 0;
	}
	void CaptureCallbackSnapshot()
	{
		if (snapshot_recovery_ == nullptr || snapshot_epoch_ == nullptr) return;
		NativeRetainedOperationSnapshot snapshot = {};
		assert(snapshot_recovery_->retained_snapshot_for_test(*snapshot_epoch_,
			OperationKind::save, &snapshot) == MISTER_RESULT_OK);
		callback_snapshots_.push_back(snapshot);
	}

	bool fdatasync_fail_once_;
	int calls_;
	SaveResourceEvidence evidence_;
	std::vector<char> trace_;
	NativeRecovery *snapshot_recovery_;
	const RecoveryEpoch *snapshot_epoch_;
	std::vector<NativeRetainedOperationSnapshot> callback_snapshots_;
	Node root_;
	Node parent_;
	Node save_root_;
	Node system_;
	Node file_;
};

class FakeContainmentIo final : public NativeContainmentIo {
public:
	FakeContainmentIo()
		: fail_at(0), advance_at(0), advance_clock(nullptr), calls(0), writes(0),
		  reads(0), releases(0), last_deadline(0), advance_to(6000),
		  step_failure_result(MISTER_RESULT_PLATFORM), core(0x40000000u), interface(0),
		  sdr(0), bridge(7), remap(1), release_result(MISTER_RESULT_OK) {}
	Result Step()
	{
		++calls;
		if (calls == advance_at && advance_clock != nullptr)
			advance_clock->SetNow(advance_to);
		return calls == fail_at ? step_failure_result : MISTER_RESULT_OK;
	}
	Result WriteCoreReset(const Access &, uint32_t mask,
		uint32_t value) override
	{
		assert(mask == 0xc0000000u && value == 0x40000000u);
		++writes;
		return Step();
	}
	Result WriteInterfaceModule(const Access &, uint32_t value) override
	{
		assert(value == 0); ++writes; return Step();
	}
	Result WriteSdrPortControl(const Access &, uint32_t offset,
		uint32_t value) override
	{
		assert(offset == 0x5080u && value == 0); ++writes; return Step();
	}
	Result WriteBridgeReset(const Access &, uint32_t value) override
	{
		assert(value == 7); ++writes; return Step();
	}
	Result WriteRemap(const Access &, uint32_t value) override
	{
		assert(value == 1); ++writes; return Step();
	}
	NativeManagerNeutralReceipt ReconcileManager(const Access &) override
	{
		const NativeManagerNeutralReceipt receipt = {
			MISTER_RESULT_OK, 2, 2, true, false, false};
		return receipt;
	}
	Result ReadManagerControl(const Access &, uint32_t *value) override
	{
		*value = 2;
		return MISTER_RESULT_OK;
	}
	Result ReadManagerMode(const Access &, uint32_t *value) override
	{
		*value = 2;
		return MISTER_RESULT_OK;
	}
	Result ReadCoreGpo(const Access &access, uint32_t *value) override
	{
		last_deadline = access.absolute_deadline_ms();
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = core; return result;
	}
	Result ReadInterfaceModule(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = interface; return result;
	}
	Result ReadSdrPortControl(const Access &, uint32_t offset,
		uint32_t *value) override
	{
		assert(offset == 0x5080u); ++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = sdr; return result;
	}
	Result ReadBridgeReset(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = bridge; return result;
	}
	Result ReadRemap(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = remap; return result;
	}
	Result ReleaseMappings(const Access &) override
	{
		++releases; const Result result = Step(); return result == MISTER_RESULT_OK ? release_result : result;
	}

	int fail_at;
	int advance_at;
	FakeClock *advance_clock;
	int calls;
	int writes;
	int reads;
	int releases;
	uint64_t last_deadline;
	uint64_t advance_to;
	Result step_failure_result;
	uint32_t core;
	uint32_t interface;
	uint32_t sdr;
	uint32_t bridge;
	uint32_t remap;
	Result release_result;
};

MisterRecoveryObservationV2 Observation()
{
	MisterRecoveryObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

void TestRecoveryEpochAuthorityAndFreshness()
{
	static_assert(!std::is_default_constructible<RecoveryEpoch>::value,
		"recovery epochs are broker minted");
	static_assert(!std::is_copy_constructible<RecoveryEpoch>::value,
		"recovery epochs are not caller-copyable");
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(1u << 8, 3000, 6000, &epoch) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	assert(!broker.has_live_generation_for_test());
	const uint64_t first_identity = epoch->identity_for_test();
	assert(first_identity != 0);
	std::unique_ptr<RecoveryEpoch> duplicate;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 4000, 7000,
		&duplicate) == MISTER_RESULT_INVALID_STATE);
	epoch.reset();
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 4000, 7000,
		&epoch) == MISTER_RESULT_OK);
	assert(epoch->identity_for_test() != first_identity);
}

void TestRecoveryInvocationBoundsGenericMutation()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CONTENT, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &invocation) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, *invocation, OperationKind::content) ==
		MISTER_RESULT_OK);
	assert(io.last_deadline == 1500);
	assert(broker.FinishInvocation(std::move(invocation)) == MISTER_RESULT_OK);
}

void TestRecoveryInvocationRebindsRetainedSaveDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources typed(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, typed, typed, typed, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
		MISTER_RESULT_OK);
	filesystem.ObserveSnapshots(&recovery, epoch.get());
	filesystem.FailFdatasyncOnce();
	assert(recovery.Perform(*epoch, *first, OperationKind::save) ==
		MISTER_RESULT_PLATFORM);
	assert(filesystem.callback_snapshots().size() == 1);
	const NativeRetainedOperationSnapshot live =
		filesystem.callback_snapshots()[0];
	assert(live.query_valid && live.retained && live.registration_is_invoked);
	assert(live.peripheral_disposition == PeripheralBrokerDisposition::no_session);
	assert(live.registration_effective_deadline_ms == 1500);
	assert(live.process_guard_active);
	NativeRetainedOperationSnapshot invoked_outcome = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&invoked_outcome) == MISTER_RESULT_OK);
	assert(invoked_outcome.registration_is_invoked &&
		invoked_outcome.registration_outcome_recorded &&
		!invoked_outcome.process_guard_active);
	assert(filesystem.trace().size() == 1 && filesystem.trace()[0] == 'd');
	assert(filesystem.evidence().fdatasync_attempts == 1);
	assert(filesystem.evidence().open_descriptor_count == 5);
	assert(!filesystem.evidence().file_synced &&
		!filesystem.evidence().directory_synced &&
		!filesystem.evidence().descriptors_closed &&
		!filesystem.evidence().closure_unknown);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot suspended = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&suspended) == MISTER_RESULT_OK);
	assert(suspended.registration_is_suspended &&
		suspended.registration_effective_deadline_ms == 0);
	assert(suspended.lease_identity == live.lease_identity &&
		suspended.registration_identity == live.registration_identity &&
		suspended.backend_identity == live.backend_identity);
	AssertOnlyInvocationSuspensionChanged(invoked_outcome, suspended);
	NativeRetainedOperationSnapshot suspended_again = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&suspended_again) == MISTER_RESULT_OK);
	AssertSameSnapshot(suspended, suspended_again);
	const SaveResourceEvidence failed_evidence = filesystem.evidence();
	const std::vector<char> failed_trace = filesystem.trace();

	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::save) ==
		MISTER_RESULT_OK);
	assert(filesystem.callback_snapshots().size() == 2);
	const NativeRetainedOperationSnapshot rebound =
		filesystem.callback_snapshots()[1];
	assert(rebound.registration_is_invoked && rebound.invocation_identity != 0);
	assert(rebound.invocation_identity != live.invocation_identity);
	assert(rebound.registration_effective_deadline_ms == 1800);
	assert(rebound.lease_identity == live.lease_identity &&
		rebound.registration_identity == live.registration_identity &&
		rebound.backend_identity == live.backend_identity);
	AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
	assert(failed_evidence.fdatasync_attempts == 1 &&
		failed_evidence.open_descriptor_count == 5);
	assert(failed_trace.size() == 1 && failed_trace[0] == 'd');
	assert(filesystem.evidence().fdatasync_attempts == 2);
	assert(filesystem.evidence().file_synced);
	assert(filesystem.evidence().directory_synced);
	assert(filesystem.evidence().descriptors_closed);
	assert(filesystem.evidence().open_descriptor_count == 0);
	assert(filesystem.evidence().descriptor_close_successes == 5);
	assert(filesystem.evidence().file_fsync_successes == 0);
	assert(filesystem.evidence().directory_fsync_successes == 0);
	assert(!filesystem.evidence().closure_unknown);
	const char expected_trace[] = {'d', 'd', '4', '3', '2', '1', '0'};
	assert(filesystem.trace().size() == sizeof(expected_trace));
	for (size_t index = 0; index != sizeof(expected_trace); ++index)
		assert(filesystem.trace()[index] == expected_trace[index]);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot destroyed = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&destroyed) == MISTER_RESULT_OK);
	assert(destroyed.query_valid && !destroyed.retained);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
}

void TestRecoveryInvocationBoundsContainmentObservation()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &invocation) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch, *invocation) == MISTER_RESULT_OK);
	assert(io.reads == 5 && io.last_deadline == 1500);
	assert(broker.FinishInvocation(std::move(invocation)) == MISTER_RESULT_OK);
}

void TestCallbackDeadlineDoesNotPoisonRecoveryEpochOrOriginalMask()
{
	class DeadlineOnceIo final : public NativeRecoveryIo {
	public:
		explicit DeadlineOnceIo(FakeClock &clock) : clock_(clock), calls_(0) {}
		Result CloseContent(const OperationLease &, RecoveryResourceState *state) override
		{
			++calls_;
			*state = calls_ == 1 ? RecoveryResourceState::unknown :
				RecoveryResourceState::neutral;
			if (calls_ == 1) clock_.SetNow(1500);
			return MISTER_RESULT_OK;
		}
		Result CloseInputDescriptors(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		Result MuteAudio(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		Result PowerDownVideo(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		Result DisableCoreProtocol(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		FakeClock &clock_;
		int calls_;
	};
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	DeadlineOnceIo io(clock);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CONTENT, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::content) ==
		MISTER_RESULT_DEADLINE);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 snapshot = Observation();
	assert(recovery.Snapshot(*epoch, &snapshot) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_CONTENT) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, 0) ==
		MISTER_RESULT_INVALID_STATE);

	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::content) ==
		MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	assert(recovery.Finish(std::move(epoch), &snapshot) == MISTER_RESULT_OK);
	assert(io.calls_ == 2);
}

void TestCallbackDeadlineRetriesEveryRetainedTypedRecoveryClass()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const OperationKind kinds[] = {OperationKind::audio, OperationKind::video,
		OperationKind::audio_video, OperationKind::core_protocol,
		OperationKind::save};
	const uint32_t requested[] = {MISTER_RESOURCE_NATIVE_AUDIO | closure,
		MISTER_RESOURCE_NATIVE_VIDEO,
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO | closure,
		MISTER_RESOURCE_CORE_PROTOCOL, MISTER_RESOURCE_SAVES};
	for (size_t index = 0; index != sizeof(kinds) / sizeof(kinds[0]); ++index) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources typed(broker);
		FakeTypedSaveRecoveryResource save;
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		io.deadline_clock = &clock;
		io.deadline_core_once = kinds[index] == OperationKind::core_protocol;
		io.states[KindIndex(OperationKind::core_protocol)] =
			RecoveryResourceState::neutral;
		typed.deadline_clock = &clock;
		typed.deadline_once_kind = kinds[index];
		typed.coupled_all_neutral = true;
		save.deadline_clock = &clock;
		save.deadline_once = kinds[index] == OperationKind::save;
		NativeRecovery recovery(broker, io, typed, typed, typed, save, containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested[index], 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		const uint64_t identity = epoch->identity_for_test();
		std::unique_ptr<OperationInvocation> first;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *first, kinds[index]) ==
			MISTER_RESULT_DEADLINE);
		assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
		clock.SetNow(1600);
		std::unique_ptr<OperationInvocation> second;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *second, kinds[index]) == MISTER_RESULT_OK);
		assert(epoch->identity_for_test() == identity);
		assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
		if (kinds[index] == OperationKind::audio ||
			kinds[index] == OperationKind::audio_video) {
			if (kinds[index] == OperationKind::audio_video) {
				std::unique_ptr<OperationInvocation> audio;
				assert(broker.BeginRecoveryInvocation(*epoch, 2500, &audio) ==
					MISTER_RESULT_OK);
				assert(recovery.Perform(*epoch, *audio, OperationKind::audio) ==
					MISTER_RESULT_OK);
				assert(broker.FinishInvocation(std::move(audio)) == MISTER_RESULT_OK);
			}
			std::unique_ptr<OperationInvocation> terminal;
			assert(broker.BeginRecoveryInvocation(*epoch, 2500, &terminal) ==
				MISTER_RESULT_OK);
			assert(recovery.Perform(*epoch, *terminal,
				OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_OK);
			assert(broker.FinishInvocation(std::move(terminal)) == MISTER_RESULT_OK);
		}
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == requested[index]);
	}
}

// Breaks if any retained recovery kind uses a callback deadline after it has
// expired, or lets a callback extend its immutable recovery group deadline.
void TestRetainedRecoveryDeadlineBoundariesForEveryKind()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	struct Case {
		OperationKind kind;
		uint32_t requested;
		uint64_t group_deadline;
	};
	const Case cases[] = {
		{OperationKind::save, MISTER_RESOURCE_SAVES, 3000},
		{OperationKind::video, MISTER_RESOURCE_NATIVE_VIDEO, 3000},
		{OperationKind::audio, MISTER_RESOURCE_NATIVE_AUDIO | closure, 3000},
		{OperationKind::audio_video,
			MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO | closure,
			3000},
		{OperationKind::core_protocol, MISTER_RESOURCE_CORE_PROTOCOL, 6000}
	};
	const int offsets[] = {-1, 0, 1};
	for (const Case &test : cases) {
		for (size_t group = 0; group != 2; ++group) {
			for (int offset : offsets) {
				FakeClock clock(1000);
				HardwareBroker broker(clock);
				FakeRecoveryIo io(broker);
				FakeTypedRecoveryResources typed(broker);
				FakeTypedSaveRecoveryResource save;
				FakeContainmentIo containment_io;
				NativeContainment containment(broker, containment_io);
				io.states[KindIndex(OperationKind::core_protocol)] =
					RecoveryResourceState::neutral;
				io.deadline_clock = &clock;
				io.deadline_core_once = test.kind == OperationKind::core_protocol;
				typed.deadline_clock = &clock;
				typed.deadline_once_kind = test.kind;
				typed.deadline_to_expire = 1100;
				typed.coupled_all_neutral = false;
				save.deadline_clock = &clock;
				save.deadline_once = test.kind == OperationKind::save;
				std::unique_ptr<NativeRecovery> recovery(new NativeRecovery(broker, io,
					typed, typed, typed, save, containment));
				std::unique_ptr<RecoveryEpoch> epoch;
				assert(broker.BeginRecovery(test.requested, 3000, 6000, &epoch) ==
					MISTER_RESULT_OK);
				io.snapshot_recovery = recovery.get();
				io.snapshot_epoch = epoch.get();
				typed.snapshot_recovery = recovery.get();
				typed.snapshot_epoch = epoch.get();
				save.snapshot_recovery = recovery.get();
				save.snapshot_epoch = epoch.get();
				const auto resource_evidence = [&]() {
					DeadlineResourceEvidence evidence = {};
					if (test.kind == OperationKind::core_protocol) {
						evidence.callback_count = static_cast<size_t>(
							io.core_protocol_session_calls);
						evidence.core_mapping_count = io.core_mapping_count;
						evidence.core_descriptor_count = io.core_descriptor_count;
						evidence.core_release_attempts = io.core_release_attempts;
						evidence.core_release_successes = io.core_release_successes;
						evidence.core_selected_transaction_closed =
							io.core_selected_transaction_closed;
					} else if (test.kind == OperationKind::save) {
						evidence.callback_count = save.evidence.callback_count;
						evidence.transaction_closed =
							save.evidence.descriptors_closed;
						evidence.local_resources_absent =
							save.evidence.descriptors_closed;
						evidence.closure_unknown = save.evidence.closure_unknown;
					} else {
						const PeripheralResourceEvidence peripheral =
							typed.Evidence(test.kind);
						evidence.callback_count = peripheral.callback_count;
						evidence.mutation_count = peripheral.mutation_count;
						evidence.final_ack_count = peripheral.final_ack_count;
						evidence.final_ack = peripheral.final_ack;
						evidence.accepted_mutation_sequence =
							peripheral.accepted_mutation_sequence;
						evidence.transaction_closed = peripheral.transaction_closed;
						evidence.local_resources_absent =
							peripheral.local_resources_absent;
						evidence.closure_unknown = peripheral.closure_unknown;
					}
					return evidence;
				};
				std::unique_ptr<OperationInvocation> first;
				assert(broker.BeginRecoveryInvocation(*epoch, 1100, &first) ==
					MISTER_RESULT_OK);
				assert(recovery->Perform(*epoch, *first, test.kind) ==
					MISTER_RESULT_DEADLINE);
				assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
				NativeRetainedOperationSnapshot suspended = {};
				assert(recovery->retained_snapshot_for_test(*epoch, test.kind,
					&suspended) == MISTER_RESULT_OK);
				assert(suspended.retained && suspended.registration_is_suspended &&
					suspended.registration_effective_deadline_ms == 0);
				const DeadlineResourceEvidence rejected_resource_before =
					resource_evidence();
				if (test.kind == OperationKind::audio ||
					test.kind == OperationKind::video ||
					test.kind == OperationKind::audio_video) {
					AssertPeripheralSnapshotPrefixMatchesEvidence(suspended,
						typed.Evidence(test.kind));
				}
				const uint64_t boundary = group == 0 ? 1500 :
					test.group_deadline;
				clock.SetNow(static_cast<uint64_t>(
					static_cast<int64_t>(boundary) + offset));
				std::unique_ptr<OperationInvocation> retry;
				const Result begin = broker.BeginRecoveryInvocation(*epoch,
					group == 0 ? 1500 : UINT64_MAX, &retry);
				if (offset >= 0) {
					if (begin == MISTER_RESULT_OK) {
						assert(recovery->Perform(*epoch, *retry, test.kind) ==
							MISTER_RESULT_DEADLINE);
						assert(broker.FinishInvocation(std::move(retry)) ==
							MISTER_RESULT_OK);
					} else {
						assert(begin == MISTER_RESULT_DEADLINE && retry == nullptr);
					}
					NativeRetainedOperationSnapshot unchanged = {};
					assert(recovery->retained_snapshot_for_test(*epoch, test.kind,
						&unchanged) == MISTER_RESULT_OK);
					if (unchanged.recovery_result == suspended.recovery_result)
						AssertSameSnapshot(suspended, unchanged);
					else
						AssertOnlyRecoveryResultChanged(suspended, unchanged,
							MISTER_RESULT_DEADLINE);
					NativeRetainedOperationSnapshot unchanged_again = {};
					assert(recovery->retained_snapshot_for_test(*epoch, test.kind,
						&unchanged_again) == MISTER_RESULT_OK);
					AssertSameSnapshot(unchanged, unchanged_again);
					AssertSameDeadlineResourceEvidence(rejected_resource_before,
						resource_evidence());
					if (group == 1) {
						assert(unchanged.recovery_result == MISTER_RESULT_DEADLINE);
						assert(unchanged.authority_identity ==
							suspended.authority_identity &&
							unchanged.requested_resource_flags == test.requested &&
							unchanged.non_fpga_deadline_ms == 3000 &&
							unchanged.fpga_deadline_ms == 6000 &&
							unchanged.lease_identity == suspended.lease_identity &&
							unchanged.registration_identity ==
								suspended.registration_identity &&
							unchanged.session_identity == suspended.session_identity &&
							unchanged.registration_is_suspended);
						std::unique_ptr<OperationInvocation> later;
						const Result later_begin = broker.BeginRecoveryInvocation(*epoch,
							UINT64_MAX, &later);
						if (later_begin == MISTER_RESULT_OK) {
							assert(recovery->Perform(*epoch, *later, test.kind) ==
								MISTER_RESULT_DEADLINE);
							assert(broker.FinishInvocation(std::move(later)) ==
								MISTER_RESULT_OK);
						} else {
							assert(later_begin == MISTER_RESULT_DEADLINE && later == nullptr);
						}
						NativeRetainedOperationSnapshot terminal_again = {};
						assert(recovery->retained_snapshot_for_test(*epoch, test.kind,
							&terminal_again) == MISTER_RESULT_OK);
						AssertSameSnapshot(unchanged, terminal_again);
						AssertSameDeadlineResourceEvidence(rejected_resource_before,
							resource_evidence());
						MisterRecoveryObservationV2 observation = Observation();
						assert(recovery->Finish(std::move(epoch), &observation) ==
							MISTER_RESULT_INVALID_STATE);
						assert(epoch != nullptr);
						recovery.reset();
						AssertSameDeadlineResourceEvidence(rejected_resource_before,
							resource_evidence());
						io.snapshot_recovery = nullptr;
						typed.snapshot_recovery = nullptr;
						save.snapshot_recovery = nullptr;
						NativeRecovery teardown_observer(broker, io, typed, typed,
							typed, save, containment);
						NativeRetainedOperationSnapshot dropped = {};
						assert(teardown_observer.retained_snapshot_for_test(*epoch,
							test.kind, &dropped) == MISTER_RESULT_OK);
						assert(dropped.query_valid && !dropped.retained &&
							dropped.active_lease_count == 0 &&
							!dropped.invocation_registered);
						const Result terminal_result = broker.FinishRecovery(
							std::move(epoch), &observation);
						assert(terminal_result == MISTER_RESULT_DEADLINE);
						assert(epoch == nullptr);
						NativeRetainedOperationSnapshot baseline = {};
						assert(teardown_observer.broker_baseline_snapshot_for_test(
							&baseline) == MISTER_RESULT_OK);
						AssertExactIdleSnapshot(baseline);
						continue;
					}
					clock.SetNow(1600);
					assert(clock.NowMs() < 2500 && 2500 < test.group_deadline);
					assert(broker.BeginRecoveryInvocation(*epoch, 2500, &retry) ==
						MISTER_RESULT_OK);
				} else {
					assert(begin == MISTER_RESULT_OK);
				}
				io.live_snapshot_captured = false;
				typed.live_snapshot_captured = false;
				save.live_snapshot_captured = false;
				assert(recovery->Perform(*epoch, *retry, test.kind) ==
					MISTER_RESULT_OK);
				const NativeRetainedOperationSnapshot rebound =
					test.kind == OperationKind::core_protocol ? io.live_snapshot :
					test.kind == OperationKind::save ? save.live_snapshot :
					typed.live_snapshot;
				assert(rebound.query_valid && rebound.retained &&
					rebound.registration_is_invoked);
				assert(rebound.lease_identity == suspended.lease_identity &&
					rebound.registration_identity ==
						suspended.registration_identity &&
					rebound.session_identity == suspended.session_identity &&
					rebound.backend_identity == suspended.backend_identity);
				AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
				if (test.kind == OperationKind::audio ||
					test.kind == OperationKind::video ||
					test.kind == OperationKind::audio_video) {
					AssertPeripheralSnapshotPrefixMatchesEvidence(rebound,
						typed.Evidence(test.kind).mutation_count ==
							rejected_resource_before.mutation_count ?
							typed.Evidence(test.kind) :
							PeripheralResourceEvidence{
								rejected_resource_before.callback_count,
								rejected_resource_before.mutation_count,
								rejected_resource_before.final_ack_count,
								rejected_resource_before.final_ack,
								rejected_resource_before.accepted_mutation_sequence,
								rejected_resource_before.transaction_closed,
								rejected_resource_before.local_resources_absent,
								rejected_resource_before.closure_unknown});
				}
				const DeadlineResourceEvidence successful_resource =
					resource_evidence();
				assert(successful_resource.callback_count ==
					rejected_resource_before.callback_count + 1);
				if (test.kind == OperationKind::audio ||
					test.kind == OperationKind::audio_video) {
					assert(successful_resource.mutation_count ==
						rejected_resource_before.mutation_count + 1);
				} else {
					assert(successful_resource.mutation_count ==
						rejected_resource_before.mutation_count);
				}
				if (test.kind == OperationKind::audio ||
					test.kind == OperationKind::video ||
					test.kind == OperationKind::audio_video) {
					assert(successful_resource.final_ack_count ==
						rejected_resource_before.final_ack_count + 1);
					assert(successful_resource.final_ack == 0xa55a);
					assert(successful_resource.transaction_closed);
					assert(successful_resource.local_resources_absent);
					assert(!successful_resource.closure_unknown);
				}
				if (test.kind == OperationKind::core_protocol) {
					assert(successful_resource.core_mapping_count == 0);
					assert(successful_resource.core_descriptor_count == 0);
					assert(successful_resource.core_release_attempts ==
						rejected_resource_before.core_release_attempts + 1);
					assert(successful_resource.core_release_successes == 1);
					assert(successful_resource.core_selected_transaction_closed);
				}
				assert(broker.FinishInvocation(std::move(retry)) ==
					MISTER_RESULT_OK);
				NativeRetainedOperationSnapshot destroyed = {};
				assert(recovery->retained_snapshot_for_test(*epoch, test.kind,
					&destroyed) == MISTER_RESULT_OK);
				assert(destroyed.query_valid && !destroyed.retained);
				if (test.kind == OperationKind::audio_video) {
					std::unique_ptr<OperationInvocation> audio;
					assert(broker.BeginRecoveryInvocation(*epoch, UINT64_MAX, &audio) ==
						MISTER_RESULT_OK);
					assert(recovery->Perform(*epoch, *audio, OperationKind::audio) ==
						MISTER_RESULT_OK);
					assert(broker.FinishInvocation(std::move(audio)) ==
						MISTER_RESULT_OK);
				}
				if (test.kind == OperationKind::audio ||
					test.kind == OperationKind::audio_video) {
					std::unique_ptr<OperationInvocation> terminal;
					assert(broker.BeginRecoveryInvocation(*epoch, 5800, &terminal) ==
						MISTER_RESULT_OK);
					assert(recovery->Perform(*epoch, *terminal,
						OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_OK);
					assert(broker.FinishInvocation(std::move(terminal)) ==
						MISTER_RESULT_OK);
				}
				MisterRecoveryObservationV2 observation = Observation();
				assert(recovery->Finish(std::move(epoch), &observation) ==
					MISTER_RESULT_OK);
				assert(observation.neutral_resource_flags == test.requested);
				NativeRetainedOperationSnapshot baseline = {};
				assert(recovery->broker_baseline_snapshot_for_test(&baseline) ==
					MISTER_RESULT_OK);
				AssertExactIdleSnapshot(baseline);
			}
		}
	}
}

void TestCallbackDeadlineRetriesObservationAndTerminalResidue()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.advance_clock = &clock;
		io.advance_at = 2;
		io.advance_to = 1500;
		std::unique_ptr<OperationInvocation> first;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
			MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch, *first) ==
			MISTER_RESULT_DEADLINE);
		assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
		io.advance_at = 0;
		std::unique_ptr<OperationInvocation> second;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
			MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch, *second) == MISTER_RESULT_OK);
		assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == closure);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo recovery_io(broker);
		FakeTypedRecoveryResources typed(broker);
		FakeTypedSaveRecoveryResource save;
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		NativeRecovery recovery(broker, recovery_io, typed, typed, typed, save,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.advance_clock = &clock;
		io.advance_at = 11;
		io.advance_to = 1500;
		io.release_result = MISTER_RESULT_DEADLINE;
		std::unique_ptr<OperationInvocation> first;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *first,
			OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_DEADLINE);
		assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
		io.advance_at = 0;
		io.release_result = MISTER_RESULT_OK;
		std::unique_ptr<OperationInvocation> second;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *second,
			OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_OK);
		assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
		assert(io.writes == 5 && io.reads == 5 && io.releases == 2);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == closure);
	}
}

void TestRetryableRecoveryResultsReachSameEpochSuccess()
{
	const Result retryable[] = {MISTER_RESULT_CLEANUP_INCOMPLETE,
		MISTER_RESULT_PLATFORM};
	for (Result first_result : retryable) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		NativeRecovery recovery(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CONTENT, 3000, 6000,
			&epoch) == MISTER_RESULT_OK);
		const uint64_t identity = epoch->identity_for_test();
		io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::unknown;
		io.results[KindIndex(OperationKind::content)] = first_result;
		assert(recovery.Perform(*epoch, OperationKind::content) == first_result);
		assert(io.last_deadline == 3000);
		MisterRecoveryObservationV2 snapshot = Observation();
		assert(recovery.Snapshot(*epoch, &snapshot) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
		io.results[KindIndex(OperationKind::content)] = MISTER_RESULT_OK;
		clock.SetNow(1500);
		assert(recovery.Perform(*epoch, OperationKind::content) == MISTER_RESULT_OK);
		assert(epoch->identity_for_test() == identity);
		assert(io.last_deadline == 3000);
		assert(recovery.Finish(std::move(epoch), &snapshot) == MISTER_RESULT_OK);
		assert(snapshot.neutral_resource_flags == MISTER_RESOURCE_CONTENT);
	}
}

void TestRetryableObservationAndTerminalFailuresReachSuccess()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const Result retryable[] = {MISTER_RESULT_CLEANUP_INCOMPLETE,
		MISTER_RESULT_PLATFORM};
	for (Result first_result : retryable) {
		{
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeContainmentIo io;
			io.fail_at = 2;
			io.step_failure_result = first_result;
			NativeContainment containment(broker, io);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			const uint64_t identity = epoch->identity_for_test();
			assert(containment.ObserveRecovery(*epoch) == first_result);
			io.fail_at = 0;
			assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
			assert(epoch->identity_for_test() == identity);
			MisterRecoveryObservationV2 observation = Observation();
			assert(broker.FinishRecovery(std::move(epoch), &observation) ==
				MISTER_RESULT_OK);
			assert(observation.neutral_resource_flags == closure);
		}
		{
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeRecoveryIo recovery_io(broker);
			FakeTypedRecoveryResources typed(broker);
			FakeTypedSaveRecoveryResource save;
			FakeContainmentIo io;
			io.release_result = first_result;
			NativeContainment containment(broker, io);
			NativeRecovery recovery(broker, recovery_io, typed, typed, typed, save,
				containment);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			const uint64_t identity = epoch->identity_for_test();
			assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
				first_result);
			assert(io.writes == 5 && io.reads == 5 && io.releases == 1);
			io.release_result = MISTER_RESULT_OK;
			assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
				MISTER_RESULT_OK);
			assert(epoch->identity_for_test() == identity);
			assert(io.writes == 5 && io.reads == 5 && io.releases == 2);
			MisterRecoveryObservationV2 observation = Observation();
			assert(recovery.Finish(std::move(epoch), &observation) ==
				MISTER_RESULT_OK);
			assert(observation.neutral_resource_flags == closure);
		}
	}
}

void TestRecoveryRejectedWithLiveGenerationOrCleanup()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<RecoveryEpoch> recovery;
	assert(broker.BeginRecovery(0, 3000, 6000, &recovery) ==
		MISTER_RESULT_INVALID_STATE);
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 3000, 6000, &cleanup) ==
		MISTER_RESULT_OK);
	assert(broker.BeginRecovery(0, 3000, 6000, &recovery) ==
		MISTER_RESULT_INVALID_STATE);
}

void TestNormativeDependenciesAndExactDeadlines()
{
	struct Case {
		OperationKind kind;
		uint32_t bit;
		uint64_t deadline;
	};
	const Case cases[] = {
		{OperationKind::input_descriptors, MISTER_RESOURCE_CORE_INPUT, 3000},
		{OperationKind::audio, MISTER_RESOURCE_NATIVE_AUDIO, 3000},
		{OperationKind::video, MISTER_RESOURCE_NATIVE_VIDEO, 3000},
		{OperationKind::content, MISTER_RESOURCE_CONTENT, 3000},
		{OperationKind::core_protocol, MISTER_RESOURCE_CORE_PROTOCOL, 6000}
	};
	for (const Case &test : cases) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		NativeRecovery recovery(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(test.bit, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(test.kind)] = RecoveryResourceState::neutral;
		assert(recovery.Perform(*epoch, test.kind) == MISTER_RESULT_OK);
		assert(io.last_deadline == test.deadline);
	}

	FakeClock clock(1000);
	HardwareBroker broker(clock);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_V2_KNOWN, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	const OperationKind forbidden[] = {OperationKind::program_fpga,
		OperationKind::input, OperationKind::scheduler, OperationKind::offload};
	for (OperationKind kind : forbidden) {
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, kind, &lease) ==
			MISTER_RESULT_INVALID_STATE);
	}

	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(terminal->absolute_deadline_ms() == 6000);
	terminal.reset();
	epoch.reset();
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	for (uint32_t missing : {MISTER_RESOURCE_FPGA, MISTER_RESOURCE_BRIDGES,
		MISTER_RESOURCE_CORE_PROTOCOL}) {
		assert(broker.BeginRecovery(closure & ~missing, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) ==
			MISTER_RESULT_INVALID_STATE);
		epoch.reset();
	}
}

void TestSaveRecoveryRequiresTypedSafeRecordAuthority()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::save)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::save) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(io.calls == 0);
}

void TestTruthfulPartitionAndExactOkRule()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	io.states[KindIndex(OperationKind::audio)] =
		RecoveryResourceState::observed_non_neutral;
	io.states[KindIndex(OperationKind::video)] = RecoveryResourceState::neutral;
	io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::video) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::content) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.neutral_resource_flags == (MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT));
	assert(observation.observed_resource_flags == MISTER_RESOURCE_NATIVE_AUDIO);
	assert((observation.neutral_resource_flags &
		observation.observed_resource_flags) == 0);
	assert(((observation.neutral_resource_flags |
		observation.observed_resource_flags) & ~requested) == 0);
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
}

void TestNonOkResultsRetainPartialFields()
{
	const Result failures[] = {MISTER_RESULT_DEADLINE, MISTER_RESULT_PLATFORM,
		MISTER_RESULT_CLEANUP_INCOMPLETE};
	for (Result failure : failures) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		NativeRecovery recovery(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
			MISTER_RESOURCE_NATIVE_AUDIO;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(OperationKind::input_descriptors)] =
			RecoveryResourceState::neutral;
		assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(OperationKind::audio)] =
			RecoveryResourceState::observed_non_neutral;
		io.results[KindIndex(OperationKind::audio)] = failure;
		assert(recovery.Perform(*epoch, OperationKind::audio) == failure);
		MisterRecoveryObservationV2 observation = Observation();
		const Result snapshot_result = MISTER_RESULT_CLEANUP_INCOMPLETE;
		assert(recovery.Snapshot(*epoch, &observation) == snapshot_result);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
		assert(observation.observed_resource_flags == MISTER_RESOURCE_NATIVE_AUDIO);
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
	}
}

void TestDeadlineExpiryDoesNotExtendAndRetainsProgress()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_NATIVE_AUDIO;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	clock.SetNow(3000);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_DEADLINE);
	const int calls_before_retry = io.calls;
	clock.SetNow(2500);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_DEADLINE);
	assert(io.calls == calls_before_retry);
	assert(io.last_deadline == 3000);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
}

void TestTerminalAdmissionDeadlineRetainsProgress()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT | closure;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	clock.SetNow(6000);
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) ==
		MISTER_RESULT_DEADLINE);
	assert(terminal == nullptr);
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(observation.observed_resource_flags == 0);
}

void TestBusyRecoveryAdmissionDoesNotLatchDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_SAVES;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> held;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::input_descriptors, &held) == MISTER_RESULT_OK);
	clock.SetNow(3000);
	std::unique_ptr<OperationLease> rejected;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::save,
		&rejected) == MISTER_RESULT_INVALID_STATE);
	assert(rejected == nullptr);
	held.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == 0);
}

void TestOperationOverrunRetainsTruthfulResult()
{
	class AdvancingRecoveryIo final : public NativeRecoveryIo {
	public:
		explicit AdvancingRecoveryIo(FakeClock &clock) : clock_(clock) {}
		Result CloseInputDescriptors(const OperationLease &,
			RecoveryResourceState *state) override
		{
			*state = RecoveryResourceState::neutral;
			clock_.SetNow(3000);
			return MISTER_RESULT_OK;
		}
		Result MuteAudio(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result PowerDownVideo(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result CloseContent(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result DisableCoreProtocol(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
	private:
		FakeClock &clock_;
	};
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	AdvancingRecoveryIo io(clock);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_DEADLINE);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(observation.observed_resource_flags == 0);
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(epoch == nullptr);
}

// A callback admitted before its immutable group deadline may truthfully
// finish at G.  Its call still returns DEADLINE, while the committed partition
// determines whether that deadline becomes durable for the recovery epoch.
void TestGroupDeadlineOverrunSeparatesNeutralFromOutstandingTruth()
{
	const uint64_t group_deadline = 3000;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeTypedSaveRecoveryResource save;
		save.deadline_clock = &clock;
		save.complete_at_deadline = true;
		std::unique_ptr<NativeRecovery> recovery(new NativeRecovery(broker, io,
			resources, resources, resources, save));
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, group_deadline, 6000,
			&epoch) == MISTER_RESULT_OK);
		assert(clock.NowMs() < group_deadline);
		assert(recovery->Perform(*epoch, OperationKind::save) ==
			MISTER_RESULT_DEADLINE);
		assert(clock.NowMs() == group_deadline);
		assert(save.calls == 1 && save.last_deadline == group_deadline);
		assert(save.evidence.callback_count == 1 && save.evidence.file_synced &&
			save.evidence.directory_synced && save.evidence.descriptors_closed &&
			!save.evidence.closure_unknown);

		NativeRetainedOperationSnapshot committed = {};
		assert(recovery->retained_snapshot_for_test(*epoch, OperationKind::save,
			&committed) == MISTER_RESULT_OK);
		assert(committed.query_valid && committed.retained &&
			committed.requested_resource_flags == MISTER_RESOURCE_SAVES &&
			committed.observed_resource_flags == 0 &&
			committed.neutral_resource_flags == MISTER_RESOURCE_SAVES &&
			committed.recovery_result == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery->Snapshot(*epoch, &observation) == MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0 &&
			observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
		NativeRetainedOperationSnapshot stable = {};
		assert(recovery->retained_snapshot_for_test(*epoch, OperationKind::save,
			&stable) == MISTER_RESULT_OK);
		AssertSameSnapshot(committed, stable);

		recovery.reset();
		NativeRecovery observer(broker, io, resources, resources, resources, save);
		assert(observer.Snapshot(*epoch, &observation) == MISTER_RESULT_OK);
		assert(observer.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(epoch == nullptr);
		NativeRetainedOperationSnapshot baseline = {};
		assert(observer.broker_baseline_snapshot_for_test(&baseline) ==
			MISTER_RESULT_OK);
		AssertExactIdleSnapshot(baseline);
	}
	{
		class PartialAtDeadlineIo final : public NativeRecoveryIo {
		public:
			explicit PartialAtDeadlineIo(FakeClock &clock) : clock_(clock) {}
			Result CloseInputDescriptors(const OperationLease &lease,
				RecoveryResourceState *state) override
			{
				++callback_count;
				last_deadline = lease.absolute_deadline_ms();
				*state = RecoveryResourceState::unknown;
				clock_.SetNow(last_deadline);
				return MISTER_RESULT_OK;
			}
			Result MuteAudio(const OperationLease &,
				RecoveryResourceState *) override
			{ ++unexpected_callback_count; return MISTER_RESULT_PLATFORM; }
			Result PowerDownVideo(const OperationLease &,
				RecoveryResourceState *) override
			{ ++unexpected_callback_count; return MISTER_RESULT_PLATFORM; }
			Result CloseContent(const OperationLease &,
				RecoveryResourceState *) override
			{ ++unexpected_callback_count; return MISTER_RESULT_PLATFORM; }
			Result DisableCoreProtocol(const OperationLease &,
				RecoveryResourceState *) override
			{ ++unexpected_callback_count; return MISTER_RESULT_PLATFORM; }
			FakeClock &clock_;
			size_t callback_count = 0;
			size_t unexpected_callback_count = 0;
			uint64_t last_deadline = 0;
		};

		FakeClock clock(1000);
		HardwareBroker broker(clock);
		PartialAtDeadlineIo io(clock);
		NativeRecovery recovery(broker, io);
		const uint32_t requested = MISTER_RESOURCE_CORE_INPUT;
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, group_deadline, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(clock.NowMs() < group_deadline);
		assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
			MISTER_RESULT_DEADLINE);
		assert(clock.NowMs() == group_deadline && io.callback_count == 1 &&
			io.unexpected_callback_count == 0 &&
			io.last_deadline == group_deadline);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Snapshot(*epoch, &observation) == MISTER_RESULT_DEADLINE);
		assert(observation.observed_resource_flags == 0 &&
			observation.neutral_resource_flags == 0);
		assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
			MISTER_RESULT_DEADLINE);
		assert(io.callback_count == 1 && io.unexpected_callback_count == 0);
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_DEADLINE);
		assert(epoch == nullptr);
	}
}

void TestCoreProtocolRecoveryUsesProfilelessSessionAuthority()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::core_protocol)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
		MISTER_RESULT_OK);
	assert(io.core_protocol_session_calls == 1);
	assert(io.last_deadline == 6000);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags ==
		MISTER_RESOURCE_CORE_PROTOCOL);
}

void TestCoreProtocolRecoveryReleaseRetryRetainsItsRecoveryLease()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::core_protocol)] =
		RecoveryResourceState::neutral;
	io.abandon_core_protocol_release_once = true;
	assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
		MISTER_RESULT_PLATFORM);
	assert(io.core_protocol_session_calls == 1);
	const uint64_t original_deadline = io.last_deadline;
	NativeRetainedOperationSnapshot retained = {};
	assert(recovery.retained_snapshot_for_test(*epoch,
		OperationKind::core_protocol, &retained) == MISTER_RESULT_OK);
	assert(retained.query_valid && retained.retained);
	assert(retained.authority == LeaseAuthority::recovery_epoch);
	assert(retained.registration_is_suspended && !retained.registration_is_invoked);
	assert(retained.registration_effective_deadline_ms == 0);
	assert(retained.registration_authority_deadline_ms == original_deadline);
	assert(retained.protocol_phase == ProtocolSessionHandleState::abandoned);
	assert(retained.typed_session_present && retained.backend_applicable);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_INVALID_STATE);
	assert(epoch != nullptr);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
		MISTER_RESULT_OK);
	assert(io.core_protocol_session_calls == 2);
	assert(io.last_deadline == original_deadline);
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
}

void TestCoreSnapshotTracksLiveSuspendAndRebind()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::core_protocol)] = RecoveryResourceState::neutral;
	io.snapshot_recovery = &recovery;
	io.snapshot_epoch = epoch.get();
	io.abandon_core_protocol_release_once = true;
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &first) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::core_protocol) ==
		MISTER_RESULT_PLATFORM);
	assert(io.live_snapshot_captured);
	const NativeRetainedOperationSnapshot callback_entry = io.live_snapshot;
	assert(callback_entry.registration_is_invoked && callback_entry.core_disposition ==
		CoreProtocolBrokerDisposition::session_current);
	NativeRetainedOperationSnapshot invoked_outcome = {};
	assert(recovery.retained_snapshot_for_test(*epoch,
		OperationKind::core_protocol, &invoked_outcome) == MISTER_RESULT_OK);
	assert(invoked_outcome.protocol_phase == ProtocolSessionHandleState::abandoned);
	assert(invoked_outcome.core_disposition ==
		CoreProtocolBrokerDisposition::session_abandoned);
	assert(!invoked_outcome.action_applicable &&
		invoked_outcome.action_word_count == 0 &&
		invoked_outcome.action_next_word_index == 0);
	assert(invoked_outcome.session_initial_mutation_sequence == 0 &&
		invoked_outcome.session_last_mutation_sequence == 0 &&
		invoked_outcome.broker_mutation_sequence == 0);
	assert(io.core_mapping_count == 1 && io.core_descriptor_count == 2);
	assert(io.core_release_attempts == 1 && io.core_release_successes == 0);
	assert(!io.core_selected_transaction_closed);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot suspended = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::core_protocol,
		&suspended) == MISTER_RESULT_OK);
	assert(suspended.registration_is_suspended && suspended.core_disposition ==
		CoreProtocolBrokerDisposition::session_abandoned &&
		suspended.protocol_phase == ProtocolSessionHandleState::abandoned);
	assert(suspended.lease_identity == callback_entry.lease_identity &&
		suspended.registration_identity == callback_entry.registration_identity &&
		suspended.session_identity == callback_entry.session_identity &&
		suspended.backend_identity == callback_entry.backend_identity);
	AssertOnlyInvocationSuspensionChanged(invoked_outcome, suspended);
	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::core_protocol) ==
		MISTER_RESULT_OK);
	const NativeRetainedOperationSnapshot rebound = io.live_snapshot;
	assert(rebound.registration_is_invoked && rebound.invocation_identity !=
		callback_entry.invocation_identity && rebound.lease_identity ==
		callback_entry.lease_identity && rebound.registration_identity ==
		callback_entry.registration_identity && rebound.session_identity ==
		callback_entry.session_identity && rebound.backend_identity ==
		callback_entry.backend_identity);
	AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
	assert(rebound.session_initial_mutation_sequence == 0 &&
		rebound.session_last_mutation_sequence == 0 &&
		rebound.broker_mutation_sequence == 0);
	assert(io.core_mapping_count == 0 && io.core_descriptor_count == 0);
	assert(io.core_release_attempts == 2 && io.core_release_successes == 1);
	assert(io.core_selected_transaction_closed);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot destroyed = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::core_protocol,
		&destroyed) == MISTER_RESULT_OK);
	assert(destroyed.query_valid && !destroyed.retained);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot baseline = {};
	assert(recovery.broker_baseline_snapshot_for_test(&baseline) ==
		MISTER_RESULT_OK);
	AssertExactIdleSnapshot(baseline);
}

void TestTypedCoupledRecoverySnapshotsVideoNeutralWhileAudioRemainsUnknown()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, containment);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	assert(resources.coupled_calls == 1);
	MisterRecoveryObservationV2 snapshot = Observation();
	assert(recovery.Snapshot(*epoch, &snapshot) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(snapshot.observed_resource_flags == 0);
	assert(snapshot.neutral_resource_flags == MISTER_RESOURCE_NATIVE_VIDEO);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_NATIVE_VIDEO) == MISTER_RESULT_INVALID_STATE);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, requested) ==
		MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_NATIVE_AUDIO) == MISTER_RESULT_INVALID_STATE);
	MisterRecoveryObservationV2 finish = Observation();
	assert(recovery.Finish(std::move(epoch), &finish) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
}

// Breaks if a live/abandoned peripheral session reports the raw no_session
// completion enum, or if an invocation rebind changes the retained identity.
void TestAudioRetainedSnapshotTracksLiveSuspendAndRebind()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, containment);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.snapshot_recovery = &recovery;
	resources.snapshot_epoch = epoch.get();
	resources.deadline_clock = &clock;
	resources.deadline_once_kind = OperationKind::audio;
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::audio) ==
		MISTER_RESULT_DEADLINE);
	assert(resources.live_snapshot_captured);
	const NativeRetainedOperationSnapshot callback_entry = resources.live_snapshot;
	assert(callback_entry.query_valid && callback_entry.retained &&
		callback_entry.registration_is_invoked);
	assert(callback_entry.peripheral_disposition ==
		PeripheralBrokerDisposition::live);
	assert(callback_entry.peripheral_phase == PeripheralSessionPhase::live);
	assert(callback_entry.backend_identity != 0 &&
		callback_entry.backend_matches_expected);
	NativeRetainedOperationSnapshot invoked_outcome = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::audio,
		&invoked_outcome) == MISTER_RESULT_OK);
	const PeripheralResourceEvidence failed_evidence =
		resources.Evidence(OperationKind::audio);
	assert(failed_evidence.callback_count == 1);
	assert(failed_evidence.mutation_count == 0);
	assert(failed_evidence.final_ack_count == 0);
	assert(failed_evidence.final_ack == 0);
	assert(failed_evidence.transaction_closed);
	assert(failed_evidence.local_resources_absent);
	assert(!failed_evidence.closure_unknown);
	AssertPeripheralSnapshotPrefixMatchesEvidence(invoked_outcome,
		failed_evidence);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot suspended = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::audio,
		&suspended) == MISTER_RESULT_OK);
	assert(suspended.registration_is_suspended &&
		suspended.peripheral_disposition == PeripheralBrokerDisposition::abandoned &&
		suspended.peripheral_phase == PeripheralSessionPhase::abandoned);
	assert(suspended.lease_identity == callback_entry.lease_identity &&
		suspended.registration_identity == callback_entry.registration_identity &&
		suspended.session_identity == callback_entry.session_identity &&
		suspended.backend_identity == callback_entry.backend_identity);
	AssertOnlyInvocationSuspensionChanged(invoked_outcome, suspended);
	AssertPeripheralSnapshotPrefixMatchesEvidence(suspended, failed_evidence);
	AssertSamePeripheralEvidence(failed_evidence,
		resources.Evidence(OperationKind::audio));

	clock.SetNow(1000);
	resources.live_snapshot_captured = false;
	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::audio) ==
		MISTER_RESULT_OK);
	const NativeRetainedOperationSnapshot rebound = resources.live_snapshot;
	assert(resources.live_snapshot_captured && rebound.registration_is_invoked);
	assert(rebound.invocation_identity != callback_entry.invocation_identity);
	assert(rebound.registration_effective_deadline_ms == 1800);
	assert(rebound.lease_identity == callback_entry.lease_identity &&
		rebound.registration_identity == callback_entry.registration_identity &&
		rebound.session_identity == callback_entry.session_identity &&
		rebound.backend_identity == callback_entry.backend_identity);
	AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
	AssertPeripheralSnapshotPrefixMatchesEvidence(rebound, failed_evidence);
	const PeripheralResourceEvidence success_evidence =
		resources.Evidence(OperationKind::audio);
	assert(success_evidence.callback_count == 2);
	assert(success_evidence.mutation_count == 1);
	assert(success_evidence.final_ack_count == 1);
	assert(success_evidence.final_ack == 0xa55a);
	assert(success_evidence.transaction_closed &&
		success_evidence.local_resources_absent &&
		!success_evidence.closure_unknown);
	assert(success_evidence.accepted_mutation_sequence ==
		failed_evidence.accepted_mutation_sequence + 1);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot destroyed = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::audio,
		&destroyed) == MISTER_RESULT_OK);
	assert(destroyed.query_valid && !destroyed.retained &&
		destroyed.lease_identity == 0 && destroyed.registration_identity == 0);

	std::unique_ptr<OperationInvocation> terminal;
	assert(broker.BeginRecoveryInvocation(*epoch, 5800, &terminal) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *terminal, OperationKind::terminal_fpga_cleanup) ==
		MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(terminal)) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot baseline = {};
	assert(recovery.broker_baseline_snapshot_for_test(&baseline) ==
		MISTER_RESULT_OK);
	AssertExactIdleSnapshot(baseline);
}

void TestCoupledSnapshotDestructionLeavesExactIdleBaseline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, containment);
	std::unique_ptr<RecoveryEpoch> epoch;
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.snapshot_recovery = &recovery;
	resources.snapshot_epoch = epoch.get();
	std::unique_ptr<OperationInvocation> invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &invocation) ==
		MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot rejected = {};
	assert(recovery.broker_baseline_snapshot_for_test(&rejected) ==
		MISTER_RESULT_INVALID_STATE);
	assert(!rejected.query_valid);
	assert(recovery.Perform(*epoch, *invocation, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	assert(resources.live_snapshot_captured);
	assert(resources.live_snapshot.peripheral_disposition ==
		PeripheralBrokerDisposition::live);
	assert(resources.live_snapshot.peripheral_phase == PeripheralSessionPhase::live);
	assert(broker.FinishInvocation(std::move(invocation)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot destroyed = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::audio_video,
		&destroyed) == MISTER_RESULT_OK);
	assert(destroyed.query_valid && !destroyed.retained &&
		destroyed.active_lease_count == 0 && !destroyed.invocation_registered);
	assert(recovery.broker_baseline_snapshot_for_test(&rejected) ==
		MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<OperationInvocation> audio;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &audio) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *audio, OperationKind::audio) == MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(audio)) == MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> terminal;
	assert(broker.BeginRecoveryInvocation(*epoch, 5800, &terminal) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *terminal, OperationKind::terminal_fpga_cleanup) ==
		MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(terminal)) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot baseline = {};
	assert(recovery.broker_baseline_snapshot_for_test(&baseline) == MISTER_RESULT_OK);
	assert(baseline.query_valid && baseline.broker_idle &&
		baseline.active_lease_count == 0 && baseline.terminal_lease_count == 0 &&
		baseline.authority_identity == 0 && baseline.requested_resource_flags == 0 &&
		baseline.non_fpga_deadline_ms == 0 && baseline.fpga_deadline_ms == 0 &&
		baseline.broker_mutation_sequence == 0 && !baseline.invocation_registered &&
		!baseline.typed_registration_present && !baseline.typed_session_present &&
		!baseline.cleanup_registered && !baseline.recovery_registered &&
		!baseline.recovery_observation_active && !baseline.terminal_neutral &&
		!baseline.containment_receipt_current &&
		!baseline.containment_evidence_pending && !baseline.hardware_transaction_active);
}

void TestVideoRetainedSnapshotTracksLiveSuspendAndRebind()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_VIDEO, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	resources.snapshot_recovery = &recovery;
	resources.snapshot_epoch = epoch.get();
	resources.deadline_clock = &clock;
	resources.deadline_once_kind = OperationKind::video;
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::video) ==
		MISTER_RESULT_DEADLINE);
	const NativeRetainedOperationSnapshot callback_entry = resources.live_snapshot;
	assert(resources.live_snapshot_captured &&
		callback_entry.registration_is_invoked &&
		callback_entry.peripheral_disposition ==
			PeripheralBrokerDisposition::live);
	NativeRetainedOperationSnapshot invoked_outcome = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::video,
		&invoked_outcome) == MISTER_RESULT_OK);
	const PeripheralResourceEvidence failed_evidence =
		resources.Evidence(OperationKind::video);
	assert(failed_evidence.callback_count == 1 &&
		failed_evidence.mutation_count == 0 &&
		failed_evidence.final_ack_count == 0 &&
		failed_evidence.transaction_closed &&
		failed_evidence.local_resources_absent &&
		!failed_evidence.closure_unknown);
	AssertPeripheralSnapshotPrefixMatchesEvidence(invoked_outcome,
		failed_evidence);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot suspended = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::video,
		&suspended) == MISTER_RESULT_OK);
	assert(suspended.registration_is_suspended && suspended.peripheral_disposition ==
		PeripheralBrokerDisposition::abandoned && suspended.lease_identity ==
		callback_entry.lease_identity && suspended.backend_identity ==
		callback_entry.backend_identity);
	AssertOnlyInvocationSuspensionChanged(invoked_outcome, suspended);
	AssertPeripheralSnapshotPrefixMatchesEvidence(suspended, failed_evidence);
	AssertSamePeripheralEvidence(failed_evidence,
		resources.Evidence(OperationKind::video));
	clock.SetNow(1000);
	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::video) == MISTER_RESULT_OK);
	const NativeRetainedOperationSnapshot rebound = resources.live_snapshot;
	assert(rebound.registration_is_invoked && rebound.invocation_identity !=
		callback_entry.invocation_identity && rebound.lease_identity ==
		callback_entry.lease_identity && rebound.session_identity ==
		callback_entry.session_identity);
	AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
	AssertPeripheralSnapshotPrefixMatchesEvidence(rebound, failed_evidence);
	const PeripheralResourceEvidence success_evidence =
		resources.Evidence(OperationKind::video);
	assert(success_evidence.callback_count == 2);
	assert(success_evidence.mutation_count == 0);
	assert(success_evidence.final_ack_count == 1);
	assert(success_evidence.final_ack == 0xa55a);
	assert(success_evidence.transaction_closed &&
		success_evidence.local_resources_absent &&
		!success_evidence.closure_unknown);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot destroyed = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::video,
		&destroyed) == MISTER_RESULT_OK && !destroyed.retained);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot baseline = {};
	assert(recovery.broker_baseline_snapshot_for_test(&baseline) ==
		MISTER_RESULT_OK);
	AssertExactIdleSnapshot(baseline);
}

void TestCoupledRetainedSnapshotTracksLiveSuspendAndRebind()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	resources.abandon_once = true;
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, containment);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	resources.snapshot_recovery = &recovery;
	resources.snapshot_epoch = epoch.get();
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &first) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	const NativeRetainedOperationSnapshot callback_entry = resources.live_snapshot;
	assert(resources.live_snapshot_captured &&
		callback_entry.registration_is_invoked &&
		callback_entry.peripheral_disposition ==
			PeripheralBrokerDisposition::live);
	NativeRetainedOperationSnapshot invoked_outcome = {};
	assert(recovery.retained_snapshot_for_test(*epoch,
		OperationKind::audio_video, &invoked_outcome) == MISTER_RESULT_OK);
	const PeripheralResourceEvidence failed_evidence =
		resources.Evidence(OperationKind::audio_video);
	assert(failed_evidence.callback_count == 1);
	assert(failed_evidence.mutation_count == 1);
	assert(failed_evidence.final_ack_count == 0);
	assert(failed_evidence.final_ack == 0xa55a);
	assert(!failed_evidence.transaction_closed);
	assert(!failed_evidence.local_resources_absent);
	assert(!failed_evidence.closure_unknown);
	AssertPeripheralSnapshotPrefixMatchesEvidence(invoked_outcome,
		failed_evidence);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot suspended = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::audio_video,
		&suspended) == MISTER_RESULT_OK);
	assert(suspended.registration_is_suspended && suspended.peripheral_disposition ==
		PeripheralBrokerDisposition::abandoned);
	assert(suspended.session_last_mutation_sequence ==
		failed_evidence.accepted_mutation_sequence);
	assert(suspended.broker_mutation_sequence ==
		failed_evidence.accepted_mutation_sequence);
	AssertOnlyInvocationSuspensionChanged(invoked_outcome, suspended);
	AssertPeripheralSnapshotPrefixMatchesEvidence(suspended, failed_evidence);
	AssertSamePeripheralEvidence(failed_evidence,
		resources.Evidence(OperationKind::audio_video));
	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	const NativeRetainedOperationSnapshot rebound = resources.live_snapshot;
	assert(rebound.registration_is_invoked && rebound.invocation_identity !=
		callback_entry.invocation_identity && rebound.lease_identity ==
		callback_entry.lease_identity && rebound.registration_identity ==
		callback_entry.registration_identity && rebound.session_identity ==
		callback_entry.session_identity);
	AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
	AssertPeripheralSnapshotPrefixMatchesEvidence(rebound, failed_evidence);
	const PeripheralResourceEvidence success_evidence =
		resources.Evidence(OperationKind::audio_video);
	assert(success_evidence.callback_count == 2);
	assert(success_evidence.mutation_count == 2);
	assert(success_evidence.final_ack_count == 1);
	assert(success_evidence.final_ack == 0xa55a);
	assert(success_evidence.accepted_mutation_sequence ==
		failed_evidence.accepted_mutation_sequence + 1);
	assert(success_evidence.transaction_closed &&
		success_evidence.local_resources_absent &&
		!success_evidence.closure_unknown);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot destroyed = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::audio_video,
		&destroyed) == MISTER_RESULT_OK && !destroyed.retained);
	std::unique_ptr<OperationInvocation> audio;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &audio) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *audio, OperationKind::audio) == MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(audio)) == MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> terminal;
	assert(broker.BeginRecoveryInvocation(*epoch, 5800, &terminal) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *terminal, OperationKind::terminal_fpga_cleanup) ==
		MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(terminal)) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot baseline = {};
	assert(recovery.broker_baseline_snapshot_for_test(&baseline) ==
		MISTER_RESULT_OK);
	AssertExactIdleSnapshot(baseline);
}

void TestTypedSaveRecoveryRetainsOneTaggedLeaseAndOriginalDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, save,
		containment);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	save.fail_once = true;
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_PLATFORM);
	assert(save.calls == 1);
	assert(save.last_deadline == 3000);
	NativeRetainedOperationSnapshot retained = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&retained) == MISTER_RESULT_OK);
	assert(retained.query_valid && retained.retained);
	assert(retained.typed_registration_present && !retained.typed_session_present);
	assert(retained.backend_applicable && retained.backend_matches_expected);
	assert(retained.registration_is_suspended &&
		retained.registration_effective_deadline_ms == 0);
	assert(retained.registration_authority_deadline_ms == 3000);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_INVALID_STATE);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(save.calls == 2);
	assert(save.last_deadline == 3000);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_SAVES) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, 0) ==
		MISTER_RESULT_INVALID_STATE);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
}

void TestTaggedSaveRecoveryUsesTheFreshNativeSaveAdapterAndExactRecord()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	filesystem.FailFdatasyncOnce();
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_PLATFORM);
	MisterRecoveryObservationV2 incomplete = Observation();
	assert(recovery.Finish(std::move(epoch), &incomplete) == MISTER_RESULT_INVALID_STATE);
	assert(epoch != nullptr);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_INVALID_STATE);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(filesystem.calls() != 0);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_SAVES) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
}

void TestSaveRecoveryRejectsMissingForgedAndExpiredSafeAuthorityBeforeIo()
{
	const SafeSaveRecoveryRecord *const exact =
		FixtureSafeSaveRecoveryRecordForTest(NativeSystem::snes);
	assert(exact != nullptr);
	SafeSaveRecoveryRecord forged = *exact;
	const SafeSaveRecoveryRecord *const records[] = {nullptr, &forged};
	for (const SafeSaveRecoveryRecord *record : records) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		io.save_record = record;
		FakeTypedRecoveryResources resources(broker);
		RecoverySaveFileSystem filesystem;
		linux_native::NativeSaveAdapter save(broker, filesystem);
		NativeRecovery recovery(broker, io, resources, resources, resources, save);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::save) ==
			MISTER_RESULT_UNSUPPORTED);
		assert(filesystem.calls() == 0);
	}

	FakeClock clock(3000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_DEADLINE);
	assert(!epoch);
	assert(filesystem.calls() == 0);
}

void TestRealSaveRecoveryReducesItsOutstandingMaskAndStillExcludesFinish()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	const uint32_t requested = MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		requested) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_CONTENT) == MISTER_RESULT_INVALID_STATE);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, 0) ==
		MISTER_RESULT_INVALID_STATE);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
}

void TestTypedCoupledRecoveryRetainsItsExactRegistrationForRetry()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources,
		containment);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.abandon_once = true;
	resources.coupled_all_neutral = true;
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(resources.coupled_calls == 1);
	const uint64_t original_deadline = resources.last_deadline;
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_INVALID_STATE);
	assert(epoch != nullptr);
	assert(recovery.Perform(*epoch, OperationKind::video) ==
		MISTER_RESULT_INVALID_STATE);
	assert(resources.coupled_calls == 1);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	assert(resources.coupled_calls == 2);
	assert(resources.last_deadline == original_deadline);
	assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
		MISTER_RESULT_OK);
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == requested);
}

void TestCoupledRecoveryClosureUnknownBlocksRawRetry()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.abandon_once = true;
	resources.closure_unknown_once = true;
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(resources.coupled_calls == 1);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(resources.coupled_calls == 1);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) == MISTER_RESULT_PLATFORM);
}

void TestCoupledRecoveryCannotBorrowTheLaterFpgaDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	clock.SetNow(3000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_DEADLINE);
	assert(resources.coupled_calls == 0);
	MisterRecoveryObservationV2 snapshot = Observation();
	assert(recovery.Snapshot(*epoch, &snapshot) == MISTER_RESULT_DEADLINE);
	assert(snapshot.neutral_resource_flags == 0);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_DEADLINE);
	assert(resources.coupled_calls == 0);
}

void TestForeignTypedRecoveryBackendCannotAdoptARetainedSession()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources owner_resources(broker);
	FakeTypedRecoveryResources foreign_resources(broker);
	NativeRecovery owner(broker, io, owner_resources, owner_resources,
		owner_resources);
	NativeRecovery foreign(broker, io, foreign_resources, foreign_resources,
		foreign_resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	owner_resources.abandon_once = true;
	assert(owner.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(owner_resources.coupled_calls == 1);
	assert(foreign.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_INVALID_STATE);
	assert(foreign_resources.coupled_calls == 0);
}

void TestDestroyingRetainedRecoveryPermitsAuthorizedSameEpochRetry()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.abandon_once = true;
	{
		NativeRecovery recovery(broker, io, resources, resources, resources);
		assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
			MISTER_RESULT_PLATFORM);
	}
	NativeRecovery after_destruction(broker, io, resources, resources,
		resources);
	assert(after_destruction.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	assert(resources.coupled_calls == 2);
	MisterRecoveryObservationV2 observation = Observation();
	assert(after_destruction.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
	epoch.reset();
	assert(broker.exact_idle_for_test());
}

void TestDestroyingAbandonedTypedRecoveryBreaksItsRetainedOwnerCycle()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	struct Case {
		OperationKind kind;
		uint32_t requested;
		bool closure_unknown;
	};
	const Case cases[] = {
		{OperationKind::audio, closure | MISTER_RESOURCE_NATIVE_AUDIO, false},
		{OperationKind::video, MISTER_RESOURCE_NATIVE_VIDEO, false},
		{OperationKind::audio_video, closure | MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO, false},
		{OperationKind::audio_video, closure | MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO, true},
		{OperationKind::core_protocol, MISTER_RESOURCE_CORE_PROTOCOL, false}
	};
	for (size_t cycle = 0; cycle != 200; ++cycle) {
		for (const Case &test : cases) {
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeRecoveryIo io(broker);
			FakeTypedRecoveryResources resources(broker);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(test.requested, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			if (test.kind == OperationKind::core_protocol) {
				io.abandon_core_protocol_release_once = true;
			} else if (test.kind == OperationKind::audio_video) {
				resources.abandon_once = true;
				resources.closure_unknown_once = test.closure_unknown;
			} else {
				resources.deadline_clock = &clock;
				resources.deadline_once_kind = test.kind;
				resources.deadline_to_expire = 3000;
			}
			HardwareBroker::OwnerDestructionSnapshot retained = {};
			{
				NativeRecovery recovery(broker, io, resources, resources, resources);
				assert(recovery.Perform(*epoch, test.kind) != MISTER_RESULT_OK);
				retained = broker.owner_destruction_snapshot_for_test();
				assert(retained.active_lease_count == 1);
				assert(retained.terminal_lease_count == 0);
				assert(!retained.invocation_registered);
				assert(retained.suspended_registration_present);
				assert(retained.hardware_transaction_active);
				assert(retained.registration_identity != 0);
				assert(retained.session_identity != 0);
				assert(retained.effective_deadline_ms == 0);
				assert(retained.operation_kind == test.kind);
				assert(retained.session_view_names_registration);
				assert(!retained.session_progress_unknown);
				assert(retained.session_last_mutation_sequence ==
					retained.mutation_sequence);
				assert(retained.core_session_present ==
					(test.kind == OperationKind::core_protocol));
				assert(retained.peripheral_session_present ==
					(test.kind != OperationKind::core_protocol));
			}
			const HardwareBroker::OwnerDestructionSnapshot destroyed =
				broker.owner_destruction_snapshot_for_test();
			assert(destroyed.recovery_identity == retained.recovery_identity);
			assert(destroyed.active_lease_count == 0);
			assert(destroyed.terminal_lease_count == 0);
			assert(!destroyed.suspended_registration_present);
			assert(!destroyed.core_session_present);
			assert(!destroyed.peripheral_session_present);
			assert(destroyed.registration_identity == 0);
			assert(destroyed.session_identity == 0);
			assert(!destroyed.session_view_names_registration);
			assert(!destroyed.hardware_transaction_active);
			assert(destroyed.mutation_sequence == retained.mutation_sequence);
			assert(destroyed.destruction_failure_count == 0);
			assert(resources.audio_calls + resources.video_calls +
				resources.coupled_calls == (test.kind == OperationKind::audio ? 1 :
					test.kind == OperationKind::video ? 1 :
					test.kind == OperationKind::audio_video ? 1 : 0));
			const RecoveryResourceTrace resource_after_destruction =
				CaptureRecoveryResourceTrace(io, resources);
			clock.SetNow(test.kind == OperationKind::core_protocol ? 6000 : 3000);
			std::unique_ptr<OperationLease> expired_retry;
			assert(broker.BeginRecoveryOperation(*epoch, test.kind,
				&expired_retry) == MISTER_RESULT_DEADLINE);
			assert(expired_retry == nullptr);
			MisterRecoveryObservationV2 observation = Observation();
			NativeRecovery observer(broker, io, resources, resources, resources);
			assert(observer.Snapshot(*epoch, &observation) ==
				(test.closure_unknown ? MISTER_RESULT_PLATFORM :
					MISTER_RESULT_DEADLINE));
			const Result finish = broker.FinishRecovery(std::move(epoch), &observation);
			assert(finish != MISTER_RESULT_INVALID_STATE);
			assert(finish == (test.closure_unknown ? MISTER_RESULT_PLATFORM :
				MISTER_RESULT_DEADLINE));
			assert(epoch == nullptr);
			AssertSameRecoveryResourceTrace(resource_after_destruction,
				CaptureRecoveryResourceTrace(io, resources));
			assert(broker.exact_idle_for_test());
			std::unique_ptr<RecoveryEpoch> fresh;
			assert(broker.BeginRecovery(0, 7000, 10000, &fresh) == MISTER_RESULT_OK);
			assert(broker.FinishRecovery(std::move(fresh), &observation) ==
				MISTER_RESULT_OK);
		}
	}
}

void TestCallbackExpiredOwnerDestructionDoesNotFenceSameEpochAdmission()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	struct Case { OperationKind kind; uint32_t requested; };
	const Case cases[] = {
		{OperationKind::audio, closure | MISTER_RESOURCE_NATIVE_AUDIO},
		{OperationKind::video, MISTER_RESOURCE_NATIVE_VIDEO},
		{OperationKind::audio_video, closure | MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO},
		{OperationKind::core_protocol, MISTER_RESOURCE_CORE_PROTOCOL}
	};
	for (const Case &test : cases) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(test.requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		resources.deadline_clock = &clock;
		resources.deadline_once_kind = test.kind;
		resources.deadline_to_expire = 1500;
		if (test.kind == OperationKind::core_protocol)
			io.deadline_core_once = true;
		std::unique_ptr<OperationInvocation> invocation;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &invocation) ==
			MISTER_RESULT_OK);
		{
			NativeRecovery recovery(broker, io, resources, resources, resources);
			assert(recovery.Perform(*epoch, *invocation, test.kind) !=
				MISTER_RESULT_OK);
			assert(broker.FinishInvocation(std::move(invocation)) == MISTER_RESULT_OK);
		}
		MisterRecoveryObservationV2 observation = Observation();
		NativeRecovery observer(broker, io, resources, resources, resources);
		assert(observer.Snapshot(*epoch, &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(observation.neutral_resource_flags == 0);
		assert(broker.retained_owner_destruction_failures_for_test() == 0);
		assert(observer.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
		const RecoveryResourceTrace resource_before_retry_admission =
			CaptureRecoveryResourceTrace(io, resources);
		std::unique_ptr<OperationLease> same_epoch_retry;
		assert(broker.BeginRecoveryOperation(*epoch, test.kind,
			&same_epoch_retry) == MISTER_RESULT_OK);
		same_epoch_retry.reset();
		std::unique_ptr<OperationInvocation> retry_invocation;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &retry_invocation) ==
			MISTER_RESULT_OK);
		assert(broker.BeginRecoveryOperation(*epoch, *retry_invocation, test.kind,
			&same_epoch_retry) == MISTER_RESULT_OK);
		assert(broker.RecordOperationOutcome(*retry_invocation, *same_epoch_retry,
			MISTER_RESULT_CLEANUP_INCOMPLETE) == MISTER_RESULT_OK);
		same_epoch_retry.reset();
		assert(broker.FinishInvocation(std::move(retry_invocation)) ==
			MISTER_RESULT_OK);
		AssertSameRecoveryResourceTrace(resource_before_retry_admission,
			CaptureRecoveryResourceTrace(io, resources));
		epoch.reset();
		assert(broker.exact_idle_for_test());
		std::unique_ptr<RecoveryEpoch> fresh;
		assert(broker.BeginRecovery(0, 4000, 7000, &fresh) == MISTER_RESULT_OK);
		assert(broker.FinishRecovery(std::move(fresh), &observation) ==
			MISTER_RESULT_OK);
	}
}

void TestRejectedInvokedOwnerDestructionIsObservableAndNeverIdle()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_AUDIO, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	resources.deadline_clock = &clock;
	resources.deadline_once_kind = OperationKind::audio;
	std::unique_ptr<OperationInvocation> invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 2000, &invocation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<NativeRecovery> recovery(
		new NativeRecovery(broker, io, resources, resources, resources));
	assert(recovery->Perform(*epoch, *invocation, OperationKind::audio) !=
		MISTER_RESULT_OK);
	const uint64_t mutations = broker.mutation_sequence_for_test();
	const RecoveryResourceTrace resource_before =
		CaptureRecoveryResourceTrace(io, resources);
	recovery.reset();
	assert(broker.retained_owner_destruction_failures_for_test() == 1);
	assert(!broker.exact_idle_for_test());
	assert(broker.mutation_sequence_for_test() == mutations);
	AssertSameRecoveryResourceTrace(resource_before,
		CaptureRecoveryResourceTrace(io, resources));
	invocation.reset();
}

void TestCycleBreakRejectsForeignLiveAndTerminalLeasesWithoutMutation()
{
	const OperationKind kinds[] = {OperationKind::audio,
		OperationKind::terminal_fpga_cleanup};
	for (OperationKind kind : kinds) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		HardwareBroker foreign(clock);
		FakeRecoveryIo foreign_io(foreign);
		FakeTypedRecoveryResources foreign_resources(foreign);
		std::unique_ptr<RecoveryEpoch> foreign_epoch;
		assert(foreign.BeginRecovery(MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO, 3000, 6000, &foreign_epoch) ==
			MISTER_RESULT_OK);
		foreign_resources.abandon_once = true;
		NativeRecovery foreign_owner(foreign, foreign_io, foreign_resources,
			foreign_resources, foreign_resources);
		assert(foreign_owner.Perform(*foreign_epoch, OperationKind::audio_video) ==
			MISTER_RESULT_PLATFORM);
		const HardwareBroker::OwnerDestructionSnapshot foreign_before =
			foreign.owner_destruction_snapshot_for_test();
		std::unique_ptr<RecoveryEpoch> epoch;
		const uint32_t requested = kind == OperationKind::audio ?
			MISTER_RESOURCE_NATIVE_AUDIO : (MISTER_RESOURCE_FPGA |
				MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL);
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, kind, &lease) ==
			MISTER_RESULT_OK);
		const HardwareBroker::OwnerDestructionSnapshot before =
			broker.owner_destruction_snapshot_for_test();
		const RecoveryResourceTrace resource_before =
			CaptureRecoveryResourceTrace(io, resources);
		const RecoveryResourceTrace foreign_resource_before =
			CaptureRecoveryResourceTrace(foreign_io, foreign_resources);
		assert(broker.BreakRetainedTypedOwnerCycleForTest(*lease) ==
			MISTER_RESULT_INVALID_STATE);
		assert(foreign.BreakRetainedTypedOwnerCycleForTest(*lease) ==
			MISTER_RESULT_INVALID_STATE);
		const HardwareBroker::OwnerDestructionSnapshot after =
			broker.owner_destruction_snapshot_for_test();
		AssertSameOwnerDestructionSnapshot(before, after);
		AssertSameOwnerDestructionSnapshot(foreign_before,
			foreign.owner_destruction_snapshot_for_test());
		AssertSameRecoveryResourceTrace(resource_before,
			CaptureRecoveryResourceTrace(io, resources));
		AssertSameRecoveryResourceTrace(foreign_resource_before,
			CaptureRecoveryResourceTrace(foreign_io, foreign_resources));
		lease.reset();
		epoch.reset();
		assert(broker.exact_idle_for_test());
		assert(foreign.BreakRetainedTypedOwnerCycleForTest(
			*foreign_owner.retained_lease_for_test()) == MISTER_RESULT_OK);
		foreign_owner.clear_retained_recovery_for_test();
		foreign_epoch.reset();
		assert(foreign.exact_idle_for_test());
	}
}

void TestCycleBreakRejectsAnAlreadyConsumedExactLeaseWithoutMutation()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	resources.abandon_once = true;
	NativeRecovery recovery(broker, io, resources, resources, resources);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	OperationLease *const lease = recovery.retained_lease_for_test();
	assert(lease != nullptr);
	assert(broker.BreakRetainedTypedOwnerCycleForTest(*lease) == MISTER_RESULT_OK);
	const HardwareBroker::OwnerDestructionSnapshot before =
		broker.owner_destruction_snapshot_for_test();
	const RecoveryResourceTrace resource_before =
		CaptureRecoveryResourceTrace(io, resources);
	assert(broker.BreakRetainedTypedOwnerCycleForTest(*lease) ==
		MISTER_RESULT_INVALID_STATE);
	const HardwareBroker::OwnerDestructionSnapshot after =
		broker.owner_destruction_snapshot_for_test();
	AssertSameOwnerDestructionSnapshot(before, after);
	AssertSameRecoveryResourceTrace(resource_before,
		CaptureRecoveryResourceTrace(io, resources));
	recovery.clear_retained_recovery_for_test();
	epoch.reset();
	assert(broker.exact_idle_for_test());
}

void TestCycleBreakRejectsMalformedTypedStateWithoutMutation()
{
	const HardwareBroker::OwnerDestructionTestFault faults[] = {
		HardwareBroker::OwnerDestructionTestFault::wrong_kind,
		HardwareBroker::OwnerDestructionTestFault::wrong_epoch,
		HardwareBroker::OwnerDestructionTestFault::live_session,
		HardwareBroker::OwnerDestructionTestFault::finalized_session,
		HardwareBroker::OwnerDestructionTestFault::missing_view_edge,
		HardwareBroker::OwnerDestructionTestFault::foreign_session,
		HardwareBroker::OwnerDestructionTestFault::containment_pending
	};
	for (HardwareBroker::OwnerDestructionTestFault fault : faults) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO, 3000, 6000, &epoch) == MISTER_RESULT_OK);
		resources.abandon_once = true;
		NativeRecovery recovery(broker, io, resources, resources, resources);
		assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
			MISTER_RESULT_PLATFORM);
		OperationLease *const lease = recovery.retained_lease_for_test();
		const HardwareBroker::OwnerDestructionSnapshot before =
			broker.owner_destruction_snapshot_for_test();
		const RecoveryResourceTrace resource_before =
			CaptureRecoveryResourceTrace(io, resources);
		assert(broker.BreakRetainedTypedOwnerCycleForTest(*lease, fault) ==
			MISTER_RESULT_INVALID_STATE);
		const HardwareBroker::OwnerDestructionSnapshot after =
			broker.owner_destruction_snapshot_for_test();
		AssertSameOwnerDestructionSnapshot(before, after);
		AssertSameRecoveryResourceTrace(resource_before,
			CaptureRecoveryResourceTrace(io, resources));
	}
}

void TestSaveOwnerDestructionUsesTheNormalLeaseReleaseControl()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	save.fail_once = true;
	{
		NativeRecovery recovery(broker, io, resources, resources, resources, save);
		assert(recovery.Perform(*epoch, OperationKind::save) ==
			MISTER_RESULT_PLATFORM);
		assert(save.calls == 1);
	}
	assert(broker.retained_owner_destruction_failures_for_test() == 0);
	epoch.reset();
	assert(broker.exact_idle_for_test());
}

void TestCycleBreakRejectsRetainedSaveControlWithoutMutation()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	save.fail_once = true;
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_PLATFORM);
	OperationLease *const lease = recovery.retained_lease_for_test();
	assert(lease != nullptr);
	const HardwareBroker::OwnerDestructionSnapshot before =
		broker.owner_destruction_snapshot_for_test();
	const int save_calls = save.calls;
	assert(broker.BreakRetainedTypedOwnerCycleForTest(*lease) ==
		MISTER_RESULT_INVALID_STATE);
	AssertSameOwnerDestructionSnapshot(before,
		broker.owner_destruction_snapshot_for_test());
	assert(save.calls == save_calls);
	recovery.clear_retained_recovery_for_test();
	epoch.reset();
	assert(broker.exact_idle_for_test());
}

void TestTypedAudioTerminalPromotionHonorsBothImmutableDeadlines()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const uint32_t requested = closure | MISTER_RESOURCE_NATIVE_AUDIO;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		NativeRecovery recovery(broker, io, resources, resources, resources,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
		assert(resources.audio_calls == 1 && resources.last_deadline == 3000);
		clock.SetNow(2999);
		assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
			MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == requested);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		NativeRecovery recovery(broker, io, resources, resources, resources,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
		clock.SetNow(3000);
		assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
			MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Snapshot(*epoch, &observation) ==
			MISTER_RESULT_DEADLINE);
		assert(observation.neutral_resource_flags == closure);
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		NativeRecovery recovery(broker, io, resources, resources, resources,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
		clock.SetNow(6000);
		assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
			MISTER_RESULT_DEADLINE);
		assert(containment_io.writes == 0);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
	}
}

void TestTerminalRecoveryAndReadOnlyObservation()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		assert(io.writes == 5 && io.reads == 5 && io.releases == 1);
		assert(broker.mutation_sequence_for_test() == 6);
		assert(broker.containment_receipt_sequence_for_test() == 6);
		std::unique_ptr<HardwareLeaseView> forbidden_view;
		assert(broker.AcquireHardwareLeaseView(*terminal, &forbidden_view) ==
			MISTER_RESULT_INVALID_STATE);
		assert(forbidden_view == nullptr);
		std::unique_ptr<OperationLease> rejected;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
			&rejected) == MISTER_RESULT_INVALID_STATE);
		assert(broker.mutation_sequence_for_test() == 6);
		assert(broker.containment_receipt_sequence_for_test() == 6);
		terminal.reset();
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == closure);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000,
			&epoch) == MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
		assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_FPGA);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		io.core = 0;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000,
			&epoch) == MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(observation.observed_resource_flags == MISTER_RESOURCE_FPGA);
		assert(observation.neutral_resource_flags == 0);
	}
}

void TestRepeatedObservationReplacesStaleClassification()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	const uint32_t requested = MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	io.core = 0;
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.observed_resource_flags == requested);
	assert(observation.neutral_resource_flags == 0);
}

void TestForeignRecoveryAuthorityCannotMutate()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeContainmentIo io;
	NativeContainment containment(first, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> second_epoch;
	assert(second.BeginRecovery(closure, 3000, 6000, &second_epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> second_lease;
	assert(second.BeginRecoveryOperation(*second_epoch,
		OperationKind::terminal_fpga_cleanup, &second_lease) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*second_epoch, *second_lease) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.writes == 0 && io.reads == 0 && io.releases == 0);
}

void TestForeignRecoveryEpochWithCurrentLeaseCannotTouchHardware()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeContainmentIo io;
	NativeContainment containment(first, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> first_epoch;
	std::unique_ptr<RecoveryEpoch> second_epoch;
	assert(first.BeginRecovery(closure, 3000, 6000, &first_epoch) ==
		MISTER_RESULT_OK);
	assert(second.BeginRecovery(closure, 3000, 6000, &second_epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> first_terminal;
	assert(first.BeginRecoveryOperation(*first_epoch,
		OperationKind::terminal_fpga_cleanup, &first_terminal) ==
		MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*second_epoch, *first_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.writes == 0 && io.reads == 0 && io.releases == 0);
	assert(first.mutation_sequence_for_test() == 0);
	assert(containment.ResetAndContain(*first_epoch, *first_terminal) ==
		MISTER_RESULT_OK);
	first_terminal.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(first.FinishRecovery(std::move(first_epoch), &observation) ==
		MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == closure);
}

void TestRejectedTerminalCallDoesNotPoisonRecovery()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
	std::unique_ptr<OperationLease> wrong_kind;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&wrong_kind) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*epoch, *wrong_kind) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
	wrong_kind.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == closure);
}

void TestTerminalDeadlineBeforeHardwareRetainsRecoveryPartition()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	clock.SetNow(6000);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_DEADLINE);
	assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
	terminal.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == closure);
}

void TestTwoHundredRecoveryCycles()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	for (int cycle = 0; cycle != 200; ++cycle) {
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		save.snapshot_recovery = &recovery;
		save.snapshot_epoch = epoch.get();
		save.live_snapshot_captured = false;
		save.fail_once = true;
		assert(recovery.Perform(*epoch, OperationKind::save) ==
			MISTER_RESULT_PLATFORM);
		assert(save.live_snapshot_captured && save.live_snapshot.retained &&
			save.live_snapshot.registration_is_invoked);
		NativeRetainedOperationSnapshot suspended = {};
		assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
			&suspended) == MISTER_RESULT_OK);
		assert(suspended.retained && suspended.registration_is_suspended &&
			suspended.registration_effective_deadline_ms == 0);
		assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		NativeRetainedOperationSnapshot baseline = {};
		assert(recovery.broker_baseline_snapshot_for_test(&baseline) ==
			MISTER_RESULT_OK);
		AssertExactIdleSnapshot(baseline);
	}
}

void TestTerminalFailuresPreservePositivePartitions()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	struct Case {
		int fail_at;
		bool mismatch;
		bool deadline;
		Result result;
		Result finish_result;
		uint32_t observed;
		uint32_t neutral;
	};
	const Case cases[] = {
		{0, true, false, MISTER_RESULT_CLEANUP_INCOMPLETE,
			MISTER_RESULT_CLEANUP_INCOMPLETE,
			MISTER_RESOURCE_FPGA | MISTER_RESOURCE_CORE_PROTOCOL,
			MISTER_RESOURCE_BRIDGES},
		{8, false, false, MISTER_RESULT_PLATFORM,
			MISTER_RESULT_CLEANUP_INCOMPLETE, 0,
			MISTER_RESOURCE_CORE_PROTOCOL},
		{0, false, true, MISTER_RESULT_DEADLINE, MISTER_RESULT_DEADLINE, 0,
			MISTER_RESOURCE_CORE_PROTOCOL},
		{11, false, false, MISTER_RESULT_PLATFORM,
			MISTER_RESULT_CLEANUP_INCOMPLETE, 0,
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL},
		{-1, false, true, MISTER_RESULT_DEADLINE, MISTER_RESULT_DEADLINE, 0,
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL}
	};
	for (const Case &test : cases) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		io.fail_at = test.fail_at;
		if (test.mismatch) io.core = 0;
		if (test.deadline) {
			io.advance_at = test.fail_at == -1 ? 10 : 8;
			io.advance_clock = &clock;
		}
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == test.result);
		terminal.reset();
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			test.finish_result);
		assert(observation.observed_resource_flags == test.observed);
		assert(observation.neutral_resource_flags == test.neutral);
	}
}

// Break caught: an idle broker must expose a coherent zero retained-state
// baseline without retaining or manufacturing a recovery epoch.
void TestRecoveryRetainedSnapshotBaselineIsIdle()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	NativeRetainedOperationSnapshot snapshot = {};
	assert(recovery.broker_baseline_snapshot_for_test(&snapshot) ==
		MISTER_RESULT_OK);
	AssertExactIdleSnapshot(snapshot);
}

// Every rejected retained-query path must erase its caller output. This makes
// an old snapshot unusable as evidence after a null/foreign/stale-owner/kind
// query failure.
void TestRecoverySnapshotFailureMatrixZeroesEveryField()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	NativeRecovery owner(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot snapshot = {};
	assert(owner.retained_snapshot_for_test(*epoch, OperationKind::save,
		nullptr) == MISTER_RESULT_INVALID_ARGUMENT);
	memset(&snapshot, 0xff, sizeof(snapshot));
	assert(owner.retained_snapshot_for_test(*epoch, OperationKind::input,
		&snapshot) == MISTER_RESULT_INVALID_ARGUMENT);
	AssertZeroSnapshot(snapshot);

	save.fail_once = true;
	assert(owner.Perform(*epoch, OperationKind::save) == MISTER_RESULT_PLATFORM);
	NativeRecovery other_owner(broker, io, resources, resources, resources, save);
	memset(&snapshot, 0xff, sizeof(snapshot));
	assert(other_owner.retained_snapshot_for_test(*epoch, OperationKind::save,
		&snapshot) == MISTER_RESULT_INVALID_STATE);
	AssertZeroSnapshot(snapshot);
	memset(&snapshot, 0xff, sizeof(snapshot));
	assert(owner.retained_snapshot_for_test(*epoch, OperationKind::video,
		&snapshot) == MISTER_RESULT_INVALID_STATE);
	AssertZeroSnapshot(snapshot);

	HardwareBroker foreign_broker(clock);
	std::unique_ptr<RecoveryEpoch> foreign_epoch;
	assert(foreign_broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000,
		&foreign_epoch) == MISTER_RESULT_OK);
	memset(&snapshot, 0xff, sizeof(snapshot));
	assert(owner.retained_snapshot_for_test(*foreign_epoch, OperationKind::save,
		&snapshot) == MISTER_RESULT_INVALID_STATE);
	AssertZeroSnapshot(snapshot);
}

void TestRecoverySnapshotIsObservationalAndBaselineRejectsAllLiveStates()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, save,
		containment);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	save.snapshot_recovery = &recovery;
	save.snapshot_epoch = epoch.get();
	save.fail_once = true;
	std::unique_ptr<OperationInvocation> first_invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first_invocation) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first_invocation, OperationKind::save) ==
		MISTER_RESULT_PLATFORM);
	NativeRetainedOperationSnapshot first = {};
	NativeRetainedOperationSnapshot second = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&first) == MISTER_RESULT_OK);
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&second) == MISTER_RESULT_OK);
	AssertSameSnapshot(first, second);
	assert(first.registration_is_invoked &&
		first.registration_outcome_recorded);
	assert(broker.FinishInvocation(std::move(first_invocation)) ==
		MISTER_RESULT_OK);
	NativeRetainedOperationSnapshot suspended = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&suspended) == MISTER_RESULT_OK);
	AssertOnlyInvocationSuspensionChanged(first, suspended);
	NativeRetainedOperationSnapshot suspended_again = {};
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&suspended_again) == MISTER_RESULT_OK);
	AssertSameSnapshot(suspended, suspended_again);
	assert(recovery.broker_baseline_snapshot_for_test(&second) ==
		MISTER_RESULT_INVALID_STATE);
	AssertZeroSnapshot(second);
	std::unique_ptr<OperationInvocation> second_invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second_invocation) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second_invocation, OperationKind::save) ==
		MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(second_invocation)) ==
		MISTER_RESULT_OK);
	assert(recovery.retained_snapshot_for_test(*epoch, OperationKind::save,
		&second) == MISTER_RESULT_OK);
	assert(second.query_valid && !second.retained &&
		second.lease_identity == 0 && second.registration_identity == 0 &&
		second.invocation_identity == 0 && second.broker_mutation_sequence ==
		first.broker_mutation_sequence);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(recovery.broker_baseline_snapshot_for_test(&second) == MISTER_RESULT_OK);
	AssertExactIdleSnapshot(second);

	// A current recovery authority, observation evidence, and a terminal receipt
	// each reject the idle-only query and erase the supplied output.
	{
		FakeClock local_clock(1000);
		HardwareBroker local_broker(local_clock);
		FakeRecoveryIo local_io(local_broker);
		NativeRecovery local_recovery(local_broker, local_io);
		std::unique_ptr<RecoveryEpoch> local_epoch;
		assert(local_broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000,
			&local_epoch) == MISTER_RESULT_OK);
		memset(&second, 0xff, sizeof(second));
		assert(local_recovery.broker_baseline_snapshot_for_test(&second) ==
			MISTER_RESULT_INVALID_STATE);
		AssertZeroSnapshot(second);
	}
	{
		FakeClock local_clock(1000);
		HardwareBroker local_broker(local_clock);
		FakeRecoveryIo local_io(local_broker);
		FakeTypedRecoveryResources local_resources(local_broker);
		FakeContainmentIo local_io_containment;
		NativeContainment local_containment(local_broker, local_io_containment);
		NativeRecovery local_recovery(local_broker, local_io, local_resources,
			local_resources, local_resources, local_containment);
		std::unique_ptr<RecoveryEpoch> local_epoch;
		const uint32_t closure = MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
		assert(local_broker.BeginRecovery(closure, 3000, 6000, &local_epoch) ==
			MISTER_RESULT_OK);
		assert(local_containment.ObserveRecovery(*local_epoch) == MISTER_RESULT_OK);
		memset(&second, 0xff, sizeof(second));
		assert(local_recovery.broker_baseline_snapshot_for_test(&second) ==
			MISTER_RESULT_INVALID_STATE);
		AssertZeroSnapshot(second);
	}
	{
		FakeClock local_clock(1000);
		HardwareBroker local_broker(local_clock);
		FakeRecoveryIo local_io(local_broker);
		FakeTypedRecoveryResources local_resources(local_broker);
		FakeContainmentIo local_io_containment;
		NativeContainment local_containment(local_broker, local_io_containment);
		NativeRecovery local_recovery(local_broker, local_io, local_resources,
			local_resources, local_resources, local_containment);
		std::unique_ptr<RecoveryEpoch> local_epoch;
		const uint32_t closure = MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
		assert(local_broker.BeginRecovery(closure, 3000, 6000, &local_epoch) ==
			MISTER_RESULT_OK);
		assert(local_recovery.Perform(*local_epoch,
			OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_OK);
		memset(&second, 0xff, sizeof(second));
		assert(local_recovery.broker_baseline_snapshot_for_test(&second) ==
			MISTER_RESULT_INVALID_STATE);
		AssertZeroSnapshot(second);
	}
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	using namespace mister::native;
	TestRecoveryEpochAuthorityAndFreshness();
	TestRecoveryInvocationBoundsGenericMutation();
	TestRecoveryInvocationRebindsRetainedSaveDeadline();
	TestRecoveryInvocationBoundsContainmentObservation();
	TestCallbackDeadlineDoesNotPoisonRecoveryEpochOrOriginalMask();
	TestCallbackDeadlineRetriesEveryRetainedTypedRecoveryClass();
	TestRetainedRecoveryDeadlineBoundariesForEveryKind();
	TestCallbackDeadlineRetriesObservationAndTerminalResidue();
	TestRetryableRecoveryResultsReachSameEpochSuccess();
	TestRetryableObservationAndTerminalFailuresReachSuccess();
	TestRecoveryRejectedWithLiveGenerationOrCleanup();
	TestNormativeDependenciesAndExactDeadlines();
	TestSaveRecoveryRequiresTypedSafeRecordAuthority();
	TestTruthfulPartitionAndExactOkRule();
	TestNonOkResultsRetainPartialFields();
	TestDeadlineExpiryDoesNotExtendAndRetainsProgress();
	TestTerminalAdmissionDeadlineRetainsProgress();
	TestBusyRecoveryAdmissionDoesNotLatchDeadline();
	TestOperationOverrunRetainsTruthfulResult();
	TestGroupDeadlineOverrunSeparatesNeutralFromOutstandingTruth();
	TestCoreProtocolRecoveryUsesProfilelessSessionAuthority();
	TestCoreProtocolRecoveryReleaseRetryRetainsItsRecoveryLease();
	TestCoreSnapshotTracksLiveSuspendAndRebind();
	TestTypedCoupledRecoverySnapshotsVideoNeutralWhileAudioRemainsUnknown();
	TestDestroyingAbandonedTypedRecoveryBreaksItsRetainedOwnerCycle();
	TestCallbackExpiredOwnerDestructionDoesNotFenceSameEpochAdmission();
	TestRejectedInvokedOwnerDestructionIsObservableAndNeverIdle();
	TestCycleBreakRejectsForeignLiveAndTerminalLeasesWithoutMutation();
	TestCycleBreakRejectsAnAlreadyConsumedExactLeaseWithoutMutation();
	TestCycleBreakRejectsMalformedTypedStateWithoutMutation();
	TestSaveOwnerDestructionUsesTheNormalLeaseReleaseControl();
	TestCycleBreakRejectsRetainedSaveControlWithoutMutation();
	TestAudioRetainedSnapshotTracksLiveSuspendAndRebind();
	TestCoupledSnapshotDestructionLeavesExactIdleBaseline();
	TestVideoRetainedSnapshotTracksLiveSuspendAndRebind();
	TestCoupledRetainedSnapshotTracksLiveSuspendAndRebind();
	TestTypedSaveRecoveryRetainsOneTaggedLeaseAndOriginalDeadline();
	TestTaggedSaveRecoveryUsesTheFreshNativeSaveAdapterAndExactRecord();
	TestSaveRecoveryRejectsMissingForgedAndExpiredSafeAuthorityBeforeIo();
	TestRealSaveRecoveryReducesItsOutstandingMaskAndStillExcludesFinish();
	TestTypedCoupledRecoveryRetainsItsExactRegistrationForRetry();
	TestCoupledRecoveryClosureUnknownBlocksRawRetry();
	TestCoupledRecoveryCannotBorrowTheLaterFpgaDeadline();
	TestForeignTypedRecoveryBackendCannotAdoptARetainedSession();
	TestDestroyingRetainedRecoveryPermitsAuthorizedSameEpochRetry();
	TestTypedAudioTerminalPromotionHonorsBothImmutableDeadlines();
	TestTerminalRecoveryAndReadOnlyObservation();
	TestRepeatedObservationReplacesStaleClassification();
	TestForeignRecoveryAuthorityCannotMutate();
	TestForeignRecoveryEpochWithCurrentLeaseCannotTouchHardware();
	TestRejectedTerminalCallDoesNotPoisonRecovery();
	TestTerminalDeadlineBeforeHardwareRetainsRecoveryPartition();
	TestTwoHundredRecoveryCycles();
	TestTerminalFailuresPreservePositivePartitions();
	TestRecoveryRetainedSnapshotBaselineIsIdle();
	TestRecoverySnapshotFailureMatrixZeroesEveryField();
	TestRecoverySnapshotIsObservationalAndBaselineRejectsAllLiveStates();
	return 0;
}
