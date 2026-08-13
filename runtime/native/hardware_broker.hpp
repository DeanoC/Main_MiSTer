// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_HARDWARE_BROKER_HPP
#define MISTER_RUNTIME_NATIVE_HARDWARE_BROKER_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_peripheral_session.hpp"

#include <stddef.h>
#include <stdint.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace mister {
namespace native {

using PlatformGenerationId = uint64_t;
using Result = MisterResult;

enum class LeaseAuthority : uint8_t {
	active_generation,
	cleanup_epoch,
	recovery_epoch
};

enum class OperationKind : uint8_t {
	program_fpga,
	core_protocol,
	input,
	scheduler,
	offload,
	save,
	audio,
	video,
	audio_video,
	content,
	input_descriptors,
	terminal_fpga_cleanup
};

enum class CoreProtocolBrokerDisposition : uint8_t {
	no_session,
	session_current,
	session_abandoned,
	success_completed,
	failure_completed
};

enum class RecoveryFailurePersistence : uint8_t {
	retryable,
	terminal
};

class HardwareBroker;
class OperationLease;
class OperationInvocation;
class NativeInput;
class NativeSpiBus;
class NativeContainment;
class NativeRecovery;
class NativeCoreProtocol;
class NativeCoreProtocolTeardown;
class NativeLifecycle;
class CoreProtocolAuthorityTestPeer;
class PeripheralAuthorityTestPeer;
class ActiveCoreProtocolSession;
class CleanupCoreProtocolSession;
class RecoveryCoreProtocolSession;
class ActiveAudioSession;
class CleanupAudioSession;
class RecoveryAudioSession;
class ActiveVideoSession;
class CleanupVideoSession;
class RecoveryVideoSession;
class ActiveAudioVideoSession;
class CleanupAudioVideoSession;
class RecoveryAudioVideoSession;
class ContainmentResumeKey;
enum class RecoveryResourceState : uint8_t;
struct BrokerLifetime;
struct OperationRegistration;
struct ProtocolSessionState;
namespace linux_native {
class NativeFpgaProgrammer;
class NativeInputAdapter;
class NativeSchedulerAdapter;
class NativeOffloadAdapter;
class NativeCoreProtocolIoAdapter;
class NativeCoreProtocolIoAdapterImpl;
class NativeCoreProtocolActiveAdapterView;
class NativeCoreProtocolCleanupAdapterView;
class NativeCoreProtocolRecoveryAdapterView;
class NativeAudioAdapter;
class NativeVideoAdapter;
class NativeAvIoAdapter;
class NativeSaveAdapter;
}

class ProcessOperationGuard final {
public:
	~ProcessOperationGuard();
	ProcessOperationGuard(const ProcessOperationGuard &) = delete;
	ProcessOperationGuard &operator=(const ProcessOperationGuard &) = delete;
	ProcessOperationGuard(ProcessOperationGuard &&) = delete;
	ProcessOperationGuard &operator=(ProcessOperationGuard &&) = delete;

	uint64_t absolute_deadline_ms() const;

private:
	friend class HardwareBroker;
	friend class OperationLease;
	friend class linux_native::NativeInputAdapter;
	friend class linux_native::NativeSchedulerAdapter;
	friend class linux_native::NativeOffloadAdapter;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeVideoAdapter;
	friend class linux_native::NativeAvIoAdapter;
	friend class linux_native::NativeSaveAdapter;
	explicit ProcessOperationGuard(
		const std::shared_ptr<OperationRegistration> &registration);
	LeaseAuthority authority() const;
	std::shared_ptr<OperationRegistration> registration_;
	bool lifetime_registered_;
};

class HardwareLeaseView final {
public:
	~HardwareLeaseView();
	HardwareLeaseView(const HardwareLeaseView &) = delete;
	HardwareLeaseView &operator=(const HardwareLeaseView &) = delete;

private:
	friend class HardwareBroker;
	friend class NativeSpiBus;
	friend class NativeContainment;
	friend class NativeCoreProtocol;
	friend class ActiveCoreProtocolSession;
	friend class CleanupCoreProtocolSession;
	friend class RecoveryCoreProtocolSession;
	friend class linux_native::NativeFpgaProgrammer;
	friend class linux_native::NativeCoreProtocolIoAdapterImpl;
	explicit HardwareLeaseView(
		const std::shared_ptr<OperationRegistration> &registration);
	uint64_t RecordMutation();
	uint64_t CurrentMutationSequence() const;
	Result RecordFpgaProgrammingMutation(size_t accepted_bytes,
		uint64_t *mutation_sequence);
	Result AuthorizeFpgaProgrammingProfile(
		const NativeCoreProfile &profile) const;
	uint64_t absolute_deadline_ms() const;

	std::shared_ptr<OperationRegistration> registration_;
};

struct CoreProtocolResidue {
	bool mapping_retained;
	bool identity_mode_may_be_asserted;
	bool user_io_selected;
	bool file_io_selected;
	bool strobe_may_be_high;
	bool download_may_be_active;
	bool status_reset_asserted;
	uint64_t last_mutation_sequence;
};

struct ProtocolMappingReleaseReceipt {
	Result result;
	bool selected_transaction_closed;
	bool unmap_attempted;
	bool mapping_absent;
	bool descriptor_close_attempted;
	bool descriptor_absent;
	uint64_t mutation_sequence;
};

struct ActiveProtocolFailureReceipt {
	Result primary_result;
	CoreProtocolResidue residue;
	ProtocolMappingReleaseReceipt mapping_release;
	uint64_t final_mutation_sequence;
};

class OperationLease final {
public:
	~OperationLease();
	OperationLease(const OperationLease &) = delete;
	OperationLease &operator=(const OperationLease &) = delete;
	OperationLease(OperationLease &&) = delete;
	OperationLease &operator=(OperationLease &&) = delete;

	OperationKind operation_kind() const;
	uint64_t absolute_deadline_ms() const;

private:
	friend class HardwareBroker;
	friend class NativeInput;
	friend class NativeSpiBus;
	friend class NativeCoreProtocol;
	friend class NativeCoreProtocolTeardown;
	friend class NativeLifecycle;
	friend class NativeRecovery;
	friend class CoreProtocolAuthorityTestPeer;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeInputAdapter;
	friend class linux_native::NativeSchedulerAdapter;
	friend class linux_native::NativeOffloadAdapter;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeVideoAdapter;
	friend class linux_native::NativeSaveAdapter;
	explicit OperationLease(
		const std::shared_ptr<OperationRegistration> &registration);
	Result AcquireHardwareLeaseView(
		std::unique_ptr<HardwareLeaseView> *view) const;
	Result AcquireInputHardwareLeaseView(const NativeCoreProfile &profile,
		std::unique_ptr<HardwareLeaseView> *view) const;
	Result AcquireInputHardwareLeaseView(HardwareBroker &owner,
		const NativeCoreProfile &profile,
		std::unique_ptr<HardwareLeaseView> *view) const;
	Result AcquireActiveCoreProtocolSession(HardwareBroker &owner,
		const NativeCoreProfile &profile,
		std::unique_ptr<ActiveCoreProtocolSession> *session) const;
	Result AcquireCleanupCoreProtocolSession(HardwareBroker &owner,
		std::unique_ptr<CleanupCoreProtocolSession> *session) const;
	Result AcquireRecoveryCoreProtocolSession(HardwareBroker &owner,
		std::unique_ptr<RecoveryCoreProtocolSession> *session) const;
	Result CompleteFailedCoreProtocolSession(HardwareBroker &owner,
		std::unique_ptr<ActiveCoreProtocolSession> &&session,
		const ActiveProtocolFailureReceipt &receipt) const;
	Result CompleteSuccessfulCoreProtocolSession(HardwareBroker &owner,
		std::unique_ptr<ActiveCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt) const;
	Result CompleteCleanupCoreProtocolSession(HardwareBroker &owner,
		std::unique_ptr<CleanupCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt) const;
	Result CompleteRecoveryCoreProtocolSession(HardwareBroker &owner,
		std::unique_ptr<RecoveryCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt) const;
	Result CompleteInvalidCoreProtocolOutcome(HardwareBroker &owner,
		Result primary_result) const;
	Result GetCoreProtocolBrokerDisposition(HardwareBroker &owner,
		CoreProtocolBrokerDisposition *disposition) const;
	Result AcquireActiveAudioSession(HardwareBroker &owner,
		const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveAudioSessionBundle> *bundle) const;
	Result AcquireCleanupAudioSession(HardwareBroker &owner,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupAudioSessionBundle> *bundle) const;
	Result AcquireRecoveryAudioSession(HardwareBroker &owner,
		const SafeAudioRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryAudioSessionBundle> *bundle) const;
	Result GetAudioSessionDisposition(HardwareBroker &owner,
		PeripheralBrokerDisposition *disposition) const;
	Result AcquireActiveVideoSession(HardwareBroker &owner,
		const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveVideoSessionBundle> *bundle) const;
	Result AcquireCleanupVideoSession(HardwareBroker &owner,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupVideoSessionBundle> *bundle) const;
	Result AcquireRecoveryVideoSession(HardwareBroker &owner,
		const SafeVideoRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryVideoSessionBundle> *bundle) const;
	Result GetVideoSessionDisposition(HardwareBroker &owner,
		PeripheralBrokerDisposition *disposition) const;
	Result AcquireActiveAudioVideoSession(HardwareBroker &owner,
		const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveAudioVideoSessionBundle> *bundle) const;
	Result AcquireCleanupAudioVideoSession(HardwareBroker &owner,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupAudioVideoSessionBundle> *bundle) const;
	Result AcquireRecoveryAudioVideoSession(HardwareBroker &owner,
		const SafeAudioVideoRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryAudioVideoSessionBundle> *bundle) const;
	Result GetAudioVideoSessionDisposition(HardwareBroker &owner,
		PeripheralBrokerDisposition *disposition) const;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result BeginConcurrentActiveOperationForTest(HardwareBroker &owner,
		OperationKind operation_kind, uint64_t absolute_deadline_ms,
		std::unique_ptr<OperationLease> *lease) const;
	Result RecordActiveCoreProtocolMutationForTest(HardwareBroker &owner,
		ActiveCoreProtocolSession &session,
		uint64_t *mutation_sequence) const;
#endif
	Result AcquireProcessOperationGuard(HardwareBroker &owner,
		OperationKind required_operation_kind,
		const NativeCoreProfile *required_profile,
		std::unique_ptr<ProcessOperationGuard> *guard) const;

	std::shared_ptr<OperationRegistration> registration_;
};

class OperationInvocation final {
public:
	~OperationInvocation();
	OperationInvocation(const OperationInvocation &) = delete;
	OperationInvocation &operator=(const OperationInvocation &) = delete;
	OperationInvocation(OperationInvocation &&) = delete;
	OperationInvocation &operator=(OperationInvocation &&) = delete;

private:
	friend class HardwareBroker;
	OperationInvocation(HardwareBroker &broker, LeaseAuthority authority,
		uint64_t authority_identity, uint64_t callback_deadline_ms,
		uint64_t invocation_identity);

	HardwareBroker *broker_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	LeaseAuthority authority_;
	uint64_t authority_identity_;
	uint64_t callback_deadline_ms_;
	uint64_t identity_;
	bool registered_;
};

class CleanupEpoch final {
public:
	~CleanupEpoch();
	CleanupEpoch(const CleanupEpoch &) = delete;
	CleanupEpoch &operator=(const CleanupEpoch &) = delete;
	CleanupEpoch(CleanupEpoch &&) = delete;
	CleanupEpoch &operator=(CleanupEpoch &&) = delete;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	uint64_t identity_for_test() const;
#endif

private:
	friend class HardwareBroker;
	CleanupEpoch(HardwareBroker &broker, PlatformGenerationId generation,
		uint64_t identity, uint64_t non_fpga_deadline_ms,
		uint64_t fpga_deadline_ms,
		const std::shared_ptr<BrokerLifetime> &lifetime);

	HardwareBroker *broker_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	PlatformGenerationId generation_;
	uint64_t identity_;
	uint64_t non_fpga_deadline_ms_;
	uint64_t fpga_deadline_ms_;
	bool registered_;
};

class RecoveryEpoch final {
public:
	~RecoveryEpoch();
	RecoveryEpoch(const RecoveryEpoch &) = delete;
	RecoveryEpoch &operator=(const RecoveryEpoch &) = delete;
	RecoveryEpoch(RecoveryEpoch &&) = delete;
	RecoveryEpoch &operator=(RecoveryEpoch &&) = delete;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	uint64_t identity_for_test() const;
#endif

private:
	friend class HardwareBroker;
	RecoveryEpoch(HardwareBroker &broker, uint64_t identity,
		uint32_t requested_resource_flags, uint64_t non_fpga_deadline_ms,
		uint64_t fpga_deadline_ms,
		const std::shared_ptr<BrokerLifetime> &lifetime);

	HardwareBroker *broker_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	uint64_t identity_;
	uint32_t requested_resource_flags_;
	uint64_t non_fpga_deadline_ms_;
	uint64_t fpga_deadline_ms_;
	bool registered_;
};

class ContainmentResumeKey final {
public:
	~ContainmentResumeKey() {}
	ContainmentResumeKey(const ContainmentResumeKey &) = delete;
	ContainmentResumeKey &operator=(const ContainmentResumeKey &) = delete;
	ContainmentResumeKey(ContainmentResumeKey &&) = delete;
	ContainmentResumeKey &operator=(ContainmentResumeKey &&) = delete;

private:
	friend class HardwareBroker;
	friend class NativeContainment;
	ContainmentResumeKey(HardwareBroker &broker, LeaseAuthority authority,
		uint64_t authority_identity, PlatformGenerationId generation,
		uint64_t mutation_sequence, uint32_t core_gpo,
		uint32_t interface_module, uint32_t sdr_port_control,
		uint32_t bridge_reset, uint32_t remap, uint32_t manager_control,
		uint32_t manager_mode, uint64_t manager_mutation_sequence,
		const std::shared_ptr<BrokerLifetime> &lifetime)
		: broker_(&broker), lifetime_(lifetime), authority_(authority),
		  authority_identity_(authority_identity), generation_(generation),
		  mutation_sequence_(mutation_sequence), core_gpo_(core_gpo),
		  interface_module_(interface_module), sdr_port_control_(sdr_port_control),
		  bridge_reset_(bridge_reset), remap_(remap), manager_control_(manager_control),
		  manager_mode_(manager_mode), manager_mutation_sequence_(manager_mutation_sequence) {}

	HardwareBroker *broker_;
	std::weak_ptr<BrokerLifetime> lifetime_;
	LeaseAuthority authority_;
	uint64_t authority_identity_;
	PlatformGenerationId generation_;
	uint64_t mutation_sequence_;
	uint32_t core_gpo_;
	uint32_t interface_module_;
	uint32_t sdr_port_control_;
	uint32_t bridge_reset_;
	uint32_t remap_;
	uint32_t manager_control_;
	uint32_t manager_mode_;
	uint64_t manager_mutation_sequence_;
};

class HardwareBroker final {
public:
	explicit HardwareBroker(NativeClock &clock);
	~HardwareBroker();
	HardwareBroker(const HardwareBroker &) = delete;
	HardwareBroker &operator=(const HardwareBroker &) = delete;

	Result Enter(const NativeCoreProfile &profile,
		PlatformGenerationId *generation);
	Result Enter(const NativeCoreProfile *profile,
		PlatformGenerationId *generation);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result EnterFixtureForTest(const NativeCoreProfile &profile,
		PlatformGenerationId *generation);
	bool has_live_generation_for_test();
	uint64_t mutation_sequence_for_test();
	uint64_t containment_receipt_sequence_for_test();
	bool containment_manager_receipt_for_test(uint32_t *control,
		uint32_t *mode, uint64_t *mutation_sequence);
	bool core_protocol_failure_receipt_for_test(
		ActiveProtocolFailureReceipt *receipt);
	bool core_protocol_session_current_for_test();
	bool core_protocol_session_abandoned_for_test(
		const OperationLease &lease);
#endif
	Result Begin(PlatformGenerationId generation, OperationKind operation_kind,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<OperationLease> *lease);
	Result Quiesce(PlatformGenerationId generation,
		uint64_t absolute_deadline_ms);
	Result LatchFailure(PlatformGenerationId generation);
	Result BeginCleanup(PlatformGenerationId generation,
		uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
		std::unique_ptr<CleanupEpoch> *epoch);
	Result BeginCleanupInvocation(const CleanupEpoch &epoch,
		uint64_t callback_deadline_ms,
		std::unique_ptr<OperationInvocation> *invocation);
	Result BeginRecoveryInvocation(const RecoveryEpoch &epoch,
		uint64_t callback_deadline_ms,
		std::unique_ptr<OperationInvocation> *invocation);
	Result BeginCleanupOperation(const CleanupEpoch &epoch,
		const OperationInvocation &invocation, OperationKind operation_kind,
		std::unique_ptr<OperationLease> *lease);
	Result ContinueCleanupOperation(const CleanupEpoch &epoch,
		const OperationInvocation &invocation, const OperationLease &lease);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result BeginCleanupOperation(const CleanupEpoch &epoch,
		OperationKind operation_kind,
		std::unique_ptr<OperationLease> *lease);
#endif
	Result AcquireHardwareLeaseView(const OperationLease &lease,
		std::unique_ptr<HardwareLeaseView> *view);
	Result ObserveContainment(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result Leave(PlatformGenerationId generation,
		std::unique_ptr<CleanupEpoch> &&epoch);
	Result BeginRecovery(uint32_t requested_resource_flags,
		uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
		std::unique_ptr<RecoveryEpoch> *epoch);
	Result BeginRecoveryOperation(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation, OperationKind operation_kind,
		std::unique_ptr<OperationLease> *lease);
	Result ContinueRecoveryOperation(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation, const OperationLease &lease);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result BeginRecoveryOperation(const RecoveryEpoch &epoch,
		OperationKind operation_kind,
		std::unique_ptr<OperationLease> *lease);
#endif
	Result FinishInvocation(std::unique_ptr<OperationInvocation> &&invocation);
	Result RecordOperationOutcome(const OperationInvocation &invocation,
		const OperationLease &lease, Result result);
	Result FinishRecovery(std::unique_ptr<RecoveryEpoch> &&epoch,
		MisterRecoveryObservationV2 *observation);

private:
	friend class OperationLease;
	friend class OperationInvocation;
	friend class CleanupEpoch;
	friend class RecoveryEpoch;
	friend class HardwareLeaseView;
	friend class ProcessOperationGuard;
	friend class NativeSpiBus;
	friend class NativeContainment;
	friend class NativeRecovery;
	friend class NativeLifecycle;
	friend class PeripheralAuthorityTestPeer;
	friend class linux_native::NativeAvIoAdapter;
	friend class linux_native::NativeAudioAdapter;
	friend class linux_native::NativeVideoAdapter;
	friend struct OperationRegistration;

	enum class State : uint8_t {
		idle,
		active,
		quiescing,
		cleanup,
		terminal_neutral,
		recovery
	};

	void ReleaseOperation(OperationRegistration &registration);
	void UnregisterInvocation(OperationInvocation &invocation);
	void ReleaseHardwareLeaseView(HardwareLeaseView &view);
	void ReleaseProcessOperationGuard(ProcessOperationGuard &guard);
	void UnregisterCleanup(CleanupEpoch &epoch);
	void UnregisterRecovery(RecoveryEpoch &epoch);
	bool IsCurrentCleanup(const CleanupEpoch &epoch) const;
	bool IsCurrentRecovery(const RecoveryEpoch &epoch) const;
	bool IsCurrentInvocation(const OperationInvocation &invocation,
		LeaseAuthority authority, uint64_t authority_identity) const;
	Result BeginInvocation(LeaseAuthority authority, uint64_t authority_identity,
		uint64_t callback_deadline_ms,
		std::unique_ptr<OperationInvocation> *invocation);
	Result BeginInvokedOperation(LeaseAuthority authority,
		uint64_t authority_identity, const OperationInvocation &invocation,
		OperationKind operation_kind, uint64_t authority_deadline_ms,
		const NativeCoreProfile *profile,
		std::unique_ptr<OperationLease> *lease);
	Result ContinueInvokedOperation(LeaseAuthority authority,
		uint64_t authority_identity, const OperationInvocation &invocation,
		const OperationLease &lease, uint64_t authority_deadline_ms);
	static bool IsHardwareOperation(OperationKind operation_kind);
	static bool IsRecoveryOperation(OperationKind operation_kind,
		uint32_t requested_resource_flags);
	static bool CleanupDeadline(OperationKind operation_kind,
		const CleanupEpoch &epoch, uint64_t *absolute_deadline_ms);
	static bool RecoveryDeadline(OperationKind operation_kind,
		const RecoveryEpoch &epoch, uint64_t *absolute_deadline_ms);
	Result AcquireHardwareLeaseViewFor(const OperationLease &lease,
		OperationKind required_operation_kind,
		const NativeCoreProfile *required_profile,
		std::unique_ptr<HardwareLeaseView> *view);
	Result AcquireActiveCoreProtocolSessionFor(const OperationLease &lease,
		const NativeCoreProfile &profile,
		std::unique_ptr<ActiveCoreProtocolSession> *session);
	Result AcquireCleanupCoreProtocolSessionFor(const OperationLease &lease,
		std::unique_ptr<CleanupCoreProtocolSession> *session);
	Result AcquireRecoveryCoreProtocolSessionFor(const OperationLease &lease,
		std::unique_ptr<RecoveryCoreProtocolSession> *session);
	Result CompleteFailedCoreProtocolSessionFor(const OperationLease &lease,
		std::unique_ptr<ActiveCoreProtocolSession> &&session,
		const ActiveProtocolFailureReceipt &receipt);
	Result CompleteSuccessfulCoreProtocolSessionFor(const OperationLease &lease,
		std::unique_ptr<ActiveCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt);
	Result CompleteCleanupCoreProtocolSessionFor(const OperationLease &lease,
		std::unique_ptr<CleanupCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt);
	Result CompleteRecoveryCoreProtocolSessionFor(const OperationLease &lease,
		std::unique_ptr<RecoveryCoreProtocolSession> &&session,
		const ProtocolMappingReleaseReceipt &receipt);
	Result CompleteInvalidCoreProtocolOutcomeFor(const OperationLease &lease,
		Result primary_result);
	Result GetCoreProtocolBrokerDispositionFor(const OperationLease &lease,
		CoreProtocolBrokerDisposition *disposition);
	PeripheralBackendIdentity CreatePeripheralBackendIdentity(
		const void *adapter_instance);
	Result AcquireActiveAudioSessionFor(const OperationLease &lease,
		const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveAudioSessionBundle> *bundle);
	Result AcquireCleanupAudioSessionFor(const OperationLease &lease,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupAudioSessionBundle> *bundle);
	Result AcquireRecoveryAudioSessionFor(const OperationLease &lease,
		const SafeAudioRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryAudioSessionBundle> *bundle);
	Result GetAudioSessionDispositionFor(const OperationLease &lease,
		PeripheralBrokerDisposition *disposition);
	Result AcquireActiveVideoSessionFor(const OperationLease &lease,
		const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveVideoSessionBundle> *bundle);
	Result AcquireCleanupVideoSessionFor(const OperationLease &lease,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupVideoSessionBundle> *bundle);
	Result AcquireRecoveryVideoSessionFor(const OperationLease &lease,
		const SafeVideoRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryVideoSessionBundle> *bundle);
	Result GetVideoSessionDispositionFor(const OperationLease &lease,
		PeripheralBrokerDisposition *disposition);
	Result AcquireActiveAudioVideoSessionFor(const OperationLease &lease,
		const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
		std::unique_ptr<ActiveAudioVideoSessionBundle> *bundle);
	Result AcquireCleanupAudioVideoSessionFor(const OperationLease &lease,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<CleanupAudioVideoSessionBundle> *bundle);
	Result AcquireRecoveryAudioVideoSessionFor(const OperationLease &lease,
		const SafeAudioVideoRecoveryRecord &record,
		const PeripheralBackendIdentity &backend,
		std::unique_ptr<RecoveryAudioVideoSessionBundle> *bundle);
	Result GetAudioVideoSessionDispositionFor(const OperationLease &lease,
		PeripheralBrokerDisposition *disposition);
	Result CompleteActiveAudioSuccess(std::unique_ptr<ActiveAudioSession> &&session,
		const PeripheralCompletionReceipt &receipt);
	Result CompleteActiveAudioFailure(std::unique_ptr<ActiveAudioSession> &&session,
		const PeripheralFailureReceipt &receipt);
	Result CompleteCleanupAudio(std::unique_ptr<CleanupAudioSession> &&session,
		const PeripheralCompletionReceipt &receipt);
	Result AbandonCleanupAudio(std::unique_ptr<CleanupAudioSession> &&session,
		const PeripheralFailureReceipt &receipt);
	Result CompleteRecoveryAudio(std::unique_ptr<RecoveryAudioSession> &&session,
		const PeripheralCompletionReceipt &receipt);
	Result AbandonRecoveryAudio(std::unique_ptr<RecoveryAudioSession> &&session,
		const PeripheralFailureReceipt &receipt);
	Result CompleteActiveVideoSuccess(std::unique_ptr<ActiveVideoSession> &&session,
		const PeripheralCompletionReceipt &receipt);
	Result CompleteActiveVideoFailure(std::unique_ptr<ActiveVideoSession> &&session,
		const PeripheralFailureReceipt &receipt);
	Result CompleteCleanupVideo(std::unique_ptr<CleanupVideoSession> &&session,
		const PeripheralCompletionReceipt &receipt);
	Result AbandonCleanupVideo(std::unique_ptr<CleanupVideoSession> &&session,
		const PeripheralFailureReceipt &receipt);
	Result CompleteRecoveryVideo(std::unique_ptr<RecoveryVideoSession> &&session,
		const PeripheralCompletionReceipt &receipt);
	Result AbandonRecoveryVideo(std::unique_ptr<RecoveryVideoSession> &&session,
		const PeripheralFailureReceipt &receipt);
	Result CompleteActiveAudioVideoSuccess(
		std::unique_ptr<ActiveAudioVideoSession> &&session,
		const CoupledAcquisitionReceipt &receipt);
	Result CompleteActiveAudioVideoFailure(
		std::unique_ptr<ActiveAudioVideoSession> &&session,
		const CoupledFailureReceipt &receipt);
	Result CompleteCleanupAudioVideo(
		std::unique_ptr<CleanupAudioVideoSession> &&session,
		const CoupledCompletionReceipt &receipt);
	Result AbandonCleanupAudioVideo(
		std::unique_ptr<CleanupAudioVideoSession> &&session,
		const CoupledFailureReceipt &receipt);
	Result CompleteRecoveryAudioVideo(
		std::unique_ptr<RecoveryAudioVideoSession> &&session,
		const CoupledRecoveryReceipt &receipt);
	Result AbandonRecoveryAudioVideo(
		std::unique_ptr<RecoveryAudioVideoSession> &&session,
		const CoupledFailureReceipt &receipt);
	Result RecordPeripheralMutation(
		const std::shared_ptr<PeripheralSessionState> &state,
		uint64_t *mutation_sequence);
	Result PreparePeripheralAction(
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionAction action, const void *profile_identity,
		uint8_t word_count, uint8_t *next_word_index);
	Result AdmitPeripheralAction(
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionAction action);
	Result RecordPeripheralActionWord(
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionAction action, uint8_t word_index);
	Result ClosePeripheralAction(
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionAction action);
	Result RecordCoupledRecoveryOperation(const RecoveryEpoch &epoch,
		const OperationLease &lease, const CoupledRecoveryReceipt &receipt,
		Result operation_result);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	Result RecordActiveCoreProtocolMutationForTest(const OperationLease &lease,
		ActiveCoreProtocolSession &session,
		uint64_t *mutation_sequence);
#endif
	Result AcquireProcessOperationGuardFor(const OperationLease &lease,
		HardwareBroker &owner, OperationKind required_operation_kind,
		const NativeCoreProfile *required_profile,
		std::unique_ptr<ProcessOperationGuard> *guard);
	uint64_t RecordMutation(const HardwareLeaseView &view);
	uint64_t CurrentMutationSequence(const HardwareLeaseView &view);
	Result RecordFpgaProgrammingMutation(const HardwareLeaseView &view,
		size_t accepted_bytes, uint64_t *mutation_sequence);
	Result AuthorizeFpgaProgrammingProfile(const HardwareLeaseView &view,
		const NativeCoreProfile &profile);
	Result ValidateCleanupContainmentAuthority(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result ValidateRecoveryContainmentAuthority(const RecoveryEpoch &epoch,
		const OperationLease &terminal_lease);
	Result ValidateContainmentBoundary(const HardwareLeaseView &view);
	Result MintContainmentResumeKey(const HardwareLeaseView &view,
		uint32_t core_gpo, uint32_t interface_module,
		uint32_t sdr_port_control, uint32_t bridge_reset, uint32_t remap,
		uint32_t manager_control, uint32_t manager_mode,
		uint64_t manager_mutation_sequence,
		std::unique_ptr<ContainmentResumeKey> *key);
	Result ValidateContainmentResumeKey(const ContainmentResumeKey &key,
		const OperationLease &terminal_lease, uint32_t core_gpo,
		uint32_t interface_module, uint32_t sdr_port_control,
		uint32_t bridge_reset, uint32_t remap, uint32_t manager_control,
		uint32_t manager_mode, uint64_t manager_mutation_sequence);
	Result StageContainmentEvidence(const OperationLease &terminal_lease,
		uint32_t core_gpo, uint32_t interface_module,
		uint32_t sdr_port_control, uint32_t bridge_reset, uint32_t remap,
		uint32_t manager_control, uint32_t manager_mode,
		bool manager_neutral_observed,
		uint64_t manager_neutral_mutation_sequence,
		bool mappings_released);
	Result CommitCleanupContainment(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result PromoteCleanupAudioWithContainment(const CleanupEpoch &epoch,
		const OperationLease &terminal_lease);
	Result CommitRecoveryContainment(const RecoveryEpoch &epoch,
		const OperationLease &terminal_lease);
	Result BeginRecoveryObservation(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation);
	Result CheckRecoveryObservationDeadline(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation);
	Result EndRecoveryObservation(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation,
		uint32_t observed_resource_flags, uint32_t neutral_resource_flags,
		Result result);
	Result RecoveryObservationDeadline(const RecoveryEpoch &epoch,
		const OperationInvocation &invocation, uint64_t *deadline_ms) const;
	Result RecordRecoveryContainmentObservation(const RecoveryEpoch &epoch,
		uint32_t observed_resource_flags, uint32_t neutral_resource_flags,
		Result result);
	Result RecordRecoveryOperation(const RecoveryEpoch &epoch,
		const OperationLease &lease, RecoveryResourceState resource_state,
		Result result);
	Result RecordRecoveryFailure(const RecoveryEpoch &epoch,
		const OperationLease &lease, Result result,
		RecoveryFailurePersistence persistence);
	Result SnapshotRecovery(const RecoveryEpoch &epoch,
		MisterRecoveryObservationV2 *observation);
	Result ValidateRecoveryRequestedFlags(const RecoveryEpoch &epoch,
		uint32_t callback_requested_flags) const;
	bool CanFinishRecovery(const RecoveryEpoch &epoch) const;
	static uint32_t RecoveryResourceForOperation(OperationKind operation_kind);
	void RecordRecoveryPartition(uint32_t observed_resource_flags,
		uint32_t neutral_resource_flags);
	void LatchRecoveryResult(Result result,
		RecoveryFailurePersistence persistence);
	void ClearContainmentReceipt();
	void ClearCoreProtocolFailureReceipt();
	void ConsumeCoreProtocolSessionState(OperationRegistration &registration,
		const std::shared_ptr<ProtocolSessionState> &state);
	Result AcquirePeripheralState(const OperationLease &lease,
		PeripheralSessionKind kind, const NativeCoreProfile *profile,
		const SafePeripheralRecoveryRecord *recovery_record,
		const PeripheralBackendIdentity &backend,
		std::shared_ptr<PeripheralSessionState> *state);
	Result GetPeripheralDisposition(const OperationLease &lease,
		PeripheralSessionKind kind, PeripheralBrokerDisposition *disposition);
	Result CompletePeripheralSession(
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralSessionKind kind, LeaseAuthority authority,
		bool active_failure, bool abandon,
		const PeripheralCompletionReceipt &receipt);
	Result CompleteCoupledSession(
		const std::shared_ptr<PeripheralSessionState> &state,
		LeaseAuthority authority, bool active_failure, bool abandon,
		const CoupledCompletionReceipt &receipt, Result primary_result);
	Result ResolvePeripheralWrapperAllocationFailure(
		const std::shared_ptr<PeripheralSessionState> &state);
	void ConsumePeripheralSessionState(OperationRegistration &registration,
		const std::shared_ptr<PeripheralSessionState> &state,
		PeripheralBrokerDisposition disposition);
	bool ReceiptMatchesCleanup(const CleanupEpoch &epoch) const;
	bool ReceiptMatchesRecovery(const RecoveryEpoch &epoch) const;

	NativeClock &clock_;
	std::shared_ptr<BrokerLifetime> lifetime_;
	mutable std::mutex mutex_;
	std::condition_variable lease_released_;
	State state_;
	PlatformGenerationId generation_;
	uint64_t cleanup_identity_;
	uint64_t cleanup_non_fpga_deadline_ms_;
	uint64_t cleanup_fpga_deadline_ms_;
	uint64_t terminal_lease_deadline_ms_;
	uint64_t recovery_identity_;
	uint32_t recovery_requested_resource_flags_;
	uint64_t recovery_non_fpga_deadline_ms_;
	uint64_t recovery_fpga_deadline_ms_;
	LeaseAuthority invocation_authority_;
	uint64_t invocation_authority_identity_;
	uint64_t invocation_identity_;
	uint64_t invocation_callback_deadline_ms_;
	bool invocation_registered_;
	bool invocation_outcome_missing_;
	std::weak_ptr<OperationRegistration> invocation_registration_;
	std::weak_ptr<OperationRegistration> suspended_registration_;
	uint64_t mutation_sequence_;
	size_t active_lease_count_;
	size_t terminal_lease_count_;
	bool cleanup_registered_;
	bool cleanup_ever_started_;
	bool quiesce_complete_;
	bool quiesce_call_active_;
	bool containment_receipt_current_;
	bool containment_evidence_pending_;
	bool cleanup_audio_shutdown_complete_;
	bool recovery_audio_shutdown_complete_;
	bool recovery_registered_;
	bool recovery_observation_active_;
	bool recovery_terminal_neutral_;
	bool hardware_transaction_active_;
	bool failure_latched_;
	Result recovery_result_;
	LeaseAuthority receipt_authority_;
	uint64_t receipt_authority_identity_;
	PlatformGenerationId receipt_generation_;
	uint32_t receipt_requested_resource_flags_;
	uint32_t receipt_core_gpo_;
	uint32_t receipt_interface_module_;
	uint32_t receipt_sdr_port_control_;
	uint32_t receipt_bridge_reset_;
	uint32_t receipt_remap_;
	uint32_t receipt_manager_control_;
	uint32_t receipt_manager_mode_;
	bool receipt_manager_neutral_observed_;
	uint64_t receipt_manager_neutral_mutation_sequence_;
	uint64_t receipt_mutation_sequence_;
	uint32_t recovery_observed_resource_flags_;
	uint32_t recovery_neutral_resource_flags_;
	bool receipt_mappings_released_;
	OperationRegistration *receipt_registration_;
	bool core_protocol_failure_receipt_current_;
	ActiveProtocolFailureReceipt core_protocol_failure_receipt_;
	std::weak_ptr<ProtocolSessionState> core_protocol_session_state_;
	std::weak_ptr<PeripheralSessionState> peripheral_session_state_;
	const NativeCoreProfile *profile_;
};

} // namespace native
} // namespace mister

#endif
