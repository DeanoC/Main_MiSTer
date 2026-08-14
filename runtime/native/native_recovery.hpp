// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_RECOVERY_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_RECOVERY_HPP

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_resources.hpp"

namespace mister {
namespace native {

class NativeContainment;

enum class RecoveryResourceState : uint8_t {
	unknown,
	observed_non_neutral,
	neutral
};

class NativeRecoveryIo {
public:
	virtual ~NativeRecoveryIo() {}
	// Task 10-owned safe recovery authority is injected privately. The base
	// implementation exposes none, so production cannot guess a profile.
	virtual const SafeAudioRecoveryRecord *SafeAudioRecord() const
	{
		return nullptr;
	}
	virtual const SafeVideoRecoveryRecord *SafeVideoRecord() const
	{
		return nullptr;
	}
	virtual const SafeAudioVideoRecoveryRecord *SafeAudioVideoRecord() const
	{
		return nullptr;
	}
	virtual const SafeSaveRecoveryRecord *SafeSaveRecord() const
	{
		return nullptr;
	}

private:
	friend class NativeRecovery;
	virtual Result CloseInputDescriptors(const OperationLease &lease,
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
	NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io,
		NativeAudioResource &audio, NativeVideoResource &video,
		NativeAudioVideoResource &audio_video);
	NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io,
		NativeAudioResource &audio, NativeVideoResource &video,
		NativeAudioVideoResource &audio_video, NativeContainment &containment);
	NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io,
		NativeAudioResource &audio, NativeVideoResource &video,
		NativeAudioVideoResource &audio_video, NativeSaveResource &save);
	NativeRecovery(HardwareBroker &broker, NativeRecoveryIo &io,
		NativeAudioResource &audio, NativeVideoResource &video,
		NativeAudioVideoResource &audio_video, NativeSaveResource &save,
		NativeContainment &containment);
	~NativeRecovery();
	Result Perform(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation, OperationKind operation_kind);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result Perform(const RecoveryEpoch &epoch, OperationKind operation_kind);
	Result retained_snapshot_for_test(const RecoveryEpoch &epoch,
		OperationKind kind, NativeRetainedOperationSnapshot *snapshot) const;
	Result broker_baseline_snapshot_for_test(
		NativeRetainedOperationSnapshot *snapshot) const;
#endif
	Result Snapshot(const RecoveryEpoch &epoch,
		MisterRecoveryObservationV2 *observation) const;
	Result ValidateCallbackRequestedFlags(const RecoveryEpoch &epoch,
		uint32_t callback_requested_flags) const;
	Result Finish(std::unique_ptr<RecoveryEpoch> &&epoch,
		MisterRecoveryObservationV2 *observation);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	OperationLease *retained_lease_for_test() const;
	void clear_retained_recovery_for_test();
#endif

private:
	enum class RetainedRecoveryKind : uint8_t {
		none,
		core_protocol,
		audio,
		video,
		audio_video,
		save
	};
	Result BeginTypedRecovery(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation, OperationKind operation_kind);
	void ClearRetainedRecovery();
	NativeRecovery(const NativeRecovery &) = delete;
	NativeRecovery &operator=(const NativeRecovery &) = delete;

	HardwareBroker &broker_;
	NativeRecoveryIo &io_;
	NativeAudioResource *audio_;
	NativeVideoResource *video_;
	NativeAudioVideoResource *audio_video_;
	NativeSaveResource *save_;
	NativeContainment *containment_;
	const RecoveryEpoch *retained_epoch_;
	RetainedRecoveryKind retained_kind_;
	std::unique_ptr<OperationLease> retained_lease_;
};

} // namespace native
} // namespace mister

#endif
