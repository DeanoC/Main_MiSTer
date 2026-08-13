// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_mmio_adapter.hpp"
#if defined(MISTER_NATIVE_MMIO_REACHABILITY_PROBE)
using LeakedRawMmioAuthority =
	mister::native::linux_native::NativeMmioTestOperations;
int main()
{
	return sizeof(LeakedRawMmioAuthority *) == 0;
}
#else
#include "runtime/native/native_core_profile.hpp"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace mister {
namespace native {
namespace linux_native {
namespace {

static_assert(!std::is_default_constructible<
	NativeContainmentIo::Access>::value,
	"containment authority must not be caller-constructible");
static_assert(!std::is_copy_constructible<
	NativeContainmentIo::Access>::value,
	"containment authority must not be caller-copyable");
static_assert(!std::is_default_constructible<ContainmentResumeKey>::value,
	"release-resume authority must be broker minted");

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t) override { return false; }
	void SetNow(uint64_t now_ms) { now_ms_ = now_ms; }

private:
	uint64_t now_ms_;
};

enum class EventKind : uint8_t {
	open,
	close,
	map,
	unmap,
	read,
	write,
	barrier
};

struct Event {
	EventKind kind;
	uintptr_t mapping;
	uint64_t offset;
	size_t length;
	uint32_t value;
};

class FakeLinuxOperations final : public NativeMmioTestOperations {
public:
	FakeLinuxOperations()
		: page_size(4096), now_ms(1000), fail_event(0), map_failed_event(0),
		  advance_after_event(0), advance_to_ms(0), event_count(0),
		  descriptor_open(false), core(0x92345678u), interface_module(9),
		  sdr(9), bridge(0), remap(0)
	{
		for (size_t index = 0; index != 6; ++index) held[index] = false;
	}

	size_t PageSize() const override { return page_size; }
	uint64_t NowMs() const override { return now_ms; }
	int Open(const char *path, int flags) override
	{
		assert(std::string(path) == "/dev/mem");
		assert((flags & O_RDWR) != 0 && (flags & O_CLOEXEC) != 0);
		if (Record(EventKind::open, 0, 0, 0, 0)) return -1;
		descriptor_open = true;
		return 37;
	}
	int Close(int descriptor) override
	{
		assert(descriptor == 37 && descriptor_open);
		if (Record(EventKind::close, 0, 0, 0, 0)) return -1;
		descriptor_open = false;
		return 0;
	}
	int Map(int descriptor, uint64_t page_offset, size_t length,
		NativeMmioTestMapping *mapping) override
	{
		assert(descriptor == 37 && descriptor_open && mapping != nullptr);
		const uintptr_t identity = IdentityForPage(page_offset);
		const bool fail = Record(EventKind::map, identity, page_offset, length, 0);
		if (fail) return -1;
		if (event_count == map_failed_event) {
			mapping->identity = std::numeric_limits<uintptr_t>::max();
			return 0;
		}
		assert(identity != 0 && !held[identity]);
		held[identity] = true;
		mapping->identity = identity;
		return 0;
	}
	int Unmap(const NativeMmioTestMapping &mapping, size_t length) override
	{
		const uintptr_t identity = mapping.identity;
		assert(identity > 0 && identity < 6 && held[identity]);
		if (Record(EventKind::unmap, identity, 0, length, 0)) return -1;
		held[identity] = false;
		return 0;
	}
	int Read32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t *value) override
	{
		const uintptr_t identity = mapping.identity;
		assert(value != nullptr && identity > 0 && identity < 6 && held[identity]);
		if (Record(EventKind::read, identity, offset, sizeof(uint32_t), 0))
			return -1;
		*value = Register(identity);
		return 0;
	}
	int Write32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t value) override
	{
		const uintptr_t identity = mapping.identity;
		assert(identity > 0 && identity < 6 && held[identity]);
		if (Record(EventKind::write, identity, offset, sizeof(uint32_t), value))
			return -1;
		Register(identity) = value;
		return 0;
	}
	int OrderingBarrier() override
	{
		return Record(EventKind::barrier, 0, 0, 0, 0) ? -1 : 0;
	}

	bool Record(EventKind kind, uintptr_t mapping, uint64_t offset,
		size_t length, uint32_t value)
	{
		Event event = {kind, mapping, offset, length, value};
		events.push_back(event);
		++event_count;
		const bool fail = fail_event != 0 && event_count == fail_event;
		if (!fail && advance_after_event != 0 &&
			event_count == advance_after_event)
			now_ms = advance_to_ms;
		return fail;
	}

	static uintptr_t IdentityForPage(uint64_t page)
	{
		if (page == 0xff706000u) return 1;
		if (page == 0xffd08000u) return 2;
		if (page == 0xffc25000u) return 3;
		if (page == 0xffd05000u) return 4;
		if (page == 0xff800000u) return 5;
		return 0;
	}
	uint32_t &Register(uintptr_t identity)
	{
		if (identity == 1) return core;
		if (identity == 2) return interface_module;
		if (identity == 3) return sdr;
		if (identity == 4) return bridge;
		assert(identity == 5);
		return remap;
	}
	size_t Count(EventKind kind) const
	{
		size_t count = 0;
		for (const Event &event : events)
			if (event.kind == kind) ++count;
		return count;
	}
	bool AnyHeld() const
	{
		for (size_t index = 1; index != 6; ++index)
			if (held[index]) return true;
		return false;
	}

	size_t page_size;
	mutable uint64_t now_ms;
	size_t fail_event;
	size_t map_failed_event;
	size_t advance_after_event;
	uint64_t advance_to_ms;
	size_t event_count;
	bool descriptor_open;
	bool held[6];
	uint32_t core;
	uint32_t interface_module;
	uint32_t sdr;
	uint32_t bridge;
	uint32_t remap;
	std::vector<Event> events;
};

void PrepareCleanup(HardwareBroker &broker, FakeClock &clock,
	PlatformGenerationId *generation, std::unique_ptr<CleanupEpoch> *epoch,
	std::unique_ptr<OperationLease> *terminal)
{
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(*generation, clock.NowMs() + 100) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(*generation, clock.NowMs() + 2000,
		clock.NowMs() + 5000, epoch) == MISTER_RESULT_OK);
	assert(broker.BeginCleanupOperation(**epoch,
		OperationKind::terminal_fpga_cleanup, terminal) == MISTER_RESULT_OK);
}

void TestExactMappingsTraceAndPermanentClosure()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);

	assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
	assert(operations.Count(EventKind::open) == 1);
	assert(operations.Count(EventKind::map) == 5);
	assert(operations.Count(EventKind::close) == 1);
	assert(operations.Count(EventKind::read) == 6);
	assert(operations.Count(EventKind::write) == 5);
	assert(operations.Count(EventKind::barrier) == 5);
	assert(operations.Count(EventKind::unmap) == 5);
	assert(operations.events.size() == 28);
	for (size_t index = 1; index <= 5; ++index) {
		assert(operations.events[index].kind == EventKind::map);
		assert(operations.events[index].mapping == index);
		assert(operations.events[index].length == 4096);
	}
	const uint64_t pages[] = {
		0xff706000u, 0xffd08000u, 0xffc25000u, 0xffd05000u,
		0xff800000u
	};
	for (size_t index = 0; index != 5; ++index)
		assert(operations.events[index + 1].offset == pages[index]);
	assert(operations.events[6].kind == EventKind::close);
	for (size_t index = 0; index != 7; ++index)
		assert(operations.events[index].kind != EventKind::write);
	assert(operations.core == 0x52345678u);
	assert(operations.interface_module == 0);
	assert(operations.sdr == 0);
	assert(operations.bridge == 7);
	assert(operations.remap == 1);
	assert(!operations.AnyHeld() && !operations.descriptor_open);
	const size_t event_count = operations.events.size();
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == event_count);
	terminal.reset();
	assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
}

void TestPosixVolatileAccessAlignmentAndOverflowChecks()
{
	assert(NativeLinuxMmioProductionPrimitivesRejectInvalidAccessForTest());
}

void TestEveryInjectedLinuxFailureIsTruthfulAndRetryable()
{
	for (size_t failure = 1; failure <= 28; ++failure) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		operations.fail_event = failure;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		const Result first = containment.ResetAndContain(*epoch, *terminal);
		assert(first != MISTER_RESULT_OK);
		assert(broker.containment_receipt_sequence_for_test() == 0);
		if (failure <= 7)
			assert(operations.Count(EventKind::write) == 0);
		operations.fail_event = 0;
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		assert(!operations.AnyHeld() && !operations.descriptor_open);
		terminal.reset();
		assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
	}
}

void TestMapFailedAndMappingGeometryAreRejectedBeforeMutation()
{
	for (int geometry = 0; geometry != 4; ++geometry) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		if (geometry == 0) operations.page_size = 0;
		if (geometry == 1) operations.page_size = 2048;
		if (geometry == 2) operations.page_size = 4095;
		if (geometry == 3)
			operations.page_size = std::numeric_limits<size_t>::max();
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_PLATFORM);
		assert(operations.Count(EventKind::open) == 0);
		assert(operations.Count(EventKind::write) == 0);
	}

	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.map_failed_event = 2;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_PLATFORM);
	assert(operations.Count(EventKind::write) == 0);
	assert(!operations.AnyHeld());
}

void TestDeadlineBeforeAndAfterEveryLinuxBoundaryFailsClosed()
{
	for (size_t boundary = 1; boundary <= 28; ++boundary) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		operations.advance_after_event = boundary;
		operations.advance_to_ms = 6000;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_DEADLINE);
		assert(broker.containment_receipt_sequence_for_test() == 0);
	}

	FakeClock clock(6000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	clock.SetNow(1000);
	assert(broker.Quiesce(generation, 1100) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(broker.BeginCleanupOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	clock.SetNow(6000);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_DEADLINE);
	assert(operations.events.empty());
}

void TestRejectedAuthorityProducesNoLinuxOperations()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(first, adapter);
	PlatformGenerationId first_generation = 0;
	PlatformGenerationId second_generation = 0;
	std::unique_ptr<CleanupEpoch> first_epoch;
	std::unique_ptr<CleanupEpoch> second_epoch;
	std::unique_ptr<OperationLease> first_terminal;
	std::unique_ptr<OperationLease> second_terminal;
	PrepareCleanup(first, clock, &first_generation, &first_epoch,
		&first_terminal);
	PrepareCleanup(second, clock, &second_generation, &second_epoch,
		&second_terminal);
	assert(containment.ResetAndContain(*second_epoch, *second_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.empty());
}

MisterRecoveryObservationV2 Observation()
{
	MisterRecoveryObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

void TestRecoveryReleaseFailureResumesWithoutRegisterReplay()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.fail_event = 24;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	const size_t writes = operations.Count(EventKind::write);
	const size_t reads = operations.Count(EventKind::read);
	operations.fail_event = 0;
	assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
	assert(operations.Count(EventKind::write) == writes);
	assert(operations.Count(EventKind::read) == reads);
	assert(!operations.AnyHeld());
	terminal.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_OK);
	assert(epoch == nullptr);
	assert(observation.neutral_resource_flags == closure);
}

void TestCleanupReleaseResumeAcceptsOnlyFreshLeaseForSameEpoch()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.fail_event = 25;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	const size_t writes = operations.Count(EventKind::write);
	const size_t reads = operations.Count(EventKind::read);
	terminal.reset();
	assert(broker.BeginCleanupOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	operations.fail_event = 0;
	assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
	assert(operations.Count(EventKind::write) == writes);
	assert(operations.Count(EventKind::read) == reads);
	terminal.reset();
	assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
}

void TestPendingReleaseRejectsForeignAndStaleAuthorityBeforeAdapterAction()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeLinuxOperations operations;
	operations.fail_event = 24;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(first, adapter);
	PlatformGenerationId first_generation = 0;
	PlatformGenerationId second_generation = 0;
	std::unique_ptr<CleanupEpoch> first_epoch;
	std::unique_ptr<CleanupEpoch> second_epoch;
	std::unique_ptr<OperationLease> first_terminal;
	std::unique_ptr<OperationLease> second_terminal;
	PrepareCleanup(first, clock, &first_generation, &first_epoch,
		&first_terminal);
	assert(containment.ResetAndContain(*first_epoch, *first_terminal) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	PrepareCleanup(second, clock, &second_generation, &second_epoch,
		&second_terminal);
	const size_t before_foreign = operations.events.size();
	assert(containment.ResetAndContain(*second_epoch, *second_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == before_foreign);
	first_epoch.reset();
	const size_t before_stale = operations.events.size();
	assert(containment.ResetAndContain(*first_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == before_stale);
}

void TestTwoHundredBoundedTerminalCycles()
{
	for (int cycle = 0; cycle != 200; ++cycle) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_OK);
		terminal.reset();
		assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
		assert(!operations.AnyHeld() && !operations.descriptor_open);
	}
}

std::string ReadPipe(int descriptor)
{
	std::string value;
	char bytes[256];
	for (;;) {
		const ssize_t count = read(descriptor, bytes, sizeof(bytes));
		if (count == 0) break;
		assert(count > 0);
		value.append(bytes, static_cast<size_t>(count));
	}
	return value;
}

void RunAllTests()
{
	TestExactMappingsTraceAndPermanentClosure();
	TestPosixVolatileAccessAlignmentAndOverflowChecks();
	TestEveryInjectedLinuxFailureIsTruthfulAndRetryable();
	TestMapFailedAndMappingGeometryAreRejectedBeforeMutation();
	TestDeadlineBeforeAndAfterEveryLinuxBoundaryFailsClosed();
	TestRejectedAuthorityProducesNoLinuxOperations();
	TestRecoveryReleaseFailureResumesWithoutRegisterReplay();
	TestCleanupReleaseResumeAcceptsOnlyFreshLeaseForSameEpoch();
	TestPendingReleaseRejectsForeignAndStaleAuthorityBeforeAdapterAction();
	TestTwoHundredBoundedTerminalCycles();
}

void TestAllPathsArePrivacySilent()
{
	int output_pipe[2] = {-1, -1};
	int error_pipe[2] = {-1, -1};
	assert(pipe(output_pipe) == 0 && pipe(error_pipe) == 0);
	fflush(stdout);
	fflush(stderr);
	const int saved_output = dup(STDOUT_FILENO);
	const int saved_error = dup(STDERR_FILENO);
	assert(saved_output >= 0 && saved_error >= 0);
	assert(dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
	assert(dup2(error_pipe[1], STDERR_FILENO) == STDERR_FILENO);
	assert(close(output_pipe[1]) == 0 && close(error_pipe[1]) == 0);
	RunAllTests();
	fflush(stdout);
	fflush(stderr);
	assert(dup2(saved_output, STDOUT_FILENO) == STDOUT_FILENO);
	assert(dup2(saved_error, STDERR_FILENO) == STDERR_FILENO);
	assert(close(saved_output) == 0 && close(saved_error) == 0);
	const std::string output = ReadPipe(output_pipe[0]);
	const std::string error = ReadPipe(error_pipe[0]);
	assert(close(output_pipe[0]) == 0 && close(error_pipe[0]) == 0);
	assert(output.empty() && error.empty());
	const char *const sentinels[] = {
		"/dev/mem-private-sentinel", "0xff706010-private-sentinel",
		"profile-private-sentinel", "content-private-sentinel",
		"0x92345678-private-sentinel"
	};
	for (const char *sentinel : sentinels) {
		assert(output.find(sentinel) == std::string::npos);
		assert(error.find(sentinel) == std::string::npos);
	}
}

} // namespace
} // namespace linux_native
} // namespace native
} // namespace mister

int main()
{
	mister::native::linux_native::TestAllPathsArePrivacySilent();
	return 0;
}
#endif
