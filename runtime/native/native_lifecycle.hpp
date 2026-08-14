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
	bool containment_mappings;
	bool scheduler;
	bool offload;
	bool input_descriptors;
	bool core_protocol_shutdown_complete;
	// Standalone video teardown completed locally through a broker-accepted
	// typed completion. VIDEO can remain set while coupled A/V or containment
	// remains.
	bool video_shutdown_complete;
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

#if defined(MISTER_NATIVE_PROFILE_TESTING)
struct NativeCleanupBrokerSnapshot {
	uintptr_t lease_identity;
	uintptr_t registration_identity;
	uintptr_t session_identity;
	PlatformGenerationId broker_generation;
	uint64_t cleanup_identity;
	uint64_t cleanup_non_fpga_deadline_ms;
	uint64_t cleanup_fpga_deadline_ms;
	uint64_t invocation_identity;
	uint64_t invocation_callback_deadline_ms;
	uint64_t session_effective_deadline_ms;
	uint64_t session_initial_mutation_sequence;
	uint64_t session_last_mutation_sequence;
	uint64_t broker_mutation_sequence;
	size_t active_lease_count;
	size_t terminal_lease_count;
	PeripheralSessionKind session_kind;
	PeripheralSessionAction session_action;
	PeripheralSessionPhase session_phase;
	uint8_t action_word_count;
	uint8_t action_next_word_index;
	bool action_transaction_closed;
	bool action_progress_unknown;
	bool recheckout_allowed;
	bool backend_matches_expected;
	bool invocation_registered;
	bool invocation_outcome_missing;
	bool registration_is_suspended;
	bool registration_is_invoked;
	bool cleanup_registered;
	bool hardware_transaction_active;
	bool broker_idle;
};
#endif

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
	NativeCleanupBrokerSnapshot cleanup_broker_snapshot_for_test(
		PeripheralSessionKind kind) const;
	NativeCleanupBrokerSnapshot cleanup_broker_callback_snapshot_for_test(
		PeripheralSessionKind kind) const;
	Result cleanup_retained_snapshot_for_test(OperationKind kind,
		NativeRetainedOperationSnapshot *snapshot) const;
	Result cleanup_retained_callback_snapshot_for_test(OperationKind kind,
		NativeRetainedOperationSnapshot *snapshot) const;
#endif
	Result Stop(uint64_t callback_deadline_ms);

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
	Result FinishSaveAcquisitionLocked(NativeSaveOpenOutcome outcome,
		uint64_t activation_deadline_ms);
	Result FinishSaveCleanupLocked(const NativeSaveCloseOutcome &outcome) const;
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
	Result EstablishCleanupLocked(uint64_t callback_deadline_ms);
	Result RunCleanupLocked(uint64_t callback_deadline_ms);
	Result BeginCleanupOperationLocked(OperationKind kind,
		std::unique_ptr<OperationLease> *lease);
	Result FinishCleanupCallbackLocked(Result result);
	Result FinishCleanupOperationLocked(Result result,
		const OperationLease &lease);
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
	uint64_t callback_deadline_ms_;
	const NativeCoreProfile *profile_;
	std::unique_ptr<CleanupEpoch> cleanup_epoch_;
	std::unique_ptr<OperationInvocation> cleanup_invocation_;
	std::unique_ptr<OperationLease> core_protocol_cleanup_lease_;
	std::unique_ptr<OperationLease> save_cleanup_lease_;
	std::unique_ptr<OperationLease> audio_cleanup_lease_;
	std::unique_ptr<OperationLease> video_cleanup_lease_;
	std::unique_ptr<OperationLease> coupled_audio_video_cleanup_lease_;
};

} // namespace native
} // namespace mister

#endif
