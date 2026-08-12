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
	NativeFpgaProgrammingReceipt receipt = {result, 0, 0};
	return receipt;
}

} // namespace

NativeFpgaProgrammer::NativeFpgaProgrammer(HardwareBroker &broker,
	NativeClock &clock, NativeFpgaByteSink &sink)
	: broker_(broker), clock_(clock), sink_(sink)
{
}

NativeFpgaProgrammingReceipt NativeFpgaProgrammer::Program(
	const OperationLease &lease,
	const NativeCoreArtifactHandle &artifact)
{
	NativeFpgaProgrammingReceipt receipt = Receipt(MISTER_RESULT_OK);
	if (!artifact.valid()) return Receipt(MISTER_RESULT_INVALID_ARGUMENT);
	if (artifact.bound_profile_ == nullptr)
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
		artifact.RewindAndRevalidate(deadline);
	if (artifact_result != NativeArtifactResult::ok) {
		receipt.result = ToProgrammingResult(artifact_result);
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
			size_t accepted = 0;
			result = sink_.Write(bytes + written, requested - written, deadline,
				&accepted);
			if (accepted == 0) {
				receipt.result = result == MISTER_RESULT_OK ?
					MISTER_RESULT_PLATFORM : result;
				return receipt;
			}
			if (accepted > UINT64_MAX - receipt.accepted_bytes)
				receipt.accepted_bytes = UINT64_MAX;
			else
				receipt.accepted_bytes += static_cast<uint64_t>(accepted);
			uint64_t mutation_sequence = 0;
			const Result mutation_result =
				view->RecordFpgaProgrammingMutation(accepted,
					&mutation_sequence);
			if (mutation_sequence != 0)
				receipt.mutation_sequence = mutation_sequence;
			if (mutation_result != MISTER_RESULT_OK) {
				receipt.result = mutation_result;
				return receipt;
			}
			if (accepted > requested - written) {
				receipt.result = MISTER_RESULT_PLATFORM;
				return receipt;
			}
			written += accepted;
			if (result != MISTER_RESULT_OK) {
				receipt.result = result;
				return receipt;
			}
			if (clock_.NowMs() >= deadline) {
				receipt.result = MISTER_RESULT_DEADLINE;
				return receipt;
			}
		}
		consumed += requested;
	}
	ssize_t extra = -1;
	artifact_result = artifact.ReadForUse(bytes, 1, &extra, deadline);
	if (artifact_result != NativeArtifactResult::ok) {
		receipt.result = ToProgrammingResult(artifact_result);
		return receipt;
	}
	if (extra != 0) {
		receipt.result = MISTER_RESULT_PLATFORM;
		return receipt;
	}
	artifact_result = artifact.Revalidate(deadline);
	receipt.result = ToProgrammingResult(artifact_result);
	return receipt;
}

} // namespace linux_native
} // namespace native
} // namespace mister
