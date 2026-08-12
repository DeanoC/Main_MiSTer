// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_spi_bus.hpp"

namespace mister {
namespace native {

bool NativeSpiBus::RecordObservedMutation(HardwareLeaseView &view,
	SpiReceipt *receipt)
{
	const uint64_t sequence = view.RecordMutation();
	if (sequence == 0) return false;
	receipt->mutation_sequence = sequence;
	return true;
}

Result NativeSpiBus::WaitForAck(const HardwareLeaseView &view,
	bool want_high, uint64_t deadline_ms, bool *observed)
{
	if (observed == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	*observed = false;
	for (;;) {
		if (clock_.NowMs() >= deadline_ms) return MISTER_RESULT_DEADLINE;
		bool high = false;
		const Result read_result = hardware_.ReadAck(view, &high);
		if (read_result != MISTER_RESULT_OK) return read_result;
		if (clock_.NowMs() >= deadline_ms) return MISTER_RESULT_DEADLINE;
		if (high == want_high) {
			*observed = true;
			return MISTER_RESULT_OK;
		}
	}
}

namespace {

bool ValidTransaction(const SpiTransaction &transaction)
{
	if (transaction.select_mask == 0 || transaction.deselect_mask == 0)
		return false;
	if (transaction.word_count == 0 || transaction.words == nullptr)
		return false;
	return transaction.ack_policy == AckPolicy::required;
}

} // namespace

NativeSpiBus::NativeSpiBus(NativeClock &clock, NativeHardwareIo &hardware)
	: clock_(clock), hardware_(hardware)
{
}

Result NativeSpiBus::Execute(const OperationLease &lease,
	const SpiTransaction &transaction, SpiReceipt *receipt,
	const SpiReceiptCommitToken *commit)
{
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	receipt->result = MISTER_RESULT_INVALID_STATE;
	receipt->selected = false;
	receipt->completed_words = 0;
	receipt->ack_low_observed = false;
	receipt->deselected = false;
	receipt->mutation_sequence = 0;
	if (!ValidTransaction(transaction)) {
		receipt->result = MISTER_RESULT_INVALID_ARGUMENT;
		return receipt->result;
	}

	// This is the only admission point. The view retains the broker
	// registration until after bounded deselect and receipt construction.
	std::unique_ptr<HardwareLeaseView> view;
	Result result = lease.AcquireHardwareLeaseView(&view);
	if (result != MISTER_RESULT_OK) {
		receipt->result = result;
		return result;
	}
	return ExecuteWithHardwareLeaseView(*view, transaction, receipt, commit);
}

Result NativeSpiBus::ExecuteWithHardwareLeaseView(HardwareLeaseView &view,
	const SpiTransaction &transaction, SpiReceipt *receipt,
	const SpiReceiptCommitToken *commit)
{
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	receipt->result = MISTER_RESULT_INVALID_STATE;
	receipt->selected = false;
	receipt->completed_words = 0;
	receipt->ack_low_observed = false;
	receipt->deselected = false;
	receipt->mutation_sequence = 0;
	if (!ValidTransaction(transaction)) {
		receipt->result = MISTER_RESULT_INVALID_ARGUMENT;
		return receipt->result;
	}
	const uint64_t absolute_deadline_ms = view.absolute_deadline_ms();

	if (clock_.NowMs() >= absolute_deadline_ms) {
		receipt->result = MISTER_RESULT_DEADLINE;
		return receipt->result;
	}
	Result result = hardware_.Select(view, transaction.select_mask);
	if (result == MISTER_RESULT_OK) {
		receipt->selected = true;
		if (!RecordObservedMutation(view, receipt))
			result = MISTER_RESULT_PLATFORM;
	}

	if (result == MISTER_RESULT_OK) {
		for (size_t index = 0; index < transaction.word_count; ++index) {
			if (clock_.NowMs() >= absolute_deadline_ms) {
				result = MISTER_RESULT_DEADLINE;
				break;
			}
			result = hardware_.WriteWord(view, transaction.words[index]);
			if (result != MISTER_RESULT_OK) break;
			if (!RecordObservedMutation(view, receipt)) {
				result = MISTER_RESULT_PLATFORM;
				break;
			}

			bool observed_high = false;
			result = WaitForAck(view, true,
				absolute_deadline_ms, &observed_high);
			if (result != MISTER_RESULT_OK) break;
			bool observed_low = false;
			result = WaitForAck(view, false,
				absolute_deadline_ms, &observed_low);
			if (result != MISTER_RESULT_OK) break;
			if (observed_low) {
				receipt->ack_low_observed = true;
				++receipt->completed_words;
			}
		}
	}

	if (receipt->selected) {
		// Deselect is attempted for every selected transaction, including a
		// timeout/failure path. The platform adapter bounds this single call;
		// the registration remains held until its result is recorded.
		const Result deselect_result =
			hardware_.Deselect(view, transaction.deselect_mask,
				absolute_deadline_ms);
		if (deselect_result == MISTER_RESULT_OK) {
			receipt->deselected = true;
			if (!RecordObservedMutation(view, receipt) &&
				result == MISTER_RESULT_OK)
				result = MISTER_RESULT_PLATFORM;
		} else if (result == MISTER_RESULT_OK) {
			result = deselect_result;
		}
	}
	if (result == MISTER_RESULT_OK &&
		clock_.NowMs() >= absolute_deadline_ms)
		result = MISTER_RESULT_DEADLINE;

	receipt->result = result;
	if (result == MISTER_RESULT_OK && commit != nullptr)
		commit->Commit(*receipt);
	return result;
}

} // namespace native
} // namespace mister
