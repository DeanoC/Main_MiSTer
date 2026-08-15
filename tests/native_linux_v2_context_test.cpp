// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_linux_v2_context.hpp"
#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_core_protocol.hpp"
#include "runtime/native/native_input.hpp"
#include "runtime/native/native_lifecycle.hpp"
#include "runtime/native/native_recovery.hpp"
#include "runtime/native/native_spi_bus.hpp"
#include "runtime/native/linux/native_audio_adapter.hpp"
#include "runtime/native/linux/native_av_io_adapter.hpp"
#include "runtime/native/linux/native_content_adapter.hpp"
#include "runtime/native/linux/native_core_artifact_adapter.hpp"
#include "runtime/native/linux/native_core_protocol_io_adapter.hpp"
#include "runtime/native/linux/native_fpga_programmer.hpp"
#include "runtime/native/linux/native_input_adapter.hpp"
#include "runtime/native/linux/native_mmio_adapter.hpp"
#include "runtime/native/linux/native_offload_adapter.hpp"
#include "runtime/native/linux/native_save_adapter.hpp"
#include "runtime/native/linux/native_scheduler_adapter.hpp"
#include "runtime/native/linux/native_video_adapter.hpp"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(MISTER_NATIVE_LINUX_V2_CONTEXT_REACHABILITY_PROBE)
static mister::native::linux_native::NativeLinuxV2FixtureProfiles forbidden_profiles;
int main() { return forbidden_profiles.snes == nullptr ? 0 : 1; }
#else

namespace linux_native = mister::native::linux_native;
using mister::native::FixtureNativeCoreProfile;
using mister::native::NativeClock;
using mister::native::NativeCoreProfile;
using namespace mister::native;

using namespace mister::native::linux_native;

static const uint32_t kConcreteGraphCallbackBudgetMs = 60000;

class TestClock final : public NativeClock {
public:
	explicit TestClock(uint64_t now) : now_(now) {}
	uint64_t NowMs() const override { return now_; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t absolute_deadline_ms) override
	{
		now_ = absolute_deadline_ms;
		return false;
	}
	void Set(uint64_t now) { now_ = now; }

private:
	uint64_t now_;
};

static void AssertSameRetainedOperationSnapshot(
	const NativeRetainedOperationSnapshot &left,
	const NativeRetainedOperationSnapshot &right)
{
	assert(memcmp(&left, &right, sizeof(left)) == 0);
}

static void NormalizeRetainedRebindFields(
	NativeRetainedOperationSnapshot *snapshot)
{
	snapshot->registration_effective_deadline_ms = 0;
	snapshot->invocation_identity = 0;
	snapshot->invocation_callback_deadline_ms = 0;
	snapshot->peripheral_phase = PeripheralSessionPhase::live;
	snapshot->protocol_phase = ProtocolSessionHandleState::live;
	snapshot->core_disposition = CoreProtocolBrokerDisposition::no_session;
	snapshot->peripheral_disposition = PeripheralBrokerDisposition::no_session;
	snapshot->invocation_registered = false;
	snapshot->invocation_outcome_missing = false;
	snapshot->registration_is_invoked = false;
	snapshot->registration_is_suspended = false;
	snapshot->registration_outcome_recorded = false;
	snapshot->process_guard_active = false;
}

static void AssertOnlyRetainedRebindFieldsChanged(
	const NativeRetainedOperationSnapshot &suspended,
	const NativeRetainedOperationSnapshot &rebound)
{
	NativeRetainedOperationSnapshot normalized_suspended = suspended;
	NativeRetainedOperationSnapshot normalized_rebound = rebound;
	NormalizeRetainedRebindFields(&normalized_suspended);
	NormalizeRetainedRebindFields(&normalized_rebound);
	AssertSameRetainedOperationSnapshot(normalized_suspended, normalized_rebound);
}

static void AssertExactIdleRetainedOperationSnapshot(
	const NativeRetainedOperationSnapshot &snapshot)
{
	NativeRetainedOperationSnapshot expected = {};
	expected.query_valid = true;
	expected.broker_idle = true;
	AssertSameRetainedOperationSnapshot(snapshot, expected);
}

struct GraphRetainedCapture {
	NativeLifecycle *lifecycle = nullptr;
	NativeRecovery *recovery = nullptr;
	const RecoveryEpoch *recovery_epoch = nullptr;
	OperationKind kind = OperationKind::save;
	bool armed = false;
	bool captured = false;
	NativeRetainedOperationSnapshot snapshot = {};
	void Arm(OperationKind value)
	{
		kind = value;
		armed = true;
		captured = false;
		snapshot = NativeRetainedOperationSnapshot();
	}
	void Capture()
	{
		if (!armed || captured) return;
		const Result result = lifecycle != nullptr ?
			lifecycle->cleanup_retained_callback_snapshot_for_test(kind, &snapshot) :
			recovery != nullptr && recovery_epoch != nullptr ?
			recovery->retained_snapshot_for_test(*recovery_epoch, kind, &snapshot) :
			MISTER_RESULT_INVALID_STATE;
		assert(result == MISTER_RESULT_OK);
		captured = true;
	}
};

static std::string GraphCopyView(MisterStringView view)
{
	return std::string(view.data == nullptr ? "" : view.data, view.length);
}

class GraphMmioOperations final : public NativeMmioTestOperations {
public:
	GraphMmioOperations()
		: descriptor_open(false), core(0x92345678u), interface_module(9),
		  sdr(9), bridge(0), remap(0), manager_stat(0x80u), manager_ctrl(2),
		  manager_gpio(3), manager_dclk_status(0), event_count(0),
		  fail_next_write_(false), fail_next_unmap_(false)
	{
		for (size_t index = 0; index != 7; ++index) held[index] = false;
	}
	~GraphMmioOperations() override
	{
		if (final_descriptor_open_ != nullptr)
			*final_descriptor_open_ = descriptor_open;
		if (final_mappings_held_ != nullptr) *final_mappings_held_ = AnyHeld();
	}
	size_t PageSize() const override { return 4096; }
	uint64_t NowMs() const override { return 100; }
	int Open(const char *, int) override
	{
		++event_count; descriptor_open = true; return 37;
	}
	int Close(int descriptor) override
	{
		assert(descriptor == 37); ++event_count; descriptor_open = false; return 0;
	}
	int Map(int descriptor, uint64_t page, size_t,
		NativeMmioTestMapping *mapping) override
	{
		assert(descriptor == 37 && descriptor_open && mapping != nullptr);
		const uintptr_t identity = IdentityForPage(page);
		assert(identity != 0 && !held[identity]);
		held[identity] = true; mapping->identity = identity; ++event_count; return 0;
	}
	int Unmap(const NativeMmioTestMapping &mapping, size_t) override
	{
		assert(mapping.identity > 0 && mapping.identity < 7 && held[mapping.identity]);
		if (fail_next_unmap_) { fail_next_unmap_ = false; return -1; }
		held[mapping.identity] = false; ++event_count; return 0;
	}
	int Read32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t *value) override
	{
		ConsumeDeadline();
		assert(value != nullptr && held[mapping.identity]); ++event_count;
		if (mapping.identity == 1 && offset == 0) *value = manager_stat;
		else if (mapping.identity == 1 && offset == 4) *value = manager_ctrl;
		else if (mapping.identity == 1 && offset == 0x0c) *value = manager_dclk_status;
		else if (mapping.identity == 1 && offset == 0x14)
			*value = (core & 0x00020000u) | 0x5a5au;
		else if (mapping.identity == 1 && offset == 0x850) *value = manager_gpio;
		else *value = Register(mapping.identity);
		return 0;
	}
	int Write32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t value) override
	{
		ConsumeDeadline();
		assert(held[mapping.identity]); ++event_count;
		if (fail_next_write_) { fail_next_write_ = false; return -1; }
		if (mapping.identity == 1 && offset == 4) {
			manager_ctrl = value;
			if ((value & 5u) == 5u) manager_stat = (manager_stat & ~7u) | 1u;
			else if ((value & 5u) == 1u) manager_stat = (manager_stat & ~7u) | 2u;
		} else if (mapping.identity == 1 && offset == 8) {
			manager_dclk_status = 1;
			manager_stat = (manager_stat & ~7u) | (value == 4 ? 3u : 4u);
		} else if (mapping.identity == 1 && offset == 0x0c) {
			manager_dclk_status = 0;
		} else if (mapping.identity == 6) {
			programmed_words.push_back(value);
		} else Register(mapping.identity) = value;
		return 0;
	}
	int OrderingBarrier() override { ++event_count; return 0; }
	bool AnyHeld() const
	{
		for (size_t index = 1; index != 7; ++index) if (held[index]) return true;
		return false;
	}
	void FailNextWrite() { fail_next_write_ = true; }
	void FailNextUnmap() { fail_next_unmap_ = true; }
	void ConsumeDeadlineAt(TestClock &clock, uint64_t absolute_deadline_ms)
		{ deadline_clock_ = &clock; deadline_ms_ = absolute_deadline_ms; }
	void SetRetainedCapture(GraphRetainedCapture *capture) { capture_ = capture; }
	void SetFinalEvidence(bool *descriptor_open, bool *mappings_held)
		{ final_descriptor_open_ = descriptor_open;
		  final_mappings_held_ = mappings_held; }
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
		assert(identity == 5); return remap;
	}
	bool descriptor_open;
	bool held[7];
	uint32_t core, interface_module, sdr, bridge, remap;
	uint32_t manager_stat, manager_ctrl, manager_gpio, manager_dclk_status;
	size_t event_count;
	std::vector<uint32_t> programmed_words;
private:
	void ConsumeDeadline()
	{
		if (deadline_clock_ != nullptr) {
			if (capture_ != nullptr) capture_->Capture();
			deadline_clock_->Set(deadline_ms_);
			deadline_clock_ = nullptr;
		}
	}
	bool fail_next_write_;
	bool fail_next_unmap_;
	TestClock *deadline_clock_ = nullptr;
	uint64_t deadline_ms_ = 0;
	GraphRetainedCapture *capture_ = nullptr;
	bool *final_descriptor_open_ = nullptr;
	bool *final_mappings_held_ = nullptr;
};

class GraphProtocolOperations final : public NativeCoreProtocolIoTestOperations {
public:
	GraphProtocolOperations()
		: gpo(0), gpi_index(0), maps(0), closes(0), descriptor_open(false),
		  fail_next_unmap(false) {}
	~GraphProtocolOperations() override
	{
		if (final_descriptor_open_ != nullptr)
			*final_descriptor_open_ = descriptor_open;
		if (final_mappings_ != nullptr) *final_mappings_ = maps;
	}
	size_t PageSize() const override { return 4096; }
	int Open(const char *, int) override { descriptor_open = true; return 9; }
	int Close(int descriptor) override
		{ assert(descriptor == 9 && descriptor_open); descriptor_open = false;
		  ++closes; return 0; }
	int Map(int descriptor, uint64_t page, size_t length,
		NativeCoreProtocolIoTestMapping *mapping) override
	{
		assert(descriptor == 9 && page == 0xff706000u && length == 4096);
		mapping->identity = 0x1000; ++maps; return 0;
	}
	int Unmap(const NativeCoreProtocolIoTestMapping &mapping, size_t length) override
	{
		assert(mapping.identity == 0x1000 && length == 4096);
		if (fail_next_unmap) { fail_next_unmap = false; return -1; }
		--maps; return 0;
	}
	int Read32(const NativeCoreProtocolIoTestMapping &mapping, size_t offset,
		uint32_t *value) override
	{
		ConsumeDeadline();
		assert(mapping.identity == 0x1000 && value != nullptr);
		if (offset == 0x10) { *value = gpo; return 0; }
		if (offset != 0x14 || gpi_index >= gpi.size()) return -1;
		*value = gpi[gpi_index++]; return 0;
	}
	int Write32(const NativeCoreProtocolIoTestMapping &mapping, size_t offset,
		uint32_t value) override
	{
		ConsumeDeadline();
		assert(mapping.identity == 0x1000 && offset == 0x10);
		gpo = value; writes.push_back(value); return 0;
	}
	int OrderingBarrier() override { return 0; }
	void ScriptMegaDriveActivation()
	{
		gpi.push_back(0x5ca623a8u); gpi.push_back(0x00010000u);
		gpi.push_back(0x00080000u);
		std::vector<uint16_t> responses(11, 0); responses.push_back(0);
		const char *name = "MegaDrive";
		for (size_t index = 0; name[index] != '\0'; ++index)
			responses.push_back(static_cast<uint8_t>(name[index]));
		responses.push_back(';'); responses.insert(responses.end(), 10, 0);
		responses.push_back(0); responses.insert(responses.end(), 11, 0);
		for (uint16_t response : responses) {
			gpi.push_back(0x00020000u); gpi.push_back(response);
		}
	}
	void ScriptSnesActivation()
	{
		gpi.push_back(0x5ca623a4u); gpi.push_back(0x00000000u);
		gpi.push_back(0x00080000u);
		std::vector<uint16_t> responses(11, 0); responses.push_back(0);
		const char *name = "SNES";
		for (size_t index = 0; name[index] != '\0'; ++index)
			responses.push_back(static_cast<uint8_t>(name[index]));
		responses.push_back(';');
		responses.insert(responses.end(), 2 + 3 + 2 + 513 + 8 * 4097 + 1 +
			2 + 9, 0);
		for (uint16_t response : responses) {
			gpi.push_back(0x00020000u); gpi.push_back(response);
		}
	}
	void ScriptCleanupStop()
	{
		for (size_t index = 0; index != 2; ++index) {
			gpi.push_back(0x00020000u); gpi.push_back(0);
		}
	}
	void ConsumeDeadlineAt(TestClock &clock, uint64_t absolute_deadline_ms)
		{ deadline_clock_ = &clock; deadline_ms_ = absolute_deadline_ms; }
	void SetRetainedCapture(GraphRetainedCapture *capture) { capture_ = capture; }
	void SetFinalEvidence(bool *descriptor_open, int *mappings)
		{ final_descriptor_open_ = descriptor_open; final_mappings_ = mappings; }
	uint32_t gpo;
	std::vector<uint32_t> gpi;
	size_t gpi_index;
	int maps, closes;
	bool descriptor_open;
	bool fail_next_unmap;
	std::vector<uint32_t> writes;
	void ConsumeDeadline()
	{
		if (deadline_clock_ != nullptr) {
			if (capture_ != nullptr) capture_->Capture();
			deadline_clock_->Set(deadline_ms_);
			deadline_clock_ = nullptr;
		}
	}
	TestClock *deadline_clock_ = nullptr;
	uint64_t deadline_ms_ = 0;
	GraphRetainedCapture *capture_ = nullptr;
	bool *final_descriptor_open_ = nullptr;
	int *final_mappings_ = nullptr;
};

class GraphInputOperations final : public NativeLinuxInputOperations {
public:
	GraphInputOperations() : emitted(false), live_descriptors(0), close_calls(0) {}
	Result OpenDirectory(uint64_t, int *value) override
		{ *value = 10; ++live_descriptors; return MISTER_RESULT_OK; }
	Result OpenWatch(uint64_t, int *value) override
		{ *value = 11; ++live_descriptors; return MISTER_RESULT_OK; }
	Result AddWatch(int, uint64_t, int *value) override { *value = 12; return MISTER_RESULT_OK; }
	Result DiscoverDevice(size_t index, NativeLinuxInputCandidate *candidate) override
	{
		if (index != 0) return MISTER_RESULT_UNSUPPORTED;
		memset(candidate, 0, sizeof(*candidate));
		candidate->identity = 1;
		strcpy(candidate->path, "/dev/input/event-fixture");
		return MISTER_RESULT_OK;
	}
	Result OpenDevice(const NativeLinuxInputCandidate &, uint64_t, int *descriptor) override
		{ *descriptor = 20; ++live_descriptors; return MISTER_RESULT_OK; }
	Result QueryCapabilities(int, NativeLinuxInputCapabilities *capabilities) override
	{
		memset(capabilities, 0, sizeof(*capabilities));
		capabilities->digital_gamepad_buttons = true;
		return MISTER_RESULT_OK;
	}
	Result SetGrab(int, bool) override { return MISTER_RESULT_OK; }
	Result ReadEvents(int, NativeLinuxInputEvent *events, size_t capacity,
		size_t *count, bool *partial) override
	{
		*partial = false;
		if (emitted || capacity < 2) { *count = 0; return MISTER_RESULT_OK; }
		events[0] = {NativeLinuxInputEventKind::digital_button,
			NativeLinuxInputCode::south, 1, 1};
		events[1] = {NativeLinuxInputEventKind::syn_report, 0, 0, 1};
		*count = 2; emitted = true; return MISTER_RESULT_OK;
	}
	Result RemoveWatch(int, int) override { return MISTER_RESULT_OK; }
	Result Close(int) override
		{ assert(live_descriptors > 0); --live_descriptors; ++close_calls;
		  return MISTER_RESULT_OK; }
	uint64_t NowMs() const override { return 100; }
	bool emitted;
	int live_descriptors;
	int close_calls;
};

class GraphExecutionOperations final : public NativeLinuxExecutionOperations {
public:
	Result Initialize(uint64_t) override { ++initializes; return MISTER_RESULT_OK; }
	Result Release() override { ++releases; return MISTER_RESULT_OK; }
	uint64_t NowMs() const override { return 100; }
	int initializes = 0;
	int releases = 0;
};

enum class GraphFaultBoundary : uint8_t {
	none, preflight, content, mappings, offload, fpga, bridges, protocol,
	video, audio, audio_video, input_descriptors, save, scheduler,
	release_scheduler, release_offload, release_save, capture_input,
	replay_input, release_input_descriptors, release_video,
	release_audio_video, release_audio, release_protocol, release_content,
	terminal
};

struct GraphFaultPlan {
	GraphFaultBoundary boundary = GraphFaultBoundary::none;
	bool after = false;
	bool fired = false;
	NativeResourceLedger acquired = {};
	NativeResourceLedger partial_ledger = {};
	int av_begins_at_fault = -1;
	size_t av_words_at_fault = 0;
	bool Hit(GraphFaultBoundary value, bool after_ownership)
	{
		if (fired || boundary != value || after != after_ownership) return false;
		partial_ledger = acquired;
		fired = true;
		return true;
	}
	void Acquire(uint32_t flags, bool *supporting = nullptr)
	{
		acquired.resource_flags |= flags;
		if (supporting != nullptr) *supporting = true;
	}
};

enum class GraphOrdinaryDeadlineKind : uint8_t {
	none,
	scheduler,
	offload,
	capture_input,
	replay_input,
	input_descriptors,
	content
};

class GraphOrdinaryDeadlineConsumer {
public:
	explicit GraphOrdinaryDeadlineConsumer(TestClock &clock) : clock_(clock) {}
	void Arm(GraphOrdinaryDeadlineKind kind, uint64_t target)
	{
		kind_ = kind;
		target_ = target;
		armed_ = true;
		consumed_ = false;
		observed_effective_deadline_ = 0;
	}
	bool Consume(GraphOrdinaryDeadlineKind kind, uint64_t effective_deadline)
	{
		if (!armed_ || kind_ != kind) return false;
		observed_effective_deadline_ = effective_deadline;
		clock_.Set(target_);
		armed_ = false;
		consumed_ = true;
		return clock_.NowMs() >= effective_deadline;
	}
	bool consumed() const { return consumed_; }
	uint64_t observed_effective_deadline() const
		{ return observed_effective_deadline_; }
private:
	TestClock &clock_;
	GraphOrdinaryDeadlineKind kind_ = GraphOrdinaryDeadlineKind::none;
	uint64_t target_ = 0;
	bool armed_ = false;
	bool consumed_ = false;
	uint64_t observed_effective_deadline_ = 0;
};

class GraphAvOperations final : public NativeAvIoTestOperations {
public:
	GraphAvOperations() : fault_(nullptr), operation_(GraphFaultBoundary::none) {}
	explicit GraphAvOperations(GraphFaultPlan &fault)
		: fault_(&fault), operation_(GraphFaultBoundary::none) {}
	void SetOperation(GraphFaultBoundary operation) { operation_ = operation; }
	void ConsumeDeadlineAt(GraphFaultBoundary operation, TestClock &clock,
		uint64_t absolute_deadline_ms)
	{
		deadline_operation_ = operation;
		deadline_clock_ = &clock;
		deadline_ms_ = absolute_deadline_ms;
	}
	void ConsumeAnyDeadlineAt(TestClock &clock, uint64_t absolute_deadline_ms)
	{
		deadline_operation_ = GraphFaultBoundary::none;
		deadline_clock_ = &clock;
		deadline_ms_ = absolute_deadline_ms;
		consume_any_deadline_ = true;
	}
	void SetRetainedCapture(GraphRetainedCapture *capture) { capture_ = capture; }
	Result Begin(uint64_t) override
	{
		if (deadline_clock_ != nullptr &&
			(consume_any_deadline_ || deadline_operation_ == operation_)) {
			if (capture_ != nullptr) capture_->Capture();
			deadline_clock_->Set(deadline_ms_);
			deadline_clock_ = nullptr;
			consume_any_deadline_ = false;
		}
		if (fault_ != nullptr && !fault_->fired && !fault_->after &&
			fault_->boundary == operation_) {
			fault_->av_begins_at_fault = begins;
			fault_->av_words_at_fault = words.size();
		}
		if (fault_ != nullptr && fault_->Hit(operation_, false))
			return MISTER_RESULT_PLATFORM;
		++begins; return MISTER_RESULT_OK;
	}
	Result SendWord(uint16_t word, uint16_t *ack, uint64_t) override
	{
		words.push_back(word);
		if (ack != nullptr) *ack = 0;
		if (fault_ != nullptr && !fault_->fired && fault_->after &&
			fault_->boundary == operation_) {
			if (operation_ == GraphFaultBoundary::video)
				fault_->Acquire(MISTER_RESOURCE_NATIVE_VIDEO);
			else if (operation_ == GraphFaultBoundary::audio)
				fault_->Acquire(MISTER_RESOURCE_NATIVE_AUDIO);
			else if (operation_ == GraphFaultBoundary::audio_video) {
				fault_->Acquire(MISTER_RESOURCE_NATIVE_AUDIO |
					MISTER_RESOURCE_NATIVE_VIDEO);
				fault_->acquired.coupled_audio_video_active = true;
			}
		}
		if (fault_ != nullptr && !fault_->fired && fault_->after &&
			fault_->boundary == operation_) {
			fault_->av_begins_at_fault = begins;
			fault_->av_words_at_fault = words.size();
		}
		return fault_ != nullptr && fault_->Hit(operation_, true) ?
			MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}
	Result Finish(uint64_t) override
	{
		++finishes;
		return MISTER_RESULT_OK;
	}
	int begins = 0;
	int finishes = 0;
	std::vector<uint16_t> words;
private:
	GraphFaultPlan *fault_;
	GraphFaultBoundary operation_;
	GraphFaultBoundary deadline_operation_ = GraphFaultBoundary::none;
	TestClock *deadline_clock_ = nullptr;
	uint64_t deadline_ms_ = 0;
	bool consume_any_deadline_ = false;
	GraphRetainedCapture *capture_ = nullptr;
};

class GraphSaveFileSystem final : public NativeSaveFileSystem {
public:
	explicit GraphSaveFileSystem(TestClock *clock = nullptr, bool exists = false)
		: clock_(clock), file_exists(exists), live_descriptors(0), close_calls(0),
		  fdatasync_calls(0), fsync_calls(0)
		{ memset(descriptor_refs, 0, sizeof(descriptor_refs)); }
	~GraphSaveFileSystem() override
		{ if (final_descriptors_ != nullptr) *final_descriptors_ = live_descriptors; }
	uint64_t NowMs() const override { return clock_ == nullptr ? 100 : clock_->NowMs(); }
	void ConsumeDeadlineAt(uint64_t absolute_deadline_ms)
		{ deadline_ms_ = absolute_deadline_ms; consume_deadline_ = true; }
	void SetRetainedCapture(GraphRetainedCapture *capture) { capture_ = capture; }
	void SetFinalEvidence(int *descriptors) { final_descriptors_ = descriptors; }
	NativeSaveOpenResult OpenAt(int parent, const char *name, int flags,
		mode_t) override
	{
		if (parent == AT_FDCWD && strcmp(name, "/") == 0) return Open(10);
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0) return Open(11);
		if (parent == 11 && strcmp(name, "saves") == 0) return Open(12);
		if (parent == 12 &&
			(strcmp(name, "snes") == 0 || strcmp(name, "megadrive") == 0))
			return Open(13);
		if (parent == 13 && strstr(name, ".sav") != nullptr) {
			if ((flags & O_CREAT) == 0 && !file_exists) return {-1, ENOENT};
			file_exists = true; return Open(14);
		}
		return {-1, EINVAL};
	}
	int Stat(int descriptor, struct stat *info) override
	{
		return Fill(descriptor, info);
	}
	int StatAt(int parent, const char *name, struct stat *info, int) override
	{
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0) return Fill(11, info);
		if (parent == 11 && strcmp(name, "saves") == 0) return Fill(12, info);
		if (parent == 12 &&
			(strcmp(name, "snes") == 0 || strcmp(name, "megadrive") == 0))
			return Fill(13, info);
		if (parent == 13 && strstr(name, ".sav") != nullptr && file_exists)
			return Fill(14, info);
		return -1;
	}
	Result MountId(int descriptor, uint64_t *mount) override
	{
		if (descriptor < 10 || descriptor > 14 || mount == nullptr)
			return MISTER_RESULT_PLATFORM;
		*mount = 1; return MISTER_RESULT_OK;
	}
	int Fdatasync(int descriptor) override
	{
		++fdatasync_calls;
		if (consume_deadline_ && clock_ != nullptr) {
			if (capture_ != nullptr) capture_->Capture();
			clock_->Set(deadline_ms_);
			consume_deadline_ = false;
		}
		return descriptor == 14 ? 0 : -1;
	}
	int Fsync(int descriptor) override
		{ ++fsync_calls; return descriptor == 13 || descriptor == 14 ? 0 : -1; }
	int Close(int descriptor) override
	{
		if (descriptor < 10 || descriptor > 14 || descriptor_refs[descriptor] == 0)
			return -1;
		--descriptor_refs[descriptor]; --live_descriptors; ++close_calls;
		return 0;
	}
	int Fill(int descriptor, struct stat *info) const
	{
		if (descriptor < 10 || descriptor > 14 || info == nullptr) return -1;
		memset(info, 0, sizeof(*info));
		info->st_dev = 1; info->st_ino = descriptor; info->st_nlink = 1;
		info->st_uid = descriptor <= 11 ? 0 : 1000;
		info->st_gid = descriptor <= 11 ? 0 : 1000;
		info->st_mode = descriptor == 14 ? (S_IFREG | 0600) :
			(descriptor <= 11 ? (S_IFDIR | 0755) : (S_IFDIR | 0700));
		info->st_size = 0; return 0;
	}
	TestClock *clock_;
	bool file_exists;
	int descriptor_refs[15];
	int live_descriptors;
	int close_calls;
	int fdatasync_calls;
	int fsync_calls;
	bool consume_deadline_ = false;
	uint64_t deadline_ms_ = 0;
	GraphRetainedCapture *capture_ = nullptr;
	int *final_descriptors_ = nullptr;
	NativeSaveOpenResult Open(int descriptor)
	{
		++descriptor_refs[descriptor]; ++live_descriptors;
		return {descriptor, 0};
	}
};

static std::string GraphJoin(const std::string &left, const std::string &right)
{
	return left + "/" + right;
}

class GraphProcessIsolationLock final {
public:
	GraphProcessIsolationLock()
		: descriptor_(open("/tmp/fogcast-native-linux-v2-context-test.lock",
			O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600))
	{
		assert(descriptor_ >= 0);
		struct stat info = {};
		assert(fstat(descriptor_, &info) == 0);
		assert(S_ISREG(info.st_mode));
		assert(info.st_uid == getuid());
		assert(info.st_nlink == 1);
		assert((info.st_mode & 0077) == 0);
		int result = 0;
		do {
			result = flock(descriptor_, LOCK_EX);
		} while (result != 0 && errno == EINTR);
		assert(result == 0);
	}
	~GraphProcessIsolationLock()
	{
		assert(flock(descriptor_, LOCK_UN) == 0);
		assert(close(descriptor_) == 0);
	}
	GraphProcessIsolationLock(const GraphProcessIsolationLock &) = delete;
	GraphProcessIsolationLock &operator=(const GraphProcessIsolationLock &) = delete;
private:
	int descriptor_;
};

class GraphRoot final {
public:
	GraphRoot(const std::string &system, const std::string &extension,
		const std::string &content_digest, uint64_t content_size)
		: system_(system), extension_(extension), root_(), names_()
	{
		char path[] = ".fogcast-v2-graph.XXXXXX";
		char *created = mkdtemp(path);
		assert(created != nullptr);
		char resolved[4096] = {};
		assert(realpath(created, resolved) != nullptr);
		root_ = resolved;
		assert(mkdir(GraphJoin(root_, system_).c_str(), 0700) == 0);
		const char *core_digest =
			"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
		names_.push_back(content_digest + "." + extension_);
		names_.push_back(std::string(core_digest) + ".rbf");
		std::vector<uint8_t> content_bytes(static_cast<size_t>(content_size), 'a');
		if (system_ == "snes" && content_size == 0x8000) {
			content_bytes.assign(0x8000, 0);
			content_bytes[0] = 0x78;
			content_bytes[0x7fc0 + 0x15] = 0x20;
			content_bytes[0x7fc0 + 0x1a] = 0x33;
			content_bytes[0x7fc0 + 0x1c] = 0xff;
			content_bytes[0x7fc0 + 0x1d] = 0xff;
			content_bytes[0x7fc0 + 0x3c] = 0x00;
			content_bytes[0x7fc0 + 0x3d] = 0x80;
		}
		if (content_size == 5) content_bytes.assign({'h', 'e', 'l', 'l', 'o'});
		const std::vector<uint8_t> core_bytes = {'h', 'e', 'l', 'l', 'o'};
		for (size_t index = 0; index != names_.size(); ++index) {
			const std::string &name = names_[index];
			const int descriptor = open(GraphJoin(GraphJoin(root_, system_), name).c_str(),
				O_WRONLY | O_CREAT | O_EXCL, 0600);
			assert(descriptor >= 0);
			const std::vector<uint8_t> &bytes = index == 0 ? content_bytes : core_bytes;
			assert(write(descriptor, bytes.data(), bytes.size()) ==
				static_cast<ssize_t>(bytes.size()));
			assert(close(descriptor) == 0);
		}
	}
	~GraphRoot()
	{
		for (const std::string &name : names_)
			assert(unlink(GraphJoin(GraphJoin(root_, system_), name).c_str()) == 0);
		assert(rmdir(GraphJoin(root_, system_).c_str()) == 0);
		assert(rmdir(root_.c_str()) == 0);
	}
	const char *path() const { return root_.c_str(); }
private:
	std::string system_, extension_, root_;
	std::vector<std::string> names_;
};

static Result GraphArtifactResult(NativeArtifactResult result)
{
	if (result == NativeArtifactResult::ok) return MISTER_RESULT_OK;
	if (result == NativeArtifactResult::deadline) return MISTER_RESULT_DEADLINE;
	if (result == NativeArtifactResult::cleanup_incomplete)
		return MISTER_RESULT_CLEANUP_INCOMPLETE;
	return MISTER_RESULT_PLATFORM;
}

class GraphLaunchAssets final : public NativeContentResource {
public:
	GraphLaunchAssets(const NativeCoreProfile &profile, const MisterLaunchV2 &launch,
		NativeContentAdapter &content, NativeCoreArtifactAdapter &core,
		GraphFaultPlan &fault, GraphOrdinaryDeadlineConsumer &deadline)
		: profile_(profile), content_(content), core_(core), core_handle_(),
		  authority_(), core_authority_(), core_retained_(false),
		  content_closed_after_fault_(false),
		  last_content_result(MISTER_RESULT_INVALID_STATE),
		  last_core_result(NativeArtifactResult::invalid_argument), fault_(fault),
		  deadline_(deadline)
	{
		system_ = GraphCopyView(launch.system);
		digest_ = GraphCopyView(launch.content.sha256);
		extension_ = GraphCopyView(launch.content.extension);
		authority_ = {system_.c_str(), system_.size(), digest_.c_str(), digest_.size(),
			launch.content.size, extension_.c_str(), extension_.size()};
		core_authority_ = authority_;
		core_authority_.sha256 =
			"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
		core_authority_.sha256_length = 64;
		core_authority_.size = 5;
		core_authority_.extension = "rbf";
		core_authority_.extension_length = 3;
		assert(content_.Configure(authority_) == MISTER_RESULT_OK);
	}
	NativeAcquisitionOutcome RetainContent(uint64_t deadline) override
	{
		if (fault_.Hit(GraphFaultBoundary::content, false))
			return {MISTER_RESULT_PLATFORM, false};
		NativeAcquisitionOutcome content = content_.RetainContent(deadline);
		last_content_result = content.result;
		if (content.result != MISTER_RESULT_OK) return content;
		const NativeArtifactResult core = core_.ResolveFixtureForTest(profile_,
			core_authority_, deadline, &core_handle_);
		last_core_result = core;
		core_retained_ = core_handle_.owns_descriptors();
		NativeAcquisitionOutcome outcome = {GraphArtifactResult(core),
			content.acquired || core_retained_};
		if (outcome.acquired) fault_.Acquire(MISTER_RESOURCE_CONTENT);
		if (fault_.Hit(GraphFaultBoundary::content, true))
			outcome.result = MISTER_RESULT_PLATFORM;
		return outcome;
	}
	Result DeriveSaveKey(const NativeCoreProfile &profile, NativeSaveKey *key) override
		{ return content_.DeriveSaveKey(profile, key); }
	Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t deadline) override
		{ return content_.DescribeRetained(description, deadline); }
	Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t deadline) override
		{ return content_.ReadRetainedAt(offset, bytes, count, deadline); }
	Result CloseContent(uint64_t deadline) override
	{
		if (content_closed_after_fault_) return MISTER_RESULT_OK;
		if (deadline_.Consume(GraphOrdinaryDeadlineKind::content, deadline))
			return MISTER_RESULT_DEADLINE;
		if (fault_.Hit(GraphFaultBoundary::release_content, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		Result primary = content_.CloseContent(deadline);
		const Result core = GraphArtifactResult(core_handle_.CloseRetainedBefore(deadline));
		core_retained_ = core_handle_.owns_descriptors();
		const Result result = primary != MISTER_RESULT_OK ? primary : core;
		if (fault_.Hit(GraphFaultBoundary::release_content, true)) {
			content_closed_after_fault_ = result == MISTER_RESULT_OK;
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return result;
	}
	void CloseContentForProcessExit() override
	{
		content_.CloseContentForProcessExit();
		(void)core_handle_.Close();
		core_retained_ = false;
	}
	const NativeCoreArtifactHandle &core_handle() const { return core_handle_; }
	bool core_retained() const { return core_retained_; }
	Result content_result() const { return last_content_result; }
	NativeArtifactResult core_result() const { return last_core_result; }
private:
	const NativeCoreProfile &profile_;
	NativeContentAdapter &content_;
	NativeCoreArtifactAdapter &core_;
	NativeCoreArtifactHandle core_handle_;
	std::string system_, digest_, extension_;
	NativeArtifactAuthority authority_, core_authority_;
	bool core_retained_;
	bool content_closed_after_fault_;
	Result last_content_result;
	NativeArtifactResult last_core_result;
	GraphFaultPlan &fault_;
	GraphOrdinaryDeadlineConsumer &deadline_;
};

class GraphPreflight final : public NativePreflight {
public:
	explicit GraphPreflight(GraphFaultPlan &fault) : fault_(fault) {}
	Result Validate(const NativeCoreProfile &, uint64_t) override
	{
		return fault_.Hit(GraphFaultBoundary::preflight, false) ||
			fault_.Hit(GraphFaultBoundary::preflight, true) ?
			MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}
private:
	GraphFaultPlan &fault_;
};

class GraphInputResource final : public NativeInputDescriptorResource {
public:
	GraphInputResource(NativeInputAdapter &adapter, GraphFaultPlan &fault,
		GraphOrdinaryDeadlineConsumer &deadline)
		: adapter_(adapter), fault_(fault), deadline_(deadline),
		  descriptors_closed_after_fault_(false) {}
	NativeAcquisitionOutcome OpenInputDescriptors(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::input_descriptors, false))
			return {MISTER_RESULT_PLATFORM, false};
		NativeAcquisitionOutcome outcome = adapter_.OpenInputDescriptors(lease);
		if (outcome.acquired)
			fault_.Acquire(MISTER_RESOURCE_CORE_INPUT,
				&fault_.acquired.input_descriptors);
		if (fault_.Hit(GraphFaultBoundary::input_descriptors, true))
			outcome.result = MISTER_RESULT_PLATFORM;
		return outcome;
	}
	Result CloseInputDescriptors(const OperationLease &lease) override
	{
		if (descriptors_closed_after_fault_) return MISTER_RESULT_OK;
		if (deadline_.Consume(GraphOrdinaryDeadlineKind::input_descriptors,
			lease.absolute_deadline_ms())) return MISTER_RESULT_DEADLINE;
		if (fault_.Hit(GraphFaultBoundary::release_input_descriptors, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result result = adapter_.CloseInputDescriptors(lease);
		if (fault_.Hit(GraphFaultBoundary::release_input_descriptors, true)) {
			descriptors_closed_after_fault_ = result == MISTER_RESULT_OK;
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return result;
	}
	void CloseInputDescriptorsForProcessExit() override
		{ adapter_.CloseInputDescriptorsForProcessExit(); }
	Result CaptureDigitalNeutral(const OperationLease &lease, NativeDigitalNeutral *values,
		bool *valid, size_t count) override
	{
		if (deadline_.Consume(GraphOrdinaryDeadlineKind::capture_input,
			lease.absolute_deadline_ms())) return MISTER_RESULT_DEADLINE;
		if (fault_.Hit(GraphFaultBoundary::capture_input, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result result = adapter_.CaptureDigitalNeutral(lease, values, valid, count);
		return fault_.Hit(GraphFaultBoundary::capture_input, true) ?
			MISTER_RESULT_CLEANUP_INCOMPLETE : result;
	}
private:
	NativeInputAdapter &adapter_;
	GraphFaultPlan &fault_;
	GraphOrdinaryDeadlineConsumer &deadline_;
	bool descriptors_closed_after_fault_;
};

class GraphHardware final : public NativeHardwareResources {
public:
	GraphHardware(NativeContainment &containment, NativeFpgaProgrammer &programmer,
		GraphLaunchAssets &assets, NativeCoreProtocol &protocol,
		NativeLinuxMmioAdapter &mmio, GraphMmioOperations &mmio_operations,
		NativeInput &input,
		const NativeCoreProfile &profile, GraphFaultPlan &fault,
		GraphOrdinaryDeadlineConsumer &deadline)
		: containment_(containment), programmer_(programmer), assets_(assets),
		  protocol_(protocol), mmio_(mmio), mmio_operations_(mmio_operations),
		  input_(input), profile_(profile),
		  programmed_same_handle(false), neutral_replay_count_(0),
		  neutral_replay_deadline_(0), replay_completed_after_fault_(false),
		  mapping_result(MISTER_RESULT_INVALID_STATE), mapping_acquired(false),
		  fault_(fault), deadline_(deadline) {}
	NativeAcquisitionOutcome AcquireContainmentMappings(
		const OperationLease &lease) override
	{
		fault_.acquired.generation = true;
		if (fault_.Hit(GraphFaultBoundary::mappings, false))
			return {MISTER_RESULT_PLATFORM, false};
		const NativeMappingAcquisitionReceipt receipt = containment_.AcquireMappings(lease);
		mapping_result = receipt.result;
		mapping_acquired = receipt.acquired;
		if (receipt.acquired)
			fault_.Acquire(MISTER_RESOURCE_FPGA,
				&fault_.acquired.containment_mappings);
		return {fault_.Hit(GraphFaultBoundary::mappings, true) ?
			MISTER_RESULT_PLATFORM : receipt.result, receipt.acquired};
	}
	NativeAcquisitionOutcome AcquireFpga(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::fpga, false))
			return {MISTER_RESULT_PLATFORM, false};
		programmed_same_handle = assets_.core_retained();
		const NativeFpgaProgrammingReceipt receipt = programmer_.Program(
			lease, assets_.core_handle());
		if (receipt.acquired) fault_.Acquire(MISTER_RESOURCE_FPGA);
		return {fault_.Hit(GraphFaultBoundary::fpga, true) ?
			MISTER_RESULT_PLATFORM : receipt.result, receipt.acquired};
	}
	NativeAcquisitionOutcome EnableBridges(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::bridges, false))
			return {MISTER_RESULT_PLATFORM, false};
		const NativeBridgeEnableReceipt receipt = containment_.EnableBridges(lease);
		if (receipt.acquired) fault_.Acquire(MISTER_RESOURCE_BRIDGES);
		return {fault_.Hit(GraphFaultBoundary::bridges, true) ?
			MISTER_RESULT_PLATFORM : receipt.result, receipt.acquired};
	}
	NativeCoreProtocolOutcome StartCoreProtocol(const OperationLease &lease,
		const NativeCoreProfile &profile, NativeContentResource &content) override
	{
		if (fault_.Hit(GraphFaultBoundary::protocol, false))
			return {MISTER_RESULT_PLATFORM, false, false};
		NativeCoreProtocolOutcome outcome = protocol_.Activate(lease, profile, content);
		if (outcome.acquired) fault_.Acquire(MISTER_RESOURCE_CORE_PROTOCOL);
		if (fault_.Hit(GraphFaultBoundary::protocol, true))
			outcome.result = MISTER_RESULT_PLATFORM;
		return outcome;
	}
	Result ReplayDigitalNeutral(const OperationLease &lease,
		const NativeDigitalNeutral &neutral) override
	{
		if (replay_completed_after_fault_) return MISTER_RESULT_OK;
		if (deadline_.Consume(GraphOrdinaryDeadlineKind::replay_input,
			lease.absolute_deadline_ms())) return MISTER_RESULT_DEADLINE;
		++neutral_replay_count_;
		neutral_replay_deadline_ = lease.absolute_deadline_ms();
		if (fault_.Hit(GraphFaultBoundary::replay_input, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		if (neutral.player >= kNativePlayerCount ||
			neutral.words[0] != profile_.input.player_command[neutral.player] ||
			neutral.words[1] != 0) return MISTER_RESULT_INVALID_ARGUMENT;
		SpiReceipt receipt = {};
		const Result result = input_.ReplayDigitalNeutral(&profile_, lease, neutral,
			&receipt);
		if (fault_.Hit(GraphFaultBoundary::replay_input, true)) {
			replay_completed_after_fault_ = result == MISTER_RESULT_OK;
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return result;
	}
	Result ShutdownCoreProtocol(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::release_protocol, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result result = protocol_.ShutdownLive(lease);
		return fault_.Hit(GraphFaultBoundary::release_protocol, true) ?
			MISTER_RESULT_CLEANUP_INCOMPLETE : result;
	}
	Result TerminalFpgaCleanup(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::terminal, false))
			mmio_operations_.FailNextWrite();
		else if (fault_.Hit(GraphFaultBoundary::terminal, true))
			mmio_operations_.FailNextUnmap();
		return containment_.ResetAndContain(lease);
	}
	void CloseCoreProtocolForProcessExit() override { protocol_.CloseForProcessExit(); }
	void CloseFpgaMappingsForProcessExit() override
		{ (void)mmio_.CloseMappingsForProcessExit(); }
	bool programmed_exact_handle() const { return programmed_same_handle; }
	size_t neutral_replay_count() const { return neutral_replay_count_; }
	uint64_t neutral_replay_deadline() const { return neutral_replay_deadline_; }
	Result last_mapping_result() const { return mapping_result; }
	bool last_mapping_acquired() const { return mapping_acquired; }
private:
	NativeContainment &containment_;
	NativeFpgaProgrammer &programmer_;
	GraphLaunchAssets &assets_;
	NativeCoreProtocol &protocol_;
	NativeLinuxMmioAdapter &mmio_;
	GraphMmioOperations &mmio_operations_;
	NativeInput &input_;
	const NativeCoreProfile &profile_;
	bool programmed_same_handle;
	size_t neutral_replay_count_;
	uint64_t neutral_replay_deadline_;
	bool replay_completed_after_fault_;
	Result mapping_result;
	bool mapping_acquired;
	GraphFaultPlan &fault_;
	GraphOrdinaryDeadlineConsumer &deadline_;
};

class GraphFaultResources final : public NativeSchedulerResource, public NativeOffloadResource,
	public NativeSaveResource {
public:
	GraphFaultResources(NativeSchedulerResource &scheduler,
		NativeOffloadResource &offload, NativeSaveResource &save,
		GraphFaultPlan &fault, GraphOrdinaryDeadlineConsumer &deadline)
		: scheduler_(scheduler), offload_(offload), save_(save), fault_(fault),
		  deadline_(deadline),
		  scheduler_closed_after_fault_(false), offload_closed_after_fault_(false),
		  save_closed_after_fault_(false) {}
	NativeAcquisitionOutcome StartScheduler(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::scheduler, false))
			return {MISTER_RESULT_PLATFORM, false};
		NativeAcquisitionOutcome out = scheduler_.StartScheduler(lease);
		if (out.acquired) fault_.Acquire(0, &fault_.acquired.scheduler);
		if (fault_.Hit(GraphFaultBoundary::scheduler, true)) out.result = MISTER_RESULT_PLATFORM;
		return out;
	}
	Result StopScheduler(const OperationLease &lease) override
	{
		if (scheduler_closed_after_fault_) return MISTER_RESULT_OK;
		if (deadline_.Consume(GraphOrdinaryDeadlineKind::scheduler,
			lease.absolute_deadline_ms())) return MISTER_RESULT_DEADLINE;
		if (fault_.Hit(GraphFaultBoundary::release_scheduler, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result out = scheduler_.StopScheduler(lease);
		if (fault_.Hit(GraphFaultBoundary::release_scheduler, true)) {
			scheduler_closed_after_fault_ = out == MISTER_RESULT_OK;
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return out;
	}
	void CloseSchedulerForProcessExit() override
		{ scheduler_.CloseSchedulerForProcessExit(); }
	NativeAcquisitionOutcome StartOffload(const OperationLease &lease) override
	{
		if (fault_.Hit(GraphFaultBoundary::offload, false))
			return {MISTER_RESULT_PLATFORM, false};
		NativeAcquisitionOutcome out = offload_.StartOffload(lease);
		if (out.acquired) fault_.Acquire(0, &fault_.acquired.offload);
		if (fault_.Hit(GraphFaultBoundary::offload, true)) out.result = MISTER_RESULT_PLATFORM;
		return out;
	}
	Result RejectAndJoinOffload(const OperationLease &lease) override
	{
		if (offload_closed_after_fault_) return MISTER_RESULT_OK;
		if (deadline_.Consume(GraphOrdinaryDeadlineKind::offload,
			lease.absolute_deadline_ms())) return MISTER_RESULT_DEADLINE;
		if (fault_.Hit(GraphFaultBoundary::release_offload, false))
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const Result out = offload_.RejectAndJoinOffload(lease);
		if (fault_.Hit(GraphFaultBoundary::release_offload, true)) {
			offload_closed_after_fault_ = out == MISTER_RESULT_OK;
			return MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return out;
	}
	void CloseOffloadForProcessExit() override { offload_.CloseOffloadForProcessExit(); }
	NativeSaveOpenOutcome OpenSave(const OperationLease &lease,
		const NativeCoreProfile &profile, const NativeSaveKey &key) override
	{
		if (fault_.Hit(GraphFaultBoundary::save, false))
			return {MISTER_RESULT_PLATFORM, false};
		NativeSaveOpenOutcome out = save_.OpenSave(lease, profile, key);
		if (out.acquired) fault_.Acquire(MISTER_RESOURCE_SAVES);
		if (fault_.Hit(GraphFaultBoundary::save, true)) out.result = MISTER_RESULT_PLATFORM;
		return out;
	}
	NativeSaveCloseOutcome FlushAndCloseSave(const OperationLease &lease) override
	{
		if (save_closed_after_fault_)
			return {MISTER_RESULT_OK, true, true, true, false};
		if (fault_.Hit(GraphFaultBoundary::release_save, false))
			return {MISTER_RESULT_CLEANUP_INCOMPLETE, false, false, false, false};
		NativeSaveCloseOutcome out = save_.FlushAndCloseSave(lease);
		if (fault_.Hit(GraphFaultBoundary::release_save, true)) {
			save_closed_after_fault_ = out.result == MISTER_RESULT_OK;
			out.result = MISTER_RESULT_CLEANUP_INCOMPLETE;
		}
		return out;
	}
	NativeSaveCloseOutcome RecoverSave(const OperationLease &lease,
		const SafeSaveRecoveryRecord &record) override
		{ return save_.RecoverSave(lease, record); }
	void CloseSaveForProcessExit() override { save_.CloseSaveForProcessExit(); }
private:
	NativeSchedulerResource &scheduler_;
	NativeOffloadResource &offload_; NativeSaveResource &save_; GraphFaultPlan &fault_;
	GraphOrdinaryDeadlineConsumer &deadline_;
	bool scheduler_closed_after_fault_;
	bool offload_closed_after_fault_;
	bool save_closed_after_fault_;
};

class GraphFaultAudio final : public NativeAudioResource {
public:
	GraphFaultAudio(NativeAudioResource &value, GraphAvOperations &operations,
		GraphFaultPlan &fault)
		: value_(value), operations_(operations), fault_(fault) {}
	PeripheralBackendIdentity BackendIdentity() const override { return value_.BackendIdentity(); }
	NativePeripheralAcquisitionOutcome StartAudio(std::unique_ptr<ActiveAudioSessionBundle> &&v) override
	{
		operations_.SetOperation(GraphFaultBoundary::audio);
		const NativePeripheralAcquisitionOutcome outcome =
			value_.StartAudio(std::move(v));
		if (outcome.acquired) fault_.Acquire(MISTER_RESOURCE_NATIVE_AUDIO);
		return outcome;
	}
	NativePeripheralReleaseOutcome StopAudio(std::unique_ptr<CleanupAudioSessionBundle> &&v) override
	{
		operations_.SetOperation(GraphFaultBoundary::release_audio);
		return value_.StopAudio(std::move(v));
	}
	NativePeripheralReleaseOutcome RecoverAudio(std::unique_ptr<RecoveryAudioSessionBundle> &&v) override
		{ return value_.RecoverAudio(std::move(v)); }
	void CloseAudioForProcessExit() override { value_.CloseAudioForProcessExit(); }
private: NativeAudioResource &value_; GraphAvOperations &operations_;
	GraphFaultPlan &fault_;
};

class GraphFaultVideo final : public NativeVideoResource {
public:
	GraphFaultVideo(NativeVideoResource &value, GraphAvOperations &operations,
		GraphFaultPlan &fault)
		: value_(value), operations_(operations), fault_(fault) {}
	PeripheralBackendIdentity BackendIdentity() const override { return value_.BackendIdentity(); }
	NativePeripheralAcquisitionOutcome StartVideo(std::unique_ptr<ActiveVideoSessionBundle> &&v) override
	{
		operations_.SetOperation(GraphFaultBoundary::video);
		const NativePeripheralAcquisitionOutcome outcome =
			value_.StartVideo(std::move(v));
		if (outcome.acquired) fault_.Acquire(MISTER_RESOURCE_NATIVE_VIDEO);
		return outcome;
	}
	NativePeripheralReleaseOutcome StopVideo(std::unique_ptr<CleanupVideoSessionBundle> &&v) override
	{
		operations_.SetOperation(GraphFaultBoundary::release_video);
		return value_.StopVideo(std::move(v));
	}
	NativePeripheralReleaseOutcome RecoverVideo(std::unique_ptr<RecoveryVideoSessionBundle> &&v) override
		{ return value_.RecoverVideo(std::move(v)); }
	void CloseVideoForProcessExit() override { value_.CloseVideoForProcessExit(); }
private: NativeVideoResource &value_; GraphAvOperations &operations_;
	GraphFaultPlan &fault_;
};

class GraphFaultAudioVideo final : public NativeAudioVideoResource {
public:
	GraphFaultAudioVideo(NativeAudioVideoResource &value,
		GraphAvOperations &operations, GraphFaultPlan &fault)
		: value_(value), operations_(operations), fault_(fault) {}
	PeripheralBackendIdentity BackendIdentity() const override { return value_.BackendIdentity(); }
	NativeCoupledAcquisitionOutcome StartAudioVideo(std::unique_ptr<ActiveAudioVideoSessionBundle> &&v) override
	{
		operations_.SetOperation(GraphFaultBoundary::audio_video);
		const NativeCoupledAcquisitionOutcome outcome =
			value_.StartAudioVideo(std::move(v));
		if (outcome.acquired) {
			fault_.Acquire(MISTER_RESOURCE_NATIVE_AUDIO |
				MISTER_RESOURCE_NATIVE_VIDEO);
			fault_.acquired.coupled_audio_video_active = true;
		}
		return outcome;
	}
	NativeCoupledReleaseOutcome StopAudioVideo(std::unique_ptr<CleanupAudioVideoSessionBundle> &&v) override
	{
		operations_.SetOperation(GraphFaultBoundary::release_audio_video);
		return value_.StopAudioVideo(std::move(v));
	}
	NativeCoupledReleaseOutcome RecoverAudioVideo(std::unique_ptr<RecoveryAudioVideoSessionBundle> &&v) override
		{ return value_.RecoverAudioVideo(std::move(v)); }
	void CloseAudioVideoForProcessExit() override { value_.CloseAudioVideoForProcessExit(); }
private: NativeAudioVideoResource &value_; GraphAvOperations &operations_;
	GraphFaultPlan &fault_;
};

struct GraphFinalBaseline {
	bool destroyed = false;
	bool mappings_held = true;
	bool mmio_descriptor_open = true;
	bool content_descriptors_held = true;
	bool core_descriptors_held = true;
	bool protocol_descriptor_open = true;
	int protocol_mappings = -1;
	int input_descriptors = -1;
	int input_close_calls = -1;
	int save_descriptors = -1;
	int save_close_calls = -1;
	int scheduler_initializes = -1;
	int scheduler_releases = -1;
	int offload_initializes = -1;
	int offload_releases = -1;
	size_t neutral_replays = 0;
	NativeResourceLedger ledger = {};
	NativeLifecycleState lifecycle_state = NativeLifecycleState::active;
	PlatformGenerationId lifecycle_generation = 1;
	NativeCleanupBrokerSnapshot broker = {};
	bool retained_live_snapshot_captured = false;
	NativeRetainedOperationSnapshot retained_live_snapshot = {};
	MisterResult content_result = MISTER_RESULT_INVALID_STATE;
	NativeArtifactResult core_result = NativeArtifactResult::invalid_argument;
	MisterResult activation_result = MISTER_RESULT_INVALID_STATE;
	MisterResult activate_return = MISTER_RESULT_INVALID_STATE;
	bool idle_return = false;
	int tick_calls = -1;
	bool broker_exact_idle = false;
	bool broker_has_live_generation = true;
	bool ordinary_deadline_consumed = false;
	uint64_t ordinary_effective_deadline = 0;
};

struct GraphTypedResourceEvidence {
	int av_begins;
	int av_finishes;
	size_t av_words;
	int save_fdatasync_calls;
	int save_fsync_calls;
	int save_close_calls;
	int save_descriptors;
	int protocol_maps;
	int protocol_closes;
	size_t protocol_writes;
};

static void AssertSameGraphTypedResourceEvidence(
	const GraphTypedResourceEvidence &left,
	const GraphTypedResourceEvidence &right)
{
	assert(left.av_begins == right.av_begins);
	assert(left.av_finishes == right.av_finishes);
	assert(left.av_words == right.av_words);
	assert(left.save_fdatasync_calls == right.save_fdatasync_calls);
	assert(left.save_fsync_calls == right.save_fsync_calls);
	assert(left.save_close_calls == right.save_close_calls);
	assert(left.save_descriptors == right.save_descriptors);
	assert(left.protocol_maps == right.protocol_maps);
	assert(left.protocol_closes == right.protocol_closes);
	assert(left.protocol_writes == right.protocol_writes);
}

class GraphGeneration final : public linux_native::NativeLinuxV2Generation {
public:
	GraphGeneration(TestClock &clock, const NativeCoreProfile &profile,
		const MisterLaunchV2 &launch, GraphGeneration **owner,
		GraphFinalBaseline *final_baseline, bool defer_cleanup_stop,
		const GraphFaultPlan &fault)
		: game_(GraphCopyView(launch.game_id)), system_(GraphCopyView(launch.system)),
		  extension_(GraphCopyView(launch.content.extension)), profile_(&profile), fault_(fault),
		  ordinary_deadline_(clock),
		  root_(system_, extension_, GraphCopyView(launch.content.sha256),
			launch.content.size), filesystem_(),
		  content_(root_.path(), filesystem_), core_(root_.path(), filesystem_),
		  assets_(profile, launch, content_, core_, fault_, ordinary_deadline_), clock_(clock), broker_(clock_),
		  mmio_operations_(), mmio_(mmio_operations_), containment_(broker_, mmio_),
		  programmer_(broker_, clock_, mmio_), protocol_operations_(),
		  protocol_io_(clock_, protocol_operations_),
		  protocol_(clock_, broker_, protocol_io_.capabilities()),
		  spi_(clock_, mmio_.user_io_only_hardware()), input_(spi_.input_port()),
		  input_operations_(), input_adapter_(broker_, clock_, profile, input_,
			input_operations_), input_resource_(input_adapter_, fault_, ordinary_deadline_),
		  av_operations_(fault_), av_io_(clock_, av_operations_),
		  audio_(clock_, broker_, av_io_), video_(clock_, broker_, av_io_),
		  save_filesystem_(&clock), save_(broker_, save_filesystem_),
		  scheduler_operations_(), offload_operations_(),
		  scheduler_(broker_, clock_, scheduler_operations_),
		  offload_(broker_, clock_, offload_operations_),
		  fault_audio_(audio_, av_operations_, fault_),
		  fault_video_(video_, av_operations_, fault_),
		  fault_audio_video_(video_, av_operations_, fault_),
		  fault_process_(scheduler_, offload_, save_, fault_, ordinary_deadline_), preflight_(fault_),
		  hardware_(containment_, programmer_, assets_, protocol_, mmio_,
			mmio_operations_, input_,
			profile, fault_, ordinary_deadline_),
		  resources_{preflight_, hardware_, fault_audio_, fault_video_,
			fault_audio_video_, fault_process_, fault_process_, fault_process_, assets_,
			input_resource_},
		  lifecycle_(clock_, broker_, resources_), tick_calls_(0), owner_(owner),
		  final_baseline_(final_baseline),
		  tick_deadline_(0), nested_deadline_(0),
		  allow_idle_(false)
	{
		retained_capture_.lifecycle = &lifecycle_;
		mmio_operations_.SetRetainedCapture(&retained_capture_);
		protocol_operations_.SetRetainedCapture(&retained_capture_);
		av_operations_.SetRetainedCapture(&retained_capture_);
		save_filesystem_.SetRetainedCapture(&retained_capture_);
		if (strcmp(profile.system, "snes") == 0)
			protocol_operations_.ScriptSnesActivation();
		else
			protocol_operations_.ScriptMegaDriveActivation();
		if (!defer_cleanup_stop) protocol_operations_.ScriptCleanupStop();
	}
	~GraphGeneration() override
	{
		if (final_baseline_ != nullptr) {
			final_baseline_->destroyed = true;
			final_baseline_->mappings_held = mmio_operations_.AnyHeld();
			final_baseline_->mmio_descriptor_open = mmio_operations_.descriptor_open;
			final_baseline_->content_descriptors_held = content_.owns_descriptors();
			final_baseline_->core_descriptors_held = assets_.core_handle().owns_descriptors();
			final_baseline_->protocol_descriptor_open = protocol_operations_.descriptor_open;
			final_baseline_->protocol_mappings = protocol_operations_.maps;
			final_baseline_->input_descriptors = input_operations_.live_descriptors;
			final_baseline_->input_close_calls = input_operations_.close_calls;
			final_baseline_->save_descriptors = save_filesystem_.live_descriptors;
			final_baseline_->save_close_calls = save_filesystem_.close_calls;
			final_baseline_->scheduler_initializes = scheduler_operations_.initializes;
			final_baseline_->scheduler_releases = scheduler_operations_.releases;
			final_baseline_->offload_initializes = offload_operations_.initializes;
			final_baseline_->offload_releases = offload_operations_.releases;
			final_baseline_->neutral_replays = hardware_.neutral_replay_count();
			final_baseline_->ledger = lifecycle_.ledger();
			final_baseline_->lifecycle_state = lifecycle_.state();
			final_baseline_->lifecycle_generation = lifecycle_.generation();
			final_baseline_->broker = lifecycle_.cleanup_broker_snapshot_for_test(
				PeripheralSessionKind::audio_video);
			final_baseline_->retained_live_snapshot_captured = retained_capture_.captured;
			final_baseline_->retained_live_snapshot = retained_capture_.snapshot;
			final_baseline_->content_result = assets_.content_result();
			final_baseline_->core_result = assets_.core_result();
			final_baseline_->activation_result = lifecycle_.latched_activation_result();
			final_baseline_->activate_return = activate_return_;
			final_baseline_->idle_return = idle_return_;
			final_baseline_->tick_calls = tick_calls_;
			final_baseline_->broker_exact_idle = broker_.exact_idle_for_test();
			final_baseline_->broker_has_live_generation =
				broker_.has_live_generation_for_test();
			final_baseline_->ordinary_deadline_consumed =
				ordinary_deadline_.consumed();
			final_baseline_->ordinary_effective_deadline =
				ordinary_deadline_.observed_effective_deadline();
		}
		if (owner_ != nullptr) *owner_ = nullptr;
	}
	MisterResult Activate(uint64_t deadline) override
	{
		const MisterResult result = lifecycle_.ActivateFixtureForTest(*profile_, deadline);
		activate_return_ = result;
		activation_ledger_ = lifecycle_.ledger();
		activation_av_begins_ = av_operations_.begins;
		activation_av_words_ = av_operations_.words.size();
		return result;
	}
	MisterResult Tick(uint64_t deadline) override
	{
		tick_deadline_ = deadline;
		std::unique_ptr<OperationLease> lease;
		Result result = broker_.Begin(lifecycle_.generation(), OperationKind::scheduler,
			deadline, &lease);
		if (result == MISTER_RESULT_OK)
			result = scheduler_.Dispatch(*lease, &DispatchInput, this);
		lease.reset();
		++tick_calls_;
		if (result != MISTER_RESULT_OK) (void)broker_.LatchFailure(lifecycle_.generation());
		return result;
	}
	MisterResult Observe(MisterObservationV2 *observation,
		uint64_t) const override
	{
		const NativeResourceLedger ledger = lifecycle_.ledger();
		observation->ready = lifecycle_.state() == NativeLifecycleState::active &&
			ledger.resource_flags == MISTER_RESOURCE_V2_KNOWN;
		observation->resource_flags = ledger.resource_flags;
		return MISTER_RESULT_OK;
	}
	MisterResult Stop(uint64_t deadline) override
	{
		stop_deadline_ = deadline;
		const MisterResult result = lifecycle_.Stop(deadline);
		allow_idle_ = lifecycle_.state() == NativeLifecycleState::idle;
		return result;
	}
	bool idle() const override
	{
		idle_return_ = allow_idle_ && lifecycle_.state() == NativeLifecycleState::idle;
		return idle_return_;
	}
	bool programmed_same_handle() const { return hardware_.programmed_exact_handle(); }
	bool mappings_held() const { return mmio_operations_.AnyHeld(); }
	int tick_calls() const { return tick_calls_; }
	bool tick_deadline_propagated() const
		{ return tick_deadline_ != 0 && nested_deadline_ == tick_deadline_; }
	size_t neutral_replay_count() const { return hardware_.neutral_replay_count(); }
	uint64_t neutral_replay_deadline() const
		{ return hardware_.neutral_replay_deadline(); }
	uint64_t stop_deadline() const { return stop_deadline_; }
	bool fault_fired() const { return fault_.fired; }
	NativeResourceLedger fault_ledger() const { return fault_.partial_ledger; }
	NativeResourceLedger ledger() const { return lifecycle_.ledger(); }
	NativeResourceLedger activation_ledger() const { return activation_ledger_; }
	int activation_av_begins() const { return activation_av_begins_; }
	size_t activation_av_words() const { return activation_av_words_; }
	int fault_av_begins() const { return fault_.av_begins_at_fault; }
	size_t fault_av_words() const { return fault_.av_words_at_fault; }
	NativeCleanupTiming cleanup_timing() const { return lifecycle_.cleanup_timing(); }
	uint64_t cleanup_identity() const
		{ return lifecycle_.cleanup_epoch_identity_for_test(); }
	NativeCleanupBrokerSnapshot cleanup_snapshot(PeripheralSessionKind kind) const
		{ return lifecycle_.cleanup_broker_snapshot_for_test(kind); }
	Result cleanup_retained_snapshot(OperationKind kind,
		NativeRetainedOperationSnapshot *snapshot) const
		{ return lifecycle_.cleanup_retained_snapshot_for_test(kind, snapshot); }
	const NativeRetainedOperationSnapshot &cleanup_live_snapshot() const
		{ assert(retained_capture_.captured); return retained_capture_.snapshot; }
	GraphTypedResourceEvidence typed_resource_evidence() const
	{
		return {av_operations_.begins, av_operations_.finishes,
			av_operations_.words.size(), save_filesystem_.fdatasync_calls,
			save_filesystem_.fsync_calls, save_filesystem_.close_calls,
			save_filesystem_.live_descriptors, protocol_operations_.maps,
			protocol_operations_.closes, protocol_operations_.writes.size()};
	}
	int input_descriptors() const { return input_operations_.live_descriptors; }
	int input_close_calls() const { return input_operations_.close_calls; }
	int av_begins() const { return av_operations_.begins; }
	size_t av_words() const { return av_operations_.words.size(); }
	int scheduler_initializes() const { return scheduler_operations_.initializes; }
	int scheduler_releases() const { return scheduler_operations_.releases; }
	int offload_initializes() const { return offload_operations_.initializes; }
	int offload_releases() const { return offload_operations_.releases; }
	bool content_descriptors_held() const { return content_.owns_descriptors(); }
	bool core_descriptors_held() const { return assets_.core_handle().owns_descriptors(); }
	void ConfigureFault(GraphFaultBoundary boundary, bool after)
	{
		fault_.boundary = boundary;
		fault_.after = after;
		fault_.fired = false;
		fault_.partial_ledger = NativeResourceLedger();
	}
	void EnableCleanupStop() { protocol_operations_.ScriptCleanupStop(); }
	void FailNextProtocolRelease() { protocol_operations_.fail_next_unmap = true; }
	void ConsumeSaveDeadlineAt(uint64_t deadline)
		{ retained_capture_.Arm(OperationKind::save);
		  save_filesystem_.ConsumeDeadlineAt(deadline); }
	void ConsumeAudioDeadlineAt(uint64_t deadline)
		{ retained_capture_.Arm(OperationKind::audio);
		  av_operations_.ConsumeDeadlineAt(GraphFaultBoundary::release_audio, clock_, deadline); }
	void ConsumeVideoDeadlineAt(uint64_t deadline)
		{ retained_capture_.Arm(OperationKind::video);
		  av_operations_.ConsumeDeadlineAt(GraphFaultBoundary::release_video, clock_, deadline); }
	void ConsumeAudioVideoDeadlineAt(uint64_t deadline)
		{ retained_capture_.Arm(OperationKind::audio_video);
		  av_operations_.ConsumeDeadlineAt(GraphFaultBoundary::release_audio_video, clock_, deadline); }
	void ConsumeCoreDeadlineAt(uint64_t deadline)
		{ retained_capture_.Arm(OperationKind::core_protocol);
		  protocol_operations_.ConsumeDeadlineAt(clock_, deadline); }
	void ConsumeTerminalDeadlineAt(uint64_t deadline)
		{ mmio_operations_.ConsumeDeadlineAt(clock_, deadline); }
	void ConsumeOrdinaryDeadlineAt(GraphOrdinaryDeadlineKind kind, uint64_t deadline)
		{ ordinary_deadline_.Arm(kind, deadline); }
	bool ordinary_deadline_consumed() const { return ordinary_deadline_.consumed(); }
	uint64_t ordinary_effective_deadline() const
		{ return ordinary_deadline_.observed_effective_deadline(); }
	static Result DispatchInput(void *context)
	{
		GraphGeneration *self = static_cast<GraphGeneration *>(context);
		std::unique_ptr<OperationLease> lease;
		self->nested_deadline_ = self->tick_deadline_;
		Result result = self->broker_.Begin(self->lifecycle_.generation(),
			OperationKind::input, self->tick_deadline_, &lease);
		if (result == MISTER_RESULT_OK)
			result = self->input_adapter_.PollInput(*lease);
		return result;
	}
private:
	std::string game_, system_, extension_;
	const NativeCoreProfile *profile_;
	GraphFaultPlan fault_;
	GraphOrdinaryDeadlineConsumer ordinary_deadline_;
	GraphRoot root_;
	NativePosixFileSystem filesystem_;
	NativeContentAdapter content_;
	NativeCoreArtifactAdapter core_;
	GraphLaunchAssets assets_;
	TestClock &clock_;
	HardwareBroker broker_;
	GraphMmioOperations mmio_operations_;
	NativeLinuxMmioAdapter mmio_;
	NativeContainment containment_;
	NativeFpgaProgrammer programmer_;
	GraphProtocolOperations protocol_operations_;
	NativeCoreProtocolIoAdapter protocol_io_;
	NativeCoreProtocol protocol_;
	NativeSpiBus spi_;
	NativeInput input_;
	GraphInputOperations input_operations_;
	NativeInputAdapter input_adapter_;
	GraphInputResource input_resource_;
	GraphAvOperations av_operations_;
	NativeAvIoAdapter av_io_;
	NativeAudioAdapter audio_;
	NativeVideoAdapter video_;
	GraphSaveFileSystem save_filesystem_;
	NativeSaveAdapter save_;
	GraphExecutionOperations scheduler_operations_, offload_operations_;
	NativeSchedulerAdapter scheduler_;
	NativeOffloadAdapter offload_;
	GraphFaultAudio fault_audio_;
	GraphFaultVideo fault_video_;
	GraphFaultAudioVideo fault_audio_video_;
	GraphFaultResources fault_process_;
	GraphPreflight preflight_;
	GraphHardware hardware_;
	NativeResourceSet resources_;
	NativeLifecycle lifecycle_;
	int tick_calls_;
	GraphGeneration **owner_;
	GraphFinalBaseline *final_baseline_;
	uint64_t tick_deadline_;
	uint64_t nested_deadline_;
	uint64_t stop_deadline_ = 0;
	bool allow_idle_;
	NativeResourceLedger activation_ledger_ = {};
	int activation_av_begins_ = 0;
	size_t activation_av_words_ = 0;
	GraphRetainedCapture retained_capture_;
	MisterResult activate_return_ = MISTER_RESULT_INVALID_STATE;
	mutable bool idle_return_ = false;
};

class GraphGenerationFactory final : public linux_native::NativeLinuxV2GenerationFactory {
public:
	explicit GraphGenerationFactory(TestClock &clock)
		: clock_(clock), last(nullptr), creates(0), defer_cleanup_stop(false), fault(),
		  load_deadline_kind(GraphOrdinaryDeadlineKind::none),
		  load_deadline_target(0), final() {}
	MisterResult Create(const NativeCoreProfile &profile, const MisterLaunchV2 &launch,
		std::unique_ptr<linux_native::NativeLinuxV2Generation> *generation) override
	{
		final = GraphFinalBaseline();
		GraphGeneration *created = new GraphGeneration(clock_, profile, launch, &last,
			&final, defer_cleanup_stop, fault);
		if (load_deadline_kind != GraphOrdinaryDeadlineKind::none)
			created->ConsumeOrdinaryDeadlineAt(load_deadline_kind,
				load_deadline_target);
		++creates;
		last = created;
		generation->reset(created);
		return MISTER_RESULT_OK;
	}
	TestClock &clock_;
	GraphGeneration *last;
	int creates;
	bool defer_cleanup_stop;
	GraphFaultPlan fault;
	GraphOrdinaryDeadlineKind load_deadline_kind;
	uint64_t load_deadline_target;
	GraphFinalBaseline final;
};

class GraphRecoveryIo final : public NativeRecoveryIo {
public:
	GraphRecoveryIo(NativeCoreProtocol &protocol, bool fail_content_once)
		: protocol_(protocol), input_descriptors_absent_(true), content_absent_(true),
		  input_calls_(0), content_calls_(0), protocol_calls_(0),
		  fail_content_once_(fail_content_once) {}
	const SafeAudioRecoveryRecord *SafeAudioRecord() const override
		{ return FixtureSafeAudioRecoveryRecordForTest(NativeSystem::megadrive); }
	const SafeVideoRecoveryRecord *SafeVideoRecord() const override
		{ return FixtureSafeVideoRecoveryRecordForTest(NativeSystem::megadrive); }
	const SafeAudioVideoRecoveryRecord *SafeAudioVideoRecord() const override
		{ return FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::megadrive); }
	const SafeSaveRecoveryRecord *SafeSaveRecord() const override
		{ return FixtureSafeSaveRecoveryRecordForTest(NativeSystem::megadrive); }
	int input_calls() const { return input_calls_; }
	int content_calls() const { return content_calls_; }
	int protocol_calls() const { return protocol_calls_; }
private:
	static Result Absent(bool absent, RecoveryResourceState *state)
	{
		if (state == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		*state = absent ? RecoveryResourceState::neutral :
			RecoveryResourceState::observed_non_neutral;
		return MISTER_RESULT_OK;
	}
	Result CloseInputDescriptors(const OperationLease &,
		RecoveryResourceState *state) override
	{
		++input_calls_;
		return Absent(input_descriptors_absent_, state);
	}
	Result MuteAudio(const OperationLease &,
		RecoveryResourceState *) override { return MISTER_RESULT_INVALID_STATE; }
	Result PowerDownVideo(const OperationLease &,
		RecoveryResourceState *) override { return MISTER_RESULT_INVALID_STATE; }
	Result CloseContent(const OperationLease &,
		RecoveryResourceState *state) override
	{
		++content_calls_;
		if (fail_content_once_) {
			fail_content_once_ = false;
			*state = RecoveryResourceState::observed_non_neutral;
			return MISTER_RESULT_DEADLINE;
		}
		return Absent(content_absent_, state);
	}
	Result DisableCoreProtocol(const OperationLease &lease,
		RecoveryResourceState *state) override
	{
		++protocol_calls_;
		return protocol_.DisableStateless(lease, state);
	}
	NativeCoreProtocol &protocol_;
	bool input_descriptors_absent_, content_absent_;
	int input_calls_, content_calls_, protocol_calls_;
	bool fail_content_once_;
};

class GraphRecoverySave final : public NativeSaveResource {
public:
	explicit GraphRecoverySave(NativeSaveAdapter &save) : save_(save) {}
	NativeSaveOpenOutcome OpenSave(const OperationLease &lease,
		const NativeCoreProfile &profile, const NativeSaveKey &key) override
		{ return save_.OpenSave(lease, profile, key); }
	NativeSaveCloseOutcome FlushAndCloseSave(const OperationLease &lease) override
		{ return save_.FlushAndCloseSave(lease); }
	NativeSaveCloseOutcome RecoverSave(const OperationLease &lease,
		const SafeSaveRecoveryRecord &record) override
		{ return save_.RecoverSave(lease, record); }
	void CloseSaveForProcessExit() override { save_.CloseSaveForProcessExit(); }
private:
	NativeSaveAdapter &save_;
};

enum class GraphRecoveryDeadlineKind : uint8_t {
	none,
	observation,
	save,
	video,
	audio,
	audio_video,
	core_protocol,
	terminal
};

struct GraphRecoveryEvidenceState {
	bool live_captured = false;
	bool suspended_captured = false;
	bool rebound_captured = false;
	bool baseline_captured = false;
	NativeRetainedOperationSnapshot live = {};
	NativeRetainedOperationSnapshot suspended = {};
	NativeRetainedOperationSnapshot rebound = {};
	NativeRetainedOperationSnapshot baseline = {};
	bool mmio_descriptor_open = true;
	bool mmio_mappings_held = true;
	bool protocol_descriptor_open = true;
	int protocol_mappings = -1;
	int save_descriptors = -1;
	int input_close_calls = -1;
	int content_close_calls = -1;
	int protocol_disable_calls = -1;
	int av_begins = -1;
	int av_finishes = -1;
	size_t av_words = 0;
	int save_fdatasync_calls = -1;
	int save_fsync_calls = -1;
	int save_close_calls = -1;
	size_t protocol_writes = 0;
	int observation_calls = 0;
	size_t observation_mmio_events = 0;
	MisterResult terminal_finish_result = MISTER_RESULT_OK;
};

static void AssertSameGraphRecoveryResources(
	const GraphRecoveryEvidenceState &left,
	const GraphRecoveryEvidenceState &right)
{
	assert(left.mmio_descriptor_open == right.mmio_descriptor_open);
	assert(left.mmio_mappings_held == right.mmio_mappings_held);
	assert(left.protocol_descriptor_open == right.protocol_descriptor_open);
	assert(left.protocol_mappings == right.protocol_mappings);
	assert(left.save_descriptors == right.save_descriptors);
	assert(left.input_close_calls == right.input_close_calls);
	assert(left.content_close_calls == right.content_close_calls);
	assert(left.protocol_disable_calls == right.protocol_disable_calls);
	assert(left.av_begins == right.av_begins);
	assert(left.av_finishes == right.av_finishes);
	assert(left.av_words == right.av_words);
	assert(left.save_fdatasync_calls == right.save_fdatasync_calls);
	assert(left.save_fsync_calls == right.save_fsync_calls);
	assert(left.save_close_calls == right.save_close_calls);
	assert(left.protocol_writes == right.protocol_writes);
}

class GraphRecoveryAttempt final : public linux_native::NativeLinuxV2RecoveryAttempt {
public:
	GraphRecoveryAttempt(TestClock &clock, uint32_t requested, uint64_t start,
		bool fail_content_once, std::vector<OperationKind> *trace, int *destroys,
		GraphRecoveryDeadlineKind deadline_kind, int deadline_offset,
		bool deadline_uses_group, GraphRecoveryEvidenceState *evidence)
		: clock_(clock), broker_(clock), mmio_operations_(), mmio_(mmio_operations_),
		  containment_(broker_, mmio_), protocol_operations_(),
		  protocol_io_(clock, protocol_operations_),
		  protocol_(clock, broker_, protocol_io_.capabilities()),
		  av_operations_(), av_io_(clock, av_operations_),
		  audio_(clock, broker_, av_io_), video_(clock, broker_, av_io_),
		  save_filesystem_(&clock, true), save_(broker_, save_filesystem_), save_resource_(save_),
		  io_(protocol_, fail_content_once), recovery_(new NativeRecovery(broker_, io_,
			audio_, video_, video_, save_resource_, containment_)), epoch_(),
		  requested_(requested), complete_(false), audio_prepared_for_terminal_(false),
		  observation_complete_(false), observation_calls_(0),
		  trace_(trace), destroys_(destroys), deadline_kind_(deadline_kind),
		  deadline_offset_(deadline_offset), deadline_armed_(false),
		  deadline_uses_group_(deadline_uses_group), evidence_(evidence)
	{
		non_fpga_deadline_ = start > UINT64_MAX - 2000 ? UINT64_MAX : start + 2000;
		fpga_deadline_ = start > UINT64_MAX - 5000 ? UINT64_MAX : start + 5000;
		retained_capture_.recovery = recovery_.get();
		retained_capture_.recovery_epoch = epoch_.get();
		mmio_operations_.SetRetainedCapture(&retained_capture_);
		protocol_operations_.SetRetainedCapture(&retained_capture_);
		av_operations_.SetRetainedCapture(&retained_capture_);
		save_filesystem_.SetRetainedCapture(&retained_capture_);
		if (evidence_ != nullptr) {
			mmio_operations_.SetFinalEvidence(&evidence_->mmio_descriptor_open,
				&evidence_->mmio_mappings_held);
			protocol_operations_.SetFinalEvidence(
				&evidence_->protocol_descriptor_open, &evidence_->protocol_mappings);
			save_filesystem_.SetFinalEvidence(&evidence_->save_descriptors);
		}
		assert(broker_.BeginRecovery(requested_, non_fpga_deadline_, fpga_deadline_, &epoch_) ==
			MISTER_RESULT_OK);
		retained_capture_.recovery_epoch = epoch_.get();
	}
	~GraphRecoveryAttempt() override
	{
		if (epoch_) {
			recovery_.reset();
			MisterRecoveryObservationV2 observation = {};
			observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
			observation.struct_size = sizeof(observation);
			const MisterResult terminal = broker_.FinishRecovery(std::move(epoch_),
				&observation);
			if (evidence_ != nullptr) evidence_->terminal_finish_result = terminal;
		}
		if (mmio_operations_.AnyHeld())
			assert(mmio_.CloseMappingsForProcessExit() == MISTER_RESULT_OK);
		assert(!epoch_);
		assert(!mmio_operations_.AnyHeld());
		assert(mmio_operations_.programmed_words.empty());
		if (evidence_ != nullptr) {
			NativeRecovery observer(broker_, io_, audio_, video_, video_, save_resource_,
				containment_);
			assert(observer.broker_baseline_snapshot_for_test(&evidence_->baseline) ==
				MISTER_RESULT_OK);
			evidence_->baseline_captured = true;
			RecordResourceEvidence();
		}
		if (destroys_ != nullptr) ++*destroys_;
	}
	uint32_t requested_resource_flags() const override { return requested_; }
	MisterResult Continue(uint64_t deadline,
		MisterRecoveryObservationV2 *observation) override
	{
		if (complete_ || !epoch_) return MISTER_RESULT_INVALID_STATE;
		if (deadline_kind_ == GraphRecoveryDeadlineKind::observation &&
			!observation_complete_) {
			const uint64_t boundary = deadline_uses_group_ ? fpga_deadline_ : deadline;
			const uint64_t target = deadline_offset_ < 0 ? boundary - 1 :
				boundary + static_cast<uint64_t>(deadline_offset_);
			if (!deadline_armed_) {
				mmio_operations_.ConsumeDeadlineAt(clock_, target);
				deadline_armed_ = true;
			}
			std::unique_ptr<OperationInvocation> invocation;
			Result result = broker_.BeginRecoveryInvocation(*epoch_, deadline,
				&invocation);
			if (result == MISTER_RESULT_OK) {
				++observation_calls_;
				result = containment_.ObserveRecovery(*epoch_, *invocation);
				const Result finished = broker_.FinishInvocation(std::move(invocation));
				if (result == MISTER_RESULT_OK) result = finished;
			}
			const Result snapshot = recovery_->Snapshot(*epoch_, observation);
			if (result == MISTER_RESULT_OK) {
				observation_complete_ = true;
				result = snapshot;
			}
			RecordResourceEvidence();
			return result;
		}
		MisterRecoveryObservationV2 before = {};
		before.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
		before.struct_size = sizeof(before);
		Result result = recovery_->Snapshot(*epoch_, &before);
		if (result != MISTER_RESULT_OK && result != MISTER_RESULT_CLEANUP_INCOMPLETE)
			return result;
		const uint32_t outstanding = requested_ & ~before.neutral_resource_flags;
		const OperationKind operation = NextOperation(outstanding);
		if (trace_ != nullptr) trace_->push_back(operation);
		if (outstanding == 0) {
			*observation = before;
			result = recovery_->Finish(std::move(epoch_), observation);
			complete_ = result == MISTER_RESULT_OK;
			RecordResourceEvidence();
			return result;
		}
		ArmDeadline(operation, deadline);
		std::unique_ptr<OperationInvocation> invocation;
		result = broker_.BeginRecoveryInvocation(*epoch_, deadline, &invocation);
		bool retained_before_perform = false;
		if (result == MISTER_RESULT_OK) {
			NativeRetainedOperationSnapshot retained = {};
			retained_before_perform = recovery_->retained_snapshot_for_test(*epoch_,
				operation, &retained) == MISTER_RESULT_OK && retained.retained;
			if (retained_before_perform) ArmRetainedCapture(operation);
			result = recovery_->Perform(*epoch_, *invocation, operation);
		}
		if (retained_capture_.captured && evidence_ != nullptr) {
			if (retained_before_perform) {
				evidence_->rebound = retained_capture_.snapshot;
				evidence_->rebound_captured = true;
			} else if (!evidence_->live_captured) {
				evidence_->live = retained_capture_.snapshot;
				evidence_->live_captured = true;
			}
		}
		if (operation == OperationKind::audio &&
			(requested_ & (MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES)) != 0 &&
			(result == MISTER_RESULT_OK || result == MISTER_RESULT_CLEANUP_INCOMPLETE))
			audio_prepared_for_terminal_ = true;
		if (invocation) {
			const Result finished = broker_.FinishInvocation(std::move(invocation));
			if (result == MISTER_RESULT_OK) result = finished;
		}
		if (epoch_ && operation != OperationKind::terminal_fpga_cleanup &&
			evidence_ != nullptr) {
			NativeRetainedOperationSnapshot suspended = {};
			if (recovery_->retained_snapshot_for_test(*epoch_, operation, &suspended) ==
				MISTER_RESULT_OK && suspended.retained) {
				evidence_->suspended = suspended;
				evidence_->suspended_captured = true;
			}
		}
		const Result snapshot = recovery_->Snapshot(*epoch_, observation);
		if (result == MISTER_RESULT_OK && snapshot != MISTER_RESULT_OK) result = snapshot;
		if (result == MISTER_RESULT_OK && observation->observed_resource_flags == 0 &&
			observation->neutral_resource_flags == requested_) {
			result = recovery_->Finish(std::move(epoch_), observation);
			complete_ = result == MISTER_RESULT_OK;
		}
		RecordResourceEvidence();
		return result;
	}
	bool complete() const override { return complete_; }
private:
	void RecordResourceEvidence()
	{
		if (evidence_ == nullptr) return;
		evidence_->mmio_descriptor_open = mmio_operations_.descriptor_open;
		evidence_->mmio_mappings_held = mmio_operations_.AnyHeld();
		evidence_->protocol_descriptor_open = protocol_operations_.descriptor_open;
		evidence_->protocol_mappings = protocol_operations_.maps;
		evidence_->save_descriptors = save_filesystem_.live_descriptors;
		evidence_->input_close_calls = io_.input_calls();
		evidence_->content_close_calls = io_.content_calls();
		evidence_->protocol_disable_calls = io_.protocol_calls();
		evidence_->av_begins = av_operations_.begins;
		evidence_->av_finishes = av_operations_.finishes;
		evidence_->av_words = av_operations_.words.size();
		evidence_->save_fdatasync_calls = save_filesystem_.fdatasync_calls;
		evidence_->save_fsync_calls = save_filesystem_.fsync_calls;
		evidence_->save_close_calls = save_filesystem_.close_calls;
		evidence_->protocol_writes = protocol_operations_.writes.size();
		evidence_->observation_calls = observation_calls_;
		evidence_->observation_mmio_events = mmio_operations_.event_count;
	}
	OperationKind NextOperation(uint32_t outstanding) const
	{
		if ((outstanding & MISTER_RESOURCE_CORE_INPUT) != 0)
			return OperationKind::input_descriptors;
		if ((outstanding & MISTER_RESOURCE_SAVES) != 0)
			return OperationKind::save;
		if ((outstanding & MISTER_RESOURCE_CONTENT) != 0)
			return OperationKind::content;
		const uint32_t av = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		if ((outstanding & av) == av) return OperationKind::audio_video;
		if ((outstanding & (MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES)) != 0) {
			if ((outstanding & MISTER_RESOURCE_NATIVE_AUDIO) != 0 &&
				!audio_prepared_for_terminal_) return OperationKind::audio;
			return OperationKind::terminal_fpga_cleanup;
		}
		if ((outstanding & MISTER_RESOURCE_NATIVE_AUDIO) != 0)
			return OperationKind::audio;
		if ((outstanding & MISTER_RESOURCE_NATIVE_VIDEO) != 0)
			return OperationKind::video;
		return OperationKind::core_protocol;
	}
	void ArmDeadline(OperationKind operation, uint64_t deadline)
	{
		if (deadline_armed_) return;
		const uint64_t authority_deadline = operation == OperationKind::core_protocol ||
			operation == OperationKind::terminal_fpga_cleanup ? fpga_deadline_ :
			non_fpga_deadline_;
		const uint64_t boundary = deadline_uses_group_ ? authority_deadline : deadline;
		const uint64_t target = deadline_offset_ < 0 ? boundary - 1 :
			boundary + static_cast<uint64_t>(deadline_offset_);
		const bool matches =
			(deadline_kind_ == GraphRecoveryDeadlineKind::save &&
			 operation == OperationKind::save) ||
			(deadline_kind_ == GraphRecoveryDeadlineKind::video &&
			 operation == OperationKind::video) ||
			(deadline_kind_ == GraphRecoveryDeadlineKind::audio &&
			 operation == OperationKind::audio) ||
			(deadline_kind_ == GraphRecoveryDeadlineKind::audio_video &&
			 operation == OperationKind::audio_video) ||
			(deadline_kind_ == GraphRecoveryDeadlineKind::core_protocol &&
			 operation == OperationKind::core_protocol) ||
			(deadline_kind_ == GraphRecoveryDeadlineKind::terminal &&
			 operation == OperationKind::terminal_fpga_cleanup);
		if (!matches) return;
		if (operation != OperationKind::terminal_fpga_cleanup)
			retained_capture_.Arm(operation);
		if (deadline_kind_ == GraphRecoveryDeadlineKind::save)
			save_filesystem_.ConsumeDeadlineAt(target);
		else if (deadline_kind_ == GraphRecoveryDeadlineKind::core_protocol)
			protocol_operations_.ConsumeDeadlineAt(clock_, target);
		else if (deadline_kind_ == GraphRecoveryDeadlineKind::terminal)
			mmio_operations_.ConsumeDeadlineAt(clock_, target);
		else
			av_operations_.ConsumeAnyDeadlineAt(clock_, target);
		deadline_armed_ = true;
	}
	void ArmRetainedCapture(OperationKind operation)
	{
		retained_capture_.Arm(operation);
		if (operation == OperationKind::save)
			save_filesystem_.ConsumeDeadlineAt(clock_.NowMs());
		else if (operation == OperationKind::core_protocol)
			protocol_operations_.ConsumeDeadlineAt(clock_, clock_.NowMs());
		else
			av_operations_.ConsumeAnyDeadlineAt(clock_, clock_.NowMs());
	}
	TestClock &clock_;
	HardwareBroker broker_;
	GraphMmioOperations mmio_operations_;
	NativeLinuxMmioAdapter mmio_;
	NativeContainment containment_;
	GraphProtocolOperations protocol_operations_;
	NativeCoreProtocolIoAdapter protocol_io_;
	NativeCoreProtocol protocol_;
	GraphAvOperations av_operations_;
	NativeAvIoAdapter av_io_;
	NativeAudioAdapter audio_;
	NativeVideoAdapter video_;
	GraphSaveFileSystem save_filesystem_;
	NativeSaveAdapter save_;
	GraphRecoverySave save_resource_;
	GraphRecoveryIo io_;
	std::unique_ptr<NativeRecovery> recovery_;
	std::unique_ptr<RecoveryEpoch> epoch_;
	uint32_t requested_;
	bool complete_;
	bool audio_prepared_for_terminal_;
	bool observation_complete_;
	int observation_calls_;
	std::vector<OperationKind> *trace_;
	int *destroys_;
	GraphRecoveryDeadlineKind deadline_kind_;
	int deadline_offset_;
	bool deadline_armed_;
	bool deadline_uses_group_;
	uint64_t non_fpga_deadline_ = 0;
	uint64_t fpga_deadline_ = 0;
	GraphRecoveryEvidenceState *evidence_;
	GraphRetainedCapture retained_capture_;
};

class GraphRecoveryFactory final : public linux_native::NativeLinuxV2RecoveryFactory {
public:
	explicit GraphRecoveryFactory(TestClock &clock)
		: clock_(clock), creates(0), destroys(0), fail_content_once(false),
		  deadline_kind(GraphRecoveryDeadlineKind::none), deadline_offset(0),
		  deadline_uses_group(false), evidence() {}
	MisterResult Create(uint32_t requested, uint64_t start,
		std::unique_ptr<linux_native::NativeLinuxV2RecoveryAttempt> *attempt) override
	{
		++creates;
		attempt->reset(new GraphRecoveryAttempt(clock_, requested, start,
			fail_content_once, &trace, &destroys, deadline_kind, deadline_offset,
			deadline_uses_group, &evidence));
		return MISTER_RESULT_OK;
	}
	TestClock &clock_;
	int creates;
	int destroys;
	bool fail_content_once;
	GraphRecoveryDeadlineKind deadline_kind;
	int deadline_offset;
	bool deadline_uses_group;
	std::vector<OperationKind> trace;
	GraphRecoveryEvidenceState evidence;
};

struct GenerationState {
	GenerationState()
		: create_calls(0), activate_calls(0), tick_calls(0), observe_calls(0),
		  stop_calls(0), destroyed(0), activate_result(MISTER_RESULT_OK),
		  tick_result(MISTER_RESULT_OK), observe_result(MISTER_RESULT_OK),
		  stop_result(MISTER_RESULT_OK), idle_value(false),
		  activate_deadline(0), tick_deadline(0), observe_deadline(0),
		  stop_deadline(0), selected_profile(nullptr), resource_flags(0), ready(0) {}
	int create_calls;
	int activate_calls;
	int tick_calls;
	mutable int observe_calls;
	int stop_calls;
	int destroyed;
	MisterResult activate_result;
	MisterResult tick_result;
	MisterResult observe_result;
	MisterResult stop_result;
	bool idle_value;
	uint64_t activate_deadline;
	uint64_t tick_deadline;
	mutable uint64_t observe_deadline;
	uint64_t stop_deadline;
	const NativeCoreProfile *selected_profile;
	uint32_t resource_flags;
	uint32_t ready;
	std::string copied_game;
	std::string copied_system;
	std::string copied_core;
	std::string copied_digest;
	std::string copied_extension;
};

static std::string CopyView(MisterStringView view)
{
	return std::string(view.data == nullptr ? "" : view.data, view.length);
}

class TestGeneration final : public linux_native::NativeLinuxV2Generation {
public:
	explicit TestGeneration(GenerationState &state) : state_(state) {}
	~TestGeneration() override { ++state_.destroyed; }
	MisterResult Activate(uint64_t deadline) override
	{
		++state_.activate_calls;
		state_.activate_deadline = deadline;
		return state_.activate_result;
	}
	MisterResult Tick(uint64_t deadline) override
	{
		++state_.tick_calls;
		state_.tick_deadline = deadline;
		return state_.tick_result;
	}
	MisterResult Observe(MisterObservationV2 *observation,
		uint64_t deadline) const override
	{
		++state_.observe_calls;
		state_.observe_deadline = deadline;
		observation->ready = state_.ready;
		observation->resource_flags = state_.resource_flags;
		return state_.observe_result;
	}
	MisterResult Stop(uint64_t deadline) override
	{
		++state_.stop_calls;
		state_.stop_deadline = deadline;
		return state_.stop_result;
	}
	bool idle() const override { return state_.idle_value; }

private:
	GenerationState &state_;
};

class TestGenerationFactory final : public linux_native::NativeLinuxV2GenerationFactory {
public:
	explicit TestGenerationFactory(GenerationState &state)
		: create_result(MISTER_RESULT_OK), return_null(false), state_(state) {}
	MisterResult Create(const NativeCoreProfile &profile,
		const MisterLaunchV2 &launch,
		std::unique_ptr<linux_native::NativeLinuxV2Generation> *generation) override
	{
		++state_.create_calls;
		state_.selected_profile = &profile;
		state_.copied_game = CopyView(launch.game_id);
		state_.copied_system = CopyView(launch.system);
		state_.copied_core = CopyView(launch.expected_core);
		state_.copied_digest = CopyView(launch.content.sha256);
		state_.copied_extension = CopyView(launch.content.extension);
		if (create_result != MISTER_RESULT_OK) return create_result;
		if (!return_null) generation->reset(new TestGeneration(state_));
		return MISTER_RESULT_OK;
	}
	MisterResult create_result;
	bool return_null;

private:
	GenerationState &state_;
};

struct RecoveryState {
	RecoveryState()
		: create_calls(0), continue_calls(0), destroyed(0), create_result(MISTER_RESULT_OK),
		  continue_result(MISTER_RESULT_OK), complete_value(false), requested(0),
		  create_now(0), continue_deadline(0), observed(0), neutral(0),
		  product_requested(0), output_abi(MISTER_RUNTIME_ABI_VERSION_V2),
		  output_size(sizeof(MisterRecoveryObservationV2)), output_reserved(0) {}
	int create_calls;
	int continue_calls;
	int destroyed;
	MisterResult create_result;
	MisterResult continue_result;
	bool complete_value;
	uint32_t requested;
	uint64_t create_now;
	uint64_t continue_deadline;
	uint32_t observed;
	uint32_t neutral;
	uint32_t product_requested;
	uint32_t output_abi;
	uint32_t output_size;
	uint32_t output_reserved;
};

class TestRecoveryAttempt final : public linux_native::NativeLinuxV2RecoveryAttempt {
public:
	explicit TestRecoveryAttempt(RecoveryState &state) : state_(state) {}
	~TestRecoveryAttempt() override { ++state_.destroyed; }
	uint32_t requested_resource_flags() const override
	{
		return state_.product_requested == 0 ? state_.requested :
			state_.product_requested;
	}
	MisterResult Continue(uint64_t deadline,
		MisterRecoveryObservationV2 *observation) override
	{
		++state_.continue_calls;
		state_.continue_deadline = deadline;
		observation->observed_resource_flags = state_.observed;
		observation->neutral_resource_flags = state_.neutral;
		observation->abi_version = state_.output_abi;
		observation->struct_size = state_.output_size;
		observation->reserved[0] = state_.output_reserved;
		return state_.continue_result;
	}
	bool complete() const override { return state_.complete_value; }

private:
	RecoveryState &state_;
};

class TestRecoveryFactory final : public linux_native::NativeLinuxV2RecoveryFactory {
public:
	explicit TestRecoveryFactory(RecoveryState &state)
		: return_null(false), state_(state) {}
	MisterResult Create(uint32_t requested, uint64_t now,
		std::unique_ptr<linux_native::NativeLinuxV2RecoveryAttempt> *attempt) override
	{
		++state_.create_calls;
		state_.requested = requested;
		state_.create_now = now;
		if (state_.create_result != MISTER_RESULT_OK) return state_.create_result;
		if (!return_null) attempt->reset(new TestRecoveryAttempt(state_));
		return MISTER_RESULT_OK;
	}
	bool return_null;

private:
	RecoveryState &state_;
};

struct Fixture {
	Fixture(uint64_t now = 100)
		: clock(now), generation_factory(generation), recovery_factory(recovery)
	{
		profiles.snes = FixtureNativeCoreProfile("snes");
		profiles.megadrive = FixtureNativeCoreProfile("megadrive");
		assert(profiles.snes != nullptr);
		assert(profiles.megadrive != nullptr);
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generation_factory, recovery_factory, &context) == MISTER_RESULT_OK);
		assert(context.get() != nullptr);
	}
	TestClock clock;
	GenerationState generation;
	RecoveryState recovery;
	TestGenerationFactory generation_factory;
	TestRecoveryFactory recovery_factory;
	linux_native::NativeLinuxV2FixtureProfiles profiles;
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
};

static MisterStringView View(const char *text)
{
	MisterStringView view = {text, static_cast<uint32_t>(strlen(text))};
	return view;
}

static MisterLaunchV2 Launch(const char *system = "snes",
	const char *core = "SNES", const char *extension = "sfc")
{
	MisterLaunchV2 launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id = View("task7-game-sentinel");
	launch.system = View(system);
	launch.expected_core = View(core);
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256 = View(
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	launch.content.size = 65536;
	launch.content.extension = View(extension);
	return launch;
}

static MisterObservationV2 Observation()
{
	MisterObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

static MisterRecoveryObservationV2 RecoveryObservation()
{
	MisterRecoveryObservationV2 observation = {};
	observation.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	observation.struct_size = sizeof(observation);
	return observation;
}

static void TestConstructionAndStableTable()
{
	std::unique_ptr<linux_native::NativeLinuxV2Context> output;
	assert(linux_native::CreateProductionNativeLinuxV2Context(nullptr) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(linux_native::CreateProductionNativeLinuxV2Context(&output) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(output.get() == nullptr);

	Fixture fixture;
	const MisterPlatformV2 *first = &fixture.context->platform();
	const MisterPlatformV2 *second = &fixture.context->platform();
	assert(first == second);
	assert(first->abi_version == MISTER_RUNTIME_ABI_VERSION_V2);
	assert(first->struct_size == sizeof(*first));
	assert(first->capability_flags == MISTER_CAP_V2_KNOWN);
	assert(first->context != nullptr);
	assert(first->start != nullptr && first->load != nullptr && first->tick != nullptr);
	assert(first->observe != nullptr && first->stop != nullptr && first->recover != nullptr);
	for (size_t index = 0; index < 4; ++index) assert(first->reserved[index] == 0);

	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(fixture.clock,
		fixture.profiles, fixture.generation_factory, fixture.recovery_factory,
		nullptr) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(fixture.clock,
		fixture.profiles, fixture.generation_factory, fixture.recovery_factory,
		&fixture.context) == MISTER_RESULT_INVALID_ARGUMENT);
}

static void TestStartLoadTickObserveAndStop()
{
	Fixture fixture;
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(nullptr, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.start(platform.context, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
	assert(platform.start(platform.context, 10) == MISTER_RESULT_INVALID_STATE);
	assert(fixture.generation.create_calls == 0);

	MisterLaunchV2 launch = Launch();
	assert(platform.load(platform.context, &launch, 25) == MISTER_RESULT_OK);
	assert(fixture.generation.create_calls == 1);
	assert(fixture.generation.activate_calls == 1);
	assert(fixture.generation.activate_deadline == 125);
	assert(fixture.generation.selected_profile == fixture.profiles.snes);
	assert(fixture.generation.copied_game == "task7-game-sentinel");
	assert(fixture.generation.copied_digest.size() == 64);
	assert(platform.load(platform.context, &launch, 25) == MISTER_RESULT_INVALID_STATE);

	assert(platform.tick(platform.context, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.tick(platform.context, 30) == MISTER_RESULT_OK);
	assert(fixture.generation.tick_deadline == 130);

	fixture.generation.ready = 1;
	fixture.generation.resource_flags = MISTER_RESOURCE_V2_KNOWN;
	MisterObservationV2 observation = Observation();
	assert(platform.observe(platform.context, &observation, 40) == MISTER_RESULT_OK);
	assert(fixture.generation.observe_deadline == 140);
	assert(observation.ready == 1);
	assert(observation.resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(CopyView(observation.observed_core) == "SNES");

	fixture.generation.stop_result = MISTER_RESULT_CLEANUP_INCOMPLETE;
	fixture.generation.idle_value = false;
	assert(platform.stop(platform.context, 50) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.generation.stop_deadline == 150);
	assert(fixture.generation.destroyed == 0);
	fixture.clock.Set(200);
	fixture.generation.stop_result = MISTER_RESULT_OK;
	fixture.generation.idle_value = true;
	assert(platform.stop(platform.context, 60) == MISTER_RESULT_OK);
	assert(fixture.generation.stop_deadline == 260);
	assert(fixture.generation.destroyed == 1);
	assert(platform.stop(platform.context, 1) == MISTER_RESULT_OK);
	observation = Observation();
	assert(platform.observe(platform.context, &observation, 1) == MISTER_RESULT_OK);
	assert(observation.ready == 0 && observation.resource_flags == 0);
	assert(observation.observed_core.data == nullptr && observation.observed_core.length == 0);
}

static void TestLaunchValidationAndFailureRetention()
{
	Fixture fixture;
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(platform.context, 1) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch();
	assert(platform.load(platform.context, nullptr, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.load(platform.context, &launch, 0) == MISTER_RESULT_INVALID_ARGUMENT);
	launch.abi_version = 1;
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = Launch("SNES", "SNES", "sfc");
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = Launch("snes", "MegaDrive", "sfc");
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = Launch("snes", "SNES", "md");
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	launch = Launch();
	launch.content.size = fixture.profiles.snes->protocol.maximum_source_bytes + 1;
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_INVALID_ARGUMENT);
	assert(fixture.generation.create_calls == 0);
	launch = Launch("megadrive", "MegaDrive", "gen");
	fixture.generation.activate_result = MISTER_RESULT_PLATFORM;
	fixture.generation.idle_value = false;
	assert(platform.load(platform.context, &launch, 9) == MISTER_RESULT_PLATFORM);
	assert(fixture.generation.selected_profile == fixture.profiles.megadrive);
	assert(fixture.generation.activate_deadline == 109);
	assert(fixture.generation.destroyed == 0);
	assert(platform.load(platform.context, &launch, 9) == MISTER_RESULT_INVALID_STATE);
	assert(platform.tick(platform.context, 9) == MISTER_RESULT_INVALID_STATE);
	assert(fixture.generation.tick_calls == 0);
	fixture.generation.stop_result = MISTER_RESULT_OK;
	fixture.generation.idle_value = true;
	assert(platform.stop(platform.context, 9) == MISTER_RESULT_OK);
	assert(fixture.generation.destroyed == 1);
}

static void TestForwardCompatibleLaunchPrefixes()
{
	Fixture fixture;
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(platform.context, 1) == MISTER_RESULT_OK);
	struct ExtendedLaunch {
		MisterLaunchV2 prefix;
		uint8_t tail[8];
	} extended = {};
	extended.prefix = Launch();
	extended.prefix.struct_size = sizeof(extended);
	extended.prefix.content.struct_size = sizeof(MisterContentRefV2) + 8;
	memset(extended.tail, 0xa5, sizeof(extended.tail));
	assert(platform.load(platform.context, &extended.prefix, 10) == MISTER_RESULT_OK);
	for (size_t index = 0; index < sizeof(extended.tail); ++index) {
		assert(extended.tail[index] == 0xa5);
	}
	fixture.generation.idle_value = true;
	assert(platform.stop(platform.context, 10) == MISTER_RESULT_OK);

	extended.prefix = Launch();
	extended.prefix.struct_size = sizeof(MisterLaunchV2) - 1;
	assert(platform.load(platform.context, &extended.prefix, 10) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	extended.prefix = Launch();
	extended.prefix.content.struct_size = sizeof(MisterContentRefV2) - 1;
	assert(platform.load(platform.context, &extended.prefix, 10) ==
		MISTER_RESULT_INVALID_ARGUMENT);
}

static void TestObserveRejectsReadyWithoutCompleteActiveResources()
{
	Fixture fixture;
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(platform.context, 1) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch();
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_OK);
	fixture.generation.ready = 1;
	fixture.generation.resource_flags = MISTER_RESOURCE_FPGA;
	MisterObservationV2 observation = Observation();
	assert(platform.observe(platform.context, &observation, 1) ==
		MISTER_RESULT_PLATFORM);
}

static void TestTickFailureRequiresExplicitStop()
{
	Fixture fixture;
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(platform.context, 1) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch();
	assert(platform.load(platform.context, &launch, 1) == MISTER_RESULT_OK);
	fixture.generation.tick_result = MISTER_RESULT_DEADLINE;
	assert(platform.tick(platform.context, 1) == MISTER_RESULT_DEADLINE);
	assert(platform.tick(platform.context, 1) == MISTER_RESULT_INVALID_STATE);
	assert(fixture.generation.tick_calls == 1);
	fixture.generation.idle_value = true;
	assert(platform.stop(platform.context, 1) == MISTER_RESULT_OK);
}

static void TestFactoryFailureAndSaturation()
{
	Fixture fixture(UINT64_MAX - 3);
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(platform.context, 1) == MISTER_RESULT_OK);
	fixture.generation_factory.create_result = MISTER_RESULT_DEADLINE;
	MisterLaunchV2 launch = Launch();
	assert(platform.load(platform.context, &launch, UINT32_MAX) == MISTER_RESULT_DEADLINE);
	assert(fixture.generation.activate_calls == 0);
	fixture.generation_factory.create_result = MISTER_RESULT_OK;
	fixture.generation_factory.return_null = true;
	assert(platform.load(platform.context, &launch, UINT32_MAX) == MISTER_RESULT_PLATFORM);
	fixture.generation_factory.return_null = false;
	assert(platform.load(platform.context, &launch, UINT32_MAX) == MISTER_RESULT_OK);
	assert(fixture.generation.activate_deadline == UINT64_MAX);
}

static void TestRecoveryAdmissionRetryAndTruthfulOutput()
{
	Fixture fixture(500);
	const MisterPlatformV2 &platform = fixture.context->platform();
	MisterRecoveryObservationV2 observation = RecoveryObservation();
	assert(platform.recover(nullptr, MISTER_RESOURCE_FPGA, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.recover(platform.context, 0, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.recover(platform.context, 1u << 30, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	assert(platform.recover(platform.context, MISTER_RESOURCE_FPGA, &observation, 0) ==
		MISTER_RESULT_INVALID_ARGUMENT);

	fixture.recovery.continue_result = MISTER_RESULT_DEADLINE;
	fixture.recovery.observed = MISTER_RESOURCE_FPGA;
	fixture.recovery.neutral = MISTER_RESOURCE_BRIDGES;
	assert(platform.recover(platform.context,
		MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES, &observation, 20) ==
		MISTER_RESULT_DEADLINE);
	assert(fixture.recovery.create_calls == 1);
	assert(fixture.recovery.create_now == 500);
	assert(fixture.recovery.continue_deadline == 520);
	assert(observation.observed_resource_flags == MISTER_RESOURCE_FPGA);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_BRIDGES);

	MisterRecoveryObservationV2 changed = RecoveryObservation();
	assert(platform.recover(platform.context, MISTER_RESOURCE_FPGA, &changed, 10) ==
		MISTER_RESULT_INVALID_STATE);
	assert(fixture.recovery.continue_calls == 1);
	fixture.clock.Set(600);
	fixture.recovery.continue_result = MISTER_RESULT_OK;
	fixture.recovery.complete_value = true;
	fixture.recovery.observed = 0;
	fixture.recovery.neutral = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES;
	observation = RecoveryObservation();
	assert(platform.recover(platform.context,
		MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES, &observation, 30) ==
		MISTER_RESULT_OK);
	assert(fixture.recovery.create_calls == 1);
	assert(fixture.recovery.continue_deadline == 630);
	assert(fixture.recovery.destroyed == 1);
	observation = RecoveryObservation();
	assert(platform.recover(platform.context,
		MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES, &observation, 30) ==
		MISTER_RESULT_INVALID_STATE);
}

static void TestRecoveryRejectedAfterStartAndInvalidOutputs()
{
	Fixture fixture;
	const MisterPlatformV2 &platform = fixture.context->platform();
	assert(platform.start(platform.context, 1) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 recovery = RecoveryObservation();
	assert(platform.recover(platform.context, MISTER_RESOURCE_FPGA, &recovery, 1) ==
		MISTER_RESULT_INVALID_STATE);
	MisterObservationV2 observation = Observation();
	observation.ready = 1;
	assert(platform.observe(platform.context, &observation, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
	recovery = RecoveryObservation();
	recovery.observed_resource_flags = MISTER_RESOURCE_FPGA;
	assert(platform.recover(platform.context, MISTER_RESOURCE_FPGA, &recovery, 1) ==
		MISTER_RESULT_INVALID_ARGUMENT);
}

static void TestRecoveryRejectsMismatchedProductPermanently()
{
	Fixture fixture;
	fixture.recovery.product_requested = MISTER_RESOURCE_CONTENT;
	MisterRecoveryObservationV2 observation = RecoveryObservation();
	assert(fixture.context->platform().recover(fixture.context->platform().context,
		MISTER_RESOURCE_FPGA, &observation, 10) == MISTER_RESULT_PLATFORM);
	assert(fixture.recovery.create_calls == 1);
	assert(fixture.recovery.continue_calls == 0);
	assert(fixture.recovery.destroyed == 1);
	observation = RecoveryObservation();
	assert(fixture.context->platform().recover(fixture.context->platform().context,
		MISTER_RESOURCE_FPGA, &observation, 20) == MISTER_RESULT_INVALID_STATE);
	assert(fixture.recovery.create_calls == 1);
	assert(fixture.recovery.continue_calls == 0);
}

static void TestRecoveryRejectsMalformedAndIncompleteSuccess()
{
	{
		Fixture fixture;
		fixture.recovery.continue_result = MISTER_RESULT_OK;
		fixture.recovery.complete_value = false;
		fixture.recovery.neutral = MISTER_RESOURCE_FPGA;
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		assert(fixture.context->platform().recover(
			fixture.context->platform().context, MISTER_RESOURCE_FPGA,
			&observation, 10) == MISTER_RESULT_PLATFORM);
		assert(fixture.recovery.destroyed == 0);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_FPGA);
	}
	{
		Fixture fixture;
		fixture.recovery.continue_result = MISTER_RESULT_OK;
		fixture.recovery.complete_value = true;
		fixture.recovery.neutral = MISTER_RESOURCE_FPGA;
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		assert(fixture.context->platform().recover(
			fixture.context->platform().context,
			MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES,
			&observation, 10) == MISTER_RESULT_PLATFORM);
		assert(fixture.recovery.destroyed == 1);
	}
	const uint32_t masks[] = {
		MISTER_RESOURCE_FPGA,
		MISTER_RESOURCE_FPGA,
		MISTER_RESOURCE_FPGA,
		MISTER_RESOURCE_FPGA,
		MISTER_RESOURCE_FPGA,
		MISTER_RESOURCE_FPGA
	};
	for (size_t test = 0; test < 6; ++test) {
		Fixture fixture;
		fixture.recovery.continue_result = MISTER_RESULT_DEADLINE;
		fixture.recovery.complete_value = false;
		fixture.recovery.observed = MISTER_RESOURCE_FPGA;
		if (test == 0) fixture.recovery.neutral = MISTER_RESOURCE_FPGA;
		if (test == 1) fixture.recovery.observed = MISTER_RESOURCE_CONTENT;
		if (test == 2) fixture.recovery.output_abi = 1;
		if (test == 3) fixture.recovery.output_reserved = 1;
		if (test == 4) fixture.recovery.output_size =
			sizeof(MisterRecoveryObservationV2) - 1;
		if (test == 5) fixture.recovery.output_size =
			sizeof(MisterRecoveryObservationV2) + 8;
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		assert(fixture.context->platform().recover(
			fixture.context->platform().context, masks[test], &observation, 10) ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.recovery.destroyed == 0);
		assert(observation.abi_version == MISTER_RUNTIME_ABI_VERSION_V2);
		assert(observation.struct_size == sizeof(observation));
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == 0);
		for (size_t index = 0; index < 4; ++index)
			assert(observation.reserved[index] == 0);
	}
}

static void TestProfileValidation()
{
	TestClock clock(1);
	GenerationState generation;
	RecoveryState recovery;
	TestGenerationFactory generation_factory(generation);
	TestRecoveryFactory recovery_factory(recovery);
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	linux_native::NativeLinuxV2FixtureProfiles invalid = {nullptr, profiles.megadrive};
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, invalid,
		generation_factory, recovery_factory, &context) == MISTER_RESULT_INVALID_ARGUMENT);
	invalid.snes = profiles.snes;
	invalid.megadrive = profiles.snes;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, invalid,
		generation_factory, recovery_factory, &context) == MISTER_RESULT_INVALID_ARGUMENT);
	invalid.snes = profiles.megadrive;
	invalid.megadrive = profiles.snes;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, invalid,
		generation_factory, recovery_factory, &context) == MISTER_RESULT_INVALID_ARGUMENT);
}

static void TestRuntimeV2Integration()
{
	Fixture fixture(1000);
	fixture.generation.ready = 1;
	fixture.generation.resource_flags = MISTER_RESOURCE_V2_KNOWN;
	MisterRuntime *runtime = nullptr;
	assert(MisterRuntime_CreateV2(&fixture.context->platform(), &runtime) ==
		MISTER_RESULT_OK);
	assert(runtime != nullptr);
	assert(MisterRuntime_StartV2(runtime, 10) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch("megadrive", "MegaDrive", "bin");
	assert(MisterRuntime_LoadV2(runtime, &launch, 20) == MISTER_RESULT_OK);
	assert(fixture.generation.activate_deadline == 1020);
	assert(MisterRuntime_TickV2(runtime, 30) == MISTER_RESULT_OK);
	MisterObservationV2 observation = Observation();
	assert(MisterRuntime_ObserveV2(runtime, &observation, 40) == MISTER_RESULT_OK);
	assert(observation.ready == 1);
	assert(CopyView(observation.observed_core) == "MegaDrive");
	fixture.generation.idle_value = true;
	assert(MisterRuntime_StopV2(runtime, 50) == MISTER_RESULT_OK);
	assert(MisterRuntime_DestroyV2(&runtime) == MISTER_RESULT_OK);
	assert(runtime == nullptr);
}

static void TestConcreteCommittedAdapterGraphThroughCallbacks()
{
	NativePosixFileSystem host_clock;
	TestClock clock(host_clock.NowMs());
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	assert(profiles.snes != nullptr && profiles.megadrive != nullptr);
	GraphGenerationFactory generations(clock);
	RecoveryState recovery_state;
	TestRecoveryFactory recoveries(recovery_state);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	const MisterPlatformV2 &platform = context->platform();
	assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch("megadrive", "MegaDrive", "md");
	launch.content.sha256 = View(
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
	launch.content.size = 5;
	const MisterResult graph_load = platform.load(platform.context, &launch,
		kConcreteGraphCallbackBudgetMs);
	assert(graph_load == MISTER_RESULT_OK);
	assert(generations.last != nullptr && generations.last->programmed_same_handle());
	MisterObservationV2 observation = Observation();
	assert(platform.observe(platform.context, &observation, 100) == MISTER_RESULT_OK);
	assert(observation.ready == 1);
	assert(observation.resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(CopyView(observation.observed_core) == "MegaDrive");
	assert(platform.tick(platform.context, 1000) == MISTER_RESULT_OK);
	assert(generations.last->tick_calls() == 1);
	assert(generations.last->tick_deadline_propagated());
	MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
	for (size_t attempt = 0; attempt != 20 && generations.last != nullptr; ++attempt) {
		stopped = platform.stop(platform.context, 5000);
		assert(stopped == MISTER_RESULT_OK ||
			stopped == MISTER_RESULT_CLEANUP_INCOMPLETE);
		if (stopped == MISTER_RESULT_OK) break;
	}
	assert(stopped == MISTER_RESULT_OK);
	assert(generations.last == nullptr);
}

static void TestConcreteSnesActivationThroughCallbacks()
{
	NativePosixFileSystem host_clock;
	TestClock clock(host_clock.NowMs());
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GraphGenerationFactory generations(clock);
	RecoveryState recovery_state;
	TestRecoveryFactory recoveries(recovery_state);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	const MisterPlatformV2 &platform = context->platform();
	assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch("snes", "SNES", "sfc");
	launch.content.sha256 = View(
		"d85093739274b43a1adc2943315e15152f5204414c749d64be5e443105f43ca6");
	launch.content.size = 32768;
	char churn_path[] = ".fogcast-v2-unrelated-ancestor-churn.XXXXXX";
	assert(mkdtemp(churn_path) != nullptr);
	assert(rmdir(churn_path) == 0);
	std::atomic<bool> stop_churn(false);
	std::atomic<unsigned> churn_count(0);
	std::thread churn([&]() {
		while (!stop_churn.load(std::memory_order_acquire)) {
			assert(mkdir(churn_path, 0700) == 0);
			assert(rmdir(churn_path) == 0);
			churn_count.fetch_add(1, std::memory_order_relaxed);
		}
	});
	const MisterResult loaded = platform.load(platform.context, &launch,
		kConcreteGraphCallbackBudgetMs);
	stop_churn.store(true, std::memory_order_release);
	churn.join();
	assert(churn_count.load(std::memory_order_relaxed) != 0);
	assert(loaded == MISTER_RESULT_OK);
	assert(generations.last != nullptr);
	assert(generations.last->programmed_same_handle());
	MisterObservationV2 observation = Observation();
	assert(platform.observe(platform.context, &observation, 100) == MISTER_RESULT_OK);
	assert(observation.ready == 1);
	assert(observation.resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(CopyView(observation.observed_core) == "SNES");
	assert(platform.tick(platform.context, 1000) == MISTER_RESULT_OK);
	assert(generations.last->tick_deadline_propagated());
	MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
	for (size_t attempt = 0; attempt != 8 && generations.last != nullptr; ++attempt) {
		stopped = platform.stop(platform.context, 5000);
		assert(stopped == MISTER_RESULT_OK ||
			stopped == MISTER_RESULT_CLEANUP_INCOMPLETE);
	}
	assert(stopped == MISTER_RESULT_OK);
	assert(generations.last == nullptr);
}

static void TestConcreteIncompleteStopRetainsAndRetriesExactProduct()
{
	NativePosixFileSystem host_clock;
	TestClock clock(host_clock.NowMs());
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GraphGenerationFactory generations(clock);
	RecoveryState recovery_state;
	TestRecoveryFactory recoveries(recovery_state);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	const MisterPlatformV2 &platform = context->platform();
	assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
	MisterLaunchV2 launch = Launch("megadrive", "MegaDrive", "md");
	launch.content.sha256 = View(
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
	launch.content.size = 5;
	assert(platform.load(platform.context, &launch, kConcreteGraphCallbackBudgetMs) ==
		MISTER_RESULT_OK);
	assert(platform.tick(platform.context, 1000) == MISTER_RESULT_OK);
	GraphGeneration *const retained = generations.last;
	assert(retained != nullptr);
	retained->FailNextProtocolRelease();
	assert(platform.stop(platform.context, 5000) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(generations.last == retained);
	assert(retained->neutral_replay_count() == 1);
	assert(retained->neutral_replay_deadline() != 0);
	assert(retained->neutral_replay_deadline() <= retained->stop_deadline());
	assert(platform.stop(platform.context, 5000) == MISTER_RESULT_OK);
	assert(generations.last == nullptr);
}

static MisterLaunchV2 ConcreteMegaDriveLaunch()
{
	MisterLaunchV2 launch = Launch("megadrive", "MegaDrive", "md");
	launch.content.sha256 = View(
		"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
	launch.content.size = 5;
	return launch;
}

static MisterLaunchV2 ConcreteSnesLaunch()
{
	MisterLaunchV2 launch = Launch("snes", "SNES", "sfc");
	launch.content.sha256 = View(
		"d85093739274b43a1adc2943315e15152f5204414c749d64be5e443105f43ca6");
	launch.content.size = 32768;
	return launch;
}

static void AssertFinalGraphBaseline(const GraphFinalBaseline &final)
{
	assert(final.destroyed);
	assert(!final.mappings_held);
	assert(!final.mmio_descriptor_open);
	assert(!final.content_descriptors_held);
	assert(!final.core_descriptors_held);
	assert(!final.protocol_descriptor_open);
	assert(final.protocol_mappings == 0);
	assert(final.input_descriptors == 0);
	assert(final.save_descriptors == 0);
	assert(final.scheduler_initializes == final.scheduler_releases);
	assert(final.offload_initializes == final.offload_releases);
	assert(final.ledger.resource_flags == 0);
	assert(!final.ledger.generation);
	assert(!final.ledger.containment_mappings);
	assert(!final.ledger.scheduler);
	assert(!final.ledger.offload);
	assert(!final.ledger.input_descriptors);
	assert(!final.ledger.coupled_audio_video_active);
	assert(final.lifecycle_state == NativeLifecycleState::idle);
	assert(final.lifecycle_generation == 0);
	assert(final.tick_calls >= 0);
	assert(final.broker_exact_idle);
	assert(!final.broker_has_live_generation);
	assert(final.broker.active_lease_count == 0);
	assert(final.broker.terminal_lease_count == 0);
	assert(!final.broker.invocation_registered);
	assert(!final.broker.cleanup_registered);
	assert(!final.broker.hardware_transaction_active);
	assert(final.broker.broker_idle);
}

static void AssertRepeatedGraphCycleBaseline(const GraphFinalBaseline &final)
{
	AssertFinalGraphBaseline(final);
	assert(final.input_close_calls == 3);
	assert(final.save_close_calls == 5);
	assert(final.neutral_replays == 1);
	assert(final.tick_calls == 1);
	assert(final.protocol_mappings == 0);
	assert(final.broker.registration_identity == 0);
	assert(final.broker.session_identity == 0);
	assert(final.broker.lease_identity == 0);
	assert(final.broker.invocation_identity == 0);
	assert(final.content_result == MISTER_RESULT_OK);
	assert(final.core_result == NativeArtifactResult::ok);
	assert(final.activate_return == MISTER_RESULT_OK);
	assert(final.idle_return);
}

static NativeResourceLedger ExpectedReleaseLedger(GraphFaultBoundary boundary)
{
	NativeResourceLedger expected = {};
	expected.resource_flags = MISTER_RESOURCE_V2_KNOWN;
	expected.generation = true;
	expected.containment_mappings = true;
	expected.scheduler = true;
	expected.offload = true;
	expected.input_descriptors = true;
	expected.coupled_audio_video_active = true;
	if (boundary == GraphFaultBoundary::release_scheduler) return expected;
	expected.scheduler = false;
	if (boundary == GraphFaultBoundary::release_offload) return expected;
	expected.offload = false;
	if (boundary == GraphFaultBoundary::release_save) return expected;
	expected.resource_flags &= ~MISTER_RESOURCE_SAVES;
	if (boundary == GraphFaultBoundary::capture_input) return expected;
	expected.digital_neutral_captured = true;
	expected.digital_neutral_valid[0] = true;
	expected.digital_neutral[0] = {0, {0x02, 0}};
	if (boundary == GraphFaultBoundary::replay_input) return expected;
	expected.digital_neutral_valid[0] = false;
	if (boundary == GraphFaultBoundary::release_input_descriptors) return expected;
	expected.input_descriptors = false;
	if (boundary == GraphFaultBoundary::release_video) return expected;
	expected.video_shutdown_complete = true;
	if (boundary == GraphFaultBoundary::release_audio) return expected;
	expected.audio_shutdown_complete = true;
	if (boundary == GraphFaultBoundary::release_audio_video) return expected;
	expected.coupled_audio_video_active = false;
	expected.video_shutdown_complete = true;
	expected.resource_flags &= ~MISTER_RESOURCE_NATIVE_VIDEO;
	if (boundary == GraphFaultBoundary::release_content) return expected;
	expected.resource_flags &= ~MISTER_RESOURCE_CONTENT;
	if (boundary == GraphFaultBoundary::release_protocol) return expected;
	expected.core_protocol_shutdown_complete = true;
	return expected;
}

static NativeResourceLedger ExpectedAcquisitionLedger(GraphFaultBoundary boundary,
	bool after)
{
	NativeResourceLedger expected = {};
	if (boundary == GraphFaultBoundary::preflight) return expected;
	if (boundary == GraphFaultBoundary::content) {
		if (after) expected.resource_flags = MISTER_RESOURCE_CONTENT;
		return expected;
	}
	expected.resource_flags = MISTER_RESOURCE_CONTENT;
	expected.generation = true;
	if (boundary == GraphFaultBoundary::mappings) {
		if (after) {
			expected.resource_flags |= MISTER_RESOURCE_FPGA;
			expected.containment_mappings = true;
		}
		return expected;
	}
	expected.resource_flags |= MISTER_RESOURCE_FPGA;
	expected.containment_mappings = true;
	if (boundary == GraphFaultBoundary::offload) {
		expected.offload = after;
		return expected;
	}
	expected.offload = true;
	if (boundary == GraphFaultBoundary::fpga) return expected;
	if (boundary == GraphFaultBoundary::bridges) {
		if (after) expected.resource_flags |= MISTER_RESOURCE_BRIDGES;
		return expected;
	}
	expected.resource_flags |= MISTER_RESOURCE_BRIDGES;
	if (boundary == GraphFaultBoundary::protocol) {
		if (after) expected.resource_flags |= MISTER_RESOURCE_CORE_PROTOCOL;
		return expected;
	}
	expected.resource_flags |= MISTER_RESOURCE_CORE_PROTOCOL;
	if (boundary == GraphFaultBoundary::video) {
		if (after) expected.resource_flags |= MISTER_RESOURCE_NATIVE_VIDEO;
		return expected;
	}
	expected.resource_flags |= MISTER_RESOURCE_NATIVE_VIDEO;
	if (boundary == GraphFaultBoundary::audio) {
		if (after) expected.resource_flags |= MISTER_RESOURCE_NATIVE_AUDIO;
		return expected;
	}
	expected.resource_flags |= MISTER_RESOURCE_NATIVE_AUDIO;
	if (boundary == GraphFaultBoundary::audio_video) {
		expected.coupled_audio_video_active = after;
		return expected;
	}
	expected.coupled_audio_video_active = true;
	if (boundary == GraphFaultBoundary::input_descriptors) {
		expected.input_descriptors = after;
		if (after) expected.resource_flags |= MISTER_RESOURCE_CORE_INPUT;
		return expected;
	}
	expected.input_descriptors = true;
	expected.resource_flags |= MISTER_RESOURCE_CORE_INPUT;
	if (boundary == GraphFaultBoundary::save) {
		if (after) expected.resource_flags |= MISTER_RESOURCE_SAVES;
		return expected;
	}
	expected.resource_flags |= MISTER_RESOURCE_SAVES;
	if (boundary == GraphFaultBoundary::scheduler)
		expected.scheduler = after;
	return expected;
}

static void AssertLedger(const NativeResourceLedger &actual,
	const NativeResourceLedger &expected)
{
	assert(actual.resource_flags == expected.resource_flags);
	assert(actual.generation == expected.generation);
	assert(actual.containment_mappings == expected.containment_mappings);
	assert(actual.scheduler == expected.scheduler);
	assert(actual.offload == expected.offload);
	assert(actual.input_descriptors == expected.input_descriptors);
	assert(actual.core_protocol_shutdown_complete ==
		expected.core_protocol_shutdown_complete);
	assert(actual.video_shutdown_complete == expected.video_shutdown_complete);
	assert(actual.audio_shutdown_complete == expected.audio_shutdown_complete);
	assert(actual.coupled_audio_video_active ==
		expected.coupled_audio_video_active);
	assert(actual.digital_neutral_captured == expected.digital_neutral_captured);
	for (size_t player = 0; player != kNativePlayerCount; ++player) {
		assert(actual.digital_neutral_valid[player] ==
			expected.digital_neutral_valid[player]);
		if (expected.digital_neutral_valid[player]) {
			assert(actual.digital_neutral[player].player ==
				expected.digital_neutral[player].player);
			assert(actual.digital_neutral[player].words[0] ==
				expected.digital_neutral[player].words[0]);
			assert(actual.digital_neutral[player].words[1] ==
				expected.digital_neutral[player].words[1]);
		}
	}
}

static void TestConcreteAcquisitionFaultMatrix()
{
	const GraphFaultBoundary boundaries[] = {GraphFaultBoundary::preflight,
		GraphFaultBoundary::content, GraphFaultBoundary::mappings,
		GraphFaultBoundary::offload, GraphFaultBoundary::fpga,
		GraphFaultBoundary::bridges, GraphFaultBoundary::protocol,
		GraphFaultBoundary::video, GraphFaultBoundary::audio,
		GraphFaultBoundary::audio_video,
		GraphFaultBoundary::input_descriptors, GraphFaultBoundary::save,
		GraphFaultBoundary::scheduler};
	for (GraphFaultBoundary boundary : boundaries) for (bool after : {false, true}) {
		NativePosixFileSystem host_clock;
		TestClock clock(host_clock.NowMs());
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GraphGenerationFactory generations(clock);
		generations.fault = {boundary, after, false};
		RecoveryState recovery_state;
		TestRecoveryFactory recoveries(recovery_state);
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		const MisterPlatformV2 &platform = context->platform();
		assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
		MisterLaunchV2 launch = ConcreteMegaDriveLaunch();
		assert(platform.load(platform.context, &launch, 5000) == MISTER_RESULT_PLATFORM);
		assert(generations.last != nullptr && generations.last->fault_fired());
		const NativeResourceLedger expected_activation =
			ExpectedAcquisitionLedger(boundary, after);
		AssertLedger(generations.last->fault_ledger(), expected_activation);
		if (boundary == GraphFaultBoundary::video ||
			boundary == GraphFaultBoundary::audio ||
			boundary == GraphFaultBoundary::audio_video) {
			const int preceding = boundary == GraphFaultBoundary::video ? 0 :
				boundary == GraphFaultBoundary::audio ? 1 : 2;
			assert(generations.last->fault_av_begins() == preceding +
				(after ? 1 : 0));
			if (!after && boundary == GraphFaultBoundary::video)
				assert(generations.last->fault_av_words() == 0);
		}
		const NativeResourceLedger partial = generations.last->ledger();
		assert(partial.resource_flags == 0);
		assert(!partial.containment_mappings && !partial.scheduler &&
			!partial.offload && !partial.input_descriptors);
		MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
		for (size_t retry = 0; retry != 16 && generations.last != nullptr; ++retry)
			stopped = platform.stop(platform.context, 5000);
		assert(stopped == MISTER_RESULT_OK);
		assert(generations.last == nullptr);
		AssertFinalGraphBaseline(generations.final);
	}
}

static void TestConcreteReleaseFaultMatrix()
{
	const GraphFaultBoundary boundaries[] = {GraphFaultBoundary::release_scheduler,
		GraphFaultBoundary::release_offload, GraphFaultBoundary::release_save,
		GraphFaultBoundary::capture_input, GraphFaultBoundary::replay_input,
		GraphFaultBoundary::release_input_descriptors,
		GraphFaultBoundary::release_video, GraphFaultBoundary::release_audio_video,
		GraphFaultBoundary::release_audio, GraphFaultBoundary::release_protocol,
		GraphFaultBoundary::release_content, GraphFaultBoundary::terminal};
	for (GraphFaultBoundary boundary : boundaries) for (bool after : {false, true}) {
		NativePosixFileSystem host_clock;
		TestClock clock(host_clock.NowMs());
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GraphGenerationFactory generations(clock);
		RecoveryState recovery_state;
		TestRecoveryFactory recoveries(recovery_state);
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		const MisterPlatformV2 &platform = context->platform();
		assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
		MisterLaunchV2 launch = ConcreteMegaDriveLaunch();
		assert(platform.load(platform.context, &launch,
			kConcreteGraphCallbackBudgetMs) == MISTER_RESULT_OK);
		assert(platform.tick(platform.context, 1000) == MISTER_RESULT_OK);
		GraphGeneration *const retained = generations.last;
		const int av_begins_before_stop = retained->av_begins();
		const size_t av_words_before_stop = retained->av_words();
		retained->ConfigureFault(boundary, after);
		assert(platform.stop(platform.context, 5000) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(generations.last == retained && retained->fault_fired());
		AssertLedger(retained->ledger(), ExpectedReleaseLedger(boundary));
		const NativeCleanupTiming timing = retained->cleanup_timing();
		const uint64_t cleanup_identity = retained->cleanup_identity();
		assert(timing.established && cleanup_identity != 0);
		assert(timing.cleanup_start_ms < timing.non_fpga_deadline_ms);
		assert(timing.non_fpga_deadline_ms <= timing.fpga_deadline_ms);
		assert(timing.non_fpga_deadline_ms <= retained->stop_deadline());
		assert(timing.fpga_deadline_ms >= timing.non_fpga_deadline_ms);
		if (boundary == GraphFaultBoundary::release_input_descriptors) {
			assert(retained->input_descriptors() == (after ? 0 : 3));
			assert(retained->input_close_calls() == (after ? 3 : 0));
		}
		if (boundary == GraphFaultBoundary::release_video ||
			boundary == GraphFaultBoundary::release_audio ||
			boundary == GraphFaultBoundary::release_audio_video) {
			const PeripheralSessionKind kind =
				boundary == GraphFaultBoundary::release_video ?
				PeripheralSessionKind::video :
				boundary == GraphFaultBoundary::release_audio ?
				PeripheralSessionKind::audio :
				PeripheralSessionKind::audio_video;
			const NativeCleanupBrokerSnapshot snapshot =
				retained->cleanup_snapshot(kind);
			assert(snapshot.lease_identity != 0);
			assert(snapshot.registration_identity != 0);
			assert(snapshot.session_identity != 0);
			assert(snapshot.cleanup_identity == cleanup_identity);
			assert(snapshot.cleanup_non_fpga_deadline_ms ==
				timing.non_fpga_deadline_ms);
			assert(snapshot.cleanup_fpga_deadline_ms == timing.fpga_deadline_ms);
			assert(snapshot.active_lease_count == 1);
			assert(snapshot.terminal_lease_count == 0);
			assert(snapshot.session_effective_deadline_ms == 0);
			assert(snapshot.session_kind == kind);
			assert(snapshot.session_phase == PeripheralSessionPhase::abandoned);
			assert(snapshot.backend_matches_expected);
			assert(snapshot.registration_is_suspended);
			assert(!snapshot.registration_is_invoked);
			assert(!snapshot.invocation_registered);
			assert(snapshot.cleanup_registered);
			assert(snapshot.hardware_transaction_active);
			assert(snapshot.recheckout_allowed);
			const int preceding = boundary == GraphFaultBoundary::release_video ? 0 :
				boundary == GraphFaultBoundary::release_audio ? 1 : 2;
			assert(retained->av_begins() == av_begins_before_stop + preceding +
				(after ? 1 : 0));
			assert(retained->av_words() >= av_words_before_stop);
		}
		const size_t replays_after_failure = retained->neutral_replay_count();
		MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
		for (size_t retry = 0; retry != 16 && generations.last != nullptr; ++retry)
			stopped = platform.stop(platform.context, 5000);
		assert(stopped == MISTER_RESULT_OK);
		assert(generations.last == nullptr);
		const size_t expected_replays =
			boundary == GraphFaultBoundary::replay_input && !after ? 2 : 1;
		assert(generations.final.neutral_replays == expected_replays);
		assert(replays_after_failure <= expected_replays);
		assert(generations.final.input_close_calls == 3);
		AssertFinalGraphBaseline(generations.final);
	}
}

static void TestConcreteNativeRecoveryProductThroughCallback()
{
	TestClock clock(100);
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GenerationState generation_state;
	TestGenerationFactory generations(generation_state);
	GraphRecoveryFactory recoveries(clock);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = RecoveryObservation();
	assert(context->platform().recover(context->platform().context,
		MISTER_RESOURCE_CONTENT, &observation, 1000) == MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CONTENT);
	assert(recoveries.creates == 1);
}

static void TestConcreteNativeRecoveryAllMaskThroughCallbacks()
{
	TestClock clock(100);
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GenerationState generation_state;
	TestGenerationFactory generations(generation_state);
	GraphRecoveryFactory recoveries(clock);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = RecoveryObservation();
	MisterResult result = MISTER_RESULT_CLEANUP_INCOMPLETE;
	for (size_t callback = 0; callback != 16; ++callback) {
		observation = RecoveryObservation();
		result = context->platform().recover(context->platform().context,
			MISTER_RESOURCE_V2_KNOWN, &observation, 1000);
		assert(result == MISTER_RESULT_OK ||
			result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert((observation.observed_resource_flags &
			observation.neutral_resource_flags) == 0);
		assert(((observation.observed_resource_flags |
			observation.neutral_resource_flags) & ~MISTER_RESOURCE_V2_KNOWN) == 0);
		if (result == MISTER_RESULT_OK) break;
	}
	assert(result == MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(recoveries.creates == 1);
	const OperationKind expected[] = {OperationKind::input_descriptors,
		OperationKind::save, OperationKind::content, OperationKind::audio_video,
		OperationKind::audio, OperationKind::terminal_fpga_cleanup};
	assert(recoveries.trace.size() == sizeof(expected) / sizeof(expected[0]));
	for (size_t index = 0; index != recoveries.trace.size(); ++index)
		assert(recoveries.trace[index] == expected[index]);
}

static void TestConcreteNativeRecoveryCoreProtocolOnly()
{
	TestClock clock(100);
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GenerationState generation_state;
	TestGenerationFactory generations(generation_state);
	GraphRecoveryFactory recoveries(clock);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = RecoveryObservation();
	assert(context->platform().recover(context->platform().context,
		MISTER_RESOURCE_CORE_PROTOCOL, &observation, 1000) == MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_PROTOCOL);
	assert(recoveries.trace.size() == 1);
	assert(recoveries.trace[0] == OperationKind::core_protocol);
}

static void TestConcreteNativeRecoveryRetryRetainsAttempt()
{
	TestClock clock(100);
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GenerationState generation_state;
	TestGenerationFactory generations(generation_state);
	GraphRecoveryFactory recoveries(clock);
	recoveries.fail_content_once = true;
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = RecoveryObservation();
	assert(context->platform().recover(context->platform().context,
		MISTER_RESOURCE_CONTENT, &observation, 1000) == MISTER_RESULT_DEADLINE);
	assert(observation.observed_resource_flags == MISTER_RESOURCE_CONTENT);
	assert(observation.neutral_resource_flags == 0);
	observation = RecoveryObservation();
	assert(context->platform().recover(context->platform().context,
		MISTER_RESOURCE_CONTENT, &observation, 1000) == MISTER_RESULT_OK);
	assert(observation.observed_resource_flags == 0);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CONTENT);
	assert(recoveries.creates == 1);
}

static uint64_t OffsetDeadline(uint64_t deadline, int offset)
{
	return offset < 0 ? deadline - 1 :
		deadline + static_cast<uint64_t>(offset);
}

static void AssertExactRecoveryObservation(
	const MisterRecoveryObservationV2 &observation, uint32_t requested);

static void TestComposedLoadUnwindDeadlineMatrix()
{
	for (bool group_shorter : {false, true}) for (int offset = -1; offset <= 1;
		++offset) {
		NativePosixFileSystem host_clock;
		TestClock clock(host_clock.NowMs());
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GraphGenerationFactory generations(clock);
		generations.fault = {GraphFaultBoundary::scheduler, true, false};
		const uint64_t callback_deadline = clock.NowMs() +
			(group_shorter ? 3000 : 100);
		const uint64_t expected_group_deadline = clock.NowMs() + 2000;
		const uint64_t effective_deadline = group_shorter ?
			expected_group_deadline : callback_deadline;
		generations.load_deadline_kind = GraphOrdinaryDeadlineKind::content;
		generations.load_deadline_target = OffsetDeadline(effective_deadline, offset);
		RecoveryState recovery_state;
		TestRecoveryFactory recoveries(recovery_state);
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		const MisterPlatformV2 &platform = context->platform();
		assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
		MisterLaunchV2 launch = ConcreteMegaDriveLaunch();
		assert(platform.load(platform.context, &launch,
			group_shorter ? 3000 : 100) == MISTER_RESULT_PLATFORM);
		GraphGeneration *const product = generations.last;
		assert(product != nullptr && product->fault_fired() &&
			product->ordinary_deadline_consumed());
		const NativeCleanupTiming timing = product->cleanup_timing();
		assert(timing.established == (offset >= 0));
		if (timing.established) {
			assert(timing.non_fpga_deadline_ms == timing.cleanup_start_ms + 2000);
			assert(timing.fpga_deadline_ms == timing.cleanup_start_ms + 5000);
		}
		assert(product->ordinary_effective_deadline() == effective_deadline);
		assert(product->scheduler_initializes() == 1 &&
			product->offload_initializes() == 1 &&
			product->scheduler_releases() == 1 &&
			product->offload_releases() == 1);
		if (offset < 0) {
			assert(!product->activation_ledger().generation);
			assert(!product->content_descriptors_held() &&
				!product->core_descriptors_held());
		} else {
			assert(product->activation_ledger().generation);
			assert(product->content_descriptors_held() ||
				product->core_descriptors_held());
		}
		if (!group_shorter && offset >= 0) {
			clock.Set(timing.cleanup_start_ms + 1);
			MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
			for (size_t retry = 0; retry != 16 && generations.last != nullptr; ++retry)
				stopped = platform.stop(platform.context, 5000);
			assert(stopped == MISTER_RESULT_OK && generations.last == nullptr);
			AssertFinalGraphBaseline(generations.final);
		} else {
			context.reset();
			assert(generations.last == nullptr);
		}
	}
}

static void TestComposedOrdinaryCleanupDeadlineMatrix()
{
	const GraphOrdinaryDeadlineKind kinds[] = {
		GraphOrdinaryDeadlineKind::scheduler,
		GraphOrdinaryDeadlineKind::offload,
		GraphOrdinaryDeadlineKind::capture_input,
		GraphOrdinaryDeadlineKind::replay_input,
		GraphOrdinaryDeadlineKind::input_descriptors,
		GraphOrdinaryDeadlineKind::content};
	const GraphFaultBoundary boundaries[] = {
		GraphFaultBoundary::release_scheduler,
		GraphFaultBoundary::release_offload,
		GraphFaultBoundary::capture_input,
		GraphFaultBoundary::replay_input,
		GraphFaultBoundary::release_input_descriptors,
		GraphFaultBoundary::release_content};
	for (size_t kind_index = 0; kind_index != sizeof(kinds) / sizeof(kinds[0]);
		++kind_index) for (bool group_shorter : {false, true})
		for (int offset = -1; offset <= 1; ++offset) {
			NativePosixFileSystem host_clock;
			TestClock clock(host_clock.NowMs());
			linux_native::NativeLinuxV2FixtureProfiles profiles = {
				FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
			GraphGenerationFactory generations(clock);
			RecoveryState recovery_state;
			TestRecoveryFactory recoveries(recovery_state);
			std::unique_ptr<linux_native::NativeLinuxV2Context> context;
			assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock,
				profiles, generations, recoveries, &context) == MISTER_RESULT_OK);
			const MisterPlatformV2 &platform = context->platform();
			assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
			MisterLaunchV2 launch = ConcreteMegaDriveLaunch();
			assert(platform.load(platform.context, &launch,
				kConcreteGraphCallbackBudgetMs) == MISTER_RESULT_OK);
			assert(platform.tick(platform.context, 1000) == MISTER_RESULT_OK);
			GraphGeneration *const product = generations.last;
			uint64_t effective_deadline = clock.NowMs() + 100;
			if (group_shorter) {
				product->ConsumeOrdinaryDeadlineAt(kinds[kind_index],
					effective_deadline);
				assert(platform.stop(platform.context, 100) == MISTER_RESULT_DEADLINE);
				assert(product->ordinary_deadline_consumed());
				const NativeCleanupTiming timing = product->cleanup_timing();
				effective_deadline = timing.non_fpga_deadline_ms;
				clock.Set(timing.cleanup_start_ms + 1);
				product->ConsumeOrdinaryDeadlineAt(kinds[kind_index],
					OffsetDeadline(effective_deadline, offset));
				const uint64_t remaining = effective_deadline - clock.NowMs() + 1000;
				assert(remaining <= UINT32_MAX);
				const MisterResult result = platform.stop(platform.context,
					static_cast<uint32_t>(remaining));
				const uint64_t observed_deadline = generations.last != nullptr ?
					product->ordinary_effective_deadline() :
					generations.final.ordinary_effective_deadline;
				assert(observed_deadline == effective_deadline);
				if (offset < 0)
					assert(result == MISTER_RESULT_OK && generations.last == nullptr);
				else {
					assert(result == MISTER_RESULT_DEADLINE && generations.last == product);
					AssertLedger(product->ledger(), ExpectedReleaseLedger(
						boundaries[kind_index]));
				}
			} else {
				product->ConsumeOrdinaryDeadlineAt(kinds[kind_index],
					OffsetDeadline(effective_deadline, offset));
				const MisterResult result = platform.stop(platform.context, 100);
				const bool consumed = generations.last != nullptr ?
					product->ordinary_deadline_consumed() :
					generations.final.ordinary_deadline_consumed;
				const uint64_t observed_deadline = generations.last != nullptr ?
					product->ordinary_effective_deadline() :
					generations.final.ordinary_effective_deadline;
				assert(consumed && observed_deadline == effective_deadline);
				if (offset < 0)
					assert(result == MISTER_RESULT_OK && generations.last == nullptr);
				else {
					assert(result == MISTER_RESULT_DEADLINE && generations.last == product);
					AssertLedger(product->ledger(), ExpectedReleaseLedger(
						boundaries[kind_index]));
					clock.Set(product->cleanup_timing().cleanup_start_ms + 1);
					MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
					for (size_t retry = 0; retry != 16 && generations.last != nullptr;
						++retry) stopped = platform.stop(platform.context, 5000);
					assert(stopped == MISTER_RESULT_OK && generations.last == nullptr);
				}
			}
			if (generations.last == nullptr)
				AssertFinalGraphBaseline(generations.final);
			else {
				context.reset();
				assert(generations.last == nullptr);
			}
		}
}

static void TestComposedRecoveryObservationDeadlineMatrix()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	for (bool group_shorter : {false, true}) for (int offset = -1; offset <= 1;
		++offset) {
		TestClock clock(1000);
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GenerationState generation_state;
		TestGenerationFactory generations(generation_state);
		GraphRecoveryFactory recoveries(clock);
		recoveries.deadline_kind = GraphRecoveryDeadlineKind::observation;
		recoveries.deadline_offset = offset;
		recoveries.deadline_uses_group = group_shorter;
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		MisterResult result = context->platform().recover(
			context->platform().context, closure, &observation,
			group_shorter ? UINT32_MAX : 100);
		AssertExactRecoveryObservation(observation, closure);
		assert(recoveries.creates == 1 && recoveries.destroys == 0 &&
			recoveries.evidence.observation_calls == 1 &&
			recoveries.evidence.observation_mmio_events > 0);
		if (!group_shorter && offset >= 0) {
			assert(result == MISTER_RESULT_DEADLINE);
			observation.observed_resource_flags = 0;
			observation.neutral_resource_flags = 0;
			clock.Set(clock.NowMs() + 1);
			observation = RecoveryObservation();
			result = context->platform().recover(context->platform().context,
				closure, &observation, 5000);
			AssertExactRecoveryObservation(observation, closure);
			assert(recoveries.evidence.observation_calls == 2);
		}
		if (group_shorter && offset >= 0) {
			assert(result == MISTER_RESULT_DEADLINE);
			const GraphRecoveryEvidenceState before = recoveries.evidence;
			clock.Set(clock.NowMs() + 1);
			observation = RecoveryObservation();
			assert(context->platform().recover(context->platform().context,
				closure, &observation, UINT32_MAX) == MISTER_RESULT_DEADLINE);
			assert(recoveries.evidence.observation_calls == 2);
			assert(recoveries.evidence.observation_mmio_events ==
				before.observation_mmio_events);
			AssertSameGraphRecoveryResources(before, recoveries.evidence);
			context.reset();
			assert(recoveries.destroys == 1);
			continue;
		}
		for (size_t retry = 0; retry != 16 && result != MISTER_RESULT_OK; ++retry) {
			observation = RecoveryObservation();
			result = context->platform().recover(context->platform().context,
				closure, &observation, group_shorter ? UINT32_MAX : 5000);
			assert(result == MISTER_RESULT_OK ||
				result == MISTER_RESULT_CLEANUP_INCOMPLETE);
			AssertExactRecoveryObservation(observation, closure);
		}
		assert(result == MISTER_RESULT_OK &&
			observation.observed_resource_flags == 0 &&
			observation.neutral_resource_flags == closure &&
			recoveries.destroys == 1 && recoveries.evidence.baseline_captured);
		assert(recoveries.evidence.observation_calls ==
			(!group_shorter && offset >= 0 ? 2 : 1));
		AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
	}
}

static void TestComposedTypedCleanupCallbackDeadlineMatrix()
{
	typedef void (GraphGeneration::*DeadlineArm)(uint64_t);
	const DeadlineArm arms[] = {&GraphGeneration::ConsumeSaveDeadlineAt,
		&GraphGeneration::ConsumeVideoDeadlineAt,
		&GraphGeneration::ConsumeAudioDeadlineAt,
		&GraphGeneration::ConsumeAudioVideoDeadlineAt,
		&GraphGeneration::ConsumeCoreDeadlineAt};
	const OperationKind kinds[] = {OperationKind::save, OperationKind::video,
		OperationKind::audio, OperationKind::audio_video,
		OperationKind::core_protocol};
	for (size_t kind_index = 0; kind_index != sizeof(kinds) / sizeof(kinds[0]);
		++kind_index) for (int offset = -1; offset <= 1; ++offset) {
		NativePosixFileSystem host_clock;
		TestClock clock(host_clock.NowMs());
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GraphGenerationFactory generations(clock);
		RecoveryState recovery_state;
		TestRecoveryFactory recoveries(recovery_state);
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		const MisterPlatformV2 &platform = context->platform();
		assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
		MisterLaunchV2 launch = ConcreteMegaDriveLaunch();
		assert(platform.load(platform.context, &launch,
			kConcreteGraphCallbackBudgetMs) == MISTER_RESULT_OK);
		GraphGeneration *const product = generations.last;
		const uint64_t callback_deadline = clock.NowMs() + 100;
		(product->*arms[kind_index])(static_cast<uint64_t>(static_cast<int64_t>(callback_deadline) +
			offset));
		const MisterResult result = platform.stop(platform.context, 100);
		if (offset < 0) {
			assert(result == MISTER_RESULT_OK);
			assert(generations.last == nullptr);
			AssertFinalGraphBaseline(generations.final);
			continue;
		}
		assert(result == MISTER_RESULT_DEADLINE ||
			result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(generations.last == product);
		const NativeRetainedOperationSnapshot live =
			product->cleanup_live_snapshot();
		NativeRetainedOperationSnapshot suspended = {};
		assert(product->cleanup_retained_snapshot(kinds[kind_index], &suspended) ==
			MISTER_RESULT_OK);
		assert(live.query_valid && live.retained && live.typed_registration_present &&
			live.registration_is_invoked && live.invocation_registered &&
			live.authority == LeaseAuthority::cleanup_epoch &&
			live.supplied_operation_kind == kinds[kind_index] &&
			live.retained_operation_kind == kinds[kind_index] &&
			live.registration_effective_deadline_ms == callback_deadline &&
			live.invocation_callback_deadline_ms == callback_deadline);
		assert(suspended.retained && suspended.registration_is_suspended &&
			suspended.registration_effective_deadline_ms == 0);
		const NativeCleanupTiming timing = product->cleanup_timing();
		assert(timing.established && timing.cleanup_start_ms < timing.non_fpga_deadline_ms);
		assert(live.authority_identity == product->cleanup_identity() &&
			live.non_fpga_deadline_ms == timing.non_fpga_deadline_ms &&
			live.fpga_deadline_ms == timing.fpga_deadline_ms &&
			live.registration_authority_deadline_ms ==
				(kinds[kind_index] == OperationKind::core_protocol ?
				 timing.fpga_deadline_ms : timing.non_fpga_deadline_ms) &&
			live.lease_identity != 0 && live.registration_identity != 0 &&
			live.backend_applicable && live.backend_matches_expected);
		AssertOnlyRetainedRebindFieldsChanged(live, suspended);
		clock.Set(timing.cleanup_start_ms + 1);
		(product->*arms[kind_index])(clock.NowMs());
		const uint64_t retry_callback_deadline = clock.NowMs() + 5000;
		MisterResult stopped = platform.stop(platform.context, 5000);
		assert(stopped == MISTER_RESULT_OK ||
			stopped == MISTER_RESULT_CLEANUP_INCOMPLETE);
		const NativeRetainedOperationSnapshot rebound = generations.last != nullptr ?
			product->cleanup_live_snapshot() : generations.final.retained_live_snapshot;
		const uint64_t authority_deadline = kinds[kind_index] ==
			OperationKind::core_protocol ? timing.fpga_deadline_ms :
			timing.non_fpga_deadline_ms;
		assert(rebound.invocation_identity != live.invocation_identity &&
			rebound.invocation_callback_deadline_ms == retry_callback_deadline &&
			rebound.registration_effective_deadline_ms ==
				(retry_callback_deadline < authority_deadline ?
				 retry_callback_deadline : authority_deadline));
		AssertOnlyRetainedRebindFieldsChanged(suspended, rebound);
		for (size_t retry = 1; retry != 16 && generations.last != nullptr; ++retry) {
			stopped = platform.stop(platform.context, 5000);
			assert(stopped == MISTER_RESULT_OK ||
				stopped == MISTER_RESULT_CLEANUP_INCOMPLETE);
		}
		assert(stopped == MISTER_RESULT_OK && generations.last == nullptr);
		AssertFinalGraphBaseline(generations.final);
	}
}

static void TestComposedTypedRecoveryCallbackDeadlineMatrix()
{
	const GraphRecoveryDeadlineKind kinds[] = {GraphRecoveryDeadlineKind::save,
		GraphRecoveryDeadlineKind::video, GraphRecoveryDeadlineKind::audio,
		GraphRecoveryDeadlineKind::audio_video,
		GraphRecoveryDeadlineKind::core_protocol};
	const OperationKind operation_kinds[] = {OperationKind::save,
		OperationKind::video, OperationKind::audio, OperationKind::audio_video,
		OperationKind::core_protocol};
	const uint32_t masks[] = {MISTER_RESOURCE_SAVES, MISTER_RESOURCE_NATIVE_VIDEO,
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL,
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO |
			MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
			MISTER_RESOURCE_CORE_PROTOCOL,
		MISTER_RESOURCE_CORE_PROTOCOL};
	for (size_t kind_index = 0; kind_index != sizeof(kinds) / sizeof(kinds[0]);
		++kind_index) for (int offset = -1; offset <= 1; ++offset) {
		TestClock clock(1000);
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GenerationState generation_state;
		TestGenerationFactory generations(generation_state);
		GraphRecoveryFactory recoveries(clock);
		recoveries.deadline_kind = kinds[kind_index];
		recoveries.deadline_offset = offset;
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		const MisterResult result = context->platform().recover(
			context->platform().context, masks[kind_index], &observation, 100);
		assert((observation.observed_resource_flags &
			observation.neutral_resource_flags) == 0);
		assert(((observation.observed_resource_flags |
			observation.neutral_resource_flags) & ~masks[kind_index]) == 0);
		if (offset < 0) {
			MisterResult completed = result;
			for (size_t retry = 0; retry != 16 && completed != MISTER_RESULT_OK; ++retry) {
				observation = RecoveryObservation();
				completed = context->platform().recover(context->platform().context,
					masks[kind_index], &observation, 5000);
				assert(completed == MISTER_RESULT_OK ||
					completed == MISTER_RESULT_CLEANUP_INCOMPLETE);
			}
			assert(completed == MISTER_RESULT_OK);
			assert(observation.observed_resource_flags == 0);
			assert(observation.neutral_resource_flags == masks[kind_index]);
			assert(recoveries.creates == 1 && recoveries.destroys == 1);
			assert(recoveries.evidence.live_captured &&
				recoveries.evidence.baseline_captured);
			AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
			continue;
		}
		assert(result == MISTER_RESULT_DEADLINE ||
			result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(recoveries.creates == 1 && recoveries.destroys == 0);
		assert(recoveries.evidence.live_captured &&
			recoveries.evidence.suspended_captured);
		const NativeRetainedOperationSnapshot &live = recoveries.evidence.live;
		const NativeRetainedOperationSnapshot &suspended =
			recoveries.evidence.suspended;
		assert(live.query_valid && live.retained && live.registration_is_invoked &&
			live.authority == LeaseAuthority::recovery_epoch &&
			live.supplied_operation_kind == operation_kinds[kind_index] &&
			live.requested_resource_flags == masks[kind_index] &&
			live.non_fpga_deadline_ms == 3000 && live.fpga_deadline_ms == 6000 &&
			live.registration_effective_deadline_ms == 1100 &&
			live.invocation_callback_deadline_ms == 1100 &&
			live.lease_identity != 0 && live.registration_identity != 0 &&
			live.backend_applicable && live.backend_matches_expected);
		assert(suspended.registration_is_suspended &&
			suspended.registration_effective_deadline_ms == 0);
		AssertOnlyRetainedRebindFieldsChanged(live, suspended);
		clock.Set(clock.NowMs() + 1);
		observation = RecoveryObservation();
		MisterResult completed = MISTER_RESULT_CLEANUP_INCOMPLETE;
		for (size_t retry = 0; retry != 16 && completed != MISTER_RESULT_OK; ++retry) {
			observation = RecoveryObservation();
			completed = context->platform().recover(context->platform().context,
				masks[kind_index], &observation, 5000);
			assert(completed == MISTER_RESULT_OK ||
				completed == MISTER_RESULT_CLEANUP_INCOMPLETE);
		}
		assert(completed == MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == masks[kind_index]);
		assert(recoveries.creates == 1 && recoveries.destroys == 1);
		assert(recoveries.evidence.rebound_captured &&
			recoveries.evidence.baseline_captured);
		assert(recoveries.evidence.rebound.invocation_identity !=
			live.invocation_identity &&
			recoveries.evidence.rebound.invocation_callback_deadline_ms ==
				clock.NowMs() + 5000);
		const uint64_t retry_authority_deadline =
			suspended.registration_authority_deadline_ms;
		assert(recoveries.evidence.rebound.registration_effective_deadline_ms ==
			(recoveries.evidence.rebound.invocation_callback_deadline_ms <
			 retry_authority_deadline ?
			 recoveries.evidence.rebound.invocation_callback_deadline_ms :
			 retry_authority_deadline));
		AssertOnlyRetainedRebindFieldsChanged(suspended,
			recoveries.evidence.rebound);
		AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
	}
}

static void TestComposedTypedCleanupGroupDeadlineMatrix()
{
	typedef void (GraphGeneration::*DeadlineArm)(uint64_t);
	const DeadlineArm arms[] = {&GraphGeneration::ConsumeSaveDeadlineAt,
		&GraphGeneration::ConsumeVideoDeadlineAt,
		&GraphGeneration::ConsumeAudioDeadlineAt,
		&GraphGeneration::ConsumeAudioVideoDeadlineAt,
		&GraphGeneration::ConsumeCoreDeadlineAt};
	const OperationKind kinds[] = {OperationKind::save, OperationKind::video,
		OperationKind::audio, OperationKind::audio_video,
		OperationKind::core_protocol};
	for (size_t kind_index = 0; kind_index != sizeof(kinds) / sizeof(kinds[0]);
		++kind_index) for (int offset = -1; offset <= 1; ++offset) {
		NativePosixFileSystem host_clock;
		TestClock clock(host_clock.NowMs());
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GraphGenerationFactory generations(clock);
		RecoveryState recovery_state;
		TestRecoveryFactory recoveries(recovery_state);
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		const MisterPlatformV2 &platform = context->platform();
		assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
		MisterLaunchV2 launch = ConcreteMegaDriveLaunch();
		assert(platform.load(platform.context, &launch, 60000) == MISTER_RESULT_OK);
		GraphGeneration *const product = generations.last;
		const uint64_t first_callback_deadline = clock.NowMs() + 100;
		(product->*arms[kind_index])(first_callback_deadline);
		assert(platform.stop(platform.context, 100) == MISTER_RESULT_DEADLINE);
		NativeRetainedOperationSnapshot suspended = {};
		assert(product->cleanup_retained_snapshot(kinds[kind_index], &suspended) ==
			MISTER_RESULT_OK);
		assert(suspended.retained && suspended.registration_is_suspended &&
			suspended.registration_effective_deadline_ms == 0);
		const NativeCleanupTiming timing = product->cleanup_timing();
		const uint64_t group_deadline = kinds[kind_index] ==
			OperationKind::core_protocol ? timing.fpga_deadline_ms :
			timing.non_fpga_deadline_ms;
		assert(suspended.registration_authority_deadline_ms == group_deadline);
		const GraphTypedResourceEvidence evidence_before =
			product->typed_resource_evidence();
		(product->*arms[kind_index])(static_cast<uint64_t>(
			static_cast<int64_t>(group_deadline) + offset));
		const uint64_t remaining = group_deadline - clock.NowMs() + 1000;
		assert(remaining <= UINT32_MAX);
		const MisterResult result = platform.stop(platform.context,
			static_cast<uint32_t>(remaining));
		if (offset < 0) {
			const NativeRetainedOperationSnapshot live = generations.last != nullptr ?
				generations.last->cleanup_live_snapshot() :
				generations.final.retained_live_snapshot;
			assert(live.query_valid && live.retained &&
				live.registration_is_invoked &&
				live.registration_effective_deadline_ms == group_deadline &&
				live.invocation_callback_deadline_ms > group_deadline);
			AssertOnlyRetainedRebindFieldsChanged(suspended, live);
			assert(result == MISTER_RESULT_OK || result == MISTER_RESULT_DEADLINE ||
				result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		} else {
			assert(result == MISTER_RESULT_DEADLINE);
			assert(generations.last == product);
			NativeRetainedOperationSnapshot unchanged = {};
			assert(product->cleanup_retained_snapshot(kinds[kind_index], &unchanged) ==
				MISTER_RESULT_OK);
			AssertSameRetainedOperationSnapshot(suspended, unchanged);
			const GraphTypedResourceEvidence evidence_after =
				product->typed_resource_evidence();
			if (kinds[kind_index] == OperationKind::save)
				assert(evidence_after.save_fdatasync_calls ==
					evidence_before.save_fdatasync_calls + 1);
			if (kinds[kind_index] == OperationKind::audio ||
				kinds[kind_index] == OperationKind::video ||
				kinds[kind_index] == OperationKind::audio_video)
				assert(evidence_after.av_begins == evidence_before.av_begins + 1);
			clock.Set(static_cast<uint64_t>(group_deadline + offset + 1));
			assert(platform.stop(platform.context, UINT32_MAX) ==
				MISTER_RESULT_DEADLINE);
			NativeRetainedOperationSnapshot terminal = {};
			assert(product->cleanup_retained_snapshot(kinds[kind_index], &terminal) ==
				MISTER_RESULT_OK);
			AssertSameRetainedOperationSnapshot(unchanged, terminal);
			AssertSameGraphTypedResourceEvidence(evidence_after,
				product->typed_resource_evidence());
		}
		context.reset();
		assert(generations.last == nullptr);
	}
}

static void TestComposedTypedRecoveryGroupDeadlineMatrix()
{
	const GraphRecoveryDeadlineKind kinds[] = {GraphRecoveryDeadlineKind::save,
		GraphRecoveryDeadlineKind::video, GraphRecoveryDeadlineKind::audio,
		GraphRecoveryDeadlineKind::audio_video,
		GraphRecoveryDeadlineKind::core_protocol};
	const OperationKind operation_kinds[] = {OperationKind::save,
		OperationKind::video, OperationKind::audio, OperationKind::audio_video,
		OperationKind::core_protocol};
	const uint32_t masks[] = {MISTER_RESOURCE_SAVES, MISTER_RESOURCE_NATIVE_VIDEO,
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_FPGA |
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL,
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO |
			MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
			MISTER_RESOURCE_CORE_PROTOCOL,
		MISTER_RESOURCE_CORE_PROTOCOL};
	for (size_t kind_index = 0; kind_index != sizeof(kinds) / sizeof(kinds[0]);
		++kind_index) for (int offset = -1; offset <= 1; ++offset) {
		TestClock clock(1000);
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GenerationState generation_state;
		TestGenerationFactory generations(generation_state);
		GraphRecoveryFactory recoveries(clock);
		recoveries.deadline_kind = kinds[kind_index];
		recoveries.deadline_offset = offset;
		recoveries.deadline_uses_group = true;
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		const MisterResult first = context->platform().recover(
			context->platform().context, masks[kind_index], &observation, UINT32_MAX);
		assert(first == MISTER_RESULT_DEADLINE ||
			first == MISTER_RESULT_CLEANUP_INCOMPLETE ||
			first == MISTER_RESULT_PLATFORM || first == MISTER_RESULT_OK);
		assert(recoveries.evidence.live_captured);
		const NativeRetainedOperationSnapshot &live = recoveries.evidence.live;
		const uint64_t group_deadline = operation_kinds[kind_index] ==
			OperationKind::core_protocol ? 6000 : 3000;
		assert(live.query_valid && live.retained && live.registration_is_invoked &&
			live.authority == LeaseAuthority::recovery_epoch &&
			live.supplied_operation_kind == operation_kinds[kind_index] &&
			live.requested_resource_flags == masks[kind_index] &&
			live.non_fpga_deadline_ms == 3000 && live.fpga_deadline_ms == 6000 &&
			live.registration_authority_deadline_ms == group_deadline &&
			live.registration_effective_deadline_ms == group_deadline &&
			live.invocation_callback_deadline_ms > group_deadline);
		if (offset >= 0 && recoveries.destroys == 0) {
			assert(recoveries.evidence.suspended_captured);
			const NativeRetainedOperationSnapshot suspended =
				recoveries.evidence.suspended;
			assert(suspended.registration_is_suspended &&
				suspended.registration_effective_deadline_ms == 0);
			AssertOnlyRetainedRebindFieldsChanged(live, suspended);
			const GraphRecoveryEvidenceState resource_before = recoveries.evidence;
			clock.Set(clock.NowMs() + 1);
			observation = RecoveryObservation();
			const MisterResult later = context->platform().recover(
				context->platform().context, masks[kind_index], &observation, UINT32_MAX);
			assert(later == MISTER_RESULT_DEADLINE ||
				later == MISTER_RESULT_INVALID_STATE);
			AssertSameGraphRecoveryResources(resource_before, recoveries.evidence);
		}
		if (offset < 0) {
			MisterResult completed = first;
			for (size_t retry = 0; retry != 16 && completed != MISTER_RESULT_OK;
				++retry) {
				observation = RecoveryObservation();
				completed = context->platform().recover(context->platform().context,
					masks[kind_index], &observation, UINT32_MAX);
				assert(completed == MISTER_RESULT_OK ||
					completed == MISTER_RESULT_CLEANUP_INCOMPLETE);
			}
			assert(completed == MISTER_RESULT_OK);
			assert(observation.observed_resource_flags == 0 &&
				observation.neutral_resource_flags == masks[kind_index]);
		}
		context.reset();
		assert(recoveries.creates == 1 && recoveries.destroys == 1 &&
			recoveries.evidence.baseline_captured);
		AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
		assert(!recoveries.evidence.mmio_descriptor_open &&
			!recoveries.evidence.mmio_mappings_held &&
			!recoveries.evidence.protocol_descriptor_open &&
			recoveries.evidence.protocol_mappings == 0 &&
			recoveries.evidence.save_descriptors == 0);
	}
}

static void AssertExactRecoveryObservation(const MisterRecoveryObservationV2 &observation,
	uint32_t requested)
{
	assert(observation.abi_version == MISTER_RUNTIME_ABI_VERSION_V2);
	assert(observation.struct_size == sizeof(observation));
	assert((observation.observed_resource_flags &
		observation.neutral_resource_flags) == 0);
	assert(((observation.observed_resource_flags |
		observation.neutral_resource_flags) & ~requested) == 0);
	for (size_t index = 0; index != 4; ++index)
		assert(observation.reserved[index] == 0);
}

static void TestComposedTerminalContainmentDeadlineMatrix()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	for (int offset = -1; offset <= 1; ++offset) {
		TestClock clock(1000);
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GenerationState generation_state;
		TestGenerationFactory generations(generation_state);
		GraphRecoveryFactory recoveries(clock);
		recoveries.deadline_kind = GraphRecoveryDeadlineKind::terminal;
		recoveries.deadline_offset = offset;
		recoveries.deadline_uses_group = false;
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		const MisterResult first = context->platform().recover(
			context->platform().context, closure, &observation, 100);
		AssertExactRecoveryObservation(observation, closure);
		assert(recoveries.creates == 1 && recoveries.trace.size() == 1 &&
			recoveries.trace[0] == OperationKind::terminal_fpga_cleanup);
		assert(generation_state.create_calls == 0);
		if (offset < 0) {
			assert(first == MISTER_RESULT_OK);
			assert(observation.observed_resource_flags == 0 &&
				observation.neutral_resource_flags == closure);
			assert(recoveries.destroys == 1 &&
				recoveries.evidence.baseline_captured);
			AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
			continue;
		}
		assert(first == MISTER_RESULT_DEADLINE);
		assert(recoveries.destroys == 0);
		// Mutating the caller-owned copy cannot affect the attempt's private
		// observation or its immutable requested mask on the next callback.
		observation.observed_resource_flags = 0;
		observation.neutral_resource_flags = 0;
		clock.Set(clock.NowMs() + 1);
		observation = RecoveryObservation();
		const MisterResult retry = context->platform().recover(
			context->platform().context, closure, &observation, 5000);
		AssertExactRecoveryObservation(observation, closure);
		assert(retry == MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0 &&
			observation.neutral_resource_flags == closure);
		assert(recoveries.trace.size() == 2 &&
			recoveries.trace[1] == OperationKind::terminal_fpga_cleanup);
		assert(recoveries.creates == 1 && recoveries.destroys == 1 &&
			recoveries.evidence.baseline_captured);
		AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
	}
}

static void TestComposedTerminalContainmentGroupDeadlineMatrix()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	for (int offset = -1; offset <= 1; ++offset) {
		TestClock clock(1000);
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GenerationState generation_state;
		TestGenerationFactory generations(generation_state);
		GraphRecoveryFactory recoveries(clock);
		recoveries.deadline_kind = GraphRecoveryDeadlineKind::terminal;
		recoveries.deadline_offset = offset;
		recoveries.deadline_uses_group = true;
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		const MisterResult first = context->platform().recover(
			context->platform().context, closure, &observation, UINT32_MAX);
		AssertExactRecoveryObservation(observation, closure);
		assert(recoveries.creates == 1 && recoveries.trace.size() == 1 &&
			recoveries.trace[0] == OperationKind::terminal_fpga_cleanup);
		if (offset < 0) {
			assert(first == MISTER_RESULT_OK);
			assert(observation.observed_resource_flags == 0 &&
				observation.neutral_resource_flags == closure);
			assert(recoveries.destroys == 1 &&
				recoveries.evidence.baseline_captured);
			AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
			continue;
		}
		assert(first == MISTER_RESULT_DEADLINE);
		assert(recoveries.destroys == 0);
		const GraphRecoveryEvidenceState partial = recoveries.evidence;
		clock.Set(clock.NowMs() + 1);
		observation = RecoveryObservation();
		const MisterResult later = context->platform().recover(
			context->platform().context, closure, &observation, UINT32_MAX);
		AssertExactRecoveryObservation(observation, closure);
		assert(later == MISTER_RESULT_DEADLINE ||
			later == MISTER_RESULT_INVALID_STATE);
		assert(recoveries.creates == 1 && recoveries.trace.size() <= 2);
		AssertSameGraphRecoveryResources(partial, recoveries.evidence);
		context.reset();
		assert(recoveries.destroys == 1 && recoveries.evidence.baseline_captured);
		AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
	}
}

static void TestTwoHundredAlternatingGenerations()
{
	// This real composed graph includes the 32 KiB SNES fixture. Refresh the
	// source clock before each callback and derive a bounded absolute callback
	// deadline from it; the dedicated matrices own the exact short-boundary
	// contract.
	const uint32_t callback_budget_ms = kConcreteGraphCallbackBudgetMs;
	NativePosixFileSystem host_clock;
	TestClock clock(host_clock.NowMs());
	linux_native::NativeLinuxV2FixtureProfiles profiles = {
		FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
	GraphGenerationFactory generations(clock);
	RecoveryState recovery_state;
	TestRecoveryFactory recoveries(recovery_state);
	std::unique_ptr<linux_native::NativeLinuxV2Context> context;
	assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
		generations, recoveries, &context) == MISTER_RESULT_OK);
	const MisterPlatformV2 &platform = context->platform();
	assert(platform.start(platform.context, 10) == MISTER_RESULT_OK);
	int snes_products = 0;
	int megadrive_products = 0;
	for (int cycle = 0; cycle < 200; ++cycle) {
		const bool snes = (cycle % 2) == 0;
		MisterLaunchV2 launch = snes ? ConcreteSnesLaunch() : ConcreteMegaDriveLaunch();
		clock.Set(host_clock.NowMs());
		assert(platform.load(platform.context, &launch, callback_budget_ms) ==
			MISTER_RESULT_OK);
		GraphGeneration *const product = generations.last;
		assert(product != nullptr && product->programmed_same_handle());
		MisterObservationV2 observation = Observation();
		clock.Set(host_clock.NowMs());
		assert(platform.observe(platform.context, &observation, callback_budget_ms) ==
			MISTER_RESULT_OK);
		assert(observation.ready == 1 &&
			observation.resource_flags == MISTER_RESOURCE_V2_KNOWN);
		assert(CopyView(observation.observed_core) == (snes ? "SNES" : "MegaDrive"));
		clock.Set(host_clock.NowMs());
		assert(platform.tick(platform.context, callback_budget_ms) == MISTER_RESULT_OK);
		assert(product->tick_deadline_propagated());
		MisterResult stopped = MISTER_RESULT_CLEANUP_INCOMPLETE;
		for (size_t retry = 0; retry != 16 && generations.last != nullptr; ++retry) {
			clock.Set(host_clock.NowMs());
			stopped = platform.stop(platform.context, callback_budget_ms);
			assert(stopped == MISTER_RESULT_OK ||
				stopped == MISTER_RESULT_CLEANUP_INCOMPLETE);
		}
		assert(stopped == MISTER_RESULT_OK);
		assert(generations.last == nullptr);
		AssertRepeatedGraphCycleBaseline(generations.final);
		assert(generations.creates == cycle + 1);
		if (snes) ++snes_products; else ++megadrive_products;
	}
	assert(snes_products == 100 && megadrive_products == 100);
	assert(generations.creates == 200);
}

static void TestTwoHundredFreshRecoveryContexts()
{
	for (int cycle = 0; cycle < 200; ++cycle) {
		TestClock clock(static_cast<uint64_t>(3000 + cycle));
		linux_native::NativeLinuxV2FixtureProfiles profiles = {
			FixtureNativeCoreProfile("snes"), FixtureNativeCoreProfile("megadrive")};
		GenerationState generation_state;
		TestGenerationFactory generations(generation_state);
		GraphRecoveryFactory recoveries(clock);
		std::unique_ptr<linux_native::NativeLinuxV2Context> context;
		assert(linux_native::CreateFixtureNativeLinuxV2ContextForTest(clock, profiles,
			generations, recoveries, &context) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = RecoveryObservation();
		MisterResult result = MISTER_RESULT_CLEANUP_INCOMPLETE;
		for (size_t callback = 0; callback != 16; ++callback) {
			observation = RecoveryObservation();
			result = context->platform().recover(context->platform().context,
				MISTER_RESOURCE_V2_KNOWN, &observation, 1000);
			assert((observation.observed_resource_flags &
				observation.neutral_resource_flags) == 0);
			assert(((observation.observed_resource_flags |
				observation.neutral_resource_flags) & ~MISTER_RESOURCE_V2_KNOWN) == 0);
			if (result == MISTER_RESULT_OK) break;
			assert(result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		}
		assert(result == MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_V2_KNOWN);
		const OperationKind expected[] = {OperationKind::input_descriptors,
			OperationKind::save, OperationKind::content, OperationKind::audio_video,
			OperationKind::audio, OperationKind::terminal_fpga_cleanup};
		assert(recoveries.trace.size() == sizeof(expected) / sizeof(expected[0]));
		for (size_t index = 0; index != recoveries.trace.size(); ++index)
			assert(recoveries.trace[index] == expected[index]);
		assert(recoveries.creates == 1 && recoveries.destroys == 1);
		assert(generation_state.create_calls == 0 &&
			generation_state.activate_calls == 0 &&
			generation_state.tick_calls == 0 &&
			generation_state.observe_calls == 0 &&
			generation_state.stop_calls == 0 && generation_state.destroyed == 0);
		assert(recoveries.evidence.baseline_captured);
		AssertExactIdleRetainedOperationSnapshot(recoveries.evidence.baseline);
		assert(!recoveries.evidence.mmio_descriptor_open &&
			!recoveries.evidence.mmio_mappings_held &&
			!recoveries.evidence.protocol_descriptor_open &&
			recoveries.evidence.protocol_mappings == 0 &&
			recoveries.evidence.save_descriptors == 0);
		assert(recoveries.evidence.input_close_calls == 1 &&
			recoveries.evidence.content_close_calls == 1 &&
			recoveries.evidence.protocol_disable_calls == 0);
	}
}

int main()
{
	GraphProcessIsolationLock process_isolation;
	TestConstructionAndStableTable();
	TestStartLoadTickObserveAndStop();
	TestLaunchValidationAndFailureRetention();
	TestForwardCompatibleLaunchPrefixes();
	TestObserveRejectsReadyWithoutCompleteActiveResources();
	TestTickFailureRequiresExplicitStop();
	TestFactoryFailureAndSaturation();
	TestRecoveryAdmissionRetryAndTruthfulOutput();
	TestRecoveryRejectedAfterStartAndInvalidOutputs();
	TestRecoveryRejectsMismatchedProductPermanently();
	TestRecoveryRejectsMalformedAndIncompleteSuccess();
	TestProfileValidation();
	TestRuntimeV2Integration();
	TestConcreteCommittedAdapterGraphThroughCallbacks();
	TestConcreteSnesActivationThroughCallbacks();
	TestConcreteIncompleteStopRetainsAndRetriesExactProduct();
	TestConcreteAcquisitionFaultMatrix();
	TestConcreteReleaseFaultMatrix();
	TestConcreteNativeRecoveryProductThroughCallback();
	TestConcreteNativeRecoveryAllMaskThroughCallbacks();
	TestConcreteNativeRecoveryCoreProtocolOnly();
	TestConcreteNativeRecoveryRetryRetainsAttempt();
	TestComposedLoadUnwindDeadlineMatrix();
	TestComposedOrdinaryCleanupDeadlineMatrix();
	TestComposedRecoveryObservationDeadlineMatrix();
	TestComposedTypedCleanupCallbackDeadlineMatrix();
	TestComposedTypedRecoveryCallbackDeadlineMatrix();
	TestComposedTypedCleanupGroupDeadlineMatrix();
	TestComposedTypedRecoveryGroupDeadlineMatrix();
	TestComposedTerminalContainmentDeadlineMatrix();
	TestComposedTerminalContainmentGroupDeadlineMatrix();
	TestTwoHundredAlternatingGenerations();
	TestTwoHundredFreshRecoveryContexts();
	return 0;
}

#endif
