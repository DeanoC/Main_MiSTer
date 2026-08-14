// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_input.hpp"
#include "runtime/native/native_resources.hpp"

namespace mister {
namespace native {

NativeInput::NativeInput(NativeInputSpiPort &port)
	: port_(port), profile_(nullptr), ledger_(), ledger_valid_{false, false},
	  uncertain_{false, false}, uncertain_map_{0, 0},
	  uncertain_identity_{{0, 0}, {0, 0}},
	  last_sequence_{0, 0}, last_map_{0, 0},
	  pending_{false, 0, 0, 0, {0, 0}},
	  cleanup_pending_{false, 0, nullptr, false, {}, false, 0, {0, 0}, 0, 0},
	  cleanup_neutral_completed_{false, false},
	  cleanup_neutral_profile_{nullptr, nullptr},
	  cleanup_residue_{{false, nullptr, 0, false, false, 0},
		{false, nullptr, 1, false, false, 0}},
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	  fail_next_commit_for_test_(false),
#endif
	  mutex_()
{
}

void NativeInput::CommitCleanupReceipt(void *context,
	const SpiReceipt &receipt) noexcept
{
	static_cast<NativeInput *>(context)->CommitCleanupNeutral(receipt);
}

void NativeInput::CommitCleanupNeutral(const SpiReceipt &receipt) noexcept
{
	if (!cleanup_pending_.active || receipt.result != MISTER_RESULT_OK ||
		receipt.completed_words != kNativeDigitalWordCount ||
		!receipt.ack_low_observed || !receipt.strobe_low_observed ||
		!receipt.deselected || receipt.target_may_be_selected ||
		receipt.strobe_may_be_high)
		return;
	const size_t player = cleanup_pending_.player;
	if (player >= kNativePlayerCount ||
		cleanup_pending_.profile == nullptr ||
		ledger_valid_[player] != cleanup_pending_.ledger_valid ||
		(cleanup_pending_.ledger_valid &&
			(ledger_[player].player != cleanup_pending_.ledger.player ||
			 ledger_[player].player_command != cleanup_pending_.ledger.player_command ||
			 ledger_[player].map != cleanup_pending_.ledger.map ||
			 ledger_[player].inverse_words[0] !=
				cleanup_pending_.ledger.inverse_words[0] ||
			 ledger_[player].inverse_words[1] !=
				cleanup_pending_.ledger.inverse_words[1] ||
			 ledger_[player].identity.player !=
				cleanup_pending_.ledger.identity.player ||
			 ledger_[player].identity.sequence !=
				cleanup_pending_.ledger.identity.sequence)) ||
		uncertain_[player] != cleanup_pending_.uncertain ||
		uncertain_map_[player] != cleanup_pending_.uncertain_map ||
		uncertain_identity_[player].player !=
			cleanup_pending_.uncertain_identity.player ||
		uncertain_identity_[player].sequence !=
			cleanup_pending_.uncertain_identity.sequence ||
		last_sequence_[player] != cleanup_pending_.last_sequence ||
		last_map_[player] != cleanup_pending_.last_map)
		return;
	const bool had_history = last_sequence_[player] != 0;
	ledger_valid_[player] = false;
	uncertain_[player] = false;
	uncertain_map_[player] = 0;
	uncertain_identity_[player] = {static_cast<uint8_t>(player), 0};
	if (had_history) last_map_[player] = 0;
	cleanup_residue_[player] = {
		false, nullptr, static_cast<uint8_t>(player), false, false, 0};
	cleanup_neutral_profile_[player] = cleanup_pending_.profile;
	cleanup_neutral_completed_[player] = true;
	cleanup_pending_.active = false;
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
		if (profile_ == nullptr) profile_ = profile;
		if (!uncertain_[player]) {
			receipt->result = MISTER_RESULT_OK;
			return MISTER_RESULT_OK;
		}
	} else {
		if (profile_ == nullptr) profile_ = profile;
		last_sequence_[player] = identity.sequence;
		last_map_[player] = map;
		if (!uncertain_[player] && ledger_valid_[player] && map != 0 &&
			ledger_[player].map == map)
			ledger_[player].identity = identity;
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

Result NativeInput::ReplayDigitalNeutral(const NativeCoreProfile *profile,
	const OperationLease &lease, const NativeDigitalNeutral &neutral,
	SpiReceipt *receipt)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ClearReceipt(receipt);
	if (receipt == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	if (profile == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	if (!ValidateNativeCoreProfileRecord(*profile))
		return MISTER_RESULT_UNSUPPORTED;
	if (neutral.player >= kNativePlayerCount || profile->input.joystick_swap ||
		neutral.words[0] != profile->input.player_command[neutral.player] ||
		neutral.words[1] != 0)
		return MISTER_RESULT_UNSUPPORTED;
	const size_t player = neutral.player;
	const uint16_t words[2] = {neutral.words[0], neutral.words[1]};
	std::unique_ptr<CleanupInputReplayView> view;
	Result result = lease.AcquireCleanupInputReplayView(*profile, neutral.player,
		words, &view);
	if (result != MISTER_RESULT_OK) {
		receipt->result = result;
		return result;
	}
	result = port_.ValidateCleanupDigitalNeutralAuthority(*view);
	if (result != MISTER_RESULT_OK) {
		receipt->result = result;
		return result;
	}
	receipt->mapping_retained = true;
	if (profile_ != nullptr && profile_ != profile) {
		receipt->result = MISTER_RESULT_UNSUPPORTED;
		return receipt->result;
	}
	if (pending_.active || cleanup_pending_.active) {
		receipt->result = MISTER_RESULT_INVALID_STATE;
		return receipt->result;
	}
	for (size_t index = 0; index < kNativePlayerCount; ++index) {
		if (!cleanup_residue_[index].retained) continue;
		if (index != player || cleanup_residue_[index].player != player ||
			cleanup_residue_[index].profile != profile) {
			receipt->result = MISTER_RESULT_INVALID_STATE;
			return receipt->result;
		}
	}
	bool closed_prefix = false;
	SpiReceipt prefix_receipt = {};
	if (cleanup_residue_[player].retained) {
		result = port_.CloseCleanupDigitalNeutralResidue(*view,
			&cleanup_residue_[player], receipt);
		if (result != MISTER_RESULT_OK || cleanup_residue_[player].retained) {
			receipt->result = result == MISTER_RESULT_OK ?
				MISTER_RESULT_PLATFORM : result;
			return receipt->result;
		}
		closed_prefix = true;
		prefix_receipt = *receipt;
	}
	if (cleanup_neutral_completed_[player]) {
		result = cleanup_neutral_profile_[player] == profile ?
			MISTER_RESULT_OK : MISTER_RESULT_UNSUPPORTED;
		receipt->result = result;
		return result;
	}
	bool execute = false;
	if (uncertain_[player]) {
		if (uncertain_identity_[player].player != player ||
			uncertain_identity_[player].sequence == 0 ||
			uncertain_identity_[player].sequence != last_sequence_[player] ||
			uncertain_map_[player] != last_map_[player] ||
			(ledger_valid_[player] &&
				(ledger_[player].player != player || ledger_[player].map == 0 ||
				 ledger_[player].player_command !=
					profile->input.player_command[player] ||
				 ledger_[player].inverse_words[0] !=
					ledger_[player].player_command ||
				 ledger_[player].inverse_words[1] != 0 ||
				 ledger_[player].identity.player != player ||
				 ledger_[player].identity.sequence == 0 ||
				 ledger_[player].identity.sequence > last_sequence_[player]))) {
			receipt->result = MISTER_RESULT_PLATFORM;
			return receipt->result;
		}
		execute = true;
	} else if (ledger_valid_[player]) {
		const DeliveredInput &delivered = ledger_[player];
		if (delivered.player != player || delivered.map == 0 ||
			delivered.player_command != profile->input.player_command[player] ||
			delivered.inverse_words[0] != delivered.player_command ||
			delivered.inverse_words[1] != 0 ||
			delivered.identity.player != player ||
			delivered.identity.sequence == 0 ||
			delivered.identity.sequence != last_sequence_[player] ||
			delivered.map != last_map_[player]) {
			receipt->result = MISTER_RESULT_PLATFORM;
			return receipt->result;
		}
		execute = true;
	} else if (last_sequence_[player] == 0 && last_map_[player] == 0) {
		execute = true;
	} else if (last_sequence_[player] != 0 && last_map_[player] == 0) {
		receipt->result = MISTER_RESULT_OK;
		return MISTER_RESULT_OK;
	} else {
		receipt->result = MISTER_RESULT_PLATFORM;
		return receipt->result;
	}
	if (!execute) {
		receipt->result = MISTER_RESULT_PLATFORM;
		return receipt->result;
	}
	cleanup_pending_ = {true, static_cast<uint8_t>(player), profile,
		ledger_valid_[player], ledger_[player], uncertain_[player],
		uncertain_map_[player], uncertain_identity_[player],
		last_sequence_[player], last_map_[player]};
	const SpiReceiptCommitToken token(this, &NativeInput::CommitCleanupReceipt);
	result = port_.ExecuteCleanupDigitalNeutral(*view, receipt, &token);
	if (closed_prefix) {
		receipt->force_strobe_low_attempted =
			receipt->force_strobe_low_attempted ||
			prefix_receipt.force_strobe_low_attempted;
		receipt->force_strobe_low_applied = receipt->force_strobe_low_applied ||
			prefix_receipt.force_strobe_low_applied;
		receipt->force_strobe_low_observed =
			receipt->force_strobe_low_observed ||
			prefix_receipt.force_strobe_low_observed;
		if (prefix_receipt.mutation_sequence > receipt->mutation_sequence)
			receipt->mutation_sequence = prefix_receipt.mutation_sequence;
	}
	if (result != MISTER_RESULT_OK) {
		cleanup_pending_.active = false;
		cleanup_residue_[player] = {receipt->target_may_be_selected ||
			receipt->strobe_may_be_high, profile, static_cast<uint8_t>(player),
			receipt->target_may_be_selected, receipt->strobe_may_be_high,
			receipt->mutation_sequence};
		return result;
	}
	if (cleanup_pending_.active) {
		cleanup_pending_.active = false;
		receipt->result = MISTER_RESULT_PLATFORM;
		return receipt->result;
	}
	if (profile_ == nullptr) profile_ = profile;
	return MISTER_RESULT_OK;
}

} // namespace native
} // namespace mister
