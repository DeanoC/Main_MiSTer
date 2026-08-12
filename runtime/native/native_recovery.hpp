// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_RECOVERY_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_RECOVERY_HPP

#include "runtime/native/hardware_broker.hpp"

namespace mister {
namespace native {

enum class RecoveryResourceState : uint8_t {
	unknown,
	observed_non_neutral,
	neutral
};

class NativeRecoveryIo {
public:
	virtual ~NativeRecoveryIo() {}

private:
	friend class NativeRecovery;
	virtual Result CloseInputDescriptors(const OperationLease &lease,
		RecoveryResourceState *state) = 0;
	virtual Result FlushAndCloseSave(const OperationLease &lease,
		RecoveryResourceState *state) = 0;
	virtual Result MuteAudio(const OperationLease &lease,
		RecoveryResourceState *state) = 0;
	virtual Result PowerDownVideo(const OperationLease &lease,
		RecoveryResourceState *state) = 0;
	virtual Result CloseContent(const OperationLease &lease,
		RecoveryResourceState *state) = 0;
	virtual Result DisableCoreProtocol(const OperationLease &lease,
		RecoveryResourceState *state) = 0;
};

class NativeRecovery final {
public:
	NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io);
	Result Perform(const RecoveryEpoch &epoch, OperationKind operation_kind);
	Result Finish(std::unique_ptr<RecoveryEpoch> &&epoch,
		MisterRecoveryObservationV2 *observation);

private:
	NativeRecovery(const NativeRecovery &) = delete;
	NativeRecovery &operator=(const NativeRecovery &) = delete;

	HardwareBroker &broker_;
	NativeRecoveryIo &io_;
};

} // namespace native
} // namespace mister

#endif
