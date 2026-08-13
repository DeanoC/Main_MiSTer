// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_recovery.hpp"
#include "tests/native_core_protocol_authority_test_peer.hpp"

#include <assert.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>

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

size_t KindIndex(OperationKind kind)
{
	return static_cast<size_t>(kind);
}

class FakeRecoveryIo final : public NativeRecoveryIo {
public:
	explicit FakeRecoveryIo(HardwareBroker &broker)
		: broker_(broker), calls(0), core_protocol_session_calls(0),
		  last_deadline(0)
	{
		for (size_t index = 0; index != 11; ++index) {
			states[index] = RecoveryResourceState::unknown;
			results[index] = MISTER_RESULT_OK;
		}
	}

	Result Apply(const OperationLease &lease, OperationKind kind,
		RecoveryResourceState *state)
	{
		return ApplyDeadline(lease.absolute_deadline_ms(), kind, state);
	}
	Result ApplyDeadline(uint64_t absolute_deadline_ms, OperationKind kind,
		RecoveryResourceState *state)
	{
		++calls;
		last_deadline = absolute_deadline_ms;
		const size_t index = KindIndex(kind);
		*state = states[index];
		return results[index];
	}
	Result CloseInputDescriptors(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::input_descriptors, state);
	}
	Result FlushAndCloseSave(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::save, state);
	}
	Result MuteAudio(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::audio, state);
	}
	Result PowerDownVideo(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::video, state);
	}
	Result CloseContent(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return Apply(lease, OperationKind::content, state);
	}
	Result DisableCoreProtocol(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		std::unique_ptr<RecoveryCoreProtocolSession> session;
		const Result acquire = CoreProtocolAuthorityTestPeer::AcquireRecovery(
			lease, broker_, &session);
		if (acquire != MISTER_RESULT_OK) return acquire;
		++core_protocol_session_calls;
		const Result primary = ApplyDeadline(lease.absolute_deadline_ms(),
			OperationKind::core_protocol, state);
		const ProtocolMappingReleaseReceipt release = {
			MISTER_RESULT_OK, true, true, true, true, true,
			broker_.mutation_sequence_for_test()};
		const Result completed = CoreProtocolAuthorityTestPeer::CompleteRecovery(
			lease, broker_, std::move(session), release);
		return completed == MISTER_RESULT_OK ? primary : completed;
	}

	HardwareBroker &broker_;
	RecoveryResourceState states[11];
	Result results[11];
	int calls;
	int core_protocol_session_calls;
	uint64_t last_deadline;
};

class FakeContainmentIo final : public NativeContainmentIo {
public:
	FakeContainmentIo()
		: fail_at(0), advance_at(0), advance_clock(nullptr), calls(0), writes(0), reads(0), releases(0), core(0x40000000u), interface(0),
		  sdr(0), bridge(7), remap(1), release_result(MISTER_RESULT_OK) {}
	Result Step()
	{
		++calls;
		if (calls == advance_at && advance_clock != nullptr)
			advance_clock->SetNow(6000);
		return calls == fail_at ? MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}
	Result WriteCoreReset(const Access &, uint32_t mask,
		uint32_t value) override
	{
		assert(mask == 0xc0000000u && value == 0x40000000u);
		++writes;
		return Step();
	}
	Result WriteInterfaceModule(const Access &, uint32_t value) override
	{
		assert(value == 0); ++writes; return Step();
	}
	Result WriteSdrPortControl(const Access &, uint32_t offset,
		uint32_t value) override
	{
		assert(offset == 0x5080u && value == 0); ++writes; return Step();
	}
	Result WriteBridgeReset(const Access &, uint32_t value) override
	{
		assert(value == 7); ++writes; return Step();
	}
	Result WriteRemap(const Access &, uint32_t value) override
	{
		assert(value == 1); ++writes; return Step();
	}
	Result ReadCoreGpo(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = core; return result;
	}
	Result ReadInterfaceModule(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = interface; return result;
	}
	Result ReadSdrPortControl(const Access &, uint32_t offset,
		uint32_t *value) override
	{
		assert(offset == 0x5080u); ++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = sdr; return result;
	}
	Result ReadBridgeReset(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = bridge; return result;
	}
	Result ReadRemap(const Access &, uint32_t *value) override
	{
		++reads; const Result result = Step(); if (result == MISTER_RESULT_OK) *value = remap; return result;
	}
	Result ReleaseMappings(const Access &) override
	{
		++releases; const Result result = Step(); return result == MISTER_RESULT_OK ? release_result : result;
	}

	int fail_at;
	int advance_at;
	FakeClock *advance_clock;
	int calls;
	int writes;
	int reads;
	int releases;
	uint32_t core;
	uint32_t interface;
	uint32_t sdr;
	uint32_t bridge;
	uint32_t remap;
	Result release_result;
};

MisterRecoveryObservationV2 Observation()
{
	MisterRecoveryObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

void TestRecoveryEpochAuthorityAndFreshness()
{
	static_assert(!std::is_default_constructible<RecoveryEpoch>::value,
		"recovery epochs are broker minted");
	static_assert(!std::is_copy_constructible<RecoveryEpoch>::value,
		"recovery epochs are not caller-copyable");
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(1u << 8, 3000, 6000, &epoch) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	assert(!broker.has_live_generation_for_test());
	const uint64_t first_identity = epoch->identity_for_test();
	assert(first_identity != 0);
	std::unique_ptr<RecoveryEpoch> duplicate;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 4000, 7000,
		&duplicate) == MISTER_RESULT_INVALID_STATE);
	epoch.reset();
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 4000, 7000,
		&epoch) == MISTER_RESULT_OK);
	assert(epoch->identity_for_test() != first_identity);
}

void TestRecoveryRejectedWithLiveGenerationOrCleanup()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*FixtureNativeCoreProfile("snes"),
		&generation) == MISTER_RESULT_OK);
	std::unique_ptr<RecoveryEpoch> recovery;
	assert(broker.BeginRecovery(0, 3000, 6000, &recovery) ==
		MISTER_RESULT_INVALID_STATE);
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> cleanup;
	assert(broker.BeginCleanup(generation, 3000, 6000, &cleanup) ==
		MISTER_RESULT_OK);
	assert(broker.BeginRecovery(0, 3000, 6000, &recovery) ==
		MISTER_RESULT_INVALID_STATE);
}

void TestNormativeDependenciesAndExactDeadlines()
{
	struct Case {
		OperationKind kind;
		uint32_t bit;
		uint64_t deadline;
	};
	const Case cases[] = {
		{OperationKind::input_descriptors, MISTER_RESOURCE_CORE_INPUT, 3000},
		{OperationKind::save, MISTER_RESOURCE_SAVES, 3000},
		{OperationKind::audio, MISTER_RESOURCE_NATIVE_AUDIO, 3000},
		{OperationKind::video, MISTER_RESOURCE_NATIVE_VIDEO, 3000},
		{OperationKind::content, MISTER_RESOURCE_CONTENT, 3000},
		{OperationKind::core_protocol, MISTER_RESOURCE_CORE_PROTOCOL, 6000}
	};
	for (const Case &test : cases) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		NativeRecovery recovery(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(test.bit, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(test.kind)] = RecoveryResourceState::neutral;
		assert(recovery.Perform(*epoch, test.kind) == MISTER_RESULT_OK);
		assert(io.last_deadline == test.deadline);
	}

	FakeClock clock(1000);
	HardwareBroker broker(clock);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_V2_KNOWN, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	const OperationKind forbidden[] = {OperationKind::program_fpga,
		OperationKind::input, OperationKind::scheduler, OperationKind::offload};
	for (OperationKind kind : forbidden) {
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch, kind, &lease) ==
			MISTER_RESULT_INVALID_STATE);
	}

	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	assert(terminal->absolute_deadline_ms() == 6000);
	terminal.reset();
	epoch.reset();
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	for (uint32_t missing : {MISTER_RESOURCE_FPGA, MISTER_RESOURCE_BRIDGES,
		MISTER_RESOURCE_CORE_PROTOCOL}) {
		assert(broker.BeginRecovery(closure & ~missing, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) ==
			MISTER_RESULT_INVALID_STATE);
		epoch.reset();
	}
}

void TestTruthfulPartitionAndExactOkRule()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_SAVES | MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	io.states[KindIndex(OperationKind::save)] =
		RecoveryResourceState::observed_non_neutral;
	io.states[KindIndex(OperationKind::audio)] = RecoveryResourceState::unknown;
	io.states[KindIndex(OperationKind::video)] = RecoveryResourceState::neutral;
	io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::video) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::content) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.neutral_resource_flags == (MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT));
	assert(observation.observed_resource_flags == MISTER_RESOURCE_SAVES);
	assert((observation.neutral_resource_flags &
		observation.observed_resource_flags) == 0);
	assert(((observation.neutral_resource_flags |
		observation.observed_resource_flags) & ~requested) == 0);
}

void TestNonOkResultsRetainPartialFields()
{
	const Result failures[] = {MISTER_RESULT_DEADLINE, MISTER_RESULT_PLATFORM,
		MISTER_RESULT_CLEANUP_INCOMPLETE};
	for (Result failure : failures) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		NativeRecovery recovery(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
			MISTER_RESOURCE_SAVES;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(OperationKind::input_descriptors)] =
			RecoveryResourceState::neutral;
		assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(OperationKind::save)] =
			RecoveryResourceState::observed_non_neutral;
		io.results[KindIndex(OperationKind::save)] = failure;
		assert(recovery.Perform(*epoch, OperationKind::save) == failure);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == failure);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
		assert(observation.observed_resource_flags == MISTER_RESOURCE_SAVES);
	}
}

void TestDeadlineExpiryDoesNotExtendAndRetainsProgress()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_SAVES;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	clock.SetNow(3000);
	assert(recovery.Perform(*epoch, OperationKind::save) ==
		MISTER_RESULT_DEADLINE);
	clock.SetNow(2500);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(io.last_deadline == 3000);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
}

void TestTerminalAdmissionDeadlineRetainsProgress()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT | closure;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	clock.SetNow(6000);
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) ==
		MISTER_RESULT_DEADLINE);
	assert(terminal == nullptr);
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(observation.observed_resource_flags == 0);
}

void TestBusyRecoveryAdmissionDoesNotLatchDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_SAVES;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> held;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::input_descriptors, &held) == MISTER_RESULT_OK);
	clock.SetNow(3000);
	std::unique_ptr<OperationLease> rejected;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::save,
		&rejected) == MISTER_RESULT_INVALID_STATE);
	assert(rejected == nullptr);
	held.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == 0);
}

void TestOperationOverrunRetainsTruthfulResult()
{
	class AdvancingRecoveryIo final : public NativeRecoveryIo {
	public:
		explicit AdvancingRecoveryIo(FakeClock &clock) : clock_(clock) {}
		Result CloseInputDescriptors(const OperationLease &,
			RecoveryResourceState *state) override
		{
			*state = RecoveryResourceState::neutral;
			clock_.SetNow(3000);
			return MISTER_RESULT_OK;
		}
		Result FlushAndCloseSave(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result MuteAudio(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result PowerDownVideo(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result CloseContent(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
		Result DisableCoreProtocol(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_PLATFORM; }
	private:
		FakeClock &clock_;
	};
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	AdvancingRecoveryIo io(clock);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_INPUT, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_DEADLINE);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(observation.observed_resource_flags == 0);
}

void TestCoreProtocolRecoveryUsesProfilelessSessionAuthority()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 3000, 6000,
		&epoch) == MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::core_protocol)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
		MISTER_RESULT_OK);
	assert(io.core_protocol_session_calls == 1);
	assert(io.last_deadline == 6000);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags ==
		MISTER_RESOURCE_CORE_PROTOCOL);
}

void TestTerminalRecoveryAndReadOnlyObservation()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == MISTER_RESULT_OK);
		assert(io.writes == 5 && io.reads == 5 && io.releases == 1);
		assert(broker.mutation_sequence_for_test() == 6);
		assert(broker.containment_receipt_sequence_for_test() == 6);
		std::unique_ptr<HardwareLeaseView> forbidden_view;
		assert(broker.AcquireHardwareLeaseView(*terminal, &forbidden_view) ==
			MISTER_RESULT_INVALID_STATE);
		assert(forbidden_view == nullptr);
		std::unique_ptr<OperationLease> rejected;
		assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
			&rejected) == MISTER_RESULT_INVALID_STATE);
		assert(broker.mutation_sequence_for_test() == 6);
		assert(broker.containment_receipt_sequence_for_test() == 6);
		terminal.reset();
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == closure);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000,
			&epoch) == MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
		assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_FPGA);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		io.core = 0;
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000,
			&epoch) == MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(observation.observed_resource_flags == MISTER_RESOURCE_FPGA);
		assert(observation.neutral_resource_flags == 0);
	}
}

void TestRepeatedObservationReplacesStaleClassification()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	const uint32_t requested = MISTER_RESOURCE_FPGA |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	io.core = 0;
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.observed_resource_flags == requested);
	assert(observation.neutral_resource_flags == 0);
}

void TestForeignRecoveryAuthorityCannotMutate()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeContainmentIo io;
	NativeContainment containment(first, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> second_epoch;
	assert(second.BeginRecovery(closure, 3000, 6000, &second_epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> second_lease;
	assert(second.BeginRecoveryOperation(*second_epoch,
		OperationKind::terminal_fpga_cleanup, &second_lease) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*second_epoch, *second_lease) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.writes == 0 && io.reads == 0 && io.releases == 0);
}

void TestForeignRecoveryEpochWithCurrentLeaseCannotTouchHardware()
{
	FakeClock clock(1000);
	HardwareBroker first(clock);
	HardwareBroker second(clock);
	FakeContainmentIo io;
	NativeContainment containment(first, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> first_epoch;
	std::unique_ptr<RecoveryEpoch> second_epoch;
	assert(first.BeginRecovery(closure, 3000, 6000, &first_epoch) ==
		MISTER_RESULT_OK);
	assert(second.BeginRecovery(closure, 3000, 6000, &second_epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> first_terminal;
	assert(first.BeginRecoveryOperation(*first_epoch,
		OperationKind::terminal_fpga_cleanup, &first_terminal) ==
		MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*second_epoch, *first_terminal) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.writes == 0 && io.reads == 0 && io.releases == 0);
	assert(first.mutation_sequence_for_test() == 0);
	assert(containment.ResetAndContain(*first_epoch, *first_terminal) ==
		MISTER_RESULT_OK);
	first_terminal.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(first.FinishRecovery(std::move(first_epoch), &observation) ==
		MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == closure);
}

void TestRejectedTerminalCallDoesNotPoisonRecovery()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
	std::unique_ptr<OperationLease> wrong_kind;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&wrong_kind) == MISTER_RESULT_OK);
	assert(containment.ResetAndContain(*epoch, *wrong_kind) ==
		MISTER_RESULT_INVALID_STATE);
	assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
	wrong_kind.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == closure);
}

void TestTerminalDeadlineBeforeHardwareRetainsRecoveryPartition()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> terminal;
	assert(broker.BeginRecoveryOperation(*epoch,
		OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
	clock.SetNow(6000);
	assert(containment.ResetAndContain(*epoch, *terminal) ==
		MISTER_RESULT_DEADLINE);
	assert(io.writes == 0 && io.reads == 5 && io.releases == 0);
	terminal.reset();
	MisterRecoveryObservationV2 observation = Observation();
	assert(broker.FinishRecovery(std::move(epoch), &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == closure);
}

void TestTwoHundredRecoveryCycles()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	for (int cycle = 0; cycle != 200; ++cycle) {
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(0, 3000, 6000, &epoch) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
	}
}

void TestTerminalFailuresPreservePositivePartitions()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	struct Case {
		int fail_at;
		bool mismatch;
		bool deadline;
		Result result;
		uint32_t observed;
		uint32_t neutral;
	};
	const Case cases[] = {
		{0, true, false, MISTER_RESULT_CLEANUP_INCOMPLETE,
			MISTER_RESOURCE_FPGA | MISTER_RESOURCE_CORE_PROTOCOL,
			MISTER_RESOURCE_BRIDGES},
		{8, false, false, MISTER_RESULT_PLATFORM, 0,
			MISTER_RESOURCE_CORE_PROTOCOL},
		{0, false, true, MISTER_RESULT_DEADLINE, 0,
			MISTER_RESOURCE_CORE_PROTOCOL},
		{11, false, false, MISTER_RESULT_PLATFORM, 0,
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL},
		{-1, false, true, MISTER_RESULT_DEADLINE, 0,
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL}
	};
	for (const Case &test : cases) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeContainmentIo io;
		io.fail_at = test.fail_at;
		if (test.mismatch) io.core = 0;
		if (test.deadline) {
			io.advance_at = test.fail_at == -1 ? 10 : 8;
			io.advance_clock = &clock;
		}
		NativeContainment containment(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> terminal;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::terminal_fpga_cleanup, &terminal) == MISTER_RESULT_OK);
		assert(containment.ResetAndContain(*epoch, *terminal) == test.result);
		terminal.reset();
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			test.result);
		assert(observation.observed_resource_flags == test.observed);
		assert(observation.neutral_resource_flags == test.neutral);
	}
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	using namespace mister::native;
	TestRecoveryEpochAuthorityAndFreshness();
	TestRecoveryRejectedWithLiveGenerationOrCleanup();
	TestNormativeDependenciesAndExactDeadlines();
	TestTruthfulPartitionAndExactOkRule();
	TestNonOkResultsRetainPartialFields();
	TestDeadlineExpiryDoesNotExtendAndRetainsProgress();
	TestTerminalAdmissionDeadlineRetainsProgress();
	TestBusyRecoveryAdmissionDoesNotLatchDeadline();
	TestOperationOverrunRetainsTruthfulResult();
	TestCoreProtocolRecoveryUsesProfilelessSessionAuthority();
	TestTerminalRecoveryAndReadOnlyObservation();
	TestRepeatedObservationReplacesStaleClassification();
	TestForeignRecoveryAuthorityCannotMutate();
	TestForeignRecoveryEpochWithCurrentLeaseCannotTouchHardware();
	TestRejectedTerminalCallDoesNotPoisonRecovery();
	TestTerminalDeadlineBeforeHardwareRetainsRecoveryPartition();
	TestTwoHundredRecoveryCycles();
	TestTerminalFailuresPreservePositivePartitions();
	return 0;
}
