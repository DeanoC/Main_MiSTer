// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_RUNTIME_STORE_HPP
#define FOGCAST_RUNTIME_STORE_HPP

#include "fogcast/runtime_state.hpp"

#include <string>

namespace fogcast {

// The Linux store owns policy; this narrow seam makes write/fsync/rename
// failures deterministic without weakening the production path.
class StoreSyscalls {
public:
	virtual ~StoreSyscalls() {}
	virtual long Write(int file, const char* bytes, unsigned long size) const = 0;
	virtual int Sync(int file) const = 0;
	virtual int Rename(const char* from, const char* to) const = 0;
	virtual int Unlink(const char* path) const = 0;
};

class StateStore {
public:
	explicit StateStore(const std::string& directory, const StoreSyscalls* syscalls = 0);
	ErrorClass Load(StateRecord* record) const;
	ErrorClass Commit(const StateRecord& record) const;

private:
	std::string directory_;
	const StoreSyscalls* syscalls_;
};

}  // namespace fogcast

#endif
