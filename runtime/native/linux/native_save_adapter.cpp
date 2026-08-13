// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_save_adapter.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <memory>

namespace mister {
namespace native {
namespace linux_native {
namespace {

#if defined(MISTER_NATIVE_SAVE_TESTING)

bool SameEntry(const struct stat &left, const struct stat &right)
{
	return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool ExactMode(const struct stat &info, uint16_t mode)
{
	return (static_cast<uint32_t>(info.st_mode) & 07777u) == mode;
}

bool ValidDirectoryComponent(const char *component)
{
	if (component == nullptr || component[0] == '\0' ||
		strcmp(component, ".") == 0 || strcmp(component, "..") == 0)
		return false;
	for (const char *cursor = component; *cursor != '\0'; ++cursor)
		if (*cursor == '/' || *cursor == '\\') return false;
	return true;
}

bool RelationMatches(NativeSaveDeviceRelation relation, dev_t parent,
	dev_t child)
{
	return (relation == NativeSaveDeviceRelation::same_device_as_parent &&
		parent == child) ||
		(relation == NativeSaveDeviceRelation::distinct_device_from_parent &&
		parent != child);
}

bool RelationMatches(NativeSaveMountRelation relation, uint64_t parent,
	uint64_t child)
{
	return (relation == NativeSaveMountRelation::same_mount_as_parent &&
		parent == child) ||
		(relation == NativeSaveMountRelation::distinct_mount_from_parent &&
		parent != child);
}

bool ValidDirectory(const struct stat &info,
	const NativeSaveDirectoryAuthority &authority)
{
	return S_ISDIR(info.st_mode) && info.st_uid == authority.uid &&
		info.st_gid == authority.gid && ExactMode(info, authority.exact_mode) &&
		(info.st_mode & 0022) == 0;
}

bool ValidFile(const struct stat &info, const NativeSaveRootAuthority &authority,
	const NativeSaveProfile &profile)
{
	if (!S_ISREG(info.st_mode) || info.st_uid != authority.file_uid ||
		info.st_gid != authority.file_gid || !ExactMode(info, authority.file_mode) ||
		info.st_nlink != 1 || info.st_size < 0 ||
		static_cast<uint64_t>(info.st_size) > profile.maximum_bytes ||
		(static_cast<uint64_t>(info.st_size) != 0 &&
		 static_cast<uint64_t>(info.st_size) % profile.sector_bytes != 0))
		return false;
	return true;
}

#endif

} // namespace

#if defined(MISTER_NATIVE_SAVE_TESTING)

NativeSaveAdapter::NativeSaveAdapter(HardwareBroker &broker,
	NativeSaveFileSystem &filesystem)
	: broker_(broker), mutex_(), filesystem_(&filesystem), authority_(nullptr),
	  profile_(nullptr), key_(), root_descriptors_(), root_identities_(), root_mount_ids_(),
	  root_descriptor_count_(0), file_descriptor_(-1), file_identity_(),
	  file_created_(false), creation_file_sync_complete_(false),
	  creation_directory_sync_complete_(false), final_data_sync_complete_(false),
	  final_file_metadata_sync_complete_(false),
	  final_directory_sync_complete_(false), closure_unknown_(false),
	  metadata_changed_(false), root_chain_validated_(false),
	  open_failure_(MISTER_RESULT_OK)
{
	for (size_t index = 0; index < kMaximumRootDescriptors; ++index)
		root_descriptors_[index] = -1;
}

#else

NativeSaveAdapter::NativeSaveAdapter(HardwareBroker &broker)
	: broker_(broker), mutex_()
{
}

#endif

NativeSaveAdapter::~NativeSaveAdapter()
{
	CloseSaveForProcessExit();
}

NativeSaveOpenOutcome NativeSaveAdapter::OpenSave(const OperationLease &lease,
	const NativeCoreProfile &profile, const NativeSaveKey &key)
{
	std::unique_ptr<ProcessOperationGuard> guard;
	Result result = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::save, &profile, &guard);
	if (result != MISTER_RESULT_OK) return {result, false};
	std::lock_guard<std::mutex> lock(mutex_);
#if !defined(MISTER_NATIVE_SAVE_TESTING)
	(void)key;
	return {MISTER_RESULT_UNSUPPORTED, false};
#else
	if (authority_ != nullptr || closure_unknown_ || !AllDescriptorsAbsentLocked())
		return {MISTER_RESULT_INVALID_STATE, !AllDescriptorsAbsentLocked()};
	if (!ValidateAuthority(profile, key)) return {MISTER_RESULT_UNSUPPORTED, false};
	authority_ = profile.save.root_authority;
	profile_ = &profile.save;
	key_ = key;
	file_created_ = false;
	creation_file_sync_complete_ = false;
	creation_directory_sync_complete_ = false;
	final_data_sync_complete_ = false;
	final_file_metadata_sync_complete_ = false;
	final_directory_sync_complete_ = false;
	metadata_changed_ = false;
	root_chain_validated_ = false;
	open_failure_ = MISTER_RESULT_OK;
	result = OpenRootChainLocked(guard->absolute_deadline_ms());
	if (result == MISTER_RESULT_OK)
		result = OpenFileLocked(guard->absolute_deadline_ms(), true);
	if (result == MISTER_RESULT_OK)
		result = RevalidateLocked(guard->absolute_deadline_ms(), true);
	if (result == MISTER_RESULT_OK)
		result = CompleteCreationBarriersLocked(guard->absolute_deadline_ms());
	if (result != MISTER_RESULT_OK) {
		open_failure_ = result;
		// No descriptor was acquired, so lifecycle will not own or clean this
		// resource.  Discard every provisional authority value before returning
		// the unowned failure; otherwise a transient first-root error poisons a
		// later activation on this reusable adapter.
		if (AllDescriptorsAbsentLocked()) ResetUnownedStateLocked();
	}
	return {result, !AllDescriptorsAbsentLocked()};
#endif
}

NativeSaveCloseOutcome NativeSaveAdapter::FlushAndCloseSave(
	const OperationLease &lease)
{
	std::unique_ptr<ProcessOperationGuard> guard;
	const Result admitted = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::save, nullptr, &guard);
	if (admitted != MISTER_RESULT_OK)
		return {admitted, false, false, false, false};
	std::lock_guard<std::mutex> lock(mutex_);
	return CloseLocked(guard->absolute_deadline_ms(), true);
}

NativeSaveCloseOutcome NativeSaveAdapter::RecoverSave(const OperationLease &lease,
	const SafeSaveRecoveryRecord &record)
{
	std::unique_ptr<ProcessOperationGuard> guard;
	const Result admitted = lease.AcquireProcessOperationGuard(broker_,
		OperationKind::save, nullptr, &guard);
	if (admitted != MISTER_RESULT_OK)
		return {admitted, false, false, false, false};
	if (guard->authority() != LeaseAuthority::recovery_epoch)
		return {MISTER_RESULT_INVALID_STATE, false, false, false, false};
	std::lock_guard<std::mutex> lock(mutex_);
#if !defined(MISTER_NATIVE_SAVE_TESTING)
	(void)record;
	return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
#else
	if (!IsExactFixtureSafeSaveRecoveryRecordForTest(&record))
		return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
	if (authority_ == nullptr) {
		const NativeCoreProfile *const profile =
			FixtureNativeCoreProfileForSafeSaveRecoveryRecordForTest(&record);
		if (profile == nullptr || !ValidateAuthority(*profile,
			NativeSaveKey{record.system_id, {}}))
			return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
		NativeSaveKey key = {};
		key.system_id = record.system_id;
		memcpy(key.content_sha256, record.content_sha256,
			sizeof(key.content_sha256));
		if (!ValidateAuthority(*profile, key))
			return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
		authority_ = profile->save.root_authority;
		profile_ = &profile->save;
		key_ = key;
		file_created_ = false;
		creation_file_sync_complete_ = false;
		creation_directory_sync_complete_ = false;
		final_data_sync_complete_ = false;
		final_file_metadata_sync_complete_ = false;
		final_directory_sync_complete_ = false;
		metadata_changed_ = false;
		root_chain_validated_ = false;
		open_failure_ = MISTER_RESULT_OK;
		const Result opened = OpenRootChainLocked(guard->absolute_deadline_ms());
		const Result file_result = opened == MISTER_RESULT_OK ?
			OpenFileLocked(guard->absolute_deadline_ms(), false) : opened;
		const Result revalidated = file_result == MISTER_RESULT_OK ?
			RevalidateLocked(guard->absolute_deadline_ms(), true) : file_result;
		if (revalidated != MISTER_RESULT_OK) {
			open_failure_ = revalidated;
			const NativeSaveCloseOutcome closed = CloseLocked(
				guard->absolute_deadline_ms(), false);
			return {revalidated, closed.data_synchronized,
				closed.metadata_synchronized, closed.descriptors_absent,
				closed.closure_unknown};
		}
	} else if (record.root_authority != authority_ ||
		record.system_id != key_.system_id ||
		memcmp(record.content_sha256, key_.content_sha256,
			sizeof(key_.content_sha256)) != 0) {
		return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
	}
	return CloseLocked(guard->absolute_deadline_ms(), true);
#endif
}

void NativeSaveAdapter::CloseSaveForProcessExit()
{
	std::lock_guard<std::mutex> lock(mutex_);
	CloseForProcessExitLocked();
}

#if defined(MISTER_NATIVE_SAVE_TESTING)

bool NativeSaveAdapter::ValidateAuthority(const NativeCoreProfile &profile,
	const NativeSaveKey &key) const
{
	const NativeSaveRootAuthority *const root = profile.save.root_authority;
	if (!IsExactFixtureNativeCoreProfile(profile) ||
		!IsExactFixtureNativeSaveRootAuthorityForTest(root) ||
		root == nullptr || root->absolute_root == nullptr || root->chain == nullptr ||
		root->chain_count == 0 || root->chain_count > kMaximumRootDescriptors ||
		key.system_id != profile.system_id ||
		profile.save.mode != NativeSaveMode::single_slot_growable_block_file ||
		profile.save.slot != 0 || profile.save.sector_bytes == 0 ||
		(profile.save.sector_bytes & (profile.save.sector_bytes - 1)) != 0 ||
		root->file_mode != 0600 || strcmp(root->chain[0].component, "/") != 0 ||
		root->chain[0].role != NativeSaveDirectoryRole::root_anchor ||
		root->chain[0].mount_relation != NativeSaveMountRelation::root_anchor ||
		root->chain[0].device_relation != NativeSaveDeviceRelation::root_anchor ||
		root->chain[root->chain_count - 1].role !=
			NativeSaveDirectoryRole::system_directory ||
		strcmp(root->chain[root->chain_count - 1].component,
			NativeSaveSystemToken(profile.system_id)) != 0)
		return false;
	char reconstructed[4096] = {};
	size_t used = 1;
	reconstructed[0] = '/';
	for (size_t index = 1; index < root->chain_count; ++index) {
		const char *const component = root->chain[index].component;
		if (!ValidDirectoryComponent(component)) return false;
		const size_t length = strlen(component);
		if (used + length + 1 >= sizeof(reconstructed)) return false;
		memcpy(reconstructed + used, component, length);
		used += length;
		if (index + 1 != root->chain_count) reconstructed[used++] = '/';
	}
	reconstructed[used] = '\0';
	return strcmp(reconstructed, root->absolute_root) == 0;
}

Result NativeSaveAdapter::CheckDeadline(uint64_t absolute_deadline_ms) const
{
	return filesystem_->NowMs() < absolute_deadline_ms ? MISTER_RESULT_OK :
		MISTER_RESULT_DEADLINE;
}

Result NativeSaveAdapter::OpenRootChainLocked(uint64_t absolute_deadline_ms)
{
	if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	for (size_t index = 0; index < authority_->chain_count; ++index) {
		const int parent = index == 0 ? AT_FDCWD : root_descriptors_[index - 1];
		const NativeSaveOpenResult opened = filesystem_->OpenAt(parent,
			authority_->chain[index].component,
			O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
		if (opened.descriptor < 0) return MISTER_RESULT_PLATFORM;
		root_descriptors_[index] = opened.descriptor;
		root_descriptor_count_ = index + 1;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (filesystem_->Stat(opened.descriptor, &root_identities_[index]) != 0 ||
			!ValidDirectory(root_identities_[index], authority_->chain[index]))
			return MISTER_RESULT_PLATFORM;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		const Result mount_result = filesystem_->MountId(opened.descriptor,
			&root_mount_ids_[index]);
		if (mount_result != MISTER_RESULT_OK) return mount_result;
		if (index != 0 &&
			(!RelationMatches(authority_->chain[index].device_relation,
				root_identities_[index - 1].st_dev, root_identities_[index].st_dev) ||
			 !RelationMatches(authority_->chain[index].mount_relation,
				root_mount_ids_[index - 1], root_mount_ids_[index])))
			return MISTER_RESULT_PLATFORM;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
	}
	root_chain_validated_ = true;
	return MISTER_RESULT_OK;
}

Result NativeSaveAdapter::OpenFileLocked(uint64_t absolute_deadline_ms,
	bool allow_creation)
{
	char filename[69] = {};
	if (!NativeSaveKeyFileName(key_, filename, sizeof(filename)))
		return MISTER_RESULT_INVALID_STATE;
	if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	NativeSaveOpenResult opened = filesystem_->OpenAt(
		root_descriptors_[root_descriptor_count_ - 1], filename,
		O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0);
	if (opened.descriptor < 0 && opened.error == ENOENT && allow_creation) {
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		opened = filesystem_->OpenAt(root_descriptors_[root_descriptor_count_ - 1],
			filename, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_EXCL, 0600);
		if (opened.descriptor < 0) return MISTER_RESULT_PLATFORM;
		file_created_ = true;
	} else if (opened.descriptor < 0) {
		return MISTER_RESULT_PLATFORM;
	}
	file_descriptor_ = opened.descriptor;
	if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	struct stat entry = {};
	if (filesystem_->Stat(file_descriptor_, &file_identity_) != 0 ||
		profile_ == nullptr || !ValidFile(file_identity_, *authority_, *profile_))
		return MISTER_RESULT_PLATFORM;
	if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	if (filesystem_->StatAt(root_descriptors_[root_descriptor_count_ - 1], filename,
		&entry, AT_SYMLINK_NOFOLLOW) != 0 || !SameEntry(file_identity_, entry))
		return MISTER_RESULT_PLATFORM;
	return CheckDeadline(absolute_deadline_ms);
}

Result NativeSaveAdapter::RevalidateLocked(uint64_t absolute_deadline_ms,
	bool include_file) const
{
	if (authority_ == nullptr || root_descriptor_count_ == 0 ||
		root_descriptor_count_ > authority_->chain_count ||
		CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return authority_ == nullptr || root_descriptor_count_ == 0 ||
			root_descriptor_count_ > authority_->chain_count ?
			MISTER_RESULT_INVALID_STATE : MISTER_RESULT_DEADLINE;
	for (size_t index = 0; index < root_descriptor_count_; ++index) {
		struct stat current = {};
		uint64_t mount_id = 0;
		if (filesystem_->Stat(root_descriptors_[index], &current) != 0 ||
			!SameEntry(root_identities_[index], current) ||
			!ValidDirectory(current, authority_->chain[index]))
			return MISTER_RESULT_PLATFORM;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		const Result mount_result = filesystem_->MountId(root_descriptors_[index],
			&mount_id);
		if (mount_result != MISTER_RESULT_OK) return mount_result;
		if (mount_id != root_mount_ids_[index]) return MISTER_RESULT_PLATFORM;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (index != 0) {
			struct stat entry = {};
			if (filesystem_->StatAt(root_descriptors_[index - 1],
				authority_->chain[index].component, &entry, AT_SYMLINK_NOFOLLOW) != 0 ||
				!SameEntry(entry, current) ||
				!RelationMatches(authority_->chain[index].device_relation,
					root_identities_[index - 1].st_dev, current.st_dev) ||
				!RelationMatches(authority_->chain[index].mount_relation,
					root_mount_ids_[index - 1], mount_id)) return MISTER_RESULT_PLATFORM;
			if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
				return MISTER_RESULT_DEADLINE;
		}
	}
	if (!include_file || file_descriptor_ < 0) return MISTER_RESULT_OK;
	char filename[69] = {};
	struct stat current = {};
	struct stat entry = {};
	if (profile_ == nullptr || !NativeSaveKeyFileName(key_, filename, sizeof(filename)) ||
		filesystem_->Stat(file_descriptor_, &current) != 0 ||
		!SameEntry(file_identity_, current) ||
		!ValidFile(current, *authority_, *profile_))
		return MISTER_RESULT_PLATFORM;
	if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	if (filesystem_->StatAt(root_descriptors_[root_descriptor_count_ - 1], filename,
		&entry, AT_SYMLINK_NOFOLLOW) != 0 || !SameEntry(current, entry))
		return MISTER_RESULT_PLATFORM;
	return CheckDeadline(absolute_deadline_ms);
}

Result NativeSaveAdapter::ObserveFinalMetadataLocked(
	uint64_t absolute_deadline_ms)
{
	if (file_descriptor_ < 0 ||
		CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	struct stat current = {};
	if (filesystem_->Stat(file_descriptor_, &current) != 0)
		return MISTER_RESULT_PLATFORM;
	if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	metadata_changed_ = metadata_changed_ ||
		current.st_size != file_identity_.st_size;
	return MISTER_RESULT_OK;
}

Result NativeSaveAdapter::CompleteCreationBarriersLocked(
	uint64_t absolute_deadline_ms)
{
	if (!file_created_) return MISTER_RESULT_OK;
	if (!creation_file_sync_complete_) {
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (filesystem_->Fsync(file_descriptor_) != 0) return MISTER_RESULT_PLATFORM;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		creation_file_sync_complete_ = true;
	}
	if (!creation_directory_sync_complete_) {
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		if (filesystem_->Fsync(root_descriptors_[root_descriptor_count_ - 1]) != 0)
			return MISTER_RESULT_PLATFORM;
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			return MISTER_RESULT_DEADLINE;
		creation_directory_sync_complete_ = true;
	}
	return MISTER_RESULT_OK;
}

Result NativeSaveAdapter::CloseDescriptorLocked(int *descriptor,
	uint64_t absolute_deadline_ms, bool process_exit)
{
	if (descriptor == nullptr || *descriptor < 0) return MISTER_RESULT_OK;
	if (!process_exit && CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	const int closing = *descriptor;
	const int result = filesystem_->Close(closing);
	// POSIX permits close to release the number even on error. Never probe or
	// retry it: a concurrent allocation could otherwise target another file.
	*descriptor = -1;
	if (result != 0) {
		closure_unknown_ = true;
		return MISTER_RESULT_CLEANUP_INCOMPLETE;
	}
	if (!process_exit && CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
		return MISTER_RESULT_DEADLINE;
	return MISTER_RESULT_OK;
}

bool NativeSaveAdapter::AllDescriptorsAbsentLocked() const
{
	if (file_descriptor_ >= 0) return false;
	for (size_t index = 0; index < root_descriptor_count_; ++index)
		if (root_descriptors_[index] >= 0) return false;
	return true;
}

void NativeSaveAdapter::ResetUnownedStateLocked()
{
	authority_ = nullptr;
	profile_ = nullptr;
	key_ = {};
	root_descriptor_count_ = 0;
	file_descriptor_ = -1;
	file_identity_ = {};
	file_created_ = false;
	creation_file_sync_complete_ = false;
	creation_directory_sync_complete_ = false;
	final_data_sync_complete_ = false;
	final_file_metadata_sync_complete_ = false;
	final_directory_sync_complete_ = false;
	closure_unknown_ = false;
	metadata_changed_ = false;
	root_chain_validated_ = false;
	open_failure_ = MISTER_RESULT_OK;
	for (size_t index = 0; index < kMaximumRootDescriptors; ++index) {
		root_descriptors_[index] = -1;
		root_identities_[index] = {};
		root_mount_ids_[index] = 0;
	}
}

#endif

NativeSaveCloseOutcome NativeSaveAdapter::CloseLocked(
	uint64_t absolute_deadline_ms, bool require_data_sync)
{
#if !defined(MISTER_NATIVE_SAVE_TESTING)
	(void)absolute_deadline_ms;
	(void)require_data_sync;
	return {MISTER_RESULT_UNSUPPORTED, false, false, false, false};
#else
	if (authority_ == nullptr) return {MISTER_RESULT_INVALID_STATE, false, false,
		false, closure_unknown_};
	if (closure_unknown_) return {MISTER_RESULT_CLEANUP_INCOMPLETE,
		final_data_sync_complete_, false, false, true};
	// A failed open with no retained file descriptor has no data to sync.  Close
	// every retained root prefix and preserve the original truthful failure,
	// whether the chain was partial or fully validated before the file open.
	if (open_failure_ != MISTER_RESULT_OK && file_descriptor_ < 0) {
		Result close_result = MISTER_RESULT_OK;
		for (size_t remaining = root_descriptor_count_; remaining != 0; --remaining) {
			const Result current = CloseDescriptorLocked(&root_descriptors_[remaining - 1],
				absolute_deadline_ms, false);
			if (close_result == MISTER_RESULT_OK && current != MISTER_RESULT_OK)
				close_result = current;
		}
		if (AllDescriptorsAbsentLocked()) root_descriptor_count_ = 0;
		const bool absent = AllDescriptorsAbsentLocked() && !closure_unknown_;
		if (close_result != MISTER_RESULT_OK || !absent)
			return {MISTER_RESULT_CLEANUP_INCOMPLETE, false, false, false,
				closure_unknown_};
		ResetUnownedStateLocked();
		// OpenSave already returned the original acquisition failure and lifecycle
		// has latched it. This is a separate, positive cleanup receipt: no file
		// was retained, so no file or directory synchronization is required.
		return {MISTER_RESULT_OK, false, false, true, false, false, false};
	}
	Result result = RevalidateLocked(absolute_deadline_ms, file_descriptor_ >= 0);
	if (result != MISTER_RESULT_OK) {
		// A failure after OpenSave has positively succeeded is an incomplete
		// release, not a reason to discard the retained identities.  Preserve the
		// exact descriptors and original guard for a same-registration retry; a
		// caller cannot obtain a neutral receipt from a path it could no longer
		// revalidate.  The separate open-failure paths below have never exposed a
		// save data plane and are obligated only to finish their owned cleanup.
		if (open_failure_ == MISTER_RESULT_OK)
			return {result, final_data_sync_complete_,
				(!file_created_ || creation_directory_sync_complete_) &&
				(!metadata_changed_ || (final_file_metadata_sync_complete_ &&
				final_directory_sync_complete_)), false, closure_unknown_};
		// A created inode has a durability obligation even when its retained
		// authority is no longer admissible as a save data plane. Complete its
		// independent creation barriers first, but retain every descriptor: a
		// final fdatasync and stable entry proof are still mandatory before any
		// positive cleanup receipt.  Closing here would make that proof
		// impossible on a same-registration retry.
		if (file_created_) {
			const Result barriers = CompleteCreationBarriersLocked(
				absolute_deadline_ms);
			if (barriers != MISTER_RESULT_OK)
				return {barriers, creation_file_sync_complete_,
					creation_directory_sync_complete_, false, closure_unknown_};
		}
		return {MISTER_RESULT_CLEANUP_INCOMPLETE, final_data_sync_complete_,
			false, false, closure_unknown_};
	}
	if (result == MISTER_RESULT_OK)
		result = CompleteCreationBarriersLocked(absolute_deadline_ms);
	if (result == MISTER_RESULT_OK && require_data_sync && file_descriptor_ >= 0 &&
		!final_data_sync_complete_) {
		if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			result = MISTER_RESULT_DEADLINE;
		else if (filesystem_->Fdatasync(file_descriptor_) != 0)
			result = MISTER_RESULT_PLATFORM;
		else if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
			result = MISTER_RESULT_DEADLINE;
		else final_data_sync_complete_ = true;
	}
	if (result == MISTER_RESULT_OK && file_descriptor_ >= 0) {
		result = RevalidateLocked(absolute_deadline_ms, true);
		if (result == MISTER_RESULT_OK)
			result = ObserveFinalMetadataLocked(absolute_deadline_ms);
		if (result == MISTER_RESULT_OK && metadata_changed_) {
			if (!final_file_metadata_sync_complete_) {
				if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
					result = MISTER_RESULT_DEADLINE;
				else if (filesystem_->Fsync(file_descriptor_) != 0)
					result = MISTER_RESULT_PLATFORM;
				else if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
					result = MISTER_RESULT_DEADLINE;
				else final_file_metadata_sync_complete_ = true;
			}
			if (result == MISTER_RESULT_OK && !final_directory_sync_complete_) {
				if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
					result = MISTER_RESULT_DEADLINE;
				else if (filesystem_->Fsync(
					root_descriptors_[root_descriptor_count_ - 1]) != 0)
					result = MISTER_RESULT_PLATFORM;
				else if (CheckDeadline(absolute_deadline_ms) != MISTER_RESULT_OK)
					result = MISTER_RESULT_DEADLINE;
				else final_directory_sync_complete_ = true;
			}
		}
	}
	if (result != MISTER_RESULT_OK)
		return {result, final_data_sync_complete_,
			(!file_created_ || creation_directory_sync_complete_) &&
			(!metadata_changed_ || (final_file_metadata_sync_complete_ &&
			final_directory_sync_complete_)), false, closure_unknown_};
	Result close_result = CloseDescriptorLocked(&file_descriptor_,
		absolute_deadline_ms, false);
	for (size_t remaining = root_descriptor_count_; remaining != 0; --remaining) {
		const Result current = CloseDescriptorLocked(&root_descriptors_[remaining - 1],
			absolute_deadline_ms, false);
		if (close_result == MISTER_RESULT_OK && current != MISTER_RESULT_OK)
			close_result = current;
	}
	if (AllDescriptorsAbsentLocked()) root_descriptor_count_ = 0;
	const bool metadata = (!file_created_ || creation_directory_sync_complete_) &&
		(!metadata_changed_ || (final_file_metadata_sync_complete_ &&
		final_directory_sync_complete_));
	const bool absent = AllDescriptorsAbsentLocked() && !closure_unknown_;
	if (close_result != MISTER_RESULT_OK || !absent)
		return {MISTER_RESULT_CLEANUP_INCOMPLETE, final_data_sync_complete_,
			metadata, false, closure_unknown_};
	const bool data = !require_data_sync || final_data_sync_complete_;
	ResetUnownedStateLocked();
	return {MISTER_RESULT_OK, data, metadata, true, false};
#endif
}

void NativeSaveAdapter::CloseForProcessExitLocked()
{
#if defined(MISTER_NATIVE_SAVE_TESTING)
	if (filesystem_ == nullptr) return;
	CloseDescriptorLocked(&file_descriptor_, UINT64_MAX, true);
	for (size_t remaining = root_descriptor_count_; remaining != 0; --remaining)
		CloseDescriptorLocked(&root_descriptors_[remaining - 1], UINT64_MAX, true);
	if (AllDescriptorsAbsentLocked()) root_descriptor_count_ = 0;
#endif
}

} // namespace linux_native
} // namespace native
} // namespace mister
