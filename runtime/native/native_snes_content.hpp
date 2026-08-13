// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_SNES_CONTENT_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_SNES_CONTENT_HPP

#include "runtime/mister_runtime.h"
#include "runtime/native/native_clock.hpp"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {

// The source is already retained by the lifecycle. This narrow interface never
// exposes a path or a descriptor and every read remains source-authoritative.
class NativeSnesContentSource {
public:
	virtual ~NativeSnesContentSource() {}
	virtual MisterResult ReadAt(uint64_t offset, void *bytes, size_t count,
		uint64_t absolute_deadline_ms) = 0;
};

struct NativeSnesContentPlan {
	uint64_t source_offset;
	uint64_t source_size;
	uint64_t rom_wire_size;
	uint8_t metadata[512];
};

MisterResult PrepareNativeSnesContent(NativeSnesContentSource &source,
	uint64_t retained_size, uint64_t minimum_source_bytes,
	uint64_t maximum_source_bytes, uint64_t maximum_wire_bytes,
	const NativeClock &clock, uint64_t absolute_deadline_ms,
	NativeSnesContentPlan *plan);

MisterResult ReadNativeSnesRomWindow(NativeSnesContentSource &source,
	const NativeSnesContentPlan &plan, uint64_t wire_offset, void *bytes,
	size_t count, const NativeClock &clock, uint64_t absolute_deadline_ms);

} // namespace native
} // namespace mister

#endif
