// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_CONTENT_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_CONTENT_ADAPTER_HPP

#include "runtime/native/linux/native_core_artifact_adapter.hpp"
#include "runtime/native/native_resources.hpp"

namespace mister {
namespace native {
namespace linux_native {

class NativeContentHandle final : public NativeHeldFile {
public:
	NativeContentHandle() {}
};

class NativeContentAdapter final : public NativeContentResource {
public:
	NativeContentAdapter(const char *root, NativeFileSystem &filesystem);
	~NativeContentAdapter() override;
	Result Configure(const NativeArtifactAuthority &authority);
	Result DeriveSaveKey(const NativeCoreProfile &profile,
		NativeSaveKey *key) override;
	NativeAcquisitionOutcome RetainContent(
		uint64_t absolute_deadline_ms) override;
	Result DescribeRetained(NativeRetainedContentDescription *description,
		uint64_t absolute_deadline_ms) override;
	Result ReadRetainedAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms) override;
	Result ReadAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms);
	Result CloseContent(uint64_t absolute_deadline_ms) override;
	void CloseContentForProcessExit() override;
	bool active() const;
	bool owns_descriptors() const;

private:
	char root_[4096];
	bool root_valid_;
	NativeFileSystem &filesystem_;
	NativeContentHandle content_;
	NativeArtifactAuthority authority_;
	char system_[64];
	char digest_[65];
	char extension_[16];
	bool configured_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
