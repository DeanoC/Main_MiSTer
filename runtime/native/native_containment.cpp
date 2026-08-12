// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_containment.hpp"

namespace mister {
namespace native {

namespace {

bool IsRecordableRecoveryFailure(Result result)
{
	return result == MISTER_RESULT_DEADLINE ||
		result == MISTER_RESULT_PLATFORM ||
		result == MISTER_RESULT_CLEANUP_INCOMPLETE;
}

} // namespace

NativeContainment::NativeContainment(HardwareBroker &broker,
	NativeContainmentIo &io)
	: broker_(broker), io_(io), pending_release_values_(), pending_release_()
{
}

Result NativeContainment::RunTerminal(const OperationLease &terminal_lease,
	Values *values, std::unique_ptr<HardwareLeaseView> *held_view)
{
	if (values == nullptr || held_view == nullptr || held_view->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::unique_ptr<HardwareLeaseView> view;
	Result result = broker_.AcquireHardwareLeaseView(terminal_lease, &view);
	if (result != MISTER_RESULT_OK) return result;
	NativeContainmentIo::Access access(view->absolute_deadline_ms());
	if (pending_release_) {
		result = broker_.ValidateContainmentResumeKey(*pending_release_,
			terminal_lease);
		if (result != MISTER_RESULT_OK) return result;
		*values = pending_release_values_;
		result = broker_.ValidateContainmentBoundary(*view);
		if (result != MISTER_RESULT_OK) return result;
		result = io_.ReleaseMappings(access);
		if (result != MISTER_RESULT_OK) return result;
		if (view->RecordMutation() == 0) {
			result = broker_.ValidateContainmentBoundary(*view);
			return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result;
		}
		values->mappings_released = true;
		pending_release_.reset();
		pending_release_values_ = Values();
		*held_view = std::move(view);
		return MISTER_RESULT_OK;
	}

#define CONTAINMENT_BOUNDARY(expression) \
	do { \
		result = broker_.ValidateContainmentBoundary(*view); \
		if (result != MISTER_RESULT_OK) return result; \
		result = (expression); \
		if (result != MISTER_RESULT_OK) return result; \
	} while (0)
#define CONTAINMENT_READ(index, expression) \
	do { \
		CONTAINMENT_BOUNDARY(expression); \
		values->known[index] = true; \
	} while (0)
#define CONTAINMENT_MUTATION(expression) \
	do { \
		result = broker_.ValidateContainmentBoundary(*view); \
		if (result != MISTER_RESULT_OK) return result; \
		values->mutation_attempted = true; \
		result = (expression); \
		if (result != MISTER_RESULT_OK) return result; \
		if (view->RecordMutation() == 0) { \
			result = broker_.ValidateContainmentBoundary(*view); \
			return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result; \
		} \
	} while (0)

	CONTAINMENT_MUTATION(io_.WriteCoreReset(access, 0xc0000000u,
		0x40000000u));
	CONTAINMENT_MUTATION(io_.WriteInterfaceModule(access, 0));
	CONTAINMENT_MUTATION(io_.WriteSdrPortControl(access, 0x5080u, 0));
	CONTAINMENT_MUTATION(io_.WriteBridgeReset(access, 7));
	CONTAINMENT_MUTATION(io_.WriteRemap(access, 1));
	CONTAINMENT_READ(0, io_.ReadCoreGpo(access, &values->core_gpo));
	CONTAINMENT_READ(1, io_.ReadInterfaceModule(access,
		&values->interface_module));
	CONTAINMENT_READ(2, io_.ReadSdrPortControl(access, 0x5080u,
		&values->sdr_port_control));
	CONTAINMENT_READ(3, io_.ReadBridgeReset(access, &values->bridge_reset));
	CONTAINMENT_READ(4, io_.ReadRemap(access, &values->remap));
	result = broker_.ValidateContainmentBoundary(*view);
	if (result != MISTER_RESULT_OK) return result;
	values->mutation_attempted = true;
	result = io_.ReleaseMappings(access);
	if (result != MISTER_RESULT_OK) {
		pending_release_values_ = *values;
		const Result key_result = broker_.MintContainmentResumeKey(*view,
			&pending_release_);
		if (key_result != MISTER_RESULT_OK) return key_result;
		return result;
	}
	if (view->RecordMutation() == 0) {
		result = broker_.ValidateContainmentBoundary(*view);
		return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result;
	}
	values->mappings_released = true;

#undef CONTAINMENT_MUTATION
#undef CONTAINMENT_READ
#undef CONTAINMENT_BOUNDARY
	*held_view = std::move(view);
	return MISTER_RESULT_OK;
}

Result NativeContainment::ResetAndContain(const CleanupEpoch &epoch,
	const OperationLease &terminal_lease)
{
	const Result preflight = broker_.ValidateCleanupContainmentAuthority(epoch,
		terminal_lease);
	if (preflight != MISTER_RESULT_OK) return preflight;
	const Result result = ResetAndContain(terminal_lease);
	if (result != MISTER_RESULT_OK) return result;
	return broker_.ObserveContainment(epoch, terminal_lease);
}

Result NativeContainment::ResetAndContain(
	const OperationLease &terminal_lease)
{
	Values values = {};
	std::unique_ptr<HardwareLeaseView> view;
	const Result result = RunTerminal(terminal_lease, &values, &view);
	if (result != MISTER_RESULT_OK) return result;
	return broker_.StageContainmentEvidence(terminal_lease,
		values.core_gpo, values.interface_module, values.sdr_port_control,
		values.bridge_reset, values.remap, true);
}

Result NativeContainment::ResetAndContain(const RecoveryEpoch &epoch,
	const OperationLease &terminal_lease)
{
	const Result preflight = broker_.ValidateRecoveryContainmentAuthority(epoch,
		terminal_lease);
	if (preflight != MISTER_RESULT_OK) return preflight;
	Values values = {};
	std::unique_ptr<HardwareLeaseView> view;
	const Result result = RunTerminal(terminal_lease, &values, &view);
	uint32_t observed = 0;
	uint32_t neutral = 0;
	Partition(values, true, &observed, &neutral);
	if (result != MISTER_RESULT_OK) {
		if (IsRecordableRecoveryFailure(result)) {
			if (values.mutation_attempted)
				broker_.RecordRecoveryContainmentObservation(epoch, observed,
					neutral, result);
			else
				broker_.RecordRecoveryFailure(epoch, result);
		}
		return result;
	}
	Result commit = broker_.StageContainmentEvidence(terminal_lease,
		values.core_gpo, values.interface_module, values.sdr_port_control,
		values.bridge_reset, values.remap, true);
	if (commit == MISTER_RESULT_OK)
		commit = broker_.CommitRecoveryContainment(epoch, terminal_lease);
	if (IsRecordableRecoveryFailure(commit))
		broker_.RecordRecoveryContainmentObservation(epoch, observed, neutral,
			commit);
	return commit;
}

void NativeContainment::Partition(const Values &values,
	bool require_mapping_release, uint32_t *observed, uint32_t *neutral)
{
	*observed = 0;
	*neutral = 0;
	const bool core_bad = values.known[0] &&
		(values.core_gpo & 0xc0000000u) != 0x40000000u;
	const bool interface_bad = values.known[1] && values.interface_module != 0;
	const bool sdr_bad = values.known[2] && values.sdr_port_control != 0;
	const bool bridge_bad = values.known[3] && values.bridge_reset != 7;
	const bool remap_bad = values.known[4] && values.remap != 1;
	if (core_bad || interface_bad || sdr_bad)
		*observed |= MISTER_RESOURCE_FPGA;
	else if (values.known[0] && values.known[1] && values.known[2] &&
		(!require_mapping_release || values.mappings_released))
		*neutral |= MISTER_RESOURCE_FPGA;
	if (bridge_bad || remap_bad)
		*observed |= MISTER_RESOURCE_BRIDGES;
	else if (values.known[3] && values.known[4])
		*neutral |= MISTER_RESOURCE_BRIDGES;
	if (core_bad || interface_bad)
		*observed |= MISTER_RESOURCE_CORE_PROTOCOL;
	else if (values.known[0] && values.known[1])
		*neutral |= MISTER_RESOURCE_CORE_PROTOCOL;
}

Result NativeContainment::ObserveRecovery(const RecoveryEpoch &epoch)
{
	Result result = broker_.BeginRecoveryObservation(epoch);
	if (result != MISTER_RESULT_OK) return result;
	Values values = {};
	bool known[5] = {false, false, false, false, false};
	Result first_failure = MISTER_RESULT_OK;
	NativeContainmentIo::Access access(UINT64_MAX);

#define RECOVERY_READ(index, expression) \
	do { \
		Result boundary = broker_.CheckRecoveryObservationDeadline(epoch); \
		if (boundary != MISTER_RESULT_OK) { \
			if (first_failure == MISTER_RESULT_OK) first_failure = boundary; \
			break; \
		} \
		boundary = (expression); \
		if (boundary == MISTER_RESULT_OK) known[index] = true; \
		else if (first_failure == MISTER_RESULT_OK) first_failure = boundary; \
	} while (0)

	RECOVERY_READ(0, io_.ReadCoreGpo(access, &values.core_gpo));
	RECOVERY_READ(1, io_.ReadInterfaceModule(access,
		&values.interface_module));
	RECOVERY_READ(2, io_.ReadSdrPortControl(access, 0x5080u,
		&values.sdr_port_control));
	RECOVERY_READ(3, io_.ReadBridgeReset(access, &values.bridge_reset));
	RECOVERY_READ(4, io_.ReadRemap(access, &values.remap));

#undef RECOVERY_READ
	for (size_t index = 0; index != 5; ++index)
		values.known[index] = known[index];
	uint32_t observed = 0;
	uint32_t neutral = 0;
	Partition(values, false, &observed, &neutral);
	return broker_.EndRecoveryObservation(epoch, observed, neutral,
		first_failure);
}

} // namespace native
} // namespace mister
