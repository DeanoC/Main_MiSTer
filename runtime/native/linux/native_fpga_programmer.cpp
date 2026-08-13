// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_fpga_programmer.hpp"

namespace mister {
namespace native {
namespace linux_native {
namespace {

Result ToProgrammingResult(NativeArtifactResult result)
{
	switch (result) {
	case NativeArtifactResult::ok: return MISTER_RESULT_OK;
	case NativeArtifactResult::invalid_argument:
	case NativeArtifactResult::invalid_identity:
		return MISTER_RESULT_INVALID_ARGUMENT;
	case NativeArtifactResult::deadline: return MISTER_RESULT_DEADLINE;
	case NativeArtifactResult::cleanup_incomplete:
	case NativeArtifactResult::insecure:
	case NativeArtifactResult::not_found:
	case NativeArtifactResult::changed:
	case NativeArtifactResult::digest_mismatch:
	case NativeArtifactResult::io:
	default: return MISTER_RESULT_PLATFORM;
	}
}

NativeFpgaProgrammingReceipt Receipt(Result result)
{
	const NativeFpgaProgrammingReceipt receipt = {
		result, false, 0, false, false, false, false, 0};
	return receipt;
}

} // namespace

NativeFpgaProgrammer::NativeFpgaProgrammer(HardwareBroker &broker,
	NativeClock &clock, NativeFpgaByteSink &sink)
	: broker_(broker), clock_(clock), sink_(sink)
{
}

NativeFpgaProgrammingReceipt NativeFpgaProgrammer::Program(
	const OperationLease &lease, const NativeCoreArtifactHandle &artifact)
{
	NativeFpgaProgrammingReceipt receipt = Receipt(MISTER_RESULT_OK);
	if (!artifact.valid() || artifact.bound_profile_ == nullptr)
		return Receipt(MISTER_RESULT_INVALID_ARGUMENT);
	if (lease.operation_kind() != OperationKind::program_fpga)
		return Receipt(MISTER_RESULT_INVALID_STATE);
	const uint64_t deadline = lease.absolute_deadline_ms();
	if (clock_.NowMs() >= deadline) return Receipt(MISTER_RESULT_DEADLINE);
	std::unique_ptr<HardwareLeaseView> view;
	Result result = broker_.AcquireHardwareLeaseView(lease, &view);
	if (result != MISTER_RESULT_OK) return Receipt(result);
	result = view->AuthorizeFpgaProgrammingProfile(*artifact.bound_profile_);
	if (result != MISTER_RESULT_OK) return Receipt(result);
	NativeArtifactResult artifact_result =
		artifact.PrepareVerifiedProgrammingRead(deadline);
	if (artifact_result != NativeArtifactResult::ok) {
		receipt.result = ToProgrammingResult(artifact_result);
		return receipt;
	}

	std::unique_ptr<NativeFpgaProgramSession> session;
	auto record_applied = [&view, &receipt](bool applied) -> Result {
		if (!applied) return MISTER_RESULT_OK;
		const uint64_t sequence = view->RecordMutation();
		if (sequence == 0) return MISTER_RESULT_PLATFORM;
		receipt.mutation_sequence = sequence;
		return MISTER_RESULT_OK;
	};
	const NativeFpgaSinkStartOutcome start =
		sink_.Begin(artifact.size(), deadline, &session);
	receipt.acquired = start.acquired || start.mutation_attempted ||
		start.mutation_applied;
	result = record_applied(start.mutation_applied);
	const bool malformed_start =
		(start.result == MISTER_RESULT_OK) != (session.get() != nullptr);
	if (result != MISTER_RESULT_OK || malformed_start ||
		start.result != MISTER_RESULT_OK) {
		receipt.result = result != MISTER_RESULT_OK ? result :
			(malformed_start ? MISTER_RESULT_PLATFORM : start.result);
		session.reset();
		return receipt;
	}

	unsigned char bytes[4096];
	uint64_t consumed = 0;
	while (consumed < artifact.size()) {
		if (clock_.NowMs() >= deadline) {
			receipt.result = MISTER_RESULT_DEADLINE;
			return receipt;
		}
		const uint64_t remaining = artifact.size() - consumed;
		const size_t requested = remaining < sizeof(bytes) ?
			static_cast<size_t>(remaining) : sizeof(bytes);
		ssize_t read_count = 0;
		artifact_result = artifact.ReadForUse(bytes, requested, &read_count,
			deadline);
		if (artifact_result != NativeArtifactResult::ok) {
			receipt.result = ToProgrammingResult(artifact_result);
			return receipt;
		}
		if (read_count <= 0 || static_cast<size_t>(read_count) != requested) {
			receipt.result = MISTER_RESULT_PLATFORM;
			return receipt;
		}
		size_t written = 0;
		while (written < requested) {
			if (clock_.NowMs() >= deadline) {
				receipt.result = MISTER_RESULT_DEADLINE;
				return receipt;
			}
			const NativeFpgaSinkWriteOutcome outcome = session->Write(
				bytes + written, requested - written, deadline);
			receipt.acquired = receipt.acquired || outcome.mutation_attempted ||
				outcome.mutation_applied || outcome.accepted_bytes != 0;
			result = record_applied(
				outcome.mutation_applied || outcome.accepted_bytes != 0);
			if (outcome.accepted_bytes > UINT64_MAX - receipt.accepted_bytes)
				receipt.accepted_bytes = UINT64_MAX;
			else
				receipt.accepted_bytes +=
					static_cast<uint64_t>(outcome.accepted_bytes);
			if (result != MISTER_RESULT_OK) {
				receipt.result = result;
				return receipt;
			}
			if (outcome.accepted_bytes == 0 ||
				outcome.accepted_bytes > requested - written) {
				receipt.result = outcome.result == MISTER_RESULT_OK ?
					MISTER_RESULT_PLATFORM : outcome.result;
				return receipt;
			}
			written += outcome.accepted_bytes;
			if (outcome.result != MISTER_RESULT_OK) {
				receipt.result = outcome.result;
				return receipt;
			}
			if (clock_.NowMs() >= deadline) {
				receipt.result = MISTER_RESULT_DEADLINE;
				return receipt;
			}
		}
		consumed += requested;
	}

	artifact_result = artifact.RevalidateProgrammedIdentity(deadline);
	if (artifact_result != NativeArtifactResult::ok) {
		receipt.result = ToProgrammingResult(artifact_result);
		return receipt;
	}
	const NativeFpgaSinkFinishOutcome finish = session->Finish(deadline);
	receipt.acquired = receipt.acquired || finish.mutation_attempted ||
		finish.mutation_applied;
	result = record_applied(finish.mutation_applied);
	receipt.configuration_done_observed = finish.configuration_done_observed;
	receipt.initialization_observed = finish.initialization_observed;
	receipt.user_mode_observed = finish.user_mode_observed;
	receipt.manager_drive_released = finish.manager_drive_released;
	if (result != MISTER_RESULT_OK) receipt.result = result;
	else if (finish.result != MISTER_RESULT_OK) receipt.result = finish.result;
	else if (!finish.configuration_done_observed ||
		!finish.initialization_observed || !finish.user_mode_observed ||
		!finish.manager_drive_released || receipt.accepted_bytes != artifact.size() ||
		receipt.mutation_sequence == 0)
		receipt.result = MISTER_RESULT_PLATFORM;
	return receipt;
}

} // namespace linux_native
} // namespace native
} // namespace mister
