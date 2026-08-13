// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_TESTS_NATIVE_PERIPHERAL_AUTHORITY_TEST_PEER_HPP
#define MISTER_TESTS_NATIVE_PERIPHERAL_AUTHORITY_TEST_PEER_HPP

#include "runtime/native/hardware_broker.hpp"

namespace mister {
namespace native {

// Test-only peer used by lifecycle fakes. Production adapters own these calls
// through their closed typed bundles; this peer never ships in a target build.
class PeripheralAuthorityTestPeer final {
public:
	static PeripheralBackendIdentity Backend(HardwareBroker &broker,
		const void *adapter_instance)
	{
		return broker.CreatePeripheralBackendIdentity(adapter_instance);
	}
	template <typename Session>
	static uint64_t Deadline(const Session &session)
	{
		return session.state_ ? session.state_->absolute_deadline_ms : 0;
	}
	template <typename Session>
	static uint64_t Deadline(const PeripheralSessionBundle<Session> &bundle)
	{
		return bundle.session_ ? Deadline(*bundle.session_) : 0;
	}
	template <typename Session>
	static Result Record(HardwareBroker &broker,
		PeripheralSessionBundle<Session> &bundle, uint64_t *sequence)
	{
		return bundle.session_ ? Record(broker, *bundle.session_, sequence) :
			MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result AcquireActiveAudio(const OperationLease &lease,
		HardwareBroker &broker, const NativeCoreProfile &profile,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveAudioSessionBundle> *bundle)
	{
		return lease.AcquireActiveAudioSession(broker, profile, backend, bundle);
	}
	static Result AcquireActiveVideo(const OperationLease &lease,
		HardwareBroker &broker, const NativeCoreProfile &profile,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveVideoSessionBundle> *bundle)
	{
		return lease.AcquireActiveVideoSession(broker, profile, backend, bundle);
	}
	static Result AcquireCleanupAudio(const OperationLease &lease,
		HardwareBroker &broker, const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupAudioSessionBundle> *bundle)
	{
		return lease.AcquireCleanupAudioSession(broker, backend, bundle);
	}
	static Result AudioDisposition(const OperationLease &lease,
		HardwareBroker &broker, PeripheralBrokerDisposition *disposition)
	{
		return lease.GetAudioSessionDisposition(broker, disposition);
	}
	static Result AcquireCleanupVideo(const OperationLease &lease,
		HardwareBroker &broker, const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupVideoSessionBundle> *bundle)
	{
		return lease.AcquireCleanupVideoSession(broker, backend, bundle);
	}
	static Result AcquireRecoveryAudio(const OperationLease &lease,
		HardwareBroker &broker, const SafeAudioRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryAudioSessionBundle> *bundle)
	{
		return lease.AcquireRecoveryAudioSession(broker, record, backend, bundle);
	}
	static Result AcquireRecoveryVideo(const OperationLease &lease,
		HardwareBroker &broker, const SafeVideoRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryVideoSessionBundle> *bundle)
	{
		return lease.AcquireRecoveryVideoSession(broker, record, backend, bundle);
	}
	static Result AcquireActiveAudioVideo(const OperationLease &lease,
		HardwareBroker &broker, const NativeCoreProfile &profile,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveAudioVideoSessionBundle> *bundle)
	{
		return lease.AcquireActiveAudioVideoSession(broker, profile, backend,
			bundle);
	}
	static Result AcquireCleanupAudioVideo(const OperationLease &lease,
		HardwareBroker &broker, const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupAudioVideoSessionBundle> *bundle)
	{
		return lease.AcquireCleanupAudioVideoSession(broker, backend, bundle);
	}
	static Result AcquireRecoveryAudioVideo(const OperationLease &lease,
		HardwareBroker &broker, const SafeAudioVideoRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryAudioVideoSessionBundle> *bundle)
	{
		return lease.AcquireRecoveryAudioVideoSession(broker, record, backend,
			bundle);
	}
	static Result VideoDisposition(const OperationLease &lease,
		HardwareBroker &broker, PeripheralBrokerDisposition *disposition)
	{
		return lease.GetVideoSessionDisposition(broker, disposition);
	}
	static Result AudioVideoDisposition(const OperationLease &lease,
		HardwareBroker &broker, PeripheralBrokerDisposition *disposition)
	{
		return lease.GetAudioVideoSessionDisposition(broker, disposition);
	}
	static Result Record(HardwareBroker &broker, ActiveAudioSession &session,
		uint64_t *sequence)
	{
		return broker.RecordPeripheralMutation(session.state_, sequence);
	}
	static Result Record(HardwareBroker &broker, RecoveryAudioSession &session,
		uint64_t *sequence)
	{
		return broker.RecordPeripheralMutation(session.state_, sequence);
	}
	static Result Record(HardwareBroker &broker, ActiveVideoSession &session,
		uint64_t *sequence)
	{
		return broker.RecordPeripheralMutation(session.state_, sequence);
	}
	static Result Record(HardwareBroker &broker, ActiveAudioVideoSession &session,
		uint64_t *sequence)
	{
		return broker.RecordPeripheralMutation(session.state_, sequence);
	}
	static Result Record(HardwareBroker &broker, RecoveryAudioVideoSession &session,
		uint64_t *sequence)
	{
		return broker.RecordPeripheralMutation(session.state_, sequence);
	}
	static Result CompleteAudio(HardwareBroker &broker,
		std::unique_ptr<ActiveAudioSession> &&session,
		const PeripheralCompletionReceipt &receipt)
	{
		return broker.CompleteActiveAudioSuccess(std::move(session), receipt);
	}
	template <typename Session>
	static Result CompleteAudio(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const PeripheralCompletionReceipt &receipt)
	{
		return bundle && bundle->session_ ? CompleteAudio(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result CompleteAudio(HardwareBroker &broker,
		std::unique_ptr<RecoveryAudioSession> &&session,
		const PeripheralCompletionReceipt &receipt)
	{
		return broker.CompleteRecoveryAudio(std::move(session), receipt);
	}
	static Result CompleteVideo(HardwareBroker &broker,
		std::unique_ptr<ActiveVideoSession> &&session,
		const PeripheralCompletionReceipt &receipt)
	{
		return broker.CompleteActiveVideoSuccess(std::move(session), receipt);
	}
	template <typename Session>
	static Result CompleteVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const PeripheralCompletionReceipt &receipt)
	{
		return bundle && bundle->session_ ? CompleteVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result CompleteVideo(HardwareBroker &broker,
		std::unique_ptr<RecoveryVideoSession> &&session,
		const PeripheralCompletionReceipt &receipt)
	{
		return broker.CompleteRecoveryVideo(std::move(session), receipt);
	}
	static Result FailAudio(HardwareBroker &broker,
		std::unique_ptr<ActiveAudioSession> &&session,
		const PeripheralFailureReceipt &receipt)
	{
		return broker.CompleteActiveAudioFailure(std::move(session), receipt);
	}
	template <typename Session>
	static Result FailAudio(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const PeripheralFailureReceipt &receipt)
	{
		return bundle && bundle->session_ ? FailAudio(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result FailVideo(HardwareBroker &broker,
		std::unique_ptr<ActiveVideoSession> &&session,
		const PeripheralFailureReceipt &receipt)
	{
		return broker.CompleteActiveVideoFailure(std::move(session), receipt);
	}
	template <typename Session>
	static Result FailVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const PeripheralFailureReceipt &receipt)
	{
		return bundle && bundle->session_ ? FailVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result CompleteAudio(HardwareBroker &broker,
		std::unique_ptr<CleanupAudioSession> &&session,
		const PeripheralCompletionReceipt &receipt)
	{
		return broker.CompleteCleanupAudio(std::move(session), receipt);
	}
	static Result CompleteVideo(HardwareBroker &broker,
		std::unique_ptr<CleanupVideoSession> &&session,
		const PeripheralCompletionReceipt &receipt)
	{
		return broker.CompleteCleanupVideo(std::move(session), receipt);
	}
	static Result AbandonAudio(HardwareBroker &broker,
		std::unique_ptr<CleanupAudioSession> &&session,
		const PeripheralFailureReceipt &receipt)
	{
		return broker.AbandonCleanupAudio(std::move(session), receipt);
	}
	template <typename Session>
	static Result AbandonAudio(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const PeripheralFailureReceipt &receipt)
	{
		return bundle && bundle->session_ ? AbandonAudio(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result AbandonVideo(HardwareBroker &broker,
		std::unique_ptr<CleanupVideoSession> &&session,
		const PeripheralFailureReceipt &receipt)
	{
		return broker.AbandonCleanupVideo(std::move(session), receipt);
	}
	template <typename Session>
	static Result AbandonVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const PeripheralFailureReceipt &receipt)
	{
		return bundle && bundle->session_ ? AbandonVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result CompleteAudioVideo(HardwareBroker &broker,
		std::unique_ptr<ActiveAudioVideoSession> &&session,
		const CoupledAcquisitionReceipt &receipt)
	{
		return broker.CompleteActiveAudioVideoSuccess(std::move(session), receipt);
	}
	template <typename Session>
	static Result CompleteAudioVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const CoupledAcquisitionReceipt &receipt)
	{
		return bundle && bundle->session_ ? CompleteAudioVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result CompleteAudioVideo(HardwareBroker &broker,
		std::unique_ptr<RecoveryAudioVideoSession> &&session,
		const CoupledRecoveryReceipt &receipt)
	{
		return broker.CompleteRecoveryAudioVideo(std::move(session), receipt);
	}
	template <typename Session>
	static Result CompleteAudioVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const CoupledRecoveryReceipt &receipt)
	{
		return bundle && bundle->session_ ? CompleteAudioVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result AbandonAudioVideo(HardwareBroker &broker,
		std::unique_ptr<RecoveryAudioVideoSession> &&session,
		const CoupledFailureReceipt &receipt)
	{
		return broker.AbandonRecoveryAudioVideo(std::move(session), receipt);
	}
	template <typename Session>
	static Result AbandonAudioVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const CoupledFailureReceipt &receipt)
	{
		return bundle && bundle->session_ ? AbandonAudioVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result RecordCoupledRecovery(HardwareBroker &broker,
		const RecoveryEpoch &epoch, const OperationLease &lease,
		const CoupledRecoveryReceipt &receipt, Result result)
	{
		return broker.RecordCoupledRecoveryOperation(epoch, lease, receipt, result);
	}
	static Result FailAudioVideo(HardwareBroker &broker,
		std::unique_ptr<ActiveAudioVideoSession> &&session,
		const CoupledFailureReceipt &receipt)
	{
		return broker.CompleteActiveAudioVideoFailure(std::move(session), receipt);
	}
	template <typename Session>
	static Result FailAudioVideo(HardwareBroker &broker,
		std::unique_ptr<PeripheralSessionBundle<Session>> &&bundle,
		const CoupledFailureReceipt &receipt)
	{
		return bundle && bundle->session_ ? FailAudioVideo(broker,
			std::move(bundle->session_), receipt) : MISTER_RESULT_INVALID_ARGUMENT;
	}
	static Result CompleteAudioVideo(HardwareBroker &broker,
		std::unique_ptr<CleanupAudioVideoSession> &&session,
		const CoupledCompletionReceipt &receipt)
	{
		return broker.CompleteCleanupAudioVideo(std::move(session), receipt);
	}
};

} // namespace native
} // namespace mister

#endif
