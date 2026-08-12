// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_HARDWARE_BROKER_HPP
#define MISTER_RUNTIME_NATIVE_HARDWARE_BROKER_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_core_profile.hpp"

#include <stddef.h>
#include <stdint.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace mister {
namespace native {

using PlatformGenerationId = uint64_t;
using Result = MisterResult;

enum class LeaseAuthority : uint8_t {
	active_generation,
	cleanup_epoch,
	recovery_epoch
};

enum class OperationKind : uint8_t {
	program_fpga,
	core_protocol,
	input,
	scheduler,
	offload,
	save,
	audio,
	video,
	content,
	input_descriptors,
	terminal_fpga_cleanup
};

class HardwareBroker;
class OperationLease;
class NativeInput;
class NativeSpiBus;
struct BrokerLifetime;
struct OperationRegistration;

class HardwareLeaseView final {
public:
	~HardwareLeaseView();
	HardwareLeaseView(const HardwareLeaseView &) = delete;
	HardwareLeaseView &operator=(const HardwareLeaseView &) = delete;

private:
	friend class HardwareBroker;
	friend class NativeSpiBus;
	explicit HardwareLeaseView(
		const std::shared_ptr<OperationRegistration> &registration);
	uint64_t RecordMutation();
	uint64_t absolute_deadline_ms() const;

	std::shared_ptr<OperationRegistration> registration_;
};

class OperationLease final {
public:
	~OperationLease();
	OperationLease(const OperationLease &) = delete;
	OperationLease &operator=(const OperationLease &) = delete;
	OperationLease(OperationLease &&) = delete;
	OperationLease &operator=(OperationLease &&) = delete;

	OperationKind operation_kind() const;
	uint64_t absolute_deadline_ms() const;

private:
	friend class HardwareBroker;
	friend class NativeInput;
	friend class NativeSpiBus;
	explicit OperationLease(
		const std::shared_ptr<OperationRegistration> &registration);
	Result AcquireHardwareLeaseView(
		std::unique_ptr<HardwareLeaseView> *view) const;
	Result AcquireInputHardwareLeaseView(const NativeCoreProfile &profile,
		std::unique_ptr<HardwareLeaseView> *view) const;

	std::shared_ptr<OperationRegistration> registration_;
};

class CleanupEpoch final {
public:
	~CleanupEpoch();
	CleanupEpoch(const CleanupEpoch &) = delete;
	CleanupEpoch &operator=(const CleanupEpoch &) = delete;
	CleanupEpoch(CleanupEpoch &&) = delete;
	CleanupEpoch &operator=(CleanupEpoch &&) = delete;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	uint64_t identity_for_test() const;
#endif

private:
	friend class HardwareBroker;
	CleanupEpoch(HardwareBroker &broker, PlatformGenerationId generation,
		uint64_t identity, uint64_t non_fpga_deadline_ms,
		uint64_t fpga_deadline_ms,
		const std::shared_ptr<BrokerLifetime> &lifetime);

	HardwareBroker *broker_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	PlatformGenerationId generation_;
	uint64_t identity_;
	uint64_t non_fpga_deadline_ms_;
	uint64_t fpga_deadline_ms_;
	bool registered_;
};

class RecoveryEpoch final {
public:
	~RecoveryEpoch();
	RecoveryEpoch(const RecoveryEpoch &) = delete;
	RecoveryEpoch &operator=(const RecoveryEpoch &) = delete;
	RecoveryEpoch(RecoveryEpoch &&) = delete;
	RecoveryEpoch &operator=(RecoveryEpoch &&) = delete;

private:
	friend class HardwareBroker;
	RecoveryEpoch(HardwareBroker &broker, uint64_t identity,
		uint32_t requested_resource_flags, uint64_t non_fpga_deadline_ms,
		uint64_t fpga_deadline_ms,
		const std::shared_ptr<BrokerLifetime> &lifetime);

	HardwareBroker *broker_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	uint64_t identity_;
	uint32_t requested_resource_flags_;
	uint64_t non_fpga_deadline_ms_;
	uint64_t fpga_deadline_ms_;
	bool registered_;
};

class HardwareBroker final {
public:
	explicit HardwareBroker(NativeClock &clock);
	~HardwareBroker();
	HardwareBroker(const HardwareBroker &) = delete;
	HardwareBroker &operator=(const HardwareBroker &) = delete;

	Result Enter(const NativeCoreProfile &profile,
		PlatformGenerationId *generation);
	Result Enter(const NativeCoreProfile *profile,
		PlatformGenerationId *generation);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result EnterFixtureForTest(const NativeCoreProfile &profile,
		PlatformGenerationId *generation);
	bool has_live_generation_for_test();
#endif
	Result Begin(PlatformGenerationId generation, OperationKind operation_kind,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<OperationLease> *lease);
	Result Quiesce(PlatformGenerationId generation,
		uint64_t absolute_deadline_ms);
	Result LatchFailure(PlatformGenerationId generation);
	Result BeginCleanup(PlatformGenerationId generation,
		uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
		std::unique_ptr<CleanupEpoch> *epoch);
	Result BeginCleanupOperation(const CleanupEpoch &epoch,
		OperationKind operation_kind,
		std::unique_ptr<OperationLease> *lease);
	Result AcquireHardwareLeaseView(const OperationLease &lease,
		std::unique_ptr<HardwareLeaseView> *view);
	Result ObserveContainment(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result Leave(PlatformGenerationId generation,
		std::unique_ptr<CleanupEpoch> &&epoch);
	Result BeginRecovery(uint32_t requested_resource_flags,
		uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
		std::unique_ptr<RecoveryEpoch> *epoch);
	Result BeginRecoveryOperation(const RecoveryEpoch &epoch,
		OperationKind operation_kind,
		std::unique_ptr<OperationLease> *lease);
	Result FinishRecovery(std::unique_ptr<RecoveryEpoch> &&epoch,
		MisterRecoveryObservationV2 *observation);

private:
	friend class OperationLease;
	friend class CleanupEpoch;
	friend class RecoveryEpoch;
	friend class HardwareLeaseView;
	friend class NativeSpiBus;
	friend struct OperationRegistration;

	enum class State : uint8_t {
		idle,
		active,
		quiescing,
		cleanup,
		terminal_neutral,
		recovery
	};

	void ReleaseOperation(OperationRegistration &registration);
	void ReleaseHardwareLeaseView(HardwareLeaseView &view);
	void UnregisterCleanup(CleanupEpoch &epoch);
	void UnregisterRecovery(RecoveryEpoch &epoch);
	bool IsCurrentCleanup(const CleanupEpoch &epoch) const;
	bool IsCurrentRecovery(const RecoveryEpoch &epoch) const;
	static bool IsHardwareOperation(OperationKind operation_kind);
	static bool IsRecoveryOperation(OperationKind operation_kind,
		uint32_t requested_resource_flags);
	static bool CleanupDeadline(OperationKind operation_kind,
		const CleanupEpoch &epoch, uint64_t *absolute_deadline_ms);
	static bool RecoveryDeadline(OperationKind operation_kind,
		const RecoveryEpoch &epoch, uint64_t *absolute_deadline_ms);
	Result AcquireHardwareLeaseViewFor(const OperationLease &lease,
		OperationKind required_operation_kind,
		const NativeCoreProfile *required_profile,
		std::unique_ptr<HardwareLeaseView> *view);
	uint64_t RecordMutation(const HardwareLeaseView &view);

	NativeClock &clock_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	std::mutex mutex_;
	std::condition_variable lease_released_;
	State state_;
	PlatformGenerationId generation_;
	uint64_t cleanup_identity_;
	uint64_t cleanup_non_fpga_deadline_ms_;
	uint64_t cleanup_fpga_deadline_ms_;
	uint64_t terminal_lease_deadline_ms_;
	uint64_t recovery_identity_;
	uint32_t recovery_requested_resource_flags_;
	uint64_t recovery_non_fpga_deadline_ms_;
	uint64_t recovery_fpga_deadline_ms_;
	uint64_t mutation_sequence_;
	size_t active_lease_count_;
	size_t terminal_lease_count_;
	bool cleanup_registered_;
	bool cleanup_ever_started_;
	bool quiesce_complete_;
	bool quiesce_call_active_;
	bool containment_receipt_current_;
	bool recovery_registered_;
	bool hardware_transaction_active_;
	bool failure_latched_;
	const NativeCoreProfile *profile_;
};

} // namespace native
} // namespace mister

#endif
