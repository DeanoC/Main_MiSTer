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
		0, false, false, false, false, false, false,
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
	  latched_activation_result_(MISTER_RESULT_OK), profile_(nullptr),
	  cleanup_epoch_()
{
}

NativeLifecycle::~NativeLifecycle()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ == NativeLifecycleState::active && generation_ != 0)
		broker_.LatchFailure(generation_);
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
		resources_.hardware.CloseVideoForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0)
		resources_.hardware.CloseAudioForProcessExit();
	if ((ledger_.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0)
		resources_.hardware.CloseCoreProtocolForProcessExit();
	if ((ledger_.resource_flags & (MISTER_RESOURCE_FPGA |
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
#endif

Result NativeLifecycle::ActivateLocked(const NativeCoreProfile &profile,
	uint64_t activation_deadline_ms, bool fixture)
{
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
	outcome = {result, false};
	if (result == MISTER_RESULT_OK) outcome = resources_.hardware.StartVideo(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_NATIVE_VIDEO,
		nullptr, activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::audio,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK) outcome = resources_.hardware.StartAudio(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_NATIVE_AUDIO,
		nullptr, activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::input_descriptors,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK)
		outcome = resources_.input_descriptors.OpenInputDescriptors(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_CORE_INPUT,
		&ledger_.input_descriptors, activation_deadline_ms);
	if (result != MISTER_RESULT_OK) return result;

	result = broker_.Begin(generation_, OperationKind::save,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK) outcome = resources_.save.OpenSave(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, MISTER_RESOURCE_SAVES, nullptr,
		activation_deadline_ms);
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

	result = broker_.Begin(generation_, OperationKind::offload,
		activation_deadline_ms, &lease);
	outcome = {result, false};
	if (result == MISTER_RESULT_OK) outcome = resources_.offload.StartOffload(*lease);
	lease.reset();
	result = FinishAcquisitionLocked(outcome, 0, &ledger_.offload,
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
	lease->reset();
	const Result quiesce = broker_.Quiesce(generation_,
		failure_drain_timing_.drain_deadline_ms);
	if (quiesce != MISTER_RESULT_OK) return activation_result;
	state_ = NativeLifecycleState::cleanup;
	if (EstablishCleanupLocked() == MISTER_RESULT_OK) RunCleanupLocked();
	return activation_result;
}

Result NativeLifecycle::FailActivationLocked(Result activation_result)
{
	latched_activation_result_ = activation_result;
	state_ = NativeLifecycleState::quiescing;
	broker_.LatchFailure(generation_);
	state_ = NativeLifecycleState::cleanup;
	if (EstablishCleanupLocked() == MISTER_RESULT_OK) RunCleanupLocked();
	return activation_result;
}

Result NativeLifecycle::FinishPreownershipContentLocked(Result activation_result)
{
	latched_activation_result_ = activation_result;
	if ((ledger_.resource_flags & MISTER_RESOURCE_CONTENT) != 0) {
		EstablishCleanupTimingLocked();
		const Result close = resources_.content.CloseContent(
			cleanup_timing_.non_fpga_deadline_ms);
		if (close != MISTER_RESULT_OK ||
			clock_.NowMs() >= cleanup_timing_.non_fpga_deadline_ms) {
			state_ = NativeLifecycleState::cleanup;
			return activation_result;
		}
		ledger_.resource_flags &= ~MISTER_RESOURCE_CONTENT;
	}
	state_ = NativeLifecycleState::idle;
	cleanup_timing_ = EmptyCleanupTiming();
	return activation_result;
}

Result NativeLifecycle::Stop()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ == NativeLifecycleState::idle) return MISTER_RESULT_OK;
	if (state_ == NativeLifecycleState::cleanup && generation_ == 0) {
		if ((ledger_.resource_flags & MISTER_RESOURCE_CONTENT) == 0) {
			state_ = NativeLifecycleState::idle;
			return MISTER_RESULT_OK;
		}
		if (!cleanup_timing_.established) EstablishCleanupTimingLocked();
		const Result result = resources_.content.CloseContent(
			cleanup_timing_.non_fpga_deadline_ms);
		if (result != MISTER_RESULT_OK) return CleanupFailure(result);
		if (clock_.NowMs() >= cleanup_timing_.non_fpga_deadline_ms)
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
		const uint64_t quiesce_deadline = failure_drain_timing_.established ?
			failure_drain_timing_.drain_deadline_ms :
			SaturatingAdd(clock_.NowMs(), 2000);
		const Result quiesce = broker_.Quiesce(generation_, quiesce_deadline);
		if (quiesce != MISTER_RESULT_OK) return CleanupFailure(quiesce);
		state_ = NativeLifecycleState::cleanup;
	}
	const Result establish = EstablishCleanupLocked();
	if (establish != MISTER_RESULT_OK) return CleanupFailure(establish);
	return RunCleanupLocked();
}

Result NativeLifecycle::EstablishCleanupLocked()
{
	EstablishCleanupTimingLocked();
	if (cleanup_epoch_) return MISTER_RESULT_OK;
	return broker_.BeginCleanup(generation_,
		cleanup_timing_.non_fpga_deadline_ms,
		cleanup_timing_.fpga_deadline_ms, &cleanup_epoch_);
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
	const Result result = broker_.BeginCleanupOperation(*cleanup_epoch_, kind, lease);
	return result == MISTER_RESULT_OK ? MISTER_RESULT_OK : CleanupFailure(result);
}

Result NativeLifecycle::FinishCleanupOperationLocked(Result result,
	const OperationLease &lease) const
{
	if (result != MISTER_RESULT_OK) return CleanupFailure(result);
	return clock_.NowMs() >= lease.absolute_deadline_ms() ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
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

Result NativeLifecycle::RunCleanupLocked()
{
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
		result = BeginCleanupOperationLocked(OperationKind::save, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.save.FlushAndCloseSave(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
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
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0) {
		result = BeginCleanupOperationLocked(OperationKind::video, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.hardware.StopVideo(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.resource_flags &= ~MISTER_RESOURCE_NATIVE_VIDEO;
	}
	if ((ledger_.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0) {
		result = BeginCleanupOperationLocked(OperationKind::audio, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.hardware.StopAudio(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.resource_flags &= ~MISTER_RESOURCE_NATIVE_AUDIO;
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
		result = BeginCleanupOperationLocked(OperationKind::core_protocol, &lease);
		if (result != MISTER_RESULT_OK) return result;
		result = resources_.hardware.ShutdownCoreProtocol(*lease);
		result = FinishCleanupOperationLocked(result, *lease);
		lease.reset();
		if (result != MISTER_RESULT_OK) return result;
		ledger_.core_protocol_shutdown_complete = true;
	}

	result = BeginCleanupOperationLocked(OperationKind::terminal_fpga_cleanup,
		&lease);
	if (result != MISTER_RESULT_OK) return result;
	result = resources_.hardware.TerminalFpgaCleanup(*lease);
	result = FinishCleanupOperationLocked(result, *lease);
	if (result != MISTER_RESULT_OK) {
		lease.reset();
		return result;
	}
	result = broker_.ObserveContainment(*cleanup_epoch_, *lease);
	lease.reset();
	if (result != MISTER_RESULT_OK) return CleanupFailure(result);

	ledger_.resource_flags &= ~(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL | MISTER_RESOURCE_CORE_INPUT);
	ledger_.core_protocol_shutdown_complete = false;
	for (size_t player = 0; player < kNativePlayerCount; ++player)
		ledger_.digital_neutral_valid[player] = false;
	state_ = NativeLifecycleState::neutral;
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
