// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_video_adapter.hpp"

#include "runtime/native/hardware_broker.hpp"

namespace mister {
namespace native {
namespace linux_native {

NativeVideoAdapter::NativeVideoAdapter(NativeClock &clock, HardwareBroker &broker,
	NativeAvIoAdapter &io)
	: broker_(broker), io_(io),
	  backend_(broker_.CreatePeripheralBackendIdentity(this))
{
	(void)clock;
}

NativeVideoAdapter::~NativeVideoAdapter() = default;

PeripheralBackendIdentity NativeVideoAdapter::BackendIdentity() const
{
	return backend_;
}

NativePeripheralAcquisitionOutcome NativeVideoAdapter::StartVideo(
	std::unique_ptr<ActiveVideoSessionBundle> &&bundle)
{
	std::unique_ptr<ActiveVideoSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, false};
	std::unique_ptr<ActiveVideoSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->profile)
		return {MISTER_RESULT_INVALID_ARGUMENT, false};
	PeripheralCompletionReceipt receipt = {};
	const Result result = io_.StartVideo(broker_, *session,
		session->state_->profile->video, &receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.CompleteActiveVideoFailure(std::move(session),
			{result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed,
			receipt.mutation_sequence != 0};
	}
	const Result completed = broker_.CompleteActiveVideoSuccess(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		receipt.mutation_sequence != 0};
}

NativePeripheralReleaseOutcome NativeVideoAdapter::StopVideo(
	std::unique_ptr<CleanupVideoSessionBundle> &&bundle)
{
	std::unique_ptr<CleanupVideoSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	std::unique_ptr<CleanupVideoSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->profile)
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	PeripheralCompletionReceipt receipt = {};
	const Result result = io_.StopVideo(broker_, *session,
		session->state_->profile->video, &receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.AbandonCleanupVideo(std::move(session),
			{result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed, false, false,
			receipt.local_resources_absent, receipt.closure_unknown};
	}
	const Result completed = broker_.CompleteCleanupVideo(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		completed == MISTER_RESULT_OK, completed == MISTER_RESULT_OK,
		receipt.local_resources_absent, receipt.closure_unknown};
}

NativePeripheralReleaseOutcome NativeVideoAdapter::RecoverVideo(
	std::unique_ptr<RecoveryVideoSessionBundle> &&bundle)
{
	std::unique_ptr<RecoveryVideoSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	std::unique_ptr<RecoveryVideoSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->recovery_record)
		return {MISTER_RESULT_INVALID_ARGUMENT, false, false, false, false};
	const SafeVideoRecoveryRecord *record = reinterpret_cast<
		const SafeVideoRecoveryRecord *>(session->state_->recovery_record);
	PeripheralCompletionReceipt receipt = {};
	const Result result = io_.RecoverVideo(broker_, *session, record->video,
		&receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.AbandonRecoveryVideo(std::move(session),
			{result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed, false, false,
			receipt.local_resources_absent, receipt.closure_unknown};
	}
	const Result completed = broker_.CompleteRecoveryVideo(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		completed == MISTER_RESULT_OK, completed == MISTER_RESULT_OK,
		receipt.local_resources_absent, receipt.closure_unknown};
}

void NativeVideoAdapter::CloseVideoForProcessExit() {}

NativeCoupledAcquisitionOutcome NativeVideoAdapter::StartAudioVideo(
	std::unique_ptr<ActiveAudioVideoSessionBundle> &&bundle)
{
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<ActiveAudioVideoSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, 0, false};
	std::unique_ptr<ActiveAudioVideoSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->profile)
		return {MISTER_RESULT_INVALID_ARGUMENT, 0, false};
	CoupledAcquisitionReceipt receipt = {};
	const Result result = io_.StartAudioVideo(broker_, *session,
		session->state_->profile->video, &receipt);
	if (result != MISTER_RESULT_OK) {
		const Result completed = broker_.CompleteActiveAudioVideoFailure(
			std::move(session), {result, receipt});
		return {completed == MISTER_RESULT_OK ? result : completed,
			receipt.affected_flags, receipt.mutation_sequence != 0};
	}
	const Result completed = broker_.CompleteActiveAudioVideoSuccess(
		std::move(session), receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		affected, receipt.mutation_sequence != 0};
}

NativeCoupledReleaseOutcome NativeVideoAdapter::StopAudioVideo(
	std::unique_ptr<CleanupAudioVideoSessionBundle> &&bundle)
{
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<CleanupAudioVideoSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false, false, 0};
	std::unique_ptr<CleanupAudioVideoSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->profile)
		return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false, false, 0};
	CoupledCompletionReceipt receipt = {};
	const Result result = io_.StopAudioVideo(broker_, *session,
		session->state_->profile->video, &receipt);
	if (result != MISTER_RESULT_OK) {
		const CoupledAcquisitionReceipt release = {result, affected,
			receipt.transaction_closed,
			receipt.local_resources_absent, receipt.closure_unknown,
			receipt.mutation_sequence, receipt.transaction_residue};
		const Result completed = broker_.AbandonCleanupAudioVideo(std::move(session),
			{result, release});
		return {completed == MISTER_RESULT_OK ? result : completed, affected,
			receipt.observed_flags, receipt.neutral_flags,
			receipt.local_resources_absent, receipt.closure_unknown,
			receipt.mutation_sequence};
	}
	const Result completed = broker_.CompleteCleanupAudioVideo(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		affected, receipt.observed_flags, receipt.neutral_flags,
		receipt.local_resources_absent, receipt.closure_unknown,
		receipt.mutation_sequence};
}

NativeCoupledReleaseOutcome NativeVideoAdapter::RecoverAudioVideo(
	std::unique_ptr<RecoveryAudioVideoSessionBundle> &&bundle)
{
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryAudioVideoSessionBundle> owned = std::move(bundle);
	if (!owned || !owned->session_ || !owned->session_->state_ ||
		!owned->backend_.Matches(backend_) ||
		!owned->session_->state_->backend.Matches(owned->backend_))
		return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false, false, 0};
	std::unique_ptr<RecoveryAudioVideoSession> session = std::move(owned->session_);
	if (!session || !session->state_ || !session->state_->recovery_record)
		return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false, false, 0};
	const SafeAudioVideoRecoveryRecord *record = reinterpret_cast<
		const SafeAudioVideoRecoveryRecord *>(session->state_->recovery_record);
	CoupledRecoveryReceipt receipt = {};
	const Result result = io_.RecoverAudioVideo(broker_, *session,
		record->video, &receipt);
	if (result != MISTER_RESULT_OK) {
		const CoupledAcquisitionReceipt release = {result, affected,
			receipt.transaction_closed,
			receipt.local_resources_absent, receipt.closure_unknown,
			receipt.mutation_sequence, receipt.transaction_residue};
		const Result completed = broker_.AbandonRecoveryAudioVideo(std::move(session),
			{result, release});
		return {completed == MISTER_RESULT_OK ? result : completed, affected,
			receipt.observed_flags, receipt.neutral_flags,
			receipt.local_resources_absent, receipt.closure_unknown,
			receipt.mutation_sequence};
	}
	const Result completed = broker_.CompleteRecoveryAudioVideo(std::move(session),
		receipt);
	return {completed == MISTER_RESULT_OK ? MISTER_RESULT_OK : completed,
		affected, receipt.observed_flags, receipt.neutral_flags,
		receipt.local_resources_absent, receipt.closure_unknown,
		receipt.mutation_sequence};
}

void NativeVideoAdapter::CloseAudioVideoForProcessExit() {}

} // namespace linux_native
} // namespace native
} // namespace mister
