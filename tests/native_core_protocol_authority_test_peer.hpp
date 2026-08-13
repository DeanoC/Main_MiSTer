// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_TESTS_NATIVE_CORE_PROTOCOL_AUTHORITY_TEST_PEER_HPP
#define MISTER_TESTS_NATIVE_CORE_PROTOCOL_AUTHORITY_TEST_PEER_HPP

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_core_protocol.hpp"

namespace mister {
namespace native {

class CoreProtocolAuthorityTestPeer final {
public:
	static Result AcquireActive(const OperationLease &lease,
		HardwareBroker &broker, const NativeCoreProfile &profile,
		std::unique_ptr<ActiveCoreProtocolSession> *session)
	{
		return lease.AcquireActiveCoreProtocolSession(broker, profile, session);
	}

	static Result AcquireCleanup(const OperationLease &lease,
		HardwareBroker &broker,
		std::unique_ptr<CleanupCoreProtocolSession> *session)
	{
		return lease.AcquireCleanupCoreProtocolSession(broker, session);
	}

	static Result AcquireRecovery(const OperationLease &lease,
		HardwareBroker &broker,
		std::unique_ptr<RecoveryCoreProtocolSession> *session)
	{
		return lease.AcquireRecoveryCoreProtocolSession(broker, session);
	}

	static uint64_t RecordMutation(const OperationLease &lease,
		HardwareBroker &broker, ActiveCoreProtocolSession &session)
	{
		uint64_t sequence = 0;
		return lease.RecordActiveCoreProtocolMutationForTest(broker, session,
			&sequence) == MISTER_RESULT_OK ? sequence : 0;
	}

	static Result CompleteFailed(const OperationLease &lease,
		HardwareBroker &broker,
		std::unique_ptr<ActiveCoreProtocolSession> &&session,
		const ActiveProtocolFailureReceipt &receipt)
	{
		return lease.CompleteFailedCoreProtocolSession(broker,
			std::move(session), receipt);
	}

	static Result CompleteSuccess(const OperationLease &lease,
		HardwareBroker &broker,
		std::unique_ptr<ActiveCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt)
	{
		return lease.CompleteSuccessfulCoreProtocolSession(broker,
			std::move(session), receipt);
	}

	static bool SessionCurrent(HardwareBroker &broker)
	{
		return broker.core_protocol_session_current_for_test();
	}

	static Result CompleteCleanup(const OperationLease &lease,
		HardwareBroker &broker,
		std::unique_ptr<CleanupCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt)
	{
		return lease.CompleteCleanupCoreProtocolSession(broker,
			std::move(session), receipt);
	}

	static Result CompleteRecovery(const OperationLease &lease,
		HardwareBroker &broker,
		std::unique_ptr<RecoveryCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt)
	{
		return lease.CompleteRecoveryCoreProtocolSession(broker,
			std::move(session), receipt);
	}

	static Result CompleteInvalidOutcome(const OperationLease &lease,
		HardwareBroker &broker, Result primary_result)
	{
		return lease.CompleteInvalidCoreProtocolOutcome(broker, primary_result);
	}

	static Result BeginConcurrentActive(const OperationLease &lease,
		HardwareBroker &broker, OperationKind operation_kind,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<OperationLease> *concurrent)
	{
		return lease.BeginConcurrentActiveOperationForTest(broker,
			operation_kind, absolute_deadline_ms, concurrent);
	}
};

} // namespace native
} // namespace mister

#endif
