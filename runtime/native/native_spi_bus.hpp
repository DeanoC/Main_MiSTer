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

struct SpiWords {
	const uint16_t *transmit_words;
	uint16_t *received_words;
	size_t word_count;
	size_t received_capacity;
};

struct SpiReceipt {
	Result result;
	bool selected;
	size_t completed_words;
	size_t response_words_observed;
	size_t captured_words;
	bool ack_high_observed;
	bool ack_low_observed;
	bool select_attempted;
	bool deselect_attempted;
	bool deselected;
	bool strobe_low_observed;
	bool force_strobe_low_attempted;
	bool force_strobe_low_applied;
	bool force_strobe_low_observed;
	NativeSpiTarget target;
	bool target_may_be_selected;
	bool strobe_may_be_high;
	bool mapping_retained;
	uint64_t mutation_sequence;
};

class NativeInput;
class NativeSpiBus;
class SpiReceiptCommitToken;

// This capability is the only production admission to the input SPI route.
// It deliberately has no target parameter, so input code cannot select file
// I/O or compose a generic selected exchange.
class NativeInputSpiPort final {
public:
	NativeInputSpiPort(const NativeInputSpiPort &) = delete;
	NativeInputSpiPort &operator=(const NativeInputSpiPort &) = delete;
private:
	friend class NativeSpiBus;
	friend class NativeInput;
	explicit NativeInputSpiPort(NativeSpiBus &bus);
	Result Execute(HardwareLeaseView &view, const SpiWords &words,
		SpiReceipt *receipt, const SpiReceiptCommitToken *commit);
	NativeSpiBus &bus_;
};

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
	NativeInputSpiPort &input_port() { return input_port_; }
#if defined(MISTER_NATIVE_SPI_TESTING)
	// Test-only typed admission. Production callers receive only their
	// purpose-specific entry points below.
	Result ExchangeForTest(const OperationLease &lease, NativeSpiTarget target,
		const SpiWords &words, SpiReceipt *receipt);
#endif

private:
	friend class NativeInputSpiPort;
	Result ExchangeWithHardwareLeaseView(HardwareLeaseView &view,
		NativeSpiTarget target, const SpiWords &words, SpiReceipt *receipt,
		const SpiReceiptCommitToken *commit);
	bool RecordAppliedMutation(HardwareLeaseView &view, SpiReceipt *receipt);
	void RecordMutationResult(HardwareLeaseView &view, SpiReceipt *receipt,
		const NativeSpiMutationResult &mutation, Result *primary);
	Result WaitForAck(const HardwareLeaseView &view, bool want_high,
		uint64_t deadline_ms, bool *observed, uint16_t *response);

	NativeClock &clock_;
	NativeHardwareIo &hardware_;
	NativeInputSpiPort input_port_;
};

} // namespace native
} // namespace mister

#endif
