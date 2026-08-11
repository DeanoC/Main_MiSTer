// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fogcast/runtime_store.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fogcast {

namespace {

class LinuxSyscalls : public StoreSyscalls {
public:
	long Write(int file, const char* bytes, unsigned long size) const override { return static_cast<long>(write(file, bytes, static_cast<size_t>(size))); }
	int Sync(int file) const override { return fsync(file); }
	int Rename(const char* from, const char* to) const override { return rename(from, to); }
	int Unlink(const char* path) const override { return unlink(path); }
};

const StoreSyscalls& DefaultSyscalls() {
	static const LinuxSyscalls instance;
	return instance;
}

bool SecureDirectory(const std::string& path) {
	struct stat status;
	return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
		status.st_uid == geteuid() && (status.st_mode & 0777) == 0700;
}

bool SecureRegularFile(const struct stat& status) {
	return S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
		(status.st_mode & 0777) == 0600;
}

bool WriteAll(const StoreSyscalls& syscalls, int file, const std::string& bytes) {
	size_t offset = 0;
	while (offset < bytes.size()) {
		const long wrote = syscalls.Write(file, bytes.data() + offset, static_cast<unsigned long>(bytes.size() - offset));
		if (wrote <= 0) return false;
		offset += static_cast<size_t>(wrote);
	}
	return true;
}

}  // namespace

StateStore::StateStore(const std::string& directory, const StoreSyscalls* syscalls) : directory_(directory), syscalls_(syscalls ? syscalls : &DefaultSyscalls()) {}

ErrorClass StateStore::Load(StateRecord* record) const {
	if (!record || !SecureDirectory(directory_)) return ErrorClass::schema;
	const std::string path = directory_ + "/state.json";
	const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (file < 0) return ErrorClass::schema;
	struct stat status;
	if (fstat(file, &status) != 0 || !SecureRegularFile(status) || status.st_size < 1 || status.st_size > 256 * 1024) {
		close(file);
		return ErrorClass::schema;
	}
	std::string bytes;
	bytes.resize(static_cast<size_t>(status.st_size));
	size_t offset = 0;
	while (offset < bytes.size()) {
		const ssize_t got = read(file, &bytes[offset], bytes.size() - offset);
		if (got <= 0) { close(file); return ErrorClass::schema; }
		offset += static_cast<size_t>(got);
	}
	if (close(file) != 0) return ErrorClass::schema;
	std::string canonical;
	std::string digest;
	return ParseStateRecord(bytes, record, &canonical, &digest);
}

ErrorClass StateStore::Commit(const StateRecord& record) const {
	if (!SecureDirectory(directory_) || record.sequence == 0 || record.canonical.empty() ||
		record.digest.size() != 64 || Sha256Hex(record.canonical) != record.digest) return ErrorClass::schema;
	if (record.canonical[record.canonical.size() - 1] != '}') return ErrorClass::schema;
	const std::string bytes = record.canonical.substr(0, record.canonical.size() - 1) + ",\"sha256\":\"" + record.digest + "\"}";
	StateRecord validated;
	std::string canonical;
	std::string digest;
	if (ParseStateRecord(bytes, &validated, &canonical, &digest) != ErrorClass::ok ||
		validated.sequence != record.sequence || canonical != record.canonical || digest != record.digest) return ErrorClass::schema;
	const std::string path = directory_ + "/state.json";
	const std::string temporary = directory_ + "/state.json.tmp";
	struct stat old;
	if (lstat(temporary.c_str(), &old) == 0 || (errno != ENOENT && errno != ENOTDIR)) return ErrorClass::schema;
	if (lstat(path.c_str(), &old) == 0) {
		if (!SecureRegularFile(old)) return ErrorClass::schema;
		StateRecord previous;
		if (Load(&previous) != ErrorClass::ok || record.sequence <= previous.sequence) return ErrorClass::schema;
	} else if (errno != ENOENT && errno != ENOTDIR) return ErrorClass::schema;
	const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (file < 0) return ErrorClass::schema;
	struct stat temporary_status;
	bool ok = fchmod(file, 0600) == 0 && fstat(file, &temporary_status) == 0 && SecureRegularFile(temporary_status);
	if (ok) ok = WriteAll(*syscalls_, file, bytes);
	if (ok) ok = syscalls_->Sync(file) == 0;
	const int file_close = close(file);
	if (!ok || file_close != 0) { syscalls_->Unlink(temporary.c_str()); return ErrorClass::schema; }
	if (syscalls_->Rename(temporary.c_str(), path.c_str()) != 0) { syscalls_->Unlink(temporary.c_str()); return ErrorClass::schema; }
	const int directory = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0) return ErrorClass::schema;
	const bool synced = syscalls_->Sync(directory) == 0;
	const int directory_close = close(directory);
	return synced && directory_close == 0 ? ErrorClass::ok : ErrorClass::schema;
}

}  // namespace fogcast
