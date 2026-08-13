// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_spi_bus.hpp"

namespace mister {
namespace native {

bool NativeSpiBus::RecordAppliedMutation(HardwareLeaseView &view,
	SpiReceipt *receipt)
{
	const uint64_t sequence = view.RecordMutation();
	if (sequence == 0) return false;
	receipt->mutation_sequence = sequence;
	return true;
}

Result NativeSpiBus::WaitForAck(const HardwareLeaseView &view,
	bool want_high, uint64_t deadline_ms, bool *observed, uint16_t *response)
{
	if (observed == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	*observed = false;
	for (;;) {
		if (clock_.NowMs() >= deadline_ms) return MISTER_RESULT_DEADLINE;
		NativeSpiAckSample sample = {};
		const Result read_result = hardware_.ReadAckSample(view, &sample);
		if (read_result != MISTER_RESULT_OK) return read_result;
		if (sample.fault) return MISTER_RESULT_PLATFORM;
		if (clock_.NowMs() >= deadline_ms) return MISTER_RESULT_DEADLINE;
		if (sample.ack_high == want_high) {
			if (response != nullptr) *response = sample.response;
			*observed = true;
			return MISTER_RESULT_OK;
		}
	}
}

namespace {

bool ValidTarget(NativeSpiTarget target)
{
	return target == NativeSpiTarget::user_io ||
		target == NativeSpiTarget::file_io;
}

bool ValidWords(const SpiWords &words)
{
	if (words.word_count == 0 || words.transmit_words == nullptr) return false;
	if (words.received_words == nullptr) return words.received_capacity == 0;
	return words.received_capacity >= words.word_count;
}

void InitializeReceipt(SpiReceipt *receipt, NativeSpiTarget target,
	bool mapping_retained)
{
	receipt->result = MISTER_RESULT_INVALID_STATE;
	receipt->selected = false;
	receipt->completed_words = 0;
	receipt->response_words_observed = 0;
	receipt->captured_words = 0;
	receipt->ack_high_observed = false;
	receipt->ack_low_observed = false;
	receipt->select_attempted = false;
	receipt->deselect_attempted = false;
	receipt->deselected = false;
	receipt->strobe_low_observed = false;
	receipt->force_strobe_low_attempted = false;
	receipt->force_strobe_low_applied = false;
	receipt->force_strobe_low_observed = false;
	receipt->target = target;
	receipt->target_may_be_selected = false;
	receipt->strobe_may_be_high = false;
	receipt->mapping_retained = mapping_retained;
	receipt->mutation_sequence = 0;
}

} // namespace

void NativeSpiBus::RecordMutationResult(HardwareLeaseView &view,
	SpiReceipt *receipt, const NativeSpiMutationResult &mutation,
	Result *primary)
{
	if (mutation.applied && !RecordAppliedMutation(view, receipt) &&
		*primary == MISTER_RESULT_OK)
		*primary = MISTER_RESULT_PLATFORM;
	if (mutation.result != MISTER_RESULT_OK) {
		if (*primary == MISTER_RESULT_OK) *primary = mutation.result;
		return;
	}
	if (!mutation.attempted || !mutation.applied || !mutation.observed) {
		if (*primary == MISTER_RESULT_OK) *primary = MISTER_RESULT_PLATFORM;
	}
}

NativeSpiBus::NativeSpiBus(NativeClock &clock, NativeHardwareIo &hardware)
	: clock_(clock), hardware_(hardware), input_port_(*this)
{
}

NativeInputSpiPort::NativeInputSpiPort(NativeSpiBus &bus) : bus_(bus)
{
}

Result NativeInputSpiPort::Execute(HardwareLeaseView &view,
	const SpiWords &words, SpiReceipt *receipt,
	const SpiReceiptCommitToken *commit)
{
	return bus_.ExchangeWithHardwareLeaseView(view, NativeSpiTarget::user_io,
		words, receipt, commit);
}

#if defined(MISTER_NATIVE_SPI_TESTING)
Result NativeSpiBus::ExchangeForTest(const OperationLease &lease,
	NativeSpiTarget target, const SpiWords &words, SpiReceipt *receipt)
{
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	InitializeReceipt(receipt, target, false);
	if (!ValidTarget(target) || !ValidWords(words)) {
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
	return ExchangeWithHardwareLeaseView(*view, target, words, receipt, nullptr);
}
#endif

Result NativeSpiBus::ExchangeWithHardwareLeaseView(HardwareLeaseView &view,
	NativeSpiTarget target, const SpiWords &words, SpiReceipt *receipt,
	const SpiReceiptCommitToken *commit)
{
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	InitializeReceipt(receipt, target, true);
	if (!ValidTarget(target) || !ValidWords(words)) {
		receipt->result = MISTER_RESULT_INVALID_ARGUMENT;
		return receipt->result;
	}
	const uint64_t absolute_deadline_ms = view.absolute_deadline_ms();

	if (clock_.NowMs() >= absolute_deadline_ms) {
		receipt->result = MISTER_RESULT_DEADLINE;
		return receipt->result;
	}
	Result result = MISTER_RESULT_OK;
	// A failed select can still have changed the selected target. Retain that
	// residue before the attempt and always drive the bounded cleanup path.
	receipt->select_attempted = true;
	receipt->target_may_be_selected = true;
	const NativeSpiMutationResult selected = hardware_.Select(view, target);
	if (selected.applied && selected.observed) receipt->selected = true;
	RecordMutationResult(view, receipt, selected, &result);
	if (result == MISTER_RESULT_OK && clock_.NowMs() >= absolute_deadline_ms)
		result = MISTER_RESULT_DEADLINE;

	if (result == MISTER_RESULT_OK) {
		for (size_t index = 0; index < words.word_count; ++index) {
			if (clock_.NowMs() >= absolute_deadline_ms) {
				result = MISTER_RESULT_DEADLINE;
				break;
			}
			// The combined data/strobe-low write can fail after a platform
			// mutation without a positive low-state observation. Preserve the
			// conservative residue until bounded force-low cleanup observes it.
			receipt->strobe_may_be_high = true;
			const NativeSpiMutationResult written =
				hardware_.WriteWordWithStrobeLow(view, words.transmit_words[index]);
			if (written.applied && written.observed) {
				receipt->strobe_may_be_high = false;
				receipt->strobe_low_observed = true;
			}
			RecordMutationResult(view, receipt, written, &result);
			if (result != MISTER_RESULT_OK) break;
			if (clock_.NowMs() >= absolute_deadline_ms) {
				result = MISTER_RESULT_DEADLINE;
				break;
			}
			// Once the high transition is attempted, force-low cleanup is needed
			// until a low transition has been positively observed.
			receipt->strobe_may_be_high = true;
			const NativeSpiMutationResult strobe_high =
				hardware_.SetStrobe(view, true);
			RecordMutationResult(view, receipt, strobe_high, &result);
			if (result != MISTER_RESULT_OK) break;
			if (clock_.NowMs() >= absolute_deadline_ms) {
				result = MISTER_RESULT_DEADLINE;
				break;
			}

			bool observed_high = false;
			result = WaitForAck(view, true,
				absolute_deadline_ms, &observed_high, nullptr);
			if (result != MISTER_RESULT_OK) break;
			if (observed_high) receipt->ack_high_observed = true;
			const NativeSpiMutationResult strobe_low =
				hardware_.SetStrobe(view, false);
			if (strobe_low.applied && strobe_low.observed) {
				receipt->strobe_may_be_high = false;
				receipt->strobe_low_observed = true;
			}
			RecordMutationResult(view, receipt, strobe_low, &result);
			if (result != MISTER_RESULT_OK) break;
			if (clock_.NowMs() >= absolute_deadline_ms) {
				result = MISTER_RESULT_DEADLINE;
				break;
			}
			bool observed_low = false;
			uint16_t response = 0;
			result = WaitForAck(view, false,
				absolute_deadline_ms, &observed_low, &response);
			if (result != MISTER_RESULT_OK) break;
			if (observed_low) {
				receipt->ack_low_observed = true;
				++receipt->response_words_observed;
				if (words.received_words != nullptr) {
					words.received_words[receipt->captured_words] = response;
					++receipt->captured_words;
				}
				++receipt->completed_words;
			}
		}
	}

	if (receipt->target_may_be_selected) {
		if (receipt->strobe_may_be_high) {
			const NativeSpiMutationResult forced_low =
				hardware_.SetStrobe(view, false);
			receipt->force_strobe_low_attempted = forced_low.attempted;
			receipt->force_strobe_low_applied = forced_low.applied;
			receipt->force_strobe_low_observed = forced_low.observed;
			if (forced_low.applied && forced_low.observed) {
				receipt->strobe_may_be_high = false;
				receipt->strobe_low_observed = true;
			}
			RecordMutationResult(view, receipt, forced_low, &result);
		}
		// Deselect is attempted for every possibly-selected transaction,
		// including a failed select. The platform adapter bounds this one call;
		// the registration remains held until the residue is recorded.
		receipt->deselect_attempted = true;
		const NativeSpiMutationResult deselected = hardware_.Deselect(view,
			target, absolute_deadline_ms);
		if (deselected.applied && deselected.observed) {
			receipt->deselected = true;
			receipt->target_may_be_selected = false;
		}
		RecordMutationResult(view, receipt, deselected, &result);
	}
	if (result == MISTER_RESULT_OK &&
		clock_.NowMs() >= absolute_deadline_ms)
		result = MISTER_RESULT_DEADLINE;
	if (result == MISTER_RESULT_OK &&
		(receipt->completed_words != words.word_count ||
			receipt->response_words_observed != words.word_count ||
			(words.received_words != nullptr &&
				receipt->captured_words != words.word_count) ||
			!receipt->ack_high_observed || !receipt->ack_low_observed ||
			!receipt->strobe_low_observed || !receipt->deselected ||
			receipt->target_may_be_selected || receipt->strobe_may_be_high))
		result = MISTER_RESULT_PLATFORM;

	receipt->result = result;
	if (result == MISTER_RESULT_OK && commit != nullptr)
		commit->Commit(*receipt);
	return result;
}

} // namespace native
} // namespace mister
