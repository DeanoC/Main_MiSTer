// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/mister_runtime_linux_v2.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

namespace mister {
namespace linux_v2 {
namespace {

struct Identity {
	const char* system;
	const char* core;
	const char* extension;
};

const Identity kIdentities[] = {
	{"megadrive", "MegaDrive", "md"},
	{"megadrive", "MegaDrive", "gen"},
	{"megadrive", "MegaDrive", "bin"},
	{"snes", "SNES", "sfc"},
	{"snes", "SNES", "smc"},
	{"snes", "SNES", "bin"}
};

const TeardownAction kActions[] = {
	TeardownAction::scheduler, TeardownAction::offload, TeardownAction::input,
	TeardownAction::audio, TeardownAction::saves, TeardownAction::video,
	TeardownAction::content, TeardownAction::spi, TeardownAction::fpga_reset,
	TeardownAction::bridges, TeardownAction::mapping
};

bool ViewEquals(MisterStringView view, const char* text) {
	const size_t size = strlen(text);
	return view.data != nullptr && view.length == size && memcmp(view.data, text, size) == 0;
}

bool LowerHex(MisterStringView view) {
	if (view.data == nullptr || view.length != 64) return false;
	for (uint32_t i = 0; i < view.length; ++i) {
		if (!((view.data[i] >= '0' && view.data[i] <= '9') ||
			(view.data[i] >= 'a' && view.data[i] <= 'f'))) return false;
	}
	return true;
}

bool EntryComponent(MisterStringView view) {
	if (view.data == nullptr || view.length == 0 || view.length > 16) return false;
	for (uint32_t i = 0; i < view.length; ++i) {
		if (!((view.data[i] >= 'a' && view.data[i] <= 'z') ||
			(view.data[i] >= '0' && view.data[i] <= '9'))) return false;
	}
	return true;
}

bool SameObject(const struct stat& left, const struct stat& right) {
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
	return same_times && left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
		left.st_mode == right.st_mode && left.st_nlink == right.st_nlink &&
		left.st_size == right.st_size;
}

bool BoundDirectory(const struct stat& info) {
	return S_ISDIR(info.st_mode);
}

StorageResult OpenDirectoryAt(int parent, const char* name, int* descriptor,
	uint64_t* device, uint64_t* inode) {
	struct stat path_info;
	if (fstatat(parent, name, &path_info, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno == ENOENT ? StorageResult::not_found : StorageResult::io;
	}
	if (!BoundDirectory(path_info)) return StorageResult::insecure_root;
	const int opened = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (opened < 0) return errno == ELOOP ? StorageResult::insecure_root : StorageResult::io;
	struct stat opened_info;
	if (fstat(opened, &opened_info) != 0 || !SameObject(path_info, opened_info)) {
		close(opened);
		return StorageResult::changed;
	}
	*descriptor = opened;
	*device = static_cast<uint64_t>(opened_info.st_dev);
	*inode = static_cast<uint64_t>(opened_info.st_ino);
	return StorageResult::ok;
}

uint32_t Rotate(uint32_t value, uint32_t count) {
	return (value >> count) | (value << (32 - count));
}

void Transform(const unsigned char block[64], uint32_t state[8]) {
	static const uint32_t k[64] = {
		0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
		0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
		0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
		0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
		0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
		0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
		0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
		0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
	};
	uint32_t words[64];
	for (unsigned i = 0; i < 16; ++i) words[i] =
		(static_cast<uint32_t>(block[i * 4]) << 24) |
		(static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
		(static_cast<uint32_t>(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
	for (unsigned i = 16; i < 64; ++i) {
		const uint32_t s0 = Rotate(words[i - 15], 7) ^ Rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
		const uint32_t s1 = Rotate(words[i - 2], 17) ^ Rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}
	uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
	for (unsigned i = 0; i < 64; ++i) {
		const uint32_t s1=Rotate(e,6)^Rotate(e,11)^Rotate(e,25), choice=(e&f)^((~e)&g);
		const uint32_t t1=h+s1+choice+k[i]+words[i], s0=Rotate(a,2)^Rotate(a,13)^Rotate(a,22);
		const uint32_t majority=(a&b)^(a&c)^(b&c);
		h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+s0+majority;
	}
	state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

StorageResult HashDescriptor(int descriptor, uint64_t size, std::string* output) {
	if (lseek(descriptor, 0, SEEK_SET) != 0) return StorageResult::io;
	uint32_t state[8] = {0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
		0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
	unsigned char block[64];
	size_t used = 0;
	uint64_t read_total = 0;
	for (;;) {
		const ssize_t count = read(descriptor, block + used, sizeof(block) - used);
		if (count < 0) {
			if (errno == EINTR) continue;
			return StorageResult::io;
		}
		if (count == 0) break;
		used += static_cast<size_t>(count);
		read_total += static_cast<uint64_t>(count);
		if (read_total > size) return StorageResult::changed;
		if (used == sizeof(block)) {
			Transform(block, state);
			used = 0;
		}
	}
	if (read_total != size) return StorageResult::changed;
	unsigned char tail[128] = {};
	if (used) memcpy(tail, block, used);
	tail[used] = 0x80;
	const size_t final_offset = used >= 56 ? 64 : 0;
	const uint64_t bits = size * 8;
	for (unsigned i = 0; i < 8; ++i) tail[final_offset + 63 - i] = static_cast<unsigned char>(bits >> (i * 8));
	Transform(tail, state);
	if (final_offset) Transform(tail + 64, state);
	static const char hex[] = "0123456789abcdef";
	output->assign(64, '0');
	for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j) {
		const unsigned char value = static_cast<unsigned char>(state[i] >> (24 - j * 8));
		(*output)[(i * 4 + j) * 2] = hex[value >> 4];
		(*output)[(i * 4 + j) * 2 + 1] = hex[value & 15];
	}
	return StorageResult::ok;
}

uint32_t NeutralMask(TeardownAction action) {
	switch (action) {
	case TeardownAction::input: return MISTER_RESOURCE_CORE_INPUT;
	case TeardownAction::audio: return MISTER_RESOURCE_NATIVE_AUDIO;
	case TeardownAction::saves: return MISTER_RESOURCE_SAVES;
	case TeardownAction::video: return MISTER_RESOURCE_NATIVE_VIDEO;
	case TeardownAction::content: return MISTER_RESOURCE_CONTENT;
	case TeardownAction::fpga_reset:
		return MISTER_RESOURCE_FPGA | MISTER_RESOURCE_CORE_PROTOCOL;
	case TeardownAction::bridges: return MISTER_RESOURCE_BRIDGES;
	case TeardownAction::mapping: return 0;
	case TeardownAction::none:
	case TeardownAction::scheduler:
	case TeardownAction::offload:
	case TeardownAction::spi:
	default: return 0;
	}
}

}  // namespace

const char* ProductionCacheRoot() { return "/media/fat/fogcast/cache"; }

const char* StorageResultName(StorageResult result) {
	switch (result) {
	case StorageResult::ok: return "ok";
	case StorageResult::invalid_argument: return "invalid_argument";
	case StorageResult::invalid_identity: return "invalid_identity";
	case StorageResult::unsupported_identity: return "unsupported_identity";
	case StorageResult::insecure_root: return "insecure_root";
	case StorageResult::insecure_entry: return "insecure_entry";
	case StorageResult::not_found: return "not_found";
	case StorageResult::changed: return "changed";
	case StorageResult::digest_mismatch: return "digest_mismatch";
	case StorageResult::io: return "io";
	default: return "io";
	}
}

const char* CoreArtifactResultName(CoreArtifactResult result) {
	switch (result) {
	case CoreArtifactResult::ok: return "ok";
	case CoreArtifactResult::invalid_argument: return "invalid_argument";
	case CoreArtifactResult::invalid_identity: return "invalid_identity";
	case CoreArtifactResult::unavailable: return "unavailable";
	default: return "unavailable";
	}
}

CoreArtifactHandle::CoreArtifactHandle() : descriptor_(-1) {}
CoreArtifactHandle::~CoreArtifactHandle() { Close(); }
bool CoreArtifactHandle::valid() const { return descriptor_ >= 0; }
void CoreArtifactHandle::Close() {
	if (descriptor_ >= 0) close(descriptor_);
	descriptor_ = -1;
}

CoreArtifactResult UnavailableCoreArtifactResolver::Resolve(
	const CoreArtifactAuthority& authority, CoreArtifactHandle* artifact) {
	if (artifact == nullptr || artifact->valid()) return CoreArtifactResult::invalid_argument;
	if (!LowerHex(authority.sha256) || authority.size == 0)
		return CoreArtifactResult::invalid_identity;
	const bool supported =
		(ViewEquals(authority.system, "megadrive") &&
			ViewEquals(authority.expected_core, "MegaDrive")) ||
		(ViewEquals(authority.system, "snes") &&
			ViewEquals(authority.expected_core, "SNES"));
	return supported ? CoreArtifactResult::unavailable : CoreArtifactResult::invalid_identity;
}

ContentHandle::ContentHandle() : descriptor_(-1), size_(0) {}
ContentHandle::~ContentHandle() { Close(); }
bool ContentHandle::valid() const { return descriptor_ >= 0; }
uint64_t ContentHandle::size() const { return size_; }
bool ContentHandle::ReadAt(uint64_t offset, void* bytes, size_t count) const {
	if (!valid() || (count != 0 && bytes == nullptr) || offset > size_ ||
		static_cast<uint64_t>(count) > size_ - offset) return false;
	size_t completed = 0;
	while (completed < count) {
		const ssize_t result = pread(descriptor_, static_cast<unsigned char*>(bytes) + completed,
			count - completed, static_cast<off_t>(offset + completed));
		if (result < 0 && errno == EINTR) continue;
		if (result <= 0) return false;
		completed += static_cast<size_t>(result);
	}
	return true;
}
void ContentHandle::Close() {
	if (descriptor_ >= 0) close(descriptor_);
	descriptor_ = -1;
	size_ = 0;
}

StorageAdapter::StorageAdapter(const char* root, StorageValidationObserver* observer)
	: root_descriptor_(-1), megadrive_descriptor_(-1), snes_descriptor_(-1),
	  status_(StorageResult::invalid_argument), root_(), root_device_(0), root_inode_(0),
	  megadrive_device_(0), megadrive_inode_(0), snes_device_(0), snes_inode_(0),
	  observer_(observer) {
	if (root == nullptr || root[0] != '/' || strlen(root) >= sizeof(root_)) return;
	strcpy(root_, root);
	struct stat path_info;
	if (lstat(root_, &path_info) != 0) {
		status_ = errno == ENOENT ? StorageResult::not_found : StorageResult::io;
		return;
	}
	if (!BoundDirectory(path_info)) { status_ = StorageResult::insecure_root; return; }
	root_descriptor_ = open(root_, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (root_descriptor_ < 0) { status_ = errno == ELOOP ? StorageResult::insecure_root : StorageResult::io; return; }
	struct stat root_info;
	if (fstat(root_descriptor_, &root_info) != 0 || !SameObject(path_info, root_info)) {
		status_ = StorageResult::changed;
		return;
	}
	root_device_ = static_cast<uint64_t>(root_info.st_dev);
	root_inode_ = static_cast<uint64_t>(root_info.st_ino);
	status_ = OpenDirectoryAt(root_descriptor_, "megadrive", &megadrive_descriptor_,
		&megadrive_device_, &megadrive_inode_);
	if (status_ != StorageResult::ok) return;
	status_ = OpenDirectoryAt(root_descriptor_, "snes", &snes_descriptor_,
		&snes_device_, &snes_inode_);
}

StorageAdapter::~StorageAdapter() {
	if (snes_descriptor_ >= 0) close(snes_descriptor_);
	if (megadrive_descriptor_ >= 0) close(megadrive_descriptor_);
	if (root_descriptor_ >= 0) close(root_descriptor_);
}

StorageResult StorageAdapter::status() const { return status_; }

StorageResult StorageAdapter::CheckDirectories() const {
	if (status_ != StorageResult::ok) return status_;
	struct stat root_path, root_opened;
	if (lstat(root_, &root_path) != 0 || fstat(root_descriptor_, &root_opened) != 0)
		return StorageResult::changed;
	if (!SameObject(root_path, root_opened) || static_cast<uint64_t>(root_opened.st_dev) != root_device_ ||
		static_cast<uint64_t>(root_opened.st_ino) != root_inode_) return StorageResult::changed;
	struct stat mega_path, mega_opened, snes_path, snes_opened;
	if (fstatat(root_descriptor_, "megadrive", &mega_path, AT_SYMLINK_NOFOLLOW) != 0 ||
		fstat(megadrive_descriptor_, &mega_opened) != 0 ||
		fstatat(root_descriptor_, "snes", &snes_path, AT_SYMLINK_NOFOLLOW) != 0 ||
		fstat(snes_descriptor_, &snes_opened) != 0) return StorageResult::changed;
	if (!SameObject(mega_path, mega_opened) || !SameObject(snes_path, snes_opened) ||
		static_cast<uint64_t>(mega_opened.st_dev) != megadrive_device_ ||
		static_cast<uint64_t>(mega_opened.st_ino) != megadrive_inode_ ||
		static_cast<uint64_t>(snes_opened.st_dev) != snes_device_ ||
		static_cast<uint64_t>(snes_opened.st_ino) != snes_inode_) return StorageResult::changed;
	return StorageResult::ok;
}

StorageResult StorageAdapter::Resolve(const MisterLaunchV2& launch, ContentHandle* content) {
	if (content == nullptr || content->valid()) return StorageResult::invalid_argument;
	StorageResult directories = CheckDirectories();
	if (directories != StorageResult::ok) return directories;
	if (!LowerHex(launch.content.sha256) || !EntryComponent(launch.content.extension) ||
		launch.content.size == 0 || launch.content.size > 32u * 1024u * 1024u)
		return StorageResult::invalid_identity;
	const Identity* selected = nullptr;
	for (size_t i = 0; i < sizeof(kIdentities) / sizeof(kIdentities[0]); ++i) {
		if (ViewEquals(launch.system, kIdentities[i].system) &&
			ViewEquals(launch.expected_core, kIdentities[i].core) &&
			ViewEquals(launch.content.extension, kIdentities[i].extension)) {
			selected = &kIdentities[i];
			break;
		}
	}
	if (selected == nullptr) return StorageResult::unsupported_identity;
	const std::string digest(launch.content.sha256.data, launch.content.sha256.length);
	const std::string extension(launch.content.extension.data, launch.content.extension.length);
	const std::string name = digest + "." + extension;
	const int directory = strcmp(selected->system, "megadrive") == 0 ?
		megadrive_descriptor_ : snes_descriptor_;
	struct stat path_before;
	if (fstatat(directory, name.c_str(), &path_before, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? StorageResult::not_found : StorageResult::io;
	if (!S_ISREG(path_before.st_mode) || path_before.st_nlink != 1)
		return StorageResult::insecure_entry;
	if (static_cast<uint64_t>(path_before.st_size) != launch.content.size)
		return StorageResult::changed;
	const int descriptor = openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0) return errno == ELOOP ? StorageResult::insecure_entry :
		(errno == ENOENT ? StorageResult::changed : StorageResult::io);
	struct stat opened_before;
	if (fstat(descriptor, &opened_before) != 0 || !SameObject(path_before, opened_before)) {
		close(descriptor);
		return StorageResult::changed;
	}
	std::string actual;
	StorageResult result = HashDescriptor(descriptor, launch.content.size, &actual);
	if (result == StorageResult::ok && observer_ != nullptr) observer_->AfterHash();
	if (result == StorageResult::ok && CheckDirectories() != StorageResult::ok)
		result = StorageResult::changed;
	struct stat opened_after, path_after;
	if (result == StorageResult::ok &&
		(fstat(descriptor, &opened_after) != 0 ||
		fstatat(directory, name.c_str(), &path_after, AT_SYMLINK_NOFOLLOW) != 0 ||
		!SameObject(opened_before, opened_after) || !SameObject(opened_after, path_after)))
		result = StorageResult::changed;
	if (result == StorageResult::ok && actual != digest) result = StorageResult::digest_mismatch;
	if (result != StorageResult::ok) {
		close(descriptor);
		return result;
	}
	content->descriptor_ = descriptor;
	content->size_ = launch.content.size;
	return StorageResult::ok;
}

ResourceTransaction::ResourceTransaction(uint32_t acquired_mask)
	: acquired_mask_(acquired_mask & MISTER_RESOURCE_V2_KNOWN), neutral_mask_(0),
	  supporting_acquired_mask_(0), supporting_neutral_mask_(0), next_action_(0),
	  cleanup_started_set_(false), cleanup_started_(0), short_deadline_(0), fpga_deadline_(0) {
	if (acquired_mask_ == MISTER_RESOURCE_V2_KNOWN) {
		supporting_acquired_mask_ = (1u << static_cast<uint32_t>(TeardownAction::scheduler)) |
			(1u << static_cast<uint32_t>(TeardownAction::offload)) |
			(1u << static_cast<uint32_t>(TeardownAction::spi)) |
			(1u << static_cast<uint32_t>(TeardownAction::mapping));
	}
}
uint32_t ResourceTransaction::acquired_mask() const { return acquired_mask_; }
uint32_t ResourceTransaction::active_mask() const { return acquired_mask_ & ~neutral_mask_; }
uint32_t ResourceTransaction::neutral_mask() const { return neutral_mask_; }
MisterResult ResourceTransaction::Acquire(uint32_t resource_mask) {
	if (resource_mask == 0 || (resource_mask & ~MISTER_RESOURCE_V2_KNOWN) != 0)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (cleanup_started_set_ || (neutral_mask_ & resource_mask) != 0)
		return MISTER_RESULT_INVALID_STATE;
	acquired_mask_ |= resource_mask;
	return MISTER_RESULT_OK;
}

MisterResult ResourceTransaction::AcquireSupporting(TeardownAction action) {
	const uint32_t ordinal = static_cast<uint32_t>(action);
	if (ordinal >= 32 || NeutralMask(action) != 0 || action == TeardownAction::none)
		return MISTER_RESULT_INVALID_ARGUMENT;
	if (cleanup_started_set_) return MISTER_RESULT_INVALID_STATE;
	const uint32_t bit = 1u << ordinal;
	if ((supporting_neutral_mask_ & bit) != 0) return MISTER_RESULT_INVALID_STATE;
	supporting_acquired_mask_ |= bit;
	return MISTER_RESULT_OK;
}

bool ResourceTransaction::ActionRequired(TeardownAction action) const {
	const uint32_t resource = NeutralMask(action);
	if (resource != 0) return (acquired_mask_ & resource) != 0;
	const uint32_t ordinal = static_cast<uint32_t>(action);
	return ordinal < 32 && (supporting_acquired_mask_ & (1u << ordinal)) != 0;
}

bool ResourceTransaction::CleanupComplete() const {
	return active_mask() == 0 &&
		(supporting_acquired_mask_ & ~supporting_neutral_mask_) == 0;
}

MisterResult ResourceTransaction::Stop(ResourceOperations& operations, uint32_t deadline_ms) {
	if (deadline_ms == 0) return MISTER_RESULT_INVALID_ARGUMENT;
	if (CleanupComplete()) return MISTER_RESULT_OK;
	const uint64_t now_at_entry = operations.NowMs();
	if (!cleanup_started_set_) {
		cleanup_started_set_ = true;
		cleanup_started_ = now_at_entry;
		short_deadline_ = cleanup_started_ > UINT64_MAX - 2000 ? UINT64_MAX : cleanup_started_ + 2000;
		fpga_deadline_ = cleanup_started_ > UINT64_MAX - 5000 ? UINT64_MAX : cleanup_started_ + 5000;
	}
	const uint64_t call_deadline = now_at_entry > UINT64_MAX - deadline_ms ?
		UINT64_MAX : now_at_entry + deadline_ms;
	const uint64_t phase_deadline = next_action_ < 7 ? short_deadline_ : fpga_deadline_;
	const uint64_t deadline = call_deadline < phase_deadline ? call_deadline : phase_deadline;
	const uint32_t phase_end = next_action_ < 7 ? 7 :
		static_cast<uint32_t>(sizeof(kActions) / sizeof(kActions[0]));
	while (next_action_ < phase_end) {
		const TeardownAction action = kActions[next_action_];
		if (!ActionRequired(action)) {
			++next_action_;
			continue;
		}
		const uint64_t now = operations.NowMs();
		if (now >= deadline) return MISTER_RESULT_DEADLINE;
		const uint64_t remaining = deadline - now;
		const uint32_t budget = remaining > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(remaining);
		const MisterResult result = operations.Run(action, budget);
		const uint64_t before_observation = operations.NowMs();
		if (before_observation >= deadline) return MISTER_RESULT_DEADLINE;
		if (result != MISTER_RESULT_OK) return result;
		const uint64_t observation_remaining = deadline - before_observation;
		const uint32_t observation_budget = observation_remaining > 0xffffffffu ?
			0xffffffffu : static_cast<uint32_t>(observation_remaining);
		bool neutral = false;
		const MisterResult observation = operations.ObserveNeutral(action,
			observation_budget, &neutral);
		if (operations.NowMs() >= deadline) return MISTER_RESULT_DEADLINE;
		if (observation != MISTER_RESULT_OK) return observation;
		if (!neutral) return MISTER_RESULT_CLEANUP_INCOMPLETE;
		const uint32_t resource = NeutralMask(action);
		neutral_mask_ |= resource & acquired_mask_;
		if (resource == 0)
			supporting_neutral_mask_ |= 1u << static_cast<uint32_t>(action);
		++next_action_;
	}
	return CleanupComplete() ? MISTER_RESULT_OK : MISTER_RESULT_CLEANUP_INCOMPLETE;
}

LinuxV2Context::LinuxV2Context(const char* cache_root)
	: storage_(cache_root), content_(), resources_(0) {}
StorageResult LinuxV2Context::storage_status() const { return storage_.status(); }
StorageResult LinuxV2Context::PrepareContent(const MisterLaunchV2& launch) {
	StorageResult result = storage_.Resolve(launch, &content_);
	if (result != StorageResult::ok) return result;
	if (resources_.Acquire(MISTER_RESOURCE_CONTENT) != MISTER_RESULT_OK) {
		content_.Close();
		return StorageResult::invalid_argument;
	}
	return StorageResult::ok;
}
bool LinuxV2Context::content_active() const { return content_.valid(); }
bool LinuxV2Context::ReadContentAt(uint64_t offset, void* bytes, size_t count) const {
	return content_.ReadAt(offset, bytes, count);
}
uint32_t LinuxV2Context::acquired_mask() const { return resources_.acquired_mask(); }
uint32_t LinuxV2Context::neutral_mask() const { return resources_.neutral_mask(); }

}  // namespace linux_v2
}  // namespace mister
