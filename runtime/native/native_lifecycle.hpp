// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_LIFECYCLE_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_LIFECYCLE_HPP

#include "runtime/native/native_resources.hpp"

#include <stdint.h>

#include <memory>
#include <mutex>

namespace mister {
namespace native {

enum class NativeLifecycleState : uint8_t {
	idle,
	preflight,
	activating,
	active,
	quiescing,
	cleanup,
	neutral
};

struct NativeResourceLedger {
	uint32_t resource_flags;
	bool generation;
	bool scheduler;
	bool offload;
	bool input_descriptors;
	bool core_protocol_shutdown_complete;
	// Mute/close completed locally. This is deliberately distinct from
	// neutral: AUDIO remains represented by resource_flags until containment.
	bool audio_shutdown_complete;
	bool coupled_audio_video_active;
	bool digital_neutral_captured;
	bool digital_neutral_valid[kNativePlayerCount];
	NativeDigitalNeutral digital_neutral[kNativePlayerCount];
};

struct NativeCleanupTiming {
	bool established;
	uint64_t cleanup_start_ms;
	uint64_t non_fpga_deadline_ms;
	uint64_t fpga_deadline_ms;
};

struct NativeFailureDrainTiming {
	bool established;
	uint64_t drain_start_ms;
	uint64_t drain_deadline_ms;
};

class NativeLifecycle final {
public:
	NativeLifecycle(NativeClock &clock, HardwareBroker &broker,
		NativeResourceSet resources);
	~NativeLifecycle();
	NativeLifecycle(const NativeLifecycle &) = delete;
	NativeLifecycle &operator=(const NativeLifecycle &) = delete;

	Result Activate(const NativeCoreProfile &profile,
		uint64_t activation_deadline_ms);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result ActivateFixtureForTest(const NativeCoreProfile &profile,
		uint64_t activation_deadline_ms);
	uint64_t cleanup_epoch_identity_for_test() const;
	NativeFailureDrainTiming failure_drain_timing_for_test() const;
#endif
	Result Stop();

	NativeLifecycleState state() const;
	PlatformGenerationId generation() const;
	NativeResourceLedger ledger() const;
	NativeCleanupTiming cleanup_timing() const;
	Result latched_activation_result() const;

private:
	Result ActivateLocked(const NativeCoreProfile &profile,
		uint64_t activation_deadline_ms, bool fixture);
	Result FinishAcquisitionLocked(NativeAcquisitionOutcome outcome,
		uint64_t resource_flags,
		bool *supporting_ledger, uint64_t activation_deadline_ms);
	Result FinishPeripheralAcquisitionLocked(
		NativePeripheralAcquisitionOutcome outcome,
		PeripheralBrokerDisposition disposition, uint64_t resource_flags,
		uint64_t activation_deadline_ms);
	Result FinishPeripheralCleanupLocked(
		const NativePeripheralReleaseOutcome &outcome,
		PeripheralBrokerDisposition disposition) const;
	Result FinishCoupledAcquisitionLocked(
		NativeCoupledAcquisitionOutcome outcome,
		PeripheralBrokerDisposition disposition,
		uint64_t activation_deadline_ms);
	Result FinishCoupledCleanupLocked(
		const NativeCoupledReleaseOutcome &outcome,
		PeripheralBrokerDisposition disposition) const;
	Result FinishCoreProtocolAcquisitionLocked(
		NativeCoreProtocolOutcome outcome,
		std::unique_ptr<OperationLease> *lease,
		uint64_t activation_deadline_ms);
	Result FailActivationLocked(Result activation_result);
	Result FinishPreownershipContentLocked(Result activation_result);
	void EstablishCleanupTimingLocked();
	void EstablishFailureDrainTimingLocked();
	Result EstablishCleanupLocked();
	Result RunCleanupLocked();
	Result BeginCleanupOperationLocked(OperationKind kind,
		std::unique_ptr<OperationLease> *lease);
	Result FinishCleanupOperationLocked(Result result,
		const OperationLease &lease) const;
	Result CaptureDigitalNeutralLocked(const OperationLease &lease);
	static uint64_t SaturatingAdd(uint64_t value, uint64_t delta);
	static Result CleanupFailure(Result result);
	void ClearGenerationLocked();

	NativeClock &clock_;
	HardwareBroker &broker_;
	NativeResourceSet resources_;
	mutable std::mutex mutex_;
	NativeLifecycleState state_;
	PlatformGenerationId generation_;
	NativeResourceLedger ledger_;
	NativeCleanupTiming cleanup_timing_;
	NativeFailureDrainTiming failure_drain_timing_;
	Result latched_activation_result_;
	const NativeCoreProfile *profile_;
	std::unique_ptr<CleanupEpoch> cleanup_epoch_;
	std::unique_ptr<OperationLease> core_protocol_cleanup_lease_;
	std::unique_ptr<OperationLease> audio_cleanup_lease_;
	std::unique_ptr<OperationLease> video_cleanup_lease_;
	std::unique_ptr<OperationLease> coupled_audio_video_cleanup_lease_;
};

} // namespace native
} // namespace mister

#endif
