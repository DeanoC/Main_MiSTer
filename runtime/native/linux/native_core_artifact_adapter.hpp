// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_CORE_ARTIFACT_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_CORE_ARTIFACT_ADAPTER_HPP

#include "runtime/native/native_core_profile.hpp"

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace mister {
namespace native {
namespace linux_native {

enum class NativeArtifactResult : uint8_t {
	ok,
	invalid_argument,
	invalid_identity,
	insecure,
	not_found,
	changed,
	digest_mismatch,
	deadline,
	io,
	cleanup_incomplete
};

const char *NativeArtifactResultName(NativeArtifactResult result);

class NativeFileSystem {
public:
	virtual ~NativeFileSystem() {}
	virtual uint64_t NowMs() const = 0;
	virtual int OpenAt(int parent, const char *name, int flags,
		mode_t mode) = 0;
	virtual int StatAt(int parent, const char *name, struct stat *info,
		int flags) = 0;
	virtual int Stat(int descriptor, struct stat *info) = 0;
	virtual off_t Seek(int descriptor, off_t offset, int whence) = 0;
	virtual ssize_t Read(int descriptor, void *bytes, size_t count) = 0;
	virtual ssize_t ReadAt(int descriptor, void *bytes, size_t count,
		off_t offset) = 0;
	virtual int Close(int descriptor) = 0;
};

class NativePosixFileSystem final : public NativeFileSystem {
public:
	uint64_t NowMs() const override;
	int OpenAt(int parent, const char *name, int flags, mode_t mode) override;
	int StatAt(int parent, const char *name, struct stat *info,
		int flags) override;
	int Stat(int descriptor, struct stat *info) override;
	off_t Seek(int descriptor, off_t offset, int whence) override;
	ssize_t Read(int descriptor, void *bytes, size_t count) override;
	ssize_t ReadAt(int descriptor, void *bytes, size_t count,
		off_t offset) override;
	int Close(int descriptor) override;
};

// Exposed only so the same in-tree implementation can be checked against
// standard known-answer vectors. Production resolution invokes it on the
// already-opened descriptor and never reopens by path.
NativeArtifactResult NativeSha256DescriptorForTest(NativeFileSystem &filesystem,
	int descriptor, uint64_t expected_size, char output[65]);

struct NativeArtifactAuthority {
	const char *system;
	size_t system_length;
	const char *sha256;
	size_t sha256_length;
	uint64_t size;
	const char *extension;
	size_t extension_length;
};

class NativeHeldFile {
public:
	NativeHeldFile();
	virtual ~NativeHeldFile();
	NativeHeldFile(const NativeHeldFile &) = delete;
	NativeHeldFile &operator=(const NativeHeldFile &) = delete;

	bool valid() const;
	bool owns_descriptors() const;
	bool closure_unknown() const;
	uint64_t size() const;
	NativeArtifactResult Close();

protected:
	friend class NativeCoreArtifactAdapter;
	friend class NativeContentAdapter;
	friend class NativeFpgaProgrammer;
	NativeArtifactResult Revalidate(uint64_t absolute_deadline_ms) const;
	NativeArtifactResult RewindAndRevalidate(uint64_t absolute_deadline_ms) const;
	NativeArtifactResult ReadForUse(void *bytes, size_t count,
		ssize_t *read_count, uint64_t absolute_deadline_ms) const;
	NativeArtifactResult ReadAtForUse(uint64_t offset, void *bytes,
		size_t count, uint64_t absolute_deadline_ms) const;
	NativeArtifactResult CloseBefore(uint64_t absolute_deadline_ms);

	NativeFileSystem *filesystem_;
	static const size_t kMaximumRootComponents = 64;
	int root_descriptors_[kMaximumRootComponents];
	struct stat root_identities_[kMaximumRootComponents];
	char root_components_[kMaximumRootComponents][256];
	size_t root_descriptor_count_;
	int directory_descriptor_;
	int file_descriptor_;
	uint64_t size_;
	bool verified_;
	const NativeCoreProfile *bound_profile_;
	char directory_name_[64];
	char file_name_[96];
	struct stat directory_identity_;
	struct stat file_identity_;

private:
	bool HasLiveDescriptors() const;
	bool closure_unknown_;
};

class NativeCoreArtifactHandle final : public NativeHeldFile {
public:
	NativeCoreArtifactHandle() {}
	NativeArtifactResult CloseRetainedBefore(uint64_t absolute_deadline_ms);

private:
	friend class NativeFpgaProgrammer;
	NativeArtifactResult PrepareVerifiedProgrammingRead(
		uint64_t absolute_deadline_ms) const;
	NativeArtifactResult RevalidateProgrammedIdentity(
		uint64_t absolute_deadline_ms) const;
};

class NativeCoreArtifactAdapter final {
public:
	NativeCoreArtifactAdapter(const char *root, NativeFileSystem &filesystem);
	NativeArtifactResult Resolve(const NativeArtifactAuthority &authority,
		uint64_t absolute_deadline_ms, NativeCoreArtifactHandle *artifact);
	NativeArtifactResult Resolve(const NativeCoreProfile &profile,
		uint64_t absolute_deadline_ms, NativeCoreArtifactHandle *artifact);
#if defined(MISTER_NATIVE_PROFILE_TESTING)
	NativeArtifactResult ResolveFixtureForTest(const NativeCoreProfile &profile,
		const NativeArtifactAuthority &authority,
		uint64_t absolute_deadline_ms, NativeCoreArtifactHandle *artifact);
#endif

private:
	friend class NativeContentAdapter;
	NativeArtifactResult ResolveHeld(const NativeArtifactAuthority &authority,
		uint64_t absolute_deadline_ms, NativeHeldFile *artifact);
	char root_[4096];
	bool root_valid_;
	NativeFileSystem &filesystem_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
