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
	virtual ~NativeContainmentIo() {}

private:
	friend class NativeContainment;
	virtual Result WriteCoreReset(const HardwareLeaseView &view,
		uint32_t mask, uint32_t value) = 0;
	virtual Result WriteInterfaceModule(const HardwareLeaseView &view,
		uint32_t value) = 0;
	virtual Result WriteSdrPortControl(const HardwareLeaseView &view,
		uint32_t offset, uint32_t value) = 0;
	virtual Result WriteBridgeReset(const HardwareLeaseView &view,
		uint32_t value) = 0;
	virtual Result WriteRemap(const HardwareLeaseView &view,
		uint32_t value) = 0;
	virtual Result ReadCoreGpo(uint32_t *value) = 0;
	virtual Result ReadInterfaceModule(uint32_t *value) = 0;
	virtual Result ReadSdrPortControl(uint32_t offset, uint32_t *value) = 0;
	virtual Result ReadBridgeReset(uint32_t *value) = 0;
	virtual Result ReadRemap(uint32_t *value) = 0;
	virtual Result ReleaseMappings(const HardwareLeaseView &view) = 0;
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
};

} // namespace native
} // namespace mister

#endif
