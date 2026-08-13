// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/native/linux/native_core_artifact_adapter.hpp"

#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace mister {
namespace native {
namespace linux_native {
namespace {

bool SameMetadata(const struct stat &left, const struct stat &right)
{
	const bool same_times =
#if defined(__APPLE__)
		left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
		left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
		left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
		left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
		left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
		left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
		left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
		left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
	return same_times && left.st_dev == right.st_dev &&
		left.st_ino == right.st_ino && left.st_mode == right.st_mode &&
		left.st_nlink == right.st_nlink && left.st_size == right.st_size;
}

bool ValidComponent(const char *text, size_t length, size_t capacity,
	bool lowercase_hex)
{
	if (text == nullptr || length == 0 || length >= capacity ||
		(length == 1 && text[0] == '.') ||
		(length == 2 && text[0] == '.' && text[1] == '.'))
		return false;
	for (size_t index = 0; index < length; ++index) {
		const unsigned char value = static_cast<unsigned char>(text[index]);
		if (value == 0 || value == '/' || value == '\\') return false;
		if (lowercase_hex) {
			if (!((value >= '0' && value <= '9') ||
				(value >= 'a' && value <= 'f'))) return false;
		} else if (!((value >= 'a' && value <= 'z') ||
			(value >= '0' && value <= '9') || value == '_' || value == '-')) {
			return false;
		}
	}
	return true;
}

bool ValidRootComponent(const char *text, size_t length)
{
	if (text == nullptr || length == 0 || length >= 256 ||
		(length == 1 && text[0] == '.') ||
		(length == 2 && text[0] == '.' && text[1] == '.'))
		return false;
	for (size_t index = 0; index < length; ++index)
		if (text[index] == '\0' || text[index] == '/' || text[index] == '\\')
			return false;
	return true;
}

void CopyComponent(char *output, const char *input, size_t length)
{
	memcpy(output, input, length);
	output[length] = '\0';
}

NativeArtifactResult CheckDeadline(NativeFileSystem &filesystem,
	uint64_t absolute_deadline_ms)
{
	return filesystem.NowMs() >= absolute_deadline_ms ?
		NativeArtifactResult::deadline : NativeArtifactResult::ok;
}

uint32_t RotateRight(uint32_t value, unsigned count)
{
	return (value >> count) | (value << (32 - count));
}

void TransformSha256(const unsigned char block[64], uint32_t state[8])
{
	static const uint32_t constants[64] = {
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
		0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
		0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
		0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
		0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
		0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
		0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
		0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
		0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
	};
	uint32_t words[64];
	for (unsigned index = 0; index < 16; ++index) {
		words[index] =
			(static_cast<uint32_t>(block[index * 4]) << 24) |
			(static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
			(static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
			static_cast<uint32_t>(block[index * 4 + 3]);
	}
	for (unsigned index = 16; index < 64; ++index) {
		const uint32_t first = RotateRight(words[index - 15], 7) ^
			RotateRight(words[index - 15], 18) ^
			(words[index - 15] >> 3);
		const uint32_t second = RotateRight(words[index - 2], 17) ^
			RotateRight(words[index - 2], 19) ^
			(words[index - 2] >> 10);
		words[index] = words[index - 16] + first + words[index - 7] + second;
	}
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	for (unsigned index = 0; index < 64; ++index) {
		const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^
			RotateRight(e, 25);
		const uint32_t choice = (e & f) ^ ((~e) & g);
		const uint32_t first = h + sum1 + choice + constants[index] +
			words[index];
		const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^
			RotateRight(a, 22);
		const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		h = g; g = f; f = e; e = d + first;
		d = c; c = b; b = a; a = first + sum0 + majority;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

NativeArtifactResult HashDescriptor(NativeFileSystem &filesystem,
	int descriptor, uint64_t expected_size, uint64_t absolute_deadline_ms,
	char output[65])
{
	if (descriptor < 0 || output == nullptr || expected_size > UINT64_MAX / 8)
		return NativeArtifactResult::invalid_argument;
	if (filesystem.NowMs() >= absolute_deadline_ms)
		return NativeArtifactResult::deadline;
	if (filesystem.Seek(descriptor, 0, SEEK_SET) != 0)
		return NativeArtifactResult::io;
	uint32_t state[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
		0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
	unsigned char block[64];
	size_t used = 0;
	uint64_t total = 0;
	for (;;) {
		if (filesystem.NowMs() >= absolute_deadline_ms)
			return NativeArtifactResult::deadline;
		const ssize_t count = filesystem.Read(descriptor, block + used,
			sizeof(block) - used);
		if (count < 0) return NativeArtifactResult::io;
		if (static_cast<size_t>(count) > sizeof(block) - used)
			return NativeArtifactResult::io;
		if (filesystem.NowMs() >= absolute_deadline_ms)
			return NativeArtifactResult::deadline;
		if (count == 0) break;
		used += static_cast<size_t>(count);
		total += static_cast<uint64_t>(count);
		if (total > expected_size) return NativeArtifactResult::changed;
		if (used == sizeof(block)) {
			TransformSha256(block, state);
			used = 0;
		}
	}
	if (total != expected_size) return NativeArtifactResult::changed;
	unsigned char tail[128] = {};
	if (used != 0) memcpy(tail, block, used);
	tail[used] = 0x80;
	const size_t final_offset = used >= 56 ? 64 : 0;
	const uint64_t bit_count = expected_size * 8;
	for (unsigned index = 0; index < 8; ++index)
		tail[final_offset + 63 - index] =
			static_cast<unsigned char>(bit_count >> (index * 8));
	TransformSha256(tail, state);
	if (final_offset != 0) TransformSha256(tail + 64, state);
	static const char hex[] = "0123456789abcdef";
	for (unsigned word = 0; word < 8; ++word) {
		for (unsigned byte = 0; byte < 4; ++byte) {
			const unsigned char value = static_cast<unsigned char>(
				state[word] >> (24 - byte * 8));
			output[(word * 4 + byte) * 2] = hex[value >> 4];
			output[(word * 4 + byte) * 2 + 1] = hex[value & 15];
		}
	}
	output[64] = '\0';
	return NativeArtifactResult::ok;
}

} // namespace

uint64_t NativePosixFileSystem::NowMs() const
{
	struct timespec now = {};
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
	return static_cast<uint64_t>(now.tv_sec) * 1000U +
		static_cast<uint64_t>(now.tv_nsec / 1000000);
}

int NativePosixFileSystem::OpenAt(int parent, const char *name, int flags,
	mode_t mode) { return openat(parent, name, flags, mode); }
int NativePosixFileSystem::StatAt(int parent, const char *name,
	struct stat *info, int flags) { return fstatat(parent, name, info, flags); }
int NativePosixFileSystem::Stat(int descriptor, struct stat *info)
{ return fstat(descriptor, info); }
off_t NativePosixFileSystem::Seek(int descriptor, off_t offset, int whence)
{ return lseek(descriptor, offset, whence); }
ssize_t NativePosixFileSystem::Read(int descriptor, void *bytes, size_t count)
{ return read(descriptor, bytes, count); }
ssize_t NativePosixFileSystem::ReadAt(int descriptor, void *bytes,
	size_t count, off_t offset) { return pread(descriptor, bytes, count, offset); }
int NativePosixFileSystem::Close(int descriptor) { return close(descriptor); }

NativeArtifactResult NativeSha256DescriptorForTest(NativeFileSystem &filesystem,
	int descriptor, uint64_t expected_size, char output[65])
{
	return HashDescriptor(filesystem, descriptor, expected_size, UINT64_MAX,
		output);
}

const char *NativeArtifactResultName(NativeArtifactResult result)
{
	switch (result) {
	case NativeArtifactResult::ok: return "ok";
	case NativeArtifactResult::invalid_argument: return "invalid_argument";
	case NativeArtifactResult::invalid_identity: return "invalid_identity";
	case NativeArtifactResult::insecure: return "insecure";
	case NativeArtifactResult::not_found: return "not_found";
	case NativeArtifactResult::changed: return "changed";
	case NativeArtifactResult::digest_mismatch: return "digest_mismatch";
	case NativeArtifactResult::deadline: return "deadline";
	case NativeArtifactResult::cleanup_incomplete: return "cleanup_incomplete";
	case NativeArtifactResult::io:
	default: return "io";
	}
}

NativeHeldFile::NativeHeldFile()
	: filesystem_(nullptr), root_descriptors_(), root_identities_(),
	  root_components_(), root_descriptor_count_(0), directory_descriptor_(-1),
	  file_descriptor_(-1), size_(0), verified_(false), bound_profile_(nullptr),
	  directory_name_(), file_name_(),
	  directory_identity_(), file_identity_(), closure_unknown_(false)
{
	for (size_t index = 0; index < kMaximumRootComponents; ++index)
		root_descriptors_[index] = -1;
}

NativeHeldFile::~NativeHeldFile()
{
	Close();
}

bool NativeHeldFile::valid() const { return verified_ && file_descriptor_ >= 0; }
bool NativeHeldFile::HasLiveDescriptors() const
{
	return root_descriptor_count_ != 0 || directory_descriptor_ >= 0 ||
		file_descriptor_ >= 0;
}
bool NativeHeldFile::owns_descriptors() const
{
	return HasLiveDescriptors() || closure_unknown_;
}
bool NativeHeldFile::closure_unknown() const { return closure_unknown_; }
uint64_t NativeHeldFile::size() const { return size_; }

NativeArtifactResult NativeHeldFile::Close()
{
	if (filesystem_ == nullptr) return closure_unknown_ || HasLiveDescriptors() ?
		NativeArtifactResult::cleanup_incomplete : NativeArtifactResult::ok;
	NativeFileSystem *const filesystem = filesystem_;
	int *const descriptors[] = {&file_descriptor_, &directory_descriptor_};
	for (size_t index = 0; index < sizeof(descriptors) / sizeof(descriptors[0]);
		++index) {
		int &descriptor = *descriptors[index];
		if (descriptor < 0) continue;
		const int retired = descriptor;
		descriptor = -1;
		if (index == 0) {
			size_ = 0;
			verified_ = false;
			bound_profile_ = nullptr;
		}
		if (filesystem->Close(retired) != 0) closure_unknown_ = true;
	}
	for (size_t remaining = root_descriptor_count_; remaining != 0;
		--remaining) {
		int &descriptor = root_descriptors_[remaining - 1];
		if (descriptor < 0) continue;
		const int retired = descriptor;
		descriptor = -1;
		if (filesystem->Close(retired) != 0) closure_unknown_ = true;
	}
	while (root_descriptor_count_ != 0 &&
		root_descriptors_[root_descriptor_count_ - 1] < 0)
		--root_descriptor_count_;
	if (!HasLiveDescriptors()) {
		filesystem_ = nullptr;
		size_ = 0;
		verified_ = false;
		bound_profile_ = nullptr;
		memset(root_identities_, 0, sizeof(root_identities_));
		memset(root_components_, 0, sizeof(root_components_));
		memset(directory_name_, 0, sizeof(directory_name_));
		memset(file_name_, 0, sizeof(file_name_));
		memset(&directory_identity_, 0, sizeof(directory_identity_));
		memset(&file_identity_, 0, sizeof(file_identity_));
	}
	return closure_unknown_ ? NativeArtifactResult::cleanup_incomplete :
		NativeArtifactResult::ok;
}

NativeArtifactResult NativeHeldFile::CloseBefore(
	uint64_t absolute_deadline_ms)
{
	if (filesystem_ == nullptr) return closure_unknown_ || HasLiveDescriptors() ?
		NativeArtifactResult::cleanup_incomplete : NativeArtifactResult::ok;
	NativeFileSystem *const filesystem = filesystem_;
	bool expired = false;
	int *const descriptors[] = {&file_descriptor_, &directory_descriptor_};
	for (size_t index = 0; index < sizeof(descriptors) / sizeof(descriptors[0]);
		++index) {
		int &descriptor = *descriptors[index];
		if (descriptor < 0) continue;
		if (filesystem->NowMs() >= absolute_deadline_ms) {
			expired = true;
			break;
		}
		const int retired = descriptor;
		descriptor = -1;
		if (index == 0) {
			size_ = 0;
			verified_ = false;
			bound_profile_ = nullptr;
		}
		if (filesystem->Close(retired) != 0) closure_unknown_ = true;
		if (filesystem->NowMs() >= absolute_deadline_ms) {
			expired = true;
			break;
		}
	}
	if (!expired) {
		for (size_t remaining = root_descriptor_count_; remaining != 0;
			--remaining) {
			int &descriptor = root_descriptors_[remaining - 1];
			if (descriptor < 0) continue;
			if (filesystem->NowMs() >= absolute_deadline_ms) {
				expired = true;
				break;
			}
			const int retired = descriptor;
			descriptor = -1;
			if (filesystem->Close(retired) != 0) closure_unknown_ = true;
			if (filesystem->NowMs() >= absolute_deadline_ms) {
				expired = true;
				break;
			}
		}
	}
	while (root_descriptor_count_ != 0 &&
		root_descriptors_[root_descriptor_count_ - 1] < 0)
		--root_descriptor_count_;
	if (!HasLiveDescriptors()) {
		filesystem_ = nullptr;
		size_ = 0;
		verified_ = false;
		bound_profile_ = nullptr;
		memset(root_identities_, 0, sizeof(root_identities_));
		memset(root_components_, 0, sizeof(root_components_));
		memset(directory_name_, 0, sizeof(directory_name_));
		memset(file_name_, 0, sizeof(file_name_));
		memset(&directory_identity_, 0, sizeof(directory_identity_));
		memset(&file_identity_, 0, sizeof(file_identity_));
	}
	if (closure_unknown_) return NativeArtifactResult::cleanup_incomplete;
	if (expired) return NativeArtifactResult::deadline;
	return NativeArtifactResult::ok;
}

NativeArtifactResult NativeCoreArtifactHandle::CloseRetainedBefore(
	uint64_t absolute_deadline_ms)
{
	return CloseBefore(absolute_deadline_ms);
}

NativeArtifactResult NativeHeldFile::Revalidate(
	uint64_t absolute_deadline_ms) const
{
	if (filesystem_ == nullptr || file_descriptor_ < 0)
		return NativeArtifactResult::invalid_argument;
	if (CheckDeadline(*filesystem_, absolute_deadline_ms) !=
		NativeArtifactResult::ok) return NativeArtifactResult::deadline;
	struct stat root_path = {}, root_open = {}, directory_path = {};
	struct stat directory_open = {}, file_path = {}, file_open = {};
	if (root_descriptor_count_ == 0 ||
		filesystem_->Stat(root_descriptors_[0], &root_open) != 0 ||
		!SameMetadata(root_identities_[0], root_open))
		return NativeArtifactResult::changed;
	for (size_t index = 1; index < root_descriptor_count_; ++index) {
		if (filesystem_->StatAt(root_descriptors_[index - 1],
			root_components_[index], &root_path, AT_SYMLINK_NOFOLLOW) != 0 ||
			filesystem_->Stat(root_descriptors_[index], &root_open) != 0 ||
			!SameMetadata(root_identities_[index], root_path) ||
			!SameMetadata(root_identities_[index], root_open))
			return NativeArtifactResult::changed;
	}
	const int root_descriptor = root_descriptors_[root_descriptor_count_ - 1];
	if (filesystem_->StatAt(root_descriptor, directory_name_, &directory_path,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		filesystem_->Stat(directory_descriptor_, &directory_open) != 0 ||
		filesystem_->StatAt(directory_descriptor_, file_name_, &file_path,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		filesystem_->Stat(file_descriptor_, &file_open) != 0)
		return NativeArtifactResult::changed;
	if (!SameMetadata(directory_identity_, directory_path) ||
		!SameMetadata(directory_identity_, directory_open) ||
		!SameMetadata(file_identity_, file_path) ||
		!SameMetadata(file_identity_, file_open))
		return NativeArtifactResult::changed;
	return CheckDeadline(*filesystem_, absolute_deadline_ms);
}

NativeArtifactResult NativeHeldFile::RewindAndRevalidate(
	uint64_t absolute_deadline_ms) const
{
	NativeArtifactResult result = Revalidate(absolute_deadline_ms);
	if (result != NativeArtifactResult::ok) return result;
	if (filesystem_->Seek(file_descriptor_, 0, SEEK_SET) != 0)
		return NativeArtifactResult::io;
	return CheckDeadline(*filesystem_, absolute_deadline_ms);
}

NativeArtifactResult NativeHeldFile::ReadForUse(void *bytes, size_t count,
	ssize_t *read_count, uint64_t absolute_deadline_ms) const
{
	if (read_count == nullptr || (count != 0 && bytes == nullptr))
		return NativeArtifactResult::invalid_argument;
	if (CheckDeadline(*filesystem_, absolute_deadline_ms) !=
		NativeArtifactResult::ok) return NativeArtifactResult::deadline;
	*read_count = filesystem_->Read(file_descriptor_, bytes, count);
	if (*read_count < 0) return NativeArtifactResult::io;
	return CheckDeadline(*filesystem_, absolute_deadline_ms);
}

NativeArtifactResult NativeHeldFile::ReadAtForUse(uint64_t offset,
	void *bytes, size_t count, uint64_t absolute_deadline_ms) const
{
	if ((count != 0 && bytes == nullptr) || offset > size_ ||
		static_cast<uint64_t>(count) > size_ - offset)
		return NativeArtifactResult::invalid_argument;
	NativeArtifactResult result = Revalidate(absolute_deadline_ms);
	if (result != NativeArtifactResult::ok) return result;
	size_t completed = 0;
	while (completed != count) {
		if (CheckDeadline(*filesystem_, absolute_deadline_ms) !=
			NativeArtifactResult::ok) return NativeArtifactResult::deadline;
		const ssize_t read_count = filesystem_->ReadAt(file_descriptor_,
			static_cast<unsigned char *>(bytes) + completed, count - completed,
			static_cast<off_t>(offset + completed));
		if (read_count <= 0 ||
			static_cast<size_t>(read_count) > count - completed)
			return NativeArtifactResult::io;
		completed += static_cast<size_t>(read_count);
	}
	return Revalidate(absolute_deadline_ms);
}

NativeCoreArtifactAdapter::NativeCoreArtifactAdapter(const char *root,
	NativeFileSystem &filesystem) : root_(), root_valid_(false),
	filesystem_(filesystem)
{
	if (root == nullptr) return;
	const size_t length = strlen(root);
	if (length == 0 || length >= sizeof(root_)) return;
	memcpy(root_, root, length + 1);
	root_valid_ = true;
}

NativeArtifactResult NativeCoreArtifactAdapter::Resolve(
	const NativeArtifactAuthority &authority, uint64_t absolute_deadline_ms,
	NativeCoreArtifactHandle *artifact)
{
	return ResolveHeld(authority, absolute_deadline_ms, artifact);
}

NativeArtifactResult NativeCoreArtifactAdapter::ResolveHeld(
	const NativeArtifactAuthority &authority, uint64_t absolute_deadline_ms,
	NativeHeldFile *artifact)
{
	if (artifact == nullptr || artifact->owns_descriptors() || !root_valid_ ||
		root_[0] != '/')
		return NativeArtifactResult::invalid_argument;
	if (!ValidComponent(authority.system, authority.system_length,
		sizeof(artifact->directory_name_), false) ||
		!ValidComponent(authority.sha256, authority.sha256_length, 65, true) ||
		authority.sha256_length != 64 || authority.size == 0 ||
		authority.size > UINT64_MAX / 8 ||
		!ValidComponent(authority.extension, authority.extension_length, 16,
			false))
		return NativeArtifactResult::invalid_identity;
	if (CheckDeadline(filesystem_, absolute_deadline_ms) !=
		NativeArtifactResult::ok) return NativeArtifactResult::deadline;

	artifact->filesystem_ = &filesystem_;
	artifact->size_ = authority.size;
	artifact->verified_ = false;
	artifact->bound_profile_ = nullptr;
	CopyComponent(artifact->directory_name_, authority.system,
		authority.system_length);
	CopyComponent(artifact->file_name_, authority.sha256,
		authority.sha256_length);
	artifact->file_name_[authority.sha256_length] = '.';
	CopyComponent(artifact->file_name_ + authority.sha256_length + 1,
		authority.extension, authority.extension_length);

	struct stat path_info = {}, open_info = {};
	int root_descriptor = filesystem_.OpenAt(AT_FDCWD, "/",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
	if (root_descriptor < 0) return NativeArtifactResult::io;
	artifact->root_descriptors_[0] = root_descriptor;
	artifact->root_descriptor_count_ = 1;
	if (filesystem_.Stat(root_descriptor, &open_info) != 0 ||
		!S_ISDIR(open_info.st_mode)) return NativeArtifactResult::changed;
	artifact->root_identities_[0] = open_info;
	const size_t root_length = strlen(root_);
	size_t cursor = 1;
	while (cursor < root_length) {
		while (cursor < root_length && root_[cursor] == '/') ++cursor;
		if (cursor == root_length) break;
		const size_t begin = cursor;
		while (cursor < root_length && root_[cursor] != '/') ++cursor;
		const size_t length = cursor - begin;
		if (!ValidRootComponent(root_ + begin, length) ||
			artifact->root_descriptor_count_ >=
				NativeHeldFile::kMaximumRootComponents)
			return NativeArtifactResult::insecure;
		const size_t index = artifact->root_descriptor_count_;
		CopyComponent(artifact->root_components_[index], root_ + begin, length);
		if (filesystem_.StatAt(root_descriptor,
			artifact->root_components_[index], &path_info,
			AT_SYMLINK_NOFOLLOW) != 0) return NativeArtifactResult::not_found;
		if (!S_ISDIR(path_info.st_mode)) return NativeArtifactResult::insecure;
		const int opened = filesystem_.OpenAt(root_descriptor,
			artifact->root_components_[index],
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
		if (opened < 0) return NativeArtifactResult::insecure;
		artifact->root_descriptors_[index] = opened;
		++artifact->root_descriptor_count_;
		if (filesystem_.Stat(opened, &open_info) != 0 ||
			!SameMetadata(path_info, open_info))
			return NativeArtifactResult::changed;
		artifact->root_identities_[index] = open_info;
		root_descriptor = opened;
	}
	if (CheckDeadline(filesystem_, absolute_deadline_ms) !=
		NativeArtifactResult::ok) return NativeArtifactResult::deadline;

	if (filesystem_.StatAt(root_descriptor,
		artifact->directory_name_, &path_info, AT_SYMLINK_NOFOLLOW) != 0)
		return NativeArtifactResult::not_found;
	if (!S_ISDIR(path_info.st_mode)) return NativeArtifactResult::insecure;
	artifact->directory_descriptor_ = filesystem_.OpenAt(
		root_descriptor, artifact->directory_name_,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
	if (artifact->directory_descriptor_ < 0) return NativeArtifactResult::io;
	if (filesystem_.Stat(artifact->directory_descriptor_, &open_info) != 0 ||
		!SameMetadata(path_info, open_info)) return NativeArtifactResult::changed;
	artifact->directory_identity_ = open_info;

	if (filesystem_.StatAt(artifact->directory_descriptor_,
		artifact->file_name_, &path_info, AT_SYMLINK_NOFOLLOW) != 0)
		return NativeArtifactResult::not_found;
	if (!S_ISREG(path_info.st_mode) || path_info.st_nlink != 1)
		return NativeArtifactResult::insecure;
	if (path_info.st_size < 0 ||
		static_cast<uint64_t>(path_info.st_size) != authority.size)
		return NativeArtifactResult::changed;
	artifact->file_descriptor_ = filesystem_.OpenAt(
		artifact->directory_descriptor_, artifact->file_name_,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
	if (artifact->file_descriptor_ < 0) return NativeArtifactResult::io;
	if (filesystem_.Stat(artifact->file_descriptor_, &open_info) != 0 ||
		!SameMetadata(path_info, open_info)) return NativeArtifactResult::changed;
	artifact->file_identity_ = open_info;

	NativeArtifactResult result = artifact->Revalidate(absolute_deadline_ms);
	char digest[65] = {};
	if (result == NativeArtifactResult::ok)
		result = HashDescriptor(filesystem_, artifact->file_descriptor_,
			authority.size, absolute_deadline_ms, digest);
	if (result == NativeArtifactResult::ok)
		result = artifact->Revalidate(absolute_deadline_ms);
	if (result == NativeArtifactResult::ok &&
		memcmp(digest, authority.sha256, 64) != 0)
		result = NativeArtifactResult::digest_mismatch;
	if (result == NativeArtifactResult::ok) artifact->verified_ = true;
	return result;
}

NativeArtifactResult NativeCoreArtifactAdapter::Resolve(
	const NativeCoreProfile &profile, uint64_t absolute_deadline_ms,
	NativeCoreArtifactHandle *artifact)
{
	if (!ValidateNativeCoreProfileRecord(profile))
		return NativeArtifactResult::invalid_identity;
	const NativeArtifactAuthority authority = {profile.system,
		strlen(profile.system), profile.artifact.sha256,
		strlen(profile.artifact.sha256), profile.artifact.size, "rbf", 3};
	const NativeArtifactResult result = Resolve(authority, absolute_deadline_ms,
		artifact);
	if (result == NativeArtifactResult::ok) artifact->bound_profile_ = &profile;
	return result;
}

#if defined(MISTER_NATIVE_PROFILE_TESTING)
NativeArtifactResult NativeCoreArtifactAdapter::ResolveFixtureForTest(
	const NativeCoreProfile &profile, const NativeArtifactAuthority &authority,
	uint64_t absolute_deadline_ms, NativeCoreArtifactHandle *artifact)
{
	if (!IsExactFixtureNativeCoreProfile(profile) || authority.system == nullptr ||
		authority.system_length != strlen(profile.system) ||
		memcmp(authority.system, profile.system, authority.system_length) != 0)
		return NativeArtifactResult::invalid_identity;
	const NativeArtifactResult result = Resolve(authority, absolute_deadline_ms,
		artifact);
	if (result == NativeArtifactResult::ok) artifact->bound_profile_ = &profile;
	return result;
}
#endif

} // namespace linux_native
} // namespace native
} // namespace mister
