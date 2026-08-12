// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CONTAINMENT_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CONTAINMENT_HPP

#include "runtime/native/hardware_broker.hpp"

#include <stdint.h>

namespace mister {
namespace native {

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
	Result ResetAndContain(const OperationLease &terminal_lease);
	Result ResetAndContain(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result ResetAndContain(const RecoveryEpoch &epoch,
		const OperationLease &terminal_lease);
	Result ObserveRecovery(const RecoveryEpoch &epoch);

private:
	struct Values {
		uint32_t core_gpo;
		uint32_t interface_module;
		uint32_t sdr_port_control;
		uint32_t bridge_reset;
		uint32_t remap;
		bool known[5];
		bool mappings_released;
		bool mutation_attempted;
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
