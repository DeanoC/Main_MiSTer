// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FOGCAST_RUNTIME_STATE_HPP
#define FOGCAST_RUNTIME_STATE_HPP

#include "fogcast/runtime_protocol.hpp"

#include <stdint.h>
#include <string>

namespace fogcast {

struct StateRecord {
	uint64_t sequence;
	uint64_t backend_epoch;
	std::string phase;
	std::string canonical;
	std::string digest;

	StateRecord() : sequence(0), backend_epoch(0) {}
};

ErrorClass ParseStateRecord(const std::string& bytes, StateRecord* record,
	std::string* canonical, std::string* digest);

// Re-encode only the top-level durable identity fields after authenticating and
// structurally parsing a complete record.  Historical ledger/snapshot tokens
// are never searched or rewritten by decimal text.
ErrorClass ReencodeStateIdentity(const std::string& source, uint64_t new_sequence,
	uint64_t new_epoch, StateRecord* result);

}  // namespace fogcast

#endif
