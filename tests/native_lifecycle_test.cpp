// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/native_lifecycle.hpp"
#include "runtime/native/native_containment.hpp"
#include "runtime/native/linux/native_save_adapter.hpp"
#include "tests/native_core_protocol_authority_test_peer.hpp"
#include "tests/native_peripheral_authority_test_peer.hpp"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <vector>

namespace mister {
namespace native {
namespace {

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms)
		: now_ms_(now_ms), force_timeout_(false), reads_until_advance_(0),
		  advance_to_ms_(0) {}

	uint64_t NowMs() const override
	{
		if (reads_until_advance_ != 0 && --reads_until_advance_ == 0)
			now_ms_ = advance_to_ms_;
		return now_ms_;
	}

	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t absolute_deadline_ms) override
	{
		return !force_timeout_ && now_ms_ < absolute_deadline_ms;
	}

	void SetNow(uint64_t now_ms)
	{
		now_ms_ = now_ms;
	}

	void Advance(uint64_t delta_ms)
	{
		now_ms_ += delta_ms;
	}

	void ForceTimeout(bool force_timeout)
	{
		force_timeout_ = force_timeout;
	}

	void AdvanceOnRead(size_t reads, uint64_t now_ms)
	{
		reads_until_advance_ = reads;
		advance_to_ms_ = now_ms;
	}

private:
	mutable uint64_t now_ms_;
	bool force_timeout_;
	mutable size_t reads_until_advance_;
	uint64_t advance_to_ms_;
};

struct LifecycleSaveNode {
	int descriptor;
	struct stat identity;
	uint64_t mount_id;
};

// The real adapter is kept intact; this is only its filesystem boundary. The
// bug caught below is that a positive adapter close receipt was not composable
// through the real lifecycle after a retained root-prefix acquisition failure.
class LifecycleSaveFileSystem final : public linux_native::NativeSaveFileSystem {
public:
	explicit LifecycleSaveFileSystem(int failing_mount_call,
		bool corrupt_system_entry_after_create = false,
		bool fail_first_root_open_once = false, uint64_t now_ms = 100)
		: failing_mount_call_(failing_mount_call), mount_calls_(0), trace_(),
		  corrupt_system_entry_after_create_(corrupt_system_entry_after_create),
		  file_created_(false), fail_first_root_open_once_(fail_first_root_open_once),
		  now_ms_(now_ms), operation_count_(0),
		  root_(Node(10, 1, S_IFDIR | 0755, 0, 0)),
		  parent_(Node(11, 2, S_IFDIR | 0755, 0, 0)),
		  save_root_(Node(12, 3, S_IFDIR | 0700, 1000, 1000)),
		  system_(Node(13, 4, S_IFDIR | 0700, 1000, 1000)),
		  file_(Node(14, 5, S_IFREG | 0600, 1000, 1000)) {}

	uint64_t NowMs() const override { return now_ms_; }
	linux_native::NativeSaveOpenResult OpenAt(int parent, const char *name,
		int flags, mode_t) override
	{
		++operation_count_;
		assert((flags & O_NOFOLLOW) != 0);
		if (parent == AT_FDCWD && strcmp(name, "/") == 0 &&
			fail_first_root_open_once_) {
			fail_first_root_open_once_ = false;
			return {-1, EIO};
		}
		if (parent == AT_FDCWD && strcmp(name, "/") == 0) return {10, 0};
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0) return {11, 0};
		if (parent == 11 && strcmp(name, "saves") == 0) return {12, 0};
		if (parent == 12 && strcmp(name, "snes") == 0) return {13, 0};
		if (parent == 13 && strstr(name, ".sav") != nullptr) {
			if (corrupt_system_entry_after_create_ && !file_created_) {
				if ((flags & O_CREAT) == 0) return {-1, ENOENT};
				file_created_ = true;
			}
			return {14, 0};
		}
		return {-1, ENOENT};
	}
	int Stat(int descriptor, struct stat *info) override
	{
		++operation_count_;
		const LifecycleSaveNode *const node = NodeFor(descriptor);
		if (node == nullptr || info == nullptr) return -1;
		*info = node->identity;
		return 0;
	}
	int StatAt(int parent, const char *name, struct stat *info, int flags) override
	{
		++operation_count_;
		assert((flags & AT_SYMLINK_NOFOLLOW) != 0);
		const LifecycleSaveNode *node = nullptr;
		if (parent == 10 && strcmp(name, "fogcast-fixture") == 0) node = &parent_;
		else if (parent == 11 && strcmp(name, "saves") == 0) node = &save_root_;
		else if (parent == 12 && strcmp(name, "snes") == 0) node = &system_;
		else if (parent == 13 && strstr(name, ".sav") != nullptr) node = &file_;
		if (node == nullptr || info == nullptr) return -1;
		*info = node->identity;
		if (corrupt_system_entry_after_create_ && parent == 12 &&
			strcmp(name, "snes") == 0)
			++info->st_ino;
		return 0;
	}
	Result MountId(int descriptor, uint64_t *mount_id) override
	{
		++operation_count_;
		++mount_calls_;
		if (mount_calls_ == failing_mount_call_) return MISTER_RESULT_PLATFORM;
		const LifecycleSaveNode *const node = NodeFor(descriptor);
		if (node == nullptr || mount_id == nullptr) return MISTER_RESULT_PLATFORM;
		*mount_id = node->mount_id;
		return MISTER_RESULT_OK;
	}
	int Fdatasync(int descriptor) override
	{
		++operation_count_;
		trace_.push_back('d');
		return NodeFor(descriptor) == nullptr ? -1 : 0;
	}
	int Fsync(int descriptor) override
	{
		++operation_count_;
		trace_.push_back(descriptor == 14 ? 'f' : 's');
		return NodeFor(descriptor) == nullptr ? -1 : 0;
	}
	int Close(int descriptor) override
	{
		++operation_count_;
		trace_.push_back(static_cast<char>('0' + descriptor - 10));
		return NodeFor(descriptor) == nullptr ? -1 : 0;
	}
	const std::vector<char> &trace() const { return trace_; }
	size_t operation_count() const { return operation_count_; }
	void SetNow(uint64_t now_ms) { now_ms_ = now_ms; }
	void RestoreSystemDirectoryEntry()
	{
		corrupt_system_entry_after_create_ = false;
	}

private:
	static LifecycleSaveNode Node(int descriptor, ino_t inode, mode_t mode,
		uid_t uid, gid_t gid)
	{
		LifecycleSaveNode node = {};
		node.descriptor = descriptor;
		node.identity.st_dev = static_cast<dev_t>(0x6f6f);
		node.identity.st_ino = static_cast<ino_t>(0x6f600 + inode);
		node.identity.st_mode = mode;
		node.identity.st_nlink = 1;
		node.identity.st_uid = uid;
		node.identity.st_gid = gid;
		node.mount_id = 77;
		return node;
	}
	const LifecycleSaveNode *NodeFor(int descriptor) const
	{
		const LifecycleSaveNode *const nodes[] = {
			&root_, &parent_, &save_root_, &system_, &file_};
		for (size_t index = 0; index < sizeof(nodes) / sizeof(nodes[0]); ++index)
			if (nodes[index]->descriptor == descriptor) return nodes[index];
		return nullptr;
	}

	int failing_mount_call_;
	int mount_calls_;
	std::vector<char> trace_;
	bool corrupt_system_entry_after_create_;
	bool file_created_;
	bool fail_first_root_open_once_;
	uint64_t now_ms_;
	size_t operation_count_;
	LifecycleSaveNode root_;
	LifecycleSaveNode parent_;
	LifecycleSaveNode save_root_;
	LifecycleSaveNode system_;
	LifecycleSaveNode file_;
};

enum class Event : uint8_t {
	preflight,
	retain_content,
	acquire_containment_mappings,
	acquire_fpga,
	enable_bridges,
	start_core_protocol,
	start_video,
	start_audio,
	start_audio_video,
	open_input_descriptors,
	open_save,
	start_scheduler,
	start_offload,
	stop_scheduler,
	reject_join_offload,
	flush_close_save,
	replay_digital_neutral,
	capture_digital_neutral,
	close_input_descriptors,
	stop_video,
	stop_audio,
	stop_audio_video,
	close_content,
	shutdown_core_protocol,
	terminal_fpga_cleanup,
	destruct_video,
	destruct_audio,
	destruct_core_protocol,
	destruct_fpga_mappings,
	destruct_scheduler,
	destruct_offload,
	destruct_input_descriptors,
	destruct_save,
	destruct_content
};

enum class MalformedProtocolOutcome : uint8_t {
	none,
	success_without_completion,
	failure_without_completion,
	success_after_failure_completion
};

class FakeResources final : public NativePreflight,
	public NativeHardwareResources,
	public NativeAudioResource,
	public NativeVideoResource,
	public NativeAudioVideoResource,
	public NativeSchedulerResource,
	public NativeOffloadResource,
	public NativeSaveResource,
	public NativeContentResource,
	public NativeInputDescriptorResource {
public:
	FakeResources(FakeClock &clock, HardwareBroker &broker)
		: clock_(clock), broker_(broker),
		  peripheral_backend_(PeripheralAuthorityTestPeer::Backend(broker_, this)),
		  failed_event_(Event::preflight),
		  fail_enabled_(false),
		  activation_delay_event_(Event::preflight), activation_delay_ms_(0),
		  expected_deadline_ms_(0), saw_wrong_deadline_(false),
		  content_saw_live_generation_(false), failure_after_acquire_(true),
		  captured_valid_{false, false}, captured_(), neutral_(),
		  protocol_profile_(nullptr), protocol_content_(nullptr),
		  shutdown_protocol_profile_(nullptr),
		  malformed_protocol_outcome_(MalformedProtocolOutcome::none),
		  expire_checkout_kind_(OperationKind::program_fpga),
		  expire_checkout_at_ms_(0)
	{
	}

	void HoldConcurrentProtocolPeer()
	{
		hold_concurrent_protocol_peer_ = true;
	}

	void ReleaseConcurrentProtocolPeer()
	{
		concurrent_protocol_peer_.reset();
	}

	void ReturnMalformedProtocolOutcome(MalformedProtocolOutcome outcome)
	{
		malformed_protocol_outcome_ = outcome;
	}

	NativeResourceSet Set()
	{
		return {*this, *this, *this, *this, *this, *this, *this, *this,
			*this, *this};
	}

	void Fail(Event event)
	{
		failed_event_ = event;
		fail_enabled_ = true;
	}

	void FailBeforeAcquisition(Event event)
	{
		Fail(event);
		failure_after_acquire_ = false;
	}

	void ClearFailure()
	{
		fail_enabled_ = false;
		failure_after_acquire_ = true;
	}

	void AddDigitalNeutral(const NativeDigitalNeutral &neutral)
	{
		assert(neutral.player < kNativePlayerCount);
		captured_[neutral.player] = neutral;
		captured_valid_[neutral.player] = true;
	}

	void Delay(Event event, uint64_t delay_ms)
	{
		activation_delay_event_ = event;
		activation_delay_ms_ = delay_ms;
	}

	void AdvanceBrokerClockAfterSaveCleanup(uint64_t now_ms)
	{
		advance_broker_clock_after_save_cleanup_ms_ = now_ms;
	}

	void ExpectActivationDeadline(uint64_t deadline_ms)
	{
		expected_deadline_ms_ = deadline_ms;
	}

	void ExpectCleanupDeadline(uint64_t deadline_ms)
	{
		expected_deadline_ms_ = deadline_ms;
	}

	void AbandonCoreProtocolReleaseOnce()
	{
		abandon_core_protocol_release_once_ = true;
	}

	void ExpireCleanupCheckout(OperationKind kind, uint64_t now_ms)
	{
		expire_checkout_kind_ = kind;
		expire_checkout_at_ms_ = now_ms;
	}

	Result Validate(const NativeCoreProfile &, uint64_t absolute_deadline_ms) override
	{
		return RunBounded(Event::preflight, absolute_deadline_ms);
	}

	NativeAcquisitionOutcome AcquireFpga(const OperationLease &lease) override
	{
		return AcquireHardware(Event::acquire_fpga, lease);
	}

	NativeAcquisitionOutcome AcquireContainmentMappings(
		const OperationLease &lease) override
	{
		return AcquireHardware(Event::acquire_containment_mappings, lease);
	}

	NativeAcquisitionOutcome EnableBridges(const OperationLease &lease) override
	{
		return AcquireHardware(Event::enable_bridges, lease);
	}

	NativeCoreProtocolOutcome StartCoreProtocol(const OperationLease &lease,
		const NativeCoreProfile &profile, NativeContentResource &content) override
	{
		protocol_profile_ = &profile;
		protocol_content_ = &content;
		std::unique_ptr<ActiveCoreProtocolSession> session;
		const Result acquire = CoreProtocolAuthorityTestPeer::AcquireActive(
			lease, broker_, profile, &session);
		if (acquire != MISTER_RESULT_OK) return {acquire, false, false};
		const NativeAcquisitionOutcome outcome =
			AcquireHardware(Event::start_core_protocol, lease);
		if (!outcome.acquired) return {outcome.result, false, false};
		const uint64_t sequence =
			CoreProtocolAuthorityTestPeer::RecordMutation(lease, broker_, *session);
		if (sequence == 0) return {MISTER_RESULT_PLATFORM, true, false};
		if (hold_concurrent_protocol_peer_) {
			const Result begin = CoreProtocolAuthorityTestPeer::BeginConcurrentActive(
				lease, broker_, OperationKind::input, lease.absolute_deadline_ms(),
				&concurrent_protocol_peer_);
			if (begin != MISTER_RESULT_OK) return {begin, true, false};
		}
		if (malformed_protocol_outcome_ ==
			MalformedProtocolOutcome::success_without_completion)
			return {MISTER_RESULT_OK, true, false};
		if (outcome.result == MISTER_RESULT_OK) {
			const ProtocolMappingReleaseReceipt success = {
				MISTER_RESULT_OK, true, true, true, true, true, sequence};
			const Result completed = CoreProtocolAuthorityTestPeer::CompleteSuccess(
				lease, broker_, std::move(session), success);
			return {completed, true, false};
		}
		if (malformed_protocol_outcome_ ==
			MalformedProtocolOutcome::failure_without_completion)
			return {outcome.result, true, false};
		const CoreProtocolResidue residue = {
			false, false, false, false, false, true, true, sequence};
		const ProtocolMappingReleaseReceipt mapping_release = {
			MISTER_RESULT_OK, true, true, true, true, true, sequence};
		const ActiveProtocolFailureReceipt receipt = {
			outcome.result, residue, mapping_release, sequence};
		const Result completed = CoreProtocolAuthorityTestPeer::CompleteFailed(
			lease, broker_, std::move(session), receipt);
		if (malformed_protocol_outcome_ ==
			MalformedProtocolOutcome::success_after_failure_completion &&
			completed == MISTER_RESULT_OK)
			return {MISTER_RESULT_OK, true, true};
		return {completed == MISTER_RESULT_OK ? outcome.result : completed,
			true, completed == MISTER_RESULT_OK};
	}

	NativePeripheralAcquisitionOutcome StartVideo(
		std::unique_ptr<ActiveVideoSessionBundle> &&session) override
	{
		return StartPeripheral(Event::start_video, std::move(session));
	}

	PeripheralBackendIdentity BackendIdentity() const override
	{
		return peripheral_backend_;
	}

	NativePeripheralAcquisitionOutcome StartAudio(
		std::unique_ptr<ActiveAudioSessionBundle> &&session) override
	{
		return StartPeripheral(Event::start_audio, std::move(session));
	}

	Result ReplayDigitalNeutral(const OperationLease &lease,
		const NativeDigitalNeutral &value) override
	{
		const Result result = RunHardware(Event::replay_digital_neutral, lease);
		if (result == MISTER_RESULT_OK) neutral_.push_back(value);
		return result;
	}

	NativePeripheralReleaseOutcome StopVideo(
		std::unique_ptr<CleanupVideoSessionBundle> &&session) override
	{
		const NativePeripheralReleaseOutcome outcome =
			StopPeripheral(Event::stop_video, std::move(session));
		ArmCleanupCheckout(OperationKind::audio);
		return outcome;
	}

	NativePeripheralReleaseOutcome StopAudio(
		std::unique_ptr<CleanupAudioSessionBundle> &&session) override
	{
		const NativePeripheralReleaseOutcome outcome =
			StopPeripheral(Event::stop_audio, std::move(session));
		ArmCleanupCheckout(OperationKind::audio_video);
		return outcome;
	}

	NativePeripheralReleaseOutcome RecoverVideo(
		std::unique_ptr<RecoveryVideoSessionBundle> &&) override
	{
		return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
	}

	NativePeripheralReleaseOutcome RecoverAudio(
		std::unique_ptr<RecoveryAudioSessionBundle> &&) override
	{
		return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
	}

	NativeCoupledAcquisitionOutcome StartAudioVideo(
		std::unique_ptr<ActiveAudioVideoSessionBundle> &&session) override
	{
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, 0, false};
		Result primary = Run(Event::start_audio_video);
		const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		if (primary == MISTER_RESULT_OK && clock_.NowMs() >=
			PeripheralAuthorityTestPeer::Deadline(*session)) {
			const CoupledAcquisitionReceipt receipt = {MISTER_RESULT_DEADLINE,
				affected, true, true, false, broker_.mutation_sequence_for_test(),
				0xa55a};
			const Result completed = PeripheralAuthorityTestPeer::FailAudioVideo(
				broker_, std::move(session), {MISTER_RESULT_DEADLINE, receipt});
			return {completed == MISTER_RESULT_OK ? MISTER_RESULT_DEADLINE :
				completed, affected, true};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK) return {recorded, 0, false};
		const CoupledAcquisitionReceipt receipt = {MISTER_RESULT_OK, affected,
			true, true, false, sequence, 0xa55a};
		const Result completed = primary == MISTER_RESULT_OK ?
			PeripheralAuthorityTestPeer::CompleteAudioVideo(broker_,
				std::move(session), receipt) :
			PeripheralAuthorityTestPeer::FailAudioVideo(broker_,
				std::move(session), {primary, receipt});
		return {completed == MISTER_RESULT_OK ? primary : completed, affected,
			primary == MISTER_RESULT_OK || failure_after_acquire_};
	}

	NativeCoupledReleaseOutcome StopAudioVideo(
		std::unique_ptr<CleanupAudioVideoSessionBundle> &&session) override
	{
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, 0, 0, 0, false,
			false, 0};
		const Result primary = Run(Event::stop_audio_video);
		const uint32_t affected = MISTER_RESOURCE_NATIVE_AUDIO |
			MISTER_RESOURCE_NATIVE_VIDEO;
		const CoupledCompletionReceipt receipt = {MISTER_RESULT_OK, affected, 0,
			MISTER_RESOURCE_NATIVE_VIDEO, true, false,
			broker_.mutation_sequence_for_test(), true, 0xa55a};
		const Result completed = PeripheralAuthorityTestPeer::CompleteAudioVideo(
			broker_, std::move(session), receipt);
		const NativeCoupledReleaseOutcome outcome = {
			completed == MISTER_RESULT_OK ? primary : completed, affected, 0,
			MISTER_RESOURCE_NATIVE_VIDEO, true, false,
			broker_.mutation_sequence_for_test()};
		ArmCleanupCheckout(OperationKind::core_protocol);
		return outcome;
	}

	NativeCoupledReleaseOutcome RecoverAudioVideo(
		std::unique_ptr<RecoveryAudioVideoSessionBundle> &&) override
	{
		return {MISTER_RESULT_UNSUPPORTED, 0, 0, 0, false, false, 0};
	}

	Result ShutdownCoreProtocol(const OperationLease &lease) override
	{
		std::unique_ptr<CleanupCoreProtocolSession> session;
		const Result acquire = CoreProtocolAuthorityTestPeer::AcquireCleanup(
			lease, broker_, &session);
		if (acquire != MISTER_RESULT_OK) return acquire;
		shutdown_protocol_profile_ = protocol_profile_;
		hardware_events.push_back(Event::shutdown_core_protocol);
		hardware_deadlines.push_back(lease.absolute_deadline_ms());
		const Result primary = Run(Event::shutdown_core_protocol);
		if (abandon_core_protocol_release_once_) {
			abandon_core_protocol_release_once_ = false;
			return MISTER_RESULT_PLATFORM;
		}
		if (primary != MISTER_RESULT_OK) return primary;
		const ProtocolMappingReleaseReceipt release = {
			MISTER_RESULT_OK, true, true, true, true, true,
			broker_.mutation_sequence_for_test()};
		const Result completed = CoreProtocolAuthorityTestPeer::CompleteCleanup(
			lease, broker_, std::move(session), release);
		return completed == MISTER_RESULT_OK ? primary : completed;
	}

	Result TerminalFpgaCleanup(const OperationLease &lease) override
	{
		return RunHardware(Event::terminal_fpga_cleanup, lease);
	}
	void CloseVideoForProcessExit() override { Record(Event::destruct_video); }
	void CloseAudioForProcessExit() override { Record(Event::destruct_audio); }
	void CloseAudioVideoForProcessExit() override {}
	void CloseCoreProtocolForProcessExit() override
	{
		Record(Event::destruct_core_protocol);
	}
	void CloseFpgaMappingsForProcessExit() override
	{
		Record(Event::destruct_fpga_mappings);
	}

	NativeAcquisitionOutcome StartScheduler(const OperationLease &lease) override
	{
		return Acquire(Event::start_scheduler, lease.absolute_deadline_ms());
	}
	Result StopScheduler(const OperationLease &lease) override
	{
		return RunBounded(Event::stop_scheduler, lease.absolute_deadline_ms());
	}
	void CloseSchedulerForProcessExit() override { Record(Event::destruct_scheduler); }
	NativeAcquisitionOutcome StartOffload(const OperationLease &lease) override
	{
		return Acquire(Event::start_offload, lease.absolute_deadline_ms());
	}
	Result RejectAndJoinOffload(const OperationLease &lease) override
	{
		return RunBounded(Event::reject_join_offload,
			lease.absolute_deadline_ms());
	}
	void CloseOffloadForProcessExit() override { Record(Event::destruct_offload); }
	NativeSaveOpenOutcome OpenSave(const OperationLease &lease,
		const NativeCoreProfile &profile, const NativeSaveKey &key) override
	{
		save_profile_ = &profile;
		save_key_ = key;
		const NativeAcquisitionOutcome outcome = Acquire(Event::open_save,
			lease.absolute_deadline_ms());
		return {outcome.result, outcome.acquired};
	}
	NativeSaveCloseOutcome FlushAndCloseSave(const OperationLease &lease) override
	{
		if (first_save_cleanup_lease_ == nullptr)
			first_save_cleanup_lease_ = &lease;
		else
			save_cleanup_lease_reused_ = first_save_cleanup_lease_ == &lease;
		bounded_events.push_back(Event::flush_close_save);
		bounded_deadlines.push_back(lease.absolute_deadline_ms());
		const Result result = Run(Event::flush_close_save);
		if (result == MISTER_RESULT_OK &&
			advance_broker_clock_after_save_cleanup_ms_ != 0)
			clock_.SetNow(advance_broker_clock_after_save_cleanup_ms_);
		const bool complete = result == MISTER_RESULT_OK;
		return {result, complete, complete, complete, false};
	}
	NativeSaveCloseOutcome RecoverSave(const OperationLease &,
		const SafeSaveRecoveryRecord &) override
	{ return {MISTER_RESULT_UNSUPPORTED, false, false, false, false}; }
	void CloseSaveForProcessExit() override { Record(Event::destruct_save); }
	Result DeriveSaveKey(const NativeCoreProfile &profile, NativeSaveKey *key) override
	{
		return key != nullptr && MakeNativeSaveKey(profile,
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
			key) ? MISTER_RESULT_OK : MISTER_RESULT_PLATFORM;
	}
	NativeAcquisitionOutcome RetainContent(uint64_t absolute_deadline_ms) override
	{
		content_saw_live_generation_ = broker_.has_live_generation_for_test();
		return Acquire(Event::retain_content, absolute_deadline_ms);
	}
	Result CloseContent(uint64_t absolute_deadline_ms) override
	{
		const Result result = RunBounded(Event::close_content, absolute_deadline_ms);
		return result;
	}
	Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t absolute_deadline_ms) override
	{
		if (description == nullptr) return MISTER_RESULT_INVALID_ARGUMENT;
		const Result result = RunBounded(Event::retain_content,
			absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		description->size = 1;
		description->extension_length = 3;
		description->extension[0] = 's';
		description->extension[1] = 'f';
		description->extension[2] = 'c';
		description->extension[3] = '\0';
		return MISTER_RESULT_OK;
	}
	Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms) override
	{
		if (bytes == nullptr || offset != 0 || count != 1)
			return MISTER_RESULT_INVALID_ARGUMENT;
		const Result result = RunBounded(Event::retain_content,
			absolute_deadline_ms);
		if (result != MISTER_RESULT_OK) return result;
		static_cast<uint8_t *>(bytes)[0] = 0;
		return MISTER_RESULT_OK;
	}
	void CloseContentForProcessExit() override { Record(Event::destruct_content); }
	NativeAcquisitionOutcome OpenInputDescriptors(
		const OperationLease &lease) override
	{
		return Acquire(Event::open_input_descriptors,
			lease.absolute_deadline_ms());
	}
	Result CloseInputDescriptors(const OperationLease &lease) override
	{
		const Result result = RunBounded(Event::close_input_descriptors,
			lease.absolute_deadline_ms());
		ArmCleanupCheckout(OperationKind::video);
		return result;
	}
	void CloseInputDescriptorsForProcessExit() override
	{
		Record(Event::destruct_input_descriptors);
	}
	Result CaptureDigitalNeutral(const OperationLease &lease,
		NativeDigitalNeutral *values, bool *valid, size_t count) override
	{
		if (values == nullptr || valid == nullptr || count != kNativePlayerCount)
			return MISTER_RESULT_INVALID_ARGUMENT;
		const Result result = RunBounded(Event::capture_digital_neutral,
			lease.absolute_deadline_ms());
		if (result != MISTER_RESULT_OK) return result;
		for (size_t player = 0; player < count; ++player) {
			values[player] = captured_[player];
			valid[player] = captured_valid_[player];
		}
		return MISTER_RESULT_OK;
	}

	size_t Count(Event event) const
	{
		return static_cast<size_t>(std::count(events.begin(), events.end(), event));
	}

	uint64_t LastHardwareDeadline(Event event) const
	{
		for (size_t index = hardware_events.size(); index != 0; --index) {
			if (hardware_events[index - 1] == event)
				return hardware_deadlines[index - 1];
		}
		return 0;
	}

	uint64_t LastBoundedDeadline(Event event) const
	{
		for (size_t index = bounded_events.size(); index != 0; --index) {
			if (bounded_events[index - 1] == event)
				return bounded_deadlines[index - 1];
		}
		return 0;
	}

	FakeClock &clock_;
	HardwareBroker &broker_;
	PeripheralBackendIdentity peripheral_backend_;
	std::vector<Event> events;
	std::vector<Event> hardware_events;
	std::vector<uint64_t> hardware_deadlines;
	std::vector<Event> bounded_events;
	std::vector<uint64_t> bounded_deadlines;
	Event failed_event_;
	bool fail_enabled_;
	Event activation_delay_event_;
	uint64_t activation_delay_ms_;
	uint64_t advance_broker_clock_after_save_cleanup_ms_ = 0;
	uint64_t expected_deadline_ms_;
	bool saw_wrong_deadline_;
	bool content_saw_live_generation_;
	bool failure_after_acquire_;
	bool captured_valid_[kNativePlayerCount];
	NativeDigitalNeutral captured_[kNativePlayerCount];
	std::vector<NativeDigitalNeutral> neutral_;
	const NativeCoreProfile *protocol_profile_;
	NativeContentResource *protocol_content_;
	const NativeCoreProfile *save_profile_ = nullptr;
	NativeSaveKey save_key_ = {};
	const OperationLease *first_save_cleanup_lease_ = nullptr;
	bool save_cleanup_lease_reused_ = false;
	const NativeCoreProfile *shutdown_protocol_profile_;
	MalformedProtocolOutcome malformed_protocol_outcome_;
	bool abandon_core_protocol_release_once_ = false;
	bool hold_concurrent_protocol_peer_ = false;
	std::unique_ptr<OperationLease> concurrent_protocol_peer_;
	OperationKind expire_checkout_kind_;
	uint64_t expire_checkout_at_ms_;

private:
	void ArmCleanupCheckout(OperationKind kind)
	{
		if (expire_checkout_kind_ != kind || expire_checkout_at_ms_ == 0) return;
		clock_.AdvanceOnRead(3, expire_checkout_at_ms_);
		expire_checkout_at_ms_ = 0;
	}
	template <typename Session>
	NativePeripheralAcquisitionOutcome StartPeripheral(Event event,
		std::unique_ptr<Session> &&session)
	{
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false};
		const uint64_t deadline = PeripheralAuthorityTestPeer::Deadline(*session);
		hardware_events.push_back(event);
		hardware_deadlines.push_back(deadline);
		Result primary = Run(event);
		if (primary == MISTER_RESULT_OK && clock_.NowMs() >=
			deadline) {
			const PeripheralCompletionReceipt receipt = {MISTER_RESULT_DEADLINE,
				true, true, true, true, false,
				broker_.mutation_sequence_for_test(), 0xa55a};
			const Result completed = PeripheralAuthorityTestPeer::FailVideo(broker_,
				std::move(session), {MISTER_RESULT_DEADLINE, receipt});
			return {completed == MISTER_RESULT_OK ? MISTER_RESULT_DEADLINE :
				completed, true};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK) return {recorded, false};
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, sequence, 0xa55a};
		const Result completed = primary == MISTER_RESULT_OK ?
			PeripheralAuthorityTestPeer::CompleteVideo(broker_, std::move(session),
				receipt) : PeripheralAuthorityTestPeer::FailVideo(broker_,
				std::move(session), {primary, receipt});
		const Result final_result = completed == MISTER_RESULT_OK ? primary :
			completed;
		return {final_result, primary == MISTER_RESULT_OK ||
			failure_after_acquire_};
	}

	NativePeripheralAcquisitionOutcome StartPeripheral(Event event,
		std::unique_ptr<ActiveAudioSessionBundle> &&session)
	{
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false};
		const uint64_t deadline = PeripheralAuthorityTestPeer::Deadline(*session);
		hardware_events.push_back(event);
		hardware_deadlines.push_back(deadline);
		Result primary = Run(event);
		if (primary == MISTER_RESULT_OK && clock_.NowMs() >=
			deadline) {
			const PeripheralCompletionReceipt receipt = {MISTER_RESULT_DEADLINE,
				true, true, true, true, false,
				broker_.mutation_sequence_for_test(), 0xa55a};
			const Result completed = PeripheralAuthorityTestPeer::FailAudio(broker_,
				std::move(session), {MISTER_RESULT_DEADLINE, receipt});
			return {completed == MISTER_RESULT_OK ? MISTER_RESULT_DEADLINE :
				completed, true};
		}
		uint64_t sequence = 0;
		const Result recorded = PeripheralAuthorityTestPeer::Record(broker_,
			*session, &sequence);
		if (recorded != MISTER_RESULT_OK) return {recorded, false};
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, sequence, 0xa55a};
		const Result completed = primary == MISTER_RESULT_OK ?
			PeripheralAuthorityTestPeer::CompleteAudio(broker_, std::move(session),
				receipt) : PeripheralAuthorityTestPeer::FailAudio(broker_,
				std::move(session), {primary, receipt});
		const Result final_result = completed == MISTER_RESULT_OK ? primary :
			completed;
		return {final_result, primary == MISTER_RESULT_OK ||
			failure_after_acquire_};
	}

	template <typename Session>
	NativePeripheralReleaseOutcome StopPeripheral(Event event,
		std::unique_ptr<Session> &&session)
	{
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false, false,
			false, false};
		hardware_events.push_back(event);
		hardware_deadlines.push_back(PeripheralAuthorityTestPeer::Deadline(*session));
		const Result primary = Run(event);
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, broker_.mutation_sequence_for_test(), 0xa55a};
		const Result completed = primary == MISTER_RESULT_OK ?
			PeripheralAuthorityTestPeer::CompleteVideo(broker_, std::move(session),
				receipt) : PeripheralAuthorityTestPeer::AbandonVideo(broker_,
				std::move(session), {primary, receipt});
		return {completed == MISTER_RESULT_OK ? primary : completed,
			primary == MISTER_RESULT_OK, false, primary == MISTER_RESULT_OK, false};
	}

	NativePeripheralReleaseOutcome StopPeripheral(Event event,
		std::unique_ptr<CleanupAudioSessionBundle> &&session)
	{
		if (!session) return {MISTER_RESULT_INVALID_ARGUMENT, false, false,
			false, false};
		hardware_events.push_back(event);
		hardware_deadlines.push_back(PeripheralAuthorityTestPeer::Deadline(*session));
		const Result primary = Run(event);
		const PeripheralCompletionReceipt receipt = {MISTER_RESULT_OK, true,
			true, true, true, false, broker_.mutation_sequence_for_test(), 0xa55a};
		const Result completed = primary == MISTER_RESULT_OK ?
			PeripheralAuthorityTestPeer::CompleteAudio(broker_, std::move(session),
				receipt) : PeripheralAuthorityTestPeer::AbandonAudio(broker_,
				std::move(session), {primary, receipt});
		return {completed == MISTER_RESULT_OK ? primary : completed,
			primary == MISTER_RESULT_OK, false, primary == MISTER_RESULT_OK, false};
	}

	void Record(Event event)
	{
		events.push_back(event);
	}

	Result Run(Event event)
	{
		Record(event);
		if (event == activation_delay_event_) clock_.Advance(activation_delay_ms_);
		return fail_enabled_ && event == failed_event_ ?
			MISTER_RESULT_PLATFORM : MISTER_RESULT_OK;
	}

	Result RunBounded(Event event, uint64_t absolute_deadline_ms)
	{
		bounded_events.push_back(event);
		bounded_deadlines.push_back(absolute_deadline_ms);
		const Result result = Run(event);
		if (result != MISTER_RESULT_OK) return result;
		return clock_.NowMs() >= absolute_deadline_ms ?
			MISTER_RESULT_DEADLINE : MISTER_RESULT_OK;
	}

	Result RunHardware(Event event, const OperationLease &lease)
	{
		hardware_events.push_back(event);
		hardware_deadlines.push_back(lease.absolute_deadline_ms());
		if (expected_deadline_ms_ != 0 &&
			lease.absolute_deadline_ms() != expected_deadline_ms_) {
			saw_wrong_deadline_ = true;
		}
		return Run(event);
	}

	NativeAcquisitionOutcome Acquire(Event event, uint64_t absolute_deadline_ms)
	{
		const Result result = RunBounded(event, absolute_deadline_ms);
		return {result, result == MISTER_RESULT_OK || failure_after_acquire_};
	}

	NativeAcquisitionOutcome AcquireHardware(Event event,
		const OperationLease &lease)
	{
		Result result = RunHardware(event, lease);
		if (result == MISTER_RESULT_OK &&
			clock_.NowMs() >= lease.absolute_deadline_ms()) {
			result = MISTER_RESULT_DEADLINE;
		}
		return {result, result == MISTER_RESULT_OK || failure_after_acquire_};
	}
};

class LifecycleContainmentIo final : public NativeContainmentIo {
public:
	NativeManagerNeutralReceipt ReconcileManager(const Access &) override
	{
		const NativeManagerNeutralReceipt receipt = {
			MISTER_RESULT_OK, 0x2u, 0, true, false, false};
		return receipt;
	}
	Result WriteCoreReset(const Access &, uint32_t, uint32_t) override
	{ return MISTER_RESULT_OK; }
	Result WriteInterfaceModule(const Access &, uint32_t) override
	{ return MISTER_RESULT_OK; }
	Result WriteSdrPortControl(const Access &, uint32_t, uint32_t) override
	{ return MISTER_RESULT_OK; }
	Result WriteBridgeReset(const Access &, uint32_t) override
	{ return MISTER_RESULT_OK; }
	Result WriteRemap(const Access &, uint32_t) override
	{ return MISTER_RESULT_OK; }
	Result ReadCoreGpo(const Access &, uint32_t *value) override
	{ if (value == nullptr) return MISTER_RESULT_INVALID_ARGUMENT; *value = 0x40000000u; return MISTER_RESULT_OK; }
	Result ReadInterfaceModule(const Access &, uint32_t *value) override
	{ if (value == nullptr) return MISTER_RESULT_INVALID_ARGUMENT; *value = 0; return MISTER_RESULT_OK; }
	Result ReadSdrPortControl(const Access &, uint32_t, uint32_t *value) override
	{ if (value == nullptr) return MISTER_RESULT_INVALID_ARGUMENT; *value = 0; return MISTER_RESULT_OK; }
	Result ReadBridgeReset(const Access &, uint32_t *value) override
	{ if (value == nullptr) return MISTER_RESULT_INVALID_ARGUMENT; *value = 7; return MISTER_RESULT_OK; }
	Result ReadRemap(const Access &, uint32_t *value) override
	{ if (value == nullptr) return MISTER_RESULT_INVALID_ARGUMENT; *value = 1; return MISTER_RESULT_OK; }
	Result ReleaseMappings(const Access &) override { return MISTER_RESULT_OK; }
};

class LifecycleHardwareWithContainment final : public NativeHardwareResources {
public:
	LifecycleHardwareWithContainment(FakeResources &delegate,
		NativeContainment &containment)
		: delegate_(delegate), containment_(containment) {}
	NativeAcquisitionOutcome AcquireFpga(const OperationLease &lease) override
	{ return delegate_.AcquireFpga(lease); }
	NativeAcquisitionOutcome AcquireContainmentMappings(
		const OperationLease &lease) override
	{ return delegate_.AcquireContainmentMappings(lease); }
	NativeAcquisitionOutcome EnableBridges(const OperationLease &lease) override
	{ return delegate_.EnableBridges(lease); }
	NativeCoreProtocolOutcome StartCoreProtocol(const OperationLease &lease,
		const NativeCoreProfile &profile, NativeContentResource &content) override
	{ return delegate_.StartCoreProtocol(lease, profile, content); }
	Result ReplayDigitalNeutral(const OperationLease &lease,
		const NativeDigitalNeutral &neutral) override
	{ return delegate_.ReplayDigitalNeutral(lease, neutral); }
	Result ShutdownCoreProtocol(const OperationLease &lease) override
	{ return delegate_.ShutdownCoreProtocol(lease); }
	Result TerminalFpgaCleanup(const OperationLease &lease) override
	{ return containment_.ResetAndContain(lease); }
	void CloseCoreProtocolForProcessExit() override
	{ delegate_.CloseCoreProtocolForProcessExit(); }
	void CloseFpgaMappingsForProcessExit() override
	{ delegate_.CloseFpgaMappingsForProcessExit(); }

private:
	FakeResources &delegate_;
	NativeContainment &containment_;
};

const Event kActivationEvents[] = {
	Event::acquire_containment_mappings,
	Event::start_offload,
	Event::acquire_fpga,
	Event::enable_bridges,
	Event::start_core_protocol,
	Event::start_video,
	Event::start_audio,
	Event::start_audio_video,
	Event::open_input_descriptors,
	Event::open_save,
	Event::start_scheduler
};

const Event kCleanupForFailedAcquisition[] = {
	Event::terminal_fpga_cleanup,
	Event::reject_join_offload,
	Event::terminal_fpga_cleanup,
	Event::terminal_fpga_cleanup,
	Event::shutdown_core_protocol,
	Event::stop_video,
	Event::stop_audio,
	Event::stop_audio_video,
	Event::close_input_descriptors,
	Event::flush_close_save,
	Event::stop_scheduler
};

static_assert(sizeof(kActivationEvents) == sizeof(kCleanupForFailedAcquisition),
	"every acquisition failure needs a matching unwind observation");

struct Fixture {
	explicit Fixture(uint64_t now_ms)
		: clock(now_ms), broker(clock), resources(clock, broker), set(resources.Set()),
		  lifecycle(clock, broker, set),
		  profile(*FixtureNativeCoreProfile("snes"))
	{
	}

	FakeClock clock;
	HardwareBroker broker;
	FakeResources resources;
	NativeResourceSet set;
	NativeLifecycle lifecycle;
	const NativeCoreProfile &profile;
};

void TestPreflightFailureNeverAcquiresOwnership()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::preflight);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.lifecycle.state() == NativeLifecycleState::idle);
	assert(fixture.lifecycle.generation() == 0);
	assert(fixture.lifecycle.ledger().resource_flags == 0);
	assert(fixture.resources.events.size() == 1);
	assert(fixture.resources.events[0] == Event::preflight);
	assert(fixture.resources.Count(Event::retain_content) == 0);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 0);
}

void TestContentIsResolvedBeforeOwnershipAndRetainedOnFirstHardwareFailure()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::acquire_fpga);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.resources.events.size() >= 4);
	assert(fixture.resources.events[0] == Event::preflight);
	assert(fixture.resources.events[1] == Event::retain_content);
	assert(!fixture.resources.content_saw_live_generation_);
	assert(fixture.resources.events[2] == Event::acquire_containment_mappings);
	assert(fixture.resources.events[3] == Event::start_offload);
	assert(fixture.resources.events[4] == Event::acquire_fpga);
	assert(fixture.resources.Count(Event::close_content) == 1);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
}

void TestFirstAcquisitionIsLedgeredBeforeFollowingFailure()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::acquire_fpga);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.lifecycle.generation() != 0);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	assert(fixture.lifecycle.cleanup_timing().established);
	assert(fixture.resources.Count(Event::close_content) == 1);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CONTENT) == 0);
	assert((fixture.lifecycle.ledger().resource_flags & MISTER_RESOURCE_FPGA) != 0);
}

void TestSuccessfulInitializationRecordsEveryAcquisition()
{
	Fixture fixture(100);
	fixture.resources.ExpectActivationDeadline(1000);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	assert(fixture.lifecycle.state() == NativeLifecycleState::active);
	assert(fixture.lifecycle.generation() != 0);
	assert(fixture.lifecycle.ledger().resource_flags == MISTER_RESOURCE_V2_KNOWN);
	assert(fixture.lifecycle.ledger().scheduler);
	assert(fixture.lifecycle.ledger().offload);
	assert(fixture.lifecycle.ledger().input_descriptors);
	assert(!fixture.resources.saw_wrong_deadline_);
	assert(fixture.resources.Count(Event::retain_content) == 1);
	assert(fixture.resources.save_profile_ == &fixture.profile);
	assert(fixture.resources.save_key_.system_id == NativeSystem::snes);
	for (size_t index = 0;
		index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]); ++index) {
		assert(fixture.resources.Count(kActivationEvents[index]) == 1);
	}
}

void TestActivationUsesAcceptedContainmentFirstOrder()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const Event expected[] = {
		Event::preflight, Event::retain_content,
		Event::acquire_containment_mappings, Event::start_offload,
		Event::acquire_fpga, Event::enable_bridges,
		Event::start_core_protocol, Event::start_video, Event::start_audio,
		Event::start_audio_video, Event::open_input_descriptors,
		Event::open_save, Event::start_scheduler};
	assert(fixture.resources.events.size() ==
		sizeof(expected) / sizeof(expected[0]));
	for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index)
		assert(fixture.resources.events[index] == expected[index]);
}

void TestStopCallbackDeadlineBoundsFirstCleanupOperation()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	fixture.resources.ExpectCleanupDeadline(150);
	assert(fixture.lifecycle.Stop(150) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(!fixture.resources.saw_wrong_deadline_);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
}

void TestStopQuiesceExpiryFreezesDrainAndCleanupTiming()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> held;
	assert(fixture.broker.Begin(fixture.lifecycle.generation(),
		OperationKind::input, 10000, &held) == MISTER_RESULT_OK);
	fixture.clock.ForceTimeout(true);
	assert(fixture.lifecycle.Stop(150) == MISTER_RESULT_DEADLINE);
	const NativeFailureDrainTiming drain =
		fixture.lifecycle.failure_drain_timing_for_test();
	const NativeCleanupTiming cleanup = fixture.lifecycle.cleanup_timing();
	assert(drain.established && drain.drain_start_ms == 100 &&
		drain.drain_deadline_ms == 2100);
	assert(cleanup.established && cleanup.cleanup_start_ms == 100 &&
		cleanup.non_fpga_deadline_ms == 2100 &&
		cleanup.fpga_deadline_ms == 5100);
	fixture.clock.SetNow(200);
	held.reset();
	fixture.clock.ForceTimeout(false);
	assert(fixture.lifecycle.Stop(500) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.failure_drain_timing_for_test().drain_deadline_ms ==
		drain.drain_deadline_ms);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms ==
		cleanup.cleanup_start_ms);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms ==
		cleanup.non_fpga_deadline_ms);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms ==
		cleanup.fpga_deadline_ms);
}

void TestRealAdapterRootPrefixFailuresReleaseSavesAndLeaveTheGeneration()
{
	const int mount_failures[] = {1, 4};
	for (int failing_mount_call : mount_failures) {
		FakeClock clock(100);
		HardwareBroker broker(clock);
		FakeResources resources(clock, broker);
		LifecycleContainmentIo containment_io;
		NativeContainment containment(broker, containment_io);
		LifecycleHardwareWithContainment hardware(resources, containment);
		LifecycleSaveFileSystem filesystem(failing_mount_call);
		linux_native::NativeSaveAdapter save(broker, filesystem);
		NativeResourceSet set = {resources, hardware, resources, resources,
			resources, resources, resources, save, resources, resources};
		NativeLifecycle lifecycle(clock, broker, set);
		const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
		assert(profile != nullptr);

		// Removing the lifecycle's positive cleanup handling makes this remain
		// in cleanup with SAVES set despite the real adapter closing every fd.
		assert(lifecycle.ActivateFixtureForTest(*profile, 1000) ==
			MISTER_RESULT_PLATFORM);
		assert(lifecycle.state() == NativeLifecycleState::idle);
		assert(lifecycle.generation() == 0);
		assert(lifecycle.ledger().resource_flags == 0);
		assert(lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_OK);
		assert(filesystem.trace().size() ==
			static_cast<size_t>(failing_mount_call));
		assert(filesystem.trace().back() == '0');
	}
}

void TestRealAdapterZeroDescriptorFailuresClearStateForReuse()
{
	struct FailureCase {
		bool fail_first_root_open_once;
		uint64_t save_clock_now_ms;
		Result expected_result;
		size_t filesystem_operations;
	};
	const FailureCase cases[] = {
		{true, 100, MISTER_RESULT_PLATFORM, 1},
		{false, 1000, MISTER_RESULT_DEADLINE, 0}
	};
	for (const FailureCase &test : cases) {
		LifecycleSaveFileSystem filesystem(0, false,
			test.fail_first_root_open_once, test.save_clock_now_ms);
		size_t operations_before_adapter_destructor = 0;
		{
			FakeClock clock(100);
			HardwareBroker broker(clock);
			FakeResources resources(clock, broker);
			LifecycleContainmentIo containment_io;
			NativeContainment containment(broker, containment_io);
			LifecycleHardwareWithContainment hardware(resources, containment);
			linux_native::NativeSaveAdapter save(broker, filesystem);
			NativeResourceSet set = {resources, hardware, resources, resources,
				resources, resources, resources, save, resources, resources};
			NativeLifecycle lifecycle(clock, broker, set);
			const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
			assert(profile != nullptr);

			// These leaves acquired no descriptor, so lifecycle does not own SAVES;
			// the real adapter must also discard every provisional authority state.
			assert(lifecycle.ActivateFixtureForTest(*profile, 1000) ==
				test.expected_result);
			assert(lifecycle.state() == NativeLifecycleState::idle);
			assert(lifecycle.generation() == 0);
			assert((lifecycle.ledger().resource_flags & MISTER_RESOURCE_SAVES) == 0);
			assert(filesystem.operation_count() == test.filesystem_operations);
			assert(lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_OK);
			assert(filesystem.operation_count() == test.filesystem_operations);

			filesystem.SetNow(100);
			assert(lifecycle.ActivateFixtureForTest(*profile, 1000) ==
				MISTER_RESULT_OK);
			assert(lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_OK);
			operations_before_adapter_destructor = filesystem.operation_count();
		}
		assert(filesystem.operation_count() == operations_before_adapter_destructor);
	}
}

void TestRealAdapterZeroDescriptorFailureDestructorPerformsNoIo()
{
	struct FailureCase {
		bool fail_first_root_open_once;
		uint64_t save_clock_now_ms;
		Result expected_result;
		size_t filesystem_operations;
	};
	const FailureCase cases[] = {
		{true, 100, MISTER_RESULT_PLATFORM, 1},
		{false, 1000, MISTER_RESULT_DEADLINE, 0}
	};
	for (const FailureCase &test : cases) {
		LifecycleSaveFileSystem filesystem(0, false,
			test.fail_first_root_open_once, test.save_clock_now_ms);
		{
			FakeClock clock(100);
			HardwareBroker broker(clock);
			FakeResources resources(clock, broker);
			LifecycleContainmentIo containment_io;
			NativeContainment containment(broker, containment_io);
			LifecycleHardwareWithContainment hardware(resources, containment);
			linux_native::NativeSaveAdapter save(broker, filesystem);
			NativeResourceSet set = {resources, hardware, resources, resources,
				resources, resources, resources, save, resources, resources};
			NativeLifecycle lifecycle(clock, broker, set);
			const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
			assert(profile != nullptr);

			assert(lifecycle.ActivateFixtureForTest(*profile, 1000) ==
				test.expected_result);
			assert(lifecycle.state() == NativeLifecycleState::idle);
			assert((lifecycle.ledger().resource_flags & MISTER_RESOURCE_SAVES) == 0);
			assert(filesystem.operation_count() == test.filesystem_operations);
		}
		assert(filesystem.operation_count() == test.filesystem_operations);
	}
}

void TestRealAdapterPostCreateAuthorityLossRetainsSavesUntilStableCleanup()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeResources resources(clock, broker);
	LifecycleContainmentIo containment_io;
	NativeContainment containment(broker, containment_io);
	LifecycleHardwareWithContainment hardware(resources, containment);
	LifecycleSaveFileSystem filesystem(0, true);
	linux_native::NativeSaveAdapter save(broker, filesystem);
	NativeResourceSet set = {resources, hardware, resources, resources,
		resources, resources, resources, save, resources, resources};
	NativeLifecycle lifecycle(clock, broker, set);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);

	// A created inode whose system entry cannot be revalidated has neither the
	// unconditional final data-sync fact nor a stable entry proof.  Returning a
	// positive close receipt here would incorrectly clear SAVES and Leave.
	assert(lifecycle.ActivateFixtureForTest(*profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(lifecycle.state() == NativeLifecycleState::cleanup);
	assert(lifecycle.generation() != 0);
	assert((lifecycle.ledger().resource_flags & MISTER_RESOURCE_SAVES) != 0);
	assert(filesystem.trace().size() == 2);
	assert(filesystem.trace()[0] == 'f');
	assert(filesystem.trace()[1] == 's');

	assert(lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(lifecycle.state() == NativeLifecycleState::cleanup);
	assert((lifecycle.ledger().resource_flags & MISTER_RESOURCE_SAVES) != 0);
	assert(filesystem.trace().size() == 2);

	filesystem.RestoreSystemDirectoryEntry();
	assert(lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_OK);
	assert(lifecycle.state() == NativeLifecycleState::idle);
	assert(lifecycle.generation() == 0);
	assert(lifecycle.ledger().resource_flags == 0);
	const char expected[] = {'f', 's', 'd', '4', '3', '2', '1', '0'};
	assert(filesystem.trace().size() == sizeof(expected));
	for (size_t index = 0; index < sizeof(expected); ++index)
		assert(filesystem.trace()[index] == expected[index]);
}

void TestCoreProtocolReceivesAdmittedProfileAndRetainedContent()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	assert(fixture.resources.protocol_profile_ == &fixture.profile);
	assert(fixture.resources.protocol_content_ == &fixture.resources);
}

void TestRetainedContentDescriptionAndReadStayBounded()
{
	Fixture fixture(100);
	NativeRetainedContentDescription description = {};
	assert(fixture.resources.DescribeRetained(&description, 1000) ==
		MISTER_RESULT_OK);
	assert(description.size == 1);
	assert(description.extension_length == 3);
	assert(strcmp(description.extension, "sfc") == 0);
	uint8_t byte = 0xff;
	assert(fixture.resources.ReadRetainedAt(0, &byte, 1, 1000) ==
		MISTER_RESULT_OK);
	assert(byte == 0);
}

void TestFailureAfterEveryAcquisitionUnwindsWithoutChangingResult()
{
	for (size_t fail_index = 0;
		fail_index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
		++fail_index) {
		Fixture fixture(100);
		fixture.resources.Fail(kActivationEvents[fail_index]);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.lifecycle.latched_activation_result() ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
		assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
		assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
		assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5100);
		for (size_t index = 0; index <= fail_index; ++index)
			assert(fixture.resources.Count(kActivationEvents[index]) == 1);
		for (size_t index = fail_index + 1;
			index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
			++index) {
			assert(fixture.resources.Count(kActivationEvents[index]) == 0);
		}
		assert(fixture.resources.Count(kCleanupForFailedAcquisition[fail_index]) >= 1);
	}
}

void TestContentFailureOrOverrunNeverMintsOwnership()
{
	Fixture failed(100);
	failed.resources.FailBeforeAcquisition(Event::retain_content);
	assert(failed.lifecycle.ActivateFixtureForTest(failed.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(failed.lifecycle.state() == NativeLifecycleState::idle);
	assert(failed.lifecycle.generation() == 0);
	assert(failed.resources.Count(Event::acquire_fpga) == 0);
	assert(failed.resources.Count(Event::close_content) == 0);

	Fixture partial(100);
	partial.resources.Fail(Event::retain_content);
	assert(partial.lifecycle.ActivateFixtureForTest(partial.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(partial.lifecycle.state() == NativeLifecycleState::idle);
	assert(partial.lifecycle.generation() == 0);
	assert(partial.resources.Count(Event::close_content) == 1);
	assert(partial.lifecycle.ledger().resource_flags == 0);

	Fixture overrun(100);
	overrun.resources.Delay(Event::retain_content, 900);
	assert(overrun.lifecycle.ActivateFixtureForTest(overrun.profile, 1000) ==
		MISTER_RESULT_DEADLINE);
	assert(overrun.lifecycle.state() == NativeLifecycleState::cleanup);
	assert(overrun.lifecycle.generation() == 0);
	assert(overrun.resources.Count(Event::acquire_fpga) == 0);
	assert(overrun.resources.Count(Event::close_content) == 1);
	assert((overrun.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CONTENT) != 0);
	overrun.clock.SetNow(1001);
	assert(overrun.lifecycle.Stop(1500) == MISTER_RESULT_OK);
}

void TestPreownershipContentCloseFailureBlocksReactivationAndRetriesLocally()
{
	Fixture fixture(100);
	fixture.resources.Delay(Event::retain_content, 900);
	fixture.resources.Fail(Event::close_content);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	assert(fixture.lifecycle.generation() == 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CONTENT) != 0);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 1000);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 3000);
	assert(fixture.resources.LastBoundedDeadline(Event::close_content) == 1000);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 2000) ==
		MISTER_RESULT_INVALID_STATE);
	assert(fixture.resources.Count(Event::retain_content) == 1);
	fixture.resources.ClearFailure();
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_OK);
	assert(fixture.lifecycle.state() == NativeLifecycleState::idle);
	assert(fixture.lifecycle.ledger().resource_flags == 0);
	assert(fixture.resources.Count(Event::close_content) == 2);
	assert(fixture.resources.LastBoundedDeadline(Event::close_content) == 3000);
}

void TestOverrunAfterEverySuccessfulAcquisitionIsLedgeredAndUnwound()
{
	for (size_t fail_index = 0;
		fail_index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
		++fail_index) {
		Fixture fixture(100);
		fixture.resources.Delay(kActivationEvents[fail_index], 900);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_DEADLINE);
		assert(fixture.lifecycle.latched_activation_result() ==
			MISTER_RESULT_DEADLINE);
		assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
		assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 1000);
		for (size_t index = 0; index <= fail_index; ++index)
			assert(fixture.resources.Count(kActivationEvents[index]) == 1);
		for (size_t index = fail_index + 1;
			index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]);
			++index) {
			assert(fixture.resources.Count(kActivationEvents[index]) == 0);
		}
	}
}

void TestActivationOverrunStaysFailedAndStartsFreshCleanupClocksOnce()
{
	Fixture fixture(100);
	fixture.resources.Delay(Event::start_scheduler, 100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 200) ==
		MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.latched_activation_result() == MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	const NativeCleanupTiming first = fixture.lifecycle.cleanup_timing();
	assert(first.established);
	assert(first.cleanup_start_ms == 200);
	assert(first.non_fpga_deadline_ms == 2200);
	assert(first.fpga_deadline_ms == 5200);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == 0);

	fixture.clock.SetNow(300);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const uint64_t epoch = fixture.lifecycle.cleanup_epoch_identity_for_test();
	assert(epoch != 0);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 200);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2200);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5200);
	for (size_t index = 0;
		index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]); ++index) {
		assert(fixture.resources.Count(kActivationEvents[index]) == 1);
	}
}

void TestMutatingProtocolFailureDrainsBeforeOneCleanupEpoch()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::start_core_protocol);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.lifecycle.latched_activation_result() ==
		MISTER_RESULT_PLATFORM);
	const NativeFailureDrainTiming drain =
		fixture.lifecycle.failure_drain_timing_for_test();
	assert(drain.established);
	assert(drain.drain_start_ms == 100);
	assert(drain.drain_deadline_ms == 2100);
	assert(fixture.lifecycle.state() == NativeLifecycleState::cleanup);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(timing.established);
	assert(timing.cleanup_start_ms == 100);
	assert(timing.non_fpga_deadline_ms == 2100);
	assert(timing.fpga_deadline_ms == 5100);
	const uint64_t epoch = fixture.lifecycle.cleanup_epoch_identity_for_test();
	assert(epoch != 0);
	const NativeResourceLedger first = fixture.lifecycle.ledger();
	assert((first.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0);
	assert(first.core_protocol_shutdown_complete);
	assert(fixture.resources.Count(Event::shutdown_core_protocol) == 1);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
	ActiveProtocolFailureReceipt retained = {};
	assert(fixture.broker.core_protocol_failure_receipt_for_test(&retained));
	assert(retained.primary_result == MISTER_RESULT_PLATFORM);

	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.lifecycle.failure_drain_timing_for_test().drain_deadline_ms ==
		2100);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5100);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CORE_PROTOCOL) != 0);
	assert(fixture.lifecycle.ledger().core_protocol_shutdown_complete);
	assert(fixture.resources.Count(Event::shutdown_core_protocol) == 1);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 2);
	assert(fixture.broker.core_protocol_failure_receipt_for_test(&retained));
	assert(retained.primary_result == MISTER_RESULT_PLATFORM);
}

void TestFailedProtocolDrainDeadlineNeverRefreshes()
{
	Fixture fixture(100);
	fixture.resources.Fail(Event::start_core_protocol);
	fixture.resources.HoldConcurrentProtocolPeer();
	fixture.clock.ForceTimeout(true);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_PLATFORM);
	assert(fixture.lifecycle.state() == NativeLifecycleState::quiescing);
	assert(fixture.lifecycle.failure_drain_timing_for_test().established);
	assert(fixture.lifecycle.failure_drain_timing_for_test().drain_start_ms == 100);
	assert(fixture.lifecycle.failure_drain_timing_for_test().drain_deadline_ms ==
		2100);
	assert(fixture.lifecycle.cleanup_timing().established);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5100);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CORE_PROTOCOL) != 0);
	assert(!fixture.lifecycle.ledger().core_protocol_shutdown_complete);
	std::unique_ptr<OperationLease> denied;
	assert(fixture.broker.Begin(fixture.lifecycle.generation(),
		OperationKind::input, 3000, &denied) == MISTER_RESULT_INVALID_STATE);

	fixture.clock.SetNow(2100);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.failure_drain_timing_for_test().drain_deadline_ms ==
		2100);
	assert(fixture.lifecycle.cleanup_timing().established);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == 0);

	fixture.resources.ReleaseConcurrentProtocolPeer();
	fixture.clock.ForceTimeout(false);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_DEADLINE);
	assert(fixture.lifecycle.failure_drain_timing_for_test().drain_deadline_ms ==
		2100);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5100);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() != 0);
}

void TestMalformedProtocolOutcomesCloseBeforeReleaseAndUseFrozenDrain()
{
	const MalformedProtocolOutcome malformed[] = {
		MalformedProtocolOutcome::success_without_completion,
		MalformedProtocolOutcome::failure_without_completion,
		MalformedProtocolOutcome::success_after_failure_completion
	};
	for (MalformedProtocolOutcome outcome : malformed) {
		Fixture fixture(100);
		fixture.resources.ReturnMalformedProtocolOutcome(outcome);
		if (outcome != MalformedProtocolOutcome::success_without_completion)
			fixture.resources.Fail(Event::start_core_protocol);
		fixture.resources.HoldConcurrentProtocolPeer();
		fixture.clock.ForceTimeout(true);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.lifecycle.latched_activation_result() ==
			MISTER_RESULT_PLATFORM);
		assert(fixture.lifecycle.state() == NativeLifecycleState::quiescing);
		const NativeFailureDrainTiming drain =
			fixture.lifecycle.failure_drain_timing_for_test();
		assert(drain.established);
		assert(drain.drain_start_ms == 100);
		assert(drain.drain_deadline_ms == 2100);
		assert(fixture.lifecycle.cleanup_timing().established);
		assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
		assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == 0);
		std::unique_ptr<OperationLease> denied;
		assert(fixture.broker.Begin(fixture.lifecycle.generation(),
			OperationKind::input, 3000, &denied) == MISTER_RESULT_INVALID_STATE);

		fixture.resources.ReleaseConcurrentProtocolPeer();
		fixture.clock.ForceTimeout(false);
		assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms == 100);
		assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms == 2100);
		assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms == 5100);
		assert(fixture.lifecycle.cleanup_epoch_identity_for_test() != 0);
	}
}

void TestCleanupDeadlineArithmeticSaturatesWithoutWrapping()
{
	Fixture fixture(UINT64_MAX - 1000);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, UINT64_MAX) ==
		MISTER_RESULT_OK);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(timing.cleanup_start_ms == UINT64_MAX - 1000);
	assert(timing.non_fpga_deadline_ms == UINT64_MAX);
	assert(timing.fpga_deadline_ms == UINT64_MAX);
}

void TestRetryRetainsEpochLedgersDeadlinesAndNeverReactivates()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const NativeDigitalNeutral first = {0, {0x02, 0}};
	const NativeDigitalNeutral second = {1, {0x03, 0}};
	fixture.resources.AddDigitalNeutral(first);
	fixture.resources.AddDigitalNeutral(second);

	fixture.resources.Fail(Event::reject_join_offload);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const uint64_t epoch = fixture.lifecycle.cleanup_epoch_identity_for_test();
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(fixture.resources.Count(Event::stop_scheduler) == 1);
	assert(fixture.resources.Count(Event::reject_join_offload) == 1);
	assert(!fixture.lifecycle.ledger().scheduler);
	assert(fixture.lifecycle.ledger().offload);

	fixture.resources.ClearFailure();
	fixture.clock.SetNow(200);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.lifecycle.cleanup_timing().cleanup_start_ms ==
		timing.cleanup_start_ms);
	assert(fixture.lifecycle.cleanup_timing().non_fpga_deadline_ms ==
		timing.non_fpga_deadline_ms);
	assert(fixture.lifecycle.cleanup_timing().fpga_deadline_ms ==
		timing.fpga_deadline_ms);
	assert(fixture.resources.Count(Event::stop_scheduler) == 1);
	assert(fixture.resources.Count(Event::reject_join_offload) == 2);
	assert(fixture.resources.neutral_.size() == 2);
	assert(fixture.resources.neutral_[0].player == 0);
	assert(fixture.resources.neutral_[0].words[0] == 0x02);
	assert(fixture.resources.neutral_[0].words[1] == 0);
	assert(fixture.resources.neutral_[1].player == 1);
	assert(fixture.resources.neutral_[1].words[0] == 0x03);
	assert(fixture.resources.neutral_[1].words[1] == 0);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
	assert(fixture.resources.LastHardwareDeadline(
		Event::replay_digital_neutral) == timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(
		Event::terminal_fpga_cleanup) == timing.fpga_deadline_ms);
	for (size_t index = 0;
		index < sizeof(kActivationEvents) / sizeof(kActivationEvents[0]); ++index) {
		assert(fixture.resources.Count(kActivationEvents[index]) == 1);
	}

	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 2);
	assert(fixture.resources.Count(Event::replay_digital_neutral) == 2);
	assert(fixture.resources.LastHardwareDeadline(
		Event::terminal_fpga_cleanup) == timing.fpga_deadline_ms);
}

void TestCoreProtocolReleaseRetryRetainsItsCleanupLease()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	fixture.resources.AbandonCoreProtocolReleaseOnce();
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(fixture.resources.Count(Event::shutdown_core_protocol) == 1);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CORE_PROTOCOL) != 0);
	assert(!fixture.lifecycle.ledger().core_protocol_shutdown_complete);

	fixture.clock.SetNow(200);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.resources.Count(Event::shutdown_core_protocol) == 2);
	assert(fixture.resources.LastHardwareDeadline(Event::shutdown_core_protocol) ==
		timing.fpga_deadline_ms);
	assert(fixture.lifecycle.ledger().core_protocol_shutdown_complete);
	assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
}

void TestTypedCheckoutDeadlineDropsNonresumableRegistration()
{
	const OperationKind kinds[] = {OperationKind::video, OperationKind::audio,
		OperationKind::audio_video, OperationKind::core_protocol};
	const Event events[] = {Event::stop_video, Event::stop_audio,
		Event::stop_audio_video, Event::shutdown_core_protocol};
	const uint64_t boundaries[] = {2099, 2100, 2101};
	for (size_t index = 0; index != sizeof(kinds) / sizeof(kinds[0]); ++index) {
		for (size_t boundary = 0; boundary !=
			sizeof(boundaries) / sizeof(boundaries[0]); ++boundary) {
			Fixture fixture(100);
			assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
				MISTER_RESULT_OK);
			fixture.resources.ExpireCleanupCheckout(kinds[index],
				boundaries[boundary]);
			const Result first = fixture.lifecycle.Stop(UINT64_MAX);
			if (boundaries[boundary] < 2100) {
				assert(first != MISTER_RESULT_DEADLINE);
				assert(fixture.resources.Count(events[index]) == 1);
				continue;
			}
			assert(first == MISTER_RESULT_DEADLINE);
			assert(fixture.resources.Count(events[index]) == 0);
			fixture.clock.SetNow(200);
			assert(fixture.lifecycle.Stop(UINT64_MAX) ==
				MISTER_RESULT_CLEANUP_INCOMPLETE);
			assert(fixture.resources.Count(events[index]) == 1);
			assert(fixture.lifecycle.Stop(UINT64_MAX) ==
				MISTER_RESULT_CLEANUP_INCOMPLETE);
		}
	}
}

void TestSaveReleaseRetryRetainsItsCleanupLeaseAndDeadline()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	fixture.resources.Fail(Event::flush_close_save);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(fixture.resources.Count(Event::flush_close_save) == 1);
	assert((fixture.lifecycle.ledger().resource_flags & MISTER_RESOURCE_SAVES) != 0);
	assert(fixture.resources.LastBoundedDeadline(Event::flush_close_save) ==
		timing.non_fpga_deadline_ms);

	fixture.resources.ClearFailure();
	fixture.clock.SetNow(200);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.resources.Count(Event::flush_close_save) == 2);
	assert(fixture.resources.save_cleanup_lease_reused_);
	assert(fixture.resources.LastBoundedDeadline(Event::flush_close_save) ==
		timing.non_fpga_deadline_ms);
	assert((fixture.lifecycle.ledger().resource_flags & MISTER_RESOURCE_SAVES) == 0);
}

void TestSaveCleanupChecksTheBrokerClockAtLedgerCommit()
{
	struct Case {
		uint64_t completion_time;
		bool save_clears;
	};
	const Case cases[] = {
		{2099, true},
		{2100, false},
		{2101, false}
	};
	for (const Case &test : cases) {
		Fixture fixture(100);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_OK);
		fixture.resources.AdvanceBrokerClockAfterSaveCleanup(test.completion_time);
		const Result result = fixture.lifecycle.Stop(UINT64_MAX);
		const bool save_owned = (fixture.lifecycle.ledger().resource_flags &
			MISTER_RESOURCE_SAVES) != 0;
		assert(save_owned == !test.save_clears);
		if (test.save_clears) assert(result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		else assert(result == MISTER_RESULT_DEADLINE);
	}
}

void TestNormalStopUsesTheNormativeOrder()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const NativeDigitalNeutral neutral = {0, {0x02, 0}};
	fixture.resources.AddDigitalNeutral(neutral);
	const size_t activation_events = fixture.resources.events.size();
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	const Event expected[] = {
		Event::stop_scheduler,
		Event::reject_join_offload,
		Event::flush_close_save,
		Event::capture_digital_neutral,
		Event::replay_digital_neutral,
		Event::close_input_descriptors,
		Event::stop_video,
		Event::stop_audio,
		Event::stop_audio_video,
		Event::close_content,
		Event::shutdown_core_protocol,
		Event::terminal_fpga_cleanup
	};
	assert(fixture.resources.events.size() == activation_events +
		sizeof(expected) / sizeof(expected[0]));
	for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index)
		assert(fixture.resources.events[activation_events + index] == expected[index]);
	assert(fixture.lifecycle.ledger().resource_flags ==
		(MISTER_RESOURCE_FPGA | MISTER_RESOURCE_BRIDGES |
		 MISTER_RESOURCE_CORE_PROTOCOL | MISTER_RESOURCE_CORE_INPUT |
		 MISTER_RESOURCE_NATIVE_AUDIO));
	assert(fixture.lifecycle.ledger().audio_shutdown_complete);
	assert(fixture.lifecycle.ledger().core_protocol_shutdown_complete);
	assert(!fixture.lifecycle.ledger().scheduler);
	assert(!fixture.lifecycle.ledger().offload);
	assert(!fixture.lifecycle.ledger().input_descriptors);
	const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
	assert(fixture.resources.LastHardwareDeadline(Event::replay_digital_neutral) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastBoundedDeadline(Event::capture_digital_neutral) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastBoundedDeadline(Event::stop_scheduler) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastBoundedDeadline(Event::reject_join_offload) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::stop_video) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::stop_audio) ==
		timing.non_fpga_deadline_ms);
	assert(fixture.resources.LastHardwareDeadline(Event::shutdown_core_protocol) ==
		timing.fpga_deadline_ms);
	assert(fixture.resources.shutdown_protocol_profile_ == &fixture.profile);
	assert(fixture.resources.LastHardwareDeadline(Event::terminal_fpga_cleanup) ==
		timing.fpga_deadline_ms);
}

void TestCleanupFailureRetainsTheFailedResourceLedger()
{
	const Event failures[] = {
		Event::stop_scheduler,
		Event::reject_join_offload,
		Event::flush_close_save,
		Event::replay_digital_neutral,
		Event::close_input_descriptors,
		Event::stop_video,
		Event::stop_audio,
		Event::close_content,
		Event::shutdown_core_protocol,
		Event::terminal_fpga_cleanup
	};
	for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
		Fixture fixture(100);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_OK);
		const NativeDigitalNeutral neutral = {0, {0x02, 0}};
		fixture.resources.AddDigitalNeutral(neutral);
		fixture.resources.Fail(failures[index]);
		assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(!fixture.resources.events.empty());
		assert(fixture.resources.events.back() == failures[index]);
		const NativeResourceLedger ledger = fixture.lifecycle.ledger();
		switch (failures[index]) {
		case Event::stop_scheduler:
			assert(ledger.scheduler);
			break;
		case Event::reject_join_offload:
			assert(ledger.offload);
			break;
		case Event::flush_close_save:
			assert((ledger.resource_flags & MISTER_RESOURCE_SAVES) != 0);
			break;
		case Event::replay_digital_neutral:
			assert(ledger.digital_neutral_valid[0]);
			assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 0);
			break;
		case Event::close_input_descriptors:
			assert(ledger.input_descriptors);
			assert((ledger.resource_flags & MISTER_RESOURCE_CORE_INPUT) != 0);
			break;
		case Event::stop_video:
			assert((ledger.resource_flags & MISTER_RESOURCE_NATIVE_VIDEO) != 0);
			break;
		case Event::stop_audio:
			assert((ledger.resource_flags & MISTER_RESOURCE_NATIVE_AUDIO) != 0);
			break;
		case Event::close_content:
			assert((ledger.resource_flags & MISTER_RESOURCE_CONTENT) != 0);
			break;
		case Event::shutdown_core_protocol:
			assert((ledger.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0);
			break;
		case Event::terminal_fpga_cleanup:
			assert((ledger.resource_flags & MISTER_RESOURCE_FPGA) != 0);
			assert((ledger.resource_flags & MISTER_RESOURCE_BRIDGES) != 0);
			assert((ledger.resource_flags & MISTER_RESOURCE_CORE_PROTOCOL) != 0);
			assert(ledger.core_protocol_shutdown_complete);
			break;
		default:
			assert(false);
		}
		if (failures[index] == Event::replay_digital_neutral) {
			const uint64_t epoch =
				fixture.lifecycle.cleanup_epoch_identity_for_test();
			const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
			fixture.resources.ClearFailure();
			fixture.clock.SetNow(200);
			const Result retry = fixture.lifecycle.Stop(UINT64_MAX);
			assert(retry == MISTER_RESULT_CLEANUP_INCOMPLETE);
			assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
			assert(fixture.resources.Count(Event::replay_digital_neutral) == 2);
			assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 1);
			assert(!fixture.lifecycle.ledger().digital_neutral_valid[0]);
			assert(fixture.resources.LastHardwareDeadline(
				Event::replay_digital_neutral) == timing.non_fpga_deadline_ms);
		}
	}
	{
		Fixture fixture(100);
		assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
			MISTER_RESULT_OK);
		const NativeDigitalNeutral neutral = {0, {0x02, 0}};
		fixture.resources.AddDigitalNeutral(neutral);
		fixture.resources.Fail(Event::replay_digital_neutral);
		assert(fixture.lifecycle.Stop(UINT64_MAX) ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		const uint64_t epoch = fixture.lifecycle.cleanup_epoch_identity_for_test();
		const NativeCleanupTiming timing = fixture.lifecycle.cleanup_timing();
		fixture.resources.ClearFailure();
		fixture.clock.SetNow(timing.non_fpga_deadline_ms);
		assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_DEADLINE);
		assert(fixture.lifecycle.cleanup_epoch_identity_for_test() == epoch);
		assert(fixture.lifecycle.ledger().digital_neutral_valid[0]);
		assert(fixture.resources.Count(Event::terminal_fpga_cleanup) == 0);
	}
}

void TestCapturedNeutralMustMatchTheActiveProfile()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	const NativeDigitalNeutral forged = {0, {0x03, 0}};
	fixture.resources.AddDigitalNeutral(forged);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(fixture.resources.Count(Event::capture_digital_neutral) == 1);
	assert(fixture.resources.Count(Event::replay_digital_neutral) == 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_CORE_INPUT) != 0);
}

void TestCleanupSuccessAtDeadlineDoesNotClearTheLedger()
{
	Fixture fixture(100);
	assert(fixture.lifecycle.ActivateFixtureForTest(fixture.profile, 1000) ==
		MISTER_RESULT_OK);
	fixture.resources.Delay(Event::stop_video, 2000);
	assert(fixture.lifecycle.Stop(UINT64_MAX) == MISTER_RESULT_DEADLINE);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_NATIVE_VIDEO) != 0);
	assert((fixture.lifecycle.ledger().resource_flags &
		MISTER_RESOURCE_NATIVE_AUDIO) != 0);
	assert(fixture.resources.Count(Event::stop_video) == 1);
	assert(fixture.resources.Count(Event::stop_audio) == 0);
}

void TestDestructorClosesOnlyOwnedProcessResources()
{
	FakeClock clock(100);
	HardwareBroker broker(clock);
	FakeResources resources(clock, broker);
	NativeResourceSet set = resources.Set();
	{
		NativeLifecycle lifecycle(clock, broker, set);
		const NativeCoreProfile &profile = *FixtureNativeCoreProfile("snes");
		assert(lifecycle.ActivateFixtureForTest(profile, 1000) == MISTER_RESULT_OK);
	}
	assert(resources.Count(Event::destruct_scheduler) == 1);
	assert(resources.Count(Event::destruct_offload) == 1);
	assert(resources.Count(Event::destruct_input_descriptors) == 1);
	assert(resources.Count(Event::destruct_save) == 1);
	assert(resources.Count(Event::destruct_content) == 1);
	assert(resources.Count(Event::destruct_video) == 1);
	assert(resources.Count(Event::destruct_audio) == 1);
	assert(resources.Count(Event::destruct_core_protocol) == 1);
	assert(resources.Count(Event::destruct_fpga_mappings) == 1);
	assert(resources.Count(Event::replay_digital_neutral) == 0);
	assert(resources.Count(Event::terminal_fpga_cleanup) == 0);
	assert(resources.Count(Event::shutdown_core_protocol) == 0);
}

} // namespace
} // namespace native
} // namespace mister

int main()
{
	mister::native::TestPreflightFailureNeverAcquiresOwnership();
	mister::native::TestContentIsResolvedBeforeOwnershipAndRetainedOnFirstHardwareFailure();
	mister::native::TestFirstAcquisitionIsLedgeredBeforeFollowingFailure();
	mister::native::TestSuccessfulInitializationRecordsEveryAcquisition();
	mister::native::TestActivationUsesAcceptedContainmentFirstOrder();
	mister::native::TestStopCallbackDeadlineBoundsFirstCleanupOperation();
	mister::native::TestStopQuiesceExpiryFreezesDrainAndCleanupTiming();
	mister::native::TestRealAdapterRootPrefixFailuresReleaseSavesAndLeaveTheGeneration();
	mister::native::TestRealAdapterZeroDescriptorFailuresClearStateForReuse();
	mister::native::TestRealAdapterZeroDescriptorFailureDestructorPerformsNoIo();
	mister::native::TestRealAdapterPostCreateAuthorityLossRetainsSavesUntilStableCleanup();
	mister::native::TestCoreProtocolReceivesAdmittedProfileAndRetainedContent();
	mister::native::TestRetainedContentDescriptionAndReadStayBounded();
	mister::native::TestFailureAfterEveryAcquisitionUnwindsWithoutChangingResult();
	mister::native::TestContentFailureOrOverrunNeverMintsOwnership();
	mister::native::TestPreownershipContentCloseFailureBlocksReactivationAndRetriesLocally();
	mister::native::TestOverrunAfterEverySuccessfulAcquisitionIsLedgeredAndUnwound();
	mister::native::TestActivationOverrunStaysFailedAndStartsFreshCleanupClocksOnce();
	mister::native::TestMutatingProtocolFailureDrainsBeforeOneCleanupEpoch();
	mister::native::TestFailedProtocolDrainDeadlineNeverRefreshes();
	mister::native::TestMalformedProtocolOutcomesCloseBeforeReleaseAndUseFrozenDrain();
	mister::native::TestCleanupDeadlineArithmeticSaturatesWithoutWrapping();
	mister::native::TestRetryRetainsEpochLedgersDeadlinesAndNeverReactivates();
	mister::native::TestCoreProtocolReleaseRetryRetainsItsCleanupLease();
	mister::native::TestTypedCheckoutDeadlineDropsNonresumableRegistration();
	mister::native::TestSaveReleaseRetryRetainsItsCleanupLeaseAndDeadline();
	mister::native::TestSaveCleanupChecksTheBrokerClockAtLedgerCommit();
	mister::native::TestNormalStopUsesTheNormativeOrder();
	mister::native::TestCleanupFailureRetainsTheFailedResourceLedger();
	mister::native::TestCleanupSuccessAtDeadlineDoesNotClearTheLedger();
	mister::native::TestCapturedNeutralMustMatchTheActiveProfile();
	mister::native::TestDestructorClosesOnlyOwnedProcessResources();
	return 0;
}
