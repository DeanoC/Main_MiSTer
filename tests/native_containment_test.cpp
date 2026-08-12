// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_core_profile.hpp"

#include <assert.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace mister {
namespace native {
namespace {

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

class FakeContainmentIo final : public NativeContainmentIo {
public:
	FakeContainmentIo()
		: fail_at(0), calls(0), core_mask(0), core_value(0), interface_value(99),
		  sdr_offset(0), sdr_value(99), bridge_value(99), remap_value(99),
		  core_gpo(0x40000000u), interface_readback(0), sdr_readback(0),
		  bridge_readback(7), remap_readback(1), release_called(false) {}

	Result Step()
	{
		++calls;
		return calls == fail_at ? MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}

	Result WriteCoreReset(const Access &, uint32_t mask,
		uint32_t value) override
	{
		core_mask = mask;
		core_value = value;
		return Step();
	}
	Result WriteInterfaceModule(const Access &,
		uint32_t value) override
	{
		interface_value = value;
		return Step();
	}
	Result WriteSdrPortControl(const Access &, uint32_t offset,
		uint32_t value) override
	{
		sdr_offset = offset;
		sdr_value = value;
		return Step();
	}
	Result WriteBridgeReset(const Access &, uint32_t value) override
	{
		bridge_value = value;
		return Step();
	}
	Result WriteRemap(const Access &, uint32_t value) override
	{
		remap_value = value;
		return Step();
	}
	Result ReadCoreGpo(const Access &, uint32_t *value) override
	{
		const Result result = Step();
		if (result == MISTER_RESULT_OK) *value = core_gpo;
		return result;
	}
	Result ReadInterfaceModule(const Access &, uint32_t *value) override
	{
		const Result result = Step();
		if (result == MISTER_RESULT_OK) *value = interface_readback;
		return result;
	}
	Result ReadSdrPortControl(const Access &, uint32_t offset,
		uint32_t *value) override
	{
		assert(offset == 0x5080u);
		const Result result = Step();
		if (result == MISTER_RESULT_OK) *value = sdr_readback;
		return result;
	}
	Result ReadBridgeReset(const Access &, uint32_t *value) override
	{
		const Result result = Step();
		if (result == MISTER_RESULT_OK) *value = bridge_readback;
		return result;
	}
	Result ReadRemap(const Access &, uint32_t *value) override
	{
		const Result result = Step();
		if (result == MISTER_RESULT_OK) *value = remap_readback;
		return result;
	}
	Result ReleaseMappings(const Access &) override
	{
		release_called = true;
		return Step();
	}

	int fail_at;
	int calls;
	uint32_t core_mask;
	uint32_t core_value;
	uint32_t interface_value;
	uint32_t sdr_offset;
	uint32_t sdr_value;
	uint32_t bridge_value;
	uint32_t remap_value;
	uint32_t core_gpo;
	uint32_t interface_readback;
	uint32_t sdr_readback;
	uint32_t bridge_readback;
	uint32_t remap_readback;
	bool release_called;
};

void PrepareCleanup(HardwareBroker &broker, FakeClock &clock,
	PlatformGenerationId *generation, std::unique_ptr<CleanupEpoch> *epoch,
	std::unique_ptr<OperationLease> *terminal)
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	assert(broker.EnterFixtureForTest(profile, generation) == MISTER_RESULT_OK);
	assert(broker.Quiesce(*generation, clock.NowMs() + 100) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(*generation, clock.NowMs() + 2000,
		clock.NowMs() + 5000, epoch) == MISTER_RESULT_OK);
	assert(broker.BeginCleanupOperation(**epoch,
		OperationKind::terminal_fpga_cleanup, terminal) == MISTER_RESULT_OK);
}

void TestExactTerminalContractAndOnlyLeaveAfterReceipt()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);

	assert(containment.ResetAndContain(*terminal) == MISTER_RESULT_OK);
	assert(io.calls == 11);
	assert(io.core_mask == 0xc0000000u);
	assert(io.core_value == 0x40000000u);
	assert(io.interface_value == 0);
	assert(io.sdr_offset == 0x5080u);
	assert(io.sdr_value == 0);
	assert(io.bridge_value == 7);
	assert(io.remap_value == 1);
	assert(io.release_called);
	assert(broker.containment_receipt_sequence_for_test() == 0);
	std::unique_ptr<HardwareLeaseView> staged_forbidden_view;
	assert(broker.AcquireHardwareLeaseView(*terminal, &staged_forbidden_view) ==
		MISTER_RESULT_INVALID_STATE);
	assert(staged_forbidden_view == nullptr);
	assert(broker.mutation_sequence_for_test() == 6);
	assert(broker.ObserveContainment(*epoch, *terminal) == MISTER_RESULT_OK);
	assert(broker.mutation_sequence_for_test() == 6);
	assert(broker.containment_receipt_sequence_for_test() == 6);
	std::unique_ptr<HardwareLeaseView> forbidden_view;
	assert(broker.AcquireHardwareLeaseView(*terminal, &forbidden_view) ==
		MISTER_RESULT_INVALID_STATE);
	assert(forbidden_view == nullptr);

	std::unique_ptr<OperationLease> rejected;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::save,
		&rejected) == MISTER_RESULT_INVALID_STATE);
	assert(broker.Begin(generation, OperationKind::input, 2000, &rejected) ==
		MISTER_RESULT_INVALID_STATE);
	assert(broker.mutation_sequence_for_test() == 6);
	assert(broker.containment_receipt_sequence_for_test() == 6);
	terminal.reset();
	assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
}

void TestEveryBoundaryMustSucceedBeforeReceipt()
{
	for (int failure = 1; failure <= 11; ++failure) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		io.fail_at = failure;
		NativeContainment containment(broker, io);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_PLATFORM);
		terminal.reset();
		assert(broker.Leave(generation, std::move(epoch)) ==
			MISTER_RESULT_INVALID_STATE);
		assert(epoch != nullptr);

		io.fail_at = 0;
		io.calls = 0;
		assert(broker.BeginCleanupOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		terminal.reset();
		assert(broker.Leave(generation, std::move(epoch)) == MISTER_RESULT_OK);
	}
}

void TestEveryExactReadbackIsRequired()
{
	for (int mismatch = 0; mismatch != 5; ++mismatch) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		if (mismatch == 0) io.core_gpo = 0x80000000u;
		if (mismatch == 1) io.interface_readback = 1;
		if (mismatch == 2) io.sdr_readback = 1;
		if (mismatch == 3) io.bridge_readback = 6;
		if (mismatch == 4) io.remap_readback = 0;
		NativeContainment containment(broker, io);
		PlatformGenerationId generation = 0;
		std::unique_ptr<CleanupEpoch> epoch;
		std::unique_ptr<OperationLease> terminal;
		PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
		assert(containment.ResetAndContain(*epoch, *terminal) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		terminal.reset();
		assert(broker.Leave(generation, std::move(epoch)) ==
			MISTER_RESULT_INVALID_STATE);
		assert(epoch != nullptr);
	}
}

void TestForeignAuthorityCannotMintReceipt()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeContainmentIo io;
	NativeContainment first_containment(first, io);
	PlatformGenerationId first_generation = 0;
	PlatformGenerationId second_generation = 0;
	std::unique_ptr<CleanupEpoch> first_epoch;
	std::unique_ptr<CleanupEpoch> second_epoch;
	std::unique_ptr<OperationLease> first_terminal;
	std::unique_ptr<OperationLease> second_terminal;
	PrepareCleanup(first, clock, &first_generation, &first_epoch, &first_terminal);
	PrepareCleanup(second, clock, &second_generation, &second_epoch,
		&second_terminal);
	assert(first_containment.ResetAndContain(*second_epoch, *second_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.calls == 0);
	second_terminal.reset();
	assert(second.Leave(second_generation, std::move(second_epoch)) ==
		MISTER_RESULT_INVALID_STATE);
}

void TestForeignEpochWithCurrentLeaseCannotTouchHardware()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeContainmentIo io;
	NativeContainment containment(first, io);
	PlatformGenerationId first_generation = 0;
	PlatformGenerationId second_generation = 0;
	std::unique_ptr<CleanupEpoch> first_epoch;
	std::unique_ptr<CleanupEpoch> second_epoch;
	std::unique_ptr<OperationLease> first_terminal;
	std::unique_ptr<OperationLease> second_terminal;
	PrepareCleanup(first, clock, &first_generation, &first_epoch, &first_terminal);
	PrepareCleanup(second, clock, &second_generation, &second_epoch,
		&second_terminal);
	assert(containment.ResetAndContain(*second_epoch, *first_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.calls == 0);
	assert(first.mutation_sequence_for_test() == 0);
	assert(first.containment_receipt_sequence_for_test() == 0);
	assert(containment.ResetAndContain(*first_epoch, *first_terminal) ==
		MISTER_RESULT_OK);
	first_terminal.reset();
	assert(first.Leave(first_generation, std::move(first_epoch)) ==
		MISTER_RESULT_OK);
}

void TestDeadlineBeforeTerminalMutationIsNonPoisoning()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &terminal);
	clock.SetNow(6000);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_DEADLINE);
	assert(io.calls == 0);
}

void TestStagedEvidenceCannotTransferToReplacementLease()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	std::unique_ptr<OperationLease> first_terminal;
	PrepareCleanup(broker, clock, &generation, &epoch, &first_terminal);
	assert(containment.ResetAndContain(*first_terminal) == MISTER_RESULT_OK);
	assert(broker.containment_receipt_sequence_for_test() == 0);
	first_terminal.reset();
	std::unique_ptr<OperationLease> replacement;
	assert(broker.BeginCleanupOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &replacement) == MISTER_RESULT_OK);
	assert(broker.ObserveContainment(*epoch, *replacement) ==
		MISTER_RESULT_UNSUPPORTED);
	replacement.reset();
	assert(broker.Leave(generation, std::move(epoch)) ==
		MISTER_RESULT_INVALID_STATE);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	using namespace mister::native;
	TestExactTerminalContractAndOnlyLeaveAfterReceipt();
	TestEveryBoundaryMustSucceedBeforeReceipt();
	TestEveryExactReadbackIsRequired();
	TestForeignAuthorityCannotMintReceipt();
	TestForeignEpochWithCurrentLeaseCannotTouchHardware();
	TestDeadlineBeforeTerminalMutationIsNonPoisoning();
	TestStagedEvidenceCannotTransferToReplacementLease();
	return 0;
}
