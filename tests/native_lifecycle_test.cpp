// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_lifecycle.hpp"

#include <assert.h>
#include <stdint.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <vector>

namespace mister {
namespace native {
namespace {

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}

	uint64_t NowMs() const override
	{
		return now_ms_;
	}

	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t absolute_deadline_ms) override
	{
		return now_ms_ < absolute_deadline_ms;
	}

	void SetNow(uint64_t now_ms)
	{
		now_ms_ = now_ms;
	}

	void Advance(uint64_t delta_ms)
	{
		now_ms_ += delta_ms;
	}

private:
	uint64_t now_ms_;
};

enum class Event : uint8_t {
	preflight,
	retain_content,
	acquire_fpga,
	enable_bridges,
	start_core_protocol,
	start_video,
	start_audio,
	open_input_descriptors,
	open_save,
	start_scheduler,
	start_offload,
	stop_scheduler,
	reject_join_offload,
	flush_close_save,
	replay_digital_neutral,
	capture_digital_neutral,
	close_input_descriptors,
	stop_video,
	stop_audio,
	close_content,
	shutdown_core_protocol,
	terminal_fpga_cleanup,
	destruct_video,
	destruct_audio,
	destruct_core_protocol,
	destruct_fpga_mappings,
	destruct_scheduler,
	destruct_offload,
	destruct_input_descriptors,
	destruct_save,
	destruct_content
};

class FakeResources final : public NativePreflight,
	public NativeHardwareResources,
	public NativeSchedulerResource,
	public NativeOffloadResource,
	public NativeSaveResource,
	public NativeContentResource,
	public NativeInputDescriptorResource {
public:
	FakeResources(FakeClock &clock, HardwareBroker &broker)
		: clock_(clock), broker_(broker), failed_event_(Event::preflight),
		  fail_enabled_(false),
		  activation_delay_event_(Event::preflight), activation_delay_ms_(0),
		  expected_deadline_ms_(0), saw_wrong_deadline_(false),
		  content_saw_live_generation_(false), failure_after_acquire_(true),
		  captured_valid_{false, false}, captured_(), neutral_()
	{
	}

	NativeResourceSet Set()
	{
		return {*this, *this, *this, *this, *this, *this, *this};
	}

	void Fail(Event event)
	{
		failed_event_ = event;
		fail_enabled_ = true;
	}

	void FailBeforeAcquisition(Event event)
	{
		Fail(event);
		failure_after_acquire_ = false;
	}

	void ClearFailure()
	{
		fail_enabled_ = false;
		failure_after_acquire_ = true;
	}

	void AddDigitalNeutral(const NativeDigitalNeutral &neutral)
	{
		assert(neutral.player < kNativePlayerCount);
		captured_[neutral.player] = neutral;
		captured_valid_[neutral.player] = true;
	}

	void Delay(Event event, uint64_t delay_ms)
	{
		activation_delay_event_ = event;
		activation_delay_ms_ = delay_ms;
	}

	void ExpectActivationDeadline(uint64_t deadline_ms)
	{
		expected_deadline_ms_ = deadline_ms;
	}

	Result Validate(const NativeCoreProfile &, uint64_t absolute_deadline_ms) override
	{
		return RunBounded(Event::preflight, absolute_deadline_ms);
	}

	NativeAcquisitionOutcome AcquireFpga(const OperationLease &lease) override
	{
		return AcquireHardware(Event::acquire_fpga, lease);
	}

	NativeAcquisitionOutcome EnableBridges(const OperationLease &lease) override
	{
		return AcquireHardware(Event::enable_bridges, lease);
	}

	NativeAcquisitionOutcome StartCoreProtocol(const OperationLease &lease) override
	{
		return AcquireHardware(Event::start_core_protocol, lease);
	}

	NativeAcquisitionOutcome StartVideo(const OperationLease &lease) override
	{
		return AcquireHardware(Event::start_video, lease);
	}

	NativeAcquisitionOutcome StartAudio(const OperationLease &lease) override
	{
		return AcquireHardware(Event::start_audio, lease);
	}

	Result ReplayDigitalNeutral(const OperationLease &lease,
		const NativeDigitalNeutral &value) override
	{
		const Result result = RunHardware(Event::replay_digital_neutral, lease);
		if (result == MISTER_RESULT_OK) neutral_.push_back(value);
		return result;
	}

	Result StopVideo(const OperationLease &lease) override
	{
		return RunHardware(Event::stop_video, lease);
	}

	Result StopAudio(const OperationLease &lease) override
	{
		return RunHardware(Event::stop_audio, lease);
	}

	Result ShutdownCoreProtocol(const OperationLease &lease) override
	{
		return RunHardware(Event::shutdown_core_protocol, lease);
	}

	Result TerminalFpgaCleanup(const OperationLease &lease) override
	{
		return RunHardware(Event::terminal_fpga_cleanup, lease);
	}
	void CloseVideoForProcessExit() override { Record(Event::destruct_video); }
	void CloseAudioForProcessExit() override { Record(Event::destruct_audio); }
	void CloseCoreProtocolForProcessExit() override
	{
		Record(Event::destruct_core_protocol);
	}
	void CloseFpgaMappingsForProcessExit() override
	{
		Record(Event::destruct_fpga_mappings);
	}

	NativeAcquisitionOutcome StartScheduler(const OperationLease &lease) override
	{
		return Acquire(Event::start_scheduler, lease.absolute_deadline_ms());
	}
	Result StopScheduler(const OperationLease &lease) override
	{
		return RunBounded(Event::stop_scheduler, lease.absolute_deadline_ms());
	}
	void CloseSchedulerForProcessExit() override { Record(Event::destruct_scheduler); }
	NativeAcquisitionOutcome StartOffload(const OperationLease &lease) override
	{
		return Acquire(Event::start_offload, lease.absolute_deadline_ms());
	}
	Result RejectAndJoinOffload(const OperationLease &lease) override
	{
		return RunBounded(Event::reject_join_offload,
			lease.absolute_deadline_ms());
	}
	void CloseOffloadForProcessExit() override { Record(Event::destruct_offload); }
	NativeAcquisitionOutcome OpenSave(const OperationLease &lease) override
	{
		return Acquire(Event::open_save, lease.absolute_deadline_ms());
	}
	Result FlushAndCloseSave(const OperationLease &lease) override
	{
		return RunBounded(Event::flush_close_save, lease.absolute_deadline_ms());
	}
	void CloseSaveForProcessExit() override { Record(Event::destruct_save); }
	NativeAcquisitionOutcome RetainContent(uint64_t absolute_deadline_ms) override
	{
		content_saw_live_generation_ = broker_.has_live_generation_for_test();
		return Acquire(Event::retain_content, absolute_deadline_ms);
	}
	Result CloseContent(uint64_t absolute_deadline_ms) override
	{
		return RunBounded(Event::close_content, absolute_deadline_ms);
	}
	void CloseContentForProcessExit() override { Record(Event::destruct_content); }
	NativeAcquisitionOutcome OpenInputDescriptors(
		const OperationLease &lease) override
	{
		return Acquire(Event::open_input_descriptors,
			lease.absolute_deadline_ms());
	}
	Result CloseInputDescriptors(const OperationLease &lease) override
	{
		return RunBounded(Event::close_input_descriptors,
			lease.absolute_deadline_ms());
	}
	void CloseInputDescriptorsForProcessExit() override
	{
		Record(Event::destruct_input_descriptors);
	}
	Result CaptureDigitalNeutral(const OperationLease &lease,
		NativeDigitalNeutral *values, bool *valid, size_t count) override
	{
		if (values == nullptr || valid == nullptr || count != kNativePlayerCount)
			return MISTER_RESULT_INVALID_ARGUMENT;
		const Result result = RunBounded(Event::capture_digital_neutral,
			lease.absolute_deadline_ms());
		if (result != MISTER_RESULT_OK) return result;
		for (size_t player = 0; player < count; ++player) {
			values[player] = captured_[player];
			valid[player] = captured_valid_[player];
		}
		return MISTER_RESULT_OK;
	}

	size_t Count(Event event) const
	{
		return static_cast<size_t>(std::count(events.begin(), events.end(), event));
	}

	uint64_t LastHardwareDeadline(Event event) const
	{
		for (size_t index = hardware_events.size(); index != 0; --index) {
			if (hardware_events[index - 1] == event)
				return hardware_deadlines[index - 1];
		}
		return 0;
	}

	uint64_t LastBoundedDeadline(Event event) const
	{
		for (size_t index = bounded_events.size(); index != 0; --index) {
			if (bounded_events[index - 1] == event)
				return bounded_deadlines[index - 1];
		}
		return 0;
	}

	FakeClock &clock_;
	HardwareBroker &broker_;
	std::vector<Event> events;
	std::vector<Event> hardware_events;
	std::vector<uint64_t> hardware_deadlines;
	std::vector<Event> bounded_events;
	std::vector<uint64_t> bounded_deadlines;
	Event failed_event_;
	bool fail_enabled_;
	Event activation_delay_event_;
	uint64_t activation_delay_ms_;
	uint64_t expected_deadline_ms_;
	bool saw_wrong_deadline_;
	bool content_saw_live_generation_;
	bool failure_after_acquire_;
	bool captured_valid_[kNativePlayerCount];
	NativeDigitalNeutral captured_[kNativePlayerCount];
	std::vector<NativeDigitalNeutral> neutral_;

private:
	void Record(Event event)
	{
		events.push_back(event);
	}

	Result Run(Event event)
	{
		Record(event);
		if (event == activation_delay_event_) clock_.Advance(activation_delay_ms_);
		return fail_enabled_ && event == failed_event_ ?
			MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}

	Result RunBounded(Event event, uint64_t absolute_deadline_ms)
	{
		bounded_events.push_back(event);
		bounded_deadlines.push_back(absolute_deadline_ms);
		const Result result = Run(event);
		if (result != MISTER_RESULT_OK) return result;
		return clock_.NowMs() >= absolute_deadline_ms ?
			MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
	}

	Result RunHardware(Event event, const OperationLease &lease)
	{
		hardware_events.push_back(event);
		hardware_deadlines.push_back(lease.absolute_deadline_ms());
		if (expected_deadline_ms_ != 0 &&
			lease.absolute_deadline_ms() != expected_deadline_ms_) {
			saw_wrong_deadline_ = true;
		}
		return Run(event);
	}

	NativeAcquisitionOutcome Acquire(Event event, uint64_t absolute_deadline_ms)
	{
		const Result result = RunBounded(event, absolute_deadline_ms);
		return {result, result == MISTER_RESULT_OK || failure_after_acquire_};
	}

	NativeAcquisitionOutcome AcquireHardware(Event event,
		const OperationLease &lease)
	{
		Result result = RunHardware(event, lease);
		if (result == MISTER_RESULT_OK &&
			clock_.NowMs() >= lease.absolute_deadline_ms()) {
			result = MISTER_RESULT_DEADLINE;
		}
		return {result, result == MISTER_RESULT_OK || failure_after_acquire_};
	}
};

const Event kActivationEvents[] = {
	Event::acquire_fpga,
	Event::enable_bridges,
	Event::start_core_protocol,
	Event::start_video,
	Event::start_audio,
	Event::open_input_descriptors,
	Event::open_save,
	Event::start_scheduler,
	Event::start_offload
};

const Event kCleanupForFailedAcquisition[] = {
	Event::terminal_fpga_cleanup,
	Event::terminal_fpga_cleanup,
	Event::shutdown_core_protocol,
	Event::stop_video,
	Event::stop_audio,
	Event::close_input_descriptors,
	Event::flush_close_save,
	Event::stop_scheduler,
	Event::reject_join_offload
};

static_assert(sizeof(kActivationEvents) == sizeof(kCleanupForFailedAcquisition),
	"every acquisition failure needs a matching unwind observation");

struct Fixture {
	explicit Fixture(uint64_t now_ms)
		: clock(now_ms), broker(clock), resources(clock, broker), set(resources.Set()),
		  lifecycle(clock, broker, set),
		  profile(*FixtureNativeCoreProfile("snes"))
	{
	}

	FakeClock clock;
	HardwareBroker broker;
	FakeResources resources;
	NativeResourceSet set;
	NativeLifecycle lifecycle;
	const NativeCoreProfile &profile;
};

void TestPreflightFailureNeverAcquiresOwnership()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::preflight);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.lifecycle.state() == NativeLifecycleState::idle);
	assert(fixture.lifecycle.generation() == 0);
	assert(fixture.lifecycle.ledger().resource_flags == 0);
	assert(fixture.resources.events.size() == 1);
	assert(fixture.resources.events[0] == Event::preflight);
	assert(fixture.resources.Count(Event::retain_content) == 0);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 0);
}

void TestContentIsResolvedBeforeOwnershipAndRetainedOnFirstHardwareFailure()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::acquire_fpga);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.resources.events.size() >= 4);
	assert(fixture.resources.events[0] == Event::preflight);
	assert(fixture.resources.events[1] == Event::retain_content);
	assert(!fixture.resources.content_saw_live_generation_);
	assert(fixture.resources.events[2] == Event::acquire_fpga);
	assert(fixture.resources.events[3] == Event::close_content);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
}

void TestFirstAcquisitionIsLedgeredBeforeFollowingFailure()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::acquire_fpga);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.lifecycle.generation() != 0);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	assert(fixture.lifecycle.cleanup_timing().established);
	assert(fixture.resources.Count(Event::close_content) == 1);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CONTENT) == 0);
	assert((fixture.lifecycle.ledger().resource_flags & MISTER_RESOURCE_FPGA) != 0);
}

void TestSuccessfulInitializationRecordsEveryAcquisition()
{
	Fixture fixture(100);
	fixture.resources.ExpectActivationDeadline(1000);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	assert(fixture.lifecycle.state() == NativeLifecycleState::active);
	assert(fixture.lifecycle.generation() != 0);
	assert(fixture.lifecycle.ledger().resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(fixture.lifecycle.ledger().scheduler);
	assert(fixture.lifecycle.ledger().offload);
	assert(fixture.lifecycle.ledger().input_descriptors);
	assert(!fixture.resources.saw_wrong_deadline_);
	assert(fixture.resources.Count(Event::retain_content) == 1);
	for (size_t index = 0;
		index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]); ++index) {
		assert(fixture.resources.Count(kActivationEvents[index]) == 1);
	}
}

void TestFailureAfterEveryAcquisitionUnwindsWithoutChangingResult()
{
	for (size_t fail_index = 0;
		fail_index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
		++fail_index) {
		Fixture fixture(100);
		fixture.resources.Fail(kActivationEvents[fail_index]);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.lifecycle.latched_activation_result() ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
		assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
		assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
		assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5100);
		for (size_t index = 0; index <= fail_index; ++index)
			assert(fixture.resources.Count(kActivationEvents[index]) == 1);
		for (size_t index = fail_index + 1;
			index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
			++index) {
			assert(fixture.resources.Count(kActivationEvents[index]) == 0);
		}
		assert(fixture.resources.Count(kCleanupForFailedAcquisition[fail_index]) >= 1);
	}
}

void TestContentFailureOrOverrunNeverMintsOwnership()
{
	Fixture failed(100);
	failed.resources.FailBeforeAcquisition(Event::retain_content);
	assert(failed.lifecycle.ActivateFixtureForTest(failed.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(failed.lifecycle.state() == NativeLifecycleState::idle);
	assert(failed.lifecycle.generation() == 0);
	assert(failed.resources.Count(Event::acquire_fpga) == 0);
	assert(failed.resources.Count(Event::close_content) == 0);

	Fixture partial(100);
	partial.resources.Fail(Event::retain_content);
	assert(partial.lifecycle.ActivateFixtureForTest(partial.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(partial.lifecycle.state() == NativeLifecycleState::idle);
	assert(partial.lifecycle.generation() == 0);
	assert(partial.resources.Count(Event::close_content) == 1);
	assert(partial.lifecycle.ledger().resource_flags == 0);

	Fixture overrun(100);
	overrun.resources.Delay(Event::retain_content, 900);
	assert(overrun.lifecycle.ActivateFixtureForTest(overrun.profile, 1000) ==
		MISTER_RESULT_DEADLINE);
	assert(overrun.lifecycle.state() == NativeLifecycleState::idle);
	assert(overrun.lifecycle.generation() == 0);
	assert(overrun.resources.Count(Event::acquire_fpga) == 0);
	assert(overrun.resources.Count(Event::close_content) == 1);
	assert((overrun.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CONTENT) == 0);
}

void TestPreownershipContentCloseFailureBlocksReactivationAndRetriesLocally()
{
	Fixture fixture(100);
	fixture.resources.Delay(Event::retain_content, 900);
	fixture.resources.Fail(Event::close_content);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	assert(fixture.lifecycle.generation() == 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CONTENT) != 0);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 1000);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 3000);
	assert(fixture.resources.LastBoundedDeadline(Event::close_content) == 3000);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 2000) ==
		MISTER_RESULT_INVALID_STATE);
	assert(fixture.resources.Count(Event::retain_content) == 1);
	fixture.resources.ClearFailure();
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_OK);
	assert(fixture.lifecycle.state() == NativeLifecycleState::idle);
	assert(fixture.lifecycle.ledger().resource_flags == 0);
	assert(fixture.resources.Count(Event::close_content) == 2);
	assert(fixture.resources.LastBoundedDeadline(Event::close_content) == 3000);
}

void TestOverrunAfterEverySuccessfulAcquisitionIsLedgeredAndUnwound()
{
	for (size_t fail_index = 0;
		fail_index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
		++fail_index) {
		Fixture fixture(100);
		fixture.resources.Delay(kActivationEvents[fail_index], 900);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_DEADLINE);
		assert(fixture.lifecycle.latched_activation_result() ==
			MISTER_RESULT_DEADLINE);
		assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
		assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 1000);
		for (size_t index = 0; index <= fail_index; ++index)
			assert(fixture.resources.Count(kActivationEvents[index]) == 1);
		for (size_t index = fail_index + 1;
			index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
			++index) {
			assert(fixture.resources.Count(kActivationEvents[index]) == 0);
		}
	}
}

void TestActivationOverrunStaysFailedAndStartsFreshCleanupClocksOnce()
{
	Fixture fixture(100);
	fixture.resources.Delay(Event::start_offload, 100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 200) ==
		MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.latched_activation_result() == MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	const NativeCleanupTiming first = fixture.lifecycle.cleanup_timing();
	assert(first.established);
	assert(first.cleanup_start_ms == 200);
	assert(first.non_fpga_deadline_ms == 2200);
	assert(first.fpga_deadline_ms == 5200);
	const uint64_t epoch = fixture.lifecycle.cleanup_epoch_identity_for_test();
	assert(epoch != 0);

	fixture.clock.SetNow(300);
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 200);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2200);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5200);
	for (size_t index = 0;
		index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]); ++index) {
		assert(fixture.resources.Count(kActivationEvents[index]) == 1);
	}
}

void TestCleanupDeadlineArithmeticSaturatesWithoutWrapping()
{
	Fixture fixture(UINT64_MAX - 1000);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, UINT64_MAX) ==
		MISTER_RESULT_OK);
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(timing.cleanup_start_ms == UINT64_MAX - 1000);
	assert(timing.non_fpga_deadline_ms == UINT64_MAX);
	assert(timing.fpga_deadline_ms == UINT64_MAX);
}

void TestRetryRetainsEpochLedgersDeadlinesAndNeverReactivates()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const NativeDigitalNeutral first = {0, {0x02, 0}};
	const NativeDigitalNeutral second = {1, {0x03, 0}};
	fixture.resources.AddDigitalNeutral(first);
	fixture.resources.AddDigitalNeutral(second);

	fixture.resources.Fail(Event::reject_join_offload);
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const uint64_t epoch = fixture.lifecycle.cleanup_epoch_identity_for_test();
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(fixture.resources.Count(Event::stop_scheduler) == 1);
	assert(fixture.resources.Count(Event::reject_join_offload) == 1);
	assert(!fixture.lifecycle.ledger().scheduler);
	assert(fixture.lifecycle.ledger().offload);

	fixture.resources.ClearFailure();
	fixture.clock.SetNow(200);
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms ==
		timing.cleanup_start_ms);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms ==
		timing.non_fpga_deadline_ms);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms ==
		timing.fpga_deadline_ms);
	assert(fixture.resources.Count(Event::stop_scheduler) == 1);
	assert(fixture.resources.Count(Event::reject_join_offload) == 2);
	assert(fixture.resources.neutral_.size() == 2);
	assert(fixture.resources.neutral_[0].player == 0);
	assert(fixture.resources.neutral_[0].words[0] == 0x02);
	assert(fixture.resources.neutral_[0].words[1] == 0);
	assert(fixture.resources.neutral_[1].player == 1);
	assert(fixture.resources.neutral_[1].words[0] == 0x03);
	assert(fixture.resources.neutral_[1].words[1] == 0);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
	assert(fixture.resources.LastHardwareDeadline(
		Event::replay_digital_neutral) == timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(
		Event::terminal_fpga_cleanup) == timing.fpga_deadline_ms);
	for (size_t index = 0;
		index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]); ++index) {
		assert(fixture.resources.Count(kActivationEvents[index]) == 1);
	}

	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 2);
	assert(fixture.resources.Count(Event::replay_digital_neutral) == 2);
	assert(fixture.resources.LastHardwareDeadline(
		Event::terminal_fpga_cleanup) == timing.fpga_deadline_ms);
}

void TestNormalStopUsesTheNormativeOrder()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const NativeDigitalNeutral neutral = {0, {0x02, 0}};
	fixture.resources.AddDigitalNeutral(neutral);
	const size_t activation_events = fixture.resources.events.size();
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const Event expected[] = {
		Event::stop_scheduler,
		Event::reject_join_offload,
		Event::flush_close_save,
		Event::capture_digital_neutral,
		Event::replay_digital_neutral,
		Event::close_input_descriptors,
		Event::stop_video,
		Event::stop_audio,
		Event::close_content,
		Event::shutdown_core_protocol,
		Event::terminal_fpga_cleanup
	};
	assert(fixture.resources.events.size() == activation_events +
		sizeof(expected) / sizeof(expected[0]));
	for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index)
		assert(fixture.resources.events[activation_events + index] == expected[index]);
	assert(fixture.lifecycle.ledger().resource_flags ==
		(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		 MISTER_RESOURCE_CORE_INPUT));
	assert(!fixture.lifecycle.ledger().scheduler);
	assert(!fixture.lifecycle.ledger().offload);
	assert(!fixture.lifecycle.ledger().input_descriptors);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(fixture.resources.LastHardwareDeadline(Event::replay_digital_neutral) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastBoundedDeadline(Event::capture_digital_neutral) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastBoundedDeadline(Event::stop_scheduler) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastBoundedDeadline(Event::reject_join_offload) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::stop_video) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::stop_audio) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::shutdown_core_protocol) ==
		timing.fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::terminal_fpga_cleanup) ==
		timing.fpga_deadline_ms);
}

void TestCleanupFailureRetainsTheFailedResourceLedger()
{
	const Event failures[] = {
		Event::stop_scheduler,
		Event::reject_join_offload,
		Event::flush_close_save,
		Event::replay_digital_neutral,
		Event::close_input_descriptors,
		Event::stop_video,
		Event::stop_audio,
		Event::close_content,
		Event::shutdown_core_protocol,
		Event::terminal_fpga_cleanup
	};
	for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
		Fixture fixture(100);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_OK);
		const NativeDigitalNeutral neutral = {0, {0x02, 0}};
		fixture.resources.AddDigitalNeutral(neutral);
		fixture.resources.Fail(failures[index]);
		assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(!fixture.resources.events.empty());
		assert(fixture.resources.events.back() == failures[index]);
		const NativeResourceLedger ledger = fixture.lifecycle.ledger();
		switch (failures[index]) {
		case Event::stop_scheduler:
			assert(ledger.scheduler);
			break;
		case Event::reject_join_offload:
			assert(ledger.offload);
			break;
		case Event::flush_close_save:
			assert((ledger.resource_flags & MISTER_RESOURCE_SAVES) != 0);
			break;
		case Event::replay_digital_neutral:
			assert(ledger.digital_neutral_valid[0]);
			break;
		case Event::close_input_descriptors:
			assert(ledger.input_descriptors);
			assert((ledger.resource_flags & MISTER_RESOURCE_CORE_INPUT) != 0);
			break;
		case Event::stop_video:
			assert((ledger.resource_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0);
			break;
		case Event::stop_audio:
			assert((ledger.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0);
			break;
		case Event::close_content:
			assert((ledger.resource_flags & MISTER_RESOURCE_CONTENT) != 0);
			break;
		case Event::shutdown_core_protocol:
			assert((ledger.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0);
			break;
		case Event::terminal_fpga_cleanup:
			assert((ledger.resource_flags & MISTER_RESOURCE_FPGA) != 0);
			assert((ledger.resource_flags & MISTER_RESOURCE_BRIDGES) != 0);
			break;
		default:
			assert(false);
		}
	}
}

void TestCapturedNeutralMustMatchTheActiveProfile()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const NativeDigitalNeutral forged = {0, {0x03, 0}};
	fixture.resources.AddDigitalNeutral(forged);
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.resources.Count(Event::capture_digital_neutral) == 1);
	assert(fixture.resources.Count(Event::replay_digital_neutral) == 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CORE_INPUT) != 0);
}

void TestCleanupSuccessAtDeadlineDoesNotClearTheLedger()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	fixture.resources.Delay(Event::stop_video, 2000);
	assert(fixture.lifecycle.Stop() == MISTER_RESULT_DEADLINE);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_NATIVE_VIDEO) != 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_NATIVE_AUDIO) != 0);
	assert(fixture.resources.Count(Event::stop_video) == 1);
	assert(fixture.resources.Count(Event::stop_audio) == 0);
}

void TestDestructorClosesOnlyOwnedProcessResources()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeResources resources(clock, broker);
	NativeResourceSet set = resources.Set();
	{
		NativeLifecycle lifecycle(clock, broker, set);
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
		assert(lifecycle.ActivateFixtureForTest(profile, 1000) == MISTER_RESULT_OK);
	}
	assert(resources.Count(Event::destruct_scheduler) == 1);
	assert(resources.Count(Event::destruct_offload) == 1);
	assert(resources.Count(Event::destruct_input_descriptors) == 1);
	assert(resources.Count(Event::destruct_save) == 1);
	assert(resources.Count(Event::destruct_content) == 1);
	assert(resources.Count(Event::destruct_video) == 1);
	assert(resources.Count(Event::destruct_audio) == 1);
	assert(resources.Count(Event::destruct_core_protocol) == 1);
	assert(resources.Count(Event::destruct_fpga_mappings) == 1);
	assert(resources.Count(Event::replay_digital_neutral) == 0);
	assert(resources.Count(Event::terminal_fpga_cleanup) == 0);
	assert(resources.Count(Event::shutdown_core_protocol) == 0);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestPreflightFailureNeverAcquiresOwnership();
	mister::native::TestContentIsResolvedBeforeOwnershipAndRetainedOnFirstHardwareFailure();
	mister::native::TestFirstAcquisitionIsLedgeredBeforeFollowingFailure();
	mister::native::TestSuccessfulInitializationRecordsEveryAcquisition();
	mister::native::TestFailureAfterEveryAcquisitionUnwindsWithoutChangingResult();
	mister::native::TestContentFailureOrOverrunNeverMintsOwnership();
	mister::native::TestPreownershipContentCloseFailureBlocksReactivationAndRetriesLocally();
	mister::native::TestOverrunAfterEverySuccessfulAcquisitionIsLedgeredAndUnwound();
	mister::native::TestActivationOverrunStaysFailedAndStartsFreshCleanupClocksOnce();
	mister::native::TestCleanupDeadlineArithmeticSaturatesWithoutWrapping();
	mister::native::TestRetryRetainsEpochLedgersDeadlinesAndNeverReactivates();
	mister::native::TestNormalStopUsesTheNormativeOrder();
	mister::native::TestCleanupFailureRetainsTheFailedResourceLedger();
	mister::native::TestCleanupSuccessAtDeadlineDoesNotClearTheLedger();
	mister::native::TestCapturedNeutralMustMatchTheActiveProfile();
	mister::native::TestDestructorClosesOnlyOwnedProcessResources();
	return 0;
}
