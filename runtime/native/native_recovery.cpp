// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_recovery.hpp"

namespace mister {
namespace native {

NativeRecovery::NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io)
	: broker_(broker), io_(io)
{
}

Result NativeRecovery::Perform(const RecoveryEpoch &epoch,
	OperationKind operation_kind)
{
	if (operation_kind == OperationKind::terminal_fpga_cleanup)
		return MISTER_RESULT_INVALID_STATE;
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
		result = io_.DisableCoreProtocol(*lease, &state);
		break;
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
	return broker_.FinishRecovery(std::move(epoch), observation);
}

} // namespace native
} // namespace mister
