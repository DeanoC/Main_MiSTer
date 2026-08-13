// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_core_protocol_io_adapter.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_recovery.hpp"
#include "runtime/native/native_resources.hpp"

#include <assert.h>

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mister {
namespace native {
namespace {

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	void SetNow(uint64_t now_ms) { now_ms_ = now_ms; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t) override { return false; }

private:
	uint64_t now_ms_;
};

class MemoryContent final : public NativeContentResource {
public:
	MemoryContent() : describe_calls(0), read_calls(0), bytes_{0x11, 0x22, 0x33} {}
	NativeAcquisitionOutcome RetainContent(uint64_t) override
		{ return {MISTER_RESULT_OK, true}; }
	Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t) override
	{
		++describe_calls;
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		description->size = bytes_.size();
		description->extension_length = 2;
		description->extension[0] = 'm';
		description->extension[1] = 'd';
		description->extension[2] = '\0';
		description->extension[3] = '\0';
		return MISTER_RESULT_OK;
	}
	Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t) override
	{
		++read_calls;
		if (bytes == nullptr || offset > bytes_.size() || count >
			bytes_.size() - static_cast<size_t>(offset))
			return MISTER_RESULT_INVALID_ARGUMENT;
		uint8_t *out = static_cast<uint8_t *>(bytes);
		for (size_t index = 0; index < count; ++index)
			out[index] = bytes_[static_cast<size_t>(offset) + index];
		return MISTER_RESULT_OK;
	}
	Result CloseContent(uint64_t) override { return MISTER_RESULT_OK; }
	void CloseContentForProcessExit() override {}

	int describe_calls;
	int read_calls;

private:
	std::vector<uint8_t> bytes_;
};

class ProtocolRecoveryIo final : public NativeRecoveryIo {
public:
	explicit ProtocolRecoveryIo(NativeCoreProtocol &protocol) : protocol_(protocol) {}
	Result CloseInputDescriptors(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result FlushAndCloseSave(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result MuteAudio(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result PowerDownVideo(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result CloseContent(const OperationLease &, RecoveryResourceState *)
		override { return MISTER_RESULT_INVALID_STATE; }
	Result DisableCoreProtocol(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		return protocol_.DisableStateless(lease, state);
	}

private:
	NativeCoreProtocol &protocol_;
};

class FakeOperations final : public linux_native::NativeCoreProtocolIoTestOperations {
public:
	enum class Operation {
		open,
		close,
		map,
		unmap,
		read_gpo,
		read_gpi,
		write_gpo,
		barrier,
	};
	struct OperationCall {
		int number;
		Operation operation;
		size_t gpi_index;
	};

	FakeOperations() : page_size(4096), page_size_calls(0), gpo(0),
		gpi_samples(), gpi_index(0), map_calls(0),
		unmap_calls(0), close_calls(0), fail_unmap_calls(0),
		fail_close_calls(0), fail_write_at(0), write_calls(0), fail_at(0),
		calls(0), operation_calls(), gpo_writes() {}
	bool Step(Operation operation)
	{
		++calls;
		operation_calls.push_back({calls, operation, gpi_index});
		return fail_at != 0 && calls == fail_at;
	}
	size_t PageSize() const override { ++page_size_calls; return page_size; }
	int Open(const char *, int) override { return Step(Operation::open) ? -1 : 9; }
	int Close(int descriptor) override
	{
		if (Step(Operation::close)) return -1;
		if (descriptor != 9) return -1;
		++close_calls;
		if (fail_close_calls != 0) {
			--fail_close_calls;
			return -1;
		}
		return 0;
	}
	int Map(int descriptor, uint64_t page_offset, size_t length,
		linux_native::NativeCoreProtocolIoTestMapping *mapping) override
	{
		if (Step(Operation::map)) return -1;
		if (descriptor != 9 || page_offset != 0xff706000u || length != 4096 ||
			mapping == nullptr) return -1;
		mapping->identity = 0x1000;
		++map_calls;
		return 0;
	}
	int Unmap(const linux_native::NativeCoreProtocolIoTestMapping &mapping,
		size_t length) override
	{
		if (Step(Operation::unmap)) return -1;
		if (mapping.identity != 0x1000 || length != 4096) return -1;
		++unmap_calls;
		if (fail_unmap_calls != 0) {
			--fail_unmap_calls;
			return -1;
		}
		return 0;
	}
	int Read32(const linux_native::NativeCoreProtocolIoTestMapping &mapping,
		size_t offset, uint32_t *value) override
	{
		if (Step(offset == 0x10 ? Operation::read_gpo : Operation::read_gpi))
			return -1;
		if (mapping.identity != 0x1000 || value == nullptr) return -1;
		if (offset == 0x10) {
			*value = gpo;
			return 0;
		}
		if (offset != 0x14 || gpi_index >= gpi_samples.size()) return -1;
		*value = gpi_samples[gpi_index++];
		return 0;
	}
	int Write32(const linux_native::NativeCoreProtocolIoTestMapping &mapping,
		size_t offset, uint32_t value) override
	{
		if (Step(Operation::write_gpo)) return -1;
		if (mapping.identity != 0x1000 || offset != 0x10) return -1;
		++write_calls;
		if (fail_write_at != 0 && write_calls == fail_write_at) return -1;
		gpo = value;
		gpo_writes.push_back(value);
		return 0;
	}
	int OrderingBarrier() override { return Step(Operation::barrier) ? -1 : 0; }

	void ScriptMegaDriveActivation()
	{
		gpi_samples.push_back(0x5ca623a8u);
		gpi_samples.push_back(0x00010000u);
		gpi_samples.push_back(0x00080000u);
		std::vector<uint16_t> responses;
		responses.insert(responses.end(), 11, 0);
		responses.push_back(0);
		const char *name = "MegaDrive";
		for (size_t index = 0; name[index] != '\0'; ++index)
			responses.push_back(static_cast<uint8_t>(name[index]));
		responses.push_back(';');
		responses.insert(responses.end(), 10, 0);
		responses.push_back(0);
		responses.insert(responses.end(), 11, 0);
		for (uint16_t response : responses) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(response);
		}
	}

	void ScriptMegaDriveFailureAfterDownloadStart()
	{
		ScriptMegaDriveActivation();
		// 3 live-probe reads, then high/low ACK samples for 29 successful
		// words through the file-I/O start command. The next ACK belongs to
		// the first payload word, after download residue is positive.
		const size_t first_payload_ack = 3 + 2 * 29;
		gpi_samples.resize(first_payload_ack + 1);
		gpi_samples[first_payload_ack] = 0x80000000u;
		// Cleanup carries the exact profile but has no active probe capability;
		// it sends only the profile-authorized file-I/O stop command.
		for (size_t index = 0; index != 2; ++index) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(0);
		}
	}

	void ScriptCleanupStop()
	{
		for (size_t index = 0; index != 2; ++index) {
			gpi_samples.push_back(0x00020000u);
			gpi_samples.push_back(0);
		}
	}

	void FailAckHighForRecipeWord(size_t word_index)
	{
		const size_t sample_index = 3 + word_index * 2;
		assert(sample_index < gpi_samples.size());
		gpi_samples.resize(sample_index + 1);
		gpi_samples[sample_index] = 0x80000000u;
	}

	void FailAckLowForRecipeWord(size_t word_index)
	{
		const size_t sample_index = 3 + word_index * 2 + 1;
		assert(sample_index < gpi_samples.size());
		gpi_samples.resize(sample_index + 1);
		gpi_samples[sample_index] = 0x80000000u;
	}

	size_t page_size;
	mutable int page_size_calls;
	uint32_t gpo;
	std::vector<uint32_t> gpi_samples;
	size_t gpi_index;
	int map_calls;
	int unmap_calls;
	int close_calls;
	int fail_unmap_calls;
	int fail_close_calls;
	int fail_write_at;
	int write_calls;
	int fail_at;
	int calls;
	std::vector<OperationCall> operation_calls;
	std::vector<uint32_t> gpo_writes;
};

void PrepareCleanup(HardwareBroker &broker, const NativeCoreProfile &profile,
	PlatformGenerationId *generation,
	std::unique_ptr<CleanupEpoch> *epoch)
{
	assert(broker.EnterFixtureForTest(profile, generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(*generation, OperationKind::input, 2000, &active) ==
		MISTER_RESULT_OK);
	active.reset();
	assert(broker.Quiesce(*generation, 2000) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(*generation, 3000, 6000, epoch) == MISTER_RESULT_OK);
}

void TestMegaDriveActivationUsesOneTypedMappingAndExactRelease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveActivation();
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &lease) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == MISTER_RESULT_OK && outcome.acquired);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1);
	assert((operations.gpo & 0x00160000u) == 0);
	assert(operations.gpi_index == operations.gpi_samples.size());
}

void TestSnesActivationIsUnsupportedBeforeAdapterHardware()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &lease) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == MISTER_RESULT_UNSUPPORTED && !outcome.acquired &&
		!outcome.broker_failure_completed);
	assert(operations.map_calls == 0 && operations.gpo_writes.empty());
}

void TestConstructionAllocationFailureIsAtomicAndPreventsActivation()
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	// The construction graph deliberately has one allocation for the adapter's
	// inseparable four capability views and one for core-protocol teardown.
	// Neither failure may expose a null view, retain partially usable authority,
	// describe content, acquire a broker session, or touch the adapter backend.
	for (size_t allocation = 1; allocation != 3; ++allocation) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		SetNativeCoreProtocolAllocationFailureForTest(allocation);
		{
			linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
			NativeCoreProtocol protocol(clock, broker, io.capabilities());
			ClearNativeCoreProtocolAllocationFailureForTest();
			assert(!protocol.valid());
			if (allocation == 1) {
				assert(!io.valid());
				assert(!io.capabilities().Available());
			} else {
				assert(!io.valid());
			}
			MemoryContent content;
			PlatformGenerationId generation = 0;
			assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> lease;
			assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
				&lease) == MISTER_RESULT_OK);
			const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
				content);
			assert(outcome.result == MISTER_RESULT_PLATFORM && !outcome.acquired &&
				!outcome.broker_failure_completed);
			assert(content.describe_calls == 0 && content.read_calls == 0);
		}
		assert(operations.calls == 0 && operations.map_calls == 0 &&
			operations.gpo_writes.empty());
	}
}

void TestMixedBundlesCannotBeConstructedOrComplete()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations_a;
	FakeOperations operations_b;
	linux_native::NativeCoreProtocolIoAdapter adapter_a(clock, operations_a);
	linux_native::NativeCoreProtocolIoAdapter adapter_b(clock, operations_b);
	NativeCoreProtocolCapabilities capabilities_a = adapter_a.capabilities();
	NativeCoreProtocolCapabilities capabilities_b = adapter_b.capabilities();
	assert(capabilities_a.Available() && capabilities_b.Available());
	assert(!adapter_a.valid() && !adapter_b.valid());
	// The public constructor accepts exactly one cohesive value. It cannot be
	// expressed with active A and teardown B (or the reverse), so an unavailable
	// bundle fails closed without causing protocol broker or adapter activity.
	NativeCoreProtocolCapabilities absent;
	NativeCoreProtocol protocol(clock, broker, std::move(absent));
	assert(!protocol.valid());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*active, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && !outcome.acquired &&
		!outcome.broker_failure_completed);
	assert(content.describe_calls == 0 && operations_a.calls == 0 &&
		operations_b.calls == 0);
	active.reset();
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_PLATFORM);
	assert(operations_a.calls == 0 && operations_b.calls == 0);
}

void TestSameBundleSurvivesDestroyedAdapterAndExternalHandle()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	NativeCoreProtocolCapabilities capabilities;
	std::unique_ptr<NativeCoreProtocol> protocol;
	{
		linux_native::NativeCoreProtocolIoAdapter adapter(clock, operations);
		capabilities = adapter.capabilities();
		assert(capabilities.Available());
		protocol.reset(new NativeCoreProtocol(clock, broker, std::move(capabilities)));
		assert(protocol->valid());
	}
	assert(!capabilities.Available());
	assert(protocol->valid());
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	RecoveryResourceState state = RecoveryResourceState::unknown;
	assert(protocol->DisableStateless(*lease, &state) == MISTER_RESULT_OK);
	assert(state == RecoveryResourceState::neutral && operations.map_calls == 1 &&
		operations.unmap_calls == 1);
}

void TestCleanupRetryRetainsMappingAndUsesTheSameLease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(broker, profile, &generation, &epoch);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	operations.fail_unmap_calls = 1;
	assert(protocol.ShutdownLive(*lease) == MISTER_RESULT_PLATFORM);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1);
	assert(protocol.ShutdownLive(*lease) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2);
}

void TestPostUnmapCloseRetryNeverRemaps()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(broker, profile, &generation, &epoch);
	std::unique_ptr<OperationLease> owner;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&owner) == MISTER_RESULT_OK);
	operations.fail_close_calls = 1;
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_PLATFORM);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 2);
}

void TestPostUnmapCloseRetryRejectsForeignAndDoesNotExtendDeadline()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	std::unique_ptr<CleanupEpoch> epoch;
	PrepareCleanup(broker, profile, &generation, &epoch);
	std::unique_ptr<OperationLease> owner;
	std::unique_ptr<OperationLease> foreign;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&owner) == MISTER_RESULT_OK);
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&foreign) == MISTER_RESULT_OK);
	const uint64_t deadline = owner->absolute_deadline_ms();
	operations.fail_close_calls = 1;
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_PLATFORM);
	assert(protocol.ShutdownLive(*foreign) == MISTER_RESULT_INVALID_STATE);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	clock.SetNow(deadline);
	assert(protocol.ShutdownLive(*owner) == MISTER_RESULT_DEADLINE);
	assert(owner->absolute_deadline_ms() == deadline);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
}

std::vector<int> FinalDeselectBoundariesAfterAckLow(size_t gpi_index)
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveActivation();
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
		&lease) == MISTER_RESULT_OK);
	assert(protocol.Activate(*lease, profile, content).result == MISTER_RESULT_OK);
	const FakeOperations::Operation expected[] = {
		FakeOperations::Operation::read_gpo,
		FakeOperations::Operation::write_gpo,
		FakeOperations::Operation::barrier,
		FakeOperations::Operation::read_gpo,
	};
	std::vector<int> boundaries;
	for (size_t index = 0; index + 4 <= operations.operation_calls.size(); ++index) {
		bool match = true;
		for (size_t offset = 0; offset != 4; ++offset) {
			const FakeOperations::OperationCall &call =
				operations.operation_calls[index + offset];
			if (call.gpi_index != gpi_index || call.operation != expected[offset]) {
				match = false;
				break;
			}
		}
		if (match) {
			for (size_t offset = 0; offset != 4; ++offset)
				boundaries.push_back(operations.operation_calls[index + offset].number);
			break;
		}
	}
	assert(boundaries.size() == 4);
	return boundaries;
}

void AssertFinalDeselectFailureReceipt(int boundary, bool expect_download,
	bool expect_reset)
{
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveActivation();
	operations.fail_at = boundary;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
		&lease) == MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	ActiveProtocolFailureReceipt receipt = {};
	assert(broker.core_protocol_failure_receipt_for_test(&receipt));
	assert(receipt.residue.download_may_be_active == expect_download);
	assert(receipt.residue.status_reset_asserted == expect_reset);
}

void TestPositiveFrameFinalDeselectBoundariesRecordResidue()
{
	// Each vector begins immediately after the final ACK-low of the exact frame.
	// The four fallible typed deselect primitives are GPO read, GPO write,
	// ordering barrier, and GPO readback. A reset/download effect is already
	// possible at this point, so every failure receipt must retain it.
	const std::vector<int> reset_assert = FinalDeselectBoundariesAfterAckLow(25);
	const std::vector<int> download_start = FinalDeselectBoundariesAfterAckLow(61);
	for (int boundary : reset_assert)
		AssertFinalDeselectFailureReceipt(boundary, false, true);
	for (int boundary : download_start)
		AssertFinalDeselectFailureReceipt(boundary, true, true);
}

void TestClearFrameFinalDeselectBoundariesDoNotClearResidue()
{
	const std::vector<int> download_stop = FinalDeselectBoundariesAfterAckLow(73);
	const std::vector<int> reset_clear = FinalDeselectBoundariesAfterAckLow(91);
	for (int boundary : download_stop)
		AssertFinalDeselectFailureReceipt(boundary, true, true);
	for (int boundary : reset_clear)
		AssertFinalDeselectFailureReceipt(boundary, false, true);
}

void TestActiveFailureTransfersOnlyRetainedMappingToCleanup()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	// This fails during payload transfer after the start command. The active
	// recipe therefore reports positive download residue after release leaves an
	// unmap retry pending. Cleanup must reuse that one mapping and issue its
	// profile-authorized stop—not map a fresh page or resurrect the completed
	// active registration.
	operations.ScriptMegaDriveFailureAfterDownloadStart();
	operations.fail_unmap_calls = 1;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = protocol.Activate(*active, profile,
		content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1 &&
		operations.close_calls == 1);
	active.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	assert(protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.close_calls == 1);
	assert(operations.gpi_index == operations.gpi_samples.size());
}

void TestSecondBundleMintCannotOmitRetainedDownloadStop()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.ScriptMegaDriveFailureAfterDownloadStart();
	operations.fail_unmap_calls = 1;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocolCapabilities first = io.capabilities();
	NativeCoreProtocolCapabilities second = io.capabilities();
	assert(first.Available() && !second.Available() && !io.valid());
	assert(operations.calls == 0 && operations.map_calls == 0);
	NativeCoreProtocol active_protocol(clock, broker, std::move(first));
	NativeCoreProtocol teardown_protocol(clock, broker, std::move(second));
	assert(active_protocol.valid() && !teardown_protocol.valid());
	MemoryContent content;
	const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> active;
	assert(broker.Begin(generation, OperationKind::core_protocol, 1000, &active) ==
		MISTER_RESULT_OK);
	const NativeCoreProtocolOutcome outcome = active_protocol.Activate(*active,
		profile, content);
	assert(outcome.result == MISTER_RESULT_PLATFORM && outcome.acquired &&
		outcome.broker_failure_completed);
	active.reset();
	assert(broker.Quiesce(generation, 2000) == MISTER_RESULT_OK);
	std::unique_ptr<CleanupEpoch> epoch;
	assert(broker.BeginCleanup(generation, 3000, 6000, &epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> cleanup;
	assert(broker.BeginCleanupOperation(*epoch, OperationKind::core_protocol,
		&cleanup) == MISTER_RESULT_OK);
	const int calls_before_rejected_cleanup = operations.calls;
	assert(teardown_protocol.ShutdownLive(*cleanup) == MISTER_RESULT_PLATFORM);
	assert(operations.calls == calls_before_rejected_cleanup);
	assert(active_protocol.ShutdownLive(*cleanup) == MISTER_RESULT_OK);
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.gpi_index == operations.gpi_samples.size());
}

void TestProfilelessRecoveryOnlyForcesIdleAndReleases()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	RecoveryResourceState state = RecoveryResourceState::unknown;
	assert(protocol.DisableStateless(*lease, &state) == MISTER_RESULT_OK);
	assert(state == RecoveryResourceState::neutral);
	assert(operations.map_calls == 1 && operations.unmap_calls == 1);
	for (uint32_t write : operations.gpo_writes) {
		assert((write & 0x80000000u) == 0);
		assert((write & 0x0000ffffu) == 0);
	}
}

void TestInvalidPageSizeFailsBeforeMappingOrRegisterIo()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	operations.page_size = 0;
	linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
	NativeCoreProtocol protocol(clock, broker, io.capabilities());
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	RecoveryResourceState state = RecoveryResourceState::neutral;
	assert(protocol.DisableStateless(*lease, &state) == MISTER_RESULT_PLATFORM);
	assert(state == RecoveryResourceState::unknown);
	assert(operations.page_size_calls == 1 && operations.calls == 0 &&
		operations.map_calls == 0 && operations.gpo_writes.empty());
}

void TestProcessExitCloseMakesOnlyBestEffortRelease()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeOperations operations;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
		&epoch) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.BeginRecoveryOperation(*epoch, OperationKind::core_protocol,
		&lease) == MISTER_RESULT_OK);
	operations.fail_unmap_calls = 1;
	{
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		RecoveryResourceState state = RecoveryResourceState::neutral;
		assert(protocol.DisableStateless(*lease, &state) == MISTER_RESULT_PLATFORM);
		assert(state == RecoveryResourceState::unknown);
		assert(broker.core_protocol_session_abandoned_for_test(*lease));
	}
	// Process exit releases only the remaining physical residue; it never
	// converts the abandoned broker session to a successful completion.
	assert(operations.map_calls == 1 && operations.unmap_calls == 2 &&
		operations.close_calls == 1);
	assert(broker.core_protocol_session_abandoned_for_test(*lease));
}

void TestEveryAdapterBoundaryFailureRetainsATruthfulNonNeutralResult()
{
	// Profileless recovery exercises map, GPO read/write/barrier/readback,
	// unmap, and descriptor close without making any protocol exchange.
	for (int boundary = 1; boundary <= 12; ++boundary) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.fail_at = boundary;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
			&epoch) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.BeginRecoveryOperation(*epoch,
			OperationKind::core_protocol, &lease) == MISTER_RESULT_OK);
		RecoveryResourceState state = RecoveryResourceState::neutral;
		assert(protocol.DisableStateless(*lease, &state) != MISTER_RESULT_OK);
		assert(state != RecoveryResourceState::neutral);
		assert(operations.calls >= boundary);
	}
}

void TestEveryActivationAdapterOperationBoundaryFailsClosed()
{
	// The complete fixture drives open, map, force-low/readback, identity
	// probe/restore, select, every data/strobe/ACK-high/ACK-low edge, selected
	// continuation, deselect/readback, unmap, and descriptor close. Injecting a
	// failure in each deterministic operation must never report activation OK.
	int total_operations = 0;
	{
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.ScriptMegaDriveActivation();
		operations.fail_at = std::numeric_limits<int>::max();
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		MemoryContent content;
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
			&lease) == MISTER_RESULT_OK);
		assert(protocol.Activate(*lease, profile, content).result == MISTER_RESULT_OK);
		total_operations = operations.calls;
	}
	assert(total_operations > 0);
	for (int boundary = 1; boundary <= total_operations; ++boundary) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		operations.ScriptMegaDriveActivation();
		operations.fail_at = boundary;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		MemoryContent content;
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("megadrive");
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::core_protocol, 1000,
			&lease) == MISTER_RESULT_OK);
		const NativeCoreProtocolOutcome outcome = protocol.Activate(*lease,
			profile, content);
		assert(outcome.result != MISTER_RESULT_OK);
		assert(operations.calls >= boundary);
	}
}

void TestTwoHundredProfilelessRecoveryCycles()
{
	for (size_t cycle = 0; cycle != 200; ++cycle) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeOperations operations;
		linux_native::NativeCoreProtocolIoAdapter io(clock, operations);
		NativeCoreProtocol protocol(clock, broker, io.capabilities());
		ProtocolRecoveryIo recovery_io(protocol);
		NativeRecovery recovery(broker, recovery_io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CORE_PROTOCOL, 1000, 2000,
			&epoch) == MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
			MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = {};
		observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		observation.struct_size = sizeof(observation);
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
	}
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestMegaDriveActivationUsesOneTypedMappingAndExactRelease();
	mister::native::TestSnesActivationIsUnsupportedBeforeAdapterHardware();
	mister::native::TestConstructionAllocationFailureIsAtomicAndPreventsActivation();
	mister::native::TestMixedBundlesCannotBeConstructedOrComplete();
	mister::native::TestSameBundleSurvivesDestroyedAdapterAndExternalHandle();
	mister::native::TestCleanupRetryRetainsMappingAndUsesTheSameLease();
	mister::native::TestPositiveFrameFinalDeselectBoundariesRecordResidue();
	mister::native::TestClearFrameFinalDeselectBoundariesDoNotClearResidue();
	mister::native::TestPostUnmapCloseRetryNeverRemaps();
	mister::native::TestPostUnmapCloseRetryRejectsForeignAndDoesNotExtendDeadline();
	mister::native::TestActiveFailureTransfersOnlyRetainedMappingToCleanup();
	mister::native::TestSecondBundleMintCannotOmitRetainedDownloadStop();
	mister::native::TestProfilelessRecoveryOnlyForcesIdleAndReleases();
	mister::native::TestInvalidPageSizeFailsBeforeMappingOrRegisterIo();
	mister::native::TestProcessExitCloseMakesOnlyBestEffortRelease();
	mister::native::TestEveryAdapterBoundaryFailureRetainsATruthfulNonNeutralResult();
	mister::native::TestEveryActivationAdapterOperationBoundaryFailsClosed();
	mister::native::TestTwoHundredProfilelessRecoveryCycles();
	return 0;
}
