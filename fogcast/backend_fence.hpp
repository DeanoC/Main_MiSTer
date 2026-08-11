// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_BACKEND_FENCE_HPP
#define FOGCAST_BACKEND_FENCE_HPP

#include "fogcast/runtime_protocol.hpp"

#include <cstddef>
#include <stdint.h>
#include <string>

namespace fogcast {

struct BackendFenceRecord {
	uint64_t sequence;
	uint64_t authority_epoch;
	std::string state;
	std::string canonical;
	std::string digest;

	BackendFenceRecord() : sequence(0), authority_epoch(0) {}
};

// Portable projection of the file properties that the fence owns.  Linux
// stat details remain in the Linux implementation; this shape is also the
// deterministic fault-injection seam for persistence tests.
struct FenceFileInfo {
	enum class Type { regular, directory, other };
	Type type;
	uint64_t owner;
	uint64_t size;
	uint32_t permissions;
	FenceFileInfo() : type(Type::other), owner(0), size(0), permissions(0) {}
};

// A missing path is distinct from an existing unsafe path and from a failed
// probe.  The production implementation returns missing only for lstat ENOENT.
enum class PathProbe { missing, present, error };

class FenceSyscalls {
public:
	virtual ~FenceSyscalls() {}
	virtual uint64_t EffectiveUser() = 0;
	virtual PathProbe Probe(const std::string& path, FenceFileInfo* info) = 0;
	virtual int OpenReadNoFollow(const std::string& path) = 0;
	virtual int OpenTemporaryNoFollow(const std::string& path) = 0;
	virtual int OpenDirectory(const std::string& path) = 0;
	virtual bool Fstat(int descriptor, FenceFileInfo* info) = 0;
	virtual std::ptrdiff_t Read(int descriptor, char* bytes, size_t size) = 0;
	virtual std::ptrdiff_t Write(int descriptor, const char* bytes, size_t size) = 0;
	virtual bool Fchmod(int descriptor, uint32_t permissions) = 0;
	virtual bool Fsync(int descriptor) = 0;
	virtual bool Rename(const std::string& from, const std::string& to) = 0;
	virtual bool Unlink(const std::string& path) = 0;
	virtual bool Close(int descriptor) = 0;
};

// Linux persistence is intentionally hidden behind this portable value API.
class BackendFence {
public:
	explicit BackendFence(const std::string& directory = std::string(),
		FenceSyscalls* syscalls = 0);
	bool valid() const;
	ErrorClass Load(BackendFenceRecord* record) const;
	ErrorClass Commit(const BackendFenceRecord& record) const;
	static BackendFenceRecord Record(uint64_t sequence, const std::string& state,
		uint64_t authority_epoch);

private:
	enum class FenceLoadStatus { loaded, not_found, invalid };
	FenceLoadStatus LoadRecord(BackendFenceRecord* record) const;
	std::string directory_;
	FenceSyscalls* syscalls_;
};

}  // namespace fogcast

#endif
