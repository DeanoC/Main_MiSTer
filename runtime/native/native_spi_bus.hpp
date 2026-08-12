// Copyright 2026 FogCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MISTER_RUNTIME_NATIVE_NATIVE_SPI_BUS_HPP
#define MISTER_RUNTIME_NATIVE_NATIVE_SPI_BUS_HPP

#include "runtime/native/hardware_broker.hpp"
#include "runtime/native/native_hardware_io.hpp"
#include "runtime/native/native_clock.hpp"

#include <stddef.h>
#include <stdint.h>

namespace mister {
namespace native {

enum class AckPolicy : uint8_t {
	required
};

struct SpiTransaction {
	uint32_t select_mask;
	const uint16_t *words;
	size_t word_count;
	AckPolicy ack_policy;
	uint32_t deselect_mask;
};

struct SpiReceipt {
	Result result;
	bool selected;
	size_t completed_words;
	bool ack_low_observed;
	bool deselected;
	uint64_t mutation_sequence;
};

class NativeInput;

class SpiReceiptCommitToken final {
public:
	SpiReceiptCommitToken(const SpiReceiptCommitToken &) = delete;
	SpiReceiptCommitToken &operator=(const SpiReceiptCommitToken &) = delete;
	void Commit(const SpiReceipt &receipt) const noexcept
	{
		callback_(context_, receipt);
	}
private:
	friend class NativeInput;
	typedef void (*Callback)(void *, const SpiReceipt &);
	SpiReceiptCommitToken(void *context, Callback callback)
		: context_(context), callback_(callback) {}
	void *context_;
	Callback callback_;
};

class NativeSpiBus final {
public:
	NativeSpiBus(NativeClock &clock, NativeHardwareIo &hardware);
	Result Execute(const OperationLease &lease,
		const SpiTransaction &transaction, SpiReceipt *receipt,
		const SpiReceiptCommitToken *commit = nullptr);

private:
	friend class NativeInput;
	Result ExecuteWithHardwareLeaseView(HardwareLeaseView &view,
		const SpiTransaction &transaction, SpiReceipt *receipt,
		const SpiReceiptCommitToken *commit);
	bool RecordObservedMutation(HardwareLeaseView &view, SpiReceipt *receipt);
	Result WaitForAck(const HardwareLeaseView &view, bool want_high,
		uint64_t deadline_ms, bool *observed);

	NativeClock &clock_;
	NativeHardwareIo &hardware_;
};

} // namespace native
} // namespace mister

#endif
