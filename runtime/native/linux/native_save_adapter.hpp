// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_SAVE_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_SAVE_ADAPTER_HPP

#include "runtime/native/native_resources.hpp"

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <mutex>

namespace mister {
namespace native {
namespace linux_native {

#if defined(MISTER_NATIVE_SAVE_TESTING)

struct NativeSaveOpenResult {
	int descriptor;
	int error;
};

class NativeSaveFileSystem {
public:
	virtual ~NativeSaveFileSystem() {}
	virtual uint64_t NowMs() const = 0;
	virtual NativeSaveOpenResult OpenAt(int parent, const char *name,
		int flags, mode_t mode) = 0;
	virtual int Stat(int descriptor, struct stat *info) = 0;
	virtual int StatAt(int parent, const char *name, struct stat *info,
		int flags) = 0;
	// Mount identity is observable only through the private fixture boundary.
	// Unsupported models a platform without STATX_MNT_ID support.
	virtual Result MountId(int descriptor, uint64_t *mount_id) = 0;
	virtual int Fdatasync(int descriptor) = 0;
	virtual int Fsync(int descriptor) = 0;
	virtual int Close(int descriptor) = 0;
};

#endif

class NativeSaveAdapter final : public NativeSaveResource {
public:
#if defined(MISTER_NATIVE_SAVE_TESTING)
	NativeSaveAdapter(HardwareBroker &broker, NativeSaveFileSystem &filesystem);
#else
	explicit NativeSaveAdapter(HardwareBroker &broker);
#endif
	~NativeSaveAdapter() override;
	NativeSaveAdapter(const NativeSaveAdapter &) = delete;
	NativeSaveAdapter &operator=(const NativeSaveAdapter &) = delete;

	NativeSaveOpenOutcome OpenSave(const OperationLease &lease,
		const NativeCoreProfile &profile, const NativeSaveKey &key) override;
	NativeSaveCloseOutcome FlushAndCloseSave(
		const OperationLease &lease) override;
	NativeSaveCloseOutcome RecoverSave(const OperationLease &lease,
		const SafeSaveRecoveryRecord &record) override;
	void CloseSaveForProcessExit() override;

private:
	static const size_t kMaximumRootDescriptors = 64;
	NativeSaveCloseOutcome CloseLocked(uint64_t absolute_deadline_ms,
		bool require_data_sync);
	void CloseForProcessExitLocked();
#if defined(MISTER_NATIVE_SAVE_TESTING)
	bool ValidateAuthority(const NativeCoreProfile &profile,
		const NativeSaveKey &key) const;
	Result CheckDeadline(uint64_t absolute_deadline_ms) const;
	Result OpenRootChainLocked(uint64_t absolute_deadline_ms);
	Result OpenFileLocked(uint64_t absolute_deadline_ms, bool allow_creation);
	Result RevalidateLocked(uint64_t absolute_deadline_ms,
		bool include_file) const;
	Result ObserveFinalMetadataLocked(uint64_t absolute_deadline_ms);
	Result CompleteCreationBarriersLocked(uint64_t absolute_deadline_ms);
	Result CloseDescriptorLocked(int *descriptor,
		uint64_t absolute_deadline_ms, bool process_exit);
	bool AllDescriptorsAbsentLocked() const;
	void ResetUnownedStateLocked();
#endif

	HardwareBroker &broker_;
	std::mutex mutex_;
#if defined(MISTER_NATIVE_SAVE_TESTING)
	NativeSaveFileSystem *filesystem_;
	const NativeSaveRootAuthority *authority_;
	const NativeSaveProfile *profile_;
	NativeSaveKey key_;
	int root_descriptors_[kMaximumRootDescriptors];
	struct stat root_identities_[kMaximumRootDescriptors];
	uint64_t root_mount_ids_[kMaximumRootDescriptors];
	size_t root_descriptor_count_;
	int file_descriptor_;
	struct stat file_identity_;
	bool file_created_;
	bool creation_file_sync_complete_;
	bool creation_directory_sync_complete_;
	bool final_data_sync_complete_;
	bool final_file_metadata_sync_complete_;
	bool final_directory_sync_complete_;
	bool closure_unknown_;
	bool metadata_changed_;
	bool root_chain_validated_;
	Result open_failure_;
#endif
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
