// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_INPUT_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_INPUT_HPP

#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_spi_bus.hpp"

#include <stddef.h>
#include <stdint.h>

#include <mutex>

namespace mister {
namespace native {

enum class NativeInputKind : uint8_t {
	digital,
	keyboard,
	mouse,
	analog,
	rumble,
	ui,
	admin,
	reconfiguration,
	unknown
};

struct InputIdentity {
	uint8_t player = 0;
	// Public delivery rejects zero; the serialized event source owns sequencing.
	uint64_t sequence = 0;
};

struct NativeInputEvent {
	NativeInputKind kind;
	uint8_t player;
	uint32_t map;
	bool pressed;
	bool joystick_swap;
	uint16_t code;
	InputIdentity identity = {0, 0};
};

struct DeliveredInput {
	uint8_t player;
	uint16_t player_command;
	uint16_t map;
	uint16_t inverse_words[2];
	InputIdentity identity;
};

class NativeInput final {
public:
	explicit NativeInput(NativeInputSpiPort &port);

	Result Deliver(const NativeCoreProfile *profile,
		const OperationLease &lease, const NativeInputEvent &event,
		SpiReceipt *receipt);

	size_t ledger_size() const;
	bool GetDeliveredInput(size_t index, DeliveredInput *snapshot) const;
	Result SnapshotDeliveredInput(HardwareBroker &owner,
		const NativeCoreProfile *profile, const OperationLease &lease,
		DeliveredInput *values, bool *valid, size_t count) const;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	void FailNextCommitForTest();
#endif

private:
	struct PendingCommit {
		bool active;
		uint8_t player;
		uint16_t map;
		uint16_t command;
		InputIdentity identity;
	};
	static void ClearReceipt(SpiReceipt *receipt);
	static void CommitReceipt(void *context,
		const SpiReceipt &receipt) noexcept;
	void CommitDelivered(const SpiReceipt &receipt) noexcept;
	NativeInputSpiPort &port_;
	const NativeCoreProfile *profile_;
	DeliveredInput ledger_[kNativePlayerCount];
	bool ledger_valid_[kNativePlayerCount];
	bool uncertain_[kNativePlayerCount];
	uint16_t uncertain_map_[kNativePlayerCount];
	InputIdentity uncertain_identity_[kNativePlayerCount];
	uint64_t last_sequence_[kNativePlayerCount];
	uint16_t last_map_[kNativePlayerCount];
	PendingCommit pending_;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	bool fail_next_commit_for_test_;
#endif
	mutable std::mutex mutex_;
};

} // namespace native
} // namespace mister

#endif
