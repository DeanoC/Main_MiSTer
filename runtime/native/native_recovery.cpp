// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_recovery.hpp"

#include "runtime/native/native_containment.hpp"

namespace mister {
namespace native {

NativeRecovery::NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io)
	: broker_(broker), io_(io), audio_(nullptr), video_(nullptr),
	  audio_video_(nullptr), containment_(nullptr), retained_epoch_(nullptr),
	  retained_kind_(RetainedRecoveryKind::none), retained_lease_()
{
}

NativeRecovery::NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io,
	NativeAudioResource &audio, NativeVideoResource &video,
	NativeAudioVideoResource &audio_video)
	: broker_(broker), io_(io), audio_(&audio), video_(&video),
	  audio_video_(&audio_video), containment_(nullptr), retained_epoch_(nullptr),
	  retained_kind_(RetainedRecoveryKind::none), retained_lease_()
{
}

NativeRecovery::NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io,
	NativeAudioResource &audio, NativeVideoResource &video,
	NativeAudioVideoResource &audio_video, NativeContainment &containment)
	: broker_(broker), io_(io), audio_(&audio), video_(&video),
	  audio_video_(&audio_video), containment_(&containment),
	  retained_epoch_(nullptr), retained_kind_(RetainedRecoveryKind::none),
	  retained_lease_()
{
}

Result NativeRecovery::BeginTypedRecovery(const RecoveryEpoch &epoch,
	OperationKind operation_kind)
{
	const RetainedRecoveryKind kind = operation_kind == OperationKind::audio ?
		RetainedRecoveryKind::audio : operation_kind == OperationKind::video ?
		RetainedRecoveryKind::video : operation_kind == OperationKind::audio_video ?
		RetainedRecoveryKind::audio_video : RetainedRecoveryKind::none;
	if (kind == RetainedRecoveryKind::none) return MISTER_RESULT_INVALID_ARGUMENT;
	if (retained_kind_ != RetainedRecoveryKind::none) {
		return retained_epoch_ == &epoch && retained_kind_ == kind &&
			retained_lease_ ? MISTER_RESULT_OK : MISTER_RESULT_INVALID_STATE;
	}
	const Result begin = broker_.BeginRecoveryOperation(epoch, operation_kind,
		&retained_lease_);
	if (begin != MISTER_RESULT_OK) {
		if (begin == MISTER_RESULT_DEADLINE)
			broker_.RecordRecoveryFailure(epoch, begin);
		return begin;
	}
	retained_epoch_ = &epoch;
	retained_kind_ = kind;
	return MISTER_RESULT_OK;
}

void NativeRecovery::ClearRetainedRecovery()
{
	retained_lease_.reset();
	retained_epoch_ = nullptr;
	retained_kind_ = RetainedRecoveryKind::none;
}

Result NativeRecovery::Perform(const RecoveryEpoch &epoch,
	OperationKind operation_kind)
{
	if (operation_kind == OperationKind::terminal_fpga_cleanup) {
		if (containment_ == nullptr || retained_kind_ != RetainedRecoveryKind::none)
			return MISTER_RESULT_INVALID_STATE;
		std::unique_ptr<OperationLease> terminal;
		const Result begin = broker_.BeginRecoveryOperation(epoch, operation_kind,
			&terminal);
		if (begin != MISTER_RESULT_OK) return begin;
		return containment_->ResetAndContain(epoch, *terminal);
	}
	MisterRecoveryObservationV2 snapshot = {};
	const Result snapshot_result = broker_.SnapshotRecovery(epoch, &snapshot);
	if (snapshot_result != MISTER_RESULT_OK &&
		snapshot_result != MISTER_RESULT_CLEANUP_INCOMPLETE)
		return snapshot_result;
	if (retained_kind_ != RetainedRecoveryKind::none &&
		(retained_epoch_ != &epoch ||
		 (operation_kind == OperationKind::core_protocol ?
		  RetainedRecoveryKind::core_protocol : operation_kind == OperationKind::audio ?
		  RetainedRecoveryKind::audio : operation_kind == OperationKind::video ?
		  RetainedRecoveryKind::video : operation_kind == OperationKind::audio_video ?
		  RetainedRecoveryKind::audio_video : RetainedRecoveryKind::none) !=
			retained_kind_))
		return MISTER_RESULT_INVALID_STATE;
	if (operation_kind == OperationKind::core_protocol) {
		if (!retained_lease_) {
			const Result begin = broker_.BeginRecoveryOperation(epoch, operation_kind,
				&retained_lease_);
			if (begin != MISTER_RESULT_OK) {
				if (begin == MISTER_RESULT_DEADLINE)
					broker_.RecordRecoveryFailure(epoch, begin);
				return begin;
			}
			retained_epoch_ = &epoch;
			retained_kind_ = RetainedRecoveryKind::core_protocol;
		}
		RecoveryResourceState state = RecoveryResourceState::unknown;
		const Result result = io_.DisableCoreProtocol(*retained_lease_,
			&state);
		// A failed mapping release has an abandoned typed session with the
		// original registration still fenced in the broker. Do not classify or
		// release it: the same epoch must retry that exact registration.
		if (result != MISTER_RESULT_OK) return result;
		if (state != RecoveryResourceState::neutral)
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result recorded = broker_.RecordRecoveryOperation(epoch,
			*retained_lease_, state, result);
		if (recorded == MISTER_RESULT_OK) {
			ClearRetainedRecovery();
		}
		return recorded;
	}
	if (operation_kind == OperationKind::audio && audio_ != nullptr) {
		const SafeAudioRecoveryRecord *record = io_.SafeAudioRecord();
		if (record == nullptr) return MISTER_RESULT_UNSUPPORTED;
		Result result = BeginTypedRecovery(epoch, operation_kind);
		if (result != MISTER_RESULT_OK) return result;
		std::unique_ptr<RecoveryAudioSessionBundle> bundle;
		result = retained_lease_->AcquireRecoveryAudioSession(broker_, *record,
			audio_->BackendIdentity(), &bundle);
		if (result != MISTER_RESULT_OK) {
			PeripheralBrokerDisposition disposition =
				PeripheralBrokerDisposition::no_session;
			if (retained_lease_->GetAudioSessionDisposition(broker_,
				&disposition) == MISTER_RESULT_OK &&
				disposition == PeripheralBrokerDisposition::abandoned)
				return MISTER_RESULT_CLEANUP_INCOMPLETE;
			return result;
		}
		const NativePeripheralReleaseOutcome outcome = audio_->RecoverAudio(
			std::move(bundle));
		PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::no_session;
		if (retained_lease_->GetAudioSessionDisposition(broker_, &disposition) !=
			MISTER_RESULT_OK) return MISTER_RESULT_PLATFORM;
		if (outcome.result != MISTER_RESULT_OK ||
			disposition != PeripheralBrokerDisposition::success_completed)
			return outcome.result == MISTER_RESULT_OK ?
				MISTER_RESULT_CLEANUP_INCOMPLETE : outcome.result;
		result = broker_.RecordRecoveryOperation(epoch, *retained_lease_,
			outcome.stable_neutral_observed ? RecoveryResourceState::neutral :
			RecoveryResourceState::unknown, outcome.result);
		if (result == MISTER_RESULT_OK) ClearRetainedRecovery();
		return result;
	}
	if (operation_kind == OperationKind::video && video_ != nullptr) {
		const SafeVideoRecoveryRecord *record = io_.SafeVideoRecord();
		if (record == nullptr) return MISTER_RESULT_UNSUPPORTED;
		Result result = BeginTypedRecovery(epoch, operation_kind);
		if (result != MISTER_RESULT_OK) return result;
		std::unique_ptr<RecoveryVideoSessionBundle> bundle;
		result = retained_lease_->AcquireRecoveryVideoSession(broker_, *record,
			video_->BackendIdentity(), &bundle);
		if (result != MISTER_RESULT_OK) {
			PeripheralBrokerDisposition disposition =
				PeripheralBrokerDisposition::no_session;
			if (retained_lease_->GetVideoSessionDisposition(broker_,
				&disposition) == MISTER_RESULT_OK &&
				disposition == PeripheralBrokerDisposition::abandoned)
				return MISTER_RESULT_CLEANUP_INCOMPLETE;
			return result;
		}
		const NativePeripheralReleaseOutcome outcome = video_->RecoverVideo(
			std::move(bundle));
		PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::no_session;
		if (retained_lease_->GetVideoSessionDisposition(broker_, &disposition) !=
			MISTER_RESULT_OK) return MISTER_RESULT_PLATFORM;
		if (outcome.result != MISTER_RESULT_OK ||
			disposition != PeripheralBrokerDisposition::success_completed)
			return outcome.result == MISTER_RESULT_OK ?
				MISTER_RESULT_CLEANUP_INCOMPLETE : outcome.result;
		result = broker_.RecordRecoveryOperation(epoch, *retained_lease_,
			outcome.stable_neutral_observed ? RecoveryResourceState::neutral :
			RecoveryResourceState::unknown, outcome.result);
		if (result == MISTER_RESULT_OK) ClearRetainedRecovery();
		return result;
	}
	if (operation_kind == OperationKind::audio_video) {
		if (audio_video_ == nullptr) return MISTER_RESULT_UNSUPPORTED;
		const SafeAudioVideoRecoveryRecord *record = io_.SafeAudioVideoRecord();
		if (record == nullptr) return MISTER_RESULT_UNSUPPORTED;
		Result result = BeginTypedRecovery(epoch, operation_kind);
		if (result != MISTER_RESULT_OK) return result;
		std::unique_ptr<RecoveryAudioVideoSessionBundle> bundle;
		result = retained_lease_->AcquireRecoveryAudioVideoSession(broker_, *record,
			audio_video_->BackendIdentity(), &bundle);
		if (result != MISTER_RESULT_OK) {
			PeripheralBrokerDisposition disposition =
				PeripheralBrokerDisposition::no_session;
			if (retained_lease_->GetAudioVideoSessionDisposition(broker_,
				&disposition) == MISTER_RESULT_OK &&
				disposition == PeripheralBrokerDisposition::abandoned)
				return MISTER_RESULT_CLEANUP_INCOMPLETE;
			return result;
		}
		const NativeCoupledReleaseOutcome outcome =
			audio_video_->RecoverAudioVideo(std::move(bundle));
		PeripheralBrokerDisposition disposition = PeripheralBrokerDisposition::no_session;
		if (retained_lease_->GetAudioVideoSessionDisposition(broker_,
			&disposition) != MISTER_RESULT_OK) return MISTER_RESULT_PLATFORM;
		if (outcome.result != MISTER_RESULT_OK ||
			disposition != PeripheralBrokerDisposition::success_completed)
			return outcome.result == MISTER_RESULT_OK ?
				MISTER_RESULT_CLEANUP_INCOMPLETE : outcome.result;
		const CoupledRecoveryReceipt receipt = {outcome.result,
			outcome.affected_flags, outcome.observed_flags, outcome.neutral_flags,
			outcome.local_resources_absent, outcome.closure_unknown,
			outcome.mutation_sequence, outcome.local_resources_absent,
			0};
		result = broker_.RecordCoupledRecoveryOperation(epoch, *retained_lease_,
			receipt, outcome.result);
		if (result == MISTER_RESULT_OK) ClearRetainedRecovery();
		return result;
	}
	std::unique_ptr<OperationLease> lease;
	Result result = broker_.BeginRecoveryOperation(epoch, operation_kind,
		&lease);
	if (result != MISTER_RESULT_OK) {
		if (result == MISTER_RESULT_DEADLINE)
			broker_.RecordRecoveryFailure(epoch, result);
		return result;
	}
	RecoveryResourceState state = RecoveryResourceState::unknown;
	switch (operation_kind) {
	case OperationKind::input_descriptors:
		result = io_.CloseInputDescriptors(*lease, &state);
		break;
	case OperationKind::save:
		result = io_.FlushAndCloseSave(*lease, &state);
		break;
	case OperationKind::audio:
		result = io_.MuteAudio(*lease, &state);
		break;
	case OperationKind::video:
		result = io_.PowerDownVideo(*lease, &state);
		break;
	case OperationKind::audio_video:
		return MISTER_RESULT_UNSUPPORTED;
	case OperationKind::content:
		result = io_.CloseContent(*lease, &state);
		break;
	case OperationKind::core_protocol:
		return MISTER_RESULT_INVALID_STATE;
	case OperationKind::program_fpga:
	case OperationKind::input:
	case OperationKind::scheduler:
	case OperationKind::offload:
	case OperationKind::terminal_fpga_cleanup:
		return MISTER_RESULT_INVALID_STATE;
	}
	return broker_.RecordRecoveryOperation(epoch, *lease, state, result);
}

Result NativeRecovery::Snapshot(const RecoveryEpoch &epoch,
	MisterRecoveryObservationV2 *observation) const
{
	return broker_.SnapshotRecovery(epoch, observation);
}

Result NativeRecovery::ValidateCallbackRequestedFlags(const RecoveryEpoch &epoch,
	uint32_t callback_requested_flags) const
{
	return broker_.ValidateRecoveryRequestedFlags(epoch,
		callback_requested_flags);
}

Result NativeRecovery::Finish(std::unique_ptr<RecoveryEpoch> &&epoch,
	MisterRecoveryObservationV2 *observation)
{
	if (!epoch) return MISTER_RESULT_INVALID_ARGUMENT;
	if (retained_kind_ != RetainedRecoveryKind::none)
		return MISTER_RESULT_INVALID_STATE;
	if (!broker_.CanFinishRecovery(*epoch))
		return MISTER_RESULT_CLEANUP_INCOMPLETE;
	return broker_.FinishRecovery(std::move(epoch), observation);
}

} // namespace native
} // namespace mister
