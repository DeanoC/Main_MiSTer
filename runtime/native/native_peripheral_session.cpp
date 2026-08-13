// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_peripheral_session.hpp"

#include "runtime/native/hardware_broker.hpp"

#include <atomic>

namespace mister {
namespace native {

PeripheralSessionState::PeripheralSessionState(PeripheralSessionKind session_kind,
	const PeripheralBackendIdentity &backend_identity)
	: view(), owner_registration(), profile(nullptr), recovery_record(nullptr),
	  backend(backend_identity), kind(session_kind), initial_mutation_sequence(0),
	  absolute_deadline_ms(0), action_profile_identity(nullptr),
	  action(PeripheralSessionAction::none), action_word_count(0),
	  action_next_word_index(0), action_transaction_closed(true),
	  last_mutation_sequence(0),
	  action_progress_unknown(false), recheckout_allowed(true),
	  phase(PeripheralSessionPhase::live)
{
}

PeripheralSessionState::~PeripheralSessionState() = default;

ActiveAudioSession::ActiveAudioSession() : state_() {}
ActiveAudioSession::~ActiveAudioSession() { MarkPeripheralSessionAbandoned(state_); }
CleanupAudioSession::CleanupAudioSession() : state_() {}
CleanupAudioSession::~CleanupAudioSession() { MarkPeripheralSessionAbandoned(state_); }
RecoveryAudioSession::RecoveryAudioSession() : state_() {}
RecoveryAudioSession::~RecoveryAudioSession() { MarkPeripheralSessionAbandoned(state_); }
ActiveVideoSession::ActiveVideoSession() : state_() {}
ActiveVideoSession::~ActiveVideoSession() { MarkPeripheralSessionAbandoned(state_); }
CleanupVideoSession::CleanupVideoSession() : state_() {}
CleanupVideoSession::~CleanupVideoSession() { MarkPeripheralSessionAbandoned(state_); }
RecoveryVideoSession::RecoveryVideoSession() : state_() {}
RecoveryVideoSession::~RecoveryVideoSession() { MarkPeripheralSessionAbandoned(state_); }
ActiveAudioVideoSession::ActiveAudioVideoSession() : state_() {}
ActiveAudioVideoSession::~ActiveAudioVideoSession() { MarkPeripheralSessionAbandoned(state_); }
CleanupAudioVideoSession::CleanupAudioVideoSession() : state_() {}
CleanupAudioVideoSession::~CleanupAudioVideoSession() { MarkPeripheralSessionAbandoned(state_); }
RecoveryAudioVideoSession::RecoveryAudioVideoSession() : state_() {}
RecoveryAudioVideoSession::~RecoveryAudioVideoSession() { MarkPeripheralSessionAbandoned(state_); }

#if defined(MISTER_NATIVE_PROFILE_TESTING)
namespace {
std::atomic<size_t> g_allocation_failure_index(0);
std::atomic<size_t> g_allocation_attempt_index(0);
} // namespace

void SetPeripheralSessionAllocationFailureForTest(size_t allocation_index)
{
	g_allocation_attempt_index.store(0, std::memory_order_relaxed);
	g_allocation_failure_index.store(allocation_index, std::memory_order_relaxed);
}

void ClearPeripheralSessionAllocationFailureForTest()
{
	g_allocation_failure_index.store(0, std::memory_order_relaxed);
	g_allocation_attempt_index.store(0, std::memory_order_relaxed);
}

bool PeripheralSessionAllocationAllowedForTest()
{
	const size_t attempt = g_allocation_attempt_index.fetch_add(1,
		std::memory_order_relaxed) + 1;
	const size_t failure = g_allocation_failure_index.load(
		std::memory_order_relaxed);
	return failure == 0 || attempt != failure;
}
#endif

} // namespace native
} // namespace mister
