// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/backend_fence.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fogcast {
namespace {

class LinuxFenceSyscalls : public FenceSyscalls {
public:
	uint64_t EffectiveUser() override { return static_cast<uint64_t>(geteuid()); }

	PathProbe Probe(const std::string& path, FenceFileInfo* info) override {
		struct stat status;
		if (lstat(path.c_str(), &status) != 0) return errno == ENOENT ?
			PathProbe::missing : PathProbe::error;
		if (info) {
			info->type = S_ISREG(status.st_mode) ? FenceFileInfo::Type::regular :
				(S_ISDIR(status.st_mode) ? FenceFileInfo::Type::directory : FenceFileInfo::Type::other);
			info->owner = static_cast<uint64_t>(status.st_uid);
			info->size = status.st_size < 0 ? 0 : static_cast<uint64_t>(status.st_size);
			info->permissions = static_cast<uint32_t>(status.st_mode & 0777);
		}
		return PathProbe::present;
	}

	int OpenReadNoFollow(const std::string& path) override {
		return open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	}

	int OpenTemporaryNoFollow(const std::string& path) override {
		return open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	}

	int OpenDirectory(const std::string& path) override {
		return open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	}

	bool Fstat(int descriptor, FenceFileInfo* info) override {
		struct stat status;
		if (fstat(descriptor, &status) != 0 || !info) return false;
		info->type = S_ISREG(status.st_mode) ? FenceFileInfo::Type::regular :
			(S_ISDIR(status.st_mode) ? FenceFileInfo::Type::directory : FenceFileInfo::Type::other);
		info->owner = static_cast<uint64_t>(status.st_uid);
		info->size = status.st_size < 0 ? 0 : static_cast<uint64_t>(status.st_size);
		info->permissions = static_cast<uint32_t>(status.st_mode & 0777);
		return true;
	}

	std::ptrdiff_t Read(int descriptor, char* bytes, size_t size) override {
		return static_cast<std::ptrdiff_t>(read(descriptor, bytes, size));
	}

	std::ptrdiff_t Write(int descriptor, const char* bytes, size_t size) override {
		return static_cast<std::ptrdiff_t>(write(descriptor, bytes, size));
	}

	bool Fchmod(int descriptor, uint32_t permissions) override {
		return fchmod(descriptor, static_cast<mode_t>(permissions)) == 0;
	}

	bool Fsync(int descriptor) override { return fsync(descriptor) == 0; }
	bool Rename(const std::string& from, const std::string& to) override {
		return rename(from.c_str(), to.c_str()) == 0;
	}
	bool Unlink(const std::string& path) override { return unlink(path.c_str()) == 0; }
	bool Close(int descriptor) override { return close(descriptor) == 0; }
};

FenceSyscalls& DefaultSyscalls() {
	static LinuxFenceSyscalls syscalls;
	return syscalls;
}

bool SecureDirectory(FenceSyscalls* syscalls, const std::string& path) {
	FenceFileInfo status;
	return syscalls->Probe(path, &status) == PathProbe::present &&
		status.type == FenceFileInfo::Type::directory &&
		status.owner == syscalls->EffectiveUser() && status.permissions == 0700;
}

bool SecureFile(FenceSyscalls* syscalls, const FenceFileInfo& status) {
	return status.type == FenceFileInfo::Type::regular &&
		status.owner == syscalls->EffectiveUser() && status.permissions == 0600;
}

bool WriteAll(FenceSyscalls* syscalls, int file, const std::string& bytes) {
	size_t offset = 0;
	while (offset < bytes.size()) {
		const std::ptrdiff_t wrote = syscalls->Write(file, bytes.data() + offset, bytes.size() - offset);
		if (wrote <= 0 || static_cast<size_t>(wrote) > bytes.size() - offset) return false;
		offset += static_cast<size_t>(wrote);
	}
	return true;
}

bool Parse(const std::string& bytes, BackendFenceRecord* record) {
	std::string canonical;
	std::string digest;
	if (ParseBackendFence(bytes, &canonical, &digest) != ErrorClass::ok) return false;
	detail::Token root;
	if (detail::ScanV1Json(canonical, 256 * 1024, &root) != ErrorClass::ok) return false;
	uint64_t sequence = 0;
	uint64_t epoch = 0;
	if (!detail::PositiveUint63(detail::Member(root, "sequence"), &sequence) ||
		!detail::PositiveUint63(detail::Member(root, "authority_epoch"), &epoch)) return false;
	const detail::Token* state = detail::Member(root, "state");
	if (!state || state->kind != detail::Token::Kind::string) return false;
	record->sequence = sequence;
	record->authority_epoch = epoch;
	record->state = state->text;
	record->canonical = canonical;
	record->digest = digest;
	return true;
}

bool ValidRecord(const BackendFenceRecord& record, std::string* bytes) {
	if (record.sequence == 0 || record.authority_epoch == 0 || record.canonical.empty() ||
		record.digest != Sha256Hex(record.canonical)) return false;
	*bytes = record.canonical.substr(0, record.canonical.size() - 1) +
		",\"sha256\":\"" + record.digest + "\"}";
	BackendFenceRecord checked;
	return Parse(*bytes, &checked) && checked.sequence == record.sequence &&
		checked.authority_epoch == record.authority_epoch && checked.state == record.state &&
		checked.canonical == record.canonical;
}

}  // namespace

BackendFence::BackendFence(const std::string& directory, FenceSyscalls* syscalls)
	: directory_(directory), syscalls_(syscalls ? syscalls : &DefaultSyscalls()) {}

bool BackendFence::valid() const { return SecureDirectory(syscalls_, directory_); }

BackendFenceRecord BackendFence::Record(uint64_t sequence, const std::string& state,
	uint64_t authority_epoch) {
	BackendFenceRecord record;
	record.sequence = sequence;
	record.authority_epoch = authority_epoch;
	record.state = state;
	record.canonical = std::string("{\"record_version\":1,\"sequence\":") +
		std::to_string(sequence) + ",\"state\":\"" + state +
		"\",\"authority_epoch\":" + std::to_string(authority_epoch) + "}";
	record.digest = Sha256Hex(record.canonical);
	return record;
}

BackendFence::FenceLoadStatus BackendFence::LoadRecord(BackendFenceRecord* record) const {
	if (!record || !valid()) return FenceLoadStatus::invalid;
	const std::string path = directory_ + "/backend-fence.json";
	FenceFileInfo path_status;
	const PathProbe probe = syscalls_->Probe(path, &path_status);
	if (probe == PathProbe::missing) return FenceLoadStatus::not_found;
	if (probe != PathProbe::present || !SecureFile(syscalls_, path_status) ||
		path_status.size < 1 || path_status.size > 256 * 1024) return FenceLoadStatus::invalid;
	const int file = syscalls_->OpenReadNoFollow(path);
	if (file < 0) return FenceLoadStatus::invalid;
	FenceFileInfo open_status;
	bool ok = syscalls_->Fstat(file, &open_status) && SecureFile(syscalls_, open_status) &&
		open_status.size == path_status.size && open_status.size >= 1 && open_status.size <= 256 * 1024;
	std::string bytes;
	if (ok) {
		bytes.assign(static_cast<size_t>(open_status.size), '\0');
		size_t offset = 0;
		while (offset < bytes.size()) {
			const std::ptrdiff_t got = syscalls_->Read(file, &bytes[offset], bytes.size() - offset);
			if (got <= 0 || static_cast<size_t>(got) > bytes.size() - offset) { ok = false; break; }
			offset += static_cast<size_t>(got);
		}
	}
	if (!syscalls_->Close(file)) ok = false;
	return ok && Parse(bytes, record) ? FenceLoadStatus::loaded : FenceLoadStatus::invalid;
}

ErrorClass BackendFence::Load(BackendFenceRecord* record) const {
	return LoadRecord(record) == FenceLoadStatus::loaded ? ErrorClass::ok : ErrorClass::schema;
}

ErrorClass BackendFence::Commit(const BackendFenceRecord& record) const {
	if (!valid()) return ErrorClass::schema;
	std::string bytes;
	if (!ValidRecord(record, &bytes)) return ErrorClass::schema;
	BackendFenceRecord prior;
	const FenceLoadStatus loaded = LoadRecord(&prior);
	BackendFenceTracker tracker;
	if (loaded == FenceLoadStatus::loaded) {
		if (tracker.Accept(prior.canonical) != ErrorClass::ok ||
			tracker.Accept(record.canonical) != ErrorClass::ok) return ErrorClass::transition;
	} else if (loaded == FenceLoadStatus::not_found) {
		if (tracker.Accept(record.canonical) != ErrorClass::ok) return ErrorClass::transition;
	} else {
		return ErrorClass::schema;
	}
	const std::string temporary = directory_ + "/backend-fence.json.tmp";
	FenceFileInfo temporary_status;
	if (syscalls_->Probe(temporary, &temporary_status) != PathProbe::missing) return ErrorClass::schema;
	const int file = syscalls_->OpenTemporaryNoFollow(temporary);
	if (file < 0) return ErrorClass::schema;
	FenceFileInfo open_status;
	bool ok = syscalls_->Fchmod(file, 0600) && syscalls_->Fstat(file, &open_status) &&
		SecureFile(syscalls_, open_status) && WriteAll(syscalls_, file, bytes) && syscalls_->Fsync(file);
	if (!syscalls_->Close(file)) ok = false;
	if (!ok) {
		syscalls_->Unlink(temporary);
		return ErrorClass::schema;
	}
	const std::string path = directory_ + "/backend-fence.json";
	if (!syscalls_->Rename(temporary, path)) {
		syscalls_->Unlink(temporary);
		return ErrorClass::schema;
	}
	const int directory = syscalls_->OpenDirectory(directory_);
	if (directory < 0) return ErrorClass::schema;
	const bool synced = syscalls_->Fsync(directory);
	const bool closed = syscalls_->Close(directory);
	return synced && closed ? ErrorClass::ok : ErrorClass::schema;
}

}  // namespace fogcast
