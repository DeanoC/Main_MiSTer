// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_HARDWARE_IO_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_HARDWARE_IO_HPP

#include "runtime/mister_runtime.h"

#include <stdint.h>

namespace mister {
namespace native {

class HardwareLeaseView;

// The native bus is deliberately expressed in terms of one platform adapter.
// Implementations of these methods own the target-specific register mapping;
// callers cannot compose raw select/data/deselect operations independently.
class NativeHardwareIo {
public:
	virtual ~NativeHardwareIo() {}

private:
	friend class NativeSpiBus;
	virtual MisterResult Select(const HardwareLeaseView &view,
		uint32_t mask) = 0;
	virtual MisterResult WriteWord(const HardwareLeaseView &view,
		uint16_t word) = 0;
	virtual MisterResult ReadAck(const HardwareLeaseView &view,
		bool *high) = 0;
	virtual MisterResult Deselect(const HardwareLeaseView &view,
		uint32_t mask, uint64_t absolute_deadline_ms) = 0;
};

} // namespace native
} // namespace mister

#endif
