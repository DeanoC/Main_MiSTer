// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MISTER_NATIVE_SAVE_REACHABILITY_PROBE)

#include "runtime/native/linux/native_save_adapter.hpp"

using LeakedFixtureFilesystem =
	mister::native::linux_native::NativeSaveFileSystem;

int main()
{
	return sizeof(LeakedFixtureFilesystem *) == 0;
}

#else

#include "runtime/native/native_core_profile.hpp"
#include "runtime/native/native_save_key.hpp"
#include "runtime/native/linux/native_save_adapter.hpp"
#include "runtime/native/hardware_broker.hpp"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace mister::native;
using namespace mister::native::linux_native;

namespace {

const char kPrivacyContentDigest[] =
	"c0dec0dec0dec0de" "c0dec0dec0dec0de"
	"c0dec0dec0dec0de" "c0dec0dec0dec0de";
const ino_t kPrivacyDescriptorIdentity = 456192;
const uint16_t kPrivacyBusNumber = 61951;

class FakeClock final : public NativeClock {
public:
	explicit FakeClock(uint64_t now_ms) : now_ms_(now_ms) {}
	uint64_t NowMs() const override { return now_ms_; }
	bool WaitUntil(std::condition_variable &, std::unique_lock<std::mutex> &,
		uint64_t deadline) override { return now_ms_ < deadline; }
private:
	uint64_t now_ms_;
};

struct SaveNode {
	int descriptor;
	struct stat identity;
	uint64_t mount_id;
};

class FakeSaveFileSystem final : public NativeSaveFileSystem {
public:
	enum class FailurePoint : uint8_t { open, stat, statat, mount };
	enum class Operation : uint8_t { open, stat, statat, mount, fdatasync, fsync, close };
	enum class NodeKind : uint8_t { root, parent, save_root, system, file };
	enum class Corruption : uint8_t {
		uid, gid, mode, writable, type, device, mount, links, overlarge,
		misaligned, entry_identity
	};
	FakeSaveFileSystem() : now_ms_(10), create_open_(false), file_exists_(false),
		mount_id_available_(true), fail_fsync_descriptor_(-1),
		fail_fsync_call_(0),
		fail_close_descriptor_(-1), failure_point_(FailurePoint::open),
		failure_call_(0), persistent_failure_(false), open_calls_(0), stat_calls_(0), statat_calls_(0),
		mount_calls_(0), advance_after_open_call_(0), advance_to_ms_(0),
		advance_operation_(Operation::open), advance_operation_call_(0),
		advance_before_operation_(false), advance_after_operation_(false),
		advance_operation_to_ms_(0), fdatasync_calls_(0), fsync_calls_(0),
		close_calls_(0),
		corrupt_system_entry_(false), corrupt_file_entry_(false),
		create_eexist_(false), fail_fdatasync_once_(false),
		advance_after_fdatasync_(false), advance_after_fdatasync_to_(0),
		last_file_name_(),
		trace_(), root_(Node(10, 1, S_IFDIR | 0755, 0, 0)),
		parent_(Node(11, 2, S_IFDIR | 0755, 0, 0)),
		save_root_(Node(12, 3, S_IFDIR | 0700, 1000, 1000)),
		system_(Node(13, 4, S_IFDIR | 0700, 1000, 1000)),
		file_(Node(14, kPrivacyDescriptorIdentity, S_IFREG | 0600, 1000, 1000)) {}

	uint64_t NowMs() const override { return now_ms_; }
	NativeSaveOpenResult OpenAt(int parent, const char *name, int flags,
		mode_t mode) override
	{
		assert(name != nullptr);
		++open_calls_;
		AdvanceBefore(Operation::open, open_calls_);
		if (FailureAt(FailurePoint::open, open_calls_))
			return {-1, EIO};
		if (advance_after_open_call_ == open_calls_) now_ms_ = advance_to_ms_;
		assert((flags & O_NOFOLLOW) != 0);
		if (parent == AT_FDCWD && strcmp(name, "/") == 0)
			return OpenResult(Operation::open, {root_.descriptor, 0});
		if (parent == root_.descriptor && strcmp(name, "fogcast-fixture") == 0)
			return OpenResult(Operation::open, {parent_.descriptor, 0});
		if (parent == parent_.descriptor && strcmp(name, "saves") == 0)
			return OpenResult(Operation::open, {save_root_.descriptor, 0});
		if (parent == save_root_.descriptor && strcmp(name, "snes") == 0)
			return OpenResult(Operation::open, {system_.descriptor, 0});
		if (parent == system_.descriptor && strstr(name, ".sav") != nullptr) {
			last_file_name_ = name;
			if ((flags & O_CREAT) == 0) return OpenResult(Operation::open, file_exists_ ?
				NativeSaveOpenResult{file_.descriptor, 0} : NativeSaveOpenResult{-1, ENOENT});
			assert((flags & O_EXCL) != 0);
			assert(mode == 0600);
			if (create_eexist_) return OpenResult(Operation::open, {-1, EEXIST});
			assert(!file_exists_);
			file_exists_ = true;
			create_open_ = true;
			return OpenResult(Operation::open, {file_.descriptor, 0});
		}
		return OpenResult(Operation::open, {-1, EINVAL});
	}
	int Stat(int descriptor, struct stat *info) override
	{
		++stat_calls_;
		AdvanceBefore(Operation::stat, stat_calls_);
		if (FailureAt(FailurePoint::stat, stat_calls_))
			return -1;
		const int result = Copy(NodeFor(descriptor), info);
		AdvanceAfter(Operation::stat, stat_calls_);
		return result;
	}
	int StatAt(int parent, const char *name, struct stat *info, int flags) override
	{
		assert((flags & AT_SYMLINK_NOFOLLOW) != 0);
		++statat_calls_;
		AdvanceBefore(Operation::statat, statat_calls_);
		if (FailureAt(FailurePoint::statat, statat_calls_)) return -1;
		int result = -1;
		if (parent == root_.descriptor && strcmp(name, "fogcast-fixture") == 0)
			result = Copy(&parent_, info);
		else if (parent == parent_.descriptor && strcmp(name, "saves") == 0)
			result = Copy(&save_root_, info);
		if (parent == save_root_.descriptor && strcmp(name, "snes") == 0) {
			result = Copy(&system_, info);
			if (result == 0 && corrupt_system_entry_) ++info->st_ino;
		}
		else if (parent == system_.descriptor && strstr(name, ".sav") != nullptr &&
			file_exists_) {
			result = Copy(&file_, info);
			if (result == 0 && corrupt_file_entry_) ++info->st_ino;
		}
		AdvanceAfter(Operation::statat, statat_calls_);
		return result;
	}
	Result MountId(int descriptor, uint64_t *mount_id) override
	{
		++mount_calls_;
		AdvanceBefore(Operation::mount, mount_calls_);
		if (FailureAt(FailurePoint::mount, mount_calls_))
			return MISTER_RESULT_PLATFORM;
		const SaveNode *const node = NodeFor(descriptor);
		if (!mount_id_available_ || node == nullptr ||
			mount_id == nullptr) return !mount_id_available_ ?
			MISTER_RESULT_UNSUPPORTED : MISTER_RESULT_PLATFORM;
		*mount_id = node->mount_id;
		AdvanceAfter(Operation::mount, mount_calls_);
		return MISTER_RESULT_OK;
	}
	int Fdatasync(int descriptor) override
	{
		assert(descriptor == file_.descriptor);
		++fdatasync_calls_;
		AdvanceBefore(Operation::fdatasync, fdatasync_calls_);
		trace_.push_back('d');
		if (fail_fdatasync_once_) {
			fail_fdatasync_once_ = false;
			return -1;
		}
		if (advance_after_fdatasync_) now_ms_ = advance_after_fdatasync_to_;
		AdvanceAfter(Operation::fdatasync, fdatasync_calls_);
		return 0;
	}
	int Fsync(int descriptor) override
	{
		++fsync_calls_;
		AdvanceBefore(Operation::fsync, fsync_calls_);
		trace_.push_back(descriptor == file_.descriptor ? 'f' : 's');
		if (descriptor == fail_fsync_descriptor_ ||
			fsync_calls_ == fail_fsync_call_) {
			fail_fsync_descriptor_ = -1;
			fail_fsync_call_ = 0;
			return -1;
		}
		const int result = descriptor == file_.descriptor ||
			descriptor == system_.descriptor ? 0 : -1;
		AdvanceAfter(Operation::fsync, fsync_calls_);
		return result;
	}
	void FailFsyncOnce(int descriptor) { fail_fsync_descriptor_ = descriptor; }
	void FailFsyncAtCallOnce(int call) { fail_fsync_call_ = call; }
	void FailFdatasyncOnce() { fail_fdatasync_once_ = true; }
	void AdvanceAfterFdatasync(uint64_t now_ms)
	{
		advance_after_fdatasync_ = true;
		advance_after_fdatasync_to_ = now_ms;
	}
	int Close(int descriptor) override
	{
		++close_calls_;
		AdvanceBefore(Operation::close, close_calls_);
		trace_.push_back(static_cast<char>('0' + descriptor - 10));
		if (descriptor == fail_close_descriptor_) return -1;
		const int result = NodeFor(descriptor) == nullptr ? -1 : 0;
		AdvanceAfter(Operation::close, close_calls_);
		return result;
	}
	void FailClose(int descriptor) { fail_close_descriptor_ = descriptor; }
	void SetFileSize(off_t size) { file_.identity.st_size = size; }
	void SetMountIdAvailable(bool available) { mount_id_available_ = available; }
	void SetSystemDevice(dev_t device) { system_.identity.st_dev = device; }
	void SetSystemMount(uint64_t mount_id) { system_.mount_id = mount_id; }
	void FailAt(FailurePoint point, int call)
	{
		failure_point_ = point;
		failure_call_ = call;
		persistent_failure_ = false;
	}
	void FailPersistentlyAt(FailurePoint point, int call)
	{
		failure_point_ = point;
		failure_call_ = call;
		persistent_failure_ = true;
	}
	void AdvanceAfterOpen(int call, uint64_t now_ms)
	{
		advance_after_open_call_ = call;
		advance_to_ms_ = now_ms;
	}
	void AdvanceBeforeOperation(Operation operation, int call, uint64_t now_ms)
	{
		advance_operation_ = operation;
		advance_operation_call_ = call;
		advance_before_operation_ = true;
		advance_after_operation_ = false;
		advance_operation_to_ms_ = now_ms;
	}
	void AdvanceAfterOperation(Operation operation, int call, uint64_t now_ms)
	{
		advance_operation_ = operation;
		advance_operation_call_ = call;
		advance_before_operation_ = false;
		advance_after_operation_ = true;
		advance_operation_to_ms_ = now_ms;
	}
	void CorruptSystemDirectoryEntry() { corrupt_system_entry_ = true; }
	void RestoreSystemDirectoryEntry() { corrupt_system_entry_ = false; }
	void SetExistingFile(bool existing) { file_exists_ = existing; }
	void FailCreateWithEexist() { create_eexist_ = true; }
	bool create_open() const { return create_open_; }
	void Corrupt(NodeKind node_kind, Corruption corruption)
	{
		SaveNode *const node = NodeByKind(node_kind);
		assert(node != nullptr);
		switch (corruption) {
		case Corruption::uid: ++node->identity.st_uid; break;
		case Corruption::gid: ++node->identity.st_gid; break;
		case Corruption::mode:
			node->identity.st_mode = (node->identity.st_mode & S_IFMT) | 0750;
			break;
		case Corruption::writable: node->identity.st_mode |= 0020; break;
		case Corruption::type:
			node->identity.st_mode = (node->identity.st_mode & 07777) | S_IFLNK;
			break;
		case Corruption::device: ++node->identity.st_dev; break;
		case Corruption::mount: ++node->mount_id; break;
		case Corruption::links: ++node->identity.st_nlink; break;
		case Corruption::overlarge: node->identity.st_size = 512 * 1024 + 512; break;
		case Corruption::misaligned: node->identity.st_size = 1; break;
		case Corruption::entry_identity:
			if (node_kind == NodeKind::system) corrupt_system_entry_ = true;
			if (node_kind == NodeKind::file) corrupt_file_entry_ = true;
			break;
		}
	}
	const char *last_file_name() const { return last_file_name_.c_str(); }
	const std::vector<char> &trace() const { return trace_; }

private:
	bool FailureAt(FailurePoint point, int call) const
	{
		return failure_point_ == point && (failure_call_ == call ||
			(persistent_failure_ && call >= failure_call_));
	}
	NativeSaveOpenResult OpenResult(Operation operation,
		NativeSaveOpenResult result)
	{
		AdvanceAfter(operation, open_calls_);
		return result;
	}
	void AdvanceBefore(Operation operation, int call)
	{
		if (advance_before_operation_ && advance_operation_ == operation &&
			advance_operation_call_ == call) now_ms_ = advance_operation_to_ms_;
	}
	void AdvanceAfter(Operation operation, int call)
	{
		if (advance_after_operation_ && advance_operation_ == operation &&
			advance_operation_call_ == call) now_ms_ = advance_operation_to_ms_;
	}
	static SaveNode Node(int descriptor, ino_t inode, mode_t mode, uid_t uid,
		gid_t gid)
	{
		SaveNode node = {};
		node.descriptor = descriptor;
		// Fixture identities are real adapter inputs; the file inode is the
		// wrapper-scanned descriptor-identity sentinel on every successful save.
		node.identity.st_dev = static_cast<dev_t>(0x6f6f);
		node.identity.st_ino = inode;
		node.identity.st_mode = mode;
		node.identity.st_nlink = 1;
		node.identity.st_uid = uid;
		node.identity.st_gid = gid;
		node.identity.st_size = 0;
		node.mount_id = 77;
		return node;
	}
	SaveNode *NodeFor(int descriptor)
	{
		SaveNode *nodes[] = {&root_, &parent_, &save_root_, &system_, &file_};
		for (size_t index = 0; index < sizeof(nodes) / sizeof(nodes[0]); ++index)
			if (nodes[index]->descriptor == descriptor) return nodes[index];
		return nullptr;
	}
	SaveNode *NodeByKind(NodeKind node_kind)
	{
		switch (node_kind) {
		case NodeKind::root: return &root_;
		case NodeKind::parent: return &parent_;
		case NodeKind::save_root: return &save_root_;
		case NodeKind::system: return &system_;
		case NodeKind::file: return &file_;
		}
		return nullptr;
	}
	static int Copy(const SaveNode *node, struct stat *info)
	{
		if (node == nullptr || info == nullptr) return -1;
		*info = node->identity;
		return 0;
	}

	uint64_t now_ms_;
	bool create_open_;
	bool file_exists_;
	bool mount_id_available_;
	int fail_fsync_descriptor_;
	int fail_fsync_call_;
	int fail_close_descriptor_;
	FailurePoint failure_point_;
	int failure_call_;
	bool persistent_failure_;
	int open_calls_;
	int stat_calls_;
	int statat_calls_;
	int mount_calls_;
	int advance_after_open_call_;
	uint64_t advance_to_ms_;
	Operation advance_operation_;
	int advance_operation_call_;
	bool advance_before_operation_;
	bool advance_after_operation_;
	uint64_t advance_operation_to_ms_;
	int fdatasync_calls_;
	int fsync_calls_;
	int close_calls_;
	bool corrupt_system_entry_;
	bool corrupt_file_entry_;
	bool create_eexist_;
	bool fail_fdatasync_once_;
	bool advance_after_fdatasync_;
	uint64_t advance_after_fdatasync_to_;
	std::string last_file_name_;
	std::vector<char> trace_;
	SaveNode root_;
	SaveNode parent_;
	SaveNode save_root_;
	SaveNode system_;
	SaveNode file_;
};

void TestSaveKeyUsesClosedSystemAndLowercaseContentDigest()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	const char *const content_sha256 =
		kPrivacyContentDigest;
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile, content_sha256, &key));
	char filename[69] = {};
	assert(NativeSaveKeyFileName(key, filename, sizeof(filename)));
	assert(strcmp(filename,
		"c0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0de.sav") == 0);
	assert(key.system_id == NativeSystem::snes);
	assert(!MakeNativeSaveKey(*profile,
		"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
		&key));
	assert(!MakeNativeSaveKey(*profile, "0123", &key));
}

void TestFixtureSaveAuthorityUsesExactClosedDirectoryChain()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	assert(profile->save.mode == NativeSaveMode::single_slot_growable_block_file);
	assert(profile->save.slot == 0);
	assert(profile->save.sector_bytes == 512);
	assert(profile->save.root_authority != nullptr);
	const NativeSaveRootAuthority &root = *profile->save.root_authority;
	assert(root.chain_count == 4);
	assert(strcmp(root.absolute_root, "/fogcast-fixture/saves/snes") == 0);
	assert(strcmp(root.chain[0].component, "/") == 0);
	assert(root.chain[0].role == NativeSaveDirectoryRole::root_anchor);
	assert(root.chain[3].role == NativeSaveDirectoryRole::system_directory);
	assert(strcmp(root.chain[3].component, "snes") == 0);
	assert(root.file_mode == 0600);
}

void TestNewSaveFsyncsFileThenSystemDirectoryBeforeAcquisition()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter adapter(broker, filesystem);
	const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
	assert(opened.result == MISTER_RESULT_OK);
	assert(opened.acquired);
	assert(strcmp(filesystem.last_file_name(),
		"c0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0dec0de.sav") == 0);
	assert(filesystem.trace().size() == 2);
	assert(filesystem.trace()[0] == 'f');
	assert(filesystem.trace()[1] == 's');
	const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
	assert(closed.result == MISTER_RESULT_OK);
	assert(closed.data_synchronized);
	assert(closed.metadata_synchronized);
	assert(closed.descriptors_absent);
	assert(!closed.closure_unknown);
	assert(filesystem.trace().size() == 8);
	assert(filesystem.trace()[2] == 'd');
	assert(filesystem.trace()[3] == '4');
	assert(filesystem.trace()[4] == '3');
	assert(filesystem.trace()[5] == '2');
	assert(filesystem.trace()[6] == '1');
	assert(filesystem.trace()[7] == '0');
}

void TestUnavailableMountIdentityFailsBeforeWritableFileAccess()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	filesystem.SetMountIdAvailable(false);
	NativeSaveAdapter adapter(broker, filesystem);
	const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
	assert(opened.result == MISTER_RESULT_UNSUPPORTED);
	assert(opened.acquired);
	assert(filesystem.last_file_name()[0] == '\0');
}

void TestPartialRootPrefixIsClosedAfterMountIdentityFailure()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	filesystem.SetMountIdAvailable(false);
	NativeSaveAdapter adapter(broker, filesystem);
	const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
	assert(opened.result == MISTER_RESULT_UNSUPPORTED && opened.acquired);
	const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
	assert(closed.result == MISTER_RESULT_OK);
	assert(!closed.data_synchronization_required);
	assert(!closed.metadata_synchronization_required);
	assert(closed.descriptors_absent);
	assert(!closed.closure_unknown);
	assert(filesystem.trace().size() == 1);
	assert(filesystem.trace()[0] == '0');
}

void TestEveryRetainedSaveResolutionFailureClosesItsDescriptors()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	auto inject_and_close = [&](FakeSaveFileSystem::FailurePoint point, int call,
		bool expect_acquired, bool expect_positive_cleanup) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.FailAt(point, call);
		NativeSaveAdapter adapter(broker, filesystem);
		const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
		assert(opened.result != MISTER_RESULT_OK);
		assert(opened.acquired == expect_acquired);
		if (opened.acquired) {
			const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
			assert(closed.descriptors_absent == expect_positive_cleanup);
			assert(!closed.closure_unknown);
			if (!expect_positive_cleanup) {
				assert(closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
				assert(filesystem.trace().size() == 2);
				assert(filesystem.trace()[0] == 'f');
				assert(filesystem.trace()[1] == 's');
			}
		}
	};
	// Exercise every possible resolver observation.  Each operation after the
	// first root open must retain and then positively close its exact prefix.
	for (int call = 1; call != 7; ++call)
		inject_and_close(FakeSaveFileSystem::FailurePoint::open, call, call != 1,
			true);
	for (int call = 1; call != 11; ++call)
		inject_and_close(FakeSaveFileSystem::FailurePoint::stat, call, true,
			call != 5);
	for (int call = 1; call != 6; ++call)
		inject_and_close(FakeSaveFileSystem::FailurePoint::statat, call, true,
			true);
	for (int call = 1; call != 9; ++call)
		inject_and_close(FakeSaveFileSystem::FailurePoint::mount, call, true,
			true);
}

void TestExpiredRetainedRootPrefixNeverRefreshesItsOriginalDeadline()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	filesystem.AdvanceAfterOpen(1, 1000);
	NativeSaveAdapter adapter(broker, filesystem);
	assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_DEADLINE);
	const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
	assert(first.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(!first.descriptors_absent && !first.closure_unknown);
	assert(filesystem.trace().empty());
	const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
	assert(second.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(filesystem.trace().empty());
}

void TestPostCreateIdentityFailureRetainsTheDescriptorChainUntilStable()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	filesystem.CorruptSystemDirectoryEntry();
	NativeSaveAdapter adapter(broker, filesystem);
	const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
	assert(opened.result == MISTER_RESULT_PLATFORM && opened.acquired);
	const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
	assert(first.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(!first.data_synchronized && !first.metadata_synchronized);
	assert(!first.descriptors_absent && !first.closure_unknown);
	assert(filesystem.trace().size() == 2);
	assert(filesystem.trace()[0] == 'f');
	assert(filesystem.trace()[1] == 's');
	filesystem.RestoreSystemDirectoryEntry();
	const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
	assert(second.result == MISTER_RESULT_OK && second.data_synchronized &&
		second.metadata_synchronized && second.descriptors_absent &&
		!second.closure_unknown);
	const char expected[] = {'f', 's', 'd', '4', '3', '2', '1', '0'};
	assert(filesystem.trace().size() == sizeof(expected));
	for (size_t index = 0; index < sizeof(expected); ++index)
		assert(filesystem.trace()[index] == expected[index]);
}

void TestEveryPersistentPostCreateAuthorityFailureRetainsAfterCreationBarriers()
{
	struct FileCase {
		FakeSaveFileSystem::NodeKind node;
		FakeSaveFileSystem::Corruption corruption;
	};
	const FileCase cases[] = {
		{FakeSaveFileSystem::NodeKind::system,
			FakeSaveFileSystem::Corruption::entry_identity},
		{FakeSaveFileSystem::NodeKind::file, FakeSaveFileSystem::Corruption::uid},
		{FakeSaveFileSystem::NodeKind::file, FakeSaveFileSystem::Corruption::gid},
		{FakeSaveFileSystem::NodeKind::file, FakeSaveFileSystem::Corruption::mode},
		{FakeSaveFileSystem::NodeKind::file, FakeSaveFileSystem::Corruption::type},
		{FakeSaveFileSystem::NodeKind::file, FakeSaveFileSystem::Corruption::links},
		{FakeSaveFileSystem::NodeKind::file,
			FakeSaveFileSystem::Corruption::overlarge},
		{FakeSaveFileSystem::NodeKind::file,
			FakeSaveFileSystem::Corruption::misaligned},
		{FakeSaveFileSystem::NodeKind::file,
			FakeSaveFileSystem::Corruption::entry_identity}
	};
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (const FileCase &test : cases) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.Corrupt(test.node, test.corruption);
		NativeSaveAdapter adapter(broker, filesystem);
		const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
		assert(opened.result == MISTER_RESULT_PLATFORM && opened.acquired);
		const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(!closed.data_synchronized && !closed.metadata_synchronized &&
			!closed.descriptors_absent && !closed.closure_unknown);
		assert(filesystem.trace().size() == 2);
		assert(filesystem.trace()[0] == 'f');
		assert(filesystem.trace()[1] == 's');
	}
}

void TestPersistentPostCreateBarrierRetriesDoNotRepeatCompletedFacts()
{
	const int failing_descriptors[] = {14, 13};
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (int failing_descriptor : failing_descriptors) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.Corrupt(FakeSaveFileSystem::NodeKind::system,
			FakeSaveFileSystem::Corruption::entry_identity);
		filesystem.FailFsyncOnce(failing_descriptor);
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result ==
			MISTER_RESULT_PLATFORM);
		const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
		assert(first.result == MISTER_RESULT_PLATFORM && !first.descriptors_absent);
		const size_t retry_start = filesystem.trace().size();
		const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
		assert(second.result == MISTER_RESULT_CLEANUP_INCOMPLETE &&
			!second.descriptors_absent && !second.closure_unknown);
		if (failing_descriptor == 14) {
			assert(filesystem.trace()[0] == 'f');
			assert(filesystem.trace()[retry_start] == 'f');
			assert(filesystem.trace()[retry_start + 1] == 's');
		} else {
			assert(filesystem.trace()[0] == 'f');
			assert(filesystem.trace()[1] == 's');
			assert(filesystem.trace()[retry_start] == 's');
		}
	}
}

void TestEveryPersistentPostCreateRevalidationFailureRetainsAfterBarriers()
{
	struct RevalidationCase {
		FakeSaveFileSystem::FailurePoint point;
		int first_call;
		int last_call;
	};
	const RevalidationCase cases[] = {
		{FakeSaveFileSystem::FailurePoint::stat, 5, 10},
		{FakeSaveFileSystem::FailurePoint::statat, 1, 5},
		{FakeSaveFileSystem::FailurePoint::mount, 5, 8}
	};
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile, kPrivacyContentDigest, &key));
	for (const RevalidationCase &test : cases) {
		for (int call = test.first_call; call <= test.last_call; ++call) {
			FakeClock clock(10);
			HardwareBroker broker(clock);
			PlatformGenerationId generation = 0;
			assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> lease;
			assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
				MISTER_RESULT_OK);
			FakeSaveFileSystem filesystem;
			filesystem.FailPersistentlyAt(test.point, call);
			NativeSaveAdapter adapter(broker, filesystem);
			const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
			assert(opened.result == MISTER_RESULT_PLATFORM && opened.acquired);
			const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
			assert(closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE &&
				!closed.data_synchronized && !closed.metadata_synchronized &&
				!closed.descriptors_absent && !closed.closure_unknown);
			assert(filesystem.trace().size() == 2);
			assert(filesystem.trace()[0] == 'f');
			assert(filesystem.trace()[1] == 's');
		}
	}
}

void TestSuccessfulSaveAuthorityFailureRetainsDescriptorsForSameLeaseRetry()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter adapter(broker, filesystem);
	assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
	filesystem.CorruptSystemDirectoryEntry();
	const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
	assert(first.result == MISTER_RESULT_PLATFORM);
	assert(!first.descriptors_absent && !first.closure_unknown);
	filesystem.RestoreSystemDirectoryEntry();
	const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
	assert(second.result == MISTER_RESULT_OK && second.data_synchronized &&
		second.metadata_synchronized && second.descriptors_absent &&
		!second.closure_unknown);
}

void TestEveryCleanupRevalidationFailureRetainsTheOriginalDescriptors()
{
	struct CleanupCase {
		FakeSaveFileSystem::FailurePoint point;
		int first_call;
		int last_call;
	};
	const CleanupCase cases[] = {
		{FakeSaveFileSystem::FailurePoint::stat, 11, 21},
		{FakeSaveFileSystem::FailurePoint::statat, 6, 13},
		{FakeSaveFileSystem::FailurePoint::mount, 9, 16}
	};
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (const CleanupCase &test : cases) {
		for (int call = test.first_call; call <= test.last_call; ++call) {
			FakeClock clock(10);
			HardwareBroker broker(clock);
			PlatformGenerationId generation = 0;
			assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> lease;
			assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
				MISTER_RESULT_OK);
			FakeSaveFileSystem filesystem;
			NativeSaveAdapter adapter(broker, filesystem);
			assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
			filesystem.FailAt(test.point, call);
			const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
			assert(first.result == MISTER_RESULT_PLATFORM);
			assert(!first.descriptors_absent && !first.closure_unknown);
			const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
			assert(second.result == MISTER_RESULT_OK && second.data_synchronized &&
				second.metadata_synchronized && second.descriptors_absent &&
				!second.closure_unknown);
		}
	}
}

void TestEverySaveOperationHonorsItsOriginalDeadline()
{
	struct OpenCase {
		FakeSaveFileSystem::Operation operation;
		int first_call;
		int last_call;
	};
	const OpenCase open_cases[] = {
		{FakeSaveFileSystem::Operation::open, 1, 6},
		{FakeSaveFileSystem::Operation::stat, 1, 10},
		{FakeSaveFileSystem::Operation::statat, 1, 5},
		{FakeSaveFileSystem::Operation::mount, 1, 8},
		{FakeSaveFileSystem::Operation::fsync, 1, 2}
	};
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	auto new_adapter = [&](HardwareBroker &broker, FakeSaveFileSystem &filesystem,
		std::unique_ptr<OperationLease> *lease,
		PlatformGenerationId *generation) {
		assert(generation != nullptr && lease != nullptr);
		assert(broker.EnterFixtureForTest(*profile, generation) == MISTER_RESULT_OK);
		assert(broker.Begin(*generation, OperationKind::save, 1000, lease) ==
			MISTER_RESULT_OK);
		return std::unique_ptr<NativeSaveAdapter>(
			new NativeSaveAdapter(broker, filesystem));
	};
	// Every operation reached during acquisition checks its immutable deadline
	// immediately after the operation. A late result retains its descriptors;
	// subsequent cleanup cannot issue I/O or mint a later deadline.
	for (const OpenCase &test : open_cases) {
		for (int boundary = 0; boundary != 2; ++boundary) {
		for (int call = test.first_call; call <= test.last_call; ++call) {
			FakeClock clock(10);
			HardwareBroker broker(clock);
			FakeSaveFileSystem filesystem;
			std::unique_ptr<OperationLease> lease;
			PlatformGenerationId generation = 0;
			std::unique_ptr<NativeSaveAdapter> adapter = new_adapter(broker,
				filesystem, &lease, &generation);
			if (boundary == 0)
				filesystem.AdvanceBeforeOperation(test.operation, call, 1000);
			else filesystem.AdvanceAfterOperation(test.operation, call, 1000);
			assert(adapter->OpenSave(*lease, *profile, key).result ==
				MISTER_RESULT_DEADLINE);
			const size_t calls = filesystem.trace().size();
			const NativeSaveCloseOutcome closed = adapter->FlushAndCloseSave(*lease);
			assert((closed.result == MISTER_RESULT_DEADLINE ||
				closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE) &&
				!closed.descriptors_absent && !closed.closure_unknown);
			assert(filesystem.trace().size() == calls);
		}
		}
	}
	// The close-side fstat/fstatat/mount observations run twice around final
	// data sync; each exact call must fail closed at equality rather than refresh.
	const OpenCase cleanup_cases[] = {
		{FakeSaveFileSystem::Operation::stat, 11, 21},
		{FakeSaveFileSystem::Operation::statat, 6, 13},
		{FakeSaveFileSystem::Operation::mount, 9, 16}
	};
	for (const OpenCase &test : cleanup_cases) {
		for (int boundary = 0; boundary != 2; ++boundary) {
		for (int call = test.first_call; call <= test.last_call; ++call) {
			FakeClock clock(10);
			HardwareBroker broker(clock);
			FakeSaveFileSystem filesystem;
			std::unique_ptr<OperationLease> lease;
			PlatformGenerationId generation = 0;
			std::unique_ptr<NativeSaveAdapter> adapter = new_adapter(broker,
				filesystem, &lease, &generation);
			assert(adapter->OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
			if (boundary == 0)
				filesystem.AdvanceBeforeOperation(test.operation, call, 1000);
			else filesystem.AdvanceAfterOperation(test.operation, call, 1000);
			const NativeSaveCloseOutcome closed = adapter->FlushAndCloseSave(*lease);
			assert(closed.result == MISTER_RESULT_DEADLINE &&
				!closed.descriptors_absent && !closed.closure_unknown);
		}
		}
	}
	for (int boundary = 0; boundary != 2; ++boundary) {
	for (int close_call = 1; close_call <= 5; ++close_call) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		FakeSaveFileSystem filesystem;
		std::unique_ptr<OperationLease> lease;
		PlatformGenerationId generation = 0;
		std::unique_ptr<NativeSaveAdapter> adapter = new_adapter(broker,
			filesystem, &lease, &generation);
		assert(adapter->OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
		if (boundary == 0)
			filesystem.AdvanceBeforeOperation(FakeSaveFileSystem::Operation::close,
				close_call, 1000);
		else filesystem.AdvanceAfterOperation(FakeSaveFileSystem::Operation::close,
			close_call, 1000);
		const NativeSaveCloseOutcome closed = adapter->FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE &&
			!closed.descriptors_absent && !closed.closure_unknown);
	}
	}
	for (int boundary = 0; boundary != 2; ++boundary) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		FakeSaveFileSystem filesystem;
		std::unique_ptr<OperationLease> lease;
		PlatformGenerationId generation = 0;
		std::unique_ptr<NativeSaveAdapter> adapter = new_adapter(broker,
			filesystem, &lease, &generation);
		assert(adapter->OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
		if (boundary == 0)
			filesystem.AdvanceBeforeOperation(FakeSaveFileSystem::Operation::fdatasync,
				1, 1000);
		else filesystem.AdvanceAfterOperation(FakeSaveFileSystem::Operation::fdatasync,
			1, 1000);
		const NativeSaveCloseOutcome closed = adapter->FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_DEADLINE &&
			!closed.descriptors_absent && !closed.closure_unknown);
	}
	// Equality and one millisecond beyond never refresh a retained prefix.
	for (uint64_t late_now : {1000u, 1001u}) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		FakeSaveFileSystem filesystem;
		std::unique_ptr<OperationLease> lease;
		PlatformGenerationId generation = 0;
		std::unique_ptr<NativeSaveAdapter> adapter = new_adapter(broker,
			filesystem, &lease, &generation);
		filesystem.AdvanceAfterOperation(FakeSaveFileSystem::Operation::open, 1,
			late_now);
		assert(adapter->OpenSave(*lease, *profile, key).result ==
			MISTER_RESULT_DEADLINE);
		const size_t calls = filesystem.trace().size();
		assert(adapter->FlushAndCloseSave(*lease).result ==
			MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(filesystem.trace().size() == calls);
	}
	// Minus one remains within the same immutable absolute deadline.
	FakeClock clock(999);
	HardwareBroker broker(clock);
	FakeSaveFileSystem filesystem;
	std::unique_ptr<OperationLease> lease;
	PlatformGenerationId generation = 0;
	std::unique_ptr<NativeSaveAdapter> adapter = new_adapter(broker,
		filesystem, &lease, &generation);
	assert(adapter->OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
	assert(adapter->FlushAndCloseSave(*lease).result == MISTER_RESULT_OK);
}

void TestDirectoryAndFileAuthorityAdversariesAreClosedWithoutWritableAccess()
{
	struct DirectoryCase {
		FakeSaveFileSystem::NodeKind node;
		FakeSaveFileSystem::Corruption corruption;
	};
	const DirectoryCase directories[] = {
		{FakeSaveFileSystem::NodeKind::root, FakeSaveFileSystem::Corruption::uid},
		{FakeSaveFileSystem::NodeKind::root, FakeSaveFileSystem::Corruption::gid},
		{FakeSaveFileSystem::NodeKind::root, FakeSaveFileSystem::Corruption::mode},
		{FakeSaveFileSystem::NodeKind::root, FakeSaveFileSystem::Corruption::writable},
		{FakeSaveFileSystem::NodeKind::root, FakeSaveFileSystem::Corruption::type},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::uid},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::gid},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::mode},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::writable},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::type},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::device},
		{FakeSaveFileSystem::NodeKind::parent, FakeSaveFileSystem::Corruption::mount},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::uid},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::gid},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::mode},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::writable},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::type},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::device},
		{FakeSaveFileSystem::NodeKind::save_root, FakeSaveFileSystem::Corruption::mount},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::uid},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::gid},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::mode},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::writable},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::type},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::device},
		{FakeSaveFileSystem::NodeKind::system, FakeSaveFileSystem::Corruption::mount},
		{FakeSaveFileSystem::NodeKind::system,
			FakeSaveFileSystem::Corruption::entry_identity}
	};
	const FakeSaveFileSystem::Corruption files[] = {
		FakeSaveFileSystem::Corruption::uid,
		FakeSaveFileSystem::Corruption::gid,
		FakeSaveFileSystem::Corruption::mode,
		FakeSaveFileSystem::Corruption::type,
		FakeSaveFileSystem::Corruption::links,
		FakeSaveFileSystem::Corruption::overlarge,
		FakeSaveFileSystem::Corruption::misaligned,
		FakeSaveFileSystem::Corruption::entry_identity
	};
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (const DirectoryCase &test : directories) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.Corrupt(test.node, test.corruption);
		NativeSaveAdapter adapter(broker, filesystem);
		const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
		assert(opened.result == MISTER_RESULT_PLATFORM);
		if (opened.acquired) {
			const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
			const bool post_create_entry_failure =
				test.node == FakeSaveFileSystem::NodeKind::system &&
				test.corruption == FakeSaveFileSystem::Corruption::entry_identity;
			assert(closed.descriptors_absent == !post_create_entry_failure);
			if (post_create_entry_failure) {
				assert(closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
				assert(filesystem.trace().size() == 2);
				assert(filesystem.trace()[0] == 'f');
				assert(filesystem.trace()[1] == 's');
			}
		}
		if (test.corruption != FakeSaveFileSystem::Corruption::entry_identity)
			assert(filesystem.last_file_name()[0] == '\0');
	}
	for (FakeSaveFileSystem::Corruption corruption : files) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.Corrupt(FakeSaveFileSystem::NodeKind::file, corruption);
		NativeSaveAdapter adapter(broker, filesystem);
		const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
		assert(opened.result == MISTER_RESULT_PLATFORM && opened.acquired);
		const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_CLEANUP_INCOMPLETE &&
			!closed.descriptors_absent && !closed.closure_unknown);
		assert(filesystem.trace().size() == 2);
		assert(filesystem.trace()[0] == 'f');
		assert(filesystem.trace()[1] == 's');
	}
}

void TestExistingCreationRaceAndCloseUncertaintyMatrix()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	{
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.SetExistingFile(true);
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
		assert(!filesystem.create_open());
		const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_OK && closed.descriptors_absent);
	}
	{
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		filesystem.FailCreateWithEexist();
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_PLATFORM);
		assert(!filesystem.create_open());
		const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_OK &&
			closed.descriptors_absent && !closed.closure_unknown);
		assert(!closed.data_synchronization_required &&
			!closed.metadata_synchronization_required);
	}
	for (int descriptor = 10; descriptor != 15; ++descriptor) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
		filesystem.FailClose(descriptor);
		const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
		assert(first.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
		assert(first.closure_unknown && !first.descriptors_absent);
		const size_t calls = filesystem.trace().size();
		assert(adapter.FlushAndCloseSave(*lease).closure_unknown);
		assert(filesystem.trace().size() == calls);
	}
}

void TestSaveSyncFailureAndDeadlineRetriesKeepTheSameDescriptors()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (int failure = 0; failure != 3; ++failure) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
		if (failure == 0) filesystem.FailFdatasyncOnce();
		else {
			filesystem.SetFileSize(512);
			filesystem.FailFsyncOnce(failure == 1 ? 14 : 13);
		}
		const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
		assert(first.result == MISTER_RESULT_PLATFORM);
		assert(!first.descriptors_absent && !first.closure_unknown);
		const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
		assert(second.result == MISTER_RESULT_OK);
		assert(second.data_synchronized && second.metadata_synchronized);
		assert(second.descriptors_absent && !second.closure_unknown);
	}
	{
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
		filesystem.AdvanceAfterFdatasync(1000);
		const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
		assert(first.result == MISTER_RESULT_DEADLINE && !first.descriptors_absent);
		const size_t calls = filesystem.trace().size();
		const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
		assert(second.result == MISTER_RESULT_DEADLINE &&
			!second.descriptors_absent && !second.closure_unknown);
		assert(filesystem.trace().size() == calls);
	}
}

void TestSaveAdapterDestructorCanOutliveItsBrokerWithoutAReceipt()
{
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter *adapter = nullptr;
	{
		FakeClock clock(10);
		HardwareBroker broker(clock);
		const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
		assert(profile != nullptr);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		NativeSaveKey key = {};
		assert(MakeNativeSaveKey(*profile,
			kPrivacyContentDigest,
			&key));
		adapter = new NativeSaveAdapter(broker, filesystem);
		assert(adapter->OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
	}
	delete adapter;
	assert(filesystem.trace().size() == 7);
	assert(filesystem.trace()[2] == '4');
	assert(filesystem.trace()[6] == '0');
}

void TestDirectoryDeviceAndMountRelationsFailBeforeWritableFileAccess()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (int case_index = 0; case_index != 2; ++case_index) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		if (case_index == 0) filesystem.SetSystemDevice(10);
		else filesystem.SetSystemMount(78);
		NativeSaveAdapter adapter(broker, filesystem);
		const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
		assert(opened.result == MISTER_RESULT_PLATFORM);
		assert(opened.acquired);
		assert(filesystem.last_file_name()[0] == '\0');
	}
}

void TestCloseFailureNeverRetriesPossiblyReusedDescriptor()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter adapter(broker, filesystem);
	assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
	filesystem.FailClose(14);
	const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
	assert(first.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(first.closure_unknown);
	assert(!first.descriptors_absent);
	const size_t calls_after_first_close = filesystem.trace().size();
	const NativeSaveCloseOutcome second = adapter.FlushAndCloseSave(*lease);
	assert(second.result == MISTER_RESULT_CLEANUP_INCOMPLETE);
	assert(second.closure_unknown);
	assert(filesystem.trace().size() == calls_after_first_close);
}

void TestCreationBarrierRetrySkipsTheCompletedFileSync()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	filesystem.FailFsyncOnce(13);
	NativeSaveAdapter adapter(broker, filesystem);
	const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
	assert(opened.result == MISTER_RESULT_PLATFORM);
	assert(opened.acquired);
	assert(filesystem.trace().size() == 2);
	assert(filesystem.trace()[0] == 'f');
	assert(filesystem.trace()[1] == 's');
	const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
	assert(closed.result == MISTER_RESULT_OK);
	assert(closed.data_synchronized);
	assert(closed.metadata_synchronized);
	assert(filesystem.trace().size() == 9);
	assert(filesystem.trace()[2] == 's');
	assert(filesystem.trace()[3] == 'd');
	assert(filesystem.trace()[4] == '4');
}

void TestCreationFileBarrierFailureRetriesOnlyItsIncompleteBarrier()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	filesystem.FailFsyncOnce(14);
	NativeSaveAdapter adapter(broker, filesystem);
	const NativeSaveOpenOutcome opened = adapter.OpenSave(*lease, *profile, key);
	assert(opened.result == MISTER_RESULT_PLATFORM && opened.acquired);
	assert(filesystem.trace().size() == 1 && filesystem.trace()[0] == 'f');
	const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
	assert(closed.result == MISTER_RESULT_OK && closed.data_synchronized &&
		closed.metadata_synchronized && closed.descriptors_absent);
	assert(filesystem.trace().size() == 9);
	assert(filesystem.trace()[1] == 'f');
	assert(filesystem.trace()[2] == 's');
	assert(filesystem.trace()[3] == 'd');
}

void TestChangedFileMetadataFsyncsFileThenSystemDirectory()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter adapter(broker, filesystem);
	assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
	filesystem.SetFileSize(512);
	const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
	assert(closed.result == MISTER_RESULT_OK);
	assert(closed.data_synchronized);
	assert(closed.metadata_synchronized);
	assert(filesystem.trace().size() == 10);
	assert(filesystem.trace()[2] == 'd');
	assert(filesystem.trace()[3] == 'f');
	assert(filesystem.trace()[4] == 's');
	assert(filesystem.trace()[5] == '4');
}

void TestMetadataFsyncDeadlineEdgesAndRetryFactsRemainIndependent()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile, kPrivacyContentDigest, &key));
	for (int metadata_fsync_call : {3, 4}) {
		for (int boundary = 0; boundary != 2; ++boundary) {
			FakeClock clock(10);
			HardwareBroker broker(clock);
			PlatformGenerationId generation = 0;
			assert(broker.EnterFixtureForTest(*profile, &generation) ==
				MISTER_RESULT_OK);
			std::unique_ptr<OperationLease> lease;
			assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
				MISTER_RESULT_OK);
			FakeSaveFileSystem filesystem;
			NativeSaveAdapter adapter(broker, filesystem);
			assert(adapter.OpenSave(*lease, *profile, key).result ==
				MISTER_RESULT_OK);
			filesystem.SetFileSize(512);
			if (boundary == 0)
				filesystem.AdvanceBeforeOperation(FakeSaveFileSystem::Operation::fsync,
					metadata_fsync_call, 1000);
			else filesystem.AdvanceAfterOperation(FakeSaveFileSystem::Operation::fsync,
				metadata_fsync_call, 1000);
			const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
			assert(first.result == MISTER_RESULT_DEADLINE);
			assert(first.data_synchronized && !first.metadata_synchronized &&
				!first.descriptors_absent && !first.closure_unknown);
			const size_t expected_trace_size =
				metadata_fsync_call == 3 ? 4 : 5;
			assert(filesystem.trace().size() == expected_trace_size);
			assert(filesystem.trace()[0] == 'f');
			assert(filesystem.trace()[1] == 's');
			assert(filesystem.trace()[2] == 'd');
			assert(filesystem.trace()[expected_trace_size - 1] ==
				(metadata_fsync_call == 3 ? 'f' : 's'));
			const NativeSaveCloseOutcome retry = adapter.FlushAndCloseSave(*lease);
			assert(retry.result == MISTER_RESULT_DEADLINE &&
				!retry.descriptors_absent && !retry.closure_unknown);
			assert(filesystem.trace().size() == expected_trace_size);
		}
	}
	for (int failed_metadata_fsync_call : {3, 4}) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result ==
			MISTER_RESULT_OK);
		filesystem.SetFileSize(512);
		filesystem.FailFsyncAtCallOnce(failed_metadata_fsync_call);
		const NativeSaveCloseOutcome first = adapter.FlushAndCloseSave(*lease);
		assert(first.result == MISTER_RESULT_PLATFORM && first.data_synchronized &&
			!first.metadata_synchronized && !first.descriptors_absent &&
			!first.closure_unknown);
		const NativeSaveCloseOutcome retry = adapter.FlushAndCloseSave(*lease);
		assert(retry.result == MISTER_RESULT_OK && retry.data_synchronized &&
			retry.metadata_synchronized && retry.descriptors_absent &&
			!retry.closure_unknown);
		const char expected_file_retry[] = {
			'f', 's', 'd', 'f', 'f', 's', '4', '3', '2', '1', '0'};
		const char expected_directory_retry[] = {
			'f', 's', 'd', 'f', 's', 's', '4', '3', '2', '1', '0'};
		const char *const expected = failed_metadata_fsync_call == 3 ?
			expected_file_retry : expected_directory_retry;
		assert(filesystem.trace().size() == sizeof(expected_file_retry));
		for (size_t index = 0; index < sizeof(expected_file_retry); ++index)
			assert(filesystem.trace()[index] == expected[index]);
	}
}

void TestForeignProfileCannotOpenFixtureSaveAuthority()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	NativeCoreProfile copied = *profile;
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter adapter(broker, filesystem);
	assert(adapter.OpenSave(*lease, copied, key).result == MISTER_RESULT_UNSUPPORTED);
	assert(filesystem.trace().empty());
}

void TestMalformedSaveAuthorityComponentsAndPathTokensNeverReachFilesystem()
{
	const char root_sentinel[] = "/task6f-root-sentinel-2f5a";
	const char system_sentinel[] = "task6f-system-sentinel-6b91";
	const char profile_sentinel[] = "task6f-profile-sentinel-4cd3";
	const char core_sentinel[] = "task6f-core-sentinel-8e20";
	const char game_sentinel[] = "task6f-game-sentinel-9a47";
	const char basename_sentinel[] = "task6f-basename-sentinel-1d6f";
	const char device_sentinel[] = "/task6f-device-sentinel-73bc";
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile, kPrivacyContentDigest, &key));
	NativeSaveDirectoryAuthority chain[5] = {};
	for (size_t index = 0; index < 4; ++index)
		chain[index] = profile->save.root_authority->chain[index];
	NativeSaveRootAuthority root = *profile->save.root_authority;
	NativeCoreProfile malformed = *profile;
	FakeClock clock(10);
	HardwareBroker broker(clock);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	auto reject = [&](const NativeCoreProfile &candidate) {
		FakeSaveFileSystem filesystem;
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, candidate, key).result ==
			MISTER_RESULT_UNSUPPORTED);
		assert(filesystem.trace().empty());
		assert(filesystem.last_file_name()[0] == '\0');
	};

	// Every unique sentinel is supplied as untrusted material.  The exact
	// authority singleton check rejects it before a descriptor operation or any
	// logging route. Device/basename/game are intentionally not save-key inputs.
	malformed.system = system_sentinel;
	malformed.core = core_sentinel;
	malformed.artifact.sha256 = game_sentinel;
	malformed.extensions[0] = basename_sentinel;
	malformed.save.root_authority = &root;
	root.absolute_root = root_sentinel;
	reject(malformed);
	malformed = *profile;
	malformed.core = profile_sentinel;
	reject(malformed);
	malformed = *profile;
	malformed.extensions[1] = device_sentinel;
	reject(malformed);
	malformed = *profile;
	malformed.video.i2c_bus_number = kPrivacyBusNumber;
	reject(malformed);

	const NativeSaveDirectoryRole roles[] = {
		NativeSaveDirectoryRole::immutable_parent,
		NativeSaveDirectoryRole::root_anchor};
	for (NativeSaveDirectoryRole role : roles) {
		chain[0] = profile->save.root_authority->chain[0];
		chain[0].role = role;
		root = *profile->save.root_authority;
		root.chain = chain;
		malformed = *profile;
		malformed.save.root_authority = &root;
		reject(malformed);
	}
	for (size_t count : {3u, 5u}) {
		for (size_t index = 0; index < 4; ++index)
			chain[index] = profile->save.root_authority->chain[index];
		chain[4] = chain[3];
		root = *profile->save.root_authority;
		root.chain = chain;
		root.chain_count = count;
		malformed = *profile;
		malformed.save.root_authority = &root;
		reject(malformed);
	}
	for (size_t index = 0; index < 4; ++index)
		chain[index] = profile->save.root_authority->chain[index];
	chain[2] = chain[1];
	root = *profile->save.root_authority;
	root.chain = chain;
	malformed = *profile;
	malformed.save.root_authority = &root;
	reject(malformed);
	chain[1] = profile->save.root_authority->chain[2];
	chain[2] = profile->save.root_authority->chain[1];
	root = *profile->save.root_authority;
	root.chain = chain;
	malformed = *profile;
	malformed.save.root_authority = &root;
	reject(malformed);
}

void TestRecoveryRecordCannotCloseAnActiveSave()
{
	FakeClock clock(10);
	HardwareBroker broker(clock);
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	PlatformGenerationId generation = 0;
	assert(broker.EnterFixtureForTest(*profile, &generation) == MISTER_RESULT_OK);
	std::unique_ptr<OperationLease> lease;
	assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
		MISTER_RESULT_OK);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	FakeSaveFileSystem filesystem;
	NativeSaveAdapter adapter(broker, filesystem);
	assert(adapter.OpenSave(*lease, *profile, key).result == MISTER_RESULT_OK);
	const SafeSaveRecoveryRecord *const record =
		FixtureSafeSaveRecoveryRecordForTest(NativeSystem::snes);
	assert(record != nullptr);
	assert(adapter.RecoverSave(*lease, *record).result ==
		MISTER_RESULT_INVALID_STATE);
	assert(filesystem.trace().size() == 2);
}

void TestTwoHundredSaveOpenCloseCycles()
{
	const NativeCoreProfile *const profile = FixtureNativeCoreProfile("snes");
	assert(profile != nullptr);
	NativeSaveKey key = {};
	assert(MakeNativeSaveKey(*profile,
		kPrivacyContentDigest,
		&key));
	for (int cycle = 0; cycle != 200; ++cycle) {
		FakeClock clock(10);
		HardwareBroker broker(clock);
		PlatformGenerationId generation = 0;
		assert(broker.EnterFixtureForTest(*profile, &generation) ==
			MISTER_RESULT_OK);
		std::unique_ptr<OperationLease> lease;
		assert(broker.Begin(generation, OperationKind::save, 1000, &lease) ==
			MISTER_RESULT_OK);
		FakeSaveFileSystem filesystem;
		NativeSaveAdapter adapter(broker, filesystem);
		assert(adapter.OpenSave(*lease, *profile, key).result ==
			MISTER_RESULT_OK);
		const NativeSaveCloseOutcome closed = adapter.FlushAndCloseSave(*lease);
		assert(closed.result == MISTER_RESULT_OK && closed.descriptors_absent);
	}
}

} // namespace

int main()
{
	TestSaveKeyUsesClosedSystemAndLowercaseContentDigest();
	TestFixtureSaveAuthorityUsesExactClosedDirectoryChain();
	TestNewSaveFsyncsFileThenSystemDirectoryBeforeAcquisition();
	TestUnavailableMountIdentityFailsBeforeWritableFileAccess();
	TestPartialRootPrefixIsClosedAfterMountIdentityFailure();
	TestEveryRetainedSaveResolutionFailureClosesItsDescriptors();
	TestExpiredRetainedRootPrefixNeverRefreshesItsOriginalDeadline();
	TestPostCreateIdentityFailureRetainsTheDescriptorChainUntilStable();
	TestEveryPersistentPostCreateAuthorityFailureRetainsAfterCreationBarriers();
	TestPersistentPostCreateBarrierRetriesDoNotRepeatCompletedFacts();
	TestEveryPersistentPostCreateRevalidationFailureRetainsAfterBarriers();
	TestSuccessfulSaveAuthorityFailureRetainsDescriptorsForSameLeaseRetry();
	TestEveryCleanupRevalidationFailureRetainsTheOriginalDescriptors();
	TestEverySaveOperationHonorsItsOriginalDeadline();
	TestDirectoryAndFileAuthorityAdversariesAreClosedWithoutWritableAccess();
	TestExistingCreationRaceAndCloseUncertaintyMatrix();
	TestSaveSyncFailureAndDeadlineRetriesKeepTheSameDescriptors();
	TestSaveAdapterDestructorCanOutliveItsBrokerWithoutAReceipt();
	TestDirectoryDeviceAndMountRelationsFailBeforeWritableFileAccess();
	TestCloseFailureNeverRetriesPossiblyReusedDescriptor();
	TestCreationBarrierRetrySkipsTheCompletedFileSync();
	TestCreationFileBarrierFailureRetriesOnlyItsIncompleteBarrier();
	TestChangedFileMetadataFsyncsFileThenSystemDirectory();
	TestMetadataFsyncDeadlineEdgesAndRetryFactsRemainIndependent();
	TestForeignProfileCannotOpenFixtureSaveAuthority();
	TestMalformedSaveAuthorityComponentsAndPathTokensNeverReachFilesystem();
	TestRecoveryRecordCannotCloseAnActiveSave();
	TestTwoHundredSaveOpenCloseCycles();
	return 0;
}

#endif
