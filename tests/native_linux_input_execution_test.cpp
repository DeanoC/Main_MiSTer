// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MISTER_NATIVE_INPUT_OPERATIONS_REACHABILITY_PROBE)
#include "runtime/native/linux/native_input_adapter.hpp"
using ForbiddenInputOperations =
	mister::native::linux_native::NativeLinuxInputOperations;
int main() { return sizeof(ForbiddenInputOperations *) == 0; }
#elif defined(MISTER_NATIVE_EXECUTION_OPERATIONS_REACHABILITY_PROBE)
#include "runtime/native/linux/native_scheduler_adapter.hpp"
using ForbiddenExecutionOperations =
	mister::native::linux_native::NativeLinuxExecutionOperations;
int main() { return sizeof(ForbiddenExecutionOperations *) == 0; }
#elif defined(MISTER_NATIVE_HARDWARE_VIEW_REACHABILITY_PROBE)
#include "runtime/native/hardware_broker.hpp"
#include <memory>
mister::native::Result AcquireForbiddenHardwareView(
	const mister::native::OperationLease &lease)
{
	std::unique_ptr<mister::native::HardwareLeaseView> view;
	return lease.AcquireHardwareLeaseView(&view);
}
int main() { return 0; }
#elif defined(MISTER_NATIVE_PROCESS_GUARD_REACHABILITY_PROBE)
#include "runtime/native/hardware_broker.hpp"
#include <memory>
mister::native::Result AcquireForbiddenProcessGuard(
	const mister::native::OperationLease &lease,
	mister::native::HardwareBroker &broker)
{
	std::unique_ptr<mister::native::ProcessOperationGuard> guard;
	return lease.AcquireProcessOperationGuard(broker,
		mister::native::OperationKind::scheduler, nullptr, &guard);
}
int main() { return 0; }
#else

#include "runtime/native/linux/native_input_adapter.hpp"
#include "runtime/native/linux/native_offload_adapter.hpp"
#include "runtime/native/linux/native_scheduler_adapter.hpp"

#include <assert.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace mister::native;
using namespace mister::native::linux_native;

namespace {

class TestClock final : public NativeClock {
public:
	explicit TestClock(uint64_t now = 10) : now_(now) {}
	uint64_t NowMs() const override { return now_.load(); }
	void Set(uint64_t now) { now_.store(now); }
	bool WaitUntil(std::condition_variable &condition,
		std::unique_lock<std::mutex> &lock, uint64_t deadline) override
	{
		condition.wait_for(lock, std::chrono::milliseconds(5));
		return NowMs() < deadline;
	}
private:
	std::atomic<uint64_t> now_;
};

class FakeHardware final : public NativeHardwareIo {
public:
	NativeSpiMutationResult Select(const HardwareLeaseView &,
		NativeSpiTarget) override
	{
		return {MISTER_RESULT_OK, true, true, true};
	}
	NativeSpiMutationResult WriteWordWithStrobeLow(
		const HardwareLeaseView &, uint16_t value) override
	{
		words.push_back(value);
		return {MISTER_RESULT_OK, true, true, true};
	}
	NativeSpiMutationResult SetStrobe(const HardwareLeaseView &, bool) override
	{
		return {MISTER_RESULT_OK, true, true, true};
	}
	Result ReadAckSample(const HardwareLeaseView &,
		NativeSpiAckSample *sample) override
	{
		sample->ack_high = (ack_index++ % 2) == 0;
		sample->fault = false;
		sample->response = 0;
		return MISTER_RESULT_OK;
	}
	NativeSpiMutationResult Deselect(const HardwareLeaseView &, NativeSpiTarget,
		uint64_t) override
	{
		return {MISTER_RESULT_OK, true, true, true};
	}
	size_t ack_index = 0;
	std::vector<uint16_t> words;
};

class FakeInputOps final : public NativeLinuxInputOperations {
public:
	Result OpenDirectory(uint64_t, int *fd) override
	{
		calls.push_back(1); *fd = 10; return Step();
	}
	Result OpenWatch(uint64_t, int *fd) override
	{
		calls.push_back(2); *fd = 11; return Step();
	}
	Result AddWatch(int, uint64_t, int *watch) override
	{
		calls.push_back(3); *watch = 12; return Step();
	}
	Result DiscoverDevice(size_t index, NativeLinuxInputCandidate *candidate) override
	{
		if (index >= candidates.size()) {
			if (expire_on_discovery_end) Expire();
			return MISTER_RESULT_UNSUPPORTED;
		}
		*candidate = candidates[index];
		if (expire_on_discovery_index == index + 1) Expire();
		return MISTER_RESULT_OK;
	}
	Result OpenDevice(const NativeLinuxInputCandidate &, uint64_t, int *fd) override
	{
		calls.push_back(10); *fd = next_fd++;
		fd_identities.push_back({*fd, candidates[capability_index].identity});
		return Step();
	}
	Result QueryCapabilities(int, NativeLinuxInputCapabilities *caps) override
	{
		calls.push_back(11); *caps = capabilities[capability_index++]; return Step();
	}
	Result SetGrab(int, bool grab) override
	{
		calls.push_back(grab ? 12 : 13); return Step();
	}
	Result ReadEvents(int descriptor, NativeLinuxInputEvent *events, size_t capacity,
		size_t *count, bool *partial) override
	{
		calls.push_back(14);
		if (expire_on_read) Expire();
		if (read_result != MISTER_RESULT_OK) return read_result;
		uint64_t identity = 0;
		for (const auto &entry : fd_identities)
			if (entry.first == descriptor) identity = entry.second;
		*count = 0;
		for (const auto &event : batch) {
			if (*count == capacity) break;
			if (event.device_identity == 0 || event.device_identity == identity)
				events[(*count)++] = event;
		}
		*partial = partial_read;
		return MISTER_RESULT_OK;
	}
	Result RemoveWatch(int, int) override
	{
		calls.push_back(20); return Step();
	}
	Result Close(int) override
	{
		calls.push_back(21); return Step();
	}
	uint64_t NowMs() const override { return now; }

	Result Step()
	{
		++step;
		if (expire_after_step != 0 && step == expire_after_step) {
			now = expire_at;
			if (clock != nullptr) clock->Set(expire_at);
		}
		if (fail_step != 0 && step == fail_step) return MISTER_RESULT_PLATFORM;
		return MISTER_RESULT_OK;
	}
	void Expire()
	{
		now = expire_at;
		if (clock != nullptr) clock->Set(expire_at);
	}
	void ResetFailure(size_t failure = 0) { fail_step = failure; step = 0; }
	size_t CallCount(int call) const
	{
		size_t count = 0;
		for (int observed : calls) count += observed == call ? 1u : 0u;
		return count;
	}

	uint64_t now = 10;
	uint64_t expire_at = 0;
	size_t expire_after_step = 0;
	size_t expire_on_discovery_index = 0;
	bool expire_on_discovery_end = false;
	bool expire_on_read = false;
	TestClock *clock = nullptr;
	int next_fd = 20;
	size_t capability_index = 0;
	size_t fail_step = 0;
	size_t step = 0;
	Result read_result = MISTER_RESULT_OK;
	bool partial_read = false;
	std::vector<int> calls;
	std::vector<NativeLinuxInputCandidate> candidates;
	std::vector<NativeLinuxInputCapabilities> capabilities;
	std::vector<NativeLinuxInputEvent> batch;
	std::vector<std::pair<int, uint64_t>> fd_identities;
};

class FakeExecutionOps final : public NativeLinuxExecutionOperations {
public:
	Result Initialize(uint64_t) override
	{
		std::unique_lock<std::mutex> lock(initialize_mutex);
		++initializes;
		initialize_entered = true;
		initialize_condition.notify_all();
		if (block_initialize)
			initialize_condition.wait(lock, [this] { return allow_initialize; });
		return initialize_result;
	}
	Result Release() override
	{
		std::unique_lock<std::mutex> lock(release_mutex);
		++releases;
		release_entered = true;
		release_condition.notify_all();
		if (block_release)
			release_condition.wait(lock, [this] { return allow_release; });
		return release_result;
	}
	uint64_t NowMs() const override { return now; }
	void WaitForRelease()
	{
		std::unique_lock<std::mutex> lock(release_mutex);
		release_condition.wait(lock, [this] { return release_entered; });
	}
	void AllowRelease()
	{
		std::lock_guard<std::mutex> lock(release_mutex);
		allow_release = true;
		release_condition.notify_all();
	}
	void WaitForInitialize()
	{
		std::unique_lock<std::mutex> lock(initialize_mutex);
		initialize_condition.wait(lock, [this] { return initialize_entered; });
	}
	void AllowInitialize()
	{
		std::lock_guard<std::mutex> lock(initialize_mutex);
		allow_initialize = true;
		initialize_condition.notify_all();
	}
	uint64_t now = 10;
	Result initialize_result = MISTER_RESULT_OK;
	Result release_result = MISTER_RESULT_OK;
	int initializes = 0;
	std::mutex initialize_mutex;
	std::condition_variable initialize_condition;
	bool block_initialize = false;
	bool initialize_entered = false;
	bool allow_initialize = false;
	std::atomic<int> releases{0};
	std::mutex release_mutex;
	std::condition_variable release_condition;
	bool block_release = false;
	bool release_entered = false;
	bool allow_release = false;
};

struct Fixture {
	Fixture()
		: profile(FixtureNativeCoreProfile("snes")), broker(clock), io(),
		  bus(clock, io), input(bus.input_port())
	{
		assert(profile != nullptr);
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	}
	std::unique_ptr<OperationLease> Lease(OperationKind kind, uint64_t deadline = 100)
	{
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, kind, deadline, &lease) == MISTER_RESULT_OK);
		return lease;
	}
	TestClock clock;
	const NativeCoreProfile *profile;
	HardwareBroker broker;
	PlatformGenerationId generation = 0;
	FakeHardware io;
	NativeSpiBus bus;
	NativeInput input;
};

NativeLinuxInputCandidate Candidate(uint64_t identity)
{
	NativeLinuxInputCandidate result = {};
	result.identity = identity;
	std::memcpy(result.path, "/private/device-sentinel", 25);
	return result;
}

NativeLinuxInputCapabilities DigitalCaps()
{
	NativeLinuxInputCapabilities caps = {};
	caps.digital_gamepad_buttons = true;
	return caps;
}

NativeLinuxInputEvent Button(uint16_t code, int32_t value,
	uint64_t device_identity = 1)
{
	NativeLinuxInputEvent event = {};
	event.kind = NativeLinuxInputEventKind::digital_button;
	event.code = code;
	event.value = value;
	event.device_identity = device_identity;
	return event;
}

void TestInputClassificationOrderingAndNeutralSnapshot()
{
	Fixture fixture;
	FakeInputOps ops;
	ops.candidates = {Candidate(1), Candidate(2), Candidate(3)};
	NativeLinuxInputCapabilities keyboard = {};
	keyboard.keyboard = true;
	ops.capabilities = {DigitalCaps(), DigitalCaps(), keyboard};
	NativeInputAdapter adapter(fixture.broker, fixture.clock, *fixture.profile,
		fixture.input, ops);
	auto open = fixture.Lease(OperationKind::input_descriptors);
	NativeAcquisitionOutcome outcome = adapter.OpenInputDescriptors(*open);
	assert(outcome.result == MISTER_RESULT_OK && outcome.acquired);
	assert(adapter.admitted_devices_for_test() == 2);
	assert(adapter.grabbed_devices_for_test() == 2);

	ops.batch = {Button(NativeLinuxInputCode::south, 1, 1),
		Button(NativeLinuxInputCode::east, 1, 1),
		Button(NativeLinuxInputCode::south, 1, 2)};
	auto input_lease = fixture.Lease(OperationKind::input);
	assert(adapter.PollInput(*input_lease) == MISTER_RESULT_OK);
	assert((fixture.io.words == std::vector<uint16_t>{2, 1, 2, 3, 3, 1}));

	NativeDigitalNeutral neutral[2] = {};
	bool valid[2] = {false, false};
	auto capture = fixture.Lease(OperationKind::input);
	assert(adapter.CaptureDigitalNeutral(*capture, neutral, valid, 2) ==
		MISTER_RESULT_OK);
	assert(valid[0] && valid[1]);
	assert(neutral[0].player == 0 && neutral[0].words[0] == 2 &&
		neutral[0].words[1] == 0);
	assert(neutral[1].player == 1 && neutral[1].words[0] == 3 &&
		neutral[1].words[1] == 0);
}

void TestInputUncertaintyUnsupportedAndSequenceOverflow()
{
	Fixture fixture;
	FakeInputOps ops;
	ops.candidates = {Candidate(1)};
	ops.capabilities = {DigitalCaps()};
	NativeInputAdapter adapter(fixture.broker, fixture.clock, *fixture.profile,
		fixture.input, ops);
	auto open = fixture.Lease(OperationKind::input_descriptors);
	assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
	auto input_lease = fixture.Lease(OperationKind::input);
	ops.batch = {{NativeLinuxInputEventKind::unsupported, 999, 1, 1},
		Button(NativeLinuxInputCode::south, 1)};
	assert(adapter.PollInput(*input_lease) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() == 2);

	ops.batch = {{NativeLinuxInputEventKind::syn_dropped, 0, 0, 1}};
	assert(adapter.PollInput(*input_lease) == MISTER_RESULT_PLATFORM);
	assert(adapter.player_uncertain_for_test(0));
	const size_t words = fixture.io.words.size();
	ops.batch = {Button(NativeLinuxInputCode::east, 1)};
	assert(adapter.PollInput(*input_lease) == MISTER_RESULT_INVALID_STATE);
	assert(fixture.io.words.size() == words);

	Fixture overflow_fixture;
	FakeInputOps overflow_ops;
	overflow_ops.candidates = {Candidate(1)};
	overflow_ops.capabilities = {DigitalCaps()};
	NativeInputAdapter overflow(overflow_fixture.broker, overflow_fixture.clock,
		*overflow_fixture.profile, overflow_fixture.input, overflow_ops);
	auto overflow_open = overflow_fixture.Lease(OperationKind::input_descriptors);
	assert(overflow.OpenInputDescriptors(*overflow_open).result == MISTER_RESULT_OK);
	overflow.set_player_sequence_for_test(0, UINT64_MAX);
	overflow_ops.batch = {Button(NativeLinuxInputCode::south, 1)};
	auto overflow_lease = overflow_fixture.Lease(OperationKind::input);
	assert(overflow.PollInput(*overflow_lease) == MISTER_RESULT_PLATFORM);
	assert(overflow_fixture.io.words.empty());
}

void TestInputRejectsEveryNonDigitalCapabilityBeforeGrab()
{
	for (size_t denied = 0; denied < 12; ++denied) {
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		NativeLinuxInputCapabilities caps = DigitalCaps();
		bool *fields[] = {&caps.keyboard, &caps.mouse, &caps.analog_absolute,
			&caps.touch, &caps.wheel, &caps.rumble_or_output,
			&caps.bluetooth_admin, &caps.uinput, &caps.fifo_or_signal,
			&caps.led, &caps.unknown, &caps.digital_gamepad_buttons};
		if (denied == 11) *fields[denied] = false;
		else *fields[denied] = true;
		ops.capabilities = {caps};
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
		assert(adapter.admitted_devices_for_test() == 0);
		assert(adapter.grabbed_devices_for_test() == 0);
	}
}

void TestProductionSourceClassificationRejectsVirtualUnknownAndWeakSources()
{
	NativeLinuxInputCapabilities base = {};
	NativeLinuxInputSourceProbe physical =
		{true, true, 0x03, false, true, false, 2};
	NativeLinuxInputCapabilities caps =
		ClassifyNativeLinuxInputSourceForTest(base, physical);
	assert(caps.digital_gamepad_buttons && !caps.uinput && !caps.unknown &&
		!caps.fifo_or_signal);

	NativeLinuxInputSourceProbe virtual_source =
		{true, true, 0x06, false, true, false, 8};
	caps = ClassifyNativeLinuxInputSourceForTest(base, virtual_source);
	assert(caps.uinput);

	NativeLinuxInputSourceProbe named_uinput =
		{true, true, 0x03, true, true, false, 8};
	caps = ClassifyNativeLinuxInputSourceForTest(base, named_uinput);
	assert(caps.uinput);

	NativeLinuxInputSourceProbe virtual_topology =
		{true, true, 0x03, false, true, true, 8};
	caps = ClassifyNativeLinuxInputSourceForTest(base, virtual_topology);
	assert(caps.uinput);

	NativeLinuxInputSourceProbe unknown =
		{true, true, 0x777, false, true, false, 8};
	caps = ClassifyNativeLinuxInputSourceForTest(base, unknown);
	assert(caps.unknown);

	NativeLinuxInputSourceProbe missing_topology =
		{true, true, 0x03, false, false, false, 8};
	caps = ClassifyNativeLinuxInputSourceForTest(base, missing_topology);
	assert(caps.unknown);

	NativeLinuxInputSourceProbe fifo =
		{false, true, 0x03, false, true, false, 8};
	caps = ClassifyNativeLinuxInputSourceForTest(base, fifo);
	assert(caps.fifo_or_signal);

	NativeLinuxInputSourceProbe weak =
		{true, true, 0x03, false, true, false, 1};
	caps = ClassifyNativeLinuxInputSourceForTest(base, weak);
	assert(!caps.digital_gamepad_buttons);
}

void TestInputHatPartialRemovalAndReadFailureStopWithoutFabrication()
{
	Fixture fixture;
	FakeInputOps ops;
	ops.candidates = {Candidate(1)};
	NativeLinuxInputCapabilities caps = {};
	caps.digital_hat = true;
	ops.capabilities = {caps};
	NativeInputAdapter adapter(fixture.broker, fixture.clock, *fixture.profile,
		fixture.input, ops);
	auto open = fixture.Lease(OperationKind::input_descriptors);
	assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
	auto input_lease = fixture.Lease(OperationKind::input);
	ops.batch = {{NativeLinuxInputEventKind::digital_hat_x, 0, -1, 1},
		{NativeLinuxInputEventKind::digital_hat_y, 0, 1, 1}};
	assert(adapter.PollInput(*input_lease) == MISTER_RESULT_OK);
	assert((fixture.io.words == std::vector<uint16_t>{2, 64, 2, 96}));
	const size_t words = fixture.io.words.size();
	ops.partial_read = true;
	ops.batch.clear();
	assert(adapter.PollInput(*input_lease) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words);

	Fixture read_fixture;
	FakeInputOps read_ops;
	read_ops.candidates = {Candidate(1)};
	read_ops.capabilities = {DigitalCaps()};
	NativeInputAdapter read_adapter(read_fixture.broker, read_fixture.clock,
		*read_fixture.profile, read_fixture.input, read_ops);
	auto read_open = read_fixture.Lease(OperationKind::input_descriptors);
	assert(read_adapter.OpenInputDescriptors(*read_open).result == MISTER_RESULT_OK);
	read_ops.read_result = MISTER_RESULT_PLATFORM;
	auto read_lease = read_fixture.Lease(OperationKind::input);
	assert(read_adapter.PollInput(*read_lease) == MISTER_RESULT_PLATFORM);
	assert(read_adapter.player_uncertain_for_test(0));
	assert(read_fixture.io.words.empty());

	Fixture removed_fixture;
	FakeInputOps removed_ops;
	removed_ops.candidates = {Candidate(1)};
	removed_ops.capabilities = {DigitalCaps()};
	NativeInputAdapter removed(removed_fixture.broker, removed_fixture.clock,
		*removed_fixture.profile, removed_fixture.input, removed_ops);
	auto removed_open = removed_fixture.Lease(OperationKind::input_descriptors);
	assert(removed.OpenInputDescriptors(*removed_open).result == MISTER_RESULT_OK);
	removed_ops.batch = {{NativeLinuxInputEventKind::device_removed, 0, 0, 1}};
	auto removed_lease = removed_fixture.Lease(OperationKind::input);
	assert(removed.PollInput(*removed_lease) == MISTER_RESULT_PLATFORM);
	assert(removed_fixture.io.words.empty());
}

void TestMixedDpadButtonsAndHatPreserveCompleteUnionInBothOrderings()
{
	{
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		NativeLinuxInputCapabilities caps = DigitalCaps();
		caps.digital_hat = true;
		ops.capabilities = {caps};
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
		auto input = fixture.Lease(OperationKind::input);
		ops.batch = {Button(NativeLinuxInputCode::left, 1),
			{NativeLinuxInputEventKind::digital_hat_x, 0, -1, 1},
			{NativeLinuxInputEventKind::digital_hat_x, 0, 0, 1}};
		assert(adapter.PollInput(*input) == MISTER_RESULT_OK);
		assert((fixture.io.words == std::vector<uint16_t>{2, 64}));
	}
	{
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		NativeLinuxInputCapabilities caps = DigitalCaps();
		caps.digital_hat = true;
		ops.capabilities = {caps};
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
		auto input = fixture.Lease(OperationKind::input);
		ops.batch = {
			{NativeLinuxInputEventKind::digital_hat_x, 0, -1, 1},
			Button(NativeLinuxInputCode::left, 1),
			Button(NativeLinuxInputCode::left, 0)};
		assert(adapter.PollInput(*input) == MISTER_RESULT_OK);
		assert((fixture.io.words == std::vector<uint16_t>{2, 64}));
	}
}

void TestEveryInputAcquisitionBoundaryRetainsPositiveResidue()
{
	for (size_t boundary = 1; boundary <= 6; ++boundary) {
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		ops.capabilities = {DigitalCaps()};
		ops.ResetFailure(boundary);
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		const NativeAcquisitionOutcome outcome =
			adapter.OpenInputDescriptors(*open);
		assert(outcome.result != MISTER_RESULT_OK);
		assert(outcome.acquired == (boundary > 1));
		if (boundary > 1) assert(adapter.owned_descriptors_for_test() != 0);
	}
}

void TestInputPartialAcquisitionCleanupRetryAndAuthority()
{
	Fixture fixture;
	FakeInputOps ops;
	ops.candidates = {Candidate(1)};
	ops.capabilities = {DigitalCaps()};
	ops.ResetFailure(4);
	NativeInputAdapter adapter(fixture.broker, fixture.clock, *fixture.profile,
		fixture.input, ops);
	auto wrong = fixture.Lease(OperationKind::scheduler);
	assert(adapter.OpenInputDescriptors(*wrong).result == MISTER_RESULT_INVALID_STATE);
	auto open = fixture.Lease(OperationKind::input_descriptors);
	NativeAcquisitionOutcome partial = adapter.OpenInputDescriptors(*open);
	assert(partial.result == MISTER_RESULT_PLATFORM && partial.acquired);
	assert(adapter.owned_descriptors_for_test() != 0);
	const size_t owned = adapter.owned_descriptors_for_test();
	const size_t directory_opens = ops.CallCount(1);
	ops.ResetFailure();
	const NativeAcquisitionOutcome denied_retry =
		adapter.OpenInputDescriptors(*open);
	assert(denied_retry.result == MISTER_RESULT_INVALID_STATE &&
		denied_retry.acquired);
	assert(adapter.owned_descriptors_for_test() == owned);
	assert(ops.CallCount(1) == directory_opens);

	wrong.reset();
	open.reset();
	assert(fixture.broker.LatchFailure(fixture.generation) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> close;
	assert(fixture.broker.BeginCleanupOperation(*epoch,
		OperationKind::input_descriptors, &close) == MISTER_RESULT_OK);
	ops.ResetFailure(1);
	assert(adapter.CloseInputDescriptors(*close) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const size_t residue = adapter.owned_descriptors_for_test();
	assert(residue != 0);
	ops.ResetFailure();
	assert(adapter.CloseInputDescriptors(*close) == MISTER_RESULT_OK);
	assert(adapter.owned_descriptors_for_test() == 0);
	assert(adapter.OpenInputDescriptors(*close).result == MISTER_RESULT_INVALID_STATE);
}

void TestPollValidatesAuthorityProfileAndDeadlineBeforeRead()
{
	Fixture fixture;
	FakeInputOps ops;
	ops.candidates = {Candidate(1)};
	ops.capabilities = {DigitalCaps()};
	NativeInputAdapter adapter(fixture.broker, fixture.clock, *fixture.profile,
		fixture.input, ops);
	auto open = fixture.Lease(OperationKind::input_descriptors);
	assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
	const size_t reads = ops.CallCount(14);

	auto wrong = fixture.Lease(OperationKind::scheduler);
	assert(adapter.PollInput(*wrong) == MISTER_RESULT_INVALID_STATE);
	assert(ops.CallCount(14) == reads);

	auto expired = fixture.Lease(OperationKind::input, 11);
	fixture.clock.Set(11);
	ops.now = 11;
	assert(adapter.PollInput(*expired) == MISTER_RESULT_DEADLINE);
	assert(ops.CallCount(14) == reads);

	TestClock foreign_clock;
	HardwareBroker foreign(foreign_clock);
	PlatformGenerationId foreign_generation = 0;
	assert(foreign.EnterFixtureForTest(*fixture.profile, &foreign_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> foreign_lease;
	assert(foreign.Begin(foreign_generation, OperationKind::input, 100,
		&foreign_lease) == MISTER_RESULT_OK);
	fixture.clock.Set(10);
	ops.now = 10;
	assert(adapter.PollInput(*foreign_lease) == MISTER_RESULT_INVALID_STATE);
	assert(ops.CallCount(14) == reads);

	const NativeCoreProfile *other = FixtureNativeCoreProfile("megadrive");
	assert(other != nullptr);
	TestClock profile_clock;
	HardwareBroker profile_broker(profile_clock);
	PlatformGenerationId profile_generation = 0;
	assert(profile_broker.EnterFixtureForTest(*other, &profile_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> profile_lease;
	assert(profile_broker.Begin(profile_generation, OperationKind::input, 100,
		&profile_lease) == MISTER_RESULT_OK);
	NativeInputEvent profile_event = {NativeInputKind::digital, 0, 1, true,
		false, 0, {0, 1}};
	SpiReceipt receipt = {};
	assert(fixture.input.Deliver(other, *profile_lease, profile_event, &receipt) ==
		MISTER_RESULT_OK);
	auto input = fixture.Lease(OperationKind::input);
	assert(adapter.PollInput(*input) == MISTER_RESULT_UNSUPPORTED);
	assert(ops.CallCount(14) == reads);
	assert(adapter.player_uncertain_for_test(0));
	assert(adapter.PollInput(*input) == MISTER_RESULT_UNSUPPORTED);
	assert(ops.CallCount(14) == reads);
}

void TestInputOpenDeadlineAfterEveryUnboundedBoundary()
{
	{
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		ops.capabilities = {DigitalCaps()};
		ops.clock = &fixture.clock;
		ops.now = 99;
		ops.expire_at = 100;
		ops.expire_on_discovery_index = 1;
		fixture.clock.Set(99);
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_DEADLINE);
		assert(ops.CallCount(10) == 0);
	}
	{
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		ops.capabilities = {DigitalCaps()};
		ops.clock = &fixture.clock;
		ops.now = 99;
		ops.expire_at = 100;
		ops.expire_after_step = 5;
		fixture.clock.Set(99);
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_DEADLINE);
		assert(ops.CallCount(12) == 0);
	}
	{
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		NativeLinuxInputCapabilities denied = DigitalCaps();
		denied.keyboard = true;
		ops.capabilities = {denied};
		ops.clock = &fixture.clock;
		ops.now = 99;
		ops.expire_at = 100;
		ops.expire_after_step = 6;
		fixture.clock.Set(99);
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_DEADLINE);
	}
	{
		Fixture fixture;
		FakeInputOps ops;
		ops.clock = &fixture.clock;
		ops.now = 99;
		ops.expire_at = 100;
		ops.expire_on_discovery_end = true;
		fixture.clock.Set(99);
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_DEADLINE);
	}
}

void TestInputCleanupDeadlineStopsBeforeLaterDescriptors()
{
	Fixture fixture;
	FakeInputOps ops;
	ops.candidates = {Candidate(1)};
	ops.capabilities = {DigitalCaps()};
	NativeInputAdapter adapter(fixture.broker, fixture.clock, *fixture.profile,
		fixture.input, ops);
	auto open = fixture.Lease(OperationKind::input_descriptors);
	assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
	open.reset();
	assert(fixture.broker.LatchFailure(fixture.generation) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> close;
	assert(fixture.broker.BeginCleanupOperation(*epoch,
		OperationKind::input_descriptors, &close) == MISTER_RESULT_OK);
	ops.ResetFailure();
	ops.clock = &fixture.clock;
	ops.now = 99;
	fixture.clock.Set(99);
	ops.expire_after_step = 1;
	ops.expire_at = 100;
	assert(adapter.CloseInputDescriptors(*close) == MISTER_RESULT_DEADLINE);
	assert(adapter.owned_descriptors_for_test() == 3);
}

void TestEveryInputCleanupBoundaryRetainsRetryResidue()
{
	for (size_t boundary = 1; boundary <= 5; ++boundary) {
		Fixture fixture;
		FakeInputOps ops;
		ops.candidates = {Candidate(1)};
		ops.capabilities = {DigitalCaps()};
		NativeInputAdapter adapter(fixture.broker, fixture.clock,
			*fixture.profile, fixture.input, ops);
		auto open = fixture.Lease(OperationKind::input_descriptors);
		assert(adapter.OpenInputDescriptors(*open).result == MISTER_RESULT_OK);
		open.reset();
		assert(fixture.broker.LatchFailure(fixture.generation) ==
			MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200,
			&epoch) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> close;
		assert(fixture.broker.BeginCleanupOperation(*epoch,
			OperationKind::input_descriptors, &close) == MISTER_RESULT_OK);
		ops.ResetFailure(boundary);
		assert(adapter.CloseInputDescriptors(*close) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(adapter.owned_descriptors_for_test() != 0);
		ops.ResetFailure();
		assert(adapter.CloseInputDescriptors(*close) == MISTER_RESULT_OK);
		assert(adapter.owned_descriptors_for_test() == 0);
	}
}

Result PlatformFailure(void *)
{
	return MISTER_RESULT_PLATFORM;
}

Result Increment(void *context)
{
	++*static_cast<std::atomic<int> *>(context);
	return MISTER_RESULT_OK;
}

struct BlockingCallback {
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool release = false;
};

Result Block(void *context)
{
	BlockingCallback &block = *static_cast<BlockingCallback *>(context);
	std::unique_lock<std::mutex> lock(block.mutex);
	block.entered = true;
	block.condition.notify_all();
	block.condition.wait(lock, [&block] { return block.release; });
	return MISTER_RESULT_OK;
}

void TestSchedulerDispatchDrainAndNoReopen()
{
	Fixture fixture;
	FakeExecutionOps ops;
	NativeSchedulerAdapter scheduler(fixture.broker, fixture.clock, ops);
	auto wrong = fixture.Lease(OperationKind::offload);
	assert(scheduler.StartScheduler(*wrong).result == MISTER_RESULT_INVALID_STATE);
	auto start = fixture.Lease(OperationKind::scheduler);
	assert(scheduler.StartScheduler(*start).result == MISTER_RESULT_OK);
	std::atomic<int> count(0);
	auto dispatch = fixture.Lease(OperationKind::scheduler);
	assert(scheduler.Dispatch(*dispatch, &Increment, &count) == MISTER_RESULT_OK);
	assert(count.load() == 1);

	BlockingCallback block;
	auto held = fixture.Lease(OperationKind::scheduler);
	std::thread worker([&] { assert(scheduler.Dispatch(*held, &Block, &block) ==
		MISTER_RESULT_OK); });
	{
		std::unique_lock<std::mutex> lock(block.mutex);
		block.condition.wait(lock, [&block] { return block.entered; });
	}
	start.reset(); dispatch.reset(); wrong.reset();
	assert(scheduler.Dispatch(*held, &Increment, &count) ==
		MISTER_RESULT_INVALID_STATE);
	held.reset();
	std::atomic<bool> quiesced(false);
	Result quiesce_result = MISTER_RESULT_INVALID_STATE;
	std::thread quiescer([&] {
		quiesce_result = fixture.broker.Quiesce(fixture.generation, 100);
		quiesced.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(15));
	assert(!quiesced.load());
	{
		std::lock_guard<std::mutex> lock(block.mutex);
		block.release = true;
	}
	block.condition.notify_all();
	worker.join();
	quiescer.join();
	assert(quiesce_result == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> stop;
	assert(fixture.broker.BeginCleanupOperation(*epoch, OperationKind::scheduler,
		&stop) == MISTER_RESULT_OK);
	assert(scheduler.StopScheduler(*stop) == MISTER_RESULT_OK);
	assert(scheduler.Dispatch(*stop, &Increment, &count) == MISTER_RESULT_INVALID_STATE);
	assert(scheduler.StopScheduler(*stop) == MISTER_RESULT_OK);
}

void TestOffloadBoundedAdmissionJoinAndProcessExit()
{
	Fixture fixture;
	FakeExecutionOps ops;
	NativeOffloadAdapter offload(fixture.broker, fixture.clock, ops);
	auto start = fixture.Lease(OperationKind::offload);
	assert(offload.StartOffload(*start).result == MISTER_RESULT_OK);
	std::atomic<int> count(0);
	auto submit = fixture.Lease(OperationKind::offload);
	assert(offload.Submit(*submit, &Increment, &count) == MISTER_RESULT_OK);
	assert(count.load() == 1);

	BlockingCallback block;
	auto held = fixture.Lease(OperationKind::offload);
	std::thread worker([&] { assert(offload.Submit(*held, &Block, &block) ==
		MISTER_RESULT_OK); });
	{
		std::unique_lock<std::mutex> lock(block.mutex);
		block.condition.wait(lock, [&block] { return block.entered; });
	}
	offload.CloseOffloadForProcessExit();
	assert(ops.releases.load() == 0);
	auto rejected = fixture.Lease(OperationKind::offload);
	assert(offload.Submit(*rejected, &Increment, &count) ==
		MISTER_RESULT_INVALID_STATE);
	{
		std::lock_guard<std::mutex> lock(block.mutex);
		block.release = true;
	}
	block.condition.notify_all();
	worker.join();
	assert(ops.releases.load() == 1);
}

void TestProcessExitAndDestructionRetainLiveCallbackState()
{
	{
		Fixture fixture;
		FakeExecutionOps ops;
		std::unique_ptr<NativeSchedulerAdapter> scheduler(
			new NativeSchedulerAdapter(fixture.broker, fixture.clock, ops));
		auto start = fixture.Lease(OperationKind::scheduler);
		assert(scheduler->StartScheduler(*start).result == MISTER_RESULT_OK);
		start.reset();
		auto dispatch = fixture.Lease(OperationKind::scheduler);
		BlockingCallback block;
		NativeSchedulerAdapter *raw = scheduler.get();
		std::thread worker([&] {
			assert(raw->Dispatch(*dispatch, &Block, &block) == MISTER_RESULT_OK);
		});
		{
			std::unique_lock<std::mutex> lock(block.mutex);
			block.condition.wait(lock, [&block] { return block.entered; });
		}
		std::atomic<bool> destroyed(false);
		std::thread destroyer([&] { scheduler.reset(); destroyed.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		assert(!destroyed.load());
		assert(ops.releases.load() == 0);
		{
			std::lock_guard<std::mutex> lock(block.mutex);
			block.release = true;
		}
		block.condition.notify_all();
		worker.join();
		destroyer.join();
		assert(destroyed.load());
		assert(ops.releases.load() == 1);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		std::unique_ptr<NativeOffloadAdapter> offload(
			new NativeOffloadAdapter(fixture.broker, fixture.clock, ops));
		auto start = fixture.Lease(OperationKind::offload);
		assert(offload->StartOffload(*start).result == MISTER_RESULT_OK);
		start.reset();
		auto submit = fixture.Lease(OperationKind::offload);
		BlockingCallback block;
		NativeOffloadAdapter *raw = offload.get();
		std::thread worker([&] {
			assert(raw->Submit(*submit, &Block, &block) == MISTER_RESULT_OK);
		});
		{
			std::unique_lock<std::mutex> lock(block.mutex);
			block.condition.wait(lock, [&block] { return block.entered; });
		}
		std::atomic<bool> destroyed(false);
		std::thread destroyer([&] { offload.reset(); destroyed.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		assert(!destroyed.load());
		assert(ops.releases.load() == 0);
		{
			std::lock_guard<std::mutex> lock(block.mutex);
			block.release = true;
		}
		block.condition.notify_all();
		worker.join();
		destroyer.join();
		assert(destroyed.load());
		assert(ops.releases.load() == 1);
	}
}

void TestDestructionRetainsPartialInitializationState()
{
	{
		Fixture fixture;
		FakeExecutionOps ops;
		ops.block_initialize = true;
		std::unique_ptr<NativeSchedulerAdapter> scheduler(
			new NativeSchedulerAdapter(fixture.broker, fixture.clock, ops));
		auto start = fixture.Lease(OperationKind::scheduler);
		NativeSchedulerAdapter *raw = scheduler.get();
		NativeAcquisitionOutcome outcome = {};
		std::thread starter([&] { outcome = raw->StartScheduler(*start); });
		ops.WaitForInitialize();
		std::atomic<bool> destroyed(false);
		std::thread destroyer([&] { scheduler.reset(); destroyed.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		assert(!destroyed.load());
		assert(ops.releases.load() == 0);
		ops.AllowInitialize();
		starter.join();
		destroyer.join();
		assert(outcome.acquired);
		assert(destroyed.load());
		assert(ops.releases.load() == 1);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		ops.block_initialize = true;
		std::unique_ptr<NativeOffloadAdapter> offload(
			new NativeOffloadAdapter(fixture.broker, fixture.clock, ops));
		auto start = fixture.Lease(OperationKind::offload);
		NativeOffloadAdapter *raw = offload.get();
		NativeAcquisitionOutcome outcome = {};
		std::thread starter([&] { outcome = raw->StartOffload(*start); });
		ops.WaitForInitialize();
		std::atomic<bool> destroyed(false);
		std::thread destroyer([&] { offload.reset(); destroyed.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		assert(!destroyed.load());
		assert(ops.releases.load() == 0);
		ops.AllowInitialize();
		starter.join();
		destroyer.join();
		assert(outcome.acquired);
		assert(destroyed.load());
		assert(ops.releases.load() == 1);
	}
}

void TestConcurrentCleanupRejectsDuplicatePrimitiveRelease()
{
	{
		Fixture fixture;
		FakeExecutionOps ops;
		ops.block_release = true;
		NativeSchedulerAdapter scheduler(fixture.broker, fixture.clock, ops);
		auto start = fixture.Lease(OperationKind::scheduler);
		assert(scheduler.StartScheduler(*start).result == MISTER_RESULT_OK);
		start.reset();
		assert(fixture.broker.Quiesce(fixture.generation, 100) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> first;
		std::unique_ptr<OperationLease> second;
		assert(fixture.broker.BeginCleanupOperation(*epoch,
			OperationKind::scheduler, &first) == MISTER_RESULT_OK);
		assert(fixture.broker.BeginCleanupOperation(*epoch,
			OperationKind::scheduler, &second) == MISTER_RESULT_OK);
		Result first_result = MISTER_RESULT_INVALID_STATE;
		std::thread releaser([&] { first_result = scheduler.StopScheduler(*first); });
		ops.WaitForRelease();
		assert(scheduler.StopScheduler(*second) == MISTER_RESULT_INVALID_STATE);
		assert(ops.releases.load() == 1);
		ops.AllowRelease();
		releaser.join();
		assert(first_result == MISTER_RESULT_OK);
		assert(scheduler.StopScheduler(*second) == MISTER_RESULT_OK);
		assert(ops.releases.load() == 1);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		ops.block_release = true;
		NativeOffloadAdapter offload(fixture.broker, fixture.clock, ops);
		auto start = fixture.Lease(OperationKind::offload);
		assert(offload.StartOffload(*start).result == MISTER_RESULT_OK);
		start.reset();
		assert(fixture.broker.Quiesce(fixture.generation, 100) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> first;
		std::unique_ptr<OperationLease> second;
		assert(fixture.broker.BeginCleanupOperation(*epoch, OperationKind::offload,
			&first) == MISTER_RESULT_OK);
		assert(fixture.broker.BeginCleanupOperation(*epoch, OperationKind::offload,
			&second) == MISTER_RESULT_OK);
		Result first_result = MISTER_RESULT_INVALID_STATE;
		std::thread releaser([&] {
			first_result = offload.RejectAndJoinOffload(*first);
		});
		ops.WaitForRelease();
		assert(offload.RejectAndJoinOffload(*second) ==
			MISTER_RESULT_INVALID_STATE);
		assert(ops.releases.load() == 1);
		ops.AllowRelease();
		releaser.join();
		assert(first_result == MISTER_RESULT_OK);
		assert(offload.RejectAndJoinOffload(*second) == MISTER_RESULT_OK);
		assert(ops.releases.load() == 1);
	}
}

void TestExecutionCleanupDeadlineFailsBeforeRelease()
{
	{
		Fixture fixture;
		FakeExecutionOps ops;
		NativeSchedulerAdapter scheduler(fixture.broker, fixture.clock, ops);
		auto start = fixture.Lease(OperationKind::scheduler);
		assert(scheduler.StartScheduler(*start).result == MISTER_RESULT_OK);
		start.reset();
		assert(fixture.broker.Quiesce(fixture.generation, 100) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> stop;
		assert(fixture.broker.BeginCleanupOperation(*epoch,
			OperationKind::scheduler, &stop) == MISTER_RESULT_OK);
		fixture.clock.Set(100);
		ops.now = 100;
		assert(scheduler.StopScheduler(*stop) == MISTER_RESULT_DEADLINE);
		assert(ops.releases.load() == 0);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		NativeOffloadAdapter offload(fixture.broker, fixture.clock, ops);
		auto start = fixture.Lease(OperationKind::offload);
		assert(offload.StartOffload(*start).result == MISTER_RESULT_OK);
		start.reset();
		assert(fixture.broker.Quiesce(fixture.generation, 100) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> join;
		assert(fixture.broker.BeginCleanupOperation(*epoch, OperationKind::offload,
			&join) == MISTER_RESULT_OK);
		fixture.clock.Set(100);
		ops.now = 100;
		assert(offload.RejectAndJoinOffload(*join) == MISTER_RESULT_DEADLINE);
		assert(ops.releases.load() == 0);
	}
}

void TestExpiredForeignDestroyedAndBusyGuards()
{
	Fixture fixture;
	FakeExecutionOps ops;
	NativeSchedulerAdapter scheduler(fixture.broker, fixture.clock, ops);
	auto expired = fixture.Lease(OperationKind::scheduler, 11);
	fixture.clock.Set(11); ops.now = 11;
	assert(scheduler.StartScheduler(*expired).result == MISTER_RESULT_DEADLINE);

	TestClock foreign_clock;
	HardwareBroker foreign(foreign_clock);
	PlatformGenerationId foreign_generation = 0;
	assert(foreign.EnterFixtureForTest(*fixture.profile, &foreign_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> foreign_lease;
	assert(foreign.Begin(foreign_generation, OperationKind::scheduler, 100,
		&foreign_lease) == MISTER_RESULT_OK);
	fixture.clock.Set(10); ops.now = 10;
	assert(scheduler.StartScheduler(*foreign_lease).result ==
		MISTER_RESULT_INVALID_STATE);

	TestClock dead_clock;
	std::unique_ptr<HardwareBroker> dead(new HardwareBroker(dead_clock));
	PlatformGenerationId dead_generation = 0;
	assert(dead->EnterFixtureForTest(*fixture.profile, &dead_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> dead_lease;
	assert(dead->Begin(dead_generation, OperationKind::scheduler, 100,
		&dead_lease) == MISTER_RESULT_OK);
	dead.reset();
	assert(scheduler.StartScheduler(*dead_lease).result ==
		MISTER_RESULT_INVALID_STATE);
}

void TestHeldGuardPreventsBrokerDestructionBisectingCallback()
{
	TestClock clock;
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	std::unique_ptr<HardwareBroker> broker(new HardwareBroker(clock));
	PlatformGenerationId generation = 0;
	assert(broker->EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	FakeExecutionOps ops;
	NativeSchedulerAdapter scheduler(*broker, clock, ops);
	std::unique_ptr<OperationLease> start;
	assert(broker->Begin(generation, OperationKind::scheduler, 100, &start) ==
		MISTER_RESULT_OK);
	assert(scheduler.StartScheduler(*start).result == MISTER_RESULT_OK);
	start.reset();
	std::unique_ptr<OperationLease> dispatch;
	assert(broker->Begin(generation, OperationKind::scheduler, 100, &dispatch) ==
		MISTER_RESULT_OK);
	BlockingCallback block;
	std::thread worker([&] {
		assert(scheduler.Dispatch(*dispatch, &Block, &block) == MISTER_RESULT_OK);
	});
	{
		std::unique_lock<std::mutex> lock(block.mutex);
		block.condition.wait(lock, [&block] { return block.entered; });
	}
	dispatch.reset();
	std::atomic<bool> destroyed(false);
	std::thread destroyer([&] { broker.reset(); destroyed.store(true); });
	std::this_thread::sleep_for(std::chrono::milliseconds(15));
	assert(!destroyed.load());
	{
		std::lock_guard<std::mutex> lock(block.mutex);
		block.release = true;
	}
	block.condition.notify_all();
	worker.join();
	destroyer.join();
	assert(destroyed.load());
}

void TestExecutionStartReleaseFailuresAndDeadlineLatch()
{
	{
		Fixture fixture;
		FakeExecutionOps ops;
		ops.initialize_result = MISTER_RESULT_PLATFORM;
		NativeSchedulerAdapter scheduler(fixture.broker, fixture.clock, ops);
		auto lease = fixture.Lease(OperationKind::scheduler);
		const NativeAcquisitionOutcome outcome = scheduler.StartScheduler(*lease);
		assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		NativeSchedulerAdapter scheduler(fixture.broker, fixture.clock, ops);
		auto start = fixture.Lease(OperationKind::scheduler);
		assert(scheduler.StartScheduler(*start).result == MISTER_RESULT_OK);
		auto dispatch = fixture.Lease(OperationKind::scheduler);
		assert(scheduler.Dispatch(*dispatch, &PlatformFailure, nullptr) ==
			MISTER_RESULT_PLATFORM);
		start.reset(); dispatch.reset();
		assert(fixture.broker.LatchFailure(fixture.generation) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> stop;
		assert(fixture.broker.BeginCleanupOperation(*epoch,
			OperationKind::scheduler, &stop) == MISTER_RESULT_OK);
		ops.release_result = MISTER_RESULT_PLATFORM;
		assert(scheduler.StopScheduler(*stop) == MISTER_RESULT_CLEANUP_INCOMPLETE);
		ops.release_result = MISTER_RESULT_OK;
		assert(scheduler.StopScheduler(*stop) == MISTER_RESULT_OK);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		ops.initialize_result = MISTER_RESULT_PLATFORM;
		NativeOffloadAdapter offload(fixture.broker, fixture.clock, ops);
		auto lease = fixture.Lease(OperationKind::offload);
		const NativeAcquisitionOutcome outcome = offload.StartOffload(*lease);
		assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired);
	}
	{
		Fixture fixture;
		FakeExecutionOps ops;
		NativeOffloadAdapter offload(fixture.broker, fixture.clock, ops);
		auto start = fixture.Lease(OperationKind::offload);
		assert(offload.StartOffload(*start).result == MISTER_RESULT_OK);
		auto run = fixture.Lease(OperationKind::offload);
		assert(offload.Submit(*run, &PlatformFailure, nullptr) ==
			MISTER_RESULT_PLATFORM);
		run.reset();
		start.reset();
		assert(fixture.broker.LatchFailure(fixture.generation) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(fixture.broker.BeginCleanup(fixture.generation, 100, 200, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> stop;
		assert(fixture.broker.BeginCleanupOperation(*epoch, OperationKind::offload,
			&stop) == MISTER_RESULT_OK);
		ops.release_result = MISTER_RESULT_PLATFORM;
		assert(offload.RejectAndJoinOffload(*stop) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		ops.release_result = MISTER_RESULT_OK;
		assert(offload.RejectAndJoinOffload(*stop) == MISTER_RESULT_OK);
		std::atomic<int> count(0);
		assert(offload.Submit(*stop, &Increment, &count) ==
			MISTER_RESULT_INVALID_STATE);
	}
}

} // namespace

int main()
{
	TestInputClassificationOrderingAndNeutralSnapshot();
	TestInputUncertaintyUnsupportedAndSequenceOverflow();
	TestInputRejectsEveryNonDigitalCapabilityBeforeGrab();
	TestProductionSourceClassificationRejectsVirtualUnknownAndWeakSources();
	TestInputHatPartialRemovalAndReadFailureStopWithoutFabrication();
	TestMixedDpadButtonsAndHatPreserveCompleteUnionInBothOrderings();
	TestEveryInputAcquisitionBoundaryRetainsPositiveResidue();
	TestInputPartialAcquisitionCleanupRetryAndAuthority();
	TestPollValidatesAuthorityProfileAndDeadlineBeforeRead();
	TestInputOpenDeadlineAfterEveryUnboundedBoundary();
	TestInputCleanupDeadlineStopsBeforeLaterDescriptors();
	TestEveryInputCleanupBoundaryRetainsRetryResidue();
	TestSchedulerDispatchDrainAndNoReopen();
	TestOffloadBoundedAdmissionJoinAndProcessExit();
	TestProcessExitAndDestructionRetainLiveCallbackState();
	TestDestructionRetainsPartialInitializationState();
	TestConcurrentCleanupRejectsDuplicatePrimitiveRelease();
	TestExecutionCleanupDeadlineFailsBeforeRelease();
	TestExpiredForeignDestroyedAndBusyGuards();
	TestHeldGuardPreventsBrokerDestructionBisectingCallback();
	TestExecutionStartReleaseFailuresAndDeadlineLatch();
	return 0;
}

#endif
