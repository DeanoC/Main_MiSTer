// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_recovery.hpp"

namespace mister {
namespace native {

NativeRecovery::NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io)
	: broker_(broker), io_(io), core_protocol_epoch_(nullptr),
	  core_protocol_lease_()
{
}

Result NativeRecovery::Perform(const RecoveryEpoch &epoch,
	OperationKind operation_kind)
{
	if (operation_kind == OperationKind::terminal_fpga_cleanup)
		return MISTER_RESULT_INVALID_STATE;
	if (operation_kind == OperationKind::core_protocol) {
		if (core_protocol_lease_) {
			if (core_protocol_epoch_ != &epoch)
				return MISTER_RESULT_INVALID_STATE;
		} else {
			const Result begin = broker_.BeginRecoveryOperation(epoch,
				operation_kind, &core_protocol_lease_);
			if (begin != MISTER_RESULT_OK) {
				if (begin == MISTER_RESULT_DEADLINE)
					broker_.RecordRecoveryFailure(epoch, begin);
				return begin;
			}
			core_protocol_epoch_ = &epoch;
		}
		RecoveryResourceState state = RecoveryResourceState::unknown;
		const Result result = io_.DisableCoreProtocol(*core_protocol_lease_,
			&state);
		// A failed mapping release has an abandoned typed session with the
		// original registration still fenced in the broker. Do not classify or
		// release it: the same epoch must retry that exact registration.
		if (result != MISTER_RESULT_OK) return result;
		if (state != RecoveryResourceState::neutral)
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result recorded = broker_.RecordRecoveryOperation(epoch,
			*core_protocol_lease_, state, result);
		if (recorded == MISTER_RESULT_OK) {
			core_protocol_lease_.reset();
			core_protocol_epoch_ = nullptr;
		}
		return recorded;
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

Result NativeRecovery::Finish(std::unique_ptr<RecoveryEpoch> &&epoch,
	MisterRecoveryObservationV2 *observation)
{
	if (core_protocol_lease_) return MISTER_RESULT_INVALID_STATE;
	return broker_.FinishRecovery(std::move(epoch), observation);
}

} // namespace native
} // namespace mister
