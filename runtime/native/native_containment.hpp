// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CONTAINMENT_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CONTAINMENT_HPP

#include "runtime/native/hardware_broker.hpp"

#include <stdint.h>

namespace mister {
namespace native {

struct NativeMappingAcquisitionReceipt {
	Result result;
	bool acquired;
	bool complete;
};

struct NativeBridgeEnableReceipt {
	Result result;
	bool acquired;
	bool mutation_applied;
	bool sdr_ports_observed;
	bool bridge_release_observed;
	bool remap_observed;
	bool core_normal_write_attempted;
	bool core_normal_observed;
	uint32_t observed_core_gpo;
	uint64_t mutation_sequence;
};

struct NativeManagerNeutralReceipt {
	Result result;
	uint32_t observed_control;
	uint32_t observed_mode;
	bool neutral_observed;
	bool mutation_attempted;
	bool mutation_applied;
};

class NativeContainmentIo {
public:
	class Access final {
	public:
		~Access() {}
		Access(const Access &) = delete;
		Access &operator=(const Access &) = delete;
		uint64_t absolute_deadline_ms() const
		{
			return absolute_deadline_ms_;
		}

	private:
		friend class NativeContainment;
		explicit Access(uint64_t absolute_deadline_ms)
			: absolute_deadline_ms_(absolute_deadline_ms) {}
		uint64_t absolute_deadline_ms_;
	};

	virtual ~NativeContainmentIo() {}

private:
	friend class NativeContainment;
	virtual NativeMappingAcquisitionReceipt AcquireMappings(
		const Access &)
	{
		const NativeMappingAcquisitionReceipt receipt = {
			MISTER_RESULT_UNSUPPORTED, false, false};
		return receipt;
	}
	virtual NativeBridgeEnableReceipt EnableBridges(const Access &)
	{
		const NativeBridgeEnableReceipt receipt = {
			MISTER_RESULT_UNSUPPORTED, false, false, false, false, false, false, false,
			0, 0};
		return receipt;
	}
	virtual NativeManagerNeutralReceipt ReconcileManager(const Access &)
	{
		const NativeManagerNeutralReceipt receipt = {
			MISTER_RESULT_UNSUPPORTED, 0, 0, false, false, false};
		return receipt;
	}
	virtual Result ReadManagerControl(const Access &, uint32_t *)
		{ return MISTER_RESULT_UNSUPPORTED; }
	virtual Result ReadManagerMode(const Access &, uint32_t *)
		{ return MISTER_RESULT_UNSUPPORTED; }
	virtual bool RecoveryMappingsHeld(const Access &) const { return false; }
	virtual bool ConsumeAppliedMutation(const Access &) { return false; }
	virtual Result WriteCoreReset(const Access &access,
		uint32_t mask, uint32_t value) = 0;
	virtual Result WriteInterfaceModule(const Access &access,
		uint32_t value) = 0;
	virtual Result WriteSdrPortControl(const Access &access,
		uint32_t offset, uint32_t value) = 0;
	virtual Result WriteBridgeReset(const Access &access,
		uint32_t value) = 0;
	virtual Result WriteRemap(const Access &access,
		uint32_t value) = 0;
	virtual Result ReadCoreGpo(const Access &access, uint32_t *value) = 0;
	virtual Result ReadInterfaceModule(const Access &access,
		uint32_t *value) = 0;
	virtual Result ReadSdrPortControl(const Access &access, uint32_t offset,
		uint32_t *value) = 0;
	virtual Result ReadBridgeReset(const Access &access, uint32_t *value) = 0;
	virtual Result ReadRemap(const Access &access, uint32_t *value) = 0;
	virtual Result ReleaseMappings(const Access &access) = 0;
};

class NativeContainment final {
public:
	NativeContainment(HardwareBroker &broker, NativeContainmentIo &io);
	NativeMappingAcquisitionReceipt AcquireMappings(
		const OperationLease &program_lease);
	NativeBridgeEnableReceipt EnableBridges(
		const OperationLease &program_lease);
	Result ResetAndContain(const OperationLease &terminal_lease);
	Result ResetAndContain(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result ResetAndContain(const RecoveryEpoch &epoch,
		const OperationLease &terminal_lease);
	Result ObserveRecovery(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result ObserveRecovery(const RecoveryEpoch &epoch);
#endif

private:
	struct Values {
		uint32_t core_gpo;
		uint32_t interface_module;
		uint32_t sdr_port_control;
		uint32_t bridge_reset;
		uint32_t remap;
		bool known[7];
		bool mappings_released;
		bool mutation_attempted;
		uint32_t manager_control;
		uint32_t manager_mode;
		bool manager_neutral_observed;
		uint64_t manager_mutation_sequence;
	};
	static void Partition(const Values &values, bool require_mapping_release,
		uint32_t *observed, uint32_t *neutral);
	Result RunTerminal(const OperationLease &terminal_lease, Values *values,
		std::unique_ptr<HardwareLeaseView> *held_view);
	NativeContainment(const NativeContainment &) = delete;
	NativeContainment &operator=(const NativeContainment &) = delete;

	HardwareBroker &broker_;
	NativeContainmentIo &io_;
	Values pending_release_values_;
	std::unique_ptr<ContainmentResumeKey> pending_release_;
};

} // namespace native
} // namespace mister

#endif
