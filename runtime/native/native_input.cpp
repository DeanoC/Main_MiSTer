// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_input.hpp"

namespace mister {
namespace native {

NativeInput::NativeInput(NativeInputSpiPort &port)
	: port_(port), profile_(nullptr), ledger_(), ledger_valid_{false, false},
	  uncertain_{false, false}, uncertain_map_{0, 0},
	  uncertain_identity_{{0, 0}, {0, 0}},
	  last_sequence_{0, 0}, last_map_{0, 0},
	  pending_{false, 0, 0, 0, {0, 0}},
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	  fail_next_commit_for_test_(false),
#endif
	  mutex_()
{
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
void NativeInput::FailNextCommitForTest()
{
	std::lock_guard<std::mutex> lock(mutex_);
	fail_next_commit_for_test_ = true;
}
#endif

void NativeInput::ClearReceipt(SpiReceipt *receipt)
{
	if (receipt == nullptr) return;
	receipt->result = MISTER_RESULT_UNSUPPORTED;
	receipt->selected = false;
	receipt->completed_words = 0;
	receipt->ack_low_observed = false;
	receipt->response_words_observed = 0;
	receipt->captured_words = 0;
	receipt->ack_high_observed = false;
	receipt->select_attempted = false;
	receipt->deselect_attempted = false;
	receipt->deselected = false;
	receipt->strobe_low_observed = false;
	receipt->force_strobe_low_attempted = false;
	receipt->force_strobe_low_applied = false;
	receipt->force_strobe_low_observed = false;
	receipt->target = NativeSpiTarget::user_io;
	receipt->target_may_be_selected = false;
	receipt->strobe_may_be_high = false;
	receipt->mapping_retained = false;
	receipt->mutation_sequence = 0;
}

size_t NativeInput::ledger_size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return (ledger_valid_[0] ? 1u : 0u) + (ledger_valid_[1] ? 1u : 0u);
}

bool NativeInput::GetDeliveredInput(size_t index,
	DeliveredInput *snapshot) const
{
	if (snapshot == nullptr) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	size_t seen = 0;
	for (size_t player = 0; player < kNativePlayerCount; ++player) {
		if (!ledger_valid_[player]) continue;
		if (seen++ == index) {
			*snapshot = ledger_[player];
			return true;
		}
	}
	return false;
}

Result NativeInput::SnapshotDeliveredInput(HardwareBroker &owner,
	const NativeCoreProfile *profile, const OperationLease &lease,
	DeliveredInput *values, bool *valid, size_t count) const
{
	if (profile == nullptr || values == nullptr || valid == nullptr ||
		count < kNativePlayerCount) return MISTER_RESULT_INVALID_ARGUMENT;
	std::unique_ptr<HardwareLeaseView> view;
	const Result result = lease.AcquireInputHardwareLeaseView(owner, *profile,
		&view);
	if (result != MISTER_RESULT_OK) return result;
	std::lock_guard<std::mutex> lock(mutex_);
	if (profile_ != nullptr && profile_ != profile) return MISTER_RESULT_UNSUPPORTED;
	for (size_t player = 0; player < kNativePlayerCount; ++player) {
		valid[player] = ledger_valid_[player];
		values[player] = ledger_valid_[player] ? ledger_[player] : DeliveredInput{};
	}
	return MISTER_RESULT_OK;
}

void NativeInput::CommitReceipt(void *context,
	const SpiReceipt &receipt) noexcept
{
	static_cast<NativeInput *>(context)->CommitDelivered(receipt);
}

void NativeInput::CommitDelivered(const SpiReceipt &receipt) noexcept
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	if (fail_next_commit_for_test_) {
		fail_next_commit_for_test_ = false;
		return;
	}
#endif
	if (!pending_.active || receipt.result != MISTER_RESULT_OK ||
		receipt.completed_words != kNativeDigitalWordCount ||
		!receipt.ack_low_observed || !receipt.deselected)
		return;
	const size_t player = pending_.player;
	if (pending_.identity.player != player ||
		pending_.identity.sequence != last_sequence_[player] ||
		pending_.map != last_map_[player])
		return;
	if (pending_.map == 0) {
		ledger_valid_[player] = false;
	} else {
		ledger_[player] = {
			pending_.player, pending_.command, pending_.map,
			{pending_.command, 0}, pending_.identity
		};
		ledger_valid_[player] = true;
	}
	uncertain_[player] = false;
	pending_.active = false;
}

Result NativeInput::Deliver(const NativeCoreProfile *profile,
	const OperationLease &lease, const NativeInputEvent &event,
	SpiReceipt *receipt)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ClearReceipt(receipt);
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	if (profile == nullptr || !ValidateNativeCoreProfileRecord(*profile))
		return MISTER_RESULT_UNSUPPORTED;
	if (event.kind != NativeInputKind::digital || event.player > 1 ||
		event.map > 0xffffu || event.joystick_swap ||
		profile->input.joystick_swap)
		return MISTER_RESULT_UNSUPPORTED;

	const size_t player = event.player;
	const uint16_t map = static_cast<uint16_t>(event.map);
	const InputIdentity identity = event.identity;
	if (identity.sequence == 0) {
		receipt->result = MISTER_RESULT_INVALID_ARGUMENT;
		return MISTER_RESULT_INVALID_ARGUMENT;
	}
	if (identity.player != event.player) {
		return MISTER_RESULT_UNSUPPORTED;
	}

	// Broker validation and the transaction fence cover every supported path,
	// including no-op success and input sequence changes. The broker verifies
	// lifetime, exact input authority, bound profile identity, and deadline.
	std::unique_ptr<HardwareLeaseView> view;
	const Result admission = lease.AcquireInputHardwareLeaseView(*profile, &view);
	if (admission != MISTER_RESULT_OK) {
		receipt->result = admission;
		return admission;
	}
	if (profile_ != nullptr && profile_ != profile)
		return MISTER_RESULT_UNSUPPORTED;
	if (identity.sequence < last_sequence_[player]) {
		receipt->result = MISTER_RESULT_PLATFORM;
		return MISTER_RESULT_PLATFORM;
	}
	if (identity.sequence == last_sequence_[player]) {
		if (map != last_map_[player] ||
			(uncertain_[player] &&
				(identity.sequence != uncertain_identity_[player].sequence ||
					map != uncertain_map_[player]))) {
			receipt->result = MISTER_RESULT_PLATFORM;
			return MISTER_RESULT_PLATFORM;
		}
		if (!uncertain_[player]) {
			receipt->result = MISTER_RESULT_OK;
			return MISTER_RESULT_OK;
		}
	} else {
		last_sequence_[player] = identity.sequence;
		last_map_[player] = map;
	}
	if (!uncertain_[player] && ledger_valid_[player] &&
		ledger_[player].map == map) {
		receipt->result = MISTER_RESULT_OK;
		return MISTER_RESULT_OK;
	}
	if (!uncertain_[player] && !ledger_valid_[player] && map == 0) {
		receipt->result = MISTER_RESULT_OK;
		return MISTER_RESULT_OK;
	}

	pending_ = {true, event.player,
		map, profile->input.player_command[player], identity};
	const uint16_t words[2] = {pending_.command, map};
	const SpiWords transaction = {words, nullptr, kNativeDigitalWordCount, 0};
	const SpiReceiptCommitToken token(this, &NativeInput::CommitReceipt);
	const Result result = port_.Execute(*view, transaction,
		receipt, &token);
	if (result != MISTER_RESULT_OK) {
		uncertain_[player] = true;
		uncertain_map_[player] = map;
		uncertain_identity_[player] = identity;
		pending_.active = false;
		return result;
	}
	if (pending_.active) {
		uncertain_[player] = true;
		uncertain_map_[player] = map;
		uncertain_identity_[player] = identity;
		pending_.active = false;
		receipt->result = MISTER_RESULT_PLATFORM;
		return MISTER_RESULT_PLATFORM;
	}
	if (profile_ == nullptr) profile_ = profile;
	return MISTER_RESULT_OK;
}

} // namespace native
} // namespace mister
