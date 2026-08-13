// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_input.hpp"

#include <assert.h>

#include <condition_variable>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace mister {
namespace native {
namespace {

class TestClock final : public NativeClock {
public:
	explicit TestClock(uint64_t now) : now_(now) {}
	uint64_t NowMs() const override { return now_; }
	void SetNow(uint64_t now) { now_ = now; }
	uint64_t &now_for_test() { return now_; }
	bool WaitUntil(std::condition_variable &condition,
		std::unique_lock<std::mutex> &lock, uint64_t deadline) override
	{
		condition.wait(lock);
		return now_ < deadline;
	}
private:
	uint64_t now_;
};

class FakeIo final : public NativeHardwareIo {
public:
	FakeIo() : ack_index(0), fail_word(false), fail_deselect(false),
		force_deselect_deadline(false), now(nullptr), words(), events() {}
	NativeSpiMutationResult Select(const HardwareLeaseView &,
		NativeSpiTarget target) override
	{
		events.push_back(target == NativeSpiTarget::user_io ?
			"select:user_io" : "select:file_io");
		return {MISTER_RESULT_OK, true, true, true};
	}
	NativeSpiMutationResult WriteWordWithStrobeLow(
		const HardwareLeaseView &, uint16_t word) override
	{
		words.push_back(word);
		events.push_back("word:" + std::to_string(word));
		return fail_word ?
			NativeSpiMutationResult{MISTER_RESULT_PLATFORM, true, true, false} :
			NativeSpiMutationResult{MISTER_RESULT_OK, true, true, true};
	}
	NativeSpiMutationResult SetStrobe(const HardwareLeaseView &, bool high) override
	{
		events.push_back(high ? "strobe:high" : "strobe:low");
		return {MISTER_RESULT_OK, true, true, true};
	}
	Result ReadAckSample(const HardwareLeaseView &,
		NativeSpiAckSample *sample) override
	{
		events.push_back("ack");
		if (ack_index >= ack_values.size()) return MISTER_RESULT_PLATFORM;
		sample->ack_high = ack_values[ack_index++];
		sample->fault = false;
		sample->response = 0;
		return MISTER_RESULT_OK;
	}
	NativeSpiMutationResult Deselect(const HardwareLeaseView &, NativeSpiTarget target,
		uint64_t) override
	{
		events.push_back(target == NativeSpiTarget::user_io ?
			"deselect:user_io" : "deselect:file_io");
		if (force_deselect_deadline && now != nullptr) *now = 100;
		return fail_deselect ?
			NativeSpiMutationResult{MISTER_RESULT_PLATFORM, true, false, false} :
			NativeSpiMutationResult{MISTER_RESULT_OK, true, true, true};
	}
	size_t ack_index;
	bool fail_word;
	bool fail_deselect;
	bool force_deselect_deadline;
	uint64_t *now;
	std::vector<bool> ack_values;
	std::vector<uint16_t> words;
	std::vector<std::string> events;
};

struct Fixture {
	Fixture()
		: clock(10), broker(clock), profile(FixtureNativeCoreProfile("snes")),
		  generation(0), lease(), io(), bus(clock, io), input(bus.input_port())
	{
		assert(profile != nullptr);
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		assert(broker.Begin(generation, OperationKind::input, 100, &lease) ==
			MISTER_RESULT_OK);
		io.now = &clock.now_for_test();
		io.ack_values = {true, false, true, false};
	}
	TestClock clock;
	HardwareBroker broker;
	const NativeCoreProfile *profile;
	PlatformGenerationId generation;
	std::unique_ptr<OperationLease> lease;
	FakeIo io;
	NativeSpiBus bus;
	NativeInput input;
};

NativeInputEvent EventWithIdentity(NativeInputKind kind, uint8_t player,
	uint32_t map, bool pressed, uint64_t sequence)
{
	NativeInputEvent event = {kind, player, map, pressed, false, 0};
	event.identity = {player, sequence};
	return event;
}

void TestPressReleaseGoldenAndExactInverse()
{
	Fixture fixture;
	NativeInputEvent press = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0031, true, 1);
	SpiReceipt receipt = {};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, press,
		&receipt) == MISTER_RESULT_OK);
	assert((fixture.io.words == std::vector<uint16_t>{0x02, 0x0031}));
	assert(fixture.input.ledger_size() == 1);
	DeliveredInput delivered = {};
	assert(fixture.input.GetDeliveredInput(0, &delivered));
	assert(delivered.player_command == 0x02);
	assert(delivered.map == 0x0031);
	assert(delivered.inverse_words[0] == 0x02);
	assert(delivered.inverse_words[1] == 0);

	NativeInputEvent release = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 2);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, release,
		&receipt) == MISTER_RESULT_OK);
	assert((fixture.io.words == std::vector<uint16_t>{0x02, 0x0031,
		0x02, 0x0000}));
	assert(fixture.input.ledger_size() == 0);
}

void TestBothPlayersAndDuplicateOrReorderedEvents()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent p1 = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0080, true, 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, p1,
		&receipt) == MISTER_RESULT_OK);
	assert((fixture.io.words == std::vector<uint16_t>{0x03, 0x0080}));
	const size_t duplicate_word_count = fixture.io.words.size();
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, p1,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() == duplicate_word_count);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent duplicate_release = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0000, false, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		duplicate_release, &receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 0);
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent reordered_release = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0000, false, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		reordered_release, &receipt) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() == 4);
}

void TestUnsupportedInputDoesZeroMmioAndNoLedger()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	receipt.response_words_observed = 9;
	const NativeInputKind unsupported[] = {
		NativeInputKind::keyboard, NativeInputKind::mouse,
		NativeInputKind::analog, NativeInputKind::rumble,
		NativeInputKind::ui, NativeInputKind::admin,
		NativeInputKind::reconfiguration, NativeInputKind::unknown};
	for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
		NativeInputEvent event = {unsupported[i], 0, 0x0001, true, false, 0};
		assert(fixture.input.Deliver(fixture.profile, *fixture.lease, event,
			&receipt) == MISTER_RESULT_UNSUPPORTED);
	}
	NativeInputEvent pause = {NativeInputKind::keyboard, 0, 0, true, false,
		0x0077};
	NativeInputEvent print_screen = {
		NativeInputKind::keyboard, 0, 0, true, false, 0x0063};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, pause,
		&receipt) == MISTER_RESULT_UNSUPPORTED);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, print_screen,
		&receipt) == MISTER_RESULT_UNSUPPORTED);
	NativeInputEvent upper_bits = {
		NativeInputKind::digital, 0, 0x10001, true, false, 0};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, upper_bits,
		&receipt) == MISTER_RESULT_UNSUPPORTED);
	NativeInputEvent swapped = {
		NativeInputKind::digital, 0, 0x0001, true, true, 0};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, swapped,
		&receipt) == MISTER_RESULT_UNSUPPORTED);
	assert(fixture.io.events.empty());
	assert(fixture.input.ledger_size() == 0);
	assert(receipt.response_words_observed == 0);
}

void TestPartialFailureRetainsConservativeLedgerAndRetry()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent press = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0010, true, 1);
	fixture.io.fail_word = true;
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, press,
		&receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.input.ledger_size() == 0);
	fixture.io.fail_word = false;
	fixture.io.words.clear();
	fixture.io.events.clear();
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, press,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true};
	NativeInputEvent release = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, release,
		&receipt) != MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, release,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 0);
}

void TestUncertainNonzeroBlocksNeutralFastPath()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent pressed = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0002, true, 1);
	fixture.io.ack_values = {true};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, pressed,
		&receipt) != MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 0);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent neutral = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 2);
	const size_t words_before = fixture.io.words.size();
	const Result result = fixture.input.Deliver(fixture.profile, *fixture.lease,
		neutral, &receipt);
	assert(result == MISTER_RESULT_OK);
	assert(fixture.io.words.size() > words_before);
	assert(fixture.input.ledger_size() == 0);
}

void TestUncertainReplacementIsNotSuppressedAsDuplicate()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent a = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, a,
		&receipt) == MISTER_RESULT_OK);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true};
	NativeInputEvent b = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0002, true, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, b,
		&receipt) != MISTER_RESULT_OK);
	const size_t words_before = fixture.io.words.size();
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent retry_a = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 3);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, retry_a,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() > words_before);
	DeliveredInput current = {};
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.map == 0x0001);
}

void TestDeselectFailureLeavesUncertainStateForRetry()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent pressed = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0004, true, 1);
	fixture.io.fail_deselect = true;
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, pressed,
		&receipt) != MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 0);
	fixture.io.fail_deselect = false;
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, pressed,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
}

void TestDeadlineAfterDeselectDoesNotCommitInput()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent pressed = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0008, true, 1);
	fixture.io.force_deselect_deadline = true;
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, pressed,
		&receipt) == MISTER_RESULT_DEADLINE);
	assert(fixture.input.ledger_size() == 0);
	fixture.io.force_deselect_deadline = false;
	fixture.clock.SetNow(10);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, pressed,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
}

void TestStaleInputIdentityCannotOverwriteNewerFullMap()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent initial = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, initial,
		&receipt) == MISTER_RESULT_OK);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent newer = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0003, true, 3);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, newer,
		&receipt) == MISTER_RESULT_OK);
	const size_t words_before = fixture.io.words.size();
	NativeInputEvent stale_map = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, stale_map,
		&receipt) == MISTER_RESULT_PLATFORM);
	NativeInputEvent stale_zero = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, stale_zero,
		&receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words_before);
	DeliveredInput current = {};
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.map == 0x0003);
	assert(current.identity.sequence == 3);
}

void TestZeroIdentityIsRejectedForBothPlayersWithoutMmio()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent player_zero = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 0);
	NativeInputEvent player_one = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0000, false, 0);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, player_zero,
		&receipt) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(receipt.result == MISTER_RESULT_INVALID_ARGUMENT);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, player_one,
		&receipt) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(receipt.result == MISTER_RESULT_INVALID_ARGUMENT);
	assert(fixture.io.events.empty());
	assert(fixture.input.ledger_size() == 0);
}

void TestSequenceBindsExactlyOneCommittedFullMap()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent a = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 7);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, a,
		&receipt) == MISTER_RESULT_OK);
	const size_t words_after_a = fixture.io.words.size();

	NativeInputEvent conflicting_zero = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 7);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		conflicting_zero, &receipt) == MISTER_RESULT_PLATFORM);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words_after_a);
	NativeInputEvent conflicting_b = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0002, true, 7);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, conflicting_b,
		&receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words_after_a);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, a,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() == words_after_a);
	DeliveredInput current = {};
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.map == 0x0001);
	assert(current.identity.sequence == 7);

	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent neutral = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 8);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, neutral,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 0);
	const size_t words_after_neutral = fixture.io.words.size();
	NativeInputEvent conflicting_repress = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0004, true, 8);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		conflicting_repress, &receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words_after_neutral);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, neutral,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() == words_after_neutral);
	assert(fixture.input.ledger_size() == 0);
}

void TestUncertainSequenceRejectsConflictingMapAndRetriesExactMap()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent uncertain = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0020, true, 11);
	fixture.io.fail_word = true;
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, uncertain,
		&receipt) == MISTER_RESULT_PLATFORM);
	const size_t words_after_failure = fixture.io.words.size();
	NativeInputEvent conflict = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0040, true, 11);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, conflict,
		&receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words_after_failure);
	assert(fixture.input.ledger_size() == 0);

	fixture.io.fail_word = false;
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, uncertain,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.io.words.size() > words_after_failure);
	DeliveredInput current = {};
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.player == 1);
	assert(current.map == 0x0020);
	assert(current.identity.sequence == 11);
}

void TestPerPlayerStaleSequencesAreRejectedIndependently()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent player_zero_new = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0003, true, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		player_zero_new, &receipt) == MISTER_RESULT_OK);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent player_one_new = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0030, true, 4);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		player_one_new, &receipt) == MISTER_RESULT_OK);
	const size_t words_before_stale = fixture.io.words.size();
	NativeInputEvent player_zero_stale = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	NativeInputEvent player_one_stale = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0010, true, 3);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		player_zero_stale, &receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		player_one_stale, &receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.words.size() == words_before_stale);
	assert(fixture.input.ledger_size() == 2);
	DeliveredInput first = {};
	DeliveredInput second = {};
	assert(fixture.input.GetDeliveredInput(0, &first));
	assert(fixture.input.GetDeliveredInput(1, &second));
	assert(first.player == 0 && first.map == 0x0003 &&
		first.identity.sequence == 2);
	assert(second.player == 1 && second.map == 0x0030 &&
		second.identity.sequence == 4);
}

void TestActiveNonInputLeasesCannotDeliverOrAdvanceInput()
{
	Fixture fixture;
	const OperationKind wrong_kinds[] = {
		OperationKind::program_fpga, OperationKind::core_protocol,
		OperationKind::audio, OperationKind::video};
	NativeInputEvent rejected = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0040, true, 99);
	for (size_t index = 0;
		index < sizeof(wrong_kinds) / sizeof(wrong_kinds[0]); ++index) {
		std::unique_ptr<OperationLease> wrong;
		assert(fixture.broker.Begin(fixture.generation, wrong_kinds[index], 100,
			&wrong) == MISTER_RESULT_OK);
		SpiReceipt receipt = {};
		assert(fixture.input.Deliver(fixture.profile, *wrong, rejected, &receipt) ==
			MISTER_RESULT_INVALID_STATE);
		assert(receipt.result == MISTER_RESULT_INVALID_STATE);
		assert(receipt.mutation_sequence == 0);
		assert(fixture.io.events.empty());
		assert(fixture.input.ledger_size() == 0);
	}

	NativeInputEvent accepted = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	SpiReceipt receipt = {};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, accepted,
		&receipt) == MISTER_RESULT_OK);
	assert(receipt.mutation_sequence == 8);
	assert(fixture.input.ledger_size() == 1);
}

void TestCleanupAcceptsOnlyExactInputOperation()
{
	Fixture fixture;
	fixture.lease.reset();
	assert(fixture.broker.Quiesce(fixture.generation, 100) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(fixture.broker.BeginCleanup(fixture.generation, 200, 300, &epoch) ==
		MISTER_RESULT_OK);
	const OperationKind wrong_kinds[] = {
		OperationKind::core_protocol, OperationKind::audio,
		OperationKind::video, OperationKind::terminal_fpga_cleanup};
	NativeInputEvent rejected = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0040, true, 99);
	for (size_t index = 0;
		index < sizeof(wrong_kinds) / sizeof(wrong_kinds[0]); ++index) {
		std::unique_ptr<OperationLease> wrong;
		assert(fixture.broker.BeginCleanupOperation(*epoch, wrong_kinds[index],
			&wrong) == MISTER_RESULT_OK);
		SpiReceipt receipt = {};
		assert(fixture.input.Deliver(fixture.profile, *wrong, rejected, &receipt) ==
			MISTER_RESULT_INVALID_STATE);
		assert(receipt.result == MISTER_RESULT_INVALID_STATE);
		assert(receipt.mutation_sequence == 0);
		assert(fixture.io.events.empty());
		assert(fixture.input.ledger_size() == 0);
		wrong.reset();
	}

	std::unique_ptr<OperationLease> input_lease;
	assert(fixture.broker.BeginCleanupOperation(*epoch, OperationKind::input,
		&input_lease) == MISTER_RESULT_OK);
	NativeInputEvent accepted = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	SpiReceipt receipt = {};
	assert(fixture.input.Deliver(fixture.profile, *input_lease, accepted,
		&receipt) == MISTER_RESULT_OK);
	assert(receipt.mutation_sequence == 8);
	assert(fixture.input.ledger_size() == 1);
}

void TestRecoveryHardwareLeasesCannotDeliverOrAdvanceInput()
{
	TestClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	FakeIo io;
	NativeSpiBus bus(clock, io);
	NativeInput input(bus.input_port());
	const uint32_t mask = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL | MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(mask, 200, 300, &epoch) == MISTER_RESULT_OK);
	const OperationKind wrong_kinds[] = {
		OperationKind::core_protocol, OperationKind::audio,
		OperationKind::video, OperationKind::terminal_fpga_cleanup};
	NativeInputEvent rejected = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0040, true, 99);
	for (size_t index = 0;
		index < sizeof(wrong_kinds) / sizeof(wrong_kinds[0]); ++index) {
		std::unique_ptr<OperationLease> wrong;
		assert(broker.BeginRecoveryOperation(*epoch, wrong_kinds[index], &wrong) ==
			MISTER_RESULT_OK);
		SpiReceipt receipt = {};
		assert(input.Deliver(profile, *wrong, rejected, &receipt) ==
			MISTER_RESULT_INVALID_STATE);
		assert(receipt.result == MISTER_RESULT_INVALID_STATE);
		assert(receipt.mutation_sequence == 0);
		assert(io.events.empty());
		assert(input.ledger_size() == 0);
		wrong.reset();
	}
	epoch.reset();

	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> input_lease;
	assert(broker.Begin(generation, OperationKind::input, 100, &input_lease) ==
		MISTER_RESULT_OK);
	io.ack_values = {true, false, true, false};
	NativeInputEvent accepted = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	SpiReceipt receipt = {};
	assert(input.Deliver(profile, *input_lease, accepted, &receipt) ==
		MISTER_RESULT_OK);
	assert(receipt.mutation_sequence == 8);
	assert(input.ledger_size() == 1);
}

void TestNoopPathsValidateDeadlineBeforeChangingSequence()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent initial = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0020, true, 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, initial,
		&receipt) == MISTER_RESULT_OK);
	const size_t events_before = fixture.io.events.size();
	DeliveredInput delivered = {};
	assert(fixture.input.GetDeliveredInput(0, &delivered));
	assert(delivered.identity.sequence == 1);

	fixture.clock.SetNow(100);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, initial,
		&receipt) == MISTER_RESULT_DEADLINE);
	assert(receipt.result == MISTER_RESULT_DEADLINE);
	assert(fixture.io.events.size() == events_before);
	NativeInputEvent newer_same_map = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0020, true, 2);
	fixture.clock.SetNow(101);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		newer_same_map, &receipt) == MISTER_RESULT_DEADLINE);
	assert(receipt.result == MISTER_RESULT_DEADLINE);
	assert(fixture.io.events.size() == events_before);
	assert(fixture.input.GetDeliveredInput(0, &delivered));
	assert(delivered.identity.sequence == 1);

	fixture.clock.SetNow(10);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease,
		newer_same_map, &receipt) == MISTER_RESULT_OK);
	assert(receipt.result == MISTER_RESULT_OK);
	assert(!receipt.selected);
	assert(receipt.completed_words == 0);
	assert(!receipt.ack_low_observed);
	assert(!receipt.deselected);
	assert(receipt.mutation_sequence == 0);
	assert(fixture.io.events.size() == events_before);
	assert(fixture.input.GetDeliveredInput(0, &delivered));
	assert(delivered.identity.sequence == 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, initial,
		&receipt) == MISTER_RESULT_PLATFORM);
	assert(fixture.io.events.size() == events_before);

	NativeInputEvent replacement = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0040, true, 3);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, replacement,
		&receipt) == MISTER_RESULT_OK);
	assert(receipt.mutation_sequence == 16);
	assert(fixture.input.GetDeliveredInput(0, &delivered));
	assert(delivered.identity.sequence == 3);
}

void TestNoopPathsRejectLeaseAfterBrokerDestruction()
{
	TestClock clock(10);
	std::unique_ptr<HardwareBroker> broker(new HardwareBroker(clock));
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker->EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker->Begin(generation, OperationKind::input, 100, &lease) ==
		MISTER_RESULT_OK);
	FakeIo io;
	io.ack_values = {true, false, true, false};
	NativeSpiBus bus(clock, io);
	NativeInput input(bus.input_port());
	SpiReceipt receipt = {};
	NativeInputEvent initial = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(input.Deliver(profile, *lease, initial, &receipt) == MISTER_RESULT_OK);
	const size_t events_before = io.events.size();
	broker.reset();

	NativeInputEvent newer_same_map = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 2);
	assert(input.Deliver(profile, *lease, newer_same_map, &receipt) ==
		MISTER_RESULT_INVALID_STATE);
	assert(receipt.result == MISTER_RESULT_INVALID_STATE);
	assert(io.events.size() == events_before);
	assert(input.Deliver(profile, *lease, initial, &receipt) ==
		MISTER_RESULT_INVALID_STATE);
	assert(receipt.result == MISTER_RESULT_INVALID_STATE);
	assert(io.events.size() == events_before);
	DeliveredInput delivered = {};
	assert(input.GetDeliveredInput(0, &delivered));
	assert(delivered.identity.sequence == 1);
}

void TestNoopPathsRejectAbaStaleLease()
{
	TestClock clock(10);
	alignas(HardwareBroker) unsigned char storage[sizeof(HardwareBroker)];
	HardwareBroker *first = new (storage) HardwareBroker(clock);
	const NativeCoreProfile *profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId first_generation = 0;
	assert(first->EnterFixtureForTest(*profile, &first_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> stale;
	assert(first->Begin(first_generation, OperationKind::input, 100, &stale) ==
		MISTER_RESULT_OK);
	FakeIo io;
	io.ack_values = {true, false, true, false};
	NativeSpiBus bus(clock, io);
	NativeInput input(bus.input_port());
	SpiReceipt receipt = {};
	NativeInputEvent initial = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(input.Deliver(profile, *stale, initial, &receipt) == MISTER_RESULT_OK);
	const size_t events_before = io.events.size();
	first->~HardwareBroker();

	HardwareBroker *second = new (storage) HardwareBroker(clock);
	PlatformGenerationId second_generation = 0;
	assert(second->EnterFixtureForTest(*profile, &second_generation) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> current;
	assert(second->Begin(second_generation, OperationKind::input, 100, &current) ==
		MISTER_RESULT_OK);
	NativeInputEvent newer_same_map = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 2);
	assert(input.Deliver(profile, *stale, newer_same_map, &receipt) ==
		MISTER_RESULT_INVALID_STATE);
	assert(receipt.result == MISTER_RESULT_INVALID_STATE);
	assert(io.events.size() == events_before);
	assert(input.Deliver(profile, *current, newer_same_map, &receipt) ==
		MISTER_RESULT_OK);
	assert(receipt.result == MISTER_RESULT_OK);
	assert(!receipt.selected);
	assert(receipt.mutation_sequence == 0);
	assert(io.events.size() == events_before);
	DeliveredInput delivered = {};
	assert(input.GetDeliveredInput(0, &delivered));
	assert(delivered.identity.sequence == 1);
	current.reset();
	second->~HardwareBroker();
	stale.reset();
}

void TestDefensiveCommitFailureKeepsReceiptTruthful()
{
	Fixture fixture;
	fixture.input.FailNextCommitForTest();
	NativeInputEvent press = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0008, true, 1);
	SpiReceipt receipt = {};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, press,
		&receipt) == MISTER_RESULT_PLATFORM);
	assert(receipt.result == MISTER_RESULT_PLATFORM);
	assert(receipt.selected);
	assert(receipt.completed_words == 2);
	assert(receipt.ack_low_observed);
	assert(receipt.deselected);
	assert(receipt.mutation_sequence == 8);
	assert(fixture.input.ledger_size() == 0);

	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, press,
		&receipt) == MISTER_RESULT_OK);
	assert(receipt.result == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
}

void TestProfileDriftAndMissingAuthorityAreRejectedBeforeMmio()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent press = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(fixture.input.Deliver(nullptr, *fixture.lease, press, &receipt) ==
		MISTER_RESULT_UNSUPPORTED);
	fixture.io.ack_values = {true, false, true, false};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, press,
		&receipt) == MISTER_RESULT_OK);
	fixture.io.events.clear();
	const NativeCoreProfile *other = FixtureNativeCoreProfile("megadrive");
	assert(fixture.input.Deliver(other, *fixture.lease, press, &receipt) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(fixture.io.events.empty());
	NativeCoreProfile copy = *fixture.profile;
	copy.authority = NativeProfileAuthority::untrusted;
	NativeInputEvent copied_profile = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0002, true, 2);
	assert(fixture.input.Deliver(&copy, *fixture.lease, copied_profile,
		&receipt) == MISTER_RESULT_UNSUPPORTED);
	NativeInputEvent player_two = {
		NativeInputKind::digital, 2, 0x0002, true, false, 0};
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, player_two,
		&receipt) == MISTER_RESULT_UNSUPPORTED);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, copied_profile,
		nullptr) == MISTER_RESULT_INVALID_ARGUMENT);
}

void TestOneFullMapPerPlayerAndNeutralRelease()
{
	Fixture fixture;
	SpiReceipt receipt = {};
	NativeInputEvent a = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, a,
		&receipt) == MISTER_RESULT_OK);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent ab = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0003, true, 2);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, ab,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
	DeliveredInput current = {};
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.map == 0x0003);
	assert(current.inverse_words[1] == 0);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent b = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0002, false, 3);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, b,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.map == 0x0002);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false};
	NativeInputEvent release = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 4);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, release,
		&receipt) == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 0);
	assert(fixture.io.words[fixture.io.words.size() - 1] == 0);
	}

void TestConcurrentDuplicatePressHasOneHardwareCommit()
{
	Fixture fixture;
	NativeInputEvent press = EventWithIdentity(
		NativeInputKind::digital, 1, 0x0080, true, 1);
	SpiReceipt first_receipt = {};
	SpiReceipt second_receipt = {};
	Result first_result = MISTER_RESULT_PLATFORM;
	Result second_result = MISTER_RESULT_PLATFORM;
	std::thread first([&] {
		first_result = fixture.input.Deliver(fixture.profile, *fixture.lease,
			press, &first_receipt);
	});
	std::thread second([&] {
		second_result = fixture.input.Deliver(fixture.profile, *fixture.lease,
			press, &second_receipt);
	});
	first.join();
	second.join();
	assert(first_result == MISTER_RESULT_OK);
	assert(second_result == MISTER_RESULT_OK);
	assert(fixture.io.words.size() == 2);
	assert(fixture.input.ledger_size() == 1);
}

void TestConcurrentPressAndReleaseRemainOneCoherentPlayerState()
{
	Fixture fixture;
	SpiReceipt initial_receipt = {};
	NativeInputEvent initial = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0001, true, 1);
	assert(fixture.input.Deliver(fixture.profile, *fixture.lease, initial,
		&initial_receipt) == MISTER_RESULT_OK);
	fixture.io.ack_index = 0;
	fixture.io.ack_values = {true, false, true, false, true, false, true, false};
	NativeInputEvent release = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0000, false, 2);
	NativeInputEvent replacement = EventWithIdentity(
		NativeInputKind::digital, 0, 0x0002, true, 3);
	SpiReceipt release_receipt = {};
	SpiReceipt replacement_receipt = {};
	Result release_result = MISTER_RESULT_PLATFORM;
	Result replacement_result = MISTER_RESULT_PLATFORM;
	std::thread release_thread([&] {
		release_result = fixture.input.Deliver(fixture.profile, *fixture.lease,
			release, &release_receipt);
	});
	std::thread replacement_thread([&] {
		replacement_result = fixture.input.Deliver(fixture.profile,
			*fixture.lease, replacement, &replacement_receipt);
	});
	release_thread.join();
	replacement_thread.join();
	assert(release_result == MISTER_RESULT_OK ||
		release_result == MISTER_RESULT_PLATFORM);
	assert(replacement_result == MISTER_RESULT_OK);
	assert(fixture.input.ledger_size() == 1);
	DeliveredInput current = {};
	assert(fixture.input.GetDeliveredInput(0, &current));
	assert(current.player == 0);
	assert(current.map == 0x0002);
	assert(current.identity.sequence == 3);
	assert(current.inverse_words[1] == 0);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestPressReleaseGoldenAndExactInverse();
	mister::native::TestBothPlayersAndDuplicateOrReorderedEvents();
	mister::native::TestUnsupportedInputDoesZeroMmioAndNoLedger();
	mister::native::TestPartialFailureRetainsConservativeLedgerAndRetry();
	mister::native::TestUncertainNonzeroBlocksNeutralFastPath();
	mister::native::TestUncertainReplacementIsNotSuppressedAsDuplicate();
	mister::native::TestDeselectFailureLeavesUncertainStateForRetry();
	mister::native::TestDeadlineAfterDeselectDoesNotCommitInput();
	mister::native::TestStaleInputIdentityCannotOverwriteNewerFullMap();
	mister::native::TestZeroIdentityIsRejectedForBothPlayersWithoutMmio();
	mister::native::TestSequenceBindsExactlyOneCommittedFullMap();
	mister::native::TestUncertainSequenceRejectsConflictingMapAndRetriesExactMap();
	mister::native::TestPerPlayerStaleSequencesAreRejectedIndependently();
	mister::native::TestActiveNonInputLeasesCannotDeliverOrAdvanceInput();
	mister::native::TestCleanupAcceptsOnlyExactInputOperation();
	mister::native::TestRecoveryHardwareLeasesCannotDeliverOrAdvanceInput();
	mister::native::TestNoopPathsValidateDeadlineBeforeChangingSequence();
	mister::native::TestNoopPathsRejectLeaseAfterBrokerDestruction();
	mister::native::TestNoopPathsRejectAbaStaleLease();
	mister::native::TestDefensiveCommitFailureKeepsReceiptTruthful();
	mister::native::TestProfileDriftAndMissingAuthorityAreRejectedBeforeMmio();
	mister::native::TestOneFullMapPerPlayerAndNeutralRelease();
	mister::native::TestConcurrentDuplicatePressHasOneHardwareCommit();
	mister::native::TestConcurrentPressAndReleaseRemainOneCoherentPlayerState();
	return 0;
}
