/*
 * Copyright 2026 FogCast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fogcast/backend_fence.hpp"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <string>

namespace {

std::string ReadFile(const std::string& path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

class UmaskScope {
public:
	explicit UmaskScope(mode_t value) : old_(umask(value)) {}
	~UmaskScope() { umask(old_); }

private:
	mode_t old_;
};

class FenceFake : public fogcast::FenceSyscalls {
public:
	FenceFake() : target_probe(fogcast::PathProbe::missing), temporary_probe(fogcast::PathProbe::missing),
		write_limit(0), fail_write(false), fail_fstat(false), fail_fchmod(false), fail_file_fsync(false), fail_directory_fsync(false),
		fail_rename(false), fail_open_directory(false), fail_close_temporary(false),
		fail_close_directory(false), fail_close_read(false), fail_read(false), allow_open_when_temporary_present(false), read_offset(0), file_fsyncs(0), directory_fsyncs(0), closes(0),
		renames(0), unlinks(0) {
		directory.type = fogcast::FenceFileInfo::Type::directory;
		directory.owner = 1;
		directory.permissions = 0700;
	}

	uint64_t EffectiveUser() override { return 1; }

	fogcast::PathProbe Probe(const std::string& path, fogcast::FenceFileInfo* info) override {
		if (path == target_path) {
			if (target_probe == fogcast::PathProbe::present && info) *info = target_info;
			return target_probe;
		}
		if (path == temporary_path) {
			if (temporary_probe == fogcast::PathProbe::present && info) *info = temporary_info;
			return temporary_probe;
		}
		if (info) *info = directory;
		return fogcast::PathProbe::present;
	}

	int OpenReadNoFollow(const std::string&) override {
		read_offset = 0;
		return target_probe == fogcast::PathProbe::present ? read_descriptor : -1;
	}
	int OpenTemporaryNoFollow(const std::string&) override {
		if (temporary_probe != fogcast::PathProbe::missing && !allow_open_when_temporary_present) return -1;
		temporary_probe = fogcast::PathProbe::present;
		temporary_info.type = fogcast::FenceFileInfo::Type::regular;
		temporary_info.owner = 1;
		temporary_info.permissions = 0600;
		temporary_bytes.clear();
		return temporary_descriptor;
	}
	int OpenDirectory(const std::string&) override { return fail_open_directory ? -1 : directory_descriptor; }
	bool Fstat(int descriptor, fogcast::FenceFileInfo* info) override {
		if (!info || fail_fstat) return false;
		if (descriptor == read_descriptor) {
			*info = target_info;
			return true;
		}
		if (descriptor != temporary_descriptor) return false;
		temporary_info.size = temporary_bytes.size();
		*info = temporary_info;
		return true;
	}
	std::ptrdiff_t Read(int descriptor, char* bytes, size_t size) override {
		if (descriptor != read_descriptor || fail_read || read_offset >= target_bytes.size()) return -1;
		const size_t available = target_bytes.size() - read_offset;
		const size_t count = available < size ? available : size;
		memcpy(bytes, target_bytes.data() + read_offset, count);
		read_offset += count;
		return static_cast<std::ptrdiff_t>(count);
	}
	std::ptrdiff_t Write(int descriptor, const char* bytes, size_t size) override {
		if (descriptor != temporary_descriptor || fail_write) return -1;
		const size_t count = write_limit && write_limit < size ? write_limit : size;
		temporary_bytes.append(bytes, count);
		return static_cast<std::ptrdiff_t>(count);
	}
	bool Fchmod(int descriptor, uint32_t permissions) override {
		if (descriptor != temporary_descriptor || fail_fchmod) return false;
		temporary_info.permissions = permissions;
		return true;
	}
	bool Fsync(int descriptor) override {
		if (descriptor == temporary_descriptor) { ++file_fsyncs; return !fail_file_fsync; }
		if (descriptor == directory_descriptor) { ++directory_fsyncs; return !fail_directory_fsync; }
		return false;
	}
	bool Rename(const std::string&, const std::string&) override {
		++renames;
		if (fail_rename) return false;
		target_probe = fogcast::PathProbe::present;
		target_info = temporary_info;
		target_bytes = temporary_bytes;
		target_info.size = target_bytes.size();
		temporary_probe = fogcast::PathProbe::missing;
		return true;
	}
	bool Unlink(const std::string&) override {
		++unlinks;
		temporary_probe = fogcast::PathProbe::missing;
		temporary_bytes.clear();
		return true;
	}
	bool Close(int descriptor) override {
		++closes;
		if (descriptor == temporary_descriptor) return !fail_close_temporary;
		if (descriptor == read_descriptor) return !fail_close_read;
		if (descriptor == directory_descriptor) return !fail_close_directory;
		return false;
	}

	const std::string target_path = "fence/backend-fence.json";
	const std::string temporary_path = "fence/backend-fence.json.tmp";
	const int temporary_descriptor = 10;
	const int directory_descriptor = 11;
	const int read_descriptor = 12;
	fogcast::PathProbe target_probe;
	fogcast::PathProbe temporary_probe;
	fogcast::FenceFileInfo directory;
	fogcast::FenceFileInfo target_info;
	fogcast::FenceFileInfo temporary_info;
	std::string target_bytes;
	std::string temporary_bytes;
	size_t write_limit;
	bool fail_write;
	bool fail_fstat;
	bool fail_fchmod;
	bool fail_file_fsync;
	bool fail_directory_fsync;
	bool fail_rename;
	bool fail_open_directory;
	bool fail_close_temporary;
	bool fail_close_directory;
	bool fail_close_read;
	bool fail_read;
	bool allow_open_when_temporary_present;
	size_t read_offset;
	int file_fsyncs;
	int directory_fsyncs;
	int closes;
	int renames;
	int unlinks;
};

fogcast::BackendFenceRecord Legacy() {
	return fogcast::BackendFence::Record(1, "legacy", 1);
}

void AssertWriteFailurePreservesTarget(FenceFake* fake) {
	const std::string before = fake->target_bytes;
	fogcast::BackendFence fence("fence", fake);
	assert(fence.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(fake->target_bytes == before);
}

}  // namespace

int main()
{
	char directory[] = "/tmp/fogcast-fence-XXXXXX";
	assert(mkdtemp(directory) != 0);
	fogcast::BackendFence fence(directory);
	fogcast::BackendFenceRecord legacy =
		fogcast::BackendFence::Record(1, "legacy", 1);
	assert(fence.Commit(legacy) == fogcast::ErrorClass::ok);
	fogcast::BackendFenceRecord loaded;
	assert(fence.Load(&loaded) == fogcast::ErrorClass::ok);
	assert(loaded.sequence == 1 && loaded.state == "legacy" &&
		loaded.authority_epoch == 1);
	assert(fence.Commit(fogcast::BackendFence::Record(2, "transitioning", 1)) ==
		fogcast::ErrorClass::ok);
	assert(fence.Commit(fogcast::BackendFence::Record(3, "native", 2)) ==
		fogcast::ErrorClass::ok);
	// Break caught: every completed graph edge has one next sequence and one
	// epoch rule; self, skip, and jump writes leave the prior bytes untouched.
	const std::string native_bytes = ReadFile(std::string(directory) + "/backend-fence.json");
	assert(fence.Commit(fogcast::BackendFence::Record(4, "native", 2)) ==
		fogcast::ErrorClass::transition);
	assert(ReadFile(std::string(directory) + "/backend-fence.json") == native_bytes);
	assert(fence.Commit(fogcast::BackendFence::Record(5, "native_quiescing", 2)) ==
		fogcast::ErrorClass::transition);
	assert(fence.Commit(fogcast::BackendFence::Record(4, "transitioning", 2)) ==
		fogcast::ErrorClass::transition);
	assert(fence.Commit(fogcast::BackendFence::Record(4, "native_quiescing", 2)) ==
		fogcast::ErrorClass::ok);
	const std::string drain_bytes = ReadFile(std::string(directory) + "/backend-fence.json");
	assert(fence.Commit(fogcast::BackendFence::Record(5, "legacy", 2)) ==
		fogcast::ErrorClass::transition);
	assert(ReadFile(std::string(directory) + "/backend-fence.json") == drain_bytes);
	assert(fence.Commit(fogcast::BackendFence::Record(5, "transitioning", 2)) ==
		fogcast::ErrorClass::ok);
	assert(fence.Commit(fogcast::BackendFence::Record(6, "native", 2)) ==
		fogcast::ErrorClass::transition);
	assert(fence.Commit(fogcast::BackendFence::Record(6, "native", 4)) ==
		fogcast::ErrorClass::transition);
	assert(fence.Commit(fogcast::BackendFence::Record(6, "native", 3)) ==
		fogcast::ErrorClass::ok);

	// Break caught: a restrictive caller umask cannot weaken the final durable
	// owner-only mode, and an abandoned temporary file fails closed without
	// replacing the committed fence.
	char secure_directory[] = "/tmp/fogcast-fence-secure-XXXXXX";
	assert(mkdtemp(secure_directory) != 0);
	fogcast::BackendFence secure(secure_directory);
	{
		UmaskScope restrictive(0777);
		assert(secure.Commit(fogcast::BackendFence::Record(1, "legacy", 1)) ==
			fogcast::ErrorClass::ok);
	}
	struct stat status;
	const std::string secure_path = std::string(secure_directory) + "/backend-fence.json";
	assert(stat(secure_path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
	const std::string secure_bytes = ReadFile(secure_path);
	const std::string temporary = std::string(secure_directory) + "/backend-fence.json.tmp";
	const int temporary_file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(temporary_file >= 0 && close(temporary_file) == 0);
	assert(secure.Commit(fogcast::BackendFence::Record(2, "transitioning", 1)) ==
		fogcast::ErrorClass::schema);
	assert(ReadFile(secure_path) == secure_bytes);
	assert(unlink(temporary.c_str()) == 0);

	// Break caught: Load exposes only a valid record.  A genuinely missing file,
	// malformed contents, and a checksum mismatch all reject and cannot be
	// silently treated as an invitation to create a new fence.
	char invalid_directory[] = "/tmp/fogcast-fence-invalid-XXXXXX";
	assert(mkdtemp(invalid_directory) != 0);
	fogcast::BackendFence invalid(invalid_directory);
	fogcast::BackendFenceRecord invalid_loaded;
	assert(invalid.Load(&invalid_loaded) == fogcast::ErrorClass::schema);
	const std::string invalid_path = std::string(invalid_directory) + "/backend-fence.json";
	int invalid_file = open(invalid_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(invalid_file >= 0);
	assert(write(invalid_file, "{}", 2) == 2 && close(invalid_file) == 0);
	const std::string malformed_bytes = ReadFile(invalid_path);
	assert(invalid.Load(&invalid_loaded) == fogcast::ErrorClass::schema);
	assert(invalid.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(ReadFile(invalid_path) == malformed_bytes);

	// Break caught: only a true lstat-ENOENT absence permits the initial
	// legacy fence; every existing or unprobeable target fails closed.
	FenceFake fake;
	fogcast::BackendFence injected("fence", &fake);
	assert(injected.Commit(Legacy()) == fogcast::ErrorClass::ok);
	assert(fake.file_fsyncs == 1 && fake.directory_fsyncs == 1 &&
		fake.renames == 1 && fake.closes == 2);
	fogcast::BackendFenceRecord fake_loaded;
	assert(injected.Load(&fake_loaded) == fogcast::ErrorClass::ok);
	assert(fake_loaded.sequence == 1 && fake_loaded.state == "legacy" &&
		fake_loaded.authority_epoch == 1);
	fake.fail_close_read = true;
	assert(injected.Load(&fake_loaded) == fogcast::ErrorClass::schema);
	fake.fail_close_read = false;
	const std::string initial = fake.target_bytes;
	fake.target_probe = fogcast::PathProbe::error;
	AssertWriteFailurePreservesTarget(&fake);
	fake.target_probe = fogcast::PathProbe::present;
	fake.target_info.type = fogcast::FenceFileInfo::Type::other;
	AssertWriteFailurePreservesTarget(&fake);
	fake.target_info.type = fogcast::FenceFileInfo::Type::regular;
	fake.target_info.owner = 99;
	AssertWriteFailurePreservesTarget(&fake);
	fake.target_info.owner = 1;
	fake.target_info.permissions = 0644;
	AssertWriteFailurePreservesTarget(&fake);
	assert(fake.target_bytes == initial);
	fake.target_info.permissions = 0600;
	fake.fail_read = true;
	AssertWriteFailurePreservesTarget(&fake);
	fake.fail_read = false;

	// Break caught: a stale temporary file or a failed no-follow probe must
	// prevent replacement before any write starts.
	FenceFake stale;
	stale.temporary_probe = fogcast::PathProbe::present;
	stale.allow_open_when_temporary_present = true;
	AssertWriteFailurePreservesTarget(&stale);
	assert(stale.renames == 0 && stale.file_fsyncs == 0);
	stale.temporary_probe = fogcast::PathProbe::error;
	AssertWriteFailurePreservesTarget(&stale);
	assert(stale.renames == 0 && stale.file_fsyncs == 0);

	// Break caught: partial writes are completed, but write/fsync/close/rename
	// failures clean the temporary file and never replace committed bytes.
	FenceFake short_write;
	short_write.write_limit = 1;
	fogcast::BackendFence partial("fence", &short_write);
	assert(partial.Commit(Legacy()) == fogcast::ErrorClass::ok);
	assert(short_write.target_bytes.find("\"sha256\"") != std::string::npos);
	FenceFake write_failure;
	write_failure.fail_write = true;
	AssertWriteFailurePreservesTarget(&write_failure);
	assert(write_failure.unlinks == 1 && write_failure.renames == 0);
	FenceFake file_sync_failure;
	file_sync_failure.fail_file_fsync = true;
	AssertWriteFailurePreservesTarget(&file_sync_failure);
	assert(file_sync_failure.file_fsyncs == 1 && file_sync_failure.unlinks == 1);
	FenceFake fstat_failure;
	fstat_failure.fail_fstat = true;
	AssertWriteFailurePreservesTarget(&fstat_failure);
	assert(fstat_failure.unlinks == 1 && fstat_failure.renames == 0);
	FenceFake fchmod_failure;
	fchmod_failure.fail_fchmod = true;
	AssertWriteFailurePreservesTarget(&fchmod_failure);
	assert(fchmod_failure.unlinks == 1 && fchmod_failure.renames == 0);
	FenceFake close_failure;
	close_failure.fail_close_temporary = true;
	AssertWriteFailurePreservesTarget(&close_failure);
	assert(close_failure.closes == 1 && close_failure.unlinks == 1 && close_failure.renames == 0);
	FenceFake rename_failure;
	rename_failure.fail_rename = true;
	AssertWriteFailurePreservesTarget(&rename_failure);
	assert(rename_failure.renames == 1 && rename_failure.unlinks == 1);

	// Break caught: after rename, a failed directory flush or close reports
	// failure rather than claiming durability.  The next operation reloads the
	// target instead of relying on an in-memory success.
	FenceFake directory_sync_failure;
	directory_sync_failure.fail_directory_fsync = true;
	fogcast::BackendFence failed_directory_sync("fence", &directory_sync_failure);
	assert(failed_directory_sync.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(directory_sync_failure.renames == 1 && directory_sync_failure.directory_fsyncs == 1 &&
		directory_sync_failure.closes == 2);
	FenceFake directory_close_failure;
	directory_close_failure.fail_close_directory = true;
	fogcast::BackendFence failed_directory_close("fence", &directory_close_failure);
	assert(failed_directory_close.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(directory_close_failure.renames == 1 && directory_close_failure.directory_fsyncs == 1 &&
		directory_close_failure.closes == 2);

	// Break caught: dangling and valid symlinks are existing unsafe fence paths,
	// never a missing initial fence.
	char links_directory[] = "/tmp/fogcast-fence-links-XXXXXX";
	assert(mkdtemp(links_directory) != 0);
	const std::string links_target = std::string(links_directory) + "/backend-fence.json";
	const std::string links_temp = std::string(links_directory) + "/backend-fence.json.tmp";
	assert(symlink("missing-target", links_target.c_str()) == 0);
	fogcast::BackendFence links(links_directory);
	assert(links.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(unlink(links_target.c_str()) == 0);
	const int link_source = open((std::string(links_directory) + "/source").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(link_source >= 0 && close(link_source) == 0);
	assert(symlink("source", links_target.c_str()) == 0);
	assert(links.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(unlink(links_target.c_str()) == 0);
	assert(symlink("missing-temp", links_temp.c_str()) == 0);
	assert(links.Commit(Legacy()) == fogcast::ErrorClass::schema);
	assert(unlink(links_temp.c_str()) == 0);
	return 0;
}
