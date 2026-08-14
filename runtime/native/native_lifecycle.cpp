// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_lifecycle.hpp"

#include <limits.h>

namespace mister {
namespace native {

namespace {

NativeResourceLedger EmptyLedger()
{
	const NativeDigitalNeutral empty = {0, {0, 0}};
	const NativeResourceLedger ledger = {
		0, false, false, false, false, false, false, false, false, false, false,
		{false, false}, {empty, empty}
	};
	return ledger;
}

NativeCleanupTiming EmptyCleanupTiming()
{
	const NativeCleanupTiming timing = {false, 0, 0, 0};
	return timing;
}

NativeFailureDrainTiming EmptyFailureDrainTiming()
{
	const NativeFailureDrainTiming timing = {false, 0, 0};
	return timing;
}

} // namespace

NativeLifecycle::NativeLifecycle(NativeClock &clock, HardwareBroker &broker,
	NativeResourceSet resources)
	: clock_(clock), broker_(broker), resources_(resources), mutex_(),
	  state_(NativeLifecycleState::idle), generation_(0), ledger_(EmptyLedger()),
	  cleanup_timing_(EmptyCleanupTiming()),
	  failure_drain_timing_(EmptyFailureDrainTiming()),
	  latched_activation_result_(MISTER_RESULT_OK), callback_deadline_ms_(0),
	  profile_(nullptr),
	  cleanup_epoch_(), cleanup_invocation_(), core_protocol_cleanup_lease_()
{
}

NativeLifecycle::~NativeLifecycle()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ == NativeLifecycleState::active && generation_ != 0)
		broker_.LatchFailure(generation_);
	// This only releases the registration. The protocol resource owns the
	// process-exit close path and session-handle destruction performs no I/O.
	core_protocol_cleanup_lease_.reset();
	save_cleanup_lease_.reset();
	audio_cleanup_lease_.reset();
	video_cleanup_lease_.reset();
	coupled_audio_video_cleanup_lease_.reset();
	cleanup_invocation_.reset();
	cleanup_epoch_.reset();
	if (ledger_.scheduler)
		resources_.scheduler.CloseSchedulerForProcessExit();
	if (ledger_.offload)
		resources_.offload.CloseOffloadForProcessExit();
	if (ledger_.input_descriptors)
		resources_.input_descriptors.CloseInputDescriptorsForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_SAVES) != 0)
		resources_.save.CloseSaveForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_CONTENT) != 0)
		resources_.content.CloseContentForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0)
		resources_.video.CloseVideoForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0)
		resources_.audio.CloseAudioForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0)
		resources_.hardware.CloseCoreProtocolForProcessExit();
	if (ledger_.containment_mappings ||
		(ledger_.resource_flags & (MISTER_RESOURCE_FPGA |
		 MISTER_RESOURCE_BRIDGES)) != 0) {
		resources_.hardware.CloseFpgaMappingsForProcessExit();
	}
}

uint64_t NativeLifecycle::SaturatingAdd(uint64_t value, uint64_t delta)
{
	return value > UINT64_MAX - delta ? UINT64_MAX : value + delta;
}

Result NativeLifecycle::CleanupFailure(Result result)
{
	return result == MISTER_RESULT_DEADLINE ? MISTER_RESULT_DEADLINE :
		MISTER_RESULT_CLEANUP_INCOMPLETE;
}

Result NativeLifecycle::Activate(const NativeCoreProfile &profile,
	uint64_t activation_deadline_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return ActivateLocked(profile, activation_deadline_ms, false);
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result NativeLifecycle::ActivateFixtureForTest(const NativeCoreProfile &profile,
	uint64_t activation_deadline_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return ActivateLocked(profile, activation_deadline_ms, true);
}

uint64_t NativeLifecycle::cleanup_epoch_identity_for_test() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return cleanup_epoch_ ? cleanup_epoch_->identity_for_test() : 0;
}

NativeFailureDrainTiming NativeLifecycle::failure_drain_timing_for_test() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return failure_drain_timing_;
}

NativeCleanupBrokerSnapshot NativeLifecycle::cleanup_broker_snapshot_for_test(
	PeripheralSessionKind kind) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return cleanup_broker_callback_snapshot_for_test(kind);
}

NativeCleanupBrokerSnapshot
NativeLifecycle::cleanup_broker_callback_snapshot_for_test(
	PeripheralSessionKind kind) const
{
	NativeCleanupBrokerSnapshot snapshot = {};
	const OperationLease *lease = kind == PeripheralSessionKind::audio ?
		audio_cleanup_lease_.get() : kind == PeripheralSessionKind::video ?
		video_cleanup_lease_.get() : coupled_audio_video_cleanup_lease_.get();
	const PeripheralBackendIdentity expected_backend =
		kind == PeripheralSessionKind::audio ?
		resources_.audio.BackendIdentity() :
		kind == PeripheralSessionKind::video ?
		resources_.video.BackendIdentity() :
		resources_.audio_video.BackendIdentity();

	std::lock_guard<std::mutex> broker_lock(broker_.mutex_);
	const std::shared_ptr<OperationRegistration> invoked =
		broker_.invocation_registration_.lock();
	const std::shared_ptr<OperationRegistration> suspended =
		broker_.suspended_registration_.lock();
	const std::shared_ptr<PeripheralSessionState> session =
		broker_.peripheral_session_state_.lock();
	const std::shared_ptr<OperationRegistration> session_owner = session ?
		session->owner_registration.lock() : std::shared_ptr<OperationRegistration>();
	const OperationRegistration *const registration = session_owner ?
		session_owner.get() : invoked ? invoked.get() : suspended.get();

	snapshot.lease_identity = reinterpret_cast<uintptr_t>(lease);
	snapshot.registration_identity = reinterpret_cast<uintptr_t>(registration);
	snapshot.session_identity = reinterpret_cast<uintptr_t>(session.get());
	snapshot.broker_generation = broker_.generation_;
	snapshot.cleanup_identity = broker_.cleanup_identity_;
	snapshot.cleanup_non_fpga_deadline_ms =
		broker_.cleanup_non_fpga_deadline_ms_;
	snapshot.cleanup_fpga_deadline_ms = broker_.cleanup_fpga_deadline_ms_;
	snapshot.invocation_identity = broker_.invocation_identity_;
	snapshot.invocation_callback_deadline_ms =
		broker_.invocation_callback_deadline_ms_;
	snapshot.broker_mutation_sequence = broker_.mutation_sequence_;
	snapshot.active_lease_count = broker_.active_lease_count_;
	snapshot.terminal_lease_count = broker_.terminal_lease_count_;
	snapshot.invocation_registered = broker_.invocation_registered_;
	snapshot.invocation_outcome_missing = broker_.invocation_outcome_missing_;
	snapshot.registration_is_suspended = registration != nullptr &&
		suspended.get() == registration;
	snapshot.registration_is_invoked = registration != nullptr &&
		invoked.get() == registration;
	snapshot.cleanup_registered = broker_.cleanup_registered_;
	snapshot.hardware_transaction_active = broker_.hardware_transaction_active_;
	snapshot.broker_idle = broker_.state_ == HardwareBroker::State::idle;
	if (!session || session->kind != kind) return snapshot;

	snapshot.session_effective_deadline_ms = session->absolute_deadline_ms;
	snapshot.session_initial_mutation_sequence =
		session->initial_mutation_sequence;
	snapshot.session_last_mutation_sequence = session->last_mutation_sequence;
	snapshot.session_kind = session->kind;
	snapshot.session_action = session->action;
	snapshot.session_phase = session->phase.load();
	snapshot.action_word_count = session->action_word_count;
	snapshot.action_next_word_index = session->action_next_word_index;
	snapshot.action_transaction_closed = session->action_transaction_closed;
	snapshot.action_progress_unknown = session->action_progress_unknown;
	snapshot.recheckout_allowed = session->recheckout_allowed;
	snapshot.backend_matches_expected = session->backend.Matches(expected_backend);
	return snapshot;
}
#endif

Result NativeLifecycle::ActivateLocked(const NativeCoreProfile &profile,
	uint64_t activation_deadline_ms, bool fixture)
{
	callback_deadline_ms_ = activation_deadline_ms;
	if (state_ != NativeLifecycleState::idle || generation_ != 0)
		return MISTER_RESULT_INVALID_STATE;
	state_ = NativeLifecycleState::preflight;
	Result result = resources_.preflight.Validate(profile,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) {
		state_ = NativeLifecycleState::idle;
		return result;
	}
	if (clock_.NowMs() >= activation_deadline_ms) {
		state_ = NativeLifecycleState::idle;
		return MISTER_RESULT_DEADLINE;
	}
	NativeAcquisitionOutcome outcome = resources_.content.RetainContent(
		activation_deadline_ms);
	if (outcome.acquired) ledger_.resource_flags |= MISTER_RESOURCE_CONTENT;
	if (outcome.result != MISTER_RESULT_OK) {
		if (outcome.acquired)
			return FinishPreownershipContentLocked(outcome.result);
		state_ = NativeLifecycleState::idle;
		return outcome.result;
	}
	if (!outcome.acquired)
		return FinishPreownershipContentLocked(MISTER_RESULT_PLATFORM);
	if (clock_.NowMs() >= activation_deadline_ms)
		return FinishPreownershipContentLocked(MISTER_RESULT_DEADLINE);

	state_ = NativeLifecycleState::activating;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	result = fixture ? broker_.EnterFixtureForTest(profile, &generation_) :
		broker_.Enter(profile, &generation_);
#else
	(void)fixture;
	result = broker_.Enter(profile, &generation_);
#endif
	if (result != MISTER_RESULT_OK) {
		generation_ = 0;
		return FinishPreownershipContentLocked(result);
	}
	ledger_.generation = true;
	profile_ = &profile;

	std::unique_ptr<OperationLease> lease;
	result = broker_.Begin(generation_, OperationKind::program_fpga,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK)
		outcome = resources_.hardware.AcquireContainmentMappings(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_FPGA,
		&ledger_.containment_mappings, activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::offload,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK)
		outcome = resources_.offload.StartOffload(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, 0, &ledger_.offload,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::program_fpga,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK) outcome = resources_.hardware.AcquireFpga(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_FPGA, nullptr,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::program_fpga,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK) outcome = resources_.hardware.EnableBridges(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_BRIDGES, nullptr,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::core_protocol,
		activation_deadline_ms, &lease);
	NativeCoreProtocolOutcome protocol_outcome = {result, false, false};
	if (result == MISTER_RESULT_OK) protocol_outcome =
		resources_.hardware.StartCoreProtocol(*lease, profile, resources_.content);
	result = FinishCoreProtocolAcquisitionLocked(protocol_outcome, &lease,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::video,
		activation_deadline_ms, &lease);
	NativePeripheralAcquisitionOutcome peripheral_outcome = {result, false};
	PeripheralBrokerDisposition peripheral_disposition =
		PeripheralBrokerDisposition::no_session;
	if (result == MISTER_RESULT_OK) {
		std::unique_ptr<ActiveVideoSessionBundle> bundle;
		result = lease->AcquireActiveVideoSession(broker_, profile,
			resources_.video.BackendIdentity(), &bundle);
		if (result == MISTER_RESULT_OK)
			peripheral_outcome = resources_.video.StartVideo(std::move(bundle));
		else
			peripheral_outcome = {result, false};
		if (lease->GetVideoSessionDisposition(broker_,
			&peripheral_disposition) != MISTER_RESULT_OK)
			peripheral_outcome = {MISTER_RESULT_PLATFORM, false};
	}
	lease.reset();
	result = FinishPeripheralAcquisitionLocked(peripheral_outcome,
		peripheral_disposition, MISTER_RESOURCE_NATIVE_VIDEO,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::audio,
		activation_deadline_ms, &lease);
	peripheral_outcome = {result, false};
	peripheral_disposition = PeripheralBrokerDisposition::no_session;
	if (result == MISTER_RESULT_OK) {
		std::unique_ptr<ActiveAudioSessionBundle> bundle;
		result = lease->AcquireActiveAudioSession(broker_, profile,
			resources_.audio.BackendIdentity(), &bundle);
		if (result == MISTER_RESULT_OK)
			peripheral_outcome = resources_.audio.StartAudio(std::move(bundle));
		else
			peripheral_outcome = {result, false};
		if (lease->GetAudioSessionDisposition(broker_,
			&peripheral_disposition) != MISTER_RESULT_OK)
			peripheral_outcome = {MISTER_RESULT_PLATFORM, false};
	}
	lease.reset();
	result = FinishPeripheralAcquisitionLocked(peripheral_outcome,
		peripheral_disposition, MISTER_RESOURCE_NATIVE_AUDIO,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	if (profile.video.coupled_transmitter) {
		result = broker_.Begin(generation_, OperationKind::audio_video,
			activation_deadline_ms, &lease);
		NativeCoupledAcquisitionOutcome coupled_outcome = {result, 0, false};
		PeripheralBrokerDisposition coupled_disposition =
			PeripheralBrokerDisposition::no_session;
		if (result == MISTER_RESULT_OK) {
			std::unique_ptr<ActiveAudioVideoSessionBundle> bundle;
			result = lease->AcquireActiveAudioVideoSession(broker_, profile,
				resources_.audio_video.BackendIdentity(), &bundle);
			if (result == MISTER_RESULT_OK)
				coupled_outcome = resources_.audio_video.StartAudioVideo(
					std::move(bundle));
			else
				coupled_outcome = {result, 0, false};
			if (lease->GetAudioVideoSessionDisposition(broker_,
				&coupled_disposition) != MISTER_RESULT_OK)
				coupled_outcome = {MISTER_RESULT_PLATFORM, 0, false};
		}
		lease.reset();
		result = FinishCoupledAcquisitionLocked(coupled_outcome,
			coupled_disposition, activation_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
	}

	result = broker_.Begin(generation_, OperationKind::input_descriptors,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK)
		outcome = resources_.input_descriptors.OpenInputDescriptors(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_CORE_INPUT,
		&ledger_.input_descriptors, activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	NativeSaveKey save_key = {};
	result = resources_.content.DeriveSaveKey(profile, &save_key);
	NativeSaveOpenOutcome save_outcome = {result, false};
	if (result == MISTER_RESULT_OK) {
		result = broker_.Begin(generation_, OperationKind::save,
			activation_deadline_ms, &lease);
		save_outcome = {result, false};
		if (result == MISTER_RESULT_OK)
			save_outcome = resources_.save.OpenSave(*lease, profile, save_key);
	}
	lease.reset();
	result = FinishSaveAcquisitionLocked(save_outcome, activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::scheduler,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK)
		outcome = resources_.scheduler.StartScheduler(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, 0, &ledger_.scheduler,
		activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	state_ = NativeLifecycleState::active;
	latched_activation_result_ = MISTER_RESULT_OK;
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::FinishAcquisitionLocked(NativeAcquisitionOutcome outcome,
	uint64_t resource_flags, bool *supporting_ledger,
	uint64_t activation_deadline_ms)
{
	if (outcome.acquired) {
		ledger_.resource_flags |= static_cast<uint32_t>(resource_flags);
		if (supporting_ledger != nullptr) *supporting_ledger = true;
	}
	if (outcome.result != MISTER_RESULT_OK)
		return FailActivationLocked(outcome.result);
	if (!outcome.acquired) return FailActivationLocked(MISTER_RESULT_PLATFORM);
	if (clock_.NowMs() >= activation_deadline_ms)
		return FailActivationLocked(MISTER_RESULT_DEADLINE);
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::FinishPeripheralAcquisitionLocked(
	NativePeripheralAcquisitionOutcome outcome,
	PeripheralBrokerDisposition disposition, uint64_t resource_flags,
	uint64_t activation_deadline_ms)
{
	const bool valid_success = outcome.result == MISTER_RESULT_OK &&
		outcome.acquired &&
		disposition == PeripheralBrokerDisposition::success_completed;
	const bool valid_failure = outcome.result != MISTER_RESULT_OK &&
		(disposition == PeripheralBrokerDisposition::failure_completed ||
		 (!outcome.acquired &&
		  disposition == PeripheralBrokerDisposition::no_session));
	if (!valid_success && !valid_failure)
		return FailActivationLocked(MISTER_RESULT_PLATFORM);
	const NativeAcquisitionOutcome normalized = {outcome.result,
		outcome.acquired};
	return FinishAcquisitionLocked(normalized, resource_flags, nullptr,
		activation_deadline_ms);
}

Result NativeLifecycle::FinishSaveAcquisitionLocked(
	NativeSaveOpenOutcome outcome, uint64_t activation_deadline_ms)
{
	const NativeAcquisitionOutcome normalized = {outcome.result, outcome.acquired};
	return FinishAcquisitionLocked(normalized, MISTER_RESOURCE_SAVES, nullptr,
		activation_deadline_ms);
}

Result NativeLifecycle::FinishSaveCleanupLocked(
	const NativeSaveCloseOutcome &outcome) const
{
	if (outcome.result != MISTER_RESULT_OK ||
		(outcome.data_synchronization_required && !outcome.data_synchronized) ||
		(outcome.metadata_synchronization_required &&
		 !outcome.metadata_synchronized) || !outcome.descriptors_absent ||
		outcome.closure_unknown)
		return CleanupFailure(outcome.result == MISTER_RESULT_OK ?
			MISTER_RESULT_CLEANUP_INCOMPLETE : outcome.result);
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::FinishPeripheralCleanupLocked(
	const NativePeripheralReleaseOutcome &outcome,
	PeripheralBrokerDisposition disposition) const
{
	if (outcome.result != MISTER_RESULT_OK ||
		disposition != PeripheralBrokerDisposition::success_completed ||
		!outcome.local_shutdown_complete || outcome.closure_unknown)
		return CleanupFailure(outcome.result == MISTER_RESULT_OK ?
			MISTER_RESULT_CLEANUP_INCOMPLETE : outcome.result);
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::FinishCoupledAcquisitionLocked(
	NativeCoupledAcquisitionOutcome outcome,
	PeripheralBrokerDisposition disposition, uint64_t activation_deadline_ms)
{
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	const bool valid_success = outcome.result == MISTER_RESULT_OK &&
		outcome.acquired && outcome.affected_flags == affected &&
		disposition == PeripheralBrokerDisposition::success_completed;
	const bool valid_failure = outcome.result != MISTER_RESULT_OK &&
		(disposition == PeripheralBrokerDisposition::failure_completed ||
		 (!outcome.acquired &&
		  disposition == PeripheralBrokerDisposition::no_session));
	if (!valid_success && !valid_failure)
		return FailActivationLocked(MISTER_RESULT_PLATFORM);
	if (outcome.acquired) {
		ledger_.resource_flags |= affected;
		// A partial coupled acquisition affects both bits. Its cleanup still
		// requires the typed coupled path even though the active registration
		// has already been atomically failed and consumed.
		ledger_.coupled_audio_video_active = true;
	}
	if (outcome.result != MISTER_RESULT_OK)
		return FailActivationLocked(outcome.result);
	if (clock_.NowMs() >= activation_deadline_ms)
		return FailActivationLocked(MISTER_RESULT_DEADLINE);
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::FinishCoupledCleanupLocked(
	const NativeCoupledReleaseOutcome &outcome,
	PeripheralBrokerDisposition disposition) const
{
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	if (outcome.result != MISTER_RESULT_OK || outcome.affected_flags != affected ||
		disposition != PeripheralBrokerDisposition::success_completed ||
		(outcome.observed_flags & outcome.neutral_flags) != 0 ||
		((outcome.observed_flags | outcome.neutral_flags) & ~affected) != 0 ||
		!outcome.local_resources_absent || outcome.closure_unknown)
		return CleanupFailure(outcome.result == MISTER_RESULT_OK ?
			MISTER_RESULT_CLEANUP_INCOMPLETE : outcome.result);
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::FinishCoreProtocolAcquisitionLocked(
	NativeCoreProtocolOutcome outcome,
	std::unique_ptr<OperationLease> *lease,
	uint64_t activation_deadline_ms)
{
	if (outcome.acquired) {
		ledger_.resource_flags |= MISTER_RESOURCE_CORE_PROTOCOL;
		ledger_.core_protocol_shutdown_complete = false;
	}

	// A failed Begin has no provider claim or live operation registration to
	// validate. All provider-returned outcomes retain the operation lease until
	// the broker's completion state has been checked under its own lock.
	if (!*lease) return FailActivationLocked(outcome.result);

	CoreProtocolBrokerDisposition disposition =
		CoreProtocolBrokerDisposition::no_session;
	const Result disposition_result =
		(*lease)->GetCoreProtocolBrokerDisposition(broker_, &disposition);
	const bool valid_success =
		disposition_result == MISTER_RESULT_OK &&
		outcome.result == MISTER_RESULT_OK && outcome.acquired &&
		!outcome.broker_failure_completed &&
		disposition == CoreProtocolBrokerDisposition::success_completed;
	const bool valid_failure =
		disposition_result == MISTER_RESULT_OK &&
		outcome.result != MISTER_RESULT_OK && outcome.acquired &&
		outcome.broker_failure_completed &&
		disposition == CoreProtocolBrokerDisposition::failure_completed;
	const bool valid_nonmutating_failure =
		disposition_result == MISTER_RESULT_OK &&
		outcome.result != MISTER_RESULT_OK && !outcome.acquired &&
		!outcome.broker_failure_completed &&
		disposition == CoreProtocolBrokerDisposition::no_session;

	if (valid_success && clock_.NowMs() < activation_deadline_ms) {
		lease->reset();
		return MISTER_RESULT_OK;
	}
	if (valid_nonmutating_failure) {
		lease->reset();
		return FailActivationLocked(outcome.result);
	}

	Result activation_result = valid_failure ? outcome.result :
		MISTER_RESULT_PLATFORM;
	if (valid_success) activation_result = MISTER_RESULT_DEADLINE;
	if (!valid_failure) {
		const Result closed = (*lease)->CompleteInvalidCoreProtocolOutcome(
			broker_, activation_result);
		if (closed != MISTER_RESULT_OK) {
			// This fallback still closes admission while the operation lease is
			// live. The broker-owned completion path above is the only expected
			// route for a current protocol registration.
			broker_.LatchFailure(generation_);
		}
	}

	latched_activation_result_ = activation_result;
	state_ = NativeLifecycleState::quiescing;
	EstablishFailureDrainTimingLocked();
	EstablishCleanupTimingLocked();
	lease->reset();
	const uint64_t quiesce_deadline = callback_deadline_ms_ <
		failure_drain_timing_.drain_deadline_ms ? callback_deadline_ms_ :
		failure_drain_timing_.drain_deadline_ms;
	const Result quiesce = broker_.Quiesce(generation_, quiesce_deadline);
	if (quiesce != MISTER_RESULT_OK) return activation_result;
	state_ = NativeLifecycleState::cleanup;
	if (EstablishCleanupLocked(callback_deadline_ms_) == MISTER_RESULT_OK)
		RunCleanupLocked(callback_deadline_ms_);
	return activation_result;
}

Result NativeLifecycle::FailActivationLocked(Result activation_result)
{
	latched_activation_result_ = activation_result;
	state_ = NativeLifecycleState::quiescing;
	broker_.LatchFailure(generation_);
	EstablishFailureDrainTimingLocked();
	EstablishCleanupTimingLocked();
	const uint64_t quiesce_deadline = callback_deadline_ms_ <
		failure_drain_timing_.drain_deadline_ms ? callback_deadline_ms_ :
		failure_drain_timing_.drain_deadline_ms;
	const Result quiesce = broker_.Quiesce(generation_, quiesce_deadline);
	if (quiesce != MISTER_RESULT_OK) return activation_result;
	state_ = NativeLifecycleState::cleanup;
	if (EstablishCleanupLocked(callback_deadline_ms_) == MISTER_RESULT_OK)
		RunCleanupLocked(callback_deadline_ms_);
	return activation_result;
}

Result NativeLifecycle::FinishPreownershipContentLocked(Result activation_result)
{
	latched_activation_result_ = activation_result;
	if ((ledger_.resource_flags & MISTER_RESOURCE_CONTENT) != 0) {
		EstablishCleanupTimingLocked();
		const uint64_t close_deadline = callback_deadline_ms_ <
			cleanup_timing_.non_fpga_deadline_ms ? callback_deadline_ms_ :
			cleanup_timing_.non_fpga_deadline_ms;
		const Result close = resources_.content.CloseContent(
			close_deadline);
		if (close != MISTER_RESULT_OK ||
			clock_.NowMs() >= close_deadline) {
			state_ = NativeLifecycleState::cleanup;
			return activation_result;
		}
		ledger_.resource_flags &= ~MISTER_RESOURCE_CONTENT;
	}
	state_ = NativeLifecycleState::idle;
	cleanup_timing_ = EmptyCleanupTiming();
	return activation_result;
}

Result NativeLifecycle::Stop(uint64_t callback_deadline_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	callback_deadline_ms_ = callback_deadline_ms;
	if (callback_deadline_ms == 0 || clock_.NowMs() >= callback_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	if (state_ == NativeLifecycleState::idle) return MISTER_RESULT_OK;
	if (state_ == NativeLifecycleState::cleanup && generation_ == 0) {
		if ((ledger_.resource_flags & MISTER_RESOURCE_CONTENT) == 0) {
			state_ = NativeLifecycleState::idle;
			return MISTER_RESULT_OK;
		}
		if (!cleanup_timing_.established) EstablishCleanupTimingLocked();
		const uint64_t close_deadline = callback_deadline_ms <
			cleanup_timing_.non_fpga_deadline_ms ? callback_deadline_ms :
			cleanup_timing_.non_fpga_deadline_ms;
		const Result result = resources_.content.CloseContent(close_deadline);
		if (result != MISTER_RESULT_OK) return CleanupFailure(result);
		if (clock_.NowMs() >= close_deadline)
			return MISTER_RESULT_DEADLINE;
		ledger_.resource_flags &= ~MISTER_RESOURCE_CONTENT;
		state_ = NativeLifecycleState::idle;
		cleanup_timing_ = EmptyCleanupTiming();
		return MISTER_RESULT_OK;
	}
	if (state_ == NativeLifecycleState::preflight ||
		state_ == NativeLifecycleState::activating ||
		state_ == NativeLifecycleState::neutral) {
		return MISTER_RESULT_INVALID_STATE;
	}
	if (state_ == NativeLifecycleState::active ||
		state_ == NativeLifecycleState::quiescing) {
		state_ = NativeLifecycleState::quiescing;
		EstablishFailureDrainTimingLocked();
		EstablishCleanupTimingLocked();
		const uint64_t quiesce_deadline =
			failure_drain_timing_.drain_deadline_ms;
		const uint64_t effective_quiesce = callback_deadline_ms < quiesce_deadline ?
			callback_deadline_ms : quiesce_deadline;
		const Result quiesce = broker_.Quiesce(generation_, effective_quiesce);
		if (quiesce != MISTER_RESULT_OK) return CleanupFailure(quiesce);
		state_ = NativeLifecycleState::cleanup;
	}
	const Result establish = EstablishCleanupLocked(callback_deadline_ms);
	if (establish != MISTER_RESULT_OK) return CleanupFailure(establish);
	return RunCleanupLocked(callback_deadline_ms);
}

Result NativeLifecycle::EstablishCleanupLocked(uint64_t callback_deadline_ms)
{
	EstablishCleanupTimingLocked();
	if (cleanup_epoch_) return MISTER_RESULT_OK;
	if (clock_.NowMs() >= callback_deadline_ms) return MISTER_RESULT_DEADLINE;
	const Result result = broker_.BeginCleanup(generation_,
		cleanup_timing_.non_fpga_deadline_ms,
		cleanup_timing_.fpga_deadline_ms, &cleanup_epoch_);
	if (result != MISTER_RESULT_OK) return result;
	return clock_.NowMs() >= callback_deadline_ms ? MISTER_RESULT_DEADLINE :
		MISTER_RESULT_OK;
}

void NativeLifecycle::EstablishCleanupTimingLocked()
{
	if (cleanup_timing_.established) return;
	cleanup_timing_.established = true;
	cleanup_timing_.cleanup_start_ms = clock_.NowMs();
	cleanup_timing_.non_fpga_deadline_ms = SaturatingAdd(
		cleanup_timing_.cleanup_start_ms, 2000);
	cleanup_timing_.fpga_deadline_ms = SaturatingAdd(
		cleanup_timing_.cleanup_start_ms, 5000);
}

void NativeLifecycle::EstablishFailureDrainTimingLocked()
{
	if (failure_drain_timing_.established) return;
	failure_drain_timing_.established = true;
	failure_drain_timing_.drain_start_ms = clock_.NowMs();
	failure_drain_timing_.drain_deadline_ms = SaturatingAdd(
		failure_drain_timing_.drain_start_ms, 2000);
}

Result NativeLifecycle::BeginCleanupOperationLocked(OperationKind kind,
	std::unique_ptr<OperationLease> *lease)
{
	if (!cleanup_invocation_) return MISTER_RESULT_INVALID_STATE;
	const Result result = *lease ? broker_.ContinueCleanupOperation(
		*cleanup_epoch_, *cleanup_invocation_, **lease) :
		broker_.BeginCleanupOperation(*cleanup_epoch_, *cleanup_invocation_,
			kind, lease);
	return result == MISTER_RESULT_OK ? MISTER_RESULT_OK : CleanupFailure(result);
}

Result NativeLifecycle::FinishCleanupCallbackLocked(Result result)
{
	if (!cleanup_invocation_) return MISTER_RESULT_PLATFORM;
	const Result finish = broker_.FinishInvocation(std::move(cleanup_invocation_));
	if (finish == MISTER_RESULT_OK) return result;
	cleanup_invocation_.reset();
	return MISTER_RESULT_PLATFORM;
}

Result NativeLifecycle::FinishCleanupOperationLocked(Result result,
	const OperationLease &lease)
{
	Result normalized = result != MISTER_RESULT_OK ? CleanupFailure(result) :
		(clock_.NowMs() >= lease.absolute_deadline_ms() ?
		 MISTER_RESULT_DEADLINE : MISTER_RESULT_OK);
	if (lease.operation_kind() != OperationKind::terminal_fpga_cleanup) {
		if (!cleanup_invocation_ || broker_.RecordOperationOutcome(
			*cleanup_invocation_, lease, normalized) != MISTER_RESULT_OK)
			return MISTER_RESULT_PLATFORM;
	}
	return normalized;
}

Result NativeLifecycle::CaptureDigitalNeutralLocked(
	const OperationLease &lease)
{
	if (ledger_.digital_neutral_captured ||
		(ledger_.resource_flags & MISTER_RESOURCE_CORE_INPUT) == 0)
		return MISTER_RESULT_OK;
	NativeDigitalNeutral values[kNativePlayerCount] = {};
	bool valid[kNativePlayerCount] = {false, false};
	const Result result = resources_.input_descriptors.CaptureDigitalNeutral(
		lease, values, valid, kNativePlayerCount);
	if (result != MISTER_RESULT_OK) return CleanupFailure(result);
	if (profile_ == nullptr) return MISTER_RESULT_CLEANUP_INCOMPLETE;
	for (size_t player = 0; player < kNativePlayerCount; ++player) {
		if (!valid[player]) continue;
		if (values[player].player != player || values[player].words[0] !=
			profile_->input.player_command[player] || values[player].words[1] != 0) {
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
	}
	for (size_t player = 0; player < kNativePlayerCount; ++player) {
		ledger_.digital_neutral[player] = values[player];
		ledger_.digital_neutral_valid[player] = valid[player];
	}
	ledger_.digital_neutral_captured = true;
	return MISTER_RESULT_OK;
}

Result NativeLifecycle::RunCleanupLocked(uint64_t callback_deadline_ms)
{
	if (cleanup_invocation_) return MISTER_RESULT_INVALID_STATE;
	Result invocation_result = broker_.BeginCleanupInvocation(*cleanup_epoch_,
		callback_deadline_ms, &cleanup_invocation_);
	if (invocation_result != MISTER_RESULT_OK)
		return CleanupFailure(invocation_result);
	const auto run = [&]() -> Result {
	std::unique_ptr<OperationLease> lease;
	Result result = MISTER_RESULT_OK;
	if (ledger_.scheduler) {
		result = BeginCleanupOperationLocked(OperationKind::scheduler, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.scheduler.StopScheduler(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.scheduler = false;
	}
	if (ledger_.offload) {
		result = BeginCleanupOperationLocked(OperationKind::offload, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.offload.RejectAndJoinOffload(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.offload = false;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_SAVES) != 0) {
		result = BeginCleanupOperationLocked(OperationKind::save,
			&save_cleanup_lease_);
		if (result != MISTER_RESULT_OK) return result;
		const NativeSaveCloseOutcome save_outcome =
			resources_.save.FlushAndCloseSave(*save_cleanup_lease_);
		result = FinishSaveCleanupLocked(save_outcome);
		result = FinishCleanupOperationLocked(result, *save_cleanup_lease_);
		if (result != MISTER_RESULT_OK) return result;
		save_cleanup_lease_.reset();
		ledger_.resource_flags &= ~MISTER_RESOURCE_SAVES;
	}
	if (!ledger_.digital_neutral_captured &&
		(ledger_.resource_flags & MISTER_RESOURCE_CORE_INPUT) != 0) {
		result = BeginCleanupOperationLocked(OperationKind::input, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = CaptureDigitalNeutralLocked(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
	}
	for (size_t player = 0; player < kNativePlayerCount; ++player) {
		if (!ledger_.digital_neutral_valid[player]) continue;
		result = BeginCleanupOperationLocked(OperationKind::input, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.hardware.ReplayDigitalNeutral(*lease,
			ledger_.digital_neutral[player]);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.digital_neutral_valid[player] = false;
	}
	if (ledger_.input_descriptors) {
		result = BeginCleanupOperationLocked(OperationKind::input_descriptors,
			&lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.input_descriptors.CloseInputDescriptors(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.input_descriptors = false;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0 &&
		!ledger_.video_shutdown_complete) {
		result = BeginCleanupOperationLocked(OperationKind::video,
			&video_cleanup_lease_);
		if (result != MISTER_RESULT_OK) return result;
		std::unique_ptr<CleanupVideoSessionBundle> bundle;
		result = video_cleanup_lease_->AcquireCleanupVideoSession(broker_,
			resources_.video.BackendIdentity(), &bundle);
		NativePeripheralReleaseOutcome peripheral_outcome = {result, false,
			false, false, false};
		if (result == MISTER_RESULT_OK)
			peripheral_outcome = resources_.video.StopVideo(std::move(bundle));
		PeripheralBrokerDisposition peripheral_disposition =
			PeripheralBrokerDisposition::no_session;
		if (video_cleanup_lease_->GetVideoSessionDisposition(broker_,
			&peripheral_disposition) != MISTER_RESULT_OK)
			peripheral_outcome.result = MISTER_RESULT_PLATFORM;
		result = FinishPeripheralCleanupLocked(peripheral_outcome,
			peripheral_disposition);
		result = FinishCleanupOperationLocked(result, *video_cleanup_lease_);
		if (result != MISTER_RESULT_OK) {
			if (peripheral_disposition != PeripheralBrokerDisposition::abandoned)
				video_cleanup_lease_.reset();
			return result;
		}
		video_cleanup_lease_.reset();
		ledger_.video_shutdown_complete = true;
		if (!ledger_.coupled_audio_video_active)
			ledger_.resource_flags &= ~MISTER_RESOURCE_NATIVE_VIDEO;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0 &&
		!ledger_.audio_shutdown_complete) {
		result = BeginCleanupOperationLocked(OperationKind::audio,
			&audio_cleanup_lease_);
		if (result != MISTER_RESULT_OK) return result;
		std::unique_ptr<CleanupAudioSessionBundle> bundle;
		result = audio_cleanup_lease_->AcquireCleanupAudioSession(broker_,
			resources_.audio.BackendIdentity(), &bundle);
		NativePeripheralReleaseOutcome peripheral_outcome = {result, false,
			false, false, false};
		if (result == MISTER_RESULT_OK)
			peripheral_outcome = resources_.audio.StopAudio(std::move(bundle));
		PeripheralBrokerDisposition peripheral_disposition =
			PeripheralBrokerDisposition::no_session;
		if (audio_cleanup_lease_->GetAudioSessionDisposition(broker_,
			&peripheral_disposition) != MISTER_RESULT_OK)
			peripheral_outcome.result = MISTER_RESULT_PLATFORM;
		result = FinishPeripheralCleanupLocked(peripheral_outcome,
			peripheral_disposition);
		result = FinishCleanupOperationLocked(result, *audio_cleanup_lease_);
		if (result != MISTER_RESULT_OK) {
			if (peripheral_disposition != PeripheralBrokerDisposition::abandoned)
				audio_cleanup_lease_.reset();
			return result;
		}
		audio_cleanup_lease_.reset();
		ledger_.audio_shutdown_complete = true;
		// A mute/ACK is only a local shutdown fact. AUDIO remains in the
		// ledger until this exact cleanup epoch has terminal containment.
	}
	if (ledger_.coupled_audio_video_active) {
		result = BeginCleanupOperationLocked(OperationKind::audio_video,
			&coupled_audio_video_cleanup_lease_);
		if (result != MISTER_RESULT_OK) return result;
		std::unique_ptr<CleanupAudioVideoSessionBundle> bundle;
		result = coupled_audio_video_cleanup_lease_->AcquireCleanupAudioVideoSession(broker_,
			resources_.audio_video.BackendIdentity(), &bundle);
		NativeCoupledReleaseOutcome coupled_outcome = {result, 0, 0, 0, false,
			false, 0};
		if (result == MISTER_RESULT_OK)
			coupled_outcome = resources_.audio_video.StopAudioVideo(
				std::move(bundle));
		PeripheralBrokerDisposition coupled_disposition =
			PeripheralBrokerDisposition::no_session;
		if (coupled_audio_video_cleanup_lease_->GetAudioVideoSessionDisposition(broker_,
			&coupled_disposition) != MISTER_RESULT_OK)
			coupled_outcome.result = MISTER_RESULT_PLATFORM;
		result = FinishCoupledCleanupLocked(coupled_outcome, coupled_disposition);
		result = FinishCleanupOperationLocked(result,
			*coupled_audio_video_cleanup_lease_);
		if (result != MISTER_RESULT_OK) {
			if (coupled_disposition != PeripheralBrokerDisposition::abandoned)
				coupled_audio_video_cleanup_lease_.reset();
			return result;
		}
		coupled_audio_video_cleanup_lease_.reset();
		ledger_.coupled_audio_video_active = false;
		if ((coupled_outcome.neutral_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0)
			ledger_.resource_flags &= ~MISTER_RESOURCE_NATIVE_VIDEO;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_CONTENT) != 0) {
		result = BeginCleanupOperationLocked(OperationKind::content, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.content.CloseContent(lease->absolute_deadline_ms());
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.resource_flags &= ~MISTER_RESOURCE_CONTENT;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0 &&
		!ledger_.core_protocol_shutdown_complete) {
		result = BeginCleanupOperationLocked(OperationKind::core_protocol,
			&core_protocol_cleanup_lease_);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.hardware.ShutdownCoreProtocol(
			*core_protocol_cleanup_lease_);
		result = FinishCleanupOperationLocked(result,
			*core_protocol_cleanup_lease_);
		if (result != MISTER_RESULT_OK) {
			CoreProtocolBrokerDisposition disposition =
				CoreProtocolBrokerDisposition::no_session;
			if (core_protocol_cleanup_lease_->GetCoreProtocolBrokerDisposition(
				broker_, &disposition) != MISTER_RESULT_OK ||
				disposition != CoreProtocolBrokerDisposition::session_abandoned)
				core_protocol_cleanup_lease_.reset();
			return result;
		}
		ledger_.core_protocol_shutdown_complete = true;
		core_protocol_cleanup_lease_.reset();
	}

	result = BeginCleanupOperationLocked(OperationKind::terminal_fpga_cleanup,
		&lease);
	if (result != MISTER_RESULT_OK) return result;
	result = resources_.hardware.TerminalFpgaCleanup(*lease);
	result = FinishCleanupOperationLocked(result, *lease);
	if (result != MISTER_RESULT_OK) {
		if (broker_.RecordOperationOutcome(*cleanup_invocation_, *lease, result) !=
			MISTER_RESULT_OK)
			result = MISTER_RESULT_PLATFORM;
		lease.reset();
		return result;
	}
	result = broker_.ObserveContainment(*cleanup_epoch_, *lease);
	if (result != MISTER_RESULT_OK) {
		result = CleanupFailure(result);
		broker_.RecordOperationOutcome(*cleanup_invocation_, *lease, result);
		lease.reset();
		return result;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0) {
		result = broker_.PromoteCleanupAudioWithContainment(*cleanup_epoch_,
			*lease);
		if (result != MISTER_RESULT_OK) {
			broker_.RecordOperationOutcome(*cleanup_invocation_, *lease,
				CleanupFailure(result));
			lease.reset();
			return CleanupFailure(result);
		}
		ledger_.resource_flags &= ~MISTER_RESOURCE_NATIVE_AUDIO;
		ledger_.audio_shutdown_complete = false;
	}
	if (broker_.RecordOperationOutcome(*cleanup_invocation_, *lease,
		MISTER_RESULT_OK) != MISTER_RESULT_OK) {
		lease.reset();
		return MISTER_RESULT_PLATFORM;
	}
	lease.reset();
	ledger_.containment_mappings = false;

	ledger_.resource_flags &= ~(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL | MISTER_RESOURCE_CORE_INPUT);
	ledger_.core_protocol_shutdown_complete = false;
	ledger_.video_shutdown_complete = false;
	ledger_.audio_shutdown_complete = false;
	for (size_t player = 0; player < kNativePlayerCount; ++player)
		ledger_.digital_neutral_valid[player] = false;
	state_ = NativeLifecycleState::neutral;
	return MISTER_RESULT_OK;
	};
	Result result = run();
	result = FinishCleanupCallbackLocked(result);
	if (result != MISTER_RESULT_OK) return result;
	result = broker_.Leave(generation_, std::move(cleanup_epoch_));
	if (result != MISTER_RESULT_OK) {
		state_ = NativeLifecycleState::cleanup;
		return CleanupFailure(result);
	}
	ClearGenerationLocked();
	return MISTER_RESULT_OK;
}

void NativeLifecycle::ClearGenerationLocked()
{
	state_ = NativeLifecycleState::idle;
	generation_ = 0;
	ledger_ = EmptyLedger();
	cleanup_timing_ = EmptyCleanupTiming();
	failure_drain_timing_ = EmptyFailureDrainTiming();
	latched_activation_result_ = MISTER_RESULT_OK;
	profile_ = nullptr;
	cleanup_invocation_.reset();
	core_protocol_cleanup_lease_.reset();
	save_cleanup_lease_.reset();
	audio_cleanup_lease_.reset();
	video_cleanup_lease_.reset();
	coupled_audio_video_cleanup_lease_.reset();
}

NativeLifecycleState NativeLifecycle::state() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return state_;
}

PlatformGenerationId NativeLifecycle::generation() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return generation_;
}

NativeResourceLedger NativeLifecycle::ledger() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return ledger_;
}

NativeCleanupTiming NativeLifecycle::cleanup_timing() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return cleanup_timing_;
}

Result NativeLifecycle::latched_activation_result() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return latched_activation_result_;
}

} // namespace native
} // namespace mister
