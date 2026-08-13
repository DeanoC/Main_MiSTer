// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_CORE_PROTOCOL_IO_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_CORE_PROTOCOL_IO_ADAPTER_HPP

#include "runtime/native/native_core_protocol.hpp"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {
namespace linux_native {

#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
class NativeCoreProtocolIoTestMapping final {
public:
	NativeCoreProtocolIoTestMapping() : identity(0) {}
	uintptr_t identity;
};

// Host tests inject only the adapter's private register backend. This seam is
// not part of a production translation unit or public target protocol.
class NativeCoreProtocolIoTestOperations {
public:
	virtual ~NativeCoreProtocolIoTestOperations() {}
	virtual size_t PageSize() const = 0;
	virtual int Open(const char *path, int flags) = 0;
	virtual int Close(int descriptor) = 0;
	virtual int Map(int descriptor, uint64_t page_offset, size_t length,
		NativeCoreProtocolIoTestMapping *mapping) = 0;
	virtual int Unmap(const NativeCoreProtocolIoTestMapping &mapping,
		size_t length) = 0;
	virtual int Read32(const NativeCoreProtocolIoTestMapping &mapping,
		size_t offset, uint32_t *value) = 0;
	virtual int Write32(const NativeCoreProtocolIoTestMapping &mapping,
		size_t offset, uint32_t value) = 0;
	virtual int OrderingBarrier() = 0;
};
#endif

class NativeCoreProtocolIoAdapterCapabilitySet;

class NativeCoreProtocolIoAdapter final {
public:
	explicit NativeCoreProtocolIoAdapter(NativeClock &clock);
#if defined(MISTER_NATIVE_CORE_PROTOCOL_IO_TESTING)
	NativeCoreProtocolIoAdapter(NativeClock &clock,
		NativeCoreProtocolIoTestOperations &operations);
#endif
	~NativeCoreProtocolIoAdapter();
	NativeCoreProtocolIoAdapter(const NativeCoreProtocolIoAdapter &) = delete;
	NativeCoreProtocolIoAdapter &operator=(
		const NativeCoreProtocolIoAdapter &) = delete;
	bool valid() const;
	NativeCoreProtocolCapabilities capabilities();

private:
	NativeCoreProtocolCapabilities capabilities_;
};

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
