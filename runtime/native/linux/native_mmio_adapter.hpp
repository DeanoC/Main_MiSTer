// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_LINUX_NATIVE_MMIO_ADAPTER_HPP
#define MISTER_RUNTIME_NATIVE_LINUX_NATIVE_MMIO_ADAPTER_HPP

#include "runtime/native/native_containment.hpp"
#include "runtime/native/native_hardware_io.hpp"
#include "runtime/native/linux/native_fpga_programmer.hpp"

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

class NativeLinuxMmioAdapter final : public NativeContainmentIo,
	public NativeFpgaByteSink, private NativeHardwareIo {
public:
	NativeLinuxMmioAdapter();
#if defined(MISTER_NATIVE_MMIO_TESTING)
	explicit NativeLinuxMmioAdapter(NativeMmioTestOperations &operations);
	Result ValidateBridgeActivationAuthorityForTest(
		const HardwareLeaseView &view) const;
	Result InstallBridgeActivationAuthorityForTest(
		std::unique_ptr<NativeBridgeActivationAuthority> authority);
	bool HasBridgeActivationAuthorityForTest() const;
#endif
	~NativeLinuxMmioAdapter() override;
	NativeHardwareIo &user_io_only_hardware() { return *this; }
	Result CloseMappingsForProcessExit();
	NativeLinuxMmioAdapter(const NativeLinuxMmioAdapter &) = delete;
	NativeLinuxMmioAdapter &operator=(const NativeLinuxMmioAdapter &) = delete;

private:
	class Impl;
	NativeMappingAcquisitionReceipt AcquireMappings(
		const Access &access) override;
	NativeBridgeEnableReceipt EnableBridges(const Access &access) override;
	Result InstallBridgeActivationAuthority(const Access &access,
		std::unique_ptr<NativeBridgeActivationAuthority> authority) override;
	Result ValidateBridgeActivationAuthority(
		const HardwareLeaseView &view) const;
	NativeManagerNeutralReceipt ReconcileManager(const Access &access) override;
	Result ReadManagerControl(const Access &access, uint32_t *value) override;
	Result ReadManagerMode(const Access &access, uint32_t *value) override;
	bool RecoveryMappingsHeld(const Access &access) const override;
	bool ConsumeAppliedMutation(const Access &access) override;
	NativeFpgaSinkStartOutcome Begin(uint64_t expected_bytes,
		uint64_t absolute_deadline_ms,
		std::unique_ptr<NativeFpgaProgramSession> *session) override;

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
	NativeSpiMutationResult Select(const HardwareLeaseView &view,
		NativeSpiTarget target) override;
	NativeSpiMutationResult WriteWordWithStrobeLow(
		const HardwareLeaseView &view, uint16_t word) override;
	NativeSpiMutationResult SetStrobe(const HardwareLeaseView &view,
		bool high) override;
	Result ReadAckSample(const HardwareLeaseView &view,
		NativeSpiAckSample *sample) override;
	NativeSpiMutationResult Deselect(const HardwareLeaseView &view,
		NativeSpiTarget target, uint64_t absolute_deadline_ms) override;
	Result CleanupValidateDigitalNeutralAuthority(
		const CleanupInputReplayView &view) override;
	Result CleanupObserveDigitalNeutralResidue(
		const CleanupInputReplayView &view, bool *user_io_selected,
		bool *strobe_high) override;
	NativeSpiMutationResult CleanupSelectUserIo(
		const CleanupInputReplayView &view) override;
	NativeSpiMutationResult CleanupWriteDigitalNeutralWord(
		const CleanupInputReplayView &view, uint8_t authorized_word_index) override;
	NativeSpiMutationResult CleanupSetStrobe(
		const CleanupInputReplayView &view, bool high) override;
	Result CleanupReadAckSample(const CleanupInputReplayView &view,
		NativeSpiAckSample *sample) override;
	NativeSpiMutationResult CleanupDeselectUserIo(
		const CleanupInputReplayView &view,
		uint64_t absolute_deadline_ms) override;

	Impl *impl_;
};

#if defined(MISTER_NATIVE_MMIO_TESTING)
bool NativeLinuxMmioProductionPrimitivesRejectInvalidAccessForTest();
#endif

} // namespace linux_native
} // namespace native
} // namespace mister

#endif
