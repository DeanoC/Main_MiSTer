// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_LINUX_V2_HPP
#define MISTER_RUNTIME_LINUX_V2_HPP

#include "runtime/mister_runtime.h"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace linux_v2 {

const char* ProductionCacheRoot();

enum class StorageResult : uint32_t {
	ok = 0,
	invalid_argument = 1,
	invalid_identity = 2,
	unsupported_identity = 3,
	insecure_root = 4,
	insecure_entry = 5,
	not_found = 6,
	changed = 7,
	digest_mismatch = 8,
	io = 9
};

const char* StorageResultName(StorageResult result);

enum class CoreArtifactResult : uint32_t {
	ok = 0,
	invalid_argument = 1,
	invalid_identity = 2,
	unavailable = 3
};

const char* CoreArtifactResultName(CoreArtifactResult result);

struct CoreArtifactAuthority {
	MisterStringView system;
	MisterStringView expected_core;
	MisterStringView sha256;
	uint64_t size;
};

class CoreArtifactHandle {
public:
	CoreArtifactHandle();
	~CoreArtifactHandle();
	CoreArtifactHandle(const CoreArtifactHandle&) = delete;
	CoreArtifactHandle& operator=(const CoreArtifactHandle&) = delete;
	bool valid() const;
	void Close();

private:
	int descriptor_;
};

class CoreArtifactResolver {
public:
	virtual ~CoreArtifactResolver() {}
	virtual CoreArtifactResult Resolve(const CoreArtifactAuthority& authority,
		CoreArtifactHandle* artifact) = 0;
};

class UnavailableCoreArtifactResolver : public CoreArtifactResolver {
public:
	CoreArtifactResult Resolve(const CoreArtifactAuthority& authority,
		CoreArtifactHandle* artifact) override;
};

class ContentHandle {
public:
	ContentHandle();
	~ContentHandle();
	ContentHandle(const ContentHandle&) = delete;
	ContentHandle& operator=(const ContentHandle&) = delete;
	bool valid() const;
	uint64_t size() const;
	bool ReadAt(uint64_t offset, void* bytes, size_t count) const;
	void Close();

private:
	friend class StorageAdapter;
	int descriptor_;
	uint64_t size_;
};

class StorageValidationObserver {
public:
	virtual ~StorageValidationObserver() {}
	virtual void AfterHash() = 0;
};

class StorageAdapter {
public:
	explicit StorageAdapter(const char* root, StorageValidationObserver* observer = nullptr);
	~StorageAdapter();
	StorageAdapter(const StorageAdapter&) = delete;
	StorageAdapter& operator=(const StorageAdapter&) = delete;
	StorageResult status() const;
	StorageResult Resolve(const MisterLaunchV2& launch, ContentHandle* content);

private:
	StorageResult CheckDirectories() const;
	int root_descriptor_;
	int megadrive_descriptor_;
	int snes_descriptor_;
	StorageResult status_;
	char root_[4096];
	uint64_t root_device_;
	uint64_t root_inode_;
	uint64_t megadrive_device_;
	uint64_t megadrive_inode_;
	uint64_t snes_device_;
	uint64_t snes_inode_;
	StorageValidationObserver* observer_;
};

enum class TeardownAction : uint32_t {
	none = 0,
	scheduler,
	offload,
	input,
	audio,
	saves,
	video,
	content,
	spi,
	fpga_reset,
	bridges,
	mapping
};

class ResourceOperations {
public:
	virtual ~ResourceOperations() {}
	virtual uint64_t NowMs() = 0;
	virtual MisterResult Run(TeardownAction action, uint32_t deadline_ms) = 0;
	virtual MisterResult ObserveNeutral(TeardownAction action, uint32_t deadline_ms,
		bool* neutral) = 0;
};

class ResourceTransaction {
public:
	explicit ResourceTransaction(uint32_t acquired_mask);
	uint32_t acquired_mask() const;
	uint32_t active_mask() const;
	uint32_t neutral_mask() const;
	MisterResult Acquire(uint32_t resource_mask);
	MisterResult AcquireSupporting(TeardownAction action);
	MisterResult Stop(ResourceOperations& operations, uint32_t deadline_ms);

private:
	bool ActionRequired(TeardownAction action) const;
	bool CleanupComplete() const;
	uint32_t acquired_mask_;
	uint32_t neutral_mask_;
	uint32_t supporting_acquired_mask_;
	uint32_t supporting_neutral_mask_;
	uint32_t next_action_;
	bool cleanup_started_set_;
	uint64_t cleanup_started_;
	uint64_t short_deadline_;
	uint64_t fpga_deadline_;
};

class LinuxV2Context {
public:
	explicit LinuxV2Context(const char* cache_root);
	StorageResult storage_status() const;
	StorageResult PrepareContent(const MisterLaunchV2& launch);
	bool content_active() const;
	bool ReadContentAt(uint64_t offset, void* bytes, size_t count) const;
	uint32_t acquired_mask() const;
	uint32_t neutral_mask() const;

private:
	StorageAdapter storage_;
	ContentHandle content_;
	ResourceTransaction resources_;
};

}  // namespace linux_v2
}  // namespace mister

#endif
