// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_spi_bus.hpp"
#include "runtime/native/native_core_profile.hpp"

#include <assert.h>

#include <condition_variable>
#include <thread>
#include <string>
#include <vector>
#include <type_traits>

namespace mister {
namespace native {

static_assert(!std::is_default_constructible<SpiReceiptCommitToken>::value,
	"SPI receipt commit token must be broker/input minted");
static_assert(!std::is_copy_constructible<SpiReceiptCommitToken>::value,
	"SPI receipt commit token must not be copied");
static_assert(!std::is_constructible<SpiReceiptCommitToken, void *,
	void *>::value, "SPI receipt commit token constructor must be private");
static_assert(!std::is_constructible<SpiReceiptCommitToken, void *,
	void (*)(void *, const SpiReceipt &)>::value,
	"SPI receipt commit callback constructor must be private");

namespace {

class TestClock final : public NativeClock {
public:
	 explicit TestClock(uint64_t now) : now_(now) {}
	uint64_t NowMs() const override { return now_; }
	bool WaitUntil(std::condition_variable &condition,
		std::unique_lock<std::mutex> &lock, uint64_t deadline) override
	{
		condition.wait(lock);
		return now_ < deadline;
	}
	void SetNow(uint64_t now) { now_ = now; }
	uint64_t &now_for_test() { return now_; }
private:
	uint64_t now_;
};

class FakeIo final : public NativeHardwareIo {
public:
	FakeIo() : fail_select(false), fail_word_at(-1), fail_deselect(false),
		advance_deadline_on_ack(false), advance_after_ack(0), now(nullptr), block_deselect(false),
		deselect_entered(false), release_deselect(false),
		force_deselect_deadline(false), deselect_deadline(0), ack_reads(),
		ack_index(0), words(), events() {}

	Result Select(const HardwareLeaseView &, uint32_t mask) override
	{
		events.push_back("select:" + std::to_string(mask));
		return fail_select ? MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}
	Result WriteWord(const HardwareLeaseView &, uint16_t word) override
	{
		words.push_back(word);
		events.push_back("word:" + std::to_string(word));
		if (fail_word_at >= 0 &&
			static_cast<int>(words.size() - 1) == fail_word_at)
			return MISTER_RESULT_PLATFORM;
		return MISTER_RESULT_OK;
	}
	Result ReadAck(const HardwareLeaseView &, bool *high) override
	{
		events.push_back("ack");
		if (ack_index >= ack_reads.size()) return MISTER_RESULT_PLATFORM;
		*high = ack_reads[ack_index++];
		if (advance_deadline_on_ack && now != nullptr &&
			(advance_after_ack == 0 || ack_index >= advance_after_ack))
			*now = 100;
		return MISTER_RESULT_OK;
	}
	Result Deselect(const HardwareLeaseView &, uint32_t mask,
		uint64_t absolute_deadline_ms) override
	{
		events.push_back("deselect:" + std::to_string(mask));
		deselect_deadline = absolute_deadline_ms;
		if (block_deselect) {
			std::unique_lock<std::mutex> lock(block_mutex);
			deselect_entered = true;
			block_condition.notify_all();
			block_condition.wait(lock, [this] { return release_deselect; });
		}
		if (force_deselect_deadline) {
			if (now != nullptr) *now = absolute_deadline_ms;
			return MISTER_RESULT_DEADLINE;
		}
		return fail_deselect ? MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}
	void WaitForDeselect()
	{
		std::unique_lock<std::mutex> lock(block_mutex);
		block_condition.wait(lock, [this] { return deselect_entered; });
	}
	void AllowDeselect()
	{
		std::lock_guard<std::mutex> lock(block_mutex);
		release_deselect = true;
		block_condition.notify_all();
	}

	bool fail_select;
	int fail_word_at;
	bool fail_deselect;
	bool advance_deadline_on_ack;
	size_t advance_after_ack;
	uint64_t *now;
	bool block_deselect;
	bool deselect_entered;
	bool release_deselect;
	bool force_deselect_deadline;
	uint64_t deselect_deadline;
	std::vector<bool> ack_reads;
	size_t ack_index;
	std::vector<uint16_t> words;
	std::vector<std::string> events;
	std::mutex block_mutex;
	std::condition_variable block_condition;
};

void Enter(HardwareBroker &broker, TestClock &clock,
	PlatformGenerationId *generation)
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	assert(broker.EnterFixtureForTest(profile, generation) == MISTER_RESULT_OK);
	assert(*generation != 0);
	(void)clock;
}

std::unique_ptr<OperationLease> Begin(HardwareBroker &broker,
	PlatformGenerationId generation, OperationKind kind)
{
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, kind, 100, &lease) == MISTER_RESULT_OK);
	return lease;
}

void TestCompleteTransactionAndReceipt()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	NativeSpiBus bus(clock, io);
	const uint16_t words[] = {0x1234, 0xabcd};
	SpiTransaction transaction = {
		0x10, words, 2, AckPolicy::required, 0x10};
	io.ack_reads = {true, false, true, false};
	SpiReceipt receipt = {};
	assert(bus.Execute(*lease, transaction, &receipt) == MISTER_RESULT_OK);
	assert(receipt.result == MISTER_RESULT_OK);
	assert(receipt.selected);
	assert(receipt.completed_words == 2);
	assert(receipt.ack_low_observed);
	assert(receipt.deselected);
	assert((io.events == std::vector<std::string>{
		"select:16", "word:4660", "ack", "ack", "word:43981",
		"ack", "ack", "deselect:16"}));
}

void TestFailureResidueAndNoOutputWithoutLease()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	NativeSpiBus bus(clock, io);
	const uint16_t words[] = {1, 2};
	SpiTransaction transaction = {1, words, 2, AckPolicy::required, 1};
	io.ack_reads = {true};
	SpiReceipt receipt = {};
	assert(bus.Execute(*lease, transaction, &receipt) == MISTER_RESULT_PLATFORM);
	assert(receipt.selected);
	assert(receipt.completed_words == 0);
	assert(!receipt.ack_low_observed);
	assert(receipt.deselected);
	io.events.clear();
	lease.reset();
	HardwareBroker nonhardware_broker(clock);
	PlatformGenerationId nonhardware_generation = 0;
	Enter(nonhardware_broker, clock, &nonhardware_generation);
	std::unique_ptr<OperationLease> nonhardware_lease = Begin(
		nonhardware_broker, nonhardware_generation, OperationKind::scheduler);
	assert(bus.Execute(*nonhardware_lease, transaction, &receipt) != MISTER_RESULT_OK);
	assert(io.events.empty());
}

void TestEveryBoundaryReportsObservedResidue()
{
	const uint16_t words[] = {1, 2};
	for (int boundary = 0; boundary < 6; ++boundary) {
		TestClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		Enter(broker, clock, &generation);
		std::unique_ptr<OperationLease> lease =
			Begin(broker, generation, OperationKind::input);
		FakeIo io;
		io.now = &clock.now_for_test();
		NativeSpiBus bus(clock, io);
		SpiTransaction transaction = {1, words, 2, AckPolicy::required, 1};
		SpiReceipt receipt = {};
		if (boundary == 0) io.fail_select = true;
		if (boundary == 1) io.fail_word_at = 0;
		if (boundary == 2) {
			io.fail_word_at = 1;
			io.ack_reads = std::vector<bool>{true, false};
		}
		if (boundary == 3) {
			io.ack_reads = std::vector<bool>{false};
			io.advance_deadline_on_ack = true;
		}
		if (boundary == 4) {
			io.ack_reads = std::vector<bool>{true, true};
			io.advance_deadline_on_ack = true;
			io.advance_after_ack = 2;
		}
		if (boundary == 5) io.force_deselect_deadline = true;
		const Result result = bus.Execute(*lease, transaction, &receipt);
		assert(result != MISTER_RESULT_OK);
		if (boundary == 0) {
			assert(!receipt.selected);
			assert(receipt.completed_words == 0);
			assert(!receipt.deselected);
		} else {
			assert(receipt.selected);
			if (boundary == 2) {
				assert(receipt.completed_words == 1);
				assert(receipt.ack_low_observed);
			} else {
				assert(receipt.completed_words == 0);
				assert(!receipt.ack_low_observed);
			}
			assert(boundary == 5 ? !receipt.deselected : receipt.deselected);
			if (boundary == 5) assert(io.deselect_deadline == 100);
		}
	}
}

void TestDeselectFailureIsDistinctFromDeadline()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	io.fail_deselect = true;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 3;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	assert(bus.Execute(*lease, transaction, &receipt) == MISTER_RESULT_PLATFORM);
	assert(receipt.selected);
	assert(receipt.completed_words == 1);
	assert(receipt.ack_low_observed);
	assert(!receipt.deselected);
	assert(io.deselect_deadline == 100);
}

void TestZeroWordTransactionIsRejectedBeforeSelect()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	NativeSpiBus bus(clock, io);
	SpiTransaction empty = {1, nullptr, 0, AckPolicy::required, 1};
	SpiReceipt rejected = {};
	assert(bus.Execute(*lease, empty, &rejected) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(rejected.result == MISTER_RESULT_INVALID_ARGUMENT);
	assert(!rejected.selected);
	assert(!rejected.deselected);
	assert(rejected.mutation_sequence == 0);
	assert(io.events.empty());

	const uint16_t word = 0x0102;
	io.ack_reads = std::vector<bool>{true, false};
	SpiTransaction probe = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	assert(bus.Execute(*lease, probe, &receipt) == MISTER_RESULT_OK);
	assert(receipt.mutation_sequence == 3);
}

void TestQuiesceCannotBisectDeselectOrReceipt()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	io.block_deselect = true;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 0x55aa;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	Result execute_result = MISTER_RESULT_PLATFORM;
	std::thread execute_thread([&] {
		execute_result = bus.Execute(*lease, transaction, &receipt);
	});
	io.WaitForDeselect();
	Result quiesce_result = MISTER_RESULT_PLATFORM;
	std::thread quiesce_thread([&] {
		quiesce_result = broker.Quiesce(generation, 100);
	});
	io.AllowDeselect();
	execute_thread.join();
	assert(execute_result == MISTER_RESULT_OK);
	assert(receipt.deselected);
	assert(receipt.completed_words == 1);
	lease.reset();
	quiesce_thread.join();
	assert(quiesce_result == MISTER_RESULT_OK);
}

void TestHardwareFenceSerializesCompetingLeases()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> first =
		Begin(broker, generation, OperationKind::input);
	std::unique_ptr<OperationLease> second =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	io.block_deselect = true;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 0x0102;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt first_receipt = {};
	SpiReceipt second_receipt = {};
	Result first_result = MISTER_RESULT_PLATFORM;
	Result second_result = MISTER_RESULT_PLATFORM;
	std::thread first_thread([&] {
		first_result = bus.Execute(*first, transaction, &first_receipt);
	});
	io.WaitForDeselect();
	std::thread second_thread([&] {
		second_result = bus.Execute(*second, transaction, &second_receipt);
	});
	second_thread.join();
	assert(second_result == MISTER_RESULT_INVALID_STATE);
	assert(!second_receipt.selected);
	assert(second_receipt.mutation_sequence == 0);
	const std::vector<std::string> expected_events = {
		"select:1", "word:258", "ack", "ack", "deselect:1"};
	assert(io.events == expected_events);
	io.AllowDeselect();
	first_thread.join();
	assert(first_result == MISTER_RESULT_OK);
	assert(first_receipt.mutation_sequence != 0);
	assert(first_receipt.deselected);
	first.reset();
	second.reset();
}

void TestRecoveryEpochDestructionDrainsConservatively()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 20, 30,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	epoch.reset();
	FakeIo io;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 1;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	assert(bus.Execute(*lease, transaction, &receipt) == MISTER_RESULT_OK);
	assert(receipt.mutation_sequence != 0);
	lease.reset();
	std::unique_ptr<RecoveryEpoch> replacement;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 20, 30,
		&replacement) == MISTER_RESULT_INVALID_STATE);
}

const OperationKind kAllKinds[] = {
	OperationKind::program_fpga, OperationKind::core_protocol,
	OperationKind::input, OperationKind::scheduler, OperationKind::offload,
	OperationKind::save, OperationKind::audio, OperationKind::video,
	OperationKind::content, OperationKind::input_descriptors,
	OperationKind::terminal_fpga_cleanup};

bool IsSpiHardwareKind(OperationKind kind)
{
	return kind == OperationKind::program_fpga ||
		kind == OperationKind::core_protocol || kind == OperationKind::input ||
		kind == OperationKind::audio || kind == OperationKind::video ||
		kind == OperationKind::terminal_fpga_cleanup;
}

void ExecuteMatrixCell(OperationLease &lease, OperationKind kind,
	TestClock &clock, uint64_t expected_sequence)
{
	FakeIo io;
	NativeSpiBus bus(clock, io);
	const uint16_t word = static_cast<uint16_t>(kind);
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	if (IsSpiHardwareKind(kind)) io.ack_reads = std::vector<bool>{true, false};
	const Result result = bus.Execute(lease, transaction, &receipt);
	if (IsSpiHardwareKind(kind)) {
		assert(result == MISTER_RESULT_OK);
		assert(receipt.selected);
		assert(receipt.completed_words == 1);
		assert(receipt.ack_low_observed);
		assert(receipt.deselected);
		assert(receipt.mutation_sequence == expected_sequence);
	} else {
		assert(result == MISTER_RESULT_INVALID_STATE);
		assert(io.events.empty());
		assert(receipt.mutation_sequence == expected_sequence);
	}
}

void TestExhaustiveAuthorityKindMatrix()
{
	for (size_t index = 0; index < sizeof(kAllKinds) / sizeof(kAllKinds[0]);
		++index) {
		TestClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		Enter(broker, clock, &generation);
		std::unique_ptr<OperationLease> lease;
		const Result begin_result = broker.Begin(generation, kAllKinds[index],
			100, &lease);
		if (kAllKinds[index] == OperationKind::terminal_fpga_cleanup) {
			assert(begin_result == MISTER_RESULT_INVALID_STATE);
			std::unique_ptr<OperationLease> probe;
			assert(broker.Begin(generation, OperationKind::core_protocol, 100,
				&probe) == MISTER_RESULT_OK);
			ExecuteMatrixCell(*probe, OperationKind::core_protocol, clock, 3);
			continue;
		}
		assert(begin_result == MISTER_RESULT_OK);
		ExecuteMatrixCell(*lease, kAllKinds[index], clock,
			IsSpiHardwareKind(kAllKinds[index]) ? 3 : 0);
		if (!IsSpiHardwareKind(kAllKinds[index])) {
			lease.reset();
			std::unique_ptr<OperationLease> probe;
			assert(broker.Begin(generation, OperationKind::core_protocol, 100,
				&probe) == MISTER_RESULT_OK);
			ExecuteMatrixCell(*probe, OperationKind::core_protocol, clock, 3);
		}
	}

	for (size_t index = 0; index < sizeof(kAllKinds) / sizeof(kAllKinds[0]);
		++index) {
		TestClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		Enter(broker, clock, &generation);
		std::unique_ptr<OperationLease> held;
		assert(broker.Begin(generation, OperationKind::input, 100, &held) ==
			MISTER_RESULT_OK);
		held.reset();
		assert(broker.Quiesce(generation, 100) == MISTER_RESULT_OK);
		std::unique_ptr<CleanupEpoch> epoch;
		assert(broker.BeginCleanup(generation, 200, 300, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		const Result begin_result = broker.BeginCleanupOperation(*epoch,
			kAllKinds[index], &lease);
		if (kAllKinds[index] == OperationKind::program_fpga) {
			assert(begin_result == MISTER_RESULT_INVALID_STATE);
			std::unique_ptr<OperationLease> probe;
			assert(broker.BeginCleanupOperation(*epoch,
				OperationKind::core_protocol, &probe) == MISTER_RESULT_OK);
			ExecuteMatrixCell(*probe, OperationKind::core_protocol, clock, 3);
			continue;
		}
		assert(begin_result == MISTER_RESULT_OK);
		if (kAllKinds[index] == OperationKind::terminal_fpga_cleanup) {
			// The terminal operation is admitted by cleanup authority but the
			// Task 1 broker deliberately cannot mint containment evidence yet.
			ExecuteMatrixCell(*lease, kAllKinds[index], clock,
				IsSpiHardwareKind(kAllKinds[index]) ? 3 : 0);
			assert(broker.ObserveContainment(*epoch, *lease) ==
				MISTER_RESULT_UNSUPPORTED);
		} else {
			ExecuteMatrixCell(*lease, kAllKinds[index], clock,
				IsSpiHardwareKind(kAllKinds[index]) ? 3 : 0);
		}
		if (!IsSpiHardwareKind(kAllKinds[index])) {
			lease.reset();
			std::unique_ptr<OperationLease> probe;
			assert(broker.BeginCleanupOperation(*epoch,
				OperationKind::core_protocol, &probe) == MISTER_RESULT_OK);
			ExecuteMatrixCell(*probe, OperationKind::core_protocol, clock, 3);
		}
	}

	for (size_t index = 0; index < sizeof(kAllKinds) / sizeof(kAllKinds[0]);
		++index) {
		TestClock clock(10);
		HardwareBroker broker(clock);
		std::unique_ptr<RecoveryEpoch> epoch;
		const uint32_t mask = MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL |
			MISTER_RESOURCE_CORE_INPUT | MISTER_RESOURCE_SAVES |
			MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO |
			MISTER_RESOURCE_CONTENT;
		assert(broker.BeginRecovery(mask, 200, 300, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		const Result begin_result = broker.BeginRecoveryOperation(*epoch,
			kAllKinds[index], &lease);
		const bool recovery_allowed =
			kAllKinds[index] == OperationKind::input_descriptors ||
			kAllKinds[index] == OperationKind::save ||
			kAllKinds[index] == OperationKind::audio ||
			kAllKinds[index] == OperationKind::video ||
			kAllKinds[index] == OperationKind::content ||
			kAllKinds[index] == OperationKind::core_protocol ||
			kAllKinds[index] == OperationKind::terminal_fpga_cleanup;
		if (!recovery_allowed) {
			assert(begin_result == MISTER_RESULT_INVALID_STATE);
			std::unique_ptr<OperationLease> probe;
			assert(broker.BeginRecoveryOperation(*epoch,
				OperationKind::core_protocol, &probe) == MISTER_RESULT_OK);
			ExecuteMatrixCell(*probe, OperationKind::core_protocol, clock, 3);
			continue;
		}
		assert(begin_result == MISTER_RESULT_OK);
		ExecuteMatrixCell(*lease, kAllKinds[index], clock,
			IsSpiHardwareKind(kAllKinds[index]) ? 3 : 0);
		if (!IsSpiHardwareKind(kAllKinds[index])) {
			lease.reset();
			std::unique_ptr<OperationLease> probe;
			assert(broker.BeginRecoveryOperation(*epoch,
				OperationKind::core_protocol, &probe) == MISTER_RESULT_OK);
			ExecuteMatrixCell(*probe, OperationKind::core_protocol, clock, 3);
		}
	}
}

void TestRecoveryMissingBitsAndTerminalClosure()
{
	const uint32_t resource_bits[] = {
		MISTER_RESOURCE_FPGA, MISTER_RESOURCE_BRIDGES,
		MISTER_RESOURCE_CORE_PROTOCOL, MISTER_RESOURCE_CORE_INPUT,
		MISTER_RESOURCE_SAVES, MISTER_RESOURCE_NATIVE_AUDIO,
		MISTER_RESOURCE_NATIVE_VIDEO, MISTER_RESOURCE_CONTENT};
	struct RecoveryRequirement {
		OperationKind kind;
		uint32_t required_mask;
	};
	const RecoveryRequirement requirements[] = {
		{OperationKind::input_descriptors, MISTER_RESOURCE_CORE_INPUT},
		{OperationKind::save, MISTER_RESOURCE_SAVES},
		{OperationKind::audio, MISTER_RESOURCE_NATIVE_AUDIO},
		{OperationKind::video, MISTER_RESOURCE_NATIVE_VIDEO},
		{OperationKind::content, MISTER_RESOURCE_CONTENT},
		{OperationKind::core_protocol, MISTER_RESOURCE_CORE_PROTOCOL},
		{OperationKind::terminal_fpga_cleanup, MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL}};
	for (size_t req = 0; req < sizeof(requirements) / sizeof(requirements[0]);
		++req) {
		for (size_t bit = 0; bit < sizeof(resource_bits) / sizeof(resource_bits[0]);
			++bit) {
			TestClock clock(10);
			HardwareBroker broker(clock);
			const uint32_t probe_resource =
				(requirements[req].required_mask & MISTER_RESOURCE_NATIVE_AUDIO) != 0
				? MISTER_RESOURCE_NATIVE_VIDEO : MISTER_RESOURCE_NATIVE_AUDIO;
			const OperationKind probe_kind = probe_resource ==
				MISTER_RESOURCE_NATIVE_AUDIO ? OperationKind::audio :
				OperationKind::video;
			const uint32_t requested_mask = resource_bits[bit] | probe_resource;
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(requested_mask, 200, 300, &epoch) ==
				MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> lease;
			const Result result = broker.BeginRecoveryOperation(*epoch,
				requirements[req].kind, &lease);
			const bool allowed = (requested_mask &
				requirements[req].required_mask) == requirements[req].required_mask;
		if (!allowed) {
				assert(result == MISTER_RESULT_INVALID_STATE);
				std::unique_ptr<OperationLease> probe;
				assert(broker.BeginRecoveryOperation(*epoch,
					probe_kind, &probe) == MISTER_RESULT_OK);
				ExecuteMatrixCell(*probe, probe_kind, clock, 3);
				continue;
			}
			// Only single-resource operations can be admitted by a single-bit
			// mask. The terminal operation must fail every single-bit cell.
			assert(requirements[req].kind !=
				OperationKind::terminal_fpga_cleanup);
			assert(result == MISTER_RESULT_OK);
			ExecuteMatrixCell(*lease, requirements[req].kind, clock,
			IsSpiHardwareKind(requirements[req].kind) ? 3 : 0);
		}
	}

	const uint32_t closure = MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL;
	for (size_t bit = 0; bit < 3; ++bit) {
		const uint32_t missing = 1u << bit;
		TestClock clock(10);
		HardwareBroker broker(clock);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery((closure & ~missing) |
			MISTER_RESOURCE_NATIVE_AUDIO, 200, 300, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) ==
			MISTER_RESULT_INVALID_STATE);
		std::unique_ptr<OperationLease> audio;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::audio, &audio) == MISTER_RESULT_OK);
		ExecuteMatrixCell(*audio, OperationKind::audio, clock, 3);
		audio.reset();
		std::unique_ptr<OperationLease> core;
		const bool core_present = (missing != MISTER_RESOURCE_CORE_PROTOCOL);
		const Result core_result = broker.BeginRecoveryOperation(*epoch,
			OperationKind::core_protocol, &core);
		assert(core_result == (core_present ? MISTER_RESULT_OK :
			MISTER_RESULT_INVALID_STATE));
		if (core_present) ExecuteMatrixCell(*core,
			OperationKind::core_protocol, clock, 6);
	}
}

void TestLifecycleTransitionsRejectLiveTransaction()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	io.block_deselect = true;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 7;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	Result execute_result = MISTER_RESULT_PLATFORM;
	std::thread execute_thread([&] {
		execute_result = bus.Execute(*lease, transaction, &receipt);
	});
	io.WaitForDeselect();
	Result quiesce_result = MISTER_RESULT_PLATFORM;
	std::thread quiesce_thread([&] {
		quiesce_result = broker.Quiesce(generation, 100);
	});
	std::unique_ptr<CleanupEpoch> premature_cleanup;
	assert(broker.BeginCleanup(generation, 200, 300, &premature_cleanup) ==
		MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<RecoveryEpoch> premature_recovery;
	assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 200, 300,
		&premature_recovery) == MISTER_RESULT_INVALID_STATE);
	io.AllowDeselect();
	execute_thread.join();
	assert(execute_result == MISTER_RESULT_OK);
	assert(receipt.mutation_sequence != 0);
	lease.reset();
	quiesce_thread.join();
	assert(quiesce_result == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 200, 300, &cleanup) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> stale;
	assert(broker.Begin(generation, OperationKind::input, 100, &stale) ==
		MISTER_RESULT_INVALID_STATE);
	assert(broker.Leave(generation, std::move(cleanup)) ==
		MISTER_RESULT_INVALID_STATE);
}

void TestFailureLatchCannotBisectTransaction()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	io.block_deselect = true;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 9;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	Result execute_result = MISTER_RESULT_PLATFORM;
	std::thread execute_thread([&] {
		execute_result = bus.Execute(*lease, transaction, &receipt);
	});
	io.WaitForDeselect();
	assert(broker.LatchFailure(generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> denied;
	assert(broker.Begin(generation, OperationKind::input, 100, &denied) ==
		MISTER_RESULT_INVALID_STATE);
	io.AllowDeselect();
	execute_thread.join();
	assert(execute_result == MISTER_RESULT_OK);
	assert(receipt.completed_words == 1);
	lease.reset();
	assert(broker.Quiesce(generation, 100) == MISTER_RESULT_OK);
}

void TestAuthorityMatrixAndRecoveryClosure()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	const OperationKind nonhardware[] = {
		OperationKind::scheduler, OperationKind::offload, OperationKind::save,
		OperationKind::content, OperationKind::input_descriptors};
	for (size_t i = 0; i < sizeof(nonhardware) / sizeof(nonhardware[0]); ++i) {
		std::unique_ptr<OperationLease> lease = Begin(broker, generation, nonhardware[i]);
		FakeIo io;
		NativeSpiBus bus(clock, io);
		const uint16_t word = 1;
		SpiReceipt receipt = {};
		SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
		assert(bus.Execute(*lease, transaction, &receipt) == MISTER_RESULT_INVALID_STATE);
		assert(io.events.empty());
	}
	std::unique_ptr<OperationLease> terminal;
	assert(broker.Begin(generation, OperationKind::terminal_fpga_cleanup, 100,
		&terminal) == MISTER_RESULT_INVALID_STATE);

	HardwareBroker recovery_broker(clock);
	std::unique_ptr<RecoveryEpoch> recovery;
	assert(recovery_broker.BeginRecovery(MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL,
		20, 30, &recovery) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> recovery_program;
	assert(recovery_broker.BeginRecoveryOperation(*recovery,
		OperationKind::program_fpga, &recovery_program) ==
		MISTER_RESULT_INVALID_STATE);
	std::unique_ptr<OperationLease> recovery_terminal;
	assert(recovery_broker.BeginRecoveryOperation(*recovery,
		OperationKind::terminal_fpga_cleanup, &recovery_terminal) ==
		MISTER_RESULT_OK);
	recovery_terminal.reset();
	MisterRecoveryObservationV2 observation = {};
	assert(recovery_broker.FinishRecovery(std::move(recovery), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == 0);

	HardwareBroker empty_recovery_broker(clock);
	std::unique_ptr<RecoveryEpoch> empty_recovery;
	assert(empty_recovery_broker.BeginRecovery(0, 20, 30, &empty_recovery) ==
		MISTER_RESULT_OK);
	assert(empty_recovery_broker.FinishRecovery(std::move(empty_recovery),
		&observation) == MISTER_RESULT_OK);

	std::unique_ptr<RecoveryEpoch> invalid_recovery;
	assert(empty_recovery_broker.BeginRecovery(MISTER_RESOURCE_V2_KNOWN + 1,
		20, 30, &invalid_recovery) == MISTER_RESULT_INVALID_ARGUMENT);

	assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA,
		20, 30, static_cast<std::unique_ptr<RecoveryEpoch> *>(nullptr)) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	std::unique_ptr<RecoveryEpoch> active_recovery;
	assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL, 20, 30, &active_recovery) ==
		MISTER_RESULT_INVALID_STATE);
}

void TestDeadlineAndStaleAuthorityProduceNoMmio()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	Enter(broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(broker, generation, OperationKind::input);
	FakeIo io;
	NativeSpiBus bus(clock, io);
	const uint16_t word = 1;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	clock.SetNow(100);
	assert(bus.Execute(*lease, transaction, &receipt) == MISTER_RESULT_DEADLINE);
	assert(io.events.empty());

	std::unique_ptr<OperationLease> stale;
	HardwareBroker *owned = new HardwareBroker(clock);
	PlatformGenerationId owned_generation = 0;
	Enter(*owned, clock, &owned_generation);
	assert(owned->Begin(owned_generation, OperationKind::input, 200, &stale) ==
		MISTER_RESULT_OK);
	delete owned;
	io.events.clear();
	assert(bus.Execute(*stale, transaction, &receipt) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.events.empty());
}

void TestBrokerDestructionDuringDeselectPreservesLastRecordedSequence()
{
	TestClock clock(10);
	HardwareBroker *broker = new HardwareBroker(clock);
	PlatformGenerationId generation = 0;
	Enter(*broker, clock, &generation);
	std::unique_ptr<OperationLease> lease =
		Begin(*broker, generation, OperationKind::input);
	FakeIo io;
	io.block_deselect = true;
	io.ack_reads = std::vector<bool>{true, false};
	NativeSpiBus bus(clock, io);
	const uint16_t word = 0x55aa;
	SpiTransaction transaction = {1, &word, 1, AckPolicy::required, 1};
	SpiReceipt receipt = {};
	Result execute_result = MISTER_RESULT_OK;
	std::thread execute_thread([&] {
		execute_result = bus.Execute(*lease, transaction, &receipt);
	});
	io.WaitForDeselect();
	delete broker;
	io.AllowDeselect();
	execute_thread.join();

	assert(execute_result == MISTER_RESULT_PLATFORM);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.selected);
	assert(receipt.completed_words == 1);
	assert(receipt.ack_low_observed);
	assert(receipt.deselected);
	assert(receipt.mutation_sequence == 2);
	lease.reset();
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	using namespace mister::native;
	TestBrokerDestructionDuringDeselectPreservesLastRecordedSequence();
	TestCompleteTransactionAndReceipt();
	TestFailureResidueAndNoOutputWithoutLease();
	TestEveryBoundaryReportsObservedResidue();
	TestDeselectFailureIsDistinctFromDeadline();
	TestZeroWordTransactionIsRejectedBeforeSelect();
	TestQuiesceCannotBisectDeselectOrReceipt();
	TestHardwareFenceSerializesCompetingLeases();
	TestRecoveryEpochDestructionDrainsConservatively();
	TestExhaustiveAuthorityKindMatrix();
	TestRecoveryMissingBitsAndTerminalClosure();
	TestLifecycleTransitionsRejectLiveTransaction();
	TestFailureLatchCannotBisectTransaction();
	TestAuthorityMatrixAndRecoveryClosure();
	TestDeadlineAndStaleAuthorityProduceNoMmio();
	return 0;
}
