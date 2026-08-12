// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_MMIO_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_MMIO_ADAPTER_HPP

#include "runtime/native/native_containment.hpp"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {
namespace linux_native {

#if defined(MISTER_NATIVE_MMIO_TESTING)
class NativeMmioTestMapping final {
public:
	NativeMmioTestMapping() : identity(0) {}
	uintptr_t identity;
};

// This injected seam exists only in the host-test build. No production
// translation unit can name or implement general MMIO operations.
class NativeMmioTestOperations {
public:
	virtual ~NativeMmioTestOperations() {}
	virtual size_t PageSize() const = 0;
	virtual uint64_t NowMs() const = 0;
	virtual int Open(const char *path, int flags) = 0;
	virtual int Close(int descriptor) = 0;
	virtual int Map(int descriptor, uint64_t page_offset, size_t length,
		NativeMmioTestMapping *mapping) = 0;
	virtual int Unmap(const NativeMmioTestMapping &mapping, size_t length) = 0;
	virtual int Read32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t *value) = 0;
	virtual int Write32(const NativeMmioTestMapping &mapping, size_t offset,
		uint32_t value) = 0;
	virtual int OrderingBarrier() = 0;
};
#endif

class NativeLinuxMmioAdapter final : public NativeContainmentIo {
public:
	NativeLinuxMmioAdapter();
#if defined(MISTER_NATIVE_MMIO_TESTING)
	explicit NativeLinuxMmioAdapter(NativeMmioTestOperations &operations);
#endif
	~NativeLinuxMmioAdapter() override;
	NativeLinuxMmioAdapter(const NativeLinuxMmioAdapter &) = delete;
	NativeLinuxMmioAdapter &operator=(const NativeLinuxMmioAdapter &) = delete;

private:
	class Impl;

	Result WriteCoreReset(const Access &access, uint32_t mask,
		uint32_t value) override;
	Result WriteInterfaceModule(const Access &access, uint32_t value) override;
	Result WriteSdrPortControl(const Access &access, uint32_t offset,
		uint32_t value) override;
	Result WriteBridgeReset(const Access &access, uint32_t value) override;
	Result WriteRemap(const Access &access, uint32_t value) override;
	Result ReadCoreGpo(const Access &access, uint32_t *value) override;
	Result ReadInterfaceModule(const Access &access, uint32_t *value) override;
	Result ReadSdrPortControl(const Access &access, uint32_t offset,
		uint32_t *value) override;
	Result ReadBridgeReset(const Access &access, uint32_t *value) override;
	Result ReadRemap(const Access &access, uint32_t *value) override;
	Result ReleaseMappings(const Access &access) override;

	Impl *impl_;
};

#if defined(MISTER_NATIVE_MMIO_TESTING)
bool NativeLinuxMmioProductionPrimitivesRejectInvalidAccessForTest();
#endif

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
