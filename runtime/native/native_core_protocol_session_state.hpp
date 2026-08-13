// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROTOCOL_SESSION_STATE_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_CORE_PROTOCOL_SESSION_STATE_HPP

#include <stdint.h>

#include <atomic>
#include <memory>

namespace mister {
namespace native {

class HardwareLeaseView;
struct NativeCoreProfile;
struct OperationRegistration;

enum class ProtocolSessionHandleState : uint8_t {
	live,
	abandoned,
	finalized
};

struct ProtocolSessionState {
	ProtocolSessionState();
	~ProtocolSessionState();

	std::unique_ptr<HardwareLeaseView> view;
	std::weak_ptr<OperationRegistration> owner_registration;
	const NativeCoreProfile *profile;
	uint64_t initial_mutation_sequence;
	std::atomic<ProtocolSessionHandleState> handle_state;
};

inline void MarkProtocolSessionAbandoned(
	const std::shared_ptr<ProtocolSessionState> &state)
{
	if (!state) return;
	ProtocolSessionHandleState expected = ProtocolSessionHandleState::live;
	state->handle_state.compare_exchange_strong(expected,
		ProtocolSessionHandleState::abandoned);
}

} // namespace native
} // namespace mister

#endif
