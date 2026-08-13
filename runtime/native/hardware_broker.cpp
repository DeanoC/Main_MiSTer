// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_core_protocol.hpp"
#include "runtime/native/native_core_protocol_session_state.hpp"
#include "runtime/native/native_recovery.hpp"

#include <atomic>
#include <new>

namespace mister {
namespace native {

ProtocolSessionState::ProtocolSessionState()
	: view(), owner_registration(), profile(nullptr),
	  initial_mutation_sequence(0),
	  handle_state(ProtocolSessionHandleState::live), io_state()
{
}

ProtocolSessionState::~ProtocolSessionState() = default;

struct BrokerLifetime {
	explicit BrokerLifetime(HardwareBroker *owner)
		: broker(owner), process_guard_count(0), destroying(false) {}

	// Every call from an outliving token takes this mutex before the broker
	// mutex. Broker code must never acquire this mutex while holding its own.
	// Adapters acquire a process guard before their private state mutex and
	// destroy that mutex guard before releasing the broker guard.
	std::mutex mutex;
	std::condition_variable process_guards_released;
	HardwareBroker *broker;
	size_t process_guard_count;
	bool destroying;
};

struct OperationRegistration {
	OperationRegistration(HardwareBroker &owner, LeaseAuthority lease_authority,
		uint64_t identity, OperationKind kind, uint64_t deadline_ms,
		const std::shared_ptr<BrokerLifetime> &broker_lifetime,
		const NativeCoreProfile *bound_profile)
		: broker(&owner), lifetime(broker_lifetime), authority(lease_authority),
		  authority_identity(identity), operation_kind(kind),
		  authority_deadline_ms(deadline_ms), effective_deadline_ms(deadline_ms),
		  invocation_identity(0), profile(bound_profile),
		  registered(false), process_guard_active(false), outcome_recorded(false),
		  core_protocol_completion(CoreProtocolBrokerDisposition::no_session),
		  core_protocol_session_state(),
		  peripheral_completion(PeripheralBrokerDisposition::no_session),
		  peripheral_session_state()
	{
	}

	~OperationRegistration()
	{
		if (!registered) return;
		std::lock_guard<std::mutex> lifetime_lock(lifetime->mutex);
		if (registered && lifetime->broker == broker)
			broker->ReleaseOperation(*this);
	}

	HardwareBroker *broker;
	std::shared_ptr<BrokerLifetime> lifetime;
	LeaseAuthority authority;
	uint64_t authority_identity;
	OperationKind operation_kind;
	uint64_t authority_deadline_ms;
	uint64_t effective_deadline_ms;
	uint64_t invocation_identity;
	const NativeCoreProfile *profile;
	bool registered;
	bool process_guard_active;
	bool outcome_recorded;
	CoreProtocolBrokerDisposition core_protocol_completion;
	std::shared_ptr<ProtocolSessionState> core_protocol_session_state;
	PeripheralBrokerDisposition peripheral_completion;
	std::shared_ptr<PeripheralSessionState> peripheral_session_state;
};

namespace {

std::atomic<uint64_t> generation_nonce_source(0);
std::atomic<uint64_t> cleanup_nonce_source(0);
std::atomic<uint64_t> peripheral_backend_nonce_source(0);
std::atomic<uint64_t> invocation_nonce_source(0);

uint64_t FreshNonzeroNonce(std::atomic<uint64_t> &source)
{
	for (;;) {
		const uint64_t nonce = source.fetch_add(1) + 1;
		if (nonce != 0) return nonce;
	}
}

bool OutputAvailable(const std::unique_ptr<OperationLease> *output)
{
	return output != nullptr && output->get() == nullptr;
}

bool OutputAvailable(const std::unique_ptr<CleanupEpoch> *output)
{
	return output != nullptr && output->get() == nullptr;
}

bool OutputAvailable(const std::unique_ptr<RecoveryEpoch> *output)
{
	return output != nullptr && output->get() == nullptr;
}

bool OutputAvailable(const std::unique_ptr<OperationInvocation> *output)
{
	return output != nullptr && output->get() == nullptr;
}

} // namespace

HardwareLeaseView::HardwareLeaseView(
	const std::shared_ptr<OperationRegistration> &registration)
	: registration_(registration)
{
}

ProcessOperationGuard::ProcessOperationGuard(
	const std::shared_ptr<OperationRegistration> &registration)
	: registration_(registration), lifetime_registered_(false)
{
}

ProcessOperationGuard::~ProcessOperationGuard()
{
	if (!registration_ || !registration_->lifetime || !lifetime_registered_)
		return;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker == registration_->broker) {
		registration_->broker->ReleaseProcessOperationGuard(*this);
	}
	lifetime_registered_ = false;
	if (registration_->lifetime->process_guard_count != 0)
		--registration_->lifetime->process_guard_count;
	registration_->lifetime->process_guards_released.notify_all();
}

uint64_t ProcessOperationGuard::absolute_deadline_ms() const
{
	return registration_ ? registration_->effective_deadline_ms : 0;
}

LeaseAuthority ProcessOperationGuard::authority() const
{
	return registration_ ? registration_->authority :
		LeaseAuthority::recovery_epoch;
}

HardwareLeaseView::~HardwareLeaseView()
{
	if (!registration_ || !registration_->lifetime) return;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker == registration_->broker)
		registration_->broker->ReleaseHardwareLeaseView(*this);
}

uint64_t HardwareLeaseView::RecordMutation()
{
	if (!registration_ || !registration_->lifetime) return 0;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker) return 0;
	return registration_->broker->RecordMutation(*this);
}

uint64_t HardwareLeaseView::CurrentMutationSequence() const
{
	if (!registration_ || !registration_->lifetime) return 0;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker) return 0;
	return registration_->broker->CurrentMutationSequence(*this);
}

Result HardwareLeaseView::RecordFpgaProgrammingMutation(
	size_t accepted_bytes, uint64_t *mutation_sequence)
{
	if (mutation_sequence == nullptr || accepted_bytes == 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	*mutation_sequence = 0;
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->RecordFpgaProgrammingMutation(*this,
		accepted_bytes, mutation_sequence);
}

Result HardwareLeaseView::AuthorizeFpgaProgrammingProfile(
	const NativeCoreProfile &profile) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->AuthorizeFpgaProgrammingProfile(*this,
		profile);
}

Result HardwareLeaseView::MintBridgeActivationAuthority(
	uint64_t bridge_mutation_sequence,
	std::unique_ptr<NativeBridgeActivationAuthority> *authority) const
{
	if (authority == nullptr || authority->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->MintBridgeActivationAuthority(*this,
		bridge_mutation_sequence, authority);
}

Result HardwareLeaseView::ValidateBridgeActivationAuthority(
	const NativeBridgeActivationAuthority &authority) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->ValidateBridgeActivationAuthority(*this,
		authority);
}

uint64_t HardwareLeaseView::absolute_deadline_ms() const
{
	return registration_->effective_deadline_ms;
}

OperationLease::OperationLease(
	const std::shared_ptr<OperationRegistration> &registration)
	: registration_(registration)
{
}

OperationLease::~OperationLease()
{
}

Result OperationLease::AcquireHardwareLeaseView(
	std::unique_ptr<HardwareLeaseView> *view) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->AcquireHardwareLeaseView(*this, view);
}

Result OperationLease::AcquireInputHardwareLeaseView(
	const NativeCoreProfile &profile,
	std::unique_ptr<HardwareLeaseView> *view) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->AcquireHardwareLeaseViewFor(*this,
		OperationKind::input, &profile, view);
}

Result OperationLease::AcquireInputHardwareLeaseView(HardwareBroker &owner,
	const NativeCoreProfile &profile,
	std::unique_ptr<HardwareLeaseView> *view) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return registration_->broker->AcquireHardwareLeaseViewFor(*this,
		OperationKind::input, &profile, view);
}

Result OperationLease::AcquireActiveCoreProtocolSession(HardwareBroker &owner,
	const NativeCoreProfile &profile,
	std::unique_ptr<ActiveCoreProtocolSession> *session) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.AcquireActiveCoreProtocolSessionFor(*this, profile, session);
}

Result OperationLease::AcquireCleanupCoreProtocolSession(HardwareBroker &owner,
	std::unique_ptr<CleanupCoreProtocolSession> *session) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.AcquireCleanupCoreProtocolSessionFor(*this, session);
}

Result OperationLease::AcquireRecoveryCoreProtocolSession(HardwareBroker &owner,
	std::unique_ptr<RecoveryCoreProtocolSession> *session) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.AcquireRecoveryCoreProtocolSessionFor(*this, session);
}

Result OperationLease::CompleteFailedCoreProtocolSession(HardwareBroker &owner,
	std::unique_ptr<ActiveCoreProtocolSession> &&session,
	const ActiveProtocolFailureReceipt &receipt) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.CompleteFailedCoreProtocolSessionFor(*this,
		std::move(session), receipt);
}

Result OperationLease::CompleteSuccessfulCoreProtocolSession(
	HardwareBroker &owner,
	std::unique_ptr<ActiveCoreProtocolSession> &&session,
	const ProtocolMappingReleaseReceipt &receipt) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.CompleteSuccessfulCoreProtocolSessionFor(*this,
		std::move(session), receipt);
}

Result OperationLease::CompleteCleanupCoreProtocolSession(
	HardwareBroker &owner,
	std::unique_ptr<CleanupCoreProtocolSession> &&session,
	const ProtocolMappingReleaseReceipt &receipt) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.CompleteCleanupCoreProtocolSessionFor(*this,
		std::move(session), receipt);
}

Result OperationLease::CompleteRecoveryCoreProtocolSession(
	HardwareBroker &owner,
	std::unique_ptr<RecoveryCoreProtocolSession> &&session,
	const ProtocolMappingReleaseReceipt &receipt) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.CompleteRecoveryCoreProtocolSessionFor(*this,
		std::move(session), receipt);
}

Result OperationLease::CompleteInvalidCoreProtocolOutcome(
	HardwareBroker &owner, Result primary_result) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.CompleteInvalidCoreProtocolOutcomeFor(*this, primary_result);
}

Result OperationLease::GetCoreProtocolBrokerDisposition(HardwareBroker &owner,
	CoreProtocolBrokerDisposition *disposition) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.GetCoreProtocolBrokerDispositionFor(*this, disposition);
}

#define MISTER_PERIPHERAL_LEASE_CALL(call) \
	if (!registration_ || !registration_->lifetime) \
		return MISTER_RESULT_INVALID_STATE; \
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex); \
	if (registration_->lifetime->broker != registration_->broker || \
		registration_->broker != &owner) \
		return MISTER_RESULT_INVALID_STATE; \
	return owner.call

Result OperationLease::AcquireActiveAudioSession(HardwareBroker &owner,
	const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
	std::unique_ptr<ActiveAudioSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireActiveAudioSessionFor(*this, profile,
		backend, bundle));
}

Result OperationLease::AcquireCleanupAudioSession(HardwareBroker &owner,
	const PeripheralBackendIdentity &backend,
	std::unique_ptr<CleanupAudioSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireCleanupAudioSessionFor(*this, backend,
		bundle));
}

Result OperationLease::AcquireRecoveryAudioSession(HardwareBroker &owner,
	const SafeAudioRecoveryRecord &record, const PeripheralBackendIdentity &backend,
	std::unique_ptr<RecoveryAudioSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireRecoveryAudioSessionFor(*this, record,
		backend, bundle));
}

Result OperationLease::GetAudioSessionDisposition(HardwareBroker &owner,
	PeripheralBrokerDisposition *disposition) const
{
	MISTER_PERIPHERAL_LEASE_CALL(GetAudioSessionDispositionFor(*this,
		disposition));
}

Result OperationLease::AcquireActiveVideoSession(HardwareBroker &owner,
	const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
	std::unique_ptr<ActiveVideoSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireActiveVideoSessionFor(*this, profile,
		backend, bundle));
}

Result OperationLease::AcquireCleanupVideoSession(HardwareBroker &owner,
	const PeripheralBackendIdentity &backend,
	std::unique_ptr<CleanupVideoSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireCleanupVideoSessionFor(*this, backend,
		bundle));
}

Result OperationLease::AcquireRecoveryVideoSession(HardwareBroker &owner,
	const SafeVideoRecoveryRecord &record, const PeripheralBackendIdentity &backend,
	std::unique_ptr<RecoveryVideoSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireRecoveryVideoSessionFor(*this, record,
		backend, bundle));
}

Result OperationLease::GetVideoSessionDisposition(HardwareBroker &owner,
	PeripheralBrokerDisposition *disposition) const
{
	MISTER_PERIPHERAL_LEASE_CALL(GetVideoSessionDispositionFor(*this,
		disposition));
}

Result OperationLease::AcquireActiveAudioVideoSession(HardwareBroker &owner,
	const NativeCoreProfile &profile, const PeripheralBackendIdentity &backend,
	std::unique_ptr<ActiveAudioVideoSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireActiveAudioVideoSessionFor(*this,
		profile, backend, bundle));
}

Result OperationLease::AcquireCleanupAudioVideoSession(HardwareBroker &owner,
	const PeripheralBackendIdentity &backend,
	std::unique_ptr<CleanupAudioVideoSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireCleanupAudioVideoSessionFor(*this,
		backend, bundle));
}

Result OperationLease::AcquireRecoveryAudioVideoSession(HardwareBroker &owner,
	const SafeAudioVideoRecoveryRecord &record,
	const PeripheralBackendIdentity &backend,
	std::unique_ptr<RecoveryAudioVideoSessionBundle> *bundle) const
{
	MISTER_PERIPHERAL_LEASE_CALL(AcquireRecoveryAudioVideoSessionFor(*this,
		record, backend, bundle));
}

Result OperationLease::GetAudioVideoSessionDisposition(HardwareBroker &owner,
	PeripheralBrokerDisposition *disposition) const
{
	MISTER_PERIPHERAL_LEASE_CALL(GetAudioVideoSessionDispositionFor(*this,
		disposition));
}

#undef MISTER_PERIPHERAL_LEASE_CALL

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result OperationLease::BeginConcurrentActiveOperationForTest(
	HardwareBroker &owner, OperationKind operation_kind,
	uint64_t absolute_deadline_ms,
	std::unique_ptr<OperationLease> *lease) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner ||
		registration_->authority != LeaseAuthority::active_generation)
		return MISTER_RESULT_INVALID_STATE;
	return owner.Begin(registration_->authority_identity, operation_kind,
		absolute_deadline_ms, lease);
}

Result OperationLease::RecordActiveCoreProtocolMutationForTest(
	HardwareBroker &owner, ActiveCoreProtocolSession &session,
	uint64_t *mutation_sequence) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker ||
		registration_->broker != &owner)
		return MISTER_RESULT_INVALID_STATE;
	return owner.RecordActiveCoreProtocolMutationForTest(*this, session,
		mutation_sequence);
}
#endif

Result OperationLease::AcquireProcessOperationGuard(HardwareBroker &owner,
	OperationKind required_operation_kind,
	const NativeCoreProfile *required_profile,
	std::unique_ptr<ProcessOperationGuard> *guard) const
{
	if (!registration_ || !registration_->lifetime)
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lifetime_lock(registration_->lifetime->mutex);
	if (registration_->lifetime->broker != registration_->broker)
		return MISTER_RESULT_INVALID_STATE;
	if (registration_->lifetime->destroying)
		return MISTER_RESULT_INVALID_STATE;
	const Result result = registration_->broker->AcquireProcessOperationGuardFor(*this,
		owner, required_operation_kind, required_profile, guard);
	if (result == MISTER_RESULT_OK) {
		++registration_->lifetime->process_guard_count;
		(*guard)->lifetime_registered_ = true;
	}
	return result;
}

OperationKind OperationLease::operation_kind() const
{
	return registration_->operation_kind;
}

uint64_t OperationLease::absolute_deadline_ms() const
{
	return registration_->effective_deadline_ms;
}

OperationInvocation::OperationInvocation(HardwareBroker &broker,
	LeaseAuthority authority, uint64_t authority_identity,
	uint64_t callback_deadline_ms, uint64_t invocation_identity)
	: broker_(&broker), lifetime_(broker.lifetime_), authority_(authority),
	  authority_identity_(authority_identity),
	  callback_deadline_ms_(callback_deadline_ms),
	  identity_(invocation_identity), registered_(true)
{
}

OperationInvocation::~OperationInvocation()
{
	if (!lifetime_ || !registered_) return;
	std::lock_guard<std::mutex> lifetime_lock(lifetime_->mutex);
	if (broker_ != nullptr && registered_ && lifetime_->broker == broker_)
		broker_->UnregisterInvocation(*this);
}

CleanupEpoch::CleanupEpoch(HardwareBroker &broker,
	PlatformGenerationId generation, uint64_t identity,
	uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
	const std::shared_ptr<BrokerLifetime> &lifetime)
	: broker_(&broker), lifetime_(lifetime), generation_(generation), identity_(identity),
	  non_fpga_deadline_ms_(non_fpga_deadline_ms),
	  fpga_deadline_ms_(fpga_deadline_ms), registered_(true)
{
}

CleanupEpoch::~CleanupEpoch()
{
	if (!lifetime_ || !registered_) return;
	std::lock_guard<std::mutex> lifetime_lock(lifetime_->mutex);
	if (broker_ != nullptr && registered_ && lifetime_->broker == broker_)
		broker_->UnregisterCleanup(*this);
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
uint64_t CleanupEpoch::identity_for_test() const
{
	return identity_;
}
#endif

RecoveryEpoch::RecoveryEpoch(HardwareBroker &broker, uint64_t identity,
	uint32_t requested_resource_flags, uint64_t non_fpga_deadline_ms,
	uint64_t fpga_deadline_ms,
	const std::shared_ptr<BrokerLifetime> &lifetime)
	: broker_(&broker), lifetime_(lifetime), identity_(identity),
	 requested_resource_flags_(requested_resource_flags),
	 non_fpga_deadline_ms_(non_fpga_deadline_ms),
	 fpga_deadline_ms_(fpga_deadline_ms), registered_(true)
{
}

RecoveryEpoch::~RecoveryEpoch()
{
	if (!lifetime_ || !registered_) return;
	std::lock_guard<std::mutex> lifetime_lock(lifetime_->mutex);
	if (broker_ != nullptr && registered_ && lifetime_->broker == broker_)
		broker_->UnregisterRecovery(*this);
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
uint64_t RecoveryEpoch::identity_for_test() const
{
	return identity_;
}
#endif

HardwareBroker::HardwareBroker(NativeClock &clock)
	: clock_(clock), lifetime_(new BrokerLifetime(this)),
	  state_(State::idle), generation_(0),
	  cleanup_identity_(0), cleanup_non_fpga_deadline_ms_(0),
	  cleanup_fpga_deadline_ms_(0), terminal_lease_deadline_ms_(0),
	  recovery_identity_(0), recovery_requested_resource_flags_(0),
	  recovery_non_fpga_deadline_ms_(0), recovery_fpga_deadline_ms_(0),
	  invocation_authority_(LeaseAuthority::active_generation),
	  invocation_authority_identity_(0), invocation_identity_(0),
	  invocation_callback_deadline_ms_(0), invocation_registered_(false),
	  invocation_outcome_missing_(false), invocation_registration_(),
	  mutation_sequence_(0), active_lease_count_(0), terminal_lease_count_(0),
	  cleanup_registered_(false), cleanup_ever_started_(false),
	  quiesce_complete_(false), quiesce_call_active_(false),
	  containment_receipt_current_(false), containment_evidence_pending_(false),
	  cleanup_audio_shutdown_complete_(false),
	  recovery_audio_shutdown_complete_(false),
	  recovery_registered_(false),
	  recovery_observation_active_(false), recovery_terminal_neutral_(false),
	  hardware_transaction_active_(false), failure_latched_(false),
	  recovery_result_(MISTER_RESULT_OK),
	  receipt_authority_(LeaseAuthority::active_generation),
	  receipt_authority_identity_(0), receipt_generation_(0),
	  receipt_requested_resource_flags_(0), receipt_core_gpo_(0),
	  receipt_interface_module_(0), receipt_sdr_port_control_(0),
	  receipt_bridge_reset_(0), receipt_remap_(0),
	  receipt_manager_control_(0), receipt_manager_mode_(0),
	  receipt_manager_neutral_observed_(false),
	  receipt_manager_neutral_mutation_sequence_(0),
	  receipt_mutation_sequence_(0), recovery_observed_resource_flags_(0),
	  recovery_neutral_resource_flags_(0), receipt_mappings_released_(false),
	  receipt_registration_(nullptr),
	  core_protocol_failure_receipt_current_(false),
	  core_protocol_session_state_(),
	  profile_(nullptr)
{
	ClearCoreProtocolFailureReceipt();
}

HardwareBroker::~HardwareBroker()
{
	std::unique_lock<std::mutex> lifetime_lock(lifetime_->mutex);
	lifetime_->destroying = true;
	while (lifetime_->process_guard_count != 0)
		lifetime_->process_guards_released.wait(lifetime_lock);
	lifetime_->broker = nullptr;
	lifetime_lock.unlock();
	const std::shared_ptr<ProtocolSessionState> state =
		core_protocol_session_state_.lock();
	if (state) {
		state->handle_state.store(ProtocolSessionHandleState::finalized);
		const std::shared_ptr<OperationRegistration> registration =
			state->owner_registration.lock();
		if (state->view) {
			state->view->registration_.reset();
			state->view.reset();
		}
		if (registration)
			registration->core_protocol_session_state.reset();
	}
	core_protocol_session_state_.reset();
	hardware_transaction_active_ = false;
}

Result HardwareBroker::Enter(const NativeCoreProfile &profile,
	PlatformGenerationId *generation)
{
	return Enter(&profile, generation);
}

Result HardwareBroker::Enter(const NativeCoreProfile *profile,
	PlatformGenerationId *generation)
{
	if (profile != nullptr &&
		profile->authority == NativeProfileAuthority::fixture)
		return MISTER_RESULT_UNSUPPORTED;
	if (generation == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	if (profile == nullptr || !ValidateNativeCoreProfileRecord(*profile))
		return MISTER_RESULT_UNSUPPORTED;

	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::idle || generation_ != 0 ||
		active_lease_count_ != 0 || cleanup_registered_ || recovery_registered_) {
		return MISTER_RESULT_INVALID_STATE;
	}

	const PlatformGenerationId next_generation =
		FreshNonzeroNonce(generation_nonce_source);
	generation_ = next_generation;
	state_ = State::active;
	cleanup_identity_ = 0;
	cleanup_non_fpga_deadline_ms_ = 0;
	cleanup_fpga_deadline_ms_ = 0;
	terminal_lease_deadline_ms_ = 0;
	recovery_identity_ = 0;
	recovery_requested_resource_flags_ = 0;
	recovery_non_fpga_deadline_ms_ = 0;
	recovery_fpga_deadline_ms_ = 0;
	mutation_sequence_ = 0;
	cleanup_ever_started_ = false;
	cleanup_audio_shutdown_complete_ = false;
	recovery_audio_shutdown_complete_ = false;
	quiesce_complete_ = false;
	ClearContainmentReceipt();
	ClearCoreProtocolFailureReceipt();
	recovery_result_ = MISTER_RESULT_OK;
	recovery_observed_resource_flags_ = 0;
	recovery_neutral_resource_flags_ = 0;
	recovery_observation_active_ = false;
	recovery_terminal_neutral_ = false;
	failure_latched_ = false;
	profile_ = profile;
	*generation = next_generation;
	return MISTER_RESULT_OK;
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result HardwareBroker::EnterFixtureForTest(const NativeCoreProfile &profile,
	PlatformGenerationId *generation)
{
	if (generation == nullptr || !IsExactFixtureNativeCoreProfile(profile) ||
		!ValidateNativeCoreProfileRecord(profile))
		return MISTER_RESULT_UNSUPPORTED;
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::idle || generation_ != 0 ||
		active_lease_count_ != 0 || cleanup_registered_ || recovery_registered_)
		return MISTER_RESULT_INVALID_STATE;
	const PlatformGenerationId next_generation =
		FreshNonzeroNonce(generation_nonce_source);
	generation_ = next_generation;
	state_ = State::active;
	profile_ = &profile;
	cleanup_identity_ = 0;
	cleanup_non_fpga_deadline_ms_ = 0;
	cleanup_fpga_deadline_ms_ = 0;
	terminal_lease_deadline_ms_ = 0;
	recovery_identity_ = 0;
	recovery_requested_resource_flags_ = 0;
	recovery_non_fpga_deadline_ms_ = 0;
	recovery_fpga_deadline_ms_ = 0;
	mutation_sequence_ = 0;
	cleanup_ever_started_ = false;
	cleanup_audio_shutdown_complete_ = false;
	recovery_audio_shutdown_complete_ = false;
	quiesce_complete_ = false;
	ClearContainmentReceipt();
	ClearCoreProtocolFailureReceipt();
	recovery_result_ = MISTER_RESULT_OK;
	recovery_observed_resource_flags_ = 0;
	recovery_neutral_resource_flags_ = 0;
	recovery_observation_active_ = false;
	recovery_terminal_neutral_ = false;
	failure_latched_ = false;
	*generation = next_generation;
	return MISTER_RESULT_OK;
}

bool HardwareBroker::has_live_generation_for_test()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return generation_ != 0;
}

uint64_t HardwareBroker::mutation_sequence_for_test()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return mutation_sequence_;
}

uint64_t HardwareBroker::containment_receipt_sequence_for_test()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return containment_receipt_current_ ? receipt_mutation_sequence_ : 0;
}

bool HardwareBroker::containment_manager_receipt_for_test(uint32_t *control,
	uint32_t *mode, uint64_t *mutation_sequence)
{
	if (control == nullptr || mode == nullptr || mutation_sequence == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!containment_receipt_current_ ||
		!receipt_manager_neutral_observed_) return false;
	*control = receipt_manager_control_;
	*mode = receipt_manager_mode_;
	*mutation_sequence = receipt_manager_neutral_mutation_sequence_;
	return true;
}

bool HardwareBroker::core_protocol_failure_receipt_for_test(
	ActiveProtocolFailureReceipt *receipt)
{
	if (receipt == nullptr) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!core_protocol_failure_receipt_current_) return false;
	*receipt = core_protocol_failure_receipt_;
	return true;
}

bool HardwareBroker::core_protocol_session_current_for_test()
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<ProtocolSessionState> state =
		core_protocol_session_state_.lock();
	return state && state->view;
}

bool HardwareBroker::core_protocol_session_abandoned_for_test(
	const OperationLease &lease)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	return registration && registration->registered &&
		registration->broker == this && current && current->view &&
		registration->core_protocol_session_state.get() == current.get() &&
		current->view->registration_.get() == registration.get() &&
		current->handle_state.load() ==
			ProtocolSessionHandleState::abandoned;
}
#endif

Result HardwareBroker::Begin(PlatformGenerationId generation,
	OperationKind operation_kind, uint64_t absolute_deadline_ms,
	std::unique_ptr<OperationLease> *lease)
{
	if (!OutputAvailable(lease)) return MISTER_RESULT_INVALID_ARGUMENT;

	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::active || generation == 0 ||
		generation != generation_ ||
		operation_kind == OperationKind::terminal_fpga_cleanup) {
		return MISTER_RESULT_INVALID_STATE;
	}
	if (clock_.NowMs() >= absolute_deadline_ms) return MISTER_RESULT_DEADLINE;

	std::shared_ptr<OperationRegistration> registration(
		new (std::nothrow) OperationRegistration(*this,
			LeaseAuthority::active_generation, generation, operation_kind,
			absolute_deadline_ms, lifetime_, profile_));
	if (!registration) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<OperationLease> admitted(
		new (std::nothrow) OperationLease(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	registration->registered = true;
	++active_lease_count_;
	*lease = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::Quiesce(PlatformGenerationId generation,
	uint64_t absolute_deadline_ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	if (generation == 0 || generation != generation_ ||
		(state_ != State::active && state_ != State::quiescing) ||
		quiesce_call_active_) {
		return MISTER_RESULT_INVALID_STATE;
	}
	quiesce_call_active_ = true;
	if (state_ == State::active) {
		state_ = State::quiescing;
		quiesce_complete_ = false;
	}

	while (active_lease_count_ != 0) {
		if (clock_.NowMs() >= absolute_deadline_ms) {
			quiesce_call_active_ = false;
			return MISTER_RESULT_DEADLINE;
		}
		if (!clock_.WaitUntil(lease_released_, lock, absolute_deadline_ms)) {
			quiesce_call_active_ = false;
			return MISTER_RESULT_DEADLINE;
		}
	}
	quiesce_complete_ = true;
	quiesce_call_active_ = false;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::LatchFailure(PlatformGenerationId generation)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (generation == 0 || generation != generation_ ||
		(state_ != State::active && state_ != State::quiescing))
		return MISTER_RESULT_INVALID_STATE;
	failure_latched_ = true;
	if (state_ == State::active) {
		state_ = State::quiescing;
		quiesce_complete_ = active_lease_count_ == 0;
	}
	return MISTER_RESULT_OK;
}

Result HardwareBroker::BeginCleanup(PlatformGenerationId generation,
	uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
	std::unique_ptr<CleanupEpoch> *epoch)
{
	if (!OutputAvailable(epoch)) return MISTER_RESULT_INVALID_ARGUMENT;

	std::lock_guard<std::mutex> lock(mutex_);
	if (generation == 0 || generation != generation_ ||
		state_ != State::quiescing || active_lease_count_ != 0 ||
		!quiesce_complete_ || cleanup_registered_ || cleanup_ever_started_ ||
		invocation_registered_) {
		return MISTER_RESULT_INVALID_STATE;
	}

	const uint64_t identity = FreshNonzeroNonce(cleanup_nonce_source);
	std::unique_ptr<CleanupEpoch> registered(new (std::nothrow) CleanupEpoch(
		*this, generation, identity, non_fpga_deadline_ms,
		fpga_deadline_ms, lifetime_));
	if (!registered) return MISTER_RESULT_PLATFORM;

	cleanup_identity_ = identity;
	cleanup_non_fpga_deadline_ms_ = non_fpga_deadline_ms;
	cleanup_fpga_deadline_ms_ = fpga_deadline_ms;
	cleanup_registered_ = true;
	cleanup_ever_started_ = true;
	cleanup_audio_shutdown_complete_ = false;
	ClearContainmentReceipt();
	state_ = State::cleanup;
	*epoch = std::move(registered);
	return MISTER_RESULT_OK;
}

bool HardwareBroker::IsCurrentInvocation(const OperationInvocation &invocation,
	LeaseAuthority authority, uint64_t authority_identity) const
{
	return invocation.registered_ && invocation.broker_ == this &&
		invocation.authority_ == authority &&
		invocation.authority_identity_ == authority_identity &&
		invocation.identity_ == invocation_identity_ &&
		invocation.callback_deadline_ms_ == invocation_callback_deadline_ms_ &&
		invocation_registered_ && invocation_authority_ == authority &&
		invocation_authority_identity_ == authority_identity;
}

Result HardwareBroker::BeginInvocation(LeaseAuthority authority,
	uint64_t authority_identity, uint64_t callback_deadline_ms,
	std::unique_ptr<OperationInvocation> *invocation)
{
	if (!OutputAvailable(invocation) || callback_deadline_ms == 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	if (invocation_registered_ || clock_.NowMs() >= callback_deadline_ms)
		return invocation_registered_ ? MISTER_RESULT_INVALID_STATE :
			MISTER_RESULT_DEADLINE;
	const bool cleanup = authority == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup && cleanup_registered_ &&
		authority_identity == cleanup_identity_;
	const bool recovery = authority == LeaseAuthority::recovery_epoch &&
		state_ == State::recovery && recovery_registered_ &&
		authority_identity == recovery_identity_ && !recovery_terminal_neutral_;
	if (!cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	const uint64_t identity = FreshNonzeroNonce(invocation_nonce_source);
	std::unique_ptr<OperationInvocation> admitted(
		new (std::nothrow) OperationInvocation(*this, authority,
			authority_identity, callback_deadline_ms, identity));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	invocation_authority_ = authority;
	invocation_authority_identity_ = authority_identity;
	invocation_identity_ = identity;
	invocation_callback_deadline_ms_ = callback_deadline_ms;
	invocation_registered_ = true;
	invocation_outcome_missing_ = false;
	invocation_registration_.reset();
	*invocation = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::BeginCleanupInvocation(const CleanupEpoch &epoch,
	uint64_t callback_deadline_ms,
	std::unique_ptr<OperationInvocation> *invocation)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!IsCurrentCleanup(epoch)) return MISTER_RESULT_INVALID_STATE;
	}
	return BeginInvocation(LeaseAuthority::cleanup_epoch, epoch.identity_,
		callback_deadline_ms, invocation);
}

Result HardwareBroker::BeginRecoveryInvocation(const RecoveryEpoch &epoch,
	uint64_t callback_deadline_ms,
	std::unique_ptr<OperationInvocation> *invocation)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!IsCurrentRecovery(epoch)) return MISTER_RESULT_INVALID_STATE;
	}
	return BeginInvocation(LeaseAuthority::recovery_epoch, epoch.identity_,
		callback_deadline_ms, invocation);
}

Result HardwareBroker::BeginInvokedOperation(LeaseAuthority authority,
	uint64_t authority_identity, const OperationInvocation &invocation,
	OperationKind operation_kind, uint64_t authority_deadline_ms,
	const NativeCoreProfile *profile,
	std::unique_ptr<OperationLease> *lease)
{
	if (!OutputAvailable(lease)) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const bool cleanup = authority == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup && cleanup_registered_ &&
		authority_identity == cleanup_identity_;
	const bool recovery = authority == LeaseAuthority::recovery_epoch &&
		state_ == State::recovery && recovery_registered_ &&
		authority_identity == recovery_identity_ &&
		!recovery_observation_active_ && !recovery_terminal_neutral_ &&
		IsRecoveryOperation(operation_kind, recovery_requested_resource_flags_);
	if (!IsCurrentInvocation(invocation, authority, authority_identity) ||
		(!cleanup && !recovery) ||
		invocation_outcome_missing_ || !invocation_registration_.expired() ||
		active_lease_count_ != 0)
		return MISTER_RESULT_INVALID_STATE;
	const uint64_t effective_deadline_ms = authority_deadline_ms <
		invocation.callback_deadline_ms_ ? authority_deadline_ms :
		invocation.callback_deadline_ms_;
	if (clock_.NowMs() >= effective_deadline_ms) return MISTER_RESULT_DEADLINE;
	std::shared_ptr<OperationRegistration> registration(
		new (std::nothrow) OperationRegistration(*this, authority,
			authority_identity, operation_kind, authority_deadline_ms,
			lifetime_, profile));
	if (!registration) return MISTER_RESULT_PLATFORM;
	registration->effective_deadline_ms = effective_deadline_ms;
	registration->invocation_identity = invocation.identity_;
	registration->outcome_recorded = false;
	std::unique_ptr<OperationLease> admitted(
		new (std::nothrow) OperationLease(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	registration->registered = true;
	++active_lease_count_;
	if (operation_kind == OperationKind::terminal_fpga_cleanup) {
		++terminal_lease_count_;
		terminal_lease_deadline_ms_ = effective_deadline_ms;
	}
	invocation_registration_ = registration;
	*lease = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::BeginCleanupOperation(const CleanupEpoch &epoch,
	const OperationInvocation &invocation, OperationKind operation_kind,
	std::unique_ptr<OperationLease> *lease)
{
	uint64_t authority_deadline_ms = 0;
	if (!CleanupDeadline(operation_kind, epoch, &authority_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ != State::cleanup || !IsCurrentCleanup(epoch))
			return MISTER_RESULT_INVALID_STATE;
	}
	return BeginInvokedOperation(LeaseAuthority::cleanup_epoch, epoch.identity_,
		invocation, operation_kind, authority_deadline_ms, profile_, lease);
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result HardwareBroker::BeginCleanupOperation(const CleanupEpoch &epoch,
	OperationKind operation_kind, std::unique_ptr<OperationLease> *lease)
{
	if (!OutputAvailable(lease)) return MISTER_RESULT_INVALID_ARGUMENT;

	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::cleanup || !IsCurrentCleanup(epoch))
		return MISTER_RESULT_INVALID_STATE;

	uint64_t absolute_deadline_ms = 0;
	if (!CleanupDeadline(operation_kind, epoch, &absolute_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= absolute_deadline_ms) return MISTER_RESULT_DEADLINE;

	const bool terminal =
		operation_kind == OperationKind::terminal_fpga_cleanup;
	if (terminal ? active_lease_count_ != 0 : terminal_lease_count_ != 0)
		return MISTER_RESULT_INVALID_STATE;

	std::shared_ptr<OperationRegistration> registration(
		new (std::nothrow) OperationRegistration(*this,
			LeaseAuthority::cleanup_epoch, epoch.identity_, operation_kind,
			absolute_deadline_ms, lifetime_, profile_));
	if (!registration) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<OperationLease> admitted(
		new (std::nothrow) OperationLease(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	registration->registered = true;
	++active_lease_count_;
	if (terminal) {
		++terminal_lease_count_;
		terminal_lease_deadline_ms_ = absolute_deadline_ms;
	}
	*lease = std::move(admitted);
	return MISTER_RESULT_OK;
}
#endif

Result HardwareBroker::AcquireHardwareLeaseView(const OperationLease &lease,
	std::unique_ptr<HardwareLeaseView> *view)
{
	return AcquireHardwareLeaseViewFor(lease, lease.operation_kind(), nullptr,
		view);
}

Result HardwareBroker::AcquireHardwareLeaseViewFor(const OperationLease &lease,
	OperationKind required_operation_kind,
	const NativeCoreProfile *required_profile,
	std::unique_ptr<HardwareLeaseView> *view)
{
	if (view == nullptr || view->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (required_operation_kind == OperationKind::core_protocol)
		return MISTER_RESULT_INVALID_STATE;

	std::lock_guard<std::mutex> lock(mutex_);
	if (hardware_transaction_active_ || containment_evidence_pending_)
		return MISTER_RESULT_INVALID_STATE;
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != required_operation_kind ||
		!IsHardwareOperation(registration->operation_kind)) {
		return MISTER_RESULT_INVALID_STATE;
	}
	if (required_profile != nullptr && registration->profile != required_profile)
		return MISTER_RESULT_UNSUPPORTED;
	const bool active_authority =
		registration->authority == LeaseAuthority::active_generation &&
		registration->authority_identity == generation_ &&
		state_ == State::active && !failure_latched_;
	const bool cleanup_authority =
		registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ &&
		state_ == State::cleanup;
	const bool recovery_authority =
		registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery &&
		!recovery_terminal_neutral_ &&
		IsRecoveryOperation(registration->operation_kind,
			recovery_requested_resource_flags_);
	if (!active_authority && !cleanup_authority && !recovery_authority)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;

	std::unique_ptr<HardwareLeaseView> admitted(
		new (std::nothrow) HardwareLeaseView(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	hardware_transaction_active_ = true;
	*view = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::AcquireActiveCoreProtocolSessionFor(
	const OperationLease &lease, const NativeCoreProfile &profile,
	std::unique_ptr<ActiveCoreProtocolSession> *session)
{
	if (session == nullptr || session->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (hardware_transaction_active_ || containment_evidence_pending_ ||
		!registration || !registration->registered ||
		registration->core_protocol_session_state ||
		registration->core_protocol_completion !=
			CoreProtocolBrokerDisposition::no_session ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		registration->profile != &profile || state_ != State::active ||
		failure_latched_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	std::unique_ptr<ActiveCoreProtocolSession> admitted(
		new (std::nothrow) ActiveCoreProtocolSession());
	if (!admitted) return MISTER_RESULT_PLATFORM;
	std::shared_ptr<ProtocolSessionState> state(
		new (std::nothrow) ProtocolSessionState());
	if (!state) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<HardwareLeaseView> view(
		new (std::nothrow) HardwareLeaseView(registration));
	if (!view) return MISTER_RESULT_PLATFORM;
	state->view = std::move(view);
	state->owner_registration = registration;
	state->profile = registration->profile;
	state->initial_mutation_sequence = mutation_sequence_;
	admitted->state_ = state;
	registration->core_protocol_session_state = state;
	core_protocol_session_state_ = state;
	hardware_transaction_active_ = true;
	*session = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::AcquireCleanupCoreProtocolSessionFor(
	const OperationLease &lease,
	std::unique_ptr<CleanupCoreProtocolSession> *session)
{
	if (session == nullptr || session->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::cleanup_epoch ||
		registration->authority_identity != cleanup_identity_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		state_ != State::cleanup || !cleanup_registered_)
		return MISTER_RESULT_INVALID_STATE;
	if (registration->core_protocol_session_state) {
		// A failed teardown retains the original registration, view, and
		// immutable epoch deadline. Only that registration can atomically
		// re-check out its abandoned typed handle.
		if (clock_.NowMs() >= registration->effective_deadline_ms)
			return MISTER_RESULT_DEADLINE;
		const std::shared_ptr<ProtocolSessionState> current =
			core_protocol_session_state_.lock();
		const std::shared_ptr<ProtocolSessionState> &retained =
			registration->core_protocol_session_state;
		if (!hardware_transaction_active_ || containment_evidence_pending_ ||
			registration->core_protocol_completion !=
				CoreProtocolBrokerDisposition::no_session ||
			current.get() != retained.get() || !retained->view ||
			retained->profile != profile_ ||
			retained->owner_registration.lock().get() != registration.get() ||
			retained->view->registration_.get() != registration.get())
			return MISTER_RESULT_INVALID_STATE;
		std::unique_ptr<CleanupCoreProtocolSession> admitted(
			new (std::nothrow) CleanupCoreProtocolSession());
		if (!admitted) return MISTER_RESULT_PLATFORM;
		ProtocolSessionHandleState expected =
			ProtocolSessionHandleState::abandoned;
		if (!retained->handle_state.compare_exchange_strong(expected,
			ProtocolSessionHandleState::live))
			return MISTER_RESULT_INVALID_STATE;
		admitted->state_ = retained;
		*session = std::move(admitted);
		return MISTER_RESULT_OK;
	}
	if (hardware_transaction_active_ || containment_evidence_pending_ ||
		registration->core_protocol_completion !=
			CoreProtocolBrokerDisposition::no_session)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	std::unique_ptr<CleanupCoreProtocolSession> admitted(
		new (std::nothrow) CleanupCoreProtocolSession());
	if (!admitted) return MISTER_RESULT_PLATFORM;
	std::shared_ptr<ProtocolSessionState> state(
		new (std::nothrow) ProtocolSessionState());
	if (!state) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<HardwareLeaseView> view(
		new (std::nothrow) HardwareLeaseView(registration));
	if (!view) return MISTER_RESULT_PLATFORM;
	state->view = std::move(view);
	state->owner_registration = registration;
	state->profile = registration->profile;
	state->initial_mutation_sequence = mutation_sequence_;
	admitted->state_ = state;
	registration->core_protocol_session_state = state;
	core_protocol_session_state_ = state;
	hardware_transaction_active_ = true;
	*session = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::AcquireRecoveryCoreProtocolSessionFor(
	const OperationLease &lease,
	std::unique_ptr<RecoveryCoreProtocolSession> *session)
{
	if (session == nullptr || session->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != recovery_identity_ ||
		registration->profile != nullptr || state_ != State::recovery ||
		!recovery_registered_ || recovery_terminal_neutral_ ||
		(recovery_requested_resource_flags_ &
		 MISTER_RESOURCE_CORE_PROTOCOL) == 0)
		return MISTER_RESULT_INVALID_STATE;
	if (registration->core_protocol_session_state) {
		// Recovery has no profile, but it retains the exact requested
		// CORE_PROTOCOL registration and its original deadline on failure.
		if (clock_.NowMs() >= registration->effective_deadline_ms)
			return MISTER_RESULT_DEADLINE;
		const std::shared_ptr<ProtocolSessionState> current =
			core_protocol_session_state_.lock();
		const std::shared_ptr<ProtocolSessionState> &retained =
			registration->core_protocol_session_state;
		if (!hardware_transaction_active_ || containment_evidence_pending_ ||
			registration->core_protocol_completion !=
				CoreProtocolBrokerDisposition::no_session ||
			current.get() != retained.get() || !retained->view ||
			retained->profile != nullptr ||
			retained->owner_registration.lock().get() != registration.get() ||
			retained->view->registration_.get() != registration.get())
			return MISTER_RESULT_INVALID_STATE;
		std::unique_ptr<RecoveryCoreProtocolSession> admitted(
			new (std::nothrow) RecoveryCoreProtocolSession());
		if (!admitted) return MISTER_RESULT_PLATFORM;
		ProtocolSessionHandleState expected =
			ProtocolSessionHandleState::abandoned;
		if (!retained->handle_state.compare_exchange_strong(expected,
			ProtocolSessionHandleState::live))
			return MISTER_RESULT_INVALID_STATE;
		admitted->state_ = retained;
		*session = std::move(admitted);
		return MISTER_RESULT_OK;
	}
	if (hardware_transaction_active_ || containment_evidence_pending_ ||
		registration->core_protocol_completion !=
			CoreProtocolBrokerDisposition::no_session)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	std::unique_ptr<RecoveryCoreProtocolSession> admitted(
		new (std::nothrow) RecoveryCoreProtocolSession());
	if (!admitted) return MISTER_RESULT_PLATFORM;
	std::shared_ptr<ProtocolSessionState> state(
		new (std::nothrow) ProtocolSessionState());
	if (!state) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<HardwareLeaseView> view(
		new (std::nothrow) HardwareLeaseView(registration));
	if (!view) return MISTER_RESULT_PLATFORM;
	state->view = std::move(view);
	state->owner_registration = registration;
	state->initial_mutation_sequence = mutation_sequence_;
	admitted->state_ = state;
	registration->core_protocol_session_state = state;
	core_protocol_session_state_ = state;
	hardware_transaction_active_ = true;
	*session = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CompleteFailedCoreProtocolSessionFor(
	const OperationLease &lease,
	std::unique_ptr<ActiveCoreProtocolSession> &&session,
	const ActiveProtocolFailureReceipt &receipt)
{
	if (!session || !session->state_ || !session->state_->view)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	const std::shared_ptr<OperationRegistration> session_owner =
		session->state_->owner_registration.lock();
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		current.get() != session->state_.get() ||
		core_protocol_failure_receipt_current_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->core_protocol_session_state.get() !=
			session->state_.get() || session_owner.get() != registration.get() ||
		session->state_->handle_state.load() !=
			ProtocolSessionHandleState::live ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		session->state_->profile != profile_ ||
		session->state_->view->registration_.get() != registration.get() ||
		state_ != State::active || failure_latched_)
		return MISTER_RESULT_INVALID_STATE;
	if (receipt.primary_result == MISTER_RESULT_OK ||
		receipt.final_mutation_sequence <=
			session->state_->initial_mutation_sequence ||
		receipt.final_mutation_sequence != mutation_sequence_ ||
		receipt.residue.last_mutation_sequence != mutation_sequence_ ||
		receipt.mapping_release.mutation_sequence != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	if (receipt.mapping_release.result == MISTER_RESULT_OK &&
		(!receipt.mapping_release.selected_transaction_closed ||
		 !receipt.mapping_release.unmap_attempted ||
		 !receipt.mapping_release.mapping_absent ||
		 !receipt.mapping_release.descriptor_close_attempted ||
		 !receipt.mapping_release.descriptor_absent))
		return MISTER_RESULT_INVALID_STATE;
	if (receipt.residue.mapping_retained ==
		(receipt.mapping_release.mapping_absent &&
		 receipt.mapping_release.descriptor_absent))
		return MISTER_RESULT_INVALID_STATE;

	core_protocol_failure_receipt_ = receipt;
	core_protocol_failure_receipt_current_ = true;
	failure_latched_ = true;
	state_ = State::quiescing;
	quiesce_complete_ = false;

	// The failure latch and residue become current under this same broker lock
	// before the duration-held protocol view is consumed. Clearing the view's
	// registration makes its destructor inert and avoids reacquiring this lock.
	session->state_->handle_state.store(ProtocolSessionHandleState::finalized);
	ConsumeCoreProtocolSessionState(*registration, session->state_);
	registration->core_protocol_completion =
		CoreProtocolBrokerDisposition::failure_completed;
	session.reset();
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CompleteSuccessfulCoreProtocolSessionFor(
	const OperationLease &lease,
	std::unique_ptr<ActiveCoreProtocolSession> &&session,
	const ProtocolMappingReleaseReceipt &receipt)
{
	if (!session || !session->state_ || !session->state_->view)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	const std::shared_ptr<OperationRegistration> session_owner =
		session->state_->owner_registration.lock();
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		current.get() != session->state_.get() ||
		!registration || !registration->registered ||
		registration->core_protocol_session_state.get() !=
			session->state_.get() || session_owner.get() != registration.get() ||
		session->state_->handle_state.load() !=
			ProtocolSessionHandleState::live ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		session->state_->profile != profile_ ||
		session->state_->view->registration_.get() != registration.get() ||
		state_ != State::active || failure_latched_)
		return MISTER_RESULT_INVALID_STATE;
	if (receipt.result != MISTER_RESULT_OK ||
		!receipt.selected_transaction_closed || !receipt.unmap_attempted ||
		!receipt.mapping_absent || !receipt.descriptor_close_attempted ||
		!receipt.descriptor_absent ||
		receipt.mutation_sequence <=
			session->state_->initial_mutation_sequence ||
		receipt.mutation_sequence != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;

	session->state_->handle_state.store(ProtocolSessionHandleState::finalized);
	ConsumeCoreProtocolSessionState(*registration, session->state_);
	registration->core_protocol_completion =
		CoreProtocolBrokerDisposition::success_completed;
	session.reset();
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CompleteCleanupCoreProtocolSessionFor(
	const OperationLease &lease,
	std::unique_ptr<CleanupCoreProtocolSession> &&session,
	const ProtocolMappingReleaseReceipt &receipt)
{
	if (!session || !session->state_ || !session->state_->view)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	const std::shared_ptr<OperationRegistration> session_owner =
		session->state_->owner_registration.lock();
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		current.get() != session->state_.get() ||
		!registration || !registration->registered ||
		registration->core_protocol_session_state.get() !=
			session->state_.get() || session_owner.get() != registration.get() ||
		session->state_->handle_state.load() !=
			ProtocolSessionHandleState::live ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::cleanup_epoch ||
		registration->authority_identity != cleanup_identity_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		session->state_->profile != profile_ ||
		session->state_->view->registration_.get() != registration.get() ||
		state_ != State::cleanup || !cleanup_registered_)
		return MISTER_RESULT_INVALID_STATE;
	if (receipt.result != MISTER_RESULT_OK ||
		!receipt.selected_transaction_closed || !receipt.unmap_attempted ||
		!receipt.mapping_absent || !receipt.descriptor_close_attempted ||
		!receipt.descriptor_absent ||
		receipt.mutation_sequence != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	session->state_->handle_state.store(ProtocolSessionHandleState::finalized);
	ConsumeCoreProtocolSessionState(*registration, session->state_);
	registration->core_protocol_completion =
		CoreProtocolBrokerDisposition::success_completed;
	session.reset();
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CompleteRecoveryCoreProtocolSessionFor(
	const OperationLease &lease,
	std::unique_ptr<RecoveryCoreProtocolSession> &&session,
	const ProtocolMappingReleaseReceipt &receipt)
{
	if (!session || !session->state_ || !session->state_->view)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	const std::shared_ptr<OperationRegistration> session_owner =
		session->state_->owner_registration.lock();
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		current.get() != session->state_.get() ||
		!registration || !registration->registered ||
		registration->core_protocol_session_state.get() !=
			session->state_.get() || session_owner.get() != registration.get() ||
		session->state_->handle_state.load() !=
			ProtocolSessionHandleState::live ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != recovery_identity_ ||
		registration->profile != nullptr || session->state_->profile != nullptr ||
		session->state_->view->registration_.get() != registration.get() ||
		state_ != State::recovery || !recovery_registered_ ||
		recovery_terminal_neutral_ ||
		(recovery_requested_resource_flags_ & MISTER_RESOURCE_CORE_PROTOCOL) == 0)
		return MISTER_RESULT_INVALID_STATE;
	if (receipt.result != MISTER_RESULT_OK ||
		!receipt.selected_transaction_closed || !receipt.unmap_attempted ||
		!receipt.mapping_absent || !receipt.descriptor_close_attempted ||
		!receipt.descriptor_absent ||
		receipt.mutation_sequence != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	session->state_->handle_state.store(ProtocolSessionHandleState::finalized);
	ConsumeCoreProtocolSessionState(*registration, session->state_);
	registration->core_protocol_completion =
		CoreProtocolBrokerDisposition::success_completed;
	session.reset();
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CompleteInvalidCoreProtocolOutcomeFor(
	const OperationLease &lease, Result primary_result)
{
	if (primary_result == MISTER_RESULT_OK)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		(state_ != State::active &&
		 !(state_ == State::quiescing && failure_latched_)))
		return MISTER_RESULT_INVALID_STATE;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	if (current) {
		const std::shared_ptr<OperationRegistration> session_owner =
			current->owner_registration.lock();
		if (!current->view ||
			registration->core_protocol_session_state.get() != current.get() ||
			session_owner.get() != registration.get() ||
			current->view->registration_.get() != registration.get() ||
			current->handle_state.load() ==
				ProtocolSessionHandleState::finalized)
			return MISTER_RESULT_INVALID_STATE;
	} else if (registration->core_protocol_session_state) {
		return MISTER_RESULT_INVALID_STATE;
	}

	if (!core_protocol_failure_receipt_current_) {
		const bool retained = current != nullptr;
		const bool mutated = current &&
			mutation_sequence_ >
				current->initial_mutation_sequence;
		const CoreProtocolResidue residue = {
			retained, mutated, mutated, mutated, mutated, mutated, mutated,
			mutation_sequence_};
		const ProtocolMappingReleaseReceipt release = {
			MISTER_RESULT_CLEANUP_INCOMPLETE, false, false, !retained,
			false, !retained, mutation_sequence_};
		core_protocol_failure_receipt_ = {
			primary_result, residue, release, mutation_sequence_};
		core_protocol_failure_receipt_current_ = true;
	}
	failure_latched_ = true;
	state_ = State::quiescing;
	quiesce_complete_ = false;
	if (current) {
		current->handle_state.store(ProtocolSessionHandleState::finalized);
		ConsumeCoreProtocolSessionState(*registration, current);
	}
	registration->core_protocol_completion =
		CoreProtocolBrokerDisposition::failure_completed;
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::GetCoreProtocolBrokerDispositionFor(
	const OperationLease &lease,
	CoreProtocolBrokerDisposition *disposition)
{
	if (disposition == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::core_protocol)
		return MISTER_RESULT_INVALID_STATE;
	const bool active = registration->authority ==
		LeaseAuthority::active_generation &&
		registration->authority_identity == generation_ &&
		registration->profile != nullptr && registration->profile == profile_;
	const bool cleanup = registration->authority == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup && cleanup_registered_ &&
		registration->authority_identity == cleanup_identity_ &&
		registration->profile != nullptr && registration->profile == profile_;
	const bool recovery = registration->authority ==
		LeaseAuthority::recovery_epoch && state_ == State::recovery &&
		recovery_registered_ &&
		registration->authority_identity == recovery_identity_ &&
		registration->profile == nullptr;
	if (!active && !cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	bool legacy_test_authority = false;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	legacy_test_authority = registration->invocation_identity == 0 &&
		registration->effective_deadline_ms != 0 && !invocation_registered_;
#endif
	if (!active && !legacy_test_authority &&
		(registration->effective_deadline_ms == 0 ||
		registration->invocation_identity == 0 || !invocation_registered_ ||
		registration->invocation_identity != invocation_identity_))
		return MISTER_RESULT_INVALID_STATE;
	if (registration->core_protocol_completion ==
			CoreProtocolBrokerDisposition::success_completed ||
		registration->core_protocol_completion ==
			CoreProtocolBrokerDisposition::failure_completed) {
		*disposition = registration->core_protocol_completion;
		return MISTER_RESULT_OK;
	}
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	if (current && current->view &&
		registration->core_protocol_session_state.get() == current.get() &&
		current->owner_registration.lock().get() == registration.get() &&
		current->view->registration_.get() == registration.get()) {
		*disposition = current->handle_state.load() ==
			ProtocolSessionHandleState::abandoned ?
			CoreProtocolBrokerDisposition::session_abandoned :
			CoreProtocolBrokerDisposition::session_current;
		return MISTER_RESULT_OK;
	}
	*disposition = CoreProtocolBrokerDisposition::no_session;
	return MISTER_RESULT_OK;
}

PeripheralBackendIdentity HardwareBroker::CreatePeripheralBackendIdentity(
	const void *adapter_instance)
{
	return PeripheralBackendIdentity(adapter_instance,
		FreshNonzeroNonce(peripheral_backend_nonce_source));
}

namespace {

bool PeripheralBackendMatches(const PeripheralBackendIdentity &left,
	const PeripheralBackendIdentity &right)
{
	return left.Matches(right);
}

bool PeripheralSessionWrapperAllocationAllowed()
{
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	return PeripheralSessionAllocationAllowedForTest();
#else
	return true;
#endif
}

bool PeripheralReceiptComplete(const PeripheralCompletionReceipt &receipt)
{
	return receipt.result == MISTER_RESULT_OK && receipt.transaction_closed &&
		receipt.mapping_absent && receipt.descriptor_absent &&
		receipt.local_resources_absent && !receipt.closure_unknown;
}

bool CoupledReceiptComplete(const CoupledCompletionReceipt &receipt)
{
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	return receipt.result == MISTER_RESULT_OK &&
		receipt.affected_flags == affected &&
		(receipt.observed_flags & receipt.neutral_flags) == 0 &&
		((receipt.observed_flags | receipt.neutral_flags) & ~affected) == 0 &&
		receipt.transaction_closed && receipt.local_resources_absent &&
		!receipt.closure_unknown;
}

bool PeripheralActionMatchesSession(PeripheralSessionAction action,
	PeripheralSessionKind kind)
{
	switch (action) {
	case PeripheralSessionAction::audio_attenuation:
		return kind == PeripheralSessionKind::audio;
	case PeripheralSessionAction::video_activation:
	case PeripheralSessionAction::video_teardown:
		return kind == PeripheralSessionKind::video;
	case PeripheralSessionAction::coupled_transmitter:
		return kind == PeripheralSessionKind::audio_video;
	case PeripheralSessionAction::none:
		return false;
	}
	return false;
}

} // namespace

Result HardwareBroker::AcquirePeripheralState(const OperationLease &lease,
	PeripheralSessionKind kind, const NativeCoreProfile *profile,
	const SafePeripheralRecoveryRecord *recovery_record,
	const PeripheralBackendIdentity &backend,
	std::shared_ptr<PeripheralSessionState> *state)
{
	if (state == nullptr || *state) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered || registration->broker != this ||
		containment_evidence_pending_)
		return MISTER_RESULT_INVALID_STATE;
	const OperationKind required_kind = kind == PeripheralSessionKind::audio ?
		OperationKind::audio : kind == PeripheralSessionKind::video ?
		OperationKind::video : OperationKind::audio_video;
	if (registration->operation_kind != required_kind) return MISTER_RESULT_INVALID_STATE;
	const bool active = registration->authority == LeaseAuthority::active_generation &&
		registration->authority_identity == generation_ && state_ == State::active &&
		!failure_latched_;
	const bool cleanup = registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ && state_ == State::cleanup;
	const bool recovery = registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ && state_ == State::recovery &&
		!recovery_terminal_neutral_ &&
		IsRecoveryOperation(required_kind, recovery_requested_resource_flags_);
	if (!active && !cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	if (active) {
		if (profile == nullptr || registration->profile != profile_ ||
			registration->profile != profile || recovery_record != nullptr)
			return MISTER_RESULT_INVALID_STATE;
	} else if (recovery) {
		if (profile != nullptr || recovery_record == nullptr)
			return MISTER_RESULT_UNSUPPORTED;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
		if (recovery_record->authority != NativeProfileAuthority::fixture ||
			!IsExactFixtureSafePeripheralRecoveryRecordForTest(recovery_record))
			return MISTER_RESULT_UNSUPPORTED;
#else
		return MISTER_RESULT_UNSUPPORTED;
#endif
	} else if (profile != nullptr || recovery_record != nullptr ||
		registration->profile != profile_) {
		return MISTER_RESULT_INVALID_STATE;
	}
	if (registration->peripheral_session_state) {
		const std::shared_ptr<PeripheralSessionState> retained =
			registration->peripheral_session_state;
		const std::shared_ptr<PeripheralSessionState> current =
			peripheral_session_state_.lock();
		PeripheralSessionPhase expected = PeripheralSessionPhase::abandoned;
		if (registration->peripheral_completion !=
				PeripheralBrokerDisposition::no_session ||
			!retained || current.get() != retained.get() || !retained->view ||
			retained->owner_registration.lock().get() != registration.get() ||
			retained->view->registration_.get() != registration.get() ||
			retained->kind != kind || !retained->recheckout_allowed ||
			!hardware_transaction_active_ ||
			!PeripheralBackendMatches(retained->backend, backend) ||
			!retained->phase.compare_exchange_strong(expected,
				PeripheralSessionPhase::live))
			return MISTER_RESULT_INVALID_STATE;
		*state = retained;
		return MISTER_RESULT_OK;
	}
	if (registration->peripheral_completion != PeripheralBrokerDisposition::no_session)
		return MISTER_RESULT_INVALID_STATE;
	if (hardware_transaction_active_) return MISTER_RESULT_INVALID_STATE;
	std::shared_ptr<PeripheralSessionState> admitted(new (std::nothrow)
		PeripheralSessionState(kind, backend));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<HardwareLeaseView> view(new (std::nothrow)
		HardwareLeaseView(registration));
	if (!view) return MISTER_RESULT_PLATFORM;
	admitted->view = std::move(view);
	admitted->owner_registration = registration;
	// Cleanup reuses the immutable profile bound to the registration. The
	// resource receives only the typed session, never an independently supplied
	// profile selector.
	admitted->profile = active ? profile : registration->profile;
	admitted->recovery_record = recovery_record;
	admitted->initial_mutation_sequence = mutation_sequence_;
	// A known no-admission receipt must match the broker sequence that was
	// current when this exact registration-owned state was first admitted.
	// Retained recheckout deliberately bypasses this initialization so suffix
	// progress and its later mutation sequence are never reset.
	admitted->last_mutation_sequence = mutation_sequence_;
	admitted->absolute_deadline_ms = registration->effective_deadline_ms;
	registration->peripheral_session_state = admitted;
	peripheral_session_state_ = admitted;
	hardware_transaction_active_ = true;
	*state = admitted;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::GetPeripheralDisposition(const OperationLease &lease,
	PeripheralSessionKind kind, PeripheralBrokerDisposition *disposition)
{
	if (disposition == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered || registration->broker != this ||
		(registration->operation_kind != (kind == PeripheralSessionKind::audio ?
			OperationKind::audio : kind == PeripheralSessionKind::video ?
			OperationKind::video : OperationKind::audio_video)))
		return MISTER_RESULT_INVALID_STATE;
	bool legacy_test_authority = false;
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	legacy_test_authority = registration->invocation_identity == 0 &&
		registration->effective_deadline_ms != 0 && !invocation_registered_;
#endif
	if (registration->authority != LeaseAuthority::active_generation &&
		!legacy_test_authority &&
		(registration->effective_deadline_ms == 0 ||
		 registration->invocation_identity == 0 || !invocation_registered_ ||
		 registration->invocation_identity != invocation_identity_))
		return MISTER_RESULT_INVALID_STATE;
	if (registration->peripheral_completion != PeripheralBrokerDisposition::no_session) {
		*disposition = registration->peripheral_completion;
		return MISTER_RESULT_OK;
	}
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (current && registration->peripheral_session_state.get() == current.get() &&
		current->kind == kind && current->view &&
		current->owner_registration.lock().get() == registration.get() &&
		current->view->registration_.get() == registration.get()) {
		*disposition = current->phase.load() == PeripheralSessionPhase::abandoned ?
			PeripheralBrokerDisposition::abandoned : PeripheralBrokerDisposition::live;
		return MISTER_RESULT_OK;
	}
	*disposition = PeripheralBrokerDisposition::no_session;
	return MISTER_RESULT_OK;
}

void HardwareBroker::ConsumePeripheralSessionState(OperationRegistration &registration,
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralBrokerDisposition disposition)
{
	if (state && state->view) {
		state->view->registration_.reset();
		state->view.reset();
	}
	if (state) state->phase.store(PeripheralSessionPhase::finalized);
	registration.peripheral_session_state.reset();
	registration.peripheral_completion = disposition;
	peripheral_session_state_.reset();
	hardware_transaction_active_ = false;
}

Result HardwareBroker::ResolvePeripheralWrapperAllocationFailure(
	const std::shared_ptr<PeripheralSessionState> &state)
{
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() ||
		registration->peripheral_session_state.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->phase.load() != PeripheralSessionPhase::live ||
		registration->peripheral_completion !=
			PeripheralBrokerDisposition::no_session || !hardware_transaction_active_)
		return MISTER_RESULT_INVALID_STATE;
	if (registration->authority == LeaseAuthority::active_generation) {
		if (state_ != State::active || registration->authority_identity != generation_ ||
			failure_latched_) return MISTER_RESULT_INVALID_STATE;
		// Active admission has not reached a consumer.  Roll it all the way
		// back so the exact registration is unfenced and can mint a fresh
		// wrapper; no hardware mutation or successful receipt is implied.
		ConsumePeripheralSessionState(*registration, state,
			PeripheralBrokerDisposition::no_session);
		lease_released_.notify_all();
		return MISTER_RESULT_OK;
	}
	if ((registration->authority == LeaseAuthority::cleanup_epoch &&
		 state_ == State::cleanup &&
		 registration->authority_identity == cleanup_identity_) ||
		(registration->authority == LeaseAuthority::recovery_epoch &&
		 state_ == State::recovery &&
		 registration->authority_identity == recovery_identity_)) {
		state->phase.store(PeripheralSessionPhase::abandoned);
		return MISTER_RESULT_OK;
	}
	return MISTER_RESULT_INVALID_STATE;
}

Result HardwareBroker::CompletePeripheralSession(
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionKind kind, LeaseAuthority authority, bool active_failure,
	bool abandon, const PeripheralCompletionReceipt &receipt)
{
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() ||
		registration->peripheral_session_state.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->kind != kind || state->phase.load() != PeripheralSessionPhase::live ||
		registration->authority != authority ||
		registration->peripheral_completion != PeripheralBrokerDisposition::no_session ||
		!hardware_transaction_active_ || containment_evidence_pending_)
		return MISTER_RESULT_INVALID_STATE;
	const bool active = authority == LeaseAuthority::active_generation &&
		state_ == State::active && registration->authority_identity == generation_ &&
		!failure_latched_;
	const bool cleanup = authority == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup && registration->authority_identity == cleanup_identity_;
	const bool recovery = authority == LeaseAuthority::recovery_epoch &&
		state_ == State::recovery && registration->authority_identity == recovery_identity_;
	if (!active && !cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	if (receipt.mutation_sequence != mutation_sequence_ ||
		(clock_.NowMs() >= registration->effective_deadline_ms &&
		 receipt.result == MISTER_RESULT_OK))
		return MISTER_RESULT_DEADLINE;
	if (active_failure) {
		const bool no_residue_before_first_mutation =
			receipt.mutation_sequence == state->initial_mutation_sequence &&
			receipt.transaction_closed && receipt.mapping_absent &&
			receipt.descriptor_absent && receipt.local_resources_absent &&
			!receipt.closure_unknown;
		if (!active || receipt.result == MISTER_RESULT_OK ||
			(receipt.mutation_sequence <= state->initial_mutation_sequence &&
			 !no_residue_before_first_mutation))
			return MISTER_RESULT_INVALID_STATE;
		failure_latched_ = true;
		state_ = State::quiescing;
		quiesce_complete_ = false;
		ConsumePeripheralSessionState(*registration, state,
			PeripheralBrokerDisposition::failure_completed);
		lease_released_.notify_all();
		return MISTER_RESULT_OK;
	}
	if (abandon) {
		if (active || (!receipt.closure_unknown &&
			(!receipt.mapping_absent || !receipt.descriptor_absent)))
			return MISTER_RESULT_INVALID_STATE;
		state->recheckout_allowed = !receipt.closure_unknown &&
			!state->action_progress_unknown;
		state->phase.store(PeripheralSessionPhase::abandoned);
		return MISTER_RESULT_OK;
	}
	if (!PeripheralReceiptComplete(receipt) ||
		(state->action != PeripheralSessionAction::none &&
			(state->action_progress_unknown || !state->action_transaction_closed ||
			 state->action_next_word_index != state->action_word_count)) ||
		(active && receipt.mutation_sequence <= state->initial_mutation_sequence))
		return MISTER_RESULT_INVALID_STATE;
	if (cleanup && kind == PeripheralSessionKind::audio)
		cleanup_audio_shutdown_complete_ = true;
	if (recovery && kind == PeripheralSessionKind::audio)
		recovery_audio_shutdown_complete_ = true;
	ConsumePeripheralSessionState(*registration, state,
		PeripheralBrokerDisposition::success_completed);
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CompleteCoupledSession(
	const std::shared_ptr<PeripheralSessionState> &state,
	LeaseAuthority authority, bool active_failure, bool abandon,
	const CoupledCompletionReceipt &receipt, Result primary_result)
{
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() || registration->peripheral_session_state.get() !=
			state.get() || !state->view || state->kind != PeripheralSessionKind::audio_video ||
		state->phase.load() != PeripheralSessionPhase::live ||
		registration->operation_kind != OperationKind::audio_video ||
		registration->authority != authority || !hardware_transaction_active_ ||
		containment_evidence_pending_ || receipt.mutation_sequence != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	const bool active = authority == LeaseAuthority::active_generation &&
		state_ == State::active && registration->authority_identity == generation_ &&
		!failure_latched_;
	const bool cleanup = authority == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup &&
		registration->authority_identity == cleanup_identity_;
	const bool recovery = authority == LeaseAuthority::recovery_epoch &&
		state_ == State::recovery &&
		registration->authority_identity == recovery_identity_ &&
		!recovery_terminal_neutral_ &&
		IsRecoveryOperation(OperationKind::audio_video,
			recovery_requested_resource_flags_);
	if (!active && !cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	if (active_failure) {
		const bool no_residue_before_first_mutation =
			receipt.mutation_sequence == state->initial_mutation_sequence &&
			receipt.transaction_closed && receipt.local_resources_absent &&
			!receipt.closure_unknown;
		if (!active || primary_result == MISTER_RESULT_OK ||
			(receipt.mutation_sequence <= state->initial_mutation_sequence &&
			 !no_residue_before_first_mutation))
			return MISTER_RESULT_INVALID_STATE;
		failure_latched_ = true;
		state_ = State::quiescing;
		quiesce_complete_ = false;
		ConsumePeripheralSessionState(*registration, state,
			PeripheralBrokerDisposition::failure_completed);
		lease_released_.notify_all();
		return MISTER_RESULT_OK;
	}
	if (abandon) {
		if (authority == LeaseAuthority::active_generation)
			return MISTER_RESULT_INVALID_STATE;
		state->recheckout_allowed = !receipt.closure_unknown &&
			!state->action_progress_unknown;
		state->phase.store(PeripheralSessionPhase::abandoned);
		return MISTER_RESULT_OK;
	}
	if (!CoupledReceiptComplete(receipt) ||
		(state->action != PeripheralSessionAction::none &&
			(state->action_progress_unknown || !state->action_transaction_closed ||
			 state->action_next_word_index != state->action_word_count)) ||
		(clock_.NowMs() >= registration->effective_deadline_ms &&
		 receipt.result == MISTER_RESULT_OK) ||
		(active && receipt.mutation_sequence <= state->initial_mutation_sequence))
		return MISTER_RESULT_INVALID_STATE;
	ConsumePeripheralSessionState(*registration, state,
		PeripheralBrokerDisposition::success_completed);
	lease_released_.notify_all();
	return MISTER_RESULT_OK;
}

#define MISTER_DEFINE_PERIPHERAL_ACQUIRE(name, session_type, bundle_type, kind_value) \
Result HardwareBroker::name(const OperationLease &lease, const NativeCoreProfile &profile, \
	const PeripheralBackendIdentity &backend, std::unique_ptr<bundle_type> *bundle) \
{ \
	if (bundle == nullptr || bundle->get() != nullptr) \
		return MISTER_RESULT_INVALID_ARGUMENT; \
	std::shared_ptr<PeripheralSessionState> state; \
	const Result result = AcquirePeripheralState(lease, kind_value, &profile, nullptr, \
		backend, &state); \
	if (result != MISTER_RESULT_OK) return result; \
	if (!PeripheralSessionWrapperAllocationAllowed()) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	std::unique_ptr<session_type> admitted(new (std::nothrow) session_type()); \
	if (!admitted) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	if (!PeripheralSessionWrapperAllocationAllowed()) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	std::unique_ptr<bundle_type> admitted_bundle(new (std::nothrow) bundle_type( \
		std::move(admitted), backend)); \
	if (!admitted_bundle) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	admitted_bundle->session_->state_ = state; \
	*bundle = std::move(admitted_bundle); \
	return MISTER_RESULT_OK; \
}

MISTER_DEFINE_PERIPHERAL_ACQUIRE(AcquireActiveAudioSessionFor,
	ActiveAudioSession, ActiveAudioSessionBundle, PeripheralSessionKind::audio)
MISTER_DEFINE_PERIPHERAL_ACQUIRE(AcquireActiveVideoSessionFor,
	ActiveVideoSession, ActiveVideoSessionBundle, PeripheralSessionKind::video)
MISTER_DEFINE_PERIPHERAL_ACQUIRE(AcquireActiveAudioVideoSessionFor,
	ActiveAudioVideoSession, ActiveAudioVideoSessionBundle,
	PeripheralSessionKind::audio_video)

#undef MISTER_DEFINE_PERIPHERAL_ACQUIRE

#define MISTER_DEFINE_PERIPHERAL_CLEANUP_ACQUIRE(name, session_type, bundle_type, kind_value) \
Result HardwareBroker::name(const OperationLease &lease, \
	const PeripheralBackendIdentity &backend, std::unique_ptr<bundle_type> *bundle) \
{ \
	if (bundle == nullptr || bundle->get() != nullptr) \
		return MISTER_RESULT_INVALID_ARGUMENT; \
	std::shared_ptr<PeripheralSessionState> state; \
	const Result result = AcquirePeripheralState(lease, kind_value, nullptr, nullptr, \
		backend, &state); \
	if (result != MISTER_RESULT_OK) return result; \
	if (!PeripheralSessionWrapperAllocationAllowed()) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	std::unique_ptr<session_type> admitted(new (std::nothrow) session_type()); \
	if (!admitted) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	if (!PeripheralSessionWrapperAllocationAllowed()) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	std::unique_ptr<bundle_type> admitted_bundle(new (std::nothrow) bundle_type( \
		std::move(admitted), backend)); \
	if (!admitted_bundle) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	admitted_bundle->session_->state_ = state; \
	*bundle = std::move(admitted_bundle); \
	return MISTER_RESULT_OK; \
}

MISTER_DEFINE_PERIPHERAL_CLEANUP_ACQUIRE(AcquireCleanupAudioSessionFor,
	CleanupAudioSession, CleanupAudioSessionBundle, PeripheralSessionKind::audio)
MISTER_DEFINE_PERIPHERAL_CLEANUP_ACQUIRE(AcquireCleanupVideoSessionFor,
	CleanupVideoSession, CleanupVideoSessionBundle, PeripheralSessionKind::video)
MISTER_DEFINE_PERIPHERAL_CLEANUP_ACQUIRE(AcquireCleanupAudioVideoSessionFor,
	CleanupAudioVideoSession, CleanupAudioVideoSessionBundle,
	PeripheralSessionKind::audio_video)

#undef MISTER_DEFINE_PERIPHERAL_CLEANUP_ACQUIRE

#define MISTER_DEFINE_PERIPHERAL_RECOVERY_ACQUIRE(name, session_type, bundle_type, kind_value, record_type) \
Result HardwareBroker::name(const OperationLease &lease, const record_type &record, \
	const PeripheralBackendIdentity &backend, std::unique_ptr<bundle_type> *bundle) \
{ \
	if (bundle == nullptr || bundle->get() != nullptr) \
		return MISTER_RESULT_INVALID_ARGUMENT; \
	std::shared_ptr<PeripheralSessionState> state; \
	const Result result = AcquirePeripheralState(lease, kind_value, nullptr, &record.base, \
		backend, &state); \
	if (result != MISTER_RESULT_OK) return result; \
	if (!PeripheralSessionWrapperAllocationAllowed()) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	std::unique_ptr<session_type> admitted(new (std::nothrow) session_type()); \
	if (!admitted) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	if (!PeripheralSessionWrapperAllocationAllowed()) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	std::unique_ptr<bundle_type> admitted_bundle(new (std::nothrow) bundle_type( \
		std::move(admitted), backend)); \
	if (!admitted_bundle) { \
		const Result resolved = ResolvePeripheralWrapperAllocationFailure(state); \
		return resolved == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM : resolved; \
	} \
	admitted_bundle->session_->state_ = state; \
	*bundle = std::move(admitted_bundle); \
	return MISTER_RESULT_OK; \
}

MISTER_DEFINE_PERIPHERAL_RECOVERY_ACQUIRE(AcquireRecoveryAudioSessionFor,
	RecoveryAudioSession, RecoveryAudioSessionBundle, PeripheralSessionKind::audio,
	SafeAudioRecoveryRecord)
MISTER_DEFINE_PERIPHERAL_RECOVERY_ACQUIRE(AcquireRecoveryVideoSessionFor,
	RecoveryVideoSession, RecoveryVideoSessionBundle, PeripheralSessionKind::video,
	SafeVideoRecoveryRecord)
MISTER_DEFINE_PERIPHERAL_RECOVERY_ACQUIRE(AcquireRecoveryAudioVideoSessionFor,
	RecoveryAudioVideoSession, RecoveryAudioVideoSessionBundle,
	PeripheralSessionKind::audio_video,
	SafeAudioVideoRecoveryRecord)

#undef MISTER_DEFINE_PERIPHERAL_RECOVERY_ACQUIRE

Result HardwareBroker::GetAudioSessionDispositionFor(const OperationLease &lease,
	PeripheralBrokerDisposition *disposition)
{
	return GetPeripheralDisposition(lease, PeripheralSessionKind::audio,
		disposition);
}

Result HardwareBroker::GetVideoSessionDispositionFor(const OperationLease &lease,
	PeripheralBrokerDisposition *disposition)
{
	return GetPeripheralDisposition(lease, PeripheralSessionKind::video,
		disposition);
}

Result HardwareBroker::GetAudioVideoSessionDispositionFor(
	const OperationLease &lease, PeripheralBrokerDisposition *disposition)
{
	return GetPeripheralDisposition(lease, PeripheralSessionKind::audio_video,
		disposition);
}

#define MISTER_DEFINE_PERIPHERAL_COMPLETE(name, session_type, kind_value, authority_value, failure, abandoned) \
Result HardwareBroker::name(std::unique_ptr<session_type> &&session, \
	const PeripheralCompletionReceipt &receipt) \
{ \
	if (!session) return MISTER_RESULT_INVALID_ARGUMENT; \
	const std::shared_ptr<PeripheralSessionState> state = session->state_; \
	const Result result = CompletePeripheralSession(state, kind_value, authority_value, \
		failure, abandoned, receipt); \
	if (result == MISTER_RESULT_OK && !abandoned) session.reset(); \
	return result; \
}

MISTER_DEFINE_PERIPHERAL_COMPLETE(CompleteActiveAudioSuccess, ActiveAudioSession,
	PeripheralSessionKind::audio, LeaseAuthority::active_generation, false, false)
MISTER_DEFINE_PERIPHERAL_COMPLETE(CompleteCleanupAudio, CleanupAudioSession,
	PeripheralSessionKind::audio, LeaseAuthority::cleanup_epoch, false, false)
MISTER_DEFINE_PERIPHERAL_COMPLETE(CompleteRecoveryAudio, RecoveryAudioSession,
	PeripheralSessionKind::audio, LeaseAuthority::recovery_epoch, false, false)
MISTER_DEFINE_PERIPHERAL_COMPLETE(CompleteActiveVideoSuccess, ActiveVideoSession,
	PeripheralSessionKind::video, LeaseAuthority::active_generation, false, false)
MISTER_DEFINE_PERIPHERAL_COMPLETE(CompleteCleanupVideo, CleanupVideoSession,
	PeripheralSessionKind::video, LeaseAuthority::cleanup_epoch, false, false)
MISTER_DEFINE_PERIPHERAL_COMPLETE(CompleteRecoveryVideo, RecoveryVideoSession,
	PeripheralSessionKind::video, LeaseAuthority::recovery_epoch, false, false)

#undef MISTER_DEFINE_PERIPHERAL_COMPLETE

#define MISTER_DEFINE_PERIPHERAL_FAILURE(name, session_type, kind_value, authority_value, active_failure, abandoned) \
Result HardwareBroker::name(std::unique_ptr<session_type> &&session, \
	const PeripheralFailureReceipt &receipt) \
{ \
	if (!session) return MISTER_RESULT_INVALID_ARGUMENT; \
	PeripheralCompletionReceipt completion = receipt.release; \
	completion.result = receipt.primary_result; \
	const std::shared_ptr<PeripheralSessionState> state = session->state_; \
	const Result result = CompletePeripheralSession(state, kind_value, authority_value, \
		active_failure, abandoned, completion); \
	if (result == MISTER_RESULT_OK && !abandoned) session.reset(); \
	return result; \
}

MISTER_DEFINE_PERIPHERAL_FAILURE(CompleteActiveAudioFailure, ActiveAudioSession,
	PeripheralSessionKind::audio, LeaseAuthority::active_generation, true, false)
MISTER_DEFINE_PERIPHERAL_FAILURE(AbandonCleanupAudio, CleanupAudioSession,
	PeripheralSessionKind::audio, LeaseAuthority::cleanup_epoch, false, true)
MISTER_DEFINE_PERIPHERAL_FAILURE(AbandonRecoveryAudio, RecoveryAudioSession,
	PeripheralSessionKind::audio, LeaseAuthority::recovery_epoch, false, true)
MISTER_DEFINE_PERIPHERAL_FAILURE(CompleteActiveVideoFailure, ActiveVideoSession,
	PeripheralSessionKind::video, LeaseAuthority::active_generation, true, false)
MISTER_DEFINE_PERIPHERAL_FAILURE(AbandonCleanupVideo, CleanupVideoSession,
	PeripheralSessionKind::video, LeaseAuthority::cleanup_epoch, false, true)
MISTER_DEFINE_PERIPHERAL_FAILURE(AbandonRecoveryVideo, RecoveryVideoSession,
	PeripheralSessionKind::video, LeaseAuthority::recovery_epoch, false, true)

#undef MISTER_DEFINE_PERIPHERAL_FAILURE

Result HardwareBroker::CompleteActiveAudioVideoSuccess(
	std::unique_ptr<ActiveAudioVideoSession> &&session,
	const CoupledAcquisitionReceipt &receipt)
{
	const CoupledCompletionReceipt completion = {receipt.result,
		receipt.affected_flags, 0, 0, receipt.local_resources_absent,
		receipt.closure_unknown, receipt.mutation_sequence,
		receipt.transaction_closed, receipt.transaction_residue};
	if (!session) return MISTER_RESULT_INVALID_ARGUMENT;
	const Result result = CompleteCoupledSession(session->state_,
		LeaseAuthority::active_generation, false, false, completion, receipt.result);
	if (result == MISTER_RESULT_OK) session.reset();
	return result;
}

Result HardwareBroker::CompleteActiveAudioVideoFailure(
	std::unique_ptr<ActiveAudioVideoSession> &&session,
	const CoupledFailureReceipt &receipt)
{
	const CoupledCompletionReceipt completion = {receipt.release.result,
		receipt.release.affected_flags, 0, 0,
		receipt.release.local_resources_absent, receipt.release.closure_unknown,
		receipt.release.mutation_sequence, receipt.release.transaction_closed,
		receipt.release.transaction_residue};
	if (!session) return MISTER_RESULT_INVALID_ARGUMENT;
	const Result result = CompleteCoupledSession(session->state_,
		LeaseAuthority::active_generation, true, false, completion,
		receipt.primary_result);
	if (result == MISTER_RESULT_OK) session.reset();
	return result;
}

#define MISTER_DEFINE_COUPLED_COMPLETE(name, session_type, authority_value, abandon) \
Result HardwareBroker::name(std::unique_ptr<session_type> &&session, \
	const CoupledCompletionReceipt &receipt) \
{ \
	if (!session) return MISTER_RESULT_INVALID_ARGUMENT; \
	const Result result = CompleteCoupledSession(session->state_, authority_value, \
		false, abandon, receipt, receipt.result); \
	if (result == MISTER_RESULT_OK && !abandon) session.reset(); \
	return result; \
}

MISTER_DEFINE_COUPLED_COMPLETE(CompleteCleanupAudioVideo,
	CleanupAudioVideoSession, LeaseAuthority::cleanup_epoch, false)
MISTER_DEFINE_COUPLED_COMPLETE(CompleteRecoveryAudioVideo,
	RecoveryAudioVideoSession, LeaseAuthority::recovery_epoch, false)

#undef MISTER_DEFINE_COUPLED_COMPLETE

#define MISTER_DEFINE_COUPLED_ABANDON(name, session_type, authority_value) \
Result HardwareBroker::name(std::unique_ptr<session_type> &&session, \
	const CoupledFailureReceipt &receipt) \
{ \
	const CoupledCompletionReceipt completion = {receipt.release.result, \
		receipt.release.affected_flags, 0, 0, receipt.release.local_resources_absent, \
		receipt.release.closure_unknown, receipt.release.mutation_sequence, \
		receipt.release.transaction_closed, receipt.release.transaction_residue}; \
	if (!session) return MISTER_RESULT_INVALID_ARGUMENT; \
	return CompleteCoupledSession(session->state_, authority_value, false, true, \
		completion, receipt.primary_result); \
}

MISTER_DEFINE_COUPLED_ABANDON(AbandonCleanupAudioVideo,
	CleanupAudioVideoSession, LeaseAuthority::cleanup_epoch)
MISTER_DEFINE_COUPLED_ABANDON(AbandonRecoveryAudioVideo,
	RecoveryAudioVideoSession, LeaseAuthority::recovery_epoch)

#undef MISTER_DEFINE_COUPLED_ABANDON

Result HardwareBroker::RecordPeripheralMutation(
	const std::shared_ptr<PeripheralSessionState> &state,
	uint64_t *mutation_sequence)
{
	if (mutation_sequence == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	*mutation_sequence = 0;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state ? state->owner_registration.lock() :
		std::shared_ptr<OperationRegistration>();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!state || !registration || !registration->registered ||
		registration->broker != this || current.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->phase.load() != PeripheralSessionPhase::live ||
		!hardware_transaction_active_ || containment_evidence_pending_ ||
		clock_.NowMs() >= registration->effective_deadline_ms ||
		mutation_sequence_ == UINT64_MAX)
		return MISTER_RESULT_INVALID_STATE;
	*mutation_sequence = ++mutation_sequence_;
	state->last_mutation_sequence = *mutation_sequence;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::PreparePeripheralAction(
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionAction action, const void *profile_identity,
	uint8_t word_count, uint8_t *next_word_index)
{
	if (next_word_index == nullptr || action == PeripheralSessionAction::none ||
		profile_identity == nullptr || word_count == 0 || !state)
		return MISTER_RESULT_INVALID_ARGUMENT;
	*next_word_index = 0;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() ||
		registration->peripheral_session_state.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->phase.load() != PeripheralSessionPhase::live ||
		!hardware_transaction_active_ || containment_evidence_pending_ ||
		!PeripheralActionMatchesSession(action, state->kind))
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	if (state->action == PeripheralSessionAction::none) {
		state->action = action;
		state->action_profile_identity = profile_identity;
		state->action_word_count = word_count;
		state->action_next_word_index = 0;
		// Preparing an exact retry binds its immutable recipe but does not open
		// a transport transaction. A failed Begin must retain this closed state
		// and the completed-word suffix unchanged.
		state->action_transaction_closed = true;
		state->action_progress_unknown = false;
	} else if (state->action != action ||
		state->action_profile_identity != profile_identity ||
		state->action_word_count != word_count || state->action_progress_unknown ||
		!state->action_transaction_closed) {
		return MISTER_RESULT_INVALID_STATE;
	}
	*next_word_index = state->action_next_word_index;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::AdmitPeripheralAction(
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionAction action)
{
	if (!state || action == PeripheralSessionAction::none)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() ||
		registration->peripheral_session_state.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->phase.load() != PeripheralSessionPhase::live ||
		!hardware_transaction_active_ || containment_evidence_pending_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	if (state->action != action || state->action_progress_unknown ||
		!state->action_transaction_closed ||
		state->action_next_word_index > state->action_word_count)
		return MISTER_RESULT_INVALID_STATE;
	state->action_transaction_closed = false;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::RecordPeripheralActionWord(
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionAction action, uint8_t word_index)
{
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() ||
		registration->peripheral_session_state.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->phase.load() != PeripheralSessionPhase::live ||
		!hardware_transaction_active_ || containment_evidence_pending_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms) {
		state->action_progress_unknown = true;
		return MISTER_RESULT_DEADLINE;
	}
	if (state->action != action || state->action_transaction_closed ||
		state->action_progress_unknown || word_index != state->action_next_word_index ||
		word_index >= state->action_word_count) {
		state->action_progress_unknown = true;
		return MISTER_RESULT_INVALID_STATE;
	}
	++state->action_next_word_index;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ClosePeripheralAction(
	const std::shared_ptr<PeripheralSessionState> &state,
	PeripheralSessionAction action)
{
	if (!state) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> registration =
		state->owner_registration.lock();
	const std::shared_ptr<PeripheralSessionState> current =
		peripheral_session_state_.lock();
	if (!registration || !registration->registered || registration->broker != this ||
		current.get() != state.get() ||
		registration->peripheral_session_state.get() != state.get() ||
		!state->view || state->view->registration_.get() != registration.get() ||
		state->phase.load() != PeripheralSessionPhase::live ||
		!hardware_transaction_active_ || containment_evidence_pending_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms ||
		state->action != action || state->action_transaction_closed ||
		state->action_progress_unknown) {
		state->action_progress_unknown = true;
		return clock_.NowMs() >= registration->effective_deadline_ms ?
			MISTER_RESULT_DEADLINE : MISTER_RESULT_INVALID_STATE;
	}
	state->action_transaction_closed = true;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::RecordCoupledRecoveryOperation(
	const RecoveryEpoch &epoch, const OperationLease &lease,
	const CoupledRecoveryReceipt &receipt, Result operation_result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::audio_video ||
		(recovery_requested_resource_flags_ & affected) != affected ||
		receipt.affected_flags != affected ||
		(receipt.observed_flags & receipt.neutral_flags) != 0 ||
		((receipt.observed_flags | receipt.neutral_flags) & ~affected) != 0)
		return MISTER_RESULT_INVALID_STATE;
	if (operation_result == MISTER_RESULT_OK &&
		clock_.NowMs() >= registration->effective_deadline_ms)
		operation_result = MISTER_RESULT_DEADLINE;
	const uint32_t classified = receipt.observed_flags |
		receipt.neutral_flags;
	recovery_observed_resource_flags_ &= ~classified;
	recovery_neutral_resource_flags_ &= ~classified;
	RecordRecoveryPartition(receipt.observed_flags, receipt.neutral_flags);
	LatchRecoveryResult(operation_result, RecoveryFailurePersistence::retryable);
	registration->outcome_recorded = true;
	return operation_result;
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result HardwareBroker::RecordActiveCoreProtocolMutationForTest(
	const OperationLease &lease, ActiveCoreProtocolSession &session,
	uint64_t *mutation_sequence)
{
	if (mutation_sequence == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	*mutation_sequence = 0;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<ProtocolSessionState> current =
		core_protocol_session_state_.lock();
	if (!session.state_ || !session.state_->view || !registration ||
		!registration->registered || registration->broker != this ||
		current.get() != session.state_.get() ||
		registration->core_protocol_session_state.get() != session.state_.get() ||
		session.state_->owner_registration.lock().get() != registration.get() ||
		session.state_->handle_state.load() !=
			ProtocolSessionHandleState::live ||
		registration->operation_kind != OperationKind::core_protocol ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		session.state_->profile != profile_ ||
		session.state_->view->registration_.get() != registration.get() ||
		state_ != State::active || failure_latched_ ||
		!hardware_transaction_active_ || containment_evidence_pending_ ||
		mutation_sequence_ == UINT64_MAX)
		return MISTER_RESULT_INVALID_STATE;
	*mutation_sequence = ++mutation_sequence_;
	return MISTER_RESULT_OK;
}
#endif

Result HardwareBroker::AcquireProcessOperationGuardFor(
	const OperationLease &lease, HardwareBroker &owner,
	OperationKind required_operation_kind,
	const NativeCoreProfile *required_profile,
	std::unique_ptr<ProcessOperationGuard> *guard)
{
	if (guard == nullptr || guard->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (&owner != this) return MISTER_RESULT_INVALID_STATE;
	if (required_operation_kind != OperationKind::scheduler &&
		required_operation_kind != OperationKind::offload &&
		required_operation_kind != OperationKind::input_descriptors &&
		required_operation_kind != OperationKind::save)
		return MISTER_RESULT_INVALID_STATE;

	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this || registration->process_guard_active ||
		registration->operation_kind != required_operation_kind)
		return MISTER_RESULT_INVALID_STATE;
	if (required_profile != nullptr && registration->profile != required_profile)
		return MISTER_RESULT_UNSUPPORTED;
	const bool active_authority =
		registration->authority == LeaseAuthority::active_generation &&
		registration->authority_identity == generation_ && state_ == State::active;
	const bool cleanup_authority =
		registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ &&
		state_ == State::cleanup;
	const bool recovery_authority =
		registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery && !recovery_terminal_neutral_ &&
		IsRecoveryOperation(registration->operation_kind,
			recovery_requested_resource_flags_);
	if (!active_authority && !cleanup_authority && !recovery_authority)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;

	std::unique_ptr<ProcessOperationGuard> admitted(
		new (std::nothrow) ProcessOperationGuard(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	registration->process_guard_active = true;
	*guard = std::move(admitted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ObserveContainment(const CleanupEpoch &epoch,
	const OperationLease &terminal_lease)
{
	return CommitCleanupContainment(epoch, terminal_lease);
}

Result HardwareBroker::BeginRecovery(uint32_t requested_resource_flags,
	uint64_t non_fpga_deadline_ms, uint64_t fpga_deadline_ms,
	std::unique_ptr<RecoveryEpoch> *epoch)
{
	if (!OutputAvailable(epoch)) return MISTER_RESULT_INVALID_ARGUMENT;
	if ((requested_resource_flags & ~MISTER_RESOURCE_V2_KNOWN) != 0)
		return MISTER_RESULT_INVALID_ARGUMENT;

	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::idle || generation_ != 0 ||
		cleanup_registered_ || recovery_registered_ ||
		active_lease_count_ != 0 || invocation_registered_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= non_fpga_deadline_ms ||
		clock_.NowMs() >= fpga_deadline_ms)
		return MISTER_RESULT_DEADLINE;

	const uint64_t identity = FreshNonzeroNonce(cleanup_nonce_source);
	std::unique_ptr<RecoveryEpoch> registered(new (std::nothrow)
		RecoveryEpoch(*this, identity, requested_resource_flags,
			non_fpga_deadline_ms, fpga_deadline_ms, lifetime_));
	if (!registered) return MISTER_RESULT_PLATFORM;
	recovery_identity_ = identity;
	recovery_requested_resource_flags_ = requested_resource_flags;
	recovery_non_fpga_deadline_ms_ = non_fpga_deadline_ms;
	recovery_fpga_deadline_ms_ = fpga_deadline_ms;
	recovery_observed_resource_flags_ = 0;
	recovery_neutral_resource_flags_ = 0;
	recovery_result_ = MISTER_RESULT_OK;
	recovery_observation_active_ = false;
	recovery_terminal_neutral_ = false;
	recovery_audio_shutdown_complete_ = false;
	ClearContainmentReceipt();
	recovery_registered_ = true;
	state_ = State::recovery;
	*epoch = std::move(registered);
	return MISTER_RESULT_OK;
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
Result HardwareBroker::BeginRecoveryOperation(const RecoveryEpoch &epoch,
	OperationKind operation_kind, std::unique_ptr<OperationLease> *lease)
{
	if (!OutputAvailable(lease)) return MISTER_RESULT_INVALID_ARGUMENT;

	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_observation_active_ || recovery_terminal_neutral_ ||
		!IsRecoveryOperation(operation_kind, epoch.requested_resource_flags_))
		return MISTER_RESULT_INVALID_STATE;
	uint64_t absolute_deadline_ms = 0;
	if (!RecoveryDeadline(operation_kind, epoch, &absolute_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	const bool terminal =
		operation_kind == OperationKind::terminal_fpga_cleanup;
	if (active_lease_count_ != 0 || (terminal && terminal_lease_count_ != 0))
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= absolute_deadline_ms) {
		LatchRecoveryResult(MISTER_RESULT_DEADLINE,
			RecoveryFailurePersistence::retryable);
		return MISTER_RESULT_DEADLINE;
	}

	std::shared_ptr<OperationRegistration> registration(
		new (std::nothrow) OperationRegistration(*this,
			LeaseAuthority::recovery_epoch, epoch.identity_, operation_kind,
			absolute_deadline_ms, lifetime_, nullptr));
	if (!registration) {
		LatchRecoveryResult(MISTER_RESULT_PLATFORM,
			RecoveryFailurePersistence::retryable);
		return MISTER_RESULT_PLATFORM;
	}
	std::unique_ptr<OperationLease> admitted(
		new (std::nothrow) OperationLease(registration));
	if (!admitted) {
		LatchRecoveryResult(MISTER_RESULT_PLATFORM,
			RecoveryFailurePersistence::retryable);
		return MISTER_RESULT_PLATFORM;
	}
	registration->registered = true;
	++active_lease_count_;
	if (terminal) {
		++terminal_lease_count_;
		terminal_lease_deadline_ms_ = absolute_deadline_ms;
	}
	*lease = std::move(admitted);
	return MISTER_RESULT_OK;
}
#endif

Result HardwareBroker::BeginRecoveryOperation(const RecoveryEpoch &epoch,
	const OperationInvocation &invocation, OperationKind operation_kind,
	std::unique_ptr<OperationLease> *lease)
{
	uint64_t authority_deadline_ms = 0;
	if (!RecoveryDeadline(operation_kind, epoch, &authority_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
			recovery_observation_active_ || recovery_terminal_neutral_ ||
			!IsRecoveryOperation(operation_kind,
				epoch.requested_resource_flags_))
			return MISTER_RESULT_INVALID_STATE;
	}
	const Result result = BeginInvokedOperation(LeaseAuthority::recovery_epoch,
		epoch.identity_, invocation, operation_kind, authority_deadline_ms,
		nullptr, lease);
	if (result != MISTER_RESULT_OK) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (IsCurrentRecovery(epoch)) LatchRecoveryResult(result,
			RecoveryFailurePersistence::retryable);
	}
	return result;
}

Result HardwareBroker::ContinueInvokedOperation(LeaseAuthority authority,
	uint64_t authority_identity, const OperationInvocation &invocation,
	const OperationLease &lease, uint64_t authority_deadline_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	const std::shared_ptr<OperationRegistration> suspended =
		suspended_registration_.lock();
	const bool cleanup = authority == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup && cleanup_registered_ &&
		authority_identity == cleanup_identity_;
	const bool recovery = authority == LeaseAuthority::recovery_epoch &&
		state_ == State::recovery && recovery_registered_ &&
		authority_identity == recovery_identity_ &&
		!recovery_observation_active_ && !recovery_terminal_neutral_ &&
		IsRecoveryOperation(registration ? registration->operation_kind :
			OperationKind::program_fpga, recovery_requested_resource_flags_);
	if (!IsCurrentInvocation(invocation, authority, authority_identity) ||
		(!cleanup && !recovery) || invocation_outcome_missing_ ||
		!invocation_registration_.expired() || !registration ||
		!registration->registered || registration->broker != this ||
		registration->authority != authority ||
		registration->authority_identity != authority_identity ||
		registration->authority_deadline_ms != authority_deadline_ms ||
		registration->effective_deadline_ms != 0 ||
		registration->invocation_identity != 0 ||
		registration->process_guard_active || active_lease_count_ != 1 ||
		terminal_lease_count_ != 0 || suspended.get() != registration.get())
		return MISTER_RESULT_INVALID_STATE;
	const bool core_abandoned = registration->core_protocol_session_state &&
		registration->core_protocol_session_state->handle_state.load() ==
			ProtocolSessionHandleState::abandoned;
	const bool peripheral_abandoned = registration->peripheral_session_state &&
		registration->peripheral_session_state->phase.load() ==
			PeripheralSessionPhase::abandoned;
	const bool save_retry = registration->operation_kind == OperationKind::save;
	if ((!core_abandoned && !peripheral_abandoned && hardware_transaction_active_) ||
		(!core_abandoned && !peripheral_abandoned && !save_retry))
		return MISTER_RESULT_INVALID_STATE;
	const uint64_t effective_deadline_ms = authority_deadline_ms <
		invocation.callback_deadline_ms_ ? authority_deadline_ms :
		invocation.callback_deadline_ms_;
	if (clock_.NowMs() >= effective_deadline_ms) return MISTER_RESULT_DEADLINE;
	registration->effective_deadline_ms = effective_deadline_ms;
	registration->invocation_identity = invocation.identity_;
	registration->outcome_recorded = false;
	if (registration->peripheral_session_state)
		registration->peripheral_session_state->absolute_deadline_ms =
			effective_deadline_ms;
	suspended_registration_.reset();
	invocation_registration_ = registration;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ContinueCleanupOperation(const CleanupEpoch &epoch,
	const OperationInvocation &invocation, const OperationLease &lease)
{
	uint64_t authority_deadline_ms = 0;
	if (!CleanupDeadline(lease.operation_kind(), epoch, &authority_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	return ContinueInvokedOperation(LeaseAuthority::cleanup_epoch,
		epoch.identity_, invocation, lease, authority_deadline_ms);
}

Result HardwareBroker::ContinueRecoveryOperation(const RecoveryEpoch &epoch,
	const OperationInvocation &invocation, const OperationLease &lease)
{
	uint64_t authority_deadline_ms = 0;
	if (!RecoveryDeadline(lease.operation_kind(), epoch, &authority_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	return ContinueInvokedOperation(LeaseAuthority::recovery_epoch,
		epoch.identity_, invocation, lease, authority_deadline_ms);
}

Result HardwareBroker::FinishInvocation(
	std::unique_ptr<OperationInvocation> &&invocation)
{
	if (!invocation) return MISTER_RESULT_INVALID_ARGUMENT;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!IsCurrentInvocation(*invocation, invocation->authority_,
			invocation->authority_identity_))
			return MISTER_RESULT_INVALID_STATE;
		if (invocation_outcome_missing_)
			return MISTER_RESULT_INVALID_STATE;
		const std::shared_ptr<OperationRegistration> registration =
			invocation_registration_.lock();
		if (registration) {
			const bool core_abandoned = registration->core_protocol_session_state &&
				registration->core_protocol_session_state->handle_state.load() ==
					ProtocolSessionHandleState::abandoned;
			const bool peripheral_abandoned =
				registration->peripheral_session_state &&
				registration->peripheral_session_state->phase.load() ==
					PeripheralSessionPhase::abandoned;
			const bool save_retry =
				registration->operation_kind == OperationKind::save;
			const bool retained_typed = core_abandoned || peripheral_abandoned;
			if (!registration->registered || registration->process_guard_active ||
				active_lease_count_ != 1 || terminal_lease_count_ != 0 ||
				(hardware_transaction_active_ && !retained_typed) ||
				(!core_abandoned && !peripheral_abandoned && !save_retry) ||
				!registration->outcome_recorded)
				return MISTER_RESULT_INVALID_STATE;
			registration->effective_deadline_ms = 0;
			registration->invocation_identity = 0;
			if (registration->peripheral_session_state)
				registration->peripheral_session_state->absolute_deadline_ms = 0;
			suspended_registration_ = registration;
		} else if (active_lease_count_ != 0 || terminal_lease_count_ != 0) {
			const std::shared_ptr<OperationRegistration> suspended =
				suspended_registration_.lock();
			const bool core_abandoned = suspended &&
				suspended->core_protocol_session_state &&
				suspended->core_protocol_session_state->handle_state.load() ==
					ProtocolSessionHandleState::abandoned;
			const bool peripheral_abandoned = suspended &&
				suspended->peripheral_session_state &&
				suspended->peripheral_session_state->phase.load() ==
					PeripheralSessionPhase::abandoned;
			const bool save_retry = suspended &&
				suspended->operation_kind == OperationKind::save;
			if (active_lease_count_ != 1 || terminal_lease_count_ != 0 ||
				!suspended || !suspended->registered ||
				suspended->effective_deadline_ms != 0 ||
				suspended->invocation_identity != 0 ||
				!suspended->outcome_recorded ||
				(!core_abandoned && !peripheral_abandoned && !save_retry))
				return MISTER_RESULT_INVALID_STATE;
		}
		invocation_registration_.reset();
		invocation_registered_ = false;
		invocation_outcome_missing_ = false;
		invocation_authority_identity_ = 0;
		invocation_identity_ = 0;
		invocation_callback_deadline_ms_ = 0;
		invocation->registered_ = false;
	}
	invocation.reset();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::RecordOperationOutcome(
	const OperationInvocation &invocation, const OperationLease &lease,
	Result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this || registration->outcome_recorded ||
		registration->invocation_identity != invocation.identity_ ||
		!IsCurrentInvocation(invocation, registration->authority,
			registration->authority_identity) ||
		registration->process_guard_active)
		return MISTER_RESULT_INVALID_STATE;
	registration->outcome_recorded = true;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::FinishRecovery(std::unique_ptr<RecoveryEpoch> &&epoch,
	MisterRecoveryObservationV2 *observation)
{
	if (!epoch || observation == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ != State::recovery || !IsCurrentRecovery(*epoch) ||
			active_lease_count_ != 0 || recovery_observation_active_ ||
			invocation_registered_)
			return MISTER_RESULT_INVALID_STATE;
		if (recovery_terminal_neutral_ && !ReceiptMatchesRecovery(*epoch))
			return MISTER_RESULT_INVALID_STATE;
		observation->observed_resource_flags =
			recovery_observed_resource_flags_ & epoch->requested_resource_flags_;
		observation->neutral_resource_flags =
			recovery_neutral_resource_flags_ & epoch->requested_resource_flags_;
		const bool all_neutral =
			observation->observed_resource_flags == 0 &&
			observation->neutral_resource_flags ==
				epoch->requested_resource_flags_;
		const uint32_t outstanding = epoch->requested_resource_flags_ &
			~observation->neutral_resource_flags;
		const uint32_t fpga_group = MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
		const bool expired = ((outstanding & fpga_group) != 0 &&
			clock_.NowMs() >= recovery_fpga_deadline_ms_) ||
			((outstanding & ~fpga_group) != 0 &&
			 clock_.NowMs() >= recovery_non_fpga_deadline_ms_);
		const Result result = recovery_result_ != MISTER_RESULT_OK ?
			recovery_result_ : (expired ? MISTER_RESULT_DEADLINE :
				(all_neutral ? MISTER_RESULT_OK :
				 MISTER_RESULT_CLEANUP_INCOMPLETE));

		epoch->registered_ = false;
		recovery_registered_ = false;
		recovery_identity_ = 0;
		recovery_requested_resource_flags_ = 0;
		recovery_non_fpga_deadline_ms_ = 0;
		recovery_fpga_deadline_ms_ = 0;
		recovery_observed_resource_flags_ = 0;
		recovery_neutral_resource_flags_ = 0;
		recovery_result_ = MISTER_RESULT_OK;
		recovery_terminal_neutral_ = false;
		recovery_audio_shutdown_complete_ = false;
		terminal_lease_deadline_ms_ = 0;
		ClearContainmentReceipt();
		state_ = State::idle;
		epoch.reset();
		return result;
	}
}

Result HardwareBroker::Leave(PlatformGenerationId generation,
	std::unique_ptr<CleanupEpoch> &&epoch)
{
	if (!epoch) return MISTER_RESULT_INVALID_ARGUMENT;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (generation == 0 || generation != generation_ ||
			state_ != State::terminal_neutral ||
			!containment_receipt_current_ || active_lease_count_ != 0 ||
			invocation_registered_ ||
			!IsCurrentCleanup(*epoch) || !ReceiptMatchesCleanup(*epoch)) {
			return MISTER_RESULT_INVALID_STATE;
		}

		epoch->registered_ = false;
		cleanup_registered_ = false;
		ClearContainmentReceipt();
		generation_ = 0;
		cleanup_identity_ = 0;
		cleanup_non_fpga_deadline_ms_ = 0;
		cleanup_fpga_deadline_ms_ = 0;
		terminal_lease_deadline_ms_ = 0;
		active_lease_count_ = 0;
		terminal_lease_count_ = 0;
		cleanup_ever_started_ = false;
		quiesce_complete_ = false;
		quiesce_call_active_ = false;
		failure_latched_ = false;
		ClearCoreProtocolFailureReceipt();
		profile_ = nullptr;
		state_ = State::idle;
	}

	epoch.reset();
	return MISTER_RESULT_OK;
}

void HardwareBroker::ReleaseOperation(OperationRegistration &registration)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!registration.registered || registration.broker != this) return;
	if (containment_evidence_pending_ && receipt_registration_ == &registration)
		ClearContainmentReceipt();
	const bool current_authority =
		(registration.authority == LeaseAuthority::active_generation &&
			registration.authority_identity == generation_) ||
		(registration.authority == LeaseAuthority::cleanup_epoch &&
			registration.authority_identity == cleanup_identity_) ||
		(registration.authority == LeaseAuthority::recovery_epoch &&
			registration.authority_identity == recovery_identity_);
	if (!current_authority) {
		registration.registered = false;
		return;
	}

	registration.registered = false;
	if (registration.invocation_identity != 0 &&
		!registration.outcome_recorded && invocation_registered_ &&
		registration.invocation_identity == invocation_identity_)
		invocation_outcome_missing_ = true;
	const std::shared_ptr<OperationRegistration> invoked =
		invocation_registration_.lock();
	if (invoked.get() == &registration) invocation_registration_.reset();
	const std::shared_ptr<OperationRegistration> suspended =
		suspended_registration_.lock();
	if (suspended.get() == &registration) suspended_registration_.reset();
	if (active_lease_count_ != 0) --active_lease_count_;
	if (registration.operation_kind == OperationKind::terminal_fpga_cleanup &&
		terminal_lease_count_ != 0) {
		--terminal_lease_count_;
		if (terminal_lease_count_ == 0) terminal_lease_deadline_ms_ = 0;
	}
	// A broker-atomic active A/V failure latches quiescence before the owning
	// operation lease is released. Once that final lease drops, cleanup may
	// begin without reopening the failed registration.
	if (failure_latched_ && state_ == State::quiescing &&
		active_lease_count_ == 0)
		quiesce_complete_ = true;
	lease_released_.notify_all();
}

void HardwareBroker::UnregisterInvocation(OperationInvocation &invocation)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!invocation.registered_ || invocation.broker_ != this) return;
	const bool current = IsCurrentInvocation(invocation,
		invocation.authority_, invocation.authority_identity_);
	invocation.registered_ = false;
	if (!current)
		return;
	const std::shared_ptr<OperationRegistration> registration =
		invocation_registration_.lock();
	if (registration && registration->registered) {
		registration->effective_deadline_ms = 0;
		registration->invocation_identity = 0;
		if (registration->peripheral_session_state)
			registration->peripheral_session_state->absolute_deadline_ms = 0;
		suspended_registration_ = registration;
	}
	invocation_registration_.reset();
	invocation_registered_ = false;
	invocation_outcome_missing_ = false;
	invocation_authority_identity_ = 0;
	invocation_identity_ = 0;
	invocation_callback_deadline_ms_ = 0;
}

void HardwareBroker::ReleaseHardwareLeaseView(HardwareLeaseView &view)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!view.registration_ || view.registration_->broker != this) return;
	hardware_transaction_active_ = false;
	lease_released_.notify_all();
}

void HardwareBroker::ReleaseProcessOperationGuard(ProcessOperationGuard &guard)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!guard.registration_ || guard.registration_->broker != this) return;
	guard.registration_->process_guard_active = false;
	lease_released_.notify_all();
}

void HardwareBroker::UnregisterCleanup(CleanupEpoch &epoch)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!epoch.registered_ || epoch.broker_ != this) return;

	epoch.registered_ = false;
	if (cleanup_registered_ && cleanup_identity_ == epoch.identity_ &&
		generation_ == epoch.generation_) {
		cleanup_registered_ = false;
		ClearContainmentReceipt();
		if (state_ == State::terminal_neutral) state_ = State::cleanup;
	}
}

void HardwareBroker::UnregisterRecovery(RecoveryEpoch &epoch)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!epoch.registered_ || epoch.broker_ != this) return;

	epoch.registered_ = false;
	if (recovery_registered_ && recovery_identity_ == epoch.identity_) {
		// Closing the owning token closes future admission, but never revokes
		// an operation already fenced to this recovery identity. If work is
		// live, retain the conservative recovery state until that lease drains;
		// with no work, unregistering can safely return to idle.
		recovery_registered_ = false;
		ClearContainmentReceipt();
		if (active_lease_count_ == 0 && !recovery_observation_active_) {
			recovery_identity_ = 0;
			recovery_requested_resource_flags_ = 0;
			recovery_non_fpga_deadline_ms_ = 0;
			recovery_fpga_deadline_ms_ = 0;
			recovery_observed_resource_flags_ = 0;
			recovery_neutral_resource_flags_ = 0;
			recovery_result_ = MISTER_RESULT_OK;
			recovery_terminal_neutral_ = false;
			if (state_ == State::recovery) state_ = State::idle;
		}
	}
}

bool HardwareBroker::IsCurrentCleanup(const CleanupEpoch &epoch) const
{
	return epoch.registered_ && epoch.broker_ == this &&
		cleanup_registered_ && epoch.generation_ == generation_ &&
		epoch.identity_ == cleanup_identity_ &&
		epoch.non_fpga_deadline_ms_ == cleanup_non_fpga_deadline_ms_ &&
		epoch.fpga_deadline_ms_ == cleanup_fpga_deadline_ms_;
}

bool HardwareBroker::IsCurrentRecovery(const RecoveryEpoch &epoch) const
{
	return epoch.registered_ && epoch.broker_ == this &&
		recovery_registered_ && epoch.identity_ == recovery_identity_ &&
		epoch.requested_resource_flags_ == recovery_requested_resource_flags_ &&
		epoch.non_fpga_deadline_ms_ == recovery_non_fpga_deadline_ms_ &&
		epoch.fpga_deadline_ms_ == recovery_fpga_deadline_ms_;
}

uint64_t HardwareBroker::RecordMutation(const HardwareLeaseView &view)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!registration || !registration->registered ||
		registration->broker != this)
		return 0;
	const bool active_authority =
		registration->authority == LeaseAuthority::active_generation &&
		registration->authority_identity == generation_ &&
		(state_ == State::active || state_ == State::quiescing);
	const bool cleanup_authority =
		registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ &&
		state_ == State::cleanup;
	const bool recovery_authority =
		registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery &&
		!recovery_terminal_neutral_ &&
		IsRecoveryOperation(registration->operation_kind,
			recovery_requested_resource_flags_);
	if (!active_authority && !cleanup_authority && !recovery_authority)
		return 0;
	if (containment_evidence_pending_) return 0;
	if (mutation_sequence_ == UINT64_MAX) return 0;
	return ++mutation_sequence_;
}

uint64_t HardwareBroker::CurrentMutationSequence(
	const HardwareLeaseView &view)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || !registration ||
		!registration->registered || registration->broker != this)
		return 0;
	return mutation_sequence_;
}

Result HardwareBroker::RecordFpgaProgrammingMutation(
	const HardwareLeaseView &view, size_t accepted_bytes,
	uint64_t *mutation_sequence)
{
	if (mutation_sequence == nullptr || accepted_bytes == 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	*mutation_sequence = 0;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::program_fpga ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		(state_ != State::active && state_ != State::quiescing))
		return MISTER_RESULT_INVALID_STATE;
	if (mutation_sequence_ == UINT64_MAX) return MISTER_RESULT_PLATFORM;
	*mutation_sequence = ++mutation_sequence_;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::AuthorizeFpgaProgrammingProfile(
	const HardwareLeaseView &view, const NativeCoreProfile &profile)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		!registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::program_fpga ||
		registration->authority != LeaseAuthority::active_generation ||
		registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		registration->profile != &profile ||
		(state_ != State::active && state_ != State::quiescing))
		return MISTER_RESULT_INVALID_STATE;
	return clock_.NowMs() >= registration->effective_deadline_ms ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

Result HardwareBroker::MintBridgeActivationAuthority(
	const HardwareLeaseView &view, uint64_t bridge_mutation_sequence,
	std::unique_ptr<NativeBridgeActivationAuthority> *authority)
{
	if (authority == nullptr || authority->get() != nullptr ||
		bridge_mutation_sequence == 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		failure_latched_ || !registration || !registration->registered ||
		registration->broker != this ||
		registration->operation_kind != OperationKind::program_fpga ||
		registration->authority != LeaseAuthority::active_generation ||
		generation_ == 0 || registration->authority_identity != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		state_ != State::active || mutation_sequence_ == UINT64_MAX ||
		bridge_mutation_sequence != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	std::unique_ptr<NativeBridgeActivationAuthority> minted(
		new (std::nothrow) NativeBridgeActivationAuthority(*this, lifetime_,
			generation_, profile_, bridge_mutation_sequence));
	if (!minted) return MISTER_RESULT_PLATFORM;
	*authority = std::move(minted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ValidateBridgeActivationAuthority(
	const HardwareLeaseView &view,
	const NativeBridgeActivationAuthority &authority)
{
	const std::shared_ptr<BrokerLifetime> authority_lifetime =
		authority.lifetime_.lock();
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		failure_latched_ || !registration || !registration->registered ||
		registration->broker != this || authority.broker_ != this ||
		!authority_lifetime || authority_lifetime.get() != lifetime_.get() ||
		registration->operation_kind != OperationKind::input ||
		registration->authority != LeaseAuthority::active_generation ||
		generation_ == 0 || registration->authority_identity != generation_ ||
		authority.generation_ != generation_ ||
		registration->profile == nullptr || registration->profile != profile_ ||
		authority.profile_ != profile_ ||
		state_ != State::active || authority.bridge_mutation_sequence_ == 0 ||
		mutation_sequence_ == UINT64_MAX ||
		authority.bridge_mutation_sequence_ > mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	return clock_.NowMs() >= registration->effective_deadline_ms ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
void HardwareBroker::SetBridgeMutationSequenceForTest(uint64_t sequence)
{
	std::lock_guard<std::mutex> lock(mutex_);
	mutation_sequence_ = sequence;
}
#endif

Result HardwareBroker::ValidateCleanupContainmentAuthority(
	const CleanupEpoch &epoch, const OperationLease &terminal_lease)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (state_ != State::cleanup || !IsCurrentCleanup(epoch) ||
		containment_evidence_pending_ || containment_receipt_current_ ||
		!registration || !registration->registered ||
		registration->broker != this ||
		registration->authority != LeaseAuthority::cleanup_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1)
		return MISTER_RESULT_INVALID_STATE;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ValidateRecoveryContainmentAuthority(
	const RecoveryEpoch &epoch, const OperationLease &terminal_lease)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_ || containment_evidence_pending_ ||
		containment_receipt_current_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1)
		return MISTER_RESULT_INVALID_STATE;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ValidateContainmentBoundary(
	const HardwareLeaseView &view)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup)
		return MISTER_RESULT_INVALID_STATE;
	const bool cleanup = registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ &&
		state_ == State::cleanup && cleanup_registered_;
	const bool recovery = registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery && recovery_registered_ &&
		!recovery_terminal_neutral_;
	if (!cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	return clock_.NowMs() >= registration->effective_deadline_ms ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

Result HardwareBroker::MintContainmentResumeKey(
	const HardwareLeaseView &view,
	uint32_t core_gpo, uint32_t interface_module, uint32_t sdr_port_control,
	uint32_t bridge_reset, uint32_t remap, uint32_t manager_control,
	uint32_t manager_mode, uint64_t manager_mutation_sequence,
	std::unique_ptr<ContainmentResumeKey> *key)
{
	if (key == nullptr || key->get() != nullptr)
		return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		view.registration_;
	if (!hardware_transaction_active_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1)
		return MISTER_RESULT_INVALID_STATE;
	const bool cleanup =
		registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ &&
		state_ == State::cleanup && cleanup_registered_;
	const bool recovery =
		registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery && recovery_registered_ &&
		!recovery_terminal_neutral_;
	if (!cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	std::unique_ptr<ContainmentResumeKey> minted(
		new (std::nothrow) ContainmentResumeKey(*this,
			registration->authority, registration->authority_identity,
			cleanup ? generation_ : 0, mutation_sequence_, core_gpo,
			interface_module, sdr_port_control, bridge_reset, remap,
			manager_control, manager_mode, manager_mutation_sequence, lifetime_));
	if (!minted) return MISTER_RESULT_PLATFORM;
	*key = std::move(minted);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::ValidateContainmentResumeKey(
	const ContainmentResumeKey &key,
	const OperationLease &terminal_lease, uint32_t core_gpo,
	uint32_t interface_module, uint32_t sdr_port_control,
	uint32_t bridge_reset, uint32_t remap, uint32_t manager_control,
	uint32_t manager_mode, uint64_t manager_mutation_sequence)
{
	const std::shared_ptr<BrokerLifetime> key_lifetime = key.lifetime_.lock();
	if (!key_lifetime || key.broker_ != this ||
		key_lifetime.get() != lifetime_.get())
		return MISTER_RESULT_INVALID_STATE;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (!hardware_transaction_active_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->lifetime.get() != key_lifetime.get() ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		registration->authority != key.authority_ ||
		registration->authority_identity != key.authority_identity_ ||
		mutation_sequence_ != key.mutation_sequence_ ||
		core_gpo != key.core_gpo_ || interface_module != key.interface_module_ ||
		sdr_port_control != key.sdr_port_control_ || bridge_reset != key.bridge_reset_ ||
		remap != key.remap_ || manager_control != key.manager_control_ ||
		manager_mode != key.manager_mode_ ||
		manager_mutation_sequence != key.manager_mutation_sequence_ ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1)
		return MISTER_RESULT_INVALID_STATE;
	const bool cleanup = key.authority_ == LeaseAuthority::cleanup_epoch &&
		state_ == State::cleanup && cleanup_registered_ &&
		cleanup_identity_ == key.authority_identity_ &&
		generation_ == key.generation_;
	const bool recovery = key.authority_ == LeaseAuthority::recovery_epoch &&
		state_ == State::recovery && recovery_registered_ &&
		recovery_identity_ == key.authority_identity_ &&
		key.generation_ == 0 && !recovery_terminal_neutral_;
	return cleanup || recovery ? MISTER_RESULT_OK :
		MISTER_RESULT_INVALID_STATE;
}

Result HardwareBroker::StageContainmentEvidence(
	const OperationLease &terminal_lease, uint32_t core_gpo,
	uint32_t interface_module, uint32_t sdr_port_control,
	uint32_t bridge_reset, uint32_t remap, uint32_t manager_control,
	uint32_t manager_mode, bool manager_neutral_observed,
	uint64_t manager_neutral_mutation_sequence, bool mappings_released)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (!hardware_transaction_active_ || containment_evidence_pending_ ||
		containment_receipt_current_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1)
		return MISTER_RESULT_INVALID_STATE;
	const bool cleanup = registration->authority == LeaseAuthority::cleanup_epoch &&
		registration->authority_identity == cleanup_identity_ &&
		state_ == State::cleanup && cleanup_registered_;
	const bool recovery = registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery && recovery_registered_ &&
		!recovery_terminal_neutral_;
	if (!cleanup && !recovery) return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	if ((core_gpo & 0xc0000000u) != 0x40000000u ||
		interface_module != 0 || sdr_port_control != 0 || bridge_reset != 7 ||
		remap != 1 || (manager_control & 0x107u) != 0x2u ||
		manager_mode > 4u || !manager_neutral_observed ||
		manager_neutral_mutation_sequence == 0 ||
		manager_neutral_mutation_sequence == UINT64_MAX ||
		manager_neutral_mutation_sequence + 1 != mutation_sequence_ ||
		!mappings_released)
		return MISTER_RESULT_CLEANUP_INCOMPLETE;
	containment_evidence_pending_ = true;
	receipt_authority_ = registration->authority;
	receipt_authority_identity_ = registration->authority_identity;
	receipt_generation_ = cleanup ? generation_ : 0;
	receipt_requested_resource_flags_ = recovery ?
		recovery_requested_resource_flags_ : 0;
	receipt_core_gpo_ = core_gpo;
	receipt_interface_module_ = interface_module;
	receipt_sdr_port_control_ = sdr_port_control;
	receipt_bridge_reset_ = bridge_reset;
	receipt_remap_ = remap;
	receipt_manager_control_ = manager_control;
	receipt_manager_mode_ = manager_mode;
	receipt_manager_neutral_observed_ = manager_neutral_observed;
	receipt_manager_neutral_mutation_sequence_ =
		manager_neutral_mutation_sequence;
	receipt_mutation_sequence_ = mutation_sequence_;
	receipt_mappings_released_ = mappings_released;
	receipt_registration_ = registration.get();
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CommitCleanupContainment(const CleanupEpoch &epoch,
	const OperationLease &terminal_lease)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (state_ != State::cleanup || !IsCurrentCleanup(epoch) ||
		!registration || !registration->registered || registration->broker != this ||
		registration->authority != LeaseAuthority::cleanup_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1)
		return MISTER_RESULT_INVALID_STATE;
	if (!containment_evidence_pending_) return MISTER_RESULT_UNSUPPORTED;
	if (receipt_authority_ != LeaseAuthority::cleanup_epoch ||
		receipt_authority_identity_ != epoch.identity_ ||
		receipt_generation_ != epoch.generation_ ||
		receipt_requested_resource_flags_ != 0 ||
		receipt_registration_ != registration.get() ||
		receipt_mutation_sequence_ != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	containment_evidence_pending_ = false;
	containment_receipt_current_ = true;
	state_ = State::terminal_neutral;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::PromoteCleanupAudioWithContainment(
	const CleanupEpoch &epoch, const OperationLease &terminal_lease)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (state_ != State::terminal_neutral || !IsCurrentCleanup(epoch) ||
		!cleanup_audio_shutdown_complete_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->authority != LeaseAuthority::cleanup_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1 ||
		!ReceiptMatchesCleanup(epoch))
		return MISTER_RESULT_INVALID_STATE;
	// Audio local shutdown expires at the non-FPGA deadline even if terminal
	// containment itself remains admissible until the longer FPGA deadline.
	if (clock_.NowMs() >= cleanup_non_fpga_deadline_ms_ ||
		clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CommitRecoveryContainment(const RecoveryEpoch &epoch,
	const OperationLease &terminal_lease)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_ || !containment_evidence_pending_ ||
		!registration || !registration->registered ||
		registration->broker != this ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1 ||
		receipt_authority_ != LeaseAuthority::recovery_epoch ||
		receipt_authority_identity_ != epoch.identity_ ||
		receipt_generation_ != 0 ||
		receipt_requested_resource_flags_ != epoch.requested_resource_flags_ ||
		receipt_registration_ != registration.get() ||
		receipt_mutation_sequence_ != mutation_sequence_)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->effective_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	containment_evidence_pending_ = false;
	containment_receipt_current_ = true;
	recovery_terminal_neutral_ = true;
	uint32_t neutral = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	// A mute ACK is only local shutdown. Promote AUDIO through this exact
	// terminal receipt while both immutable recovery deadlines remain live.
	if ((recovery_requested_resource_flags_ & MISTER_RESOURCE_NATIVE_AUDIO) != 0 &&
		recovery_audio_shutdown_complete_ &&
		clock_.NowMs() < recovery_non_fpga_deadline_ms_ &&
		clock_.NowMs() < registration->effective_deadline_ms)
		neutral |= MISTER_RESOURCE_NATIVE_AUDIO;
	RecordRecoveryPartition(0, neutral);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::BeginRecoveryObservation(const RecoveryEpoch &epoch,
	const OperationInvocation &invocation)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		!IsCurrentInvocation(invocation, LeaseAuthority::recovery_epoch,
			epoch.identity_) || !invocation_registration_.expired() ||
		recovery_observation_active_ || recovery_terminal_neutral_ ||
		active_lease_count_ != 0)
		return MISTER_RESULT_INVALID_STATE;
	const uint64_t deadline = epoch.fpga_deadline_ms_ <
		invocation.callback_deadline_ms_ ? epoch.fpga_deadline_ms_ :
		invocation.callback_deadline_ms_;
	if (clock_.NowMs() >= deadline) {
		LatchRecoveryResult(MISTER_RESULT_DEADLINE,
			RecoveryFailurePersistence::retryable);
		return MISTER_RESULT_DEADLINE;
	}
	recovery_observation_active_ = true;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CheckRecoveryObservationDeadline(
	const RecoveryEpoch &epoch, const OperationInvocation &invocation)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recovery_observation_active_ || state_ != State::recovery ||
		!IsCurrentRecovery(epoch) ||
		!IsCurrentInvocation(invocation, LeaseAuthority::recovery_epoch,
			epoch.identity_))
		return MISTER_RESULT_INVALID_STATE;
	const uint64_t deadline = epoch.fpga_deadline_ms_ <
		invocation.callback_deadline_ms_ ? epoch.fpga_deadline_ms_ :
		invocation.callback_deadline_ms_;
	return clock_.NowMs() >= deadline ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

Result HardwareBroker::EndRecoveryObservation(const RecoveryEpoch &epoch,
	const OperationInvocation &invocation,
	uint32_t observed_resource_flags, uint32_t neutral_resource_flags,
	Result result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recovery_observation_active_ || state_ != State::recovery ||
		!IsCurrentRecovery(epoch) ||
		!IsCurrentInvocation(invocation, LeaseAuthority::recovery_epoch,
			epoch.identity_))
		return MISTER_RESULT_INVALID_STATE;
	recovery_observation_active_ = false;
	observed_resource_flags &= epoch.requested_resource_flags_;
	neutral_resource_flags &= epoch.requested_resource_flags_;
	if ((observed_resource_flags & neutral_resource_flags) != 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	const uint64_t deadline = epoch.fpga_deadline_ms_ <
		invocation.callback_deadline_ms_ ? epoch.fpga_deadline_ms_ :
		invocation.callback_deadline_ms_;
	if (result == MISTER_RESULT_OK && clock_.NowMs() >= deadline)
		result = MISTER_RESULT_DEADLINE;
	const uint32_t observed_bundle = epoch.requested_resource_flags_ &
		(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		 MISTER_RESOURCE_CORE_PROTOCOL);
	recovery_observed_resource_flags_ &= ~observed_bundle;
	recovery_neutral_resource_flags_ &= ~observed_bundle;
	RecordRecoveryPartition(observed_resource_flags, neutral_resource_flags);
	LatchRecoveryResult(result, RecoveryFailurePersistence::retryable);
	return result;
}

Result HardwareBroker::RecoveryObservationDeadline(const RecoveryEpoch &epoch,
	const OperationInvocation &invocation, uint64_t *deadline_ms) const
{
	if (deadline_ms == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!IsCurrentRecovery(epoch) ||
		!IsCurrentInvocation(invocation, LeaseAuthority::recovery_epoch,
			epoch.identity_))
		return MISTER_RESULT_INVALID_STATE;
	*deadline_ms = epoch.fpga_deadline_ms_ < invocation.callback_deadline_ms_ ?
		epoch.fpga_deadline_ms_ : invocation.callback_deadline_ms_;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::RecordRecoveryContainmentObservation(
	const RecoveryEpoch &epoch, uint32_t observed_resource_flags,
	uint32_t neutral_resource_flags, Result result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_)
		return MISTER_RESULT_INVALID_STATE;
	observed_resource_flags &= epoch.requested_resource_flags_;
	neutral_resource_flags &= epoch.requested_resource_flags_;
	if ((observed_resource_flags & neutral_resource_flags) != 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	const uint32_t bundle = epoch.requested_resource_flags_ &
		(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		 MISTER_RESOURCE_CORE_PROTOCOL);
	recovery_observed_resource_flags_ &= ~bundle;
	recovery_neutral_resource_flags_ &= ~bundle;
	RecordRecoveryPartition(observed_resource_flags, neutral_resource_flags);
	LatchRecoveryResult(result, RecoveryFailurePersistence::retryable);
	return result;
}

Result HardwareBroker::RecordRecoveryOperation(const RecoveryEpoch &epoch,
	const OperationLease &lease, RecoveryResourceState resource_state,
	Result result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != epoch.identity_)
		return MISTER_RESULT_INVALID_STATE;
	const uint32_t bit = RecoveryResourceForOperation(
		registration->operation_kind);
	if (bit == 0 || (bit & epoch.requested_resource_flags_) == 0)
		return MISTER_RESULT_INVALID_STATE;
	if (result == MISTER_RESULT_OK &&
		clock_.NowMs() >= registration->effective_deadline_ms)
		result = MISTER_RESULT_DEADLINE;
	uint32_t observed = 0;
	uint32_t neutral = 0;
	if (resource_state == RecoveryResourceState::observed_non_neutral)
		observed = bit;
	else if (resource_state == RecoveryResourceState::neutral)
		neutral = bit;
	recovery_observed_resource_flags_ &= ~bit;
	recovery_neutral_resource_flags_ &= ~bit;
	RecordRecoveryPartition(observed, neutral);
	LatchRecoveryResult(result, RecoveryFailurePersistence::retryable);
	registration->outcome_recorded = true;
	return result;
}

Result HardwareBroker::RecordRecoveryFailure(const RecoveryEpoch &epoch,
	const OperationLease &lease, Result result,
	RecoveryFailurePersistence persistence)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		lease.registration_;
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_ || !registration ||
		!registration->registered || registration->broker != this ||
		registration->authority != LeaseAuthority::recovery_epoch ||
		registration->authority_identity != epoch.identity_)
		return MISTER_RESULT_INVALID_STATE;
	if (result == MISTER_RESULT_DEADLINE &&
		clock_.NowMs() >= registration->authority_deadline_ms)
		recovery_result_ = MISTER_RESULT_DEADLINE;
	else
		LatchRecoveryResult(result, persistence);
	return result;
}

Result HardwareBroker::SnapshotRecovery(const RecoveryEpoch &epoch,
	MisterRecoveryObservationV2 *observation)
{
	if (observation == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch))
		return MISTER_RESULT_INVALID_STATE;
	observation->observed_resource_flags =
		recovery_observed_resource_flags_ & epoch.requested_resource_flags_;
	observation->neutral_resource_flags =
		recovery_neutral_resource_flags_ & epoch.requested_resource_flags_;
	if (recovery_result_ != MISTER_RESULT_OK) return recovery_result_;
	const uint32_t outstanding = epoch.requested_resource_flags_ &
		~observation->neutral_resource_flags;
	const uint32_t fpga_group = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	if (((outstanding & fpga_group) != 0 &&
		 clock_.NowMs() >= recovery_fpga_deadline_ms_) ||
		((outstanding & ~fpga_group) != 0 &&
		 clock_.NowMs() >= recovery_non_fpga_deadline_ms_)) {
		// Unlike a shorter invocation deadline, expiry of an immutable epoch
		// group deadline is terminal for this recovery attempt. Retain it so a
		// later callback cannot revive the same attempt.
		recovery_result_ = MISTER_RESULT_DEADLINE;
		return MISTER_RESULT_DEADLINE;
	}
	return observation->observed_resource_flags == 0 &&
		observation->neutral_resource_flags == epoch.requested_resource_flags_ ?
		MISTER_RESULT_OK : MISTER_RESULT_CLEANUP_INCOMPLETE;
}

Result HardwareBroker::ValidateRecoveryRequestedFlags(
	const RecoveryEpoch &epoch, uint32_t callback_requested_flags) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch))
		return MISTER_RESULT_INVALID_STATE;
	return callback_requested_flags == recovery_requested_resource_flags_ ?
		MISTER_RESULT_OK :
		MISTER_RESULT_INVALID_STATE;
}

bool HardwareBroker::CanFinishRecovery(const RecoveryEpoch &epoch) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return state_ == State::recovery && IsCurrentRecovery(epoch) &&
		active_lease_count_ == 0 && !recovery_observation_active_ &&
		recovery_result_ == MISTER_RESULT_OK &&
		recovery_observed_resource_flags_ == 0 &&
		recovery_neutral_resource_flags_ ==
			epoch.requested_resource_flags_;
}

uint32_t HardwareBroker::RecoveryResourceForOperation(
	OperationKind operation_kind)
{
	switch (operation_kind) {
	case OperationKind::input_descriptors: return MISTER_RESOURCE_CORE_INPUT;
	case OperationKind::save: return MISTER_RESOURCE_SAVES;
	case OperationKind::audio: return MISTER_RESOURCE_NATIVE_AUDIO;
	case OperationKind::video: return MISTER_RESOURCE_NATIVE_VIDEO;
	case OperationKind::audio_video:
		return MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO;
	case OperationKind::content: return MISTER_RESOURCE_CONTENT;
	case OperationKind::core_protocol: return MISTER_RESOURCE_CORE_PROTOCOL;
	case OperationKind::terminal_fpga_cleanup:
		return MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
			MISTER_RESOURCE_CORE_PROTOCOL;
	case OperationKind::program_fpga:
	case OperationKind::input:
	case OperationKind::scheduler:
	case OperationKind::offload:
		return 0;
	}
	return 0;
}

void HardwareBroker::RecordRecoveryPartition(uint32_t observed_resource_flags,
	uint32_t neutral_resource_flags)
{
	observed_resource_flags &= recovery_requested_resource_flags_;
	neutral_resource_flags &= recovery_requested_resource_flags_;
	recovery_observed_resource_flags_ |= observed_resource_flags;
	recovery_neutral_resource_flags_ |= neutral_resource_flags;
	recovery_observed_resource_flags_ &= ~recovery_neutral_resource_flags_;
}

void HardwareBroker::LatchRecoveryResult(Result result,
	RecoveryFailurePersistence persistence)
{
	if (result == MISTER_RESULT_OK || recovery_result_ != MISTER_RESULT_OK)
		return;
	// Per-operation DEADLINE, CLEANUP_INCOMPLETE, and PLATFORM are current-call
	// results. Their truthful partition and retained residue remain retryable.
	// Only an explicitly reviewed non-retryable PLATFORM classification may
	// poison the attempt; permanent deadline expiry is latched at its immutable
	// authority comparison sites instead.
	if (persistence == RecoveryFailurePersistence::terminal &&
		result == MISTER_RESULT_PLATFORM)
		recovery_result_ = MISTER_RESULT_PLATFORM;
}

void HardwareBroker::ClearContainmentReceipt()
{
	containment_receipt_current_ = false;
	containment_evidence_pending_ = false;
	receipt_authority_ = LeaseAuthority::active_generation;
	receipt_authority_identity_ = 0;
	receipt_generation_ = 0;
	receipt_requested_resource_flags_ = 0;
	receipt_core_gpo_ = 0;
	receipt_interface_module_ = 0;
	receipt_sdr_port_control_ = 0;
	receipt_bridge_reset_ = 0;
	receipt_remap_ = 0;
	receipt_manager_control_ = 0;
	receipt_manager_mode_ = 0;
	receipt_manager_neutral_observed_ = false;
	receipt_manager_neutral_mutation_sequence_ = 0;
	receipt_mutation_sequence_ = 0;
	receipt_mappings_released_ = false;
	receipt_registration_ = nullptr;
}

void HardwareBroker::ClearCoreProtocolFailureReceipt()
{
	const CoreProtocolResidue residue = {
		false, false, false, false, false, false, false, 0};
	const ProtocolMappingReleaseReceipt mapping_release = {
		MISTER_RESULT_OK, false, false, false, false, false, 0};
	core_protocol_failure_receipt_current_ = false;
	core_protocol_failure_receipt_ = {
		MISTER_RESULT_OK, residue, mapping_release, 0};
}

void HardwareBroker::ConsumeCoreProtocolSessionState(
	OperationRegistration &registration,
	const std::shared_ptr<ProtocolSessionState> &state)
{
	if (state && state->view) {
		state->view->registration_.reset();
		state->view.reset();
	}
	registration.core_protocol_session_state.reset();
	core_protocol_session_state_.reset();
	hardware_transaction_active_ = false;
}

bool HardwareBroker::ReceiptMatchesCleanup(const CleanupEpoch &epoch) const
{
	return containment_receipt_current_ &&
		receipt_authority_ == LeaseAuthority::cleanup_epoch &&
		receipt_authority_identity_ == epoch.identity_ &&
		receipt_generation_ == epoch.generation_ &&
		receipt_requested_resource_flags_ == 0 &&
		(receipt_core_gpo_ & 0xc0000000u) == 0x40000000u &&
		receipt_interface_module_ == 0 && receipt_sdr_port_control_ == 0 &&
		receipt_bridge_reset_ == 7 && receipt_remap_ == 1 &&
		receipt_manager_neutral_observed_ &&
		(receipt_manager_control_ & 0x107u) == 0x2u &&
		receipt_manager_mode_ <= 4u &&
		receipt_manager_neutral_mutation_sequence_ != 0 &&
		receipt_manager_neutral_mutation_sequence_ <= receipt_mutation_sequence_ &&
		receipt_mappings_released_ &&
		receipt_mutation_sequence_ == mutation_sequence_;
}

bool HardwareBroker::ReceiptMatchesRecovery(const RecoveryEpoch &epoch) const
{
	return containment_receipt_current_ &&
		receipt_authority_ == LeaseAuthority::recovery_epoch &&
		receipt_authority_identity_ == epoch.identity_ &&
		receipt_generation_ == 0 &&
		receipt_requested_resource_flags_ == epoch.requested_resource_flags_ &&
		(receipt_core_gpo_ & 0xc0000000u) == 0x40000000u &&
		receipt_interface_module_ == 0 && receipt_sdr_port_control_ == 0 &&
		receipt_bridge_reset_ == 7 && receipt_remap_ == 1 &&
		receipt_manager_neutral_observed_ &&
		(receipt_manager_control_ & 0x107u) == 0x2u &&
		receipt_manager_mode_ <= 4u &&
		receipt_manager_neutral_mutation_sequence_ != 0 &&
		receipt_manager_neutral_mutation_sequence_ <= receipt_mutation_sequence_ &&
		receipt_mappings_released_ &&
		receipt_mutation_sequence_ == mutation_sequence_;
}

bool HardwareBroker::IsHardwareOperation(OperationKind operation_kind)
{
	switch (operation_kind) {
	case OperationKind::program_fpga:
	case OperationKind::core_protocol:
	case OperationKind::input:
	case OperationKind::audio:
	case OperationKind::video:
	case OperationKind::audio_video:
	case OperationKind::terminal_fpga_cleanup:
		return true;
	case OperationKind::scheduler:
	case OperationKind::offload:
	case OperationKind::save:
	case OperationKind::content:
	case OperationKind::input_descriptors:
		return false;
	}
	return false;
}

bool HardwareBroker::CleanupDeadline(OperationKind operation_kind,
	const CleanupEpoch &epoch, uint64_t *absolute_deadline_ms)
{
	if (absolute_deadline_ms == nullptr) return false;
	switch (operation_kind) {
	case OperationKind::scheduler:
	case OperationKind::offload:
	case OperationKind::input:
	case OperationKind::input_descriptors:
	case OperationKind::save:
	case OperationKind::audio:
	case OperationKind::video:
	case OperationKind::audio_video:
	case OperationKind::content:
		*absolute_deadline_ms = epoch.non_fpga_deadline_ms_;
		return true;
	case OperationKind::core_protocol:
	case OperationKind::terminal_fpga_cleanup:
		*absolute_deadline_ms = epoch.fpga_deadline_ms_;
		return true;
	case OperationKind::program_fpga:
		return false;
	}
	return false;
}

bool HardwareBroker::IsRecoveryOperation(OperationKind operation_kind,
	uint32_t requested_resource_flags)
{
	switch (operation_kind) {
	case OperationKind::input_descriptors:
		return (requested_resource_flags & MISTER_RESOURCE_CORE_INPUT) != 0;
	case OperationKind::save:
		return (requested_resource_flags & MISTER_RESOURCE_SAVES) != 0;
	case OperationKind::audio:
		return (requested_resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0;
	case OperationKind::video:
		return (requested_resource_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0;
	case OperationKind::audio_video:
		return (requested_resource_flags & (MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO)) ==
			(MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO);
	case OperationKind::content:
		return (requested_resource_flags & MISTER_RESOURCE_CONTENT) != 0;
	case OperationKind::core_protocol:
		return (requested_resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0;
	case OperationKind::terminal_fpga_cleanup:
		return (requested_resource_flags & (MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL)) ==
			(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
			MISTER_RESOURCE_CORE_PROTOCOL);
	case OperationKind::program_fpga:
	case OperationKind::input:
	case OperationKind::scheduler:
	case OperationKind::offload:
		return false;
	}
	return false;
}

bool HardwareBroker::RecoveryDeadline(OperationKind operation_kind,
	const RecoveryEpoch &epoch, uint64_t *absolute_deadline_ms)
{
	if (absolute_deadline_ms == nullptr ||
		!IsRecoveryOperation(operation_kind, epoch.requested_resource_flags_))
		return false;
	switch (operation_kind) {
	case OperationKind::core_protocol:
	case OperationKind::terminal_fpga_cleanup:
		*absolute_deadline_ms = epoch.fpga_deadline_ms_;
		return true;
	case OperationKind::input_descriptors:
	case OperationKind::save:
	case OperationKind::audio:
	case OperationKind::video:
	case OperationKind::audio_video:
	case OperationKind::content:
		*absolute_deadline_ms = epoch.non_fpga_deadline_ms_;
		return true;
	case OperationKind::program_fpga:
	case OperationKind::input:
	case OperationKind::scheduler:
	case OperationKind::offload:
		return false;
	}
	return false;
}

} // namespace native
} // namespace mister
