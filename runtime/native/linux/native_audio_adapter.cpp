// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_audio_adapter.hpp"

#include "runtime/native/hardware_broker.hpp"

namespace mister {
namespace native {
namespace linux_native {

NativeAudioAdapter::NativeAudioAdapter(NativeClock &clock, HardwareBroker &broker,
	NativeAvIoAdapter &io)
	: broker_(broker), io_(io),
	  backend_(broker_.CreatePeripheralBackendIdentity(this))
{
	(void)clock;
}

NativeAudioAdapter::~NativeAudioAdapter() = default;

PeripheralBackendIdentity NativeAudioAdapter::BackendIdentity() const
{
	return backend_;
}

NativePeripheralAcquisitionOutcome NativeAudioAdapter::StartAudio(
	std::unique_ptr<ActiveAudioSessionBundle> &&bundle)
{
	std::unique_ptr<ActiveAudioSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, false};
	std::unique_ptr<ActiveAudioSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->profile)
		return {MISTER_RESULT_INVALID_ARGUMENT, false};
	PeripheralCompletionReceipt receipt = {};
	const Result result = io_.StartAudio(broker_, *session,
		session->state_->profile->audio, &receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.CompleteActiveAudioFailure(std::move(session),
			{result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed,
			receipt.mutation_sequence != 0};
	}
	const Result completed = broker_.CompleteActiveAudioSuccess(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		receipt.mutation_sequence != 0};
}

NativePeripheralReleaseOutcome NativeAudioAdapter::StopAudio(
	std::unique_ptr<CleanupAudioSessionBundle> &&bundle)
{
	std::unique_ptr<CleanupAudioSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	std::unique_ptr<CleanupAudioSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->profile)
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	PeripheralCompletionReceipt receipt = {};
	const Result result = io_.StopAudio(broker_, *session,
		session->state_->profile->audio, &receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.AbandonCleanupAudio(std::move(session),
			{result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed, false, false,
			receipt.local_resources_absent, receipt.closure_unknown};
	}
	const Result completed = broker_.CompleteCleanupAudio(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		completed == MISTER_RESULT_OK, false, receipt.local_resources_absent,
		receipt.closure_unknown};
}

NativePeripheralReleaseOutcome NativeAudioAdapter::RecoverAudio(
	std::unique_ptr<RecoveryAudioSessionBundle> &&bundle)
{
	std::unique_ptr<RecoveryAudioSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	std::unique_ptr<RecoveryAudioSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->recovery_record)
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	// The broker admitted this state only through the typed audio-recovery
	// overload, whose record embeds its immutable base as the first member.
	const SafeAudioRecoveryRecord *record = reinterpret_cast<
		const SafeAudioRecoveryRecord *>(session->state_->recovery_record);
	PeripheralCompletionReceipt receipt = {};
	const Result result = io_.RecoverAudio(broker_, *session, record->audio,
		&receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.AbandonRecoveryAudio(std::move(session),
			{result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed, false, false,
			receipt.local_resources_absent, receipt.closure_unknown};
	}
	const Result completed = broker_.CompleteRecoveryAudio(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		completed == MISTER_RESULT_OK, false, receipt.local_resources_absent,
		receipt.closure_unknown};
}

void NativeAudioAdapter::CloseAudioForProcessExit() {}

} // namespace linux_native
} // namespace native
} // namespace mister
