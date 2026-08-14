// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_HARDWARE_IO_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_HARDWARE_IO_HPP

#include "runtime/mister_runtime.h"

#include <stdint.h>

namespace mister {
namespace native {

class HardwareLeaseView;
class CleanupInputReplayView;

// The only SPI targets admitted by the target-native runtime. Platform code
// owns their physical register mapping; no caller can supply a raw bit mask.
enum class NativeSpiTarget : uint8_t {
	user_io,
	file_io
};

struct NativeSpiAckSample {
	bool ack_high;
	bool fault;
	uint16_t response;
};

// A mutation can fail after the platform attempted it. The adapter therefore
// reports the platform result separately from whether the write was applied
// and positively observed. Callers retain conservative residue whenever the
// expected state was not observed.
struct NativeSpiMutationResult {
	MisterResult result;
	bool attempted;
	bool applied;
	bool observed;
};

// The native bus is deliberately expressed in terms of one platform adapter.
// Implementations of these methods own the target-specific register mapping;
// callers cannot compose raw select/data/deselect operations independently.
class NativeHardwareIo {
public:
	virtual ~NativeHardwareIo() {}

private:
	friend class NativeSpiBus;
	virtual MisterResult CleanupValidateDigitalNeutralAuthority(
		const CleanupInputReplayView &) { return MISTER_RESULT_INVALID_STATE; }
	virtual MisterResult CleanupObserveDigitalNeutralResidue(
		const CleanupInputReplayView &, bool *, bool *)
		{ return MISTER_RESULT_INVALID_STATE; }
	virtual NativeSpiMutationResult CleanupSelectUserIo(
		const CleanupInputReplayView &)
		{ return {MISTER_RESULT_INVALID_STATE, false, false, false}; }
	virtual NativeSpiMutationResult CleanupWriteDigitalNeutralWord(
		const CleanupInputReplayView &, uint8_t)
		{ return {MISTER_RESULT_INVALID_STATE, false, false, false}; }
	virtual NativeSpiMutationResult CleanupSetStrobe(
		const CleanupInputReplayView &, bool)
		{ return {MISTER_RESULT_INVALID_STATE, false, false, false}; }
	virtual MisterResult CleanupReadAckSample(
		const CleanupInputReplayView &, NativeSpiAckSample *)
		{ return MISTER_RESULT_INVALID_STATE; }
	virtual NativeSpiMutationResult CleanupDeselectUserIo(
		const CleanupInputReplayView &, uint64_t)
		{ return {MISTER_RESULT_INVALID_STATE, false, false, false}; }
	virtual NativeSpiMutationResult Select(const HardwareLeaseView &view,
		NativeSpiTarget target) = 0;
	virtual NativeSpiMutationResult WriteWordWithStrobeLow(
		const HardwareLeaseView &view,
		uint16_t word) = 0;
	virtual NativeSpiMutationResult SetStrobe(const HardwareLeaseView &view,
		bool high) = 0;
	virtual MisterResult ReadAckSample(const HardwareLeaseView &view,
		NativeSpiAckSample *sample) = 0;
	virtual NativeSpiMutationResult Deselect(const HardwareLeaseView &view,
		NativeSpiTarget target, uint64_t absolute_deadline_ms) = 0;
};

} // namespace native
} // namespace mister

#endif
