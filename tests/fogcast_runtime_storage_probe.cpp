// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/mister_runtime_linux_v2.hpp"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

const unsigned int kCoordinationTimeoutMs = 5000;

bool AbsolutePath(const char* path) {
	return path != nullptr && path[0] == '/' && strlen(path) < PATH_MAX;
}

bool CreateMarker(const char* path, bool* created) {
	if (created) *created = false;
	if (!AbsolutePath(path)) return false;
	const int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0) return false;
	if (created) *created = true;
	struct stat info;
	const bool secure = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
		info.st_nlink == 1 && info.st_uid == geteuid() && (info.st_mode & 0777) == 0600;
	const int close_result = close(descriptor);
	if (!secure || close_result != 0) {
		(void)unlink(path);
		if (created) *created = false;
		return false;
	}
	return true;
}

uint64_t MonotonicMs() {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return static_cast<uint64_t>(now.tv_sec) * 1000u +
		static_cast<uint64_t>(now.tv_nsec) / 1000000u;
}

bool WaitForRelease(const char* path) {
	if (!AbsolutePath(path)) return false;
	const uint64_t start = MonotonicMs();
	if (start == 0) return false;
	for (;;) {
		struct stat info;
		if (lstat(path, &info) == 0) {
			return S_ISREG(info.st_mode) && info.st_nlink == 1 &&
				info.st_uid == geteuid() && (info.st_mode & 0777) == 0600;
		}
		if (errno != ENOENT) return false;
		const uint64_t now = MonotonicMs();
		if (now == 0 || now - start >= kCoordinationTimeoutMs) return false;
		usleep(10000);
	}
}

void SignalReady() {
	puts("ready");
	fflush(stdout);
}

class ProbeAfterHash final : public mister::linux_v2::StorageValidationObserver {
public:
	ProbeAfterHash(const char* marker, const char* release)
		: marker_(marker), release_(release), called_(false), marker_created_(false), ok_(false) {}
	void AfterHash() override {
		called_ = true;
		if (!CreateMarker(marker_, &marker_created_)) return;
		SignalReady();
		ok_ = WaitForRelease(release_);
	}
	bool ok() const { return ok_; }
	bool called() const { return called_; }
	bool marker_created() const { return marker_created_; }
private:
	const char* marker_;
	const char* release_;
	bool called_;
	bool marker_created_;
	bool ok_;
};

}  // namespace

int main(int argc, char** argv) {
	using namespace mister::linux_v2;
	if (argc != 7) {
		puts("invalid_arguments");
		return 2;
	}
	char* end = nullptr;
	errno = 0;
	const unsigned long long size = strtoull(argv[5], &end, 10);
	if (errno == ERANGE || !end || *end || size == 0 || argv[5][0] == '0') {
		puts("invalid_arguments");
		return 2;
	}
	const char* mode = getenv("FOGCAST_STORAGE_PROBE_MODE");
	if (mode == nullptr || mode[0] == '\0') mode = "validate";
	const bool after_hash_mode = strcmp(mode, "after_hash") == 0;
	const bool hold_mode = strcmp(mode, "hold") == 0;
	if (!after_hash_mode && !hold_mode && strcmp(mode, "validate") != 0) {
		puts("invalid_arguments");
		return 2;
	}
	const char* marker = getenv("FOGCAST_STORAGE_PROBE_MARKER");
	const char* release = getenv("FOGCAST_STORAGE_PROBE_RELEASE");
	if ((after_hash_mode || hold_mode) &&
		(!AbsolutePath(marker) || !AbsolutePath(release) || strcmp(marker, release) == 0)) {
		puts("invalid_arguments");
		return 2;
	}

	MisterLaunchV2 launch = {};
	launch.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.struct_size = sizeof(launch);
	launch.game_id = {"probe", 5};
	launch.system = {argv[2], static_cast<uint32_t>(strlen(argv[2]))};
	launch.expected_core = {argv[3], static_cast<uint32_t>(strlen(argv[3]))};
	launch.content.abi_version = MISTER_RUNTIME_ABI_VERSION_V2;
	launch.content.struct_size = sizeof(launch.content);
	launch.content.sha256 = {argv[4], static_cast<uint32_t>(strlen(argv[4]))};
	launch.content.size = size;
	launch.content.extension = {argv[6], static_cast<uint32_t>(strlen(argv[6]))};
	ProbeAfterHash after_hash(marker, release);
	StorageAdapter storage(argv[1], after_hash_mode ? &after_hash : nullptr);
	ContentHandle content;
	bool hold_marker_created = false;
	StorageResult result = storage.status();
	if (result == StorageResult::ok) result = storage.Resolve(launch, &content);
	if (after_hash_mode && after_hash.called() && !after_hash.ok() &&
		result == StorageResult::ok) result = StorageResult::io;
	if (hold_mode && result == StorageResult::ok) {
		if (!CreateMarker(marker, &hold_marker_created)) {
			result = StorageResult::io;
		} else {
			SignalReady();
			if (!WaitForRelease(release)) {
				result = StorageResult::io;
			} else {
				unsigned char byte = 0;
				if (!content.ReadAt(0, &byte, 1)) result = StorageResult::io;
			}
		}
	}
	if ((after_hash_mode && after_hash.marker_created()) || hold_marker_created)
		if (AbsolutePath(marker)) (void)unlink(marker);
	puts(StorageResultName(result));
	fflush(stdout);
	return result == StorageResult::ok ? 0 : 1;
}
