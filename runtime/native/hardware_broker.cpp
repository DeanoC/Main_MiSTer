// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/hardware_broker.hpp"

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
	  containment_receipt_current_(false), recovery_registered_(false),
	  hardware_transaction_active_(false), failure_latched_(false), profile_(nullptr)
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
	containment_receipt_current_ = false;
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
	containment_receipt_current_ = false;
	failure_latched_ = false;
	*generation = next_generation;
	return MISTER_RESULT_OK;
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
	containment_receipt_current_ = false;
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
	if (hardware_transaction_active_)
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
		(state_ == State::cleanup || state_ == State::terminal_neutral);
	const bool recovery_authority =
		registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery &&
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
	std::lock_guard<std::mutex> lock(mutex_);
	const std::shared_ptr<OperationRegistration> &registration =
		terminal_lease.registration_;
	if (state_ != State::cleanup || !IsCurrentCleanup(epoch) ||
		!registration || !registration->registered ||
		registration->broker != this ||
		registration->authority != LeaseAuthority::cleanup_epoch ||
		registration->authority_identity != epoch.identity_ ||
		registration->operation_kind != OperationKind::terminal_fpga_cleanup ||
		active_lease_count_ != 1 || terminal_lease_count_ != 1) {
		return MISTER_RESULT_INVALID_STATE;
	}
	if (registration->absolute_deadline_ms != terminal_lease_deadline_ms_ ||
		clock_.NowMs() >= registration->absolute_deadline_ms)
		return MISTER_RESULT_DEADLINE;

	// Task 1 establishes the authority seam but cannot truthfully mint a
	// containment receipt. The terminal adapter task will supply the required
	// readbacks and post-unmap evidence before enabling this transition.
	return MISTER_RESULT_UNSUPPORTED;
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
		!IsRecoveryOperation(operation_kind, epoch.requested_resource_flags_))
		return MISTER_RESULT_INVALID_STATE;
	uint64_t absolute_deadline_ms = 0;
	if (!RecoveryDeadline(operation_kind, epoch, &absolute_deadline_ms))
		return MISTER_RESULT_INVALID_STATE;
	if (clock_.NowMs() >= absolute_deadline_ms)
		return MISTER_RESULT_DEADLINE;
	const bool terminal =
		operation_kind == OperationKind::terminal_fpga_cleanup;
	if (active_lease_count_ != 0 || (terminal && terminal_lease_count_ != 0))
		return MISTER_RESULT_INVALID_STATE;

	std::shared_ptr<OperationRegistration> registration(
		new (std::nothrow) OperationRegistration(*this,
			LeaseAuthority::recovery_epoch, epoch.identity_, operation_kind,
			absolute_deadline_ms, lifetime_, profile_));
	if (!registration) return MISTER_RESULT_PLATFORM;
	std::unique_ptr<OperationLease> admitted(
		new (std::nothrow) OperationLease(registration));
	if (!admitted) return MISTER_RESULT_PLATFORM;
	registration->registered = true;
	++active_lease_count_;
	if (terminal) ++terminal_lease_count_;
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
			active_lease_count_ != 0)
			return MISTER_RESULT_INVALID_STATE;
		observation->observed_resource_flags = 0;
		observation->neutral_resource_flags = 0;
		if (epoch->requested_resource_flags_ != 0)
			return MISTER_RESULT_CLEANUP_INCOMPLETE;

		epoch->registered_ = false;
		recovery_registered_ = false;
		recovery_identity_ = 0;
		recovery_requested_resource_flags_ = 0;
		recovery_non_fpga_deadline_ms_ = 0;
		recovery_fpga_deadline_ms_ = 0;
		state_ = State::idle;
	}
	epoch.reset();
	return MISTER_RESULT_OK;
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
			!IsCurrentCleanup(*epoch)) {
			return MISTER_RESULT_INVALID_STATE;
		}

		epoch->registered_ = false;
		cleanup_registered_ = false;
		containment_receipt_current_ = false;
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
		containment_receipt_current_ = false;
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
		if (active_lease_count_ == 0) {
			recovery_registered_ = false;
			recovery_identity_ = 0;
			recovery_requested_resource_flags_ = 0;
			recovery_non_fpga_deadline_ms_ = 0;
			recovery_fpga_deadline_ms_ = 0;
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
		(state_ == State::cleanup || state_ == State::terminal_neutral);
	const bool recovery_authority =
		registration->authority == LeaseAuthority::recovery_epoch &&
		registration->authority_identity == recovery_identity_ &&
		state_ == State::recovery &&
		IsRecoveryOperation(registration->operation_kind,
			recovery_requested_resource_flags_);
	if (!active_authority && !cleanup_authority && !recovery_authority)
		return 0;
	if (mutation_sequence_ == UINT64_MAX) return 0;
	return ++mutation_sequence_;
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
