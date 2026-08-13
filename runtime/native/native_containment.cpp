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

NativeMappingAcquisitionReceipt NativeContainment::AcquireMappings(
	const OperationLease &program_lease)
{
	const NativeMappingAcquisitionReceipt denied = {
		MISTER_RESULT_INVALID_STATE, false, false};
	if (program_lease.operation_kind() != OperationKind::program_fpga)
		return denied;
	std::unique_ptr<HardwareLeaseView> view;
	const Result result = broker_.AcquireHardwareLeaseView(program_lease, &view);
	if (result != MISTER_RESULT_OK) {
		NativeMappingAcquisitionReceipt receipt = denied;
		receipt.result = result;
		return receipt;
	}
	NativeContainmentIo::Access access(view->absolute_deadline_ms());
	return io_.AcquireMappings(access);
}

NativeBridgeEnableReceipt NativeContainment::EnableBridges(
	const OperationLease &program_lease)
{
	NativeBridgeEnableReceipt receipt = {
		MISTER_RESULT_INVALID_STATE, false, false, false, false, false, false, false,
		0, 0};
	if (program_lease.operation_kind() != OperationKind::program_fpga)
		return receipt;
	std::unique_ptr<HardwareLeaseView> view;
	receipt.result = broker_.AcquireHardwareLeaseView(program_lease, &view);
	if (receipt.result != MISTER_RESULT_OK) return receipt;
	NativeContainmentIo::Access access(view->absolute_deadline_ms());
	receipt = io_.EnableBridges(access);
	if (receipt.mutation_applied) {
		receipt.mutation_sequence = view->RecordMutation();
		if (receipt.mutation_sequence == 0)
			receipt.result = MISTER_RESULT_PLATFORM;
	}
	if (receipt.result == MISTER_RESULT_OK &&
		(!receipt.acquired || !receipt.mutation_applied ||
		 !receipt.sdr_ports_observed ||
		 !receipt.bridge_release_observed || !receipt.remap_observed ||
		 !receipt.core_normal_write_attempted ||
		 !receipt.core_normal_observed ||
		 (receipt.observed_core_gpo & 0xc0000000u) != 0x80000000u ||
		 receipt.mutation_sequence == 0))
		receipt.result = MISTER_RESULT_PLATFORM;
	return receipt;
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
			terminal_lease, pending_release_values_.core_gpo,
			pending_release_values_.interface_module,
			pending_release_values_.sdr_port_control,
			pending_release_values_.bridge_reset, pending_release_values_.remap,
			pending_release_values_.manager_control,
			pending_release_values_.manager_mode,
			pending_release_values_.manager_mutation_sequence);
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
		const bool applied = io_.ConsumeAppliedMutation(access) || \
			result == MISTER_RESULT_OK; \
		if (applied && view->RecordMutation() == 0) { \
			result = broker_.ValidateContainmentBoundary(*view); \
			return result == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : result; \
		} \
		if (result != MISTER_RESULT_OK) return result; \
	} while (0)

	CONTAINMENT_MUTATION(io_.WriteCoreReset(access, 0xc0000000u,
		0x40000000u));
	CONTAINMENT_MUTATION(io_.WriteInterfaceModule(access, 0));
	CONTAINMENT_MUTATION(io_.WriteSdrPortControl(access, 0x5080u, 0));
	CONTAINMENT_MUTATION(io_.WriteBridgeReset(access, 7));
	CONTAINMENT_MUTATION(io_.WriteRemap(access, 1));
	result = broker_.ValidateContainmentBoundary(*view);
	if (result != MISTER_RESULT_OK) return result;
	const NativeManagerNeutralReceipt manager = io_.ReconcileManager(access);
	values->mutation_attempted = values->mutation_attempted ||
		manager.mutation_attempted;
	if (manager.mutation_applied) {
		values->manager_mutation_sequence = view->RecordMutation();
		if (values->manager_mutation_sequence == 0)
			return MISTER_RESULT_PLATFORM;
	} else {
		values->manager_mutation_sequence = view->CurrentMutationSequence();
	}
	if (manager.result != MISTER_RESULT_OK) return manager.result;
	values->manager_control = manager.observed_control;
	values->manager_mode = manager.observed_mode;
	values->manager_neutral_observed = manager.neutral_observed;
	if (!manager.neutral_observed ||
		(manager.observed_control & 0x107u) != 0x2u ||
		manager.observed_mode > 4u || values->manager_mutation_sequence == 0)
		return MISTER_RESULT_CLEANUP_INCOMPLETE;
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
			values->core_gpo, values->interface_module, values->sdr_port_control,
			values->bridge_reset, values->remap, values->manager_control,
			values->manager_mode, values->manager_mutation_sequence,
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
		values.bridge_reset, values.remap, values.manager_control,
		values.manager_mode, values.manager_neutral_observed,
		values.manager_mutation_sequence, true);
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
				broker_.RecordRecoveryFailure(epoch, terminal_lease, result,
					RecoveryFailurePersistence::retryable);
		}
		return result;
	}
	Result commit = broker_.StageContainmentEvidence(terminal_lease,
		values.core_gpo, values.interface_module, values.sdr_port_control,
		values.bridge_reset, values.remap, values.manager_control,
		values.manager_mode, values.manager_neutral_observed,
		values.manager_mutation_sequence, true);
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
	const bool manager_bad =
		(values.known[5] && (values.manager_control & 0x107u) != 0x2u) ||
		(values.known[6] && values.manager_mode > 4u);
	if (core_bad || interface_bad || sdr_bad || manager_bad)
		*observed |= MISTER_RESOURCE_FPGA;
	else if (values.known[0] && values.known[1] && values.known[2] &&
		values.known[5] && values.known[6] &&
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

Result NativeContainment::ObserveRecovery(const RecoveryEpoch &epoch,
	const OperationInvocation &invocation)
{
	Result result = broker_.BeginRecoveryObservation(epoch, invocation);
	if (result != MISTER_RESULT_OK) return result;
	uint64_t deadline_ms = 0;
	result = broker_.RecoveryObservationDeadline(epoch, invocation, &deadline_ms);
	if (result != MISTER_RESULT_OK)
		return broker_.EndRecoveryObservation(epoch, invocation, 0, 0, result);
	Values values = {};
	bool known[7] = {false, false, false, false, false, false, false};
	Result first_failure = MISTER_RESULT_OK;
	NativeContainmentIo::Access access(deadline_ms);

#define RECOVERY_READ(index, expression) \
	do { \
		Result boundary = broker_.CheckRecoveryObservationDeadline(epoch, invocation); \
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
	RECOVERY_READ(5, io_.ReadManagerControl(access,
		&values.manager_control));
	RECOVERY_READ(6, io_.ReadManagerMode(access, &values.manager_mode));

#undef RECOVERY_READ
	for (size_t index = 0; index != 7; ++index)
		values.known[index] = known[index];
	values.mappings_released = !io_.RecoveryMappingsHeld(access);
	uint32_t observed = 0;
	uint32_t neutral = 0;
	Partition(values, true, &observed, &neutral);
	return broker_.EndRecoveryObservation(epoch, invocation, observed, neutral,
		first_failure);
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result NativeContainment::ObserveRecovery(const RecoveryEpoch &epoch)
{
	std::unique_ptr<OperationInvocation> invocation;
	const Result begin = broker_.BeginRecoveryInvocation(epoch, UINT64_MAX,
		&invocation);
	if (begin != MISTER_RESULT_OK) return begin;
	const Result result = ObserveRecovery(epoch, *invocation);
	const Result finish = broker_.FinishInvocation(std::move(invocation));
	if (finish != MISTER_RESULT_OK) {
		invocation.reset();
		return MISTER_RESULT_PLATFORM;
	}
	return result;
}
#endif

} // namespace native
} // namespace mister
