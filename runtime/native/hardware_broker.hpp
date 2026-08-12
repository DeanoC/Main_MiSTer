// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_HARDWARE_BROKER_HPP
#define MISTER_RUNTIME_NATIVE_HARDWARE_BROKER_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"

#include <stddef.h>
#include <stdint.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace mister {
namespace native {

struct NativeCoreProfile;

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
struct BrokerLifetime;
struct OperationRegistration;

class HardwareLeaseView final {
public:
	HardwareLeaseView(const HardwareLeaseView &) = delete;
	HardwareLeaseView &operator=(const HardwareLeaseView &) = delete;

private:
	friend class HardwareBroker;
	explicit HardwareLeaseView(
		const std::shared_ptr<OperationRegistration> &registration);

	std::shared_ptr<OperationRegistration> registration_;
};

class OperationLease final {
public:
	~OperationLease();
	OperationLease(const OperationLease &) = delete;
	OperationLease &operator=(const OperationLease &) = delete;
	OperationLease(OperationLease &&) = delete;
	OperationLease &operator=(OperationLease &&) = delete;

	OperationKind operation_kind() const { return operation_kind_; }
	uint64_t absolute_deadline_ms() const;

private:
	friend class HardwareBroker;
	explicit OperationLease(
		const std::shared_ptr<OperationRegistration> &registration);

	std::shared_ptr<OperationRegistration> registration_;
	OperationKind operation_kind_;
};

class CleanupEpoch final {
public:
	~CleanupEpoch();
	CleanupEpoch(const CleanupEpoch &) = delete;
	CleanupEpoch &operator=(const CleanupEpoch &) = delete;
	CleanupEpoch(CleanupEpoch &&) = delete;
	CleanupEpoch &operator=(CleanupEpoch &&) = delete;

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

class HardwareBroker final {
public:
	explicit HardwareBroker(NativeClock &clock);
	~HardwareBroker();
	HardwareBroker(const HardwareBroker &) = delete;
	HardwareBroker &operator=(const HardwareBroker &) = delete;

	Result Enter(const NativeCoreProfile &profile,
		PlatformGenerationId *generation);
	Result Begin(PlatformGenerationId generation, OperationKind operation_kind,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<OperationLease> *lease);
	Result Quiesce(PlatformGenerationId generation,
		uint64_t absolute_deadline_ms);
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

private:
	friend class OperationLease;
	friend class CleanupEpoch;
	friend struct OperationRegistration;

	enum class State : uint8_t {
		idle,
		active,
		quiescing,
		cleanup,
		terminal_neutral
	};

	void ReleaseOperation(OperationRegistration &registration);
	void UnregisterCleanup(CleanupEpoch &epoch);
	bool IsCurrentCleanup(const CleanupEpoch &epoch) const;
	static bool IsHardwareOperation(OperationKind operation_kind);
	static bool CleanupDeadline(OperationKind operation_kind,
		const CleanupEpoch &epoch, uint64_t *absolute_deadline_ms);

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
	size_t active_lease_count_;
	size_t terminal_lease_count_;
	bool cleanup_registered_;
	bool cleanup_ever_started_;
	bool quiesce_complete_;
	bool quiesce_call_active_;
	bool containment_receipt_current_;
};

} // namespace native
} // namespace mister

#endif
