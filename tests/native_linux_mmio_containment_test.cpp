// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_mmio_adapter.hpp"
#if defined(MISTER_NATIVE_BRIDGE_TOKEN_CONSTRUCT_REACHABILITY_PROBE)
int main()
{
	mister::native::NativeBridgeActivationAuthority authority;
	return sizeof(authority) == 0;
}
#elif defined(MISTER_NATIVE_BRIDGE_TOKEN_COPY_REACHABILITY_PROBE)
#include <type_traits>
static_assert(std::is_copy_constructible<
	mister::native::NativeBridgeActivationAuthority>::value,
	"probe succeeds only if bridge authority becomes copyable");
int main() { return 0; }
#elif defined(MISTER_NATIVE_BRIDGE_TOKEN_MOVE_REACHABILITY_PROBE)
#include <type_traits>
static_assert(std::is_move_constructible<
	mister::native::NativeBridgeActivationAuthority>::value,
	"probe succeeds only if bridge authority becomes movable");
int main() { return 0; }
#elif defined(MISTER_NATIVE_BRIDGE_TOKEN_MINT_REACHABILITY_PROBE)
int main()
{
	return sizeof(&mister::native::HardwareLeaseView::
		MintBridgeActivationAuthority) == 0;
}
#elif defined(MISTER_NATIVE_BRIDGE_TOKEN_INSTALL_REACHABILITY_PROBE)
int main()
{
	return sizeof(&mister::native::linux_native::NativeLinuxMmioAdapter::
		InstallBridgeActivationAuthority) == 0;
}
#elif defined(MISTER_NATIVE_BRIDGE_TOKEN_VALIDATE_REACHABILITY_PROBE)
int main()
{
	return sizeof(&mister::native::linux_native::NativeLinuxMmioAdapter::
		ValidateBridgeActivationAuthority) == 0;
}
#elif defined(MISTER_NATIVE_MMIO_REACHABILITY_PROBE)
using LeakedRawMmioAuthority =
	mister::native::linux_native::NativeMmioTestOperations;
int main()
{
	return sizeof(LeakedRawMmioAuthority *) == 0;
}
#elif defined(MISTER_NATIVE_USER_IO_RAW_SELECT_REACHABILITY_PROBE)
int main()
{
	return sizeof(&mister::native::linux_native::NativeLinuxMmioAdapter::Select) ==
		0;
}
#elif defined(MISTER_NATIVE_USER_IO_RAW_CAST_REACHABILITY_PROBE)
int main()
{
	mister::native::linux_native::NativeLinuxMmioAdapter adapter;
	return static_cast<mister::native::NativeHardwareIo *>(&adapter) == nullptr;
}
#elif defined(MISTER_NATIVE_CLEANUP_INPUT_VIEW_CONSTRUCT_REACHABILITY_PROBE)
int main()
{
	mister::native::CleanupInputReplayView view;
	return sizeof(view) == 0;
}
#elif defined(MISTER_NATIVE_CLEANUP_INPUT_VIEW_MINT_REACHABILITY_PROBE)
int main()
{
	return sizeof(&mister::native::OperationLease::
		AcquireCleanupInputReplayView) == 0;
}
#elif defined(MISTER_NATIVE_CLEANUP_INPUT_RAW_ROUTE_REACHABILITY_PROBE)
int main()
{
	return sizeof(&mister::native::linux_native::NativeLinuxMmioAdapter::
		CleanupSelectUserIo) == 0;
}
#else
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_input.hpp"
#include "runtime/native/native_resources.hpp"
#include "runtime/native/native_spi_bus.hpp"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
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

class BridgeActivationAuthorityTestPeer final {
public:
	static Result ValidateInput(linux_native::NativeLinuxMmioAdapter &adapter,
		const OperationLease &lease, HardwareBroker &broker,
		const NativeCoreProfile &profile)
	{
		std::unique_ptr<HardwareLeaseView> view;
		const Result result = lease.AcquireInputHardwareLeaseView(
			broker, profile, &view);
		return result == MISTER_RESULT_OK ?
			adapter.ValidateBridgeActivationAuthorityForTest(*view) : result;
	}

	static Result ValidateAny(linux_native::NativeLinuxMmioAdapter &adapter,
		const OperationLease &lease)
	{
		std::unique_ptr<HardwareLeaseView> view;
		const Result result = lease.AcquireHardwareLeaseView(&view);
		return result == MISTER_RESULT_OK ?
			adapter.ValidateBridgeActivationAuthorityForTest(*view) : result;
	}

	static void SetMutationSequence(HardwareBroker &broker, uint64_t sequence)
	{
		broker.SetBridgeMutationSequenceForTest(sequence);
	}

	static Result Mint(HardwareBroker &broker, const OperationLease &lease,
		uint64_t sequence,
		std::unique_ptr<NativeBridgeActivationAuthority> *authority)
	{
		std::unique_ptr<HardwareLeaseView> view;
		Result result = lease.AcquireHardwareLeaseView(&view);
		return result == MISTER_RESULT_OK ?
			broker.MintBridgeActivationAuthority(*view, sequence, authority) :
			result;
	}
};

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

bool SamePrimitive(const Event &left, const Event &right);

class FakeLinuxOperations final : public NativeMmioTestOperations {
public:
	FakeLinuxOperations()
		: page_size(4096), now_ms(1000), fail_event(0), map_failed_event(0),
		  corrupt_read_event(0), advance_after_event(0), advance_to_ms(0),
		  latch_failure_event(0), latch_failure_broker(nullptr),
		  latch_failure_generation(0), sequence_at_failure_latch(0), event_count(0),
		  descriptor_open(false), core(0x92345678u), interface_module(9),
		  sdr(9), bridge(0), remap(0), manager_stat(0x80u), manager_ctrl(2),
		  manager_gpio(3), manager_gpi_response(0x5a5au), manager_gpi_fault(false),
		  manager_dclk_status(0)
	{
		for (size_t index = 0; index != 7; ++index) held[index] = false;
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
		assert(identity > 0 && identity < 7 && held[identity]);
		if (Record(EventKind::unmap, identity, 0, length, 0)) return -1;
		held[identity] = false;
		return 0;
	}
	int Read32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t *value) override
	{
		const uintptr_t identity = mapping.identity;
		assert(value != nullptr && identity > 0 && identity < 7 && held[identity]);
		if (Record(EventKind::read, identity, offset, sizeof(uint32_t), 0))
			return -1;
		if (identity == 1 && offset == 0) *value = manager_stat;
		else if (identity == 1 && offset == 4) *value = manager_ctrl;
		else if (identity == 1 && offset == 0x0c) *value = manager_dclk_status;
		else if (identity == 1 && offset == 0x14) *value =
			(core & 0x00020000u) | manager_gpi_response |
			(manager_gpi_fault ? 0x80000000u : 0);
		else if (identity == 1 && offset == 0x850) *value = manager_gpio;
		else *value = Register(identity);
		if (corrupt_read_event != 0 && event_count == corrupt_read_event)
			*value ^= offset == 0x14 ? 0x80000000u : 0x00100000u;
		return 0;
	}
	int Write32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t value) override
	{
		const uintptr_t identity = mapping.identity;
		assert(identity > 0 && identity < 7 && held[identity]);
		if (Record(EventKind::write, identity, offset, sizeof(uint32_t), value))
			return -1;
		if (identity == 1 && offset == 4) {
			manager_ctrl = value;
			if ((value & 5u) == 5u) manager_stat = (manager_stat & ~7u) | 1u;
			else if ((value & 5u) == 1u)
				manager_stat = (manager_stat & ~7u) | 2u;
		} else if (identity == 1 && offset == 8) {
			manager_dclk_status = 1;
			manager_stat = (manager_stat & ~7u) |
				(value == 4 ? 3u : 4u);
		} else if (identity == 1 && offset == 0x0c) {
			manager_dclk_status = 0;
		} else if (identity == 6) {
			data_words.push_back(value);
		} else Register(identity) = value;
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
		if (!fail && latch_failure_event != 0 &&
			event_count == latch_failure_event) {
			assert(latch_failure_broker != nullptr);
			assert(latch_failure_broker->LatchFailure(
				latch_failure_generation) == MISTER_RESULT_OK);
			sequence_at_failure_latch =
				latch_failure_broker->mutation_sequence_for_test();
		}
		return fail;
	}

	static uintptr_t IdentityForPage(uint64_t page)
	{
		if (page == 0xff706000u) return 1;
		if (page == 0xffd08000u) return 2;
		if (page == 0xffc25000u) return 3;
		if (page == 0xffd05000u) return 4;
		if (page == 0xff800000u) return 5;
		if (page == 0xffb90000u) return 6;
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
		for (size_t index = 1; index != 7; ++index)
			if (held[index]) return true;
		return false;
	}

	size_t page_size;
	mutable uint64_t now_ms;
	size_t fail_event;
	size_t map_failed_event;
	size_t corrupt_read_event;
	size_t advance_after_event;
	uint64_t advance_to_ms;
	size_t latch_failure_event;
	HardwareBroker *latch_failure_broker;
	PlatformGenerationId latch_failure_generation;
	uint64_t sequence_at_failure_latch;
	size_t event_count;
	bool descriptor_open;
	bool held[7];
	uint32_t core;
	uint32_t interface_module;
	uint32_t sdr;
	uint32_t bridge;
	uint32_t remap;
	uint32_t manager_stat;
	uint32_t manager_ctrl;
	uint32_t manager_gpio;
	uint32_t manager_gpi_response;
	bool manager_gpi_fault;
	uint32_t manager_dclk_status;
	std::vector<uint32_t> data_words;
	std::vector<Event> events;
};

void TestPurposeNamedUserIoCapabilityConstructsOnlyTypedInputPort()
{
	FakeClock clock(1000);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeSpiBus bus(clock, adapter.user_io_only_hardware());
	NativeInputSpiPort &port = bus.input_port();
	(void)port;
}

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
	uint32_t manager_control = 0;
	uint32_t manager_mode = UINT32_MAX;
	uint64_t manager_sequence = 0;
	assert(broker.containment_manager_receipt_for_test(&manager_control,
		&manager_mode, &manager_sequence));
	assert((manager_control & 0x107u) == 0x2u && manager_mode <= 4u &&
		manager_sequence != 0 &&
		manager_sequence <= broker.containment_receipt_sequence_for_test());
	assert(operations.Count(EventKind::open) == 1);
	assert(operations.Count(EventKind::map) == 5);
	assert(operations.Count(EventKind::close) == 1);
	assert(operations.Count(EventKind::read) == 10);
	assert(operations.Count(EventKind::write) == 5);
	assert(operations.Count(EventKind::barrier) == 5);
	assert(operations.Count(EventKind::unmap) == 5);
	assert(operations.events.size() == 32);
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

void TestSixPageProgramBridgeAndTerminalCycle()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, UINT64_MAX,
		&program) == MISTER_RESULT_OK);
	const NativeMappingAcquisitionReceipt mapped =
		containment.AcquireMappings(*program);
	assert(mapped.result == MISTER_RESULT_OK && mapped.acquired && mapped.complete);
	assert(operations.Count(EventKind::map) == 6);
	assert(operations.Count(EventKind::write) == 0);
	assert(broker.mutation_sequence_for_test() == 0);

	char root_template[] = "/tmp/fogcast-mmio-program.XXXXXX";
	assert(mkdtemp(root_template) != nullptr);
	char resolved_root[4096] = {};
	assert(realpath(root_template, resolved_root) != nullptr);
	const std::string root(resolved_root);
	const std::string directory = root + "/snes";
	assert(mkdir(directory.c_str(), 0700) == 0);
	const std::string file = directory +
		"/2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const int descriptor = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(descriptor >= 0 && write(descriptor, "hello", 5) == 5 &&
		close(descriptor) == 0);
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter artifacts(root.c_str(), filesystem);
	NativeArtifactAuthority authority = {
		"snes", 4,
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
		64, 5, "rbf", 3};
	NativeCoreArtifactHandle artifact;
	const NativeArtifactResult artifact_result = artifacts.ResolveFixtureForTest(
		*FixtureNativeCoreProfile("snes"), authority,
		filesystem.NowMs() + 1000, &artifact);
	assert(artifact_result == NativeArtifactResult::ok);
	NativeFpgaProgrammer programmer(broker, clock, adapter);
	const NativeFpgaProgrammingReceipt programmed =
		programmer.Program(*program, artifact);
	assert(programmed.result == MISTER_RESULT_OK && programmed.acquired);
	assert(programmed.accepted_bytes == 5 &&
		programmed.configuration_done_observed &&
		programmed.initialization_observed && programmed.user_mode_observed &&
		programmed.manager_drive_released && programmed.mutation_sequence == 3);
	assert(operations.data_words.size() == 2);
	assert(operations.data_words[0] == 0x6c6c6568u);
	assert(operations.data_words[1] == 0x0000006fu);
	const size_t events_after_program = operations.events.size();
	const NativeFpgaProgrammingReceipt retry =
		programmer.Program(*program, artifact);
	assert(retry.result == MISTER_RESULT_INVALID_STATE);
	assert(!retry.acquired && retry.accepted_bytes == 0 &&
		retry.mutation_sequence == 0);
	assert(operations.events.size() == events_after_program);
	const NativeBridgeEnableReceipt enabled = containment.EnableBridges(*program);
	assert(enabled.result == MISTER_RESULT_OK && enabled.acquired &&
		enabled.sdr_ports_observed && enabled.bridge_release_observed &&
		enabled.remap_observed && enabled.core_normal_write_attempted &&
		enabled.core_normal_observed && enabled.mutation_sequence == 4);
	assert((enabled.observed_core_gpo & 0xc0000000u) == 0x80000000u);
	assert(adapter.HasBridgeActivationAuthorityForTest());
	const size_t events_before_install_rejections = operations.events.size();
	assert(adapter.InstallBridgeActivationAuthorityForTest(
		std::unique_ptr<NativeBridgeActivationAuthority>()) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	std::unique_ptr<NativeBridgeActivationAuthority> duplicate;
	assert(BridgeActivationAuthorityTestPeer::Mint(broker, *program,
		enabled.mutation_sequence, &duplicate) == MISTER_RESULT_OK);
	assert(adapter.InstallBridgeActivationAuthorityForTest(std::move(duplicate)) ==
		MISTER_RESULT_INVALID_STATE);
	assert(duplicate == nullptr);
	FakeLinuxOperations foreign_operations;
	NativeLinuxMmioAdapter foreign_adapter(foreign_operations);
	std::unique_ptr<NativeBridgeActivationAuthority> foreign;
	assert(BridgeActivationAuthorityTestPeer::Mint(broker, *program,
		enabled.mutation_sequence, &foreign) == MISTER_RESULT_OK);
	assert(foreign_adapter.InstallBridgeActivationAuthorityForTest(
		std::move(foreign)) == MISTER_RESULT_INVALID_STATE);
	assert(foreign == nullptr);
	assert(operations.events.size() == events_before_install_rejections);
	assert(foreign_operations.events.empty());
	program.reset();
	std::unique_ptr<OperationLease> input;
	assert(broker.Begin(generation, OperationKind::input, 5000, &input) ==
		MISTER_RESULT_OK);
	const size_t events_before_validation = operations.events.size();
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter, *input,
		broker, *FixtureNativeCoreProfile("snes")) == MISTER_RESULT_OK);
	assert(operations.events.size() == events_before_validation);
	input.reset();

	const size_t input_start = operations.events.size();
	NativeSpiBus bus(clock, adapter.user_io_only_hardware());
	NativeInput native_input(bus.input_port());
	std::unique_ptr<OperationLease> delivery;
	assert(broker.Begin(generation, OperationKind::input, 5000, &delivery) ==
		MISTER_RESULT_OK);
	const NativeInputEvent event = {
		NativeInputKind::digital, 0, 0x12u, true, false, 0, {0, 1}};
	SpiReceipt input_receipt = {};
	assert(native_input.Deliver(FixtureNativeCoreProfile("snes"), *delivery,
		event, &input_receipt) == MISTER_RESULT_OK);
	assert(input_receipt.completed_words == 2 && input_receipt.deselected &&
		input_receipt.mapping_retained && input_receipt.mutation_sequence == 12);
	assert(native_input.ledger_size() == 1);
	std::vector<uint32_t> input_writes;
	for (size_t index = input_start; index != operations.events.size(); ++index) {
		const Event &input_event = operations.events[index];
		if (input_event.kind == EventKind::write && input_event.mapping == 1 &&
			input_event.offset == 0x10)
			input_writes.push_back(input_event.value);
	}
	const uint32_t expected_input_writes[] = {
		0x92305678u,
		0x92300002u, 0x92320002u, 0x92300002u,
		0x92300012u, 0x92320012u, 0x92300012u,
		0x92200012u
	};
	assert(input_writes == std::vector<uint32_t>(expected_input_writes,
		expected_input_writes + 8));
	delivery.reset();

	const uint16_t probe_word = 0x33u;
	const SpiWords probe_words = {&probe_word, nullptr, 1, 0};
	SpiReceipt rejected = {};
	const size_t before_file_io = operations.events.size();
	std::unique_ptr<OperationLease> file_probe;
	assert(broker.Begin(generation, OperationKind::input, 5000, &file_probe) ==
		MISTER_RESULT_OK);
	assert(bus.ExchangeForTest(*file_probe, NativeSpiTarget::file_io,
		probe_words, &rejected) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(operations.events.size() == before_file_io &&
		rejected.mutation_sequence == 0 && !rejected.selected);
	file_probe.reset();

	const uint32_t rejected_core_states[] = {
		0, 0x40000000u, 0xc0000000u
	};
	for (uint32_t rejected_state : rejected_core_states) {
		operations.core = (operations.core & ~0xc0000000u) | rejected_state;
		const size_t before_reset_probe = operations.events.size();
		const size_t writes_before_reset = operations.Count(EventKind::write);
		std::unique_ptr<OperationLease> reset_probe;
		assert(broker.Begin(generation, OperationKind::input, 5000,
			&reset_probe) == MISTER_RESULT_OK);
		assert(bus.ExchangeForTest(*reset_probe, NativeSpiTarget::user_io,
			probe_words, &rejected) == MISTER_RESULT_INVALID_STATE);
		assert(operations.Count(EventKind::write) == writes_before_reset &&
			operations.events.size() == before_reset_probe + 1 &&
			operations.events.back().kind == EventKind::read &&
			rejected.mutation_sequence == 0);
		reset_probe.reset();
	}
	operations.core = (operations.core & ~0xc0000000u) | 0x80000000u;

	operations.manager_gpi_fault = true;
	const size_t writes_before_fault = operations.Count(EventKind::write);
	std::unique_ptr<OperationLease> fault_probe;
	assert(broker.Begin(generation, OperationKind::input, 5000, &fault_probe) ==
		MISTER_RESULT_OK);
	assert(bus.ExchangeForTest(*fault_probe, NativeSpiTarget::user_io,
		probe_words, &rejected) == MISTER_RESULT_PLATFORM);
	assert(rejected.selected && rejected.deselected &&
		rejected.mapping_retained && rejected.completed_words == 0 &&
		operations.Count(EventKind::write) == writes_before_fault + 5);
	fault_probe.reset();
	operations.manager_gpi_fault = false;

	std::unique_ptr<OperationLease> baseline_probe;
	assert(broker.Begin(generation, OperationKind::input, 5000,
		&baseline_probe) == MISTER_RESULT_OK);
	const size_t baseline_start = operations.events.size();
	SpiReceipt baseline_receipt = {};
	assert(bus.ExchangeForTest(*baseline_probe, NativeSpiTarget::user_io,
		probe_words, &baseline_receipt) == MISTER_RESULT_OK);
	const std::vector<Event> baseline_input_events(
		operations.events.begin() + baseline_start, operations.events.end());
	assert(!baseline_input_events.empty());
	baseline_probe.reset();

	for (size_t boundary = 1; boundary <= baseline_input_events.size(); ++boundary) {
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::input, 5000, &lease) ==
			MISTER_RESULT_OK);
		operations.fail_event = operations.event_count + boundary;
		const size_t start = operations.events.size();
		SpiReceipt receipt = {};
		assert(bus.ExchangeForTest(*lease, NativeSpiTarget::user_io,
			probe_words, &receipt) != MISTER_RESULT_OK);
		assert(operations.events.size() >= start + boundary &&
			SamePrimitive(operations.events[start + boundary - 1],
				baseline_input_events[boundary - 1]));
		operations.fail_event = 0;
		lease.reset();
		std::unique_ptr<OperationLease> cleanup;
		assert(broker.Begin(generation, OperationKind::input, 5000, &cleanup) ==
			MISTER_RESULT_OK);
		SpiReceipt cleanup_receipt = {};
		(void)bus.ExchangeForTest(*cleanup, NativeSpiTarget::user_io,
			probe_words, &cleanup_receipt);
		assert(!cleanup_receipt.target_may_be_selected &&
			!cleanup_receipt.strobe_may_be_high);
		cleanup.reset();

		assert(broker.Begin(generation, OperationKind::input, 5000, &lease) ==
			MISTER_RESULT_OK);
		operations.advance_after_event = operations.event_count + boundary;
		operations.advance_to_ms = 5000;
		const size_t deadline_start = operations.events.size();
		assert(bus.ExchangeForTest(*lease, NativeSpiTarget::user_io,
			probe_words, &receipt) == MISTER_RESULT_DEADLINE);
		assert(operations.events.size() >= deadline_start + boundary &&
			SamePrimitive(operations.events[deadline_start + boundary - 1],
				baseline_input_events[boundary - 1]));
		operations.advance_after_event = 0;
		operations.now_ms = 1000;
		lease.reset();
		assert(broker.Begin(generation, OperationKind::input, 5000, &cleanup) ==
			MISTER_RESULT_OK);
		(void)bus.ExchangeForTest(*cleanup, NativeSpiTarget::user_io,
			probe_words, &cleanup_receipt);
		assert(!cleanup_receipt.target_may_be_selected &&
			!cleanup_receipt.strobe_may_be_high);
	}
	for (size_t boundary = 1; boundary <= baseline_input_events.size(); ++boundary) {
		if (baseline_input_events[boundary - 1].kind != EventKind::read ||
			boundary == 1 ||
			(baseline_input_events[boundary - 2].kind != EventKind::barrier &&
				baseline_input_events[boundary - 1].offset != 0x14))
			continue;
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::input, 5000, &lease) ==
			MISTER_RESULT_OK);
		operations.corrupt_read_event = operations.event_count + boundary;
		SpiReceipt receipt = {};
		assert(bus.ExchangeForTest(*lease, NativeSpiTarget::user_io,
			probe_words, &receipt) != MISTER_RESULT_OK);
		operations.corrupt_read_event = 0;
		lease.reset();
		std::unique_ptr<OperationLease> cleanup;
		assert(broker.Begin(generation, OperationKind::input, 5000, &cleanup) ==
			MISTER_RESULT_OK);
		(void)bus.ExchangeForTest(*cleanup, NativeSpiTarget::user_io,
			probe_words, &receipt);
		assert(!receipt.target_may_be_selected && !receipt.strobe_may_be_high);
	}
	auto leave_selected_residue = [&]() {
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::input, 5000, &lease) ==
			MISTER_RESULT_OK);
		operations.advance_after_event = operations.event_count + 4;
		operations.advance_to_ms = 5000;
		SpiReceipt receipt = {};
		assert(bus.ExchangeForTest(*lease, NativeSpiTarget::user_io,
			probe_words, &receipt) == MISTER_RESULT_DEADLINE);
		assert(receipt.target_may_be_selected && receipt.mapping_retained);
		operations.advance_after_event = 0;
		operations.now_ms = 1000;
		lease.reset();
	};
	leave_selected_residue();
	{
		FakeClock foreign_clock(1000);
		HardwareBroker foreign_broker(foreign_clock);
		PlatformGenerationId foreign_generation = 0;
		assert(foreign_broker.EnterFixtureForTest(
			*FixtureNativeCoreProfile("snes"), &foreign_generation) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> foreign_input;
		assert(foreign_broker.Begin(foreign_generation, OperationKind::input,
			5000, &foreign_input) == MISTER_RESULT_OK);
		const size_t before_foreign_exchange = operations.events.size();
		const uint64_t before_foreign_sequence =
			foreign_broker.mutation_sequence_for_test();
		SpiReceipt rejected_authority = {};
		assert(bus.ExchangeForTest(*foreign_input, NativeSpiTarget::user_io,
			probe_words, &rejected_authority) == MISTER_RESULT_INVALID_STATE);
		assert(operations.events.size() == before_foreign_exchange &&
			foreign_broker.mutation_sequence_for_test() ==
				before_foreign_sequence &&
			rejected_authority.target_may_be_selected &&
			rejected_authority.deselect_attempted &&
			!rejected_authority.deselected);
	}
	const uint64_t current_sequence = broker.mutation_sequence_for_test();
	BridgeActivationAuthorityTestPeer::SetMutationSequence(broker, 0);
	std::unique_ptr<OperationLease> stale_exchange;
	assert(broker.Begin(generation, OperationKind::input, 5000,
		&stale_exchange) == MISTER_RESULT_OK);
	const size_t before_stale_exchange = operations.events.size();
	SpiReceipt rejected_authority = {};
	assert(bus.ExchangeForTest(*stale_exchange, NativeSpiTarget::user_io,
		probe_words, &rejected_authority) == MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == before_stale_exchange &&
		broker.mutation_sequence_for_test() == 0 &&
		rejected_authority.target_may_be_selected &&
		rejected_authority.deselect_attempted &&
		!rejected_authority.deselected);
	stale_exchange.reset();
	BridgeActivationAuthorityTestPeer::SetMutationSequence(
		broker, current_sequence);
	std::unique_ptr<OperationLease> valid_cleanup;
	assert(broker.Begin(generation, OperationKind::input, 5000,
		&valid_cleanup) == MISTER_RESULT_OK);
	assert(bus.ExchangeForTest(*valid_cleanup, NativeSpiTarget::user_io,
		probe_words, &rejected_authority) == MISTER_RESULT_INVALID_STATE);
	assert(rejected_authority.deselected &&
		!rejected_authority.target_may_be_selected &&
		broker.mutation_sequence_for_test() == current_sequence + 1);
	valid_cleanup.reset();
	const size_t events_after_input = operations.events.size();

	std::unique_ptr<OperationLease> wrong_kind;
	assert(broker.Begin(generation, OperationKind::scheduler, 5000,
		&wrong_kind) == MISTER_RESULT_OK);
	assert(BridgeActivationAuthorityTestPeer::ValidateAny(adapter, *wrong_kind) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == events_after_input);
	wrong_kind.reset();

	std::unique_ptr<OperationLease> wrong_profile;
	assert(broker.Begin(generation, OperationKind::input, 5000,
		&wrong_profile) == MISTER_RESULT_OK);
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter,
		*wrong_profile, broker, *FixtureNativeCoreProfile("megadrive")) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(operations.events.size() == events_after_input);
	wrong_profile.reset();

	BridgeActivationAuthorityTestPeer::SetMutationSequence(broker, 5);
	std::unique_ptr<OperationLease> later_input;
	assert(broker.Begin(generation, OperationKind::input, 5000, &later_input) ==
		MISTER_RESULT_OK);
	operations.core = (operations.core & ~0xc0000000u) | 0x40000000u;
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter, *later_input,
		broker, *FixtureNativeCoreProfile("snes")) == MISTER_RESULT_OK);
	assert(operations.events.size() == events_after_input);
	operations.core = (operations.core & ~0xc0000000u) | 0x80000000u;
	later_input.reset();

	BridgeActivationAuthorityTestPeer::SetMutationSequence(broker, 3);
	std::unique_ptr<OperationLease> rollback_input;
	assert(broker.Begin(generation, OperationKind::input, 5000, &rollback_input) ==
		MISTER_RESULT_OK);
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter,
		*rollback_input, broker, *FixtureNativeCoreProfile("snes")) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == events_after_input);
	rollback_input.reset();

	BridgeActivationAuthorityTestPeer::SetMutationSequence(broker, UINT64_MAX);
	std::unique_ptr<OperationLease> saturated_input;
	assert(broker.Begin(generation, OperationKind::input, 5000,
		&saturated_input) == MISTER_RESULT_OK);
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter,
		*saturated_input, broker, *FixtureNativeCoreProfile("snes")) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == events_after_input);
	saturated_input.reset();
	BridgeActivationAuthorityTestPeer::SetMutationSequence(broker, 5);

	std::unique_ptr<OperationLease> expired_input;
	assert(broker.Begin(generation, OperationKind::input, 1100, &expired_input) ==
		MISTER_RESULT_OK);
	clock.SetNow(1100);
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter,
		*expired_input, broker, *FixtureNativeCoreProfile("snes")) ==
		MISTER_RESULT_DEADLINE);
	assert(operations.events.size() == events_after_input);
	clock.SetNow(1000);
	expired_input.reset();

	FakeClock foreign_clock(1000);
	HardwareBroker foreign_broker(foreign_clock);
	PlatformGenerationId foreign_generation = 0;
	assert(foreign_broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&foreign_generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> foreign_input;
	assert(foreign_broker.Begin(foreign_generation, OperationKind::input, 5000,
		&foreign_input) == MISTER_RESULT_OK);
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter,
		*foreign_input, foreign_broker, *FixtureNativeCoreProfile("snes")) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == events_after_input);
	foreign_input.reset();

	std::unique_ptr<OperationLease> failed_input;
	leave_selected_residue();
	assert(broker.Begin(generation, OperationKind::input, 5000, &failed_input) ==
		MISTER_RESULT_OK);
	assert(broker.LatchFailure(generation) == MISTER_RESULT_OK);
	const size_t before_failed_exchange = operations.events.size();
	const uint64_t before_failed_sequence = broker.mutation_sequence_for_test();
	assert(bus.ExchangeForTest(*failed_input, NativeSpiTarget::user_io,
		probe_words, &rejected_authority) == MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == before_failed_exchange);
	assert(broker.mutation_sequence_for_test() == before_failed_sequence);
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter,
		*failed_input, broker, *FixtureNativeCoreProfile("snes")) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == before_failed_exchange);
	failed_input.reset();
	assert(artifact.Close() == NativeArtifactResult::ok);
	assert(unlink(file.c_str()) == 0 && rmdir(directory.c_str()) == 0 &&
		rmdir(root.c_str()) == 0);

	assert(broker.Quiesce(generation, 5100) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup_input;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::input,
		&cleanup_input) == MISTER_RESULT_OK);
	const NativeDigitalNeutral captured_neutral = {0, {0x02, 0}};
	SpiReceipt cleanup_receipt = {};
	// The deliberately retained selected state is reconciled without starting a
	// new transaction. A later call performs the exact captured neutral replay.
	assert(native_input.ReplayDigitalNeutral(FixtureNativeCoreProfile("snes"),
		*cleanup_input, captured_neutral, &cleanup_receipt) != MISTER_RESULT_OK);
	assert(!cleanup_receipt.target_may_be_selected &&
		!cleanup_receipt.strobe_may_be_high);
	assert(native_input.ReplayDigitalNeutral(FixtureNativeCoreProfile("snes"),
		*cleanup_input, captured_neutral, &cleanup_receipt) == MISTER_RESULT_OK);
	assert(cleanup_receipt.completed_words == 2 && cleanup_receipt.deselected);
	const size_t cleanup_events = operations.events.size();
	assert(native_input.ReplayDigitalNeutral(FixtureNativeCoreProfile("snes"),
		*cleanup_input, captured_neutral, &cleanup_receipt) == MISTER_RESULT_OK);
	assert(operations.events.size() == cleanup_events);
	cleanup_input.reset();
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginCleanupOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
	assert(!adapter.HasBridgeActivationAuthorityForTest());
	assert(!operations.AnyHeld() && !operations.descriptor_open);
	terminal.reset();
	assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
}

void TestMappingOnlyCannotValidateBridgeActivationAuthority()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, 5000,
		&program) == MISTER_RESULT_OK);
	assert(containment.AcquireMappings(*program).complete);
	program.reset();
	std::unique_ptr<OperationLease> input;
	assert(broker.Begin(generation, OperationKind::input, 5000, &input) ==
		MISTER_RESULT_OK);
	const size_t before = operations.events.size();
	assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter, *input,
		broker, *FixtureNativeCoreProfile("snes")) ==
		MISTER_RESULT_INVALID_STATE);
	assert(operations.events.size() == before);
	input.reset();
	assert(adapter.CloseMappingsForProcessExit() == MISTER_RESULT_OK);
}

struct ProgramBoundaryResult {
	NativeFpgaProgrammingReceipt receipt;
	uint64_t broker_sequence;
	size_t data_words;
	std::vector<Event> program_events;
};

struct BridgeBoundaryResult {
	NativeBridgeEnableReceipt receipt;
	uint64_t before_sequence;
	uint64_t after_sequence;
	std::vector<Event> bridge_events;
	size_t cleanup_replay_events;
};

MisterRecoveryObservationV2 Observation();

bool SamePrimitive(const Event &left, const Event &right)
{
	return left.kind == right.kind && left.mapping == right.mapping &&
		left.offset == right.offset && left.length == right.length &&
		left.value == right.value;
}

ProgramBoundaryResult RunProgramBoundary(size_t fail_event,
	size_t advance_after_event)
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.fail_event = fail_event;
	operations.advance_after_event = advance_after_event;
	operations.advance_to_ms = UINT64_MAX;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, UINT64_MAX,
		&program) == MISTER_RESULT_OK);
	assert(containment.AcquireMappings(*program).complete);

	char root_template[] = "/tmp/fogcast-mmio-boundary.XXXXXX";
	assert(mkdtemp(root_template) != nullptr);
	char resolved_root[4096] = {};
	assert(realpath(root_template, resolved_root) != nullptr);
	const std::string root(resolved_root);
	const std::string directory = root + "/snes";
	assert(mkdir(directory.c_str(), 0700) == 0);
	const std::string file = directory +
		"/2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const int descriptor = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(descriptor >= 0 && write(descriptor, "hello", 5) == 5 &&
		close(descriptor) == 0);
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter artifacts(root.c_str(), filesystem);
	NativeArtifactAuthority authority = {
		"snes", 4,
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
		64, 5, "rbf", 3};
	NativeCoreArtifactHandle artifact;
	assert(artifacts.ResolveFixtureForTest(*FixtureNativeCoreProfile("snes"),
		authority, filesystem.NowMs() + 1000, &artifact) ==
		NativeArtifactResult::ok);
	NativeFpgaProgrammer programmer(broker, clock, adapter);
	const size_t program_start = operations.events.size();
	const NativeFpgaProgrammingReceipt receipt =
		programmer.Program(*program, artifact);
	const size_t event_count = operations.events.size();
	const NativeFpgaProgrammingReceipt retry =
		programmer.Program(*program, artifact);
	assert(retry.result == MISTER_RESULT_INVALID_STATE && !retry.acquired &&
		retry.accepted_bytes == 0 && retry.mutation_sequence == 0);
	assert(operations.events.size() == event_count);
	const ProgramBoundaryResult result = {receipt,
		broker.mutation_sequence_for_test(), operations.data_words.size(),
		std::vector<Event>(operations.events.begin() + program_start,
			operations.events.begin() + event_count)};
	program.reset();
	assert(artifact.Close() == NativeArtifactResult::ok);
	assert(adapter.CloseMappingsForProcessExit() == MISTER_RESULT_OK);
	assert(unlink(file.c_str()) == 0 && rmdir(directory.c_str()) == 0 &&
		rmdir(root.c_str()) == 0);
	return result;
}

BridgeBoundaryResult RunBridgeBoundary(size_t fail_offset, bool expire_adapter,
	const char *system = "snes", size_t advance_offset = 0,
	int input_player = -1, size_t latch_failure_after_input_event = 0,
	bool cleanup_replay = false, bool reconcile_unselected_strobe = false,
	size_t cleanup_advance_offset = 0, bool cleanup_advance_after = false,
	bool advance_same_map = false, bool latch_failure_before_cleanup = false)
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile(system),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, UINT64_MAX,
		&program) == MISTER_RESULT_OK);
	assert(containment.AcquireMappings(*program).complete);
	char root_template[] = "/tmp/fogcast-mmio-bridge.XXXXXX";
	assert(mkdtemp(root_template) != nullptr);
	char resolved_root[4096] = {};
	assert(realpath(root_template, resolved_root) != nullptr);
	const std::string root(resolved_root);
	const std::string directory = root + "/" + system;
	assert(mkdir(directory.c_str(), 0700) == 0);
	const std::string file = directory +
		"/2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.rbf";
	const int descriptor = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(descriptor >= 0 && write(descriptor, "hello", 5) == 5 &&
		close(descriptor) == 0);
	NativePosixFileSystem filesystem;
	NativeCoreArtifactAdapter artifacts(root.c_str(), filesystem);
	NativeArtifactAuthority authority = {
		system, strlen(system),
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
		64, 5, "rbf", 3};
	NativeCoreArtifactHandle artifact;
	assert(artifacts.ResolveFixtureForTest(*FixtureNativeCoreProfile(system),
		authority, filesystem.NowMs() + 1000, &artifact) ==
		NativeArtifactResult::ok);
	NativeFpgaProgrammer programmer(broker, clock, adapter);
	assert(programmer.Program(*program, artifact).result == MISTER_RESULT_OK);
	const uint64_t before = broker.mutation_sequence_for_test();
	const size_t bridge_start = operations.events.size();
	if (fail_offset != 0) operations.fail_event = operations.event_count + fail_offset;
	if (advance_offset != 0) {
		operations.advance_after_event = operations.event_count + advance_offset;
		operations.advance_to_ms = UINT64_MAX;
	}
	if (expire_adapter) operations.now_ms = UINT64_MAX;
	const NativeBridgeEnableReceipt receipt = containment.EnableBridges(*program);
	BridgeBoundaryResult result = {receipt, before,
		broker.mutation_sequence_for_test(),
		std::vector<Event>(operations.events.begin() + bridge_start,
			operations.events.end()), 0};
	program.reset();
	std::unique_ptr<NativeSpiBus> input_bus;
	std::unique_ptr<NativeInput> native_input;
	const NativeCoreProfile *input_profile = FixtureNativeCoreProfile(system);
	if (receipt.result == MISTER_RESULT_OK) {
		std::unique_ptr<OperationLease> input;
		assert(broker.Begin(generation, OperationKind::input, UINT64_MAX,
			&input) == MISTER_RESULT_OK);
		const size_t before_validation = operations.events.size();
		assert(BridgeActivationAuthorityTestPeer::ValidateInput(adapter, *input,
			broker, *FixtureNativeCoreProfile(system)) == MISTER_RESULT_OK);
		assert(operations.events.size() == before_validation);
		if (input_player >= 0) {
			assert(input_player <= 1);
			assert(input_profile != nullptr &&
				input_profile->input.player_command[0] == 0x02 &&
				input_profile->input.player_command[1] == 0x03);
			input_bus.reset(new NativeSpiBus(clock,
				adapter.user_io_only_hardware()));
			native_input.reset(new NativeInput(input_bus->input_port()));
			const size_t input_start = operations.events.size();
			if (latch_failure_after_input_event != 0) {
				operations.latch_failure_event = operations.event_count +
					latch_failure_after_input_event;
				operations.latch_failure_broker = &broker;
				operations.latch_failure_generation = generation;
			}
			const NativeInputEvent press = {
				NativeInputKind::digital, static_cast<uint8_t>(input_player),
				0x12u, true, false, 0,
				{static_cast<uint8_t>(input_player), 1}};
			SpiReceipt delivered = {};
			const Result delivery_result =
				native_input->Deliver(input_profile, *input, press, &delivered);
			if (latch_failure_after_input_event != 0) {
				assert(delivery_result == MISTER_RESULT_INVALID_STATE);
				assert(operations.events.size() == input_start +
					latch_failure_after_input_event);
				assert(operations.sequence_at_failure_latch != 0 &&
					broker.mutation_sequence_for_test() ==
					operations.sequence_at_failure_latch +
						(latch_failure_after_input_event == 18 ? 0 : 1));
				assert(delivered.target_may_be_selected &&
					delivered.mapping_retained);
				input.reset();
			} else {
				assert(delivery_result == MISTER_RESULT_OK);
			}
			if (latch_failure_after_input_event == 0) {
			assert(delivered.completed_words == 2 && delivered.deselected &&
				delivered.mapping_retained && delivered.mutation_sequence == 12);
			std::vector<uint32_t> writes;
			for (size_t index = input_start; index != operations.events.size(); ++index) {
				const Event &event = operations.events[index];
				if (event.kind == EventKind::write && event.mapping == 1 &&
					event.offset == 0x10) writes.push_back(event.value);
			}
			const uint32_t command = input_player == 0 ? 0x02u : 0x03u;
			const uint32_t expected[] = {
				0x92305678u,
				0x92300000u | command, 0x92320000u | command,
				0x92300000u | command,
				0x92300012u, 0x92320012u, 0x92300012u, 0x92200012u
			};
			assert(writes == std::vector<uint32_t>(expected, expected + 8));
			if (advance_same_map) {
				const size_t events_before_same_map = operations.events.size();
				const uint64_t mutation_before_same_map =
					broker.mutation_sequence_for_test();
				const NativeInputEvent newer_same_map = {
					NativeInputKind::digital, static_cast<uint8_t>(input_player),
					0x12u, true, false, 0,
					{static_cast<uint8_t>(input_player), 2}};
				assert(native_input->Deliver(input_profile, *input, newer_same_map,
					&delivered) == MISTER_RESULT_OK);
				assert(delivered.result == MISTER_RESULT_OK && !delivered.selected &&
					delivered.completed_words == 0 &&
					!delivered.ack_low_observed &&
					delivered.response_words_observed == 0 &&
					delivered.captured_words == 0 &&
					!delivered.ack_high_observed &&
					!delivered.select_attempted &&
					!delivered.deselect_attempted && !delivered.deselected &&
					!delivered.strobe_low_observed &&
					!delivered.force_strobe_low_attempted &&
					!delivered.force_strobe_low_applied &&
					!delivered.force_strobe_low_observed &&
					!delivered.target_may_be_selected &&
					!delivered.strobe_may_be_high &&
					!delivered.mapping_retained &&
					delivered.mutation_sequence == 0);
				assert(operations.events.size() == events_before_same_map &&
					broker.mutation_sequence_for_test() == mutation_before_same_map);
				DeliveredInput logical = {};
				assert(native_input->GetDeliveredInput(0, &logical) &&
					logical.player == input_player && logical.map == 0x12u &&
					logical.identity.player == input_player &&
					logical.identity.sequence == 2);
			}
			if (latch_failure_before_cleanup)
				assert(broker.LatchFailure(generation) == MISTER_RESULT_OK);
			if (!cleanup_replay) {
				input.reset();
				assert(broker.Begin(generation, OperationKind::input, UINT64_MAX,
					&input) == MISTER_RESULT_OK);
				const NativeInputEvent neutral = {
					NativeInputKind::digital, static_cast<uint8_t>(input_player),
					0, false, false, 0,
					{static_cast<uint8_t>(input_player), 2}};
				assert(native_input->Deliver(input_profile, *input, neutral,
					&delivered) == MISTER_RESULT_OK);
				assert(native_input->ledger_size() == 0);
			}
			}
		}
	}
	assert(artifact.Close() == NativeArtifactResult::ok);
	operations.fail_event = 0;
	operations.now_ms = 1000;
	assert(broker.Quiesce(generation, 5100) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 3000, 6000, &cleanup) ==
		MISTER_RESULT_OK);
	if (cleanup_replay && native_input && input_player >= 0) {
		std::unique_ptr<OperationLease> cleanup_input;
		assert(broker.BeginCleanupOperation(*cleanup, OperationKind::input,
			&cleanup_input) == MISTER_RESULT_OK);
		const NativeDigitalNeutral neutral = {
			static_cast<uint8_t>(input_player),
			{input_profile->input.player_command[input_player], 0}};
		SpiReceipt neutral_receipt = {};
		const size_t cleanup_start = operations.events.size();
		if (reconcile_unselected_strobe) {
			operations.advance_after_event = operations.event_count + 5;
			operations.advance_to_ms = 3000;
			assert(native_input->ReplayDigitalNeutral(input_profile, *cleanup_input,
				neutral, &neutral_receipt) == MISTER_RESULT_DEADLINE);
			assert(neutral_receipt.target_may_be_selected &&
				neutral_receipt.strobe_may_be_high &&
				native_input->ledger_size() == 1);
			operations.advance_after_event = 0;
			operations.now_ms = 1000;
			operations.core = (operations.core & ~0x00120000u) | 0x00020000u;
			const size_t prefix_start = operations.events.size();
			assert(native_input->ReplayDigitalNeutral(input_profile, *cleanup_input,
				neutral, &neutral_receipt) == MISTER_RESULT_OK);
			std::vector<uint32_t> prefix_writes;
			for (size_t index = prefix_start; index != operations.events.size(); ++index) {
				const Event &event = operations.events[index];
				if (event.kind == EventKind::write && event.mapping == 1 &&
					event.offset == 0x10) prefix_writes.push_back(event.value);
			}
			assert(prefix_writes.size() >= 2);
			assert((prefix_writes[0] & 0x00120000u) == 0);
			assert((prefix_writes[1] & 0x00120000u) == 0x00100000u);
			assert(neutral_receipt.force_strobe_low_attempted &&
				neutral_receipt.force_strobe_low_applied &&
				neutral_receipt.force_strobe_low_observed);
		} else if (cleanup_advance_offset != 0) {
			operations.advance_after_event = operations.event_count +
				cleanup_advance_offset;
			operations.advance_to_ms = cleanup_advance_after ? 3001 : 3000;
			assert(native_input->ReplayDigitalNeutral(input_profile, *cleanup_input,
				neutral, &neutral_receipt) == MISTER_RESULT_DEADLINE);
			assert(native_input->ledger_size() == 1);
			operations.advance_after_event = 0;
			operations.now_ms = 1000;
			assert(native_input->ReplayDigitalNeutral(input_profile, *cleanup_input,
				neutral, &neutral_receipt) == MISTER_RESULT_OK);
		} else {
		assert(native_input->ReplayDigitalNeutral(input_profile, *cleanup_input,
			neutral, &neutral_receipt) == MISTER_RESULT_OK);
		}
		result.cleanup_replay_events = operations.events.size() - cleanup_start;
		assert(neutral_receipt.completed_words == 2 &&
			neutral_receipt.deselected && native_input->ledger_size() == 0);
		const size_t before_retry = operations.events.size();
		assert(native_input->ReplayDigitalNeutral(input_profile, *cleanup_input,
			neutral, &neutral_receipt) == MISTER_RESULT_OK);
		assert(operations.events.size() == before_retry);
	}
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginCleanupOperation(*cleanup,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*cleanup, *terminal) == MISTER_RESULT_OK);
	terminal.reset();
	assert(broker.Leave(generation, std::move(cleanup)) == MISTER_RESULT_OK);
	assert(!operations.AnyHeld() && !operations.descriptor_open);
	assert(unlink(file.c_str()) == 0 && rmdir(directory.c_str()) == 0 &&
		rmdir(root.c_str()) == 0);
	return result;
}

void TestNewerSameMapNormalAndFailureLatchedCleanupReachTerminal()
{
	const char *systems[] = {"snes", "megadrive"};
	for (const char *system : systems) {
		for (int player = 0; player != 2; ++player) {
			for (int failure_latched = 0; failure_latched != 2; ++failure_latched) {
				const BridgeBoundaryResult result = RunBridgeBoundary(0, false,
					system, 0, player, 0, true, false, 0, false, true,
					failure_latched != 0);
				assert(result.receipt.result == MISTER_RESULT_OK &&
					result.cleanup_replay_events != 0);
			}
		}
	}
}

void TestCleanupReadFirstPreservesIndependentLinuxSelectAndStrobeBits()
{
	const BridgeBoundaryResult result =
		RunBridgeBoundary(0, false, "snes", 0, 0, 0, true, true);
	assert(result.receipt.result == MISTER_RESULT_OK);
}

void TestCleanupDeadlineEqualityAndAfterEveryLinuxPrimitive()
{
	const BridgeBoundaryResult baseline =
		RunBridgeBoundary(0, false, "snes", 0, 0, 0, true);
	assert(baseline.cleanup_replay_events != 0);
	for (size_t offset = 1; offset <= baseline.cleanup_replay_events; ++offset) {
		const BridgeBoundaryResult equal = RunBridgeBoundary(
			0, false, "snes", 0, 0, 0, true, false, offset, false);
		assert(equal.receipt.result == MISTER_RESULT_OK);
		const BridgeBoundaryResult after = RunBridgeBoundary(
			0, false, "snes", 0, 0, 0, true, false, offset, true);
		assert(after.receipt.result == MISTER_RESULT_OK);
	}
}

bool PrefixContainsRead(const std::vector<Event> &events, size_t count,
	uintptr_t mapping)
{
	for (size_t index = 0; index != count; ++index)
		if (events[index].kind == EventKind::read &&
			events[index].mapping == mapping) return true;
	return false;
}

size_t PrefixReadCount(const std::vector<Event> &events, size_t count,
	uintptr_t mapping)
{
	size_t reads = 0;
	for (size_t index = 0; index != count; ++index)
		if (events[index].kind == EventKind::read &&
			events[index].mapping == mapping) ++reads;
	return reads;
}

bool PrefixContainsWrite(const std::vector<Event> &events, size_t count)
{
	for (size_t index = 0; index != count; ++index)
		if (events[index].kind == EventKind::write) return true;
	return false;
}

void AssertBridgeReceiptPrefix(const BridgeBoundaryResult &result,
	size_t committed_count, size_t applied_count)
{
	const bool applied = PrefixContainsWrite(result.bridge_events, applied_count);
	assert(result.receipt.acquired);
	assert(result.receipt.mutation_applied == applied);
	assert(result.after_sequence == result.before_sequence + (applied ? 1 : 0));
	assert(result.receipt.mutation_sequence ==
		(applied ? result.after_sequence : 0));
	assert(result.receipt.sdr_ports_observed ==
		PrefixContainsRead(result.bridge_events, committed_count, 3));
	assert(result.receipt.bridge_release_observed ==
		PrefixContainsRead(result.bridge_events, committed_count, 4));
	assert(result.receipt.remap_observed ==
		PrefixContainsRead(result.bridge_events, committed_count, 5));
	const bool core_attempted = [&]() {
		for (const Event &event : result.bridge_events)
			if (event.mapping == 1 && event.offset == 0x10u) return true;
		return false;
	}();
	const bool core_observed =
		PrefixReadCount(result.bridge_events, committed_count, 1) == 2;
	assert(result.receipt.core_normal_write_attempted == core_attempted);
	assert(result.receipt.core_normal_observed == core_observed);
	if (core_observed)
		assert((result.receipt.observed_core_gpo & 0xc0000000u) == 0x80000000u);
	else
		assert(result.receipt.observed_core_gpo == 0);
}

void TestBridgeAcquiredAndAppliedReceiptsAreDistinctAtEveryPrimitive()
{
	const BridgeBoundaryResult baseline = RunBridgeBoundary(0, false);
	assert(!baseline.bridge_events.empty());
	BridgeBoundaryResult result = RunBridgeBoundary(0, true);
	assert(result.receipt.result == MISTER_RESULT_DEADLINE);
	assert(result.receipt.acquired && !result.receipt.mutation_applied);
	assert(result.after_sequence == result.before_sequence);
	for (size_t offset = 1; offset <= baseline.bridge_events.size(); ++offset) {
		const BridgeBoundaryResult platform = RunBridgeBoundary(offset, false);
		assert(platform.receipt.result == MISTER_RESULT_PLATFORM);
		const bool platform_applied =
			PrefixContainsWrite(baseline.bridge_events, offset - 1);
		assert(platform.bridge_events.size() >= offset);
		assert(SamePrimitive(platform.bridge_events[offset - 1],
			baseline.bridge_events[offset - 1]));
		AssertBridgeReceiptPrefix(platform, offset - 1, offset - 1);
		assert(platform.receipt.mutation_applied == platform_applied);

		const BridgeBoundaryResult before = offset == 1 ?
			RunBridgeBoundary(0, true) :
			RunBridgeBoundary(0, false, "snes", offset - 1);
		assert(before.receipt.result == MISTER_RESULT_DEADLINE);
		const size_t before_events = offset == 1 ? 0 : offset - 1;
		assert(before.bridge_events.size() == before_events);
		AssertBridgeReceiptPrefix(before,
			before_events == 0 ? 0 : before_events - 1, before_events);

		const BridgeBoundaryResult after =
			RunBridgeBoundary(0, false, "snes", offset);
		assert(after.receipt.result == MISTER_RESULT_DEADLINE);
		assert(after.bridge_events.size() == offset);
		assert(SamePrimitive(after.bridge_events[offset - 1],
			baseline.bridge_events[offset - 1]));
		AssertBridgeReceiptPrefix(after, offset - 1, offset);
		if (offset == baseline.bridge_events.size()) {
			assert(after.receipt.core_normal_write_attempted &&
				!after.receipt.core_normal_observed);
		}
	}
	result = RunBridgeBoundary(0, false);
	assert(result.receipt.result == MISTER_RESULT_OK);
	assert(result.receipt.acquired && result.receipt.mutation_applied);
	assert(result.after_sequence == result.before_sequence + 1);
}

void TestProgramFailureAndDeadlineAtEverySemanticPrimitive()
{
	const size_t acquisition_events = 8;
	const ProgramBoundaryResult baseline = RunProgramBoundary(0, 0);
	assert(baseline.receipt.result == MISTER_RESULT_OK &&
		!baseline.program_events.empty());
	for (size_t offset = 1; offset <= baseline.program_events.size(); ++offset) {
		ProgramBoundaryResult result =
			RunProgramBoundary(acquisition_events + offset, 0);
		assert(result.receipt.result == MISTER_RESULT_PLATFORM);
		assert(result.receipt.acquired && result.receipt.mutation_sequence ==
			result.broker_sequence);
		assert(result.receipt.accepted_bytes <= 5 && result.broker_sequence <= 3);
		assert(result.data_words == (result.receipt.accepted_bytes + 3) / 4);
		assert(result.program_events.size() >= offset);
		assert(SamePrimitive(result.program_events[offset - 1],
			baseline.program_events[offset - 1]));

		result = RunProgramBoundary(0, acquisition_events + offset);
		assert(result.receipt.result == MISTER_RESULT_DEADLINE);
		assert(result.receipt.acquired && result.receipt.mutation_sequence ==
			result.broker_sequence);
		assert(result.receipt.accepted_bytes <= 5 && result.broker_sequence <= 3);
		assert(result.data_words == (result.receipt.accepted_bytes + 3) / 4);
		assert(result.program_events.size() >= offset);
		assert(SamePrimitive(result.program_events[offset - 1],
			baseline.program_events[offset - 1]));
	}
}

void TestTwoHundredAlternatingConcreteProgramTerminalRecoveryCycles()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	for (int cycle = 0; cycle != 200; ++cycle) {
		const char *const system = (cycle & 1) == 0 ? "snes" : "megadrive";
		const int player = (cycle / 2) & 1;
		const BridgeBoundaryResult active =
			RunBridgeBoundary(0, false, system, 0, player, 0, true);
		assert(active.receipt.result == MISTER_RESULT_OK &&
			active.receipt.mutation_applied &&
			active.after_sequence == active.before_sequence + 1 &&
			active.cleanup_replay_events != 0);

		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
		assert(operations.Count(EventKind::map) == 5 &&
			operations.Count(EventKind::write) == 0 && operations.AnyHeld());
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		terminal.reset();
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == closure);
		assert(!operations.AnyHeld() && !operations.descriptor_open &&
			operations.Count(EventKind::unmap) == 5);
	}
}

void TestEveryPostSelectPrimitiveRevalidatesTheExactInputView()
{
	// Select readback completes at event 4. The subsequent boundaries are the
	// word write, strobe-high, ACK read, and final deselect respectively.
	const size_t invalidation_boundaries[] = {4, 8, 12, 18};
	for (size_t boundary : invalidation_boundaries) {
		const BridgeBoundaryResult result = RunBridgeBoundary(
			0, false, "snes", 0, 0, boundary);
		assert(result.receipt.result == MISTER_RESULT_OK);
	}
}

void TestProgramReceiptsRetainAppliedStateAcrossLateBoundaries()
{
	const size_t begin_boundaries[] = {10, 11};
	for (size_t boundary : begin_boundaries) {
		const ProgramBoundaryResult result = RunProgramBoundary(0, boundary);
		assert(result.receipt.result == MISTER_RESULT_DEADLINE);
		assert(result.receipt.acquired && result.receipt.accepted_bytes == 0);
		assert(result.receipt.mutation_sequence == 1 &&
			result.broker_sequence == 1 && result.data_words == 0);
	}
	ProgramBoundaryResult result = RunProgramBoundary(11, 0);
	assert(result.receipt.result == MISTER_RESULT_PLATFORM);
	assert(result.receipt.acquired && result.receipt.accepted_bytes == 0);
	assert(result.receipt.mutation_sequence == 1 &&
		result.broker_sequence == 1 && result.data_words == 0);
	result = RunProgramBoundary(10, 0);
	assert(result.receipt.result == MISTER_RESULT_PLATFORM);
	assert(result.receipt.acquired && result.receipt.accepted_bytes == 0);
	assert(result.receipt.mutation_sequence == 0 &&
		result.broker_sequence == 0 && result.data_words == 0);

	const size_t data_boundaries[] = {35, 36};
	for (size_t boundary : data_boundaries) {
		result = RunProgramBoundary(0, boundary);
		assert(result.receipt.result == MISTER_RESULT_DEADLINE);
		assert(result.receipt.acquired && result.receipt.accepted_bytes == 4);
		assert(result.receipt.mutation_sequence == 2 &&
			result.broker_sequence == 2 && result.data_words == 1);
	}
	result = RunProgramBoundary(36, 0);
	assert(result.receipt.result == MISTER_RESULT_PLATFORM);
	assert(result.receipt.acquired && result.receipt.accepted_bytes == 4);
	assert(result.receipt.mutation_sequence == 2 &&
		result.broker_sequence == 2 && result.data_words == 1);
	result = RunProgramBoundary(35, 0);
	assert(result.receipt.result == MISTER_RESULT_PLATFORM);
	assert(result.receipt.acquired && result.receipt.accepted_bytes == 0);
	assert(result.receipt.mutation_sequence == 1 &&
		result.broker_sequence == 1 && result.data_words == 0);

	result = RunProgramBoundary(39, 0);
	assert(result.receipt.result == MISTER_RESULT_PLATFORM);
	assert(result.receipt.accepted_bytes == 5 &&
		result.receipt.mutation_sequence == 2 && result.broker_sequence == 2);
	result = RunProgramBoundary(42, 0);
	assert(result.receipt.result == MISTER_RESULT_PLATFORM);
	assert(result.receipt.accepted_bytes == 5 &&
		result.receipt.mutation_sequence == 3 && result.broker_sequence == 3);
}

void TestPosixVolatileAccessAlignmentAndOverflowChecks()
{
	assert(NativeLinuxMmioProductionPrimitivesRejectInvalidAccessForTest());
}

void TestManagerResidueIsNeutralizedBeforeMappingRelease()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.manager_ctrl = 0x101u;
	operations.manager_stat = 3u;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
	assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
	assert((operations.manager_ctrl & 0x107u) == 0x2u);
	assert((operations.manager_stat & 7u) <= 4u);
	assert(!operations.held[6]);
	const size_t first_unmap = [&]() {
		for (size_t index = 0; index != operations.events.size(); ++index)
			if (operations.events[index].kind == EventKind::unmap) return index;
		return operations.events.size();
	}();
	bool neutral_write_before_unmap = false;
	for (size_t index = 0; index < first_unmap; ++index) {
		const Event &event = operations.events[index];
		if (event.kind == EventKind::write && event.mapping == 1 &&
			event.offset == 4 && (event.value & 0x107u) == 0x2u)
			neutral_write_before_unmap = true;
	}
	assert(neutral_write_before_unmap);
	terminal.reset();
	assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
}

void TestAppliedTerminalWriteIsRecordedAcrossLateFailureBoundaries()
{
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		operations.fail_event = 9;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_PLATFORM);
		assert((operations.core & 0xc0000000u) == 0x80000000u);
		assert(broker.mutation_sequence_for_test() == 0);
	}
	for (int boundary = 0; boundary != 3; ++boundary) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		if (boundary == 0) operations.fail_event = 10;
		else {
			operations.advance_after_event = boundary == 1 ? 9 : 10;
			operations.advance_to_ms = 6000;
		}
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		const Result result = containment.ResetAndContain(*epoch, *terminal);
		assert(result == (boundary == 0 ? MISTER_RESULT_PLATFORM :
			MISTER_RESULT_DEADLINE));
		assert((operations.core & 0xc0000000u) == 0x40000000u);
		assert(broker.mutation_sequence_for_test() == 1);
		assert(operations.Count(EventKind::unmap) == 0);
	}
}

void TestAppliedManagerNeutralWriteIsRecordedWhenItsBarrierFails()
{
	FakeClock baseline_clock(1000);
	HardwareBroker baseline_broker(baseline_clock);
	FakeLinuxOperations baseline_operations;
	baseline_operations.manager_ctrl = 0x101u;
	baseline_operations.manager_stat = 3u;
	NativeLinuxMmioAdapter baseline_adapter(baseline_operations);
	NativeContainment baseline_containment(baseline_broker, baseline_adapter);
	PlatformGenerationId baseline_generation = 0;
	std::unique_ptr<CleanupEpoch> baseline_epoch;
	std::unique_ptr<OperationLease> baseline_terminal;
	PrepareCleanup(baseline_broker, baseline_clock, &baseline_generation,
		&baseline_epoch, &baseline_terminal);
	assert(baseline_containment.ResetAndContain(*baseline_epoch,
		*baseline_terminal) == MISTER_RESULT_OK);
	size_t manager_barrier = 0;
	for (size_t index = 0; index + 1 < baseline_operations.events.size(); ++index) {
		const Event &event = baseline_operations.events[index];
		if (event.kind == EventKind::write && event.mapping == 1 &&
			event.offset == 4 && (event.value & 0x107u) == 0x5u) {
			assert(baseline_operations.events[index + 1].kind == EventKind::barrier);
			manager_barrier = index + 2;
			break;
		}
	}
	assert(manager_barrier != 0);
	const size_t manager_write = manager_barrier - 1;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		operations.manager_ctrl = 0x101u;
		operations.manager_stat = 3u;
		operations.fail_event = manager_write;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_PLATFORM);
		assert((operations.manager_ctrl & 0x107u) == 0x101u);
		assert(broker.mutation_sequence_for_test() == 5);
	}

	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.manager_ctrl = 0x101u;
	operations.manager_stat = 3u;
	operations.fail_event = manager_barrier;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_PLATFORM);
	assert((operations.manager_ctrl & 0x107u) == 0x5u);
	assert(broker.mutation_sequence_for_test() == 6);
	assert(operations.Count(EventKind::unmap) == 0);
}

void TestProcessExitMappingCloseMakesNoBrokerReceipt()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> program;
	assert(broker.Begin(generation, OperationKind::program_fpga, 5000,
		&program) == MISTER_RESULT_OK);
	assert(containment.AcquireMappings(*program).complete);
	assert(adapter.CloseMappingsForProcessExit() == MISTER_RESULT_OK);
	assert(!operations.AnyHeld() && !operations.descriptor_open);
	assert(broker.mutation_sequence_for_test() == 0);
	assert(broker.containment_receipt_sequence_for_test() == 0);
	assert(containment.AcquireMappings(*program).result ==
		MISTER_RESULT_INVALID_STATE);
}

void TestEveryInjectedLinuxFailureIsTruthfulAndRetryable()
{
	for (size_t failure = 1; failure <= 32; ++failure) {
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
		if (failure == 7) {
			assert(containment.ResetAndContain(*epoch, *terminal) ==
				MISTER_RESULT_CLEANUP_INCOMPLETE);
			continue;
		}
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
	for (size_t boundary = 1; boundary <= 32; ++boundary) {
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

void TestNonNeutralManagerFailureAndDeadlineAtEverySemanticPrimitive()
{
	std::vector<Event> baseline;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		operations.manager_ctrl = 0x101u;
		operations.manager_stat = 3u;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		baseline = operations.events;
	}
	assert(!baseline.empty());
	for (size_t boundary = 1; boundary <= baseline.size(); ++boundary) {
		for (int deadline = 0; deadline != 2; ++deadline) {
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeLinuxOperations operations;
			operations.manager_ctrl = 0x101u;
			operations.manager_stat = 3u;
			if (deadline != 0) {
				operations.advance_after_event = boundary;
				operations.advance_to_ms = 6000;
			} else {
				operations.fail_event = boundary;
			}
			NativeLinuxMmioAdapter adapter(operations);
			NativeContainment containment(broker, adapter);
			PlatformGenerationId generation = 0;
			std::unique_ptr<CleanupEpoch> epoch;
			std::unique_ptr<OperationLease> terminal;
			PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
			const Result result = containment.ResetAndContain(*epoch, *terminal);
			assert(result == (deadline != 0 ? MISTER_RESULT_DEADLINE :
				MISTER_RESULT_PLATFORM) ||
				(!deadline && result == MISTER_RESULT_CLEANUP_INCOMPLETE));
			assert(broker.containment_receipt_sequence_for_test() == 0);
			assert(operations.events.size() >= boundary);
			assert(SamePrimitive(operations.events[boundary - 1],
				baseline[boundary - 1]));
			operations.fail_event = 0;
			operations.advance_after_event = 0;
			operations.now_ms = 1000;
			(void)adapter.CloseMappingsForProcessExit();
		}
	}
}

void TestFreshRecoveryFailureAndDeadlineAtEverySemanticPrimitive()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::vector<Event> baseline;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
		baseline = operations.events;
	}
	assert(!baseline.empty());
	for (size_t boundary = 1; boundary <= baseline.size(); ++boundary) {
		for (int deadline = 0; deadline != 2; ++deadline) {
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeLinuxOperations operations;
			if (deadline != 0) {
				operations.advance_after_event = boundary;
				operations.advance_to_ms = 6000;
			} else {
				operations.fail_event = boundary;
			}
			NativeLinuxMmioAdapter adapter(operations);
			NativeContainment containment(broker, adapter);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			assert(containment.ObserveRecovery(*epoch) != MISTER_RESULT_OK);
			assert(operations.Count(EventKind::write) == 0 &&
				operations.events.size() >= boundary);
			assert(SamePrimitive(operations.events[boundary - 1],
				baseline[boundary - 1]));
			MisterRecoveryObservationV2 observation = Observation();
			assert(broker.FinishRecovery(std::move(epoch), &observation) ==
				MISTER_RESULT_CLEANUP_INCOMPLETE);
			assert((observation.neutral_resource_flags & closure) == 0);
			operations.fail_event = 0;
			operations.advance_after_event = 0;
			operations.now_ms = 1000;
			(void)adapter.CloseMappingsForProcessExit();
		}
	}
}

void TestRecoveryNonNeutralManagerFailureDeadlineAndRetryAtEveryPrimitive()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::vector<Event> baseline;
	size_t manager_start = 0;
	size_t manager_end = 0;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeLinuxOperations operations;
		operations.manager_ctrl = 0x101u;
		operations.manager_stat = 3u;
		NativeLinuxMmioAdapter adapter(operations);
		NativeContainment containment(broker, adapter);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		baseline = operations.events;
		for (size_t index = 0; index != baseline.size(); ++index) {
			const Event &event = baseline[index];
			if (manager_start == 0 && event.kind == EventKind::read &&
				event.mapping == 1 && event.offset == 4)
				manager_start = index;
			else if (manager_start != 0 && event.kind == EventKind::read &&
				event.mapping == 1 && event.offset == 0x10u) {
				manager_end = index;
				break;
			}
		}
		assert(manager_start != 0 && manager_end > manager_start);
		terminal.reset();
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
	}

	for (size_t boundary = manager_start; boundary != manager_end; ++boundary) {
		for (int mode = 0; mode != 3; ++mode) {
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeLinuxOperations operations;
			operations.manager_ctrl = 0x101u;
			operations.manager_stat = 3u;
			if (mode != 0) {
				operations.advance_after_event =
					mode == 1 ? boundary : boundary + 1;
				operations.advance_to_ms = 6000;
			} else {
				operations.fail_event = boundary + 1;
			}
			NativeLinuxMmioAdapter adapter(operations);
			NativeContainment containment(broker, adapter);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> terminal;
			assert(broker.BeginRecoveryOperation(*epoch,
				OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
			assert(terminal->absolute_deadline_ms() == 6000);
			const Result first = containment.ResetAndContain(*epoch, *terminal);
			assert(first == (mode != 0 ? MISTER_RESULT_DEADLINE :
				MISTER_RESULT_PLATFORM));
			if (mode == 1) {
				assert(operations.events.size() == boundary);
			} else {
				assert(operations.events.size() >= boundary + 1);
				assert(SamePrimitive(operations.events[boundary], baseline[boundary]));
			}
			assert(operations.Count(EventKind::unmap) == 0 &&
				operations.AnyHeld() && !operations.descriptor_open);
			assert(broker.containment_receipt_sequence_for_test() == 0);

			const size_t applied_end = boundary + (mode == 2 ? 1 : 0);
			std::vector<uint32_t> applied_controls;
			for (size_t index = manager_start; index != applied_end; ++index) {
				const Event &event = operations.events[index];
				if (event.kind == EventKind::write && event.mapping == 1 &&
					event.offset == 4) applied_controls.push_back(event.value & 0x107u);
			}
			assert(broker.mutation_sequence_for_test() ==
				5 + (applied_controls.empty() ? 0 : 1));

			const size_t retry_start = operations.events.size();
			operations.fail_event = 0;
			operations.advance_after_event = 0;
			operations.now_ms = 1000;
			assert(terminal->absolute_deadline_ms() == 6000);
			assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
			for (size_t index = retry_start; index != operations.events.size(); ++index) {
				const Event &event = operations.events[index];
				if (event.kind != EventKind::write || event.mapping != 1 ||
					event.offset != 4) continue;
				for (uint32_t applied : applied_controls)
					assert((event.value & 0x107u) != applied);
			}
			assert((operations.manager_ctrl & 0x107u) == 0x2u &&
				(operations.manager_stat & 7u) <= 4u);
			assert(!operations.AnyHeld() && !operations.descriptor_open &&
				operations.Count(EventKind::unmap) == 5);
			uint32_t manager_control = 0;
			uint32_t manager_mode = UINT32_MAX;
			uint64_t manager_sequence = 0;
			assert(broker.containment_manager_receipt_for_test(&manager_control,
				&manager_mode, &manager_sequence));
			assert((manager_control & 0x107u) == 0x2u && manager_mode <= 4u);
			assert(manager_sequence + 1 ==
				broker.containment_receipt_sequence_for_test());
			assert(broker.containment_receipt_sequence_for_test() ==
				broker.mutation_sequence_for_test());
			terminal.reset();
			MisterRecoveryObservationV2 observation = Observation();
			assert(broker.FinishRecovery(std::move(epoch), &observation) ==
				MISTER_RESULT_OK);
			assert(observation.neutral_resource_flags == closure);
		}
	}
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
	operations.fail_event = 28;
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

void TestFreshRecoveryObservationMapsOnlyContainmentAndNeverWrites()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	assert(operations.Count(EventKind::open) == 1);
	assert(operations.Count(EventKind::map) == 5);
	assert(operations.Count(EventKind::close) == 1);
	assert(operations.Count(EventKind::read) == 7);
	assert(operations.Count(EventKind::write) == 0);
	assert(operations.Count(EventKind::barrier) == 0);
	assert(!operations.held[6]);

	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
	assert(!operations.AnyHeld() && !operations.descriptor_open);
	terminal.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == closure);
}

void TestFpgaOnlyRecoveryCannotReportNeutralWhileMappingsRemainHeld()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.core = 0x52345678u;
	operations.interface_module = 0;
	operations.sdr = 0;
	operations.bridge = 7;
	operations.remap = 1;
	NativeLinuxMmioAdapter adapter(operations);
	NativeContainment containment(broker, adapter);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	assert(operations.Count(EventKind::map) == 5);
	assert(operations.Count(EventKind::unmap) == 0);
	assert(operations.AnyHeld());
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert((observation.neutral_resource_flags & MISTER_RESOURCE_FPGA) == 0);
	assert(operations.Count(EventKind::write) == 0);
	assert(operations.Count(EventKind::unmap) == 0);
}

void TestCleanupReleaseResumeAcceptsOnlyFreshLeaseForSameEpoch()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeLinuxOperations operations;
	operations.fail_event = 29;
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
	operations.fail_event = 28;
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
	TestPurposeNamedUserIoCapabilityConstructsOnlyTypedInputPort();
	TestExactMappingsTraceAndPermanentClosure();
	TestSixPageProgramBridgeAndTerminalCycle();
	TestMappingOnlyCannotValidateBridgeActivationAuthority();
	TestProgramReceiptsRetainAppliedStateAcrossLateBoundaries();
	TestProgramFailureAndDeadlineAtEverySemanticPrimitive();
	TestBridgeAcquiredAndAppliedReceiptsAreDistinctAtEveryPrimitive();
	TestTwoHundredAlternatingConcreteProgramTerminalRecoveryCycles();
	TestNewerSameMapNormalAndFailureLatchedCleanupReachTerminal();
	TestCleanupReadFirstPreservesIndependentLinuxSelectAndStrobeBits();
	TestCleanupDeadlineEqualityAndAfterEveryLinuxPrimitive();
	TestEveryPostSelectPrimitiveRevalidatesTheExactInputView();
	TestPosixVolatileAccessAlignmentAndOverflowChecks();
	TestManagerResidueIsNeutralizedBeforeMappingRelease();
	TestAppliedTerminalWriteIsRecordedAcrossLateFailureBoundaries();
	TestAppliedManagerNeutralWriteIsRecordedWhenItsBarrierFails();
	TestProcessExitMappingCloseMakesNoBrokerReceipt();
	TestEveryInjectedLinuxFailureIsTruthfulAndRetryable();
	TestMapFailedAndMappingGeometryAreRejectedBeforeMutation();
	TestDeadlineBeforeAndAfterEveryLinuxBoundaryFailsClosed();
	TestNonNeutralManagerFailureAndDeadlineAtEverySemanticPrimitive();
	TestRejectedAuthorityProducesNoLinuxOperations();
	TestFreshRecoveryFailureAndDeadlineAtEverySemanticPrimitive();
	TestRecoveryNonNeutralManagerFailureDeadlineAndRetryAtEveryPrimitive();
	TestRecoveryReleaseFailureResumesWithoutRegisterReplay();
	TestFreshRecoveryObservationMapsOnlyContainmentAndNeverWrites();
	TestFpgaOnlyRecoveryCannotReportNeutralWhileMappingsRemainHeld();
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
