// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_recovery.hpp"
#include "runtime/native/linux/native_save_adapter.hpp"
#include "tests/native_core_protocol_authority_test_peer.hpp"
#include "tests/native_peripheral_authority_test_peer.hpp"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

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
		  last_deadline(0), save_record(FixtureSafeSaveRecoveryRecordForTest(
			NativeSystem::snes))
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
		if (deadline_core_once && deadline_clock != nullptr) {
			deadline_core_once = false;
			last_deadline = lease.absolute_deadline_ms();
			*state = RecoveryResourceState::unknown;
			deadline_clock->SetNow(last_deadline);
			return MISTER_RESULT_DEADLINE;
		}
		const Result primary = ApplyDeadline(lease.absolute_deadline_ms(),
			OperationKind::core_protocol, state);
		if (abandon_core_protocol_release_once) {
			abandon_core_protocol_release_once = false;
			return MISTER_RESULT_PLATFORM;
		}
		const ProtocolMappingReleaseReceipt release = {
			MISTER_RESULT_OK, true, true, true, true, true,
			broker_.mutation_sequence_for_test()};
		const Result completed = CoreProtocolAuthorityTestPeer::CompleteRecovery(
			lease, broker_, std::move(session), release);
		return completed == MISTER_RESULT_OK ? primary : completed;
	}
	const SafeAudioRecoveryRecord *SafeAudioRecord() const override
	{
		return FixtureSafeAudioRecoveryRecordForTest(NativeSystem::snes);
	}
	const SafeVideoRecoveryRecord *SafeVideoRecord() const override
	{
		return FixtureSafeVideoRecoveryRecordForTest(NativeSystem::snes);
	}
	const SafeAudioVideoRecoveryRecord *SafeAudioVideoRecord() const override
	{
		return FixtureSafeAudioVideoRecoveryRecordForTest(NativeSystem::snes);
	}
	const SafeSaveRecoveryRecord *SafeSaveRecord() const override
	{
		return save_record;
	}

	HardwareBroker &broker_;
	RecoveryResourceState states[11];
	Result results[11];
	int calls;
	int core_protocol_session_calls;
	uint64_t last_deadline;
	bool abandon_core_protocol_release_once = false;
	FakeClock *deadline_clock = nullptr;
	bool deadline_core_once = false;
	const SafeSaveRecoveryRecord *save_record;
};

class FakeTypedRecoveryResources final : public NativeAudioResource,
	public NativeVideoResource, public NativeAudioVideoResource {
public:
	explicit FakeTypedRecoveryResources(HardwareBroker &broker)
		: broker_(broker), backend_(PeripheralAuthorityTestPeer::Backend(broker,
			this)), audio_calls(0), video_calls(0), coupled_calls(0), last_deadline(0),
		  abandon_once(false), closure_unknown_once(false), deadline_clock(nullptr),
		  deadline_once_kind(OperationKind::program_fpga), deadline_to_expire(1500),
		  coupled_all_neutral(false) {}

	PeripheralBackendIdentity BackendIdentity() const override { return backend_; }
	NativePeripheralAcquisitionOutcome StartAudio(
		std::unique_ptr<ActiveAudioSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false}; }
	NativePeripheralReleaseOutcome StopAudio(
		std::unique_ptr<CleanupAudioSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false, false, false, false}; }
	NativePeripheralReleaseOutcome RecoverAudio(
		std::unique_ptr<RecoveryAudioSessionBundle> &&session) override
	{
		++audio_calls;
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false, false,
			false, false};
		last_deadline = PeripheralAuthorityTestPeer::Deadline(*session);
		if (deadline_once_kind == OperationKind::audio && deadline_clock != nullptr) {
			deadline_once_kind = OperationKind::program_fpga;
			deadline_clock->SetNow(last_deadline);
			session.reset();
			return {MISTER_RESULT_DEADLINE, false, false, false, false};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK)
			return {recorded, false, false, false, false};
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, sequence, 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteAudio(
			broker_, std::move(session), receipt);
		return {completed, completed == MISTER_RESULT_OK, false,
			completed == MISTER_RESULT_OK, false};
	}
	void CloseAudioForProcessExit() override {}
	NativePeripheralAcquisitionOutcome StartVideo(
		std::unique_ptr<ActiveVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false}; }
	NativePeripheralReleaseOutcome StopVideo(
		std::unique_ptr<CleanupVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, false, false, false, false}; }
	NativePeripheralReleaseOutcome RecoverVideo(
		std::unique_ptr<RecoveryVideoSessionBundle> &&session) override
	{
		++video_calls;
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false, false,
			false, false};
		if (deadline_once_kind == OperationKind::video && deadline_clock != nullptr) {
			last_deadline = deadline_to_expire;
			deadline_once_kind = OperationKind::program_fpga;
			deadline_clock->SetNow(deadline_to_expire);
			session.reset();
			return {MISTER_RESULT_DEADLINE, false, false, false, false};
		}
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, broker_.mutation_sequence_for_test(), 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteVideo(
			broker_, std::move(session), receipt);
		return {completed, completed == MISTER_RESULT_OK,
			completed == MISTER_RESULT_OK,
			completed == MISTER_RESULT_OK, false};
	}
	void CloseVideoForProcessExit() override {}
	NativeCoupledAcquisitionOutcome StartAudioVideo(
		std::unique_ptr<ActiveAudioVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, 0, false}; }
	NativeCoupledReleaseOutcome StopAudioVideo(
		std::unique_ptr<CleanupAudioVideoSessionBundle> &&) override
	{ return {MISTER_RESULT_UNSUPPORTED, 0, 0, 0, false, false, 0}; }
	NativeCoupledReleaseOutcome RecoverAudioVideo(
		std::unique_ptr<RecoveryAudioVideoSessionBundle> &&session) override
	{
		++coupled_calls;
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false,
			false, 0};
		last_deadline = PeripheralAuthorityTestPeer::Deadline(*session);
		const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		if (deadline_once_kind == OperationKind::audio_video &&
			deadline_clock != nullptr) {
			deadline_once_kind = OperationKind::program_fpga;
			deadline_clock->SetNow(last_deadline);
			const CoupledFailureReceipt failure = {MISTER_RESULT_DEADLINE,
				{MISTER_RESULT_DEADLINE, affected, false, false, false,
					broker_.mutation_sequence_for_test(), 0xa55a}};
			const Result abandoned = PeripheralAuthorityTestPeer::AbandonAudioVideo(
				broker_, std::move(session), failure);
			return {abandoned == MISTER_RESULT_OK ? MISTER_RESULT_DEADLINE :
				abandoned, affected, 0, 0, false, false,
				broker_.mutation_sequence_for_test()};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK)
			return {recorded, 0, 0, 0, false, false, 0};
		if (abandon_once) {
			abandon_once = false;
			const bool closure_unknown = closure_unknown_once;
			closure_unknown_once = false;
			const CoupledFailureReceipt failure = {MISTER_RESULT_PLATFORM,
				{MISTER_RESULT_PLATFORM, affected, false, false, closure_unknown,
					sequence, 0xa55a}};
			const Result abandoned = PeripheralAuthorityTestPeer::AbandonAudioVideo(
				broker_, std::move(session), failure);
			return {abandoned == MISTER_RESULT_OK ? MISTER_RESULT_PLATFORM :
				abandoned, affected, 0, 0, false, closure_unknown, sequence};
		}
		const uint32_t neutral = coupled_all_neutral ? affected :
			MISTER_RESOURCE_NATIVE_VIDEO;
		const CoupledRecoveryReceipt receipt = {MISTER_RESULT_OK, affected, 0,
			neutral, true, false, sequence, true, 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteAudioVideo(
			broker_, std::move(session), receipt);
		return {completed, affected, 0, neutral, true,
			false, sequence};
	}
	void CloseAudioVideoForProcessExit() override {}

	HardwareBroker &broker_;
	PeripheralBackendIdentity backend_;
	int audio_calls;
	int video_calls;
	int coupled_calls;
	uint64_t last_deadline;
	bool abandon_once;
	bool closure_unknown_once;
	FakeClock *deadline_clock;
	OperationKind deadline_once_kind;
	uint64_t deadline_to_expire;
	bool coupled_all_neutral;
};

class FakeTypedSaveRecoveryResource final : public NativeSaveResource {
public:
	NativeSaveOpenOutcome OpenSave(const OperationLease &, const NativeCoreProfile &,
		const NativeSaveKey &) override
	{
		return {MISTER_RESULT_UNSUPPORTED, false};
	}
	NativeSaveCloseOutcome FlushAndCloseSave(const OperationLease &) override
	{
		return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
	}
	NativeSaveCloseOutcome RecoverSave(const OperationLease &lease,
		const SafeSaveRecoveryRecord &) override
	{
		++calls;
		last_deadline = lease.absolute_deadline_ms();
		if (deadline_once && deadline_clock != nullptr) {
			deadline_once = false;
			deadline_clock->SetNow(last_deadline);
			return {MISTER_RESULT_DEADLINE, false, false, false, false};
		}
		if (fail_once) {
			fail_once = false;
			return {MISTER_RESULT_PLATFORM, false, false, false, false};
		}
		return {MISTER_RESULT_OK, true, true, true, false};
	}
	void CloseSaveForProcessExit() override {}

	int calls = 0;
	uint64_t last_deadline = 0;
	bool fail_once = false;
	FakeClock *deadline_clock = nullptr;
	bool deadline_once = false;
};

class RecoverySaveFileSystem final : public linux_native::NativeSaveFileSystem {
public:
	RecoverySaveFileSystem() : fdatasync_fail_once_(false), calls_(0)
	{
		Initialize(&root_, 10, 1, S_IFDIR | 0755, 0, 0);
		Initialize(&parent_, 11, 2, S_IFDIR | 0755, 0, 0);
		Initialize(&save_root_, 12, 3, S_IFDIR | 0700, 1000, 1000);
		Initialize(&system_, 13, 4, S_IFDIR | 0700, 1000, 1000);
		Initialize(&file_, 14, 5, S_IFREG | 0600, 1000, 1000);
	}

	uint64_t NowMs() const override { return 1000; }
	linux_native::NativeSaveOpenResult OpenAt(int parent, const char *name,
		int, mode_t) override
	{
		++calls_;
		if (parent == AT_FDCWD && strcmp(name, "/") == 0) return {10, 0};
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0) return {11, 0};
		if (parent == 11 && strcmp(name, "saves") == 0) return {12, 0};
		if (parent == 12 && strcmp(name, "snes") == 0) return {13, 0};
		if (parent == 13 && strstr(name, ".sav") != nullptr) return {14, 0};
		return {-1, ENOENT};
	}
	int Stat(int descriptor, struct stat *info) override
	{ return Copy(NodeFor(descriptor), info); }
	int StatAt(int parent, const char *name, struct stat *info, int) override
	{
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0)
			return Copy(&parent_, info);
		if (parent == 11 && strcmp(name, "saves") == 0) return Copy(&save_root_, info);
		if (parent == 12 && strcmp(name, "snes") == 0) return Copy(&system_, info);
		if (parent == 13 && strstr(name, ".sav") != nullptr) return Copy(&file_, info);
		return -1;
	}
	Result MountId(int descriptor, uint64_t *mount_id) override
	{
		if (NodeFor(descriptor) == nullptr || mount_id == nullptr)
			return MISTER_RESULT_PLATFORM;
		*mount_id = 1;
		return MISTER_RESULT_OK;
	}
	int Fdatasync(int descriptor) override
	{
		if (descriptor != 14) return -1;
		if (fdatasync_fail_once_) {
			fdatasync_fail_once_ = false;
			return -1;
		}
		return 0;
	}
	int Fsync(int descriptor) override { return descriptor == 14 || descriptor == 13 ? 0 : -1; }
	int Close(int descriptor) override { return NodeFor(descriptor) == nullptr ? -1 : 0; }
	void FailFdatasyncOnce() { fdatasync_fail_once_ = true; }
	int calls() const { return calls_; }

private:
	struct Node { int descriptor; struct stat identity; };
	static void Initialize(Node *node, int descriptor, ino_t inode, mode_t mode,
		uid_t uid, gid_t gid)
	{
		node->descriptor = descriptor;
		memset(&node->identity, 0, sizeof(node->identity));
		node->identity.st_dev = 1;
		node->identity.st_ino = inode;
		node->identity.st_mode = mode;
		node->identity.st_nlink = 1;
		node->identity.st_uid = uid;
		node->identity.st_gid = gid;
	}
	Node *NodeFor(int descriptor)
	{
		Node *nodes[] = {&root_, &parent_, &save_root_, &system_, &file_};
		for (size_t index = 0; index != sizeof(nodes) / sizeof(nodes[0]); ++index)
			if (nodes[index]->descriptor == descriptor) return nodes[index];
		return nullptr;
	}
	static int Copy(const Node *node, struct stat *info)
	{
		if (node == nullptr || info == nullptr) return -1;
		*info = node->identity;
		return 0;
	}

	bool fdatasync_fail_once_;
	int calls_;
	Node root_;
	Node parent_;
	Node save_root_;
	Node system_;
	Node file_;
};

class FakeContainmentIo final : public NativeContainmentIo {
public:
	FakeContainmentIo()
		: fail_at(0), advance_at(0), advance_clock(nullptr), calls(0), writes(0),
		  reads(0), releases(0), last_deadline(0), advance_to(6000),
		  step_failure_result(MISTER_RESULT_PLATFORM), core(0x40000000u), interface(0),
		  sdr(0), bridge(7), remap(1), release_result(MISTER_RESULT_OK) {}
	Result Step()
	{
		++calls;
		if (calls == advance_at && advance_clock != nullptr)
			advance_clock->SetNow(advance_to);
		return calls == fail_at ? step_failure_result : MISTER_RESULT_OK;
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
	Result ReadCoreGpo(const Access &access, uint32_t *value) override
	{
		last_deadline = access.absolute_deadline_ms();
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
	uint64_t last_deadline;
	uint64_t advance_to;
	Result step_failure_result;
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

void TestRecoveryInvocationBoundsGenericMutation()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CONTENT, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &invocation) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, *invocation, OperationKind::content) ==
		MISTER_RESULT_OK);
	assert(io.last_deadline == 1500);
	assert(broker.FinishInvocation(std::move(invocation)) == MISTER_RESULT_OK);
}

void TestRecoveryInvocationRebindsRetainedSaveDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources typed(broker);
	FakeTypedSaveRecoveryResource save;
	save.fail_once = true;
	NativeRecovery recovery(broker, io, typed, typed, typed, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::save) ==
		MISTER_RESULT_PLATFORM);
	assert(save.calls == 1 && save.last_deadline == 1500);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);

	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 1800, &second) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::save) ==
		MISTER_RESULT_OK);
	assert(save.calls == 2 && save.last_deadline == 1800);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
}

void TestRecoveryInvocationBoundsContainmentObservation()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeContainmentIo io;
	NativeContainment containment(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_FPGA, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> invocation;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &invocation) ==
		MISTER_RESULT_OK);
	assert(containment.ObserveRecovery(*epoch, *invocation) == MISTER_RESULT_OK);
	assert(io.reads == 5 && io.last_deadline == 1500);
	assert(broker.FinishInvocation(std::move(invocation)) == MISTER_RESULT_OK);
}

void TestCallbackDeadlineDoesNotPoisonRecoveryEpochOrOriginalMask()
{
	class DeadlineOnceIo final : public NativeRecoveryIo {
	public:
		explicit DeadlineOnceIo(FakeClock &clock) : clock_(clock), calls_(0) {}
		Result CloseContent(const OperationLease &, RecoveryResourceState *state) override
		{
			++calls_;
			*state = calls_ == 1 ? RecoveryResourceState::unknown :
				RecoveryResourceState::neutral;
			if (calls_ == 1) clock_.SetNow(1500);
			return MISTER_RESULT_OK;
		}
		Result CloseInputDescriptors(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		Result MuteAudio(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		Result PowerDownVideo(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		Result DisableCoreProtocol(const OperationLease &,
			RecoveryResourceState *) override { return MISTER_RESULT_UNSUPPORTED; }
		FakeClock &clock_;
		int calls_;
	};
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	DeadlineOnceIo io(clock);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_CONTENT, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationInvocation> first;
	assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *first, OperationKind::content) ==
		MISTER_RESULT_DEADLINE);
	assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 snapshot = Observation();
	assert(recovery.Snapshot(*epoch, &snapshot) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_CONTENT) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, 0) ==
		MISTER_RESULT_INVALID_STATE);

	std::unique_ptr<OperationInvocation> second;
	assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, *second, OperationKind::content) ==
		MISTER_RESULT_OK);
	assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
	assert(recovery.Finish(std::move(epoch), &snapshot) == MISTER_RESULT_OK);
	assert(io.calls_ == 2);
}

void TestCallbackDeadlineRetriesEveryRetainedTypedRecoveryClass()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const OperationKind kinds[] = {OperationKind::audio, OperationKind::video,
		OperationKind::audio_video, OperationKind::core_protocol,
		OperationKind::save};
	const uint32_t requested[] = {MISTER_RESOURCE_NATIVE_AUDIO | closure,
		MISTER_RESOURCE_NATIVE_VIDEO,
		MISTER_RESOURCE_NATIVE_AUDIO | MISTER_RESOURCE_NATIVE_VIDEO,
		MISTER_RESOURCE_CORE_PROTOCOL, MISTER_RESOURCE_SAVES};
	for (size_t index = 0; index != sizeof(kinds) / sizeof(kinds[0]); ++index) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources typed(broker);
		FakeTypedSaveRecoveryResource save;
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		io.deadline_clock = &clock;
		io.deadline_core_once = kinds[index] == OperationKind::core_protocol;
		io.states[KindIndex(OperationKind::core_protocol)] =
			RecoveryResourceState::neutral;
		typed.deadline_clock = &clock;
		typed.deadline_once_kind = kinds[index];
		typed.coupled_all_neutral = true;
		save.deadline_clock = &clock;
		save.deadline_once = kinds[index] == OperationKind::save;
		NativeRecovery recovery(broker, io, typed, typed, typed, save, containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested[index], 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		const uint64_t identity = epoch->identity_for_test();
		std::unique_ptr<OperationInvocation> first;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *first, kinds[index]) ==
			MISTER_RESULT_DEADLINE);
		assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
		clock.SetNow(1600);
		std::unique_ptr<OperationInvocation> second;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *second, kinds[index]) == MISTER_RESULT_OK);
		assert(epoch->identity_for_test() == identity);
		assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
		if (kinds[index] == OperationKind::audio) {
			std::unique_ptr<OperationInvocation> terminal;
			assert(broker.BeginRecoveryInvocation(*epoch, 2500, &terminal) ==
				MISTER_RESULT_OK);
			assert(recovery.Perform(*epoch, *terminal,
				OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_OK);
			assert(broker.FinishInvocation(std::move(terminal)) == MISTER_RESULT_OK);
		}
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == requested[index]);
	}
}

void TestCallbackDeadlineRetriesObservationAndTerminalResidue()
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
		io.advance_clock = &clock;
		io.advance_at = 2;
		io.advance_to = 1500;
		std::unique_ptr<OperationInvocation> first;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
			MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch, *first) ==
			MISTER_RESULT_DEADLINE);
		assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
		io.advance_at = 0;
		std::unique_ptr<OperationInvocation> second;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
			MISTER_RESULT_OK);
		assert(containment.ObserveRecovery(*epoch, *second) == MISTER_RESULT_OK);
		assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(broker.FinishRecovery(std::move(epoch), &observation) ==
			MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == closure);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo recovery_io(broker);
		FakeTypedRecoveryResources typed(broker);
		FakeTypedSaveRecoveryResource save;
		FakeContainmentIo io;
		NativeContainment containment(broker, io);
		NativeRecovery recovery(broker, recovery_io, typed, typed, typed, save,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.advance_clock = &clock;
		io.advance_at = 11;
		io.advance_to = 1500;
		io.release_result = MISTER_RESULT_DEADLINE;
		std::unique_ptr<OperationInvocation> first;
		assert(broker.BeginRecoveryInvocation(*epoch, 1500, &first) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *first,
			OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_DEADLINE);
		assert(broker.FinishInvocation(std::move(first)) == MISTER_RESULT_OK);
		io.advance_at = 0;
		io.release_result = MISTER_RESULT_OK;
		std::unique_ptr<OperationInvocation> second;
		assert(broker.BeginRecoveryInvocation(*epoch, 2500, &second) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, *second,
			OperationKind::terminal_fpga_cleanup) == MISTER_RESULT_OK);
		assert(broker.FinishInvocation(std::move(second)) == MISTER_RESULT_OK);
		assert(io.writes == 5 && io.reads == 5 && io.releases == 2);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(observation.neutral_resource_flags == closure);
	}
}

void TestRetryableRecoveryResultsReachSameEpochSuccess()
{
	const Result retryable[] = {MISTER_RESULT_CLEANUP_INCOMPLETE,
		MISTER_RESULT_PLATFORM};
	for (Result first_result : retryable) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		NativeRecovery recovery(broker, io);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_CONTENT, 3000, 6000,
			&epoch) == MISTER_RESULT_OK);
		const uint64_t identity = epoch->identity_for_test();
		io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::unknown;
		io.results[KindIndex(OperationKind::content)] = first_result;
		assert(recovery.Perform(*epoch, OperationKind::content) == first_result);
		assert(io.last_deadline == 3000);
		MisterRecoveryObservationV2 snapshot = Observation();
		assert(recovery.Snapshot(*epoch, &snapshot) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
		io.results[KindIndex(OperationKind::content)] = MISTER_RESULT_OK;
		clock.SetNow(1500);
		assert(recovery.Perform(*epoch, OperationKind::content) == MISTER_RESULT_OK);
		assert(epoch->identity_for_test() == identity);
		assert(io.last_deadline == 3000);
		assert(recovery.Finish(std::move(epoch), &snapshot) == MISTER_RESULT_OK);
		assert(snapshot.neutral_resource_flags == MISTER_RESOURCE_CONTENT);
	}
}

void TestRetryableObservationAndTerminalFailuresReachSuccess()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const Result retryable[] = {MISTER_RESULT_CLEANUP_INCOMPLETE,
		MISTER_RESULT_PLATFORM};
	for (Result first_result : retryable) {
		{
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeContainmentIo io;
			io.fail_at = 2;
			io.step_failure_result = first_result;
			NativeContainment containment(broker, io);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			const uint64_t identity = epoch->identity_for_test();
			assert(containment.ObserveRecovery(*epoch) == first_result);
			io.fail_at = 0;
			assert(containment.ObserveRecovery(*epoch) == MISTER_RESULT_OK);
			assert(epoch->identity_for_test() == identity);
			MisterRecoveryObservationV2 observation = Observation();
			assert(broker.FinishRecovery(std::move(epoch), &observation) ==
				MISTER_RESULT_OK);
			assert(observation.neutral_resource_flags == closure);
		}
		{
			FakeClock clock(1000);
			HardwareBroker broker(clock);
			FakeRecoveryIo recovery_io(broker);
			FakeTypedRecoveryResources typed(broker);
			FakeTypedSaveRecoveryResource save;
			FakeContainmentIo io;
			io.release_result = first_result;
			NativeContainment containment(broker, io);
			NativeRecovery recovery(broker, recovery_io, typed, typed, typed, save,
				containment);
			std::unique_ptr<RecoveryEpoch> epoch;
			assert(broker.BeginRecovery(closure, 3000, 6000, &epoch) ==
				MISTER_RESULT_OK);
			const uint64_t identity = epoch->identity_for_test();
			assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
				first_result);
			assert(io.writes == 5 && io.reads == 5 && io.releases == 1);
			io.release_result = MISTER_RESULT_OK;
			assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
				MISTER_RESULT_OK);
			assert(epoch->identity_for_test() == identity);
			assert(io.writes == 5 && io.reads == 5 && io.releases == 2);
			MisterRecoveryObservationV2 observation = Observation();
			assert(recovery.Finish(std::move(epoch), &observation) ==
				MISTER_RESULT_OK);
			assert(observation.neutral_resource_flags == closure);
		}
	}
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

void TestSaveRecoveryRequiresTypedSafeRecordAuthority()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::save)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::save) ==
		MISTER_RESULT_UNSUPPORTED);
	assert(io.calls == 0);
}

void TestTruthfulPartitionAndExactOkRule()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	NativeRecovery recovery(broker, io);
	const uint32_t requested = MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	io.states[KindIndex(OperationKind::audio)] =
		RecoveryResourceState::observed_non_neutral;
	io.states[KindIndex(OperationKind::video)] = RecoveryResourceState::neutral;
	io.states[KindIndex(OperationKind::content)] = RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::video) == MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::content) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(observation.neutral_resource_flags == (MISTER_RESOURCE_CORE_INPUT |
		MISTER_RESOURCE_NATIVE_VIDEO | MISTER_RESOURCE_CONTENT));
	assert(observation.observed_resource_flags == MISTER_RESOURCE_NATIVE_AUDIO);
	assert((observation.neutral_resource_flags &
		observation.observed_resource_flags) == 0);
	assert(((observation.neutral_resource_flags |
		observation.observed_resource_flags) & ~requested) == 0);
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
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
			MISTER_RESOURCE_NATIVE_AUDIO;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(OperationKind::input_descriptors)] =
			RecoveryResourceState::neutral;
		assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
			MISTER_RESULT_OK);
		io.states[KindIndex(OperationKind::audio)] =
			RecoveryResourceState::observed_non_neutral;
		io.results[KindIndex(OperationKind::audio)] = failure;
		assert(recovery.Perform(*epoch, OperationKind::audio) == failure);
		MisterRecoveryObservationV2 observation = Observation();
		const Result snapshot_result = MISTER_RESULT_CLEANUP_INCOMPLETE;
		assert(recovery.Snapshot(*epoch, &observation) == snapshot_result);
		assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
		assert(observation.observed_resource_flags == MISTER_RESOURCE_NATIVE_AUDIO);
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
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
		MISTER_RESOURCE_NATIVE_AUDIO;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	io.states[KindIndex(OperationKind::input_descriptors)] =
		RecoveryResourceState::neutral;
	assert(recovery.Perform(*epoch, OperationKind::input_descriptors) ==
		MISTER_RESULT_OK);
	clock.SetNow(3000);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_DEADLINE);
	const int calls_before_retry = io.calls;
	clock.SetNow(2500);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_DEADLINE);
	assert(io.calls == calls_before_retry);
	assert(io.last_deadline == 3000);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) ==
		MISTER_RESULT_DEADLINE);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
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
		MISTER_RESULT_DEADLINE);
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
	assert(recovery.Snapshot(*epoch, &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_CORE_INPUT);
	assert(observation.observed_resource_flags == 0);
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(epoch == nullptr);
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

void TestCoreProtocolRecoveryReleaseRetryRetainsItsRecoveryLease()
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
	io.abandon_core_protocol_release_once = true;
	assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
		MISTER_RESULT_PLATFORM);
	assert(io.core_protocol_session_calls == 1);
	const uint64_t original_deadline = io.last_deadline;
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_INVALID_STATE);
	assert(epoch != nullptr);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::core_protocol) ==
		MISTER_RESULT_OK);
	assert(io.core_protocol_session_calls == 2);
	assert(io.last_deadline == original_deadline);
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
}

void TestTypedCoupledRecoverySnapshotsVideoNeutralWhileAudioRemainsUnknown()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	assert(resources.coupled_calls == 1);
	MisterRecoveryObservationV2 snapshot = Observation();
	assert(recovery.Snapshot(*epoch, &snapshot) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(snapshot.observed_resource_flags == 0);
	assert(snapshot.neutral_resource_flags == MISTER_RESOURCE_NATIVE_VIDEO);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_NATIVE_VIDEO) == MISTER_RESULT_INVALID_STATE);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, requested) ==
		MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_NATIVE_AUDIO) == MISTER_RESULT_INVALID_STATE);
	MisterRecoveryObservationV2 finish = Observation();
	assert(recovery.Finish(std::move(epoch), &finish) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
}

void TestTypedSaveRecoveryRetainsOneTaggedLeaseAndOriginalDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	FakeTypedSaveRecoveryResource save;
	FakeContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	NativeRecovery recovery(broker, io, resources, resources, resources, save,
		containment);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	save.fail_once = true;
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_PLATFORM);
	assert(save.calls == 1);
	assert(save.last_deadline == 3000);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_INVALID_STATE);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(save.calls == 2);
	assert(save.last_deadline == 3000);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_SAVES) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, 0) ==
		MISTER_RESULT_INVALID_STATE);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
}

void TestTaggedSaveRecoveryUsesTheFreshNativeSaveAdapterAndExactRecord()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	filesystem.FailFdatasyncOnce();
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_PLATFORM);
	MisterRecoveryObservationV2 incomplete = Observation();
	assert(recovery.Finish(std::move(epoch), &incomplete) == MISTER_RESULT_INVALID_STATE);
	assert(epoch != nullptr);
	assert(recovery.Perform(*epoch, OperationKind::audio) ==
		MISTER_RESULT_INVALID_STATE);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(filesystem.calls() != 0);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_SAVES) == MISTER_RESULT_OK);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == MISTER_RESOURCE_SAVES);
}

void TestSaveRecoveryRejectsMissingForgedAndExpiredSafeAuthorityBeforeIo()
{
	const SafeSaveRecoveryRecord *const exact =
		FixtureSafeSaveRecoveryRecordForTest(NativeSystem::snes);
	assert(exact != nullptr);
	SafeSaveRecoveryRecord forged = *exact;
	const SafeSaveRecoveryRecord *const records[] = {nullptr, &forged};
	for (const SafeSaveRecoveryRecord *record : records) {
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		io.save_record = record;
		FakeTypedRecoveryResources resources(broker);
		RecoverySaveFileSystem filesystem;
		linux_native::NativeSaveAdapter save(broker, filesystem);
		NativeRecovery recovery(broker, io, resources, resources, resources, save);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::save) ==
			MISTER_RESULT_UNSUPPORTED);
		assert(filesystem.calls() == 0);
	}

	FakeClock clock(3000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(MISTER_RESOURCE_SAVES, 3000, 6000, &epoch) ==
		MISTER_RESULT_DEADLINE);
	assert(!epoch);
	assert(filesystem.calls() == 0);
}

void TestRealSaveRecoveryReducesItsOutstandingMaskAndStillExcludesFinish()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	RecoverySaveFileSystem filesystem;
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeRecovery recovery(broker, io, resources, resources, resources, save);
	std::unique_ptr<RecoveryEpoch> epoch;
	const uint32_t requested = MISTER_RESOURCE_SAVES | MISTER_RESOURCE_CONTENT;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	assert(recovery.Perform(*epoch, OperationKind::save) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		requested) == MISTER_RESULT_OK);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch,
		MISTER_RESOURCE_CONTENT) == MISTER_RESULT_INVALID_STATE);
	assert(recovery.ValidateCallbackRequestedFlags(*epoch, 0) ==
		MISTER_RESULT_INVALID_STATE);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(epoch != nullptr);
}

void TestTypedCoupledRecoveryRetainsItsExactRegistrationForRetry()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.abandon_once = true;
	resources.coupled_all_neutral = true;
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(resources.coupled_calls == 1);
	const uint64_t original_deadline = resources.last_deadline;
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Finish(std::move(epoch), &observation) ==
		MISTER_RESULT_INVALID_STATE);
	assert(epoch != nullptr);
	assert(recovery.Perform(*epoch, OperationKind::video) ==
		MISTER_RESULT_INVALID_STATE);
	assert(resources.coupled_calls == 1);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_OK);
	assert(resources.coupled_calls == 2);
	assert(resources.last_deadline == original_deadline);
	assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
	assert(observation.neutral_resource_flags == requested);
}

void TestCoupledRecoveryClosureUnknownBlocksRawRetry()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.abandon_once = true;
	resources.closure_unknown_once = true;
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(resources.coupled_calls == 1);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(resources.coupled_calls == 1);
	MisterRecoveryObservationV2 observation = Observation();
	assert(recovery.Snapshot(*epoch, &observation) == MISTER_RESULT_PLATFORM);
}

void TestCoupledRecoveryCannotBorrowTheLaterFpgaDeadline()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	NativeRecovery recovery(broker, io, resources, resources, resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	clock.SetNow(3000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_DEADLINE);
	assert(resources.coupled_calls == 0);
	MisterRecoveryObservationV2 snapshot = Observation();
	assert(recovery.Snapshot(*epoch, &snapshot) == MISTER_RESULT_DEADLINE);
	assert(snapshot.neutral_resource_flags == 0);
	clock.SetNow(2000);
	assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_DEADLINE);
	assert(resources.coupled_calls == 0);
}

void TestForeignTypedRecoveryBackendCannotAdoptARetainedSession()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources owner_resources(broker);
	FakeTypedRecoveryResources foreign_resources(broker);
	NativeRecovery owner(broker, io, owner_resources, owner_resources,
		owner_resources);
	NativeRecovery foreign(broker, io, foreign_resources, foreign_resources,
		foreign_resources);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	owner_resources.abandon_once = true;
	assert(owner.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_PLATFORM);
	assert(owner_resources.coupled_calls == 1);
	assert(foreign.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_INVALID_STATE);
	assert(foreign_resources.coupled_calls == 0);
}

void TestDestroyingRetainedRecoveryDoesNotPermitNewRawAvIo()
{
	FakeClock clock(1000);
	HardwareBroker broker(clock);
	FakeRecoveryIo io(broker);
	FakeTypedRecoveryResources resources(broker);
	const uint32_t requested = MISTER_RESOURCE_NATIVE_AUDIO |
		MISTER_RESOURCE_NATIVE_VIDEO;
	std::unique_ptr<RecoveryEpoch> epoch;
	assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
		MISTER_RESULT_OK);
	resources.abandon_once = true;
	{
		NativeRecovery recovery(broker, io, resources, resources, resources);
		assert(recovery.Perform(*epoch, OperationKind::audio_video) ==
			MISTER_RESULT_PLATFORM);
	}
	NativeRecovery after_destruction(broker, io, resources, resources,
		resources);
	assert(after_destruction.Perform(*epoch, OperationKind::audio_video) ==
		MISTER_RESULT_INVALID_STATE);
	assert(resources.coupled_calls == 1);
}

void TestTypedAudioTerminalPromotionHonorsBothImmutableDeadlines()
{
	const uint32_t closure = MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		MISTER_RESOURCE_CORE_PROTOCOL;
	const uint32_t requested = closure | MISTER_RESOURCE_NATIVE_AUDIO;
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		NativeRecovery recovery(broker, io, resources, resources, resources,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
		assert(resources.audio_calls == 1 && resources.last_deadline == 3000);
		clock.SetNow(2999);
		assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
			MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) == MISTER_RESULT_OK);
		assert(observation.observed_resource_flags == 0);
		assert(observation.neutral_resource_flags == requested);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		NativeRecovery recovery(broker, io, resources, resources, resources,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
		clock.SetNow(3000);
		assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
			MISTER_RESULT_OK);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Snapshot(*epoch, &observation) ==
			MISTER_RESULT_DEADLINE);
		assert(observation.neutral_resource_flags == closure);
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
	}
	{
		FakeClock clock(1000);
		HardwareBroker broker(clock);
		FakeRecoveryIo io(broker);
		FakeTypedRecoveryResources resources(broker);
		FakeContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		NativeRecovery recovery(broker, io, resources, resources, resources,
			containment);
		std::unique_ptr<RecoveryEpoch> epoch;
		assert(broker.BeginRecovery(requested, 3000, 6000, &epoch) ==
			MISTER_RESULT_OK);
		assert(recovery.Perform(*epoch, OperationKind::audio) == MISTER_RESULT_OK);
		clock.SetNow(6000);
		assert(recovery.Perform(*epoch, OperationKind::terminal_fpga_cleanup) ==
			MISTER_RESULT_DEADLINE);
		assert(containment_io.writes == 0);
		MisterRecoveryObservationV2 observation = Observation();
		assert(recovery.Finish(std::move(epoch), &observation) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(epoch != nullptr);
	}
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
		Result finish_result;
		uint32_t observed;
		uint32_t neutral;
	};
	const Case cases[] = {
		{0, true, false, MISTER_RESULT_CLEANUP_INCOMPLETE,
			MISTER_RESULT_CLEANUP_INCOMPLETE,
			MISTER_RESOURCE_FPGA | MISTER_RESOURCE_CORE_PROTOCOL,
			MISTER_RESOURCE_BRIDGES},
		{8, false, false, MISTER_RESULT_PLATFORM,
			MISTER_RESULT_CLEANUP_INCOMPLETE, 0,
			MISTER_RESOURCE_CORE_PROTOCOL},
		{0, false, true, MISTER_RESULT_DEADLINE, MISTER_RESULT_DEADLINE, 0,
			MISTER_RESOURCE_CORE_PROTOCOL},
		{11, false, false, MISTER_RESULT_PLATFORM,
			MISTER_RESULT_CLEANUP_INCOMPLETE, 0,
			MISTER_RESOURCE_BRIDGES | MISTER_RESOURCE_CORE_PROTOCOL},
		{-1, false, true, MISTER_RESULT_DEADLINE, MISTER_RESULT_DEADLINE, 0,
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
			test.finish_result);
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
	TestRecoveryInvocationBoundsGenericMutation();
	TestRecoveryInvocationRebindsRetainedSaveDeadline();
	TestRecoveryInvocationBoundsContainmentObservation();
	TestCallbackDeadlineDoesNotPoisonRecoveryEpochOrOriginalMask();
	TestCallbackDeadlineRetriesEveryRetainedTypedRecoveryClass();
	TestCallbackDeadlineRetriesObservationAndTerminalResidue();
	TestRetryableRecoveryResultsReachSameEpochSuccess();
	TestRetryableObservationAndTerminalFailuresReachSuccess();
	TestRecoveryRejectedWithLiveGenerationOrCleanup();
	TestNormativeDependenciesAndExactDeadlines();
	TestSaveRecoveryRequiresTypedSafeRecordAuthority();
	TestTruthfulPartitionAndExactOkRule();
	TestNonOkResultsRetainPartialFields();
	TestDeadlineExpiryDoesNotExtendAndRetainsProgress();
	TestTerminalAdmissionDeadlineRetainsProgress();
	TestBusyRecoveryAdmissionDoesNotLatchDeadline();
	TestOperationOverrunRetainsTruthfulResult();
	TestCoreProtocolRecoveryUsesProfilelessSessionAuthority();
	TestCoreProtocolRecoveryReleaseRetryRetainsItsRecoveryLease();
	TestTypedCoupledRecoverySnapshotsVideoNeutralWhileAudioRemainsUnknown();
	TestTypedSaveRecoveryRetainsOneTaggedLeaseAndOriginalDeadline();
	TestTaggedSaveRecoveryUsesTheFreshNativeSaveAdapterAndExactRecord();
	TestSaveRecoveryRejectsMissingForgedAndExpiredSafeAuthorityBeforeIo();
	TestRealSaveRecoveryReducesItsOutstandingMaskAndStillExcludesFinish();
	TestTypedCoupledRecoveryRetainsItsExactRegistrationForRetry();
	TestCoupledRecoveryClosureUnknownBlocksRawRetry();
	TestCoupledRecoveryCannotBorrowTheLaterFpgaDeadline();
	TestForeignTypedRecoveryBackendCannotAdoptARetainedSession();
	TestDestroyingRetainedRecoveryDoesNotPermitNewRawAvIo();
	TestTypedAudioTerminalPromotionHonorsBothImmutableDeadlines();
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
