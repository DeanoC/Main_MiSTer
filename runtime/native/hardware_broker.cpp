// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_recovery.hpp"

#include <atomic>
#include <new>

namespace mister {
namespace native {

struct BrokerLifetime {
	explicit BrokerLifetime(HardwareBroker *owner) : broker(owner) {}

	// Every call from an outliving token takes this mutex before the broker
	// mutex. Broker code must never acquire this mutex while holding its own.
	std::mutex mutex;
	HardwareBroker *broker;
};

struct OperationRegistration {
	OperationRegistration(HardwareBroker &owner, LeaseAuthority lease_authority,
		uint64_t identity, OperationKind kind, uint64_t deadline_ms,
		const std::shared_ptr<BrokerLifetime> &broker_lifetime,
		const NativeCoreProfile *bound_profile)
		: broker(&owner), lifetime(broker_lifetime), authority(lease_authority),
		  authority_identity(identity), operation_kind(kind),
		  absolute_deadline_ms(deadline_ms), profile(bound_profile), registered(false)
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
	uint64_t absolute_deadline_ms;
	const NativeCoreProfile *profile;
	bool registered;
};

namespace {

std::atomic<uint64_t> generation_nonce_source(0);
std::atomic<uint64_t> cleanup_nonce_source(0);

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

} // namespace

HardwareLeaseView::HardwareLeaseView(
	const std::shared_ptr<OperationRegistration> &registration)
	: registration_(registration)
{
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

uint64_t HardwareLeaseView::absolute_deadline_ms() const
{
	return registration_->absolute_deadline_ms;
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

OperationKind OperationLease::operation_kind() const
{
	return registration_->operation_kind;
}

uint64_t OperationLease::absolute_deadline_ms() const
{
	return registration_->absolute_deadline_ms;
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
	  mutation_sequence_(0), active_lease_count_(0), terminal_lease_count_(0),
	  cleanup_registered_(false), cleanup_ever_started_(false),
	  quiesce_complete_(false), quiesce_call_active_(false),
	  containment_receipt_current_(false), containment_evidence_pending_(false),
	  recovery_registered_(false),
	  recovery_observation_active_(false), recovery_terminal_neutral_(false),
	  hardware_transaction_active_(false), failure_latched_(false),
	  recovery_result_(MISTER_RESULT_OK),
	  receipt_authority_(LeaseAuthority::active_generation),
	  receipt_authority_identity_(0), receipt_generation_(0),
	  receipt_requested_resource_flags_(0), receipt_core_gpo_(0),
	  receipt_interface_module_(0), receipt_sdr_port_control_(0),
	  receipt_bridge_reset_(0), receipt_remap_(0),
	  receipt_mutation_sequence_(0), recovery_observed_resource_flags_(0),
	  recovery_neutral_resource_flags_(0), receipt_mappings_released_(false),
	  receipt_registration_(nullptr),
	  profile_(nullptr)
{
}

HardwareBroker::~HardwareBroker()
{
	std::lock_guard<std::mutex> lifetime_lock(lifetime_->mutex);
	lifetime_->broker = nullptr;
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
	quiesce_complete_ = false;
	ClearContainmentReceipt();
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
	quiesce_complete_ = false;
	ClearContainmentReceipt();
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
		!quiesce_complete_ || cleanup_registered_ || cleanup_ever_started_) {
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
	ClearContainmentReceipt();
	state_ = State::cleanup;
	*epoch = std::move(registered);
	return MISTER_RESULT_OK;
}

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
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= registration->absolute_deadline_ms)
		return MISTER_RESULT_DEADLINE;

	std::unique_ptr<HardwareLeaseView> admitted(
		new (std::nothrow) HardwareLeaseView(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	hardware_transaction_active_ = true;
	*view = std::move(admitted);
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
		active_lease_count_ != 0)
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
	ClearContainmentReceipt();
	recovery_registered_ = true;
	state_ = State::recovery;
	*epoch = std::move(registered);
	return MISTER_RESULT_OK;
}

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
		LatchRecoveryResult(MISTER_RESULT_DEADLINE);
		return MISTER_RESULT_DEADLINE;
	}

	std::shared_ptr<OperationRegistration> registration(
		new (std::nothrow) OperationRegistration(*this,
			LeaseAuthority::recovery_epoch, epoch.identity_, operation_kind,
			absolute_deadline_ms, lifetime_, profile_));
	if (!registration) {
		LatchRecoveryResult(MISTER_RESULT_PLATFORM);
		return MISTER_RESULT_PLATFORM;
	}
	std::unique_ptr<OperationLease> admitted(
		new (std::nothrow) OperationLease(registration));
	if (!admitted) {
		LatchRecoveryResult(MISTER_RESULT_PLATFORM);
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

Result HardwareBroker::FinishRecovery(std::unique_ptr<RecoveryEpoch> &&epoch,
	MisterRecoveryObservationV2 *observation)
{
	if (!epoch || observation == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ != State::recovery || !IsCurrentRecovery(*epoch) ||
			active_lease_count_ != 0 || recovery_observation_active_)
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
		const Result result = recovery_result_ != MISTER_RESULT_OK ?
			recovery_result_ : (all_neutral ? MISTER_RESULT_OK :
				MISTER_RESULT_CLEANUP_INCOMPLETE);

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
	if (active_lease_count_ != 0) --active_lease_count_;
	if (registration.operation_kind == OperationKind::terminal_fpga_cleanup &&
		terminal_lease_count_ != 0) {
		--terminal_lease_count_;
		if (terminal_lease_count_ == 0) terminal_lease_deadline_ms_ = 0;
	}
	lease_released_.notify_all();
}

void HardwareBroker::ReleaseHardwareLeaseView(HardwareLeaseView &view)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!view.registration_ || view.registration_->broker != this) return;
	hardware_transaction_active_ = false;
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
	return clock_.NowMs() >= registration->absolute_deadline_ms ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

Result HardwareBroker::StageContainmentEvidence(
	const OperationLease &terminal_lease, uint32_t core_gpo,
	uint32_t interface_module, uint32_t sdr_port_control,
	uint32_t bridge_reset, uint32_t remap, bool mappings_released)
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
	if (clock_.NowMs() >= registration->absolute_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	if ((core_gpo & 0xc0000000u) != 0x40000000u ||
		interface_module != 0 || sdr_port_control != 0 || bridge_reset != 7 ||
		remap != 1 || !mappings_released)
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
	if (clock_.NowMs() >= registration->absolute_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	containment_evidence_pending_ = false;
	containment_receipt_current_ = true;
	state_ = State::terminal_neutral;
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
	if (clock_.NowMs() >= registration->absolute_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	containment_evidence_pending_ = false;
	containment_receipt_current_ = true;
	recovery_terminal_neutral_ = true;
	RecordRecoveryPartition(0, MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL);
	return MISTER_RESULT_OK;
}

Result HardwareBroker::BeginRecoveryObservation(const RecoveryEpoch &epoch)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_observation_active_ || recovery_terminal_neutral_ ||
		active_lease_count_ != 0)
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= epoch.fpga_deadline_ms_) {
		LatchRecoveryResult(MISTER_RESULT_DEADLINE);
		return MISTER_RESULT_DEADLINE;
	}
	recovery_observation_active_ = true;
	return MISTER_RESULT_OK;
}

Result HardwareBroker::CheckRecoveryObservationDeadline(
	const RecoveryEpoch &epoch)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recovery_observation_active_ || state_ != State::recovery ||
		!IsCurrentRecovery(epoch))
		return MISTER_RESULT_INVALID_STATE;
	return clock_.NowMs() >= epoch.fpga_deadline_ms_ ?
		MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
}

Result HardwareBroker::EndRecoveryObservation(const RecoveryEpoch &epoch,
	uint32_t observed_resource_flags, uint32_t neutral_resource_flags,
	Result result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recovery_observation_active_ || state_ != State::recovery ||
		!IsCurrentRecovery(epoch))
		return MISTER_RESULT_INVALID_STATE;
	recovery_observation_active_ = false;
	observed_resource_flags &= epoch.requested_resource_flags_;
	neutral_resource_flags &= epoch.requested_resource_flags_;
	if ((observed_resource_flags & neutral_resource_flags) != 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (result == MISTER_RESULT_OK && clock_.NowMs() >= epoch.fpga_deadline_ms_)
		result = MISTER_RESULT_DEADLINE;
	const uint32_t observed_bundle = epoch.requested_resource_flags_ &
		(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		 MISTER_RESOURCE_CORE_PROTOCOL);
	recovery_observed_resource_flags_ &= ~observed_bundle;
	recovery_neutral_resource_flags_ &= ~observed_bundle;
	RecordRecoveryPartition(observed_resource_flags, neutral_resource_flags);
	LatchRecoveryResult(result);
	return result;
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
	LatchRecoveryResult(result);
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
		clock_.NowMs() >= registration->absolute_deadline_ms)
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
	LatchRecoveryResult(result);
	return result;
}

Result HardwareBroker::RecordRecoveryFailure(const RecoveryEpoch &epoch,
	Result result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != State::recovery || !IsCurrentRecovery(epoch) ||
		recovery_terminal_neutral_)
		return MISTER_RESULT_INVALID_STATE;
	LatchRecoveryResult(result);
	return result;
}

uint32_t HardwareBroker::RecoveryResourceForOperation(
	OperationKind operation_kind)
{
	switch (operation_kind) {
	case OperationKind::input_descriptors: return MISTER_RESOURCE_CORE_INPUT;
	case OperationKind::save: return MISTER_RESOURCE_SAVES;
	case OperationKind::audio: return MISTER_RESOURCE_NATIVE_AUDIO;
	case OperationKind::video: return MISTER_RESOURCE_NATIVE_VIDEO;
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

void HardwareBroker::LatchRecoveryResult(Result result)
{
	if (result == MISTER_RESULT_OK || recovery_result_ != MISTER_RESULT_OK)
		return;
	if (result == MISTER_RESULT_DEADLINE || result == MISTER_RESULT_PLATFORM ||
		result == MISTER_RESULT_CLEANUP_INCOMPLETE)
		recovery_result_ = result;
	else
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
	receipt_mutation_sequence_ = 0;
	receipt_mappings_released_ = false;
	receipt_registration_ = nullptr;
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
