// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "tests/native_core_protocol_authority_test_peer.hpp"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>

namespace mister {
namespace native {

namespace {

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms)
		: now_ms_(now_ms), wait_calls_(0), force_timeout_(false),
		  pause_after_wait_(false), wait_return_paused_(false),
		  allow_wait_return_(false)
	{
		assert(pthread_mutex_init(&wait_mutex_, nullptr) == 0);
		assert(pthread_cond_init(&wait_condition_, nullptr) == 0);
	}

	~FakeClock() override
	{
		assert(pthread_cond_destroy(&wait_condition_) == 0);
		assert(pthread_mutex_destroy(&wait_mutex_) == 0);
	}

	uint64_t NowMs() const override
	{
		return now_ms_.load();
	}

	bool WaitUntil(std::condition_variable &condition,
		std::unique_lock<std::mutex> &lock,
		uint64_t absolute_deadline_ms) override
	{
		assert(pthread_mutex_lock(&wait_mutex_) == 0);
		wait_calls_.fetch_add(1);
		assert(pthread_cond_broadcast(&wait_condition_) == 0);
		assert(pthread_mutex_unlock(&wait_mutex_) == 0);

		if (force_timeout_.load()) return false;
		condition.wait(lock);
		if (pause_after_wait_.load()) {
			lock.unlock();
			assert(pthread_mutex_lock(&wait_mutex_) == 0);
			wait_return_paused_.store(true);
			assert(pthread_cond_broadcast(&wait_condition_) == 0);
			while (!allow_wait_return_.load()) {
				assert(pthread_cond_wait(&wait_condition_, &wait_mutex_) == 0);
			}
			assert(pthread_mutex_unlock(&wait_mutex_) == 0);
			lock.lock();
		}
		return NowMs() < absolute_deadline_ms;
	}

	void SetNow(uint64_t now_ms)
	{
		now_ms_.store(now_ms);
	}

	void ForceTimeout(bool force_timeout)
	{
		force_timeout_.store(force_timeout);
	}

	void PauseAfterWait()
	{
		pause_after_wait_.store(true);
	}

	void WaitForPausedReturn()
	{
		assert(pthread_mutex_lock(&wait_mutex_) == 0);
		while (!wait_return_paused_.load()) {
			struct timespec timeout;
			assert(clock_gettime(CLOCK_REALTIME, &timeout) == 0);
			timeout.tv_sec += 2;
			const int result = pthread_cond_timedwait(
				&wait_condition_, &wait_mutex_, &timeout);
			assert(result == 0);
		}
		assert(pthread_mutex_unlock(&wait_mutex_) == 0);
	}

	void AllowWaitReturn()
	{
		assert(pthread_mutex_lock(&wait_mutex_) == 0);
		allow_wait_return_.store(true);
		assert(pthread_cond_broadcast(&wait_condition_) == 0);
		assert(pthread_mutex_unlock(&wait_mutex_) == 0);
	}

	void WaitForBrokerWait()
	{
		assert(pthread_mutex_lock(&wait_mutex_) == 0);
		while (wait_calls_.load() == 0) {
			struct timespec timeout;
			assert(clock_gettime(CLOCK_REALTIME, &timeout) == 0);
			timeout.tv_sec += 2;
			const int result = pthread_cond_timedwait(
				&wait_condition_, &wait_mutex_, &timeout);
			assert(result == 0);
		}
		assert(pthread_mutex_unlock(&wait_mutex_) == 0);
	}

private:
	std::atomic<uint64_t> now_ms_;
	std::atomic<unsigned> wait_calls_;
	std::atomic<bool> force_timeout_;
	std::atomic<bool> pause_after_wait_;
	std::atomic<bool> wait_return_paused_;
	std::atomic<bool> allow_wait_return_;
	pthread_mutex_t wait_mutex_;
	pthread_cond_t wait_condition_;
};

struct QuiesceContext {
	HardwareBroker *broker;
	PlatformGenerationId generation;
	uint64_t deadline_ms;
	Result result;
};

void *QuiesceThread(void *opaque)
{
	QuiesceContext *context = static_cast<QuiesceContext *>(opaque);
	context->result = context->broker->Quiesce(
		context->generation, context->deadline_ms);
	return nullptr;
}

struct BeginContext {
	HardwareBroker *broker;
	PlatformGenerationId generation;
	uint64_t deadline_ms;
	Result result;
};

void *BeginThread(void *opaque)
{
	BeginContext *context = static_cast<BeginContext *>(opaque);
	std::unique_ptr<OperationLease> lease;
	context->result = context->broker->Begin(context->generation,
		OperationKind::input, context->deadline_ms, &lease);
	return nullptr;
}

void PrepareCleanup(HardwareBroker &broker, FakeClock &clock,
	const NativeCoreProfile &profile, PlatformGenerationId *generation,
	std::unique_ptr<CleanupEpoch> *epoch)
{
	assert(broker.EnterFixtureForTest(profile, generation) == MISTER_RESULT_OK);
	assert(*generation != 0);
	assert(broker.Quiesce(*generation, clock.NowMs() + 100) ==
		MISTER_RESULT_OK);
	const uint64_t cleanup_start = clock.NowMs();
	assert(broker.BeginCleanup(*generation, cleanup_start + 2000,
		cleanup_start + 5000, epoch) == MISTER_RESULT_OK);
}

void TestOwningTypesAreNotForgeable()
{
	static_assert(!std::is_default_constructible<OperationLease>::value,
		"operation leases must be broker-created");
	static_assert(!std::is_copy_constructible<OperationLease>::value,
		"operation leases must not be copied");
	static_assert(!std::is_move_constructible<OperationLease>::value,
		"operation lease objects stay at one registered address");
	static_assert(std::is_move_constructible<
		std::unique_ptr<OperationLease> >::value,
		"unique ownership must remain movable");
	static_assert(!std::is_default_constructible<CleanupEpoch>::value,
		"cleanup epochs must be broker-created");
	static_assert(!std::is_copy_constructible<CleanupEpoch>::value,
		"cleanup epochs must not be copied");
	static_assert(!std::is_move_constructible<CleanupEpoch>::value,
		"cleanup epoch objects stay at one registered address");
	static_assert(!std::is_default_constructible<HardwareLeaseView>::value,
		"hardware views must not be constructible by adapters or tests");
	static_assert(!std::is_copy_constructible<HardwareLeaseView>::value,
		"hardware views must retain unique owning handles");
	static_assert(!std::is_default_constructible<
		ActiveCoreProtocolSession>::value,
		"active protocol authority must be broker-created");
	static_assert(!std::is_default_constructible<
		CleanupCoreProtocolSession>::value,
		"cleanup protocol authority must be broker-created");
	static_assert(!std::is_default_constructible<
		RecoveryCoreProtocolSession>::value,
		"recovery protocol authority must be broker-created");
	static_assert(!std::is_convertible<ActiveCoreProtocolSession *,
		CleanupCoreProtocolSession *>::value,
		"active protocol authority cannot become cleanup authority");
	static_assert(!std::is_convertible<CleanupCoreProtocolSession *,
		RecoveryCoreProtocolSession *>::value,
		"cleanup protocol authority cannot become recovery authority");
	static_assert(!std::is_convertible<RecoveryCoreProtocolSession *,
		ActiveCoreProtocolSession *>::value,
		"recovery protocol authority cannot become active authority");
}

void TestProtocolSessionsRequireExactAuthority()
{
	FakeClock active_clock(1000);
	HardwareBroker active_broker(active_clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(active_broker.EnterFixtureForTest(profile, &generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active_lease;
	assert(active_broker.Begin(generation, OperationKind::core_protocol, 2000,
		&active_lease) == MISTER_RESULT_OK);
	NativeCoreProfile copied_profile = profile;
	std::unique_ptr<ActiveCoreProtocolSession> wrong_profile;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*active_lease,
		active_broker, copied_profile, &wrong_profile) ==
		MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<ActiveCoreProtocolSession> active;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*active_lease,
		active_broker, profile, &active) == MISTER_RESULT_OK);
	assert(active_lease->absolute_deadline_ms() == 2000);
	std::unique_ptr<CleanupCoreProtocolSession> wrong_cleanup;
	assert(CoreProtocolAuthorityTestPeer::AcquireCleanup(*active_lease,
		active_broker, &wrong_cleanup) == MISTER_RESULT_INVALID_STATE);
	active.reset();
	active_lease.reset();

	FakeClock cleanup_clock(3000);
	HardwareBroker cleanup_broker(cleanup_clock);
	std::unique_ptr<CleanupEpoch> cleanup_epoch;
	PrepareCleanup(cleanup_broker, cleanup_clock, profile, &generation,
		&cleanup_epoch);
	std::unique_ptr<OperationLease> cleanup_lease;
	assert(cleanup_broker.BeginCleanupOperation(*cleanup_epoch,
		OperationKind::core_protocol, &cleanup_lease) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupCoreProtocolSession> cleanup;
	assert(CoreProtocolAuthorityTestPeer::AcquireCleanup(*cleanup_lease,
		cleanup_broker, &cleanup) == MISTER_RESULT_OK);
	assert(cleanup_lease->absolute_deadline_ms() == cleanup_clock.NowMs() + 5000);
	std::unique_ptr<RecoveryCoreProtocolSession> wrong_recovery;
	assert(CoreProtocolAuthorityTestPeer::AcquireRecovery(*cleanup_lease,
		cleanup_broker, &wrong_recovery) == MISTER_RESULT_INVALID_STATE);

	FakeClock recovery_clock(7000);
	HardwareBroker recovery_broker(recovery_clock);
	std::unique_ptr<RecoveryEpoch> recovery_epoch;
	assert(recovery_broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL,
		9000, 12000, &recovery_epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> recovery_lease;
	assert(recovery_broker.BeginRecoveryOperation(*recovery_epoch,
		OperationKind::core_protocol, &recovery_lease) == MISTER_RESULT_OK);
	std::unique_ptr<RecoveryCoreProtocolSession> recovery;
	assert(CoreProtocolAuthorityTestPeer::AcquireRecovery(*recovery_lease,
		recovery_broker, &recovery) == MISTER_RESULT_OK);
	assert(recovery_lease->absolute_deadline_ms() == 12000);
	std::unique_ptr<ActiveCoreProtocolSession> wrong_active;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*recovery_lease,
		recovery_broker, profile, &wrong_active) == MISTER_RESULT_INVALID_STATE);
}

void TestEnterRequiresExactTrustedFixtureAuthority()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	const NativeCoreProfile *fixture = FixtureNativeCoreProfile("snes");
	assert(fixture != nullptr);
	NativeCoreProfile copy = *fixture;
	assert(broker.Enter(copy, &generation) == MISTER_RESULT_UNSUPPORTED);
	assert(generation == 0);
	copy = *fixture;
	copy.authority = NativeProfileAuthority::untrusted;
	assert(broker.Enter(copy, &generation) == MISTER_RESULT_UNSUPPORTED);
	assert(generation == 0);
	copy = *fixture;
	copy.input.digital_word_count = 1;
	assert(broker.Enter(copy, &generation) == MISTER_RESULT_UNSUPPORTED);
	assert(generation == 0);
	assert(broker.Enter(static_cast<const NativeCoreProfile *>(nullptr),
		&generation) == MISTER_RESULT_UNSUPPORTED);
	assert(generation == 0);
	assert(ProductionNativeCoreProfile("snes") == nullptr);
	assert(broker.Enter(*fixture, &generation) == MISTER_RESULT_UNSUPPORTED);
	assert(broker.EnterFixtureForTest(*fixture, &generation) == MISTER_RESULT_OK);
	assert(generation != 0);
}

void TestLeaseDurationQuiesceAndFreshGeneration()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId first_generation = 0;
	PlatformGenerationId duplicate_generation = 0;
	assert(broker.EnterFixtureForTest(profile, &first_generation) == MISTER_RESULT_OK);
	assert(first_generation != 0);
	assert(broker.EnterFixtureForTest(profile, &duplicate_generation) ==
		MISTER_RESULT_INVALID_STATE);
	assert(duplicate_generation == 0);

	std::unique_ptr<OperationLease> held;
	assert(broker.Begin(first_generation, OperationKind::input, 2000, &held) ==
		MISTER_RESULT_OK);
	assert(held.get() != nullptr);
	assert(held->operation_kind() == OperationKind::input);
	assert(held->absolute_deadline_ms() == 2000);
	std::unique_ptr<HardwareLeaseView> hardware_view;
	assert(broker.AcquireHardwareLeaseView(*held, &hardware_view) ==
		MISTER_RESULT_OK);

	QuiesceContext quiesce = {
		&broker, first_generation, 3000, MISTER_RESULT_PLATFORM};
	pthread_t quiesce_thread;
	assert(pthread_create(&quiesce_thread, nullptr, QuiesceThread,
		&quiesce) == 0);
	clock.WaitForBrokerWait();

	std::unique_ptr<OperationLease> denied;
	assert(broker.Begin(first_generation, OperationKind::scheduler, 2000,
		&denied) == MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<CleanupEpoch> too_early;
	assert(broker.BeginCleanup(first_generation, 3000, 6000, &too_early) ==
		MISTER_RESULT_INVALID_STATE);

	held.reset();
	assert(broker.BeginCleanup(first_generation, 3000, 6000, &too_early) ==
		MISTER_RESULT_INVALID_STATE);
	hardware_view.reset();
	assert(pthread_join(quiesce_thread, nullptr) == 0);
	assert(quiesce.result == MISTER_RESULT_OK);
	assert(broker.Begin(first_generation, OperationKind::input, 2000,
		&denied) == MISTER_RESULT_INVALID_STATE);

	const uint64_t cleanup_start = clock.NowMs();
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(first_generation, cleanup_start + 2000,
		cleanup_start + 5000, &epoch) == MISTER_RESULT_OK);

	BeginContext other_thread = {
		&broker, first_generation, 2000, MISTER_RESULT_PLATFORM};
	pthread_t begin_thread;
	assert(pthread_create(&begin_thread, nullptr, BeginThread,
		&other_thread) == 0);
	assert(pthread_join(begin_thread, nullptr) == 0);
	assert(other_thread.result == MISTER_RESULT_INVALID_STATE);

	const OperationKind non_fpga_kinds[] = {
		OperationKind::scheduler,
		OperationKind::offload,
		OperationKind::input,
		OperationKind::input_descriptors,
		OperationKind::save,
		OperationKind::audio,
		OperationKind::video,
		OperationKind::content,
	};
	for (size_t index = 0;
		index < sizeof(non_fpga_kinds) / sizeof(non_fpga_kinds[0]); ++index) {
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginCleanupOperation(*epoch, non_fpga_kinds[index],
			&lease) == MISTER_RESULT_OK);
		assert(lease->operation_kind() == non_fpga_kinds[index]);
		assert(lease->absolute_deadline_ms() == cleanup_start + 2000);
	}

	std::unique_ptr<OperationLease> core_protocol;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&core_protocol) == MISTER_RESULT_OK);
	assert(core_protocol->absolute_deadline_ms() == cleanup_start + 5000);
	core_protocol.reset();

	std::unique_ptr<OperationLease> never_cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::program_fpga,
		&never_cleanup) == MISTER_RESULT_INVALID_STATE);

	clock.SetNow(cleanup_start + 2500);
	std::unique_ptr<OperationLease> expired_non_fpga;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::content,
		&expired_non_fpga) == MISTER_RESULT_DEADLINE);
	std::unique_ptr<OperationLease> retained_fpga_deadline;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&retained_fpga_deadline) == MISTER_RESULT_OK);
	assert(retained_fpga_deadline->absolute_deadline_ms() ==
		cleanup_start + 5000);
	retained_fpga_deadline.reset();

	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginCleanupOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(terminal->absolute_deadline_ms() == cleanup_start + 5000);
	assert(broker.ObserveContainment(*epoch, *terminal) ==
		MISTER_RESULT_UNSUPPORTED);
	terminal.reset();
	assert(broker.Leave(first_generation, std::move(epoch)) ==
		MISTER_RESULT_INVALID_STATE);
	assert(epoch.get() != nullptr);

	FakeClock second_clock(5000);
	HardwareBroker second_broker(second_clock);
	PlatformGenerationId second_generation = 0;
	assert(second_broker.EnterFixtureForTest(profile, &second_generation) == MISTER_RESULT_OK);
	assert(second_generation != 0);
	assert(second_generation != first_generation);
	assert(broker.Begin(first_generation, OperationKind::input,
		clock.NowMs() + 100, &denied) == MISTER_RESULT_INVALID_STATE);
}

void TestBoundedQuiesceNeverReopensAdmission()
{
	FakeClock clock(4000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);

	std::unique_ptr<OperationLease> held;
	assert(broker.Begin(generation, OperationKind::offload, 5000, &held) ==
		MISTER_RESULT_OK);
	clock.ForceTimeout(true);
	assert(broker.Quiesce(generation, 4500) == MISTER_RESULT_DEADLINE);
	std::unique_ptr<OperationLease> denied;
	assert(broker.Begin(generation, OperationKind::input, 5000, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	held.reset();
	clock.ForceTimeout(false);
	assert(broker.Quiesce(generation, 4500) == MISTER_RESULT_OK);

	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 6000, 9000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> duplicate;
	assert(broker.BeginCleanup(generation, 7000, 10000, &duplicate) ==
		MISTER_RESULT_INVALID_STATE);

	std::unique_ptr<OperationLease> first_retry;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::save,
		&first_retry) == MISTER_RESULT_OK);
	assert(first_retry->absolute_deadline_ms() == 6000);
	first_retry.reset();
	clock.SetNow(5000);
	std::unique_ptr<OperationLease> second_retry;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::save,
		&second_retry) == MISTER_RESULT_OK);
	assert(second_retry->absolute_deadline_ms() == 6000);
}

void TestFailureLatchClosesPreissuedHardwareLeaseAdmission()
{
	FakeClock clock(4500);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);

	std::unique_ptr<OperationLease> preissued;
	assert(broker.Begin(generation, OperationKind::input, 5500, &preissued) ==
		MISTER_RESULT_OK);
	assert(broker.LatchFailure(generation) == MISTER_RESULT_OK);

	std::unique_ptr<HardwareLeaseView> denied;
	assert(broker.AcquireHardwareLeaseView(*preissued, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	assert(denied == nullptr);
}

void TestFailedProtocolCompletionLatchesAndRequiresBoundedDrain()
{
	FakeClock clock(5000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);

	std::unique_ptr<OperationLease> preissued_input;
	assert(broker.Begin(generation, OperationKind::input, 9000,
		&preissued_input) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> failing_lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&failing_lease) == MISTER_RESULT_OK);
	std::unique_ptr<ActiveCoreProtocolSession> session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*failing_lease, broker,
		profile, &session) == MISTER_RESULT_OK);
	const CoreProtocolResidue unmutated_residue = {
		false, false, false, false, false, false, false, 0};
	const ProtocolMappingReleaseReceipt unmutated_release = {
		MISTER_RESULT_OK, true, true, true, true, true, 0};
	const ActiveProtocolFailureReceipt unmutated_receipt = {
		MISTER_RESULT_PLATFORM, unmutated_residue, unmutated_release, 0};
	assert(CoreProtocolAuthorityTestPeer::CompleteFailed(*failing_lease, broker,
		std::move(session), unmutated_receipt) == MISTER_RESULT_INVALID_STATE);
	assert(session != nullptr);
	const uint64_t sequence =
		CoreProtocolAuthorityTestPeer::RecordMutation(*failing_lease, broker,
			*session);
	assert(sequence != 0);

	const CoreProtocolResidue residue = {
		false, false, false, false, false, true, true, sequence};
	const ProtocolMappingReleaseReceipt mapping_release = {
		MISTER_RESULT_OK, true, true, true, true, true, sequence};
	ActiveProtocolFailureReceipt receipt = {
		MISTER_RESULT_PLATFORM, residue, mapping_release, sequence + 1};
	assert(CoreProtocolAuthorityTestPeer::CompleteFailed(*failing_lease, broker,
		std::move(session), receipt) == MISTER_RESULT_INVALID_STATE);
	assert(session != nullptr);

	receipt.final_mutation_sequence = sequence;
	assert(CoreProtocolAuthorityTestPeer::CompleteFailed(*failing_lease, broker,
		std::move(session), receipt) == MISTER_RESULT_OK);
	assert(session == nullptr);
	ActiveProtocolFailureReceipt stored = {};
	assert(broker.core_protocol_failure_receipt_for_test(&stored));
	assert(stored.primary_result == MISTER_RESULT_PLATFORM);
	assert(stored.final_mutation_sequence == sequence);
	assert(stored.residue.download_may_be_active);
	assert(stored.residue.status_reset_asserted);
	assert(!stored.residue.mapping_retained);
	std::unique_ptr<HardwareLeaseView> denied;
	assert(broker.AcquireHardwareLeaseView(*preissued_input, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	assert(denied == nullptr);

	std::unique_ptr<CleanupEpoch> premature;
	assert(broker.BeginCleanup(generation, 8000, 11000, &premature) ==
		MISTER_RESULT_INVALID_STATE);
	failing_lease.reset();
	clock.ForceTimeout(true);
	assert(broker.Quiesce(generation, 7000) == MISTER_RESULT_DEADLINE);
	assert(broker.BeginCleanup(generation, 8000, 11000, &premature) ==
		MISTER_RESULT_INVALID_STATE);
	preissued_input.reset();
	clock.SetNow(7000);
	clock.ForceTimeout(false);
	assert(broker.Quiesce(generation, 7000) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(generation, 9000, 12000, &premature) ==
		MISTER_RESULT_OK);
	assert(broker.core_protocol_failure_receipt_for_test(&stored));
	assert(stored.final_mutation_sequence == sequence);
}

void TestSuccessfulProtocolCompletionIsExplicitAndBrokerDominated()
{
	FakeClock clock(5000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> protocol_lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&protocol_lease) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> preissued;
	assert(broker.Begin(generation, OperationKind::input, 9000, &preissued) ==
		MISTER_RESULT_OK);
	std::unique_ptr<ActiveCoreProtocolSession> session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*protocol_lease, broker,
		profile, &session) == MISTER_RESULT_OK);
	assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	const uint64_t sequence = CoreProtocolAuthorityTestPeer::RecordMutation(
		*protocol_lease, broker, *session);
	assert(sequence != 0);
	std::unique_ptr<HardwareLeaseView> denied_while_current;
	assert(broker.AcquireHardwareLeaseView(*preissued, &denied_while_current) ==
		MISTER_RESULT_INVALID_STATE);

	ProtocolMappingReleaseReceipt receipt = {
		MISTER_RESULT_OK, true, true, true, true, true, sequence + 1};
	assert(CoreProtocolAuthorityTestPeer::CompleteSuccess(*protocol_lease, broker,
		std::move(session), receipt) == MISTER_RESULT_INVALID_STATE);
	assert(session != nullptr);
	assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	receipt.mutation_sequence = sequence;
	assert(CoreProtocolAuthorityTestPeer::CompleteSuccess(*protocol_lease, broker,
		std::move(session), receipt) == MISTER_RESULT_OK);
	assert(session == nullptr);
	assert(!CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	std::unique_ptr<ActiveCoreProtocolSession> duplicate;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*protocol_lease, broker,
		profile, &duplicate) == MISTER_RESULT_INVALID_STATE);

	std::unique_ptr<HardwareLeaseView> view;
	assert(broker.AcquireHardwareLeaseView(*preissued, &view) ==
		MISTER_RESULT_OK);
}

void TestInvalidProtocolOutcomeClosesAdmissionWhileLeaseIsLive()
{
	FakeClock clock(5000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> preissued;
	assert(broker.Begin(generation, OperationKind::input, 9000, &preissued) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> protocol_lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&protocol_lease) == MISTER_RESULT_OK);
	std::unique_ptr<ActiveCoreProtocolSession> session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*protocol_lease, broker,
		profile, &session) == MISTER_RESULT_OK);
	assert(CoreProtocolAuthorityTestPeer::RecordMutation(*protocol_lease, broker,
		*session) != 0);

	assert(CoreProtocolAuthorityTestPeer::CompleteInvalidOutcome(*protocol_lease,
		broker, MISTER_RESULT_PLATFORM) == MISTER_RESULT_OK);
	assert(!CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	std::unique_ptr<HardwareLeaseView> denied;
	assert(broker.AcquireHardwareLeaseView(*preissued, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	session.reset();
	protocol_lease.reset();
	clock.ForceTimeout(true);
	assert(broker.Quiesce(generation, 7000) == MISTER_RESULT_DEADLINE);
	preissued.reset();
	clock.SetNow(7000);
	clock.ForceTimeout(false);
	assert(broker.Quiesce(generation, 7000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 9000, 12000, &cleanup) ==
		MISTER_RESULT_OK);
}

void TestForeignActiveRegistrationCannotInvalidateCurrentSession()
{
	FakeClock clock(5000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> owner;
	std::unique_ptr<OperationLease> foreign;
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&owner) == MISTER_RESULT_OK);
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&foreign) == MISTER_RESULT_OK);
	std::unique_ptr<ActiveCoreProtocolSession> session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*owner, broker, profile,
		&session) == MISTER_RESULT_OK);
	assert(broker.mutation_sequence_for_test() == 0);
	ActiveProtocolFailureReceipt residue = {};
	assert(!broker.core_protocol_failure_receipt_for_test(&residue));

	assert(CoreProtocolAuthorityTestPeer::CompleteInvalidOutcome(*foreign,
		broker, MISTER_RESULT_PLATFORM) == MISTER_RESULT_INVALID_STATE);
	assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	assert(broker.mutation_sequence_for_test() == 0);
	assert(!broker.core_protocol_failure_receipt_for_test(&residue));

	const uint64_t sequence = CoreProtocolAuthorityTestPeer::RecordMutation(
		*owner, broker, *session);
	assert(sequence == 1);
	const ProtocolMappingReleaseReceipt receipt = {
		MISTER_RESULT_OK, true, true, true, true, true, sequence};
	assert(CoreProtocolAuthorityTestPeer::CompleteSuccess(*foreign, broker,
		std::move(session), receipt) == MISTER_RESULT_INVALID_STATE);
	assert(session != nullptr);
	const CoreProtocolResidue failure_residue = {
		false, false, false, false, false, true, true, sequence};
	const ActiveProtocolFailureReceipt failure = {
		MISTER_RESULT_PLATFORM, failure_residue, receipt, sequence};
	assert(CoreProtocolAuthorityTestPeer::CompleteFailed(*foreign, broker,
		std::move(session), failure) == MISTER_RESULT_INVALID_STATE);
	assert(session != nullptr);
	assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	assert(!broker.core_protocol_failure_receipt_for_test(&residue));
	assert(CoreProtocolAuthorityTestPeer::CompleteSuccess(*owner, broker,
		std::move(session), receipt) == MISTER_RESULT_OK);
	assert(!CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	std::unique_ptr<ActiveCoreProtocolSession> foreign_session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*foreign, broker, profile,
		&foreign_session) == MISTER_RESULT_OK);
	const uint64_t foreign_sequence =
		CoreProtocolAuthorityTestPeer::RecordMutation(*foreign, broker,
			*foreign_session);
	const ProtocolMappingReleaseReceipt foreign_receipt = {
		MISTER_RESULT_OK, true, true, true, true, true, foreign_sequence};
	assert(CoreProtocolAuthorityTestPeer::CompleteSuccess(*foreign, broker,
		std::move(foreign_session), foreign_receipt) == MISTER_RESULT_OK);
}

void TestProtocolHandleAbandonmentRetainsBrokerFence()
{
	FakeClock clock(5000);
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> protocol_lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&protocol_lease) == MISTER_RESULT_OK);
	std::unique_ptr<ActiveCoreProtocolSession> session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*protocol_lease, broker,
		profile, &session) == MISTER_RESULT_OK);
	assert(!broker.core_protocol_session_abandoned_for_test(*protocol_lease));
	session.reset();
	assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	assert(broker.core_protocol_session_abandoned_for_test(*protocol_lease));
	std::unique_ptr<OperationLease> foreign;
	assert(broker.Begin(generation, OperationKind::core_protocol, 9000,
		&foreign) == MISTER_RESULT_OK);
	assert(CoreProtocolAuthorityTestPeer::CompleteInvalidOutcome(*foreign,
		broker, MISTER_RESULT_PLATFORM) == MISTER_RESULT_INVALID_STATE);
	assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));

	std::unique_ptr<OperationLease> preissued;
	assert(broker.Begin(generation, OperationKind::input, 9000, &preissued) ==
		MISTER_RESULT_OK);
	std::unique_ptr<HardwareLeaseView> denied;
	assert(broker.AcquireHardwareLeaseView(*preissued, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	assert(CoreProtocolAuthorityTestPeer::CompleteInvalidOutcome(*protocol_lease,
		broker, MISTER_RESULT_PLATFORM) == MISTER_RESULT_OK);
	assert(!CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
	protocol_lease.reset();
	foreign.reset();
	preissued.reset();
	assert(broker.Quiesce(generation, 7000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> denied_cleanup;
	assert(broker.BeginCleanup(generation, 7000, 10000, &denied_cleanup) ==
		MISTER_RESULT_OK);
}

void TestCleanupAndRecoveryProtocolAbandonmentRetainBrokerFence()
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	{
		FakeClock clock(3000);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		PrepareCleanup(broker, clock, profile, &generation, &epoch);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
			&lease) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupCoreProtocolSession> session;
		assert(CoreProtocolAuthorityTestPeer::AcquireCleanup(*lease, broker,
			&session) == MISTER_RESULT_OK);
		assert(!broker.core_protocol_session_abandoned_for_test(*lease));
		session.reset();
		assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
		assert(broker.core_protocol_session_abandoned_for_test(*lease));
		std::unique_ptr<OperationLease> denied;
		assert(broker.BeginCleanupOperation(*epoch, OperationKind::input,
			&denied) == MISTER_RESULT_OK);
		std::unique_ptr<HardwareLeaseView> view;
		assert(broker.AcquireHardwareLeaseView(*denied, &view) ==
			MISTER_RESULT_INVALID_STATE);
	}
	{
		FakeClock clock(7000);
		HardwareBroker broker(clock);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL |
			MISTER_RESOURCE_NATIVE_AUDIO, 9000, 12000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
			&lease) == MISTER_RESULT_OK);
		std::unique_ptr<RecoveryCoreProtocolSession> session;
		assert(CoreProtocolAuthorityTestPeer::AcquireRecovery(*lease, broker,
			&session) == MISTER_RESULT_OK);
		assert(!broker.core_protocol_session_abandoned_for_test(*lease));
		session.reset();
		assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
		assert(broker.core_protocol_session_abandoned_for_test(*lease));
		lease.reset();
		std::unique_ptr<OperationLease> denied;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::audio,
			&denied) == MISTER_RESULT_INVALID_STATE);
	}
}

void TestCleanupAndRecoveryProtocolCompletionAreExplicit()
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	{
		FakeClock clock(3000);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		PrepareCleanup(broker, clock, profile, &generation, &epoch);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
			&lease) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> foreign;
		assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
			&foreign) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupCoreProtocolSession> session;
		assert(CoreProtocolAuthorityTestPeer::AcquireCleanup(*lease, broker,
			&session) == MISTER_RESULT_OK);
		const ProtocolMappingReleaseReceipt receipt = {
			MISTER_RESULT_OK, true, true, true, true, true,
			broker.mutation_sequence_for_test()};
		assert(CoreProtocolAuthorityTestPeer::CompleteCleanup(*foreign, broker,
			std::move(session), receipt) == MISTER_RESULT_INVALID_STATE);
		assert(session != nullptr);
		assert(CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
		assert(CoreProtocolAuthorityTestPeer::CompleteCleanup(*lease, broker,
			std::move(session), receipt) == MISTER_RESULT_OK);
		assert(!CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
		std::unique_ptr<CleanupCoreProtocolSession> duplicate;
		assert(CoreProtocolAuthorityTestPeer::AcquireCleanup(*lease, broker,
			&duplicate) == MISTER_RESULT_INVALID_STATE);
		assert(CoreProtocolAuthorityTestPeer::AcquireCleanup(*foreign, broker,
			&duplicate) == MISTER_RESULT_OK);
		assert(CoreProtocolAuthorityTestPeer::CompleteCleanup(*foreign, broker,
			std::move(duplicate), receipt) == MISTER_RESULT_OK);
	}
	{
		FakeClock clock(7000);
		HardwareBroker broker(clock);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL,
			9000, 12000, &epoch) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
			&lease) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> foreign;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
			&foreign) == MISTER_RESULT_INVALID_STATE);
		std::unique_ptr<RecoveryCoreProtocolSession> session;
		assert(CoreProtocolAuthorityTestPeer::AcquireRecovery(*lease, broker,
			&session) == MISTER_RESULT_OK);
		const ProtocolMappingReleaseReceipt receipt = {
			MISTER_RESULT_OK, true, true, true, true, true,
			broker.mutation_sequence_for_test()};
		assert(CoreProtocolAuthorityTestPeer::CompleteRecovery(*lease, broker,
			std::move(session), receipt) == MISTER_RESULT_OK);
		assert(!CoreProtocolAuthorityTestPeer::SessionCurrent(broker));
		std::unique_ptr<RecoveryCoreProtocolSession> duplicate;
		assert(CoreProtocolAuthorityTestPeer::AcquireRecovery(*lease, broker,
			&duplicate) == MISTER_RESULT_INVALID_STATE);
		lease.reset();
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
			&foreign) == MISTER_RESULT_OK);
		assert(CoreProtocolAuthorityTestPeer::AcquireRecovery(*foreign, broker,
			&duplicate) == MISTER_RESULT_OK);
		assert(CoreProtocolAuthorityTestPeer::CompleteRecovery(*foreign, broker,
			std::move(duplicate), receipt) == MISTER_RESULT_OK);
	}
}

void TestCleanupCannotOvertakeQuiesceReturn()
{
	FakeClock clock(6000);
	clock.PauseAfterWait();
	HardwareBroker broker(clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);

	std::unique_ptr<OperationLease> held;
	assert(broker.Begin(generation, OperationKind::input, 7000, &held) ==
		MISTER_RESULT_OK);
	QuiesceContext quiesce = {
		&broker, generation, 7000, MISTER_RESULT_PLATFORM};
	pthread_t quiesce_thread;
	assert(pthread_create(&quiesce_thread, nullptr, QuiesceThread,
		&quiesce) == 0);
	clock.WaitForBrokerWait();
	assert(broker.Quiesce(generation, 7000) == MISTER_RESULT_INVALID_STATE);
	held.reset();
	clock.WaitForPausedReturn();

	std::unique_ptr<CleanupEpoch> premature;
	assert(broker.BeginCleanup(generation, 8000, 11000, &premature) ==
		MISTER_RESULT_INVALID_STATE);
	clock.AllowWaitReturn();
	assert(pthread_join(quiesce_thread, nullptr) == 0);
	assert(quiesce.result == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(generation, 8000, 11000, &premature) ==
		MISTER_RESULT_OK);
}

void TestOwningTokenMayOutliveBrokerSafely()
{
	FakeClock clock(9000);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	std::unique_ptr<HardwareBroker> broker(new HardwareBroker(clock));
	PlatformGenerationId generation = 0;
	assert(broker->EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker->Begin(generation, OperationKind::input, 10000, &lease) ==
		MISTER_RESULT_OK);
	broker.reset();
	lease.reset();

	broker.reset(new HardwareBroker(clock));
	assert(broker->EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	assert(broker->Begin(generation, OperationKind::core_protocol, 10000,
		&lease) == MISTER_RESULT_OK);
	std::unique_ptr<ActiveCoreProtocolSession> session;
	assert(CoreProtocolAuthorityTestPeer::AcquireActive(*lease, *broker, profile,
		&session) == MISTER_RESULT_OK);
	broker.reset();
	assert(session != nullptr);
	session.reset();
	lease.reset();

	broker.reset(new HardwareBroker(clock));
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(*broker, clock, profile, &generation, &epoch);
	broker.reset();
	epoch.reset();
}

void TestForeignNullDuplicateAndDestructionRejection()
{
	FakeClock first_clock(100);
	FakeClock second_clock(200);
	HardwareBroker first(first_clock);
	HardwareBroker second(second_clock);
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId first_generation = 0;
	PlatformGenerationId second_generation = 0;
	std::unique_ptr<CleanupEpoch> first_epoch;
	std::unique_ptr<CleanupEpoch> second_epoch;
	PrepareCleanup(first, first_clock, profile, &first_generation, &first_epoch);
	PrepareCleanup(second, second_clock, profile, &second_generation, &second_epoch);

	std::unique_ptr<OperationLease> foreign_lease;
	assert(first.BeginCleanupOperation(*second_epoch, OperationKind::save,
		&foreign_lease) == MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<OperationLease> foreign_terminal;
	assert(second.BeginCleanupOperation(*second_epoch,
		OperationKind::terminal_fpga_cleanup, &foreign_terminal) ==
		MISTER_RESULT_OK);
	assert(first.ObserveContainment(*second_epoch, *foreign_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(first.Leave(first_generation, std::unique_ptr<CleanupEpoch>()) ==
		MISTER_RESULT_INVALID_ARGUMENT);

	std::unique_ptr<OperationLease> occupied;
	assert(first.BeginCleanupOperation(*first_epoch, OperationKind::save,
		&occupied) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> replacement;
	assert(first.BeginCleanupOperation(*first_epoch, OperationKind::content,
		&occupied) == MISTER_RESULT_INVALID_ARGUMENT);
	occupied.reset();

	first_epoch.reset();
	assert(first.Begin(first_generation, OperationKind::input,
		first_clock.NowMs() + 100, &replacement) == MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<CleanupEpoch> replacement_epoch;
	assert(first.BeginCleanup(first_generation, 2200, 5200,
		&replacement_epoch) == MISTER_RESULT_INVALID_STATE);

	assert(second.ObserveContainment(*second_epoch, *foreign_terminal) ==
		MISTER_RESULT_UNSUPPORTED);
	foreign_terminal.reset();
	assert(second.Leave(second_generation, std::move(second_epoch)) ==
		MISTER_RESULT_INVALID_STATE);
	assert(second_epoch.get() != nullptr);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestOwningTypesAreNotForgeable();
	mister::native::TestProtocolSessionsRequireExactAuthority();
	mister::native::TestEnterRequiresExactTrustedFixtureAuthority();
	mister::native::TestLeaseDurationQuiesceAndFreshGeneration();
	mister::native::TestBoundedQuiesceNeverReopensAdmission();
	mister::native::TestFailureLatchClosesPreissuedHardwareLeaseAdmission();
	mister::native::TestFailedProtocolCompletionLatchesAndRequiresBoundedDrain();
	mister::native::TestSuccessfulProtocolCompletionIsExplicitAndBrokerDominated();
	mister::native::TestInvalidProtocolOutcomeClosesAdmissionWhileLeaseIsLive();
	mister::native::TestForeignActiveRegistrationCannotInvalidateCurrentSession();
	mister::native::TestProtocolHandleAbandonmentRetainsBrokerFence();
	mister::native::TestCleanupAndRecoveryProtocolAbandonmentRetainBrokerFence();
	mister::native::TestCleanupAndRecoveryProtocolCompletionAreExplicit();
	mister::native::TestCleanupCannotOvertakeQuiesceReturn();
	mister::native::TestOwningTokenMayOutliveBrokerSafely();
	mister::native::TestForeignNullDuplicateAndDestructionRejection();
	return 0;
}
